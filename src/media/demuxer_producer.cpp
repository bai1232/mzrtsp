/*
 * DemuxerProducer 实现（v0.2.3 重构后只做编排）
 * ============================================================================
 * `read()` 的形状（唯一的循环）：
 *
 *   while (true) {
 *       session.read(&raw)                       // 读 + 跳过非音视频 + EOF/循环重开
 *       if (Eof) → 非循环：让视频链 flush（转码残余），再 EndOfStream
 *       current[0] = raw（原始时间戳）
 *       for (stage : chain) runStage(stage, &current)   // 0..N 逐级加工
 *       if (!chainOwnsTimeline) 给每个产物加趟偏移       // 转换类的时间轴接续
 *       _pending += current
 *       if (_pending 为空) continue                // 解码器攒帧：正常，继续读
 *       return servePending()                      // 一次只交一个包
 *   }
 * ============================================================================
 */

#include "media/demuxer_producer.h"

#include "core/logger.h"
#include "core/util.h"
#include "ffmpeg/h264_util.h"   // looksLikeAnnexB（选阶段时判"输入是不是 Annex-B"）
#include "media/stage_passthrough.h"
#include "media/stage_transcode.h"

#include <utility>

namespace mzmedia {

DemuxerProducer::DemuxerProducer(const Config &config)
    : _config(config)
    , _session(SourceSession::Config{config.demux, config.loop}) {}

// 无参构造与"带 Config"分开写：`Config` 自带默认成员初始值，
// 在类内把它当默认实参用会触发 [class.mem] 的"必须在类完成前初始化"限制（GCC 直接报错）。
DemuxerProducer::DemuxerProducer() : DemuxerProducer(Config{}) {}

void DemuxerProducer::setError(const std::string &message) {
    std::lock_guard<std::mutex> lock(_mutex);
    _last_error = message;
}

std::string DemuxerProducer::lastError() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _last_error;
}

bool DemuxerProducer::opened() const {
    return _session.opened();
}

const std::string &DemuxerProducer::path() const {
    return _session.path();
}

const Demuxer &DemuxerProducer::demuxer() const {
    return _session.demuxer();
}

const std::atomic<bool> *DemuxerProducer::setAbortFlag(const std::atomic<bool> *flag) {
    return _session.setAbortFlag(flag);
}

uint64_t DemuxerProducer::bitrateBps() const {
    const uint64_t opened = _session.openedAtMs();
    if (opened == 0) {
        return 0; // 还没打开：没有"码率"可言（不猜）
    }
    const int64_t elapsed_ms =
        static_cast<int64_t>(getCurrentMillisecond()) - static_cast<int64_t>(opened);
    if (elapsed_ms <= 0) {
        return 0; // 不足 1ms：返回 0，避免除零造出天文数字
    }
    return _bytes.load() * 8u * 1000u / static_cast<uint64_t>(elapsed_ms);
}

uint64_t DemuxerProducer::openedAtMs() const {
    return _session.openedAtMs();
}
uint64_t DemuxerProducer::skippedUnknownStreams() const {
    return _session.skippedUnknownStreams();
}
uint64_t DemuxerProducer::timestampFailures() const {
    return _session.timestampFailures();
}
uint64_t DemuxerProducer::annexBPackets() const {
    return _annexb_video && !_video_chain.empty() ? _video_chain.front()->convertedPackets() : 0;
}
uint64_t DemuxerProducer::adtsPackets() const {
    return _adts_audio && !_audio_chain.empty() ? _audio_chain.front()->convertedPackets() : 0;
}
uint64_t DemuxerProducer::ptsOutOfOrder() const {
    return _session.ptsOutOfOrder();
}
uint64_t DemuxerProducer::dtsClamped() const {
    return _session.dtsClamped();
}
uint64_t DemuxerProducer::loopCount() const {
    return _session.loopCount();
}
uint64_t DemuxerProducer::loopFailures() const {
    return _session.loopFailures();
}
int64_t DemuxerProducer::loopOffsetMs() const {
    return _session.loopOffsetMs();
}

