/*
 * FrameQueue 实现（M5-a）
 * ============================================================================
 * 核心是 push 的四态语义（见头文件）：
 *   Accepted / DroppedToMakeRoom / DroppedIncoming / RejectedNoSpace
 * 前三个都不需要调用方做额外动作；只有第四个（**不可丢**的包进不去）必须让调用方
 * 断开该订阅者 —— 否则它会一直收不到关键帧/音频，表现为"永久花屏 + 静音"，
 * 而且没有任何错误发生（最难查的那类问题）。
 * ============================================================================
 */

#include "media/frame_queue.h"

#include "core/logger.h"

#include <utility>

namespace mzmedia {

FrameQueue::FrameQueue() = default;

bool FrameQueue::setLimits(const Limits &limits) {
    if (limits.max_packets == 0 || limits.max_bytes == 0) {
        WarnL << "FrameQueue::setLimits 被拒：上限不能为 0（0 不等于无界，有界性不可协商）"
              << " max_packets=" << limits.max_packets << " max_bytes=" << limits.max_bytes;
        return false;
    }
    if (limits.max_packets > kHardMaxPackets || limits.max_bytes > kHardMaxBytes) {
        WarnL << "FrameQueue::setLimits 被拒：超过硬上限（max_packets<=" << kHardMaxPackets
              << ", max_bytes<=" << kHardMaxBytes << "）";
        return false;
    }
    _limits = limits;
    return true;
}

bool FrameQueue::fits(size_t size) const {
    return _queue.size() + 1 <= _limits.max_packets && _bytes + size <= _limits.max_bytes;
}

bool FrameQueue::dropOldestDroppable() {
    for (auto it = _queue.begin(); it != _queue.end(); ++it) {
        if (*it && (*it)->droppable()) {
            _bytes -= (*it)->size();
            _queue.erase(it);
            ++_stats.dropped_non_key;
            return true;
        }
    }
    return false;
}

FrameQueue::PushResult FrameQueue::push(MediaPacket::Ptr packet) {
    if (!packet) {
        ++_stats.rejected_no_space;
        WarnL << "FrameQueue::push 收到空包（调用方 bug）";
        return PushResult::RejectedNoSpace;
    }

    const size_t size = packet->size();

    // 单包比整个字节上限还大：腾空也没用。绝不为一个包把队列清空
    if (size > _limits.max_bytes) {
        if (packet->droppable()) {
            ++_stats.dropped_incoming;
            return PushResult::DroppedIncoming;
        }
        ++_stats.rejected_no_space;
        WarnL << "FrameQueue::push 拒绝：不可丢的包 " << size << " 字节 > 队列上限 "
              << _limits.max_bytes << "（调用方应断开该订阅者）";
        return PushResult::RejectedNoSpace;
    }

    bool dropped_old = false;
    while (!fits(size)) {
        if (!dropOldestDroppable()) {
            // 腾不出空间了：新包可丢 → 丢新包（正常降级）；新包不可丢 → 交给调用方
            if (packet->droppable()) {
                ++_stats.dropped_incoming;
                return PushResult::DroppedIncoming;
            }
            ++_stats.rejected_no_space;
            return PushResult::RejectedNoSpace;
        }
        dropped_old = true;
    }

    _bytes += size;
    _queue.push_back(std::move(packet));
    ++_stats.pushed;
    return dropped_old ? PushResult::DroppedToMakeRoom : PushResult::Accepted;
}

FrameQueue::PopResult FrameQueue::pop(MediaPacket::Ptr *packet) {
    if (packet == nullptr) {
        WarnL << "FrameQueue::pop 传了空出参（调用方 bug）";
        return PopResult::Empty;
    }
    if (_queue.empty()) {
        // "暂时没有"与"不会再有"必须分开：混在一起会让消费端把结束当饥饿，永远等下去
        return _end_of_stream ? PopResult::EndOfStream : PopResult::Empty;
    }
    *packet = std::move(_queue.front());
    _queue.pop_front();
    _bytes -= (*packet)->size();
    ++_stats.popped;
    return PopResult::Packet;
}

bool FrameQueue::markEndOfStream() {
    if (_end_of_stream) {
        return false; // 幂等：状态没变
    }
    _end_of_stream = true;
    return true;
}

size_t FrameQueue::clear() {
    const size_t count = _queue.size();
    _stats.dropped_on_clear += count;
    _queue.clear();
    _bytes = 0;
    return count;
}

} // namespace mzmedia
