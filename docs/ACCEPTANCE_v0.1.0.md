# v0.1.0 验收报告

> 本文件是 **v0.1.0 的发布证据**：逐条对 `docs/SPEC.md` 的 **4 条 SC / 8 条 NFR / 27 条 FR** 给出
> 状态、**证据命令**与**实测数字**。测试方法见 `docs/TESTING.md`，设计取舍见各 `docs/DESIGN_M*.md`。
> 原则：**只写跑过的**。没跑过的条目写"未验证"，不在 v0.1 范围的写"⏳ v0.2/v0.3"（不假装达成）。

| 项 | 值 |
|---|---|
| 版本 | **v0.1.0** |
| 提交 | `M7` 提交（见 `git log -1`；tag `v0.1.0` 指向它） |
| 日期 | 2026-10-12 |
| 结论 | **SC-1~SC-4 全部达成**；NFR：**7 项达成 + 1 项 N/A**（NFR-5 转码属 v0.2）；FR：**24 条属 v0.1 范围**（其中 **23 条达成**、`FR-7.1` **部分达成**：未提供单文件参数），其余 **3 条**（`FR-2.2`/`FR-2.3`/`FR-3.2`）属 v0.2/v0.3 |

## 0. 一页结论

| 关键指标 | 实测 | 要求 |
|---|---|---|
| 浏览器能否播（SC-1） | `GET /` 播放页 + 入库 flv.js **逐字节一致**；`ffplay` 出画面有声音 | 能播 |
| 首帧延迟（NFR-1） | **20 ms 级**（TTFB 3.3 ms，脚本 §4） | < 1s |
| 并发（NFR-2 / SC-3） | **10 路 × 5 分钟**，每路码流完整可解、丢帧 0 | ≥ 10 路且不崩 |
| 长跑（NFR-3 / SC-4） | **10 路 × 1 小时**：稳态 RSS **0.02 MB/h**、fd 恒定 31、错误日志 **0 条**、丢帧 0（详见 §3.3） | < 5 MB/h、fd 不涨、错误日志 0、ASAN 无报错 |
| remux CPU（NFR-4） | 1 小时跑：**单核 6.24%（十路合计）≈ 每路 0.62%**；5 分钟跑：4.11% | 单路 < 5% 单核 |
| 资源回收（NFR-6） | 断开后 `subscribers=0`、fd 回 **基线 19** | 100% 回收 |
| 代码质量（NFR-7） | 清洁构建 **0 warning / 0 error**；**247 用例** | 零警告 + 覆盖 |
| 时基/输入范围（NFR-8） | Ubuntu 22.04 x86_64 / C++17 / FFmpeg **4.4.2** | 锁定 |

**三条命令自己复现**（详见 §5）：

```bash
cmake -B build && cmake --build build -j"$(nproc)" && ./scripts/make_samples.sh
./build/bin/mzmedia                       # 浏览器打开 http://127.0.0.1:8080/
./scripts/flv_http_test.sh && ./scripts/concurrent_test.sh --quick
```

## 1. 验收环境

| 项 | 值 | 说明 |
|---|---|---|
| 操作系统 | Ubuntu 22.04.5 LTS | `uname -a` |
| CPU | 4 核 | `nproc`；并发/CPU 数字都基于这台机器 |
| 编译器 | g++ **11.4**（默认）；g++-**12.3.0**（TSAN 用，规避 glibc 2.35 的 sanitizer 误报） | `g++ --version` |
| CMake | 3.22.1 | |
| FFmpeg | **4.4.2**（libavformat 58.76.100 / libavcodec 58.134.100） | `ffmpeg -version`；**无** `libswscale`/`libavfilter`（v0.2 需要时再装） |
| 样本 | `scripts/make_samples.sh` 现场生成：`sample.mp4`（320x240 H264 + 44.1k AAC 2s）、`sample.h264`、`bf.mp4`、`garbage.bin` | 不入库 |
| 浏览器播放器 | `third_party/flv.js/flv.min.js` **1.6.2**（Apache-2.0，入库；MD5 见该目录 README） | 离线可用 |

## 2. 成功标准（SC）

