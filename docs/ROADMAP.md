# 路线图

> 版本号严格遵循 [VERSIONING.md](VERSIONING.md)：`vX.Y.Z`，能力矩阵中的 `v0.Y` 对应语义化版本的 `Y` 位递增。

## v0.1.0 —— MP4/H264 → HTTP-FLV（当前版本）

**目标**：达成成功标准 SC-1（本地 MP4 → HTTP-FLV，浏览器能播）与 SC-3（多客户端不崩）。

| 阶段 | 任务 | 产出 | 验收标准 |
|---|---|---|---|
| **M1** Core | 日志（分级/异步/滚动）、util、`Ticker`、`TaskQueue`、`ThreadPool`、`Semaphore` | `libmzmedia` 骨架 | 单测：日志分级与滚动、线程池 1000 任务无丢失、任务队列 abort 正确 |
| **M2** Network | `PipeWrap`、`EventPoller`（epoll ET/LT、跨线程 `async`）、`Timer`、`Socket`、`Buffer`、`TcpServer`、`Session`、**连接空闲检测与心跳钩子（FR-4.4：读空闲 60s / 写阻塞 30s）+ TCP KeepAlive** | 可用的 TCP 服务端 | `echo` 示例用 `nc` 回显 100MB 无错；定时器精度：**0 早触发** + 相对宿主裸 `nanosleep` 基线的增量 ≤5ms（绝对 ±10ms 门禁在本宿主实测不可达，数据见 `docs/DESIGN_M2.md` §7.1 / §8 R11）；**空闲超时按 FR-4.4 触发，断开后 fd 回落（NFR-6）** |
| **M3** Http | `HttpParser`、`HttpServer`、`HttpConnection`、chunked、CORS、Range、内置测试页 | 可访问的 HTTP 服务 | `curl` 取到测试页；chunked 响应可被 `curl -N` 流式接收；**404/405/400/414/431 规范响应，且解析错误后真的关连接（FR-4.5 / FR-4.4）** |
| **M4** FFmpeg 封装 | `AvPtr` 系列（RAII）、`Demuxer`、`CodecMatrix`、时间基换算 | 能解封装 MP4/H264 | 单测：从样本 MP4 解出正确的流参数；裸流能提取 SPS/PPS 并报出分辨率 |
| **M5** Media | `MediaPacket`、`FrameQueue`、`GopCache`、`Subscriber`、`MediaSource`（**M5-a** ✅）；`FrameQueue` 加锁成 SPSC + 订阅者绑定消费者线程 + **唤醒合并** + `SourcePump` 源线程（**M5-b**，设计见 `docs/DESIGN_M5.md`）；`SourceManager` 懒启动 / 复用 / 空闲释放 + `DemuxerProducer` 真读 MP4（**M5-c** ✅，**FR-2.1** / 按需启动 **FR-1.2**；源创建 / 释放落日志 **FR-6.3**）；节流 **FR-3.5** ✅（按源时间轴与墙钟对齐，默认开、`speed` 可调）+ `dumpStatsJson()` 经 `HttpServer::setExtraStatsProvider()` 接进 `/api/stats` **FR-6.1** ✅ + `broken()` 巡检（真正断连留 M6）（**M5-d** ✅） | 一源多消费者分发 | 单测（分组 `media` + `ntimed_media`，清单见 `docs/DESIGN_M5.md` §5）：3 个订阅者收到**完全一致**的帧序列（每帧指针相同 = 零拷贝）；慢订阅者丢帧但不影响他人（**FR-5.2**：丢**最旧的一段**、音视频**成对**，**视频关键帧绝不丢**，腾不出空间则断开该订阅者）；每个订阅者队列上限 = **码率上限 × 延迟额度**（初值 8 Mbps × 2 s = 2MB，**只按字节**，**FR-5.1**），非法值被拒；任一订阅者异常不影响源与其他订阅者（**FR-5.3**）；订阅者释放后自动注销、队列 100% 回收（**NFR-6**）；**源线程 ⇄ 消费者线程之间数据不丢不重不卡、唤醒不丢也不刷**（M5-b） |
| **M6** Output | `FlvMuxer`（**M6-a** ✅：纯函数式 FLV 封装 **FR-3.1** / **FR-3.3** / **FR-3.4**，设计见 `docs/DESIGN_M6.md`）；`FlvSender` + `/live/<name>.flv` 接线（**FR-4.1**）+ `HttpResponse` 异步流式/附件 + `Session::shutdownAfterFlush()`（**M6-b** ✅）；**最小 app `src/main.cpp`（零参数启动 FR-7.2 / 命令行含日志级别 FR-7.1）+ 测试页 flv.js 1.6.2（入库）+ 循环播放 `--loop`（默认关）+ Annex-B 裸流→AVCC（FR-2.1）+ `broken()` 真实断连 + stats 接线（M6-c ✅）**；10 路 / 5 分钟验收 + 回填 M5 未决（**M6-d ✅**，`scripts/concurrent_test.sh`） | **能播了** | **M6-a ✅**：`scripts/flv_mux_test.sh` **8/8**（`ffprobe` 认产物）；**M6-b ✅**：`scripts/flv_http_test.sh` —— 经过 **HTTP + chunked + 连接关闭**之后拉到的仍是一份合法 FLV（**55,623 字节**、h264 **320x240** + aac、解码 **50 帧**、`ffmpeg` 无 error、断开后 `subscribers=0`）；**M6-c ✅**：同一脚本扩到 **49/49** —— 播放页与 `/flv.min.js` 可取（逐字节一致）、**H264 裸流也能拉出合法 FLV**（`ffprobe` + `ffmpeg` + python 三重判定）、**FR-7.2 零参数启动横幅打印 URL**、**NFR-1 首帧 < 1s**（实测 20ms 级）、**`--loop` 3 秒 @4x 得到 11 秒媒体时长且时间戳跨趟单调**、源结束后再拉**不挂连接**；`ffplay` 出画面、有声音；**M6-d ✅**：`concurrent_test.sh` 10 路 × 5 分钟（`--loop` = 150 次重开）**每路码流完整可解、丢帧 0、断开后 fd 回落到基线、错误日志 0 条**；服务端 **CPU 单核 4.11%（十路合计）**、RSS 稳态 **0.36 MB/h**（NFR-3 要求 <5MB/h）、`max_subscribers=16` 恰好生效（第 17 路 503）；`DESIGN_M5` §8 未决**逐条回填**（不加包数硬顶 / 不升级锁粒度 / 唤醒合并不用改 / 节流基准不用改） |
| **M7** 收尾 | 集成验收、`scripts/` 验证脚本、文档补齐、性能数据采集 | 可发布版本 | 全部 SC/NFR 验收通过；`v0.1.0` 打 tag 并推送 |