// ---- 转码观测：转码阶段可能不存在 ----
bool DemuxerProducer::transcodeInputHevc() const {
    return _transcode != nullptr && _transcode->inputHevc();
}
int DemuxerProducer::transcodeOutWidth() const {
    return _transcode != nullptr ? _transcode->outWidth() : 0;
}
int DemuxerProducer::transcodeOutHeight() const {
    return _transcode != nullptr ? _transcode->outHeight() : 0;
}
uint64_t DemuxerProducer::transcodeInputs() const {
    return _transcode != nullptr ? _transcode->inputs() : 0;
}
uint64_t DemuxerProducer::transcodeOutputs() const {
    return _transcode != nullptr ? _transcode->outputs() : 0;
}
uint64_t DemuxerProducer::transcodeOutMediaMs() const {
    return _transcode != nullptr ? _transcode->outMediaMs() : 0;
}
int64_t DemuxerProducer::transcodeCostMs() const {
    return _transcode != nullptr ? _transcode->costMs() : 0;
}
int64_t DemuxerProducer::transcodeElapsedMs() const {
    return _transcode != nullptr ? _transcode->elapsedMs() : 0;
}
double DemuxerProducer::transcodeSpeed() const {
    return _transcode != nullptr ? _transcode->speed() : 0.0;
}
bool DemuxerProducer::transcodeVfr() const {
    return _transcode != nullptr && _transcode->vfr();
}
bool DemuxerProducer::transcodeScaling() const {
    return _transcode != nullptr && _transcode->scaling();
}
std::string DemuxerProducer::transcodeStatsJson() const {
    if (_transcode == nullptr) {
        return "\"enabled\":false,\"in_hevc\":false,\"out_width\":0,\"out_height\":0,"
               "\"scaling\":false,\"vfr\":false,\"inputs\":0,\"outputs\":0,"
               "\"out_media_ms\":0,\"cost_ms\":0,\"elapsed_ms\":0,\"speed\":0.00";
    }
    return _transcode->statsJson();
}

bool DemuxerProducer::chainOwnsTimeline(bool video) const {
    const std::vector<PacketStage::Ptr> &chain = video ? _video_chain : _audio_chain;
    for (const auto &stage : chain) {
        if (stage->ownsTimeline()) {
            return true;
        }
    }
    return false;
}

bool DemuxerProducer::appendTranscodeStage() {
    if (!_has_video_info) {
        setError("转码需要视频流，但这个源只有音频（v0.2 不做音频转码）→ 明确拒绝");
        return false;
    }
    // 音频必须是 AAC：FLV/TS 只能装 AAC，而 SPEC §9 明确"不做音频转码"。
    // 这一条**先判**：否则会白建一次编解码器、白转几帧才发现音频没法处理。
    if (_has_audio_info && _audio_info.codec_id != AV_CODEC_ID_AAC) {
        setError("音频编码是「" + _audio_info.codec_name +
                 "」，FLV 只认 AAC，而 v0.2 明确不做音频转码 → 明确拒绝（不静默丢音轨）");
        return false;
    }
    TranscodeStage::Config tc;
    tc.out_height = _config.transcode.out_height;
    tc.max_height = _config.transcode.max_height;
    tc.out_bitrate_kbps = _config.transcode.out_bitrate_kbps;
    tc.gop_frames = _config.transcode.gop_frames;
    tc.max_out_packets = _config.transcode.max_out_packets;

    auto stage = std::make_unique<TranscodeStage>(tc);
    StageContext ctx{_session.path(), _config.demux};
    StreamInfo out_info;
    // 用**当前视频链的输出形状**建转码器（Annex-B 已被上一步转成 AVCC）
    std::string error;
    if (!stage->prepare(_video_info, ctx, &out_info, &error)) {
        setError(error);
        return false;
    }
    _video_info = out_info; // 输出层从此看到转码后的宽高与 avcC
    _transcode = stage.get();
    _video_chain.push_back(std::move(stage));
    return true;
}

