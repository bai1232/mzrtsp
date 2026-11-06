/*
 * RtspTiming：RTP 时间戳与 RTP-Info 文本（**纯函数，可逐字节单测**）
 * ============================================================================
 * 抽出来的理由：这两块最容易写错、又最容易被面试追问，而它们本质是纯计算：
 *   · **视频**：RTP ts = 呈现时间（PTS）× 90 kHz（**不是 dts**：B 帧下 dts 会毁掉显示顺序）；
 *   · **音频**：RTP ts 以**采样数**为单位（`pts_ms × sample_rate / 1000`），
 *     不要"每帧 +1024"那种累加（两条口径同时存在就会漂）。
 *   · 起始偏移（90000 / 采样率）：RFC 3550 要求初始值随机；这里用固定偏移 1 秒，
 *     另一个作用是**避开 ts=0**：ffmpeg 的 RTSP/TS 解复用器对 ts=0 会走"33 位回绕"分支。
 * ============================================================================
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mzmedia {

class RtspTiming {
public:
    static constexpr uint32_t kVideoStartOffset90k = 90000; // 1 秒

    /// 视频：PTS(ms) → 90 kHz；负值钳 0（调用方应计数）
    static uint32_t videoRtpTs(int64_t pts_ms);

    /// 音频：PTS(ms) → 采样数；负值钳 0
    static uint32_t audioRtpTs(int64_t pts_ms, int sample_rate);

    /// 音频的起始偏移（取 1 秒的采样数，避开 ts=0）
    static uint32_t audioStartOffset(int sample_rate) {
        return static_cast<uint32_t>(sample_rate > 0 ? sample_rate : 44100);
    }

    /// 一条轨的 RTP-Info 条目
    struct TrackInfo {
        int track_id = 0;   ///< 0 = video，1 = audio
        uint16_t seq = 0;   ///< 该轨首个 RTP 包的 seq
        uint32_t rtptime = 0; ///< 该轨首个 RTP 包的 ts
    };

    /// `RTP-Info: url=<base>/trackID=0;seq=1;rtptime=2,url=<base>/trackID=1;…`
    /// @param base 形如 `rtsp://host:port/live/<name>`
    static std::string buildRtpInfo(const std::string &base, const std::vector<TrackInfo> &tracks);
};

} // namespace mzmedia
