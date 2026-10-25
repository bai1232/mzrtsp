/*
 * MpegTsMuxer：把 MediaPacket 封成 MPEG-TS（M9 / FR-3.2 与 FR-3.4 的地基）
 * ============================================================================
 * 形状来源：docs/DESIGN_M6.md §3.1 的 `FlvMuxer`（**同一个形状**：给它包，它还你字节），
 *          上位需求 `docs/SPEC.md` FR-3.2（HLS 用 mpegts 切片）、FR-3.4（TS 时间基 = 90kHz）。
 *
 * **本类不碰网络、不碰线程、不碰文件**：这样"字节拼得对不对"可以交给 `ffprobe` 独立裁判
 * （`examples/ts_mux_demo.cpp` + `scripts/ts_mux_test.sh`），不必把整条链路拉起来猜。
 *
 * 四条硬约束：
 *   1) **必须能独立解码**：每个切片开头要 PAT + PMT，首帧关键帧前要补 SPS/PPS
 *      —— MP4 的 H264 把 SPS/PPS 放在 avcC 里，**码流包里没有**，不补就"切片单放一片黑"；
 *   2) **音频要重新加 ADTS 头**：`MediaPacket` 里的音频是 raw AAC（FLV 要），TS 要 ADTS；
 *      头由 `StreamInfo::extradata`（AudioSpecificConfig）推出来 —— 推不出来就明确失败；
 *   3) **时间基是 90kHz**（FR-3.4）：`ms × 90`，33 bit 自然回绕；DTS/PTS 都写（有 B 帧时必须分开）；
 *   4) **PES 边界不能静默错**：视频 PES 长度写 0（不限制，规范只允许视频这样做），
 *      音频 PES 长度必须精确且 ≤ 65535，超了明确失败（不截断）。
 *
 * 有界性：单帧超过 65535 的视频 PES 是合法的（长度写 0），但**单帧字节数**仍受
 * `Demuxer::max_packet_size`（8MB）约束，这里不再另设上限；切片总量上限由 `HlsWriter` 负责。
 * ============================================================================
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ffmpeg/demuxer.h"   // StreamInfo
#include "media/media_packet.h"

namespace mzmedia {

class MpegTsMuxer {
public:
    /// 输入流的编码信息（来自 `DemuxerProducer::muxerVideo()/muxerAudio()` 的快照）
    struct Streams {
        const StreamInfo *video = nullptr; ///< 必须是 H264 且带 avcC（用来取 SPS/PPS）
        const StreamInfo *audio = nullptr; ///< 可选；给了就必须是 AAC 且带 AudioSpecificConfig
    };

    enum class Result : uint8_t {
        Ok = 0,
        NoStreams = 1,             // 既没有视频也没有音频
        UnsupportedVideoCodec = 2, // 不是 H264（HEVC 明确不做，SPEC 有决策记录）
        UnsupportedAudioCodec = 3, // 不是 AAC
        MissingExtradata = 4,      // 缺 avcC / AudioSpecificConfig —— **不猜**，直接拒绝
        InvalidPacket = 5,         // 空包 / 不属于这两路流
        TooLarge = 6,              // 音频 PES 超过 65535 / ADTS 帧长超过 13 bit
        NotPrepared = 7,           // 没 prepare 就写
    };

    /// PID 分配（写死并集中在这里：切片里 PAT/PMT 每次都要带，改了这里就全改）
    static constexpr uint16_t kPmtPid = 0x1000;
    static constexpr uint16_t kVideoPid = 0x0100;
    static constexpr uint16_t kAudioPid = 0x0101;
    static constexpr size_t kTsPacketSize = 188;

    explicit MpegTsMuxer(const Streams &streams);

    /// 校验编码与初始化数据，并把 SPS/PPS、ASC 解析出来。**失败就明确给出原因**
    Result prepare();
    bool prepared() const {
        return _prepared;
    }
    static const char *resultName(Result result);
    const std::string &lastError() const {
        return _last_error;
    }

    /**
     * 写 PAT + PMT（各一个 TS 包）
     * @note **每个 HLS 切片开头都要调一次**：切片必须能独立解码，播放器也可能从任意切片起播
     */
    Result writePatPmt(std::string *out);

    /**
     * 一个包 → PES → TS 包（保持输入顺序）
     * @note 视频**关键帧**会自动：① 前面补 SPS/PPS（Annex-B 形式）② 首个 TS 包带 PCR。
     *       PCR 只在关键帧插（间隔通常 1~2s）—— 已满足 HLS 对 PCR 间隔（≤100ms）的要求吗？
     *       **不满足时是明确可见的**：`pcrCount()` 与 `lastDts90k()` 能让调用方算出间隔。
     */
    Result writePacket(const MediaPacket &packet, std::string *out);

    // 观测（HLS 切片器用它判断 2s 边界与诊断）
    uint64_t tsPackets() const {
        return _ts_packets;
    }
    uint64_t pesPackets() const {
        return _pes_packets;
    }
    uint64_t bytesWritten() const {
        return _bytes_written;
    }
    /// 补过 SPS/PPS 的关键帧数（正常应 ≈ 关键帧数）
    uint64_t spsPpsInjected() const {
        return _sps_pps_injected;
    }
    /// 插过 PCR 的次数
    uint64_t pcrCount() const {
        return _pcr_count;
    }
    /// 最近一个包的 dts（90kHz，33 bit，**含起点偏移**）
    uint64_t lastDts90k() const {
        return _last_dts90k;
    }
    /// 负时间戳被钳到 0 的次数（正常应为 0；非 0 说明源时间轴有负值）
    /// 【v0.2.1】muxer 级的负时间戳基准（毫秒）：首包最小时间戳为负时整体抬到 0
    /// @note 它是"整体平移"，**不改变**任何两包之间的差值（音画关系因此不受影响）
    int64_t baseShiftMs() const {
        return _base_ms;
    }
    /// 移位后**仍**为负、只能钳到 0 的包数（正常应为 0；非 0 说明时间轴本身异常）
    uint64_t negativeTsClamped() const {
        return _negative_ts_clamped;
    }

