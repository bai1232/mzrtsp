/*
 * DemuxerProducer 单元测试（M6-c，分组 `producer`）
 * ============================================================================
 * 契约来源：docs/DESIGN_M6.md §3.6 / §5.6；上位需求 FR-2.1（MP4 / H264 裸流）、
 *           FR-3.1（AVC sequence header 必须是 avcC）、`--loop` 的循环重开语义
 *
 * 为什么单独一个分组：这里测的是**输入形状**的转换与重开，
 *   用真样本（`sample.mp4` / `sample.h264`）但**不起线程、不等时序** —— 直接调 `read()`，
 *   每一步都可断言。这与 `media`（纯策略、不需要样本）和 `srcmgr`（真起源线程 + 定时器）
 *   的依赖面都不同，混在一起会让"没样本就整组红"的范围无谓扩大。
 *
 * 覆盖维度（正常 / 空 / 满 / 断开 / 超大）：
 *   正常  MP4：流快照直接可用；`loop` 关闭时读到 Eof 就停（不重开）
 *   正常  裸流：extradata 是 Annex-B → 快照里给出构造好的 avcC；每个包都转成 AVCC
 *   空    没有 SPS/PPS 的裸流 / 缺 extradata → **明确失败**（不出坏流）
 *   满    —（解封装上限由 M4 的 Demuxer 用例覆盖）
 *   断开  循环重开：dts 跨段**严格递增**、偏移量可查、流参数变了就明确失败
 *
 * 依赖样本（由 `scripts/make_samples.sh` 现场生成）；找不到样本时用例**明确失败**，
 * 绝不静默跳过（"没测到"不能看起来像"测过了"）。
 * ============================================================================
 */

#include "test_main.h"

#include "core/util.h"
#include "ffmpeg/demuxer.h"
#include "ffmpeg/h264_util.h"
#include "media/demuxer_producer.h"
#include "output/flv_muxer.h"
#include "media/source_pump.h"

#include <cstdio>
#include <cstdlib>
#include <string>
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

std::string readWholeFile(const std::string &path) {
    std::string out;
    FILE *f = ::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return out;
    }
    char buffer[8192];
    size_t n = 0;
    while ((n = ::fread(buffer, 1, sizeof(buffer), f)) > 0) {
        out.append(buffer, n);
    }
    ::fclose(f);
    return out;
}

/**
 * 校验一个包是**合法 AVCC**：一路按 4 字节长度切下去，必须正好用完整个包
 * @return true = 形状正确（长度字段都不为 0、不越界、NAL 类型合法）
 */
bool isAvccShaped(const uint8_t *data, size_t size, int *nal_count) {
    *nal_count = 0;
    size_t pos = 0;
    while (pos + 4 <= size) {
        const size_t len = (static_cast<size_t>(data[pos]) << 24) |
                           (static_cast<size_t>(data[pos + 1]) << 16) |
                           (static_cast<size_t>(data[pos + 2]) << 8) |
                           static_cast<size_t>(data[pos + 3]);
        if (len == 0 || pos + 4 + len > size) {
            return false;
        }
        const uint8_t type = static_cast<uint8_t>(data[pos + 4] & 0x1Fu);
        if (type < 1 || type > 12) {
            return false;
        }
        ++(*nal_count);
        pos += 4 + len;
    }
    return pos == size && *nal_count > 0;
}

} // namespace

// ---------------------------------------------------------------------------
// 正常：MP4 输入（不需要转换）
// ---------------------------------------------------------------------------

