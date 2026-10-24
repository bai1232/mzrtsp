/*
 * HlsService 实现（M9-b）
 * ============================================================================
 * 三条容易写错的规则，都在下面用注释标了原因：
 *   · 订阅者绑的是 `_poller`（不是 HTTP 连接所属的 poller）：切片器是**一源一份**的，
 *     它必须有一个固定线程跑 drain，不能跟着某个请求的 poller 走；
 *   · 租约必须用**定时器**扫（不能只靠"下次请求时顺手清理"）：没人来请求时也得释放；
 *   · 释放要在**锁外**做：drop 订阅者/源句柄可能触发源线程 join，持锁会卡住所有 HTTP 线程。
 * ============================================================================
 */

#include "output/hls_service.h"

#include "core/logger.h"
#include "core/util.h"

namespace mzmedia {

HlsService::HlsService(const SourceManager::Ptr &manager, const EventPoller::Ptr &poller,
                       const Config &config)
    : _manager(manager)
    , _poller(poller)
    , _config(config) {}

HlsService::Ptr HlsService::create(const SourceManager::Ptr &manager, const EventPoller::Ptr &poller,
                                   const Config &config) {
    if (!manager) {
        ErrorP("HlsService::create: SourceManager 为空（无法懒启动源）");
        return nullptr;
    }
    // 有界性不可协商：0 不是"不限"
    if (config.lease_ms == 0) {
        ErrorP("HlsService::create: lease_ms 不能为 0（0 不是无限租约，而是非法）");
        return nullptr;
    }
    if (config.sweep_ms == 0) {
        ErrorP("HlsService::create: sweep_ms 不能为 0（不扫 = 永不释放 = 泄漏）");
        return nullptr;
    }
    auto service = Ptr(new HlsService(manager, poller, config));
    service->scheduleSweep();
    InfoP("HlsService: 就绪（租约 %u ms / 扫描 %u ms / 目标切片 %u ms / 窗口 %zu）",
          config.lease_ms, config.sweep_ms, config.writer.target_duration_ms, config.writer.window);
    return service;
}

HlsService::~HlsService() {
    shutdown();
}

void HlsService::scheduleSweep() {
    if (!_poller) {
        WarnP("HlsService: 没有 poller → 不做租约扫描（源不会自动释放，属配置错误）");
        return;
    }
    auto self = shared_from_this();
    const uint32_t sweep_ms = _config.sweep_ms;
    // doDelayTask 必须在轮询线程排；create() 在主线程 → 用 async 投递（会内联执行，若已在轮询线程）
    if (!_poller->async([self, sweep_ms]() {
            self->_sweep_task = self->_poller->doDelayTask(sweep_ms, [self, sweep_ms]() -> uint64_t {
                self->sweepInLoop();
                return sweep_ms;   // 返回非 0 = 继续重复
            });
            if (!self->_sweep_task) {
                WarnP("HlsService: 租约扫描定时器创建失败 → 空闲源不会被释放");
            }
        })) {
        WarnP("HlsService: 无法排入租约扫描任务（poller 已退出）→ 空闲源不会被释放");
    }
}

void HlsService::sweepInLoop() {
    const uint64_t now = getCurrentMillisecond();
    std::vector<std::string> to_release;
    std::vector<std::shared_ptr<Entry>> doomed;   // 锁外析构：drop 订阅者可能等源线程 join
    {
        std::lock_guard<std::mutex> lck(_mutex);
        if (_stopped) {
            return;
        }
        for (auto &item : _entries) {
            Entry &entry = *item.second;
            if (!entry.subscriber) {
                to_release.push_back(item.first);   // 不该发生；防御
                continue;
            }
            if (entry.subscriber->broken()) {
                ++_total_broken;
                WarnP("HlsService[%s]: 订阅者 broken（切片队列放不下关键帧）→ 释放该流",
                      item.first.c_str());
                to_release.push_back(item.first);
                continue;
            }
            const uint64_t idle = (now > entry.last_access_ms) ? (now - entry.last_access_ms) : 0;
            if (idle >= _config.lease_ms) {
                ++_total_lease_expired;
                InfoP("HlsService[%s]: 租约到期（空闲 %llu ms ≥ %u）→ 释放源", item.first.c_str(),
                      static_cast<unsigned long long>(idle), _config.lease_ms);
                to_release.push_back(item.first);
            }
        }
        for (const auto &name : to_release) {
            auto it = _entries.find(name);
            if (it != _entries.end()) {
                doomed.push_back(it->second);   // 先接住，erase 不会立刻析构
                _entries.erase(it);
            }
        }
    }
    // 锁外析构：drop Subscriber（退订，源侧自动注销）与 MediaSource 句柄（允许空闲释放）
    doomed.clear();
}

std::shared_ptr<HlsService::Entry> HlsService::acquireEntry(const std::string &name,
                                                           const std::string &path,
                                                           std::string *error) {
    {
        std::lock_guard<std::mutex> lck(_mutex);
        auto it = _entries.find(name);
        if (it != _entries.end()) {
            touchLocked(*it->second);
            return it->second;
        }
    }

    // 建源 + 切片器（**不要在持锁时做**：acquire 会 open 文件、可能耗时）
    MediaSource::Ptr source = _manager->acquire(path);
    if (!source) {
        if (error != nullptr) {
            *error = "源打不开（" + _manager->lastError() + "）";
        }
        return nullptr;
    }
    const SourceManager::MuxerStreams streams = _manager->muxerStreamsFor(path);
    if (streams.video == nullptr && streams.audio == nullptr) {
        if (error != nullptr) {
            *error = "这个源没有音视频流";
        }
        return nullptr;
    }
    HlsWriter::Config writer_config = _config.writer;
    writer_config.name = name;
    HlsWriter::Ptr writer = HlsWriter::create(
        writer_config, MpegTsMuxer::Streams{streams.video, streams.audio});
    if (!writer) {
        if (error != nullptr) {
            *error = "这路流封不了 TS（HLS 需要 H264 + AAC，且要有 avcC/AudioSpecificConfig）";
        }
        return nullptr;
    }
    Subscriber::Ptr subscriber = source->subscribe();
    if (!subscriber) {
        if (error != nullptr) {
            *error = "订阅失败（人数上限 / 关键帧放不进队列）";
        }
        return nullptr;
    }
    // 切片器的消费线程固定为 `_poller`（一源一份，不能跟着某个请求的连接走）
    (void) subscriber->bindPoller(_poller);
    std::weak_ptr<HlsWriter> weak_writer = writer;
    (void) subscriber->setDrainCallback([weak_writer](const Subscriber::Ptr &sub) {
        HlsWriter::Ptr w = weak_writer.lock();
        if (!w) {
            return;   // 切片器已释放：不再消费（订阅者会被一起释放）
        }
        MediaPacket::Ptr packet;
        while (sub->queue().pop(&packet) == FrameQueue::PopResult::Packet) {
            (void) w->onPacket(*packet);   // 失败已记日志 + 计数（不在这里抛）
        }
        if (sub->queue().endOfStream()) {
            w->finish();   // EOS → 播放列表加 #EXT-X-ENDLIST（播放器才会正常收尾）
        }
    });

    auto entry = std::make_shared<Entry>();
    entry->source = source;
    entry->subscriber = subscriber;
    entry->writer = writer;
    entry->last_access_ms = getCurrentMillisecond();
    {
        std::lock_guard<std::mutex> lck(_mutex);
        // 并发：另一个请求可能刚建好了同一个名字 → 以先到的为准，本条目直接丢弃（会退订）
        auto it = _entries.find(name);
        if (it != _entries.end()) {
            touchLocked(*it->second);
            return it->second;
        }
        _entries.emplace(name, entry);
    }
    InfoP("HlsService[%s]: 已创建 HLS 流（源 %s）", name.c_str(), path.c_str());
    return entry;
}

void HlsService::touchLocked(Entry &entry) {
    entry.last_access_ms = getCurrentMillisecond();
}

bool HlsService::playlist(const std::string &name, const std::string &path, std::string *out,
                          std::string *error) {
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "输出为空";
        }
        return false;
    }
    auto entry = acquireEntry(name, path, error);
    if (!entry) {
        return false;
    }
    *out = entry->writer->playlist();
    return true;
}

