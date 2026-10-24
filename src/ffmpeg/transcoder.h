/*
 * Transcoder：解码 →（libswscale 缩放）→ 编码 x264（M10-a / NFR-5）
 * ============================================================================
 * 形状来源：docs/DESIGN_M9.md（同一条"纯函数式加工"的纪律）、docs/ROADMAP.md v0.2 的转码行、
 *           上位需求 NFR-5「转码 ≥1.0x 实时（1080p→720p，x264 veryfast）」
 *
 * 只做一件事：**吃一个 H264 压缩包（AVCC），吐一串 H264 压缩包（AVCC）**。
 *   · 不碰网络、不碰线程、不碰文件 —— 于是"转得对不对"可以交给 ffprobe 独立裁判；
 *   · 不碰音频：`SPEC.md` §9 明确"不做音频转码"，所以音频在调用方**原样透传**（remux）；
 *   · 输入输出都是 **AVCC**（不是 Annex-B）：因为下游 `FlvMuxer`/`MpegTsMuxer` 要的就是这个形状，
 *     这样"转码"对输出层完全透明（SC-2：加能力不改核心）。
 *
 * 三个刻意的选择（每条都有排除项，见 docs/DESIGN_M6.md §3.6 的同一套写法）：
 *   1) **时间基固定 1/1000**（毫秒）：本项目全链路的时间戳都是毫秒（`MediaPacket` 只有 ms），
 *      编解码器跟着用 ms 省掉一层换算；代价是精度只有 1ms（对 23.976/29.97 这类有理帧率，
 *      由编码器按 pts 自己算帧间隔，实测不产生回退）。
 *   2) **不用 B 帧**（`max_b_frames = 0`）：dts 恒等于 pts，下游的时间戳守卫/循环拼接都简单；
 *      代价是同码率下画质略低（veryfast + 无 B 帧是"直播优先"的取舍，与 NFR-5 的口径一致）。
 *   3) **当场构造 avcC**：编码器的 extradata 是 Annex-B 的 SPS/PPS，而 FLV 要
 *      `AVCDecoderConfigurationRecord` → 复用 M4-b 的 `buildAvcC()`；逐包 `annexBToAvcc()`。
 *
 * 有界性：
 *   · 一次 `transcode()` 最多吐出 `Config::max_out_packets` 个包（解码器可能一进多出，编码器有延迟），
 *     超了**明确失败**（不静默丢弃）；
 *   · `out_bitrate_kbps == 0` 时按"像素数 × 帧率"推导，并夹在 [500 kbps, 8 Mbps]（有界）。
 * ============================================================================
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ffmpeg/demuxer.h"   // StreamInfo
#include "ffmpeg/av_ptr.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace mzmedia {

class Transcoder {
public:
    using Ptr = std::shared_ptr<Transcoder>;

    struct Config {
        int out_width = 0;           ///< 0 = 跟随源宽（不缩放）
        int out_height = 0;          ///< 0 = 跟随源高（不缩放）
        int out_bitrate_kbps = 0;    ///< 0 = 按像素数×帧率推导（夹在 [500, 8000] kbps）
        int gop_frames = 50;         ///< 关键帧间隔（帧）：50 ≈ 2s@25fps
        int max_out_packets = 64;    ///< 一次 transcode() 最多吐出多少包（0 非法）
    };

    enum class Result : uint8_t {
        Ok = 0,
        BadConfig = 1,        ///< 宽高为奇数/负数、gop/max_out_packets 非法
        UnsupportedCodec = 2, ///< 只有 H264 能进（HEVC 等需要在调用方先判 CodecMatrix）
        NoDecoder = 3,        ///< 本机 FFmpeg 没有这个解码器
        NoEncoder = 4,        ///< 本机 FFmpeg 没有 H264 编码器（缺 libx264）
        DecodeFailed = 5,
        EncodeFailed = 6,
        TooManyOutputs = 7,   ///< 单次产出超过 max_out_packets（**不静默丢弃**）
        BadStream = 8,        ///< 输入包为空 / 解码器没准备好
    };

    /// 一帧编码后的产物（AVCC 字节 + 毫秒时间戳）
    struct OutPacket {
        std::shared_ptr<std::vector<uint8_t>> data;
        bool key = false;
        int64_t dts_ms = 0;
        int64_t pts_ms = 0;
    };

    /**
     * @param src_video 源视频流信息（必须有 avcC；宽高/帧率用于决定输出与码率）
     * @return nullptr = 建不起来（原因见日志；`lastError()` 只有实例才有，所以失败一律 ErrorP）
     */
    static Ptr create(const StreamInfo &src_video, const Config &config);
    static const char *resultName(Result result);
    /// @note 必须 public：`Ptr` 是 shared_ptr，它的 deleter 在任意翻译单元里调析构
    ~Transcoder();
    Transcoder(const Transcoder &) = delete;
    Transcoder &operator=(const Transcoder &) = delete;

    /**
     * 吃一个**视频压缩包**（AVCC，时间戳是毫秒），吐出 0..N 个编码后的包
     * @param avcc 长度前缀的 NAL（与 `MediaPacket` 里的视频载荷同形）
     * @note 0 个输出是**正常**的：解码器要攒帧、编码器有延迟；EOF 时用 `flush()` 收尾
     */
    Result transcode(const uint8_t *avcc, size_t size, int64_t dts_ms, int64_t pts_ms,
                     std::vector<OutPacket> *out);

    /// 冲洗解码器 + 编码器（把所有缓存的帧都变成包）；可重复调用
    Result flush(std::vector<OutPacket> *out);

    /**
     * 输出流的形状（H264 + 现场构造的 avcC + 目标宽高 + 同一 stream index）
     * @note 直接喂 `FlvMuxer::Streams{video, audio}` 即可 —— 输出层不知道这段流是转码来的
     */
    const StreamInfo &videoInfo() const {
        return _out_info;
    }

    // 观测（NFR-5 的 speed 用 outMediaMs/wallMs 算）
    uint64_t inputPackets() const {
        return _input_packets;
    }
    uint64_t decodedFrames() const {
        return _decoded_frames;
    }
    uint64_t scaledFrames() const {
        return _scaled_frames;
    }
    uint64_t encodedPackets() const {
        return _encoded_packets;
    }
    uint64_t keyPackets() const {
        return _key_packets;
    }
    /// 已产出的媒体时长（毫秒）：最大 pts − 最小 pts
    uint64_t outMediaMs() const {
        return _out_media_ms;
    }
    /// 是否需要缩放（false = 尺寸与像素格式都与目标一致，帧直接进编码器）
    bool scaling() const {
        return _sws != nullptr;
    }
    /// 是否检测到"输入帧间距不均匀"（VFR）：此时输出被**按平均帧率规整**成 CFR（已 Warn）
    bool vfrDetected() const {
        return _vfr_warned;
    }
    const std::string &lastError() const {
        return _last_error;
    }

