/*
 * 媒体分发层单元测试（M5-a，分组 `media`）
 * ============================================================================
 * 契约来源：docs/DESIGN_M5.md §3 / §5；上位需求 FR-5.1 / FR-5.2 / FR-5.3 / NFR-6
 *
 * 覆盖维度（AI_COLLAB §3.4：正常 / 空 / 满 / 断开 / 超大）：
 *   正常  一进一出、FIFO、计数一致、3 个订阅者序列完全一致（且指针相同 = 零拷贝）
 *   空    队列空、GOP 缓存空、没有 GOP 起点时的包不缓存、空包/0 字节/非法流索引
 *   满    帧数上限与字节上限各自触发；丢的只能是可丢包（音频与关键帧绝不丢）
 *   断开  订阅者 shared_ptr 释放 → 自动注销；EOS 广播与幂等；clear() 回收
 *   超大  单包超过队列上限（可丢 → 丢新包；不可丢 → 报错由调用方断开）；
 *         关键帧灌不进队列 → 拒绝接入；GOP 超上限 → 收敛但保留关键帧
 *
 * 本组**单线程**（无内部线程、无等待）→ 可进 TSAN 严格组。
 * 这里刻意不依赖 FFmpeg/样本文件：帧由用例自己造，所以策略能被确定性覆盖。
 * ============================================================================
 */

#include "test_main.h"

#include "media/frame_queue.h"
#include "media/gop_cache.h"
#include "media/media_packet.h"
#include "media/media_source.h"

#include <vector>

using namespace mzmedia;

namespace {

constexpr size_t kMB = 1024u * 1024u;

/// 造一个视频包（key=true 时是关键帧）
MediaPacket::Ptr video(int stream, bool key, size_t bytes, int64_t ms = 0) {
    auto payload = std::make_shared<const std::vector<uint8_t>>(bytes, key ? 0x11 : 0x22);
    return MediaPacket::create(MediaKind::Video, stream, payload, key, ms, ms);
}

/// 造一个音频包（音频没有关键帧概念，一律不可丢）
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
    MZ_ASSERT_FALSE(key->droppable());   // 视频关键帧不可丢

    auto non_key = video(0, false, 50, 40);
    MZ_ASSERT_NOT_NULL(non_key.get());
    MZ_ASSERT_TRUE(non_key->droppable()); // 视频非关键帧可丢
    MZ_ASSERT_EQ(non_key->dtsMs(), 40);

    auto audio_packet = audio(1, 30);
    MZ_ASSERT_NOT_NULL(audio_packet.get());
    MZ_ASSERT_FALSE(audio_packet->droppable()); // 音频一律不可丢

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
    MZ_ASSERT_EQ(queue.limits().max_packets, 64u);       // FR-5.1 的初值
    MZ_ASSERT_EQ(queue.limits().max_bytes, 8u * kMB);

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
    MZ_ASSERT_TRUE(out == p1);                           // FIFO：先入先出
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
    FrameQueue::Limits limits;
    limits.max_packets = 4;
    limits.max_bytes = 1u * kMB;

    FrameQueue queue;
    MZ_ASSERT_TRUE(queue.setLimits(limits));

    auto key = video(0, true, 10);
    auto aud = audio(1, 10);
    auto n1 = video(0, false, 10);
    auto n2 = video(0, false, 10);
    MZ_ASSERT_EQ(queue.push(key), FrameQueue::PushResult::Accepted);
    MZ_ASSERT_EQ(queue.push(aud), FrameQueue::PushResult::Accepted);
    MZ_ASSERT_EQ(queue.push(n1), FrameQueue::PushResult::Accepted);
    MZ_ASSERT_EQ(queue.push(n2), FrameQueue::PushResult::Accepted);
    MZ_ASSERT_EQ(queue.packets(), 4u);

    // 满了：再来一个视频非关键帧 → 丢最旧的可丢包（n1）给它让位
    auto n3 = video(0, false, 10);
    MZ_ASSERT_EQ(queue.push(n3), FrameQueue::PushResult::DroppedToMakeRoom);
    MZ_ASSERT_EQ(queue.packets(), 4u);
    MZ_ASSERT_EQ(queue.stats().dropped_non_key, 1u);

