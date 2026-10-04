# M5 设计：媒体分发层（Media）

> 上游需求：`docs/SPEC.md` §5.5（FR-5.1 **修订版** / FR-5.2 **修订版** / FR-5.3）、`NFR-6`、`FR-6.1`；`docs/ARCHITECTURE.md` §3（线程模型）/ §4（一源多消费者）/ §5（背压与慢客户端）
> 上游实现：`docs/DESIGN_M2.md`（`Session` 发送队列与背压）、`docs/DESIGN_M4.md`（`Demuxer` 产包）
> 本文件对应 ROADMAP 的 **M5 Media** 一行，拆成 M5-a / M5-b / M5-c / M5-d 四批。

## 1. 范围与验收

| 批次 | 内容 | 状态 |
|---|---|---|
| **M5-a** | `MediaPacket`、`FrameQueue`、`GopCache`、`Subscriber`、`MediaSource`（**单线程策略**，帧由外部喂入） | ✅ |
| **M5-b** | **线程打通**：`FrameQueue` 加锁成 SPSC；`Subscriber` 绑定消费者线程 + **唤醒合并**；`SourcePump`（源线程驱动一个**可打断**的读回调） | ✅ |
| **M5-b′**（决策落地） | FR-5.1 改为**只按字节**且上限=**码率×延迟额度**；FR-5.2 改为**音视频成对丢**；GOP 上限=**码率×最大 GOP 时长**；统计最小版 `dumpStats()` | ✅（本文件） |
| **M5-c** | `SourceManager`（懒启动 / 复用 / 空闲释放）+ `DemuxerProducer`（把 M4 的 `Demuxer` 接到 `SourcePump` 的读回调上，中止标志接 `interrupt_callback`） | ✅ |
| **M5-d** | 节流（**FR-3.5**：按源时间轴与墙钟对齐推送，不得以磁盘速度全速灌入）；`dumpStatsJson()` + `HttpServer::setExtraStatsProvider()` 接进 `/api/stats`（**FR-6.1**）；`broken()` 只做**巡检**（真正断连留 M6） | ✅ |

**验收（对应 `docs/ROADMAP.md:15`，按批次分配）**：

| 验收点 | 落在哪批 |
|---|---|
| 3 个订阅者收到**完全一致**的帧序列（每帧指针相同 = 零拷贝） | M5-a ✅ |
| 慢订阅者丢帧、且不影响其他订阅者（FR-5.2 / FR-5.3） | M5-a ✅ |
| 每个订阅者队列上限 = **码率上限 × 延迟额度**（初值 8 Mbps × 2 s = **2 MB**，**只按字节**；FR-5.1 修订） | M5-b′ ✅ |
| 订阅者断开后队列 100% 回收（NFR-6） | M5-a ✅ |
| 源线程 ⇄ 消费者线程之间**数据不丢、不重、不卡**；唤醒**既不丢也不刷**（合并） | M5-b ✅ |
| 源被停止 / 读失败 / 读完时，消费者都能**明确知道"不会再有数据"**（绝不永久等待） | M5-b ✅ |
| 丢弃单位是"最旧的一段"（**音视频成对**），关键帧绝不丢 | M5-b′ ✅ |
| 懒启动、空闲释放、源与线程 100% 回收（**FR-1.2** 按需启动、**NFR-6**） | M5-c ✅ |
| 真读 v0.1 的 **MP4（H264 + AAC）** 到 EOF；源创建 / 释放落日志（**FR-2.1** / **FR-6.3**） | M5-c ✅ |
| **按源时间轴与墙钟对齐推送**（不得以磁盘速度灌入），且节流**可被打断**（**FR-3.5**） | M5-d ✅ |
| `/api/stats` 能带上 media 的源 / 订阅 / 丢帧统计（**FR-6.1**） | M5-d ✅ |
| 10 路并发压测、1 小时长跑 | M6 / M7 |

## 2. 分层位置与依赖方向

```
Output(未做) ← Media ← Network(EventPoller) / Core
```

- `media/` 依赖 `core/`（日志、`Semaphore`）与 **`network/` 的 `EventPoller`**（M5-b 起，用来做跨线程唤醒）。
  分层顺序 `core → network → http → ffmpeg → media → output` 允许这个方向。
- **不依赖 FFmpeg**：帧由外部喂给 `MediaSource`，所以"数据从哪来"与"分发策略"解耦。
  好处是策略（上限 / 丢帧 / GOP）与线程穿透都能用**确定性用例**覆盖，不需要样本文件。
- 反向依赖检查（`media/` 不得出现 http / ffmpeg）：
  `grep -rn "http/\|ffmpeg/" src/media/` → 应为空

## 3. 契约

**线程契约（M5-c 修正）**：数据方向是 **SPSC** —— 源线程只调 `pushPacket`；
**订阅管理（`subscribe` / `unsubscribe` / `subscriberCount` / `endOfStream` / `limits`）可由任意线程调用**
（内部 `_mutex` 保护订阅集合、`GopCache`、`_ended`）；消费者线程只调 `queue().pop()` / drain 回调。
**两条纪律**：① 唤醒一律在**锁外**做（`async` 会内联执行 drain 回调，持锁调用即死锁）；
② `gopCache()` 返回的引用只供源线程 / 内部使用（它不代理加锁）。
`GopCache` 本身仍是"外部同步"的组件，只被 `MediaSource` 在持锁状态下使用。

### 3.1 `MediaPacket`（`media_packet.h`）

不可变的编码数据包 —— 一帧数据被 N 个订阅者共享的载体。

```cpp
enum class MediaKind : uint8_t { Video, Audio };

class MediaPacket {
public:
    using Ptr     = std::shared_ptr<MediaPacket>;
    using Payload = std::shared_ptr<const std::vector<uint8_t>>;

    // 工厂：payload 为空 / 尺寸为 0 / stream_index < 0 → 返回 nullptr（不抛、不静默造空包）
    static Ptr create(MediaKind kind, int stream_index, Payload payload,
                      bool key_frame, int64_t dts_ms, int64_t pts_ms);

    MediaKind kind() const;  int streamIndex() const;  bool isKeyFrame() const;
    int64_t dtsMs() const;   int64_t ptsMs() const;
    size_t size() const;     const uint8_t *data() const;
    const Payload &payload() const;

    /// 是否允许被丢弃：**唯一不可丢的是视频关键帧**（音频与视频非关键帧都可丢）
    bool droppable() const;
};
```

- **对象创建后不可变**，所以可以被多个线程只读共享（可变只有 `shared_ptr` 的引用计数）。
- **负时间戳合法**：B 帧/部分容器的首包 dts 就是负的，不能拦。
- 不校验 pts/dts 的相对大小 —— 那是封装层的事（`MonotonicGuard`）。

**为什么 `droppable()` 放在包上**：策略要判断的是"这个包丢了会不会让后面全花屏"，
这是包自身的性质；散在队列里按 `stream_index` 猜迟早会漏一路流。

