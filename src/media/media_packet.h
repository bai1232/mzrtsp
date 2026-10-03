/*
 * MediaPacket：一帧编码数据的**不可变**载体（M5-a）
 * ============================================================================
 * 形状来源：docs/DESIGN_M5.md §3.1；上位需求 FR-5.1 / FR-5.2 / FR-5.3
 *
 * 命名说明：本类叫 MediaPacket 而不是 MediaFrame —— ARCHITECTURE.md §4 用的就是这个
 * 名字，而且 v0.1/v0.2 在链路上搬运的是**编码后**的数据包（remux 不解码）；
 * v0.3 的"解码帧"是另一类对象，不该同名。决策记录见 DESIGN_M5.md §7。
 *
 * 线程契约（重要）：本类所有接口**不加锁**；对象一旦 create 出来就不可变，
 * 因此可以被多个线程同时**只读**共享。可变的只有 shared_ptr 的引用计数。
 *
 * 为什么用 shared_ptr<const vector<uint8_t>> 而不是裸指针 + 长度：
 *   一帧要分发给 N 个订阅者（零拷贝），生命周期必须由引用计数表达；
 *   const 保证分发出去之后没人能改内容（否则一个订阅者改了会影响其他人）。
 * ============================================================================
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace mzmedia {

/// 媒体类型：决定"丢了会不会花屏"（视频关键帧丢了必花屏；音频没有关键帧概念）
enum class MediaKind : uint8_t {
    Video = 0,
    Audio = 1,
};

class MediaPacket {
public:
    using Ptr = std::shared_ptr<MediaPacket>;
    using Payload = std::shared_ptr<const std::vector<uint8_t>>;

    /**
     * 工厂
     * @param kind 媒体类型
     * @param stream_index 流索引（>= 0；负值视为调用方 bug）
     * @param payload 编码数据（不可为空、不可为 0 字节）
     * @param key_frame 关键帧标志（音频无意义，传什么都行，`droppable()` 不看它）
     * @param dts_ms 解码时间戳（毫秒；**允许负值** —— B 帧/部分容器的首个 dts 就是负的）
     * @param pts_ms 显示时间戳（毫秒；允许负值）
     * @return nullptr = 参数非法（payload 为空/0 字节、stream_index < 0）
     * @note 不做"造一个空包代替"的静默降级（AI_COLLAB §4.5）；
     *       也不校验 pts/dts 的相对大小 —— 那是封装层的事（见 MonotonicGuard）
     */
    static Ptr create(MediaKind kind, int stream_index, Payload payload, bool key_frame, int64_t dts_ms,
                      int64_t pts_ms);

    MediaKind kind() const {
        return _kind;
    }
    int streamIndex() const {
        return _stream_index;
    }
    bool isKeyFrame() const {
        return _key_frame;
    }
    int64_t dtsMs() const {
        return _dts_ms;
    }
    int64_t ptsMs() const {
        return _pts_ms;
    }

    /// 编码数据字节数
    size_t size() const {
        return _payload ? _payload->size() : 0;
    }
    /// 编码数据首地址（同一 MediaPacket 被多个订阅者共享时，这个指针相同）
    const uint8_t *data() const {
        return _payload ? _payload->data() : nullptr;
    }
    const Payload &payload() const {
        return _payload;
    }

    /**
     * 是否允许被丢弃（拥塞时）
     * @return **唯一不可丢的是视频关键帧**；音频与视频非关键帧都可丢
     * @note FR-5.2（本批修订）：音频也参与丢弃 —— 但丢弃的单位是"队头最旧的一段"，
     *       同一段的音频与视频一起走，所以不会出现"视频跳了、音频还在原地"的音画错位
     *       （见 `docs/DESIGN_M5.md` §4.2）。比"永远保音频"更符合实时流的取舍：
     *       宁可两端都断一下，也不要 A/V 长期错位。
     */
    bool droppable() const {
        return !(_kind == MediaKind::Video && _key_frame);
    }

private:
    MediaPacket(MediaKind kind, int stream_index, Payload payload, bool key_frame, int64_t dts_ms,
                int64_t pts_ms)
        : _kind(kind)
        , _stream_index(stream_index)
        , _payload(std::move(payload))
        , _key_frame(key_frame)
        , _dts_ms(dts_ms)
        , _pts_ms(pts_ms) {}

    MediaKind _kind;
    int _stream_index;
    Payload _payload;
    bool _key_frame;
    int64_t _dts_ms;
    int64_t _pts_ms;
};

} // namespace mzmedia
