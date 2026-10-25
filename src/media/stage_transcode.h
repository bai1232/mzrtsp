/*
 * TranscodeStage：转码（解码 → 缩放 → 编码），时间轴**自造**（v0.2.3 重构抽出）
 * ============================================================================
 * 逻辑逐字搬自 v0.2.2 的 `DemuxerProducer::openTranscoder/transcodeVideo/enqueueTranscoded/
 * flushTranscoder/servePending`，只把"状态"从生产者挪到了本阶段。
 *
 * 三条必须记住的契约（M10-a/M10-b 踩出来的）：
 *   1) **一进 0..N 出**：解码器攒帧、编码器有延迟 → `process()` 返回 0 个包是**正常**的；
 *   2) **EOF 必须 flush**：不冲残余就是"少最后几帧且不报错"；循环模式下**不** flush；
 *   3) **时间轴自造**：输出 = 锚点（源首包 dts）+ 帧序号 × 步长 + 接缝修正。
 *      不用源 pts（被 `Demuxer` 的 dts 守卫/乱序影响），也不用 `loop_offset_ms`
 *      （转码器的帧序号跨趟连续，再叠趟偏移会双计 —— v0.2.1 实测视频跑到 14s、音频 8s）。
 * ============================================================================
 */

#pragma once

#include "media/packet_stage.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mzmedia {

class Transcoder;

class TranscodeStage final : public PacketStage {
public:
    struct Config {
        int out_height = 0;        ///< 显式目标高度（0 = 没给；给了必须偶数且不高过源）
        int max_height = 0;        ///< 上限高度（源高于它才缩）
        int out_bitrate_kbps = 0;  ///< 0 = 推导
        int gop_frames = 50;
        int max_out_packets = 64;
    };

    explicit TranscodeStage(const Config &config);
    ~TranscodeStage() override;

    const char *name() const override {
        return "transcode";
    }
    /// 建转码器（可能失败：编码不支持 / 参数非法 / 本机缺编解码器）
    bool prepare(const StreamInfo &src, const StageContext &ctx, StreamInfo *out,
                 std::string *error) override;
    bool process(const MediaPacket &in, std::vector<MediaPacket::Ptr> *out,
                 std::string *error) override;
    /// 转码自己生成时间轴：生产者**不许**再加趟偏移（加了会双计）
    bool ownsTimeline() const override {
        return true;
    }
    /// 冲编解码器残余（**非循环**的 EOF 才调）
    bool flush(std::vector<MediaPacket::Ptr> *out, std::string *error) override;
    /// 循环接缝：把"本趟真实长度"与"本趟转出时长"的差额补进时间轴
    void onSeam(int64_t pass_length_ms) override;
    std::string statsJson() const override;

    // 观测（供 `DemuxerProducer` 的老接口转发；也可直接读 statsJson）
    int outWidth() const;
    int outHeight() const;
    bool inputHevc() const {
        return _in_hevc;
    }
    uint64_t inputs() const {
        return _inputs;
    }
    uint64_t outputs() const {
        return _outputs;
    }
    uint64_t outMediaMs() const {
        return _out_media_ms;
    }
    int64_t costMs() const;
    int64_t elapsedMs() const;
    double speed() const;
    bool vfr() const;
    bool scaling() const;

private:
    Config _config;
    std::shared_ptr<Transcoder> _transcoder;
    int _out_width = 0;
    int _out_height = 0;
    int _stream_index = -1;
    bool _in_hevc = false;
    bool _tx_flushed = false;

    uint64_t _inputs = 0;
    uint64_t _outputs = 0;
    uint64_t _out_media_ms = 0;
    uint64_t _frames_out = 0;
    double _frame_step_ms = 40.0;
    uint64_t _frames_at_pass_start = 0;
    int64_t _ts_shift = 0;
    int64_t _first_ms = 0;
    int64_t _end_ms = 0;
    int64_t _cost_us = 0;
    bool _vfr = false;
    bool _scaling = false;
};

} // namespace mzmedia
