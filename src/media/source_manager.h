/*
 * SourceManager：源的懒启动与空闲释放（M5-c）
 * ============================================================================
 * 形状来源：docs/DESIGN_M5.md §3.7；上位依据 `ARCHITECTURE.md` §7
 *   「源释放 | lazy 启动 + 空闲 60s 释放 | 无人观看时不占 CPU/内存」
 *
 * 它管三件事，每件都有一句"为什么"：
 *   1) **懒启动**：第一次 `acquire()` 才 open 文件、才起源线程 —— 没人看的文件不占 CPU/内存；
 *   2) **复用**：同一个 path 再次 acquire 拿到**同一个** `MediaSource`（一源多消费者）；
 *   3) **空闲释放**：最后一个句柄放手后开始计时，到点仍无人 → 停源线程 + 关文件。
 *
 * 生命周期怎么做到不悬垂（本类唯一需要动脑的地方）：
 *   · 管理器持有 `MediaSource` 的强引用；对外返回的**句柄**用"自定义 deleter + 捕获强引用"
 *     的写法：句柄本身也让对象活着（`keep`），所以即使 `releaseAll()` 已经摘掉了条目，
 *     调用方手里的句柄依然指向有效对象 —— 句柄一放，`keep` 析构，对象才真正回收。
 *   · 句柄的 deleter 只通过 `weak_ptr` 回调管理器：管理器已经析构时它是 no-op（不会 UAF）。
 *
 * 计时器的两个坑（都处理了，别改回去）：
 *   · `EventPoller::doDelayTask` **只能在轮询线程调用** → 一律用 `async()` 投递；
 *     若调用方本来就在轮询线程，`async` 会内联执行（`may_sync` 语义），不会多绕一圈。
 *   · 投递/挂表失败（poller 已退出）**绝不能静默"永不释放"**（那就是泄漏）→
 *     计数 + Warn + **退化为立即释放**：宁可多回收一次，也不要留一个没人管的线程。
 * ============================================================================
 */

#pragma once

#include "media/demuxer_producer.h"
#include "media/media_source.h"
#include "media/source_pump.h"
#include "network/event_poller.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace mzmedia {

/// 必须用 `create()` 构造（句柄的 deleter 需要 weak_ptr 回指管理器）
class SourceManager : public std::enable_shared_from_this<SourceManager> {
public:
    using Ptr = std::shared_ptr<SourceManager>;

    struct Config {
        /// 空闲释放阈值（`ARCHITECTURE.md` §7 = 60s）
        /// @note **0 是合法值**：含义是"句柄一放就释放"（不缓存源）——它不是"无界"，
        ///       而是最保守的一档；也正因为有这一档，空闲释放的用例可以**完全确定性**
        uint32_t idle_release_ms = 60000;
        /// 【M6-c】空闲到期但**还有人订阅**时的复查间隔（毫秒）
        /// @note 为什么需要它：「句柄数为 0」不等于「没人看」—— 应用层 handler 一返回就把句柄放了，
        ///       而连接还连着、订阅者还在推流。这时**不能**释放（会把源线程停掉，观看者突然"播完"），
        ///       于是改为隔一段时间再看一眼。它必须 > 0：0 会退化成"async 里再 async"的忙等
        uint32_t deferred_recheck_ms = 1000;
        Throttle::Config throttle;        // FR-3.5：默认开、1 倍速（压测/单测可调 speed）
        MediaSource::Limits source;       // 每源的队列 / GOP / 人数上限（推导值在 Limits 里）
        DemuxerProducer::Config producer; // 解封装上限（流数 / 单包 / 超时 / 循环重开）
        /// 【M10-b / FR-1.3】同时最多几路源在转码
        /// @note **0 非法**（它不是"不限"）：转码吃满 CPU 会让整台机器不可用，
        ///       所以这是硬上限；应用层按 `nproc-1` 给初值，脚本可调小做确定性验收
        size_t max_transcoding = 1;
    };

