/*
 * HlsService：HLS 的"HTTP 侧面"—— 名字 → (源 + 切片器) 的注册表 + 租约式空闲释放（M9-b）
 * ============================================================================
 * 为什么单独一层（而不是塞进 `HttpServer` 或 `main.cpp`）：
 *   · `http/` 不允许依赖 `media/`（ARCHITECTURE.md §1），而 HLS 要"按名字拿到源"；
 *   · `main.cpp` 只做接线，不该管租约/定时器/生命周期；
 *   · 于是这一层放在 `output/`（允许依赖 media），对上只暴露两个只读入口：
 *       `playlist(name, path, out, error)` / `slice(name, seq, out, error)`
 *
 * **生命周期（FR-1.3 的 HLS 版本）**：HLS 是拉模型，服务端看不到"连接"。所以用**租约**近似：
 *   · 任意 `/hls/` 请求（m3u8 或切片）把该名字的租约续到 `lease_ms`（默认 60s，与 FR-1.3 同量级）；
 *   · 租约到期且没有订阅者 → 释放（drop writer + 订阅者 + 源句柄 → 源被 SourceManager 回收）；
 *   · 定时器每 `sweep_ms` 扫一遍（**必须是定时器**：没人来请求时也得能释放，否则就是泄漏）。
 *
 * 切片共享：一个名字只有**一个** `HlsWriter`（= 一源一份输出，FR-1.1）。N 个播放器看同一路，
 * 服务端只切一份 —— 这与 FLV 的"每连接一份 sender"是本质区别（见 DESIGN_M9 §3.2）。
 * ============================================================================
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "media/source_manager.h"
#include "network/event_poller.h"
#include "output/hls_writer.h"

namespace mzmedia {

class HlsService : public std::enable_shared_from_this<HlsService> {
public:
    using Ptr = std::shared_ptr<HlsService>;

    struct Config {
        HlsWriter::Config writer;        ///< 切片参数（`name` 每次按请求覆盖）
        uint32_t lease_ms = 60000;       ///< 拉流请求的租约（FR-1.3 的 60s 同量级）
        uint32_t sweep_ms = 5000;        ///< 扫描间隔（0 = 非法：不能不扫，否则永不释放）
    };

    /// @param manager 用来按名字懒启动/复用源
    /// @param poller 订阅者的唤醒线程 + 租约定时器所在线程（通常是 app 的 poller）
    static Ptr create(const SourceManager::Ptr &manager, const EventPoller::Ptr &poller,
                      const Config &config);
    ~HlsService();
    HlsService(const HlsService &) = delete;
    HlsService &operator=(const HlsService &) = delete;

    /// 停掉定时器并释放全部（等源线程回收）；幂等
    void shutdown();

    /**
     * `GET /hls/<name>.m3u8`
     * @param path 媒体文件路径（由调用方从名字解析出来；本层不认识 media_root）
     * @return false = 打不开/封不了（`error` 是人话，调用方据此回 4xx/5xx）
     */
    bool playlist(const std::string &name, const std::string &path, std::string *out,
                  std::string *error);

    /**
     * `GET /hls/<name>-<seq>.ts`
     * @return false = 没有这个流 / 该序号已滑出窗口（播放器拿旧 m3u8 请求老切片是**正常**的 404）
     */
    bool slice(const std::string &name, uint64_t seq, std::string *out, std::string *error);

    // 观测（接进 /api/stats）
    size_t streamCount() const;
    uint64_t totalLeaseExpired() const;
    uint64_t totalSlicesServed() const;
    std::string dumpStatsJson() const;

private:
    struct Entry {
        MediaSource::Ptr source;    ///< 句柄：让源活着（否则空闲释放会把它停掉）
        Subscriber::Ptr subscriber; ///< 订阅者：释放它 = 退订（NFR-6）
        HlsWriter::Ptr writer;
        uint64_t last_access_ms = 0;
    };

    HlsService(const SourceManager::Ptr &manager, const EventPoller::Ptr &poller, const Config &config);

    /// 拿到（必要时创建）某个名字的条目；失败返回 nullptr 且写 `error`
    std::shared_ptr<Entry> acquireEntry(const std::string &name, const std::string &path,
                                        std::string *error);
    void touchLocked(Entry &entry);
    /// 释放条目（**在锁外**调用；会 drop 订阅者/句柄）
    void releaseEntry(const std::string &name, const char *why);
    void scheduleSweep();
    /// 必须在 poller 线程
    void sweepInLoop();

    SourceManager::Ptr _manager;
    EventPoller::Ptr _poller;
    Config _config;

    mutable std::mutex _mutex;
    std::map<std::string, std::shared_ptr<Entry>> _entries;
    EventPoller::DelayTask::Ptr _sweep_task;
    bool _stopped = false;

    std::atomic<uint64_t> _total_lease_expired{0};
    std::atomic<uint64_t> _total_slices_served{0};
    std::atomic<uint64_t> _total_broken{0};
};

} // namespace mzmedia
