/*
 * MediaSource / Subscriber 实现（M5-a）
 * ============================================================================
 * 分发循环本身很短，值得看的是三个"必须明确"的地方：
 *   1) 订阅者用 weak_ptr 记录 → 消费者一释放就惰性注销（NFR-6，不需要通知机制）；
 *   2) 接入时灌 GOP 缓存，且**关键帧灌不进去就拒绝接入**（宁可拒接也不让对端花屏）；
 *   3) push 的失败路径只标记 broken()，**不在这里断连接** —— 断连接是连接层的事
 *      （M5-b），媒体层只负责明确告知。
 * ============================================================================
 */

#include "media/media_source.h"

#include "core/logger.h"

#include <utility>

namespace mzmedia {

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
        switch (sub->queue().push(packet)) {
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
            sub->markBroken(); // 不可丢的包都进不去 = 这个订阅者已经无法正确播放
            break;
        }
    }
    return stats;
}

} // namespace mzmedia
