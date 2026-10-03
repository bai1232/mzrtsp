/*
 * MediaSource / Subscriber 实现（M5-a 建立，M5-b 接通线程）
 * ============================================================================
 * 分发循环本身很短，值得看的是四个"必须明确"的地方：
 *   1) 订阅者用 weak_ptr 记录 → 消费者一释放就惰性注销（NFR-6，不需要通知机制）；
 *   2) 接入时灌 GOP 缓存，且**关键帧灌不进去就拒绝接入**（宁可拒接也不让对端花屏）；
 *   3) push 的失败路径只标记 broken()，**不在这里断连接** —— 断连接是连接层的事，
 *      媒体层只负责明确告知；
 *   4) 【M5-b】入队成功后只发一个**合并过的唤醒**（async），数据本身留在队列里。
 * ============================================================================
 */

#include "media/media_source.h"

#include "core/logger.h"

#include <utility>

namespace mzmedia {

// ---------------------------------------------------------------------------
// Subscriber：绑定消费者线程 + 唤醒
// ---------------------------------------------------------------------------

bool Subscriber::bindPoller(const EventPoller::Ptr &poller) {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _poller = poller;
    }
    if (!poller) {
        return false; // 解绑 = 同步模式：消费者自己取，不产生跨线程唤醒
    }
    // 绑定瞬间队列里可能已经有数据（接入时灌的 GOP）→ 补一次唤醒，
    // 否则这些数据要一直等到下一帧才会被发现
    if (!_queue.empty()) {
        (void) notifyIfNeeded();
    }
    return true;
}

EventPoller::Ptr Subscriber::poller() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _poller;
}

Subscriber::DrainCallback Subscriber::setDrainCallback(DrainCallback callback) {
    std::lock_guard<std::mutex> lock(_mutex);
    DrainCallback previous = std::move(_drain);
    _drain = std::move(callback);
    return previous;
}

bool Subscriber::clearNotifyPending() {
    return _notify_pending.exchange(false);
}

bool Subscriber::notifyIfNeeded() {
    if (_notify_pending.exchange(true)) {
        ++_notify_coalesced_count; // 已有未决唤醒 → 合并（数据在队列里，不会丢）
        return false;
    }

    EventPoller::Ptr poller;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        poller = _poller;
    }
    if (!poller) {
        _notify_pending.store(false); // 未绑定 poller = 同步模式，不需要唤醒
        return false;
    }

    // 任务里持住 shared_ptr：drain 期间订阅者不会被析构（否则回调里会踩空）
    Subscriber::Ptr self = shared_from_this();
    const bool accepted = poller->async([self] {
        // 顺序：**先清标记再 drain**。这样 drain 期间新 push 会再排一次唤醒，不会丢唤醒
        self->clearNotifyPending();
        DrainCallback callback;
        {
            std::lock_guard<std::mutex> lock(self->_mutex);
            callback = self->_drain;
        }
        if (callback) {
            callback(self);
        }
    });

    if (!accepted) {
        // poller 已退出 / 任务队列满：计数并**复位标记**，否则这个订阅者会被永久卡住
        ++_notify_rejected_count;
        _notify_pending.store(false);
        return false;
    }
    ++_notify_count;
    return true;
}

// ---------------------------------------------------------------------------
// MediaSource
// ---------------------------------------------------------------------------

MediaSource::MediaSource()
    : MediaSource(Limits{}) {}

MediaSource::MediaSource(const Limits &limits)
    : _gop(limits.max_gop_bytes) {
    // 先站稳默认值（有界），再由 setLimits 校验入参；非法项保持默认并记日志
    _limits = Limits{};
    (void) setLimits(limits);
}

bool MediaSource::setLimits(const Limits &limits) {
    // 复用各组件自己的校验（FrameQueue / GopCache），避免"同一套规则写两遍"然后漂移
    FrameQueue queue_probe;
    if (!queue_probe.setLimits(limits.queue)) {
        return false;
    }
    GopCache gop_probe(limits.max_gop_bytes);
    if (gop_probe.maxBytes() != limits.max_gop_bytes) {
        WarnL << "MediaSource::setLimits 被拒：max_gop_bytes 非法（" << limits.max_gop_bytes << "）";
        return false;
    }
    if (limits.max_subscribers == 0) {
        WarnL << "MediaSource::setLimits 被拒：max_subscribers 不能为 0（0 不等于无界）";
        return false;
    }
    if (limits.max_subscribers > kHardMaxSubscribers) {
        WarnL << "MediaSource::setLimits 被拒：max_subscribers 超过硬上限 " << kHardMaxSubscribers;
        return false;
    }

    _limits = limits;
    (void) _gop.setMaxBytes(limits.max_gop_bytes);
    return true;
}

