/*
 * 源管理单元测试（M5-c，分组 `srcmgr`）
 * ============================================================================
 * 契约来源：docs/DESIGN_M5.md §3.6 / §3.7 / §5.3；上位需求 FR-1.2（从头推流、按需启动）、
 *           FR-2.1（v0.1 支持 MP4 / H264 裸流）、FR-6.3（源创建/释放落日志）、NFR-6（资源回收）
 *
 * 覆盖维度：
 *   正常  真读 sample.mp4（视频+音频）到 EOF；懒启动、复用同一源
 *   空    不存在的文件 → **明确失败**（返回 nullptr + 计数 + lastError），不注册源
 *   满    —（源数没有硬上限：每个 path 一个源，`max_subscribers` 由 MediaSource 管）
 *   断开  句柄释放 → 空闲计时 → 停线程回收；`releaseAll()` 立即回收；再 acquire 会重建
 *   超大  —（解封装上限由 M4 的 Demuxer 用例覆盖）
 *
 * 为什么能进 TSAN 严格组：只用**无超时**等待（`Semaphore::wait()` / `sleep_for` + 有界轮询），
 * 不碰本环境已知的 `condition_variable` 超时误报。
 *
 * 依赖样本（由 `scripts/make_samples.sh` 现场生成）；找不到样本时用例**明确失败**，
 * 绝不静默跳过（"没测到"不能看起来像"测过了"）。
 * ============================================================================
 */

#include "test_main.h"

#include "core/semaphore.h"
#include "core/util.h"
#include "media/demuxer_producer.h"
#include "media/source_manager.h"
#include "network/event_poller.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace mzmedia;

