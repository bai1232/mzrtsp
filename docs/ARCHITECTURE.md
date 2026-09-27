# 架构设计

> 配套文档：[SPEC.md](SPEC.md)（需求）· [CODEC_MATRIX.md](CODEC_MATRIX.md)（可行性判定）

## 1. 分层

```
┌──────────────────────────────────────────────────────────────────┐
│ App 层        mzmedia（main）：命令行解析、组装、优雅退出          │
├──────────────────────────────────────────────────────────────────┤
│ Http 层       HttpServer / HttpParser / HttpConnection            │
│              路由、chunked、CORS、Range、静态文件、内置测试页       │
├──────────────────────────────────────────────────────────────────┤
│ Output 层     FlvSender（v0.1） / HlsWriter（v0.2）               │
│              把编码后数据封装成输出格式并写入各自的客户端连接       │
├──────────────────────────────────────────────────────────────────┤
│ Media 层      MediaSource（一源一份输出 + 多订阅者）              │
│              SourceManager · Subscriber · FrameQueue · GopCache   │
├──────────────────────────────────────────────────────────────────┤
│ FFmpeg 封装层 Demuxer · Remuxer · Transcoder（v0.2）· Filter（v0.3）│
│              AvPtr<T>（RAII 智能指针）· CodecMatrix（可行性判定）  │
├──────────────────────────────────────────────────────────────────┤
│ Network 层    EventPoller · TcpServer · TcpClient · Session        │
│              Socket · Buffer · UdpSocket（预留）                   │
├──────────────────────────────────────────────────────────────────┤
│ Core 层       ThreadPool · TaskQueue · Semaphore · Timer           │
│              Logger · util · Ticker · NoticeCenter                 │
└──────────────────────────────────────────────────────────────────┘
```

**依赖方向**：只允许上层依赖下层。`Media` 层**不得**直接调用 `Http`；输出通过抽象接口（`IMediaSink`）与 `Media` 交互，从而做到"新增输出格式不改核心"（成功标准 SC-2）。

## 2. 模块职责

| 模块 | 核心类 | 职责 | 不做什么 |
|---|---|---|---|
| Core | `Logger` `util` `Ticker` `TaskQueue` `ThreadPool` `Semaphore` | 日志、工具、线程与任务 | 不碰媒体、不碰网络 |
| Network | `EventPoller` `TcpServer` `Session` `Buffer` `Socket` | epoll 事件循环、TCP 生命周期、粘包缓冲 | 不解析 HTTP/RTSP |
| Http | `HttpServer` `HttpParser` `HttpConnection` | HTTP/1.1 解析与响应、路由、chunked/CORS/Range | 不碰 FFmpeg |
| FFmpeg 封装 | `Demuxer` `Remuxer` `AvPtr` | 输入解封装、输出封装、FFmpeg 对象 RAII | 不做业务分发 |
| Media | `MediaSource` `SourceManager` `FrameQueue` `GopCache` | 一源一份输出、订阅者管理、GOP 缓存、节流 | 不关心输出是 FLV 还是 TS |
| Output | `FlvSender` `HlsWriter` | 具体输出格式的封装与发送 | 不管理源的生命周期 |
| Stats | `StatsCenter` | 指标采集与 `/api/stats` | — |

## 3. 线程模型

| 线程 | 数量 | 职责 | 阻塞行为 |
|---|---|---|---|
| EventPoller 线程 | `hardware_concurrency()`（默认 4） | accept、HTTP 解析、**向客户端写数据**、定时器 | 绝不阻塞（写队列满即丢帧/断开） |
| 源线程 / 转码线程 | 每源 1 个（v0.1）；v0.2 起由 `ThreadPool` 承载 | demux 读包、remux/transcode、投递到订阅者队列 | 允许随输入 IO 阻塞 |
| 日志线程 | 1 | 异步落盘 | — |

**核心规则**

1. **连接亲和**：一个 HTTP 连接从建立到关闭绑定同一个 `EventPoller`，其上的读解析与写发送**同线程完成，不加锁**。
2. **跨线程只走一条通道**：源线程产出数据后，通过 `EventPoller::async()` 唤醒目标客户端所在线程投递，避免直接跨线程操作连接对象。
3. **绝不阻塞事件循环**：`demux`/`transcode` 这类耗时操作一律在源线程/线程池，事件线程只做"搬运"。

## 4. 一源多消费者模型（核心设计）

```
                    ┌───────────────────────────────┐
                    │ MediaSource  (key = 输入标识)  │
                    │  ├── 生产线程：demux/mux 一份   │
                    │  ├── GopCache：最近 1 个 GOP    │
                    │  └── subscribers: N 个订阅者    │
                    └───────────┬───────────────────┘
                                │ 每帧：shared_ptr<MediaPacket> 分发（零拷贝）
          ┌─────────────────────┼─────────────────────┐
   Subscriber A           Subscriber B          Subscriber C
   ├ queue(≤64帧/8MB)     ├ queue               ├ queue
   ├ 独立时间戳基准        ├ ...                 ├ ...
   └ 绑定 poller#1        └ 绑定 poller#2       └ 绑定 poller#1
```

