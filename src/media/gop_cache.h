/*
 * GopCache：最近 **1 个 GOP** 的缓存（M5-a）
 * ============================================================================
 * 形状来源：docs/DESIGN_M5.md §3.3；上位依据 ARCHITECTURE.md §4「GOP 缓存：新订阅者
 * 先灌入缓存的关键帧及其后续帧，保证首帧可解码」
 *
 * 它只解决一件事：**中途接入**的订阅者不能从 GOP 中间开始（首帧解不出来 → 花屏到
 * 下一个关键帧）。所以缓存以"视频关键帧"为唯一起点，关键帧一到就丢掉整个旧 GOP。
 *
 * 线程契约：不加锁、不可重入，只在源线程使用（与 MediaSource 同线程）。
 *
 * 两条容易踩的边界（都写进用例）：
 *   1) 还没有关键帧时，别的包**不缓存**（否则 snapshot() 会给出一个解不出的开头）；
 *   2) 单个 GOP 超过 max_bytes 时**丢最旧的非关键帧**，但**关键帧必须保留** ——
 *      这是有意的取舍：宁可缓存超一点，也不能让"从关键帧接入"这条保证失效。
 *      因此上界是 `max_bytes + 一个关键帧大小`（关键帧本身由 M4 的 max_packet_size 兜底）。
 *
 * 与 FrameQueue 的**有意差异**：队列里音频不可丢（丢了是静音空洞），缓存里音频**可丢**
 * （新订阅者晚几毫秒收到音频无妨，但缓存必须有界）。差异写在这里，避免被当成 bug。
 * ============================================================================
 */

#pragma once

#include "media/media_packet.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mzmedia {

class GopCache {
public:
    struct Stats {
        uint64_t started_gop = 0; // 见到过多少个关键帧（= 重开过多少次 GOP）
        uint64_t cached = 0;      // 缓存的非关键帧数
        uint64_t dropped = 0;     // 为收敛丢掉的包数
        uint64_t skipped = 0;     // 未缓存：入参为空 / 还没有 GOP 起点
    };

    enum class FeedResult : uint8_t {
        StartedNewGop = 0, // 视频关键帧：旧 GOP 已丢弃，本包成为新起点
        Cached = 1,        // 已追加到当前 GOP
        Skipped = 2,       // 未缓存（空包 / 还没有关键帧）
    };

    explicit GopCache(size_t max_bytes = 8u * 1024u * 1024u);

    /// @return false = 上限非法（0 或超过硬上限），保持原值不变
    bool setMaxBytes(size_t bytes);

    size_t maxBytes() const {
        return _max_bytes;
    }

    FeedResult feed(const MediaPacket::Ptr &packet);

    /// 当前 GOP 的快照（**第一项必是视频关键帧**）；无缓存时返回空
    std::vector<MediaPacket::Ptr> snapshot() const;

    size_t packets() const {
        return _packets.size();
    }
    size_t bytes() const {
        return _bytes;
    }

    /// 清空缓存；@return 丢弃的包数
    size_t clear();

    const Stats &stats() const {
        return _stats;
    }

private:
    static constexpr size_t kHardMaxBytes = 256u * 1024u * 1024u;

    /// 缓存里可丢的包：除"视频关键帧"以外的一切（含音频，见文件头说明）
    static bool droppableInCache(const MediaPacket::Ptr &packet);

    /// 从缓存头部开始丢第一个可丢包；@return false = 没有可丢的（只剩关键帧）
    bool dropOldestDroppable();

    std::vector<MediaPacket::Ptr> _packets;
    size_t _bytes = 0;
    size_t _max_bytes;
    Stats _stats;
    bool _warned_overflow = false; // 一个 GOP 内只 Warn 一次，避免刷日志
};

} // namespace mzmedia