MZ_TEST(producer_mp4_snapshot_is_usable_without_conversion) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }

    DemuxerProducer producer;
    MZ_ASSERT_TRUE(producer.open(path));
    MZ_ASSERT_FALSE(producer.annexBVideo()); // MP4 的 extradata 本来就是 avcC
    MZ_ASSERT_NOT_NULL(producer.muxerVideo());
    MZ_ASSERT_NOT_NULL(producer.muxerAudio());
    if (producer.muxerVideo() == nullptr) {
        return;
    }
    // 快照必须与解封装给出的一致（不是"另抄一份可能抄错的"）
    MZ_ASSERT_EQ(producer.muxerVideo()->index, producer.demuxer().firstVideo()->index);
    MZ_ASSERT_EQ(producer.muxerVideo()->width, 320);
    MZ_ASSERT_TRUE(producer.muxerVideo()->extradata != nullptr);
    if (producer.muxerVideo()->extradata != nullptr) {
        // 首字节 = configurationVersion = 1（avcC 的标志，Annex-B 会是 0）
        MZ_ASSERT_EQ(producer.muxerVideo()->extradata->at(0), 0x01u);
    }

    // 包原样转发（不转换）：读几帧，检查关键帧与**按流**的时间戳单调
    // （音视频交错时全局 dts 不保证单调，只有同一条流内保证 —— M4 的守卫是按流做的）
    int packets = 0;
    int64_t last_video_dts = 0;
    int64_t last_audio_dts = 0;
    bool has_video = false;
    bool has_audio = false;
    bool monotonic = true;
    while (packets < 20) {
        MediaPacket::Ptr packet;
        std::string error;
        const SourcePump::ReadResult result = producer.read(&packet, &error);
        MZ_ASSERT_TRUE(result == SourcePump::ReadResult::Packet); // 前 20 个包不该出现 Eof/Error
        if (result != SourcePump::ReadResult::Packet) {
            return;
        }
        MZ_ASSERT_NOT_NULL(packet.get());
        if (!packet) {
            return;
        }
        int64_t &last = (packet->kind() == MediaKind::Video) ? last_video_dts : last_audio_dts;
        bool &seen = (packet->kind() == MediaKind::Video) ? has_video : has_audio;
        if (seen && packet->dtsMs() < last) {
            monotonic = false; // 同一条流内回退（首包可能带负的起始偏移，所以要先看 seen）
        }
        last = packet->dtsMs();
        seen = true;
        ++packets;
    }
    MZ_ASSERT_TRUE(monotonic);
    MZ_ASSERT_EQ(producer.annexBPackets(), 0u); // 没发生转换
    MZ_ASSERT_EQ(producer.loopCount(), 0u);
}

MZ_TEST(producer_without_loop_stops_at_eof) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }
    DemuxerProducer producer;
    MZ_ASSERT_TRUE(producer.open(path));

    // 读到结束：必须是 EndOfStream（**不是** Error），且不重开
    int guard = 0;
    while (guard++ < 2000) {
        MediaPacket::Ptr packet;
        std::string error;
        const SourcePump::ReadResult result = producer.read(&packet, &error);
        if (result == SourcePump::ReadResult::EndOfStream) {
            break;
        }
        MZ_ASSERT_TRUE(result == SourcePump::ReadResult::Packet);
        if (result != SourcePump::ReadResult::Packet) {
            MZ_ASSERT_TRUE(error.empty()); // 失败必须带原因
            return;
        }
    }
    MZ_ASSERT_EQ(producer.loopCount(), 0u);
    MZ_ASSERT_EQ(producer.loopOffsetMs(), 0);
    MZ_ASSERT_GT(producer.totalPackets(), 100u); // 样本有 50 视频 + 88 音频
}

// ---------------------------------------------------------------------------
// 正常：H264 裸流（Annex-B → AVCC）
// ---------------------------------------------------------------------------

MZ_TEST(producer_annexb_sample_gets_avcc_snapshot) {
    const std::string path = samplePath("sample.h264");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }

    DemuxerProducer producer;
    MZ_ASSERT_TRUE(producer.open(path));
    // 判定：解封装的 extradata 是 Annex-B，所以必须转
    MZ_ASSERT_TRUE(producer.annexBVideo());
    MZ_ASSERT_NOT_NULL(producer.muxerVideo());
    if (producer.muxerVideo() == nullptr) {
        return;
    }
    // ★ 快照里的 extradata 必须是**构造出来的 avcC**（首字节 1），不是裸的 SPS/PPS
    MZ_ASSERT_TRUE(producer.muxerVideo()->extradata != nullptr);
    if (producer.muxerVideo()->extradata == nullptr) {
        return;
    }
    const std::vector<uint8_t> &avcc = *producer.muxerVideo()->extradata;
    MZ_ASSERT_GT(avcc.size(), 11u);
    MZ_ASSERT_EQ(avcc[0], 0x01u);   // configurationVersion
    MZ_ASSERT_EQ(avcc[4], 0xFFu);   // lengthSizeMinusOne = 3（4 字节长度，与转换一致）
    MZ_ASSERT_EQ(avcc[5], 0xE1u);   // 1 个 SPS
    MZ_ASSERT_EQ(avcc[8], 0x67u);   // 嵌入的确实是 SPS

    // ★ 下游（FlvMuxer）必须接受它 —— M6-a 时代这里会返回 AnnexBNotSupported
    FlvMuxer muxer(FlvMuxer::Streams{producer.muxerVideo(), producer.muxerAudio()});
    MZ_ASSERT_EQ(muxer.prepare(), FlvMuxer::Result::Ok);
    MZ_ASSERT_STR_EQ(std::string(FlvMuxer::resultName(FlvMuxer::Result::Ok)), "Ok");
}

