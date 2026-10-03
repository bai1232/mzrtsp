# M5 设计：媒体分发层（Media）

> 上游需求：`docs/SPEC.md` §5.5（FR-5.1 / FR-5.2 / FR-5.3）、`NFR-6`、`FR-6.1`；`docs/ARCHITECTURE.md` §3（线程模型）/ §4（一源多消费者）/ §5（背压与慢客户端）
> 上游实现：`docs/DESIGN_M2.md`（`Session` 发送队列与背压）、`docs/DESIGN_M4.md`（`Demuxer` 产包）
> 本文件对应 ROADMAP 的 **M5 Media** 一行，拆成 M5-a / M5-b / M5-c / M5-d 四批。

## 1. 范围与验收

| 批次 | 内容 | 状态 |
|---|---|---|
| **M5-a** | `MediaPacket`、`FrameQueue`、`GopCache`、`Subscriber`、`MediaSource`（**单线程策略**，帧由外部喂入） | ✅ 已完成 |
| **M5-b** | **线程打通**：`FrameQueue` 加锁成 SPSC；`Subscriber` 绑定消费者线程 + **唤醒合并**；`SourcePump`（源线程驱动一个**可打断**的读回调） | ✅ 已完成（本文件） |
| M5-c | `SourceManager`（懒启动 / 空闲 60s 释放）+ `DemuxerProducer`（把 M4 的 `Demuxer` 接到 `SourcePump` 的读回调上，`interrupt_callback` 接停止请求） | 后续 |
| M5-d | 节流（FR-3.5：按墙钟对齐推送）、30s 写阻塞断开与 `broken()` 的端到端串通、丢帧计数进 `/api/stats`（FR-6.1） | 后续 |

**验收（对应 `docs/ROADMAP.md:15`，按批次分配）**：

| 验收点 | 落在哪批 |
|---|---|
| 3 个订阅者收到**完全一致**的帧序列（每帧指针相同 = 零拷贝） | M5-a ✅ |
| 慢订阅者丢帧、但**关键帧与音频不丢**，且不影响其他订阅者（FR-5.2 / FR-5.3） | M5-a ✅ |
| 每个订阅者队列上限 **64 帧或 8MB**（先到者为准，FR-5.1） | M5-a ✅ |
| 订阅者断开后队列 100% 回收（NFR-6） | M5-a ✅ |
| 源线程 ⇄ 消费者线程之间**数据不丢、不重、不卡**；唤醒**既不丢也不刷**（合并） | M5-b ✅ |
| 源被停止 / 读失败 / 读完时，消费者都能**明确知道"不会再有数据"**（绝不永久等待） | M5-b ✅ |
| 懒启动、空闲 60s 释放、源与线程 100% 回收 | M5-c |
| 10 路并发压测、1 小时长跑 | M6 / M7 |

## 2. 分层位置与依赖方向

```
Output(未做) ← Media ← Network(EventPoller) / Core
```

- `media/` 依赖 `core/`（日志、`Semaphore`）与 **`network/` 的 `EventPoller`**（M5-b 起，用来做跨线程唤醒）。
  分层顺序 `core → network → http → ffmpeg → media → output` 允许这个方向。
- **不依赖 FFmpeg**：帧由外部喂给 `MediaSource`，所以"数据从哪来"与"分发策略"解耦。
  好处是策略（上限 / 丢帧 / GOP）与线程穿透都能用**确定性用例**覆盖，不需要样本文件。
  这与 SPEC 成功标准 SC-2（加格式不改核心）同一个思路：**边界靠接口，不靠实现耦合**。
- 反向依赖检查（`media/` 不得出现 http / ffmpeg）：
  `grep -rn "http/\|ffmpeg/" src/media/` → 应为空

## 3. 契约