bool DemuxerProducer::buildChains() {
    _has_video_info = false;
    _has_audio_info = false;
    _annexb_video = false;
    _adts_audio = false;
    _video_info = StreamInfo{};
    _audio_info = StreamInfo{};
    _video_chain.clear();
    _audio_chain.clear();
    _transcode = nullptr;

    StageContext ctx{_session.path(), _config.demux};
    std::string error;

    // ---- 视频链：输入形状 → （可选）转码 ----
    if (const StreamInfo *src_video = _session.firstVideo()) {
        const bool need_annexb = src_video->codec_id == AV_CODEC_ID_H264 &&
                                 src_video->extradata && !src_video->extradata->empty() &&
                                 looksLikeAnnexB(src_video->extradata->data(),
                                                 src_video->extradata->size());
        PacketStage::Ptr stage = need_annexb ? PacketStage::Ptr(new AnnexBStage())
                                             : PacketStage::Ptr(new RawStage());
        StreamInfo out_info;
        if (!stage->prepare(*src_video, ctx, &out_info, &error)) {
            setError(error);
            ErrorL << "DemuxerProducer::open 失败：" << lastError();
            return false;
        }
        _video_info = out_info;
        _has_video_info = true;
        _annexb_video = need_annexb;
        _video_chain.push_back(std::move(stage));
    }

    // ---- 音频链：输入形状（ADTS）----
    if (const StreamInfo *src_audio = _session.firstAudio()) {
        const bool need_adts = src_audio->codec_id == AV_CODEC_ID_AAC &&
                               (!src_audio->extradata || src_audio->extradata->empty());
        PacketStage::Ptr stage =
            need_adts ? PacketStage::Ptr(new AdtsStage()) : PacketStage::Ptr(new RawStage());
        StreamInfo out_info;
        if (!stage->prepare(*src_audio, ctx, &out_info, &error)) {
            setError(error);
            ErrorL << "DemuxerProducer::open 失败：" << lastError();
            return false;
        }
        _audio_info = out_info;
        _has_audio_info = true;
        _adts_audio = need_adts;
        _audio_chain.push_back(std::move(stage));
    }

    // ---- 转码：追加到视频链尾（音频在这里已经就绪，才能做"音频必须是 AAC"的校验）----
    if (_config.transcode.enabled && !appendTranscodeStage()) {
        ErrorL << "DemuxerProducer::open 失败：" << lastError();
        return false;
    }
    return true;
}

bool DemuxerProducer::open(const std::string &path) {
    if (!_session.open(path)) {
        setError(_session.lastError()); // 不吞掉下层原因
        return false;
    }
    if (!buildChains()) {
        (void) _session.close(); // 不留半开的对象
        ErrorL << "DemuxerProducer::open 失败：" << lastError();
        return false;
    }
    // 循环接缝 → 转码阶段对齐时间轴（用回调：**读**不该认识**形状**）
    _session.setSeamCallback([this](int64_t pass_length_ms) {
        for (auto &stage : _video_chain) {
            stage->onSeam(pass_length_ms);
        }
    });
    return true;
}

bool DemuxerProducer::runStage(PacketStage &stage, std::vector<MediaPacket::Ptr> *current,
                               std::string *error) {
    std::vector<MediaPacket::Ptr> next;
    for (const auto &in : *current) {
        if (!in) {
            continue;
        }
        if (!stage.process(*in, &next, error)) {
            return false;
        }
    }
    *current = std::move(next);
    return true;
}

SourcePump::ReadResult DemuxerProducer::servePending(MediaPacket::Ptr *packet) {
    MediaPacket::Ptr front = std::move(_pending.front());
    _pending.pop_front();
    ++_packets;
    _bytes += front->size();
    *packet = std::move(front);
    return SourcePump::ReadResult::Packet;
}

