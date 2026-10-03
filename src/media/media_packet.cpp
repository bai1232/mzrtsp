/*
 * MediaPacket 工厂实现（M5-a）
 * ============================================================================
 * 只有工厂有实现，其余全是内联只读访问 —— 对象不可变，没有别的行为。
 * 失败一律返回 nullptr 并记日志（不抛异常：这条路在分发热路径上被频繁走到）。
 * ============================================================================
 */

#include "media/media_packet.h"

#include "core/logger.h"

namespace mzmedia {

MediaPacket::Ptr MediaPacket::create(MediaKind kind, int stream_index, Payload payload, bool key_frame,
                                     int64_t dts_ms, int64_t pts_ms) {
    if (stream_index < 0) {
        ErrorL << "MediaPacket::create 失败：stream_index 非法（" << stream_index << "）";
        return nullptr;
    }
    if (!payload) {
        ErrorL << "MediaPacket::create 失败：payload 为空指针（stream_index=" << stream_index << "）";
        return nullptr;
    }
    if (payload->empty()) {
        ErrorL << "MediaPacket::create 失败：payload 为 0 字节（stream_index=" << stream_index << "）";
        return nullptr;
    }
    // 私有构造函数，只能在这里 new；用 shared_ptr 管理，避免 make_shared 需要公开构造函数
    return Ptr(new MediaPacket(kind, stream_index, std::move(payload), key_frame, dts_ms, pts_ms));
}

} // namespace mzmedia
