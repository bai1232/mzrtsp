/*
 * SourceSession 实现（v0.2.3 重构抽出；逻辑逐字搬自 v0.2.2 的 `DemuxerProducer`）
 * ============================================================================
 * 这里只做"读"。**没有**形状转换、没有转码、没有 FLV —— 那些在 `PacketStage` 与输出层。
 * 把逻辑搬过来时刻意保留了每一处注释与判据（尤其是循环重开的三个坑），避免"重构顺手改行为"。
 * ============================================================================
 */

#include "media/source_session.h"

#include "core/logger.h"
#include "core/util.h"

#include <chrono>
#include <thread>
#include <utility>

namespace mzmedia {

/// 循环重开时给时间轴留的间隙（毫秒）：保证第二趟的首包**严格**排在第一趟末包之后
/// （时间戳是整数毫秒，而真实帧长可能是小数；见 passEndMs() 的说明）
constexpr int64_t kLoopGapMs = 1;

SourceSession::SourceSession(const Config &config)
    : _config(config)
    , _demux(config.demux)
    , _label(config.label) {}

void SourceSession::setError(const std::string &message) {
    std::lock_guard<std::mutex> lock(_mutex);
    _last_error = message;
}

std::string SourceSession::lastError() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _last_error;
}

const StreamInfo *SourceSession::findStream(int index) const {
    for (const auto &info : _demux.streams()) {
        if (info.index == index) {
            return &info;
        }
    }
    return nullptr;
}

int64_t SourceSession::estimateFrameDurationMs(const StreamInfo &info) {
    // **向上取整**：时间戳只以整数毫秒表示，向下取整会让"这一趟的结束时刻"偏小
    // （AAC 一帧真实是 23.22ms → 23ms），于是第二趟的首包可能和第一趟的末包撞在同一毫秒上
    // （实测：ffmpeg 的 null muxer 会报 "non monotonically increasing dts"）。
    // 宁可多算不到 1ms（时间轴多一点点间隙，听不出来），也不要让时间轴倒回去。
    if (info.is_video && info.frame_rate.num > 0 && info.frame_rate.den > 0) {
        const int64_t num = info.frame_rate.num;
        const int64_t den = info.frame_rate.den;
        return (1000LL * den + num - 1) / num; // ceil(1000 * den / num)
    }
    if (info.is_audio && info.sample_rate > 0) {
        // v0.1 只认 AAC：一帧固定 1024 个样本。只用于"重开偏移"的估算，不参与时间戳换算
        const int64_t rate = info.sample_rate;
        return (1024LL * 1000 + rate - 1) / rate;
    }
    return 0; // 拿不到就返回 0（不猜）
}

bool SourceSession::open(const std::string &path) {
    return open(path, Demuxer::Options{});
}

std::string SourceSession::displayName() const {
    if (!_label.empty()) {
        return _label;
    }
    return redactMediaUrl(_path);
}

bool SourceSession::open(const std::string &path, const Demuxer::Options &options) {
    if (path.empty()) {
        setError("path 为空");
        ErrorL << "SourceSession::open 失败：" << lastError();
        return false;
    }
    if (_demux.opened()) {
        setError("已经打开过（需要换源请新建 session）");
        ErrorL << "SourceSession::open 失败：" << lastError();
        return false;
    }
    if (!_demux.open(path, options)) {
        setError(_demux.lastError()); // 不吞掉下层原因
        return false;
    }
    _path = path;
    _network = isNetworkMediaUrl(path);
    _options = options;   // 重连要用**同一批**打开选项（transport/timeout/探测上限）
    _opened_at_ms.store(static_cast<int64_t>(getCurrentMillisecond()));
    _loop_offset_ms.store(0);
    _pass.assign(_demux.streams().size(), PassTrack{});
    // 打开时记下**源**流参数快照（重开时校验用；不刷新 —— 输出层持有它的裸指针）
    if (const StreamInfo *video = _demux.firstVideo()) {
        _raw_video_info = *video;
        _has_video_info = true;
    }
    if (const StreamInfo *audio = _demux.firstAudio()) {
        _raw_audio_info = *audio;
        _has_audio_info = true;
    }
    if (_network) {
        _net_state.store(NetState::Playing);
        ++_connects;
    }
    return true;
}