**线程契约（M5-b 起）**：数据方向是 **SPSC** —— 源线程只调 `pushPacket` / `notifyIfNeeded`，
消费者线程只调 `queue().pop()` / drain 回调；两者**可以不是同一线程**。
内部只在 `FrameQueue` 与 `Subscriber` 的绑定状态上各用一把锁，**不做回调嵌套加锁**。
M5-a 的其余接口（`GopCache`、`MediaSource` 的订阅管理）仍假定"只在源线程调用"。

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

    // 是否允许被丢弃：视频非关键帧 = 可丢；音频、视频关键帧 = 不可丢
    bool droppable() const;
};
```

- **对象创建后不可变**，所以可以被多个线程只读共享（可变只有 `shared_ptr` 的引用计数）。
- **负时间戳合法**：B 帧/部分容器的首包 dts 就是负的，不能拦（用例 1 覆盖）。
- 不校验 pts/dts 的相对大小 —— 那是封装层的事（`MonotonicGuard`）。

**为什么 `droppable()` 放在包上而不是队列里**：策略要判断的是"这个包丢了会不会让后面全花屏"，
这是包自身的性质。音频没有关键帧概念，丢了就是**静音空洞**，所以音频一律不可丢
（FR-5.2 说的"丢弃非关键帧"针对的是视频）。

### 3.2 `FrameQueue`（`frame_queue.h`）

每个订阅者一个的**有界**队列（FR-5.1）。

> **【M5-b 变更】线程契约从"单线程"改为 SPSC + 一把互斥锁**：源线程只 `push`、消费者线程只 `pop`；
> 所有取值接口返回**快照**而不是引用 —— 返回 `const Stats&` 会让调用方读到正在被改的计数，
> 那是"看着对、TSAN 才报"的典型数据竞争。
> 为什么这么改：M5-b 选定"**数据通道 = 队列，唤醒 = `async`**"（见 §4.4）。若让帧本体走 `async`，
> 真正的缓冲会变成 poller 的任务队列（65536 条 ≈ 65536 帧），FR-5.1 的 64 帧/8MB 就管不住内存了。

```cpp
class FrameQueue {
public:
    struct Limits { size_t max_packets = 64; size_t max_bytes = 8u * 1024 * 1024; };
    struct Stats  { uint64_t pushed, popped, dropped_non_key,
                             dropped_incoming, rejected_no_space, dropped_on_clear; };

    enum class PushResult : uint8_t { Accepted, DroppedToMakeRoom,
                                      DroppedIncoming, RejectedNoSpace };
    enum class PopResult  : uint8_t { Packet, Empty, EndOfStream };

    FrameQueue();                                   // 默认 Limits
    bool   setLimits(const Limits &limits);         // false = 非法（0 / 超硬上限），不生效
    Limits limits() const;                          // 快照

    PushResult push(MediaPacket::Ptr packet);       // 源线程
    PopResult  pop(MediaPacket::Ptr *packet);       // 消费者线程；返回码区分"空"与"流结束"
    bool       markEndOfStream();                   // true = 状态改变（重复调用返回 false）
    size_t     clear();                             // 返回被丢弃的包数（NFR-6 回收用）

    size_t packets() const;  size_t bytes() const;  bool empty() const;  bool endOfStream() const;
    Stats  stats() const;                           // **快照**（不是引用）
};
```

**`push` 的四种结果**（形状定死，调用方必须处理 —— 实现时从三态改成四态：
"丢掉新包自己"与"不可丢的包进不去"是**两种不同语义**，前者调用方什么都不用做，后者必须断开订阅者）：

| 结果 | 含义 | 调用方该做什么 |
|---|---|---|
| `Accepted` | 已入队，没有丢任何包 | 继续 |
| `DroppedToMakeRoom` | 已入队，但为腾空间丢了若干**可丢**的旧包 | 继续；计数在 `stats()` |
| `DroppedIncoming` | 没入队，丢的是**新包自己**（新包可丢） | 继续（正常降级，无需动作） |
| `RejectedNoSpace` | 没入队，且新包**不可丢**（关键帧/音频） | **断开该订阅者**（FR-5.2）；`Subscriber::broken()` 已置位 |

### 3.3 `GopCache`（`gop_cache.h`）

最近 **1 个 GOP** 的缓存，只为一个目的：让**中途接入**的订阅者从关键帧开始（否则首帧解不出来，花屏到下一个关键帧）。

```cpp
class GopCache {
public:
    enum class FeedResult : uint8_t { StartedNewGop, Cached, Skipped };

    explicit GopCache(size_t max_bytes = 8u * 1024 * 1024);
    bool   setMaxBytes(size_t bytes);        // 0 → false
    size_t maxBytes() const;

