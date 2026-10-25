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
│              SourceSession（读）· PacketStage（形状）· DemuxerProducer（编排）│
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

**依赖方向**：只允许上层依赖下层。`Media` 层**不得**直接调用 `Http`。

**输出与 `Media` 的接缝是什么（v0.2.3 修订，此前文档写错了）**：不是某个叫 `IMediaSink` 的接口
（**代码里从来没有这个类**），而是两样具体的东西 ——
① `Subscriber::setDrainCallback(...)`：谁来决定"什么时候把队列里的包搬走"；
② 写出口结构体（`FlvSender::Sink{write, end, abort}`）：字节往哪儿写。

于是"新增输出格式"= **新写一个消费者**（它自己持有一个封装器 + 一个写出口）+ 在 app 里挂一条路由；
`Media`/`Network`/`Http` 都不用改。M9 加 HLS、M10-c 给 HLS 加转码，对核心目录（`media_source` /
`subscriber` / `frame_queue` / `network` / `http`）的改动分别是 **19 行（新增函数）**、**0 行** —— 这就是
SC-2 的实证（口径见 §1.1 的"开闭原则机检"）。

**App 层（v0.1 实际形态，M6-c）**：`src/main.cpp` = `bin/mzmedia`，只做**接线**：
命令行 → `SourceManager::Config`、把 `/live/` 与 `/flv.min.js` 两条路由挂到 `HttpServer`、
把 `FlvSender` 交给连接托管。它**不认识** FLV 字节、也不碰 `Demuxer`（只用 `SourceManager` 的两三个入口）。
层内代码不允许反向依赖，检查方式（**只查真正的 include，注释里的路径不算**）：

```bash
grep -rn '#include "http/'   src/output/ src/media/   # 应为空
grep -rn '#include "mzmedia.h"' src/                  # 只允许 src/main.cpp
```

**一个受控例外（M9-b 引入，v0.2.0 记录在案）**：`src/output/hls_service.h` include 了
`network/event_poller.h` —— HLS 的**租约计时器**要用 `EventPoller::doDelayTask`。
它仍然是**向下**依赖（Output 在 Network 之上，见上图的依赖方向），不是反向依赖；
而 `FlvSender`/`FlvMuxer`/`MpegTsMuxer`/`HlsWriter` 这些**纯输出层**类**一律不碰** network/http。
把 poller 抽象成"定时器接口"再注入，对这个规模是净增复杂度，所以选择留这一处并在此写清。

### 1.1 输入侧的两条轴（v0.2.3 重构后）

`DemuxerProducer` 曾经把"读"与"输入形状"塞在一个类里（789 行），于是每加一种输入形状或能力
都要改它。现在拆成两条**互不认识**的轴（详见 `DESIGN_REFACTOR_PIPELINE.md`）：

