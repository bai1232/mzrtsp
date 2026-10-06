/*
 * FLV 封装单元测试（M6-a，分组 `flv`）
 * ============================================================================
 * 契约来源：docs/DESIGN_M6.md §3 / §5；上位需求 FR-3.1 / FR-3.3 / FR-3.4
 *
 * 覆盖维度：
 *   正常  FLV Header / sequence header / 视频 tag / 音频 tag 的**逐字节布局**
 *   空    没有流；没有初始化头（avcC / AudioSpecificConfig）
 *   满    单包超过 24 位 tag 长度上限；CompositionTime 超 24 位
 *   断开  ——（本层无连接概念，连接侧在 M6-b）
 *   超大  32 位时间戳回绕；早于基准的包被钳到 0（不产生 40 亿这种值）
 *
 * 为什么坚持字节级断言：FLV 写错是"能连上、播放器一片黑"，没有异常、没有报错 ——
 * 最难查的那类问题。所以这里不写"看起来对"，直接比对偏移与字段。
 * 真实文件的端到端验证在 `scripts/flv_mux_test.sh`（demo 产文件 + ffprobe 校验）。
 * ============================================================================
 */

#include "test_main.h"

#include "core/util.h"
#include "ffmpeg/demuxer.h"
#include "media/media_packet.h"
#include "output/flv_muxer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace mzmedia;

namespace {

/// 找样本（与其它分组同一算法：向上最多 8 层，与从哪启动无关）
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

uint32_t readU24(const std::string &s, size_t offset) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(s[offset])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[offset + 1])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(s[offset + 2]));
}

uint32_t readU32(const std::string &s, size_t offset) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(s[offset])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[offset + 1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[offset + 2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(s[offset + 3]));
}

/// 按**无符号**读一个字节
/// @note 必须走 uint8_t：`char` 在 x86 上是有符号的，`static_cast<int>(out[i])` 会把 0xAF 变成 -81
int byteAt(const std::string &s, size_t offset) {
    return static_cast<int>(static_cast<uint8_t>(s[offset]));
}

/// tag 的时间戳（低 24 位 + 高 8 位）
uint32_t readTimestamp(const std::string &s, size_t tag_offset) {
    return readU24(s, tag_offset + 4) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[tag_offset + 7])) << 24);
}

/// 一个"结构上合法"的假 avcC（首字节必须是 1）
std::shared_ptr<const std::vector<uint8_t>> fakeAvcC() {
    return std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0x01, 0x42, 0x00, 0x1e, 0xff, 0xe1, 0x00});
}

/// AAC-LC / 44.1kHz / 立体声 的 AudioSpecificConfig（2 字节）
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

