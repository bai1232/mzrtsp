/*
 * 媒体分发层单元测试（M5-a 建立，M5-b 加线程，本批随 FR-5.1/5.2 修订更新策略断言）
 * ============================================================================
 * 契约来源：docs/DESIGN_M5.md §3 / §5；上位需求 FR-5.1（上限 = 码率 × 延迟额度，只按字节）、
 *           FR-5.2（溢出丢最旧的一段，音视频成对）、FR-5.3、NFR-6、FR-6.1
 *
 * 覆盖维度（AI_COLLAB §3.4：正常 / 空 / 满 / 断开 / 超大）：
 *   正常  一进一出、FIFO、计数一致、3 个订阅者序列完全一致（且指针相同 = 零拷贝）
 *   空    队列空、GOP 缓存空、没有 GOP 起点时的包不缓存、空包/0 字节/非法流索引
 *   满    字节上限触发；丢的只能是可丢包（**唯一不可丢的是视频关键帧**），且"最旧的一段"整体丢
 *   断开  订阅者 shared_ptr 释放 → 自动注销；EOS 广播与幂等；clear() 回收
 *   超大  单包超过队列上限（可丢 → 丢新包；不可丢 → 报错由调用方断开）；
 *         关键帧灌不进队列 → 拒绝接入；GOP 超上限 → 收敛但保留关键帧
 *
 * 本组**单线程**（无内部线程、无等待）→ 可进 TSAN 严格组。
 * 这里刻意不依赖 FFmpeg/样本文件：帧由用例自己造，所以策略能被确定性覆盖。
 * ============================================================================
 */

#include "test_main.h"

#include "core/util.h"
#include "media/frame_queue.h"
#include "media/gop_cache.h"
#include "media/media_packet.h"
#include "media/media_source.h"
#include "media/throttle.h"

#include <atomic>
#include <chrono>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace mzmedia;

