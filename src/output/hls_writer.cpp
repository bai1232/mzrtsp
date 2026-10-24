/*
 * HlsWriter 实现（M9-b / FR-3.2）
 * ============================================================================
 * 切片规则只有三条，但每条都有"看起来能用、其实是坏片"的失败模式：
 *   1) 只切在**关键帧**上（有视频时）——否则切片首帧不是 IDR，播放端黑屏/花屏；
 *   2) 每片开头写 PAT/PMT —— 否则切片不能独立起播（播放器不知道 PID 与编码）；
 *   3) 只丢**整片**、不丢半片 —— 半片是坏片（时间戳/帧都不完整），宁可从窗口里淘汰。
 * ============================================================================
 */

#include "output/hls_writer.h"

#include "core/logger.h"

#include <cmath>

namespace mzmedia {

namespace {
/// 估算一帧时长（毫秒）：切片收尾时用"下一片首个 dts − 本片首个 dts"，
/// 但 EOS 时没有"下一片"，只能用最后一包的 dts + 一帧时长。
int64_t clampFrameDuration(int64_t value) {
    if (value <= 0) {
        return 40;   // 25fps 的兜底（只在拿不到 dts 差时用）
    }
    return value;
}
} // namespace

HlsWriter::HlsWriter(const Config &config, MpegTsMuxer::Streams streams)
    : _config(config)
    , _muxer(streams) {
    _has_video = (streams.video != nullptr);
}

HlsWriter::Ptr HlsWriter::create(const Config &config, MpegTsMuxer::Streams streams) {
    if (config.name.empty()) {
        ErrorP("HlsWriter: Config::name 为空（播放列表里的 URI 前缀必须有值）");
        return nullptr;
    }
    if (config.target_duration_ms == 0 || config.window == 0 || config.max_slice_bytes == 0 ||
        config.max_total_bytes == 0) {
        // 有界性不可协商：0 不是"不限"，而是非法配置
        ErrorP("HlsWriter: 配置非法（target=%u window=%zu slice_max=%zu total_max=%zu）—— 0 不是无界",
               config.target_duration_ms, config.window, config.max_slice_bytes,
               config.max_total_bytes);
        return nullptr;
    }
    auto writer = Ptr(new HlsWriter(config, streams));
    if (writer->_muxer.prepare() != MpegTsMuxer::Result::Ok) {
        ErrorP("HlsWriter: TS 封装无法开始 —— %s", writer->_muxer.lastError().c_str());
        return nullptr;
    }
    InfoP("HlsWriter[%s]: 就绪（目标 %u ms / 窗口 %zu 片 / 单片上限 %zu 字节 / GOP 硬顶 %u ms）",
          config.name.c_str(), config.target_duration_ms, config.window, config.max_slice_bytes,
          config.max_slice_ms);
    return writer;
}

const char *HlsWriter::resultName(Result result) {
    switch (result) {
    case Result::Ok:
        return "Ok";
    case Result::BadStreams:
        return "BadStreams";
    case Result::InvalidPacket:
        return "InvalidPacket";
    case Result::TooLarge:
        return "TooLarge";
    }
    return "unknown";
}

HlsWriter::Result HlsWriter::fail(Result result, const std::string &message) {
    {
        std::lock_guard<std::mutex> lck(_mutex);
        _last_error = message;
    }
    ErrorP("HlsWriter[%s]: %s（%s）", _config.name.c_str(), resultName(result), message.c_str());
    return result;
}

HlsWriter::Result HlsWriter::onPacket(const MediaPacket &packet) {
    if (packet.size() == 0) {
        return fail(Result::InvalidPacket, "空包");
    }
    std::lock_guard<std::mutex> lck(_mutex);
    if (_finished) {
        return Result::Ok;   // EOS 之后的包：正常忽略（源已经结束了）
    }
    const bool is_video = _has_video && packet.kind() == MediaKind::Video;
    // ★ 只有**视频**关键帧才能作为切点：AAC 音频包的 key 标志是**每个包都为真**
    //   （AV_PKT_FLAG_KEY 在音频上恒置位），用它判切点会切在音频包上 ——
    //   实测后果：切片时长变成音频的 dts（2019ms），尾巴多出一个 40ms 的片，
    //   且跨切片时间戳回退（后一片的 dts 比前一片小）→ 播放器报 dts 不单调。
    const bool video_key = is_video && packet.isKeyFrame();
    const int64_t dts = packet.dtsMs();
    // 用相邻视频包的 dts 差估一帧时长（EOS 时收尾用）
    if (is_video && _current_open) {
        const int64_t delta = dts - _current_last_dts_ms;
        if (delta > 0 && delta < 1000) {
            _frame_duration_ms = delta;
        }
    }

    // ---- 决定是否要开新片 ----
    if (!_current_open) {
        if (_has_video && !video_key) {
            // 有视频：第一个切片必须从**视频关键帧**开始（否则首片解不了）；
            // 音频包在这里被丢弃并计数（它们会随视频关键帧一起进入下一片之前的缓冲）
            ++_skipped_before_key;
            return Result::Ok;
        }
        _current.clear();
        // 每片开头都要 PAT + PMT：切片必须能独立起播
        if (_muxer.writePatPmt(&_current) != MpegTsMuxer::Result::Ok) {
            return fail(Result::BadStreams, _muxer.lastError());
        }
        _current_open = true;
        _current_first_dts_ms = dts;
        _current_last_dts_ms = dts;
    } else if (video_key) {
        const int64_t elapsed = dts - _current_first_dts_ms;
        if (elapsed >= static_cast<int64_t>(_config.target_duration_ms)) {
            // 关键帧 + 已达目标时长 → 在这里切（新片从这个关键帧开始）
            closeCurrentLocked(dts);
            _current.clear();
            if (_muxer.writePatPmt(&_current) != MpegTsMuxer::Result::Ok) {
                return fail(Result::BadStreams, _muxer.lastError());
            }
            _current_open = true;
            _current_first_dts_ms = dts;
            _current_last_dts_ms = dts;
        } else if (elapsed > static_cast<int64_t>(_config.max_slice_ms)) {
            // 走不到（elapsed >= target 已经切了），留着防御 target > max 的非法配置
            ++_oversize_slices;
            WarnP("HlsWriter[%s]: 当前切片已 %lld ms（> 硬顶 %u ms）仍等不到可切的关键帧",
                  _config.name.c_str(), static_cast<long long>(elapsed), _config.max_slice_ms);
        }
    } else if (!_has_video && dts - _current_first_dts_ms >=
                                     static_cast<int64_t>(_config.target_duration_ms)) {
        // 纯音频：没有关键帧概念，按时间切（音频包本来就可独立解码）
        closeCurrentLocked(dts);
        _current.clear();
        if (_muxer.writePatPmt(&_current) != MpegTsMuxer::Result::Ok) {
            return fail(Result::BadStreams, _muxer.lastError());
        }
        _current_open = true;
        _current_first_dts_ms = dts;
        _current_last_dts_ms = dts;
    }

    // ---- 追加到当前片 ----
    const MpegTsMuxer::Result muxed = _muxer.writePacket(packet, &_current);
    if (muxed != MpegTsMuxer::Result::Ok) {
        return fail(Result::BadStreams, _muxer.lastError());
    }
    if (is_video) {
        _current_last_dts_ms = dts;
    }
    if (_current.size() > _config.max_slice_bytes) {
        // 单片超上限：**不能**在 GOP 中间切（那是坏片）→ 计数 + 告警，等下一个关键帧
        ++_oversize_slices;
        WarnP("HlsWriter[%s]: 单片已达 %zu 字节（上限 %zu），仍在 GOP 内 —— 等关键帧再切",
              _config.name.c_str(), _current.size(), _config.max_slice_bytes);
    }
    return Result::Ok;
}

void HlsWriter::closeCurrentLocked(int64_t next_first_dts_ms) {
    if (!_current_open) {
        return;
    }
    const int64_t end_dts = (next_first_dts_ms >= 0) ? next_first_dts_ms
                                                     : (_current_last_dts_ms + _frame_duration_ms);
    int64_t duration = end_dts - _current_first_dts_ms;
    if (duration <= 0) {
        duration = clampFrameDuration(_frame_duration_ms);
    }
    Slice slice;
    slice.seq = _next_seq++;
    slice.duration_ms = duration;
    slice.bytes = std::move(_current);
    _current.clear();
    _current_open = false;
    _bytes_in_ring += slice.bytes.size();
    _ring.push_back(std::move(slice));
    ++_slices_produced;
    if (duration > static_cast<int64_t>(_config.max_slice_ms)) {
        ++_oversize_slices;
        WarnP("HlsWriter[%s]: 切片时长 %lld ms 超过硬顶 %u ms（源 GOP 太长；切片无法更短）",
              _config.name.c_str(), static_cast<long long>(duration), _config.max_slice_ms);
    }
    enforceLimitsLocked();
}

void HlsWriter::enforceLimitsLocked() {
    // 窗口：只保留最近 window 片（FR-3.2）；丢的是**整片**并计数
    while (_ring.size() > _config.window) {
        _bytes_in_ring -= _ring.front().bytes.size();
        _ring.pop_front();
        ++_slices_dropped;
    }
    // 总量上限（防御 window × 单片上限 仍不够的情形，例如码率暴涨）
    while (_bytes_in_ring > _config.max_total_bytes && !_ring.empty()) {
        _bytes_in_ring -= _ring.front().bytes.size();
        _ring.pop_front();
        ++_slices_dropped;
    }
}

void HlsWriter::finish() {
    std::lock_guard<std::mutex> lck(_mutex);
    if (_finished) {
        return;
    }
    closeCurrentLocked(-1);   // 用末包 + 一帧时长收尾
    _finished = true;
    InfoP("HlsWriter[%s]: 源结束 → 共 %llu 片（窗口内 %zu 片 / %zu 字节，丢弃 %llu 片）",
          _config.name.c_str(), static_cast<unsigned long long>(_slices_produced), _ring.size(),
          _bytes_in_ring, static_cast<unsigned long long>(_slices_dropped));
}

std::string HlsWriter::playlist() const {
    std::lock_guard<std::mutex> lck(_mutex);
    // TARGETDURATION 必须 >= 实际最大切片时长（协议要求），窗口为空时用目标值
    int64_t max_duration = static_cast<int64_t>(_config.target_duration_ms);
    for (const Slice &slice : _ring) {
        if (slice.duration_ms > max_duration) {
            max_duration = slice.duration_ms;
        }
    }
    const long target_sec = static_cast<long>(std::ceil(static_cast<double>(max_duration) / 1000.0));

    std::string m3u8;
    m3u8.reserve(256 + _ring.size() * 96);
    m3u8 += "#EXTM3U\n";
    m3u8 += "#EXT-X-VERSION:3\n";
    m3u8 += "#EXT-X-TARGETDURATION:" + std::to_string(target_sec) + "\n";
    m3u8 += "#EXT-X-MEDIA-SEQUENCE:" + std::to_string(_ring.empty() ? _next_seq : _ring.front().seq) +
            "\n";
    for (const Slice &slice : _ring) {
        // 时长保留 3 位小数（#EXTINF 的惯例）
        const long long ms = static_cast<long long>(slice.duration_ms);
        char dur[32] = {0};
        std::snprintf(dur, sizeof(dur), "%lld.%03lld", ms / 1000, ms % 1000);
        m3u8 += "#EXTINF:";
        m3u8 += dur;
        m3u8 += ",\n";
        m3u8 += "/hls/" + _config.name + "-" + std::to_string(slice.seq) + ".ts\n";
    }
    if (_finished) {
        m3u8 += "#EXT-X-ENDLIST\n";
    }
    return m3u8;
}

std::shared_ptr<const HlsWriter::Slice> HlsWriter::findSlice(uint64_t seq) const {
    std::lock_guard<std::mutex> lck(_mutex);
    for (const Slice &slice : _ring) {
        if (slice.seq == seq) {
            // 返回副本的 shared_ptr：调用方在锁外读取，ring 淘汰时不会悬垂
            return std::make_shared<const Slice>(slice);
        }
    }
    return nullptr;
}

} // namespace mzmedia
