/*
 * DemuxerProducer 实现（M5-c；M6-c 加循环重开 + Annex-B → AVCC）
 * ============================================================================
 * `read()` 的循环只在两个地方兜圈：**跳过非音视频流**、**循环重开**。
 * 其余情况一律"一步给出结论"（有包 / 读完 / 出错），因为 `SourcePump`
 * 的退出语义依赖这些结局被如实转达 —— 把"跳过"伪装成"读完"会让源提前结束，
 * 把"重开失败"伪装成"读完"会让观众以为片子播完了（其实是文件读不下去了）。
 *
 * 线程说明：`read()` 由源线程调用；计数是 atomic、`lastError()` 走锁、
 * 流快照（`_video_info` 等）在 `open()` 里写完后**只读**，因此观测方读它们都安全。
 * ============================================================================
 */

#include "media/demuxer_producer.h"

#include "core/logger.h"
#include "ffmpeg/aac_util.h"
#include "ffmpeg/h264_util.h"

#include <utility>
#include <vector>

namespace mzmedia {

/// 循环重开时给时间轴留的间隙（毫秒）：保证第二趟的首包**严格**排在第一趟末包之后
/// （时间戳是整数毫秒，而真实帧长可能是小数；见 passEndMs() 的说明）
constexpr int64_t kLoopGapMs = 1;

DemuxerProducer::DemuxerProducer(const Config &config)
    : _config(config)
    , _demux(config.demux) {}

// 无参构造与"带 Config"分开写：`Config` 自带默认成员初始值，
// 在类内把它当默认实参用会触发 [class.mem] 的"必须在类完成前初始化"限制（GCC 直接报错）。
// MediaSource 也是同样的写法。
DemuxerProducer::DemuxerProducer() : DemuxerProducer(Config{}) {}

void DemuxerProducer::setError(const std::string &message) {
    std::lock_guard<std::mutex> lock(_mutex);
    _last_error = message;
}

std::string DemuxerProducer::lastError() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _last_error;
}

uint64_t DemuxerProducer::bitrateBps() const {
    const int64_t opened = _opened_at_ms.load();
    if (opened <= 0) {
        return 0; // 还没打开：没有"码率"可言（不猜）
    }
    const int64_t elapsed_ms = static_cast<int64_t>(getCurrentMillisecond()) - opened;
    if (elapsed_ms <= 0) {
        return 0; // 不足 1ms：返回 0，避免除零造出天文数字
    }
    return static_cast<uint64_t>(_bytes.load()) * 8u * 1000u /
           static_cast<uint64_t>(elapsed_ms);
}

bool DemuxerProducer::open(const std::string &path) {
    if (path.empty()) {
        setError("path 为空");
        ErrorL << "DemuxerProducer::open 失败：" << lastError();
        return false;
    }
    if (_demux.opened()) {
        setError("已经打开过（需要换文件请新建 producer）");
        ErrorL << "DemuxerProducer::open 失败：" << lastError();
        return false;
    }
    if (!_demux.open(path)) {
        setError(_demux.lastError()); // 不吞掉下层原因
        return false;
    }
    // 【M8】_path 必须在 prepareStreams() **之前**记下：ADTS 探针要用它重新打开同一个文件
    // （裸流/MP4 用不到，但把赋值提前没有副作用 —— 失败路径会把 _path 清掉，不留半状态）。
    _path = path;
    if (!prepareStreams()) {
        _demux.close(); // 初始化数据不可用：不留一个半开的对象
        _path.clear();
        ErrorL << "DemuxerProducer::open 失败：" << lastError();
        return false;
    }
    _opened_at_ms.store(static_cast<int64_t>(getCurrentMillisecond()));
    _loop_offset_ms.store(0);
    _pass.assign(_demux.streams().size(), PassTrack{});
    return true;
}

const StreamInfo *DemuxerProducer::findStream(int index) const {
    for (const auto &info : _demux.streams()) {
        if (info.index == index) {
            return &info;
        }
    }
    return nullptr;
}

