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
#include "ffmpeg/aac_util.h"
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

MZ_TEST(producer_bitrate_and_uptime_are_measured) {
    // 【M7 / FR-6.1】`/api/stats` 要暴露"每源码率"与"运行时长"——
    // 数据由 DemuxerProducer 提供，所以语义在这里锁死：
    //   · 没打开 → 0（没有"码率"可言，不猜）
    //   · 打开后 → `openedAtMs() > 0`，且读过包之后 `bitrateBps() > 0`
    //   · 语义是"累计字节 / 已运行时长"的**平均**码率（所以读得比实时快时它会偏高，
    //     1x 推送时它 ≈ 媒体码率）
    DemuxerProducer not_opened;
    MZ_ASSERT_EQ(not_opened.bitrateBps(), 0u);
    MZ_ASSERT_EQ(not_opened.openedAtMs(), 0u);

    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }
    DemuxerProducer producer;
    MZ_ASSERT_TRUE(producer.open(path));
    MZ_ASSERT_GT(producer.openedAtMs(), 0u);

    // 读满 30 个包（样本 2 秒，全速读约几毫秒 → 平均码率会明显高于媒体码率）
    MediaPacket::Ptr packet;
    std::string error;
    for (int i = 0; i < 30; ++i) {
        const SourcePump::ReadResult r = producer.read(&packet, &error);
        MZ_ASSERT_EQ(r, SourcePump::ReadResult::Packet);
        if (r != SourcePump::ReadResult::Packet) {
            return;
        }
    }
    const uint64_t bps = producer.bitrateBps();
    // 注意**不要**断言"这一瞬间算出来是 0"：那是拿"30 个包读完是否 <1ms"当时序前提，
    // 在 TSAN/ASAN 插桩下必然翻车（TSAN 实测就红了）。"elapsed < 1ms → 返回 0"只是为了
    // 不做除零的防御分支，不做时序断言。
    (void) bps;
    // 等 2ms 之后就有了可信的时长基准 → 码率必须为正
    sleepMs(2);
    const uint64_t bps2 = producer.bitrateBps();
    MZ_ASSERT_GT(producer.totalBytes(), 0u);
    MZ_ASSERT_GT(bps2, 0u);
    // 合理性上界：320x240/25fps 的样本不可能到 1 Gbps（这条挡住"时长算错"导致的爆炸值）
    MZ_ASSERT_TRUE(bps2 < 1000000000u);
    std::printf("    [ INFO ] 平均码率 %llu bit/s（累计 %llu 字节 / 已运行 %llu ms）\n",
                static_cast<unsigned long long>(bps2),
                static_cast<unsigned long long>(producer.totalBytes()),
                static_cast<unsigned long long>(getCurrentMillisecond() - producer.openedAtMs()));
}


// ---------------------------------------------------------------------------
// M8 / FR-2.2：MPEG-TS 的 AAC 是 ADTS —— 必须**当场造出 ASC**并逐包剥头
// ---------------------------------------------------------------------------