**为什么音频也可丢（FR-5.2 修订）**：早期版本"音频一律不可丢"，听起来更保守，实际会出事 ——
拥塞时只丢视频、音频照收，**视频跳着走、音频连续放**，音画越走越偏；
而音频占的字节会持续挤压本来就紧张的队列。改成"音视频成对丢"后，
丢的是**同一时间窗**（最旧的一段），两端一起断一下，时间轴仍然对齐。

### 3.2 `FrameQueue`（`frame_queue.h`）

每个订阅者一个的**有界**队列。

> **【M5-b′ 变更】上限只按字节**（FR-5.1 修订）：`max_bytes = 码率上限 × 延迟额度`。
> 原来的"64 帧"已删除 —— 帧数与字节数是同一件事的两种量纲，而真正的参数是**时间**（能容忍几秒延迟）。
> 跑 4K 时 64 帧可能不到 1 秒（帧数上限先到，延迟失真）；跑低码率时 64 帧可能几十秒（内存白占）。
> **只保留字节上限，语义唯一且能直接从"延迟额度"推出来。**

```cpp
class FrameQueue {
public:
    struct Limits { size_t max_bytes = 2000000; };   // 初值 = 8 Mbps × 2 s（推导见 §4.5）
    struct Stats  { uint64_t pushed, popped, dropped,
                             dropped_incoming, rejected_no_space, dropped_on_clear; };

    enum class PushResult : uint8_t { Accepted, DroppedToMakeRoom,
                                      DroppedIncoming, RejectedNoSpace };
    enum class PopResult  : uint8_t { Packet, Empty, EndOfStream };

    FrameQueue();
    bool   setLimits(const Limits &limits);   // false = 非法（0 / 超硬上限），不生效
    Limits limits() const;                    // 快照

    PushResult push(MediaPacket::Ptr packet); // 源线程
    PopResult  pop(MediaPacket::Ptr *packet); // 消费者线程
    bool       markEndOfStream();             // true = 状态改变（重复调用 false）
    size_t     clear();                       // 返回被丢弃的包数（NFR-6 回收用）

    size_t bytes() const;    // 快照
    size_t packets() const;  // 快照（**只作观测**，不参与限流）
    bool   empty() const;    bool endOfStream() const;
    Stats  stats() const;    // **快照**（不是引用：返回引用会被另一线程改）
};
```

**`push` 的四种结果**（形状定死，调用方必须处理）：

| 结果 | 含义 | 调用方该做什么 |
|---|---|---|
| `Accepted` | 已入队，没有丢任何包 | 继续 |
| `DroppedToMakeRoom` | 已入队，但为腾空间丢了**最旧的一段**（音视频一起走） | 继续；计数在 `stats()` |
| `DroppedIncoming` | 没入队，丢的是**新包自己**（新包可丢） | 继续（正常降级，无需动作） |
| `RejectedNoSpace` | 没入队，且新包**不可丢**（视频关键帧） | **断开该订阅者**（FR-5.2）；`Subscriber::broken()` 已置位 |

### 3.3 `GopCache`（`gop_cache.h`）

最近 **1 个 GOP** 的缓存，只为一个目的：让**中途接入**的订阅者从关键帧开始（否则首帧解不出来，花屏到下一个关键帧）。

```cpp
class GopCache {
public:
    enum class FeedResult : uint8_t { StartedNewGop, Cached, Skipped };

    explicit GopCache(size_t max_bytes = 2000000);   // 初值 = 码率上限 × 最大 GOP 时长
    bool   setMaxBytes(size_t bytes);                // 0 → false
    size_t maxBytes() const;

    FeedResult feed(const MediaPacket::Ptr &packet); // 视频关键帧 = StartedNewGop（丢弃上一个 GOP）
    std::vector<MediaPacket::Ptr> snapshot() const;  // 第一项必是当前 GOP 的关键帧
    size_t packets() const;  size_t bytes() const;
    size_t clear();
    const Stats &stats() const;
};
```

- **动态增长 + 上限**，而上限是**算出来的**：`码率上限 × 最大 GOP 时长`（不要拍 8MB ——
  GOP 时长随编码器设置变、码率随分辨率变，两个都不定就写死字节数，等于用一个与场景无关的常数
  去保证"装得下一个 GOP"，装不下时表现为新订阅者长时间花屏）。
- 关键帧到达 ⇒ **丢弃整个旧 GOP** 重新开始。
- 还没有关键帧时**不缓存任何包**。
- 超过上限时丢最旧的可丢包，但**关键帧必须保留**（否则"从关键帧接入"这条保证就没了）——
  因此上界是 `max_bytes + 一个关键帧大小`（关键帧单包由 M4 的 `max_packet_size` 兜底）。
- **可丢集合与 `FrameQueue` 已统一**：两处唯一不可丢的都是**视频关键帧**（音频在两边都可丢）。
  早期版本刻意让两边不同，本批统一 —— 同一个概念不搞两套规则。

### 3.4 `MediaSource` / `Subscriber`（`media_source.h`）

```cpp
class Subscriber {
public:
    using Ptr = std::shared_ptr<Subscriber>;
    using DrainCallback = std::function<void(const Ptr &)>;

    Id id() const;  FrameQueue &queue();  const FrameQueue &queue() const;
    bool broken() const;

    // M5-b：绑定消费者线程 + 唤醒
    bool bindPoller(const EventPoller::Ptr &poller);          // nullptr = 解绑（同步模式）
    EventPoller::Ptr poller() const;
    DrainCallback setDrainCallback(DrainCallback callback);   // 返回上一个
    bool notifyIfNeeded();       // 源线程：**最多一个未决唤醒**（合并）
    bool clearNotifyPending();   // 消费者线程在 drain **之前**调用
    uint64_t notifyCount() const; notifyRejectedCount() const; notifyCoalescedCount() const;

    // 【占位，M5-d/M6】订阅者的独立时间戳基准（ARCHITECTURE.md §4）：
    //   FLV 的时间戳必须从 0 起递增，而各客户端接入时刻不同 → 必须按订阅者维护偏移。
    //   现在不实现（没有输出层可用），但位置留在头文件里，避免 M6 到处找地方塞。
    //   int64_t timestampBaseMs() const;  void setTimestampBaseMs(int64_t);
};

class MediaSource {
public:
    using Ptr = std::shared_ptr<MediaSource>;

    /// **上限由物理量推导**，不在这里拍字节数（见 §4.5）
    struct Limits {
        size_t   max_bitrate_bps   = 8000000;   // 单路码率上限：8 Mbps
        uint32_t latency_budget_ms = 2000;      // 延迟额度：2 s → 队列上限 2,000,000 B
        uint32_t max_gop_ms        = 2000;      // 最大 GOP 时长：2 s → GOP 上限 2,000,000 B
        size_t   max_subscribers   = 16;        // **人数**是硬边界

        size_t queueMaxBytes() const;           // = 码率 × 延迟额度
        size_t gopMaxBytes() const;             // = 码率 × 最大 GOP 时长
        uint64_t outputBitrateBudgetBps() const;// **观测用**：满订阅时的输出带宽
    };

    struct PushStats { size_t delivered, dropped_to_make_room, dropped_incoming,
                              rejected_no_space, notified, subscribers; };

    MediaSource();
    explicit MediaSource(const Limits &limits);
    bool setLimits(const Limits &limits);   // false = 非法（0 / 超硬上限 / 推导值超硬上限）
    Limits limits() const;

    Subscriber::Ptr subscribe();            // nullptr = 超人数上限 / 关键帧灌不进队列
    bool   unsubscribe(Subscriber::Id id);
    size_t subscriberCount();
    bool   endOfStream();

    PushStats pushPacket(MediaPacket::Ptr packet);
    const GopCache &gopCache() const;

    std::string dumpStats() const;          // FR-6.1 最小版；**任意线程可调**
    uint64_t totalDelivered() const;  uint64_t totalDropped() const;
    uint64_t totalDroppedIncoming() const;  uint64_t totalRejected() const;
    uint64_t totalSubscribe() const;  uint64_t totalSeedFailed() const;
    uint64_t totalAutoUnsubscribe() const;  uint64_t totalBroken() const;
};
```