StreamInfo makeAudio(int index = 1, AVCodecID codec = AV_CODEC_ID_AAC,
                     std::shared_ptr<const std::vector<uint8_t>> extra = fakeAsc()) {
    StreamInfo info;
    info.index = index;
    info.codec_id = codec;
    info.codec_name = (codec == AV_CODEC_ID_AAC) ? "aac" : "mp3";
    info.is_audio = true;
    info.sample_rate = 44100;
    info.channels = 2;
    info.time_base = TimeBase{1, 1000};
    info.extradata = std::move(extra);
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
// 头部与初始化头
// ---------------------------------------------------------------------------

MZ_TEST(flv_header_bytes_are_exact) {
    StreamInfo video = makeVideo();
    StreamInfo audio = makeAudio();

    // 只有视频：flags = 0x01
    FlvMuxer video_only(FlvMuxer::Streams{&video, nullptr});
    MZ_ASSERT_EQ(video_only.prepare(), FlvMuxer::Result::Ok);
    std::string head;
    MZ_ASSERT_TRUE(video_only.writeHeader(&head));
    MZ_ASSERT_EQ(head.size(), 13u); // 9 字节 header + 4 字节 PreviousTagSize0
    MZ_ASSERT_EQ(head[0], 'F');
    MZ_ASSERT_EQ(head[1], 'L');
    MZ_ASSERT_EQ(head[2], 'V');
    MZ_ASSERT_EQ(static_cast<int>(head[3]), 1);          // version
    MZ_ASSERT_EQ(static_cast<int>(head[4]), 0x01);       // 只有视频
    MZ_ASSERT_EQ(readU32(head, 5), 9u);                  // DataOffset
    MZ_ASSERT_EQ(readU32(head, 9), 0u);                  // PreviousTagSize0

    // 有音频：flags = 0x05
    FlvMuxer both(FlvMuxer::Streams{&video, &audio});
    MZ_ASSERT_EQ(both.prepare(), FlvMuxer::Result::Ok);
    std::string head2;
    MZ_ASSERT_TRUE(both.writeHeader(&head2));
    MZ_ASSERT_EQ(static_cast<int>(head2[4]), 0x05);
}

MZ_TEST(flv_sequence_headers_layout) {
    StreamInfo video = makeVideo();
    StreamInfo audio = makeAudio();
    FlvMuxer muxer(FlvMuxer::Streams{&video, &audio});
    MZ_ASSERT_EQ(muxer.prepare(), FlvMuxer::Result::Ok);

    std::string out;
    MZ_ASSERT_TRUE(muxer.writeHeader(&out));
    MZ_ASSERT_EQ(muxer.writeSequenceHeaders(&out), FlvMuxer::Result::Ok);
    MZ_ASSERT_EQ(muxer.tagCount(), 2u);

    // ---- 第一个 tag：AVC sequence header ----
    const size_t tag0 = 13;
    MZ_ASSERT_EQ(static_cast<int>(out[tag0]), 9);                    // tagType = video
    const uint32_t size0 = readU24(out, tag0 + 1);
    MZ_ASSERT_EQ(size0, static_cast<uint32_t>(5 + video.extradata->size()));
    MZ_ASSERT_EQ(readTimestamp(out, tag0), 0u);                      // sequence header 的时间戳为 0
    MZ_ASSERT_EQ(readU24(out, tag0 + 8), 0u);                        // StreamID = 0
    const size_t data0 = tag0 + 11;
    MZ_ASSERT_EQ(static_cast<int>(out[data0]), 0x17);                // 关键帧 + AVC
    MZ_ASSERT_EQ(static_cast<int>(out[data0 + 1]), 0x00);            // AVCPacketType = sequence header
    MZ_ASSERT_EQ(readU24(out, data0 + 2), 0u);                       // CompositionTime = 0
    MZ_ASSERT_EQ(std::memcmp(out.data() + data0 + 5, video.extradata->data(), video.extradata->size()),
                 0); // 负载就是 avcC
    MZ_ASSERT_EQ(readU32(out, data0 + size0), 11u + size0);           // PreviousTagSize

    // ---- 第二个 tag：AAC sequence header ----
    const size_t tag1 = data0 + size0 + 4;
    MZ_ASSERT_EQ(static_cast<int>(out[tag1]), 8);                    // tagType = audio
    const uint32_t size1 = readU24(out, tag1 + 1);
    MZ_ASSERT_EQ(size1, static_cast<uint32_t>(2 + audio.extradata->size()));
    const size_t data1 = tag1 + 11;
    MZ_ASSERT_EQ(byteAt(out, data1), 0xAF);                          // AAC + 44kHz + 16bit + stereo
    MZ_ASSERT_EQ(byteAt(out, data1 + 1), 0x00);                      // AACPacketType = sequence header
    MZ_ASSERT_EQ(std::memcmp(out.data() + data1 + 2, audio.extradata->data(), audio.extradata->size()),
                 0); // 负载就是 AudioSpecificConfig
    MZ_ASSERT_EQ(muxer.bytesWritten(), static_cast<uint64_t>(11 + size0 + 4 + 11 + size1 + 4));
}

// ---------------------------------------------------------------------------
// prepare：编码与初始化数据的把关
// ---------------------------------------------------------------------------

MZ_TEST(flv_prepare_rejects_bad_inputs) {
    // 既没有视频也没有音频
    FlvMuxer none(FlvMuxer::Streams{});
    MZ_ASSERT_EQ(none.prepare(), FlvMuxer::Result::NoStreams);

    // 视频不是 H264（HEVC 明确不做）
    StreamInfo hevc = makeVideo(0, AV_CODEC_ID_HEVC);
    FlvMuxer hevc_muxer(FlvMuxer::Streams{&hevc, nullptr});
    MZ_ASSERT_EQ(hevc_muxer.prepare(), FlvMuxer::Result::UnsupportedVideoCodec);
    MZ_ASSERT_TRUE(!hevc_muxer.prepared());

    // H264 但没有 avcC
    StreamInfo no_extra = makeVideo(0, AV_CODEC_ID_H264, nullptr);
    FlvMuxer no_extra_muxer(FlvMuxer::Streams{&no_extra, nullptr});
    MZ_ASSERT_EQ(no_extra_muxer.prepare(), FlvMuxer::Result::MissingExtradata);

    // 初始化数据看着是 Annex-B（首字节 0x00）→ M6-a 明确拒绝，不做转换
    auto annexb = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e});
    StreamInfo raw = makeVideo(0, AV_CODEC_ID_H264, annexb);
    FlvMuxer raw_muxer(FlvMuxer::Streams{&raw, nullptr});
    MZ_ASSERT_EQ(raw_muxer.prepare(), FlvMuxer::Result::AnnexBNotSupported);
    MZ_ASSERT_TRUE(raw_muxer.lastError().find("Annex-B") != std::string::npos);

    // 音频不是 AAC
    StreamInfo video = makeVideo();
    StreamInfo mp3 = makeAudio(1, AV_CODEC_ID_MP3);
    FlvMuxer mp3_muxer(FlvMuxer::Streams{&video, &mp3});
    MZ_ASSERT_EQ(mp3_muxer.prepare(), FlvMuxer::Result::UnsupportedAudioCodec);

    // AAC 但没有 AudioSpecificConfig
    StreamInfo no_asc = makeAudio(1, AV_CODEC_ID_AAC, nullptr);
    FlvMuxer no_asc_muxer(FlvMuxer::Streams{&video, &no_asc});
    MZ_ASSERT_EQ(no_asc_muxer.prepare(), FlvMuxer::Result::MissingExtradata);

    // 没 prepare 就写：必须拒绝
    FlvMuxer not_prepared(FlvMuxer::Streams{&video, nullptr});
    std::string out;
    MZ_ASSERT_EQ(not_prepared.writePacket(*videoPacket(0, true, 8, 0, 0), &out),
                 FlvMuxer::Result::NotPrepared);
}

