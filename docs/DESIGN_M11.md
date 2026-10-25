# M11 设计：RTSP 输入 + 断流重连（v0.3-a）

> 权威契约以代码头文件为准；本文件说明"为什么"。上位需求：**FR-2.3**（v0.3 支持以 RTSP URL
> 作为输入、以 FFmpeg 为客户端拉流、须支持**断流检测与重连**）。
> 本批只做 **v0.3-a（输入侧）**；**解码处理/滤镜（v0.3-b）另批**，不在本文件范围内。

## 1. 范围

| 范围内（M11-a） | 明确不做 |
|---|---|
| `rtsp://` / `rtsps://` 作为输入（FFmpeg 当客户端）；**断流检测 + 自动重连（指数退避）**；重连期**不打断观众**；时间戳接续；`/api/stats` 暴露 RTSP 状态；凭据脱敏；入口 `--rtsp` | 滤镜/水印/帧率变换（v0.3-b）；RTSP **服务端**模式（`listen`）；多播（`udp_multicast`）；RTP 丢包重传/前向纠错；`rtsps` 的证书校验策略（沿用 FFmpeg 默认，不做额外放行） |

## 2. 事实基础（先实测，再设计）

下面每条都在本机（Ubuntu 22.04 / FFmpeg 4.4.2）用**我们未改动的 `Demuxer`** + 一个 200 行的
最小 RTSP 服务端（`/home/bpy/pb/rtsp_fake_server.py`，**待入库为验收工具**）测过：

| # | 场景 | 实测结果 | 对设计的含义 |
|---|---|---|---|
| 1 | `Demuxer::open("rtsp://…")`，服务端支持 UDP（FFmpeg 默认） | **成功**，`open` 耗时 **1140 ms**；`streams()[0]=h264 320x240 25/1`，**extradata 34 B 且是 Annex-B**（`00 00 00 01 67…`） | `rtsp://` 今天就能开；**现成的 AnnexB 阶段直接吃下**（脱壳/参数集都不用新写） |
| 2 | 服务端**只接受 TCP**（对 UDP SETUP 回 **461**） | 日志有 `SETUP failed: 461`，但 ffmpeg **自动回退 TCP** → `open=1`、正常读包 | **TCP 不是可用性前提**；`rtsp_transport` 选项是为了"确定性/排障"，不是"能不能连" |
| 3 | 半开（服务端保连接、不再发数据） | `read()` 阻塞 **5034 ms** 后返回 Error（我们 5 s 读超时通过 `interrupt_callback` 生效） | **读超时就是断流检测**，精度 ±35 ms |
| 4 | 对端关掉 RTSP **控制连接**（RTP 走 UDP） | 仍 **5005 ms** 才报错 —— 走的还是读超时 | UDP 下 `av_read_frame` 的错误路径**几乎不触发**；**超时是主要判据** |
| 5 | SDP 里**没有** `sprop-parameter-sets` | `open` 也"成功"但耗时 **5115 ms**（把 open 超时烧光）；`h264 0x0`、**extradata 0 字节**、包时间戳全 0 | 必须**前置校验**并**明确拒绝**（否则观众拿到 415 黑盒，还白等 5 s） |
| 6 | `pts/dts` 出现"首包 200 ms、后续 0/40/80…" | **`ffprobe`（独立裁判）看到同样的怪值** → 是我那个**假服务端**的 RTP 打包问题 | **撤回**"RTSP 源的 dts 不可信"这个推断；真机行为列**未验证**（见 §7） |

> 教训（记在 `RETROSPECTIVE` §3 的同一类）：**先确认测量工具本身是对的**。第 6 条差点让我
> 基于一个坏测试床去改产品代码。

## 3. 落点：只动"读"这条轴

v0.2.3 的重构已经把输入侧拆成两条互不认识的轴（`DESIGN_REFACTOR_PIPELINE.md`）：

```
SourceSession（读） ← 本批只动这里（+ 一个新文件 ReconnectPolicy）
PacketStage 链（形状：AnnexB/Adts/Transcode） ← 一行不动
DemuxerProducer（编排） ← 一行不动（除了把"网络流超时档"传下去）
输出层 / http / media 分发 ← 一行不动
```