namespace {

constexpr size_t kMB = 1024u * 1024u;

/// 造一个视频包（key=true 时是关键帧）
MediaPacket::Ptr video(int stream, bool key, size_t bytes, int64_t ms = 0) {
    auto payload = std::make_shared<const std::vector<uint8_t>>(bytes, key ? 0x11 : 0x22);
    return MediaPacket::create(MediaKind::Video, stream, payload, key, ms, ms);
}

/// 造一个音频包（音频没有关键帧概念 → 按 FR-5.2 修订版它**可丢**）
MediaPacket::Ptr audio(int stream, size_t bytes, int64_t ms = 0) {
    auto payload = std::make_shared<const std::vector<uint8_t>>(bytes, 0x33);
    return MediaPacket::create(MediaKind::Audio, stream, payload, false, ms, ms);
}

/// 把队列取空（不改 EOS 标记）
std::vector<MediaPacket::Ptr> drain(FrameQueue *queue) {
    std::vector<MediaPacket::Ptr> out;
    MediaPacket::Ptr packet;
    while (queue->pop(&packet) == FrameQueue::PopResult::Packet) {
        out.push_back(packet);
    }
    return out;
}

/// 容器里是否含**同一个对象**（不是"内容相等"，是零拷贝语义）
bool contains(const std::vector<MediaPacket::Ptr> &v, const MediaPacket::Ptr &p) {
    for (const auto &item : v) {
        if (item == p) {
            return true;
        }
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// MediaPacket
// ---------------------------------------------------------------------------

MZ_TEST(media_packet_metadata_and_validation) {
    auto key = video(0, true, 100);
    MZ_ASSERT_NOT_NULL(key.get());
    if (!key) {
        return;
    }
    MZ_ASSERT_EQ(key->kind(), MediaKind::Video);
    MZ_ASSERT_EQ(key->streamIndex(), 0);
    MZ_ASSERT_TRUE(key->isKeyFrame());
    MZ_ASSERT_EQ(key->size(), 100u);
    MZ_ASSERT_NOT_NULL(key->data());
    MZ_ASSERT_FALSE(key->droppable());   // 视频关键帧：唯一不可丢的

    auto non_key = video(0, false, 50, 40);
    MZ_ASSERT_NOT_NULL(non_key.get());
    MZ_ASSERT_TRUE(non_key->droppable()); // 视频非关键帧可丢
    MZ_ASSERT_EQ(non_key->dtsMs(), 40);

    auto audio_packet = audio(1, 30);
    MZ_ASSERT_NOT_NULL(audio_packet.get());
    // FR-5.2 修订：音频也**可丢**（丢的时候按"最旧的一段"整段丢，音视频成对，避免音画错位）
    MZ_ASSERT_TRUE(audio_packet->droppable());

    // 零拷贝分发的基础：多个 Ptr 指向**同一个对象**、同一块数据
    MediaPacket::Ptr shared = key;
    MZ_ASSERT_TRUE(shared.get() == key.get());
    MZ_ASSERT_TRUE(shared->data() == key->data());
    MZ_ASSERT_EQ(shared.use_count(), 2L);

    // 非法输入：必须返回 nullptr，绝不"造一个空包"顶替
    MZ_ASSERT_NULL(MediaPacket::create(MediaKind::Video, 0, nullptr, true, 0, 0).get());
    auto empty_payload = std::make_shared<const std::vector<uint8_t>>();
    MZ_ASSERT_NULL(MediaPacket::create(MediaKind::Video, 0, empty_payload, true, 0, 0).get());
    MZ_ASSERT_NULL(MediaPacket::create(MediaKind::Video, -1, key->payload(), true, 0, 0).get());

    // 负时间戳是合法的（B 帧/部分容器的首包 dts 就是负的）—— 不能拦
    MZ_ASSERT_NOT_NULL(video(0, false, 10, -1024).get());
}

// ---------------------------------------------------------------------------
// FrameQueue
// ---------------------------------------------------------------------------

MZ_TEST(media_queue_fifo_and_accounting) {
    FrameQueue queue;
    MZ_ASSERT_EQ(queue.packets(), 0u);
    MZ_ASSERT_TRUE(queue.empty());
    // FR-5.1 修订：上限**只按字节**，且默认值是推导出来的（8 Mbps × 2 s）
    MZ_ASSERT_EQ(queue.limits().max_bytes, 2000000u);

    auto p1 = video(0, true, 10, 1);
    auto p2 = video(0, false, 20, 2);
    auto p3 = video(0, false, 30, 3);
    MZ_ASSERT_EQ(queue.push(p1), FrameQueue::PushResult::Accepted);
    MZ_ASSERT_EQ(queue.push(p2), FrameQueue::PushResult::Accepted);
    MZ_ASSERT_EQ(queue.push(p3), FrameQueue::PushResult::Accepted);
    MZ_ASSERT_EQ(queue.packets(), 3u);
    MZ_ASSERT_EQ(queue.bytes(), 60u);

    MediaPacket::Ptr out;
    MZ_ASSERT_EQ(queue.pop(&out), FrameQueue::PopResult::Packet);
    MZ_ASSERT_TRUE(out == p1); // FIFO：先入先出
    MZ_ASSERT_EQ(queue.bytes(), 50u);
    MZ_ASSERT_EQ(queue.pop(&out), FrameQueue::PopResult::Packet);
    MZ_ASSERT_TRUE(out == p2);
    MZ_ASSERT_EQ(queue.pop(&out), FrameQueue::PopResult::Packet);
    MZ_ASSERT_TRUE(out == p3);
    MZ_ASSERT_EQ(queue.pop(&out), FrameQueue::PopResult::Empty);
    MZ_ASSERT_EQ(queue.stats().pushed, 3u);
    MZ_ASSERT_EQ(queue.stats().popped, 3u);
}

MZ_TEST(media_queue_drops_only_droppable) {
    // FR-5.2 修订：唯一不可丢的是**视频关键帧**；丢弃单位是"队头最旧的一段"（音视频成对）
    FrameQueue::Limits limits;
    limits.max_bytes = 60; // 只放得下 6 个 10 字节的包
    FrameQueue queue;
    MZ_ASSERT_TRUE(queue.setLimits(limits));

    auto key = video(0, true, 10);
    auto a1 = audio(1, 10);
    auto v1 = video(0, false, 10);
    auto a2 = audio(1, 10);
    auto v2 = video(0, false, 10);
    auto a3 = audio(1, 10);
    for (const auto &packet : {key, a1, v1, a2, v2, a3}) {
        MZ_ASSERT_EQ(queue.push(packet), FrameQueue::PushResult::Accepted);
    }
    MZ_ASSERT_EQ(queue.bytes(), 60u);
    MZ_ASSERT_EQ(queue.packets(), 6u);

    // 来了一个 20 字节的包：要腾 20 字节 → 丢掉队头最旧的一段（a1 + v1，音频与视频各一）
    auto big = video(0, false, 20);
    MZ_ASSERT_EQ(queue.push(big), FrameQueue::PushResult::DroppedToMakeRoom);
    MZ_ASSERT_EQ(queue.bytes(), 60u);
    MZ_ASSERT_EQ(queue.stats().dropped, 2u); // 一次丢两个：音频 + 视频（成对，不是"只丢视频"）

    auto kept = drain(&queue);
    MZ_ASSERT_TRUE(contains(kept, key)); // 关键帧绝不丢
    MZ_ASSERT_FALSE(contains(kept, a1)); // 最旧的一段被整体丢掉
    MZ_ASSERT_FALSE(contains(kept, v1));
    MZ_ASSERT_TRUE(contains(kept, a2));
    MZ_ASSERT_TRUE(contains(kept, v2));
    MZ_ASSERT_TRUE(contains(kept, a3));
    MZ_ASSERT_TRUE(contains(kept, big));

    // 继续灌视频非关键帧：关键帧必须一直在（丢了它后面全花屏），字节数不超上限
    FrameQueue queue2;
    MZ_ASSERT_TRUE(queue2.setLimits(limits));
    MZ_ASSERT_EQ(queue2.push(key), FrameQueue::PushResult::Accepted);
    for (int i = 0; i < 20; ++i) {
        (void) queue2.push(video(0, false, 10, i));
    }
    auto kept2 = drain(&queue2);
    MZ_ASSERT_TRUE(contains(kept2, key));
    MZ_ASSERT_LE(queue2.bytes(), limits.max_bytes);
}

MZ_TEST(media_queue_byte_limit_only) {
    // FR-5.1 修订：只有一个上限 —— 字节（原来的"64 帧"已删除）
    FrameQueue::Limits limits;
    limits.max_bytes = 100;
    FrameQueue queue;
    MZ_ASSERT_TRUE(queue.setLimits(limits));

    MZ_ASSERT_EQ(queue.push(video(0, false, 40)), FrameQueue::PushResult::Accepted);
    MZ_ASSERT_EQ(queue.push(video(0, false, 40)), FrameQueue::PushResult::Accepted);
    MZ_ASSERT_EQ(queue.bytes(), 80u);
    // 80 + 40 > 100 → 丢掉最旧的可丢包
    MZ_ASSERT_EQ(queue.push(video(0, false, 40)), FrameQueue::PushResult::DroppedToMakeRoom);
    MZ_ASSERT_EQ(queue.bytes(), 80u);
    MZ_ASSERT_LE(queue.bytes(), queue.limits().max_bytes); // 上限是硬的

    // 单包就超过整个字节上限：可丢的丢新包（正常降级），不可丢的必须报错（调用方断开）
    MZ_ASSERT_EQ(queue.push(video(0, true, 200)), FrameQueue::PushResult::RejectedNoSpace);
    MZ_ASSERT_EQ(queue.push(video(0, false, 200)), FrameQueue::PushResult::DroppedIncoming);
    MZ_ASSERT_EQ(queue.bytes(), 80u); // 队列没被这两个包动过
    MZ_ASSERT_EQ(queue.stats().rejected_no_space, 1u);
    MZ_ASSERT_EQ(queue.stats().dropped_incoming, 1u);
}

MZ_TEST(media_queue_keyframe_makes_room_or_rejects) {
    FrameQueue::Limits limits;
    limits.max_bytes = 20;
    FrameQueue queue;
    MZ_ASSERT_TRUE(queue.setLimits(limits));

    auto n1 = video(0, false, 10);
    auto aud = audio(1, 10);
    MZ_ASSERT_EQ(queue.push(n1), FrameQueue::PushResult::Accepted);
    MZ_ASSERT_EQ(queue.push(aud), FrameQueue::PushResult::Accepted);

    // 关键帧到来：丢掉最旧的一段可丢包（音频也算可丢）让它进去
    auto key = video(0, true, 10);
    MZ_ASSERT_EQ(queue.push(key), FrameQueue::PushResult::DroppedToMakeRoom);
    auto kept = drain(&queue);
    MZ_ASSERT_EQ(kept.size(), 2u);
    MZ_ASSERT_TRUE(contains(kept, aud));
    MZ_ASSERT_TRUE(contains(kept, key));

    // 队里只剩**不可丢**的包（全是视频关键帧）→ 再来关键帧也挤不进去：必须报错，不能静默丢
    FrameQueue queue2;
    MZ_ASSERT_TRUE(queue2.setLimits(limits));
    MZ_ASSERT_EQ(queue2.push(video(0, true, 10)), FrameQueue::PushResult::Accepted);
    MZ_ASSERT_EQ(queue2.push(video(0, true, 10)), FrameQueue::PushResult::Accepted);
    MZ_ASSERT_EQ(queue2.push(video(0, true, 10)), FrameQueue::PushResult::RejectedNoSpace);
    MZ_ASSERT_EQ(queue2.packets(), 2u);
    MZ_ASSERT_EQ(queue2.stats().rejected_no_space, 1u);
}

MZ_TEST(media_queue_rejects_invalid_limits_and_eos) {
    FrameQueue queue;

    // 0 不等于无界：必须拒绝，且保持原值
    FrameQueue::Limits zero;
    zero.max_bytes = 0;
    MZ_ASSERT_FALSE(queue.setLimits(zero));
    FrameQueue::Limits too_huge;
    too_huge.max_bytes = 1024u * kMB; // 超过硬上限
    MZ_ASSERT_FALSE(queue.setLimits(too_huge));
    MZ_ASSERT_EQ(queue.limits().max_bytes, 2000000u); // 默认推导值仍然有效

    FrameQueue::Limits ok;
    ok.max_bytes = 100;
    MZ_ASSERT_TRUE(queue.setLimits(ok));
    MZ_ASSERT_EQ(queue.limits().max_bytes, 100u);

    // EOS：区分"暂时没有"与"不会再有"；标记幂等
    MediaPacket::Ptr out;
    MZ_ASSERT_EQ(queue.pop(&out), FrameQueue::PopResult::Empty);
    MZ_ASSERT_TRUE(queue.markEndOfStream());
    MZ_ASSERT_FALSE(queue.markEndOfStream());
    MZ_ASSERT_TRUE(queue.endOfStream());
    MZ_ASSERT_EQ(queue.pop(&out), FrameQueue::PopResult::EndOfStream);

    // clear()：返回丢弃数，且不动 EOS 标记
    MZ_ASSERT_EQ(queue.push(video(0, false, 1)), FrameQueue::PushResult::Accepted);
    MZ_ASSERT_EQ(queue.push(video(0, false, 1)), FrameQueue::PushResult::Accepted);
    MZ_ASSERT_EQ(queue.packets(), 2u);
    MZ_ASSERT_EQ(queue.clear(), 2u);
    MZ_ASSERT_EQ(queue.packets(), 0u);
    MZ_ASSERT_EQ(queue.bytes(), 0u);
    MZ_ASSERT_EQ(queue.stats().dropped_on_clear, 2u);
    MZ_ASSERT_TRUE(queue.endOfStream());
}

// ---------------------------------------------------------------------------
// GopCache
// ---------------------------------------------------------------------------

MZ_TEST(media_gop_cache_keeps_only_last_gop) {
    GopCache cache;
    MZ_ASSERT_EQ(cache.maxBytes(), 2000000u); // 推导值：8 Mbps × 2 s
    MZ_ASSERT_TRUE(cache.snapshot().empty());

    // 还没有关键帧：别的包不缓存（否则 snapshot() 的开头解不出来）
    MZ_ASSERT_EQ(cache.feed(video(0, false, 10)), GopCache::FeedResult::Skipped);
    MZ_ASSERT_EQ(cache.stats().skipped, 1u);
    MZ_ASSERT_TRUE(cache.snapshot().empty());

    auto k1 = video(0, true, 10);
    auto n1 = video(0, false, 10);
    auto n2 = video(0, false, 10);
    MZ_ASSERT_EQ(cache.feed(k1), GopCache::FeedResult::StartedNewGop);
    MZ_ASSERT_EQ(cache.feed(n1), GopCache::FeedResult::Cached);
    MZ_ASSERT_EQ(cache.feed(n2), GopCache::FeedResult::Cached);
    MZ_ASSERT_EQ(cache.packets(), 3u);

    // 第二个关键帧：旧 GOP 全部作废
    auto k2 = video(0, true, 10);
    MZ_ASSERT_EQ(cache.feed(k2), GopCache::FeedResult::StartedNewGop);
    MZ_ASSERT_EQ(cache.packets(), 1u);
    MZ_ASSERT_EQ(cache.stats().started_gop, 2u);
    auto snapshot = cache.snapshot();
    MZ_ASSERT_EQ(snapshot.size(), 1u);
    if (!snapshot.empty()) {
        MZ_ASSERT_TRUE(snapshot.front() == k2); // 第一项必是关键帧
    }
    MZ_ASSERT_FALSE(contains(snapshot, k1));
    MZ_ASSERT_FALSE(contains(snapshot, n1));

    // 空包：跳过并计数
    MZ_ASSERT_EQ(cache.feed(nullptr), GopCache::FeedResult::Skipped);
    MZ_ASSERT_EQ(cache.packets(), 1u);
}

MZ_TEST(media_gop_cache_byte_limit_keeps_keyframe) {
    GopCache cache(100);
    MZ_ASSERT_FALSE(cache.setMaxBytes(0)); // 0 被拒
    MZ_ASSERT_EQ(cache.maxBytes(), 100u);  // 保持原值

    auto key = video(0, true, 30);
    MZ_ASSERT_EQ(cache.feed(key), GopCache::FeedResult::StartedNewGop);
    MZ_ASSERT_EQ(cache.bytes(), 30u);

    auto n1 = video(0, false, 40);
    auto n2 = video(0, false, 40);
    MZ_ASSERT_EQ(cache.feed(n1), GopCache::FeedResult::Cached);
    MZ_ASSERT_EQ(cache.bytes(), 70u);
    // 70 + 40 > 100 → 丢掉最旧的可丢包（n1）
    MZ_ASSERT_EQ(cache.feed(n2), GopCache::FeedResult::Cached);
    MZ_ASSERT_EQ(cache.bytes(), 70u);
    MZ_ASSERT_EQ(cache.stats().dropped, 1u);
    MZ_ASSERT_LE(cache.bytes(), cache.maxBytes());

    auto snapshot = cache.snapshot();
    MZ_ASSERT_TRUE(!snapshot.empty());
    if (!snapshot.empty()) {
        MZ_ASSERT_TRUE(snapshot.front() == key); // 关键帧必须保留
    }
    MZ_ASSERT_TRUE(contains(snapshot, n2));
    MZ_ASSERT_FALSE(contains(snapshot, n1));

    // 缓存里音频可丢（与 FrameQueue **同一规则**：只有视频关键帧不可丢）
    MZ_ASSERT_EQ(cache.feed(audio(1, 50)), GopCache::FeedResult::Cached);
    MZ_ASSERT_LE(cache.bytes(), cache.maxBytes());
    auto snapshot2 = cache.snapshot();
    MZ_ASSERT_TRUE(!snapshot2.empty());
    if (!snapshot2.empty()) {
        MZ_ASSERT_TRUE(snapshot2.front() == key);
    }
}

// ---------------------------------------------------------------------------
// MediaSource
// ---------------------------------------------------------------------------

MZ_TEST(media_source_fanout_identical) {
    // ROADMAP 的 M5 验收：3 个订阅者收到**完全一致**的帧序列
    MediaSource source;
    auto s1 = source.subscribe();
    auto s2 = source.subscribe();
    auto s3 = source.subscribe();
    MZ_ASSERT_NOT_NULL(s1.get());
    MZ_ASSERT_NOT_NULL(s2.get());
    MZ_ASSERT_NOT_NULL(s3.get());
    if (!s1 || !s2 || !s3) {
        return;
    }
    MZ_ASSERT_NE(s1->id(), s2->id());
    MZ_ASSERT_EQ(source.subscriberCount(), 3u);
    // 上限是推导出来的：8 Mbps × 2 s
    MZ_ASSERT_EQ(source.limits().queueMaxBytes(), 2000000u);
    MZ_ASSERT_EQ(source.limits().gopMaxBytes(), 2000000u);

    std::vector<MediaPacket::Ptr> sent;
    sent.push_back(video(0, true, 100, 0));
    for (int i = 1; i <= 19; ++i) {
        sent.push_back(video(0, false, 50, i * 40));
    }
    for (const auto &packet : sent) {
        const auto stats = source.pushPacket(packet);
        MZ_ASSERT_EQ(stats.delivered, 3u);
        MZ_ASSERT_EQ(stats.rejected_no_space, 0u);
    }
    MZ_ASSERT_EQ(source.totalDelivered(), 60u);

    const auto a = drain(&s1->queue());
    const auto b = drain(&s2->queue());
    const auto c = drain(&s3->queue());
    MZ_ASSERT_EQ(a.size(), 20u);
    MZ_ASSERT_EQ(b.size(), 20u);
    MZ_ASSERT_EQ(c.size(), 20u);
    if (a.size() != 20u || b.size() != 20u || c.size() != 20u) {
        return; // 断言不中断执行，先早退避免越界
    }
    for (size_t i = 0; i < 20; ++i) {
        // 零拷贝：三个订阅者拿到的是**同一个对象**（指针相同），不是各拷贝一份
        MZ_ASSERT_TRUE(a[i].get() == sent[i].get());
        MZ_ASSERT_TRUE(b[i].get() == a[i].get());
        MZ_ASSERT_TRUE(c[i].get() == a[i].get());
        MZ_ASSERT_TRUE(a[i]->data() == sent[i]->data());
        MZ_ASSERT_EQ(a[i]->dtsMs(), sent[i]->dtsMs());
    }
}

MZ_TEST(media_source_slow_consumer_isolated) {
    // FR-5.3：任一客户端异常（不读数据）不得影响源与其他客户端
    MediaSource source;
    MediaSource::Limits limits;
    // 故意给得极小：64 kbps × 10 ms = 80 字节（放得下 8 个 10 字节的包）
    limits.max_bitrate_bps = 64000;
    limits.latency_budget_ms = 10;
    MZ_ASSERT_TRUE(source.setLimits(limits));
    MZ_ASSERT_EQ(source.limits().queueMaxBytes(), 80u);

    auto fast1 = source.subscribe();
    auto fast2 = source.subscribe();
    auto slow = source.subscribe(); // 这个订阅者一直不取帧
    MZ_ASSERT_EQ(source.subscriberCount(), 3u);

    const int kCount = 30;
    std::vector<MediaPacket::Ptr> f1;
    std::vector<MediaPacket::Ptr> f2;
    for (int i = 0; i < kCount; ++i) {
        (void) source.pushPacket(video(0, false, 10, i));
        // 快的订阅者"及时消费"（真实场景里就是它们的 poller 线程在往 socket 写）
        MediaPacket::Ptr packet;
        while (fast1->queue().pop(&packet) == FrameQueue::PopResult::Packet) {
            f1.push_back(packet);
        }
        while (fast2->queue().pop(&packet) == FrameQueue::PopResult::Packet) {
            f2.push_back(packet);
        }
    }

    MZ_ASSERT_EQ(f1.size(), static_cast<size_t>(kCount)); // 快的订阅者一个都不少
    MZ_ASSERT_EQ(f2.size(), static_cast<size_t>(kCount));
    MZ_ASSERT_EQ(fast1->queue().stats().dropped, 0u);
    MZ_ASSERT_EQ(fast2->queue().stats().dropped, 0u);

    MZ_ASSERT_EQ(slow->queue().packets(), 8u);         // 上限生效（80 字节 / 10）
    MZ_ASSERT_EQ(slow->queue().stats().dropped, 22u);   // 30 - 8
    MZ_ASSERT_GT(source.totalDropped(), 0u);
    MZ_ASSERT_FALSE(slow->broken()); // 丢的是可丢包 → 不算"坏了"

    // 慢订阅者留下的是**最新的** 8 个（丢的是旧的）
    const auto rest = drain(&slow->queue());
    MZ_ASSERT_EQ(rest.size(), 8u);
    if (rest.size() == 8u) {
        MZ_ASSERT_EQ(rest.front()->dtsMs(), static_cast<int64_t>(kCount - 8));
        MZ_ASSERT_EQ(rest.back()->dtsMs(), static_cast<int64_t>(kCount - 1));
    }
}

MZ_TEST(media_source_new_subscriber_starts_at_keyframe) {
    MediaSource source;
    // GOP1: k1 n1 n2 ／ GOP2: k2 n3
    (void) source.pushPacket(video(0, true, 10, 0));
    (void) source.pushPacket(video(0, false, 10, 1));
    (void) source.pushPacket(video(0, false, 10, 2));
    auto k2 = video(0, true, 10, 3);
    auto n3 = video(0, false, 10, 4);
    (void) source.pushPacket(k2);
    (void) source.pushPacket(n3);

    // 中途接入：必须从当前 GOP 的关键帧开始（否则首帧解不出来，一直花屏到下一个关键帧）
    auto sub = source.subscribe();
    MZ_ASSERT_NOT_NULL(sub.get());
    if (!sub) {
        return;
    }
    const auto seeded = drain(&sub->queue());
    MZ_ASSERT_EQ(seeded.size(), 2u);
    if (seeded.size() != 2u) {
        return;
    }
    MZ_ASSERT_TRUE(seeded.front() == k2);
    MZ_ASSERT_TRUE(seeded.front()->isKeyFrame());
    MZ_ASSERT_EQ(seeded.front()->kind(), MediaKind::Video);
    MZ_ASSERT_TRUE(seeded.back() == n3);

    // 关键帧比整个队列上限还大 → 拒绝接入（宁可不接，也不让对端从 GOP 中间开始）
    MediaSource tiny;
    MediaSource::Limits limits;
    limits.max_bitrate_bps = 400000; // 400 kbps × 1 ms = 50 字节 < 关键帧的 100 字节
    limits.latency_budget_ms = 1;
    MZ_ASSERT_TRUE(tiny.setLimits(limits));
    MZ_ASSERT_EQ(tiny.limits().queueMaxBytes(), 50u);
    (void) tiny.pushPacket(video(0, true, 100, 0));
    auto bad = tiny.subscribe();
    MZ_ASSERT_NULL(bad.get());
    MZ_ASSERT_EQ(tiny.totalSeedFailed(), 1u);
    MZ_ASSERT_EQ(tiny.subscriberCount(), 0u);
}

MZ_TEST(media_source_subscriber_lifetime_and_limits) {
    MediaSource source;
    MediaSource::Limits limits;
    limits.max_subscribers = 2;
    MZ_ASSERT_TRUE(source.setLimits(limits));

    // max_subscribers = 0 被拒，且原值不变（人数是硬边界）
    MediaSource::Limits zero_subs;
    zero_subs.max_subscribers = 0;
    MZ_ASSERT_FALSE(source.setLimits(zero_subs));
    MZ_ASSERT_EQ(source.limits().max_subscribers, 2u);
    // 码率 / 时长也必须是正数（0 一律拒绝）
    MediaSource::Limits zero_bitrate;
    zero_bitrate.max_bitrate_bps = 0;
    MZ_ASSERT_FALSE(source.setLimits(zero_bitrate));
    MediaSource::Limits zero_budget;
    zero_budget.latency_budget_ms = 0;
    MZ_ASSERT_FALSE(source.setLimits(zero_budget));
    MZ_ASSERT_EQ(source.limits().max_bitrate_bps, 8000000u);

    auto s1 = source.subscribe();
    auto s2 = source.subscribe();
    MZ_ASSERT_NOT_NULL(s1.get());
    MZ_ASSERT_NOT_NULL(s2.get());
    if (!s1 || !s2) {
        return;
    }
    MZ_ASSERT_NULL(source.subscribe().get()); // 超上限 → 明确拒绝
    MZ_ASSERT_EQ(source.subscriberCount(), 2u);
    MZ_ASSERT_GT(source.totalRejected(), 0u);

    // NFR-6：消费者释放 shared_ptr 后由 weak_ptr 自动注销
    const auto id1 = s1->id();
    s1.reset();
    MZ_ASSERT_EQ(source.subscriberCount(), 1u);
    MZ_ASSERT_EQ(source.totalAutoUnsubscribe(), 1u);

    (void) source.pushPacket(video(0, false, 10, 0));
    MZ_ASSERT_EQ(s2->queue().packets(), 1u); // 只剩 s2 收到

    // EOS 广播：幂等，且消费者能区分"空"与"结束"
    MZ_ASSERT_TRUE(source.endOfStream());
    MZ_ASSERT_FALSE(source.endOfStream());
    MZ_ASSERT_TRUE(s2->queue().endOfStream());

    // EOS 之后接入的订阅者必须立刻知道（否则消费端会永远等一个不会来的包）
    auto s3 = source.subscribe();
    MZ_ASSERT_NOT_NULL(s3.get());
    if (!s3) {
        return;
    }
    MediaPacket::Ptr out;
    MZ_ASSERT_EQ(s3->queue().pop(&out), FrameQueue::PopResult::EndOfStream);

    // 主动退订：返回"到底存不存在"
    MZ_ASSERT_TRUE(source.unsubscribe(s3->id()));
    MZ_ASSERT_FALSE(source.unsubscribe(s3->id()));
    MZ_ASSERT_FALSE(source.unsubscribe(id1)); // 早就自动注销了
}

MZ_TEST(media_source_dump_stats_minimal) {
    // FR-6.1 的最小版（本批定）：几个 atomic 计数器 + 一行 dumpStats()，
    // 可从**任意线程**调用（HTTP 线程将来直接把它塞进 /api/stats）
    MediaSource source;
    auto sub = source.subscribe();
    MZ_ASSERT_NOT_NULL(sub.get());
    if (!sub) {
        return;
    }
    (void) source.pushPacket(video(0, true, 10, 0));
    (void) source.pushPacket(video(0, false, 10, 1));

    const std::string stats = source.dumpStats();
    MZ_ASSERT_TRUE(stats.find("media_source{") == 0);                     // 一眼能认出是谁
    MZ_ASSERT_TRUE(stats.find("delivered=2") != std::string::npos);       // 计数
    MZ_ASSERT_TRUE(stats.find("queue_bytes=2000000") != std::string::npos); // 推导出的上限
    MZ_ASSERT_TRUE(stats.find("gop_bytes=2000000") != std::string::npos);
    MZ_ASSERT_TRUE(stats.find("out_budget=128000000bps") != std::string::npos); // 8Mbps × 16 路
    MZ_ASSERT_TRUE(stats.find("rejected=0") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Subscriber 的绑定语义（M5-b）
// ---------------------------------------------------------------------------

MZ_TEST(media_subscriber_bind_poller_and_callbacks) {
    MediaSource source;
    auto sub = source.subscribe();
    MZ_ASSERT_NOT_NULL(sub.get());
    if (!sub) {
        return;
    }

    // 未绑定 poller = 同步模式：不产生任何唤醒
    MZ_ASSERT_FALSE(sub->notifyIfNeeded());
    MZ_ASSERT_EQ(sub->notifyCount(), 0u);
    MZ_ASSERT_NULL(sub->poller().get());
    MZ_ASSERT_FALSE(sub->clearNotifyPending()); // 本来就没有未决唤醒

    // 注册回调返回**上一个**（沿用全项目"注册返回上一个"的约定）
    auto first = sub->setDrainCallback([](const Subscriber::Ptr &) {});
    MZ_ASSERT_TRUE(!first);
    auto second = sub->setDrainCallback([](const Subscriber::Ptr &) {});
    MZ_ASSERT_TRUE(static_cast<bool>(second));
    auto third = sub->setDrainCallback(nullptr);
    MZ_ASSERT_TRUE(static_cast<bool>(third)); // 返回的仍是刚才那个（返回"上一个"）

    // 解绑：bindPoller(nullptr) 返回 false（表示"现在是未绑定状态"）
    MZ_ASSERT_FALSE(sub->bindPoller(nullptr));
    MZ_ASSERT_NULL(sub->poller().get());
}

// ---------------------------------------------------------------------------
// Throttle：按墙钟节流（M5-d，FR-3.5）
// ---------------------------------------------------------------------------

MZ_TEST(media_throttle_paces_by_wall_clock) {
    Throttle throttle; // 默认 enabled=true / speed=1.0
    throttle.reset();
    std::atomic<bool> abort{false};

    const int64_t begin = static_cast<int64_t>(getCurrentMillisecond());
    MZ_ASSERT_TRUE(throttle.pace(0, abort));  // 首个包：只对表，不等待
    MZ_ASSERT_TRUE(throttle.pace(40, abort)); // 40ms 处的包 → 应等到 ~40ms
    const int64_t elapsed = static_cast<int64_t>(getCurrentMillisecond()) - begin;

    MZ_ASSERT_GE(elapsed, 30);  // 真的等过墙钟（内核不会早醒，下界因此是稳的）
    MZ_ASSERT_LT(elapsed, 400); // 也不能夸张地等
    MZ_ASSERT_EQ(throttle.paceCount(), 2u);
    MZ_ASSERT_EQ(throttle.abortedCount(), 0u);
    MZ_ASSERT_GT(throttle.waitedMs(), 0);

    // 快放：speed=100 → 500ms 处的包只需 ~5ms
    Throttle::Config fast;
    fast.speed = 100.0;
    MZ_ASSERT_TRUE(throttle.setConfig(fast));
    throttle.reset();
    const int64_t begin_fast = static_cast<int64_t>(getCurrentMillisecond());
    MZ_ASSERT_TRUE(throttle.pace(0, abort));
    MZ_ASSERT_TRUE(throttle.pace(500, abort));
    MZ_ASSERT_LT(static_cast<int64_t>(getCurrentMillisecond()) - begin_fast, 200);
}

MZ_TEST(media_throttle_aborts_immediately) {
    std::atomic<bool> abort{true}; // 一开始就是"要停了"
    Throttle throttle;
    throttle.reset();

    const int64_t begin = static_cast<int64_t>(getCurrentMillisecond());
    MZ_ASSERT_FALSE(throttle.pace(0, abort));      // 立刻拒绝，不进入等待
    MZ_ASSERT_FALSE(throttle.pace(60000, abort));  // 一分钟后的包也不等
    MZ_ASSERT_LT(static_cast<int64_t>(getCurrentMillisecond()) - begin, 100);
    MZ_ASSERT_EQ(throttle.abortedCount(), 2u);

    // 中途被叫停：本该等 5 秒，必须在"一片"之内醒来（否则 stop() 的 join 要等满一拍）
    std::atomic<bool> stop{false};
    Throttle long_wait;
    long_wait.reset();
    MZ_ASSERT_TRUE(long_wait.pace(0, stop));
    std::thread killer([&stop] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        stop.store(true);
    });
    const int64_t begin_wait = static_cast<int64_t>(getCurrentMillisecond());
    const bool ok = long_wait.pace(5000, stop);
    const int64_t waited = static_cast<int64_t>(getCurrentMillisecond()) - begin_wait;
    killer.join();

    MZ_ASSERT_FALSE(ok);
    MZ_ASSERT_LT(waited, 500);
    MZ_ASSERT_EQ(long_wait.abortedCount(), 1u);
}

MZ_TEST(media_throttle_rejects_invalid_speed) {
    Throttle throttle;
    Throttle::Config bad;

    bad.speed = 0.0;
    MZ_ASSERT_FALSE(throttle.setConfig(bad));
    bad.speed = -1.0;
    MZ_ASSERT_FALSE(throttle.setConfig(bad));
    bad.speed = std::numeric_limits<double>::quiet_NaN();
    MZ_ASSERT_FALSE(throttle.setConfig(bad));
    bad.speed = std::numeric_limits<double>::infinity();
    MZ_ASSERT_FALSE(throttle.setConfig(bad));
    bad.speed = 100000.0; // 超过硬上限（1000 倍速）
    MZ_ASSERT_FALSE(throttle.setConfig(bad));

    // 被拒之后必须保持原值（不能变成"半套配置"）
    MZ_ASSERT_NEAR(throttle.config().speed, 1.0, 1e-9);
    MZ_ASSERT_TRUE(throttle.config().enabled);
}

MZ_TEST(media_throttle_disabled_passes_through) {
    Throttle throttle;
    Throttle::Config off;
    off.enabled = false;
    MZ_ASSERT_TRUE(throttle.setConfig(off));
    throttle.reset();

    std::atomic<bool> abort{false};
    const int64_t begin = static_cast<int64_t>(getCurrentMillisecond());
    for (int i = 0; i < 5; ++i) {
        MZ_ASSERT_TRUE(throttle.pace(i * 1000, abort)); // 时间戳差 4 秒也不等
    }
    MZ_ASSERT_LT(static_cast<int64_t>(getCurrentMillisecond()) - begin, 100);
    MZ_ASSERT_EQ(throttle.waitedMs(), 0);
    MZ_ASSERT_EQ(throttle.paceCount(), 5u);
}