    FeedResult feed(const MediaPacket::Ptr &packet);   // 视频关键帧 = StartedNewGop（丢弃上一个 GOP）
    std::vector<MediaPacket::Ptr> snapshot() const;    // 第一项必是当前 GOP 的关键帧
    size_t packets() const;  size_t bytes() const;
    size_t clear();
    const Stats &stats() const;   // started_gop / cached / dropped / skipped
};
```

- 关键帧到达 ⇒ **丢弃整个旧 GOP** 重新开始（只留最近一个）。
- 还没有关键帧时**不缓存任何包**（否则 `snapshot()` 会给出一个解不出的开头）。
- 单个 GOP 超过 `max_bytes` 时**收敛**：丢最旧的**可丢**包，**关键帧必须保留**
  （否则"从关键帧接入"这条保证就没了）—— 这是有意的取舍，写进 §7 决策记录。
- **与 `FrameQueue` 的有意差异**：缓存里音频**可丢**（新订阅者晚几毫秒听到音频无妨，但缓存必须有界）；
  队列里音频不可丢（实时流的丢音频 = 静音空洞）。差异写在这里，避免被当成 bug。

### 3.4 `MediaSource` / `Subscriber`（`media_source.h`）

```cpp
class Subscriber {
public:
    using Ptr = std::shared_ptr<Subscriber>;
    using Id  = uint64_t;
    using DrainCallback = std::function<void(const Ptr &)>;   // 消费者线程回调

    Id id() const;  FrameQueue &queue();  const FrameQueue &queue() const;
    bool broken() const;                                      // 不可丢的包进不去 → 调用方应断开它

    // ---- M5-b：绑定消费者线程 + 唤醒（跨线程投递的"唤醒通道"）----
    bool bindPoller(const EventPoller::Ptr &poller);          // nullptr = 解绑（同步模式）
    EventPoller::Ptr poller() const;
    DrainCallback setDrainCallback(DrainCallback callback);   // 返回上一个

    bool notifyIfNeeded();       // 源线程调用：**最多一个未决唤醒**（合并）
    bool clearNotifyPending();   // 消费者线程在 drain **之前**调用（先清再取，才不丢唤醒）

    uint64_t notifyCount() const;           // 真的投递出去的唤醒次数
    uint64_t notifyRejectedCount() const;   // 被拒次数（poller 已退出 / 任务队列满）
    uint64_t notifyCoalescedCount() const;  // 被合并掉的次数
};

class MediaSource {
public:
    using Ptr = std::shared_ptr<MediaSource>;

    struct Limits {
        FrameQueue::Limits queue;                    // 每订阅者一份
        size_t max_gop_bytes   = 8u * 1024 * 1024;   // GopCache 上限
        size_t max_subscribers = 16;                 // 有界（§4.3 AI_COLLAB：0/无界不是选项）
    };
    struct PushStats { size_t delivered, dropped_to_make_room, dropped_incoming,
                              rejected_no_space, notified, subscribers; };

    MediaSource();
    bool   setLimits(const Limits &limits);      // false = 非法，不生效
    Limits limits() const;

    Subscriber::Ptr subscribe();                 // nullptr = 超过 max_subscribers / 灌不进关键帧
    bool   unsubscribe(Subscriber::Id id);       // false = 该 id 不存在
    size_t subscriberCount();                    // 先清理已析构的订阅者（weak_ptr 自动注销）
    bool   endOfStream();                        // 广播流结束；true = 状态改变

    PushStats pushPacket(MediaPacket::Ptr packet);
    const GopCache &gopCache() const;

