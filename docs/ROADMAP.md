# 路线图

> 版本号严格遵循 [VERSIONING.md](VERSIONING.md)：`vX.Y.Z`，能力矩阵中的 `v0.Y` 对应语义化版本的 `Y` 位递增。

## v0.1.0 —— MP4/H264 → HTTP-FLV（当前版本）

**目标**：达成成功标准 SC-1（本地 MP4 → HTTP-FLV，浏览器能播）与 SC-3（多客户端不崩）。

| 阶段 | 任务 | 产出 | 验收标准 |
|---|---|---|---|
| **M1** Core | 日志（分级/异步/滚动）、util、`Ticker`、`TaskQueue`、`ThreadPool`、`Semaphore` | `libmzmedia` 骨架 | 单测：日志分级与滚动、线程池 1000 任务无丢失、任务队列 abort 正确 |
| **M2** Network | `PipeWrap`、`EventPoller`（epoll ET/LT、跨线程 `async`）、`Timer`、`Socket`、`Buffer`、`TcpServer`、`Session` | 可用的 TCP 服务端 | `echo` 示例用 `nc` 回显 100MB 无错；定时器精度 ±5ms |
| **M3** Http | `HttpParser`、`HttpServer`、`HttpConnection`、chunked、CORS、Range、内置测试页 | 可访问的 HTTP 服务 | `curl` 取到测试页；chunked 响应可被 `curl -N` 流式接收 |
| **M4** FFmpeg 封装 | `AvPtr` 系列（RAII）、`Demuxer`、`CodecMatrix`、时间基换算 | 能解封装 MP4/H264 | 单测：从样本 MP4 解出正确的流参数；裸流能提取 SPS/PPS 并报出分辨率 |
| **M5** Media | `MediaSource`、`SourceManager`、`Subscriber`、`FrameQueue`、`GopCache`、节流 | 一源多消费者分发 | 单测：3 个订阅者收到完全一致的帧序列；慢订阅者丢帧但关键帧不丢 |
| **M6** Output | `FlvSender`、FLV 封装（AVC sequence header、AAC sequence header、per-client 时间戳基准） | **能播了** | **`ffplay` 出画面、有声音**；10 路并发各自播放 5 分钟无异常 |
| **M7** 收尾 | 集成验收、`scripts/` 验证脚本、文档补齐、性能数据采集 | 可发布版本 | 全部 SC/NFR 验收通过；`v0.1.0` 打 tag 并推送 |

**M6 是项目的"第一个里程碑式胜利"** —— 在此之前一切都在摸黑，之后都是增量。

## v0.2.0 —— 多输入 + HLS + 转码

| 任务 | 说明 |
|---|---|
| MKV / TS 输入 | 复用 M4 的 `Demuxer`，`CodecMatrix` 增补组合 |
| **HLS 输出** | `HlsWriter`：mpegts 切片 2s、滑动窗口 6 片、`.m3u8` 序列管理、切片静态服务（含 Range） |
| **转码** | `Transcoder`：解码 → 编码；`libswscale` 做像素格式/分辨率转换（**需先 `apt install libswscale-dev`**） |
| 转码调度 | 由 `ThreadPool` 承载；lazy 启动 + 空闲 60s 释放；实时倍数统计 |
| 验收 | `hls.js` / 原生播放器能播；转码 ≥1.0x 实时（1080p→720p x264 veryfast） |

## v0.3.0 —— RTSP 输入 + 解码处理

| 任务 | 说明 |
|---|---|
| RTSP 输入 | FFmpeg 作为客户端拉流；断流检测与**自动重连**（指数退避）；编码探测以实际码流为准 |
| 解码处理 | `Filter` 链：缩放、水印、帧率变换（`libavfilter`，**需先 `apt install libavfilter-dev`**） |
| 验收 | 从一台 RTSP 源（可用 `ffmpeg -re -f rtsp` 本机自建）拉流并转出 HTTP-FLV，连续 1 小时不断流 |

## 风险清单

| 风险 | 影响 | 应对 |
|---|---|---|
| **FFmpeg 4.x API 锁定** | 升级系统后编译失败 | CMake 版本探测 + 明确报错；文档记录版本；不做 5.x 兼容层（超出范围） |
| **`libswscale-dev` / `libavfilter-dev` 缺失** | v0.2/v0.3 无法编译 | 已实测确认缺失，`apt install` 需要 sudo 密码，提前安排 |
| **时间戳漂移 / 回绕** | 播放卡顿、`non-monotonous DTS` | 全程整数有理换算；32 位回绕专项测试（NFR-3 长跑覆盖） |
| **AAC 封装头写错** | 浏览器无声或直接拒播 | 单测覆盖 `AudioSpecificConfig`；与 `ffprobe` 结果比对 |
| **慢客户端拖垮内存** | 1 小时稳定性不达标 | 有界队列 + 丢非关键帧 + 30s 断开（FR-5） |
| **转码 CPU 打满** | 多客户端时服务不可用 | 一源一份输出 + 线程池限流 + `/api/stats` 暴露实时倍数 |
| **GPL 依赖** | 分发受限 | README 已声明；如需宽松分发可换非 GPL 的 FFmpeg 构建 |

## 未来（未排期，仅记录想法）

- 点播能力（HTTP Range + 重建时间轴，突破"直播式重放"限制）
- 多码率 ABR（HLS master playlist）
- WebSocket-FLV 输出（展示 `IMediaSink` 的可扩展性）
- 音视频轨道选择、字幕轨
