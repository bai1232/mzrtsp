# v0.2.3 验收报告（重构版）

> 这是一次**行为不变的重构**，所以验收口径不是"新功能能用"，而是：
> **同一输入 → 同一份输出字节**，且全部门禁与重构前一致。
> 设计见 [DESIGN_REFACTOR_PIPELINE.md](DESIGN_REFACTOR_PIPELINE.md)；
> v0.2 的功能矩阵见 [ACCEPTANCE_v0.2.0.md](ACCEPTANCE_v0.2.0.md)（本版**不新增**任何功能）。

| 项 | 值 |
|---|---|
| 版本 | **v0.2.3**（`Z` 位：内部重构，不改变功能与字节） |
| 日期 | 2026-10-25 |
| 结论 | `DemuxerProducer` 从 **789 行 / 22 个成员函数**拆成「`SourceSession`（读）+ `PacketStage` 链（形状）+ `DemuxerProducer`（编排）」；**11 条产物的 md5 与重构前逐条相同**；294 用例 / 25 分组全绿；ASAN 25/25；TSAN 388 用例真问题 0 |

## 0. 一页结论

| 判据 | 重构前（v0.2.2） | 重构后（v0.2.3） |
|---|---|---|
| `DemuxerProducer` 规模 | **789 + 377 行**（22 个成员函数） | **412 + 188 行**（只剩编排） |
| 新增零件 | — | `SourceSession` 322+197、`PacketStage` 114、`stage_passthrough` 217+80、`stage_transcode` 281+104 |
| **输出字节**（11 条覆盖 remux/Annex-B/ADTS/B 帧/转码/TS） | 基线 | **11/11 md5 完全相同** |
| 单进程全量 | 288/288、63,275 断言、14.8 s | **294/294、64,027 断言、15.0 s** |
| 分组 / ASAN / TSAN | 24 / 24 / 379 | **25 / 25 / 388** |
| 对"核心目录"的侵入 | — | **0 行**（`media_source`/`subscriber`/`frame_queue`/`network`/`http` 一行未动） |

## 1. 为什么重构（问题用数据说话）

`DemuxerProducer` 一个类里塞了 5 类职责（读、循环重开与趟偏移、Annex-B→AVCC、ADTS→AAC/ASC、
转码编排）。后果是**每加一件输入侧的事都要改它**：

| 里程碑 | 对 `demuxer_producer.cpp` 的改动 | 对"核心目录"的改动 |
|---|---|---|
| M8（MKV/TS 输入） | **+125 / −1** | 0 |
| M10-b（转码接线） | **+260** | `media_source` +10（新增只读 `ended()`） |
| v0.2.1（B 帧时间戳） | **+38 / −7** | 0 |

对比同期输出侧（M9 加 HLS、M10-a 加转码组件、M10-c 转码+HLS）：**全部是新增文件**，
核心目录合计只改了 29 行（19 行新增函数 + 10 行新增接口）。**输出轴是闭的，输入轴没闭** —— 本版把输入轴补上。

## 2. 拆成了什么（结构）

```
DemuxerProducer（编排：搭链 / 驱动 / 转发观测）
   ├── SourceSession    读：open / read / 跳过非音视频 / 时间戳 / EOF / 循环重开 + 趟偏移
   └── PacketStage 链   形状：Raw → （可选）AnnexB / Adts → （可选）Transcode
```

| 文件 | 行数 | 职责 |
|---|---|---|
| `media/source_session.{h,cpp}` | 197 + 322 | 一次源的**读**（v0.3 的 RTSP 重连将只动这里） |
| `media/packet_stage.h` | 114 | 阶段接口（**扩展点**） |
| `media/stage_passthrough.{h,cpp}` | 80 + 217 | Raw / AnnexB / Adts |
| `media/stage_transcode.{h,cpp}` | 104 + 281 | 转码（自造时间轴 + 接缝修正 + speed） |
| `media/demuxer_producer.{h,cpp}` | 188 + 412 | 只剩编排；**对外接口一个没变** |

