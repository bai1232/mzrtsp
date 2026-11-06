/*
 * SdpBuilder：从 `StreamInfo` 生成 RTSP 的 SDP
 * ============================================================================
 * 为什么单独一层：SDP 是"协议文本"，而 `StreamInfo` 是"码流事实"。
 * 把它做成纯函数（输入 StreamInfo，输出字符串）就能**逐字节单测**，
 * 不需要起服务、不需要网络、也不需要真播放器。
 *
 * 两条容易漏的合规点（都被 VLC/NVR 挑剔过）：
 *   1) **必须有 `c=` 连接行**（RFC 4566：除非媒体描述里写了 c=，否则会话级必须有一条）；
 *      `ffplay` 宽容、VLC 更严、部分 NVR 直接拒。
 *   2) `m=` 的端口写 **0**：表示"用 RTSP 的 SETUP 协商出来的动态端口"，
 *      否则客户端会去连一个我们根本没监听的端口。
 *
 * 参数来源（都要能对上）：
 *   · 视频：avcC（`AVCDecoderConfigurationRecord`）里的 SPS/PPS/profile/compat/level
 *     → `sprop-parameter-sets`、`profile-level-id`
 *   · 音频：AAC 的 ASC（2 字节）→ `config=` 的十六进制
 * ============================================================================
 */

#pragma once

#include "ffmpeg/demuxer.h"

#include <string>

namespace mzmedia {

class SdpBuilder {
public:
    struct Streams {
        const StreamInfo *video = nullptr;
        const StreamInfo *audio = nullptr;
    };

    /**
     * 生成 SDP
     * @param name 会话名（`s=` 用，同时进 `o=` 的会话标识）
     * @param ip   本机出口 IP（必填：`c=IN IP4 <ip>`）
     * @param error 失败原因（缺 avcC/ASC、参数非法）
     * @return 空串 = 不可用
     */
    static std::string build(const Streams &streams, const std::string &name, const std::string &ip,
                             std::string *error);

    /// 从 avcC 里取 SPS/PPS（供 SDP 与单测共用）
    /// @return false = extradata 不是合法的 avcC
    static bool parseAvcC(const std::vector<uint8_t> &extradata, std::vector<uint8_t> *sps,
                          std::vector<uint8_t> *pps, uint8_t *profile, uint8_t *compat,
                          uint8_t *level, size_t *length_size);
};

} // namespace mzmedia