namespace {

/// 找样本（与 `test_ffmpeg.cpp` 同一算法：向上最多 8 层，与从哪启动无关）
std::string samplePath(const std::string &name) {
    std::vector<std::string> candidates;
    if (const char *env = ::getenv("MZ_SAMPLES_DIR"); env != nullptr) {
        candidates.push_back(std::string(env) + "/" + name);
    }
    std::string prefix;
    for (int depth = 0; depth < 8; ++depth) {
        candidates.push_back(prefix + "samples/" + name);
        prefix += "../";
    }
    for (const auto &c : candidates) {
        if (FILE *f = ::fopen(c.c_str(), "rb"); f != nullptr) {
            ::fclose(f);
            return c;
        }
    }
    return {};
}

/// 有界轮询：等条件成立（最多约 timeout_ms），返回是否等到
template<typename Fn>
bool waitFor(Fn condition, int timeout_ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return condition();
}

/// 把订阅者的队列取空
std::vector<MediaPacket::Ptr> drain(FrameQueue *queue) {
    std::vector<MediaPacket::Ptr> out;
    MediaPacket::Ptr packet;
    while (queue->pop(&packet) == FrameQueue::PopResult::Packet) {
        out.push_back(packet);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// DemuxerProducer：把 Demuxer 包成读回调
// ---------------------------------------------------------------------------

MZ_TEST(srcmgr_demuxer_producer_reads_mp4) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty()); // 找不到样本 → 明确失败（不是跳过）
    if (path.empty()) {
        return;
    }

    DemuxerProducer producer;
    MZ_ASSERT_TRUE(producer.open(path));
    MZ_ASSERT_TRUE(producer.opened());
    MZ_ASSERT_STR_EQ(producer.path(), path);

    // 流信息：样本是 320x240 H264 + AAC（见 scripts/make_samples.sh）
    const Demuxer &demux = producer.demuxer();
    MZ_ASSERT_NOT_NULL(demux.firstVideo());
    if (demux.firstVideo() != nullptr) {
        MZ_ASSERT_EQ(demux.firstVideo()->width, 320);
        MZ_ASSERT_EQ(demux.firstVideo()->height, 240);
    }
    MZ_ASSERT_NOT_NULL(demux.firstAudio());

    int video_packets = 0;
    int audio_packets = 0;
    int key_frames = 0;
    int64_t last_dts = -1;
    bool dts_monotonic = true;

    while (true) {
        MediaPacket::Ptr packet;
        std::string error;
        const auto result = producer.read(&packet, &error);
        if (result == SourcePump::ReadResult::EndOfStream) {
            break;
        }
        MZ_ASSERT_EQ(result, SourcePump::ReadResult::Packet);
        if (result != SourcePump::ReadResult::Packet) {
            MZ_FAIL("读包失败：" + error);
            return;
        }
        if (packet->kind() == MediaKind::Video) {
            ++video_packets;
            if (packet->isKeyFrame()) {
                ++key_frames;
            }
            if (packet->dtsMs() < last_dts) {
                dts_monotonic = false;
            }
            last_dts = packet->dtsMs();
        } else {
            ++audio_packets;
        }
        MZ_ASSERT_GT(packet->size(), 0u);
    }

    MZ_ASSERT_GT(video_packets, 0);
    MZ_ASSERT_GT(audio_packets, 0);
    MZ_ASSERT_EQ(key_frames, 1);            // 2 秒样本只有开头一个关键帧
    MZ_ASSERT_TRUE(dts_monotonic);          // 视频 dts 必须单调（Demuxer 的守卫保证）

    MZ_ASSERT_EQ(producer.totalPackets(), static_cast<uint64_t>(video_packets + audio_packets));
    MZ_ASSERT_GT(producer.totalBytes(), 0u);
    MZ_ASSERT_EQ(producer.skippedUnknownStreams(), 0u); // MP4 样本里没有字幕/数据流
    MZ_ASSERT_STR_EQ(producer.lastError(), "");
}

// ---------------------------------------------------------------------------
// SourceManager：懒启动 / 复用 / 空闲释放 / 回收
// ---------------------------------------------------------------------------

MZ_TEST(srcmgr_lazy_start_and_reuse) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }

    auto poller = EventPoller::create("test-srcmgr-reuse");
    auto manager = SourceManager::create(poller);
    MZ_ASSERT_EQ(manager->sourceCount(), 0u); // 懒：还没 acquire 就没有源

    auto h1 = manager->acquire(path);
    MZ_ASSERT_NOT_NULL(h1.get());
    if (!h1) {
        poller->shutdown();
        return;
    }
    MZ_ASSERT_EQ(manager->sourceCount(), 1u);
    MZ_ASSERT_EQ(manager->totalCreated(), 1u);
    MZ_ASSERT_EQ(manager->handleCount(), 1u);

    // 同一个 path 再 acquire → **同一个** MediaSource（一源多消费者）
    auto h2 = manager->acquire(path);
    MZ_ASSERT_NOT_NULL(h2.get());
    if (h2) {
        MZ_ASSERT_TRUE(h1.get() == h2.get());
    }
    MZ_ASSERT_EQ(manager->totalCreated(), 1u); // 没有重建
    MZ_ASSERT_EQ(manager->totalReused(), 1u);
    MZ_ASSERT_EQ(manager->handleCount(), 2u);

    // 源线程真的在跑：订阅者能收到真包的帧（视频关键帧一定在）
    auto sub = h1->subscribe();
    MZ_ASSERT_NOT_NULL(sub.get());
    if (sub) {
        const bool got = waitFor([&] { return sub->queue().packets() > 0; });
        MZ_ASSERT_TRUE(got);
        const auto packets = drain(&sub->queue());
        MZ_ASSERT_GT(packets.size(), 0u);
        // 中途接入 → 第一条必须是视频关键帧（GOP 缓存保证）
        if (!packets.empty()) {
            MZ_ASSERT_TRUE(packets.front()->isKeyFrame());
        }
    }

    // 放掉一个句柄，源仍在（还有人用）
    h2.reset();
    MZ_ASSERT_EQ(manager->sourceCount(), 1u);
    MZ_ASSERT_EQ(manager->handleCount(), 1u);

    (void) poller->shutdown();
}

MZ_TEST(srcmgr_idle_release_without_cache) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }

    // idle = 0 = 句柄一放就释放（不缓存源）。同时也是"没有 poller"时的退化行为
    auto manager = SourceManager::create(nullptr);
    auto cfg = manager->config();
    cfg.idle_release_ms = 0;
    MZ_ASSERT_TRUE(manager->setConfig(cfg));
    MZ_ASSERT_EQ(manager->config().idle_release_ms, 0u);

    auto handle = manager->acquire(path);
    MZ_ASSERT_NOT_NULL(handle.get());
    if (!handle) {
        return;
    }
    MZ_ASSERT_EQ(manager->sourceCount(), 1u);

    handle.reset(); // 放掉最后一个句柄 → 同步回收
    MZ_ASSERT_EQ(manager->sourceCount(), 0u);
    MZ_ASSERT_EQ(manager->totalIdleReleased(), 1u);
    MZ_ASSERT_EQ(manager->handleCount(), 0u);
    MZ_ASSERT_EQ(manager->totalIdleScheduleFailed(), 0u); // 不是"挂表失败"，是策略本身
}

