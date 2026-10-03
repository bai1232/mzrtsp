/*
 * FrameQueue：每个订阅者一个的**有界**包队列（M5-a 建立，M5-b 变线程安全）
 * ============================================================================
 * 形状来源：docs/DESIGN_M5.md §3.2；上位需求 FR-5.1（64 帧 / 8MB，先到者为准）
 *                                          FR-5.2（溢出优先丢非关键帧并计数）
 *
 * 线程契约（M5-b 起，**已变更**）：
 *   - **SPSC**：单生产者 = 源线程（只调 push），单消费者 = 该订阅者所属的消费者线程（只调 pop）；
 *   - 内部用**一把互斥锁**保护，所以两种操作不必在同一线程；但**不承诺**多生产者/多消费者；
 *   - 所有取值接口（packets/bytes/stats/...）返回**快照**，可在任意线程调用；
 *   - 锁内不调用任何回调（本类没有回调），不存在重入问题。
 *   为什么改成线程安全：M5-b 选定「数据通道 = 队列，唤醒 = async」（DESIGN_M5.md §4.4）。
 *   若改成"帧本体走 async"，真正的缓冲会变成 poller 的任务队列（65536 条），
 *   FR-5.1 的 64 帧/8MB 计量就失真了、丢帧计数会长期为 0。
 *
 * 三条硬要求（不变）：
 *   1) 上限**不可绕过**：setLimits(0) 必须被拒（AI_COLLAB §4.6：0/无界不是选项）；
 *   2) 丢弃**必须分类计数**：丢的是"可丢包"还是"关键帧腾不出空间"，调用方动作不同；
 *   3) 丢弃**不静默**：RejectedNoSpace（不可丢的包进不去）由调用方决定断开该订阅者。
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

    /// 当前包数（快照）
    size_t packets() const;
    /// 当前字节数（快照）
    size_t bytes() const;
    /// 是否为空（快照）
    bool empty() const;
    /// 是否已标记流结束
    bool endOfStream() const;
    /// 计数快照（可在任意线程读取）
    Stats stats() const;

private:
    // 硬上限：防止有人把上限调成天文数字，等价于"无界"（有界性不可协商）
    static constexpr size_t kHardMaxPackets = 1u << 20;                // 1,048,576 帧
    static constexpr size_t kHardMaxBytes = 256u * 1024u * 1024u;      // 256MB

    /// 以下三个只在持锁时调用
    bool fits(size_t size) const;
    bool dropOldestDroppable();

    mutable std::mutex _mutex;
    std::deque<MediaPacket::Ptr> _queue;
    size_t _bytes = 0;
    Limits _limits;
    Stats _stats;
    bool _end_of_stream = false;
};

} // namespace mzmedia
