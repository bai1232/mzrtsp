#include "output/rtp_packetizer.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace mzmedia {

namespace {
constexpr size_t kRtpHeaderSize = 12;
constexpr size_t kFuAHeaderSize = 2;

uint32_t randomSsrc() {
    // 用地址 + 单调计数拼一个：只要求"每会话不同"，不要求密码学随机
    static std::atomic<uint32_t> counter{1};
    const uint32_t salt = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(&counter) ^ static_cast<uintptr_t>(counter.load()));
    return (salt * 2654435761u) ^ (counter.fetch_add(1) * 40503u);
}
} // namespace

RtpPacketizer::RtpPacketizer(const Config &config)
    : _config(config)
    , _seq(static_cast<uint16_t>(randomSsrc() & 0xFFFF)) {
    if (_config.ssrc == 0) {
        _config.ssrc = randomSsrc();
    }
}

void RtpPacketizer::appendRtpHeader(std::string *packet, bool marker, uint32_t rtp_ts) {
    const uint8_t b0 = 0x80; // V=2, P=0, X=0, CC=0
    const uint8_t b1 = static_cast<uint8_t>((marker ? 0x80 : 0x00) | (_config.payload_type & 0x7F));
    packet->push_back(static_cast<char>(b0));
    packet->push_back(static_cast<char>(b1));
    packet->push_back(static_cast<char>((_seq >> 8) & 0xFF));
    packet->push_back(static_cast<char>(_seq & 0xFF));
    packet->push_back(static_cast<char>((rtp_ts >> 24) & 0xFF));
    packet->push_back(static_cast<char>((rtp_ts >> 16) & 0xFF));
    packet->push_back(static_cast<char>((rtp_ts >> 8) & 0xFF));
    packet->push_back(static_cast<char>(rtp_ts & 0xFF));
    packet->push_back(static_cast<char>((_config.ssrc >> 24) & 0xFF));
    packet->push_back(static_cast<char>((_config.ssrc >> 16) & 0xFF));
    packet->push_back(static_cast<char>((_config.ssrc >> 8) & 0xFF));
    packet->push_back(static_cast<char>(_config.ssrc & 0xFF));
    ++_seq;
}

size_t RtpPacketizer::fragmentsFor(size_t nal_len) const {
    if (nal_len == 0) {
        return 0;
    }
    const size_t single_max = _config.mtu - kRtpHeaderSize;
    if (nal_len <= single_max) {
        return 1;
    }
    // NAL 头 1 字节被 FU indicator/header 取代 → 负载只有 nal_len - 1
    const size_t payload_cap = _config.mtu - kRtpHeaderSize - kFuAHeaderSize;
    const size_t body = nal_len - 1;
    return (body + payload_cap - 1) / payload_cap;
}

void RtpPacketizer::packetizeNal(const uint8_t *nal, size_t len, uint32_t rtp_ts, bool marker,
                                 std::vector<std::string> *out) {
    if (nal == nullptr || len == 0 || out == nullptr) {
        return;
    }
    const uint8_t nal_header = nal[0];
    const uint8_t nal_type = nal_header & 0x1F;
    const size_t single_max = _config.mtu - kRtpHeaderSize;

    if (len <= single_max) {
        std::string packet;
        packet.reserve(kRtpHeaderSize + len);
        appendRtpHeader(&packet, marker, rtp_ts);
        packet.append(reinterpret_cast<const char *>(nal), len);
        out->push_back(std::move(packet));
        ++_packets;
        _bytes += kRtpHeaderSize + len;
        return;
    }

    // FU-A：indicator 保留 NAL 的 F/NRI，只把 type 换成 28
    const uint8_t indicator = static_cast<uint8_t>((nal_header & 0xE0) | 28);
    const size_t payload_cap = _config.mtu - kRtpHeaderSize - kFuAHeaderSize;
    size_t offset = 1; // 跳过原 NAL 头
    while (offset < len) {
        const size_t chunk = (payload_cap < len - offset) ? payload_cap : (len - offset);
        const bool start = (offset == 1);
        const bool end = (offset + chunk >= len);
        std::string packet;
        packet.reserve(kRtpHeaderSize + kFuAHeaderSize + chunk);
        appendRtpHeader(&packet, marker && end, rtp_ts);
        packet.push_back(static_cast<char>(indicator));
        packet.push_back(static_cast<char>((start ? 0x80 : 0x00) | (end ? 0x40 : 0x00) | nal_type));
        packet.append(reinterpret_cast<const char *>(nal + offset), chunk);
        out->push_back(std::move(packet));
        ++_packets;
        _bytes += kRtpHeaderSize + kFuAHeaderSize + chunk;
        offset += chunk;
    }
}

void RtpPacketizer::packetizeAac(const uint8_t *au, size_t len, uint32_t rtp_ts,
                                 std::vector<std::string> *out) {
    if (au == nullptr || len == 0 || len > 8191 || out == nullptr) {
        return; // 13 位 AU-size 装不下：拒绝（调用方计数），不静默截断
    }
    std::string packet;
    packet.reserve(kRtpHeaderSize + 4 + len);
    appendRtpHeader(&packet, true, rtp_ts); // 一个 AU 一包：marker 置位
    // AU-headers-length = 16 bit；AU-size 13 bit + AU-index 3 bit
    const uint16_t size13 = static_cast<uint16_t>((len & 0x1FFF) << 3);
    packet.push_back(0x00);
    packet.push_back(0x10); // 头部长度 = 16 bits
    packet.push_back(static_cast<char>((size13 >> 8) & 0xFF));
    packet.push_back(static_cast<char>(size13 & 0xFF));
    packet.append(reinterpret_cast<const char *>(au), len);
    out->push_back(std::move(packet));
    ++_packets;
    _bytes += kRtpHeaderSize + 4 + len;
}

} // namespace mzmedia
