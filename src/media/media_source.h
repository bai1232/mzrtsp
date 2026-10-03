/*
 * MediaSource / Subscriber：一源多消费者（M5-a 建立，M5-b 接通线程，本批上限改为推导值）
 * ============================================================================
 * 形状来源：docs/DESIGN_M5.md §3.4；上位依据 ARCHITECTURE.md §3（线程模型）、§4（一源多消费者）、
 *           §5（背压）；需求：FR-5.1（**修订版**：队列上限 = 码率上限 × 延迟额度，只按字节）
 *                FR-5.2（溢出丢最旧的一段，音视频成对）、FR-5.3、NFR-6、FR-6.1
 *
 * 【上限怎么来的】不在这里拍字节数，而是从两个物理量推：
 *     queueMaxBytes() = max_bitrate_bps × latency_budget_ms / 8000   （能容忍多少秒延迟）
 *     gopMaxBytes()   = max_bitrate_bps × max_gop_ms        / 8000   （一个 GOP 有多大）
 *   原因：字节数是**结果**，不是**参数**。写死字节数会在 4K（装不下 1 秒）与低码率
 *   （浪费几十秒内存）两头都失准。
 *
 * 【线程模型】——照 ARCHITECTURE.md §3 规则 2 的原文：
 *   「源线程产出数据后，通过 `EventPoller::async()` 唤醒目标客户端所在线程投递」
 *   落到实现是**两件事分开**：
 *     · 数据通道 = 该订阅者的 `FrameQueue`（源线程 push；SPSC，内部一把锁）；
 *     · 唤醒通道 = `EventPoller::async()`，且**合并**：每订阅者最多一个未决唤醒。
 *   为什么不让"帧本体走 async"：真正的缓冲会变成 poller 的任务队列（上限 65536 条），
 *   队列上限就管不住内存了，丢帧计数也会长期为 0。详见 DESIGN_M5.md §4.4。
 *
 * 【零拷贝】一帧只 `create` 一次，`pushPacket` 把**同一个 MediaPacket** 放进 N 个订阅者的队列
 *   —— 只增加引用计数，不复制字节（用例直接断言"三个订阅者拿到的指针相同"）。
 *
 * 【订阅者生命周期】源只持有 `weak_ptr`（ARCHITECTURE.md §4「引用计数即生命周期」）。
 *   消费者把 `shared_ptr` 一放，源在下次 push/subscriberCount 时**惰性注销**并计数 ——
 *   源不需要被通知，也不会因为消费者异常退出而残留队列（NFR-6）。
 *
 * 【观测】计数器全部是 `std::atomic`，`dumpStats()` 可以**从任意线程**（例如 HTTP 线程）
 *   安全调用 —— FR-6.1 的最小版实现（完整 StatsCenter 与 `/api/stats` 接线见 M5-d）。
 * ============================================================================
 */

#pragma once

#include "media/frame_queue.h"
#include "media/gop_cache.h"
#include "media/media_packet.h"
#include "network/event_poller.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mzmedia {

/// 一个消费者（绑定到某个 poller 之后，就由那个线程来消费队列）
class Subscriber : public std::enable_shared_from_this<Subscriber> {
public:
    using Ptr = std::shared_ptr<Subscriber>;
    using Id = uint64_t;

    /// 消费者线程回调。参数是本订阅者的 `shared_ptr`（**drain 期间它不会被析构**）。
    /// 约定：回调里应当把队列取空；返回后框架不再做任何事
    using DrainCallback = std::function<void(const Ptr &)>;

    Id id() const {
        return _id;
    }

    FrameQueue &queue() {
        return _queue;
    }
    const FrameQueue &queue() const {
        return _queue;
    }

    /**
     * 是否已经"没法正确播放"了
     * @note 当**不可丢**的包（视频关键帧）腾不出空间、或接入时灌不进关键帧，置位。
     *       这是粘性标志：调用方看到它就该断开这个订阅者并 unsubscribe ——
     *       FR-5.2 的"断开"由连接层做，媒体层只负责**明确告知**，不静默继续送
     */
    bool broken() const {
        return _broken;
    }

    // ---------------------------------------------------------------------
    // M5-b：绑定的消费者线程 + 唤醒（跨线程投递的"唤醒通道"）
    // ---------------------------------------------------------------------

    /**
     * 绑定消费者所在的 poller
     * @param poller 传 nullptr = 解绑（同步模式：消费者自己取队列，不产生任何跨线程唤醒）
     * @return true = 绑定成功；false = 入参为空（已解绑）
     * @note 绑定后若队列里已有数据（例如接入时灌的 GOP），会**立刻补一次唤醒**
     */
    bool bindPoller(const EventPoller::Ptr &poller);