MZ_TEST(producer_annexb_packets_are_converted_to_avcc) {
    const std::string path = samplePath("sample.h264");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }
    DemuxerProducer producer;
    MZ_ASSERT_TRUE(producer.open(path));

    int packets = 0;
    int key_frames = 0;
    int64_t last_dts = 0;
    bool has_last = false;
    bool monotonic = true;
    while (packets < 30) {
        MediaPacket::Ptr packet;
        std::string error;
        const SourcePump::ReadResult result = producer.read(&packet, &error);
        MZ_ASSERT_TRUE(result == SourcePump::ReadResult::Packet);
        if (result != SourcePump::ReadResult::Packet || !packet) {
            return;
        }
        int nal_count = 0;
        if (!isAvccShaped(packet->data(), packet->size(), &nal_count)) {
            MZ_FAIL("转出来的包不是合法 AVCC（长度前缀写错了）");
            return;
        }
        MZ_ASSERT_GT(nal_count, 0);
        if (packet->isKeyFrame()) {
            ++key_frames;
        }
        if (has_last && packet->dtsMs() < last_dts) {
            monotonic = false;
        }
        last_dts = packet->dtsMs();
        has_last = true;
        ++packets;
    }
    MZ_ASSERT_TRUE(monotonic);
    MZ_ASSERT_GT(key_frames, 0);                                 // 裸流里必须有 IDR
    MZ_ASSERT_EQ(producer.annexBPackets(), static_cast<uint64_t>(packets)); // 每个视频包都转过
}