| 轴 | 谁负责 | 变化频率 | 加东西时改哪 |
|---|---|---|---|
| **读**（打开/读包/EOF/循环/**重连**） | `SourceSession` | 换协议才动（v0.3 的 RTSP） | 只动它 |
| **形状**（Annex-B→AVCC、ADTS→AAC、转码、将来的滤镜） | `PacketStage` 实现 + 一条链 | 每加一种加工就加一个 | 新文件 + `buildChains()` 一支 |

两条轴的唯一耦合是**循环接缝回调**（`setSeamCallback`）与**链尾统一加趟偏移**
（除非链里有阶段 `ownsTimeline()`）—— 都是显式契约，有专门用例断言。

**开闭原则的机检口径**（每次交付后统计）：

```bash
git diff --stat <上一个 tag>..HEAD -- src/media/media_source.* src/media/subscriber.* \
    src/media/frame_queue.* src/network/ src/http/
```

新增函数/只读接口 ≤ 20 行属"接线"（正常）；**改既有逻辑 > 30 行**必须在验收文档里写清
"为什么非改不可、替代方案为何更差"。这条规则让"加功能改了多少老代码"每次发版都被看见。

**`third_party/` 是分层之外的特例**：里面只有两个浏览器播放器 —— `flv.js`（HTTP-FLV，Apache-2.0）
与 `hls.js`（HLS，Apache-2.0，M9-c 入库）。它们**不参与 C++ 构建**（被 `src/main.cpp` 当静态文件
服务出去：`/flv.min.js`、`/hls.min.js`）—— 所以"分层/零依赖"的约束对它们不适用，
但每个都必须带 `LICENSE` 与来源说明（见各自目录的 `README.md`，含版本与散列，可复核）。

## 2. 模块职责

> 口径：**v0.1.0 实际实现**（M6-d 收尾时对齐过一次）。⏳ 标记的是"列在架构里但尚未实现"的类，
> 它们的落地批次写在括号里 —— 免得读者以为代码里已经有了。

| 模块 | 核心类（实际存在） | 职责 | 不做什么 |
|---|---|---|---|
| Core | `Logger` `util` `Ticker` `OnceToken` `TaskCancelable` `ThreadGroup` `NoticeCenter` `TaskQueue` `ThreadPool` `Semaphore` | 日志（分级/异步/滚动文件）、工具、线程与任务 | 不碰媒体、不碰网络 |
| Network | `EventPoller`（含 `EventPollerPool`）`PipeWrap` `Socket`/`SockException` `Buffer` `Session` `TcpServer` | epoll ET/LT 事件循环、跨线程投递、定时器、TCP 生命周期、粘包缓冲 | 不解析 HTTP/RTSP |
| Http | `HttpServer` `HttpParser` `HttpResponse` `HttpSession` | HTTP/1.1 解析与响应、路由、chunked（含异步流式）、CORS、Range、内置播放页 | 不碰 FFmpeg（`/live/` 由 app 注册 → 反向依赖被杜绝） |
| FFmpeg 封装 | `AvPtr` `Demuxer` `CodecMatrix` `h264_util` `TimeBase`；⏳ `Remuxer`/`Transcoder`（v0.2） | 输入解封装、编码可行性判定、H264 语法工具（Annex-B/avcC/SPS）、时间基换算、FFmpeg 对象 RAII | 不做业务分发；**不解码**（v0.1） |
| Media | `MediaPacket` `FrameQueue` `GopCache` `Subscriber` `MediaSource` `SourcePump` `DemuxerProducer` `SourceManager` `Throttle` | 一源多消费者分发、订阅者生命周期、GOP 缓存、按墙钟节流、懒启动/空闲释放、输入形状转换（Annex-B→AVCC）与循环重开 | 不关心输出是 FLV 还是 TS（v0.2 的 TS） |
| Output | `FlvMuxer`（纯函数）、`FlvSender`（每连接）、`MpegTsMuxer`（纯函数）、**`HlsWriter`（一源一份切片器）+ `HlsService`（注册表 + 租约）**（M9） | 具体输出格式的封装与发送 | 不依赖 `http/`（写出目标是 `Sink` 抽象；HLS 只暴露 `playlist()/slice()` 两个只读入口） |
| **App** | `src/main.cpp` → `build/bin/mzmedia` | 命令行、组装、路由接线（`/live/`、`/flv.min.js`）、优雅退出 | 媒体逻辑一律在库里：app 不认识 FLV 字节、也不碰 `Demuxer` |
| Stats | ⏳ `StatsCenter`（**尚未抽**） | 现在由各模块 `dumpStats()`/`dumpStatsJson()` + `HttpServer::setExtraStatsProvider()` 拼成 `/api/stats` | 等**第二个 provider** 出现再抽（`AI_COLLAB §3.6`；见 `DESIGN_M5` §8 未决 12） |

## 3. 线程模型

| 线程 | 数量 | 职责 | 阻塞行为 |
|---|---|---|---|
| EventPoller 线程 | `hardware_concurrency()`（默认 4） | accept、HTTP 解析、**向客户端写数据**、定时器 | 绝不阻塞（写队列满即丢帧/断开） |
| 源线程 | **每源 1 个**（v0.1 起，v0.2/M10-b 仍如此；转码**就地跑在这条线程上**） | demux 读包、remux/transcode、投递到订阅者队列 | 允许随输入 IO 阻塞 |
| 日志线程 | 1 | 异步落盘 | — |

**核心规则**

0. **转码不进线程池**（M10-b 的决定，取代本文早先"v0.2 起由 `ThreadPool` 承载"的计划）：
   一路源 = 一条源线程，转码**就地**在这条线程里做。理由是"顺序 + 背压"本来就是免费的：
   `Transcoder` 必须按包序调用（解码器有状态），放到线程池里要么串行队列（等于现在）要么乱序（错帧）；
   而"一源一份输出 + 并发硬上限（`nproc-1`）"已经从源头限住了 CPU。线程池留给"彼此独立、可乱序"的活。
   代价写在 `docs/DESIGN_M10.md` §8.10（单源不能跨核并行；实测 1080p→720p 单线程 3.26x，够用）。

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
| 源释放 | lazy 启动 + **无句柄且无订阅者**时释放（默认空闲 60s） | 无人观看时不占 CPU/内存。**只看句柄数是不够的**：应用层 handler 一返回就把句柄放了，而连接还连着（M6-c 修的 bug，见 `DESIGN_M6.md` §4.8） |
| 应用入口 | `src/main.cpp` 只做**接线**（路由 / 配置 / 生命周期），零参数可启动 | 媒体逻辑一律在库里：app 拿不到 `Demuxer`/`FlvMuxer` 的内部知识，也不需要懂 FLV 字节（M6-c） |
| 浏览器播放器 | `flv.js` **入库**（`third_party/`，Apache-2.0） | 浏览器原生不能播 FLV；引 CDN 会让"离线/演示环境"变成"服务坏了"。这是全仓唯一的第三方文件，C++ 侧仍然零第三方依赖 |
| 文件语义 | 直播式重放，不支持 seek；`--loop` 用**连续时间轴**重开（不重置） | HTTP-FLV 是直播语义；seek 需重建时间轴，收益低。循环若把时间戳归零，播放端会看到回退（花屏）——所以偏移在解封装层累加（`DESIGN_M6.md` §4.6） |
| 新增格式 | 新写一个**消费者**（`drain 回调` + 写出口）+ app 挂路由；核心目录 0 改动 | 满足 SC-2"加格式不改核心"；具体见 §1 的"输出与 Media 的接缝" |
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
│   ├── output/        # FlvMuxer/FlvSender、MpegTsMuxer、HlsWriter/HlsService（互不继承，理由见 §9）
│   ├── stats/         # StatsCenter
│   ├── mzmedia.h      # 伞头：使用方的单行包含入口（库内部禁止包含）
│   └── main.cpp
├── tests/             # 单元测试（自研轻量断言宏）
├── examples/          # 最小示例（每个库组件一个可运行 demo）
├── third_party/
│   ├── flv.js/        # HTTP-FLV 播放器（Apache-2.0，含 LICENSE 与来源说明）
│   └── hls.js/        # HLS 播放器（Apache-2.0，M9-c 入库，含 LICENSE 与来源说明）
├── scripts/           # make_samples / http_test / flv_mux_test / flv_http_test / tsan
├── samples/           # 测试素材（脚本现场生成，不入库）
└── docs/
```

### 头文件包含约定

| 角色 | 包含方式 | 原因 |
|---|---|---|
| **使用方**（`examples/`、`tests/`、外部集成） | `#include "mzmedia.h"` 伞头单行包含 | 少写 include，接入方便 |
| **库内部**（`src/**/*.cpp`、模块头之间） | 只包含自己需要的模块头，**禁止**包含 `mzmedia.h` | 伞头会引入无关依赖、拖慢编译、掩盖真实耦合 |

新增模块时，把它的公开头文件加进伞头的"实际 include 区"，并保持分层顺序。

## 9. 扩展路径（SC-2 的验证方式）

新增一种输出格式只需三步，**不改动** Core/Network/Media：

1. 写一个**消费者**：`source->subscribe()` + `subscriber->setDrainCallback(...)`，
   回调里把队列搬进你自己的封装器，再通过你的写出口（例如 HTTP chunked / 文件 / socket）发出去。
   （例：`FlvSender` = 订阅者 + `FlvMuxer` + `Sink{write,end,abort}`；`HlsWriter` = 订阅者 + `MpegTsMuxer` + 切片环）
2. 在 `app` 里挂你的路由前缀（`main.cpp` 只做接线，一行）
3. 加单元测试与验收脚本

**为什么不把封装器写成继承**（`FlvMuxer` / `MpegTsMuxer` 目前是平级独立类）：
它们的**操作集几乎不重叠**（FLV：一次 header + sequence header + 32 位 ms / per-client 基准；
TS：每片 PAT/PMT + PCR + 33 位 90kHz + 关键帧注入 SPS/PPS），**生命周期也不同**
（FLV 每连接一份；TS 每源一份、被 `HlsWriter` 跨片复用），而且**没有任何函数会通过基类调用它们** ——
硬抽 `IMuxer` 只会得到"两个类的并集接口"或"调用方 downcast"，两者都比现状差。
等出现**第三个**格式且确有"按格式名统一驱动"的代码时，再抽一个按驱动者需要**收窄**的接口
（`begin()/write(packet)/end()`）。判据写在 `DESIGN_REFACTOR_PIPELINE.md` §4 的排除项里。

新增一种输入：**容器/协议层靠 FFmpeg 免费扩展**（`Demuxer::open` 直接吃 URL/新容器）；
若带来**新的输入形状**（如 Annex-B / ADTS），加一个 `PacketStage` 实现即可（见 §1.1 的两条轴），
不需要 `IDemuxer` —— 目前只有一个"读"实现，抽接口是"为想象中的第二个实现付账"。