bool SourceSession::close() {
    if (!_demux.opened()) {
        return false;
    }
    (void) _demux.close();
    _pass.clear();
    _path.clear();
    return true;
}

bool SourceSession::sameStreamParams(const StreamInfo &a, const StreamInfo &b) {
    if (a.index != b.index || a.codec_id != b.codec_id || a.is_video != b.is_video ||
        a.is_audio != b.is_audio || a.width != b.width || a.height != b.height ||
        a.sample_rate != b.sample_rate || a.channels != b.channels) {
        return false;
    }
    const bool a_has = a.extradata && !a.extradata->empty();
    const bool b_has = b.extradata && !b.extradata->empty();
    if (a_has != b_has) {
        return false;
    }
    if (!a_has) {
        return true;
    }
    return *a.extradata == *b.extradata; // 初始化数据变了 = 编码参数变了
}

void SourceSession::trackPass(int index, int64_t raw_dts_ms) {
    if (index < 0 || static_cast<size_t>(index) >= _pass.size()) {
        return; // 不在快照里（理论上不该发生；宁可少记一笔也不要越界写）
    }
    PassTrack &track = _pass[static_cast<size_t>(index)];
    if (!track.has_first) {
        track.first_dts = raw_dts_ms; // 记下这一趟该流的起点（可能是负数）
        track.has_first = true;
    }
    if (!track.has_last) {
        track.last_dts = raw_dts_ms;
        track.has_last = true;
        return;
    }
    track.prev_dts = track.last_dts;
    track.has_prev = true;
    if (raw_dts_ms > track.last_dts) {
        track.last_dts = raw_dts_ms;
    }
}

int64_t SourceSession::passEndMs() {
    int64_t end = 0;
    bool any = false;
    for (size_t i = 0; i < _pass.size(); ++i) {
        const PassTrack &track = _pass[i];
        if (!track.has_last) {
            continue;
        }
        any = true;
        int64_t duration = 0;
        if (track.has_prev && track.last_dts > track.prev_dts) {
            duration = track.last_dts - track.prev_dts; // 观测到的帧间隔：比任何估算都可靠
        }
        if (duration <= 0) {
            const StreamInfo *info = findStream(static_cast<int>(i));
            if (info != nullptr) {
                duration = estimateFrameDurationMs(*info);
            }
        }
        const int64_t stream_end = track.last_dts + duration + kLoopGapMs;
        if (stream_end > end) {
            end = stream_end;
        }
    }
    if (!any) {
        return 0; // 这一趟一个包都没有
    }
    // 保证每趟至少前进 1ms：否则"全是 0 时间戳"的文件会让循环在同一时刻原地打转
    return end > 0 ? end : 1;
}

int64_t SourceSession::passStartMs() {
    int64_t start = 0;
    bool any = false;
    for (const auto &track : _pass) {
        if (!track.has_first) {
            continue;
        }
        // 取**最小**的首包 dts：偏移要让"最早开始的那条流"接得上，其余流自然更晚（不会倒退）
        if (!any || track.first_dts < start) {
            start = track.first_dts;
            any = true;
        }
    }
    return any ? start : 0;
}

