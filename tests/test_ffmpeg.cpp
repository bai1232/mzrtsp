/*
 * FFmpeg 封装单元测试（M4-a，分组 `ffmpeg`）
 * ============================================================================
 * 覆盖维度（AI_COLLAB §3.4：正常 / 空 / 满 / 断开 / 超大）：
 *   正常  打开 MP4 并读出流参数、读完所有包到 Eof、时间戳单调
 *   空    未打开就读（必须是 Error，**不能是 Eof**）、空样本路径
 *   满    流数上限、单包上限（都要拒绝 + 计数）
 *   断开  文件不存在 / 畸形字节（不崩、返回 false + 原因）
 *   超大  单包超过 8MB 上限、时间戳换算溢出
 *
 * 样本由 `scripts/make_samples.sh` 现场生成（不依赖外网）；找不到样本时用例**明确失败**
 * （绝不静默跳过 —— 那会让"没测到"看起来像"测过了"）。
 * ============================================================================
 */

#include "test_main.h"

#include "core/util.h"
#include "ffmpeg/codec_matrix.h"
#include "ffmpeg/demuxer.h"
#include "ffmpeg/h264_util.h"
#include "ffmpeg/time_base.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using namespace mzmedia;

namespace {

/// 找样本：**与当前工作目录无关**（顺序：$MZ_SAMPLES_DIR → 从当前目录向上最多 8 层）。
/// 为什么向上搜索：用例的 cwd 可能是仓库根 / build / build/tests（ctest，深 2 级），
/// 也可能被别的用例临时改到仓库内任意位置 —— 硬编码固定的相对层级是不稳的
/// （真踩过，见 `docs/RETROSPECTIVE.md` §2.4）。层数故意给得宽松，不测边界值。
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
    std::printf("    [ INFO ] 找不到样本 %s（试过 %zu 个路径，请先跑 scripts/make_samples.sh）\n",
                name.c_str(), candidates.size());
    return {};
}