**为什么重连必须落在 `SourceSession::read()` 里面**（不是"源退出后由 `SourceManager` 重建"）：
`SourcePump::run()` 收到 `ReadResult::Error` 就 **break**，然后 `source->endOfStream()` ——
订阅者立刻收到 EOS，**FLV 连接随之结束**（`source_pump.cpp` 的现有语义，实测过）。
"断流重连"的产品意图恰恰相反：**观众不该掉线**。所以重连在 `read()` 内部完成，
对 `SourcePump` 表现为"这次读得久一点"，绝不返回 Error。

## 4. 契约

### 4.1 入口（【我定】项）

```
--rtsp <url>[#<name>]     可重复；name 缺省 `rtsp1`, `rtsp2`…
--rtsp-timeout-ms <n>     网络流读超时（默认 10000；取值 [2000, 60000]；0 非法）
--rtsp-backoff-max-ms <n> 退避上限（默认 30000；[1000, 300000]；0 非法）
--rtsp-transport <tcp|udp|auto>  默认 auto（= FFmpeg 默认行为：先 UDP，被拒自动回退 TCP）
```

- 名字只允许 `[A-Za-z0-9_.-]`（与 `/live/` 同一套白名单）；`/live/<name>.flv` 与
  `/hls/<name>.m3u8` 因此**不需要任何改动**就能拉到 RTSP 源；
- 有 RTSP 源时，`/live/` 的 `fileExists()` 检查必须**跳过**（URL 源没有文件）；
- 入口**只来自命令行/配置**，**绝不**从 URL query 取地址（避免 SSRF：任何人能让服务端连内网）。

### 4.2 打开选项（`Demuxer::open(path, options)` 重载）

| 选项 | 值 | 理由 |
|---|---|---|
| `rtsp_transport` | `tcp` / `udp` / 不设（auto） | 默认 auto；`tcp` 用于排障或明确要求无丢包场景 |
| `stimeout` | `rtsp_timeout_ms × 1000`（微秒） | FFmpeg 4.4 的名字（5.x 改名 `timeout`）→ **换大版本必须复查**，写进风险表 |
| `max_delay` | `500000`（0.5 s） | 允许轻微乱序缓冲；不设时某些相机会抖动 |

> 重载是**新增函数**，老调用方（本地文件）零影响 —— 这也是"接线"而不是"改逻辑"。

### 4.3 断流判定

```
断流 = (av_read_frame 返回错误) 或 (一次读超过 rtsp_timeout_ms 没有任何包)
```

- 主判据是**读超时**（事实 3/4：半开与对端消失都只能靠它发现）；
- `av_read_frame` 的错误（RST/协议错）为辅；
- 判定为断流后**不**返回 Error，而是进入重连（§4.4）；
- **不引入 RTSP 保活探针**（OPTIONS/GET_PARAMETER）：事实 3 证明超时已经够快（5 s 级），
  保活要自己实现 RTSP 事务，复杂度与可测性都不划算 → 记为"将来若真机发现超时不够灵敏再上"。

### 4.4 重连策略（`ReconnectPolicy`，**纯逻辑、可单测**）

```
退避序列：1s → 2s → 4s → 8s → 16s → 30s（封顶，--rtsp-backoff-max-ms 可调）
无随机抖动（可测性优先；这个规模抖动收益不明显）
成功判据：重连后**读到第一个包**才算成功 → 退避重置为 1s
失败继续：失败不改变"当前段"的时间轴状态，下一轮继续退避
```

新文件 `media/reconnect_policy.{h,cpp}`（不依赖 FFmpeg、不依赖线程）：

```cpp
class ReconnectPolicy {
public:
    struct Config { int64_t initial_ms = 1000; int64_t max_ms = 30000; int max_attempts = 0; };
    int64_t nextDelayMs();      // 调用一次 = 记一次失败，返回该等多久
    void onSuccess();           // 读到第一个包 → 退避复位
    uint64_t attempts() const;
};
```

**为什么单独成类**：它是唯一可以"纯逻辑 + 无 IO"测出来的部分（`pipeline` 组的做法），
`SourceSession` 里只留"什么时候调它"。

### 4.5 重连期的观众语义（【我定】项）