MZ_TEST(producer_ts_aac_gets_asc_and_strips_adts) {
    // 能证明什么（三件事，缺一不可）：
    //   ① `open()` 之内就把 ASC 造好了（连接拿快照裸指针写 AAC sequence header，等不到首包）；
    //   ② ASC 的字节对得上真实码流（AAC-LC / 44100 / mono → 12 08）；
    //   ③ 每个音频包都被**逐字节**剥掉了 ADTS 头（用裸 Demuxer 的原始包长做对照）。
    // 什么情况会红：
    //   · 只造 ASC 不剥头 → ③ 的 size 比对差 7/9 字节（播放端会听到噪声）；
    //   · 只剥头不造 ASC → ② 为空 / `FlvMuxer::prepare()` 返回 MissingExtradata；
    //   · 把 7 字节写死 → CRC 流（9 字节头）会多带 2 字节；
    //   · 采样率/声道位域解错 → ASC 不等于 12 08。
    const std::string path = samplePath("sample.ts");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }

    // ---- ① 对照组：用裸 Demuxer 取"首个音频包的原始大小 + ADTS 头长"
    size_t raw_size = 0;
    uint8_t header_bytes = 0;
    uint16_t frame_length = 0;
    {
        Demuxer raw;
        MZ_ASSERT_TRUE(raw.open(path));
        const StreamInfo *raw_audio = raw.firstAudio();
        MZ_ASSERT_NOT_NULL(raw_audio);
        if (raw_audio == nullptr) {
            return;
        }
        for (int i = 0; i < 512; ++i) {
            const Demuxer::ReadResult r = raw.readPacket();
            if (r != Demuxer::ReadResult::Packet) {
                break;
            }
            if (raw.packetStreamIndex() != raw_audio->index) {
                continue;
            }
            const AVPacket *pkt = raw.packet();
            AdtsHeader h;
            MZ_ASSERT_TRUE(parseAdtsHeader(pkt->data, static_cast<size_t>(pkt->size), &h));
            raw_size = static_cast<size_t>(pkt->size);
            header_bytes = h.header_bytes;
            frame_length = h.frame_length;
            break;
        }
    }
    MZ_ASSERT_GT(raw_size, 0u);
    MZ_ASSERT_TRUE(header_bytes == 7 || header_bytes == 9);

    // ---- ② ASC 必须已经在快照里
    DemuxerProducer producer;
    MZ_ASSERT_TRUE(producer.open(path));
    MZ_ASSERT_TRUE(producer.adtsAudio());
    const StreamInfo *audio = producer.muxerAudio();
    MZ_ASSERT_NOT_NULL(audio);
    if (audio == nullptr) {
        return;
    }
    MZ_ASSERT_TRUE(audio->extradata != nullptr);
    if (audio->extradata == nullptr) {
        return;
    }
    MZ_ASSERT_EQ(audio->extradata->size(), 2u);
    MZ_ASSERT_EQ((*audio->extradata)[0], 0x12u);   // AAC-LC / 44100
    MZ_ASSERT_EQ((*audio->extradata)[1], 0x08u);   // mono
    // 下游必须接受（M6-a 时代这里会因为缺 ASC 返回 MissingExtradata）
    FlvMuxer muxer(FlvMuxer::Streams{producer.muxerVideo(), producer.muxerAudio()});
    MZ_ASSERT_EQ(muxer.prepare(), FlvMuxer::Result::Ok);
    // 视频侧同时也是 Annex-B → avcC（TS 的视频是 Annex-B）
    MZ_ASSERT_TRUE(producer.annexBVideo());

    // ---- ③ 首个音频包必须**已经被剥掉 ADTS 头**
    MediaPacket::Ptr audio_packet;
    for (int i = 0; i < 512; ++i) {
        MediaPacket::Ptr packet;
        std::string error;
        const SourcePump::ReadResult result = producer.read(&packet, &error);
        MZ_ASSERT_TRUE(result == SourcePump::ReadResult::Packet);
        if (result != SourcePump::ReadResult::Packet || !packet) {
            return;
        }
        if (packet->kind() == MediaKind::Audio) {
            audio_packet = packet;
            break;
        }
    }
    MZ_ASSERT_TRUE(audio_packet != nullptr);
    if (audio_packet == nullptr) {
        return;
    }
    if (raw_size == frame_length) {
        // 一包一帧（本项目样本就是）：可以**逐字节**对账
        MZ_ASSERT_EQ(audio_packet->size(), raw_size - header_bytes);
    } else {
        // 一包多帧：只要求"变小了且还非空"（多帧的精确对账由 aac_util 的单测覆盖）
        MZ_ASSERT_LT(audio_packet->size(), raw_size);
        MZ_ASSERT_GT(audio_packet->size(), 0u);
    }
    MZ_ASSERT_EQ(producer.adtsPackets(), 1u);   // 只剥过这一个音频包
}

MZ_TEST(producer_ts_video_only_still_has_no_audio) {
    // 能证明什么：没有音频轨的 TS **不能**因为"造不出 ASC"而失败（FR-3.3：纯视频也要能出 FLV）。
    // 什么情况会红：把"没有音频"当成"ADTS 探测失败"（那会把纯视频 TS 也拒掉）。
    const std::string path = samplePath("vonly.ts");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }
    DemuxerProducer producer;
    MZ_ASSERT_TRUE(producer.open(path));
    MZ_ASSERT_FALSE(producer.adtsAudio());
    MZ_ASSERT_NULL(producer.muxerAudio());
    MZ_ASSERT_NOT_NULL(producer.muxerVideo());
    FlvMuxer muxer(FlvMuxer::Streams{producer.muxerVideo(), producer.muxerAudio()});
    MZ_ASSERT_EQ(muxer.prepare(), FlvMuxer::Result::Ok);
}

