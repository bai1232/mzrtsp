/*
 * FrameQueue 实现（M5-a 建立，M5-b 加锁成 SPSC，本批改为只按字节限流）
 * ============================================================================
 * 核心是 push 的四态语义（见头文件）：
 *   Accepted / DroppedToMakeRoom / DroppedIncoming / RejectedNoSpace
 * 前三个都不需要调用方做额外动作；只有第四个（**视频关键帧**进不去）必须让调用方断开订阅者
 * —— 否则后面全花屏，而且没有任何错误发生（最难查的那类问题）。
 *
 * 丢弃单位（FR-5.2 修订版）：从**队头**开始连续丢可丢包，直到放得下。
 *   因为丢的是"最旧的一段"，一段里的音频与视频自然一起走 → A/V 不会错位。
 *   注意不要写成"只丢视频非关键帧"：那会让视频跳一段而音频继续，音画越走越偏。
 *
 * 加锁要点：
 *   - 一把锁覆盖全部状态（队列 + 字节数 + 计数 + EOS），不拆细粒度：两个线程、O(1) 操作，
 *     拆开只会增加出错面；
 *   - **锁内不做任何外部调用**（本类没有回调）→ 不存在"持锁调外部代码"的死锁面；
 *   - 取值接口返回**快照**而不是引用：返回 `const Stats&` 会在调用方读的时候被另一个线程改，
 *     那是数据竞争（也是"看着对、TSAN 才报"的典型）。
 * ============================================================================
 */

#include "media/frame_queue.h"

#include "core/logger.h"

#include <utility>

namespace mzmedia {

FrameQueue::FrameQueue() = default;

bool FrameQueue::setLimits(const Limits &limits) {
    if (limits.max_bytes == 0) {
        WarnL << "FrameQueue::setLimits 被拒：上限不能为 0（0 不等于无界，有界性不可协商）";
        return false;
    }
    if (limits.max_bytes > kHardMaxBytes) {
        WarnL << "FrameQueue::setLimits 被拒：超过硬上限 " << kHardMaxBytes << "（传入 "
              << limits.max_bytes << "）";
        return false;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _limits = limits;
    return true;
}

FrameQueue::Limits FrameQueue::limits() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _limits;
}

bool FrameQueue::fits(size_t size) const {
    return _bytes + size <= _limits.max_bytes;
}

bool FrameQueue::dropOldestDroppable() {
    for (auto it = _queue.begin(); it != _queue.end(); ++it) {
        if (*it && (*it)->droppable()) {
            _bytes -= (*it)->size();
            _queue.erase(it);
            ++_stats.dropped;
            return true;
        }
    }
    return false;
}

FrameQueue::PushResult FrameQueue::push(MediaPacket::Ptr packet) {
    if (!packet) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            ++_stats.rejected_no_space;
        }
        WarnL << "FrameQueue::push 收到空包（调用方 bug）";
        return PushResult::RejectedNoSpace;
    }

    const size_t size = packet->size();
    bool too_large_and_not_droppable = false;
    PushResult result = PushResult::Accepted;

    {
        std::lock_guard<std::mutex> lock(_mutex);

        // 单包比整个字节上限还大：腾空也没用。绝不为一个包把队列清空
        if (size > _limits.max_bytes) {
            if (packet->droppable()) {
                ++_stats.dropped_incoming;
                result = PushResult::DroppedIncoming;
            } else {
                ++_stats.rejected_no_space;
                too_large_and_not_droppable = true;
                result = PushResult::RejectedNoSpace;
            }
        } else {
            bool dropped_old = false;
            bool made_room = true;
            // 从队头连续丢可丢包（最旧的一段，音视频一起走）直到放得下
            while (!fits(size)) {
                if (!dropOldestDroppable()) {
                    made_room = false;
                    break;
                }
                dropped_old = true;
            }
            if (!made_room) {
                // 队里已经只有不可丢的包（视频关键帧）：新包可丢就丢新包，否则交给调用方
                if (packet->droppable()) {
                    ++_stats.dropped_incoming;
                    result = PushResult::DroppedIncoming;
                } else {
                    ++_stats.rejected_no_space;
                    result = PushResult::RejectedNoSpace;
                }
            } else {
                _bytes += size;
                _queue.push_back(std::move(packet));
                ++_stats.pushed;
                result = dropped_old ? PushResult::DroppedToMakeRoom : PushResult::Accepted;
            }
        }
    } // 出锁之后才记日志：锁内不做任何外部调用

    if (too_large_and_not_droppable) {
        WarnL << "FrameQueue::push 拒绝：不可丢的包 " << size
              << " 字节 > 队列上限（调用方应断开该订阅者）";
    }
    return result;
}

FrameQueue::PopResult FrameQueue::pop(MediaPacket::Ptr *packet) {
    if (packet == nullptr) {
        WarnL << "FrameQueue::pop 传了空出参（调用方 bug）";
        return PopResult::Empty;
    }
    std::lock_guard<std::mutex> lock(_mutex);
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
    std::lock_guard<std::mutex> lock(_mutex);
    if (_end_of_stream) {
        return false; // 幂等：状态没变
    }
    _end_of_stream = true;
    return true;
}

size_t FrameQueue::clear() {
    std::lock_guard<std::mutex> lock(_mutex);
    const size_t count = _queue.size();
    _stats.dropped_on_clear += count;
    _queue.clear();
    _bytes = 0;
    return count;
}

size_t FrameQueue::bytes() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _bytes;
}

size_t FrameQueue::packets() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _queue.size();
}

bool FrameQueue::empty() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _queue.empty();
}

bool FrameQueue::endOfStream() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _end_of_stream;
}

FrameQueue::Stats FrameQueue::stats() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _stats; // 返回快照，绝不返回内部引用
}

} // namespace mzmedia