    /// 【M10-b】一路源的转码规格（`enabled` + 目标高度）
    /// @note 它就是 `DemuxerProducer::Config::Transcode` 的"请求态"：`out_height == 0` = 只重编码
    struct TranscodeSpec {
        bool enabled = false;
        /// **显式**目标高度（`h=`）：0 = 没给。给了就必须偶数且不高过源（否则明确拒绝）
        int out_height = 0;
        /// **上限**高度（`--transcode-height`）：源高于它就缩到它，源不高于它就**不缩放**
        /// @note 与 `out_height` 分开的理由：`?transcode=1` 的意图是"给我一路能播的流"，
        ///       不该因为源本身只有 240p 就被"默认 720p"判成放大而拒绝
        int max_height = 0;
        int out_bitrate_kbps = 0; ///< 0 = 推导（有界）
        /// 进"源键 / 日志 / stats"的短描述：`""` = 不转码，`"src"` = 只重编码，
        /// `"h=720"` = 显式缩到 720p，`"auto=720"` = 源高于 720p 才缩
        std::string describe() const;
    };

    /// 输出层（FLV）要用的流信息快照（M6-c）
    /// @note `video` 在 Annex-B 输入时**已经带现场构造的 avcC** —— 输出层不需要知道输入形状
    struct MuxerStreams {
        const StreamInfo *video = nullptr;
        const StreamInfo *audio = nullptr;
        bool annex_b_video = false; ///< 视频输入是 Annex-B（日志/诊断用）
    };

    /// @param poller 空闲计时用；传 nullptr → 退化为"句柄释放即回收"（不静默变成"永不释放"）
    static Ptr create(const EventPoller::Ptr &poller);
    ~SourceManager(); // 停掉所有源：绝不留线程与未释放的 ffmpeg 上下文

    /// @return false = 非法（idle 超硬上限 / 每源上限非法），保持原值不变
    bool setConfig(const Config &config);
    Config config() const;

    /**
     * 取得源（懒启动 / 复用）
     * @return nullptr = 打不开（**不注册源**、计数 + 记日志，绝不静默）
     * @note 句柄释放后触发空闲计时；`idle_release_ms == 0` 时立即回收
     */
    MediaSource::Ptr acquire(const std::string &path);

    /**
     * 【M10-b】取得**转码源**（同一个文件 × 同一个转码规格 = 同一个源）
     * @param spec `enabled == false` 时等价于 `acquire(path)`
     * @return nullptr = 打不开 / 被拒（转码并发已满、参数非法；原因见 `lastError()`）
     * @note 转码源与直通源是**两个独立的源**（各自解封装）：换来的是"不动 `media/` 的分发层"
     *       （SC-2），代价是同一个文件被 demux 两次、转码那一份多占 CPU。见 docs/DESIGN_M10.md §8
     */
    MediaSource::Ptr acquire(const std::string &path, const TranscodeSpec &spec);

    /// 源键（`sourceKey(path, spec)`）：直通源 = 文件路径，转码源 = `路径|h=720` / `路径|src`
    static std::string sourceKey(const std::string &path, const TranscodeSpec &spec);

    /// 该源解封装出的流信息（M6-c：输出层写 FLV 的 sequence header 要用）
    /// @return 两路都是 nullptr = 没有这个源
    /// @note 返回的指针指向**源自己持有的快照**（不是 `Demuxer` 里的引用）：
    ///       · 可以跨连接长期保存（循环重开也不会换掉它）；
    ///       · 但**源必须先活着** —— 调用方用 `acquire()` 拿到的句柄或连接上的订阅者
    ///         来保证这一点（NFR-6 的断开即退订就是从这里来的）。
    MuxerStreams muxerStreamsFor(const std::string &path) const;
    /// 【M10-b】转码源的流信息（`video` 给的是**转码后**的宽高与 avcC）
    MuxerStreams muxerStreamsFor(const std::string &path, const TranscodeSpec &spec) const;

    /// 立刻释放某个源（不等空闲计时）；@return false = 没有这个源
    bool release(const std::string &path);
    /// 释放全部（关停路径用）；@return 释放的源数。**已在外面的句柄仍然有效**（见文件头）
    size_t releaseAll();

