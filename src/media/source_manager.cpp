/*
 * SourceManager 实现（M5-c）
 * ============================================================================
 * 三个易错点，都在注释里标死了：
 *   1) **IO 必须在锁外**：open 可能慢甚至超时，而这个锁轮询线程也要拿（挂空闲计时器）——
 *      占着它就等于阻塞事件循环；
 *   2) **计时器只在轮询线程挂**：doDelayTask 的约定；挂不上就退化为立即释放（绝不静默泄漏）；
 *   3) **停线程在锁外**：pump->stop() 会 join，不要在持有管理器锁时做。
 * ============================================================================
 */

#include "media/source_manager.h"

#include "core/logger.h"

#include <utility>
#include <vector>

namespace mzmedia {

namespace {

/// 最简 JSON 字符串转义（只处理 `"` 与 `\`）：v0.1 的路径来自命令行，不做完整校验
std::string jsonEscape(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

} // namespace

SourceManager::Ptr SourceManager::create(const EventPoller::Ptr &poller) {
    return Ptr(new SourceManager(poller));
}

SourceManager::SourceManager(const EventPoller::Ptr &poller)
    : _poller(poller) {
    if (!poller) {
        // 没有 poller 就挂不了空闲计时：**明确退化为"句柄释放即回收"**，
        // 而不是"看起来在缓存、其实永远不释放"（那就是泄漏）
        _config.idle_release_ms = 0;
        WarnL << "SourceManager：未提供 EventPoller → 关闭空闲缓存（句柄释放即回收源）";
    }
}

SourceManager::~SourceManager() {
    (void) releaseAll();
}

bool SourceManager::setConfig(const Config &config) {
    if (config.idle_release_ms > kHardMaxIdleMs) {
        WarnL << "SourceManager::setConfig 被拒：idle_release_ms 超过硬上限 " << kHardMaxIdleMs;
        return false;
    }
    // 每源上限复用 MediaSource 自己的校验（避免同一套规则写两遍然后漂移）
    MediaSource probe;
    if (!probe.setLimits(config.source)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _config = config;
    if (!_poller) {
        _config.idle_release_ms = 0; // 没有 poller 就没有空闲缓存这一档
    }
    return true;
}

SourceManager::Config SourceManager::config() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _config;
}

MediaSource::Ptr SourceManager::acquire(const std::string &path) {
    if (path.empty()) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _last_error = "path 为空";
        }
        ++_total_acquire_rejected;
        WarnL << "SourceManager::acquire 被拒：path 为空";
        return nullptr;
    }

    Config config;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto it = _entries.find(path);
        if (it != _entries.end()) {
            const auto &entry = it->second;
            // 复用：取消空闲计时（cancel 只写一个 atomic，任意线程可调）
            if (entry->idle_task) {
                entry->idle_task->cancel();
                entry->idle_task.reset();
            }
            ++entry->handles;
            ++_handles;
            ++_total_reused;
            return makeHandle(entry);
        }
        config = _config;
    }

    // ---- 懒启动：IO 一律在锁外做 ----
    auto fresh = std::make_shared<Entry>();
    fresh->path = path;
    fresh->producer = std::make_shared<DemuxerProducer>(config.producer);
    if (!fresh->producer->open(path)) {
        const std::string reason = fresh->producer->lastError();
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _last_error = reason;
        }
        ++_total_acquire_rejected;
        WarnL << "SourceManager::acquire 失败（不注册源）：" << path << " → " << reason;
        return nullptr;
    }

    fresh->pump = std::make_shared<SourcePump>();
    // 节流**必须在 start() 之前配**（FR-3.5）：默认 1 倍速，压测/单测可调 speed
    if (!fresh->pump->setThrottleConfig(config.throttle)) {
        const std::string reason = "节流参数非法（speed 必须为正数且有限）";
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _last_error = reason;
        }
        ++_total_acquire_rejected;
        WarnL << "SourceManager::acquire 失败（不注册源）：" << reason;
        return nullptr;
    }
    // 把"要停了"接进 FFmpeg 的 interrupt_callback：stop() 才能真正打断 av_read_frame
    (void) fresh->producer->setAbortFlag(&fresh->pump->stopFlag());
    fresh->source = std::make_shared<MediaSource>(config.source);

    auto producer = fresh->producer;
    auto source = fresh->source;
    if (!fresh->pump->start(
            [producer](MediaPacket::Ptr *packet, std::string *error) {
                return producer->read(packet, error);
            },
            source)) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _last_error = "源线程启动失败";
        }
        ++_total_acquire_rejected;
        WarnL << "SourceManager::acquire 失败：源线程启动失败（" << path << "）";
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        // 双重检查：open 期间可能已经有人把同一个 path 建好了（那条路也要走复用）
        const auto it = _entries.find(path);
        if (it != _entries.end()) {
            const auto &existing = it->second;
            if (existing->idle_task) {
                existing->idle_task->cancel();
                existing->idle_task.reset();
            }
            ++existing->handles;
            ++_handles;
            ++_total_reused;
            stopEntry(fresh); // 我们多建的那份作废（停线程很快：中止标志会打断读）
            return makeHandle(existing);
        }
        ++fresh->handles;
        ++_handles;
        _entries[path] = fresh;
        ++_total_created;
    }

    InfoL << "源已创建（懒启动）：" << path; // FR-6.3：关键事件（源创建）必须落日志
    return makeHandle(fresh);
}