- **订阅者生命周期**：源持有 `std::weak_ptr<Subscriber>`（`ARCHITECTURE.md` §4）。
  消费者把 `shared_ptr` 一放，源在下次 `pushPacket` / `subscriberCount` 时**惰性注销**并计数（NFR-6）。
- **`subscribe()` 会灌入 GOP 缓存**：保证第一条是视频关键帧。关键帧本身超过队列上限 →
  **订阅失败**（`nullptr` + `totalSeedFailed()`），而不是让消费者拿到"从中间开始"的流。
- **`endOfStream()`**：给每个订阅者队列打 EOS 标记 + 各补一次唤醒；
  消费者 `pop()` 会拿到 `EndOfStream`（区分"暂时没数据"与"不会再有数据"）。
  源结束后接入的订阅者**立刻**拿到 EOS。
- **`pushPacket` 的失败路径只置 `broken()`，不在这里断连接**：断连接是连接层的事（M5-d）。
- **统计全部是 `std::atomic`**，`dumpStats()` 可从任意线程调用 —— 这是 FR-6.1 的**最小版**
  （完整 `StatsCenter` 与 `/api/stats` 接线见 M5-d）：一行文本、可 grep，包含
  推导出的上限、满订阅输出带宽、以及 delivered / dropped / rejected / subscribe / broken / auto_unsub。

### 3.5 `SourcePump`（`source_pump.h`）

**源线程**：反复向一个"**可打断**的读回调"要包，推给 `MediaSource`。

```cpp
class SourcePump {
public:
    enum class ReadResult : uint8_t { Packet, EndOfStream, Error };
    using ReadFn = std::function<ReadResult(MediaPacket::Ptr *packet, std::string *error)>;

    bool start(ReadFn read, MediaSource::Ptr source);   // false = 已在跑 / 入参为空
    bool stop();                                        // 请求停止 + join；false = 本来就没在跑
    bool running() const;
    bool stopRequested() const;   // 供 ReadFn 查询；真实实现接 Demuxer 的 interrupt_callback
    bool eof() const;             // **正常读完**才算（"被停"不算）
    std::string lastError() const;
    uint64_t readCount() const;   // 读到包的次数
    uint64_t pushedCount() const; // 调用 pushPacket 的次数
};
```

- `stop()` 会 **join**，所以 `ReadFn` **必须能被打断**（定期查 `stopRequested()`）——唯一的硬约束。
- 三种结束（**读完 / 读失败 / 被停**）都广播 EOS；`eof()` 与 `lastError()` 把"读完"和"失败"分开。
- `ReadFn` 说好返回 `Packet` 却给空包 → **当作错误停下来**并记原因（否则是"死循环 + 什么都不做"的静默故障）。

### 3.6 `DemuxerProducer`（`demuxer_producer.h`，M5-c）

把 M4 的 `Demuxer` 包成 `SourcePump` 的读回调 —— 它**只做一件事**：`AVPacket → MediaPacket`。

```cpp
class DemuxerProducer {
public:
    struct Config { Demuxer::Limits demux; };

    bool open(const std::string &path);       // false = 打不开（原因见 lastError）
    const Demuxer &demuxer() const;           // M6 要用流信息写 FLV 的 sequence header
    const std::atomic<bool> *setAbortFlag(const std::atomic<bool> *flag);  // 返回上一个

    SourcePump::ReadResult read(MediaPacket::Ptr *packet, std::string *error); // 直接当 ReadFn 用
    uint64_t totalPackets() const;  uint64_t totalBytes() const;
    uint64_t skippedUnknownStreams() const;  uint64_t timestampFailures() const;
    std::string lastError() const;
};
```

- **每包一次拷贝**（全链路唯一一处）：`Demuxer::packet()` 指向它内部的 `AVPacket`，下一帧就被复用，
  必须把字节搬进共享载荷。扇出到 N 个客户端仍然是零拷贝。
- **非音视频流跳过但计数**（字幕 / 数据流）：静默跳过等于悄悄丢数据。
- 时间戳换算失败**仍然发包**（用 Demuxer 保持的上一次值 + 单调钳制），只累加计数 ——
  丢一个包比时间戳不够精确更糟。

### 3.7 `SourceManager`（`source_manager.h`，M5-c）

源的**懒启动 / 复用 / 空闲释放**（`ARCHITECTURE.md` §7「lazy 启动 + 空闲 60s 释放」）。

```cpp
class SourceManager : public std::enable_shared_from_this<SourceManager> {
public:
    using Ptr = std::shared_ptr<SourceManager>;
    struct Config {
        uint32_t idle_release_ms = 60000;   // 0 = 句柄一放就释放（不缓存源）
        MediaSource::Limits source;         // 每源的队列 / GOP / 人数上限
        DemuxerProducer::Config producer;   // 解封装上限
    };

    static Ptr create(const EventPoller::Ptr &poller);   // 必须用 create（句柄回指需要 weak_ptr）
    bool setConfig(const Config &config);
    MediaSource::Ptr acquire(const std::string &path);   // nullptr = 打不开（**不注册源**）
    bool release(const std::string &path);               // 立刻释放
    size_t releaseAll();                                 // 关停路径
    size_t sourceCount() const;  size_t handleCount() const;
    std::string dumpStats() const;  std::string lastError() const;
};
```

四条必须做对的地方（改回去就会出事）：

1. **句柄的生命周期**：对外返回的 `MediaSource::Ptr` 用「自定义 deleter + 捕获强引用」构造 ——
   句柄自己也让对象活着，所以 `releaseAll()` 摘掉条目之后句柄**依然有效**（不会悬垂）；
   deleter 只通过 `weak_ptr` 回调管理器，管理器已析构时是 no-op。
2. **IO 在锁外**：`open` 可能慢甚至超时，而管理器锁**轮询线程也要拿**（挂计时器）——
   占着它就等于阻塞事件循环。