private:
    Transcoder() = default;

    Result fail(Result result, const std::string &message);
    /// 把解码器/编码器里攒着的帧全部取出来（`drain` 的两端都要循环到 EAGAIN）
    Result drainDecoder(std::vector<OutPacket> *out);
    Result drainEncoder(std::vector<OutPacket> *out);
    /// 编码器输出的一包（Annex-B）→ AVCC，并记录时间戳
    Result appendEncodedPacket(AVPacket *packet, std::vector<OutPacket> *out);

    Config _config;
    StreamInfo _out_info;
    std::string _last_error;

    AvCodecCtx _dec;
    AvCodecCtx _enc;
    AvFrame _decoded;      ///< 解码器交出的帧（复用一个）
    AvFrame _scaled;       ///< 缩放后的帧（进编码器）
    SwsContext *_sws = nullptr;   ///< nullptr = 不需要缩放（尺寸/格式已经匹配）
    AvPacket _pkt;                ///< 送进解码器的临时包（复用）
    AvPacket _enc_pkt;            ///< 编码器输出的临时包（复用）
    bool _flushed = false;

    int _in_width = 0;
    int _in_height = 0;
    uint64_t _input_packets = 0;
    uint64_t _decoded_frames = 0;
    uint64_t _scaled_frames = 0;
    uint64_t _encoded_packets = 0;
    uint64_t _key_packets = 0;
    uint64_t _out_media_ms = 0;
    // ---- 输出时间轴：**自己生成**，不用输入的 pts（原因见 .cpp 里 drainDecoder 的注释）----
    double _frame_step_ms = 40.0;   ///< 1000 × den / num（25fps → 40ms）
    int64_t _ts_offset_ms = 0;      ///< 源第一个包的 dts（锚定起点，保证与透传音频同步）
    bool _have_offset = false;
    int64_t _frame_index = 0;       ///< 已送进编码器的帧序号（时间轴 = offset + index × step）
    int64_t _last_in_dts_ms = 0;    ///< 用来检测 VFR（间距忽大忽小）
    bool _have_last_dts = false;
    bool _vfr_warned = false;
    int64_t _first_pts_ms = -1;
    int64_t _last_pts_ms = -1;
};

} // namespace mzmedia