MZ_TEST(producer_annexb_without_extradata_is_rejected) {
    // 把裸流里的 SPS/PPS NAL 全删掉 → 解封装要么打不开、要么没有初始化数据。
    // 两种都必须**明确失败**：绝不能用裸 SPS/PPS 冒充 avcC，也不能出一路坏流。
    const std::string src = samplePath("sample.h264");
    MZ_ASSERT_FALSE(src.empty());
    const std::string raw = readWholeFile(src);
    if (raw.empty()) {
        MZ_FAIL("裸流样本为空");
        return;
    }

    std::vector<H264Nal> nals;
    const size_t count = splitAnnexBNals(reinterpret_cast<const uint8_t *>(raw.data()), raw.size(),
                                         &nals);
    MZ_ASSERT_GT(count, 0u);
    if (count == 0) {
        return;
    }
    std::string stripped;
    for (const auto &nal : nals) {
        if (nal.type == 7 || nal.type == 8) {
            continue; // 去掉 SPS / PPS
        }
        const uint8_t start_code[4] = {0x00, 0x00, 0x00, 0x01};
        stripped.append(reinterpret_cast<const char *>(start_code), 4);
        stripped.append(reinterpret_cast<const char *>(nal.data), nal.size);
    }
    MZ_ASSERT_GT(stripped.size(), 0u);
    const std::string path = "/tmp/mzmedia_no_sps_" + std::to_string(::getpid()) + ".h264";
    MZ_ASSERT_TRUE(saveFile(path, stripped));

    DemuxerProducer producer;
    const bool opened = producer.open(path);
    if (!opened) {
        // 分支 1：连流信息都拿不到 → 明确失败并给出原因
        MZ_ASSERT_FALSE(producer.lastError().empty());
    } else {
        // 分支 2：能打开，但初始化数据不可用 → 下游必须**拒绝**而不是出坏流
        MZ_ASSERT_FALSE(producer.annexBVideo()); // 没有 SPS/PPS 可转，不该走转换分支
        FlvMuxer muxer(FlvMuxer::Streams{producer.muxerVideo(), nullptr});
        const FlvMuxer::Result prepared = muxer.prepare();
        MZ_ASSERT_TRUE(prepared == FlvMuxer::Result::MissingExtradata ||
                       prepared == FlvMuxer::Result::NoStreams);
    }
    (void) ::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 循环重开（--loop）
// ---------------------------------------------------------------------------

MZ_TEST(producer_loop_offsets_timestamps_monotonically) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }
    DemuxerProducer::Config config;
    config.loop = true;
    DemuxerProducer producer(config);
    MZ_ASSERT_TRUE(producer.open(path));

    int64_t last_video_dts = 0;
    int64_t last_audio_dts = 0;
    bool has_last_video = false;
    bool has_last_audio = false;
    bool video_monotonic = true;
    bool audio_monotonic = true;
    uint64_t loops_seen = 0;
    int packets = 0;
    int64_t video_before_loop = -1;   // 重开前最后一个视频包的 dts
    int64_t video_after_loop = -1;    // 重开后第一个视频包的 dts
    int64_t audio_before_loop = -1;   // 音频同理：**它才是"撞在同一毫秒"的高危项**
    int64_t audio_after_loop = -1;    // （AAC 一帧真实 23.22ms，整数毫秒只能写 23）
    int packets_after_loop = 0;
    int64_t first_dts_after_loop = -1;

    // 读到"跨过一次循环 + 新一趟又读了 50 个包"为止（上限 1000 包，防止写错时死循环）
    while (packets < 1000) {
        MediaPacket::Ptr packet;
        std::string error;
        const SourcePump::ReadResult result = producer.read(&packet, &error);
        MZ_ASSERT_EQ(result, SourcePump::ReadResult::Packet); // loop=true 时不该出现 Eof
        if (result != SourcePump::ReadResult::Packet) {
            MZ_ASSERT_FALSE(error.empty());
            return;
        }
        MZ_ASSERT_NOT_NULL(packet.get());
        if (!packet) {
            return;
        }
        // 边界识别：重开发生在 read() 内部，所以"loopCount 变大"的那一包就是新一趟的首包
        if (producer.loopCount() > loops_seen) {
            loops_seen = producer.loopCount();
            first_dts_after_loop = packet->dtsMs();
            packets_after_loop = 0;
        } else if (loops_seen > 0) {
            ++packets_after_loop;
        }
        if (packet->kind() == MediaKind::Video) {
            if (has_last_video && packet->dtsMs() < last_video_dts) {
                video_monotonic = false; // 同一条流内回退 = 播放端花屏，绝不能出现
            }
            last_video_dts = packet->dtsMs();
            has_last_video = true;
            if (loops_seen == 0) {
                video_before_loop = packet->dtsMs();
            } else if (video_after_loop < 0) {
                video_after_loop = packet->dtsMs();
            }
        } else {
            if (has_last_audio && packet->dtsMs() < last_audio_dts) {
                audio_monotonic = false;
            }
            last_audio_dts = packet->dtsMs();
            has_last_audio = true;
            if (loops_seen == 0) {
                audio_before_loop = packet->dtsMs();
            } else if (audio_after_loop < 0) {
                audio_after_loop = packet->dtsMs();
            }
        }
        ++packets;
        if (loops_seen >= 1 && packets_after_loop >= 50) {
            break;
        }
    }

    MZ_ASSERT_TRUE(video_monotonic);
    MZ_ASSERT_TRUE(audio_monotonic);
    MZ_ASSERT_EQ(loops_seen, 1u);
    MZ_ASSERT_EQ(producer.loopCount(), 1u);
    MZ_ASSERT_GT(producer.loopOffsetMs(), 0);
    // ★ 跨段处：新一趟的第一个包 dts 必须**严格大于**上一趟的最后一个包 dts（两条流各自成立）。
    //   音频这一条是回退的高危项：一帧真实是 23.22ms，而 FLV 时间戳只能是整数毫秒 ——
    //   偏移量必须算"末帧 + 帧长向上取整 + 1ms 间隙"，否则两趟会撞在同一毫秒上
    //   （ffmpeg 的 null muxer 会报 non monotonically increasing dts，见 flv_http_test.sh §9）。
    MZ_ASSERT_GT(video_before_loop, 0);
    MZ_ASSERT_GT(video_after_loop, video_before_loop);
    MZ_ASSERT_GT(audio_before_loop, 0);
    MZ_ASSERT_GT(audio_after_loop, audio_before_loop);
    // 偏移量应该约等于样本时长（2s 样本；放宽是为了不把 ffmpeg 的时长精度当成被测对象）。
    // 首包可能比偏移量略小：容器的时间轴可以带负的起始偏移（样本音频是 -23ms 起）
    MZ_ASSERT_TRUE(first_dts_after_loop >= producer.loopOffsetMs() - 100);
    MZ_ASSERT_TRUE(producer.loopOffsetMs() >= 1800);
    MZ_ASSERT_TRUE(producer.loopOffsetMs() <= 2600);
    MZ_ASSERT_EQ(producer.loopFailures(), 0u);
    std::printf("    [ INFO ] 循环：%d 包 / 偏移 %lld ms / 重开 %llu 次\n", packets,
                static_cast<long long>(producer.loopOffsetMs()),
                static_cast<unsigned long long>(producer.loopCount()));
}