3. **计时器只在轮询线程挂**（`doDelayTask` 的约定）→ 一律用 `async()` 投递；
   投递 / 挂表失败（poller 已退出）**绝不静默"永不释放"** → 计数 + Warn + **退化为立即释放**。
4. **停线程在锁外**：`pump->stop()` 会 join，持有管理器锁时不做。

### 3.8 `Throttle`（`throttle.h`，M5-d）

按源时间轴与**墙钟**对齐推送 —— **FR-3.5**："文件输入按源时间轴与墙钟对齐推送，不得以磁盘速度全速灌入"。

```cpp
class Throttle {
public:
    struct Config { bool enabled = true; double speed = 1.0; };  // 默认**开**、1 倍速
    bool setConfig(const Config &config); // false = speed ≤0 / NaN / inf / 超 1000 倍
    void reset();                          // start() 时对表（记墙钟与首个 dts）
    bool pace(int64_t dts_ms, const std::atomic<bool> &abort);   // false = 被中止
    int64_t waitedMs() const; uint64_t paceCount() const; uint64_t abortedCount() const;
};
```

- **为什么必须有**：磁盘比网络快几个数量级。不节流的话，2 秒样本会在几毫秒内读完并塞满每个订阅者的
  队列（然后按 FR-5.1/5.2 大面积丢帧）——对观看者来说不是"流畅播放"，而是"瞬间冲完 + 一堆丢帧"。
- **可被打断**：内部按 ≤50ms 分片睡，中止标志一置位立刻返回 false；否则 `SourcePump::stop()` 的 join
  要等满一拍。
- **不补偿**：已经落后就直接放行。补偿会让落后的源疯狂追赶，把下游又冲爆一次。
- `speed`：`1.0` = 实时；`>1` = 快放（压测/单测用，如 8.0）；`enabled = false` 只给压测 ——
  日常路径按 FR-1.2 应当开着。时基用 `dtsMs()`（M4 已换算 + 单调钳制）与**单调时钟**（不用系统时钟）。

### 3.9 统计接线（FR-6.1）

- `MediaSource::dumpStatsJson()` → `"media_source":{...}`（订阅数、推导上限、broken id 列表、各计数）
- `SourceManager::dumpStatsJson()` → `"source_manager":{...},"media_sources":[{...},...]`
- `HttpServer::setExtraStatsProvider(std::function<std::string()>)` 把片段拼进 `/api/stats`
  （返回上一个，沿用项目约定）
  - **契约**：provider 返回**合法 JSON 对象片段**（不含最外层花括号），键名用模块名做前缀避免撞名
  - **为什么用钩子而不是让 `http` 依赖 `media`**：分层方向是 `http ← 上层装配`（M7 的 main 接线），
    http 层编译期不拖上 media；也避免"只有一个 provider 就引入注册表"（`AI_COLLAB §3.6`）
  - `StatsCenter`（`ARCHITECTURE.md` §2 列出的模块）**推迟到有第二个 provider 时**再抽（§8 未决 12）
- `broken()`：本批**只做巡检**（`brokenSubscriberIds()` 进 JSON）；真正断连留 M6（现在没有连接对象可断）。

## 4. 关键机制

### 4.1 零拷贝分发

`MediaPacket::Payload` 是 `shared_ptr<const std::vector<uint8_t>>`，`MediaSource::pushPacket`
把**同一个 `Ptr`** 放进 N 个订阅者的队列 —— 只增加引用计数，不复制字节。
用例 `media_source_fanout_identical` 直接用**指针相等**来断言"三个订阅者拿到的是同一帧"。

### 4.2 溢出策略：丢"最旧的一段"，音视频成对（FR-5.2 修订）

- 队列里**唯一不可丢的是视频关键帧**（关键帧丢了 → 后面全花屏，必须由调用方断开订阅者）。
- 超限时从**队头**开始连续丢可丢包，直到放得下 —— 丢的是"最旧的一段"，
  一段里的音频与视频**自然一起走**，所以不会出现"视频跳一段、音频继续放"的音画错位。
- 新包自己就超上限：可丢 → 丢新包（`DroppedIncoming`）；不可丢 → `RejectedNoSpace`。
- 队里只剩关键帧（腾不出空间）：新包可丢 → 丢新包；不可丢 → `RejectedNoSpace` + `broken()`。
- 所有丢弃都进 `stats()` 并被 `MediaSource` 汇总 —— 可观测是辅助，上限才是防线。

### 4.3 策略与线程分开（M5-a 的取舍，M5-b 已兑现）

策略（丢谁 / 上限怎么算 / GOP 怎么截断）与"谁在什么线程调用"是**正交**的两件事。
M5-a 把策略做成**纯函数式单线程组件**（进 TSAN 严格组），线程与投递留给 M5-b。
M5-b 兑现了这一点：`FrameQueue` 只多了一把锁，**丢帧 / GOP 策略一行没改**，
M5-a 的用例加锁之后原样通过（这就是"正交"的验证方式）。

### 4.4 跨线程通道：数据走队列、唤醒走 `async`

| 通道 | 载体 | 特点 |
|---|---|---|
| **数据** | 该订阅者的 `FrameQueue`（加锁 SPSC） | 一帧只 `create` 一次，N 个订阅者共享同一个 `shared_ptr`；上限与丢帧策略就在这里生效 |
| **唤醒** | `EventPoller::async()` | **只是"去看一眼"**，不携带数据；因此可以合并 |

1. **唤醒合并**：每订阅者最多一个未决唤醒。100 帧只产生 **1 次**跨线程投递。
2. **先清标记，再 drain**：顺序反了就会丢唤醒（drain 期间新到的帧以为"已经有人被唤醒了"）。
3. **投递被拒要复位标记**：`async` 返回 false（poller 已退出 / 队列满）时必须复位 + 计数，否则永久卡住。

为什么不把帧本体交给 `async`：真正的缓冲会变成 poller 的任务队列（上限 65536 条），
队列上限就管不住内存，丢帧计数也会长期为 0 —— **有界性被绕过，比"少一次拷贝"严重得多**。

### 4.5 上限是**推导值**，不是拍出来的数

三个上限都由两个物理量算出来（推导入口只有一处：`MediaSource::Limits`）：

| 上限 | 公式 | 初值 |
|---|---|---|
| 每订阅者队列 | `max_bitrate_bps × latency_budget_ms / 8000` | 8 Mbps × 2 s = **2,000,000 B** |
| GOP 缓存 | `max_bitrate_bps × max_gop_ms / 8000` | 8 Mbps × 2 s = **2,000,000 B** |
| 每源输出带宽（**仅观测**） | `max_bitrate_bps × max_subscribers` | 8 Mbps × 16 = 128 Mbps |

为什么不当可调参数：**字节数是结果，不是参数**。直接写死字节数，换分辨率/换编码器设置就失准；
而从"能容忍几秒延迟""一个 GOP 多长"推出来，换场景只需要改物理量。