// ---------------------------------------------------------------------------
// 视频 / 音频 tag
// ---------------------------------------------------------------------------

MZ_TEST(flv_video_tag_layout_and_composition_time) {
    StreamInfo video = makeVideo();
    FlvMuxer muxer(FlvMuxer::Streams{&video, nullptr});
    MZ_ASSERT_EQ(muxer.prepare(), FlvMuxer::Result::Ok);
    muxer.setTimestampBaseMs(1000);

    std::string out;
    // 关键帧：dts=1040 / pts=1080 → 时间戳 40、CompositionTime 40
    MZ_ASSERT_EQ(muxer.writePacket(*videoPacket(0, true, 16, 1040, 1080), &out),
                 FlvMuxer::Result::Ok);
    MZ_ASSERT_EQ(static_cast<int>(out[0]), 9);
    const uint32_t size0 = readU24(out, 1);
    MZ_ASSERT_EQ(size0, 5u + 16u);
    MZ_ASSERT_EQ(readTimestamp(out, 0), 40u);
    const size_t data0 = 11;
    MZ_ASSERT_EQ(static_cast<int>(out[data0]), 0x17);             // 关键帧 + AVC
    MZ_ASSERT_EQ(static_cast<int>(out[data0 + 1]), 0x01);         // AVCPacketType = NALU
    MZ_ASSERT_EQ(readU24(out, data0 + 2), 40u);                   // CompositionTime = pts - dts
    MZ_ASSERT_EQ(readU32(out, data0 + size0), 11u + size0);
    MZ_ASSERT_EQ(muxer.lastTimestampMs(), 40u);

    // 非关键帧 + B 帧语义（pts < dts，CompositionTime 为负）：24 位补码
    std::string out2;
    MZ_ASSERT_EQ(muxer.writePacket(*videoPacket(0, false, 8, 1080, 1040), &out2),
                 FlvMuxer::Result::Ok);
    MZ_ASSERT_EQ(static_cast<int>(out2[11]), 0x27);               // 非关键帧 + AVC
    MZ_ASSERT_EQ(readU24(out2, 13), 0xFFFFD8u);                   // -40 的 24 位补码
    MZ_ASSERT_EQ(readTimestamp(out2, 0), 80u);
}

