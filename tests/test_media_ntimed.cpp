/*
 * 媒体分发层「带线程」的单元测试（M5-b，分组 `ntimed_media`）
 * ============================================================================
 * 契约来源：docs/DESIGN_M5.md §3.4 / §3.5 / §4.4；上位需求 FR-5.1 ~ FR-5.3、NFR-6
 *
 * 这个文件与 `test_media.cpp` 的分工：
 *   - `test_media.cpp`：**纯策略**（上限/丢帧/GOP），单线程、确定性；
 *   - 本文件：**线程打通**（队列并发、唤醒合并、源线程），必须真的起线程。
 *
 * 为什么也能进 TSAN **严格组**：全程只用**无超时**等待（`Semaphore::wait()`）与
 * `sleep_for`（不是 `condition_variable` 的超时接口），不会碰到本环境已知的 TSAN 误报。
 * 计时类判据一律不放在这里 —— 并发用例要的是"不丢/不重/不卡"，不是"多快"。
 *
 * 覆盖维度：正常（推-唤醒-取）、空（没有数据不发唤醒）、满（合并唤醒、数据仍在队列）、
 *           断开（poller 退出、源线程被停）、超大（2 万帧并发搬运不丢不重）
 * ============================================================================
 */

#include "test_main.h"

#include "core/semaphore.h"
#include "media/media_source.h"
#include "media/source_pump.h"
#include "network/event_poller.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace mzmedia;

namespace {

MediaPacket::Ptr video(int stream, bool key, size_t bytes, int64_t ms = 0) {
    auto payload = std::make_shared<const std::vector<uint8_t>>(bytes, key ? 0x11 : 0x22);
    return MediaPacket::create(MediaKind::Video, stream, payload, key, ms, ms);
}

} // namespace

// ---------------------------------------------------------------------------
// FrameQueue：SPSC 并发
// ---------------------------------------------------------------------------

MZ_TEST(ntimed_media_queue_spsc_no_loss) {
    // 加锁是否真的到位，只有并发跑才看得出来（TSAN 同时会检查数据竞争）
    FrameQueue queue;
    FrameQueue::Limits limits;
    limits.max_bytes = 64u * 1024u * 1024u; // 20,000 × 16 B = 320 KB，远小于上限
    MZ_ASSERT_TRUE(queue.setLimits(limits));

    const int kCount = 20000;
    std::thread producer([&queue, kCount] {
        for (int i = 0; i < kCount; ++i) {
            (void) queue.push(video(0, (i % 100) == 0, 16, i));
        }
    });

    std::vector<int64_t> received;
    received.reserve(static_cast<size_t>(kCount));
    MediaPacket::Ptr packet;
    while (static_cast<int>(received.size()) < kCount) {
        if (queue.pop(&packet) == FrameQueue::PopResult::Packet) {
            received.push_back(packet->dtsMs());
        }
    }
    producer.join();

    MZ_ASSERT_EQ(received.size(), static_cast<size_t>(kCount));
    bool in_order = true;
    for (size_t i = 0; i < received.size(); ++i) {
        if (received[i] != static_cast<int64_t>(i)) {
            in_order = false;
            break;
        }
    }
    MZ_ASSERT_TRUE(in_order); // 单生产单消费 → 顺序也必须成立

    const auto stats = queue.stats();
    MZ_ASSERT_EQ(stats.pushed, static_cast<uint64_t>(kCount));
    MZ_ASSERT_EQ(stats.popped, static_cast<uint64_t>(kCount));
    MZ_ASSERT_EQ(stats.dropped, 0u);
    MZ_ASSERT_EQ(queue.packets(), 0u);
}

// ---------------------------------------------------------------------------
// 唤醒通道：合并 + 在消费者线程 drain + 不丢唤醒
// ---------------------------------------------------------------------------

