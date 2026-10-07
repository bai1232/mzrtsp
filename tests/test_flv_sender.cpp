/*
 * FlvSender 单元测试（M6-b；用例名 `flv_sender_*`，归入 ctest 分组 `flv`）
 * ============================================================================
 * 契约来源：docs/DESIGN_M6.md §3.4 / §5；上位需求 FR-3.1 / FR-3.4、NFR-1
 *
 * 为什么能确定性测试：`FlvSender` 的写出目标抽象成 **Sink**（不是 socket），
 * 所以这里用假 sink 收字节、直接调 `onDrain()` —— 不需要网络、不需要线程、不依赖时序。
 * 真正的 HTTP 字节级验收在 `scripts/flv_http_test.sh`（curl 拉流 + ffprobe）。
 *
 * 覆盖维度：
 *   正常  start() 立即发 header + 两个 sequence header；drain 把队列取空并保序封装
 *   空    没有 EOS 时 drain 只搬数据；EOS 后 finish 一次（幂等）
 *   满    broken（关键帧进不去队列）→ Aborted（不与"正常结束"混淆）
 *   断开  sink 写失败 → 停止写、置 aborted；连接断后不再有任何写入
 *   超大  ——（FLV 的字段边界在 `flv_muxer_*` 用例里覆盖）
 * ============================================================================
 */

#include "test_main.h"

#include "media/media_source.h"
#include "output/flv_sender.h"
#include "output/flv_muxer.h"

#include <memory>
#include <string>
#include <vector>

using namespace mzmedia;

namespace {

uint32_t readU24(const std::string &s, size_t offset) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(s[offset])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[offset + 1])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(s[offset + 2]));
}

/// 假 sink：把字节收进一个字符串，并记录 end 被调了几次
struct FakeSink {
    std::string bytes;
    int end_calls = 0;
    int write_calls = 0;
    /// 失败注入：fail_enabled 为真且已写过 fail_after 次之后，write 返回 false
    bool fail_enabled = false;
    int fail_after = 0;

    FlvSender::Sink make() {
        FlvSender::Sink sink;
        sink.write = [this](const char *data, size_t len) {
            ++write_calls;
            if (fail_enabled && write_calls > fail_after) {
                return false;
            }
            bytes.append(data, len);
            return true;
        };
        sink.end = [this]() { ++end_calls; };
        return sink;
    }
};

std::shared_ptr<const std::vector<uint8_t>> fakeAvcC() {
    return std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0x01, 0x42, 0x00, 0x1e, 0xff, 0xe1, 0x00});
}

std::shared_ptr<const std::vector<uint8_t>> fakeAsc() {
    return std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0x12, 0x10});
}

StreamInfo makeVideo(int index = 0, AVCodecID codec = AV_CODEC_ID_H264,
                     std::shared_ptr<const std::vector<uint8_t>> extra = fakeAvcC()) {
    StreamInfo info;
    info.index = index;
    info.codec_id = codec;
    info.codec_name = (codec == AV_CODEC_ID_H264) ? "h264" : "hevc";
    info.is_video = true;
    info.width = 320;
    info.height = 240;
    info.time_base = TimeBase{1, 1000};
    info.extradata = std::move(extra);
    return info;
}

StreamInfo makeAudio(int index = 1) {
    StreamInfo info;
    info.index = index;
    info.codec_id = AV_CODEC_ID_AAC;
    info.codec_name = "aac";
    info.is_audio = true;
    info.sample_rate = 44100;
    info.channels = 2;
    info.time_base = TimeBase{1, 1000};
    info.extradata = fakeAsc();
    return info;
}

MediaPacket::Ptr videoPacket(int index, bool key, size_t bytes, int64_t dts_ms, int64_t pts_ms) {
    auto payload = std::make_shared<const std::vector<uint8_t>>(bytes, 0xAB);
    return MediaPacket::create(MediaKind::Video, index, payload, key, dts_ms, pts_ms);
}