**每源的上限为什么用"人数"而不是"带宽"**：带宽是**事后**统计量 —— 接连接那一刻你不知道他要用多少，
拿它当准入只能"先接了再踢"，等于没有上限；而人数上限当场就能给出 fd 与内存的上界
（`max_subscribers × queueMaxBytes`）。带宽因此只做**观测**（`outputBitrateBudgetBps()` 进 `dumpStats()`），
不当准入条件。

### 4.6 源线程的职责边界

`SourcePump` 只做三件事：**读 → 推 → 收尾广播 EOS**。它**允许**随输入 IO 阻塞，
但绝不阻塞事件循环：不碰任何 socket、不碰 `Session`，跨线程只通过 `MediaSource::pushPacket`。

## 5. 测试计划（35 个用例：`media` 18 个 + `ntimed_media` 6 个 + `srcmgr` 10 个 + `ntimed_http` 1 个钩子用例）

> 分组口径：按**用例名子串**匹配，所以 `media` 组会连带匹配 `ntimed_media_*`（§8.6 已记录这个性质）。
> 上表按"实际唯一用例"统计：§5.1 的 14 个 + §5.4 的 4 个节流用例 = `media` 用例 18 个。

### 5.1 单线程纯策略（分组 `media`，14 个）

| # | 用例 | 覆盖维度 | 断言要点 |
|---|---|---|---|
| 1 | `media_packet_metadata_and_validation` | 正常 / 空 | 元数据可读；`droppable()` 三分支（**只有视频关键帧为 false**）；空 payload / 0 字节 / 非法流索引 → nullptr；负 dts 合法 |
| 2 | `media_queue_fifo_and_accounting` | 正常 | FIFO 顺序；`bytes()` 与 push/pop 一致；`Empty` 与 `Packet` 区分；默认上限 = 2,000,000 |
| 3 | `media_queue_drops_only_droppable` | 满 | 丢"最旧的一段"：一次腾空间丢掉**音频 + 视频各一**（成对）；关键帧绝不丢；字节数不超上限 |
| 4 | `media_queue_byte_limit_only` | 满 / 超大 | 唯一的字节上限生效；单包超上限：可丢 → `DroppedIncoming`，不可丢 → `RejectedNoSpace` |
| 5 | `media_queue_keyframe_makes_room_or_rejects` | 满 | 新关键帧挤掉最旧的一段进场；**队里只剩关键帧** → `RejectedNoSpace` |
| 6 | `media_queue_rejects_invalid_limits_and_eos` | 空 / 断开 | `setLimits(0)` / 超硬上限 → false 且保持默认；EOS 幂等；`pop` 返回 `EndOfStream`；`clear()` 返回丢弃数 |
| 7 | `media_gop_cache_keeps_only_last_gop` | 正常 | 第二个关键帧后旧 GOP 消失；`snapshot()` 第一项是关键帧；无起点时不缓存 |
| 8 | `media_gop_cache_byte_limit_keeps_keyframe` | 超大 | 超上限丢最旧可丢包、**关键帧保留**；默认上限 = 2,000,000 |
| 9 | `media_source_fanout_identical` | 正常 | **ROADMAP 验收**：3 个订阅者帧序列完全一致，每帧**指针相同**（零拷贝） |
| 10 | `media_source_slow_consumer_isolated` | 满 / 隔离 | 慢订阅者只丢自己的包，其他订阅者序列完整（FR-5.3） |
| 11 | `media_source_new_subscriber_starts_at_keyframe` | 中途接入 / 超大 | 新订阅者第一条必是视频关键帧；关键帧超队列上限 → `subscribe()` 返回 nullptr |
| 12 | `media_source_subscriber_lifetime_and_limits` | 断开 / 回收 | `shared_ptr` 释放后自动注销（NFR-6）；人数上限拒绝；码率/时长为 0 也拒绝；EOS 幂等且之后接入者立刻知道 |
| 13 | `media_source_dump_stats_minimal` | FR-6.1 | 一行统计含推导上限、满订阅带宽与各计数（**最小版**） |
| 14 | `media_subscriber_bind_poller_and_callbacks` | 形状 | 绑定 / 解绑语义；`setDrainCallback` 返回**上一个**；未绑定 poller 时不产生唤醒 |

### 5.2 线程打通（分组 `ntimed_media`，6 个）

| # | 用例 | 覆盖维度 | 断言要点 |
|---|---|---|---|
| 1 | `ntimed_media_queue_spsc_no_loss` | 超大 / 并发 | 2 万帧并发搬运：**不丢、不重、保序**（TSAN 同时验证加锁是否到位） |
| 2 | `ntimed_media_notify_coalesced_and_drained_on_poller_thread` | 满 / 正常 | 100 帧 → **恰好 1 次**唤醒；数据一条不少；drain 运行在**轮询线程**；取空后新数据能再排一次唤醒 |
| 3 | `ntimed_media_notify_rejected_when_poller_down` | 断开 | 唤醒被拒且计数；**下一次仍会重试**（标记复位）；数据仍在队列里 |
| 4 | `ntimed_media_pump_reads_until_eof` | 正常 / 读完 | 端到端：源线程 → 队列 → 唤醒 → 轮询线程 drain；`eof()` 真、计数准、EOS 已广播 |
| 5 | `ntimed_media_pump_error_reported` | 失败 | 读失败 → `eof()` 假且原因可读；EOS 仍广播；返回 Packet 却给空包 → 当错误停下 |
| 6 | `ntimed_media_pump_stop_is_clean` | 断开 | 停一个正在运行的源线程：`stop()` 真、幂等、线程回收；`eof()` 假；EOS 已广播；剩下的数据仍可取完 |

**为什么 `ntimed_media` 也能进 TSAN 严格组**：全程只用**无超时**等待（`Semaphore::wait()`）与
`sleep_for`，不碰本环境已知的 `condition_variable` 超时误报；并发用例要的是"不丢 / 不重 / 不卡"。

### 5.3 真接 Demuxer + 源管理（分组 `srcmgr`，8 个）

| # | 用例 | 覆盖维度 | 断言要点 |
|---|---|---|---|
| 1 | `srcmgr_demuxer_producer_reads_mp4` | 正常 | 真读 `sample.mp4` 到 EOF：320x240、视频 dts 单调、关键帧 1 个、音视频包都 > 0、`skippedUnknownStreams == 0`、`lastError()` 空 |
| 2 | `srcmgr_lazy_start_and_reuse` | 正常 | acquire 前源数为 0；两次 acquire 是**同一指针**且 `totalCreated == 1`；订阅者能收到真包且首包是关键帧 |
| 3 | `srcmgr_idle_release_without_cache` | 断开 | `idle = 0` 且**没有 poller**：句柄一放就回收（`sourceCount == 0`、`idle_released == 1`、`idle_schedule_failed == 0`）。<br>命名注意：**不要**叫 `*_immediate` —— 里面有 "media" 子串，会被纯逻辑的 `media` 分组过滤到（子串匹配的坑） |
| 4 | `srcmgr_idle_timer_releases_after_window` | 断开 | `idle = 50ms`：释放句柄后**先不回收**，等窗口到点才回收（`idle_released == 1`、`released == 0`） |
| 5 | `srcmgr_acquire_cancels_idle_timer` | 正常 / 断开 | 窗口内再 acquire → 复用同一源；等过原定时长后源**仍在**（计时被取消）；再放手才会回收 |
| 6 | `srcmgr_open_failure_not_silent` | 失败 | 打不开 / 空 path → nullptr、源数为 0、计数 +1、`lastError()` 非空 |
| 7 | `srcmgr_release_all_stops_sources` | 断开 | `releaseAll()` 立即停源；**句柄仍有效**（不悬垂）；再 acquire 会重建（`totalCreated == 2`）；`release()` 第二次返回 false |
| 8 | `srcmgr_dump_stats_minimal` | FR-6.1 | 一行统计含 sources / handles / created / idle_schedule_failed |

