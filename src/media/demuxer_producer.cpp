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

namespace {

/// JSON 字符串转义（`last_error` 里可能有引号/反斜杠/换行 —— 不能让 stats 变成非法 JSON）
std::string jsonEscapeJsonString(const std::string &in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

} // namespace

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
std::string DemuxerProducer::rtspStatsJson() const {
    if (!_network) {
        return "null"; // 本地文件源：没有这个字段的内容（不制造假数据）
    }
    // 【M11-a2】状态由 `SourceSession` 维护（idle/playing/reconnecting/failed + 计数）
    const int64_t age = _session.lastPacketAgeMs();
    std::string out = "{\"enabled\":true,\"state\":\"";
    out += SourceSession::netStateName(_session.netState());
    out += "\",\"url\":\"" + redactMediaUrl(_session.path()) + "\"";
    out += ",\"connects\":" + std::to_string(_session.connects());
    out += ",\"reconnects\":" + std::to_string(_session.reconnects());
    out += ",\"attempts\":" + std::to_string(_session.reconnectAttempts());
    out += ",\"backoff_ms\":" + std::to_string(_session.nextBackoffMs());
    out += ",\"last_packet_age_ms\":" + std::to_string(age);
    const std::string net_error = _session.lastNetError();
    if (!net_error.empty()) {
        out += ",\"last_error\":\"" + jsonEscapeJsonString(net_error) + "\"";
    }
    out += "}";
    return out;
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
    // 【M11-a】网络源（rtsp://）：开关、超时、传输方式都在这里生效
    _network = isNetworkMediaUrl(path);
    Demuxer::Options options;
    if (_network) {
        if (!_config.rtsp.enabled) {
            // **不静默尝试联网**：没开就是没开（否则一个拼错的 URL 会去连内网/公网）
            setError("这个源是 RTSP（" + redactMediaUrl(path) +
                     "），但服务端没有开启 RTSP 输入（启动时缺 --rtsp）→ 明确拒绝");
            ErrorL << "DemuxerProducer::open 失败：" << lastError();
            _network = false; // 不留"这是网络源"的半状态（统计/校验都不该看见它）
            return false;
        }
        switch (_config.rtsp.transport) {
        case RtspConfig::Transport::Tcp:
            options.av_options["rtsp_transport"] = "tcp";
            break;
        case RtspConfig::Transport::Udp:
            options.av_options["rtsp_transport"] = "udp";
            break;
        case RtspConfig::Transport::Auto:
            break; // 不设 = FFmpeg 默认（UDP 优先、被拒自动回退 TCP，实测过）
        }
        if (_config.rtsp.timeout_ms > 0) {
            // FFmpeg 4.4 的 socket 级 TCP I/O 超时叫 stimeout（**微秒**）；5.x 改名 timeout
            options.av_options["stimeout"] = std::to_string(_config.rtsp.timeout_ms * 1000);
        }
        // 探测上限：默认 FFmpeg 会一直读到"能确定参数"为止，对端参数不全时会**烧掉整个 open 超时**
        // （实测：SDP 缺 sprop-parameter-sets 时白等 5.1 s）。网络源给 2 s 就够拿到 SDP 里的参数了。
        // 参数本来就在 SDP 里（DESCRIBE），探测只需要一小段：给 1 s / 0.5 MB
        options.av_options["analyzeduration"] = "1000000";
        options.av_options["probesize"] = "500000";
        // 参数没有就当场失败（否则 find_stream_info 空转到 open 超时，实测白等 5–10 s）
        options.require_header_parameters = true;
        InfoP("DemuxerProducer: 打开 RTSP 源 %s（transport=%s，读超时 %d ms）",
              redactMediaUrl(path).c_str(),
              _config.rtsp.transport == RtspConfig::Transport::Tcp    ? "tcp"
              : _config.rtsp.transport == RtspConfig::Transport::Udp  ? "udp"
                                                                              : "auto",
              _config.rtsp.timeout_ms);
    } else {
        options = Demuxer::Options{};
    }

    // 【M11-a2】断流重连（只有网络源；本地文件由 `loop` 那一套负责）
    if (_network) {
        SourceSession::Reconnect reconnect;
        reconnect.enabled = _config.rtsp.reconnect_enabled;
        reconnect.policy.initial_ms = _config.rtsp.backoff_initial_ms;
        reconnect.policy.max_ms = _config.rtsp.backoff_max_ms;
        reconnect.policy.max_attempts = _config.rtsp.max_attempts;
        if (!_session.setReconnect(reconnect)) {
            setError("RTSP 重连参数非法（backoff_initial/backoff_max/max_attempts 超界）→ 明确拒绝");
            ErrorL << "DemuxerProducer::open 失败：" << lastError();
            _network = false;
            return false;
        }
    }
    if (!_session.open(path, options)) {
        setError(_session.lastError()); // 不吞掉下层原因
        if (_network) {
            // 网络源的错误信息里也可能带凭据 → 脱敏后再落日志
            const std::string safe = redactMediaUrl(lastError());
            ErrorL << "DemuxerProducer::open 失败（网络源）：" << safe;
        }
        return false;
    }
    // 【M11-a】网络源的**前置校验**（DESIGN_M11 §4.8）：宁可现在拒绝，也不要让观众拿到 415 黑盒
    if (_network) {
        const StreamInfo *video = _session.firstVideo();
        if (video != nullptr) {
            if (video->width <= 0 || video->height <= 0) {
                setError("对端没有给出可用的视频参数（宽高 = " + std::to_string(video->width) + "x" +
                         std::to_string(video->height) +
                         "）→ 明确拒绝：多半是 SDP 缺 sprop-parameter-sets / 码流里没有 SPS-PPS");
                (void) _session.close();
                _network = false;
                ErrorL << "DemuxerProducer::open 失败：" << lastError();
                return false;
            }
            if (video->codec_id == AV_CODEC_ID_H264 &&
                (!video->extradata || video->extradata->empty())) {
                setError("对端没有给出 SPS/PPS（SDP 缺 sprop-parameter-sets）→ 构造不出 avcC，"
                         "明确拒绝（不去码流里猜参数集）");
                (void) _session.close();
                _network = false;
                ErrorL << "DemuxerProducer::open 失败：" << lastError();
                return false;
            }
        }
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
        if (_network) {
            _last_packet_ms = static_cast<int64_t>(getCurrentMillisecond());
        }
        return servePending(packet);
    }
}

} // namespace mzmedia