MZ_TEST(flv_audio_tag_layout) {
    StreamInfo video = makeVideo();
    StreamInfo audio = makeAudio();
    FlvMuxer muxer(FlvMuxer::Streams{&video, &audio});
    MZ_ASSERT_EQ(muxer.prepare(), FlvMuxer::Result::Ok);
    muxer.setTimestampBaseMs(0);

    std::string out;
    MZ_ASSERT_EQ(muxer.writePacket(*audioPacket(1, 12, 23, 23), &out), FlvMuxer::Result::Ok);
    MZ_ASSERT_EQ(static_cast<int>(out[0]), 8);                    // tagType = audio
    MZ_ASSERT_EQ(readU24(out, 1), 2u + 12u);                      // 2 字节音频头 + 负载
    MZ_ASSERT_EQ(readTimestamp(out, 0), 23u);
    MZ_ASSERT_EQ(byteAt(out, 11), 0xAF);                          // AAC + 44kHz + 16bit + stereo
    MZ_ASSERT_EQ(byteAt(out, 12), 0x01);                          // AACPacketType = raw
}

MZ_TEST(flv_rejects_foreign_stream_and_oversized_composition) {
    StreamInfo video = makeVideo();
    FlvMuxer muxer(FlvMuxer::Streams{&video, nullptr});
    MZ_ASSERT_EQ(muxer.prepare(), FlvMuxer::Result::Ok);
    std::string out;

    // 包不属于这两路流：**不能静默丢**
    MZ_ASSERT_EQ(muxer.writePacket(*videoPacket(99, true, 8, 0, 0), &out),
                 FlvMuxer::Result::InvalidPacket);

    // CompositionTime 超 24 位有符号范围
    MZ_ASSERT_EQ(muxer.writePacket(*videoPacket(0, true, 8, 0, 9000000), &out),
                 FlvMuxer::Result::InvalidPacket);

    // 单包超过 24 位 tag 长度上限
    MZ_ASSERT_EQ(muxer.writePacket(*videoPacket(0, true, 0x1000000u, 0, 0), &out),
                 FlvMuxer::Result::TooLarge);
}

// ---------------------------------------------------------------------------
// 时间戳基准（每个客户端一份）
// ---------------------------------------------------------------------------

MZ_TEST(flv_timestamp_base_is_per_client) {
    StreamInfo video = makeVideo();
    const auto p0 = videoPacket(0, true, 8, 0, 0);
    const auto p1 = videoPacket(0, false, 8, 40, 40);
    const auto p2 = videoPacket(0, false, 8, 80, 80);

    // 客户端 A：从头接入 → 时间戳 0 / 40 / 80
    FlvMuxer a(FlvMuxer::Streams{&video, nullptr});
    MZ_ASSERT_EQ(a.prepare(), FlvMuxer::Result::Ok);
    std::string out_a;
    MZ_ASSERT_EQ(a.writePacket(*p0, &out_a), FlvMuxer::Result::Ok);
    MZ_ASSERT_EQ(a.writePacket(*p1, &out_a), FlvMuxer::Result::Ok);
    MZ_ASSERT_TRUE(a.baseFixed());
    MZ_ASSERT_EQ(a.timestampBaseMs(), 0);

    // 客户端 B：中途接入（从 p2 开始）→ 自己的时间戳也从 0 开始
    FlvMuxer b(FlvMuxer::Streams{&video, nullptr});
    MZ_ASSERT_EQ(b.prepare(), FlvMuxer::Result::Ok);
    std::string out_b;
    MZ_ASSERT_EQ(b.writePacket(*p2, &out_b), FlvMuxer::Result::Ok);
    MZ_ASSERT_EQ(b.timestampBaseMs(), 80);
    MZ_ASSERT_EQ(b.lastTimestampMs(), 0u);   // ← 这就是"per-client 基准"的意义
    MZ_ASSERT_EQ(readTimestamp(out_b, 0), 0u);
}

MZ_TEST(flv_timestamp_wraps_at_32bit_and_clamps_before_base) {
    StreamInfo video = makeVideo();

    // 32 位自然回绕：dts 越过 2^32 之后，时间戳按规范绕回小值
    FlvMuxer wrap(FlvMuxer::Streams{&video, nullptr});
    MZ_ASSERT_EQ(wrap.prepare(), FlvMuxer::Result::Ok);
    wrap.setTimestampBaseMs(0);
    std::string out;
    const int64_t big = 0x100000005LL; // 2^32 + 5
    MZ_ASSERT_EQ(wrap.writePacket(*videoPacket(0, false, 8, big, big), &out),
                 FlvMuxer::Result::Ok);
    MZ_ASSERT_EQ(wrap.lastTimestampMs(), 5u);

    // 早于基准：钳到 0 并计数（不能出现 40 亿这种时间戳）
    FlvMuxer clamp_muxer(FlvMuxer::Streams{&video, nullptr});
    MZ_ASSERT_EQ(clamp_muxer.prepare(), FlvMuxer::Result::Ok);
    clamp_muxer.setTimestampBaseMs(1000);
    std::string out2;
    MZ_ASSERT_EQ(clamp_muxer.writePacket(*videoPacket(0, false, 8, 900, 900), &out2),
                 FlvMuxer::Result::Ok);
    MZ_ASSERT_EQ(clamp_muxer.lastTimestampMs(), 0u);
    MZ_ASSERT_EQ(clamp_muxer.clampedBeforeBase(), 1u);
}