| 行为 | 选择 | 理由 |
|---|---|---|
| 源线程 | **不死**（`read()` 内部等待+重连） | `SourcePump` 见 Error 就会 `endOfStream()` → 观众掉线（事实见 §3） |
| 订阅者 | **不收 EOS**，队列保持 | 观众侧表现为"卡住"而不是"播放结束" |
| FLV 连接 | 保持 | 重连成功后就地继续推 |
| HLS 切片 | 暂停产出；恢复后继续（窗口自然滑空） | ⚠ 播放器可能在窗口滑空后报错重取 m3u8 —— 真机/浏览器行为**未验证** |
| 时间戳 | **接续**：新一段的 dts 接到上一段末尾（复用循环重开那套"趟偏移"） | 不接续 = 播放器看到时间戳倒退 → 花屏/卡死 |
| 可观测 | `/api/stats` 的 `rtsp` 段（见 §4.7） | "卡住"必须能被看出来，否则排查只能靠猜 |

> 可选（**待拍板**）：重连超过 N 秒（比如 30 s）仍失败时，是否**主动发 EOS** 让播放器重连？
> 推荐：**先不发**，但把 `last_packet_age_ms` 暴露出来，由运维判断；理由见 §9 的问题 3。

### 4.6 时间戳接续（复用已有机制）

`SourceSession` 已有"趟偏移"（`pass_end − pass_start`，用于循环重开）。重连时新的一段
**同样要接在上一段之后** —— 所以把"重连"也当成一次"接缝"：

```
重连成功那一次 → 用同一个 passStartMs/passEndMs 机制算出 pass_length，
                 _loop_offset_ms += pass_length，并回调 onSeam(pass_length)
```

好处：**转码的时间轴对齐逻辑一行不改**（它本来就处理"接缝"）。代价：`SourceSession` 里
"循环重开"与"重连"共用同一个偏移变量 —— 语义上它们确实是同一件事（**源时间轴不连续**）。

### 4.7 观测（`/api/stats`）

```json
"producer": { ..., "rtsp": { "state": "playing|connecting|reconnecting|failed",
                             "connects": 3, "reconnects": 2, "attempts": 2,
                             "last_error": "…", "backoff_ms": 4000,
                             "last_packet_age_ms": 120 } }
```

文件源没有 `rtsp` 段（或 `"rtsp": null`），不制造假字段。

### 4.8 明确拒绝（**不猜**）

| 情况 | 行为 |
|---|---|
| 打开后 `width/height == 0` 或 H264 的 `extradata` 为空（SDP 没给 `sprop-parameter-sets`） | **明确拒绝**：`"对端没有给出 SPS/PPS（SDP 缺 sprop-parameter-sets）→ 无法构造 avcC"`；**不**去码流里"等一个关键帧抠参数集"（那是另一档复杂度，无样本前不做） |
| 动态码率/动态分辨率（`width/height` 在流中变化） | 现有 `Demuxer` 只在 open 时快照；**不处理**，记为已知限制（真机少见，`ffmpeg` 会 Warn） |
| 非 H264 / 非 AAC | 走 `CodecMatrix`：允许转码时转（M10 已有），否则 415 |
| 凭据 | `rtsp://user:pass@host/...`：**键**保留完整 URL（唯一性），**日志/`/api/stats`/错误信息一律脱敏**为 `rtsp://user:***@host/...` |

## 5. 批次拆分（每批一个 commit + 一次确认）

| 批次 | 内容 | 验收 |
|---|---|---|
| **M11-a1 ✅ 已完成**：能连上、能拉 | `Demuxer::open(path, options)` 重载 + 网络流 Limits 档 + §4.2 选项 + §4.8 前置校验 + `--rtsp` 入口 + `rtsp` 观测字段（状态恒 `playing`） | 假服务端 4 场景（UDP / TCP-only / 无 SPS-PPS / 关连接）；`ffprobe`/`ffmpeg` 从 `/live/<name>.flv` 拉到合法 FLV；`hls` 也能切 |
| **M11-a2**：断流能自愈 | `ReconnectPolicy`（新文件）+ `SourceSession` 接入 + 时间戳接续 + 状态机 + 退避日志 | 假服务端：`--after close` / `--after silent` → 自动重连、观众不掉线、重连后 dts 严格递增；退避序列可断言（日志或 stats） |
| **M11-a3**：验收与收尾 | `scripts/rtsp_test.sh`（假服务端驱动，含"半开"场景）+ 入库假服务端 + 文档（TESTING/ROADMAP/ACCEPTANCE）+ 全量门禁 | 脚本全绿；`refactor_equiv_check.sh` 11/11（证明本地文件路径零影响）；1 小时长跑（RTSP 源） |