**M6 是项目的"第一个里程碑式胜利"** —— 在此之前一切都在摸黑，之后都是增量。

## v0.2.0 —— 多输入 + HLS + 转码（✅ **已发布 2026-10-25**，tag `v0.2.0`）

> 发布证据：**[docs/ACCEPTANCE_v0.2.0.md](ACCEPTANCE_v0.2.0.md)**。下面每一行都指向实际验过的命令与数字。

| 任务 | 说明 |
|---|---|
| **M8 ✅ MKV / TS 输入（FR-2.2）** | 复用 M4 的 `Demuxer`，`CodecMatrix` 增补组合。TS 的 AAC 是 ADTS（无 extradata）→ 新增 `aac_util`（ADTS 头解析 / ASC 构造 / 逐帧剥头）+ `DemuxerProducer` 的音频 REPACK（用一次性探针 Demuxer 取首帧 ADTS 头，**不改主读游标**）。**实测**：MKV / TS / 纯视频 TS 三条都能拉出合法 FLV；ASC=`12 08`；见 `CODEC_MATRIX.md` §6.1 |
| **HLS 输出**（**FR-3.2** / **FR-4.1** / **FR-4.2** / **FR-3.4**） | **M9-a ✅** `MpegTsMuxer` + **M9-b ✅** `HlsWriter`/`HlsService`：2s 目标切片（只切视频关键帧）、窗口 6 片滚动、`.m3u8`（`TARGETDURATION`=实测最大 / `MEDIA-SEQUENCE` 前进 / EOS 加 `ENDLIST`）、`/hls/` 路由 + 切片 Range、租约 60s 空闲释放。验收 `ts_mux_test.sh` **29/29** + `hls_test.sh` **32/32**（含播放页与 `hls.js` 入库的逐字节比对 + 内联 JS 的 `node --check`）。设计见 `docs/DESIGN_M9.md` |
| **转码**（**NFR-5** / **FR-6.1**（实时倍数）/ **FR-7.1**（转码开关）） | **M10-a ✅** `Transcoder`（`src/ffmpeg/transcoder.*`）：解码 → `libswscale` → x264（`veryfast`+`zerolatency`+无 B 帧，`GLOBAL_HEADER`），现场由编码器 extradata 构造 `avcC`、逐包转 AVCC，**自己生成 CFR 输出时间轴**（输入 pts 会被 `Demuxer` 的单调守卫钳坏，详见 `docs/DESIGN_M10.md` §3.3）。**实测**：1920x1080→1280x720，125 帧一帧不少，`ffmpeg -f null -` 零 error，`speed` **2.89x**（74 帧/s）≥ NFR-5 的 1.0x；`transcode_test.sh` **12/12**；需先 `apt install libswscale-dev`（已装，CMake 缺库直接 `FATAL_ERROR`） |
| 转码调度（**FR-1.3** / **FR-7.1**） | **M10-b ✅** 接进 `DemuxerProducer`（视频过 `Transcoder`、音频透传）与 `SourceManager`（转码源 = **独立源键**，`路径|h=720`）：lazy 启动（`acquire` 才建编解码器）、并发**硬上限**（默认 `nproc-1`，`0` 非法）、EOS 后让出名额；`--transcode` / `--transcode-height` / `--transcode-max`；URL `?transcode=1&h=`；`/api/stats` 的 `transcode.speed`。**不做**：转码+HLS（M10-c）、音频转码（SPEC §9） |
| 验收（**NFR-5** / **SC-2** 的第二次验证） | **M10-a ✅** 转码 ≥1.0x 实时（1080p→720p x264 veryfast **2.89x** 端到端 / 服务内 `speed` **3.26x**）；**M10-b ✅** 转码流能播（`ffprobe` 认 h264 + aac、`ffmpeg -f null -` 零 error、HEVC→H264 生效）；新增输出/输入**不改 `media/` 分发层、`network/`**（`Transcoder` 只碰 `ffmpeg/`；转码源复用同一套 `MediaSource`） |

