/*
 * CodecMatrix 实现（M4-b）
 * ============================================================================
 * v0.1 只做 HTTP-FLV，所以判定就围绕"FLV 容器 + 浏览器能不能播"：
 *   - 视频：H264 ✅；无视频 ✅（纯音频也合法）；HEVC ❌（SPEC 明确不做）；其它 → 需要转码
 *   - 音频：AAC ✅；无音频 ✅；其它（mp3/ac3…）→ 需要转码
 *   - 输出：只认 "flv"；其它格式 → Unsupported（v0.2 的 HLS 再补）
 * ============================================================================
 */

#include "ffmpeg/codec_matrix.h"

#include "core/logger.h"

namespace mzmedia {

CopyOrTranscode decideOutput(AVCodecID video, AVCodecID audio, const std::string &output) {
    if (output != "flv") {
        // 明确不支持，而不是"当作 flv 处理"（那会产出一份播不了的流）
        ErrorP("CodecMatrix: 输出格式 %s 尚未实现（v0.1 只做 flv）", output.c_str());
        return CopyOrTranscode::Unsupported;
    }
    if (video != AV_CODEC_ID_NONE && video != AV_CODEC_ID_H264) {
        if (video == AV_CODEC_ID_HEVC) {
            // HTTP-FLV 的播放端（flv.js/MSE）不支持 HEVC → 这不是"转码能解决"的事
            ErrorP("CodecMatrix: HEVC → FLV 明确不支持（浏览器播不了，SPEC 已冻结）");
            return CopyOrTranscode::Unsupported;
        }
        return CopyOrTranscode::Transcode;
    }
    if (audio != AV_CODEC_ID_NONE && audio != AV_CODEC_ID_AAC) {
        return CopyOrTranscode::Transcode;
    }
    return CopyOrTranscode::Remux;
}

const char *copyOrTranscodeName(CopyOrTranscode value) {
    switch (value) {
    case CopyOrTranscode::Remux: return "remux";
    case CopyOrTranscode::Transcode: return "transcode";
    case CopyOrTranscode::Unsupported: return "unsupported";
    }
    return "unknown";
}

} // namespace mzmedia