MZ_TEST(ntimed_media_notify_coalesced_and_drained_on_poller_thread) {
    auto poller = EventPoller::create("test-media-notify");
    // 上限给足：本例要测的是"唤醒合并不丢数据"，不是丢帧策略（丢帧另有专门用例）
    // 4 Mbps × 2 s = 1,000,000 B ≫ 100 帧 × 8 B
    MediaSource::Limits limits;
    limits.max_bitrate_bps = 4000000;
    limits.latency_budget_ms = 2000;
    auto source = std::make_shared<MediaSource>(limits);
    auto sub = source->subscribe();
    MZ_ASSERT_NOT_NULL(sub.get());
    if (!sub) {
        poller->shutdown();
        return;
    }

    Semaphore gate(0);  // 占住轮询线程用
    Semaphore idle(0);  // 每次 drain 结束 post 一次
    std::atomic<int> received{0};
    std::atomic<bool> on_poller_thread{false};
    std::atomic<bool> first_drain{true};

    // 先占住轮询线程：后面的唤醒只能排队，未决标记不会被清 → "合并"可被确定性观察到
    MZ_ASSERT_TRUE(poller->async([&gate] { gate.wait(); }));
    MZ_ASSERT_TRUE(sub->bindPoller(poller));
    sub->setDrainCallback([&](const Subscriber::Ptr &self) {
        if (first_drain.exchange(false)) {
            on_poller_thread.store(poller->isCurrentThread());
        }
        MediaPacket::Ptr packet;
        while (self->queue().pop(&packet) == FrameQueue::PopResult::Packet) {
            received.fetch_add(1);
        }
        idle.post();
    });

    const int kCount = 100;
    for (int i = 0; i < kCount; ++i) {
        (void) source->pushPacket(video(0, false, 8, i));
    }
    // 100 帧只换来 1 次跨线程唤醒；数据全在队列里（唤醒只是"去看一眼"，不是数据通道）
    MZ_ASSERT_EQ(sub->notifyCount(), 1u);
    MZ_ASSERT_GT(sub->notifyCoalescedCount(), 0u);
    MZ_ASSERT_EQ(sub->queue().packets(), static_cast<size_t>(kCount));
    // 等待条件按"实际入队数"算：万一以后上限被调小、有丢帧，也不会把用例挂死
    const int queued = static_cast<int>(sub->queue().packets());

    gate.post(); // 放行轮询线程
    while (received.load() < queued) {
        idle.wait();
    }
    MZ_ASSERT_TRUE(on_poller_thread.load());  // drain 确实跑在轮询线程上
    MZ_ASSERT_EQ(sub->queue().packets(), 0u); // 队列被取空
    MZ_ASSERT_EQ(received.load(), queued);

    // 不丢唤醒：上一轮 drain 结束后进来的数据，必须能再排一次唤醒
    const uint64_t before = sub->notifyCount();
    (void) source->pushPacket(video(0, false, 8, kCount));
    MZ_ASSERT_EQ(sub->notifyCount(), before + 1);
    while (received.load() < queued + 1) {
        idle.wait();
    }
    MZ_ASSERT_EQ(received.load(), queued + 1);

    poller->shutdown();
}

MZ_TEST(ntimed_media_notify_rejected_when_poller_down) {
    // 断开维度：消费者线程已经不在了，唤醒投不出去 —— 必须计数，且**不能把订阅者永久卡住**
    auto poller = EventPoller::create("test-media-notify-down");
    auto source = std::make_shared<MediaSource>();
    auto sub = source->subscribe();
    MZ_ASSERT_NOT_NULL(sub.get());
    if (!sub) {
        poller->shutdown();
        return;
    }
    MZ_ASSERT_TRUE(sub->bindPoller(poller));
    poller->shutdown();

    (void) source->pushPacket(video(0, false, 8, 0));
    MZ_ASSERT_EQ(sub->notifyRejectedCount(), 1u);
    MZ_ASSERT_EQ(sub->notifyCount(), 0u);
    MZ_ASSERT_EQ(sub->queue().packets(), 1u); // 数据仍然在队列里（没丢）

    // 第二次仍然会尝试唤醒（说明标记被复位了，否则会被"未决"永久挡住）
    (void) source->pushPacket(video(0, false, 8, 1));
    MZ_ASSERT_EQ(sub->notifyRejectedCount(), 2u);
    MZ_ASSERT_EQ(sub->queue().packets(), 2u);
}

// ---------------------------------------------------------------------------
// SourcePump：源线程
// ---------------------------------------------------------------------------