MediaSource::Ptr SourceManager::makeHandle(const std::shared_ptr<Entry> &entry) {
    // 句柄的写法：**自定义 deleter + 捕获强引用 keep**
    //   · keep 让 MediaSource 活到最后一个句柄释放（即使条目已被 releaseAll 摘掉，也不会悬垂）
    //   · deleter 只通过 weak_ptr 回调管理器：管理器已析构时是 no-op
    MediaSource::Ptr keep = entry->source;
    MediaSource *raw = keep.get();
    std::weak_ptr<SourceManager> weak_self = weak_from_this();
    const std::string path = entry->path;
    return MediaSource::Ptr(raw, [weak_self, path, keep](MediaSource *) {
        if (auto self = weak_self.lock()) {
            self->onHandleReleased(path);
        }
    });
}

void SourceManager::onHandleReleased(const std::string &path) {
    uint32_t idle_ms = 0;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto it = _entries.find(path);
        if (it == _entries.end()) {
            return; // 已经被释放（例如 releaseAll）→ 生命周期交给句柄里的 keep
        }
        Entry &entry = *it->second;
        if (entry.handles > 0) {
            --entry.handles;
        }
        if (_handles.load() > 0) {
            --_handles;
        }
        if (entry.handles > 0) {
            return; // 还有别的句柄在用，源继续跑
        }
        idle_ms = _config.idle_release_ms;
    }

    if (idle_ms == 0 || !_poller) {
        releaseEntry(path, true); // 不缓存：立即回收（idle 语义，故计入 idle_released）
        return;
    }

    // 计时器只能在轮询线程挂 → 投递过去；本就在轮询线程时 async 会内联执行
    std::weak_ptr<SourceManager> weak_self = weak_from_this();
    const bool posted = _poller->async([weak_self, path] {
        if (auto self = weak_self.lock()) {
            self->armIdleTimer(path);
        }
    });
    if (!posted) {
        // poller 已退出：**绝不能静默"永不释放"**（那就是泄漏）→ 退化为立即释放
        ++_total_idle_schedule_failed;
        WarnL << "SourceManager：poller 已退出，源 " << path << " 退化为立即释放";
        releaseEntry(path, true);
    }
}

void SourceManager::armIdleTimer(const std::string &path) {
    uint32_t idle_ms = 0;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto it = _entries.find(path);
        if (it == _entries.end()) {
            return; // 已被释放
        }
        const Entry &entry = *it->second;
        if (entry.handles > 0 || entry.idle_task) {
            return; // 期间又被 acquire 了 / 已经挂过计时
        }
        idle_ms = _config.idle_release_ms;
    }

    if (idle_ms == 0 || !_poller) {
        releaseEntry(path, true);
        return;
    }

    std::weak_ptr<SourceManager> weak_self = weak_from_this();
    EventPoller::DelayTask::Ptr task = _poller->doDelayTask(idle_ms, [weak_self, path]() -> uint64_t {
        if (auto self = weak_self.lock()) {
            self->onIdleTimeout(path);
        }
        return 0; // 一次性
    });
    if (!task) {
        ++_total_idle_schedule_failed;
        WarnL << "SourceManager：定时器被拒（poller 已退出），源 " << path << " 立即释放";
        releaseEntry(path, true);
        return;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    const auto it = _entries.find(path);
    if (it == _entries.end()) {
        task->cancel(); // 挂表期间被释放
        return;
    }
    it->second->idle_task = task;
}

