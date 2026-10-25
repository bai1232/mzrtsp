/*
 * SourceSession：**一次源的读**（v0.2.3 重构抽出）
 * ============================================================================
 * 它只做"读"这一件事，但把读这件事的所有**生命周期细节**收在一处：
 *   · 打开/关闭（`Demuxer` 之上再加"打开时刻"）；
 *   · 读包：跳过字幕/数据流（**计数**，静默跳过 = 悄悄丢数据）、时间戳换算失败**仍然发**（计数）；
 *   · EOF：非循环 → EndOfStream；循环 → **重开并把时间戳接到上一趟之后**（M6-c 的语义，逐字保留）；
 *   · 观测：输入包数/字节、跳过数、时间戳失败数、pts 乱序数、dts 钳制数、循环次数/偏移/失败。
 *
 * 为什么把它单独抽出来（而不是留在 `DemuxerProducer` 里）：
 *   · `DemuxerProducer` 当时同时管"读""输入形状转换""转码编排"三件事，每加一种输入形状或能力
 *     都在改同一个 789 行的类（M8 +125 行、M10-b +260 行）；
 *   · "读"与"形状"是**两条不同的变化轴**：v0.3 的 RTSP 要变的是**读**（重连），
 *     而输入形状（Annex-B/ADTS）不该受影响 —— 反过来也一样。
 *
 * 循环重开的三个细节（都是 M6-c 踩出来的，**别改回去**）：
 *   1) 偏移由这里加（不是让播放端猜）：FLV/TS 的时间戳必须递增；
 *   2) 偏移 = `pass_end − pass_start`（v0.2.1：首包 dts 为负的源，少减 `pass_start` 会在接缝处**倒退**）；
 *   3) 每趟都校验流参数没变（文件被换掉 = 明确失败），空趟不再重开（否则死循环 + 100% CPU）。
 * ============================================================================
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ffmpeg/demuxer.h"
#include "media/media_packet.h"
#include "media/reconnect_policy.h"

namespace mzmedia {

class SourceSession {
public:
    struct Config {
        Demuxer::Limits demux; ///< 流数 / 单包 / open 与 read 超时
        /// 循环重开（M6-c）：读到文件尾把文件重新打开，时间戳接着往后接
        bool loop = false;
    };

    /// 【M11-a2】网络源的断流重连（本地文件不用）
    /// @note 单列成 setter 而不是塞进 `Config`：`_demux` 在构造时就用 `Config.demux` 建好了，
    ///       重连接在 open 之前打开更自然（也让"文件源不会重连"成为结构上的事实）
    struct Reconnect {
        bool enabled = false;
        ReconnectPolicy::Config policy; ///< 退避曲线（1→2→4→8→16→封顶）
    };

    /// 网络源的状态（stats 用）：`Idle` = 本地文件/没连过；`connecting` 只在首连时短暂出现
    enum class NetState { Idle, Playing, Reconnecting, Failed };
    static const char *netStateName(NetState state);

    /// 一个**原始**包（还没做形状转换/转码）
    struct Packet {
        MediaKind kind = MediaKind::Video;
        int stream_index = -1;
        bool key = false;
        int64_t raw_dts_ms = 0;
        int64_t raw_pts_ms = 0;
        /// 已拷贝的字节（Demuxer 的 AVPacket 下一帧就被复用 → 必须拷一次）
        std::shared_ptr<const std::vector<uint8_t>> payload;
    };

    enum class Result { Packet, EndOfStream, Error };

    explicit SourceSession(const Config &config);
    ~SourceSession() = default;
    SourceSession(const SourceSession &) = delete;
    SourceSession &operator=(const SourceSession &) = delete;

    /// @return false = 打不开（原因见 lastError()）
    bool open(const std::string &path);
    /// 【M11-a】带打开选项的重载（网络源：rtsp_transport / stimeout …）
    bool open(const std::string &path, const Demuxer::Options &options);
    /// 源是不是网络源（`rtsp://` / `rtsps://`）：上层据此决定"要不要前置校验 SPS/PPS"
    bool network() const {
        return _network;
    }
    /// 关闭（幂等）；@return false = 本来就没打开
    bool close();
    bool opened() const {
        return _demux.opened();
    }
    const std::string &path() const {
        return _path;
    }

    /// 源流信息（**未转换**；输出层要的是转换后的，见 `PacketStage::prepare`）
    const std::vector<StreamInfo> &streams() const {
        return _demux.streams();
    }
    const StreamInfo *firstVideo() const {
        return _demux.firstVideo();
    }
    const StreamInfo *firstAudio() const {
        return _demux.firstAudio();
    }
    const Demuxer &demuxer() const {
        return _demux;
    }

    /// 绑定外部中止标志（通常是 `SourcePump::stopFlag()`）
    /// @note 本类也留一份指针：重连的等待要能被它**立刻打断**（否则 stop() 会卡在退避睡眠里）
    const std::atomic<bool> *setAbortFlag(const std::atomic<bool> *flag) {
        _abort = flag;
        return _demux.setAbortFlag(flag);
    }

    /// 【M11-a2】配置断流重连（必须在 `open()` 之前调）
    /// @return false = 退避参数非法（保持原值不变）
    bool setReconnect(const Reconnect &reconnect);

    // ---- 【M11-a2】网络源观测（stats 用；本地文件恒 Idle/0）----
    NetState netState() const {
        return _net_state.load();
    }
    uint64_t connects() const {
        return _connects.load();
    }
    uint64_t reconnects() const {
        return _reconnects.load();
    }
    uint64_t reconnectAttempts() const {
        return _policy.attempts();
    }
    int64_t nextBackoffMs() const {
        return _policy.nextDelayIfFailedMs();
    }
    int64_t lastPacketAgeMs() const;
    /// 最近一次断流/重连失败的原因（**已脱敏**；成功连上后不清空，便于排查"刚才为什么断了"）
    std::string lastNetError() const;

    /**
     * 读一个包（内部处理：跳过非音视频流、EOF、循环重开、时间戳换算与守卫）
     * @note 返回 `Packet` 时 `out->payload` 非空；返回 `EndOfStream` 是**稳定终态**（可重复调用）
     */
    Result read(Packet *out, std::string *error);

    /// 当前趟偏移（毫秒）：转换类阶段要把它加到时间戳上；转码类忽略它
    int64_t loopOffsetMs() const {
        return _loop_offset_ms.load();
    }

    /**
     * 循环重开的接缝回调：`pass_length_ms` = 这一趟的真实长度
     * @note 转码阶段要用它对齐自造的时间轴（"帧序号 × 步长"与"这一趟真实多长"的差额）。
     *       用回调而不是让 session 认识阶段：**读**与**形状**是两条轴，不能互相依赖。
     */
    void setSeamCallback(std::function<void(int64_t)> callback) {
        _on_seam = std::move(callback);
    }

    // ---- 观测（读侧；全部 atomic 或只读）----
    uint64_t totalPackets() const {
        return _packets.load();
    }
    uint64_t totalBytes() const {
        return _bytes.load();
    }
    uint64_t skippedUnknownStreams() const {
        return _skipped_unknown.load();
    }
    uint64_t timestampFailures() const {
        return _ts_failures.load();
    }
    uint64_t ptsOutOfOrder() const {
        return _demux.ptsOutOfOrder();
    }
    uint64_t dtsClamped() const {
        return _demux.dtsClamped();
    }
    uint64_t loopCount() const {
        return _loops.load();
    }
    uint64_t loopFailures() const {
        return _loop_failures.load();
    }
    uint64_t openedAtMs() const {
        return static_cast<uint64_t>(_opened_at_ms.load());
    }
    std::string lastError() const;