### 5.4 节流与统计接线（M5-d，7 个：6 个新增 + 1 个 http 钩子）

| # | 用例 | 分组 | 覆盖维度 | 断言要点 |
|---|---|---|---|---|
| 1 | `media_throttle_paces_by_wall_clock` | media | 正常 | `speed=1`：首包不等、40ms 处的包**真的等** ~40ms（下界 30ms / 上界 400ms）；`speed=100`：500ms 处的包几乎不等 |
| 2 | `media_throttle_aborts_immediately` | media | 断开 | 中止标志一开始就置位 → 立即返回 false、不进入等待；等待中途置位 → **一片之内**醒来（不睡满 5 秒） |
| 3 | `media_throttle_rejects_invalid_speed` | media | 非法 / 空 | 0 / 负数 / NaN / inf / 超硬上限 全部被拒，**保持原值** |
| 4 | `media_throttle_disabled_passes_through` | media | 正常 | `enabled=false`：时间戳差 4 秒也不等，`waitedMs()==0` |
| 5 | `srcmgr_dump_stats_json_has_manager_and_sources` | srcmgr | FR-6.1 | 片段以 `"source_manager":{` 开头；含 `sources` / `media_sources` / 每源的 `media_source`；**花括号配平**（能直接嵌进 `/api/stats`） |
| 6 | `srcmgr_throttle_limits_delivery_rate` | srcmgr | FR-3.5 | **8 倍速**读 2 秒样本：用时 **≥100ms**（没有全速灌入）且 **<1500ms**（没有慢到实时） |
| 7 | `ntimed_http_api_stats_extra_provider` | ntimed_http | FR-6.1 | 设置 provider 后 `/api/stats` 含该片段、原有计数仍在、JSON 正常收尾；**start() 之后再设置不生效**（返回空） |

## 6. 风险清单

| # | 风险 | 触发条件 | 应对 |
|---|---|---|---|
| 1 | 关键帧被丢 → 后面全花屏 | 上限判断把关键帧算成可丢 | `droppable()` 只对视频关键帧返回 false；用例 1/3/5 锁死；变异验证（把音频当可丢的历史版本已被本批取代） |
| 2 | 新订阅者从 GOP 中间接入 → 首帧解不出 | 缓存截断时丢了关键帧 | `GopCache` 收敛时**优先保留关键帧**；用例 8 / 11 |
| 3 | 订阅者泄漏 | 源强引用订阅者 | 源只持 `weak_ptr`；用例 12 |
| 4 | 上限被绕过（`setLimits(0)` 变无界） | 有人传 0 想"不限" | 明确拒绝 + 不生效；用例 6 / 12 |
| 5 | 单包比整个队列还大 → 该订阅者永远收不到东西 | 4K 关键帧 > 队列上限 | `RejectedNoSpace` / `subscribe` 失败 + 计数；用例 4 / 11 |
| 6 | **丢唤醒**（帧到了却没人取） | 清标记与取数据顺序写反；或唤醒被拒后没复位 | 顺序固定"先清再取"；拒绝时复位 + 计数；用例 2 / 3 |
| 7 | **唤醒风暴** | 忘记合并 | 每订阅者最多一个未决唤醒；用例 2 断言 100 帧 = 1 次 |
| 8 | 源线程泄漏 / 退出卡死 | 忘记 join，或 `ReadFn` 不可打断 | 析构 = `stop()`；`stop()` 必 join；中断契约写在头文件；用例 6 |
| 9 | 结束语义混淆 → 消费者永久等待 | "被停"当成"读完"，或退出不广播 EOS | 三者都广播 EOS；`eof()` 只表示读完；用例 4 / 5 / 6 |
| 10 | 跨线程读 `Stats` 得到撕裂值 | 取值接口返回内部引用 | 全部返回**快照** |
| 11 | **只按字节后，小包洪泛会放大记账外开销** | 生产者发出大量极小包（例如 1 字节/包）：2 MB 上限可容纳 200 万包，每包 `deque` 节点 + `shared_ptr` 控制块 ≈ 百字节 → 实际内存远大于 2 MB | 真实媒体包 ≥ 数十字节（128 kbps / 50 fps 的音频帧 ≈ 320 B），故当前量级下不构成问题；**未加"包数防呆硬顶"**（用户明确要求删掉帧数上限），列为 §8 未决 7，需要时一行即可补 |
| 12 | 上限推导参数配错（码率填成 8 bps） | 手工传 `Limits` | `setLimits` 校验三者为正数且不超硬上限；推导出的字节数还要过各组件硬上限；用例 12 |
| 13 | **订阅管理与源线程并发**（M5-c 实测踩到） | 调用方在源线程推流的同时 `subscribe()`（M6 的 HTTP 线程就是这种形态） | 内部 `_mutex` 保护订阅集合 / `GopCache` / `_ended`；**唤醒移到锁外**；`media` / `ntimed_media` / `srcmgr` 全在 TSAN 严格组 —— 本批就是靠它抓到 **13 处真 data race** |

## 7. 决策记录（为什么这么选 / 排除了什么）