    auto kept = drain(&queue);
    MZ_ASSERT_EQ(kept.size(), 4u);
    MZ_ASSERT_TRUE(contains(kept, key));    // 关键帧绝不丢
    MZ_ASSERT_TRUE(contains(kept, aud));    // 音频绝不丢
    MZ_ASSERT_FALSE(contains(kept, n1));    // 丢的是最旧的可丢包
    MZ_ASSERT_TRUE(contains(kept, n2));
    MZ_ASSERT_TRUE(contains(kept, n3));

    // 继续灌非关键帧：关键帧与音频必须一直在（丢掉它们 = 花屏 + 静音）
    FrameQueue queue2;
    MZ_ASSERT_TRUE(queue2.setLimits(limits));
    MZ_ASSERT_EQ(queue2.push(key), FrameQueue::PushResult::Accepted);
    MZ_ASSERT_EQ(queue2.push(aud), FrameQueue::PushResult::Accepted);
    int accepted = 0;
    int made_room = 0;
    for (int i = 0; i < 20; ++i) {
        const auto result = queue2.push(video(0, false, 10, i));
        if (result == FrameQueue::PushResult::Accepted) {
            ++accepted;
        } else if (result == FrameQueue::PushResult::DroppedToMakeRoom) {
            ++made_room;
        }
    }
    // 队列里已有 2 个（关键帧 + 音频），上限 4：头 2 个直接进，其余 18 个都得先丢一个旧包
    MZ_ASSERT_EQ(accepted, 2);
    MZ_ASSERT_EQ(made_room, 18);
    MZ_ASSERT_EQ(queue2.packets(), 4u);
    auto kept2 = drain(&queue2);
    MZ_ASSERT_TRUE(contains(kept2, key));
    MZ_ASSERT_TRUE(contains(kept2, aud));
    MZ_ASSERT_LE(kept2.size(), 4u);
}

MZ_TEST(media_queue_limits_first_wins) {
    // 字节上限先到（帧数上限给得很大）
    FrameQueue::Limits limits;
    limits.max_packets = 1000;
    limits.max_bytes = 100;

    FrameQueue queue;
    MZ_ASSERT_TRUE(queue.setLimits(limits));
    MZ_ASSERT_EQ(queue.push(video(0, false, 40)), FrameQueue::PushResult::Accepted);
    MZ_ASSERT_EQ(queue.push(video(0, false, 40)), FrameQueue::PushResult::Accepted);
    MZ_ASSERT_EQ(queue.bytes(), 80u);
    // 80 + 40 > 100 → 丢掉最旧的可丢包
    MZ_ASSERT_EQ(queue.push(video(0, false, 40)), FrameQueue::PushResult::DroppedToMakeRoom);
    MZ_ASSERT_EQ(queue.bytes(), 80u);
    MZ_ASSERT_EQ(queue.packets(), 2u);
    MZ_ASSERT_LE(queue.bytes(), queue.limits().max_bytes); // 上限是硬的

    // 单包就超过整个字节上限：可丢的丢新包（正常降级），不可丢的必须报错（调用方断开）
    MZ_ASSERT_EQ(queue.push(video(0, true, 200)), FrameQueue::PushResult::RejectedNoSpace);
    MZ_ASSERT_EQ(queue.push(video(0, false, 200)), FrameQueue::PushResult::DroppedIncoming);
    MZ_ASSERT_EQ(queue.bytes(), 80u); // 队列没被这两个包动过
    MZ_ASSERT_EQ(queue.stats().rejected_no_space, 1u);
    MZ_ASSERT_EQ(queue.stats().dropped_incoming, 1u);

    // 帧数上限先到（字节上限给得很大）
    FrameQueue::Limits limits2;
    limits2.max_packets = 2;
    limits2.max_bytes = 1u * kMB;
    FrameQueue queue2;
    MZ_ASSERT_TRUE(queue2.setLimits(limits2));
    MZ_ASSERT_EQ(queue2.push(video(0, false, 1)), FrameQueue::PushResult::Accepted);
    MZ_ASSERT_EQ(queue2.push(video(0, false, 1)), FrameQueue::PushResult::Accepted);
    MZ_ASSERT_EQ(queue2.push(video(0, false, 1)), FrameQueue::PushResult::DroppedToMakeRoom);
    MZ_ASSERT_EQ(queue2.packets(), 2u);
}