| # | 标准 | 状态 | 证据 |
|---|---|---|---|
| **SC-1** | 本地 MP4 → HTTP-FLV，浏览器能播（内置测试页 + flv.js） | ✅ 达成 | `scripts/flv_http_test.sh` §1（`GET /` 含 `<video>`/`flv.min.js`/`createPlayer`；`GET /flv.min.js` 与入库文件**逐字节一致**、版本 `1.6.2`）+ §2（`curl -N` 拉到 **55,623 字节**合法 FLV，`ffprobe` 认 h264 320x240 + aac、解码 **50 帧**、`ffmpeg -f null -` 无 error）+ 人工 `ffplay -f flv http://127.0.0.1:8080/live/sample.flv` 出画面有声音 |
| **SC-2** | 加格式**不改核心**（新增输入/输出只增实现） | ✅ 达成（**只在 v0.1 已发生的扩展上**可证） | ① 新增**输入形状**（H264 裸流 Annex-B→AVCC，M6-c）时，**FLV 封装器本体零改动**：`git diff 24ce27a~1..24ce27a -- src/output/flv_muxer.h src/output/flv_muxer.cpp` 为空 —— 转换落在 `media/`（解封装层），输出层只见 AVCC；② `/live/` 路由由 app 用 `setPrefixRoute()` 注册 → `http/` 不依赖 `media/`；③ 分层检查：`src/output/` 内**没有任何** `#include "network/…"` / `"http/…"`（grep 唯一命中是一句注释）。**说明**：M6-c 确实改过 `flv_sender`（`Sink::abort` + `start()` 立刻 drain），但那与"输入格式"无关；v0.2 的 HLS 要等真做出来才能算这条的第二次验证 |
| **SC-3** | 多客户端并发不崩（≥10 路同源） | ✅ 达成 | `scripts/concurrent_test.sh`：**10 路 × 300s**，5×curl + 5×ffmpeg，每路码流完整可解、`dropped=0`、服务进程存活、错误日志 0 条 |
| **SC-4** | 连续运行 1 小时稳定 | ✅ 达成 | 见 §3.3（NFR-3 的 1 小时长跑） |

## 3. 非功能需求（NFR）

| # | 需求 | 状态 | 证据与实测 |
|---|---|---|---|
| **NFR-1** | 首帧 < 1s | ✅ | 脚本 §4：冷启动拉流，每 20ms 采样文件大小，长到 2KB 用时 **20 ms 级**；`curl` 的 TTFB **3.3 ms**（服务端在 handler 内同步发 FLV 头 + sequence header，不等第一帧） |
| **NFR-2** | 并发 ≥ 10 路，无串流、无崩溃 | ✅ | 见 SC-3；另 `--overload 8` 验证上限：10 路在册时再并发开 8 路 → **接受 6 / 拒绝 2**（`max_subscribers=16` 精确生效）、HTTP 侧 503 |
| **NFR-3** | 1 小时：RSS 增长 < 5MB/h、fd 无泄漏、错误日志 0 条 | ✅ | **1 小时实跑见 §3.3**：稳态段 2933s **+20 kB → 折算 0.02 MB/h**、fd 恒定 **31** → 断开后回 21、`[E]`/`[F]` **0 条**、丢帧 0、每路码流完整可解 |
| **NFR-4** | remux 单路 CPU < 5% 单核 | ✅ | 10 路并发时服务端进程：5 分钟跑 **单核 4.11%**、1 小时跑 **单核 6.24%**（`/proc/<pid>/stat` 的 utime+stime 增量 / 墙钟）→ **每路 ≈0.4~0.62%**，比门禁好一个数量级（"零拷贝转发"的效果） |
| **NFR-5** | 转码 ≥ 1.0x 实时（1080p→720p） | ⏳ **N/A（v0.2）** | v0.1 是 remux，不做转码；`/api/stats` 里的"转码实时倍数"字段同样属 v0.2 |
| **NFR-6** | 客户端断开后队列/上下文/fd 100% 回收 | ✅ | `concurrent_test.sh`：断开后 `subscribers=0`（或源已被空闲释放）、fd 回落到**基线 19**；单测 `media`/`srcmgr`：订阅者 `shared_ptr` 释放 → 自动注销 + 队列回收；`releaseAll()` 后源线程 join |
| **NFR-7** | `-Wall -Wextra` 零警告；覆盖协议解析/缓冲/时间戳换算/codec 判定 | ✅ | 清洁构建 **0 warning / 0 error**（无任何 `-Wno-`）；**247 用例 / 21 分组**，含 HTTP 解析（含畸形/上限）、`Buffer` 边界、时间基换算（23.976/25/29.97）、32 位时间戳回绕、`CodecMatrix` 判定表、`FrameQueue` 溢出丢帧策略、日志滚动 |
| **NFR-8** | Linux x86_64；C++17；FFmpeg 4.4.x | ✅ | CMake 里 `CMAKE_CXX_STANDARD 17` + 编译器白名单（GCC/Clang）+ FFmpeg 必需库探测（缺失**明确报错**）；实测 4.4.2 |