int64_t DemuxerProducer::estimateFrameDurationMs(const StreamInfo &info) {
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

bool DemuxerProducer::prepareStreams() {
    // 只在这里写一次快照（见头文件的说明）
    _has_video_info = false;
    _has_audio_info = false;
    _annexb_video = false;
    _adts_audio = false;
    _video_info = StreamInfo{};
    _audio_info = StreamInfo{};
    _raw_video_info = StreamInfo{};
    _raw_audio_info = StreamInfo{};

    if (const StreamInfo *video = _demux.firstVideo()) {
        _raw_video_info = *video;
        _video_info = *video;
        _has_video_info = true;

        const bool has_extra = video->extradata && !video->extradata->empty();
        if (video->codec_id == AV_CODEC_ID_H264 && has_extra &&
            looksLikeAnnexB(video->extradata->data(), video->extradata->size())) {
            // H264 裸流（.h264 / TS）：初始化数据是 Annex-B 的 SPS/PPS → 现场构造 avcC
            std::vector<uint8_t> sps;
            std::vector<uint8_t> pps;
            if (!extractSpsPps(video->extradata->data(), video->extradata->size(), &sps, &pps)) {
                setError("H264 输入是 Annex-B，但初始化数据里没有同时出现 SPS 与 PPS → "
                         "构造不出 avcC（明确失败，不用裸 SPS/PPS 冒充初始化头）");
                return false;
            }
            std::vector<uint8_t> avcc;
            if (!buildAvcC(sps.data(), sps.size(), pps.data(), pps.size(), &avcc)) {
                setError("SPS(" + std::to_string(sps.size()) + "B)/PPS(" + std::to_string(pps.size()) +
                         "B) 构造 avcC 失败（参数非法或超长）");
                return false;
            }
            _video_info.extradata = std::make_shared<const std::vector<uint8_t>>(std::move(avcc));
            _annexb_video = true;
            InfoP("DemuxerProducer: 输入是 H264 裸流（Annex-B）→ 构造 avcC %zu 字节，发包时转 AVCC",
                  _video_info.extradata->size());
        }
    }

    if (const StreamInfo *audio = _demux.firstAudio()) {
        _raw_audio_info = *audio;
        _audio_info = *audio;
        _has_audio_info = true;

        // 【M8 / FR-2.2】TS 的 AAC 是 ADTS：没有 extradata，必须**现场造 ASC**。
        // 为什么必须在 open() 之内做完：连接拿的是流快照的裸指针，AAC sequence header
        // 在建连那一刻就要发（FR-3.1 的"立即"），等第一个包到了再造就来不及了。
        const bool has_extra = audio->extradata && !audio->extradata->empty();
        if (audio->codec_id == AV_CODEC_ID_AAC && !has_extra) {
            if (!probeAdtsAudioConfig()) {
                return false;   // 原因已由 probeAdtsAudioConfig 写进 lastError（明确失败，不猜）
            }
        }
    }

    _pass.assign(_demux.streams().size(), PassTrack{});
    return true;
}

bool DemuxerProducer::probeAdtsAudioConfig() {
    // 为什么要**另开一个** Demuxer 探针，而不是用主解封装读一帧：
    //   主 Demuxer 的读游标一旦前进，读到的那些包就必须原样留到 read() 里再交付
    //   （否则首帧/首包丢失 → 播放端从半途开始）。要实现这个就得引入"重放队列"，
    //   而 ASC 又必须在 open() 之内就绪（见 prepareStreams 里的注释）。一次性探针
    //   把这两个约束都绕开了：它只读前 N 个包、读完就关，主游标一个字节都没动。
    // 代价：打开同一个文件两次（本地文件是一次 open + 读几个包，毫秒级）。
    Demuxer probe(_config.demux);
    if (!probe.open(_path)) {
        setError("探测 ADTS 头失败：无法用探针打开 " + _path + "（" + probe.lastError() + "）");
        ErrorL << "DemuxerProducer: " << lastError();
        return false;
    }
    const StreamInfo *probe_audio = probe.firstAudio();
    if (probe_audio == nullptr) {
        (void) probe.close();
        setError("探测 ADTS 头失败：探针没有看到音频流（与主解封装不一致）");
        ErrorL << "DemuxerProducer: " << lastError();
        return false;
    }
    const int want_index = probe_audio->index;

    // 有界：最多看这么多包（正常 TS 里音频包间隔只有几个包；找不到就是文件本身异常）
    constexpr int kMaxProbePackets = 512;
    AdtsHeader header;
    bool found = false;
    for (int i = 0; i < kMaxProbePackets; ++i) {
        const Demuxer::ReadResult result = probe.readPacket();
        if (result == Demuxer::ReadResult::Eof) {
            break;
        }
        if (result == Demuxer::ReadResult::Error) {
            setError("探测 ADTS 头失败：探针读包出错（" + probe.lastError() + "）");
            (void) probe.close();
            ErrorL << "DemuxerProducer: " << lastError();
            return false;
        }
        if (probe.packetStreamIndex() != want_index) {
            continue;
        }
        const AVPacket *pkt = probe.packet();
        if (pkt == nullptr || pkt->data == nullptr || pkt->size <= 0) {
            continue;
        }
        if (!parseAdtsHeader(pkt->data, static_cast<size_t>(pkt->size), &header)) {
            setError("探测 ADTS 头失败：TS 的 AAC 包不是合法 ADTS 帧（同步字 / 表外采样率 / 帧长非法）"
                     " → 造不出 AudioSpecificConfig（明确拒绝，不猜）");
            (void) probe.close();
            ErrorL << "DemuxerProducer: " << lastError();
            return false;
        }
        found = true;
        break;
    }
    (void) probe.close();
    if (!found) {
        setError("探测 ADTS 头失败：前 " + std::to_string(kMaxProbePackets) +
                 " 个包里没有 AAC 音频帧 → 造不出 AudioSpecificConfig（明确拒绝，不猜）");
        ErrorL << "DemuxerProducer: " << lastError();
        return false;
    }

    uint8_t asc[2] = {0, 0};
    if (!buildAudioSpecificConfig(header, asc)) {
        setError("由 ADTS 头构造 AudioSpecificConfig 失败（字段超出位宽）");
        ErrorL << "DemuxerProducer: " << lastError();
        return false;
    }
    // 交叉校验（两套独立路径互相印证，与 M4-b 的 SPS 交叉校验同一思路）：
    //   · 采样率**必须**一致 —— 不一致说明两套读法有一边错了，继续下去会产出音调不对的流；
    //   · 声道数只 Warn（ASC 用 ADTS 的 channel_configuration 才是权威；StreamInfo 是从 ADTS 推的，
    //     而 7.1 的 channel_configuration=7 与"8 声道"不是同一个数值）。
    const uint32_t adts_rate = aacSampleRateFromIndex(header.sampling_frequency_index);
    if (_audio_info.sample_rate > 0 && adts_rate != static_cast<uint32_t>(_audio_info.sample_rate)) {
        setError("ADTS 头声明的采样率 " + std::to_string(adts_rate) + " Hz 与解封装报的 " +
                 std::to_string(_audio_info.sample_rate) + " Hz 不一致 → 明确拒绝（不猜用哪个）");
        ErrorL << "DemuxerProducer: " << lastError();
        return false;
    }
    if (_audio_info.channels > 0 &&
        static_cast<int>(header.channel_configuration) != _audio_info.channels) {
        WarnP("DemuxerProducer: ADTS 的 channel_configuration=%u 与解封装报的 channels=%d 不同"
              "（7.1 的 channel_configuration=7 对应 8 声道，属于正常差异）→ 以 ADTS 为准",
              static_cast<unsigned>(header.channel_configuration), _audio_info.channels);
    }

    _audio_info.extradata = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{asc[0], asc[1]});
    _adts_audio = true;
    InfoP("DemuxerProducer: 音频是 ADTS（TS）→ 现场构造 AudioSpecificConfig %02X %02X"
          "（profile=%u 采样率索引=%u 声道配置=%u，头 %u 字节）→ 发包时逐包剥 ADTS 头",
          static_cast<unsigned>(asc[0]), static_cast<unsigned>(asc[1]),
          static_cast<unsigned>(header.profile),
          static_cast<unsigned>(header.sampling_frequency_index),
          static_cast<unsigned>(header.channel_configuration),
          static_cast<unsigned>(header.header_bytes));
    return true;
}

