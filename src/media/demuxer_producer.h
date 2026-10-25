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
 *   4) 【M8 / FR-2.2】**ADTS → raw AAC + 现场造 ASC**：MPEG-TS 里的 AAC 是 ADTS（每帧 7/9 字节
 *      自描述头，`avformat` **不给 extradata**），而 FLV 要「AudioSpecificConfig + raw AAC 帧」。
 *      视频侧的对称物就是上面那条 Annex-B → AVCC：同一个地方、同一套理由（输入形状归解封装层）。
 *      两个约束决定了它的形状：
 *        · ASC **必须在 `open()` 之内就绪** —— 连接会拿着流快照的裸指针写 AAC sequence header，
 *          所以不能"等第一个包到了再造"；
 *        · 而造 ASC 需要**真读一帧音频**（ADTS 头里的 profile / 采样率索引 / 声道配置才是权威）。
 *      做法：用**一次性探针 Demuxer** 打开同一个文件，读到首个音频包就解析 ADTS 头造 ASC，
 *      然后关掉探针 —— 主解封装的读游标**完全不动**（否则首帧就得进"重放队列"，那是另一套复杂度）。
 *      找不到音频帧 / 头不合法 → **明确失败**（不猜 AAC-LC，也不默认 44100）。
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
 *
 * 【M10-b 转码（`Config::transcode`，对应 `--transcode` / `?transcode=1&h=`）】
 *   视频包在**这里**过一道 `Transcoder`（解码 → 缩放 → 编码），音频**原样透传**（SPEC §9 不做音频转码）。
 *   为什么放在这一层：解封装形状（Annex-B / ADTS / extradata）的知识已经在这里，转码要用的
 *   "源 avcC 与分辨率"也在这里；放上去（SourcePump/MediaSource）就会把 ffmpeg 依赖带到分发层。
 *   三条必须说清楚的形状：
 *     · **一进可能零出、多出**：解码器攒帧、编码器有延迟，所以 `read()` 允许"读一个输入、吐 0..N 个输出"，
 *       产物先进 `_pending`（有界），再一个一个交出去 —— `SourcePump` 的契约是"一次一个包"；
 *     · **EOF 必须 flush**：不冲编解码器残余就是"少最后几帧"且**不报错**；循环模式下**不** flush
 *       （那只是"这一趟结束"，解码器还要接着用）；
 *     · **时间戳**：转码输出的时间轴由 `Transcoder` 自己生成（源 pts 被 `Demuxer` 的单调守卫钳过），
 *       循环重开的偏移仍然在这里加 —— 与透传的音频共用同一条时间轴（详见 docs/DESIGN_M10.md §8）。
 * ============================================================================
 */

#pragma once

#include "ffmpeg/demuxer.h"
#include "ffmpeg/transcoder.h"
#include "media/media_packet.h"
#include "media/source_pump.h"

