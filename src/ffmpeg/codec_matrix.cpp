/*
 * CodecMatrix 实现（M4-b；M10-b 加"转码能力"分支）
 * ============================================================================
 * v0.1 只做 HTTP-FLV，所以判定就围绕"FLV 容器 + 浏览器能不能播"：
 *   - 视频：H264 ✅；无视频 ✅（纯音频也合法）；HEVC ❌（无转码能力时，SPEC 明确不支持）；
 *           其它 → 需要转码
 *   - 音频：AAC ✅；无音频 ✅；其它（mp3/ac3…）→ 需要转码（v0.2 仍**不做**音频转码）
 *   - 输出：只认 "flv"；其它格式 → Unsupported（v0.2 的 HLS 复用 TS 封装，不走这里）
 *
 * 【M10-b】HEVC 的那一条现在取决于**本机能力**：有 HEVC 解码器 + H264 编码器时可以转码，
 * 否则仍是"明确不支持"。这是**同一张判定表的运行时版本**，不是放宽标准 ——
 * 没有能力时依然拒绝，且日志说清缺什么。
 * ============================================================================
 */

#include "ffmpeg/codec_matrix.h"

#include "core/logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace mzmedia {

namespace {

/// 判定表的共用一个实现：`caps` 是"本机能做什么"
CopyOrTranscode decide(AVCodecID video, AVCodecID audio, const std::string &output,
                       const TranscodeCaps &caps) {
    if (output != "flv") {
        // 明确不支持，而不是"当作 flv 处理"（那会产出一份播不了的流）
        ErrorP("CodecMatrix: 输出格式 %s 尚未实现（v0.1 只做 flv）", output.c_str());
        return CopyOrTranscode::Unsupported;
    }
    if (video != AV_CODEC_ID_NONE && video != AV_CODEC_ID_H264) {
        if (video == AV_CODEC_ID_HEVC && !caps.hevc_to_h264()) {
            // HTTP-FLV 的播放端（flv.js/MSE）不支持 HEVC → 只有转成 H264 才可能播
            ErrorP("CodecMatrix: HEVC → FLV 不支持（本机转码能力：HEVC 解码器=%s，H264 编码器=%s；"
                   "SPEC 的「不做 HEVC」针对的是**不转码**的直通路径）",
                   caps.hevc_decode ? "有" : "无", caps.h264_encode ? "有" : "无");
            return CopyOrTranscode::Unsupported;
        }
        // H264 以外的其它编码，或"HEVC + 有转码能力" → 都要解码重编
        return CopyOrTranscode::Transcode;
    }
    if (audio != AV_CODEC_ID_NONE && audio != AV_CODEC_ID_AAC) {
        // v0.2 明确不做音频转码：判定为 Transcode，调用方据此**明确拒绝**（不静默丢音轨）
        return CopyOrTranscode::Transcode;
    }
    return CopyOrTranscode::Remux;
}

} // namespace

TranscodeCaps detectTranscodeCaps() {
    // 只探一次（avcodec 的注册表是进程级只读的）；用函数内 static 保证线程安全
    static const TranscodeCaps caps = [] {
        TranscodeCaps result;
        result.hevc_decode = avcodec_find_decoder(AV_CODEC_ID_HEVC) != nullptr;
        result.h264_encode = avcodec_find_encoder(AV_CODEC_ID_H264) != nullptr;
        InfoL << "CodecMatrix：本机转码能力 HEVC 解码器=" << (result.hevc_decode ? "有" : "无")
              << " H264 编码器=" << (result.h264_encode ? "有" : "无");
        return result;
    }();
    return caps;
}

CopyOrTranscode decideOutput(AVCodecID video, AVCodecID audio, const std::string &output) {
    // 不知道能力 = 当作没有能力（M4-b 的既有行为，用例锁的就是这一条）
    return decide(video, audio, output, TranscodeCaps{});
}

CopyOrTranscode decideOutput(AVCodecID video, AVCodecID audio, const std::string &output,
                             const TranscodeCaps &caps) {
    return decide(video, audio, output, caps);
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
