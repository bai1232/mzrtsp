/*
 * MediaSource / Subscriber：一源多消费者（M5-a）
 * ============================================================================
 * 形状来源：docs/DESIGN_M5.md §3.4；上位依据 ARCHITECTURE.md §4（一源多消费者）、§5（背压）
 *          需求：FR-5.1（每客户端独立队列 + 上限）、FR-5.2（丢非关键帧 + 计数）、
 *                FR-5.3（隔离性）、NFR-6（断开后 100% 回收）
 *
 * 本批是**单线程契约**：不加锁、不可重入。`pushPacket` 由源线程调用；每个订阅者的
 * `queue().pop()` 由该订阅者所属线程调用。跨线程投递（EventPoller::async）留 M5-b ——
 * 把"策略"和"线程"分开，策略才能用确定性用例覆盖（DESIGN_M5.md §4.3）。
 *
 * 零拷贝：一帧只 `create` 一次，`pushPacket` 把**同一个 MediaPacket** 放进 N 个订阅者的
 * 队列 —— 只增加引用计数，不复制字节。用例直接断言"三个订阅者拿到的指针相同"。
 *
 * 订阅者生命周期：源只持有 `weak_ptr`（ARCHITECTURE.md §4「引用计数即生命周期」）。
 * 消费者把 `shared_ptr` 一放，源在下次 push/subscriberCount 时**惰性注销**并计数 ——
 * 源不需要被通知，也不会因为消费者异常退出而残留队列（NFR-6）。
 * ============================================================================
 */

#pragma once

#include "media/frame_queue.h"
#include "media/gop_cache.h"
#include "media/media_packet.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace mzmedia {

/// 一个消费者（在 M5-b 里会绑定到某个 HTTP 连接/poller）
class Subscriber {
public:
    using Ptr = std::shared_ptr<Subscriber>;
    using Id = uint64_t;

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
     * @note 当**不可丢**的包（关键帧/音频）腾不出空间、或接入时灌不进关键帧，置位。
     *       这是粘性标志：调用方（M5-b）看到它就该断开这个订阅者并 unsubscribe ——
     *       FR-5.2 的"断开"由连接层做，媒体层只负责**明确告知**，不静默继续送
     */
    bool broken() const {
        return _broken;
    }

private:
    friend class MediaSource;

    Subscriber(Id id, const FrameQueue::Limits &limits)
        : _id(id) {
        // 上限合法性由 MediaSource::setLimits 统一把关；万一非法，setLimits 保持默认值
        // （默认值是合法的 64 帧/8MB），不会退化成无界
        (void) _queue.setLimits(limits);
    }

    void markBroken() {
        _broken = true;
    }

    Id _id;
    FrameQueue _queue;
    bool _broken = false;
};

class MediaSource {
public:
    struct Limits {
        FrameQueue::Limits queue;                        // 每个订阅者一份
        size_t max_gop_bytes = 8u * 1024u * 1024u;       // GopCache 上限
        size_t max_subscribers = 16;                     // 有界：0/无界不是选项
    };

    /// 一次 push 的结果（调用方据此判断"要不要断开某人"）
    struct PushStats {
        size_t delivered = 0;            // 成功入队的订阅者数
        size_t dropped_to_make_room = 0; // 其中"丢了旧的可丢包才入队"的订阅者数
        size_t dropped_incoming = 0;     // 其中"新包被丢掉了"（新包可丢，正常降级，无需动作）
        size_t rejected_no_space = 0;    // 入队失败的订阅者数（新包不可丢 → 对应 Subscriber::broken()）
        size_t subscribers = 0;          // 本次实际参与分发的订阅者数（已清理失效者）
    };

    MediaSource();
    explicit MediaSource(const Limits &limits);

    /// @return false = 上限非法（队列上限见 FrameQueue::setLimits、max_subscribers 为 0
    ///         或超过硬上限、max_gop_bytes 同 GopCache::setMaxBytes），保持原值不变
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

    /// 源线程：推进一帧（内部会先喂 GOP 缓存，再分发给所有订阅者）
    PushStats pushPacket(MediaPacket::Ptr packet);

    const GopCache &gopCache() const {
        return _gop;
    }

    // ---- 观测（FR-6.1 的素材；绝不用计数替代上限）----
    uint64_t totalDelivered() const {
        return _total_delivered;
    }
    uint64_t totalDropped() const {
        return _total_dropped_to_make_room;
    }
    uint64_t totalDroppedIncoming() const {
        return _total_dropped_incoming;
    }
    uint64_t totalRejected() const {
        return _total_rejected;
    }
    uint64_t totalSubscribe() const {
        return _total_subscribe;
    }
    uint64_t totalSeedFailed() const {
        return _total_seed_failed;
    }
    uint64_t totalAutoUnsubscribe() const {
        return _total_auto_unsubscribe;
    }

private:
    static constexpr size_t kHardMaxSubscribers = 4096;

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

    uint64_t _total_delivered = 0;
    uint64_t _total_dropped_to_make_room = 0;
    uint64_t _total_dropped_incoming = 0;
    uint64_t _total_rejected = 0;
    uint64_t _total_subscribe = 0;
    uint64_t _total_seed_failed = 0;
    uint64_t _total_auto_unsubscribe = 0;
};

} // namespace mzmedia