private:
    /// 一趟的 dts 轨迹（算"这一趟从哪儿开始 / 到哪儿结束"用；只有源线程碰它）
    struct PassTrack {
        int64_t last_dts = 0;
        int64_t prev_dts = 0;
        /// 这一趟该流的**首个** dts（MP4 编辑列表 / B 帧源可能是负的，见 passStartMs）
        int64_t first_dts = 0;
        bool has_last = false;
        bool has_prev = false;
        bool has_first = false;
    };

    void setError(const std::string &message);
    const StreamInfo *findStream(int index) const;
    void trackPass(int index, int64_t raw_dts_ms);
    /// 本趟结束时刻（毫秒）：各流"最后 dts + 一帧时长"的最大值；没有任何包 = 0
    int64_t passEndMs();
    /// 本趟起点（毫秒）：各流首个 dts 的最小值；没有任何包 = 0
    int64_t passStartMs();
    /// 一次循环重开；@return false = 重开失败（原因见 lastError()）
    bool reopenForLoop();
    /**
     * 【M11-a2】断流重连（**只有网络源**）
     * @return true = 重连成功（时间轴在读到新一段第一个包时接续，见 read() 的 `_reconnect_needs_timeline`）；false = 放弃（原因见 error）
     * @note 退避等待是**可打断**的：每 50ms 看一眼中止标志，否则 `SourcePump::stop()` 会卡在睡眠里
     */
    bool reconnectNetwork(std::string *error);
    /// 按"上一包的生效 dts + 一帧间隔"给新一段接上时间轴（重连与循环共用同一套语义）
    void continueTimelineAt(int64_t raw_first_dts_ms);
    /// 可打断的睡眠（@return false = 被中止）
    bool sleepInterruptibleMs(int64_t delay_ms);
    /// 用帧率/采样率估一帧时长（拿不到就返回 0，**不猜**）
    static int64_t estimateFrameDurationMs(const StreamInfo &info);
    /// 流参数是否一致（重开时校验；不一致说明文件被换了）
    static bool sameStreamParams(const StreamInfo &a, const StreamInfo &b);

    Config _config;
    Demuxer _demux;
    std::string _path;
    mutable std::mutex _mutex; // 保护 _last_error
    std::string _last_error;

    std::atomic<uint64_t> _packets{0};
    std::atomic<uint64_t> _bytes{0};
    std::atomic<int64_t> _opened_at_ms{0};
    std::atomic<uint64_t> _skipped_unknown{0};
    std::atomic<uint64_t> _ts_failures{0};
    std::atomic<uint64_t> _loops{0};
    std::atomic<uint64_t> _loop_failures{0};
    std::atomic<int64_t> _loop_offset_ms{0};

    /// 只有源线程碰：一趟的 dts 轨迹
    std::vector<PassTrack> _pass;
    std::function<void(int64_t)> _on_seam; ///< 接缝通知（转码对齐时间轴用）
    bool _network = false;                 ///< 网络源（rtsp://）：校验/超时的口径不同

    // ---- 【M11-a2】网络源状态 ----
    Reconnect _reconnect;
    ReconnectPolicy _policy;
    Demuxer::Options _options;               ///< 重开时要用**同一批**打开选项
    const std::atomic<bool> *_abort = nullptr;
    std::atomic<NetState> _net_state{NetState::Idle};
    std::atomic<uint64_t> _connects{0};
    std::atomic<uint64_t> _reconnects{0};
    std::atomic<int64_t> _last_packet_ms{0}; ///< 最近读到包的时刻
    /// 已交出去的包中"生效 dts"（原始+趟偏移）的最大值：重连后要把新一段接在它后面
    int64_t _last_effective_dts = 0;
    bool _has_effective_dts = false;
    bool _reconnect_needs_timeline = false; ///< 刚重连上，下一包要用来接续时间轴
    mutable std::mutex _net_mutex; // 保护 _last_net_error
    std::string _last_net_error;

    // ---- 打开时的流参数快照：循环重开时**校验**它没变（不是刷新它）----
    // 为什么不刷新：输出层持有这两份快照的裸指针（`FlvMuxer::Streams`），刷新 = 跨线程写。
    StreamInfo _raw_video_info;
    StreamInfo _raw_audio_info;
    bool _has_video_info = false;
    bool _has_audio_info = false;
};