## 5.1 M11-a1 实测记录（2026-10-25）

| 项 | 结果 |
|---|---|
| 正常源 → `/live/cam1.flv` | **200**，`ffprobe` 认 `h264 320x240`，`ffmpeg -f null -` **零 error** |
| 同源 → `/hls/cam1.m3u8` | 播放列表有切片（假源需 `--loop` 让关键帧周期出现），切片 `h264` 且解码零 error |
| `/api/stats` 的 `rtsp` 段 | `state=playing`、`connects=1`、`last_packet_age_ms` 实时更新 |
| **缺 SPS/PPS** | **11 ms** 就返回 500 + 点名 `sprop-parameter-sets`（修前：`find_stream_info` 空等到 open 超时，实测 **10.07 s**） |
| TCP-only 源（461） | 仍能拉（FFmpeg 自动回退 TCP） |
| 未注册名字 | 404（URL 不能从请求注入） |
| **凭据脱敏** | 日志与 stats 均无明文密码；日志里可见 `user:***@` |
| 脚本 | `scripts/rtsp_test.sh` **22/22**；`scripts/rtsp_fake_server.py` 已入库（零第三方依赖） |

实现要点（与设计的两处偏差，都是**改进**）：
1. `Demuxer::Options::require_header_parameters`（设计里没有）：网络源在**打开阶段**就要求有
   视频参数，否则立刻失败 —— 否则 `find_stream_info` 会因为"H264 没有 SPS/PPS 解不出帧"
   而**既不达 probesize 也不达 analyzeduration**，只能空转到超时（实测 10 s）。
   本地文件默认关闭，行为不变（MKV/TS 的宽高本来就得靠 `find_stream_info`）。
2. 脱敏从"日志"扩到"**所有出口**"：`SourceSession::setError` / `DemuxerProducer::setError`
   / `SourceManager` 的日志与 stats / `HlsService` 的日志 —— 因为 HTTP 响应会把
   `lastError()` 原样带给客户端。

## 6. 测试计划

| 层级 | 用例 | 证明什么 / 什么会让它红 |
|---|---|---|
| 单测 `rtsp`（新组） | `rtsp_reconnect_policy_backoff_sequence` | 退避 1,2,4,8,16,30,30…；成功后复位。红 = 序列错或没封顶 |
| 单测 | `rtsp_open_rejects_missing_sprop` | 缺 SPS/PPS → 明确失败且**不**白等满超时（用假服务端 `--no-sprop`） |
| 单测 `pipeline` | `pipeline_source_session_reconnects_after_silence` | 半开→重连→继续读；红 = read 返回 Error（观众会掉线） |
| 单测 | `pipeline_source_session_reconnect_keeps_timestamps_monotonic` | 重连后 dts 接续；红 = 倒退 |
| 脚本 `rtsp_test.sh` | ① 正常拉流（`ffprobe` 认 h264/分辨率）② **TCP-only**（461 回退）③ 无 SPS-PPS → 415 ④ `--after close` → 自动恢复 ⑤ `--after silent` → 自动恢复 ⑥ `/api/stats` 的 `rtsp` 段 ⑦ 退避序列出现在日志 ⑧ 凭据脱敏（日志/统计里无密码明文） | 真源不可控 → 用假服务端把"断流"变成**可注入**；红 = 重连没发生/观众掉线/时间戳倒退 |
| 回归 | `flv_http` 104 项、`hls` 40 项、`ts_mux`、`transcode`、`refactor_equiv_check` 11 条 | 本地文件路径**零影响** |

**入库的验收工具**：把 `/home/bpy/pb/rtsp_fake_server.py` 整理进 `scripts/rtsp_fake_server.py`
（纯 python3 标准库，无第三方依赖），并在 `TESTING.md` 写明"它能证明什么、不能证明什么"
（能：协议行为/断流注入/缺参数集；不能：真机时间戳、UDP 丢包、鉴权）。

## 7. 风险清单

