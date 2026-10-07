/*
 * MediaSource / Subscriber 实现（M5-a → M5-c）
 * ============================================================================
 * 【M5-c 修正】订阅管理改为**线程安全**（TSAN 抓出来的真问题）：
 *   M5-b 之前，`MediaSource` 的所有接口都假定"只在源线程调用"。M5-c 接上真实的源线程之后，
 *   调用方（HTTP 线程 / 测试线程）自然会在源线程推流的同时 `subscribe()` ——
 *   实测 TSAN 报了 13 处 data race，全部指向 `subscribe()` 里读 `GopCache` 的那一行。
 *   现在：订阅集合 / `_gop` / `_ended` / `_next_id` 全由 `_mutex` 保护。
 *
 * 【两条纪律】写这段代码时最容易踩的两个坑：
 *   1) **唤醒必须在锁外**：`EventPoller::async` 在"调用者本身就是轮询线程"时会内联执行，
 *      内联的 drain 回调可能再次进入本类（例如 subscribe）→ 持锁调用 = 死锁。
 *      所以 `pushPacket` / `endOfStream` 先把要唤醒的订阅者收进 `_notify_scratch`，出锁再唤醒。
 *   2) **日志在锁外**：日志会拿日志器的锁，属于"外部调用"；锁内只改状态。
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
    : _gop(limits.gopMaxBytes()) {
    // 先站稳默认值（有界），再由 setLimits 校验入参；非法项保持默认并记日志
    _limits = Limits{};
    (void) setLimits(limits);
}

bool MediaSource::setLimits(const Limits &limits) {
    // 校验的是"物理量"（码率 / 时长 / 人数），字节数是推导出来的结果
    if (limits.max_bitrate_bps == 0) {
        WarnL << "MediaSource::setLimits 被拒：max_bitrate_bps 不能为 0";
        return false;
    }
    if (limits.max_bitrate_bps > kHardMaxBitrateBps) {
        WarnL << "MediaSource::setLimits 被拒：max_bitrate_bps 超过硬上限 " << kHardMaxBitrateBps;
        return false;
    }
    if (limits.latency_budget_ms == 0 || limits.latency_budget_ms > kHardMaxLatencyMs) {
        WarnL << "MediaSource::setLimits 被拒：latency_budget_ms 非法（" << limits.latency_budget_ms
              << "，允许 1~" << kHardMaxLatencyMs << "）";
        return false;
    }
    if (limits.max_gop_ms == 0 || limits.max_gop_ms > kHardMaxGopMs) {
        WarnL << "MediaSource::setLimits 被拒：max_gop_ms 非法（" << limits.max_gop_ms << "，允许 1~"
              << kHardMaxGopMs << "）";
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
    // 推导出的字节数也要过各自组件的硬上限（复用它们的校验，避免两套规则漂移）
    FrameQueue queue_probe;
    if (!queue_probe.setLimits(FrameQueue::Limits{limits.queueMaxBytes()})) {
        return false;
    }
    GopCache gop_probe(limits.gopMaxBytes());
    if (gop_probe.maxBytes() != limits.gopMaxBytes()) {
        WarnL << "MediaSource::setLimits 被拒：推导出的 max_gop_bytes 非法（" << limits.gopMaxBytes()
              << "）";
        return false;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    _limits = limits;
    (void) _gop.setMaxBytes(limits.gopMaxBytes());
    return true;
}

MediaSource::Limits MediaSource::limits() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _limits;
}

size_t MediaSource::pruneExpiredLocked() {
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
    Subscriber::Ptr sub;
    bool rejected_by_limit = false;
    bool seed_failed = false;
    size_t seed_size = 0;
    size_t queue_limit = 0;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        pruneExpiredLocked();
        queue_limit = _limits.queueMaxBytes();
        if (_entries.size() >= _limits.max_subscribers) {
            ++_total_rejected;
            rejected_by_limit = true;
        } else {
            const FrameQueue::Limits queue_limits{queue_limit};
            sub.reset(new Subscriber(_next_id++, queue_limits));

            // 灌 GOP 缓存：保证消费者拿到的第一条是视频关键帧（否则首帧解不出 = 静默错误）
            const std::vector<MediaPacket::Ptr> seed = _gop.snapshot();
            if (!seed.empty()) {
                const FrameQueue::PushResult first = sub->queue().push(seed.front());
                if (first == FrameQueue::PushResult::RejectedNoSpace) {
                    ++_total_seed_failed;
                    ++_total_rejected;
                    seed_failed = true;
                    seed_size = seed.front()->size();
                    sub.reset(); // 宁可不接，也不让对端从 GOP 中间开始
                } else {
                    for (size_t i = 1; i < seed.size(); ++i) {
                        // 后续包装不下就按队列自己的策略丢（丢了多少在 queue().stats() 里可见）
                        (void) sub->queue().push(seed[i]);
                    }
                }
            }

            if (sub) {
                if (_ended) {
                    // 源已经结束：新订阅者必须**立刻**知道，否则消费端会永远等
                    (void) sub->queue().markEndOfStream();
                }
                _entries.push_back(Entry{sub, sub->id()});
                ++_total_subscribe;
            }
        }
    }

    // 日志一律在锁外（锁内只改状态）
    if (rejected_by_limit) {
        WarnL << "MediaSource::subscribe 被拒：订阅者已达上限 " << _limits.max_subscribers;
    }
    if (seed_failed) {
        WarnL << "MediaSource::subscribe 失败：关键帧 " << seed_size
              << " 字节灌不进队列（上限 " << queue_limit << "）→ 宁可不接";
    }
    return sub;
}

bool MediaSource::unsubscribe(Subscriber::Id id) {
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto it = _entries.begin(); it != _entries.end(); ++it) {
        if (it->id == id) {
            _entries.erase(it);
            return true;
        }
    }
    return false;
}

size_t MediaSource::subscriberCount() {
    std::lock_guard<std::mutex> lock(_mutex);
    pruneExpiredLocked();
    return _entries.size();
}

bool MediaSource::endOfStream() {
    bool changed = false;
    std::vector<Subscriber::Ptr> to_notify;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        pruneExpiredLocked();
        if (!_ended) {
            _ended = true;
            changed = true;
            for (const auto &entry : _entries) {
                if (Subscriber::Ptr sub = entry.sub.lock()) {
                    (void) sub->queue().markEndOfStream();
                    // 队列里可能还有没消费完的数据（也可能消费者正等着）→ 出锁后唤醒它
                    to_notify.push_back(sub);
                }
            }
        }
    }
    if (changed) {
        for (const auto &sub : to_notify) {
            (void) sub->notifyIfNeeded(); // 锁外：async 可能内联执行
        }
    }
    return changed;
}

MediaSource::PushStats MediaSource::pushPacket(MediaPacket::Ptr packet) {
    PushStats stats;
    Subscriber::Id broken_id = 0; // 只在锁内记，出锁后再告警
    std::vector<Subscriber::Ptr> to_notify;

    if (!packet) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            ++_total_rejected;
        }
        WarnL << "MediaSource::pushPacket 收到空包（调用方 bug）";
        return stats;
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        pruneExpiredLocked();
        stats.subscribers = _entries.size();

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
                    broken_id = sub->id();
                }
                break;
            }

            // 队列里真的多了东西才需要唤醒（丢新包/被拒的情况下没有新数据）
            if (result == FrameQueue::PushResult::Accepted ||
                result == FrameQueue::PushResult::DroppedToMakeRoom) {
                to_notify.push_back(sub);
            }
        }
    } // 出锁

    if (broken_id != 0) {
        WarnL << "订阅者 id=" << broken_id << " 已置为 broken（不可丢的包进不去）→ 调用方应断开它";
    }
    for (const auto &sub : to_notify) {
        if (sub->notifyIfNeeded()) { // 锁外：async 可能内联执行 drain 回调
            ++stats.notified;
        }
    }
    return stats;
}

std::vector<Subscriber::Id> MediaSource::brokenSubscriberIds() const {
    // 只读巡检：真正的断连由连接层做（DESIGN_M5 §7）
    std::vector<Subscriber::Id> ids;
    std::lock_guard<std::mutex> lock(_mutex);
    for (const auto &entry : _entries) {
        if (Subscriber::Ptr sub = entry.sub.lock()) {
            if (sub->broken()) {
                ids.push_back(entry.id);
            }
        }
    }
    return ids;
}

std::string MediaSource::dumpStatsJson() {
    // 契约：合法 JSON 对象**片段**（不含最外层花括号），键名用模块名做前缀
    std::string out;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        // **先惰性清理**：断开的订阅者是靠 shared_ptr 释放来发现的，
        // 而源可能已经 EOS、不再有流量 → 不清的话统计会永远停在旧值
        // （NFR-6 的验收就是拿这个数字当判据的）
        pruneExpiredLocked();
        out = "\"max_subscribers\":" + std::to_string(_limits.max_subscribers) +
              ",\"subscribers\":" + std::to_string(_entries.size()) +
              ",\"queue_bytes\":" + std::to_string(_limits.queueMaxBytes()) +
              ",\"gop_bytes\":" + std::to_string(_limits.gopMaxBytes()) +
              ",\"out_budget_bps\":" + std::to_string(_limits.outputBitrateBudgetBps());
        out += ",\"broken\":[";
        bool first = true;
        for (const auto &entry : _entries) {
            if (Subscriber::Ptr sub = entry.sub.lock()) {
                if (sub->broken()) {
                    if (!first) {
                        out += ",";
                    }
                    first = false;
                    out += std::to_string(entry.id);
                }
            }
        }
        out += "]";
        out += ",\"counters\":{\"delivered\":" + std::to_string(_total_delivered.load()) +
               ",\"dropped\":" + std::to_string(_total_dropped_to_make_room.load()) +
               ",\"dropped_incoming\":" + std::to_string(_total_dropped_incoming.load()) +
               ",\"rejected\":" + std::to_string(_total_rejected.load()) +
               ",\"subscribe\":" + std::to_string(_total_subscribe.load()) +
               ",\"seed_failed\":" + std::to_string(_total_seed_failed.load()) +
               ",\"auto_unsub\":" + std::to_string(_total_auto_unsubscribe.load()) +
               ",\"broken\":" + std::to_string(_total_broken.load()) + "}";
    }
    return "\"media_source\":{" + out + "}";
}

std::string MediaSource::dumpStats() const {
    // 最小版（FR-6.1）：一行、可 grep、可从任意线程调用。
    // 完整 StatsCenter 与 /api/stats 接线见 M5-d。
    std::lock_guard<std::mutex> lock(_mutex);
    return "media_source{limits: bitrate=" + std::to_string(_limits.max_bitrate_bps) +
           "bps latency=" + std::to_string(_limits.latency_budget_ms) +
           "ms gop=" + std::to_string(_limits.max_gop_ms) +
           "ms queue_bytes=" + std::to_string(_limits.queueMaxBytes()) +
           " gop_bytes=" + std::to_string(_limits.gopMaxBytes()) +
           " max_subs=" + std::to_string(_limits.max_subscribers) +
           " out_budget=" + std::to_string(_limits.outputBitrateBudgetBps()) +
           "bps | counters: delivered=" + std::to_string(_total_delivered.load()) +
           " dropped=" + std::to_string(_total_dropped_to_make_room.load()) +
           " dropped_incoming=" + std::to_string(_total_dropped_incoming.load()) +
           " rejected=" + std::to_string(_total_rejected.load()) +
           " subscribe=" + std::to_string(_total_subscribe.load()) +
           " seed_failed=" + std::to_string(_total_seed_failed.load()) +
           " auto_unsub=" + std::to_string(_total_auto_unsubscribe.load()) +
           " broken=" + std::to_string(_total_broken.load()) + "}";
}

} // namespace mzmedia