    // 观测（FR-6.1 的素材）
    uint64_t totalDelivered() const;  uint64_t totalDropped() const;
    uint64_t totalDroppedIncoming() const;
    uint64_t totalRejected() const;   uint64_t totalSubscribe() const;
    uint64_t totalSeedFailed() const; uint64_t totalAutoUnsubscribe() const;
    uint64_t totalBroken() const;     // 因"不可丢的包进不去"被置为 broken 的订阅者数
};
```

- **订阅者生命周期**：源持有 `std::weak_ptr<Subscriber>`（`ARCHITECTURE.md` §4「引用计数即生命周期」）。
  消费者把 `shared_ptr` 一放，源在下次 `pushPacket` / `subscriberCount` 时**惰性注销**并计数 ——
  源不需要"被通知"，也不会因消费者异常退出而残留队列（NFR-6）。
- **`subscribe()` 会灌入 GOP 缓存**：按 `snapshot()` 顺序入队，保证第一条是视频关键帧。
  若关键帧本身超过队列 `max_bytes`（单包比整个队列还大）→ **订阅失败**（返回 nullptr +
  `totalSeedFailed()`），而不是让消费者拿到"从中间开始"的流。
- **`endOfStream()`**：给每个订阅者队列打 EOS 标记 + 各补一次唤醒（队列里可能还有数据没取完），
  消费者 `pop()` 会拿到 `EndOfStream`（区分"暂时没数据"与"不会再有数据"—— 与 `Demuxer` 的 EOF/Error 分离同一原则）。
- **`pushPacket` 的失败路径只置 `broken()`，不在这里断连接**：断连接是连接层的事，媒体层只负责明确告知。

### 3.5 `SourcePump`（`source_pump.h`，M5-b）

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
    bool eof() const;             // **正常读完**才算（"被停"不算 —— 调用方必须能区分）
    std::string lastError() const;
    uint64_t readCount() const;   // 读到包的次数
    uint64_t pushedCount() const; // 调用 pushPacket 的次数
};
```

- `stop()` 会 **join**，所以 `ReadFn` **必须能被打断**（定期查 `stopRequested()`）——这是它唯一的硬约束。
- 三种结束（**读完 / 读失败 / 被停**）都广播 EOS；`eof()` 与 `lastError()` 把"读完"和"失败"分开。
- `ReadFn` 说好返回 `Packet` 却给空包 → **当作错误停下来**并记原因；否则就是一个"死循环 + 什么都不做"的静默故障。

## 4. 关键机制

### 4.1 零拷贝分发

`MediaPacket::Payload` 是 `shared_ptr<const std::vector<uint8_t>>`，`MediaSource::pushPacket`
把**同一个 `Ptr`** 放进 N 个订阅者的队列 —— 只增加引用计数，不复制字节。
用例 `media_source_fanout_identical` 直接用**指针相等**来断言"三个订阅者拿到的是同一帧"。

### 4.2 上限与溢出策略（FR-5.1 / FR-5.2）

- 两个上限**先到者为准**：`packets() >= max_packets` 或 `bytes() >= max_bytes` 都算满。
- 满了以后来新包：只丢 `droppable()` 的旧包（从队头开始找第一个可丢包）腾空间。
- 腾不出空间：可丢的新包 → `DroppedIncoming`；**不可丢**的新包 → `RejectedNoSpace`，**不静默丢**。
- 所有丢弃都进 `stats()` 且能被 `MediaSource` 汇总 —— 可观测是辅助，上限才是防线。
- 单包比整个字节上限还大：可丢 → 丢新包；不可丢 → 报错（调用方断开），**绝不为一个包把队列清空**。

### 4.3 策略与线程分开（M5-a 的取舍，M5-b 已兑现）

策略（丢谁 / 上限怎么算 / GOP 怎么截断）与"谁在什么线程调用"是**正交**的两件事。
混在一起写，"慢消费者"这种场景就只能靠 sleep 和时序去凑，用例会变成 flaky。
所以 M5-a 把策略做成**纯函数式单线程组件**（进 TSAN 严格组），线程与投递留给 M5-b。

M5-b 兑现了这一点：`FrameQueue` 只多了一把锁，**丢帧 / GOP 策略一行没改**，M5-a 的 13 个用例
加锁之后原样通过（这就是"正交"的验证方式）。

### 4.4 跨线程通道：数据走队列、唤醒走 `async`（M5-b）

两条通道分开，是这一层的核心：

| 通道 | 载体 | 特点 |
|---|---|---|
| **数据** | 该订阅者的 `FrameQueue`（加锁 SPSC） | 一帧只 `create` 一次，N 个订阅者共享同一个 `shared_ptr`（零拷贝）；上限与丢帧策略就在这里生效（FR-5.1 / FR-5.2） |
| **唤醒** | `EventPoller::async()` | **只是"去看一眼"**，不携带数据；因此可以合并 |

