#include "output/sdp_builder.h"

#include "core/logger.h"

#include <cstdio>

namespace mzmedia {

namespace {

std::string base64Encode(const uint8_t *data, size_t size) {
    static const char *kTable = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < size) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back(kTable[v & 0x3F]);
        i += 3;
    }
    if (i + 1 == size) {
        const uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.append("==");
    } else if (i + 2 == size) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::string hexEncode(const uint8_t *data, size_t size) {
    static const char *kHex = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0x0F]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

} // namespace

bool SdpBuilder::parseAvcC(const std::vector<uint8_t> &extradata, std::vector<uint8_t> *sps,
                           std::vector<uint8_t> *pps, uint8_t *profile, uint8_t *compat,
                           uint8_t *level, size_t *length_size) {
    // 结构：version(1) profile(1) compat(1) level(1) 0b111111xx(1) 0b111xxxxx(1) SPS...
    if (extradata.size() < 7 || extradata[0] != 1) {
        return false;
    }
    const uint8_t num_sps = extradata[5] & 0x1F;
    if (num_sps == 0) {
        return false;
    }
    size_t pos = 6;
    if (pos + 2 > extradata.size()) {
        return false;
    }
    const size_t sps_len = (static_cast<size_t>(extradata[pos]) << 8) | extradata[pos + 1];
    pos += 2;
    if (sps_len == 0 || pos + sps_len > extradata.size()) {
        return false;
    }
    if (sps) {
        sps->assign(extradata.begin() + static_cast<long>(pos),
                    extradata.begin() + static_cast<long>(pos + sps_len));
    }
    pos += sps_len;
    if (pos >= extradata.size()) {
        return false;
    }
    const size_t num_pps = extradata[pos];
    ++pos;
    if (num_pps == 0 || pos + 2 > extradata.size()) {
        return false;
    }
    const size_t pps_len = (static_cast<size_t>(extradata[pos]) << 8) | extradata[pos + 1];
    pos += 2;
    if (pps_len == 0 || pos + pps_len > extradata.size()) {
        return false;
    }
    if (pps) {
        pps->assign(extradata.begin() + static_cast<long>(pos),
                    extradata.begin() + static_cast<long>(pos + pps_len));
    }
    if (profile) {
        *profile = extradata[1];
    }
    if (compat) {
        *compat = extradata[2];
    }
    if (level) {
        *level = extradata[3];
    }
    if (length_size) {
        *length_size = static_cast<size_t>((extradata[4] & 0x03) + 1);
    }
    return true;
}

std::string SdpBuilder::build(const Streams &streams, const std::string &name, const std::string &ip,
                              std::string *error) {
    auto fail = [error](const char *why) {
        if (error != nullptr) {
            *error = why;
        }
        return std::string();
    };
    if (streams.video == nullptr) {
        return fail("RTSP 输出目前只支持带视频的源");
    }
    if (ip.empty()) {
        return fail("SDP 需要本机出口 IP（c= 行必填）");
    }

    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    uint8_t profile = 0;
    uint8_t compat = 0;
    uint8_t level = 0;
    size_t length_size = 0;
    const bool has_extradata = streams.video->extradata != nullptr &&
                               !streams.video->extradata->empty() &&
                               parseAvcC(*streams.video->extradata, &sps, &pps, &profile, &compat,
                                         &level, &length_size);
    if (!has_extradata) {
        return fail("视频没有 avcC（RTSP 输出需要 sprop-parameter-sets）");
    }

    std::string sdp;
    sdp += "v=0\r\n";
    sdp += "o=- 0 0 IN IP4 " + ip + "\r\n";
    sdp += "s=" + (name.empty() ? std::string("mzmedia") : name) + "\r\n";
    // RFC 4566 必填：会话级连接行（除媒体描述里各自写了 c=）
    sdp += "c=IN IP4 " + ip + "\r\n";
    sdp += "t=0 0\r\n";
    sdp += "a=control:*\r\n";
    sdp += "a=range:npt=0-\r\n";

    char profile_level[16];
    std::snprintf(profile_level, sizeof(profile_level), "%02X%02X%02X", profile, compat, level);
    sdp += "m=video 0 RTP/AVP 96\r\n";
    sdp += "a=rtpmap:96 H264/90000\r\n";
    sdp += "a=fmtp:96 packetization-mode=1;sprop-parameter-sets=" + base64Encode(sps.data(), sps.size()) +
           "," + base64Encode(pps.data(), pps.size()) + ";profile-level-id=" + profile_level + "\r\n";
    sdp += "a=control:trackID=0\r\n";

    if (streams.audio != nullptr && streams.audio->extradata != nullptr &&
        !streams.audio->extradata->empty()) {
        const auto &asc = *streams.audio->extradata;
        if (asc.size() == 2) {
            const int rate = streams.audio->sample_rate > 0 ? streams.audio->sample_rate : 44100;
            const int channels = streams.audio->channels > 0 ? streams.audio->channels : 1;
            char fmtp[160];
            std::snprintf(fmtp, sizeof(fmtp),
                          "a=fmtp:97 profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;"
                          "indexdeltalength=3;config=%s\r\n",
                          hexEncode(asc.data(), asc.size()).c_str());
            sdp += "m=audio 0 RTP/AVP 97\r\n";
            sdp += "a=rtpmap:97 MPEG4-GENERIC/" + std::to_string(rate) + "/" +
                   std::to_string(channels) + "\r\n";
            sdp += fmtp;
            sdp += "a=control:trackID=1\r\n";
        }
    }
    if (!sps.empty() && sps[0] != 0x67) {
        WarnL << "SdpBuilder：SPS 首字节不是 0x67（" << static_cast<int>(sps[0]) << "），照发但可能不被认";
    }
    return sdp;
}

} // namespace mzmedia
