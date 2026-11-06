/*
 * RtpPacketizer：把 `MediaPacket`（AVCC 视频 / 裸 AAC）打成 RTP 包
 * ============================================================================
 * 三条容易写错、且都能逐字节测的东西：
 *
 * 1) **时间戳用 PTS，不是 DTS**。RTP 时间戳 = 媒体的**呈现时刻**；B 帧下它允许
 *    "回退"（解码序 ≠ 显示序），重排由 `seq` 承担。拿 dts 会让画面顺序错乱。
 *
 * 2) **FU-A 分片数**：原 NAL 的 1 字节 header 被 `FU indicator + FU header` 取代，
 *    不进入负载 —— 所以是 `ceil((nal_len - 1) / (mtu - 12 - 2))`，不是 `ceil(nal_len / ...)`。
 *    单片直发判据：`nal_len <= mtu - 12`。
 *
 * 3) **marker 只在"一帧/一个 AU 的最后一个包"置位**：中间 NAL 的最后一包不能置位，
 *    否则接收端会把半帧当成完整帧交上去。
 * ============================================================================
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mzmedia {

class RtpPacketizer {
public:
    struct Config {
        uint8_t payload_type = 96; ///< 视频 96（动态）；音频另给 97
        uint32_t ssrc = 0;         ///< 每会话随机（0 = 由构造随机）
        size_t mtu = 1400;         ///< IP MTU（不含以太网头）；默认 1400 保守值
    };

    explicit RtpPacketizer(const Config &config);

    /// 一个 H264 NAL（AVCC 里的裸 NAL，**不含**长度前缀/起始码）→ 1..N 个 RTP 包
    /// @param marker 该 NAL 是否是一帧的最后一个
    void packetizeNal(const uint8_t *nal, size_t len, uint32_t rtp_ts, bool marker,
                      std::vector<std::string> *out);

    /// 一个裸 AAC AU → 1 个 RTP 包（RFC 3640，AAC-hbr，13 位 AU-size）
    void packetizeAac(const uint8_t *au, size_t len, uint32_t rtp_ts, std::vector<std::string> *out);

    /// 该 NAL 会被切成几片（单测直接断言这个公式）
    size_t fragmentsFor(size_t nal_len) const;

    uint16_t nextSeq() const {
        return _seq;
    }
    uint8_t payloadType() const {
        return _config.payload_type;
    }
    uint32_t ssrc() const {
        return _config.ssrc;
    }
    uint64_t packets() const {
        return _packets;
    }
    uint64_t bytes() const {
        return _bytes;
    }

private:
    void appendRtpHeader(std::string *packet, bool marker, uint32_t rtp_ts);

    Config _config;
    uint16_t _seq;
    uint64_t _packets = 0;
    uint64_t _bytes = 0;
};

} // namespace mzmedia