**两条显式契约**（各有一组用例断言）：
1. 阶段只处理**原始**时间戳；趟偏移由编排者在**链尾**统一加，**除非**链里有 `ownsTimeline()`
   的阶段（转码）—— 加了就双计（v0.2.1 实测：视频 14 s / 音频 8 s）；
2. 循环接缝用**回调**（`SourceSession::setSeamCallback`）通知转码阶段，"读"不认识"形状"。

## 3. 等价性证据（核心）

`scripts/refactor_equiv_check.sh`：跑五个 demo，覆盖 11 条产物路径，逐条比 md5。
基线 `scripts/refactor_baseline.txt` **在重构前**（v0.2.2 的构建上）生成。

| # | 用例 | 覆盖的路径 | 结果 |
|---|---|---|---|
| 1 | `flv_mp4` | MP4 → FLV（avcC + AVCC 透传） | ✅ 同 |
| 2 | `flv_mkv` | MKV → FLV | ✅ 同 |
| 3 | `flv_ts` | TS → FLV（**Annex-B→AVCC** + **ADTS→raw AAC + ASC**） | ✅ 同 |
| 4 | `flv_vonly_ts` | 纯视频 TS → FLV | ✅ 同 |
| 5 | `flv_h264_raw` | H264 裸流 → FLV（Annex-B） | ✅ 同 |
| 6 | `flv_bframes` | B 帧源 → FLV（pts 乱序 + 负首 dts） | ✅ 同 |
| 7 | `tx_sample_120p` | 转码（缩放）→ FLV | ✅ 同 |
| 8 | `tx_sample_src` | 转码（只重编码）→ FLV | ✅ 同 |
| 9 | `tx_1080_to_720p` | 1080p→720p → FLV | ✅ 同 |
| 10 | `ts_mp4` | MP4 → TS（PAT/PMT/PES/90kHz） | ✅ 同 |
| 11 | `ts_bframes` | B 帧源 → TS（**负首 dts 整体平移**） | ✅ 同 |

> 这份基线今后仍有用：任何"声称只是重构"的改动都要过它。**有意改变行为**时必须
> `--save` 重生成，并在 `CHANGELOG` 写明"为什么字节变了"。

## 4. 新增的可测面积

重构的收益不只是"少改老代码"，还有**可测面积变大**：以前这些只能"整条链一起测"。

新分组 `pipeline`（`tests/test_pipeline_stages.cpp`，6 例 / 770 断言）：

| 用例 | 证明什么 | 什么会让它红 |
|---|---|---|
| `pipeline_raw_stage_passes_through_untouched` | 透传不动时间戳/关键帧，且**零拷贝**（`payload()` 指针相同） | 顺手加了偏移、或复制了一份载荷 |
| `pipeline_annexb_stage_builds_avcc_snapshot` | 建出 avcC（首字节 0x01）+ 逐包转 AVCC（4 字节长度前缀） | 忘转、或转成 Annex-B |
| `pipeline_adts_stage_builds_asc_and_strips_header` | 由真实 TS 造 `ASC=12 08` + 剥掉 ADTS 头 | 不剥头（同步字残留）、或不造 ASC |
| `pipeline_source_session_reads_to_eof` | 读到 EOS 的计数与**稳定终态**、`close()` 幂等 | EOF 变成 Error、重复读报错 |
| `pipeline_source_session_loop_offsets_negative_start` | **首包 dts 为负**的源跨接缝"原始+偏移"严格递增 | 趟偏移少减 `pass_start`（v0.2.1 的 bug） |
| `pipeline_transcode_stage_owns_timeline_and_flushes` | `ownsTimeline()==true`（不许双计）、flush 幂等、**进出包数相等** | 漏 flush（少最后几帧）或标错 `ownsTimeline` |

## 5. 门禁（本版实测，命令可复跑）