/**
 * 【M11-a2】把新一段的**原始**时间轴接到上一段之后（纯函数，重连/循环重开共用）
 * @param last_effective_dts_ms 上一包**生效**（已含趟偏移）的 dts；`has_last == false` 时忽略
 * @param has_last              上一段是否真的读过包
 * @param raw_first_dts_ms      新一段第一个包的**原始** dts
 * @param frame_gap_ms          一帧的时长（调用方保证 > 0；拿不到帧率时用 1 ms）
 * @return 要加在**新一段所有包的原始 dts**上的偏移量
 * @note 语义：**总是**让新一段的第一个包落在"上一包 + 一帧"处 —— 对"对端从 0 重来"和
 *       "对端接着发"都成立，且**不保留断流空档**（断了 60 s 也接上去：观众的播放时间轴
 *       不该出现 60 s 空洞，否则播放器会以为要等 60 s 然后卡住）。
 *       抽成纯函数是为了能**单测**这条公式（它一错，重连处就是花屏/卡顿）。
 */
inline int64_t timelineOffsetForReconnect(int64_t last_effective_dts_ms, bool has_last,
                                          int64_t raw_first_dts_ms, int64_t frame_gap_ms) {
    const int64_t base = has_last ? last_effective_dts_ms : raw_first_dts_ms;
    return base + frame_gap_ms - raw_first_dts_ms;
}

} // namespace mzmedia
