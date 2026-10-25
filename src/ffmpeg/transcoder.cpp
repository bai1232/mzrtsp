/*
 * Transcoder 实现（M10-a / NFR-5）
 * ============================================================================
 * 数据流：AVCC 包 → avcodec(解码) → AVFrame → [libswscale] → AVFrame(YUV420P)
 *        → avcodec(x264) → AVPacket(Annex-B) → avccToAnnexB 的逆变换 → AVCC 包
 *
 * "一进多出"是正常的：解码器可能攒几帧才吐一帧，编码器也有延迟 → 所以每次喂包后都要
 * 把两端都 drain 到 EAGAIN，EOF 时再用 flush() 把残余冲干净。漏掉任何一段都会表现为
 * "最后几帧丢了"（播放端看到时长偏短），而且**不报错** —— 这类静默丢帧是本项目最怕的。
 * ============================================================================
 */

#include "ffmpeg/transcoder.h"

#include "core/logger.h"
#include "ffmpeg/h264_util.h"

#include <cstring>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

namespace mzmedia {

namespace {

constexpr int kMinBitrateKbps = 500;
constexpr int kMaxBitrateKbps = 8000;

/// yuv420p 要求宽高为偶数：向下取偶（宁可少 1 像素，也不让编码器拒绝）
int evenify(int value) {
    if (value <= 0) {
        return 0;
    }
    return (value & 1) ? (value - 1) : value;
}

/// 码率推导：像素数 × 帧率 × 0.10 bit（经验值），夹在 [500k, 8M] —— 有界，且"0 不是无界"
int deriveBitrateKbps(int width, int height, TimeBase fps) {
    double rate = 25.0;
    if (fps.num > 0 && fps.den > 0) {
        const double v = static_cast<double>(fps.num) / static_cast<double>(fps.den);
        if (v > 0.0 && v <= 120.0) {
            rate = v;
        }
    }
    const double bps = static_cast<double>(width) * static_cast<double>(height) * rate * 0.10;
    int kbps = static_cast<int>(bps / 1000.0);
    if (kbps < kMinBitrateKbps) {
        kbps = kMinBitrateKbps;
    }
    if (kbps > kMaxBitrateKbps) {
        kbps = kMaxBitrateKbps;
    }
    return kbps;
}

std::string avErr(int err) {
    char buf[128] = {0};
    if (av_strerror(err, buf, sizeof(buf)) < 0) {
        return "errno=" + std::to_string(err);
    }
    return std::string(buf) + "(" + std::to_string(err) + ")";
}

} // namespace

Transcoder::~Transcoder() {
    if (_sws != nullptr) {
        sws_freeContext(_sws); // 唯一需要手动释放的东西（其它都走 AvPtr）
        _sws = nullptr;
    }
}

const char *Transcoder::resultName(Result result) {
    switch (result) {
    case Result::Ok:
        return "Ok";
    case Result::BadConfig:
        return "BadConfig";
    case Result::UnsupportedCodec:
        return "UnsupportedCodec";
    case Result::NoDecoder:
        return "NoDecoder";
    case Result::NoEncoder:
        return "NoEncoder";
    case Result::DecodeFailed:
        return "DecodeFailed";
    case Result::EncodeFailed:
        return "EncodeFailed";
    case Result::TooManyOutputs:
        return "TooManyOutputs";
    case Result::BadStream:
        return "BadStream";
    }
    return "unknown";
}

Transcoder::Result Transcoder::fail(Result result, const std::string &message) {
    _last_error = message;
    ErrorP("Transcoder: %s（%s）", resultName(result), message.c_str());
    return result;
}

Transcoder::Ptr Transcoder::create(const StreamInfo &src_video, const Config &config) {
    // 【M10-b】可接受的输入：H264（任何容器）与 HEVC（**必须带 hvcC 初始化数据**，即 MP4/MKV）。
    // HEVC 的 Annex-B 裸流（TS）不在这里支持：那需要现场构造 hvcC（与 avcC 对称的另一套格式），
    // 没有实测样本前不做 —— 明确拒绝，不猜。
    const bool is_h264 = src_video.codec_id == AV_CODEC_ID_H264;
    const bool is_hevc = src_video.codec_id == AV_CODEC_ID_HEVC;
    if (!is_h264 && !is_hevc) {
        ErrorP("Transcoder: 只接受 H264 / HEVC 输入（当前「%s」）—— 其它编码应由 CodecMatrix 判定/拒绝",
               src_video.codec_name.c_str());
        return nullptr;
    }
    if (!src_video.extradata || src_video.extradata->empty()) {
        ErrorP("Transcoder: 源视频缺初始化数据（H264 要 avcC，HEVC 要 hvcC）—— 没它解不了码");
        return nullptr;
    }
    if (config.max_out_packets <= 0 || config.gop_frames <= 0) {
        ErrorP("Transcoder: 配置非法（max_out_packets=%d gop_frames=%d）—— 0 不是无界",
               config.max_out_packets, config.gop_frames);
        return nullptr;
    }
    const bool has_w = config.out_width > 0;
    const bool has_h = config.out_height > 0;
    if (has_w != has_h) {
        ErrorP("Transcoder: out_width/out_height 要么都给要么都不给（现在 %dx%d）", config.out_width,
               config.out_height);
        return nullptr;
    }
    int out_w = has_w ? config.out_width : src_video.width;
    int out_h = has_h ? config.out_height : src_video.height;
    if (out_w <= 0 || out_h <= 0) {
        ErrorP("Transcoder: 拿不到输出尺寸（out=%dx%d，源=%dx%d）", config.out_width,
               config.out_height, src_video.width, src_video.height);
        return nullptr;
    }
    const int even_w = evenify(out_w);
    const int even_h = evenify(out_h);
    if (even_w != out_w || even_h != out_h) {
        WarnP("Transcoder: 目标 %dx%d 不是偶数 → 取偶为 %dx%d（yuv420p 要求）", out_w, out_h, even_w,
              even_h);
    }
    out_w = even_w;
    out_h = even_h;

    auto self = Ptr(new Transcoder());
    self->_config = config;

    // ---------------- 解码器 ----------------
    // 【M10-b】解码器跟着**输入编码**走：H264 源用 H264 解码器，HEVC 源用 HEVC 解码器。
    // 输出永远是新编码的 H264（浏览器只认这个），所以"输入编码"只影响这一段。
    const AVCodec *decoder = avcodec_find_decoder(src_video.codec_id);
    if (decoder == nullptr) {
        ErrorP("Transcoder: 本机 FFmpeg 没有「%s」解码器（无法解码该源）",
               src_video.codec_name.c_str());
        return nullptr;
    }
    self->_dec.reset(avcodec_alloc_context3(decoder));
    if (!self->_dec) {
        ErrorP("Transcoder: avcodec_alloc_context3(decoder) 失败");
        return nullptr;
    }
    {
        AVCodecContext &dec = *self->_dec;
        // 时间基固定 1/1000：我们的时间戳本来就是毫秒（见头文件"三个刻意的选择"）
        dec.pkt_timebase = AVRational{1, 1000};
        const size_t extra_size = src_video.extradata->size();
        dec.extradata = static_cast<uint8_t *>(
            av_mallocz(extra_size + AV_INPUT_BUFFER_PADDING_SIZE));
        if (dec.extradata == nullptr) {
            ErrorP("Transcoder: 拷贝初始化数据（avcC/hvcC）失败（内存不足？）");
            return nullptr;
        }
        std::memcpy(dec.extradata, src_video.extradata->data(), extra_size);
        dec.extradata_size = static_cast<int>(extra_size);
        const int ret = avcodec_open2(&dec, decoder, nullptr);
        if (ret < 0) {
            ErrorP("Transcoder: 解码器打开失败：%s", avErr(ret).c_str());
            return nullptr;
        }
    }

    // ---------------- 编码器 ----------------
    const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (encoder == nullptr) {
        ErrorP("Transcoder: 本机 FFmpeg 没有 H264 编码器（缺 libx264）");
        return nullptr;
    }
    self->_enc.reset(avcodec_alloc_context3(encoder));
    if (!self->_enc) {
        ErrorP("Transcoder: avcodec_alloc_context3(encoder) 失败");
        return nullptr;
    }
    const int bitrate_kbps = (config.out_bitrate_kbps > 0)
                                 ? config.out_bitrate_kbps
                                 : deriveBitrateKbps(out_w, out_h, src_video.frame_rate);
    int framerate_num = 25;
    int framerate_den = 1;
    if (src_video.frame_rate.num > 0 && src_video.frame_rate.den > 0) {
        framerate_num = src_video.frame_rate.num;
        framerate_den = src_video.frame_rate.den;
    }
    {
        AVCodecContext &enc = *self->_enc;
        enc.width = out_w;
        enc.height = out_h;
        enc.pix_fmt = AV_PIX_FMT_YUV420P;
        enc.time_base = AVRational{1, 1000};
        enc.framerate = AVRational{framerate_num, framerate_den};   // libav 的类型，两套别混
        enc.gop_size = config.gop_frames;
        enc.max_b_frames = 0;   // 直播优先：dts 恒等于 pts（见头文件）
        enc.bit_rate = static_cast<int64_t>(bitrate_kbps) * 1000;
        enc.thread_count = 0;   // 让 x264 自己按核数分配
        // 让 SPS/PPS 进 extradata（这样我们才能构造 avcC；而不是每帧都重复带）
        enc.flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        (void) av_opt_set(enc.priv_data, "preset", "veryfast", 0);
        (void) av_opt_set(enc.priv_data, "tune", "zerolatency", 0);
        const int ret = avcodec_open2(&enc, encoder, nullptr);
        if (ret < 0) {
            ErrorP("Transcoder: 编码器打开失败：%s", avErr(ret).c_str());
            return nullptr;
        }
        if (enc.extradata == nullptr || enc.extradata_size <= 0) {
            ErrorP("Transcoder: 编码器没有给出 extradata（拿不到 SPS/PPS）");
            return nullptr;
        }
    }

    // ---------------- 构造 avcC（FLV/HLS 要的形状） ----------------
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    std::vector<uint8_t> avcc;
    AVCodecContext &enc = *self->_enc;
    if (extractSpsPps(enc.extradata, static_cast<size_t>(enc.extradata_size), &sps, &pps)) {
        if (!buildAvcC(sps.data(), sps.size(), pps.data(), pps.size(), &avcc)) {
            ErrorP("Transcoder: 由 SPS(%zuB)/PPS(%zuB) 构造 avcC 失败", sps.size(), pps.size());
            return nullptr;
        }
    } else if (parseAvcC(enc.extradata, static_cast<size_t>(enc.extradata_size), &sps, &pps)) {
        // 有些构建直接给 avcC：那就原样用
        avcc.assign(enc.extradata, enc.extradata + enc.extradata_size);
    } else {
        ErrorP("Transcoder: 编码器的 extradata 既不是 Annex-B 也不是 avcC");
        return nullptr;
    }

    self->_out_info = StreamInfo{};
    self->_out_info.index = src_video.index; // **保持同一个 stream index**：MediaPacket 靠它区分流
    self->_out_info.codec_id = AV_CODEC_ID_H264;
    self->_out_info.codec_name = "h264";
    self->_out_info.is_video = true;
    self->_out_info.width = out_w;
    self->_out_info.height = out_h;
    self->_out_info.time_base = TimeBase{1, 1000};
    self->_out_info.frame_rate = TimeBase{static_cast<int64_t>(enc.framerate.num),
                                          static_cast<int64_t>(enc.framerate.den)};
    self->_out_info.extradata = std::make_shared<const std::vector<uint8_t>>(std::move(avcc));

    self->_decoded = makeFrame();
    self->_scaled = makeFrame();
    self->_pkt = makePacket();
    self->_enc_pkt = makePacket();
    if (!self->_decoded || !self->_scaled || !self->_pkt || !self->_enc_pkt) {
        ErrorP("Transcoder: 分配帧/包失败");
        return nullptr;
    }
    self->_in_width = src_video.width;
    self->_in_height = src_video.height;
    // 输出时间轴的步长：1000 × den / num（25fps → 40ms；30000/1001 → 33.366…ms）
    self->_frame_step_ms = 1000.0 * static_cast<double>(framerate_den) /
                           static_cast<double>(framerate_num > 0 ? framerate_num : 25);

    InfoP("Transcoder: 就绪 —— %s %dx%d → h264 %dx%d，%d kbps，preset=veryfast tune=zerolatency，"
          "GOP=%d，时间基 1/1000，avcC %zu 字节",
          src_video.codec_name.c_str(), src_video.width, src_video.height, out_w, out_h,
          bitrate_kbps, config.gop_frames, self->_out_info.extradata->size());
    return self;
}

Transcoder::Result Transcoder::appendEncodedPacket(AVPacket *packet, std::vector<OutPacket> *out) {
    std::vector<uint8_t> avcc;
    if (!annexBToAvcc(packet->data, static_cast<size_t>(packet->size), &avcc)) {
        return fail(Result::EncodeFailed, "编码器输出无法转成 AVCC（起始码/长度不对）");
    }
    if (out->size() >= static_cast<size_t>(_config.max_out_packets)) {
        return fail(Result::TooManyOutputs,
                    "单次产出超过 " + std::to_string(_config.max_out_packets) +
                        " 个包（不静默丢弃；调大 max_out_packets 或检查输入）");
    }
    OutPacket item;
    item.data = std::make_shared<std::vector<uint8_t>>(std::move(avcc));
    item.key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
    item.dts_ms = packet->dts;
    item.pts_ms = packet->pts;
    ++_encoded_packets;
    if (item.key) {
        ++_key_packets;
    }
    // 媒体时长 = 最大 pts − 最小 pts（NFR-5 的 speed = 媒体时长 / 墙钟）
    if (_first_pts_ms < 0) {
        _first_pts_ms = item.pts_ms;
    }
    if (item.pts_ms > _last_pts_ms) {
        _last_pts_ms = item.pts_ms;
        _out_media_ms = static_cast<uint64_t>(_last_pts_ms - _first_pts_ms);
    }
    out->push_back(std::move(item));
    av_packet_unref(packet);
    return Result::Ok;
}

Transcoder::Result Transcoder::drainEncoder(std::vector<OutPacket> *out) {
    for (;;) {
        const int ret = avcodec_receive_packet(_enc.get(), _enc_pkt.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return Result::Ok;
        }
        if (ret < 0) {
            return fail(Result::EncodeFailed, "receive_packet 失败：" + avErr(ret));
        }
        const Result appended = appendEncodedPacket(_enc_pkt.get(), out);
        if (appended != Result::Ok) {
            return appended;
        }
    }
}

Transcoder::Result Transcoder::drainDecoder(std::vector<OutPacket> *out) {
    for (;;) {
        const int ret = avcodec_receive_frame(_dec.get(), _decoded.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return Result::Ok;
        }
        if (ret < 0) {
            return fail(Result::DecodeFailed, "receive_frame 失败：" + avErr(ret));
        }
        ++_decoded_frames;
        AVFrame *frame = _decoded.get();

        // 第一帧之后才确定解码器的像素格式（有些流要解一帧才知道）→ 懒创建 sws
        if (_sws == nullptr && (frame->width != _out_info.width || frame->height != _out_info.height ||
                                frame->format != AV_PIX_FMT_YUV420P)) {
            _sws = sws_getContext(frame->width, frame->height,
                                  static_cast<AVPixelFormat>(frame->format), _out_info.width,
                                  _out_info.height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr,
                                  nullptr, nullptr);
            if (_sws == nullptr) {
                return fail(Result::EncodeFailed, "sws_getContext 失败（缩放器建不起来）");
            }
            _in_width = frame->width;
            _in_height = frame->height;
            InfoP("Transcoder: 需要缩放 %dx%d(format=%d) → %dx%d(yuv420p)", _in_width, _in_height,
                  frame->format, _out_info.width, _out_info.height);
        }

        // 输出时间轴由**我们**生成：offset + 帧序号 × 步长。
        // 不用解码器给的 pts（`best_effort_timestamp` 来自输入 pts，而输入的 pts 被钳过）。
        const int64_t pts = _ts_offset_ms + static_cast<int64_t>(_frame_index * _frame_step_ms);
        const AVFrame *to_encode = frame;
        if (_sws != nullptr) {
            av_frame_unref(_scaled.get());
            _scaled->format = AV_PIX_FMT_YUV420P;
            _scaled->width = _out_info.width;
            _scaled->height = _out_info.height;
            if (av_frame_get_buffer(_scaled.get(), 0) < 0) {
                av_frame_unref(_decoded.get());
                return fail(Result::EncodeFailed, "av_frame_get_buffer 失败（缩放目标帧）");
            }
            sws_scale(_sws, frame->data, frame->linesize, 0, frame->height, _scaled->data,
                      _scaled->linesize);
            _scaled->pts = pts;
            ++_scaled_frames;
            to_encode = _scaled.get();
        } else {
            frame->pts = pts; // 不缩放时直接把时间轴写在解码帧上（编码器只读它）
        }
        ++_frame_index;
        const int sret = avcodec_send_frame(_enc.get(), to_encode);
        av_frame_unref(_decoded.get()); // 解码帧的生命周期到此为止（编码器已收下/已复制）
        if (sret < 0) {
            return fail(Result::EncodeFailed, "send_frame 失败：" + avErr(sret));
        }
        const Result drained = drainEncoder(out);
        if (drained != Result::Ok) {
            return drained;
        }
    }
}

Transcoder::Result Transcoder::transcode(const uint8_t *avcc, size_t size, int64_t dts_ms,
                                         int64_t pts_ms, std::vector<OutPacket> *out) {
    if (out == nullptr) {
        return fail(Result::BadStream, "out 为空");
    }
    if (avcc == nullptr || size == 0) {
        return fail(Result::BadStream, "空包（size == 0）");
    }
    if (!_dec || !_enc || !_pkt) {
        return fail(Result::BadStream, "编解码器没准备好");
    }
    if (size > static_cast<size_t>(INT32_MAX)) {
        return fail(Result::BadStream, "单包过大");
    }
    av_packet_unref(_pkt.get());
    const int alloc_ret = av_new_packet(_pkt.get(), static_cast<int>(size));
    if (alloc_ret < 0) {
        return fail(Result::DecodeFailed, "av_new_packet 失败：" + avErr(alloc_ret));
    }
    std::memcpy(_pkt->data, avcc, size); // av_new_packet 保证尾部有 AV_INPUT_BUFFER_PADDING_SIZE 的 0
    // 【关键】只把 **dts** 交给解码器；pts 让解码器自己从码流里还原（B 帧的显示顺序是它的事）。
    // 为什么不能用调用方给的 pts：本项目 `Demuxer` 的时间戳**单调守卫**会钳制"乱序"的 pts
    // （实测：源 pts 0,160,80,40,120 → 被钳成 0,160,160,160,160），把它喂给编码器会得到
    // 非递增的输入时间戳 → 输出的 dts 乱序（ffmpeg 报 non monotonically increasing dts）。
    _pkt->pts = AV_NOPTS_VALUE;
    _pkt->dts = dts_ms;
    (void) pts_ms; // 有意不用（原因见上）
    if (!_have_offset) {
        _ts_offset_ms = dts_ms; // 锚定源起点：保证与"透传的音频"共用同一条时间轴
        _have_offset = true;
    }
    // VFR 检测：相邻 dts 间距偏离标称帧步长 >10% 就 Warn 一次（**不静默规整**）
    if (_have_last_dts) {
        const double delta = static_cast<double>(dts_ms - _last_in_dts_ms);
        if (delta > 0.0 && _frame_step_ms > 0.0 &&
            (delta < _frame_step_ms * 0.9 || delta > _frame_step_ms * 1.1)) {
            if (!_vfr_warned) {
                _vfr_warned = true;
                WarnP("Transcoder: 输入帧间距 %.1f ms ≠ 标称 %.1f ms（VFR？）→ 输出时间轴按标称帧率"
                      "重新生成（CFR）；音画同步以起点为准", delta, _frame_step_ms);
            }
        }
    }
    _last_in_dts_ms = dts_ms;
    _have_last_dts = true;
    ++_input_packets;
    const int ret = avcodec_send_packet(_dec.get(), _pkt.get());
    if (ret < 0) {
        return fail(Result::DecodeFailed, "send_packet 失败：" + avErr(ret));
    }
    return drainDecoder(out);
}

Transcoder::Result Transcoder::flush(std::vector<OutPacket> *out) {
    if (out == nullptr) {
        return fail(Result::BadStream, "out 为空");
    }
    if (_flushed) {
        return Result::Ok; // 幂等
    }
    if (_dec) {
        const int ret = avcodec_send_packet(_dec.get(), nullptr); // EOF
        if (ret < 0 && ret != AVERROR_EOF) {
            return fail(Result::DecodeFailed, "flush 解码器失败：" + avErr(ret));
        }
        const Result drained = drainDecoder(out);
        if (drained != Result::Ok) {
            return drained;
        }
    }
    if (_enc) {
        const int ret = avcodec_send_frame(_enc.get(), nullptr); // EOF
        if (ret < 0 && ret != AVERROR_EOF) {
            return fail(Result::EncodeFailed, "flush 编码器失败：" + avErr(ret));
        }
        const Result drained = drainEncoder(out);
        if (drained != Result::Ok) {
            return drained;
        }
    }
    _flushed = true;
    return Result::Ok;
}

} // namespace mzmedia