| 决策 | 选择 | 排除的选项与原因 |
|---|---|---|
| 类名 | **`MediaPacket`**（不是 `MediaFrame`） | 与 `ARCHITECTURE.md` §4 措辞一致；v0.1/v0.2 搬运的是**编码后**的包，v0.3 的解码帧不该同名 |
| 数据表示 | `shared_ptr<const vector<uint8_t>>` | 排除裸指针 + 长度（生命周期手动管）；排除 `Buffer`（network 层的可变缓冲，会造成反向依赖）；排除每订阅者一份拷贝（NFR-4） |
| 可丢性判断 | 放在包上（`droppable()`） | 排除"队列按 stream_index 猜"（策略散落、易漏） |
| **【本批】音频可丢 + 成对丢** | 唯一不可丢 = **视频关键帧**；丢"最旧的一段"（音视频一起走） | 排除"音频一律不可丢"（拥塞时只丢视频 → 视频跳着走、音频连续放，音画越走越偏，且音频持续挤占队列）；排除"超限直接断开"（`broken()` 已能表达，断开留给连接层按 FR-5.2 判断） |
| 【本批】队列上限**只按字节** | `max_bytes = 码率上限 × 延迟额度`，删掉"64 帧" | 排除"帧数 or 字节先到者为准"（两种量纲混杂、场景相关；4K 下 64 帧不到 1 秒、低码率下几十秒）；排除"保留帧数上限当安全网"（用户明确要求删掉；包数放大问题列为未决 7） |
| 【本批】上限**推导**而非拍值 | 推导入口只有 `MediaSource::Limits::queueMaxBytes()/gopMaxBytes()` | 排除"各处写死 8MB"（换分辨率/编码器就失准）；排除"外部配置文件"（v0.1 没有配置层，SPEC FR-7 只要求命令行） |
| 【本批】GOP 上限 = 码率 × 最大 GOP 时长 | 动态增长 + 推导上限 | 排除"拍 8MB"（与场景无关）；排除"按帧数缓存"（同队列理由） |
| 【本批】每源上限用**人数** | `max_subscribers` 是硬边界；带宽只做观测 | 排除"用带宽做准入"（带宽是事后统计量，接连接时不可预判 → 等于没有上限）；排除"只限带宽不限人数"（fd/内存无上界） |
| 【本批】统计最小版 | 几个 `atomic` 计数 + `dumpStats()` 一行文本 | 排除"现在就建 StatsCenter/JSON 接口"（M5-d 才接层；`AI_COLLAB §3.6` 不过度设计）；排除"计数器非原子"（HTTP 线程要读） |
| 【本批】GopCache 与 FrameQueue 规则统一 | 两处唯一不可丢的都是视频关键帧 | 排除"继续让两边不同"（同一个概念两套规则，迟早有人按错的那套理解） |
| 【本批】时间戳基准只留占位 | 头文件里写清签名与落点（M6 的 `FlvSender`） | 排除"现在就实现"（没有输出层可接，会是死代码）；排除"不提"（M6 会到处找地方塞，且这是 ARCHITECTURE §4 的明确要求） |
| 队列满策略 | 丢**队头方向**最早的可丢包 | 排除"丢新来的"（老帧最没用）；排除"丢整个 GOP"（复杂度翻倍） |
| `push` 返回值 | 枚举**四态** | 排除 `bool`；排除三态（"丢掉新包"与"不可丢的包进不去"动作不同：前者无需动作，后者必须断开） |
| 上限非法值 | `setLimits`/`setMaxBytes` 返回 `bool`，`0` 拒绝 | 排除 `0 = 不限`（有界性不可协商）；排除构造函数抛异常 |
| 订阅者生命周期 | 源持 `weak_ptr`，惰性注销 | 排除"源持 `shared_ptr` + 显式退订"（消费者异常退出就泄漏，NFR-6 不达标） |
| `subscribe()` 灌 GOP 失败 | **返回 nullptr** | 排除"照常返回、让消费者从中间接"（首帧解不出 = 静默错误） |
| 数据通道 | **队列**（加锁 SPSC），唤醒只走 `async` | 排除"帧本体走 `async`"（缓冲变成 poller 任务队列，上限失真、丢帧计数为 0） |
| 队列同步 | `std::mutex` 一把锁覆盖全部状态 | 排除无锁 SPSC（当前两台线程 × O(1) 操作，收益看不出来）；排除"只保护部分成员" |
| 唤醒策略 | **合并**（每订阅者最多一个未决唤醒） | 排除"每帧投一次"（10 客户端 × 60fps = 600 次/秒纯浪费） |
| 取数据顺序 | 消费者**先清未决标记，再 drain** | 排除"先 drain 再清"（drain 期间新帧会以为有人被唤醒 → 丢唤醒） |
| `async` 被拒 | 计数 + **复位**未决标记 | 排除"不复位"（订阅者永久卡死）；排除"重试自旋"（拖死源线程） |
| 源线程独立成类 | `SourcePump`（读回调由调用方提供） | 排除"线程塞进 `MediaSource`"（策略类被迫依赖线程/IO，确定性用例就没了） |
| "结束"的语义 | 三种结束都广播 EOS；`eof()` 只表示正常读完 | 排除"只有读完才广播 EOS"（被停/出错时消费者永久等待） |
| 空包处理 | 当作**错误**停下并记原因 | 排除"跳过继续读"（ReadFn 坏掉时变成死循环 + 什么都不做的静默故障） |

| 【M5-c】真读用 `DemuxerProducer` 包装 | 把 `Demuxer` 包成 `ReadFn`（读回调由调用方提供） | 排除"让 `SourcePump` 直接持有 `Demuxer`"（pump 被 FFmpeg 绑死，M5-b 的假源用例就没了）；排除"在 `MediaSource` 里读文件"（策略类又被迫依赖 IO） |
| 【M5-c】每包**拷一次** | `AVPacket → shared_ptr<const vector<uint8_t>>` | 排除"真零拷贝"（要改 `MediaPacket` 的 payload 类型 + `av_packet_ref` 包装，波及 M5-a/b 全部代码）；NFR-4 的"单路 < 5% 单核"远够，而**扇出到 N 个客户端仍然是零拷贝** |
| 【M5-c】非音视频流跳过但**计数** | 只把 Video/Audio 喂给媒体层 | 排除"当作错误"（一条字幕轨不该让整个源失败）；排除"静默跳过"（等于悄悄丢数据） |
| 【M5-c】时间戳换算失败**仍发包** | 用上次值 + 单调钳制，只累加计数 | 排除"丢包"（丢数据比时间戳不够精确更糟）；排除"静默填 0"（那是伪造时间戳） |
| 【M5-c】中止标志接进 `interrupt_callback` | `Demuxer::setAbortFlag()` + `SourcePump::stopFlag()` | 排除"只靠读超时"（停一个卡在 `av_read_frame` 的源要等满 5s 才回来）；排除"另起线程强制关"（`DESIGN_M4` §7 已排除） |
| 【M5-c】句柄用「自定义 deleter + 捕获强引用」 | `releaseAll()` 之后句柄依然有效 | 排除"裸指针 + 管理器强引用"（`releaseAll` 后句柄悬垂）；排除"句柄持有 `shared_ptr<Entry>`"（管理器被句柄拖住，无法真正回收） |
| 【M5-c】IO / 停线程都在**锁外** | 只在对 `_entries` 增删时持锁 | 排除"持锁 open"（open 可能超时，占着锁 = 阻塞轮询线程挂计时器）；排除"持锁 join"（同理） |
| 【M5-c】空闲计时挂不上就**立即释放** | 计数 + Warn + 立即回收 | 排除"静默不释放"（源线程与文件句柄泄漏，且没有任何迹象）；排除"重试"（poller 都没了，重试无意义） |
| 【M5-c】`idle_release_ms = 0` 合法 | 含义是"不缓存源"（最保守的一档） | 排除"0 一律拒绝"（那是容量类参数的习惯；这里 0 不是无界而是"立刻回收"）；好处是用例可以完全确定性 |
| 【M5-c】订阅管理**线程安全**（TSAN 抓出来后修正） | `MediaSource` 内部一把锁保护订阅集合 / `GopCache` / `_ended`；**唤醒移到锁外** | 排除"要求调用方把 subscribe 投递回源线程"（调用方根本没有源线程的句柄；M6 的 HTTP 线程只会直接调）；排除"锁内唤醒"（`async` 在轮询线程上会**内联执行** drain 回调 → 重入死锁）；排除"唤醒清单用成员暂存"（两个线程的调用会互相清空） |
| 【M5-d】节流**默认开**、`speed` 可调 | `Throttle::Config{enabled = true, speed = 1.0}` | 排除"默认关"（FR-1.2 要的就是按时间轴节流，默认关等于指望每个调用方都记得开）；排除"按包数限速"（与帧率耦合，换个流就失准） |
| 【M5-d】节流**不补偿**落后 | 已经超过应当的时刻就直接放行 | 排除"追赶"（落后时突发推送会把下游刚排空的队列再冲爆一次） |
| 【M5-d】节流**可被打断**（≤50ms 分片） | 中止标志一置位立刻返回 false | 排除"一次睡到底"（`stop()` 的 join 要等满一拍，惰性释放与关停都会变慢） |
| 【M5-d】`/api/stats` 用**单钩子**而不是注册表 | `HttpServer::setExtraStatsProvider()` + 各模块 `dumpStatsJson()` | 排除"现在就建 `StatsCenter`"（当前只有一个 provider，`AI_COLLAB §3.6` 要求抽象有第二个实现者）；排除"让 http 依赖 media"（破坏分层）；排除"固定缓冲 snprintf"（追加片段后长度不可控 → 静默截断） |
| 【M5-d】`broken()` 只做**巡检** | `brokenSubscriberIds()` 进 JSON，断连留 M6 | 排除"媒体层自动 unsubscribe"（现在没有连接对象可断，且会让"谁断的"不可追溯） |
| 【M5-d】计数与状态在**同一临界区** | `releaseEntry` 在锁内"摘条目 + 计数" | 排除"join 完再计数"：节流让 join 变慢后，外部会读到 `sourceCount()==0` 但计数仍为 0 的中间态（用例已抓到） |