/// 读整个文件（用例内部用，失败返回空串）
std::string readWholeFile(const std::string &path) {
    FILE *f = ::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return {};
    }
    std::string out;
    char buf[8192];
    size_t n = 0;
    while ((n = ::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    ::fclose(f);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// 时间基（纯整数逻辑，不需要样本、不需要 libav*）
// ---------------------------------------------------------------------------

MZ_TEST(ffmpeg_timebase_rescale_basic) {
    const TimeBase ms{1, 1000};
    const TimeBase tb90k{1, 90000};
    const TimeBase tb25{1, 25};
    int64_t out = -1;

    MZ_ASSERT_TRUE(rescaleTimestamp(1000, ms, tb90k, &out));   // 1 秒
    MZ_ASSERT_EQ(out, 90000);
    MZ_ASSERT_TRUE(rescaleTimestamp(90000, tb90k, ms, &out));   // 回到毫秒
    MZ_ASSERT_EQ(out, 1000);
    MZ_ASSERT_TRUE(rescaleTimestamp(25, tb25, ms, &out));       // 25 帧 @25fps = 1 秒
    MZ_ASSERT_EQ(out, 1000);
    MZ_ASSERT_TRUE(rescaleTimestamp(-1000, ms, tb90k, &out));   // 负数（B 帧时间戳）
    MZ_ASSERT_EQ(out, -90000);
}

MZ_TEST(ffmpeg_timebase_rescale_invalid_and_overflow) {
    const TimeBase ms{1, 1000};
    const TimeBase bad_zero{0, 1000};
    const TimeBase bad_neg{1, -1000};
    const TimeBase huge{1000000, 1};
    int64_t out = 0;

    MZ_ASSERT_FALSE(rescaleTimestamp(0, bad_zero, ms, &out));   // 时间基非法
    MZ_ASSERT_FALSE(rescaleTimestamp(0, ms, bad_neg, &out));
    MZ_ASSERT_FALSE(rescaleTimestamp(0, ms, ms, nullptr));      // 空输出
    // 溢出：INT64_MAX 毫秒 → 微秒 就应该判失败（而不是给一个错值）
    MZ_ASSERT_FALSE(rescaleTimestamp(INT64_MAX, ms, TimeBase{1, 1000000}, &out));
    MZ_ASSERT_FALSE(rescaleTimestamp(INT64_MAX, huge, ms, &out));
}

MZ_TEST(ffmpeg_monotonic_guard_clamps_and_counts) {
    MonotonicGuard guard;
    MZ_ASSERT_EQ(guard.apply(100), 100);
    MZ_ASSERT_EQ(guard.monotonicClampCount(), 0u);
    MZ_ASSERT_EQ(guard.apply(90), 100);      // 回退 → 钳到上一个值
    MZ_ASSERT_EQ(guard.monotonicClampCount(), 1u);
    MZ_ASSERT_EQ(guard.apply(120), 120);
    MZ_ASSERT_EQ(guard.apply(120), 120);     // 相等不算回退
    MZ_ASSERT_EQ(guard.monotonicClampCount(), 1u);
    guard.reset();
    MZ_ASSERT_EQ(guard.monotonicClampCount(), 0u);
    MZ_ASSERT_EQ(guard.apply(5), 5);
}

// ---------------------------------------------------------------------------
// Demuxer：真实样本
// ---------------------------------------------------------------------------

MZ_TEST(ffmpeg_demux_open_mp4_reports_streams) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        MZ_FAIL("找不到样本，请先跑 scripts/make_samples.sh");
        return;   // ★ 断言不会中断执行：必须自己 return，否则后面解引用空指针会段错误
    }
    Demuxer demuxer;
    if (!demuxer.open(path)) {
        MZ_FAIL("打开样本失败");
        return;
    }
    MZ_ASSERT_TRUE(demuxer.opened());
    MZ_ASSERT_TRUE(demuxer.lastError().empty());
    MZ_ASSERT_GE(demuxer.streams().size(), 2u);   // 样本是 h264 + aac

    const StreamInfo *video = demuxer.firstVideo();
    MZ_ASSERT_NOT_NULL(video);
    if (video == nullptr) {
        MZ_FAIL("样本里没有视频流");
        return;
    }
    MZ_ASSERT_EQ(static_cast<int>(video->codec_id), static_cast<int>(AV_CODEC_ID_H264));
    MZ_ASSERT_STR_EQ(video->codec_name, std::string("h264"));
    MZ_ASSERT_EQ(video->width, 320);
    MZ_ASSERT_EQ(video->height, 240);
    MZ_ASSERT_GT(video->time_base.num, 0);      // 时基必须有效（0 会让换算全废）
    MZ_ASSERT_GT(video->time_base.den, 0);
    // 帧率 25/1（样本用 rate=25 生成）
    MZ_ASSERT_EQ(video->frame_rate.num * 1, video->frame_rate.den * 25);

    const StreamInfo *audio = demuxer.firstAudio();
    MZ_ASSERT_NOT_NULL(audio);
    if (audio == nullptr) {
        MZ_FAIL("样本里没有音频流");
        return;
    }
    MZ_ASSERT_EQ(static_cast<int>(audio->codec_id), static_cast<int>(AV_CODEC_ID_AAC));
    MZ_ASSERT_GT(audio->sample_rate, 0);
}

MZ_TEST(ffmpeg_demux_open_missing_file_fails_loudly) {
    Demuxer demuxer;
    MZ_ASSERT_FALSE(demuxer.open("/nonexistent/definitely/not/here.mp4"));
    MZ_ASSERT_FALSE(demuxer.opened());
    MZ_ASSERT_FALSE(demuxer.lastError().empty());   // 必须给出原因，不能静默
}

MZ_TEST(ffmpeg_demux_open_garbage_file_fails) {
    const std::string path = samplePath("garbage.bin");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }
    Demuxer demuxer;
    MZ_ASSERT_FALSE(demuxer.open(path));   // 随机字节：打不开或拿不到流信息
    MZ_ASSERT_FALSE(demuxer.lastError().empty());
}

MZ_TEST(ffmpeg_demux_unopened_read_is_error_not_eof) {
    // ★ EOF 与 Error 必须分开：把"未打开"当成 Eof 会静默提前结束
    Demuxer demuxer;
    MZ_ASSERT_EQ(static_cast<int>(demuxer.readPacket()),
                 static_cast<int>(Demuxer::ReadResult::Error));
    MZ_ASSERT_FALSE(demuxer.lastError().empty());
}