MZ_TEST(ntimed_media_pump_reads_until_eof) {
    auto poller = EventPoller::create("test-media-pump-eof");
    auto source = std::make_shared<MediaSource>();
    auto sub = source->subscribe();
    MZ_ASSERT_NOT_NULL(sub.get());
    if (!sub) {
        poller->shutdown();
        return;
    }

    Semaphore idle(0);
    std::atomic<int> received{0};
    MZ_ASSERT_TRUE(sub->bindPoller(poller));
    sub->setDrainCallback([&](const Subscriber::Ptr &self) {
        MediaPacket::Ptr packet;
        while (self->queue().pop(&packet) == FrameQueue::PopResult::Packet) {
            received.fetch_add(1);
        }
        idle.post();
    });

    const int kFrames = 50;
    std::atomic<int> next{0};
    SourcePump pump;
    MZ_ASSERT_TRUE(pump.start(
        [&](MediaPacket::Ptr *packet, std::string *) {
            const int index = next.fetch_add(1);
            if (index >= kFrames) {
                return SourcePump::ReadResult::EndOfStream;
            }
            *packet = video(0, index == 0, 16, index);
            return SourcePump::ReadResult::Packet;
        },
        source));

    // 源线程 → 队列 → 唤醒 → 轮询线程 drain：整条链路端到端
    while (received.load() < kFrames) {
        idle.wait();
    }
    // 先等源线程真的退出再断言 stop 的返回值 —— 否则就是在赌"哪条线程先跑完"
    // （eof 已置位但 _running 还没落地时调用 stop()，返回值会是 true）
    while (pump.running()) {
        std::this_thread::yield();
    }
    // 已经自然结束 → stop() 返回 false（状态没变），但会把线程回收
    MZ_ASSERT_FALSE(pump.stop());
    MZ_ASSERT_FALSE(pump.running());
    MZ_ASSERT_TRUE(pump.eof());
    MZ_ASSERT_STR_EQ(pump.lastError(), "");
    MZ_ASSERT_EQ(pump.readCount(), static_cast<uint64_t>(kFrames));
    MZ_ASSERT_EQ(pump.pushedCount(), static_cast<uint64_t>(kFrames));
    MZ_ASSERT_EQ(received.load(), kFrames);
    MZ_ASSERT_TRUE(sub->queue().endOfStream()); // 结束必须广播给消费者
    MZ_ASSERT_GT(sub->notifyCount(), 0u);       // 而且是被唤醒发现的，不是靠轮询

    poller->shutdown();
}

MZ_TEST(ntimed_media_pump_error_reported) {
    auto source = std::make_shared<MediaSource>();
    auto sub = source->subscribe();
    if (!sub) {
        return;
    }

    SourcePump pump;
    MZ_ASSERT_TRUE(pump.start(
        [](MediaPacket::Ptr *, std::string *error) {
            *error = "模拟：文件打不开";
            return SourcePump::ReadResult::Error;
        },
        source));
    while (pump.running()) {
        std::this_thread::yield();
    }
    MZ_ASSERT_FALSE(pump.eof()); // 失败不是 EOF（必须能分开）
    MZ_ASSERT_STR_EQ(pump.lastError(), "模拟：文件打不开");
    MZ_ASSERT_EQ(pump.readCount(), 0u);
    MZ_ASSERT_TRUE(sub->queue().endOfStream()); // 出错也要广播 EOS，消费者不能永远等
    MZ_ASSERT_FALSE(pump.stop());

    // 说好返回 Packet 却给空包：必须停下并报错，绝不允许"死循环 + 什么都不做"
    SourcePump pump2;
    MZ_ASSERT_TRUE(pump2.start(
        [](MediaPacket::Ptr *, std::string *) { return SourcePump::ReadResult::Packet; }, source));
    while (pump2.running()) {
        std::this_thread::yield();
    }
    MZ_ASSERT_EQ(pump2.readCount(), 0u);
    MZ_ASSERT_TRUE(pump2.lastError().find("包为空") != std::string::npos);
    MZ_ASSERT_FALSE(pump2.stop());
}

MZ_TEST(ntimed_media_pump_stop_is_clean) {
    auto source = std::make_shared<MediaSource>();
    auto sub = source->subscribe();
    if (!sub) {
        return;
    }

    SourcePump pump;
    std::atomic<int> produced{0};
    MZ_ASSERT_TRUE(pump.start(
        [&](MediaPacket::Ptr *packet, std::string *) {
            // 真实生产者的"可打断"写法：每次读之前先看停止请求
            // （真实实现把它接到 FFmpeg 的 interrupt_callback 上）
            if (pump.stopRequested()) {
                return SourcePump::ReadResult::EndOfStream;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const int index = produced.fetch_add(1);
            *packet = video(0, false, 8, index);
            return SourcePump::ReadResult::Packet;
        },
        source));

    while (pump.pushedCount() < 3) {
        std::this_thread::yield();
    }
    MZ_ASSERT_TRUE(pump.running());
    MZ_ASSERT_TRUE(pump.stop());  // 真的停掉一个正在运行的线程 → true
    MZ_ASSERT_FALSE(pump.running());
    MZ_ASSERT_FALSE(pump.stop()); // 幂等
    MZ_ASSERT_FALSE(pump.eof());  // 被停 ≠ 读完（调用方要能区分）
    MZ_ASSERT_GT(pump.pushedCount(), 0u);
    MZ_ASSERT_TRUE(sub->queue().endOfStream()); // 被停也要广播 EOS

    // EOS 之后队列里剩下的数据仍然可取完（不能因为"结束"就把数据扔掉）
    MediaPacket::Ptr packet;
    size_t rest = 0;
    while (sub->queue().pop(&packet) == FrameQueue::PopResult::Packet) {
        ++rest;
    }
    MZ_ASSERT_EQ(rest, static_cast<size_t>(pump.pushedCount()));
}