## 8. 未决事项

| # | 事项 | 何时定 |
|---|---|---|
| 1 | `max_subscribers` 初值 16 是否合适 | **M6** 接入真实连接（HTTP 层）后按 `totalRejected` 观察 |
| 2 | ~~队列上限 64 帧/8MB 是否合适~~ | ✅ **已定（M5-b′）**：只按字节，`= 码率上限 × 延迟额度`（初值 8 Mbps × 2 s = 2 MB） |
| 3 | ~~GOP 缓存 8MB 是否够~~ | ✅ **已定（M5-b′）**：`= 码率上限 × 最大 GOP 时长`（初值 2 MB） |
| 4 | ~~音频不可丢在拥塞下导致队列只涨不落~~ | ✅ **已定（M5-b′）**：改为**音视频成对丢**（丢最旧的一段） |
| 5 | 订阅者的**独立时间戳基准**（`ARCHITECTURE.md` §4） | **M6**：`FlvSender` 按订阅者维护；本批已在 `Subscriber` 头文件留**占位注释**（签名与落点都写好） |
| 6 | ~~`StatsCenter` / `/api/stats` 接入~~ | ✅ **最小版已实现（M5-b′）**：`atomic` 计数 + `dumpStats()`；完整接线（JSON、源数/客户端数聚合）在 **M5-d** |
| 7 | **包数防呆硬顶**（只按字节后，小包洪泛会放大记账外内存开销，见 §6 风险 11） | 待 M6 实测真实包尺寸；若确有必要，加一个**内部**硬顶（非可调业务上限），一行代码 |
| 8 | `FrameQueue` 的锁粒度是否需要升级（无锁 SPSC 环形缓冲） | M6 拿到真实吞吐后；当前无数据支持升级 |
| 9 | 唤醒合并是否够（单次 drain 总取不完时要不要批量投递） | M6 实测；当前 100 帧 = 1 次唤醒，余量很大 |
| 10 | **循环播放**（FR-1.2 的"可选循环"） | **M6**：要"读完重开 + 时间戳基准重置"，与源生命周期纠缠 —— M5-c 已明确不做（用户已确认） |
| 11 | 空闲释放阈值 60s 是否合适（真实播放里"暂停一分钟再回来"就要重建源） | M7 采集：看 `totalIdleReleased` 与重建开销，必要时调大 |
| 12 | `StatsCenter`（`ARCHITECTURE.md` §2 列出的模块）要不要现在抽 | 等**第二个** provider 出现（当前只有 media 一个；`AI_COLLAB §3.6` 要求抽象有第二个实现者） |
| 13 | 节流的对齐基准：现在按**混合 dts**（音视频交替） | M6 实测音画同步后再看是否需要"以音频时钟为准"（FR-3.4 规定音频为主时钟，但那偏封装/播放侧） |
| 14 | `broken()` 订阅者的**真实断连动作** | **M6**：接上 `FlvSender` + `Session` 后，在送数据前巡检 `brokenSubscriberIds()` 并主动断开 |

## 9. 假设清单与影响面（`AI_COLLAB.md` §1 的②③）

**假设**：

- 输入规模：单源 1080p 约 2~5 Mbps；**码率上限取 8 Mbps**（留余量）；订阅者 ≤ 16。
- 延迟额度 **2 s**、最大 GOP 时长 **2 s**：直播式重放的可接受延迟与常见 GOP 长度。
- 调用方式：`pushPacket` / `notifyIfNeeded` 由**源线程**串行调用；`pop` / drain 由消费者线程串行调用；
  同一个 `FrameQueue` 只有**一个生产者、一个消费者**（SPSC）。
- 运行环境：Linux x86_64 / C++17；`media/` 不依赖 FFmpeg。
- 失败容忍：**丢帧但绝不静默**（计数 + 返回值）；关键帧腾不出空间时宁可让调用方断开订阅者；
  源结束时一定广播 EOS（绝不让消费者永久等待）。

**影响面（M5-a → M5-b′ 累计）**：

- 新增：`src/media/{media_packet,frame_queue,gop_cache,media_source,source_pump,demuxer_producer,source_manager,throttle}.h/.cpp`、
  `tests/test_media.cpp`、`tests/test_media_ntimed.cpp`、`tests/test_source_manager.cpp`、本文件。
- 改动：`CMakeLists.txt`、`tests/CMakeLists.txt`（分组 `media` / `ntimed_media` / `srcmgr`）、`src/mzmedia.h`、
  `scripts/tsan.sh`（严格组）、`src/ffmpeg/demuxer.h/.cpp`（`setAbortFlag`）、
  `src/http/http_server.h/.cpp`（`setExtraStatsProvider` + `/api/stats` 改 `std::string` 拼接）、
  `docs/SPEC.md`（FR-5.1 / FR-5.2 修订）、`docs/ROADMAP.md`、`docs/TESTING.md`、`docs/DESIGN_M4.md`、`CHANGELOG.md`。
- **不动**：`core/`、`network/` 任何现有代码；`media/` 的既有限流 / 丢帧策略。