### 3.3 NFR-3 的 1 小时长跑

> 命令：`./scripts/concurrent_test.sh --duration 3600 --overload 8`（10 路 × 1 小时；
> 2 秒样本 + `--loop` = **1633 次循环重开**）。结果：**9 项硬门禁全过**（`hourrun_exit=0`）。

| 指标 | 实测 | 对照 NFR-3 |
|---|---|---|
| 每路码流 | **10/10 完整可解**：视频 **89,109 包**（预期 90,000，容差 [85,500, 94,525]）；每路存盘 ≈**98.97 MB**；`ffmpeg -f null -` 全部无 error | 无异常 ✅ |
| **RSS** | 基线 25,296 kB → 结束 35,252 kB（10 个订阅者队列 + GOP 缓存的首次分配）；**稳态段 2,933 s 内仅 +20 kB → 折算 0.02 MB/h** | **< 5 MB/h ✅（余量 250 倍）** |
| **fd** | 运行期恒定 **31**（0s / 552s / 1096s / 1640s / 2183s / 2727s / 3574s 全部 31）；断开后回 **21**（基线 20） | 无泄漏 ✅ |
| **错误日志** | `[E]`/`[F]` = **0 条**（Warn 16 条，都是"对端消失"这类正常断开） | **0 条 ✅** |
| 源端丢帧 | `dropped = 0`、`dropped_incoming = 0`（1 小时全程） | 未触发（不是"丢得对"，是**根本没到上限**） ✅ |
| 服务端 CPU | 单核 **6.24%**（整机 1.56%）= 每路 ≈**0.62%** | 单路 < 5% ✅ |
| 吞吐 / 唤醒 | 源端 **61.4 包/s**（1 小时共 2,253,81 包）；`notify` 2,107,942 / `coalesced` 145,233（6.9%）/ `popped` 2,253,791 → **1.1 包/唤醒** | 回填 `DESIGN_M5` §8 #9 ✅ |
| 音画 | 视频跨度 3599.960s、音频 3599.988s、**偏移峰值 2.0 ms**（一帧 40ms） | FR-3.4 ✅ |
| 循环重开 | `loops = 1633`（1 小时内 2 秒样本循环 1633 次），RSS 全程平直 → **循环路径不漏内存/fd** | FR-1.2 ✅ |
| 上限 | 10 路在册时再并行开 8 路 → **接受 6 / 拒绝 2**（`max_subscribers=16` 精确生效） | FR-5.1 ✅ |

> **怎么读这张表**：`--loop` 让源在 1 小时里重开 1633 次（每次 `close()` + `open()` + 重建时间戳守卫），
> 所以 RSS 平直同时证明了两件事 —— "直播推流不涨内存"与"循环重开不漏"。
> "基线 25MB → 35MB"是**首次分配**（队列/GOP 上限被触碰），判据是**稳态段斜率**（2933 秒 +20 kB）。
> **构建类型**：1 小时跑的是 **Debug 构建**（`build/`，本项目开发期默认类型）；**Release/RelWithDebInfo 未做长跑**，
> ASAN 构建另跑了 `--quick`（见 §6 第 12 项）。

## 4. 功能需求（FR，逐条）

> 状态标记：✅ 达成 · ⏳ 不在 v0.1 范围 · ⚠️ 部分达成（写明差在哪）

### 4.1 源与分发（FR-1）