// ---------------------------------------------------------------------------
// 【M10-b】转码：源 → 解码 → 缩放 → 编码 → 输出层看到的仍是"一路 H264"
// ---------------------------------------------------------------------------

MZ_TEST(producer_transcode_downscales_and_rewrites_avcc) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }

    // 直通源：用来做**对照**（转码必须重建 avcC —— 分辨率变了，SPS 里的宽高就变了）
    DemuxerProducer plain;
    MZ_ASSERT_TRUE(plain.open(path));
    MZ_ASSERT_NOT_NULL(plain.muxerVideo());

    DemuxerProducer::Config config;
    config.transcode.enabled = true;
    config.transcode.out_height = 120; // 320x240 → 160x120（等比）
    DemuxerProducer producer(config);
    std::string error;
    MZ_ASSERT_TRUE(producer.open(path));
    MZ_ASSERT_TRUE(producer.transcodeEnabled());
    MZ_ASSERT_FALSE(producer.transcodeInputHevc());
    MZ_ASSERT_NOT_NULL(producer.muxerVideo());
    MZ_ASSERT_NOT_NULL(producer.muxerAudio());
    if (producer.muxerVideo() == nullptr) {
        return;
    }
    // 输出层看到的流形状 = **转码后**的（宽高换了、编码仍是 H264、index 不变）
    MZ_ASSERT_EQ(producer.muxerVideo()->width, 160);
    MZ_ASSERT_EQ(producer.muxerVideo()->height, 120);
    MZ_ASSERT_EQ(producer.muxerVideo()->codec_id, AV_CODEC_ID_H264);
    MZ_ASSERT_TRUE(producer.muxerVideo()->extradata != nullptr);
    MZ_ASSERT_EQ(producer.muxerVideo()->index, plain.muxerVideo()->index);
    if (plain.muxerVideo()->extradata && producer.muxerVideo()->extradata) {
        // avcC 必须**重建**：同尺寸时才会与原样一致，这里缩小了，SPS 一定不同
        MZ_ASSERT_TRUE(*plain.muxerVideo()->extradata != *producer.muxerVideo()->extradata);
        MZ_ASSERT_EQ(producer.muxerVideo()->extradata->at(0), 0x01u); // avcC 的 configurationVersion
    }

    int video = 0;
    int audio = 0;
    int key = 0;
    int64_t last_dts = -1;
    bool monotonic = true;
    int guard = 0;
    while (guard++ < 4000) {
        MediaPacket::Ptr packet;
        const SourcePump::ReadResult result = producer.read(&packet, &error);
        if (result == SourcePump::ReadResult::EndOfStream) {
            break;
        }
        MZ_ASSERT_EQ(result, SourcePump::ReadResult::Packet);
        if (result != SourcePump::ReadResult::Packet) {
            MZ_FAIL("转码读包失败：" + error);
            return;
        }
        MZ_ASSERT_NOT_NULL(packet.get());
        if (!packet) {
            return;
        }
        if (packet->kind() == MediaKind::Video) {
            int nal = 0;
            MZ_ASSERT_TRUE(isAvccShaped(packet->data(), packet->size(), &nal)); // 输出仍是 AVCC
            if (last_dts >= 0 && packet->dtsMs() <= last_dts) {
                monotonic = false; // 转码器自造的 CFR 时间轴必须严格递增
            }
            last_dts = packet->dtsMs();
            if (packet->isKeyFrame()) {
                ++key;
            }
            ++video;
        } else {
            ++audio;
        }
    }
    MZ_ASSERT_TRUE(monotonic);
    MZ_ASSERT_EQ(video, 50);   // 源 50 帧 → 转码后仍 50 帧（一帧不少：flush 兜住了尾部）
    MZ_ASSERT_EQ(audio, 88);   // 音频**原样透传**（SPEC §9 不做音频转码）
    MZ_ASSERT_GT(key, 0);      // 至少开头一个关键帧（否则播放端起不来）
    MZ_ASSERT_EQ(producer.transcodeInputs(), 50u);
    MZ_ASSERT_EQ(producer.transcodeOutputs(), 50u); // flush 之后进出相等
    MZ_ASSERT_GT(producer.transcodeOutMediaMs(), 1800u); // 2 秒样本（50 帧 × 40ms = 2000ms）
    MZ_ASSERT_TRUE(producer.transcodeScaling());
    MZ_ASSERT_FALSE(producer.transcodeVfr());            // 源是 CFR
    MZ_ASSERT_GT(producer.transcodeCostMs(), 0);         // 真的花了时间（不是假的 0）
    MZ_ASSERT_GT(producer.transcodeSpeed(), 0.0);        // 有可判定的实时倍数
    MZ_ASSERT_GT(producer.transcodeElapsedMs(), 0);
    MZ_ASSERT_STR_EQ(producer.lastError(), "");
}