三条必须做对的细节：

1. **唤醒合并**：每订阅者最多一个未决唤醒（`_notify_pending`）。100 帧只产生 **1 次**跨线程投递
   —— 用例 `ntimed_media_notify_coalesced_and_drained_on_poller_thread` 断言的就是这个数。
2. **先清标记，再 drain**：消费者拿到唤醒后**先**清未决标记、**再**取数据。顺序反了就会丢唤醒
   （drain 期间新到的帧以为"已经有人被唤醒了"，结果没人来取）。
3. **投递被拒要复位标记**：`async` 返回 false（poller 已退出 / 任务队列满）时**必须复位**未决标记，
   否则这个订阅者会被永久卡住；同时计数（`notifyRejectedCount`）。

为什么不把帧本体交给 `async`：真正的缓冲会变成 poller 的任务队列（上限 65536 条 ≈ 65536 帧），
FR-5.1 的"64 帧/8MB"就管不住内存，而且丢帧计数会长期为 0 —— **有界性被绕过，比"少一次拷贝"严重得多**。

### 4.5 源线程的职责边界

`SourcePump` 只做三件事：**读 → 推 → 收尾广播 EOS**。它**允许**随输入 IO 阻塞（读文件/网络），
但绝不阻塞事件循环：它不碰任何 socket、不碰 `Session`，跨线程只通过 `MediaSource::pushPacket`。
退出前一律广播 EOS，保证消费者不会永久等待 —— 这是"静默失败"最容易藏身的地方。

## 5. 测试计划（19 个用例：`media` 13 个 + `ntimed_media` 6 个）

### 5.1 M5-a：单线程纯策略（分组 `media`，13 个）

| # | 用例 | 覆盖维度 | 断言要点 |
|---|---|---|---|
| 1 | `media_packet_metadata_and_validation` | 正常 / 空 | 元数据可读；`droppable()` 三分支；空 payload / 0 字节 / 非法流索引 → nullptr；负 dts 合法 |
| 2 | `media_queue_fifo_and_accounting` | 正常 | FIFO 顺序；`packets()/bytes()` 与 push/pop 一致；`Empty` 与 `Packet` 区分 |
| 3 | `media_queue_drops_only_droppable` | 满 | 帧数上限触发时：**音频包与视频关键帧不丢**，只丢视频非关键帧 |
| 4 | `media_queue_limits_first_wins` | 满 / 超大 | 字节上限独立触发；单包超过 `max_bytes`：可丢 → `DroppedIncoming`，不可丢 → `RejectedNoSpace` |
| 5 | `media_queue_keyframe_makes_room_or_rejects` | 满 | 新关键帧到来：丢掉可丢包让位（`DroppedToMakeRoom`）；队列里已无可丢包（关键帧 + 音频）→ `RejectedNoSpace` |
| 6 | `media_queue_rejects_invalid_limits_and_eos` | 空 / 断开 | `setLimits(0)` → false 且不生效；`markEndOfStream()` 幂等；`pop` 返回 `EndOfStream`；`clear()` 返回丢弃数 |
| 7 | `media_gop_cache_keeps_only_last_gop` | 正常 | 第二个关键帧后旧 GOP 全部消失；`snapshot()` 第一项是关键帧；无起点时不缓存 |
| 8 | `media_gop_cache_byte_limit_keeps_keyframe` | 超大 | GOP 超上限 → 丢最旧**可丢**包，**关键帧保留**；缓存里音频可丢 |
| 9 | `media_source_fanout_identical` | 正常 | **ROADMAP 验收**：3 个订阅者帧序列完全一致，且每帧**指针相同**（零拷贝） |
| 10 | `media_source_slow_consumer_isolated` | 满 / 隔离 | 慢订阅者只丢自己的包，其他订阅者序列完整（FR-5.3） |
| 11 | `media_source_new_subscriber_starts_at_keyframe` | 中途接入 / 超大 | 新订阅者第一条必是视频关键帧；关键帧超队列上限 → `subscribe()` 返回 nullptr |
| 12 | `media_source_subscriber_lifetime_and_limits` | 断开 / 回收 | `shared_ptr` 释放后自动注销（NFR-6）；`max_subscribers` 满 → 拒绝；EOS 广播且幂等；EOS 之后接入者立刻知道 |
| 13 | `media_subscriber_bind_poller_and_callbacks` | 形状 | 绑定 / 解绑语义；`setDrainCallback` 返回**上一个**；未绑定 poller 时不产生唤醒 |