// ---------------------------------------------------------------------------
// 真实文件：真 demux → 真封装 → 落盘（ffprobe 校验见 scripts/flv_mux_test.sh）
// ---------------------------------------------------------------------------

MZ_TEST(flv_muxes_real_mp4_to_file) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty()); // 找不到样本 → 明确失败（不是跳过）
    if (path.empty()) {
        return;
    }

    Demuxer demuxer;
    MZ_ASSERT_TRUE(demuxer.open(path));
    const StreamInfo *video = demuxer.firstVideo();
    const StreamInfo *audio = demuxer.firstAudio();
    MZ_ASSERT_NOT_NULL(video);
    MZ_ASSERT_NOT_NULL(audio);
    if (video == nullptr || audio == nullptr) {
        return;
    }
    MZ_ASSERT_NOT_NULL(video->extradata.get()); // MP4 的 avcC 必须被带出来
    MZ_ASSERT_NOT_NULL(audio->extradata.get());

    FlvMuxer muxer(FlvMuxer::Streams{video, audio});
    MZ_ASSERT_EQ(muxer.prepare(), FlvMuxer::Result::Ok);

    std::string flv;
    MZ_ASSERT_TRUE(muxer.writeHeader(&flv));
    MZ_ASSERT_EQ(muxer.writeSequenceHeaders(&flv), FlvMuxer::Result::Ok);

    int video_packets = 0;
    int audio_packets = 0;
    bool first_video_is_key = false;
    while (true) {
        const Demuxer::ReadResult result = demuxer.readPacket();
        if (result == Demuxer::ReadResult::Eof) {
            break;
        }
        MZ_ASSERT_EQ(result, Demuxer::ReadResult::Packet);
        if (result != Demuxer::ReadResult::Packet) {
            break;
        }
        int64_t pts = 0;
        int64_t dts = 0;
        (void) demuxer.packetTimestampsMs(&pts, &dts);
        const AVPacket *pkt = demuxer.packet();
        if (pkt == nullptr) {
            break;
        }
        const int index = demuxer.packetStreamIndex();
        const bool is_video = (index == video->index);
        auto payload = std::make_shared<const std::vector<uint8_t>>(
            pkt->data, pkt->data + static_cast<size_t>(pkt->size));
        const bool key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        MediaPacket::Ptr packet = MediaPacket::create(is_video ? MediaKind::Video : MediaKind::Audio,
                                                      index, payload, key, dts, pts);
        MZ_ASSERT_NOT_NULL(packet.get());
        if (!packet) {
            break;
        }
        if (is_video && video_packets == 0) {
            first_video_is_key = key;
        }
        MZ_ASSERT_EQ(muxer.writePacket(*packet, &flv), FlvMuxer::Result::Ok);
        if (is_video) {
            ++video_packets;
        } else {
            ++audio_packets;
        }
    }

    MZ_ASSERT_GT(video_packets, 0);
    MZ_ASSERT_GT(audio_packets, 0);
    MZ_ASSERT_TRUE(first_video_is_key); // 首个视频包必须是关键帧，否则播放端起不来
    MZ_ASSERT_EQ(muxer.tagCount(), static_cast<uint64_t>(2 + video_packets + audio_packets));
    MZ_ASSERT_EQ(muxer.clampedBeforeBase(), 0u); // 正常文件的基准就是最早的 dts

    // 落盘：给 scripts/flv_mux_test.sh 做 ffprobe 校验用（也顺手验证"字节能完整写出"）
    const std::string out_path = "/tmp/mzmedia_flv_mux_test.flv";
    MZ_ASSERT_TRUE(saveFile(out_path, flv));
    MZ_ASSERT_TRUE(fileExists(out_path));
    (void) std::remove(out_path.c_str());
}