bool SourceSession::reopenForLoop() {
    // 偏移必须在关文件**之前**算（要用当前这趟的流信息与 dts 轨迹）
    const int64_t pass_end = passEndMs();
    if (pass_end <= 0) {
        // 空趟：重开还会立刻 EOF → 那就是死循环 + 100% CPU。明确停下来
        ++_loop_failures;
        setError("循环重开：这一趟没有读到任何包（空文件/截断）→ 停止循环");
        ErrorL << "SourceSession: " << lastError();
        return false;
    }

    if (!_demux.close()) {
        ++_loop_failures;
        setError("循环重开：关闭上一次的上下文失败（本来没打开）");
        return false;
    }
    if (!_demux.open(_path)) {
        ++_loop_failures;
        setError("循环重开失败：" + _demux.lastError());
        ErrorL << "SourceSession: " << lastError();
        return false;
    }

    // 参数校验：同一路径重开，流参数必须一模一样。
    // 变了说明文件在运行期被换掉了 —— 此时继续用**旧快照**（连接可能正拿着它的指针写 FLV）
    // 就会把新文件的字节按旧参数封装，静默出一路坏流；所以这里明确失败。
    const StreamInfo *video = _demux.firstVideo();
    const StreamInfo *audio = _demux.firstAudio();
    const bool video_ok = (_has_video_info == (video != nullptr)) &&
                          (!_has_video_info || sameStreamParams(_raw_video_info, *video));
    const bool audio_ok = (_has_audio_info == (audio != nullptr)) &&
                          (!_has_audio_info || sameStreamParams(_raw_audio_info, *audio));
    if (!video_ok || !audio_ok) {
        ++_loop_failures;
        setError("循环重开：源 " + _path + " 的流参数与上一趟不一致（被换掉了？）→ 停止");
        ErrorL << "SourceSession: " << lastError();
        return false;
    }

    // 【v0.2.1】偏移 = 这一趟的**真实长度**（`pass_end - pass_start`）。
    // 少了 `pass_start` 这一项时，首包 dts 为负的源（B 帧 / MP4 编辑列表）会在接缝处**倒退**：
    // 实测 `bf.mp4` 的首包 dts = -80ms、末包 960ms，按旧公式下一趟从 921ms 开始（倒退 39ms），
    // 循环播放每个接缝都抖一下 —— FLV 与 TS 两条路径都受影响。
    const int64_t pass_start = passStartMs();
    const int64_t pass_length = pass_end - pass_start;
    _loop_offset_ms.fetch_add(pass_length);
    _pass.assign(_demux.streams().size(), PassTrack{}); // 新一趟从零开始记
    ++_loops;
    if (_on_seam) {
        _on_seam(pass_length);   // 转码阶段据此对齐时间轴（转换类忽略）
    }
    InfoP("SourceSession: 循环重开第 %llu 次：本趟 [%lld, %lld] ms → 偏移 +%lld ms",
          static_cast<unsigned long long>(_loops.load()), static_cast<long long>(pass_start),
          static_cast<long long>(pass_end), static_cast<long long>(pass_length));
    return true;
}

const char *SourceSession::netStateName(NetState state) {
    switch (state) {
    case NetState::Idle: return "idle";
    case NetState::Playing: return "playing";
    case NetState::Reconnecting: return "reconnecting";
    case NetState::Failed: return "failed";
    }
    return "unknown";
}

bool SourceSession::setReconnect(const Reconnect &reconnect) {
    if (reconnect.enabled && !_policy.setConfig(reconnect.policy)) {
        return false; // 退避参数非法：保持原值（不静默改成"看起来合理"的数）
    }
    _reconnect = reconnect;
    return true;
}

int64_t SourceSession::lastPacketAgeMs() const {
    const int64_t last = _last_packet_ms.load();
    if (last <= 0) {
        return 0;
    }
    const int64_t now = static_cast<int64_t>(getCurrentMillisecond());
    return (now > last) ? (now - last) : 0;
}

std::string SourceSession::lastNetError() const {
    std::lock_guard<std::mutex> lock(_net_mutex);
    return _last_net_error;
}

bool SourceSession::sleepInterruptibleMs(int64_t delay_ms) {
    // 小片睡眠：既尊重中止标志（stop() 不该等满 30s），也不忙等
    constexpr int64_t kSliceMs = 50;
    int64_t remaining = delay_ms;
    while (remaining > 0) {
        if (_abort != nullptr && _abort->load()) {
            return false;
        }
        const int64_t slice = (remaining < kSliceMs) ? remaining : kSliceMs;
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        remaining -= slice;
    }
    return !(_abort != nullptr && _abort->load());
}

void SourceSession::continueTimelineAt(int64_t raw_first_dts_ms) {
    // 目标：新一段的**第一帧**落在"上一包生效 dts + 一帧间隔"之后。
    // 无论对端是"接着发"（raw dts 已经是后续值）还是"从 0 重来"，这个公式都成立：
    //   offset = 目标 − 新的原始 dts
    int64_t gap = 0;
    if (const StreamInfo *video = _demux.firstVideo()) {
        gap = estimateFrameDurationMs(*video);
    }
    if (gap <= 0) {
        gap = kLoopGapMs; // 拿不到帧率就至少前进 1ms（不原地打转）
    }
    _loop_offset_ms.store(
        timelineOffsetForReconnect(_last_effective_dts, _has_effective_dts, raw_first_dts_ms, gap));
    InfoP("SourceSession: 重连后接续时间轴 —— 上一包生效 dts=%lld，新一段原始 dts=%lld → 偏移 %lld ms",
          static_cast<long long>(_last_effective_dts), static_cast<long long>(raw_first_dts_ms),
          static_cast<long long>(_loop_offset_ms.load()));
}

