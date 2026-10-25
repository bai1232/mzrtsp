# 流水线重构（v0.2.3）：把 `DemuxerProducer` 拆成「读 / 形状 / 编排」

> 这是一次**行为不变的重构**（不新增能力、不改任何输出字节），所以没有新需求，只有结构。
> 验收口径不是"新功能能用"，而是：**同一输入 → 同一输出字节**（`scripts/refactor_equiv_check.sh`）。

## 1. 为什么（问题用数据说话）

`DemuxerProducer` 在 v0.1–v0.2 期间长成了一条"隐式流水线"，一个类里塞了 5 类职责：

| 职责 | 代码 |
|---|---|
| 读包 / 跳过非音视频 / 时间戳换算失败仍发 | `read()` 前半段 |
| 循环重开 + 趟偏移（`pass_end − pass_start`） | `reopenForLoop()` / `passEndMs()` / `passStartMs()` / `trackPass()` |
| Annex-B → AVCC + 现场造 avcC | `prepareStreams()` 的 Annex-B 分支 + `read()` 的一支 |
| ADTS → raw AAC + 一次性探针造 ASC | `probeAdtsAudioConfig()` + `read()` 的一支 |
| 转码编排（0..N 产物、EOF flush、接缝对齐、speed 观测） | `openTranscoder()` / `transcodeVideo()` / `enqueueTranscoded()` / `flushTranscoder()` / `servePending()` |

规模：**789 行 / 22 个成员函数**。代价是"每加一件输入侧的事都要改它"：

| 里程碑 | 对 `demuxer_producer.cpp` 的改动 |
|---|---|
| M8（MKV/TS 输入） | **+125 / −1** |
| M10-b（转码接线） | **+260** |
| v0.2.1（B 帧时间戳） | **+38 / −7** |

这就是"加新功能还要改以前代码"的具体形态 —— 违反开闭原则（OCP）。
（对比：同期对 `media/` 分发核心 + `network/` + `http/` 的侵入总共 **29 行**，其中 19 行是新增函数、
10 行是新增只读接口 —— **输出轴是闭的，输入轴没闭**。）

## 2. 拆成什么

```
              ┌─────────────────────── DemuxerProducer（编排，241 行）────────────────────────┐
              │  搭链 buildChains()  →  驱动 read()  →  观测转发（老 getter 一个不少）          │
              └───────────────┬──────────────────────────────────┬────────────────────────────┘
                              │                                  │
                    ┌─────────▼──────────┐            ┌──────────▼───────────┐
                    │  SourceSession     │            │  PacketStage 链       │
                    │  读：open/read/EOF │            │  形状：Raw / AnnexB / │
                    │  循环重开/趟偏移    │            │  Adts / Transcode     │
                    └────────────────────┘            └──────────────────────┘
```

| 文件 | 职责 | 规模 |
|---|---|---|
| `media/source_session.{h,cpp}` | 一次源的**读**：打开、读包、跳过非音视频、时间戳、EOF、**循环重开 + 趟偏移**、读侧观测 | 197 + 322 行 |
| `media/packet_stage.h` | **阶段接口**（扩展点）：`prepare` / `process` / `flush` / `onSeam` / `ownsTimeline` / `convertedPackets` / `statsJson` | 114 行 |
| `media/stage_passthrough.{h,cpp}` | `RawStage`（透传）/ `AnnexBStage`（→AVCC + avcC）/ `AdtsStage`（→raw AAC + ASC 探针） | 80 + 217 行 |
| `media/stage_transcode.{h,cpp}` | `TranscodeStage`：转码 + 自造时间轴 + 接缝修正 + speed 观测 | 104 + 281 行 |
| `media/demuxer_producer.{h,cpp}` | **只剩编排**：搭链、驱动、转发观测（原 377 + 789 行） | 188 + 412 行 |

## 3. 两条契约（重构后必须守住的）

### 3.1 时间轴：链上不许"顺手加偏移"

- **阶段只处理原始时间戳**；趟偏移由 `DemuxerProducer` 在**链尾**统一加；
- **例外**：链里有阶段 `ownsTimeline() == true`（转码自己生成"锚点 + 帧序号 × 步长"），
  此时**不许**再加偏移 —— 加了就是双计（v0.2.1 实测：`--loop` 下视频跑到 14 s、音频 8 s）。

这条写成接口上的一个虚函数而不是注释，是为了让"要不要加偏移"变成**可检查的事实**：
`pipeline_transcode_stage_owns_timeline_and_flushes` 直接断言它。

### 3.2 循环接缝：用回调，不用互相认识

`SourceSession` 不知道"形状"的存在，它只在重开时喊一声：

