#include "output/rtsp_timing.h"

namespace mzmedia {

uint32_t RtspTiming::videoRtpTs(int64_t pts_ms) {
    if (pts_ms < 0) {
        pts_ms = 0;
    }
    const uint64_t ticks = kVideoStartOffset90k + static_cast<uint64_t>(pts_ms) * 90u;
    return static_cast<uint32_t>(ticks & 0xFFFFFFFFu);
}

uint32_t RtspTiming::audioRtpTs(int64_t pts_ms, int sample_rate) {
    if (pts_ms < 0) {
        pts_ms = 0;
    }
    const uint32_t rate = static_cast<uint32_t>(sample_rate > 0 ? sample_rate : 44100);
    const uint64_t samples = static_cast<uint64_t>(pts_ms) * rate / 1000u;
    return static_cast<uint32_t>((audioStartOffset(sample_rate) + samples) & 0xFFFFFFFFu);
}

std::string RtspTiming::buildRtpInfo(const std::string &base, const std::vector<TrackInfo> &tracks) {
    std::string out = "RTP-Info: ";
    bool first = true;
    for (const auto &track : tracks) {
        if (!first) {
            out += ",";
        }
        first = false;
        out += "url=" + base + "/trackID=" + std::to_string(track.track_id);
        out += ";seq=" + std::to_string(track.seq);
        out += ";rtptime=" + std::to_string(track.rtptime);
    }
    out += "\r\n";
    return out;
}

} // namespace mzmedia