### 5.2 M5-b：线程打通（分组 `ntimed_media`，6 个）

| # | 用例 | 覆盖维度 | 断言要点 |
|---|---|---|---|
| 1 | `ntimed_media_queue_spsc_no_loss` | 超大 / 并发 | 2 万帧并发搬运：**不丢、不重、保序**，`pushed == popped == 20000`（TSAN 同时验证加锁是否到位） |
| 2 | `ntimed_media_notify_coalesced_and_drained_on_poller_thread` | 满 / 正常 | 100 帧 → **恰好 1 次**唤醒；数据一条不少地在队列里；drain 确实运行在**轮询线程**上；取空后新数据能**再排一次**唤醒（不丢唤醒） |
| 3 | `ntimed_media_notify_rejected_when_poller_down` | 断开 | poller 已退出 → 唤醒被拒并计数；**下一次仍会重试**（标记被复位，不会永久卡住）；数据仍在队列里 |
| 4 | `ntimed_media_pump_reads_until_eof` | 正常 / 读完 | 端到端：源线程 → 队列 → 唤醒 → 轮询线程 drain；`eof()` 为真、`lastError()` 为空、计数准确、EOS 已广播 |
| 5 | `ntimed_media_pump_error_reported` | 失败 | 读失败 → `eof()` 为假且原因可读；EOS 仍广播；**返回 Packet 却给空包 → 当错误停下**（不许"死循环 + 什么都不做"） |
| 6 | `ntimed_media_pump_stop_is_clean` | 断开 | 停一个正在运行的源线程：`stop()` 返回 true 且线程回收、幂等；`eof()` 为假（被停 ≠ 读完）；EOS 已广播；**EOS 之后队列里剩下的数据仍可取完** |

**为什么 `ntimed_media` 也能进 TSAN 严格组**：全程只用**无超时**等待（`Semaphore::wait()`）与
`sleep_for`，不碰本环境已知的 `condition_variable` 超时误报；并发用例要的是"不丢 / 不重 / 不卡"，
不是"多快"，所以不放任何计时判据。

## 6. 风险清单

| # | 风险 | 触发条件 | 应对 |
|---|---|---|---|
| 1 | 丢帧策略把"不可丢"的包丢了 → 播放端花屏/静音 | 上限判断里把音频或关键帧算了进去 | 策略只作用于 `droppable()`；用例 3 用混合流锁死；变异验证（把音频当可丢 → 必红） |
| 2 | 新订阅者从 GOP 中间接入 → 首帧解不出 | 缓存被截断时丢了关键帧 | `GopCache` 收敛时**优先保留关键帧**；用例 8 / 11 锁死 |
| 3 | 订阅者泄漏（消费者退出后队列还在） | 源强引用订阅者 | 源只持 `weak_ptr`；用例 12 断言注销与计数 |
| 4 | 上限被绕过（`setLimits(0)` 变成无界） | 有人传 0 想"不限" | 明确拒绝 + 不生效；用例 6 |
| 5 | 单包比整个队列还大 → 该订阅者永远收不到东西 | 4K 关键帧 > 8MB | 明确 `RejectedNoSpace` / `subscribe` 失败 + 计数，不静默；用例 4 / 11 |
| 6 | **丢唤醒**（帧到了却没人取，永久滞留） | 消费者清标记与取数据的顺序写反；或唤醒被 `async` 拒绝后没复位 | 顺序固定为"先清再取"（§4.4）；拒绝时复位 + 计数；用例 2 / 3 |
| 7 | **唤醒风暴**（每帧一次跨线程投递） | 忘记合并，或每帧都新建订阅者级唤醒 | 每订阅者最多一个未决唤醒；用例 2 断言 100 帧 = 1 次 |
| 8 | 源线程泄漏 / 退出时卡死 | `stop()` 忘记 join，或 `ReadFn` 不可打断 | 析构 = `stop()`；`stop()` 必 join；中断契约写在头文件；用例 6 |
| 9 | 结束语义混淆 → 消费者永久等待 | "被停"被当成"读完"，或退出不广播 EOS | 三者都广播 EOS；`eof()` 与"被停"分开；用例 4 / 5 / 6 的变异验证（不广播 EOS → 3 个用例红） |
| 10 | 跨线程读 `Stats` 得到撕裂值 | 取值接口返回内部引用 | M5-b 起全部返回**快照**（§3.2） |