MZ_TEST(producer_transcode_height_is_only_a_cap_by_default) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }
    // 源只有 240p，上限设 720p：**不缩放**（也不报错）——
    // 这是"`?transcode=1` 的意图是给我一路能播的流"，不该因为上限比源高就判成放大而拒绝
    DemuxerProducer::Config config;
    config.transcode.enabled = true;
    config.transcode.max_height = 720;
    DemuxerProducer producer(config);
    MZ_ASSERT_TRUE(producer.open(path));
    MZ_ASSERT_NOT_NULL(producer.muxerVideo());
    if (producer.muxerVideo() == nullptr) {
        return;
    }
    MZ_ASSERT_EQ(producer.muxerVideo()->width, 320);
    MZ_ASSERT_EQ(producer.muxerVideo()->height, 240);
    MZ_ASSERT_FALSE(producer.transcodeScaling()); // 同尺寸同像素格式 → 不建 sws
}

MZ_TEST(producer_transcode_rejects_bad_requests) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }
    // 奇数高度：yuv420p 要求偶数。**不**"悄悄取偶"—— 参数写错了要让用户知道
    {
        DemuxerProducer::Config config;
        config.transcode.enabled = true;
        config.transcode.out_height = 121;
        DemuxerProducer producer(config);
        MZ_ASSERT_FALSE(producer.open(path));
        MZ_ASSERT_TRUE(producer.lastError().find("偶数") != std::string::npos);
        MZ_ASSERT_FALSE(producer.transcodeEnabled());
    }
    // 放大：明确拒绝（浪费 CPU 又不会更清楚）
    {
        DemuxerProducer::Config config;
        config.transcode.enabled = true;
        config.transcode.out_height = 480;
        DemuxerProducer producer(config);
        MZ_ASSERT_FALSE(producer.open(path));
        MZ_ASSERT_TRUE(producer.lastError().find("不放大") != std::string::npos);
    }
    // 只有音频的源：转码只指视频，音频不转 → 明确拒绝（不产出"没画面"的流）
    {
        const std::string audio_only = samplePath("sample_audio.aac");
        MZ_ASSERT_FALSE(audio_only.empty());
        if (!audio_only.empty()) {
            DemuxerProducer::Config config;
            config.transcode.enabled = true;
            config.transcode.max_height = 120;
            DemuxerProducer producer(config);
            MZ_ASSERT_FALSE(producer.open(audio_only));
            MZ_ASSERT_TRUE(producer.lastError().find("只有音频") != std::string::npos);
        }
    }
    // 关掉转码时：同样的源必须**照常**打开（默认行为不能被这批改动影响）
    {
        DemuxerProducer producer;
        MZ_ASSERT_TRUE(producer.open(path));
        MZ_ASSERT_FALSE(producer.transcodeEnabled());
        MZ_ASSERT_EQ(producer.transcodeInputs(), 0u);
    }
}

