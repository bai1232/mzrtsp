/*
 * CodecMatrix：判定"该输入 × 该输出"能不能直接 remux（M4-b）
 * ============================================================================
 * 形状来源：docs/DESIGN_M4.md §4；判定表对应 docs/CODEC_MATRIX.md
 *
 * 为什么把判定单独抽出来：**假成功是最贵的失败** —— 如果对 HEVC 输入"照 remux 不误"，
 * 结果是浏览器一片黑，排查成本极高。这里宁可返回 Unsupported 并记日志。
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
    Transcode,    // 必须转码（v0.2 才有能力，现在会明确拒绝/降级）
    Unsupported,  // 明确不支持（如 HEVC → FLV：浏览器播不了，SPEC 已冻结"不做"）
};

/**
 * 判定输出策略
 * @param video 视频流 codec（无视频传 AV_CODEC_ID_NONE）
 * @param audio 音频流 codec（无音频传 AV_CODEC_ID_NONE）
 * @param output 输出格式（v0.1 只实现 "flv"；其它 → Unsupported + 日志）
 * @note 不认识的组合一律走"明确不支持"，绝不默认 Remux
 */
CopyOrTranscode decideOutput(AVCodecID video, AVCodecID audio, const std::string &output);

/// 日志/JSON 用的名字
const char *copyOrTranscodeName(CopyOrTranscode value);

} // namespace mzmedia
