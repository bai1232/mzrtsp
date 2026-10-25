/*
 * TranscodeStage 实现（v0.2.3 重构抽出）
 * ============================================================================
 * 逐字搬自 v0.2.2 的 `DemuxerProducer` 转码段，只把状态从生产者挪进来。
 * 唯一的结构性变化：`process()` 与 `flush()` 各自把产物转成 MediaPacket（转码产物类型是
 * `Transcoder::OutPacket`，与 MediaPacket 不同，无法共用一段）。
 * ============================================================================
 */

#include "media/stage_transcode.h"

#include "core/logger.h"
#include "core/util.h"
#include "ffmpeg/codec_matrix.h"
#include "ffmpeg/transcoder.h"

#include <cmath>
#include <cstdio>
#include <utility>

namespace mzmedia {

namespace {

/// 定点两位小数（JSON 数字）
std::string jsonFixed2(double value) {
    char buffer[32] = {0};
    std::snprintf(buffer, sizeof(buffer), "%.2f", value);
    return std::string(buffer);
}

} // namespace

TranscodeStage::TranscodeStage(const Config &config)
    : _config(config) {}

// 必须写在这里（`Transcoder` 是不完整类型；头文件里只放前置声明）
TranscodeStage::~TranscodeStage() = default;

bool TranscodeStage::prepare(const StreamInfo &src, const StageContext &ctx, StreamInfo *out,
                             std::string *error) {
    (void) ctx;
    _stream_index = src.index;
    _in_hevc = (src.codec_id == AV_CODEC_ID_HEVC);
    *out = src;

    // 音频必须是 AAC：FLV/TS 只能装 AAC，而 SPEC §9 明确"不做音频转码"。
    // 这一条由调用方（DemuxerProducer）在装阶段前判过；这里不再重复。

    // 判定交给 CodecMatrix（单一职责）：**能力感知**的版本 —— HEVC 在有解码器+编码器时可转
    const TranscodeCaps caps = detectTranscodeCaps();
    const CopyOrTranscode verdict = decideOutput(src.codec_id, AV_CODEC_ID_NONE, "flv", caps);
    if (verdict == CopyOrTranscode::Unsupported) {
        *error = "转码被拒：CodecMatrix 判定「" + src.codec_name +
                 "」→ FLV 不可行（原因见日志：缺解码器/编码器）";
        return false;
    }

    Transcoder::Config tc;
    tc.out_bitrate_kbps = _config.out_bitrate_kbps;
    tc.gop_frames = _config.gop_frames;
    tc.max_out_packets = _config.max_out_packets;

    // 目标高度有三种来源，优先级从高到低：
    //   1) `out_height`（URL 的 h=）：用户明确要的高度 —— 与源一样 = 不缩放，比源高 = 拒绝；
    //   2) `max_height`（--transcode-height）：只当**上限**用 —— 源高于它才缩；
    //   3) 都没有：不缩放（只重编码）。
    int want_height = _config.out_height;
    const bool explicit_height = want_height > 0;
    if (!explicit_height && _config.max_height > 0 && src.height > 0 &&
        _config.max_height < src.height) {
        want_height = _config.max_height;
    }
    if (want_height < 0 || (want_height > 0 && (want_height & 1) != 0)) {
        *error = "目标高度非法：" + std::to_string(want_height) +
                 "（0 = 不缩放；否则必须是**偶数**正数 —— yuv420p 要求）";
        return false;
    }
    if (want_height > 0) {
        if (src.width <= 0 || src.height <= 0) {
            *error = "拿不到源分辨率（" + std::to_string(src.width) + "x" +
                     std::to_string(src.height) + "）→ 无法等比缩放，明确拒绝";
            return false;
        }
        if (want_height > src.height && explicit_height) {
            // 放大既浪费 CPU 又不会更清楚：这类"参数写错了"要当场拒，不能默默照做
            *error = "目标高度 " + std::to_string(want_height) + " 大于源高度 " +
                     std::to_string(src.height) + " → 明确拒绝（本服务不放大）";
            return false;
        }
        if (want_height != src.height) {
            tc.out_height = want_height;
            // 等比算宽（奇数宽由 Transcoder 取偶并 Warn）—— 用 llround 避免截断成 0
            tc.out_width = static_cast<int>(std::llround(
                static_cast<double>(src.width) * static_cast<double>(want_height) /
                static_cast<double>(src.height)));
        }
        // want_height == 源高：out_width/out_height 保持 0 = 不缩放（只重编码）
    }

    Transcoder::Ptr built = Transcoder::create(src, tc);
    if (!built) {
        *error = "转码器建不起来（输入「" + src.codec_name + "」 " + std::to_string(src.width) +
                 "x" + std::to_string(src.height) +
                 "；原因见日志：缺编解码器 / 初始化数据不可用 / 参数非法）";
        return false;
    }
    _transcoder = built;
    _out_width = built->videoInfo().width;
    _out_height = built->videoInfo().height;
    // 输出按 CFR 生成（时间轴 = 锚点 + 帧序号 × 步长），步长取自输出帧率
    const TimeBase out_fps = built->videoInfo().frame_rate;
    if (out_fps.num > 0 && out_fps.den > 0) {
        _frame_step_ms =
            1000.0 * static_cast<double>(out_fps.den) / static_cast<double>(out_fps.num);
    }
    // ★ 关键：把输出流信息换成**转码后的**形状（宽高 + 现场构造的 avcC）。
    //   输出层从此看到的就是"一路 H264"，完全不知道它原来是 HEVC 或原来是 1080p。
    *out = built->videoInfo();
    out->index = src.index;   // 保持同一个 stream index（MediaPacket 靠它区分流）
    InfoP("TranscodeStage: 已启用 —— %s %dx%d → h264 %dx%d（out_height=%d）",
          _in_hevc ? "hevc" : "h264", src.width, src.height, _out_width, _out_height, want_height);
    return true;
}

bool TranscodeStage::process(const MediaPacket &in, std::vector<MediaPacket::Ptr> *out,
                             std::string *error) {
    if (!_transcoder) {
        *error = "转码器没准备好（调用方 bug）";
        return false;
    }
    std::vector<Transcoder::OutPacket> outs;
    // 只把**转码本身**的耗时计入 cost（不含解封装/节流/网络）：NFR-5 的 speed 分母
    const uint64_t cost_begin_us = getCurrentMicrosecond();
    const Transcoder::Result result =
        _transcoder->transcode(in.data(), in.size(), in.dtsMs(), in.ptsMs(), &outs);
    _cost_us += static_cast<int64_t>(getCurrentMicrosecond() - cost_begin_us);
    ++_inputs;
    if (_first_ms == 0) {
        _first_ms = static_cast<int64_t>(getCurrentMillisecond()); // speed 的墙钟基准
    }
    if (result != Transcoder::Result::Ok) {
        *error = std::string("转码失败（") + Transcoder::resultName(result) + "）：" +
                 _transcoder->lastError();
        return false;
    }
    _vfr = _transcoder->vfrDetected();
    _scaling = _transcoder->scaling();
    // 产物 → MediaPacket：转码器自造时间轴 + 接缝修正（见 onSeam）
    for (auto &item : outs) {
        const int64_t out_dts = item.dts_ms + _ts_shift;
        const int64_t out_pts = item.pts_ms + _ts_shift;
        MediaPacket::Ptr built =
            MediaPacket::create(MediaKind::Video, in.streamIndex(), std::move(item.data), item.key,
                                out_dts, out_pts);
        if (!built) {
            *error = "转码产物构造 MediaPacket 失败（时间戳/载荷非法）";
            return false;
        }
        ++_frames_out;
        // 媒体时长 = 帧数 × 标称帧步长（CFR 输出；用 dts 跨度会被接缝跳变虚高）
        _out_media_ms = static_cast<uint64_t>(
            std::llround(static_cast<double>(_frames_out) * _frame_step_ms));
        ++_outputs;
        out->push_back(std::move(built));
    }
    return true;
}

bool TranscodeStage::flush(std::vector<MediaPacket::Ptr> *out, std::string *error) {
    if (!_transcoder) {
        return true;
    }
    if (_tx_flushed) {
        return true; // 幂等
    }
    std::vector<Transcoder::OutPacket> outs;
    const uint64_t cost_begin_us = getCurrentMicrosecond();
    const Transcoder::Result result = _transcoder->flush(&outs);
    _cost_us += static_cast<int64_t>(getCurrentMicrosecond() - cost_begin_us);
    if (result != Transcoder::Result::Ok) {
        *error = std::string("转码收尾失败（") + Transcoder::resultName(result) + "）：" +
                 _transcoder->lastError();
        return false;
    }
    _tx_flushed = true;
    for (auto &item : outs) {
        const int64_t out_dts = item.dts_ms + _ts_shift;
        const int64_t out_pts = item.pts_ms + _ts_shift;
        MediaPacket::Ptr built = MediaPacket::create(MediaKind::Video, _stream_index,
                                                     std::move(item.data), item.key, out_dts,
                                                     out_pts);
        if (!built) {
            *error = "转码残余构造 MediaPacket 失败（时间戳/载荷非法）";
            return false;
        }
        ++_frames_out;
        _out_media_ms = static_cast<uint64_t>(
            std::llround(static_cast<double>(_frames_out) * _frame_step_ms));
        ++_outputs;
        out->push_back(std::move(built));
    }
    // 转码到这里就结束了：冻结墙钟（否则源播完后 speed 会一路衰减成假数字）
    if (_end_ms == 0) {
        _end_ms = static_cast<int64_t>(getCurrentMillisecond());
    }
    return true;
}

void TranscodeStage::onSeam(int64_t pass_length_ms) {
    if (!_transcoder) {
        return;
    }
    const int64_t frames_this_pass =
        static_cast<int64_t>(_frames_out - _frames_at_pass_start);
    const int64_t video_pass_ms = static_cast<int64_t>(
        std::llround(static_cast<double>(frames_this_pass) * _frame_step_ms));
    // shift += 本趟真实长度 − 本趟转出的媒体时长（**增量**：帧序号全局连续，
    // 按"绝对位置"算会把前几趟的偏移重复扣掉 —— M10-b 踩过）
    _ts_shift += pass_length_ms - video_pass_ms;
    _frames_at_pass_start = _frames_out;
    InfoP("TranscodeStage: 接缝对齐 —— 本趟 %lld 帧（%lld ms）vs 真实 %lld ms，"
          "修正后累计 %+lld ms",
          static_cast<long long>(frames_this_pass), static_cast<long long>(video_pass_ms),
          static_cast<long long>(pass_length_ms), static_cast<long long>(_ts_shift));
}

std::string TranscodeStage::statsJson() const {
    std::string out = "\"enabled\":true";
    out += ",\"in_hevc\":" + std::string(_in_hevc ? "true" : "false");
    out += ",\"out_width\":" + std::to_string(_out_width);
    out += ",\"out_height\":" + std::to_string(_out_height);
    out += ",\"scaling\":" + std::string(_scaling ? "true" : "false");
    out += ",\"vfr\":" + std::string(_vfr ? "true" : "false");
    out += ",\"inputs\":" + std::to_string(_inputs);
    out += ",\"outputs\":" + std::to_string(_outputs);
    out += ",\"out_media_ms\":" + std::to_string(_out_media_ms);
    out += ",\"cost_ms\":" + std::to_string(costMs());
    out += ",\"elapsed_ms\":" + std::to_string(elapsedMs());
    out += ",\"speed\":" + jsonFixed2(speed());
    return out;
}

int TranscodeStage::outWidth() const {
    return _out_width;
}

int TranscodeStage::outHeight() const {
    return _out_height;
}

int64_t TranscodeStage::costMs() const {
    return _cost_us > 0 ? _cost_us / 1000 : 0;
}

int64_t TranscodeStage::elapsedMs() const {
    if (_first_ms <= 0) {
        return 0;
    }
    const int64_t end = (_end_ms > 0) ? _end_ms : static_cast<int64_t>(getCurrentMillisecond());
    const int64_t elapsed = end - _first_ms;
    return elapsed > 0 ? elapsed : 0;
}

double TranscodeStage::speed() const {
    const int64_t cost = costMs();
    if (cost <= 0) {
        return 0.0;
    }
    return static_cast<double>(_out_media_ms) / static_cast<double>(cost);
}

bool TranscodeStage::vfr() const {
    return _vfr;
}

bool TranscodeStage::scaling() const {
    return _scaling;
}

} // namespace mzmedia