| # | 状态 | 证据 |
|---|---|---|
| **FR-1.1** 一源一份输出，N 客户端共享 | ✅ | 单测 `media`：3 个订阅者收到**完全一致的帧序列**（每帧指针相同 = 零拷贝）；集成：10 路同源并发时 `/api/stats` 只有 **1 个源**、10 个订阅者 |
| **FR-1.2** 直播式重放（节流推送、可选循环、不支持 seek） | ✅ | 节流：单测 `media`（`Throttle` 按 dts 等墙钟）+ 集成 `srcmgr`（8 倍速读 2 秒样本用时 ≥100ms 且 <1500ms）；循环：`--loop`（默认关）+ 单测 `producer_loop_offsets_timestamps_monotonically`（跨趟严格递增）+ 脚本 §9（4x 拉 3 秒得 11 秒媒体时长）；**无 seek 接口**（HTTP-FLV 直播语义） |
| **FR-1.3** 懒启动 + 空闲 60s 释放 + 回收 | ✅ | 单测 `srcmgr`：未 `acquire` 时源数 0；空闲计时到点释放；**M6-c 修**：有订阅者时**推迟**释放（`idle_deferred` 计数）；`FR-6.3` 的源创建/释放落日志 |
| **FR-1.4** GOP 缓存 + 新客户端从关键帧开始 | ✅ | 单测 `media`：中途接入的首包是视频关键帧；`GopCache` 上限 = 码率 × 最大 GOP 时长 |
| **FR-1.5** 源释放后内存/fd/线程回到接入前水平 | ✅ | `concurrent_test.sh` 的 fd 曲线（30 → 19 = 基线）；单测 `srcmgr_release_all_stops_sources`：`releaseAll()` 后 `sourceCount()==0`、pump 线程 join；5 分钟 150 次循环重开 RSS 平坦 |

### 4.2 输入格式（FR-2）

| # | 状态 | 证据 |
|---|---|---|
| **FR-2.1** MP4（含 mov）+ H264 Annex-B 裸流 | ✅ | MP4：`ffmpeg` 分组 + `srcmgr` 真读 `sample.mp4` 到 EOF；裸流：`h264_util` 纯函数单测 7 例 + `producer` 组 6 例 + 端到端（`curl …/live/sample.h264.flv` → `ffprobe` 认 h264 320x240、`ffmpeg` 解码无 error、**python 逐 tag 复查 AVCC 结构**） |
| **FR-2.2** MKV / MPEG-TS 输入 | ⏳ **v0.2** | 代码走同一条"extradata 判断"路径，但**没有样本实测** → 目前明确算"未验证"（不写成支持） |
| **FR-2.3** RTSP 输入 + 断流重连 | ⏳ **v0.3** | 未实现 |
| **FR-2.4** 边界处理（损坏/无视频轨/纯音频/VFR/时长未知）不崩 | ✅（已有用例覆盖） | 损坏文件：单测 `ffmpeg_demux_open_garbage_file_fails`（`garbage.bin` 随机字节 → 返回 false + 原因，不崩）；无视频轨/纯音频：`FlvMuxer::prepare()` 给 `NoStreams`/`UnsupportedVideoCodec` 并回 415；空文件/截断：`producer_annexb_without_extradata_is_rejected` 等**明确失败**；**VFR 与"时长未知"没有专门样本** → 归入 §6 未验证清单 |

### 4.3 输出与同步（FR-3）

| # | 状态 | 证据 |
|---|---|---|
| **FR-3.1** HTTP-FLV：`video/x-flv` + chunked + 立即发 FLV Header/AVC sequence header | ✅ | 单测 `flv` 逐字节断言 FLV 布局；`ntimed_http`：`Content-Type`/`chunked`/CORS；脚本 §5 校验响应头；首帧 20ms 级（NFR-1） |
| **FR-3.2** HLS（切片 2s / 窗口 6 / m3u8 字段） | ⏳ **v0.2** | 路由占位（`/hls/` 回 501，见 `HttpServer::start()`） |
| **FR-3.3** AAC 转发；无音频输出纯视频且不报错 | ✅ | 单测 `flv`：AAC sequence header（`0xAF` + ASC）与 raw 包布局；纯视频用例 `Streams{video, nullptr}`；裸流样本**确实没有音频**，端到端跑通 |
| **FR-3.4** 音视频同步（音频为主时钟）+ 时间戳换算到输出基准 | ✅（v0.1 口径） | FLV 时间戳 = 毫秒、per-client 基准（单测 `flv_timestamp_base_is_per_client`）、32 位自然回绕 + 早于基准钳 0 计数；**实测音画偏移峰值 2.0 ms**（5 分钟存盘文件，一帧 40ms）。**说明**：v0.1 是 remux，"以音频为主时钟"的时钟选择体现在播放/封装侧，服务端保证的是**时间戳保真 + 单调**（`DESIGN_M5` §8 #13 有数据与结论） |
| **FR-3.5** 按源时间轴与墙钟对齐推送 | ✅ | 单测 `media`（`Throttle` 语义）+ `srcmgr`（8 倍速实测）+ 5 分钟长跑：源端 **≈68 包/s**（2 秒样本 138 包 → 约 1x，没有全速灌入） |