MZ_TEST(ffmpeg_demux_read_all_packets_until_eof) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }
    Demuxer demuxer;
    if (!demuxer.open(path)) {
        MZ_FAIL("打开样本失败");
        return;
    }

    uint64_t packets = 0;
    for (;;) {
        const Demuxer::ReadResult r = demuxer.readPacket();
        if (r == Demuxer::ReadResult::Eof) {
            break;
        }
        MZ_ASSERT_EQ(static_cast<int>(r), static_cast<int>(Demuxer::ReadResult::Packet));
        MZ_ASSERT_NOT_NULL(demuxer.packet());
        if (demuxer.packet() == nullptr) {
            MZ_FAIL("Packet 结果却拿到空包");
            return;
        }
        MZ_ASSERT_GT(demuxer.packet()->size, 0);
        ++packets;
    }
    std::printf("    [ INFO ] 读完样本：%llu 包 / %llu 字节\n",
                static_cast<unsigned long long>(packets),
                static_cast<unsigned long long>(demuxer.totalBytes()));
    MZ_ASSERT_GT(packets, 0u);
    MZ_ASSERT_EQ(demuxer.totalPackets(), packets);
    MZ_ASSERT_GT(demuxer.totalBytes(), 0u);
    MZ_ASSERT_EQ(demuxer.rejectedPackets(), 0u);
    MZ_ASSERT_EQ(demuxer.rescaleFailures(), 0u);
    // 到 Eof 之后再读：仍是 Eof（稳定终态），不是 Error
    MZ_ASSERT_EQ(static_cast<int>(demuxer.readPacket()),
                 static_cast<int>(Demuxer::ReadResult::Eof));
}

MZ_TEST(ffmpeg_demux_timestamps_monotonic_per_stream) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }
    Demuxer demuxer;
    if (!demuxer.open(path)) {
        MZ_FAIL("打开样本失败");
        return;
    }

    std::map<int, int64_t> last_pts;
    std::map<int, int64_t> last_dts;
    uint64_t checked = 0;
    for (;;) {
        const Demuxer::ReadResult r = demuxer.readPacket();
        if (r == Demuxer::ReadResult::Eof) {
            break;
        }
        MZ_ASSERT_EQ(static_cast<int>(r), static_cast<int>(Demuxer::ReadResult::Packet));
        int64_t pts = 0;
        int64_t dts = 0;
        MZ_ASSERT_TRUE(demuxer.packetTimestampsMs(&pts, &dts));
        const int idx = demuxer.packetStreamIndex();
        if (last_pts.count(idx) != 0) {
            // 同一路流内：时间戳不得回退（守卫保证；这里验它真的生效）
            MZ_ASSERT_GE(pts, last_pts[idx]);
            MZ_ASSERT_GE(dts, last_dts[idx]);
        }
        last_pts[idx] = pts;
        last_dts[idx] = dts;
        ++checked;
    }
    MZ_ASSERT_GT(checked, 0u);
    MZ_ASSERT_EQ(demuxer.monotonicClamped(), 0u);   // 干净样本不该发生回退
}

MZ_TEST(ffmpeg_demux_max_streams_limit_rejects) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    Demuxer::Limits limits;
    limits.max_streams = 0;   // 样本至少有 2 路 → 必然超限
    Demuxer demuxer(limits);
    MZ_ASSERT_FALSE(demuxer.open(path));
    MZ_ASSERT_TRUE(demuxer.lastError().find("超过上限") != std::string::npos);
}

MZ_TEST(ffmpeg_demux_max_packet_size_limit_rejects) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    Demuxer::Limits limits;
    limits.max_packet_size = 1;   // 任何真实包都超限
    Demuxer demuxer(limits);
    if (!demuxer.open(path)) {
        MZ_FAIL("打开样本失败");
        return;
    }
    MZ_ASSERT_EQ(static_cast<int>(demuxer.readPacket()),
                 static_cast<int>(Demuxer::ReadResult::Error));
    MZ_ASSERT_EQ(demuxer.rejectedPackets(), 1u);
    MZ_ASSERT_TRUE(demuxer.lastError().find("上限") != std::string::npos);
}

// ---------------------------------------------------------------------------
// M4-b：CodecMatrix 判定 + H264 SPS/PPS
// ---------------------------------------------------------------------------

MZ_TEST(ffmpeg_codec_matrix_flv_h264_aac_is_remux) {
    // 正常：MP4/H264 + AAC → FLV 可以只转封装
    MZ_ASSERT_EQ(static_cast<int>(decideOutput(AV_CODEC_ID_H264, AV_CODEC_ID_AAC, "flv")),
                 static_cast<int>(CopyOrTranscode::Remux));
    MZ_ASSERT_STR_EQ(copyOrTranscodeName(CopyOrTranscode::Remux), std::string("remux"));
}