## 7. 决策记录（为什么这么选 / 排除了什么）

| 决策 | 选择 | 排除的选项与原因 |
|---|---|---|
| 类名 | **`MediaPacket`**（不是 `MediaFrame`） | 与 `ARCHITECTURE.md` §4 的措辞一致；且 v0.1/v0.2 搬运的是**编码后**的数据包，v0.3 的"解码帧"是另一类对象，不该同名 |
| 数据表示 | `shared_ptr<const vector<uint8_t>>` | 排除裸指针 + 长度（生命周期要手动管）；排除 `Buffer`（那是 network 层的可变缓冲，会引入反向依赖）；排除每订阅者一份拷贝（NFR-4 零拷贝） |
| 可丢性判断 | 放在包上（`droppable()`），音频一律不可丢 | 排除"队列按 stream_index 猜"（策略散落、易漏）；排除"音频也参与丢弃"（丢音频 = 静音空洞，比花屏更明显） |
| 队列满策略 | 丢**队头方向**第一个可丢包（最旧的优先） | 排除"丢新来的"（队头的老视频帧最没用，且会破坏"关键帧一定进队"的直觉）；排除"丢整个 GOP"（复杂度翻倍，收益不明确） |
| `push` 返回值 | 枚举**四态**（`Accepted` / `DroppedToMakeRoom` / `DroppedIncoming` / `RejectedNoSpace`） | 排除 `bool`；排除三态（"丢掉新包"与"不可丢的包进不去"语义完全不同：前者无需动作，后者必须断开） |
| 上限配置 | `setLimits` 返回 `bool`，`0` 拒绝 | 排除 `0 = 不限`（有界性不可协商）；排除构造函数里抛异常 |
| GopCache 超限 | 丢最旧可丢包、**保留关键帧** | 排除"整个 GOP 丢弃"（新订阅者长时间收不到可解码数据）；排除"超限停止缓存"（`snapshot()` 返回半个 GOP，播放端要等到下一个关键帧才恢复） |
| GopCache 里音频可丢 | 与 `FrameQueue` **有意不同** | 缓存只是启动缓冲，音频晚几毫秒无妨；但缓存必须有界。实时队列里音频不可丢（静音空洞） |
| 订阅者生命周期 | 源持 `weak_ptr`，惰性注销 | 排除"源持 `shared_ptr` + 显式 unsubscribe"（消费者异常退出就泄漏，NFR-6 不达标） |
| `subscribe()` 灌 GOP 失败 | **返回 nullptr**（订阅失败） | 排除"照常返回、让消费者从中间接"（首帧解不出 = 静默错误，最难查） |
| 【M5-b】数据通道 | **队列**（加锁 SPSC），唤醒只走 `async` | 排除"帧本体走 `async`"：真正的缓冲会变成 poller 的任务队列（65536 条），FR-5.1 的 64 帧/8MB 失真、丢帧计数长期为 0 |
| 【M5-b】队列同步 | `std::mutex` 一把锁覆盖全部状态 | 排除无锁 SPSC 环形缓冲（代码量与出错面换来的收益在当前量级看不出来：两台线程 × O(1) 操作）；排除"只保护部分成员"（拆细粒度只会增加出错面） |
| 【M5-b】唤醒策略 | **合并**（每订阅者最多一个未决唤醒） | 排除"每帧投一次"（10 客户端 × 60fps = 600 次/秒的跨线程唤醒，纯浪费）；排除"不合并也不计数"（无法观测是否刷爆） |
| 【M5-b】取数据的顺序 | 消费者**先清未决标记，再 drain** | 排除"先 drain 再清"：drain 期间新到的帧会以为有人被唤醒，导致**丢唤醒**（帧永久滞留） |
| 【M5-b】`async` 被拒 | 计数 + **复位**未决标记 | 排除"不复位"（订阅者被永久卡住 = 静默假死）；排除"重试循环"（在源线程里自旋，会把源线程拖死） |
| 【M5-b】源线程独立成类 | `SourcePump`（读回调由调用方提供） | 排除"线程塞进 `MediaSource`"（策略类会被迫依赖线程与 IO，确定性用例就没了）；排除直接写死 `Demuxer`（M5-c 才接，且测试需要假源） |
| 【M5-b】"结束"的语义 | 三种结束都广播 EOS；`eof()` 只表示**正常读完** | 排除"只有读完才广播 EOS"（被停/出错时消费者会永久等待）；排除"`eof()` 一律为真"（调用方无法区分是读完还是被掐） |
| 【M5-b】空包处理 | 当作**错误**停下来并记原因 | 排除"跳过继续读"（ReadFn 坏掉时会变成死循环 + 什么都不做的静默故障） |