bool HlsService::slice(const std::string &name, uint64_t seq, std::string *out, std::string *error) {
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "输出为空";
        }
        return false;
    }
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard<std::mutex> lck(_mutex);
        auto it = _entries.find(name);
        if (it == _entries.end()) {
            if (error != nullptr) {
                *error = "没有这个 HLS 流（播放器必须先取 m3u8）";
            }
            return false;
        }
        touchLocked(*it->second);
        entry = it->second;
    }
    auto slice = entry->writer->findSlice(seq);
    if (!slice) {
        if (error != nullptr) {
            *error = "切片 " + std::to_string(seq) + " 不在当前窗口（已滑出 = 正常的老请求）";
        }
        return false;
    }
    *out = slice->bytes;
    ++_total_slices_served;
    return true;
}

void HlsService::releaseEntry(const std::string &name, const char *why) {
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard<std::mutex> lck(_mutex);
        auto it = _entries.find(name);
        if (it == _entries.end()) {
            return;
        }
        entry = it->second;
        _entries.erase(it);
    }
    InfoP("HlsService[%s]: 释放（%s）", name.c_str(), why);
    // entry 在这里析构：drop Subscriber → 源侧自动注销；drop MediaSource 句柄 → 允许空闲释放
}

size_t HlsService::streamCount() const {
    std::lock_guard<std::mutex> lck(_mutex);
    return _entries.size();
}