MZ_TEST(ffmpeg_codec_matrix_video_only_and_audio_only) {
    // 边界：只有视频 / 只有音频都合法
    MZ_ASSERT_EQ(static_cast<int>(decideOutput(AV_CODEC_ID_H264, AV_CODEC_ID_NONE, "flv")),
                 static_cast<int>(CopyOrTranscode::Remux));
    MZ_ASSERT_EQ(static_cast<int>(decideOutput(AV_CODEC_ID_NONE, AV_CODEC_ID_AAC, "flv")),
                 static_cast<int>(CopyOrTranscode::Remux));
}

MZ_TEST(ffmpeg_codec_matrix_unsupported_and_transcode) {
    // HEVC → FLV：明确不支持（不是"能转码"）
    MZ_ASSERT_EQ(static_cast<int>(decideOutput(AV_CODEC_ID_HEVC, AV_CODEC_ID_AAC, "flv")),
                 static_cast<int>(CopyOrTranscode::Unsupported));
    // 未知输出格式：明确不支持（不当作 flv 处理）
    MZ_ASSERT_EQ(static_cast<int>(decideOutput(AV_CODEC_ID_H264, AV_CODEC_ID_AAC, "hls")),
                 static_cast<int>(CopyOrTranscode::Unsupported));
    // MPEG2 视频 / MP3 音频 → 需要转码（v0.2）
    MZ_ASSERT_EQ(static_cast<int>(decideOutput(AV_CODEC_ID_MPEG2VIDEO, AV_CODEC_ID_AAC, "flv")),
                 static_cast<int>(CopyOrTranscode::Transcode));
    MZ_ASSERT_EQ(static_cast<int>(decideOutput(AV_CODEC_ID_H264, AV_CODEC_ID_MP3, "flv")),
                 static_cast<int>(CopyOrTranscode::Transcode));
}

MZ_TEST(ffmpeg_h264_split_annexb_nals) {
    // 手工构造 Annex-B：4 字节起始码 + SPS(0x67) + 3 字节起始码 + PPS(0x68) + IDR(0x65)
    const uint8_t data[] = {0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1e,
                            0, 0, 1, 0x68, 0xce, 0x3c, 0x80,
                            0, 0, 1, 0x65, 0x88, 0x84};
    std::vector<H264Nal> nals;
    MZ_ASSERT_EQ(splitAnnexBNals(data, sizeof(data), &nals), 3u);
    MZ_ASSERT_EQ(nals[0].type, 7u);
    MZ_ASSERT_EQ(nals[1].type, 8u);
    MZ_ASSERT_EQ(nals[2].type, 5u);
    MZ_ASSERT_EQ(nals[0].size, 4u);   // 0x67 0x42 0x00 0x1e

    // 没有起始码 → 0 个（不猜）
    const uint8_t none[] = {0x67, 0x42, 0x00, 0x1e};
    MZ_ASSERT_EQ(splitAnnexBNals(none, sizeof(none), &nals), 0u);
    MZ_ASSERT_EQ(splitAnnexBNals(nullptr, 0, &nals), 0u);
}

MZ_TEST(ffmpeg_h264_extract_requires_both_sps_and_pps) {
    // 只有 SPS、没有 PPS → 必须失败（调用方不能假设 PPS 存在）
    const uint8_t only_sps[] = {0, 0, 1, 0x67, 0x42, 0x00, 0x1e};
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    MZ_ASSERT_FALSE(extractSpsPps(only_sps, sizeof(only_sps), &sps, &pps));
    MZ_ASSERT_FALSE(extractSpsPps(nullptr, 0, &sps, &pps));
}

MZ_TEST(ffmpeg_h264_extract_from_raw_sample) {
    const std::string path = samplePath("sample.h264");
    MZ_ASSERT_FALSE(path.empty());
    const std::string raw = readWholeFile(path);
    MZ_ASSERT_GT(raw.size(), 0u);
    if (raw.empty()) {
        return;
    }
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    MZ_ASSERT_TRUE(extractSpsPps(reinterpret_cast<const uint8_t *>(raw.data()), raw.size(), &sps, &pps));
    MZ_ASSERT_GT(sps.size(), 0u);
    MZ_ASSERT_GT(pps.size(), 0u);
    MZ_ASSERT_EQ(static_cast<int>(sps[0] & 0x1F), 7);   // NAL header 必须真的是 SPS
    MZ_ASSERT_EQ(static_cast<int>(pps[0] & 0x1F), 8);
    std::printf("    [ INFO ] 裸流提取：SPS %zu 字节 / PPS %zu 字节\n", sps.size(), pps.size());
}