MZ_TEST(srcmgr_idle_timer_releases_after_window) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }

    auto poller = EventPoller::create("test-srcmgr-idle");
    auto manager = SourceManager::create(poller);
    auto cfg = manager->config();
    cfg.idle_release_ms = 50; // 短窗口：只为让用例快速跑完
    MZ_ASSERT_TRUE(manager->setConfig(cfg));

    auto handle = manager->acquire(path);
    MZ_ASSERT_NOT_NULL(handle.get());
    if (!handle) {
        poller->shutdown();
        return;
    }
    handle.reset();

    // 句柄释放后**不能立刻**回收，要等空闲窗口到点
    // 超时给得宽：空闲释放依赖"轮询线程被唤醒 + 定时器到点"，在 ASAN / 满载下会被拖慢
    // （实测：ctest 满载时曾经超过 3s）。失败时打印统计，避免下次只剩一句"期望为真"。
    const bool released = waitFor([&] { return manager->sourceCount() == 0; }, 10000);
    if (!released) {
        MZ_FAIL("空闲释放超时（10s），当前统计：" + manager->dumpStats());
    }
    MZ_ASSERT_TRUE(released);
    MZ_ASSERT_EQ(manager->totalIdleReleased(), 1u);
    MZ_ASSERT_EQ(manager->totalReleased(), 0u); // 不是主动释放
    MZ_ASSERT_EQ(manager->totalIdleScheduleFailed(), 0u);

    (void) poller->shutdown();
}

MZ_TEST(srcmgr_acquire_cancels_idle_timer) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }

    auto poller = EventPoller::create("test-srcmgr-cancel");
    auto manager = SourceManager::create(poller);
    auto cfg = manager->config();
    cfg.idle_release_ms = 120;
    MZ_ASSERT_TRUE(manager->setConfig(cfg));

    auto handle = manager->acquire(path);
    MZ_ASSERT_NOT_NULL(handle.get());
    if (!handle) {
        poller->shutdown();
        return;
    }
    handle.reset(); // 开始空闲计时

    // 计时窗口内又有人来用 → 必须**取消**计时、复用同一个源
    auto again = manager->acquire(path);
    MZ_ASSERT_NOT_NULL(again.get());
    MZ_ASSERT_EQ(manager->totalCreated(), 1u);
    MZ_ASSERT_EQ(manager->totalReused(), 1u);

    // 等过原定时长：源必须还在（计时已被取消，没有偷偷释放）
    std::this_thread::sleep_for(std::chrono::milliseconds(240));
    MZ_ASSERT_EQ(manager->sourceCount(), 1u);
    MZ_ASSERT_EQ(manager->totalIdleReleased(), 0u);

    // 再放掉 → 这次才会被回收（超时同样给宽，理由见上一个用例）
    again.reset();
    const bool released_after = waitFor([&] { return manager->sourceCount() == 0; }, 10000);
    if (!released_after) {
        MZ_FAIL("空闲释放超时（10s），当前统计：" + manager->dumpStats());
    }
    MZ_ASSERT_TRUE(released_after);
    MZ_ASSERT_EQ(manager->totalIdleReleased(), 1u);

    (void) poller->shutdown();
}

MZ_TEST(srcmgr_open_failure_not_silent) {
    auto poller = EventPoller::create("test-srcmgr-bad");
    auto manager = SourceManager::create(poller);

    const std::string bad = "/tmp/mzmedia-不存在的文件-请忽略.mp4";
    auto handle = manager->acquire(bad);
    MZ_ASSERT_NULL(handle.get()); // 打不开 → 明确失败
    MZ_ASSERT_EQ(manager->sourceCount(), 0u);
    MZ_ASSERT_EQ(manager->totalCreated(), 0u);
    MZ_ASSERT_EQ(manager->totalAcquireRejected(), 1u);
    MZ_ASSERT_TRUE(!manager->lastError().empty()); // 原因必须能读到

    // 空 path 也被拒（调用方 bug 不静默）
    MZ_ASSERT_NULL(manager->acquire("").get());
    MZ_ASSERT_EQ(manager->totalAcquireRejected(), 2u);

    (void) poller->shutdown();
}