#include <atomic>
#include <cstdint>
#include <deque>
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

        /// 【M10-b / FR-7.1 / FR-1.3】转码（一路源一份，跑在**源线程**上）
        struct Transcode {
            /// 默认 **关**：不转码时行为与 v0.1 完全一致（HEVC 仍然 415，绝不静默降级）
            bool enabled = false;
            /// 目标高度：0 = **不缩放**（只重编码，分辨率保持源）；>0 = 等比缩到该高度
            /// @note 只给高度：宽高比由源决定（等比），宽算不出来时明确拒绝，不猜
            int out_height = 0;
            /// **上限**高度：源高于它就缩到它，源不高于它就**不缩放**（0 = 不设上限）
            /// @note 与 `out_height` 分开：`out_height` 是"用户明确要这个高度"（放大 = 拒绝），
            ///       `max_height` 是"顺便帮我压到这个高度以内"（小的源保持原样）
            int max_height = 0;
            /// 目标码率（kbps）：0 = 由像素数×帧率推导并夹在 [500, 8000]（有界）
            int out_bitrate_kbps = 0;
            /// 关键帧间隔（帧）：与 HLS 的 2s 切片目标同量级
            int gop_frames = 50;
            /// 单次转码最多吐出多少包（0 非法）：解码器可能一进多出，异常输入不能把内存拉爆
            int max_out_packets = 64;
        };
        Transcode transcode;
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
    /// 【M7 / FR-6.1】本源的**平均码率**（bit/s）= 累计字节 × 8 / 已运行秒数
    /// @note 用**单调时钟**算时长；不足 1ms 时返回 0（不制造天文数字）；未打开时为 0
    uint64_t bitrateBps() const;
    /// 源打开时刻（单调时钟毫秒；0 = 还没打开）：`/api/stats` 里每源的 `uptime_ms` 用它算
    uint64_t openedAtMs() const {
        return static_cast<uint64_t>(_opened_at_ms.load());
    }
    uint64_t skippedUnknownStreams() const {
        return _skipped_unknown.load();
    }
    uint64_t timestampFailures() const {
        return _ts_failures.load();
    }
    /// 【v0.2.1】**pts 回退（乱序）**的包数 —— B 帧源的**正常现象**（源线程写，观测方可能跨线程读）
    /// @note 它是"这条链路有没有把 B 帧的显示顺序搞坏"的最直观指标：>0 才说明 pts 是原样的
    uint64_t ptsOutOfOrder() const {
        return _demux.ptsOutOfOrder();
    }
    /// **dts** 回退被钳制的次数（正常应恒为 0；非 0 说明源的解码序都不单调了）
    uint64_t dtsClamped() const {
        return _demux.dtsClamped();
    }
    /// Annex-B → AVCC 转换过的视频包数（Annex-B 输入 ≈ 视频包总数）
    uint64_t annexBPackets() const {
        return _annexb_packets.load();
    }
    /// 【M8】音频是否是 ADTS（即发生过"现场造 ASC + 逐包剥 ADTS 头"）
    bool adtsAudio() const {
        return _adts_audio;
    }
    /// 【M8】剥过 ADTS 头的音频包数（ADTS 输入 ≈ 音频包总数）
    uint64_t adtsPackets() const {
        return _adts_packets.load();
    }

    // -----------------------------------------------------------------------
    // 【M10-b】转码观测（源线程写、任意线程读 → 一律 atomic 或"建好后只读"）
    // -----------------------------------------------------------------------
    /// 本源的视频是否在转码（建好后只读；用独立 atomic，避免跨线程读 `shared_ptr` 成员）
    bool transcodeEnabled() const {
        return _tx_enabled.load();
    }
    /// 转码输入是 HEVC 吗（产出永远是 H264；仅在 transcodeEnabled() 时有意义）
    bool transcodeInputHevc() const {
        return _tx_in_hevc.load();
    }
    int transcodeOutWidth() const {
        return _tx_out_width.load();
    }
    int transcodeOutHeight() const {
        return _tx_out_height.load();
    }
    /// 喂给转码器的视频包数（**输入**侧）
    uint64_t transcodeInputs() const {
        return _tx_inputs.load();
    }
    /// 转码器吐出的包数（**输出**侧；EOF 的 flush 之后两者应相等）
    uint64_t transcodeOutputs() const {
        return _tx_outputs.load();
    }
    /// 转码输出的媒体时长（毫秒）= 最大 dts − 最小 dts
    uint64_t transcodeOutMediaMs() const {
        return _tx_out_media_ms.load();
    }
    /// 从**第一次喂包**到现在（或转码结束时刻）经过的墙钟（毫秒）；0 = 还没开始
    /// @note 这个数**含节流等待**（源按墙钟推流），所以它 ≈ 媒体时长 —— 看"转码快不快"要读 cost
    int64_t transcodeElapsedMs() const;
    /**
     * 【FR-6.1 / NFR-5】累计花在 `transcode()`/`flush()` **里面**的墙钟（毫秒）
     * @note 这才是"转码本身有多快"的分母：不含解封装、不含节流等待、不含网络。
     *       源按 1 倍速推流时，用 elapsed 作分母得到的 speed ≈ 1.0（那是**节流**的速度，不是转码的速度）
     */
    int64_t transcodeCostMs() const;
    /// 【FR-6.1 / NFR-5】实时倍数 = 已输出媒体时长 / 转码耗时；0 = 还没有可判定的时长
    double transcodeSpeed() const;
    /// 输入帧间距偏离标称帧率（VFR）→ 输出被按 CFR 规整（已 Warn 一次）
    bool transcodeVfr() const {
        return _tx_vfr.load();
    }
    /// 是否建了缩放器（false = 只重编码，尺寸/像素格式与目标一致）
    bool transcodeScaling() const {
        return _tx_scaling.load();
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
    /// 一趟的 dts 轨迹（算"这一趟从哪儿开始 / 到哪儿结束"用；只有源线程碰它）
    struct PassTrack {
        int64_t last_dts = 0;
        int64_t prev_dts = 0;
        /// 【v0.2.1】这一趟该流的**首个** dts。为什么需要它：MP4 的编辑列表/B 帧源首包
        /// dts 可能是负的（实测 `bf.mp4` = **-80ms**），只按"末包 + 帧长"算趟偏移会让
        /// 下一趟的首包**倒退**（960 → 921），循环播放时每个接缝都抖一下
        int64_t first_dts = 0;
        bool has_last = false;
        bool has_prev = false;
        bool has_first = false;
    };

    const StreamInfo *findStream(int index) const;
    /// 记失败原因（内部加锁：读方可能在别的线程）
    void setError(const std::string &message);
    /**
     * 【M8】用一次性探针读到首个 ADTS 音频包，造出 `AudioSpecificConfig` 填进 `_audio_info`
     * @return false = 打不开探针 / 前 N 个包里没有音频 / ADTS 头不合法 / 与 Demuxer 报的参数冲突
     *         （原因写入 `lastError()`，**不猜** AAC-LC、也不默认采样率）
     * @note 只在 `prepareStreams()` 里、且"AAC 输入且没有 extradata"时调用一次
     */
    bool probeAdtsAudioConfig();
    /// open() 之后：准备 muxer 用的流副本（含 Annex-B → avcC 构造）
    /// @return false = 初始化数据不足以封 FLV（**初始化头当场失败**，不是等每个包都失败）
    bool prepareStreams();
    /// 一次循环重开；@return false = 重开失败（原因见 lastError()）
    bool reopenForLoop();
    /// 本趟结束时刻（毫秒）：各流"最后 dts + 一帧时长"的最大值；没有任何包 = 0
    int64_t passEndMs();
    /// 本趟起点（毫秒）：各流**首个 dts** 的最小值；没有任何包 = 0
    /// @note 循环偏移要用 `pass_end - pass_start`，否则首包 dts 为负的源会在接缝处倒退
    int64_t passStartMs();
    /// 用帧率/采样率估一帧时长（拿不到就返回 0，**不猜**）
    static int64_t estimateFrameDurationMs(const StreamInfo &info);
    /// 流参数是否一致（重开时校验；不一致说明文件被换了）
    static bool sameStreamParams(const StreamInfo &a, const StreamInfo &b);
    void trackPass(int index, int64_t raw_dts_ms);

    // ---- 【M10-b】转码 ----
    /// `prepareStreams()` 之后建转码器并把 `_video_info` 换成**输出**形状
    /// @return false = 明确拒绝（没有视频 / 编码不支持 / 音频需要转码 / 参数非法 / 建不起来）
    bool openTranscoder();
    /// 一次视频包转码 → 产物进 `_pending`；@return false = 失败（原因已记）
    bool transcodeVideo(const std::vector<uint8_t> &avcc, int64_t dts_ms, int64_t pts_ms);
    /// 把 `_pending` 里的一个包交出去（同时记账）
    SourcePump::ReadResult servePending(MediaPacket::Ptr *packet);
    /// 转码产物 → `_pending`（同时更新观测计数）；@return false = 构造 MediaPacket 失败
    bool enqueueTranscoded(std::vector<Transcoder::OutPacket> *outs);
    /// EOF 时把编解码器里的残余冲出来（**漏了就是少最后几帧且不报错**）
    bool flushTranscoder();

    Config _config;
    Demuxer _demux;
    std::string _path;
    mutable std::mutex _mutex; // 保护 _last_error
    std::string _last_error;
    std::atomic<uint64_t> _packets{0};
    std::atomic<uint64_t> _bytes{0};
    /// 源打开时刻（单调时钟毫秒）：`bitrateBps()` 的时长基准（0 = 还没打开）
    std::atomic<int64_t> _opened_at_ms{0};
    std::atomic<uint64_t> _skipped_unknown{0};
    std::atomic<uint64_t> _ts_failures{0};
    std::atomic<uint64_t> _annexb_packets{0};
    std::atomic<uint64_t> _adts_packets{0};   ///< 【M8】剥过 ADTS 头的音频包数
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
    /// 【M8】音频输入是 ADTS（TS）：发包时逐包剥头
    bool _adts_audio = false;

    // ---- 【M10-b】转码状态 ----
    /// 建好后只读；nullptr = 本源不转码
    Transcoder::Ptr _transcoder;
    /// "本源在转码"的跨线程可见标志（在源线程启动**之前**置位，之后只读）
    std::atomic<bool> _tx_enabled{false};
    /// 已转出但还没交出去的包（**有界**：一次 transcode() 最多 max_out_packets 个）
    std::deque<MediaPacket::Ptr> _pending;
    /// 是否已经 flush 过（flush 幂等；循环模式下**不** flush）
    bool _tx_flushed = false;
    /// 输入编码是 HEVC（产出永远是 H264；只影响解码器；跨线程读 → atomic）
    std::atomic<bool> _tx_in_hevc{false};
    std::atomic<int> _tx_out_width{0};
    std::atomic<int> _tx_out_height{0};
    std::atomic<uint64_t> _tx_inputs{0};
    std::atomic<uint64_t> _tx_outputs{0};
    std::atomic<uint64_t> _tx_out_media_ms{0};
    std::atomic<bool> _tx_vfr{false};
    std::atomic<bool> _tx_scaling{false};
    /// 第一次喂包的单调时钟毫秒（0 = 还没开始）：speed 的墙钟基准
    std::atomic<int64_t> _tx_first_ms{0};
    /// 转码**结束**的单调时钟毫秒（0 = 还在转）：不冻结的话，elapsed 会一路涨
    std::atomic<int64_t> _tx_end_ms{0};
    /// 累计转码耗时（微秒）：`transcode()`/`flush()` 里的墙钟总和（NFR-5 的分母）
    std::atomic<int64_t> _tx_cost_us{0};
    /// 已转出的**帧数** × 标称帧步长 = 输出媒体时长（只有源线程碰）
    /// @note 为什么不用"输出 dts 的跨度"：循环重开的偏移会造出一个**假间隙**（实测 4.96s 被算成 9.76s），
    ///       于是 speed 被虚高。转码输出按构造就是 CFR，帧数 × 步长才是它的真实时长
    uint64_t _tx_frames_out = 0;
    double _tx_frame_step_ms = 40.0;
    /// 本趟开始时已转出的帧数（循环接缝处算"这一趟转了多少媒体"用）
    uint64_t _tx_frames_at_pass_start = 0;
    /// 循环接缝累积的时间轴修正量（毫秒）：让"帧序号 × 步长"的转码时间轴与
    /// 透传音频的"原始 dts + 趟偏移"在接缝处**对齐**（详见图纸 DESIGN_M10 §8.5）
    int64_t _tx_ts_shift = 0;

    // ---- 只有源线程碰：一趟的 dts 轨迹 ----
    std::vector<PassTrack> _pass;
};

} // namespace mzmedia