## v0.2.1 —— B 帧源的时间戳（✅ **已发布 2026-10-25**，tag `v0.2.1`）

> 补丁版：只修缺陷。证据见 **[docs/ACCEPTANCE_v0.2.1.md](ACCEPTANCE_v0.2.1.md)**。

| 缺陷 | 影响 | 修法 |
|---|---|---|
| `Demuxer` 把 **pts 也**做单调钳制（`_pts_guard`） | B 帧源的显示顺序被篡改：CompositionTime 从 `{80,160,40,40}` 变成 `{80,160,120,80}`，**画面顺序错乱但不报错**（x264 默认带 B 帧 → 现实里的 MP4 大多踩中） | pts 不钳（乱序只计数 `ptsOutOfOrder()`）；dts 继续钳 |
| `MpegTsMuxer` 逐包把负时间戳钳到 0 | 前几帧 dts 差异被抹掉（`-80,-40,0,40` → `0,0,0,40`，三个 PES 的 dts 完全相同） | 首包确定**整体平移量**，之后所有包 + base（差值不变）；仍为负的才钳 0 并计数 |
| 循环重开的趟偏移没算首包 dts | 首包 dts 为负的源在**每个接缝倒退**（961 → 921 ms），循环播放每次抖一下 | 偏移改用 `pass_end − pass_start`；转码路径的接缝修正改成增量 |
| **验收缺口**（根因）| 所有脚本/用例都用**无 B 帧**的 `sample.mp4` → 门禁全绿却测不到这条 | 新增 `bf.mp4` 的分层用例 5 例 + `flv_http_test.sh` §12（5 项），并加了 3 组变异验证 |

## v0.3.0 —— RTSP 输入 + 解码处理

| 任务 | 说明 |
|---|---|
| RTSP 输入 | FFmpeg 作为客户端拉流；断流检测与**自动重连**（指数退避）；编码探测以实际码流为准 |
| 解码处理 | `Filter` 链：缩放、水印、帧率变换（`libavfilter`，**需先 `apt install libavfilter-dev`**） |
| 验收 | 从一台 RTSP 源（可用 `ffmpeg -re -f rtsp` 本机自建）拉流并转出 HTTP-FLV，连续 1 小时不断流 |

## 需求追溯（SPEC → 里程碑 → 验收证据）

> **用途**：一眼看出"每条需求由哪个里程碑落地、用哪条证据验的"。三方对齐机检
> （`SPEC.md` + 本文件 + `TESTING.md` 三处都提到该 ID）就是靠这张表兜住的。
> 状态：✅ 已达成本版 · ⏳ 后续版本。