MZ_TEST(producer_transcode_hevc_becomes_h264) {
    const std::string path = samplePath("sample_hevc.mp4");
    if (path.empty()) {
        // HEVC 样本要靠 libx265 现场生成；本机没有 libx265 时**明确打出跳过原因**
        // （不是静默 pass：`make_samples.sh` 也会打一行 "跳过 sample_hevc.mp4"）
        std::printf("    [跳过] samples/sample_hevc.mp4 不存在（本机 FFmpeg 缺 libx265）\n");
        return;
    }
    // 先确认"不转码就进不了 FLV"：编码是 HEVC（输出层会在 FlvSender 那里 415）
    {
        DemuxerProducer plain;
        MZ_ASSERT_TRUE(plain.open(path));
        MZ_ASSERT_NOT_NULL(plain.muxerVideo());
        if (plain.muxerVideo() != nullptr) {
            MZ_ASSERT_EQ(plain.muxerVideo()->codec_id, AV_CODEC_ID_HEVC);
        }
    }
    // 转码后：编解码器换成 H264，宽高按上限缩
    DemuxerProducer::Config config;
    config.transcode.enabled = true;
    config.transcode.max_height = 120; // 源 240p → 缩到 120p
    DemuxerProducer producer(config);
    std::string error;
    MZ_ASSERT_TRUE(producer.open(path));
    MZ_ASSERT_TRUE(producer.transcodeEnabled());
    MZ_ASSERT_TRUE(producer.transcodeInputHevc()); // 输入是 HEVC（诊断字段）
    MZ_ASSERT_NOT_NULL(producer.muxerVideo());
    if (producer.muxerVideo() == nullptr) {
        return;
    }
    MZ_ASSERT_EQ(producer.muxerVideo()->codec_id, AV_CODEC_ID_H264); // 输出是 H264
    MZ_ASSERT_EQ(producer.muxerVideo()->width, 160);
    MZ_ASSERT_EQ(producer.muxerVideo()->height, 120);

    int video = 0;
    int guard = 0;
    int64_t last_dts = -1;
    bool monotonic = true;
    while (guard++ < 4000) {
        MediaPacket::Ptr packet;
        const SourcePump::ReadResult result = producer.read(&packet, &error);
        if (result == SourcePump::ReadResult::EndOfStream) {
            break;
        }
        MZ_ASSERT_EQ(result, SourcePump::ReadResult::Packet);
        if (result != SourcePump::ReadResult::Packet) {
            MZ_FAIL("HEVC 转码读包失败：" + error);
            return;
        }
        if (packet && packet->kind() == MediaKind::Video) {
            int nal = 0;
            MZ_ASSERT_TRUE(isAvccShaped(packet->data(), packet->size(), &nal));
            if (last_dts >= 0 && packet->dtsMs() <= last_dts) {
                monotonic = false;
            }
            last_dts = packet->dtsMs();
            ++video;
        }
    }
    MZ_ASSERT_TRUE(monotonic);
    MZ_ASSERT_GT(video, 40); // 2 秒 @25fps ≈ 50 帧（允许少量解码/编码边界差异）
    MZ_ASSERT_EQ(producer.transcodeInputs(), producer.transcodeOutputs());
}

MZ_TEST(producer_transcode_loop_keeps_audio_and_video_aligned) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }
    // 循环 + 转码：**视频走"帧序号 × 步长"、音频走"原始 dts + 趟偏移"**，两条时间轴必须在
    // 接缝处对齐。曾经的写法把趟偏移**又**加了一次（转码时间轴本来就连续）→ 实测视频跑到
    // 14.0s、音频才 8.1s，音画完全错位；本用例锁的就是"跨过接缝后两条轴仍贴在一起"。
    DemuxerProducer::Config config;
    config.loop = true;
    config.transcode.enabled = true;
    config.transcode.out_height = 120;
    DemuxerProducer producer(config);
    std::string error;
    MZ_ASSERT_TRUE(producer.open(path));
    MZ_ASSERT_TRUE(producer.transcodeEnabled());

    int64_t last_video = -1;
    int64_t last_audio = -1;
    int64_t max_gap = 0;
    int guard = 0;
    while (guard++ < 400) {
        MediaPacket::Ptr packet;
        const SourcePump::ReadResult result = producer.read(&packet, &error);
        MZ_ASSERT_EQ(result, SourcePump::ReadResult::Packet); // 循环模式不该 Eof
        if (result != SourcePump::ReadResult::Packet) {
            MZ_FAIL("循环转码读包失败：" + error);
            return;
        }
        if (!packet) {
            return;
        }
        if (packet->kind() == MediaKind::Video) {
            last_video = packet->dtsMs();
        } else {
            last_audio = packet->dtsMs();
        }
        if (last_video >= 0 && last_audio >= 0) {
            const int64_t gap = last_video > last_audio ? last_video - last_audio
                                                        : last_audio - last_video;
            if (gap > max_gap) {
                max_gap = gap;
            }
        }
        // 跨过第一趟（2 秒样本）就够证明接缝对齐；再读下去只会拖长用例
        if (last_video > 5000) {
            break;
        }
    }
    MZ_ASSERT_GT(producer.loopCount(), 0u);   // 确实跨了接缝（否则这条什么都没证明）
    MZ_ASSERT_GT(last_video, 5000);           // 真的读到第二趟
    // 300ms 的界：正常只有"解码器差一两帧"的延迟（≈80ms）；
    // 双计趟偏移的 bug 会让 gap 直接跳到 ~2000ms（一趟的长度），所以这个界既能过也能红
    MZ_ASSERT_LT(max_gap, 300);
}

