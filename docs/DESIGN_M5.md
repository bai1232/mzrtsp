# M5 设计：媒体分发层（Media）

> 上游需求：`docs/SPEC.md` §5.5（FR-5.1 / FR-5.2 / FR-5.3）、`NFR-6`；`docs/ARCHITECTURE.md` §4（一源多消费者）/ §5（背压与慢客户端）
> 上游实现：`docs/DESIGN_M2.md`（`Session` 发送队列与背压）、`docs/DESIGN_M4.md`（`Demuxer` 产包）
> 本文件对应 ROADMAP 的 **M5 Media** 一行，拆成 M5-a / M5-b / M5-c 三批。

## 1. 范围与验收

| 批次 | 内容 | 状态 |
|---|---|---|
| **M5-a**（本批） | `MediaPacket`、`FrameQueue`、`GopCache`、`Subscriber`、`MediaSource`（**单线程契约**，帧由外部喂入） | 本文件 |
| M5-b | 源线程真正驱动：`SourceManager`（懒启动 / 空闲释放）、从 `Demuxer` 取包喂进 `MediaSource`、跨线程投递（`EventPoller::async`） | 后续 |
| M5-c | 节流（FR-3.5）、30s 写阻塞断开的端到端串通（可与 M6 合并）、`/api/stats` 暴露丢帧计数 | 后续 |

**M5-a 的验收**（对应 `docs/ROADMAP.md:15`）：

- 3 个订阅者收到**完全一致**的帧序列；
- 慢订阅者丢帧、但**关键帧不丢**，且不影响其他订阅者；
- 每个订阅者队列上限 **64 帧或 8MB**（先到者为准，FR-5.1）；
- 订阅者断开后队列 100% 回收（NFR-6）。

## 2. 分层位置与依赖方向

```
Output(未做) ← Media ← FFmpeg(未用) / Core(用)
```

- `media/` **只依赖 `core/`**（本批只用 `util` 的日志与 `Logger`）。
- **本批刻意不依赖 FFmpeg**：帧由外部喂给 `MediaSource`，所以"数据从哪来"与"分发策略"解耦。
  好处是策略（上限 / 丢帧 / GOP）可以用**确定性用例**覆盖，不需要样本文件、不需要线程、不依赖时序。
  这与 SPEC 成功标准 SC-2（加格式不改核心）同一个思路：**边界靠接口，不靠实现耦合**。
- 反向依赖检查（应为空）：`grep -rn "network/\|http/\|ffmpeg/" src/media/`

## 3. 契约（M5-a）

**线程契约（最重要的一条）**：本批所有公开接口**不加锁、不可重入、只在单一调用线程使用**
（哪个线程由调用方决定：源线程调 `pushPacket`，消费者线程调 `pop`）。
跨线程投递（`EventPoller::async`）留 M5-b；M5-a 不引入任何线程，因此可以进 TSAN 严格组。
写进每个头文件，避免"看着像线程安全"的误用。

### 3.1 `MediaPacket`（`media_packet.h`）

不可变的编码数据包 —— 一帧数据被 N 个订阅者共享的载体。

```cpp
enum class MediaKind : uint8_t { Video, Audio };

class MediaPacket {
public:
    using Ptr     = std::shared_ptr<MediaPacket>;
    using Payload = std::shared_ptr<const std::vector<uint8_t>>;

    // 工厂：payload 为空 / 尺寸为 0 → 返回 nullptr（不抛、不静默造一个空包）
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

**为什么 `droppable()` 放在包上而不是队列里**：策略要判断的是"这个包丢了会不会让后面全花屏"，
这是包自身的性质。音频没有关键帧概念，丢了就是**静音空洞**，所以音频一律不可丢
（FR-5.2 说的"丢弃非关键帧"针对的是视频）。

### 3.2 `FrameQueue`（`frame_queue.h`）

每个订阅者一个的**有界**队列（FR-5.1）。

```cpp
class FrameQueue {
public:
    struct Limits { size_t max_packets = 64; size_t max_bytes = 8u * 1024 * 1024; };
    struct Stats  { uint64_t pushed, popped, dropped_non_key, rejected_no_space, dropped_on_clear; };

    enum class PushResult : uint8_t { Accepted, DroppedToMakeRoom, RejectedNoSpace };
    enum class PopResult  : uint8_t { Packet, Empty, EndOfStream };

    FrameQueue();                                   // 默认 Limits
    bool   setLimits(const Limits &limits);         // false = 非法（0 / 超硬上限），不生效
    Limits limits() const;