size_t MediaSource::pruneExpired() {
    size_t removed = 0;
    for (auto it = _entries.begin(); it != _entries.end();) {
        if (it->sub.expired()) {
            it = _entries.erase(it);
            ++removed;
            ++_total_auto_unsubscribe;
        } else {
            ++it;
        }
    }
    return removed;
}

Subscriber::Ptr MediaSource::subscribe() {
    pruneExpired();
    if (_entries.size() >= _limits.max_subscribers) {
        ++_total_rejected;
        WarnL << "MediaSource::subscribe 被拒：订阅者已达上限 " << _limits.max_subscribers;
        return nullptr;
    }

    Subscriber::Ptr sub(new Subscriber(_next_id++, _limits.queue));

    // 灌 GOP 缓存：保证消费者拿到的第一条是视频关键帧（否则首帧解不出 = 静默错误）
    const std::vector<MediaPacket::Ptr> seed = _gop.snapshot();
    if (!seed.empty()) {
        const FrameQueue::PushResult first = sub->queue().push(seed.front());
        if (first == FrameQueue::PushResult::RejectedNoSpace) {
            ++_total_seed_failed;
            ++_total_rejected;
            WarnL << "MediaSource::subscribe 失败：关键帧 " << seed.front()->size()
                  << " 字节灌不进队列（上限 " << _limits.queue.max_bytes << "）→ 宁可不接";
            return nullptr;
        }
        for (size_t i = 1; i < seed.size(); ++i) {
            // 后续包装不下就按队列自己的策略丢（丢了多少在 queue().stats() 里可见）
            (void) sub->queue().push(seed[i]);
        }
    }

    if (_ended) {
        // 源已经结束：新订阅者必须**立刻**知道，否则消费端会永远等一个不会来的包
        (void) sub->queue().markEndOfStream();
    }

    _entries.push_back(Entry{sub, sub->id()});
    ++_total_subscribe;
    return sub;
}

bool MediaSource::unsubscribe(Subscriber::Id id) {
    for (auto it = _entries.begin(); it != _entries.end(); ++it) {
        if (it->id == id) {
            _entries.erase(it);
            return true;
        }
    }
    return false;
}

size_t MediaSource::subscriberCount() {
    pruneExpired();
    return _entries.size();
}

bool MediaSource::endOfStream() {
    pruneExpired();
    if (_ended) {
        return false; // 幂等
    }
    _ended = true;
    for (const auto &entry : _entries) {
        if (Subscriber::Ptr sub = entry.sub.lock()) {
            (void) sub->queue().markEndOfStream();
            // 队列里可能还有没消费完的数据（也可能消费者正等着）→ 唤醒它去看一眼 EOS
            (void) sub->notifyIfNeeded();
        }
    }
    return true;
}

MediaSource::PushStats MediaSource::pushPacket(MediaPacket::Ptr packet) {
    PushStats stats;
    pruneExpired();
    stats.subscribers = _entries.size();

    if (!packet) {
        ++_total_rejected;
        WarnL << "MediaSource::pushPacket 收到空包（调用方 bug）";
        return stats;
    }

    // 先喂 GOP 缓存：新订阅者接入时靠它从关键帧开始
    (void) _gop.feed(packet);

    for (const auto &entry : _entries) {
        Subscriber::Ptr sub = entry.sub.lock();
        if (!sub) {
            continue; // 极端时序：prune 之后刚好被释放 → 跳过，下次 push 再清
        }
        const FrameQueue::PushResult result = sub->queue().push(packet);
        switch (result) {
        case FrameQueue::PushResult::Accepted:
            ++stats.delivered;
            ++_total_delivered;
            break;
        case FrameQueue::PushResult::DroppedToMakeRoom:
            ++stats.delivered;
            ++_total_delivered;
            ++stats.dropped_to_make_room;
            ++_total_dropped_to_make_room;
            break;
        case FrameQueue::PushResult::DroppedIncoming:
            ++stats.dropped_incoming;
            ++_total_dropped_incoming;
            break;
        case FrameQueue::PushResult::RejectedNoSpace:
            ++stats.rejected_no_space;
            ++_total_rejected;
            if (sub->markBroken()) {
                ++_total_broken;
                WarnL << "订阅者 id=" << sub->id()
                      << " 已置为 broken（不可丢的包进不去）→ 调用方应断开它";
            }
            break;
        }

        // 队列里真的多了东西才需要唤醒（丢新包/被拒的情况下没有新数据）
        if (result == FrameQueue::PushResult::Accepted ||
            result == FrameQueue::PushResult::DroppedToMakeRoom) {
            if (sub->notifyIfNeeded()) {
                ++stats.notified;
            }
        }
    }
    return stats;
}

} // namespace mzmedia