MediaPacket::Ptr audioPacket(int index, size_t bytes, int64_t dts_ms, int64_t pts_ms) {
    auto payload = std::make_shared<const std::vector<uint8_t>>(bytes, 0xCD);
    return MediaPacket::create(MediaKind::Audio, index, payload, false, dts_ms, pts_ms);
}

} // namespace

// ---------------------------------------------------------------------------
// start / create
// ---------------------------------------------------------------------------

MZ_TEST(flv_sender_start_writes_header_and_sequence_headers) {
    StreamInfo video = makeVideo();
    StreamInfo audio = makeAudio();
    MediaSource source;
    auto subscriber = source.subscribe();
    MZ_ASSERT_NOT_NULL(subscriber.get());
    if (!subscriber) {
        return;
    }

    FakeSink sink;
    auto sender = FlvSender::create(sink.make(), FlvMuxer::Streams{&video, &audio}, subscriber);
    MZ_ASSERT_NOT_NULL(sender.get());
    if (!sender) {
        return;
    }

    MZ_ASSERT_EQ(sender->start(), FlvSender::Result::Ok);
    // FR-3.1：连接建立后**立即**发出 header + 两个 sequence header（不等第一帧）
    MZ_ASSERT_GT(sink.bytes.size(), 13u);
    MZ_ASSERT_EQ(sink.bytes[0], 'F');
    MZ_ASSERT_EQ(sink.bytes[1], 'L');
    MZ_ASSERT_EQ(sink.bytes[2], 'V');
    MZ_ASSERT_EQ(sink.write_calls, 1); // 一次写出去（首帧延迟最小）
    // 第一个 tag 是视频 sequence header（类型 9、AVCPacketType=0）
    MZ_ASSERT_EQ(static_cast<int>(static_cast<uint8_t>(sink.bytes[13])), 9);
    MZ_ASSERT_EQ(static_cast<int>(static_cast<uint8_t>(sink.bytes[24])), 0x17);
    MZ_ASSERT_EQ(static_cast<int>(static_cast<uint8_t>(sink.bytes[25])), 0x00);
    MZ_ASSERT_FALSE(sender->finished());
}

MZ_TEST(flv_sender_create_rejects_bad_streams) {
    StreamInfo hevc = makeVideo(0, AV_CODEC_ID_HEVC);
    MediaSource source;
    auto subscriber = source.subscribe();
    if (!subscriber) {
        return;
    }
    FakeSink sink;
    // HEVC：FLV 的浏览器播放端不支持 → 明确拒绝（SPEC 有决策记录）
    MZ_ASSERT_NULL(FlvSender::create(sink.make(), FlvMuxer::Streams{&hevc, nullptr}, subscriber).get());

    // sink 缺 write：调用方 bug，不能让它在发送时才炸
    FlvSender::Sink bad;
    bad.end = []() {};
    MZ_ASSERT_NULL(FlvSender::create(bad, FlvMuxer::Streams{}, subscriber).get());
    MZ_ASSERT_NULL(FlvSender::create(sink.make(), FlvMuxer::Streams{}, nullptr).get());
}

MZ_TEST(flv_sender_rejects_drain_before_start) {
    StreamInfo video = makeVideo();
    MediaSource source;
    auto subscriber = source.subscribe();
    if (!subscriber) {
        return;
    }
    FakeSink sink;
    auto sender = FlvSender::create(sink.make(), FlvMuxer::Streams{&video, nullptr}, subscriber);
    MZ_ASSERT_NOT_NULL(sender.get());
    if (!sender) {
        return;
    }
    MZ_ASSERT_EQ(sender->onDrain(), FlvSender::Result::NotStarted);
    MZ_ASSERT_EQ(sink.write_calls, 0);
}

// ---------------------------------------------------------------------------
// drain：搬包、保序、收尾
// ---------------------------------------------------------------------------

