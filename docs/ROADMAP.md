# 路线图

> 版本号严格遵循 [VERSIONING.md](VERSIONING.md)：`vX.Y.Z`，能力矩阵中的 `v0.Y` 对应语义化版本的 `Y` 位递增。

## v0.1.0 —— MP4/H264 → HTTP-FLV（当前版本）

**目标**：达成成功标准 SC-1（本地 MP4 → HTTP-FLV，浏览器能播）与 SC-3（多客户端不崩）。

| 阶段 | 任务 | 产出 | 验收标准 |
|---|---|---|---|
| **M1** Core | 日志（分级/异步/滚动）、util、`Ticker`、`TaskQueue`、`ThreadPool`、`Semaphore` | `libmzmedia` 骨架 | 单测：日志分级与滚动、线程池 1000 任务无丢失、任务队列 abort 正确 |
| **M2** Network | `PipeWrap`、`EventPoller`（epoll ET/LT、跨线程 `async`）、`Timer`、`Socket`、`Buffer`、`TcpServer`、`Session`、**连接空闲检测与心跳钩子（FR-4.4：读空闲 60s / 写阻塞 30s）+ TCP KeepAlive** | 可用的 TCP 服务端 | `echo` 示例用 `nc` 回显 100MB 无错；定时器精度：**0 早触发** + 相对宿主裸 `nanosleep` 基线的增量 ≤5ms（绝对 ±10ms 门禁在本宿主实测不可达，数据见 `docs/DESIGN_M2.md` §7.1 / §8 R11）；**空闲超时按 FR-4.4 触发，断开后 fd 回落（NFR-6）** |
| **M3** Http | `HttpParser`、`HttpServer`、`HttpConnection`、chunked、CORS、Range、内置测试页 | 可访问的 HTTP 服务 | `curl` 取到测试页；chunked 响应可被 `curl -N` 流式接收 |
| **M4** FFmpeg 封装 | `AvPtr` 系列（RAII）、`Demuxer`、`CodecMatrix`、时间基换算 | 能解封装 MP4/H264 | 单测：从样本 MP4 解出正确的流参数；裸流能提取 SPS/PPS 并报出分辨率 |
| **M5** Media | `MediaPacket`、`FrameQueue`、`GopCache`、`Subscriber`、`MediaSource`（**M5-a**，设计见 `docs/DESIGN_M5.md`）；`SourceManager` + 源线程驱动 + 跨线程投递（**M5-b**）；节流 FR-3.5 + 30s 写阻塞串通 + 丢帧计数进 `/api/stats`（**M5-c**） | 一源多消费者分发 | 单测（分组 `media`，清单见 `docs/DESIGN_M5.md` §5）：3 个订阅者收到**完全一致**的帧序列（每帧指针相同 = 零拷贝）；慢订阅者丢帧但**关键帧与音频不丢**（**FR-5.2**：溢出优先丢非关键帧并累加计数，关键帧腾不出空间则断开该订阅者）；每个订阅者队列上限 **64 帧或 8MB（先到者为准，FR-5.1）**，`0`/无界被拒；任一订阅者异常不影响源与其他订阅者（**FR-5.3**）；订阅者释放后自动注销、队列 100% 回收（**NFR-6**） |
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
| **TSAN 误报掩盖真竞争** | 并发缺陷漏检，演变成线上偶发崩溃 | 已定位为 glibc 2.35 `pthread_cond_clockwait` 缺拦截器；用"分组 + 签名判定"处理（`scripts/tsan.sh`，详见 `TESTING.md` 第 8 节），并建议升级 sanitizer 运行时 |

## 未来（未排期，仅记录想法）

- 点播能力（HTTP Range + 重建时间轴，突破"直播式重放"限制）
- 多码率 ABR（HLS master playlist）
- WebSocket-FLV 输出（展示 `IMediaSink` 的可扩展性）
- 音视频轨道选择、字幕轨
