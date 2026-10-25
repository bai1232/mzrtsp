/*
 * CodecMatrix：判定"该输入 × 该输出"能不能直接 remux（M4-b；M10-b 加"转码能力"分支）
 * ============================================================================
 * 形状来源：docs/DESIGN_M4.md §4；判定表对应 docs/CODEC_MATRIX.md；
 *           M10-b 的转码分支见 docs/DESIGN_M10.md §8
 *
 * 为什么把判定单独抽出来：**假成功是最贵的失败** —— 如果对 HEVC 输入"照 remux 不误"，
 * 结果是浏览器一片黑，排查成本极高。这里宁可返回 Unsupported 并记日志。
 *
 * 【M10-b】"能不能转码"不是一个**静态**事实，而是"本机 FFmpeg 有没有这个解码器/编码器"。
 * 所以判定分成两层：
 *   · `decideOutput(video, audio, output)`（M4-b 的形状，**行为不变**）：假设没有任何转码能力；
 *   · `decideOutput(video, audio, output, caps)`：调用方把 `detectTranscodeCaps()` 的结果传进来，
 *     于是 HEVC 在"有 HEVC 解码器 + 有 H264 编码器"的机器上可以是 `Transcode`。
 * 保留两层的理由：既有用例（`ffmpeg_codec_matrix_unsupported_and_transcode`）锁的是"无转码能力"
 * 时的行为，那条必须继续是 Unsupported —— 否则等于悄悄放宽了"不做 HEVC"的旧约定。
 * ============================================================================
 */

#pragma once

#include <string>

extern "C" {
#include <libavcodec/codec_id.h>
}

namespace mzmedia {

enum class CopyOrTranscode {
    Remux,        // 可直接转封装（不解码）
    Transcode,    // 必须转码（转码关闭时明确拒绝，绝不"照 remux"）
    Unsupported,  // 明确不支持（如缺解码器/编码器、输出格式未实现）
};

/// 【M10-b】本机的转码能力（由 FFmpeg 实际具备的编解码器决定，不靠编译期假设）
struct TranscodeCaps {
    bool hevc_decode = false; ///< 有 HEVC 解码器（能解 HEVC 源）
    bool h264_encode = false; ///< 有 H264 编码器（能产出 FLV/HLS 要的 H264）
    /// 两者都具备 = 能把 HEVC 源转成 H264
    bool hevc_to_h264() const {
        return hevc_decode && h264_encode;
    }
};

/**
 * 探测本机能力（`avcodec_find_decoder/find_encoder`，缓存一次）
 * @note 只探"能不能找到"，不建上下文 —— 判定要便宜，真正的失败留给 `Transcoder::create()`
 */
TranscodeCaps detectTranscodeCaps();

/**
 * 判定输出策略（**不知道转码能力**的版本：等价于"没有转码能力"，M4-b 行为）
 * @param video 视频流 codec（无视频传 AV_CODEC_ID_NONE）
 * @param audio 音频流 codec（无音频传 AV_CODEC_ID_NONE）
 * @param output 输出格式（v0.1 只实现 "flv"；HLS 走的是同一个 flv/ts 判定）
 * @note 不认识的组合一律走"明确不支持"，绝不默认 Remux
 */
CopyOrTranscode decideOutput(AVCodecID video, AVCodecID audio, const std::string &output);

/**
 * 判定输出策略（**知道转码能力**的版本，M10-b）
 * @note 与上面的区别只有一处：HEVC 视频在 `caps.hevc_to_h264()` 时为 `Transcode`，
 *       否则仍是 `Unsupported`（此时日志里会说清"缺哪个能力"）
 */
CopyOrTranscode decideOutput(AVCodecID video, AVCodecID audio, const std::string &output,
                             const TranscodeCaps &caps);

/// 日志/JSON 用的名字
const char *copyOrTranscodeName(CopyOrTranscode value);

} // namespace mzmedia