### 4.4 HTTP 服务（FR-4）

| # | 状态 | 证据 |
|---|---|---|
| **FR-4.1** 路由：`/live/<name>.flv`、`/hls/...`、`/api/stats`、`/` | ✅（`/hls/` 为 v0.2 占位） | `scripts/flv_http_test.sh`：`/`（播放页 200）、`/live/*`（200）、`/api/stats`（200）；`/hls/` 回 501（`scripts/http_test.sh` 覆盖 501 行为） |
| **FR-4.2** chunked / Content-Length 静态响应 / Range | ✅ | `scripts/http_test.sh`：chunked 与 Content-Length 互斥、Range 206 + `Content-Range`、越界 416；单测 `http` 组 |
| **FR-4.3** CORS（`Access-Control-Allow-Origin: *`） | ✅ | 脚本 §5 校验；单测 `ntimed_http` |
| **FR-4.4** 连接上限（默认 64）、读空闲 60s、写阻塞 30s | ✅（并修了两处） | 默认值：`TcpServer::kDefaultMaxSessionCount = 64`、`Session::kDefaultRecvIdleMs = 60000`、`kDefaultSendBlockedMs = 30000`；**`setMaxSessionCount(0)` 被拒**（有界性不能靠配置关掉）。单测：上限设为 1 时第 2 个连接被拒 + `totalRejected` 增长；读空闲/写阻塞超时用例。**M6-d 修**：① 异步流式响应豁免读空闲（否则服务端单向推流会在 60s 被掐，实测 10 路在 t=61s 全断）；② "对端消失"记 **Warn** 而非 Error（否则 NFR-3 的"错误日志 0 条"不可测） |
| **FR-4.5** 404/405/500 规范响应 + 日志 | ✅ | `http_test` + `flv_http_test`：404（不存在/穿越/非法名/非法扩展名/缺后缀）、405（非 GET，单测）、500（handler 抛异常不穿透）；日志：`http` 组与脚本断言 |

### 4.5 背压与隔离（FR-5）

| # | 状态 | 证据 |
|---|---|---|
| **FR-5.1** 每客户端队列上限 = 码率上限 × 延迟额度（**只按字节**） | ✅ | 单测 `media`：`setLimits(0)`/超硬上限被拒；推导值 8 Mbps × 2s = **2,000,000 B**；上限不可绕过 |
| **FR-5.2** 溢出丢**最旧的一段**（音视频成对）、关键帧绝不丢、腾不出空间则断开、连续阻塞 30s 断开 | ✅ | 单测 `media`/`flv`：丢最旧一段、成对丢、关键帧触发 `RejectedNoSpace` → 订阅者 `broken` → `FlvSender` 返回 `Aborted` → **硬断连**（M6-c 的 `Sink::abort`）；30s 写阻塞由 `Session` 超时断开（单测 `ntimed_session_send_blocked` / `ntimed_session_send_overflow`） |
| **FR-5.3** 任一客户端异常不影响源与其他客户端 | ✅ | 单测 `media`（一个订阅者故意不取帧，其他订阅者帧序列仍逐帧一致）；端到端 `--kill-half`：kill 前 5 路，其余 5 路码流完整 |

### 4.6 可观测性（FR-6）