| 门禁 | 命令 | 实测 |
|---|---|---|
| 编译 | `cmake --build build -j4 --clean-first` | **0 warning / 0 error**（无任何 `-Wno-`） |
| 单元（串行） | `cd build && ctest --output-on-failure` | **25/25 分组、294 用例** |
| 单元（单进程全量） | `./build/bin/mzmedia_unittest` | **294/294、64,027 断言** |
| 单元（ASAN） | `cmake --build build-asan && cd build-asan && ctest` | **25/25** |
| 并发（TSAN） | `./scripts/tsan.sh` | **388 用例 | 已知误报 0 | 真问题 0** |
| **重构等价性** | `./scripts/refactor_equiv_check.sh` | **11/11 字节一致** |
| 端到端 | `scripts/{transcode,ts_mux,hls,flv_mux,http,flv_http}_test.sh` | 12/12、29/29、40/40、8/8、全绿、104/104 |

## 6. 已知影响与风险（诚实版）

| # | 项 | 说明 |
|---|---|---|
| 1 | **性能**：多一层虚函数 + 一次 vector 周转 | 单进程全量 14.8 s → 15.0 s（**+1.3%**），在噪声量级；转码 speed 未变（3.0x 级）。**没有**做逐包 benchmark |
| 2 | **`buildChains()` 仍是"策略选择点"** | 加一种新输入形状要改这一处（一行 if）。这是**刻意**的：把选择集中到一处比"动态注册"更可读；真要再进一步，等第三个输入形状出现 |
| 3 | **链的顺序**当前写死「形状 → 转码」 | 若将来要"转码后再滤镜"，顺序需要重新讨论（`DESIGN_REFACTOR_PIPELINE.md` §7 已记） |
| 4 | **`examples/flv_mux_demo` 行为变化** | 从"用源流信息"改成"用输出快照"：`.h264` / TS 输入由**被拒**变成**可用**。这是修 bug（它自己的注释早就说要走 producer），但确实是 demo 的可观测变化 |
| 5 | **`SourceSession::read()` 的"原始时间戳"语义** | 趟偏移由编排者加。写新阶段时必须守这条，否则会双计；`ownsTimeline()` 与 `refactor_equiv_check.sh` 是这两道闸 |
| 6 | 未做 | 没有把 `SourceSession` 做成接口（`IDemuxer`）—— 等第二个"读"实现（RTSP）落地后再看是否需要；也没有动 `http_server` 里内嵌的测试页（另一个独立小债，见 `DESIGN_REFACTOR_PIPELINE.md` §8 的机检口径） |

## 7. 发布信息与回滚

- **tag**：`v0.2.3`（annotated，指向本文件所在的发布提交）
- **语义化版本**：`Z` 位（内部重构，不影响功能）——`VERSIONING.md` §3 明确允许
- **回滚**：`git revert <commit>`；或 `git checkout v0.2.2`（回到单类实现，行为相同）
- 变更：见 [`CHANGELOG.md`](../CHANGELOG.md) 的 `[0.2.3]`

### 7.1 tag 独立构建与测试（`VERSIONING.md` §5）

tag 推送后**从本地仓库重新 clone**（不依赖工作区任何未提交改动）：

```bash
git clone --branch v0.2.3 /home/bpy/MyZltoolkit /tmp/v023_verify && cd /tmp/v023_verify
./scripts/make_samples.sh
cmake -B build && cmake --build build -j4
cd build && ctest --output-on-failure
```

| 步骤 | 实测 |
|---|---|
| clone | `clone=0`；`HEAD=c1247b5`；`git describe --tags` = **`v0.2.3`** |
| `make_samples.sh` | `samples=0` |
| 配置 + 构建 | `configure=0`、`build=0`，**`warnings=0`** |
| `ctest` | **25/25 通过**（294 用例） |
| 单进程全量 | **294/294、64,027 断言** |

> 回滚性：`git checkout v0.2.2` 能独立构建（回到单类实现，行为相同 —— 正因为行为相同，可用
> `scripts/refactor_equiv_check.sh` 交叉验证两个 tag 的产物字节是否一致）。