| # | 风险 | 触发条件 | 现状/应对 |
|---|---|---|---|
| 1 | **真机的时间戳与假服务端不同** | 真实摄像机（含 B 帧/乱序 RTP） | **未验证**（事实 6）；真机上先看 `ptsOutOfOrder`/`dtsClamped` 计数，再决定是否要"dts 重建" |
| 2 | **UDP 丢包**（RTP 无重传） | 网络抖动 | 如实上报（`av_read_frame` 错误/超时），**不修复**；文档写清"UDP 丢包不在本版范围，建议 `--rtsp-transport tcp`" |
| 3 | **重连风暴** | 对端长期挂掉 + 退避上限太大/太小 | 退避封顶 30 s + `attempts` 计数；`--rtsp-backoff-max-ms` 有界 |
| 4 | **重连后花屏/时间戳倒退** | 对端重连后时间基归零 | 时间戳接续（§4.6）；若对端 `dts` 也归零，"接续"会人为造出巨大间隔 → 需真机观察 |
| 5 | **观众"卡住但不报错"** | 重连期长 | 保持连接是刻意选择；`last_packet_age_ms` 暴露；可选"超时发 EOS"留给运维决定（§9 问题 3） |
| 6 | **FFmpeg 选项名随版本变化** | 升级到 5.x/6.x | `stimeout` → `timeout`；版本锁定 4.4.x 已写进 `README`/`NFR-8`，升级时复查 |
| 7 | **凭据泄露** | URL 带密码 | 脱敏函数 + 脚本断言（§6 第 8 项） |
| 8 | **SSRF** | 若做成 URL query 入口 | **不做**：入口只来自命令行（§4.1） |

## 8. 假设清单

| # | 假设 | 若不成立会怎样 |
|---|---|---|
| 1 | **输入规模**：1–2 路 RTSP（4 核 VM），单路 1080p 以内 | 多路时 CPU/带宽要重新评估；`/api/stats` 已有 `bitrate_bps` 可看 |
| 2 | **调用方式**：`--rtsp` 启动期注册（不加/删源的热插拔） | 需要热插拔就得做配置重载，属另一批 |
| 3 | **运行环境**：FFmpeg 4.4.x（`stimeout` 语义）；无第三方 RTSP 服务端依赖（验收用自带的假服务端） | 换 5.x 要改选项名；真机验收需要一台摄像机或 `mediamtx` |
| 4 | **失败容忍**：对端不可达时**一直重试**（不设总次数上限）直到进程退出；每次失败都计数 + 退避 + **可观测**，绝不静默 | 若要求"打 N 次就放弃"，加 `--rtsp-max-attempts`（有界，0 非法）即可 |

## 9. 待拍板（≤5 问，各带推荐）

1. **入口形态**：`--rtsp <url>[#name]`（推荐）还是给一个 `sources.conf` 配置文件？
2. **默认 transport**：`auto`（FFmpeg 默认：UDP 优先、被拒回退 TCP，推荐）还是**默认强制 `tcp`**（无丢包、延迟稍高，对监控场景更稳）？
3. **重连太久要不要主动 EOS**：推荐"**不**主动 EOS，只暴露 `last_packet_age_ms`"；若你更希望播放器自己重连，我可以加 `--rtsp-eos-after-ms`（有界、默认关闭）。
4. **网络流读超时默认值**：推荐 **10 s**（事实 3/4 里 5 s 已能发现，但真实相机的关键帧间隔可能 2–4 s，10 s 更稳）；你若要求更快恢复，可给 3–5 s。
5. **真机验收**：本机只有那台假服务端。要不要我按"假服务端 + 1 小时长跑"验收，并在文档里明确写「真机未验」；还是你那边能提供一台 RTSP 源（型号/URL）做真机 1 小时？

---

## 附：与既有文档的对齐

- `SPEC.md` **FR-2.3**（本节即其实现设计）；
- `ROADMAP.md` v0.3.0 的"RTSP 输入"行；
- `TESTING.md` 的 FR-2.3 行（本批**新增**：原表里没有这一行 —— 是我之前三方对齐机检时
  `grep "FR-2.3"` 被正则的 `.` 匹配到 `NFR-2/3` 造成的**假通过**，已修正为 `grep -F`）。
