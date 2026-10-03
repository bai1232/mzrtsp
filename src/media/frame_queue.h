/*
 * FrameQueue：每个订阅者一个的**有界**包队列（M5-a）
 * ============================================================================
 * 形状来源：docs/DESIGN_M5.md §3.2；上位需求 FR-5.1（64 帧 / 8MB，先到者为准）
 *                                          FR-5.2（溢出优先丢非关键帧并计数）
 *
 * 线程契约：不加锁、不可重入。约定 push 只由源线程调用，pop 只由该订阅者所属线程
 * 调用（M5-b 用 EventPoller::async 投递保证）—— 本类自己不做同步。
 *
 * 三条硬要求：
 *   1) 上限**不可绕过**：setLimits(0) 必须被拒（AI_COLLAB §4.6：0/无界不是选项）；
 *   2) 丢弃**必须分类计数**：丢的是"可丢包"还是"关键帧腾不出空间"，调用方动作不同；
 *   3) 丢弃**不静默**：每次丢弃都进 stats()，关键帧放不下时返回 RejectedNoSpace
 *      由调用方决定（断开该订阅者），而不是悄悄丢掉关键帧让播放端花屏。
 * ============================================================================
 */

#pragma once

#include "media/media_packet.h"

#include <cstddef>
#include <cstdint>
#include <deque>

namespace mzmedia {

class FrameQueue {
public:
    /// 上限（FR-5.1 的初值：64 帧或 8MB，**先到者为准**）
    struct Limits {
        size_t max_packets = 64;
        size_t max_bytes = 8u * 1024u * 1024u;
    };

    /// 观测量（FR-5.2 要求"累加丢弃计数"）
    struct Stats {
        uint64_t pushed = 0;            // 成功入队（含"丢了旧包让位"后的入队）
        uint64_t popped = 0;            // 被消费者取走
        uint64_t dropped_non_key = 0;   // 为腾空间丢掉的**可丢**旧包
        uint64_t dropped_incoming = 0;  // 新包自己可丢却腾不出空间 → 丢掉新包（正常降级）
        uint64_t rejected_no_space = 0; // 新包**不可丢**（关键帧/音频）却进不去 → 调用方必须处理
        uint64_t dropped_on_clear = 0;  // clear() 丢掉的包
    };

    enum class PushResult : uint8_t {
        Accepted = 0,          // 已入队，没有丢任何包
        DroppedToMakeRoom = 1, // 已入队，但为腾空间丢了若干可丢的旧包
        DroppedIncoming = 2,   // 没入队，丢的是**新包自己**（它可丢 → 调用方无需动作）
        RejectedNoSpace = 3,   // 没入队，且新包**不可丢**（关键帧/音频）→ 调用方必须处理
    };

    enum class PopResult : uint8_t {
        Packet = 0,      // 拿到一个包
        Empty = 1,       // 暂时没有（还会再有）
        EndOfStream = 2, // 不会再有（与 Empty 必须分开：见 DESIGN_M4 §7 的 EOF/Error 分离）
    };

    FrameQueue();

    /// @return false = 上限非法（任一项为 0 或超过硬上限），**保持原值不变**
    bool setLimits(const Limits &limits);

    Limits limits() const {
        return _limits;
    }

    /// 入队（源线程调用）。见 PushResult 三态语义
    PushResult push(MediaPacket::Ptr packet);

    /// 取包（消费者线程调用）；@param packet 出参，成功时写入
    PopResult pop(MediaPacket::Ptr *packet);

    /// 标记"不会再有数据了"；@return true = 状态发生改变（重复调用返回 false）
    bool markEndOfStream();

    /// 清空队列（订阅者断开/回收时用）；@return 被丢弃的包数。**不动 EOS 标记**
    size_t clear();

    size_t packets() const {
        return _queue.size();
    }
    size_t bytes() const {
        return _bytes;
    }
    bool empty() const {
        return _queue.empty();
    }
    bool endOfStream() const {
        return _end_of_stream;
    }
    const Stats &stats() const {
        return _stats;
    }

private:
    // 硬上限：防止有人把上限调成天文数字，等价于"无界"（有界性不可协商）
    static constexpr size_t kHardMaxPackets = 1u << 20;                // 1,048,576 帧
    static constexpr size_t kHardMaxBytes = 256u * 1024u * 1024u;      // 256MB

    /// 再放一个 size 字节的包会不会超上限
    bool fits(size_t size) const;

    /// 从队头开始找第一个可丢包并丢掉；@return false = 队里没有可丢的包
    bool dropOldestDroppable();

    std::deque<MediaPacket::Ptr> _queue;
    size_t _bytes = 0;
    Limits _limits;
    Stats _stats;
    bool _end_of_stream = false;
};

} // namespace mzmedia
