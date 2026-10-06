/*
 * FlvMuxer：把编码后的包拼成 FLV 字节流（M6-a）
 * ============================================================================
 * 形状来源：docs/DESIGN_M6.md §3；上位需求 `docs/SPEC.md`
 *   FR-3.1「`Content-Type: video/x-flv`，chunked；连接建立后**立即**发送 FLV Header +
 *           AVC sequence header（SPS/PPS），随后持续推流」
 *   FR-3.3「AAC 转发（AAC sequence header + raw AAC）；无音频输入时输出**纯视频流**，不得报错」
 *   FR-3.4「时间戳统一转换到输出格式基准（FLV → 毫秒）」
 *
 * **本类不碰网络、不碰线程、不碰文件**：给它包，它还你字节。这样"字节拼得对不对"可以用
 * `ffprobe` 独立验证（DESIGN_M6.md §5），不用把整条链路拉起来猜。
 *
 * 四条硬约束：
 *   1) **没有初始化头就不开工**：H264 必须有 avcC（`AVCDecoderConfigurationRecord`）、
 *      AAC 必须有 AudioSpecificConfig —— 缺了就是"能连上但一片黑/无声"，宁可 `prepare()` 直接失败；
 *   2) **不做格式转换**：M6-a 只吃 MP4/avcC 的 length-prefixed 包（FLV 也正是这个形状）。
 *      Annex-B（H264 裸流）会在这里被**明确拒绝**（不是静默出坏流）—— 转换在 M6-c；
 *   3) **时间戳是 32 位毫秒**：`dts - base` 后按 uint32 自然回绕（FLV 规范如此），
 *      而 `CompositionTime = pts - dts` 是 24 位**有符号**，超范围要报错而不是截断；
 *   4) **早于基准的包不产生负时间戳**：钳到 0 并**计数 + 告警一次**（静默回绕会让播放端看到
 *      40 亿这种时间戳，属于最难查的那类问题）。
 *
 * 每个客户端一份的**时间戳基准**（ARCHITECTURE.md §4）：FLV 要求时间戳从 0 附近开始递增，
 * 而各客户端接入时刻不同 —— 所以 `FlvMuxer` 是**每订阅者一个实例**，基准用首个包的 dts 自动定，
 * 也可以用 `setTimestampBaseMs()` 显式指定。
 * ============================================================================
 */

#pragma once

#include "ffmpeg/demuxer.h"
#include "media/media_packet.h"

#include <cstdint>
#include <string>

namespace mzmedia {

class FlvMuxer {
public:
    /// 输入流的编码信息（来自 `Demuxer::streams()`）
    struct Streams {
        const StreamInfo *video = nullptr; ///< 必须是 H264 且带 avcC；无视频流时为 nullptr
        const StreamInfo *audio = nullptr; ///< 可选；给了就必须是 AAC 且带 AudioSpecificConfig
    };

    enum class Result : uint8_t {
        Ok = 0,
        NoStreams = 1,             // 既没有视频也没有音频
        UnsupportedVideoCodec = 2, // 不是 H264（HEVC 明确不做，SPEC 有决策记录）
        UnsupportedAudioCodec = 3, // 不是 AAC
        MissingExtradata = 4,      // 缺 avcC / AudioSpecificConfig —— **不猜**，直接拒绝
        AnnexBNotSupported = 5,    // 初始化数据看着像 Annex-B（H264 裸流）：M6-a 不做转换
        NotPrepared = 6,           // 没 prepare 就写
        InvalidPacket = 7,         // 空包 / 包不属于这两路流 / 超出 FLV 字段宽度
        TooLarge = 8,              // tag 数据超过 24 位长度上限（16MB-1）
    };

    explicit FlvMuxer(const Streams &streams);
    ~FlvMuxer() = default;
    FlvMuxer(const FlvMuxer &) = delete;
    FlvMuxer &operator=(const FlvMuxer &) = delete;

    /// 校验编码与初始化数据。**失败就明确给出原因**（绝不"先答应、之后每帧都失败"）
    Result prepare();
    bool prepared() const {
        return _prepared;
    }
    std::string lastError() const {
        return _last_error;
    }
    /// 结果码 → 可读名字（日志与测试都用它，避免只看到数字）
    static const char *resultName(Result result);

    // ---- 每客户端一份的时间戳基准 ----
    /// 显式设置基准（毫秒）；不设置时用**首个包**的 dts 自动定基准
    void setTimestampBaseMs(int64_t dts_ms);
    int64_t timestampBaseMs() const {
        return _base_dts_ms;
    }
    bool baseFixed() const {
        return _base_fixed;
    }

    /// FLV Header（9 字节）+ PreviousTagSize0（4 字节）
    bool writeHeader(std::string *out) const;
    /// AVC / AAC sequence header（各一个 tag）。**必须紧跟 header**（FR-3.1）
    Result writeSequenceHeaders(std::string *out);
    /// 一帧 → 一个 tag
    Result writePacket(const MediaPacket &packet, std::string *out);

    // 观测
    uint64_t tagCount() const {
        return _tag_count;
    }
    /// 累计写出的 tag 字节数（含 11 字节 tag 头与 4 字节 PreviousTagSize，**不含 FLV Header**）
    uint64_t bytesWritten() const {
        return _bytes_written;
    }
    uint32_t lastTimestampMs() const {
        return _last_timestamp;
    }
    /// 有多少个包因为早于基准被钳到 0（正常文件应当是 0）
    uint64_t clampedBeforeBase() const {
        return _clamped_before_base;
    }

private:
    Result appendTag(uint8_t type, uint32_t timestamp_ms, const std::string &data, std::string *out);
    Result writeVideoTag(const MediaPacket &packet, std::string *out);
    Result writeAudioTag(const MediaPacket &packet, std::string *out);
    uint32_t timestampFor(int64_t dts_ms);
    Result fail(Result result, const std::string &message);

    Streams _streams;
    bool _prepared = false;
    std::string _last_error;

    int64_t _base_dts_ms = 0;
    bool _base_fixed = false;

    uint64_t _tag_count = 0;
    uint64_t _bytes_written = 0;
    uint32_t _last_timestamp = 0;
    uint64_t _clamped_before_base = 0;
};

} // namespace mzmedia