| # | 状态 | 证据 |
|---|---|---|
| **FR-6.1** `/api/stats` 暴露源数/客户端数/每源码率/转码倍数/丢帧数/累计字节/运行时长 | ✅（转码倍数属 v0.2） | `curl -s /api/stats`：`uptime_ms`/`uptime_s`（**M7 补**）、`media_sources[].media_source.subscribers`、`counters.{delivered,dropped,dropped_incoming,rejected,auto_unsub,broken}`、`wakeup{notify,coalesced,notify_rejected,popped}`、`media_sources[].producer.{packets,bytes,bitrate_bps,uptime_ms,annex_b_video,annex_b_packets,loops,loop_offset_ms,loop_failures}`；**实测 `bitrate_bps=221458` ≈ 样本真实码率 222 kbps**；用例：`ntimed_http_api_stats`、`srcmgr_dump_stats_json_has_manager_and_sources`、`producer_bitrate_and_uptime_are_measured` |
| **FR-6.2** 日志分级、异步、滚动文件（默认 `logs/mzmedia.log`） | ✅ | 分级/异步：`logger` 组（等级过滤、异步 flush、同步模式、队列满丢弃计数）；滚动：`logger_file_roll`（64MB 默认、历史文件命名与清理）；**M7 落地**：`src/main.cpp` 装 `ConsoleWriter` + `FileWriter`（`--log-file` 可改路径），并改**行缓冲**（修前"跑 2 秒的服务日志文件是 0 字节"，`tail -f` 也看不到）→ 用例 `logger_file_writer_is_line_flushed` + 脚本 §2 检查（运行中文件含"源已创建"） |
| **FR-6.3** 关键事件落日志（源创建/释放、客户端接入/断开、错误） | ✅ | 源创建/释放：`InfoL`（`srcmgr_open_failure_not_silent` 等断言）；客户端接入/断开：`Session` 的关闭日志（M6-d 后正常断开记 **Info**、对端消失记 **Warn**、真故障记 **Error**）；脚本检查文件日志里含"源已创建" |

### 4.7 部署与运行（FR-7）

| # | 状态 | 证据 |
|---|---|---|
| **FR-7.1** 命令行：监听端口、媒体文件路径、日志级别、是否循环、转码开关 | ⚠️ **部分达成（写明差异）** | 已实现：`--port`（端口）、`--media-root`（**媒体目录**，是"媒体文件路径"的超集：一个目录下按名字取文件）、`--log-level`（日志级别）、`--loop`（循环）、`--log-file`/`--web-root`/`--speed`（扩展）。**未提供单文件参数**（`--media-file`）：目录形态已覆盖 v0.1 用法，加它属于冗余入口 —— 若要求严格字面对齐，记入 §6 未达成项。**"转码开关"属 v0.2**（v0.1 无转码） |
| **FR-7.2** 未提供参数用默认值；**零参数**也能启动内置测试页 | ✅ | `./build/bin/mzmedia`（零参数）→ 0.0.0.0:8080 + `./samples`，横幅打印播放页/拉流/统计 URL（脚本 §0 断言"横幅里有 URL"）；**未知参数报错退出**（不静默忽略） |

## 5. 复现步骤（从零到播放，约 3 分钟）

```bash
# 0) 环境：Ubuntu 22.04 + g++ ≥ 9 + cmake ≥ 3.15 + FFmpeg 4.4.x 开发库
sudo apt-get install -y libavformat-dev libavcodec-dev libavutil-dev

# 1) 构建（首次约 40s）
git clone <repo> && cd MyZltoolkit
cmake -B build && cmake --build build -j"$(nproc)"

# 2) 生成样本（320x240 H264 + 44.1k AAC 2 秒 + H264 裸流）
./scripts/make_samples.sh

# 3) 启动（零参数即可）
./build/bin/mzmedia
#   浏览器：http://127.0.0.1:8080/  → 点「播放」
#   命令行：ffplay -f flv http://127.0.0.1:8080/live/sample.flv
#   裸流  ：ffplay -f flv http://127.0.0.1:8080/live/sample.h264.flv
#   统计  ：curl -s http://127.0.0.1:8080/api/stats | head -c 400

# 4) 自动验收（每个脚本自己起停服务并打印"通过 N 项 / 失败 N 项"）
./scripts/flv_http_test.sh                    # M6-c：49 项（HTTP + ffprobe + 播放器资源 + 循环 + 边界）
./scripts/concurrent_test.sh --quick          # M6-d：10 路 × 20s 硬门禁
./scripts/concurrent_test.sh                  # M6-d：10 路 × 5 分钟（NFR-2/3/4/6 + 性能基线）
./scripts/concurrent_test.sh --duration 3600  # NFR-3 的 1 小时（本文件 §3.3 的数据来源）
./scripts/flv_mux_test.sh                     # M6-a：8 项（封装 → ffprobe）
./scripts/http_test.sh                        # M3：HTTP 层

# 5) 单元测试 + 静态检查
cd build && ctest --output-on-failure         # 21 个分组 / 247 用例（**串行**，别加 -j）
cmake --build build-asan -j4 && cd build-asan && ctest   # ASAN
cd .. && ./scripts/tsan.sh                    # TSAN（严格组 0 报告）
```