MZ_TEST(srcmgr_release_all_stops_sources) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }

    auto poller = EventPoller::create("test-srcmgr-releaseall");
    auto manager = SourceManager::create(poller);

    auto handle = manager->acquire(path);
    MZ_ASSERT_NOT_NULL(handle.get());
    if (!handle) {
        poller->shutdown();
        return;
    }
    MZ_ASSERT_EQ(manager->sourceCount(), 1u);

    MZ_ASSERT_EQ(manager->releaseAll(), 1u); // 关停路径：立即停源
    MZ_ASSERT_EQ(manager->sourceCount(), 0u);
    MZ_ASSERT_EQ(manager->totalReleased(), 1u);
    MZ_ASSERT_EQ(manager->totalIdleReleased(), 0u);

    // 句柄还拿着也不悬垂（对象由句柄里的 keep 保住），放掉即可
    MZ_ASSERT_NOT_NULL(handle.get());
    handle.reset();

    // 再 acquire → 重建（计数能证明"真的重建了"，而不是拿到了一个已停的源）
    auto again = manager->acquire(path);
    MZ_ASSERT_NOT_NULL(again.get());
    MZ_ASSERT_EQ(manager->totalCreated(), 2u);
    MZ_ASSERT_EQ(manager->sourceCount(), 1u);

    MZ_ASSERT_TRUE(manager->release(path));
    MZ_ASSERT_FALSE(manager->release(path)); // 第二次不存在

    (void) poller->shutdown();
}

MZ_TEST(srcmgr_dump_stats_minimal) {
    auto manager = SourceManager::create(nullptr);
    const std::string stats = manager->dumpStats();
    MZ_ASSERT_TRUE(stats.find("source_manager{") == 0);
    MZ_ASSERT_TRUE(stats.find("sources=0") != std::string::npos);
    MZ_ASSERT_TRUE(stats.find("handles=0") != std::string::npos);
    MZ_ASSERT_TRUE(stats.find("created=0") != std::string::npos);
    MZ_ASSERT_TRUE(stats.find("idle_schedule_failed=0") != std::string::npos);
}

// ---------------------------------------------------------------------------
// M5-d：统计 JSON 与节流（FR-6.1 / FR-3.5）
// ---------------------------------------------------------------------------

MZ_TEST(srcmgr_dump_stats_json_has_manager_and_sources) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }

    auto poller = EventPoller::create("test-srcmgr-json");
    auto manager = SourceManager::create(poller);
    auto handle = manager->acquire(path);
    MZ_ASSERT_NOT_NULL(handle.get());
    if (!handle) {
        poller->shutdown();
        return;
    }

    const std::string json = manager->dumpStatsJson();
    MZ_ASSERT_TRUE(json.find("\"source_manager\":{") == 0);          // 片段形态：键名做前缀
    MZ_ASSERT_TRUE(json.find("\"sources\":1") != std::string::npos);
    MZ_ASSERT_TRUE(json.find("\"media_sources\":[") != std::string::npos);
    MZ_ASSERT_TRUE(json.find("\"media_source\":{") != std::string::npos);
    MZ_ASSERT_TRUE(json.find("\"subscribers\":0") != std::string::npos);
    MZ_ASSERT_TRUE(json.find("\"counters\":{") != std::string::npos);

    // 最粗的配平检查：花括号必须成对（片段要能直接嵌进 /api/stats 的对象里）
    const size_t open_braces = static_cast<size_t>(std::count(json.begin(), json.end(), '{'));
    const size_t close_braces = static_cast<size_t>(std::count(json.begin(), json.end(), '}'));
    MZ_ASSERT_EQ(open_braces, close_braces);

    (void) poller->shutdown();
}

MZ_TEST(srcmgr_throttle_limits_delivery_rate) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }

    auto poller = EventPoller::create("test-srcmgr-throttle");
    auto manager = SourceManager::create(poller);
    auto cfg = manager->config();
    cfg.throttle.enabled = true;
    cfg.throttle.speed = 8.0; // 样本 2 秒 → 约 250ms 读完（不节流的话是几毫秒）
    MZ_ASSERT_TRUE(manager->setConfig(cfg));

    auto handle = manager->acquire(path);
    MZ_ASSERT_NOT_NULL(handle.get());
    if (!handle) {
        poller->shutdown();
        return;
    }
    auto sub = handle->subscribe();
    MZ_ASSERT_NOT_NULL(sub.get());
    if (!sub) {
        poller->shutdown();
        return;
    }

    const int64_t begin = static_cast<int64_t>(getCurrentMillisecond());
    // 等到源结束（EOS 广播到订阅者队列）
    MZ_ASSERT_TRUE(waitFor([&] { return sub->queue().endOfStream(); }, 5000));
    const int64_t elapsed = static_cast<int64_t>(getCurrentMillisecond()) - begin;

    // 下界证明"没有以磁盘速度全速灌入"（FR-3.5 的核心），上界证明"没有慢到实时"
    MZ_ASSERT_GE(elapsed, 100);
    MZ_ASSERT_LT(elapsed, 1500);
    MZ_ASSERT_GT(sub->queue().stats().pushed, 0u);

    (void) poller->shutdown();
}