bool SourceSession::reconnectNetwork(std::string *error) {
    while (true) {
        if (_abort != nullptr && _abort->load()) {
            if (error != nullptr) {
                *error = "重连被中止（源被停）";
            }
            _net_state.store(NetState::Failed);
            return false;
        }
        const int64_t delay = _policy.nextDelayMs();
        if (delay < 0) {
            if (error != nullptr) {
                *error = "重连次数达到上限（" +
                         std::to_string(_reconnect.policy.max_attempts) +
                         " 次）仍失败 → 放弃这一路（不伪装成「播完了」）";
            }
            _net_state.store(NetState::Failed);
            ErrorL << "SourceSession: " << (error != nullptr ? *error : "重连放弃");
            return false;
        }
        _net_state.store(NetState::Reconnecting);
        WarnP("SourceSession[%s]: 断流 → %lld ms 后第 %llu 次重连",
              displayName().c_str(), static_cast<long long>(delay),
              static_cast<unsigned long long>(_policy.attempts()));
        if (!sleepInterruptibleMs(delay)) {
            if (error != nullptr) {
                *error = "重连等待期间被中止（源被停）";
            }
            _net_state.store(NetState::Failed);
            return false;
        }

        (void) _demux.close();
        if (!_demux.open(_path, _options)) {
            // 打不开 = 这一轮失败：**继续退避**（对端可能还没起来），并记下原因供排查
            {
                std::lock_guard<std::mutex> lock(_net_mutex);
                _last_net_error = redactMediaUrl(_demux.lastError());
            }
            WarnP("SourceSession[%s]: 第 %llu 次重连打不开（%s）",
                  displayName().c_str(), static_cast<unsigned long long>(_policy.attempts()),
                  redactMediaUrl(_demux.lastError()).c_str());
            continue;
        }
        // 参数没变才继续（与循环重开同一套校验：变了 = 换了源，明确失败）
        const StreamInfo *video = _demux.firstVideo();
        const StreamInfo *audio = _demux.firstAudio();
        const bool video_ok = (_has_video_info == (video != nullptr)) &&
                              (!_has_video_info || sameStreamParams(_raw_video_info, *video));
        const bool audio_ok = (_has_audio_info == (audio != nullptr)) &&
                              (!_has_audio_info || sameStreamParams(_raw_audio_info, *audio));
        if (!video_ok || !audio_ok) {
            _net_state.store(NetState::Failed);
            if (error != nullptr) {
                *error = "重连后流参数与断开前不一致（对端换了码流？）→ 放弃这一路";
            }
            ErrorL << "SourceSession: " << (error != nullptr ? *error : "参数不一致");
            return false;
        }
        _pass.assign(_demux.streams().size(), PassTrack{}); // 新一段从零记
        _reconnect_needs_timeline = true; // 读到新一段的第一个包时接续时间轴
        ++_connects;
        ++_reconnects;
        _net_state.store(NetState::Playing);
        InfoP("SourceSession[%s]: 重连成功（第 %llu 次尝试）", displayName().c_str(),
              static_cast<unsigned long long>(_policy.attempts()));
        return true;   // 退避复位在"读到第一个包"之后（read() 里调 onSuccess）
    }
}