MZ_TEST(media_queue_keyframe_makes_room_or_rejects) {
    FrameQueue::Limits limits;
    limits.max_packets = 2;
    limits.max_bytes = 1u * kMB;

    FrameQueue queue;
    MZ_ASSERT_TRUE(queue.setLimits(limits));
    auto n1 = video(0, false, 10);
    auto n2 = video(0, false, 10);
    MZ_ASSERT_EQ(queue.push(n1), FrameQueue::PushResult::Accepted);
    MZ_ASSERT_EQ(queue.push(n2), FrameQueue::PushResult::Accepted);

    // 关键帧到来：丢掉可丢的旧包让它进去（关键帧绝不因为"队列满"而被丢）
    auto key = video(0, true, 10);
    MZ_ASSERT_EQ(queue.push(key), FrameQueue::PushResult::DroppedToMakeRoom);
    auto kept = drain(&queue);
    MZ_ASSERT_EQ(kept.size(), 2u);
    MZ_ASSERT_TRUE(contains(kept, n2));
    MZ_ASSERT_TRUE(contains(kept, key));

    // 队列里全是不可丢的包（关键帧 + 音频）→ 新关键帧也挤不进去：必须报错
    FrameQueue queue2;
    MZ_ASSERT_TRUE(queue2.setLimits(limits));
    MZ_ASSERT_EQ(queue2.push(video(0, true, 10)), FrameQueue::PushResult::Accepted);
    MZ_ASSERT_EQ(queue2.push(audio(1, 10)), FrameQueue::PushResult::Accepted);
    MZ_ASSERT_EQ(queue2.push(video(0, true, 10)), FrameQueue::PushResult::RejectedNoSpace);
    MZ_ASSERT_EQ(queue2.packets(), 2u);
    MZ_ASSERT_EQ(queue2.stats().rejected_no_space, 1u);
}

MZ_TEST(media_queue_rejects_invalid_limits_and_eos) {
    FrameQueue queue;

    // 0 不等于无界：必须拒绝，且保持原值（不能变成"半套上限"）
    FrameQueue::Limits zero_packets;
    zero_packets.max_packets = 0;
    MZ_ASSERT_FALSE(queue.setLimits(zero_packets));
    FrameQueue::Limits zero_bytes;
    zero_bytes.max_bytes = 0;
    MZ_ASSERT_FALSE(queue.setLimits(zero_bytes));
    FrameQueue::Limits too_huge;
    too_huge.max_bytes = 1024u * kMB; // 超过硬上限
    MZ_ASSERT_FALSE(queue.setLimits(too_huge));
    MZ_ASSERT_EQ(queue.limits().max_packets, 64u);
    MZ_ASSERT_EQ(queue.limits().max_bytes, 8u * kMB);

    FrameQueue::Limits ok;
    ok.max_packets = 2;
    ok.max_bytes = 1u * kMB;
    MZ_ASSERT_TRUE(queue.setLimits(ok));
    MZ_ASSERT_EQ(queue.limits().max_packets, 2u);

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
    MZ_ASSERT_EQ(cache.maxBytes(), 8u * kMB);
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

    // 缓存里音频**可丢**（与 FrameQueue 的有意差异，见 gop_cache.h 文件头）
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
    limits.queue.max_packets = 8;
    limits.queue.max_bytes = 1u * kMB;
    MZ_ASSERT_TRUE(source.setLimits(limits));

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
    MZ_ASSERT_EQ(fast1->queue().stats().dropped_non_key, 0u);
    MZ_ASSERT_EQ(fast2->queue().stats().dropped_non_key, 0u);

    MZ_ASSERT_EQ(slow->queue().packets(), 8u);                    // 上限生效
    MZ_ASSERT_EQ(slow->queue().stats().dropped_non_key, 22u);      // 30 - 8
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
    limits.queue.max_bytes = 50;
    MZ_ASSERT_TRUE(tiny.setLimits(limits));
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

    // max_subscribers = 0 被拒，且原值不变
    MediaSource::Limits zero_subs;
    zero_subs.max_subscribers = 0;
    MZ_ASSERT_FALSE(source.setLimits(zero_subs));
    MZ_ASSERT_EQ(source.limits().max_subscribers, 2u);

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
