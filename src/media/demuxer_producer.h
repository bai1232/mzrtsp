/*
 * DemuxerProducer：把 M4 的 `Demuxer` 包装成 `SourcePump` 的读回调（M5-c；M6-c 加循环与 Annex-B）
 * ============================================================================
 * 形状来源：docs/DESIGN_M5.md §3.6（原形）、docs/DESIGN_M6.md §3.6（M6-c 的两项新增）；
 *          上游 `docs/DESIGN_M4.md`（Demuxer）、`DESIGN_M5.md` §3.5（SourcePump）
 *
 * 它的职责只有一件：**把 AVPacket 变成 MediaPacket**，并如实转达四种结局
 * （有包 / 有包且是新一段 / 读完 / 出错）。不做分发、不做缓冲、不做线程 —— 那些是
 * `SourcePump` 与 `MediaSource` 的事（分工明确才能各自被确定性测试）。
 *
 * 三处必须说清楚的取舍（DESIGN_M5.md §7、DESIGN_M6.md §7）：
 *   1) **每包一次拷贝**：`Demuxer::packet()` 指向它内部的 AVPacket，下一帧就被复用，
 *      所以必须把字节搬进共享载荷。这是全链路**唯一**一次拷贝；扇出到 N 个客户端仍然
 *      零拷贝（所有订阅者共享同一个 `shared_ptr`）。
 *   2) **非音视频流跳过**：字幕/数据流不喂给媒体层（媒体层只认 Video/Audio），
 *      但仍然**计数**（`skippedUnknownStreams()`）—— 静默跳过等于悄悄丢数据。
 *   3) 【M6-c】**Annex-B → AVCC**：H264 裸流（.h264 / TS）的解封装结果是 Annex-B（起始码分隔），
 *      而 FLV 要 length-prefixed + avcC。转换放在**这里**（而不是 `output/`）的原因：
 *      "输入是什么形状"是解封装层的知识，输出层只管"我要 avcC"；转换后 `MediaPacket` 与
 *      MP4 输入**完全同形**，于是 `FlvMuxer` 一行都不用改（也因此能被已有单测覆盖）。
 *
 * 时间戳换算失败（`packetTimestampsMs` 返回 false）时**仍然发包**（用 Demuxer 保持的
 * 上一次值 + 单调钳制），只累加计数：丢一个包比时间戳不够精确更糟。
 *
 * 【M6-c 循环重开（`Config::loop`，对应 `--loop`）】
 *   读到 EOF 后关闭并**重开同一个文件**，把这一趟的时间戳整体接在上一趟之后
 *   （偏移 = 上一趟的"最后 dts + 一帧时长"）。三件事都是刻意的：
 *   · 偏移由**生产者**加（不是让播放端自己猜）：FLV 的 tag 时间戳必须递增，
 *     时间轴回退会让播放端花屏/卡死，而我们没法控制播放端怎么反应；
 *   · 上限有界：每趟都会**校验流参数没变**（同一文件被换掉 = 明确失败），
 *     并且"一趟一个包都没读到"时**不再重开**（否则就是死循环 + 100% CPU）；
 *   · 时间轴**连续**：下游（节流、GOP 缓存、FLV 封装、播放端）完全看不出"这是第二趟"，
 *     所以不需要任何"新一段"标记。曾经的方案是让 SourcePump 在重开时重置节流基准
 *     （`ReadResult::PacketNewSegment`）—— 实测在"偏移累加"下**没有任何可观测差别**
 *     （4 倍速拉 3 秒：加/不加都是约 12.0 秒媒体时长），属于验证不了的复杂度，已删除。
 *     （见 docs/DESIGN_M6.md §7）
 * ============================================================================
 */

#pragma once

#include "ffmpeg/demuxer.h"
#include "media/media_packet.h"
#include "media/source_pump.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace mzmedia {

class DemuxerProducer {
public:
    struct Config {
        Demuxer::Limits demux; // 流数 / 单包 / open 与 read 超时
        /// 循环重开（M6-c）：读到文件尾把文件重新打开，时间戳接着往后接
        /// @note 默认 **false**："播完即结束"是默认行为，循环必须显式打开
        bool loop = false;
    };

    explicit DemuxerProducer(const Config &config);
    DemuxerProducer();
    ~DemuxerProducer() = default;
    DemuxerProducer(const DemuxerProducer &) = delete;
    DemuxerProducer &operator=(const DemuxerProducer &) = delete;

    /// @return false = 打不开（原因见 lastError()，已记日志）
    bool open(const std::string &path);
    bool opened() const {
        return _demux.opened();
    }
    const std::string &path() const {
        return _path;
    }
    /// 只读访问（排查/观测用；**输出层请用 muxerVideo()/muxerAudio()**）
    const Demuxer &demuxer() const {
        return _demux;
    }

