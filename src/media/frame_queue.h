/*
 * FrameQueue：每个订阅者一个的**有界**包队列（M5-a 建立，M5-b 线程安全，本批改为**只按字节**限流）
 * ============================================================================
 * 形状来源：docs/DESIGN_M5.md §3.2；上位需求 FR-5.1（**修订版**：上限 = 码率上限 × 延迟额度，
 *                                           只按字节）、FR-5.2（溢出丢最旧的一段，音视频成对）
 *
 * 线程契约：**SPSC** —— 单生产者 = 源线程（只调 push），单消费者 = 该订阅者所属的消费者线程
 *   （只调 pop）。内部一把互斥锁；所有取值接口返回**快照**，可在任意线程调用；
 *   锁内不调用任何回调（本类没有回调）。
 *
 * 为什么**只按字节**、不再有"帧数上限"（FR-5.1 修订）：
 *   帧数上限与字节上限是同一件事的两种量纲，而"延迟额度"是**时间**量纲：
 *   队列上限 = 码率上限 × 延迟额度。跑 4K 时 64 帧可能不到 1 秒（帧数上限先到、延迟失真），
 *   跑低码率时 64 帧可能几十秒（字节上限先到、内存浪费）。只保留字节上限，
 *   语义唯一且能直接从"能容忍多少秒延迟"推出来。取舍记录见 DESIGN_M5.md §7。
 *
 * 三条硬要求：
 *   1) 上限**不可绕过**：setLimits(0) 必须被拒（AI_COLLAB §4.6：0/无界不是选项）；
 *   2) 丢弃**不静默**：每次丢弃都进 stats()；不可丢的包（视频关键帧）进不去时返回
 *      RejectedNoSpace，由调用方决定断开订阅者；
 *   3) 丢弃单位是"**队头最旧的一段**"（音视频一起走），不是"只丢视频"。
 * ============================================================================
 */

#pragma once

#include "media/media_packet.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

namespace mzmedia {

class FrameQueue {
public:
    /// 上限：**只有字节**（FR-5.1 修订版）
    /// 初值 2,000,000 B = 8 Mbps × 2 s（推导见 `MediaSource::Limits`，不要在各处拍字节数）
    struct Limits {
        size_t max_bytes = 2000000;
    };

    /// 观测量（FR-5.2 要求"累加丢弃计数"）
    struct Stats {
        uint64_t pushed = 0;            // 成功入队（含"丢了旧包让位"后的入队）
        uint64_t popped = 0;            // 被消费者取走
        uint64_t dropped = 0;           // 为腾空间丢掉的旧包（音频与视频非关键帧都算）
        uint64_t dropped_incoming = 0;  // 新包自己可丢却腾不出空间 → 丢掉新包（正常降级）
        uint64_t rejected_no_space = 0; // 新包**不可丢**（视频关键帧）却进不去 → 调用方必须处理
        uint64_t dropped_on_clear = 0;  // clear() 丢掉的包
    };

    enum class PushResult : uint8_t {
        Accepted = 0,          // 已入队，没有丢任何包
        DroppedToMakeRoom = 1, // 已入队，但为腾空间丢了若干可丢的旧包
        DroppedIncoming = 2,   // 没入队，丢的是**新包自己**（它可丢 → 调用方无需动作）
        RejectedNoSpace = 3,   // 没入队，且新包**不可丢**（视频关键帧）→ 调用方必须处理
    };

    enum class PopResult : uint8_t {
        Packet = 0,      // 拿到一个包
        Empty = 1,       // 暂时没有（还会再有）
        EndOfStream = 2, // 不会再有（与 Empty 必须分开：见 DESIGN_M4 §7 的 EOF/Error 分离）
    };

    FrameQueue();

    /// @return false = 上限非法（0 或超过硬上限），**保持原值不变**
    /// @note 只在启动前（没有并发访问时）调用；运行期改上限需要先停流
    bool setLimits(const Limits &limits);

    Limits limits() const;

    /// 入队（**源线程**调用）。见 PushResult 四态语义
    PushResult push(MediaPacket::Ptr packet);

    /// 取包（**消费者线程**调用）；@param packet 出参，成功时写入
    PopResult pop(MediaPacket::Ptr *packet);

    /// 标记"不会再有数据了"；@return true = 状态发生改变（重复调用返回 false）
    bool markEndOfStream();

    /// 清空队列（订阅者断开/回收时用）；@return 被丢弃的包数。**不动 EOS 标记**
    size_t clear();

    /// 当前字节数（快照）
    size_t bytes() const;
    /// 当前包数（快照；**只作观测**，不再参与限流 —— FR-5.1 修订后上限只按字节）
    size_t packets() const;
    /// 是否为空（快照）
    bool empty() const;
    /// 是否已标记流结束
    bool endOfStream() const;
    /// 计数快照（可在任意线程读取）
    Stats stats() const;

private:
    // 硬上限：防止有人把上限调成天文数字，等价于"无界"（有界性不可协商）
    static constexpr size_t kHardMaxBytes = 256u * 1024u * 1024u; // 256MB

    /// 以下两个只在持锁时调用
    bool fits(size_t size) const;
    /// 从队头开始丢一个可丢包；@return false = 队列里已没有可丢的包
    bool dropOldestDroppable();

    mutable std::mutex _mutex;
    std::deque<MediaPacket::Ptr> _queue;
    size_t _bytes = 0;
    Limits _limits;
    Stats _stats;
    bool _end_of_stream = false;
};

} // namespace mzmedia