SourceSession::Result SourceSession::read(Packet *out, std::string *error) {
    if (out == nullptr || error == nullptr) {
        setError("read 的出参为空（调用方 bug）");
        ErrorL << "SourceSession::read 失败：" << lastError();
        return Result::Error;
    }
    if (!_demux.opened()) {
        setError("尚未 open 就读包");
        ErrorL << "SourceSession::read 失败：" << lastError();
        return Result::Error;
    }
    error->clear();

    while (true) {
        const Demuxer::ReadResult result = _demux.readPacket();
        // 【M11-a2b】网络源的 **EOF 也是断流**：对端关掉 TCP（interleaved）时，`av_read_frame`
        //   给的是 EOF 而不是 Error。以前这里直接当"播完了"→ `SourcePump::endOfStream()` →
        //   **观众被踢下线**（实测：TCP 传输 + 对端 close，1 秒后 curl 拿到正常结束码 0）。
        //   直播流没有"结尾"：网络源一律走重连，只有本地文件才允许 EOF。
        const bool peer_gone = (result == Demuxer::ReadResult::Eof) && _network;
        if (result == Demuxer::ReadResult::Eof && !peer_gone) {
            if (!_config.loop) {
                return Result::EndOfStream;
            }
            if (!reopenForLoop()) {
                return Result::Error; // 重开失败 = 明确失败，不伪装成"读完"
            }
            continue; // 重开成功：接着读新一趟（时间戳已由偏移接上）
        }
        if (result == Demuxer::ReadResult::Error || peer_gone) {
            const std::string reason = peer_gone ? "对端关闭了连接（直播流的 EOF = 断流）"
                                                 : _demux.lastError();
            setError(reason); // 如实转达（含超时/超限/损坏）
            // 【M11-a2】网络源：断流/超时/EOF → **进入重连**，绝不把结束抛给 SourcePump
            //   （SourcePump 见 Error/EOS 就 endOfStream()，观众会掉线 —— 见 DESIGN_M11 §3）
            if (_network && _reconnect.enabled && !(_abort != nullptr && _abort->load())) {
                {
                    std::lock_guard<std::mutex> lock(_net_mutex);
                    _last_net_error = redactMediaUrl(reason);
                }
                std::string reconnect_error;
                if (!reconnectNetwork(&reconnect_error)) {
                    setError(reconnect_error);
                    *error = lastError();
                    return Result::Error;
                }
                continue; // 重连成功：接着读（时间戳已在读到新一段第一个包时接上）
            }
            *error = lastError();
            return Result::Error;
        }

        const AVPacket *pkt = _demux.packet();
        if (pkt == nullptr || pkt->data == nullptr || pkt->size <= 0) {
            setError("读到空包（data 为空或 size <= 0）");
            ErrorL << "SourceSession::read 失败：" << lastError();
            *error = lastError();
            return Result::Error;
        }

        const int stream_index = _demux.packetStreamIndex();
        const StreamInfo *info = findStream(stream_index);
        MediaKind kind = MediaKind::Video;
        if (info != nullptr && info->is_video) {
            kind = MediaKind::Video;
        } else if (info != nullptr && info->is_audio) {
            kind = MediaKind::Audio;
        } else {
            // 字幕/数据流：媒体层不认，跳过但**计数**（静默跳过 = 悄悄丢数据）
            ++_skipped_unknown;
            continue;
        }

        int64_t pts_ms = 0;
        int64_t dts_ms = 0;
        if (!_demux.packetTimestampsMs(&pts_ms, &dts_ms)) {
            // 换算失败仍然发（Demuxer 保持上一次的值并做了单调钳制）；丢包比时间戳不精确更糟
            ++_ts_failures;
        }
        trackPass(stream_index, dts_ms); // 记本趟的**原始** dts（重开偏移要基于它算）

        // 全链路**唯一**一次拷贝：Demuxer 的 AVPacket 下一帧就被复用
        auto payload = std::make_shared<const std::vector<uint8_t>>(
            pkt->data, pkt->data + static_cast<size_t>(pkt->size));

        // 【M11-a2】刚重连上：用这一包确定新一段的趟偏移（把时间轴接在上一包之后）
        if (_network && _reconnect_needs_timeline) {
            _reconnect_needs_timeline = false;
            continueTimelineAt(dts_ms);
        }

        out->kind = kind;
        out->stream_index = stream_index;
        out->key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        out->raw_dts_ms = dts_ms;
        out->raw_pts_ms = pts_ms;
        out->payload = std::move(payload);

        const int64_t effective = dts_ms + _loop_offset_ms.load();
        if (!_has_effective_dts || effective > _last_effective_dts) {
            _last_effective_dts = effective;
            _has_effective_dts = true;
        }
        _last_packet_ms.store(static_cast<int64_t>(getCurrentMillisecond()));

        ++_packets;
        _bytes += out->payload->size();
        return Result::Packet;
    }
}

} // namespace mzmedia