MZ_TEST(flv_sender_drain_muxes_all_packets_in_order) {
    StreamInfo video = makeVideo();
    StreamInfo audio = makeAudio();
    MediaSource source;
    auto subscriber = source.subscribe();
    MZ_ASSERT_NOT_NULL(subscriber.get());
    if (!subscriber) {
        return;
    }

    FakeSink sink;
    auto sender = FlvSender::create(sink.make(), FlvMuxer::Streams{&video, &audio}, subscriber);
    MZ_ASSERT_NOT_NULL(sender.get());
    if (!sender) {
        return;
    }
    MZ_ASSERT_EQ(sender->start(), FlvSender::Result::Ok);
    const size_t header_size = sink.bytes.size();

    // 往源里推 3 个包（源会把它们放进订阅者队列）
    (void) source.pushPacket(videoPacket(0, true, 16, 0, 0));
    (void) source.pushPacket(audioPacket(1, 8, 10, 10));
    (void) source.pushPacket(videoPacket(0, false, 16, 40, 40));

    MZ_ASSERT_EQ(sender->onDrain(), FlvSender::Result::Ok);
    MZ_ASSERT_EQ(sender->packetsMuxed(), 3u);
    MZ_ASSERT_EQ(subscriber->queue().packets(), 0u); // 队列被取空
    MZ_ASSERT_GT(sink.bytes.size(), header_size);

    // 三个 tag 依次是：视频(9) / 音频(8) / 视频(9)，且时间戳 0 / 10 / 40
    size_t offset = header_size;
    const int types[3] = {9, 8, 9};
    const uint32_t stamps[3] = {0, 10, 40};
    for (int i = 0; i < 3; ++i) {
        MZ_ASSERT_EQ(static_cast<int>(static_cast<uint8_t>(sink.bytes[offset])), types[i]);
        const uint32_t size = readU24(sink.bytes, offset + 1);
        const uint32_t stamp = readU24(sink.bytes, offset + 4) |
                               (static_cast<uint32_t>(static_cast<uint8_t>(sink.bytes[offset + 7])) << 24);
        MZ_ASSERT_EQ(stamp, stamps[i]);
        offset += 11 + size + 4; // tag 头 + 数据 + PreviousTagSize
    }
    MZ_ASSERT_EQ(offset, sink.bytes.size()); // 没有多余字节
    MZ_ASSERT_EQ(sender->lastTimestampMs(), 40u);
}

MZ_TEST(flv_sender_eos_finishes_once) {
    StreamInfo video = makeVideo();
    MediaSource source;
    auto subscriber = source.subscribe();
    MZ_ASSERT_NOT_NULL(subscriber.get());
    if (!subscriber) {
        return;
    }
    FakeSink sink;
    auto sender = FlvSender::create(sink.make(), FlvMuxer::Streams{&video, nullptr}, subscriber);
    MZ_ASSERT_NOT_NULL(sender.get());
    if (!sender) {
        return;
    }
    MZ_ASSERT_EQ(sender->start(), FlvSender::Result::Ok);
    (void) source.pushPacket(videoPacket(0, true, 16, 0, 0));

    MZ_ASSERT_FALSE(sender->finished());
    MZ_ASSERT_EQ(sender->onDrain(), FlvSender::Result::Ok);
    MZ_ASSERT_FALSE(sender->finished()); // 还没有 EOS
    MZ_ASSERT_EQ(sink.end_calls, 0);

    // 源结束 → 下一次 drain 收尾（发结束块 + 关连接由 HTTP 侧做）
    MZ_ASSERT_TRUE(source.endOfStream());
    MZ_ASSERT_EQ(sender->onDrain(), FlvSender::Result::Ok);
    MZ_ASSERT_TRUE(sender->finished());
    MZ_ASSERT_EQ(sink.end_calls, 1);

    // 幂等：再 drain 不会重复收尾、也不会再写
    const size_t bytes = sink.bytes.size();
    MZ_ASSERT_EQ(sender->onDrain(), FlvSender::Result::Ok);
    MZ_ASSERT_EQ(sink.end_calls, 1);
    MZ_ASSERT_EQ(sink.bytes.size(), bytes);
}

