/*
 * DemuxerProducer：**编排**（v0.2.3 重构后只剩这一件事）
 * ============================================================================
 * 它曾经是"读 + 输入形状转换 + 转码编排"的合并体（789 行 / 22 个成员函数），
 * 每加一种输入形状或能力都要改它（M8 +125 行、M10-b +260 行）—— 违反开闭原则。
 *
 * 现在它只做三件事：
 *   1) **搭链**：`buildChains()` 为视频/音频各搭一条 `PacketStage` 链
 *      （Raw / AnnexB / Adts / Transcode 的组合）；
 *   2) **驱动**：`read()` 从 `SourceSession` 拿原始包 → 过链 → 产物排队 → 一次交一个；
 *   3) **转发观测**：老的 getter 全部保留（对外接口不变），内部转发给 session/阶段的计数。
 *
 * 扩展点（**加能力时只该动这里一行**）：
 *   · 新的输入形状（或 v0.3 的"滤镜/水印"）→ 新写一个 `PacketStage`，在 `buildChains()` 里加一支；
 *   · 新的"读"能力（RTSP 重连）→ 动 `SourceSession`，与阶段无关。
 *   `media/` 的分发层、`output/`、`http/` 都不受影响。
 *
 * 三条对外语义（自 v0.1 起没变，将来也不该变）：
 *   · 每次 `read()` 只交一个包（没有就先攒着/继续读，绝不交空包）；
 *   · 结局如实转达：Packet / EndOfStream（稳定终态）/ Error（带人话原因）；
 *   · 失败一律**明确**（不静默降级、不猜、不吞下层原因）。
 * ============================================================================
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ffmpeg/demuxer.h"
#include "media/media_packet.h"
#include "media/packet_stage.h"
#include "media/source_pump.h"
#include "media/source_session.h"

namespace mzmedia {

class TranscodeStage;

class DemuxerProducer {
public:
    /// 转码配置（【M10-b】转码 = 一路源一份，跑在**源线程**上）
    struct TranscodeConfig {
        /// 默认 **关**：不转码时行为与 v0.1 完全一致（HEVC 仍然 415，绝不静默降级）
        bool enabled = false;
        /// 目标高度：0 = **不缩放**（只重编码）；>0 = 等比缩到该高度（必须偶数、不高过源）
        int out_height = 0;
        /// **上限**高度：源高于它就缩到它，源不高于它就**不缩放**（0 = 不设上限）
        int max_height = 0;
        /// 目标码率（kbps）：0 = 由像素数×帧率推导并夹在 [500, 8000]（有界）
        int out_bitrate_kbps = 0;
        /// 关键帧间隔（帧）
        int gop_frames = 50;
        /// 单次转码最多吐出多少包（0 非法）
        int max_out_packets = 64;
    };

    /// 【M11-a】RTSP 输入（网络源）的开关与打开参数
    struct RtspConfig {
        /// 默认 **关**：不带 `--rtsp` 时，`rtsp://` 源一律明确拒绝（不静默尝试联网）
        bool enabled = false;
        /// 网络流的读超时（毫秒）：一次读超过这么久没有包 = 断流（默认 10s，见 DESIGN_M11 §4.3）
        int timeout_ms = 10000;
        /// 传输方式：`auto`（FFmpeg 默认：UDP 优先、被拒自动回退 TCP）/ `tcp` / `udp`
        enum class Transport { Auto = 0, Tcp = 1, Udp = 2 };
        Transport transport = Transport::Auto;
        /// 【M11-a2】断流重连（网络源）：退避 1→2→4→8→16→封顶 30s，读到第一个包才复位
        bool reconnect_enabled = true;
        int64_t backoff_initial_ms = 1000;
        int64_t backoff_max_ms = 30000;
        /// 0 = 不设上限（一直退避重试，但每次都有计数与日志）
        int max_attempts = 0;
    };

    struct Config {
        Demuxer::Limits demux; ///< 流数 / 单包 / open 与 read 超时
        /// 循环重开（M6-c）：读到文件尾把文件重新打开，时间戳接着往后接
        bool loop = false;
        /// 【M11-a】RTSP（网络源）
        RtspConfig rtsp;
        /// 【M10-b】转码
        TranscodeConfig transcode;
        /// 兼容别名：老代码写 `Config::Transcode` 的地方继续可用
        using Transcode = TranscodeConfig;
    };

    explicit DemuxerProducer(const Config &config);
    DemuxerProducer();
    ~DemuxerProducer() = default;
    DemuxerProducer(const DemuxerProducer &) = delete;
    DemuxerProducer &operator=(const DemuxerProducer &) = delete;

    /// @return false = 打不开 / 输入形状不支持 / 转码建不起来（原因见 lastError()）
    bool open(const std::string &path);
    bool opened() const;
    const std::string &path() const;
    /// 只读访问**源**解封装（排查/观测用；输出层请用 muxerVideo()/muxerAudio()）
    const Demuxer &demuxer() const;

    /// 绑定外部中止标志（通常是 `SourcePump::stopFlag()`）；@return 上一个标志
    const std::atomic<bool> *setAbortFlag(const std::atomic<bool> *flag);

    /// **直接作为 `SourcePump::ReadFn` 使用**
    SourcePump::ReadResult read(MediaPacket::Ptr *packet, std::string *error);

    // -----------------------------------------------------------------------
    // 输出层接口：给的是**本对象持有的副本**（连接会长期持有这两个裸指针）
    // -----------------------------------------------------------------------
    const StreamInfo *muxerVideo() const {
        return _has_video_info ? &_video_info : nullptr;
    }
    const StreamInfo *muxerAudio() const {
        return _has_audio_info ? &_audio_info : nullptr;
    }
    /// 输入视频是否为 Annex-B（即链上装了 AnnexB 阶段）
    bool annexBVideo() const {
        return _annexb_video;
    }
    /// 【M8】音频是否是 ADTS（即链上装了 Adts 阶段）
    bool adtsAudio() const {
        return _adts_audio;
    }

    // ---- 观测（**可在其他线程读**）----
    uint64_t totalPackets() const {
        return _packets.load();
    }
    uint64_t totalBytes() const {
        return _bytes.load();
    }
    /// 【M7 / FR-6.1】本源的**平均码率**（bit/s）= 累计**输出**字节 × 8 / 已运行秒数
    uint64_t bitrateBps() const;
    uint64_t openedAtMs() const;
    uint64_t skippedUnknownStreams() const;
    uint64_t timestampFailures() const;
    uint64_t annexBPackets() const;
    uint64_t adtsPackets() const;
    uint64_t ptsOutOfOrder() const;
    uint64_t dtsClamped() const;
    uint64_t loopCount() const;
    uint64_t loopFailures() const;
    int64_t loopOffsetMs() const;

    // ---- 【M10-b】转码观测（未启用时全部返回"关闭"语义，不制造数字）----
    bool transcodeEnabled() const {
        return _transcode != nullptr;
    }
    bool transcodeInputHevc() const;
    int transcodeOutWidth() const;
    int transcodeOutHeight() const;
    uint64_t transcodeInputs() const;
    uint64_t transcodeOutputs() const;
    uint64_t transcodeOutMediaMs() const;
    int64_t transcodeCostMs() const;
    int64_t transcodeElapsedMs() const;
    double transcodeSpeed() const;
    bool transcodeVfr() const;
    bool transcodeScaling() const;
    /// JSON 片段（不含外层花括号）：供 `/api/stats` 直接拼接
    std::string transcodeStatsJson() const;
    /// 【M11-a】`"rtsp":{...}` 或 `"rtsp":null`（本地文件源）
    std::string rtspStatsJson() const;

    /// 是不是网络源（`rtsp://`）；非网络源时 `rtspStatsJson()` 返回 `null`
    bool networkSource() const {
        return _network;
    }

    std::string lastError() const;

private:
    /// 为视频/音频各搭一条链（**唯一的"策略选择处"**：加一种输入形状 = 这里加一支）
    bool buildChains();
    /// 在视频链尾追加转码阶段（含几条"明确拒绝"的前置校验）
    bool appendTranscodeStage();
    /// 把链上第 `index` 个阶段套到一批包上（`current` 原地替换）
    bool runStage(PacketStage &stage, std::vector<MediaPacket::Ptr> *current,
                  std::string *error);
    SourcePump::ReadResult servePending(MediaPacket::Ptr *packet);
    void setError(const std::string &message);
    /// 链里是否有阶段自己拥有时间轴（转码）→ 有的话生产者**不加**趟偏移
    bool chainOwnsTimeline(bool video) const;

    Config _config;
    SourceSession _session;
    /// 视频/音频各一条链（顺序即加工顺序）
    std::vector<PacketStage::Ptr> _video_chain;
    std::vector<PacketStage::Ptr> _audio_chain;
    /// 转码阶段的非拥有指针（仅用于观测转发）
    TranscodeStage *_transcode = nullptr;

    std::deque<MediaPacket::Ptr> _pending; ///< 一次加工可能产出多个包（有界：max_out_packets）
    bool _flushed = false;                 ///< 非循环 EOF 时冲过一次残余

    mutable std::mutex _mutex; // 保护 _last_error
    std::string _last_error;
    std::atomic<uint64_t> _packets{0};
    std::atomic<uint64_t> _bytes{0};

    // ---- 输出流快照：只在 open() 里写一次，之后**只读** ----
    StreamInfo _video_info;
    StreamInfo _audio_info;
    bool _has_video_info = false;
    bool _has_audio_info = false;
    bool _annexb_video = false;
    bool _adts_audio = false;

    // ---- 【M11-a】网络源状态（a1：只有 playing；a2 起有 reconnecting/failed）----
    bool _network = false;
    int64_t _last_packet_ms = 0; ///< 最近一次读到包的时刻（`last_packet_age_ms` 用）
};

} // namespace mzmedia