    EventPoller::Ptr poller() const;

    /// 注册消费者线程回调；@return **上一个**回调（沿用"注册返回上一个"的约定）
    DrainCallback setDrainCallback(DrainCallback callback);

    /**
     * 源线程调用：有新数据了，唤醒消费者线程
     * @return true = 本次真的投递了唤醒；false = 被合并 / 未绑定 poller / 投递被拒
     * @note **唤醒合并**：每个订阅者最多一个未决唤醒。数据在队列里，唤醒只是"去看一眼"，
     *       合并不会丢数据；不合并则是每帧一次跨线程投递，纯属浪费
     */
    bool notifyIfNeeded();

    /**
     * 消费者线程在 drain **之前**调用：清掉未决标记。
     * 顺序很重要 —— 先清再 drain，这样 drain 期间新进来的 push 会再排一次唤醒，不会丢唤醒。
     * @return true = 状态发生改变
     */
    bool clearNotifyPending();

    /// 唤醒真的被投递出去的次数
    uint64_t notifyCount() const {
        return _notify_count.load();
    }
    /// 唤醒投递被拒的次数（poller 任务队列满 / poller 已退出）
    uint64_t notifyRejectedCount() const {
        return _notify_rejected_count.load();
    }
    /// 因"已有未决唤醒"而被合并掉的次数（观测用）
    uint64_t notifyCoalescedCount() const {
        return _notify_coalesced_count.load();
    }

    // ---------------------------------------------------------------------
    // 【占位，M5-d / M6 实现】订阅者的**独立时间戳基准**（ARCHITECTURE.md §4）
    //   为什么必须存在：FLV 的 timestamp 要求从 0 开始递增，而各客户端接入时刻不同；
    //   如果共用源的时间戳，后接入的客户端会看到巨大的初始时间戳 → 播放器异常。
    //   现在不实现（还没有输出层可以用它），但**位置留在这里**，避免 M6 到处找地方塞：
    //     int64_t timestampBaseMs() const;          // 该订阅者的时间戳偏移
    //     void    setTimestampBaseMs(int64_t ms);
    //   预计落点：M6 的 `FlvSender` 在首次拿到包时以"第一个包的 dts"作为基准写进去。
    // ---------------------------------------------------------------------

private:
    friend class MediaSource;

    Subscriber(Id id, const FrameQueue::Limits &limits)
        : _id(id) {
        // 上限合法性由 MediaSource::setLimits 统一把关；万一非法，setLimits 保持默认值
        // （默认值是合法的推导值），不会退化成无界
        (void) _queue.setLimits(limits);
    }

    /// @return true = 状态发生改变（已经是 broken 则返回 false，避免重复计数）
    bool markBroken() {
        if (_broken) {
            return false;
        }
        _broken = true;
        return true;
    }

    Id _id;
    FrameQueue _queue;
    bool _broken = false;

    mutable std::mutex _mutex; // 保护 _poller / _drain（源线程与绑定线程可能同时碰）
    EventPoller::Ptr _poller;
    DrainCallback _drain;

    std::atomic<bool> _notify_pending{false};
    std::atomic<uint64_t> _notify_count{0};
    std::atomic<uint64_t> _notify_rejected_count{0};
    std::atomic<uint64_t> _notify_coalesced_count{0};
};

class MediaSource {
public:
    using Ptr = std::shared_ptr<MediaSource>;

    /// 上限**由物理量推导**，不再拍字节数（见文件头"上限怎么来的"）
    struct Limits {
        size_t max_bitrate_bps = 8u * 1000u * 1000u; // 单路码率上限：8 Mbps
        uint32_t latency_budget_ms = 2000;           // 延迟额度：2 s → 队列上限 2,000,000 B
        uint32_t max_gop_ms = 2000;                  // 最大 GOP 时长：2 s → GOP 缓存上限 2,000,000 B
        size_t max_subscribers = 16;                 // **人数**是硬边界（0/无界不是选项）

        /// 队列上限 = 码率上限 × 延迟额度
        size_t queueMaxBytes() const {
            return bytesFor(max_bitrate_bps, latency_budget_ms);
        }
        /// GOP 缓存上限 = 码率上限 × 最大 GOP 时长
        size_t gopMaxBytes() const {
            return bytesFor(max_bitrate_bps, max_gop_ms);
        }
        /// 该源"满订阅"时的输出带宽（**观测用**，不做准入判定 —— 见 DESIGN_M5.md §7）
        uint64_t outputBitrateBudgetBps() const {
            return static_cast<uint64_t>(max_bitrate_bps) * max_subscribers;
        }
    };