**关键点**

- **零拷贝**：一帧数据用 `std::shared_ptr<MediaPacket>` 持有，N 个订阅者共享同一块内存，**只有引用计数增加**。
- **独立队列**：每个订阅者有独立的有界队列，因此慢客户端只影响自己的队列。
- **独立时间戳基准**：FLV 的 timestamp 需要从 0 开始递增，各客户端接入时刻不同，必须**按订阅者各自维护时间戳偏移**，否则后接入的客户端时间戳跳变导致播放器异常。
- **GOP 缓存**：新订阅者先灌入缓存的关键帧及其后续帧，保证首帧可解码。
- **引用计数即生命周期**：`MediaSource` 用 `weak_ptr` 记录订阅者；订阅者析构自动注销，源不需要主动感知。

## 5. 背压与慢客户端

| 环节 | 策略 |
|---|---|
| 源 → 订阅者队列 | 队列满时**丢非关键帧**并累加 `dropped` 计数；关键帧绝不丢（否则后续全花屏） |
| 订阅者 → socket | 写缓冲区满则等待 `EPOLLOUT`，**不阻塞线程** |
| 长时间不可写 | 累计超过 30s 未消化 → **断开该客户端**，回收资源 |

## 6. 资源管理（FFmpeg 对象必须 RAII）

FFmpeg 是 C 库，所有上下文都要手动释放。**必须**封装成 C++ 智能指针：

| FFmpeg 对象 | 释放函数 | 封装 |
|---|---|---|
| `AVFormatContext`（输入） | `avformat_close_input` | `InputFormatPtr` |
| `AVFormatContext`（输出） | `avformat_free_context` + `avio_closep` | `OutputFormatPtr` |
| `AVCodecContext` | `avcodec_free_context` | `CodecContextPtr` |
| `AVPacket` | `av_packet_free` | `PacketPtr` |
| `AVFrame` | `av_frame_free` | `FramePtr` |
| `SwsContext`（v0.2） | `sws_freeContext` | `SwsContextPtr` |
| `AVFilterGraph`（v0.3） | `avfilter_graph_free` | `FilterGraphPtr` |

> **这是 NFR-3"连续运行 1 小时不泄漏"的前提**，任何裸 `new`/`malloc` 的 FFmpeg 对象都不允许进入代码库。

## 7. 关键设计决策

| 决策 | 选择 | 理由 |
|---|---|---|
| 事件触发模式 | epoll **ET 默认**，提供 LT 开关 | ET 减少 `epoll_wait` 唤醒次数；代价是必须一次读到 `EAGAIN` |
| 连接与线程 | 单 Reactor × N + 连接亲和 | 避免每连接加锁；代价是负载不均，用"最少连接数分配"缓解 |
| 转码粒度 | **一源一份**输出，多客户端共享 | 10 客户端各转一份会耗尽 CPU；共享是真实流媒体服务器做法 |
| 源释放 | lazy 启动 + 空闲 60s 释放 | 无人观看时不占 CPU/内存 |
| 文件语义 | 直播式重放，不支持 seek | HTTP-FLV 是直播语义；seek 需重建时间轴，收益低 |
| 新增格式 | 通过 `IMediaSink` 抽象扩展 | 满足 SC-2"加格式不改核心" |
| 依赖边界 | 网络自研，FFmpeg 只做编解码 | 并发模型与协议栈是能力展示点；编解码自研不现实 |

## 8. 目录结构（规划）

```
mzmedia/
├── CMakeLists.txt
├── src/
│   ├── core/          # Logger、util、Ticker、线程池、任务队列、定时器
│   ├── network/       # EventPoller、TcpServer、Session、Buffer、Socket
│   ├── http/          # HttpServer、HttpParser、HttpConnection、静态与测试页
│   ├── ffmpeg/        # AvPtr、Demuxer、Remuxer、Transcoder、CodecMatrix
│   ├── media/         # MediaSource、SourceManager、Subscriber、FrameQueue、GopCache
│   ├── output/        # IMediaSink、FlvSender、HlsWriter
│   ├── stats/         # StatsCenter
│   └── main.cpp
├── tests/             # 单元测试（自研轻量断言宏）
├── examples/          # 最小示例
├── scripts/           # play.sh / verify.sh / bench.sh / longrun.sh
├── media/             # 测试素材（软链，不入库）
└── docs/
```

## 9. 扩展路径（SC-2 的验证方式）

新增一种输出格式只需三步，**不改动** Core/Network/Media：

1. 实现 `IMediaSink`（例：`RtmpSink`）
2. 在 `SourceManager` 注册该 sink 的路由前缀
3. 加单元测试与验收脚本

新增一种输入同理：实现 `IDemuxer` 并在 `CodecMatrix` 中登记其编码组合的可行性。