bool DemuxerProducer::sameStreamParams(const StreamInfo &a, const StreamInfo &b) {
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

void DemuxerProducer::trackPass(int index, int64_t raw_dts_ms) {
    if (index < 0 || static_cast<size_t>(index) >= _pass.size()) {
        return; // 不在快照里（理论上不该发生；宁可少记一笔也不要越界写）
    }
    PassTrack &track = _pass[static_cast<size_t>(index)];
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

int64_t DemuxerProducer::passEndMs() {
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
        // `+ kLoopGapMs`：整数毫秒下的"严格递增"间隙。真实帧长可能是小数（AAC 23.22ms），
        // 不留间隙的话第二趟的首包会与第一趟的末包落在同一毫秒上 —— 播放端未必报错，
        // 但严格的时间戳检查器会（ffmpeg 的 null muxer 就会报 non monotonically increasing dts，
        // 这是 scripts/flv_http_test.sh §9 抓到的第二个问题）。
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

bool DemuxerProducer::reopenForLoop() {
    // 偏移必须在关文件**之前**算（要用当前这趟的流信息与 dts 轨迹）
    const int64_t pass_end = passEndMs();
    if (pass_end <= 0) {
        // 空趟：重开还会立刻 EOF → 那就是死循环 + 100% CPU。明确停下来
        ++_loop_failures;
        setError("循环重开：这一趟没有读到任何包（空文件/截断）→ 停止循环");
        ErrorL << "DemuxerProducer: " << lastError();
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
        ErrorL << "DemuxerProducer: " << lastError();
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
        setError("循环重开：文件 " + _path + " 的流参数与上一趟不一致（文件被换掉了？）→ 停止");
        ErrorL << "DemuxerProducer: " << lastError();
        return false;
    }

    _loop_offset_ms.fetch_add(pass_end);
    _pass.assign(_demux.streams().size(), PassTrack{}); // 新一趟从零开始记
    ++_loops;
    InfoP("DemuxerProducer: 循环重开第 %llu 次：偏移 +%lld ms（本趟到 %lld ms）",
          static_cast<unsigned long long>(_loops.load()), static_cast<long long>(pass_end),
          static_cast<long long>(pass_end));
    return true;
}

SourcePump::ReadResult DemuxerProducer::read(MediaPacket::Ptr *packet, std::string *error) {
    if (packet == nullptr || error == nullptr) {
        setError("read 的出参为空（调用方 bug）");
        ErrorL << "DemuxerProducer::read 失败：" << lastError();
        return SourcePump::ReadResult::Error;
    }
    if (!_demux.opened()) {
        setError("尚未 open 就读包");
        ErrorL << "DemuxerProducer::read 失败：" << lastError();
        return SourcePump::ReadResult::Error;
    }

    while (true) {
        const Demuxer::ReadResult result = _demux.readPacket();
        if (result == Demuxer::ReadResult::Eof) {
            if (!_config.loop) {
                return SourcePump::ReadResult::EndOfStream;
            }
            if (!reopenForLoop()) {
                return SourcePump::ReadResult::Error; // 重开失败 = 明确失败，不伪装成"读完"
            }
            continue; // 重开成功：接着读新一趟（时间戳已由偏移接上）
        }
        if (result == Demuxer::ReadResult::Error) {
            setError(_demux.lastError()); // 如实转达（含超时/超限/损坏）
            return SourcePump::ReadResult::Error;
        }

        const AVPacket *pkt = _demux.packet();
        if (pkt == nullptr || pkt->data == nullptr || pkt->size <= 0) {
            setError("读到空包（data 为空或 size <= 0）");
            ErrorL << "DemuxerProducer::read 失败：" << lastError();
            return SourcePump::ReadResult::Error;
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
        std::shared_ptr<std::vector<uint8_t>> payload;
        if (kind == MediaKind::Video && _annexb_video) {
            // Annex-B → AVCC：转完与 MP4 输入同形，输出层因此完全不用知道输入是什么
            auto avcc = std::make_shared<std::vector<uint8_t>>();
            if (!annexBToAvcc(pkt->data, static_cast<size_t>(pkt->size), avcc.get())) {
                setError("Annex-B → AVCC 转换失败（包内没有合法起始码 / NAL 超长）");
                ErrorL << "DemuxerProducer::read 失败：" << lastError();
                return SourcePump::ReadResult::Error;
            }
            payload = std::move(avcc);
            ++_annexb_packets;
        } else if (kind == MediaKind::Audio && _adts_audio) {
            // 【M8】ADTS → raw AAC：FLV 的 AudioTag 里**不能**带 ADTS 头（那样播放端会当噪声）
            auto raw = std::make_shared<std::vector<uint8_t>>();
            if (!adtsToRawAac(pkt->data, static_cast<size_t>(pkt->size), raw.get())) {
                setError("ADTS → raw AAC 转换失败（帧不完整 / 一帧多块 / 同步字丢失）");
                ErrorL << "DemuxerProducer::read 失败：" << lastError();
                return SourcePump::ReadResult::Error;
            }
            payload = std::move(raw);
            ++_adts_packets;
        } else {
            payload = std::make_shared<std::vector<uint8_t>>(
                pkt->data, pkt->data + static_cast<size_t>(pkt->size));
        }

        // 循环重开：把这一趟的时间戳整体挪到上一趟之后（FLV 时间戳必须递增）
        const int64_t offset = _loop_offset_ms.load();
        const int64_t out_dts = dts_ms + offset;
        const int64_t out_pts = pts_ms + offset;

        const bool key_frame = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        MediaPacket::Ptr media_packet =
            MediaPacket::create(kind, stream_index, std::move(payload), key_frame, out_dts, out_pts);
        if (!media_packet) {
            setError("MediaPacket::create 失败（参数非法）");
            ErrorL << "DemuxerProducer::read 失败：" << lastError();
            return SourcePump::ReadResult::Error;
        }

        ++_packets;
        _bytes += media_packet->size();
        *packet = std::move(media_packet);
        return SourcePump::ReadResult::Packet;
    }
}

} // namespace mzmedia
