/*
 * Demuxer 实现（M4-a）
 * ============================================================================
 * 两个容易写错的点，写在最前面：
 *   1) `avformat_open_input` **失败**时，ctx 仍归调用方 → 必须自己 `avformat_free_context`，
 *      否则每失败一次泄漏一个上下文；成功后才交给 RAII（avformat_close_input）。
 *   2) 超时靠 `interrupt_callback`：它在 FFmpeg 内部的**阻塞循环里**被反复调用，
 *      所以"open 卡在网络上"也能被打断 —— 这是唯一真正有效的超时手段。
 * ============================================================================
 */

#include "ffmpeg/demuxer.h"

#include "core/logger.h"
#include "core/util.h"

#include <libavutil/error.h>

#include <string>

namespace mzmedia {

namespace {

std::string avErrStr(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (::av_strerror(err, buf, sizeof(buf)) != 0) {
        return "errno=" + std::to_string(err);
    }
    return buf;
}

TimeBase fromRational(AVRational r) {
    return TimeBase{r.num, r.den};
}

} // namespace

Demuxer::Demuxer() = default;

Demuxer::Demuxer(const Limits &limits) : _limits(limits) {}

Demuxer::~Demuxer() = default;

int Demuxer::interruptCb(void *opaque) {
    auto *self = static_cast<Demuxer *>(opaque);
    // 返回非 0 = 请求中断。
    // 先看外部中止标志（M5-c）：源被停时要**立刻**把 av_read_frame 打断，
    // 否则 SourcePump::stop() 的 join 要一直等到读超时（5s）才回来。
    if (self->_abort != nullptr && self->_abort->load()) {
        return 1;
    }
    return getCurrentMillisecond() > static_cast<uint64_t>(self->_deadline_ms) ? 1 : 0;
}

const std::atomic<bool> *Demuxer::setAbortFlag(const std::atomic<bool> *flag) {
    const std::atomic<bool> *previous = _abort;
    _abort = flag;
    return previous;
}

bool Demuxer::open(const std::string &path) {
    return open(path, Options{});
}

bool Demuxer::open(const std::string &path, const Options &options) {
    if (opened()) {
        _last_error = "已经打开过，需先销毁再开新文件";
        ErrorP("Demuxer: open 在已打开状态下调用");
        return false;
    }
    AVFormatContext *ctx = ::avformat_alloc_context();
    if (ctx == nullptr) {
        _last_error = "avformat_alloc_context 失败";
        ErrorP("Demuxer: %s", _last_error.c_str());
        return false;
    }
    ctx->interrupt_callback.callback = &Demuxer::interruptCb;
    ctx->interrupt_callback.opaque = this;
    _deadline_ms = static_cast<int64_t>(getCurrentMillisecond()) + _limits.open_timeout_ms;

    // 【M11-a】把选项交给 FFmpeg（空表 = 与以前完全一样：本地文件路径不受影响）
    AVDictionary *av_options = nullptr;
    for (const auto &kv : options.av_options) {
        if (!kv.first.empty() && !kv.second.empty()) {
            (void) ::av_dict_set(&av_options, kv.first.c_str(), kv.second.c_str(), 0);
        }
    }
    const int err = ::avformat_open_input(&ctx, path.c_str(), nullptr, &av_options);
    // FFmpeg 会把**没被消费**的选项留在表里：只 Warn 不失败（有些选项在某些协议上无意义），
    // 但要让它可见 —— 静默忽略一个拼错的选项，排查成本比报错高得多。
    if (av_options != nullptr) {
        const AVDictionaryEntry *leftover = nullptr;
        size_t leftover_count = 0;
        while ((leftover = ::av_dict_get(av_options, "", leftover, AV_DICT_IGNORE_SUFFIX)) != nullptr) {
            WarnP("Demuxer: 打开选项 %s=%s 没有被使用（协议不支持？键名拼错？）", leftover->key,
                  leftover->value);
            ++leftover_count;
        }
        (void) leftover_count;
        ::av_dict_free(&av_options);
    }
    if (err < 0) {
        ::avformat_free_context(ctx);   // 失败时 ctx 仍归我们
        _last_error = "avformat_open_input 失败: " + avErrStr(err);
        ErrorP("Demuxer: 打开 %s 失败（%s）", path.c_str(), _last_error.c_str());
        return false;
    }
    _fmt.reset(ctx);   // 成功：交给 RAII（释放用 avformat_close_input）

    // 【M11-a】网络源：SDP 给不出视频参数时**立刻失败**，不要空转到超时（实测会白等 5–10 s）
    if (options.require_header_parameters) {
        for (unsigned i = 0; i < _fmt->nb_streams; ++i) {
            const AVCodecParameters *par = _fmt->streams[i]->codecpar;
            if (par->codec_type != AVMEDIA_TYPE_VIDEO) {
                continue;
            }
            const bool has_extra = par->extradata != nullptr && par->extradata_size > 0;
            if ((par->width <= 0 || par->height <= 0) && !has_extra) {
                _last_error = "打开阶段没有拿到视频参数（宽高 = " + std::to_string(par->width) + "x" +
                              std::to_string(par->height) + "，初始化数据 " +
                              std::to_string(par->extradata_size) +
                              " 字节）→ 多半是 SDP 缺 sprop-parameter-sets；"
                              "不做 find_stream_info（那会空转到超时）";
                ErrorP("Demuxer: %s", _last_error.c_str());
                _fmt.reset();
                return false;
            }
        }
    }

    _deadline_ms = static_cast<int64_t>(getCurrentMillisecond()) + _limits.open_timeout_ms;
    const int info_err = ::avformat_find_stream_info(_fmt.get(), nullptr);
    if (info_err < 0) {
        _last_error = "avformat_find_stream_info 失败: " + avErrStr(info_err);
        ErrorP("Demuxer: %s 读流信息失败（%s）", path.c_str(), _last_error.c_str());
        _fmt.reset();
        return false;
    }

    if (static_cast<int>(_fmt->nb_streams) > _limits.max_streams) {
        // 拒绝，不"只取前 N 路"—— 静默截断会让后面所有统计都对不上
        _last_error = "流数 " + std::to_string(_fmt->nb_streams) + " 超过上限 " +
                      std::to_string(_limits.max_streams);
        ErrorP("Demuxer: %s", _last_error.c_str());
        _fmt.reset();
        return false;
    }

    for (unsigned i = 0; i < _fmt->nb_streams; ++i) {
        AVStream *st = _fmt->streams[i];
        AVCodecParameters *par = st->codecpar;
        StreamInfo info;
        info.index = static_cast<int>(i);
        info.codec_id = par->codec_id;
        const AVCodecDescriptor *desc = ::avcodec_descriptor_get(par->codec_id);
        info.codec_name = (desc != nullptr && desc->name != nullptr) ? desc->name : "unknown";
        info.is_video = (par->codec_type == AVMEDIA_TYPE_VIDEO);
        info.is_audio = (par->codec_type == AVMEDIA_TYPE_AUDIO);
        info.width = par->width;
        info.height = par->height;
        info.sample_rate = par->sample_rate;
        info.channels = par->channels;   // FFmpeg 4.4 用 channels（5.x 才是 ch_layout）
        info.time_base = fromRational(st->time_base);
        info.frame_rate = fromRational(st->avg_frame_rate);
        if (par->extradata != nullptr && par->extradata_size > 0) {
            // M6-a：FLV 的 AVC/AAC sequence header 要用它。
            // 拷贝一份（不能只存指针：AVFormatContext 一关就没了）
            info.extradata = std::make_shared<const std::vector<uint8_t>>(
                par->extradata, par->extradata + par->extradata_size);
        }
        _streams.push_back(info);
        InfoP("Demuxer: 流 %d：%s %dx%d rate=%d/%d tb=%d/%d init=%zuB", info.index,
              info.codec_name.c_str(), info.width, info.height,
              static_cast<int>(info.frame_rate.num), static_cast<int>(info.frame_rate.den),
              static_cast<int>(info.time_base.num), static_cast<int>(info.time_base.den),
              info.extradata ? info.extradata->size() : 0u);
    }

    _pkt = makePacket();
    if (!_pkt) {
        _last_error = "av_packet_alloc 失败";
        _fmt.reset();
        return false;
    }
    _dts_guard.resize(_streams.size());
    _last_pts.assign(_streams.size(), 0);
    _has_last_pts.assign(_streams.size(), false);
    _path = path;
    return true;
}

bool Demuxer::opened() const {
    return _fmt != nullptr;
}

bool Demuxer::close() {
    if (!opened()) {
        return false;
    }
    _fmt.reset();
    _pkt.reset();
    _streams.clear();
    _dts_guard.clear();
    _last_pts.clear();
    _has_last_pts.clear();
    _pkt_stream_index = -1;
    _pkt_pts_ms = 0;
    _pkt_dts_ms = 0;
    _eof = false;
    _path.clear();
    return true;
}

const std::string &Demuxer::path() const {
    return _path;
}

const std::vector<StreamInfo> &Demuxer::streams() const {
    return _streams;
}

const StreamInfo *Demuxer::firstVideo() const {
    for (const auto &s : _streams) {
        if (s.is_video) {
            return &s;
        }
    }
    return nullptr;
}

const StreamInfo *Demuxer::firstAudio() const {
    for (const auto &s : _streams) {
        if (s.is_audio) {
            return &s;
        }
    }
    return nullptr;
}

Demuxer::ReadResult Demuxer::readPacket() {
    if (!opened()) {
        _last_error = "未打开就 readPacket";
        ErrorP("Demuxer: %s", _last_error.c_str());
        return ReadResult::Error;
    }
    if (_eof) {
        return ReadResult::Eof;   // EOF 是"稳定的终态"：重复读仍返回 Eof（不是 Error）
    }
    ::av_packet_unref(_pkt.get());
    _deadline_ms = static_cast<int64_t>(getCurrentMillisecond()) + _limits.read_timeout_ms;
    const int err = ::av_read_frame(_fmt.get(), _pkt.get());
    if (err == AVERROR_EOF) {
        _eof = true;
        return ReadResult::Eof;
    }
    if (err < 0) {
        _last_error = "av_read_frame 失败: " + avErrStr(err);
        ErrorP("Demuxer: %s", _last_error.c_str());
        return ReadResult::Error;
    }
    if (_pkt->size > _limits.max_packet_size) {
        ++_rejected_packets;
        _last_error = "单包 " + std::to_string(_pkt->size) + " 字节超过上限 " +
                      std::to_string(_limits.max_packet_size);
        ErrorP("Demuxer: %s（拒绝，不缓存）", _last_error.c_str());
        ::av_packet_unref(_pkt.get());
        return ReadResult::Error;
    }

    _pkt_stream_index = _pkt->stream_index;
    ++_total_packets;
    _total_bytes += static_cast<uint64_t>(_pkt->size);

    // 时间戳：换算到毫秒 + 单调钳制（两者失败都计数）
    int64_t pts_ms = _pkt_pts_ms;
    int64_t dts_ms = _pkt_dts_ms;
    const size_t idx = static_cast<size_t>(_pkt_stream_index);
    if (idx < _streams.size()) {
        const TimeBase src = _streams[idx].time_base;
        const TimeBase ms{1, 1000};
        int64_t raw_pts = (_pkt->pts == AV_NOPTS_VALUE) ? _pkt->dts : _pkt->pts;
        int64_t raw_dts = _pkt->dts;
        if (raw_pts == AV_NOPTS_VALUE || raw_dts == AV_NOPTS_VALUE ||
            !rescaleTimestamp(raw_pts, src, ms, &pts_ms) ||
            !rescaleTimestamp(raw_dts, src, ms, &dts_ms)) {
            ++_rescale_failures;   // 缺时间戳/换算失败：计数 + 保持上一次的值
        } else {
            // 【v0.2.1】**pts 不做单调钳制**：B 帧源的 pts 天生乱序（同一个 GOP 里
            // 解码序 I P B B 的 pts 可能是 0, 120, 40, 80），钳了就等于**篡改显示顺序**。
            // 只统计"回退"次数（正常现象），让它一路传到封装层算 CompositionTime。
            if (_has_last_pts[idx] && pts_ms < _last_pts[idx]) {
                ++_pts_out_of_order;
            }
            _last_pts[idx] = pts_ms;
            _has_last_pts[idx] = true;
            // dts 必须单调（容器/播放器都要求），这里仍然钳 + 计数（真异常才会计数增长）
            dts_ms = _dts_guard[idx].apply(dts_ms);
            _pkt_pts_ms = pts_ms;
            _pkt_dts_ms = dts_ms;
        }
    } else {
        ++_rescale_failures;
    }
    return ReadResult::Packet;
}

const AVPacket *Demuxer::packet() const {
    return _pkt.get();
}

int Demuxer::packetStreamIndex() const {
    return _pkt_stream_index;
}

bool Demuxer::packetTimestampsMs(int64_t *pts_ms, int64_t *dts_ms) {
    if (pts_ms == nullptr || dts_ms == nullptr) {
        return false;
    }
    if (!opened() || _pkt_stream_index < 0) {
        return false;
    }
    *pts_ms = _pkt_pts_ms;
    *dts_ms = _pkt_dts_ms;
    return true;
}

uint64_t Demuxer::totalPackets() const {
    return _total_packets;
}

uint64_t Demuxer::totalBytes() const {
    return _total_bytes;
}

uint64_t Demuxer::rejectedPackets() const {
    return _rejected_packets;
}

uint64_t Demuxer::rescaleFailures() const {
    return _rescale_failures;
}

uint64_t Demuxer::dtsClamped() const {
    uint64_t total = 0;
    for (const auto &g : _dts_guard) {
        total += g.monotonicClampCount();
    }
    return total;
}

uint64_t Demuxer::ptsOutOfOrder() const {
    return _pts_out_of_order;
}

const std::string &Demuxer::lastError() const {
    return _last_error;
}

} // namespace mzmedia