    /// 一次 push 的结果（调用方据此判断"要不要断开某人"）
    struct PushStats {
        size_t delivered = 0;            // 成功入队的订阅者数
        size_t dropped_to_make_room = 0; // 其中"丢了旧的可丢包才入队"的订阅者数
        size_t dropped_incoming = 0;     // 其中"新包被丢掉了"（新包可丢，正常降级，无需动作）
        size_t rejected_no_space = 0;    // 入队失败的订阅者数（新包不可丢 → 对应 Subscriber::broken()）
        size_t notified = 0;             // 其中真正投递了唤醒的订阅者数（被合并的不算）
        size_t subscribers = 0;          // 本次实际参与分发的订阅者数（已清理失效者）
    };

    MediaSource();
    explicit MediaSource(const Limits &limits);

    /// @return false = 上限非法（码率/时长/人数为 0 或超硬上限，或推导出的字节数超硬上限），
    ///         保持原值不变
    bool setLimits(const Limits &limits);

    Limits limits() const {
        return _limits;
    }

    /**
     * 新增订阅者，并**灌入当前 GOP 缓存**（保证第一条是视频关键帧）
     * @return nullptr = 超过 max_subscribers / 关键帧灌不进该订阅者的队列
     *         （后者说明"单包比整个队列上限还大"，此时宁可不接，也不让对端花屏）
     */
    Subscriber::Ptr subscribe();

    /// 主动移除订阅者（连接层断开时调用）；@return false = 该 id 不存在
    bool unsubscribe(Subscriber::Id id);

    /// 当前订阅者数（会先惰性清理已析构的订阅者）
    size_t subscriberCount();

    /// 广播"不会再有数据了"；@return true = 状态发生改变（重复调用返回 false）
    bool endOfStream();

    /// 源线程：推进一帧（内部会先喂 GOP 缓存，再分发给所有订阅者，并唤醒它们的消费者线程）
    PushStats pushPacket(MediaPacket::Ptr packet);

    const GopCache &gopCache() const {
        return _gop;
    }

    /// 一行统计（FR-6.1 的最小版）；**可从任意线程调用**（计数器都是 atomic）
    std::string dumpStats() const;

    // ---- 观测（FR-6.1 的素材；绝不用计数替代上限）----
    uint64_t totalDelivered() const {
        return _total_delivered.load();
    }
    uint64_t totalDropped() const {
        return _total_dropped_to_make_room.load();
    }
    uint64_t totalDroppedIncoming() const {
        return _total_dropped_incoming.load();
    }
    uint64_t totalRejected() const {
        return _total_rejected.load();
    }
    uint64_t totalSubscribe() const {
        return _total_subscribe.load();
    }
    uint64_t totalSeedFailed() const {
        return _total_seed_failed.load();
    }
    uint64_t totalAutoUnsubscribe() const {
        return _total_auto_unsubscribe.load();
    }
    /// 因"不可丢的包进不去"被置为 broken 的订阅者数（累计）
    uint64_t totalBroken() const {
        return _total_broken.load();
    }

private:
    static constexpr size_t kHardMaxSubscribers = 4096;
    static constexpr size_t kHardMaxBitrateBps = 100u * 1000u * 1000u; // 100 Mbps
    static constexpr uint32_t kHardMaxLatencyMs = 10u * 1000u;         // 10 s
    static constexpr uint32_t kHardMaxGopMs = 60u * 1000u;             // 60 s

    /// 把"码率 × 时长"折成字节数（**推导上限的唯一入口**，避免各处拍数字）
    static size_t bytesFor(size_t bitrate_bps, uint32_t ms) {
        return static_cast<size_t>(static_cast<uint64_t>(bitrate_bps) * ms / 8000u);
    }

    /// 清掉 shared_ptr 已释放的订阅者；@return 本次清理掉的个数
    size_t pruneExpired();

    struct Entry {
        std::weak_ptr<Subscriber> sub;
        Subscriber::Id id = 0;
    };

    Limits _limits;
    GopCache _gop;
    std::vector<Entry> _entries;
    Subscriber::Id _next_id = 1;
    bool _ended = false;

    std::atomic<uint64_t> _total_delivered{0};
    std::atomic<uint64_t> _total_dropped_to_make_room{0};
    std::atomic<uint64_t> _total_dropped_incoming{0};
    std::atomic<uint64_t> _total_rejected{0};
    std::atomic<uint64_t> _total_subscribe{0};
    std::atomic<uint64_t> _total_seed_failed{0};
    std::atomic<uint64_t> _total_auto_unsubscribe{0};
    std::atomic<uint64_t> _total_broken{0};
};

} // namespace mzmedia