## 8. 未决事项

| # | 事项 | 何时定 |
|---|---|---|
| 1 | `max_subscribers` 初值 16 是否合适 | M5-c 接入真实连接后按 `totalRejected` 观察 |
| 2 | 每订阅者 64 帧 / 8MB 是否合适（4K 流下 64 帧可能不到 1s） | M6 实测（FR-5.1 只给了上限，没给"多少秒"） |
| 3 | GOP 缓存 8MB 是否够（1080p 一个 GOP 可能 2~4MB） | M6 |
| 4 | 音频不可丢的策略在极端拥塞下是否导致队列只涨不落 | M5-d 压测（若需要，再决定"音视频成对丢弃"） |
| 5 | 订阅者的**独立时间戳基准**（`ARCHITECTURE.md` §4） | **M6**：由 `FlvSender` 按订阅者维护，M5-a/b 不提供（避免过度设计） |
| 6 | `StatsCenter` / `/api/stats` 接入源数与丢帧计数（FR-6.1） | M5-d |
| 7 | `FrameQueue` 的锁粒度是否需要升级（无锁 SPSC 环形缓冲） | M6 拿到真实吞吐后；当前无数据支持升级 |
| 8 | 唤醒合并是否够（单次 drain 总是取不完时要不要"批量投递/共享唤醒"） | M6 实测；当前 100 帧 = 1 次唤醒，余量很大 |

## 9. 假设清单与影响面（`AI_COLLAB.md` §1 的②③）

**假设**：

- 输入规模：单源 1080p 约 2~5 Mbps（MP4 样本 320x240 只是单测素材）；订阅者 ≤ 16。
- 调用方式：`pushPacket` / `notifyIfNeeded` 由**源线程**串行调用；`pop` / drain 由该订阅者所属的
  **消费者线程**串行调用；两者可以不同线程，但**同一个 `FrameQueue` 只有一个生产者与一个消费者**（SPSC）。
- 运行环境：Linux x86_64 / C++17；`media/` 不依赖 FFmpeg，故不受 FFmpeg 版本影响。
- 失败容忍：**丢帧但绝不静默**（计数 + 返回值）；关键帧腾不出空间时宁可断开订阅者也不让它花屏；
  源结束时一定广播 EOS（绝不让消费者永久等待）。

**影响面（M5-a + M5-b 累计）**：

- 新增：`src/media/{media_packet,frame_queue,gop_cache,media_source,source_pump}.h/.cpp`、
  `tests/test_media.cpp`、`tests/test_media_ntimed.cpp`、本文件。
- 改动：`CMakeLists.txt`（加入新源文件）、`tests/CMakeLists.txt`（新分组 `media` / `ntimed_media`）、
  `src/mzmedia.h`（伞头加 media 段）、`scripts/tsan.sh`（严格组加 `media` / `ntimed_media`）、
  `docs/ROADMAP.md`（M5 行补 FR 编号与批次）、`docs/TESTING.md`（补 FR-5.1/5.3/6.1 验证行）、`CHANGELOG.md`。
- **不动**：`core/`、`network/`、`http/`、`ffmpeg/` 任何现有代码。