uint64_t HlsService::totalLeaseExpired() const {
    return _total_lease_expired.load();
}

uint64_t HlsService::totalSlicesServed() const {
    return _total_slices_served.load();
}

std::string HlsService::dumpStatsJson() const {
    std::lock_guard<std::mutex> lck(_mutex);
    std::string json = "\"hls\":{\"streams\":";
    json += std::to_string(_entries.size());
    json += ",\"lease_expired\":";
    json += std::to_string(_total_lease_expired.load());
    json += ",\"slices_served\":";
    json += std::to_string(_total_slices_served.load());
    json += ",\"broken\":";
    json += std::to_string(_total_broken.load());
    json += ",\"lease_ms\":";
    json += std::to_string(_config.lease_ms);
    json += ",\"target_duration_ms\":";
    json += std::to_string(_config.writer.target_duration_ms);
    json += ",\"window\":";
    json += std::to_string(_config.writer.window);
    json += ",\"playlists\":[";
    bool first = true;
    for (const auto &item : _entries) {
        if (!first) {
            json += ",";
        }
        first = false;
        json += "{\"name\":\"" + item.first + "\"";
        if (item.second->writer) {
            json += ",\"slices\":" + std::to_string(item.second->writer->sliceCount());
            json += ",\"produced\":" + std::to_string(item.second->writer->slicesProduced());
            json += ",\"dropped\":" + std::to_string(item.second->writer->slicesDropped());
            json += ",\"bytes\":" + std::to_string(item.second->writer->bytesInRing());
            json += ",\"finished\":";
            json += item.second->writer->finished() ? "true" : "false";
        }
        json += "}";
    }
    json += "]}";
    return json;
}

void HlsService::shutdown() {
    EventPoller::DelayTask::Ptr task;
    {
        std::lock_guard<std::mutex> lck(_mutex);
        if (_stopped) {
            return;
        }
        _stopped = true;
        task = _sweep_task;
        _sweep_task.reset();
    }
    if (task) {
        task->cancel();   // 幂等；在 poller 线程外也能调
    }
    std::map<std::string, std::shared_ptr<Entry>> entries;
    {
        std::lock_guard<std::mutex> lck(_mutex);
        entries.swap(_entries);
    }
    if (!entries.empty()) {
        InfoP("HlsService: 退出时释放 %zu 路 HLS 流", entries.size());
    }
    entries.clear();   // 锁外析构：退订 + 允许源回收
}

} // namespace mzmedia