SourcePump::ReadResult DemuxerProducer::read(MediaPacket::Ptr *packet, std::string *error) {
    if (packet == nullptr || error == nullptr) {
        setError("read 的出参为空（调用方 bug）");
        ErrorL << "DemuxerProducer::read 失败：" << lastError();
        return SourcePump::ReadResult::Error;
    }
    if (!_session.opened()) {
        setError("尚未 open 就读包");
        ErrorL << "DemuxerProducer::read 失败：" << lastError();
        return SourcePump::ReadResult::Error;
    }

    // 上一次加工吐出的包还没交完：先交（`SourcePump` 的契约是"一次一个包"）
    if (!_pending.empty()) {
        return servePending(packet);
    }

    while (true) {
        SourceSession::Packet raw;
        std::string read_error;
        const SourceSession::Result result = _session.read(&raw, &read_error);
        if (result == SourceSession::Result::Error) {
            setError(read_error); // 如实转达（含超时/超限/损坏/重开失败）
            return SourcePump::ReadResult::Error;
        }
        if (result == SourceSession::Result::EndOfStream) {
            // 非循环 EOF：让视频链把残余冲出来（**漏了就是少最后几帧且不报错**）。
            // 循环模式下不会走到这里（session 内部已经重开）。
            if (!_flushed) {
                _flushed = true;
                std::string flush_error;
                std::vector<MediaPacket::Ptr> flushed;
                for (auto &stage : _video_chain) {
                    std::vector<MediaPacket::Ptr> stage_out;
                    if (!stage->flush(&stage_out, &flush_error)) {
                        setError(flush_error);
                        return SourcePump::ReadResult::Error;
                    }
                    for (auto &pkt : stage_out) {
                        flushed.push_back(std::move(pkt));
                    }
                }
                for (auto &pkt : flushed) {
                    _pending.push_back(std::move(pkt));
                }
                if (!_pending.empty()) {
                    return servePending(packet);
                }
            }
            return SourcePump::ReadResult::EndOfStream;
        }

        const bool is_video = (raw.kind == MediaKind::Video);
        std::vector<PacketStage::Ptr> &chain = is_video ? _video_chain : _audio_chain;
        if (chain.empty()) {
            setError("这条流没有对应的加工阶段（调用方 bug / 流快照不一致）");
            ErrorL << "DemuxerProducer::read 失败：" << lastError();
            return SourcePump::ReadResult::Error;
        }

        // 原始包（**零拷贝**：session 已把字节拷进共享缓冲区，这里只是包一层）
        MediaPacket::Ptr in = MediaPacket::create(raw.kind, raw.stream_index, raw.payload, raw.key,
                                                  raw.raw_dts_ms, raw.raw_pts_ms);
        if (!in) {
            setError("MediaPacket::create 失败（参数非法）");
            ErrorL << "DemuxerProducer::read 失败：" << lastError();
            return SourcePump::ReadResult::Error;
        }
        std::vector<MediaPacket::Ptr> current;
        current.push_back(std::move(in));
        std::string stage_error;
        for (const auto &stage : chain) {
            if (!runStage(*stage, &current, &stage_error)) {
                setError(stage_error);
                ErrorL << "DemuxerProducer::read 失败：" << lastError();
                return SourcePump::ReadResult::Error;
            }
            if (current.empty()) {
                break; // 该阶段暂时没有产物（转码攒帧）：不必再往下走
            }
        }

        // 链尾统一加趟偏移 —— **除非**链里有阶段自己拥有时间轴（转码）
        if (!current.empty() && !chainOwnsTimeline(is_video)) {
            const int64_t offset = _session.loopOffsetMs();
            if (offset != 0) {
                std::vector<MediaPacket::Ptr> shifted;
                shifted.reserve(current.size());
                for (const auto &pkt : current) {
                    auto moved = MediaPacket::create(pkt->kind(), pkt->streamIndex(), pkt->payload(),
                                                     pkt->isKeyFrame(), pkt->dtsMs() + offset,
                                                     pkt->ptsMs() + offset);
                    if (!moved) {
                        setError("加趟偏移后构造 MediaPacket 失败（参数非法）");
                        ErrorL << "DemuxerProducer::read 失败：" << lastError();
                        return SourcePump::ReadResult::Error;
                    }
                    shifted.push_back(std::move(moved));
                }
                current = std::move(shifted);
            }
        }

        for (auto &pkt : current) {
            _pending.push_back(std::move(pkt));
        }
        if (_pending.empty()) {
            continue; // 编解码器还在攒帧（正常现象，不是错误）
        }
        return servePending(packet);
    }
}

} // namespace mzmedia