MZ_TEST(ffmpeg_h264_sps_dimension_matches_demuxer) {
    // ★ M4 的验收之一：裸流提 SPS/PPS 并**报出分辨率**，且与解封装得到的一致
    const std::string path = samplePath("sample.h264");
    MZ_ASSERT_FALSE(path.empty());
    const std::string raw = readWholeFile(path);
    if (raw.empty()) {
        MZ_FAIL("裸流样本为空");
        return;
    }
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    if (!extractSpsPps(reinterpret_cast<const uint8_t *>(raw.data()), raw.size(), &sps, &pps)) {
        MZ_FAIL("SPS/PPS 提取失败");
        return;
    }
    int width = 0;
    int height = 0;
    MZ_ASSERT_TRUE(parseSpsDimension(sps.data(), sps.size(), &width, &height));
    MZ_ASSERT_EQ(width, 320);
    MZ_ASSERT_EQ(height, 240);

    // 交叉验证：解封装同一份裸流得到的分辨率必须一致（两套独立路径互相印证）
    Demuxer demuxer;
    if (!demuxer.open(path)) {
        MZ_FAIL("打不开裸流样本");
        return;
    }
    const StreamInfo *video = demuxer.firstVideo();
    MZ_ASSERT_NOT_NULL(video);
    if (video == nullptr) {
        return;
    }
    MZ_ASSERT_EQ(video->width, width);
    MZ_ASSERT_EQ(video->height, height);
}

MZ_TEST(ffmpeg_h264_sps_unparsable_fails) {
    // 畸形：解析不了的数据必须失败（不返回假分辨率）
    // 注意：SPS 没有校验和 —— "0xff 填充"这类字节可能凑出语法合法结果（16x16），
    //       那种情况靠调用方**交叉校验**（见 ffmpeg_h264_sps_dimension_matches_demuxer）。
    //       这里用"真解析不了"的数据：长串 0（exp-Golomb 前导零越界）、截断、空指针。
    const uint8_t zeros[] = {0x67, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    int w = 0;
    int h = 0;
    MZ_ASSERT_FALSE(parseSpsDimension(zeros, sizeof(zeros), &w, &h));
    MZ_ASSERT_FALSE(parseSpsDimension(nullptr, 0, &w, &h));
    const uint8_t too_short[] = {0x67, 0x42};
    MZ_ASSERT_FALSE(parseSpsDimension(too_short, sizeof(too_short), &w, &h));
    // 空指针输出也要挡住
    const uint8_t ok[] = {0x67, 0x42, 0x00, 0x1e, 0xab, 0x40, 0xb0, 0x4b};
    MZ_ASSERT_FALSE(parseSpsDimension(ok, sizeof(ok), nullptr, &h));
    MZ_ASSERT_FALSE(parseSpsDimension(ok, sizeof(ok), &w, nullptr));
}

MZ_TEST(ffmpeg_samples_found_from_nested_cwd) {
    // 回归（`docs/RETROSPECTIVE.md` §2.4）：样本查找必须容忍"工作目录在仓库内更深的位置"。
    // 修前只硬编码了 2 级相对路径（`samples/` / `../samples/` / `../../samples/`）；
    // 本用例进到"样本目录下的第 2 层"，需要向上 3 级 —— 修前必红。
    const std::string found = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(found.empty());
    if (found.empty()) {
        return;   // 断言不中断执行，必须显式早退
    }
    // 以"实际找到的路径"为基准，保证与从哪个目录启动无关（仓库根 / build / build/tests）
    const std::string samplesDir = found.substr(0, found.size() - std::string("sample.mp4").size());
    const std::string probe = samplesDir + "mz_cwd_probe/deep";
    MZ_ASSERT_TRUE(createDirectory(probe));
    {
        const ::mztest::ScopedCwd cwd(probe.c_str());
        MZ_ASSERT_TRUE(cwd.ok);
        if (!cwd.ok) {
            return;
        }
        MZ_ASSERT_FALSE(samplePath("sample.mp4").empty());
    }
    (void) ::system(("rm -rf " + probe).c_str());
}