// ---------------------------------------------------------------------------
// 【v0.2.1】B 帧源：pts 必须原样透传，循环接缝不许倒退
// ---------------------------------------------------------------------------

MZ_TEST(producer_bframes_pts_pass_through_unclamped) {
    const std::string path = samplePath("bf.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }
    DemuxerProducer producer;
    MZ_ASSERT_TRUE(producer.open(path));
    int video = 0;
    int64_t last_dts = 0;
    bool dts_monotonic = true;
    bool first = true;
    bool saw_pts_regression = false;
    int64_t last_pts = 0;
    int guard = 0;
    while (guard++ < 500) {
        MediaPacket::Ptr packet;
        std::string error;
        const SourcePump::ReadResult r = producer.read(&packet, &error);
        if (r == SourcePump::ReadResult::EndOfStream) {
            break;
        }
        MZ_ASSERT_EQ(r, SourcePump::ReadResult::Packet);
        if (r != SourcePump::ReadResult::Packet) {
            MZ_FAIL("读包失败：" + error);
            return;
        }
        if (packet->kind() != MediaKind::Video) {
            continue;
        }
        if (!first && packet->dtsMs() < last_dts) {
            dts_monotonic = false;
        }
        if (!first && packet->ptsMs() < last_pts) {
            saw_pts_regression = true; // B 帧的正常现象：生产者**不许**把它抹平
        }
        last_dts = packet->dtsMs();
        last_pts = packet->ptsMs();
        first = false;
        ++video;
    }
    MZ_ASSERT_EQ(video, 25);
    MZ_ASSERT_TRUE(dts_monotonic);        // dts 仍单调
    MZ_ASSERT_TRUE(saw_pts_regression);   // ★ pts 回退被保留（钳制会让它消失）
    MZ_ASSERT_GT(producer.ptsOutOfOrder(), 0u);
    MZ_ASSERT_EQ(producer.dtsClamped(), 0u);
}

MZ_TEST(producer_loop_with_negative_first_dts_does_not_regress) {
    // 【v0.2.1 修的真 bug】`bf.mp4` 的首包 dts 是 **-80ms**。循环重开时旧公式把趟偏移算成
    // "末包 + 帧长"（= 1001ms），而下一趟的首包是 -80 + 1001 = 921ms < 上一趟末包 960ms
    // → **每个接缝 dts 倒退 39ms**（FLV/TS 两条路径都抖）。修法：偏移改用 `pass_end - pass_start`。
    const std::string path = samplePath("bf.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }
    DemuxerProducer::Config config;
    config.loop = true;
    DemuxerProducer producer(config);
    MZ_ASSERT_TRUE(producer.open(path));
    int64_t last_dts = 0;
    bool monotonic = true;
    int count = 0;
    int guard = 0;
    // 读够两趟半（25 帧/趟 @1s），确保跨过至少两个接缝
    while (guard++ < 500 && count < 60) {
        MediaPacket::Ptr packet;
        std::string error;
        const SourcePump::ReadResult r = producer.read(&packet, &error);
        MZ_ASSERT_EQ(r, SourcePump::ReadResult::Packet); // 循环模式不该 Eof
        if (r != SourcePump::ReadResult::Packet) {
            MZ_FAIL("循环读包失败：" + error);
            return;
        }
        if (packet->kind() != MediaKind::Video) {
            continue;
        }
        if (count > 0 && packet->dtsMs() <= last_dts) {
            monotonic = false; // 倒退**或重复**都算失败（重复会让播放器卡住）
        }
        last_dts = packet->dtsMs();
        ++count;
    }
    MZ_ASSERT_GT(producer.loopCount(), 0u);   // 确实跨了接缝
    MZ_ASSERT_GT(count, 50);                  // 读到了第二趟之后
    MZ_ASSERT_TRUE(monotonic);
}