| 需求 | 里程碑 | 验收证据（命令 / 用例） |
|---|---|---|
| **SC-1** 浏览器能播 | M6-c（FLV）/ M9-c（HLS） | `flv_http_test.sh` §1/§2、`hls_test.sh` §1b（两个播放器逐字节一致 + `node --check`） |
| **SC-2** 加格式不改核心 | M6-c / M9 / M10 | `ACCEPTANCE_v0.2.0.md` §4.0（`git diff` 计数 + 分层 grep） |
| **SC-3** ≥10 路并发不崩 | M6-d | `concurrent_test.sh`（10 路 × 1 小时）+ ASAN `--quick --kill-half --overload 8` |
| **SC-4** 1 小时稳定 | M7 | `concurrent_test.sh --duration 3600`（§3.3 的 9 项硬门禁） |
| **FR-1.1** 一源一份输出、N 客户端共享 | M5 | 单测 `media`（3 订阅者帧指针一致）+ `/api/stats` 只 1 个源 |
| **FR-1.2** 直播式重放（节流/循环/无 seek） | M5-c / M6-c | `producer_loop_offsets_timestamps_monotonically`、`flv_http_test.sh` §9 |
| **FR-1.3** 懒启动 + 空闲 60s 释放（+ 转码并发上限） | M5-c / M6-c / **M10-b** | `srcmgr` 组 15 例、`hls_test.sh` §9（租约）、`flv_http_test.sh` §11.4（503） |
| **FR-1.4** GOP 缓存、从关键帧起播 | M5 | 单测 `media`、`hls_test.sh` §5 |
| **FR-1.5** 释放后资源回基线 | M5-c / M7 | `concurrent_test.sh` fd 曲线、`srcmgr_release_all_stops_sources` |
| **FR-2.1** MP4 + H264 裸流 | M4 / M6-c | `ffmpeg` 组、`producer` 组、`flv_http_test.sh` §2/§3 |
| **FR-2.2** MKV / MPEG-TS 输入 | **M8** | `flv_http_test.sh` §3b（17 项）、`ffmpeg` 组 5 例 + `producer` 组 2 例 |
| **FR-2.3** RTSP 输入 + 重连 | ⏳ M11（v0.3） | —（未实现） |
| **FR-2.4** 边界（损坏/纯音频/VFR/时长未知）不崩 | M4 / M8 / M10 | `ffmpeg_demux_open_garbage_file_fails`、`producer_transcode_rejects_bad_requests`；VFR 无样本（未验证） |
| **FR-3.1** HTTP-FLV + 立即发 sequence header | M6-b / M6-c | `flv` 组逐字节、`flv_http_test.sh` §5（首帧 20–60 ms） |
| **FR-3.2** HLS：2s 切片 / 窗口 6 / m3u8 字段 | **M9** | `hls_test.sh` 32 项 + `hls` 组 6 例 + `ts_mux_test.sh` 29 项 |
| **FR-3.3** AAC 转发 / 无音频出纯视频 | M6-b / M8 | `flv` 组、`vonly.ts` 端到端、转码时音频透传（88→88） |
| **FR-3.4** 音画同步 + 时间基换算（FLV ms / TS 90kHz）+ **B 帧显示顺序** | M4 / M9-a / M10 / **v0.2.1** | `mpegts` 组（90kHz / 回绕 / +90000 偏移 / **负首 dts 整体平移**）、`ts_mux_test.sh` 29 项、`flv_http_test.sh` §12（B 帧 cts + 循环接缝）、循环+转码对齐用例 |
| **FR-3.5** 按源时间轴与墙钟对齐推送 | M5（`Throttle`） | 单测 `media`、`srcmgr` 8 倍速、1 小时跑 68 包/s |
| **FR-4.1** 路由 `/live/` `/hls/` `/api/stats` `/` | M3 / M6-b / **M9-b** | `flv_http_test.sh`、`hls_test.sh`、`http_test.sh` |
| **FR-4.2** chunked / Content-Length / Range | M3 / M9-b | `http_test.sh`、`hls_test.sh` §4（切片 Range 206 + 188 B） |
| **FR-4.3** CORS | M3 | `http_test.sh` §2、`flv_http_test.sh` §5 |
| **FR-4.4** 连接上限 / 读空闲 / 写阻塞 | M3 / M6-d | 单测 `http`/`ntimed_http`、`--overload` 集成 |
| **FR-4.5** 404/405/500 规范响应 + 日志 | M3 / v0.1.1 | `flv_http_test.sh` §6、`http_test.sh`、`ACCEPTANCE_v0.1.1.md` |
| **FR-5.1** 每客户端队列上限（只按字节） | M5 | 单测 `media`（上限不可绕过） |
| **FR-5.2** 丢最旧一段 / 关键帧不丢 / 阻塞断开 | M5 / M6-c | 单测 `media`/`flv`、`ntimed_session_send_blocked` |
| **FR-5.3** 隔离性 | M5 | 单测 `media`、`--kill-half` |
| **FR-6.1** `/api/stats` 全量字段（含转码倍数） | M7 / **M10-b** | `flv_http_test.sh` §7/§11.5、`hls_test.sh` §8 |
| **FR-6.2** 日志分级 / 异步 / 滚动 | M2 / M7 | `logger` 组 + 运行中日志文件断言 |
| **FR-6.3** 关键事件落日志（含转码启动/失败） | M5-c / M7 / M10-b | `srcmgr` 断言 + 日志检查 |
| **FR-7.1** 命令行（端口/目录/日志/循环/**转码开关**） | M6-c / **M10-b** | `--help` 逐项 + `flv_http_test.sh` §10/§11 |
| **FR-7.2** 默认值 / 零参数启动测试页 | M6-c | 脚本 §0（横幅含 URL）、零参数启动 |
| **NFR-1** 首帧 < 1s | M6-c | `flv_http_test.sh` §4（20–60 ms） |
| **NFR-2** ≥10 路并发无串流 | M6-d | `concurrent_test.sh`（1 小时） |
| **NFR-3** 1 小时 RSS/fd/日志 | M7 | `concurrent_test.sh --duration 3600` |
| **NFR-4** remux CPU < 5%/路 | M7 | 同上（CPU 折算） |
| **NFR-5** 转码 ≥ 1.0x（1080p→720p） | **M10** | `transcode_test.sh`（**3.03x**）+ `/api/stats` 的 `speed`（**3.26x**） |
| **NFR-6** 断开后 100% 回收 | M5-c / M6-d | `concurrent_test.sh`、`hls_test.sh` §9 |
| **NFR-7** 零警告 + 覆盖 | 每批 | `--clean-first` 重建 0 warning、281 用例 |
| **NFR-8** Linux/C++17/FFmpeg 4.4.x | M0 | CMake 探测（含 libswscale）+ `ffmpeg -version` |

## 风险清单

| 风险 | 影响 | 应对 |
|---|---|---|
| **FFmpeg 4.x API 锁定** | 升级系统后编译失败 | CMake 版本探测 + 明确报错；文档记录版本；不做 5.x 兼容层（超出范围） |
| **`libswscale-dev` / `libavfilter-dev` 缺失** | 无法编译 | `libswscale-dev` **已装**（v0.2.0 必需，缺库 CMake 直接 `FATAL_ERROR`）；`libavfilter-dev` 属 v0.3 |
| **时间戳漂移 / 回绕** | 播放卡顿、`non-monotonous DTS` | 全程整数有理换算；32 位回绕专项测试（NFR-3 长跑覆盖） |
| **AAC 封装头写错** | 浏览器无声或直接拒播 | 单测覆盖 `AudioSpecificConfig`；与 `ffprobe` 结果比对 |
| **慢客户端拖垮内存** | 1 小时稳定性不达标 | 有界队列 + 丢非关键帧 + 30s 断开（FR-5） |
| **转码 CPU 打满** | 多客户端时服务不可用 | 一源一份输出 + **转码并发硬上限**（`--transcode-max`，默认 `nproc-1`，`0` 非法）+ `/api/stats` 暴露实时倍数（`transcode.speed`）。**注**：转码跑在源线程上（不进 `ThreadPool`，理由见 `ARCHITECTURE.md` §3 第 0 条） |
| **GPL 依赖** | 分发受限 | README 已声明；如需宽松分发可换非 GPL 的 FFmpeg 构建 |
| **TSAN 误报掩盖真竞争** | 并发缺陷漏检，演变成线上偶发崩溃 | 已定位为 glibc 2.35 `pthread_cond_clockwait` 缺拦截器；用"分组 + 签名判定"处理（`scripts/tsan.sh`，详见 `TESTING.md` 第 8 节），并建议升级 sanitizer 运行时 |

## 未来（未排期，仅记录想法）

- 点播能力（HTTP Range + 重建时间轴，突破"直播式重放"限制）
- 多码率 ABR（HLS master playlist）
- WebSocket-FLV 输出（展示 `IMediaSink` 的可扩展性）
- 音视频轨道选择、字幕轨