```cpp
_session.setSeamCallback([this](int64_t pass_length_ms) {
    for (auto &stage : _video_chain) stage->onSeam(pass_length_ms);   // 转码据此刻对齐
});
```

于是 **v0.3 的 RTSP 重连只动 `SourceSession`**，与阶段、输出层、`http/` 全都无关；
反过来，加"滤镜/水印"只需一个新阶段插进链里，`SourceSession` 也不用动。这就是两条轴分开了。

## 4. 扩展方式（这才是 OCP 要的东西）

| 要加的东西 | 要改的地方 |
|---|---|
| 一种新的**输入形状**（例如某天要做的 HEVC 参数集重建） | 新写一个 `PacketStage` + `buildChains()` 里加一支选择 |
| 一条**新的加工**（v0.3 的滤镜/水印） | 新写一个 `PacketStage`，插进链的中间（转码之前或之后都行） |
| 一种新的**读**能力（v0.3 的 RTSP 破流重连） | 只动 `SourceSession`（不动阶段、不动输出） |
| 一种新的**输出格式** | 新写一个 `IMediaSink` 实现（这条轴 v0.1 起就是闭的：M9 加 HLS 只新增文件） |

## 5. 怎么证明"没改变行为"

`scripts/refactor_equiv_check.sh`（**新增**，11 条）：

- 跑五个 demo（`flv_mux_demo` / `ts_mux_demo` / `transcode_demo`），覆盖 **remux / Annex-B / ADTS /
  B 帧源 / 转码（缩放与只重编码）/ TS 封装（含负首 dts 平移）** 这些路径；
- 对产物算 md5，与 `scripts/refactor_baseline.txt` 的基线逐条比对；
- 基线是**重构之前**在 v0.2.2 上生成的；重构后 **11/11 相同**（`md5sum` 逐条列出）。

> 这份基线今后还有用：任何"声称只是重构"的改动都要过它；过了才说明真的没改行为。
> **有意改变行为**时必须 `--save` 重新生成，并在 `CHANGELOG` 写明"为什么字节变了"。

## 6. 重构顺带修的一个小坑

`examples/flv_mux_demo.cpp` 原来用 `producer.demuxer().firstVideo()`（**源**流信息）建 `FlvMuxer`，
于是 Annex-B 输入（`.h264` / TS）一律被 `FlvMuxer::prepare()` 拒绝（`AnnexBNotSupported`）——
与它自己注释里"读包统一走 DemuxerProducer"自相矛盾。改成 `muxerVideo()/muxerAudio()`（**输出**快照）后，
TS / 裸流也能用这个 demo 验了 —— 这也是等价性脚本能覆盖 ADTS 路径的前提。

## 7. 风险与边界

| # | 风险 | 现状 |
|---|---|---|
| 1 | 拆分后"多一层调用"，性能有没有损失？ | 每包多一次虚函数调用 + 一次 vector 周转；实测单进程全量用例耗时 **14.8 s → 15.2 s**（+约 3%），在噪声量级内；转码 speed 未变（3.0x 级） |
| 2 | 转码的时间轴"双计"会不会再犯？ | `ownsTimeline()` 是接口契约 + 有专门用例断言；`producer_transcode_loop_keeps_audio_and_video_aligned` 仍在（界 300 ms） |
| 3 | 链的顺序有没有可能被写错（比如先转码后转 AVCC）？ | 目前 `buildChains()` 是唯一入口，顺序写死为「形状 → 转码」；若将来要"转码后再滤镜"，顺序需要再讨论（见下） |
| 4 | 是否已经算"完全 OCP"？ | **不是**。`buildChains()` 里仍然是一个"策略选择点"（加新形状要改这一处）。这是**刻意**的：把选择集中到一处，比"注册表 + 动态加载"更简单、更可读。要再进一步，得等第三个输入形状出现 |

## 8. 机检口径（把"违反 OCP"变成可统计的数字）

每次交付后统计对这三个目录的改动行数：

```bash
git diff --stat <上一个 tag>..HEAD -- src/media/media_source.* src/media/subscriber.* \
    src/media/frame_queue.* src/network/ src/http/
```

| 改动性质 | 阈值 | 要求 |
|---|---|---|
| 新增函数 / 只读接口 / 新增文件 | ≤ 20 行 | 正常（"接线"） |
| **改既有逻辑** | **> 30 行** | 必须在验收文档里写**为什么非改不可**，并给出替代方案为何更差 |

理由：这条规则拦不住坏设计，但它让"我为加功能改了多少老代码"在每次发版时**被迫被看见**
（v0.2 期间 `http_server.cpp` 因为内置测试页被改了 87 行，就是没有这条规则才悄悄发生的）。