void SourceManager::onIdleTimeout(const std::string &path) {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto it = _entries.find(path);
        if (it == _entries.end()) {
            return;
        }
        if (it->second->handles > 0) {
            return; // 又有人用了（理论上计时已被取消，这里再兜一层）
        }
    }
    releaseEntry(path, true);
}

bool SourceManager::release(const std::string &path) {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_entries.find(path) == _entries.end()) {
            return false;
        }
    }
    releaseEntry(path, false);
    return true;
}

size_t SourceManager::releaseAll() {
    std::vector<std::string> paths;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        paths.reserve(_entries.size());
        for (const auto &kv : _entries) {
            paths.push_back(kv.first);
        }
    }
    for (const auto &path : paths) {
        releaseEntry(path, false);
    }
    return paths.size();
}

void SourceManager::releaseEntry(const std::string &path, bool idle_triggered) {
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto it = _entries.find(path);
        if (it == _entries.end()) {
            return;
        }
        entry = it->second;
        if (entry->idle_task) {
            entry->idle_task->cancel();
            entry->idle_task.reset();
        }
        _entries.erase(it);
        // 计数与"条目已摘除"放在**同一个临界区**：外部看到 sourceCount()==0 时计数必定已可见。
        // 曾经的写法是"先摘条目 → join → 再计数"，join 慢的时候（例如源线程正睡在节流片里）
        // 观察者会读到一个"已经没了但还没记账"的中间态。
        if (idle_triggered) {
            ++_total_idle_released;
        } else {
            ++_total_released;
        }
    }

    stopEntry(entry); // 锁外：join 可能耗时

    if (idle_triggered) {
        InfoL << "源空闲释放：" << path; // FR-6.3：关键事件（源释放）必须落日志
    } else {
        InfoL << "源已释放：" << path;
    }
}

void SourceManager::stopEntry(const std::shared_ptr<Entry> &entry) {
    if (entry && entry->pump) {
        (void) entry->pump->stop(); // 中止标志已接上 → 能立刻打断正在进行的读
    }
}

size_t SourceManager::sourceCount() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _entries.size();
}

size_t SourceManager::handleCount() const {
    return static_cast<size_t>(_handles.load());
}

std::string SourceManager::dumpStats() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return "source_manager{sources=" + std::to_string(_entries.size()) +
           " handles=" + std::to_string(_handles.load()) +
           " idle_release_ms=" + std::to_string(_config.idle_release_ms) +
           " created=" + std::to_string(_total_created.load()) +
           " reused=" + std::to_string(_total_reused.load()) +
           " idle_released=" + std::to_string(_total_idle_released.load()) +
           " released=" + std::to_string(_total_released.load()) +
           " rejected=" + std::to_string(_total_acquire_rejected.load()) +
           " idle_schedule_failed=" + std::to_string(_total_idle_schedule_failed.load()) + "}";
}

std::string SourceManager::dumpStatsJson() const {
    // 契约：返回**合法 JSON 对象片段**（不含最外层花括号），键名用模块名做前缀避免与 http 侧撞名
    std::string out = "\"source_manager\":{\"sources\":";
    std::vector<std::pair<std::string, MediaSource::Ptr>> snapshot; // 锁外取每个源的统计
    {
        std::lock_guard<std::mutex> lock(_mutex);
        out += std::to_string(_entries.size());
        out += ",\"handles\":" + std::to_string(_handles.load());
        out += ",\"idle_release_ms\":" + std::to_string(_config.idle_release_ms);
        out += ",\"created\":" + std::to_string(_total_created.load());
        out += ",\"reused\":" + std::to_string(_total_reused.load());
        out += ",\"idle_released\":" + std::to_string(_total_idle_released.load());
        out += ",\"released\":" + std::to_string(_total_released.load());
        out += ",\"rejected\":" + std::to_string(_total_acquire_rejected.load());
        out += ",\"idle_schedule_failed\":" + std::to_string(_total_idle_schedule_failed.load());
        out += "}";
        for (const auto &kv : _entries) {
            if (auto source = kv.second->source) {
                snapshot.emplace_back(kv.first, source);
            }
        }
    }

    // **锁外**逐个取源统计：锁序固定为「管理器 → 源」，绝不允许反向
    out += ",\"media_sources\":[";
    bool first = true;
    for (const auto &item : snapshot) {
        if (!first) {
            out += ",";
        }
        first = false;
        out += "{\"path\":\"" + jsonEscape(item.first) + "\"," + item.second->dumpStatsJson() + "}";
    }
    out += "]";
    return out;
}

std::string SourceManager::lastError() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _last_error;
}

} // namespace mzmedia