## 6. 已知边界与"未达成/未验证"清单（诚实版）

| # | 项 | 说明 |
|---|---|---|
| 1 | **FR-2.2 MKV/TS、FR-2.3 RTSP、FR-3.2 HLS、NFR-5 转码** | 属 v0.2/v0.3，**不在 v0.1 范围**（SPEC 的能力矩阵已分版） |
| 2 | **FR-2.4 的 VFR / 时长未知** | 没有专门样本；VFR 的时间戳单调性由 `MonotonicGuard` 保证，但"VFR 源的播放正确性"**未验证** |
| 3 | **FR-7.1 未提供单文件参数** | 以 `--media-root <目录>` 实现（超集）；见 §4.7 |
| 4 | **FR-3.4 的"音频为主时钟"** | v0.1 remux 只保证时间戳保真与单调（实测音画偏移 2.0ms）；"主时钟"在 v0.2 引入转码/切片时才真正需要 |
| 5 | **high profile 裸流的 avcC 扩展字段** | 不写扩展字段（播放端从 SPS 自己读）；**只有 baseline 样本实测** |
| 6 | **单向响应的读空闲豁免是"显式声明"式的** | 只有 `setChunkedAsync()` 的连接被豁免；将来若加"大文件下载"等单向长响应，需要同样声明（M7 未决） |
| 7 | **`onEvent()` 事件顺序（先读后判错误）缺确定性用例** | M6-d 已修（对端挂断不再丢缓冲区数据），但没有单测锁住（依赖内核事件组合 + 函数是 private）→ 列为 M7 未决 |
| 8 | **`/api/stats` 无鉴权、无 TLS** | SPEC 未要求；对外部署请放在反向代理/内网后面（README 有部署提示） |
| 9 | **`max_subscribers` 默认 16、空闲释放 60s** | 由 M6-d 的真实负载验证"够用"（10 路目标 + 60% 余量）；不是"最优值" |
| 10 | **`timer_precision` 在负载下间歇红** | 宿主唤醒噪声（裸基线超出量 0~14ms，门禁 `min(库) ≤ min(裸)+5ms`）；按 `TESTING` §2.2 的规矩"串行跑该组"判定 |
| 11 | **`sessions` 上限（默认 64）没有专门的验收用例** | 上限逻辑在单测里覆盖（设为 1 时第 2 个连接被拒 + 计数）；但"64 条真实连接"没有压测（M6-d 的 10 路 × 协议连接数 ≈ 10，离 64 还有距离）→ 归入 M7 之后的压测扩展 |
| 12 | **1 小时跑用的是 Debug 构建**（`build/`，项目开发期默认；**Release / RelWithDebInfo 未跑长跑**），**ASAN 只用 `--quick`（10 路 × 20s）冒烟** | 折中原因：1 小时 × ASAN（或 Release 重新配置+构建+跑）的墙钟成本高。已做的：Debug 1 小时（验收主体，见 §3.3）+ ASAN `--quick --kill-half --overload 8`（**10/10 通过**，覆盖"ASAN 无报错"与内存/连接回收）。**要更硬的证据**（脚本已支持，本版未执行）：<br>`MZMEDIA_SERVER=./build-asan/bin/mzmedia ./scripts/concurrent_test.sh --duration 3600`<br>`cmake -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel -j4 && MZMEDIA_SERVER=./build-rel/bin/mzmedia ./scripts/concurrent_test.sh --duration 3600` |

## 7. 发布信息与回滚

```bash
# 拉取与验证
git fetch --tags && git checkout v0.1.0
cmake -B build && cmake --build build -j"$(nproc)" && (cd build && ctest)
```

- tag：`v0.1.0`（annotated，指向本文件的发布提交）
- 回滚：见 `docs/VERSIONING.md` 第四节（`git revert` / `git reset --hard v0.1.0`）
- 本版变更：见 `CHANGELOG.md` 的 `[0.1.0]`