    PushResult push(MediaPacket::Ptr packet);
    PopResult  pop(MediaPacket::Ptr *packet);       // 出参；返回码区分"空"与"流结束"
    bool       markEndOfStream();                   // true = 状态改变（重复调用返回 false）
    size_t     clear();                             // 返回被丢弃的包数（NFR-6 回收用）

    size_t packets() const;  size_t bytes() const;  bool empty() const;  bool endOfStream() const;
    const Stats &stats() const;
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
    const Stats &stats() const;   // started_gop / cached / dropped（超上限收敛时）
};
```

- 关键帧到达 ⇒ **丢弃整个旧 GOP** 重新开始（只留最近一个）。
- 单个 GOP 超过 `max_bytes` 时**收敛**：丢最旧的**可丢**包，**关键帧必须保留**
  （否则"从关键帧接入"这条保证就没了）—— 这是有意的取舍，写进 §7 决策记录。

### 3.4 `MediaSource` / `Subscriber`（`media_source.h`）

```cpp
class Subscriber {
public:
    using Ptr = std::shared_ptr<Subscriber>;
    using Id  = uint64_t;
    Id id() const;  FrameQueue &queue();  const FrameQueue &queue() const;
};

class MediaSource {
public:
    struct Limits {
        FrameQueue::Limits queue;                    // 每订阅者一份
        size_t max_gop_bytes   = 8u * 1024 * 1024;   // GopCache 上限
        size_t max_subscribers = 16;                 // 有界（§4.3 AI_COLLAB：0/无界不是选项）
    };
    struct PushStats { size_t delivered, dropped_to_make_room, rejected_no_space, subscribers; };

    MediaSource();
    bool   setLimits(const Limits &limits);      // false = 非法，不生效
    Limits limits() const;

    Subscriber::Ptr subscribe();                 // nullptr = 超过 max_subscribers / 灌不进关键帧
    size_t subscriberCount();                    // 先清理已析构的订阅者（weak_ptr 自动注销）
    bool   endOfStream();                        // 广播流结束；true = 状态改变

    PushStats pushPacket(MediaPacket::Ptr packet);
    const GopCache &gopCache() const;

