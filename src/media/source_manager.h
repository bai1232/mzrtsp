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
        Throttle::Config throttle;        // FR-3.5：默认开、1 倍速（压测/单测可调 speed）
        MediaSource::Limits source;       // 每源的队列 / GOP / 人数上限（推导值在 Limits 里）
        DemuxerProducer::Config producer; // 解封装上限（流数 / 单包 / 超时）
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

    /// 立刻释放某个源（不等空闲计时）；@return false = 没有这个源
    bool release(const std::string &path);
    /// 释放全部（关停路径用）；@return 释放的源数。**已在外面的句柄仍然有效**（见文件头）
    size_t releaseAll();

    size_t sourceCount() const;
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

private:
    struct Entry {
        std::string path;
        MediaSource::Ptr source;
        std::shared_ptr<SourcePump> pump;
        std::shared_ptr<DemuxerProducer> producer;
        size_t handles = 0;
        EventPoller::DelayTask::Ptr idle_task;
    };

    explicit SourceManager(const EventPoller::Ptr &poller);

    MediaSource::Ptr makeHandle(const std::shared_ptr<Entry> &entry);
    void onHandleReleased(const std::string &path);
    /// **必须在轮询线程调用**（doDelayTask 的约定）
    void armIdleTimer(const std::string &path);
    void onIdleTimeout(const std::string &path);
    /// 摘除条目（锁内）+ 停线程（锁外，join 可能耗时）
    void releaseEntry(const std::string &path, bool idle_triggered);
    static void stopEntry(const std::shared_ptr<Entry> &entry);

    static constexpr uint32_t kHardMaxIdleMs = 30u * 60u * 1000u; // 30 分钟

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
    std::atomic<uint64_t> _handles{0};
};

} // namespace mzmedia