MZ_TEST(flv_sender_broken_subscriber_aborts) {
    StreamInfo video = makeVideo();
    // 队列上限极小（400 kbps × 1 ms = 50 字节）→ 100 字节的关键帧进不去 → broken
    MediaSource::Limits limits;
    limits.max_bitrate_bps = 400000;
    limits.latency_budget_ms = 1;
    auto source = std::make_shared<MediaSource>(limits);
    MZ_ASSERT_EQ(source->limits().queueMaxBytes(), 50u);

    auto subscriber = source->subscribe();
    MZ_ASSERT_NOT_NULL(subscriber.get());
    if (!subscriber) {
        return;
    }
    FakeSink sink;
    auto sender = FlvSender::create(sink.make(), FlvMuxer::Streams{&video, nullptr}, subscriber);
    MZ_ASSERT_NOT_NULL(sender.get());
    if (!sender) {
        return;
    }
    MZ_ASSERT_EQ(sender->start(), FlvSender::Result::Ok);

    (void) source->pushPacket(videoPacket(0, true, 100, 0, 0)); // 关键帧 > 队列上限
    MZ_ASSERT_TRUE(subscriber->broken());

    // broken 的含义是"关键帧已经进不去" → 继续发只会让对端花屏，明确断开
    MZ_ASSERT_EQ(sender->onDrain(), FlvSender::Result::Aborted);
    MZ_ASSERT_FALSE(sender->finished()); // 与"正常结束"区分开
    MZ_ASSERT_EQ(sink.end_calls, 0);
}

MZ_TEST(flv_sender_stops_when_sink_fails) {
    StreamInfo video = makeVideo();
    MediaSource source;
    auto subscriber = source.subscribe();
    MZ_ASSERT_NOT_NULL(subscriber.get());
    if (!subscriber) {
        return;
    }
    FakeSink sink;
    sink.fail_enabled = true;
    sink.fail_after = 1; // 第一次写（header）成功，之后都失败
    auto sender = FlvSender::create(sink.make(), FlvMuxer::Streams{&video, nullptr}, subscriber);
    MZ_ASSERT_NOT_NULL(sender.get());
    if (!sender) {
        return;
    }
    MZ_ASSERT_EQ(sender->start(), FlvSender::Result::Ok);
    (void) source.pushPacket(videoPacket(0, true, 16, 0, 0));
    (void) source.pushPacket(videoPacket(0, false, 16, 40, 40));

    // 连接已坏：onDrain 报 SinkFailed 并置 aborted，之后再 drain 不再写
    MZ_ASSERT_EQ(sender->onDrain(), FlvSender::Result::SinkFailed);
    MZ_ASSERT_TRUE(sender->aborted());
    const int calls = sink.write_calls;
    MZ_ASSERT_EQ(sender->onDrain(), FlvSender::Result::Ok);
    MZ_ASSERT_EQ(sink.write_calls, calls);
}

MZ_TEST(flv_sender_foreign_packet_is_reported) {
    StreamInfo video = makeVideo();
    MediaSource source;
    auto subscriber = source.subscribe();
    MZ_ASSERT_NOT_NULL(subscriber.get());
    if (!subscriber) {
        return;
    }
    FakeSink sink;
    auto sender = FlvSender::create(sink.make(), FlvMuxer::Streams{&video, nullptr}, subscriber);
    MZ_ASSERT_NOT_NULL(sender.get());
    if (!sender) {
        return;
    }
    MZ_ASSERT_EQ(sender->start(), FlvSender::Result::Ok);

    // 不属于这两路流的包：**不能静默丢**（媒体层会把它推给所有订阅者）
    (void) source.pushPacket(videoPacket(99, true, 16, 0, 0));
    MZ_ASSERT_EQ(sender->onDrain(), FlvSender::Result::BadStreams);
    MZ_ASSERT_TRUE(!sender->lastError().empty());
}