    size_t sourceCount() const;
    /// 【M10-b】当前在转码的源数（用于"并发上限还剩几个"）
    size_t transcodingCount() const;
    /// 当前在外的句柄数
    size_t handleCount() const;
    /// 一行统计（与 `MediaSource::dumpStats()` 同一风格）
    std::string dumpStats() const;
    /// JSON 对象**片段**（不含最外层花括号），供 `/api/stats` 直接拼接（FR-6.1）
    /// @note 形态：`"source_manager":{...},"media_sources":[{...},...]`
    std::string dumpStatsJson() const;
    /// 最近一次失败原因（快照）
    std::string lastError() const;

    // 观测
    uint64_t totalCreated() const {
        return _total_created.load();
    }
    uint64_t totalReused() const {
        return _total_reused.load();
    }
    uint64_t totalIdleReleased() const {
        return _total_idle_released.load();
    }
    uint64_t totalReleased() const {
        return _total_released.load();
    }
    uint64_t totalAcquireRejected() const {
        return _total_acquire_rejected.load();
    }
    uint64_t totalIdleScheduleFailed() const {
        return _total_idle_schedule_failed.load();
    }
    /// 因"还有人订阅"而推迟的空闲释放次数（M6-c）：每次复查到订阅者还在就 +1
    uint64_t totalIdleDeferred() const {
        return _total_idle_deferred.load();
    }
    /// 【M10-b】因"转码并发已满"被拒的次数（**不降级成直通**：宁可不给，也不给假的）
    uint64_t totalTranscodeRejected() const {
        return _total_transcode_rejected.load();
    }

private:
    struct Entry {
        /// map 键（直通源 = 文件路径；转码源 = `路径|h=720` / `路径|src`）
        std::string key;
        /// 真实文件路径（日志/stats 里给人看的那个）
        std::string file;
        TranscodeSpec spec;
        MediaSource::Ptr source;
        std::shared_ptr<SourcePump> pump;
        std::shared_ptr<DemuxerProducer> producer;
        size_t handles = 0;
        EventPoller::DelayTask::Ptr idle_task;
    };

    explicit SourceManager(const EventPoller::Ptr &poller);

    MediaSource::Ptr makeHandle(const std::shared_ptr<Entry> &entry);
    void onHandleReleased(const std::string &key);
    /// **必须在轮询线程调用**（doDelayTask 的约定）
    void armIdleTimer(const std::string &key);
    void onIdleTimeout(const std::string &key);
    /// 取条目（返回副本，供**不持锁**时用它的字段）；没有则 nullptr
    std::shared_ptr<Entry> findEntry(const std::string &key) const;
    /// 【M10-b】在转码的源数（**调用方必须已持锁**：dumpStatsJson 在锁内拼这段）
    size_t transcodingCountLocked() const;
    /// 空闲释放的统一守卫（M6-c 修的真问题）：**有订阅者就不释放**
    /// @return true = 已释放；false = 还有订阅者（已重新安排复查）
    bool tryIdleRelease(const std::string &key, uint32_t idle_ms);
    /// 还有订阅者时的复查调度（**必须在轮询线程调用**）
    void scheduleIdleRecheck(const std::string &key, uint32_t delay_ms);
    /// 摘除条目（锁内）+ 停线程（锁外，join 可能耗时）
    void releaseEntry(const std::string &key, bool idle_triggered);
    static void stopEntry(const std::shared_ptr<Entry> &entry);

    static constexpr uint32_t kHardMaxIdleMs = 30u * 60u * 1000u; // 30 分钟
    /// 转码并发的硬上限（按核数 × 8 都够了；再大就是配置错误，不是"想开更多"）
    static constexpr size_t kHardMaxTranscoding = 64;

    EventPoller::Ptr _poller;
    mutable std::mutex _mutex;
    Config _config;
    std::map<std::string, std::shared_ptr<Entry>> _entries;
    std::string _last_error;

    std::atomic<uint64_t> _total_created{0};
    std::atomic<uint64_t> _total_reused{0};
    std::atomic<uint64_t> _total_idle_released{0};
    std::atomic<uint64_t> _total_released{0};
    std::atomic<uint64_t> _total_acquire_rejected{0};
    std::atomic<uint64_t> _total_idle_schedule_failed{0};
    std::atomic<uint64_t> _total_idle_deferred{0};
    std::atomic<uint64_t> _total_transcode_rejected{0};
    std::atomic<uint64_t> _handles{0};
};

} // namespace mzmedia