    /// 绑定外部中止标志（通常是 `SourcePump::stopFlag()`）；@return 上一个标志
    const std::atomic<bool> *setAbortFlag(const std::atomic<bool> *flag) {
        return _demux.setAbortFlag(flag);
    }

    /// **直接作为 `SourcePump::ReadFn` 使用**
    SourcePump::ReadResult read(MediaPacket::Ptr *packet, std::string *error);

    // -----------------------------------------------------------------------
    // 输出层接口（M6-c）：给的是**本对象持有的副本**，不是 `Demuxer` 里的引用
    // -----------------------------------------------------------------------
    /// 视频流信息；若输入是 Annex-B 裸流，这里给的是**已经带 avcC** 的副本
    /// @return nullptr = 没有视频流
    const StreamInfo *muxerVideo() const {
        return _has_video_info ? &_video_info : nullptr;
    }
    const StreamInfo *muxerAudio() const {
        return _has_audio_info ? &_audio_info : nullptr;
    }
    /// 输入视频是否为 Annex-B（即发生过现场构造 avcC + 逐包转换）
    bool annexBVideo() const {
        return _annexb_video;
    }

    // 观测（**可在其他线程读**：源线程在跑，观测方可能是 HTTP 线程）
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
    /// Annex-B → AVCC 转换过的视频包数（Annex-B 输入 ≈ 视频包总数）
    uint64_t annexBPackets() const {
        return _annexb_packets.load();
    }
    /// 循环重开的次数
    uint64_t loopCount() const {
        return _loops.load();
    }
    /// 循环重开失败/被拒的次数（文件被换掉、空趟、重开打不开）
    uint64_t loopFailures() const {
        return _loop_failures.load();
    }
    /// 累计接上的时间戳偏移（毫秒）：= 第 N 趟的起点相对第 1 趟的起点
    int64_t loopOffsetMs() const {
        return _loop_offset_ms.load();
    }
    std::string lastError() const;

private:
    /// 一趟的 dts 轨迹（算"这一趟到哪儿结束"用；只有源线程碰它）
    struct PassTrack {
        int64_t last_dts = 0;
        int64_t prev_dts = 0;
        bool has_last = false;
        bool has_prev = false;
    };

    const StreamInfo *findStream(int index) const;
    /// 记失败原因（内部加锁：读方可能在别的线程）
    void setError(const std::string &message);
    /// open() 之后：准备 muxer 用的流副本（含 Annex-B → avcC 构造）
    /// @return false = 初始化数据不足以封 FLV（**初始化头当场失败**，不是等每个包都失败）
    bool prepareStreams();
    /// 一次循环重开；@return false = 重开失败（原因见 lastError()）
    bool reopenForLoop();
    /// 本趟结束时刻（毫秒）：各流"最后 dts + 一帧时长"的最大值；没有任何包 = 0
    int64_t passEndMs();
    /// 用帧率/采样率估一帧时长（拿不到就返回 0，**不猜**）
    static int64_t estimateFrameDurationMs(const StreamInfo &info);
    /// 流参数是否一致（重开时校验；不一致说明文件被换了）
    static bool sameStreamParams(const StreamInfo &a, const StreamInfo &b);
    void trackPass(int index, int64_t raw_dts_ms);

    Config _config;
    Demuxer _demux;
    std::string _path;
    mutable std::mutex _mutex; // 保护 _last_error
    std::string _last_error;
    std::atomic<uint64_t> _packets{0};
    std::atomic<uint64_t> _bytes{0};
    std::atomic<uint64_t> _skipped_unknown{0};
    std::atomic<uint64_t> _ts_failures{0};
    std::atomic<uint64_t> _annexb_packets{0};
    std::atomic<uint64_t> _loops{0};
    std::atomic<uint64_t> _loop_failures{0};
    std::atomic<int64_t> _loop_offset_ms{0};

    // ---- 流快照：只在 open() 里写一次，之后**只读** ----
    // 为什么不做成"每次重开都刷新"：连接持有这两份快照的**裸指针**（FlvMuxer::Streams），
    // 刷新就会变成跨线程写 → 数据竞争 + 悬垂。所以改成分工：
    // 重开时**校验**参数没变（变了就明确失败），而不是就地更新。
    StreamInfo _video_info;
    StreamInfo _audio_info;
    StreamInfo _raw_video_info; // 解封装原始信息（Annex-B 时用来和下一趟比对）
    StreamInfo _raw_audio_info;
    bool _has_video_info = false;
    bool _has_audio_info = false;
    bool _annexb_video = false;

    // ---- 只有源线程碰：一趟的 dts 轨迹 ----
    std::vector<PassTrack> _pass;
};

} // namespace mzmedia
