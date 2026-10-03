/*
 * GopCache 实现（M5-a 建立；本批上限改为推导值、可丢集合与 FrameQueue 统一）
 * ============================================================================
 * 策略要点（详见 gop_cache.h 文件头）：
 *   - 视频关键帧 = 唯一的 GOP 起点，到了就丢掉整个旧 GOP；
 *   - 还没有起点时不缓存任何包（否则 snapshot() 会给出解不出的开头）；
 *   - 超过上限丢最旧的非关键帧，但**关键帧永远保留**（上界 = max_bytes + 关键帧大小）。
 * ============================================================================
 */

#include "media/gop_cache.h"

#include "core/logger.h"

namespace mzmedia {

// 初值 = 码率上限 8 Mbps × 最大 GOP 时长 2 s = 2,000,000 B（推导见 MediaSource::Limits）
namespace {
constexpr size_t kDefaultMaxBytes = 2000000;
} // namespace

GopCache::GopCache(size_t max_bytes)
    : _max_bytes(kDefaultMaxBytes) {
    // 先站稳默认值（有界），再校验入参；非法则保持默认并记日志（绝不退化成无界）
    (void) setMaxBytes(max_bytes);
}

bool GopCache::setMaxBytes(size_t bytes) {
    if (bytes == 0) {
        WarnL << "GopCache::setMaxBytes 被拒：上限不能为 0（0 不等于无界）";
        return false;
    }
    if (bytes > kHardMaxBytes) {
        WarnL << "GopCache::setMaxBytes 被拒：超过硬上限 " << kHardMaxBytes << "（传入 " << bytes << "）";
        return false;
    }
    _max_bytes = bytes;
    return true;
}

bool GopCache::droppableInCache(const MediaPacket::Ptr &packet) {
    // 与 FrameQueue 同一规则：**唯一不可丢的是视频关键帧**，音频在两边都可丢。
    // （早期版本刻意让两边不同，本批统一 —— 同一个概念不搞两套规则。）
    return packet && !(packet->kind() == MediaKind::Video && packet->isKeyFrame());
}

bool GopCache::dropOldestDroppable() {
    for (auto it = _packets.begin(); it != _packets.end(); ++it) {
        if (droppableInCache(*it)) {
            _bytes -= (*it)->size();
            _packets.erase(it);
            ++_stats.dropped;
            return true;
        }
    }
    return false;
}

GopCache::FeedResult GopCache::feed(const MediaPacket::Ptr &packet) {
    if (!packet) {
        ++_stats.skipped;
        return FeedResult::Skipped;
    }

    if (packet->kind() == MediaKind::Video && packet->isKeyFrame()) {
        // 关键帧 = 新的 GOP 起点：旧 GOP 全部作废
        _packets.clear();
        _bytes = 0;
        _warned_overflow = false;
        ++_stats.started_gop;
        _packets.push_back(packet);
        _bytes = packet->size();
        return FeedResult::StartedNewGop;
    }

    if (_packets.empty()) {
        // 还没有起点：缓存它只会让 snapshot() 给出一个解不出的开头
        ++_stats.skipped;
        return FeedResult::Skipped;
    }

    // 收敛：加入这个包若超上限，就丢最旧的可丢包
    while (_bytes + packet->size() > _max_bytes) {
        if (!dropOldestDroppable()) {
            // 只剩关键帧了：为了"从关键帧起的连续性"仍然追加。
            // 上界因此是 max_bytes + 一个关键帧大小（关键帧单包由 M4 的 max_packet_size 兜底）
            if (!_warned_overflow) {
                _warned_overflow = true;
                WarnL << "GopCache 超出上限：只剩关键帧可留，本 GOP 将超过 max_bytes=" << _max_bytes
                      << "（上界 = max_bytes + 关键帧大小）";
            }
            break;
        }
    }

    _packets.push_back(packet);
    _bytes += packet->size();
    ++_stats.cached;
    return FeedResult::Cached;
}

std::vector<MediaPacket::Ptr> GopCache::snapshot() const {
    return _packets;
}

size_t GopCache::clear() {
    const size_t count = _packets.size();
    _packets.clear();
    _bytes = 0;
    _warned_overflow = false;
    return count;
}

} // namespace mzmedia