    // 观测（FR-6.1 的素材）
    uint64_t totalDelivered() const;  uint64_t totalDropped() const;
    uint64_t totalRejected() const;   uint64_t totalSubscribe() const;
    uint64_t totalSeedFailed() const; uint64_t totalAutoUnsubscribe() const;
};
```

- **订阅者生命周期**：源持有 `std::weak_ptr<Subscriber>`（`ARCHITECTURE.md` §4「引用计数即生命周期」）。
  消费者把 `shared_ptr` 一放，源在下次 `pushPacket` / `subscriberCount` 时**惰性注销**并计数 ——
  源不需要"被通知"，也不会因消费者异常退出而残留队列（NFR-6）。
- **`subscribe()` 会灌入 GOP 缓存**：按 `snapshot()` 顺序入队，保证第一条是视频关键帧。
  若关键帧本身超过队列 `max_bytes`（单包比整个队列还大）→ **订阅失败**（返回 nullptr +
  `totalSeedFailed()`），而不是让消费者拿到"从中间开始"的流。
- **`endOfStream()`**：给每个订阅者队列打 EOS 标记，消费者 `pop()` 会拿到 `EndOfStream`
  （区分"暂时没数据"与"不会再有数据"—— 与 `DEMUXER` 的 EOF/Error 分离同一原则）。

## 4. 关键机制

### 4.1 零拷贝分发

`MediaPacket::Payload` 是 `shared_ptr<const std::vector<uint8_t>>`，`MediaSource::pushPacket`
把**同一个 `Ptr`** 放进 N 个订阅者的队列 —— 只增加引用计数，不复制字节。
用例 `media_source_fanout_identical` 直接用**指针相等**来断言"三个订阅者拿到的是同一帧"。

### 4.2 上限与溢出策略（FR-5.1 / FR-5.2）

- 两个上限**先到者为准**：`packets() >= max_packets` 或 `bytes() >= max_bytes` 都算满。
- 满了以后来新包：只丢 `droppable()` 的旧包（从队头开始）腾空间。
- 腾不出空间：返回 `RejectedNoSpace`，**不静默丢**（§4.5 AI_COLLAB：静默丢弃属于失败）。
- 所有丢弃都进 `stats()` 且能被 `MediaSource` 汇总 —— 可观测是辅助，上限才是防线。

### 4.3 为什么本批不引入线程

策略（丢谁 / 上限怎么算 / GOP 怎么截断）与"谁在什么线程调用"是**正交**的两件事。
混在一起写，"慢消费者"这种场景就只能靠 sleep 和时序去凑，用例会变成 flaky。
所以 M5-a 把策略做成**纯函数式单线程组件**（可进 TSAN 严格组），把线程与投递留给 M5-b。
代价：`MediaSource` 目前不能直接给 HTTP 用 —— 这是有意的，M5-b 补。

## 5. 测试计划（M5-a：12 个用例，分组 `media`）

| # | 用例 | 覆盖维度 | 断言要点 |
|---|---|---|---|
| 1 | `media_packet_metadata_and_validation` | 正常 / 空 | 元数据可读；`droppable()` 三分支；空 payload / 0 字节 → `create` 返回 nullptr |
| 2 | `media_queue_fifo_and_accounting` | 正常 | FIFO 顺序；`packets()/bytes()` 与 push/pop 一致；`Empty` 与 `Packet` 区分 |
| 3 | `media_queue_drops_only_droppable` | 满 | 帧数上限触发时：**音频包与视频关键帧不丢**，只丢视频非关键帧 |
| 4 | `media_queue_limits_first_wins` | 满 / 超大 | 帧数未满但字节到顶 → 同样按上限处理；单包超过 `max_bytes`：可丢 → `DroppedIncoming`，不可丢 → `RejectedNoSpace` |
| 5 | `media_queue_keyframe_makes_room_or_rejects` | 满 | 新关键帧到来：丢掉可丢包让位（`DroppedToMakeRoom`）；队列里已无可丢包（关键帧 + 音频）→ `RejectedNoSpace` |
| 6 | `media_queue_rejects_invalid_limits_and_eos` | 空 / 断开 | `setLimits(0)` → false 且不生效；`markEndOfStream()` 幂等；`pop` 返回 `EndOfStream`；`clear()` 返回丢弃数 |
| 7 | `media_gop_cache_keeps_only_last_gop` | 正常 | 第二个关键帧后旧 GOP 全部消失；`snapshot()` 第一项是关键帧 |
| 8 | `media_gop_cache_byte_limit_keeps_keyframe` | 超大 | GOP 超上限 → 丢最旧**可丢**包，**关键帧保留**；`snapshot()` 仍以关键帧开头 |
| 9 | `media_source_fanout_identical` | 正常 | **ROADMAP 验收**：3 个订阅者帧序列完全一致，且每帧**指针相同**（零拷贝） |
| 10 | `media_source_slow_consumer_isolated` | 满 / 隔离 | 慢订阅者（不 pop）只丢自己的包，其他订阅者序列完整（FR-5.3） |
| 11 | `media_source_new_subscriber_starts_at_keyframe` | 中途接入 / 超大 | 新订阅者第一条必是视频关键帧；关键帧超队列上限 → `subscribe()` 返回 nullptr |
| 12 | `media_source_subscriber_lifetime_and_limits` | 断开 / 回收 | `shared_ptr` 释放后自动注销（NFR-6）；`max_subscribers` 满 → 拒绝；`endOfStream()` 广播且幂等 |

## 6. 风险清单

| # | 风险 | 触发条件 | 应对 |
|---|---|---|---|
| 1 | 丢帧策略把"不可丢"的包丢了 → 播放端花屏/静音 | 上限判断里把音频或关键帧算了进去 | 策略只作用于 `droppable()`；用例 3 用混合流锁死；变异验证（把音频当可丢 → 必红） |
| 2 | 新订阅者从 GOP 中间接入 → 首帧解不出 | 缓存被截断时丢了关键帧 | `GopCache` 收敛时**优先保留关键帧**；用例 8 / 11 锁死 |
| 3 | 订阅者泄漏（消费者退出后队列还在） | 源强引用订阅者 | 源只持 `weak_ptr`；用例 12 断言注销与计数 |
| 4 | 上限被绕过（`setLimits(0)` 变成无界） | 有人传 0 想"不限" | 明确拒绝 + 不生效（§4.6）；用例 6 |
| 5 | 单包比整个队列还大 → 该订阅者永远收不到东西 | 4K 关键帧 > 8MB（M4 未决 2 也在盯这个） | 明确 `RejectedNoSpace` / `subscribe` 失败 + 计数，不静默；用例 4 / 11 |
| 6 | 把单线程契约误当线程安全 | M5-b 接线程时忘了投递 | 头文件写死契约；M5-b 用 `EventPoller::async` 投递，届时补并发用例 |

## 7. 决策记录（为什么这么选 / 排除了什么）

| 决策 | 选择 | 排除的选项与原因 |
|---|---|---|
| 类名 | **`MediaPacket`**（不是 `MediaFrame`） | 与 `ARCHITECTURE.md` §4 的措辞一致；且 v0.1/v0.2 搬运的是**编码后**的数据包，v0.3 的"解码帧"是另一类对象，不该同名（`DESIGN_M3.md` §8 有同名先例：ROADMAP 措辞以设计文件为准） |
| 数据表示 | `shared_ptr<const vector<uint8_t>>` | 排除裸指针 + 长度（生命周期要手动管）；排除 `Buffer`（那是 network 层的可变缓冲，会引入反向依赖）；排除每订阅者一份拷贝（NFR-4 零拷贝） |
| 可丢性判断 | 放在包上（`droppable()`），音频一律不可丢 | 排除"队列按 stream_index 猜"（策略散落、易漏）；排除"音频也参与丢弃"（丢音频 = 静音空洞，比花屏更明显） |
| 队列满策略 | 丢**队头**可丢包（最旧的优先） | 排除"丢新来的"（队头的老视频帧最没用，且丢新包会破坏"关键帧一定会进队"的直觉）；排除"丢整个 GOP"（实现复杂度翻倍，收益不明确） |
| `push` 返回值 | 枚举三态（`Accepted/DroppedToMakeRoom/RejectedNoSpace`） | 排除 `bool`（"丢着旧包成功入队"与"彻底失败"是两种语义，调用方动作不同：前者继续、后者断开） |
| 上限配置 | `setLimits` 返回 `bool`，`0` 拒绝 | 排除 `0 = 不限`（有界性不可协商，`AI_COLLAB.md` §4.6）；排除构造函数里抛异常 |
| GopCache 超限 | 丢最旧可丢包、**保留关键帧** | 排除"整个 GOP 丢弃"（新订阅者会长时间收不到可解码数据）；排除"超限就停止缓存"（`snapshot()` 会返回半个 GOP 且没有尾巴，播放端等到下一个关键帧才恢复） |
| 订阅者生命周期 | 源持 `weak_ptr`，惰性注销 | 排除"源持 `shared_ptr` + 显式 unsubscribe"（消费者异常退出就泄漏，NFR-6 不达标） |
| `subscribe()` 灌 GOP 失败 | **返回 nullptr**（订阅失败） | 排除"照常返回、让消费者从中间接"（首帧解不出 = 静默错误，最难查） |
| 本批不引入线程 | 单线程纯策略，跨线程留 M5-b | 排除"一次做完源线程 + 投递"（用例会依赖时序，且 TSAN 严格组会失去确定性） |

## 8. 未决事项

| # | 事项 | 何时定 |
|---|---|---|
| 1 | `max_subscribers` 初值 16 是否合适 | M5-b 接入真实连接后按 `totalRejected` 观察 |
| 2 | 每订阅者 64 帧 / 8MB 是否合适（4K 流下 64 帧可能不到 1s） | M6 实测（FR-5.1 只给了上限，没给"多少秒"） |
| 3 | GOP 缓存 8MB 是否够（1080p 一个 GOP 可能 2~4MB） | M6 |
| 4 | 音频不可丢的策略在极端拥塞下是否导致队列只涨不落 | M5-c 压测（若需要，再决定"音视频成对丢弃"） |
| 5 | 订阅者的**独立时间戳基准**（`ARCHITECTURE.md` §4） | **M6**：由 `FlvSender` 按订阅者维护，M5-a 不提供（避免过度设计） |
| 6 | `StatsCenter` / `/api/stats` 接入丢帧计数（FR-6.1） | M6 |

## 9. 假设清单与影响面（`AI_COLLAB.md` §1 的②③）

**假设**：

- 输入规模：单源 1080p 约 2~5 Mbps（MP4 样本 320x240 只是单测素材）；订阅者 ≤ 16。
- 调用方式：`pushPacket` 由源线程串行调用；每个订阅者的 `pop` 由该订阅者所属线程串行调用；
  **两者可以不是同一线程**，但同一对象不会被两个线程同时访问（M5-b 用投递保证）。
- 运行环境：Linux x86_64 / C++17；`media/` 不依赖 FFmpeg，故不受 FFmpeg 版本影响。
- 失败容忍：**丢帧但绝不静默**（计数 + 返回值）；关键帧腾不出空间时宁可断开订阅者也不让它花屏。

**影响面**：

- 新增：`src/media/{media_packet,frame_queue,gop_cache,media_source}.h/.cpp`、`tests/test_media.cpp`、本文件。
- 改动：`CMakeLists.txt`（加入新源文件）、`tests/CMakeLists.txt`（新分组 `media`）、`src/mzmedia.h`（伞头加 media 段）、
  `docs/ROADMAP.md`（M5 行补 FR 编号引用）、`docs/TESTING.md`（补 FR-5.1/5.3 验证行）、`CHANGELOG.md`。
- **不动**：`core/`、`network/`、`http/`、`ffmpeg/` 任何现有代码（本批是纯新增 + 文档）。