private:
    Result fail(Result result, const std::string &message);
    /// 把一段 PES（含起始码）切成 188 字节的 TS 包写出去
    /// @param pcr 是否在**本 PES 的首个 TS 包**的 adaptation field 里带 PCR
    void writePes(uint16_t pid, uint8_t *cc, const uint8_t *pes, size_t len, bool pcr,
                  uint64_t pcr_base90k, std::string *out);
    /// 写一个 PSI 段（PAT/PMT）：payload 以 pointer_field 开头，其余用 0xFF 填充
    void writeSection(uint16_t pid, uint8_t *cc, const std::vector<uint8_t> &section, std::string *out);
    static uint32_t crc32Mpeg(const uint8_t *data, size_t size);

    Streams _streams;
    bool _prepared = false;
    std::string _last_error;

    std::vector<uint8_t> _sps;
    std::vector<uint8_t> _pps;
    size_t _nal_length_size = 4;   ///< avcC 的 lengthSizeMinusOne + 1
    uint8_t _audio_asc[2] = {0, 0};///< AudioSpecificConfig 的**核心 2 字节**（扩展位忽略，见 .cpp）

    uint8_t _cc_pat = 0;
    uint8_t _cc_pmt = 0;
    uint8_t _cc_video = 0;
    uint8_t _cc_audio = 0;

    uint64_t _ts_packets = 0;
    uint64_t _pes_packets = 0;
    uint64_t _bytes_written = 0;
    uint64_t _sps_pps_injected = 0;
    uint64_t _pcr_count = 0;
    uint64_t _last_dts90k = 0;
    /// 时间轴起点偏移（90kHz 刻度）：**不要从 0 开始**。
    /// 为什么：ffmpeg 的 mpegts demuxer 会把"首包 PTS = 0"当成 33 位回绕，吐出 2^33
    /// （实测 `ffprobe … packet=pts` 直接给 8589934592）。ffmpeg 自己产的 TS 也从 ~1.4s 起。
    /// 取 1s（= 90000）给播放器留出前滚空间；切片时长按 **ms 差**算，不受此偏移影响。
    static constexpr uint64_t kStartOffset90k = 90000;
    /// 负时间戳（部分 MP4 的编辑列表会给负 dts）被钳到 0 的次数
    uint64_t _negative_ts_clamped = 0;
    int64_t _base_ms = 0;      ///< 负时间戳的整体平移量（首包确定，之后不变）
    bool _base_set = false;
};

} // namespace mzmedia
