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
#include "ffmpeg/aac_util.h"
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

// ---------------------------------------------------------------------------
// M6-c：Annex-B → AVCC + avcC 构造（纯函数；样本相关的两例在最后）
// ---------------------------------------------------------------------------

MZ_TEST(ffmpeg_avcc_looks_like_annexb) {
    const uint8_t sc3[] = {0x00, 0x00, 0x01, 0x65};
    const uint8_t sc4[] = {0x00, 0x00, 0x00, 0x01, 0x65};
    const uint8_t avcc[] = {0x01, 0x42, 0xC0, 0x1E, 0xFF};  // avcC 首字节固定为 1
    MZ_ASSERT_TRUE(looksLikeAnnexB(sc3, sizeof(sc3)));
    MZ_ASSERT_TRUE(looksLikeAnnexB(sc4, sizeof(sc4)));
    MZ_ASSERT_FALSE(looksLikeAnnexB(avcc, sizeof(avcc)));
    MZ_ASSERT_FALSE(looksLikeAnnexB(nullptr, 0));
    MZ_ASSERT_FALSE(looksLikeAnnexB(sc3, 2));   // 太短：不足以判断
    const uint8_t zeros[] = {0x00, 0x00, 0x00, 0x00};
    MZ_ASSERT_FALSE(looksLikeAnnexB(zeros, sizeof(zeros))); // 全 0 不是起始码
}

MZ_TEST(ffmpeg_avcc_convert_known_bytes) {
    // 输入：00 00 00 01 [65 AA BB] 00 00 01 [41 CC]  → 两个 NAL（3 字节 + 2 字节）
    const uint8_t annexb[] = {0x00, 0x00, 0x00, 0x01, 0x65, 0xAA, 0xBB,
                              0x00, 0x00, 0x01, 0x41, 0xCC};
    const uint8_t expect[] = {0x00, 0x00, 0x00, 0x03, 0x65, 0xAA, 0xBB,
                              0x00, 0x00, 0x00, 0x02, 0x41, 0xCC};
    std::vector<uint8_t> out;
    MZ_ASSERT_TRUE(annexBToAvcc(annexb, sizeof(annexb), &out));
    MZ_ASSERT_EQ(out.size(), sizeof(expect));
    if (out.size() == sizeof(expect)) {
        MZ_ASSERT_EQ(::memcmp(out.data(), expect, sizeof(expect)), 0);
    }
    // 尾部多余的一个 0 字节（4 字节起始码的残留）要被吃掉，不能算进 NAL
    const uint8_t trailing[] = {0x00, 0x00, 0x01, 0x41, 0xCC, 0x00};
    MZ_ASSERT_TRUE(annexBToAvcc(trailing, sizeof(trailing), &out));
    MZ_ASSERT_EQ(out.size(), 6u);          // 4 字节长度 + 2 字节 NAL
    MZ_ASSERT_EQ(out[3], 0x02u);
}

MZ_TEST(ffmpeg_avcc_convert_rejects_bad_input) {
    std::vector<uint8_t> out;
    // 没有起始码：**明确失败**，绝不能"原样拷一份冒充成功"（那正是花屏的成因）
    const uint8_t no_start[] = {0x65, 0xAA, 0xBB, 0x41};
    MZ_ASSERT_FALSE(annexBToAvcc(no_start, sizeof(no_start), &out));
    MZ_ASSERT_EQ(out.size(), 0u);          // 失败时输出必须是空的
    MZ_ASSERT_FALSE(annexBToAvcc(nullptr, 0, &out));
    MZ_ASSERT_FALSE(annexBToAvcc(no_start, 0, &out));
    MZ_ASSERT_FALSE(annexBToAvcc(no_start, sizeof(no_start), nullptr));
    // 只有起始码、没有 NAL 内容：拿不出任何 NAL → 失败
    const uint8_t only_sc[] = {0x00, 0x00, 0x01};
    MZ_ASSERT_FALSE(annexBToAvcc(only_sc, sizeof(only_sc), &out));
}

MZ_TEST(ffmpeg_avcc_build_layout) {
    const std::vector<uint8_t> sps = {0x67, 0x42, 0xC0, 0x1E, 0x11};
    const std::vector<uint8_t> pps = {0x68, 0xCE, 0x3C, 0x80};
    const std::vector<uint8_t> expect = {
        0x01, 0x42, 0xC0, 0x1E,                    // configurationVersion / profile / compat / level
        0xFF,                                      // reserved(6) + lengthSizeMinusOne=3 → 4 字节长度
        0xE1,                                      // reserved(3) + numOfSequenceParameterSets=1
        0x00, 0x05, 0x67, 0x42, 0xC0, 0x1E, 0x11,  // SPS 长度 + SPS
        0x01,                                      // numOfPictureParameterSets=1
        0x00, 0x04, 0x68, 0xCE, 0x3C, 0x80};       // PPS 长度 + PPS
    std::vector<uint8_t> out;
    MZ_ASSERT_TRUE(buildAvcC(sps.data(), sps.size(), pps.data(), pps.size(), &out));
    MZ_ASSERT_EQ(out.size(), expect.size());
    if (out.size() == expect.size()) {
        MZ_ASSERT_EQ(::memcmp(out.data(), expect.data(), expect.size()), 0);
    }
}

MZ_TEST(ffmpeg_avcc_build_rejects_bad_params) {
    std::vector<uint8_t> out;
    const uint8_t sps[] = {0x67, 0x42, 0xC0, 0x1E};
    const uint8_t pps[] = {0x68, 0xCE};
    const uint8_t not_sps[] = {0x68, 0x42, 0xC0, 0x1E}; // 类型是 PPS，不能当 SPS 用
    const uint8_t not_pps[] = {0x67, 0xCE};
    MZ_ASSERT_FALSE(buildAvcC(nullptr, 0, pps, sizeof(pps), &out));
    MZ_ASSERT_FALSE(buildAvcC(sps, sizeof(sps), pps, sizeof(pps), nullptr));
    MZ_ASSERT_FALSE(buildAvcC(sps, 3, pps, sizeof(pps), &out));         // SPS 太短（拿不到 profile/level）
    MZ_ASSERT_FALSE(buildAvcC(not_sps, sizeof(not_sps), pps, sizeof(pps), &out));
    MZ_ASSERT_FALSE(buildAvcC(sps, sizeof(sps), not_pps, sizeof(not_pps), &out));
    MZ_ASSERT_FALSE(buildAvcC(sps, sizeof(sps), pps, 0, &out));          // 空 PPS
    // 超过 16 位长度字段：明确失败（不能截断长度，那会让播放端按错长度切数据）
    std::vector<uint8_t> huge(0x10000 + 1, 0x00);
    huge[0] = 0x67;
    huge[1] = 0x42;
    huge[2] = 0xC0;
    huge[3] = 0x1E;
    MZ_ASSERT_FALSE(buildAvcC(huge.data(), huge.size(), pps, sizeof(pps), &out));
}

MZ_TEST(ffmpeg_avcc_roundtrip_on_raw_sample) {
    // 真实裸流：整份文件 Annex-B → AVCC，再按 4 字节长度反解，必须**逐个 NAL 对上**
    const std::string path = samplePath("sample.h264");
    MZ_ASSERT_FALSE(path.empty());
    const std::string raw = readWholeFile(path);
    if (raw.empty()) {
        MZ_FAIL("裸流样本为空");
        return;
    }
    const uint8_t *data = reinterpret_cast<const uint8_t *>(raw.data());
    std::vector<H264Nal> nals;
    const size_t nal_count = splitAnnexBNals(data, raw.size(), &nals);
    MZ_ASSERT_GT(nal_count, 0u);
    if (nal_count == 0) {
        return;
    }

    std::vector<uint8_t> avcc;
    MZ_ASSERT_TRUE(annexBToAvcc(data, raw.size(), &avcc));
    size_t expected_size = 0;
    for (const auto &nal : nals) {
        expected_size += 4 + nal.size;
    }
    MZ_ASSERT_EQ(avcc.size(), expected_size);

    size_t pos = 0;
    size_t count = 0;
    while (pos + 4 <= avcc.size()) {
        const size_t len = (static_cast<size_t>(avcc[pos]) << 24) |
                           (static_cast<size_t>(avcc[pos + 1]) << 16) |
                           (static_cast<size_t>(avcc[pos + 2]) << 8) |
                           static_cast<size_t>(avcc[pos + 3]);
        if (len == 0 || pos + 4 + len > avcc.size()) {
            MZ_FAIL("AVCC 长度字段与数据不匹配（长度前缀写错了）");
            return;
        }
        const uint8_t type = static_cast<uint8_t>(avcc[pos + 4] & 0x1Fu);
        MZ_ASSERT_TRUE(type >= 1 && type <= 12); // H.264 的合法 nal_unit_type
        pos += 4 + len;
        ++count;
    }
    MZ_ASSERT_EQ(pos, avcc.size());       // 没有剩余字节
    MZ_ASSERT_EQ(count, nal_count);       // NAL 个数一致
    std::printf("    [ INFO ] 裸流转 AVCC：%zu 个 NAL / %zu 字节\n", count, avcc.size());
}

MZ_TEST(ffmpeg_avcc_build_from_raw_sample_sps_pps) {
    // 从真实裸流里抽 SPS/PPS 构造 avcC，并**逐字段核对**（不靠"看起来对"）
    const std::string path = samplePath("sample.h264");
    MZ_ASSERT_FALSE(path.empty());
    const std::string raw = readWholeFile(path);
    if (raw.empty()) {
        MZ_FAIL("裸流样本为空");
        return;
    }
    const uint8_t *data = reinterpret_cast<const uint8_t *>(raw.data());
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    if (!extractSpsPps(data, raw.size(), &sps, &pps)) {
        MZ_FAIL("SPS/PPS 提取失败");
        return;
    }
    std::vector<uint8_t> avcc;
    MZ_ASSERT_TRUE(buildAvcC(sps.data(), sps.size(), pps.data(), pps.size(), &avcc));
    MZ_ASSERT_EQ(avcc.size(), 11 + sps.size() + pps.size());
    if (avcc.size() != 11 + sps.size() + pps.size()) {
        return;
    }
    MZ_ASSERT_EQ(avcc[0], 0x01u);                       // configurationVersion
    MZ_ASSERT_EQ(avcc[1], sps[1]);                      // AVCProfileIndication
    MZ_ASSERT_EQ(avcc[2], sps[2]);                      // profile_compatibility
    MZ_ASSERT_EQ(avcc[3], sps[3]);                      // AVCLevelIndication
    MZ_ASSERT_EQ(avcc[4], 0xFFu);                       // lengthSizeMinusOne = 3
    MZ_ASSERT_EQ(avcc[5], 0xE1u);                       // 1 个 SPS
    MZ_ASSERT_EQ(avcc[6], 0x00u);
    MZ_ASSERT_EQ(avcc[7], static_cast<uint8_t>(sps.size()));
    MZ_ASSERT_EQ(avcc[8], 0x67u);                       // 嵌入的确实是 SPS
    MZ_ASSERT_TRUE(::memcmp(avcc.data() + 8, sps.data(), sps.size()) == 0);
    const size_t pps_off = 8 + sps.size();
    MZ_ASSERT_EQ(avcc[pps_off], 0x01u);                 // 1 个 PPS
    MZ_ASSERT_EQ(avcc[pps_off + 1], 0x00u);
    MZ_ASSERT_EQ(avcc[pps_off + 2], static_cast<uint8_t>(pps.size()));
    MZ_ASSERT_EQ(avcc[pps_off + 3], 0x68u);
    MZ_ASSERT_TRUE(::memcmp(avcc.data() + pps_off + 3, pps.data(), pps.size()) == 0);

    // 交叉验证：解封装层给出的 extradata 是 Annex-B（所以**必须**转换，不能直接塞进 FLV）
    Demuxer demuxer;
    if (!demuxer.open(path)) {
        MZ_FAIL("打不开裸流样本");
        return;
    }
    const StreamInfo *video = demuxer.firstVideo();
    MZ_ASSERT_NOT_NULL(video);
    if (video == nullptr || !video->extradata) {
        MZ_FAIL("裸流的视频流没有 extradata");
        return;
    }
    MZ_ASSERT_TRUE(looksLikeAnnexB(video->extradata->data(), video->extradata->size()));
}


// ---------------------------------------------------------------------------
// M8 / FR-2.2：ADTS 头解析、AudioSpecificConfig 构造、ADTS → raw AAC
//   全部是纯函数：字节自己造，不依赖样本、不依赖解码器。
//   每条断言旁边写明「能证明什么 / 什么情况会红」。
// ---------------------------------------------------------------------------

namespace {

/// 造一个合法 ADTS 帧（头 + 指定长度的 payload，payload 填 0x55）
std::vector<uint8_t> makeAdtsFrame(uint8_t profile, uint8_t sfi, uint8_t cc, size_t payload_size,
                                   bool with_crc = false, uint8_t raw_blocks = 0) {
    const size_t header_bytes = with_crc ? 9 : 7;
    const size_t frame_length = header_bytes + payload_size;
    std::vector<uint8_t> f(frame_length, 0x55);
    f[0] = 0xFF;
    // byte1: syncword 低 4 位 = 1111 | ID=0(MPEG-4) | layer=00 | protection_absent
    f[1] = static_cast<uint8_t>(with_crc ? 0xF0 : 0xF1);
    // byte2: profile(2) | sfi(4) | private(1) | channel_config 高位(1)
    f[2] = static_cast<uint8_t>((profile << 6) | ((sfi & 0x0F) << 2) | ((cc >> 2) & 0x01));
    // byte3: channel_config 低 2 位 | ... | frame_length 高 2 位
    f[3] = static_cast<uint8_t>(((cc & 0x03) << 6) | ((frame_length >> 11) & 0x03));
    f[4] = static_cast<uint8_t>((frame_length >> 3) & 0xFF);
    f[5] = static_cast<uint8_t>(((frame_length & 0x07) << 5) | 0x1F);
    f[6] = static_cast<uint8_t>(0xFC | (raw_blocks & 0x03));
    return f;
}

} // namespace

MZ_TEST(ffmpeg_aac_adts_header_and_asc) {
    // 能证明什么：ADTS 头字段的位域位置（profile / sfi / channel_config / frame_length）
    //             与 ASC 的位打包都对；AAC-LC/44100/单声道 → 12 08（立体声 → 12 10）。
    // 什么情况会红：位偏移写错（例如 channel_config 高低位拆错）、ASC 打包错、把 CRC 当 payload。
    const std::vector<uint8_t> mono = makeAdtsFrame(1, 4, 1, 20);   // AAC-LC / 44100 / mono
    AdtsHeader h;
    MZ_ASSERT_TRUE(parseAdtsHeader(mono.data(), mono.size(), &h));
    MZ_ASSERT_EQ(h.profile, 1u);
    MZ_ASSERT_EQ(h.sampling_frequency_index, 4u);
    MZ_ASSERT_EQ(h.channel_configuration, 1u);
    MZ_ASSERT_EQ(h.header_bytes, 7u);
    MZ_ASSERT_EQ(h.frame_length, mono.size());
    MZ_ASSERT_EQ(aacSampleRateFromIndex(h.sampling_frequency_index), 44100u);

    uint8_t asc[2] = {0, 0};
    MZ_ASSERT_TRUE(buildAudioSpecificConfig(h, asc));
    MZ_ASSERT_EQ(asc[0], 0x12u);   // AOT=2(AAC-LC) | sfi 高 3 位 = 010
    MZ_ASSERT_EQ(asc[1], 0x08u);   // sfi 低位 0 | channel_config 1

    const std::vector<uint8_t> stereo = makeAdtsFrame(1, 4, 2, 20);
    MZ_ASSERT_TRUE(parseAdtsHeader(stereo.data(), stereo.size(), &h));
    MZ_ASSERT_EQ(h.channel_configuration, 2u);
    MZ_ASSERT_TRUE(buildAudioSpecificConfig(h, asc));
    MZ_ASSERT_EQ(asc[0], 0x12u);
    MZ_ASSERT_EQ(asc[1], 0x10u);   // 443?→ 12 10 是 AAC-LC/44100/立体声的标准 ASC

    const std::vector<uint8_t> f48 = makeAdtsFrame(1, 3, 2, 8);   // 48000 / 立体声
    MZ_ASSERT_TRUE(parseAdtsHeader(f48.data(), f48.size(), &h));
    MZ_ASSERT_TRUE(buildAudioSpecificConfig(h, asc));
    MZ_ASSERT_EQ(asc[0], 0x11u);
    MZ_ASSERT_EQ(asc[1], 0x90u);   // 11 90 = AAC-LC/48000/立体声

    // 采样率表：13..15 一律 0（表外不猜），0/12 两端必须对
    MZ_ASSERT_EQ(aacSampleRateFromIndex(0), 96000u);
    MZ_ASSERT_EQ(aacSampleRateFromIndex(12), 7350u);
    MZ_ASSERT_EQ(aacSampleRateFromIndex(13), 0u);
    MZ_ASSERT_EQ(aacSampleRateFromIndex(15), 0u);
    MZ_ASSERT_EQ(aacIndexFromSampleRate(44100), 4);
    MZ_ASSERT_EQ(aacIndexFromSampleRate(44101), -1);
}

MZ_TEST(ffmpeg_aac_adts_crc_header_is_nine_bytes) {
    // 能证明什么：protection_absent=0 时头是 9 字节（含 2 字节 CRC），剥头必须按 9 算。
    // 什么情况会红：把 7 写死（CRC 流的 payload 会多带 2 字节 → 播放端噪声/爆音）。
    const std::vector<uint8_t> frame = makeAdtsFrame(1, 4, 1, 16, /*with_crc=*/true);
    AdtsHeader h;
    MZ_ASSERT_TRUE(parseAdtsHeader(frame.data(), frame.size(), &h));
    MZ_ASSERT_FALSE(h.protection_absent);
    MZ_ASSERT_EQ(h.header_bytes, 9u);

    std::vector<uint8_t> raw;
    MZ_ASSERT_TRUE(adtsToRawAac(frame.data(), frame.size(), &raw));
    MZ_ASSERT_EQ(raw.size(), 16u);
    MZ_ASSERT_EQ(raw.front(), 0x55u);   // 从第 9 字节开始才是 payload
}

MZ_TEST(ffmpeg_aac_adts_rejects_malformed) {
    // 能证明什么：畸形输入一律**明确失败**（返回 false），不产生"看起来能用"的半成品。
    // 什么情况会红：把任一检查放宽成"默认值"（那会把畸形流的坏参数静默塞进 ASC）。
    AdtsHeader h;
    const std::vector<uint8_t> good = makeAdtsFrame(1, 4, 1, 8);

    MZ_ASSERT_FALSE(parseAdtsHeader(nullptr, 16, &h));
    MZ_ASSERT_FALSE(parseAdtsHeader(good.data(), 6, &h));          // 短于 7 字节
    MZ_ASSERT_FALSE(parseAdtsHeader(good.data(), good.size(), nullptr));

    std::vector<uint8_t> bad = good;
    bad[0] = 0xFE;                                                  // 同步字错
    MZ_ASSERT_FALSE(parseAdtsHeader(bad.data(), bad.size(), &h));

    bad = good;
    bad[1] = 0xF3;                                                  // layer != 0
    MZ_ASSERT_FALSE(parseAdtsHeader(bad.data(), bad.size(), &h));

    for (uint8_t sfi : {13, 14, 15}) {                              // 表外采样率索引
        const std::vector<uint8_t> f = makeAdtsFrame(1, sfi, 1, 8);
        MZ_ASSERT_FALSE(parseAdtsHeader(f.data(), f.size(), &h));
    }
    {
        const std::vector<uint8_t> f = makeAdtsFrame(1, 4, 0, 8);    // channel_config=0（PCE）
        MZ_ASSERT_FALSE(parseAdtsHeader(f.data(), f.size(), &h));
    }
    {
        // 帧长（13 bit）小于头长：把 frame_length 改成 3
        std::vector<uint8_t> f = good;
        f[3] = static_cast<uint8_t>(f[3] & 0xFC);
        f[4] = 0;
        f[5] = static_cast<uint8_t>(f[5] & 0x1F);
        MZ_ASSERT_FALSE(parseAdtsHeader(f.data(), f.size(), &h));
    }
}

MZ_TEST(ffmpeg_aac_adts_to_raw_multi_frame) {
    // 能证明什么：一个包里多帧时逐帧剥头、按序拼接；半帧/多块一律失败（不静默丢尾巴）。
    // 什么情况会红：只剥第一帧的头；或把帧长当 payload 长（少/多算 7 字节）。
    std::vector<uint8_t> stream;
    for (int i = 0; i < 3; ++i) {
        const std::vector<uint8_t> f = makeAdtsFrame(1, 4, 2, 20);
        stream.insert(stream.end(), f.begin(), f.end());
    }
    std::vector<uint8_t> raw;
    MZ_ASSERT_TRUE(adtsToRawAac(stream.data(), stream.size(), &raw));
    MZ_ASSERT_EQ(raw.size(), 60u);                                  // 3 × 20
    for (uint8_t b : raw) {
        MZ_ASSERT_EQ(b, 0x55u);
    }

    // 半帧：帧长声明 27（7+20），但只给 20 字节
    MZ_ASSERT_FALSE(adtsToRawAac(stream.data(), 20, &raw));

    // 一帧多块（raw_blocks != 0）：本实现明确不做
    const std::vector<uint8_t> multi = makeAdtsFrame(1, 4, 2, 20, false, 1);
    MZ_ASSERT_FALSE(adtsToRawAac(multi.data(), multi.size(), &raw));

    // 同步字丢失（第二帧头坏）
    std::vector<uint8_t> broken = stream;
    broken[27] = 0x00;
    MZ_ASSERT_FALSE(adtsToRawAac(broken.data(), broken.size(), &raw));
}

MZ_TEST(ffmpeg_aac_adts_matches_real_ts_sample) {
    // 能证明什么：**自己写的解析器**能读懂 ffmpeg 真实产出的 TS 音频包（交叉校验，
    //   与 M4-b 的"SPS 解析 vs Demuxer 两套独立路径逐项一致"同一思路）。
    // 什么情况会红：位域按错误规范写（对着手造字节能过、对着真实码流就错）。
    const std::string path = samplePath("sample.ts");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }
    Demuxer demuxer;
    MZ_ASSERT_TRUE(demuxer.open(path));
    const StreamInfo *audio = demuxer.firstAudio();
    MZ_ASSERT_NOT_NULL(audio);
    if (audio == nullptr) {
        return;
    }
    // TS 的 AAC **没有** extradata —— 这正是 M8 要处理的那种形状
    MZ_ASSERT_TRUE(audio->extradata == nullptr || audio->extradata->empty());

    bool found = false;
    for (int i = 0; i < 512; ++i) {
        const Demuxer::ReadResult r = demuxer.readPacket();
        MZ_ASSERT_TRUE(r == Demuxer::ReadResult::Packet);
        if (r != Demuxer::ReadResult::Packet) {
            return;
        }
        if (demuxer.packetStreamIndex() != audio->index) {
            continue;
        }
        const AVPacket *pkt = demuxer.packet();
        MZ_ASSERT_NOT_NULL(pkt);
        AdtsHeader h;
        MZ_ASSERT_TRUE(parseAdtsHeader(pkt->data, static_cast<size_t>(pkt->size), &h));
        // 与 Demuxer 报的流参数交叉校验（两套独立路径）
        MZ_ASSERT_EQ(aacSampleRateFromIndex(h.sampling_frequency_index),
                     static_cast<uint32_t>(audio->sample_rate));
        uint8_t asc[2] = {0, 0};
        MZ_ASSERT_TRUE(buildAudioSpecificConfig(h, asc));
        MZ_ASSERT_EQ(asc[0], 0x12u);   // AAC-LC
        MZ_ASSERT_EQ(asc[1], 0x08u);   // 44100 / mono（样本是 sine 单声道）
        found = true;
        break;
    }
    MZ_ASSERT_TRUE(found);
}


// ---------------------------------------------------------------------------
// M9：TS/HLS 需要的**反向**转换（AVCC→Annex-B、avcC→SPS/PPS、raw AAC→ADTS）
// ---------------------------------------------------------------------------

MZ_TEST(ffmpeg_h264_avcc_to_annexb_roundtrip) {
    // 能证明什么：AVCC ↔ Annex-B 互逆；length_size 用 avcC 里的值（本项目是 4）。
    // 什么情况会红：把"长度前缀"当数据、长度字段位宽写错、0 长度 NAL 被静默跳过。
    const std::vector<uint8_t> nal_a = {0x65, 0x11, 0x22, 0x33};      // IDR 片段
    const std::vector<uint8_t> nal_b = {0x41, 0x44, 0x55};            // 非 IDR 片段
    std::vector<uint8_t> avcc;
    for (const auto *nal : {&nal_a, &nal_b}) {
        avcc.push_back(static_cast<uint8_t>((nal->size() >> 24) & 0xFF));
        avcc.push_back(static_cast<uint8_t>((nal->size() >> 16) & 0xFF));
        avcc.push_back(static_cast<uint8_t>((nal->size() >> 8) & 0xFF));
        avcc.push_back(static_cast<uint8_t>(nal->size() & 0xFF));
        avcc.insert(avcc.end(), nal->begin(), nal->end());
    }
    std::vector<uint8_t> annexb;
    MZ_ASSERT_TRUE(avccToAnnexB(avcc.data(), avcc.size(), 4, &annexb));
    // 期望：00 00 01 + nal_a + 00 00 01 + nal_b
    MZ_ASSERT_EQ(annexb.size(), 3 + nal_a.size() + 3 + nal_b.size());
    MZ_ASSERT_EQ(annexb[0], 0x00u);
    MZ_ASSERT_EQ(annexb[1], 0x00u);
    MZ_ASSERT_EQ(annexb[2], 0x01u);
    MZ_ASSERT_EQ(annexb[3], 0x65u);
    MZ_ASSERT_EQ(annexb[3 + nal_a.size()], 0x00u);
    MZ_ASSERT_EQ(annexb[3 + nal_a.size() + 3], 0x41u);

    // 再转回去必须与原始 AVCC **逐字节相同**（互逆性）
    std::vector<uint8_t> back;
    MZ_ASSERT_TRUE(annexBToAvcc(annexb.data(), annexb.size(), &back));
    MZ_ASSERT_EQ(back.size(), avcc.size());
    bool same = (back.size() == avcc.size());
    for (size_t i = 0; same && i < avcc.size(); ++i) {
        same = (back[i] == avcc[i]);
    }
    MZ_ASSERT_TRUE(same);

    // 畸形：0 长度 NAL / 声明越界 / 长度前缀字节数非法 → 一律失败
    const std::vector<uint8_t> zero = {0x00, 0x00, 0x00, 0x00};
    MZ_ASSERT_FALSE(avccToAnnexB(zero.data(), zero.size(), 4, &back));
    const std::vector<uint8_t> too_long = {0x00, 0x00, 0x00, 0x09, 0x65, 0x00};
    MZ_ASSERT_FALSE(avccToAnnexB(too_long.data(), too_long.size(), 4, &back));
    MZ_ASSERT_FALSE(avccToAnnexB(avcc.data(), avcc.size(), 3, &back));
}

MZ_TEST(ffmpeg_h264_parse_avcc_extracts_sps_pps) {
    // 能证明什么：能从 avcC 里把 SPS/PPS 原样取出来（HLS 每个切片的关键帧要补它们）。
    // 什么情况会红：偏移算错（多/少 1 字节）、没做 NAL 类型自检。
    const std::vector<uint8_t> sps = {0x67, 0x42, 0xC0, 0x0D, 0xDA, 0x05};
    const std::vector<uint8_t> pps = {0x68, 0xCE, 0x0F, 0xC8};
    std::vector<uint8_t> avcc;
    MZ_ASSERT_TRUE(buildAvcC(sps.data(), sps.size(), pps.data(), pps.size(), &avcc));

    std::vector<uint8_t> out_sps;
    std::vector<uint8_t> out_pps;
    MZ_ASSERT_TRUE(parseAvcC(avcc.data(), avcc.size(), &out_sps, &out_pps));
    MZ_ASSERT_EQ(out_sps.size(), sps.size());
    MZ_ASSERT_EQ(out_pps.size(), pps.size());
    bool sps_same = (out_sps.size() == sps.size());
    for (size_t i = 0; sps_same && i < sps.size(); ++i) {
        sps_same = (out_sps[i] == sps[i]);
    }
    MZ_ASSERT_TRUE(sps_same);
    MZ_ASSERT_EQ(out_pps[0], 0x68u);

    // 畸形：版本不是 1 / 截断
    std::vector<uint8_t> bad = avcc;
    bad[0] = 2;
    MZ_ASSERT_FALSE(parseAvcC(bad.data(), bad.size(), &out_sps, &out_pps));
    MZ_ASSERT_FALSE(parseAvcC(avcc.data(), 8, &out_sps, &out_pps));
}

MZ_TEST(ffmpeg_aac_raw_to_adts_roundtrip) {
    // 能证明什么：raw AAC + ASC → ADTS 帧，再解析回来字段一致（TS/HLS 靠它把 raw 变 ADTS）。
    // 什么情况会红：头字段错、帧长少算 7 字节、AOT 映射错（profile = AOT - 1）。
    const uint8_t asc[2] = {0x12, 0x08};   // AAC-LC / 44100 / mono
    std::vector<uint8_t> raw(120, 0x5A);
    std::vector<uint8_t> frame;
    MZ_ASSERT_TRUE(rawAacToAdts(asc, 2, raw.data(), raw.size(), &frame));
    MZ_ASSERT_EQ(frame.size(), 7 + raw.size());

    AdtsHeader header;
    MZ_ASSERT_TRUE(parseAdtsHeader(frame.data(), frame.size(), &header));
    MZ_ASSERT_EQ(header.profile, 1u);                                    // AOT 2 - 1
    MZ_ASSERT_EQ(header.sampling_frequency_index, 4u);
    MZ_ASSERT_EQ(header.channel_configuration, 1u);
    MZ_ASSERT_EQ(header.frame_length, static_cast<uint16_t>(frame.size()));
    MZ_ASSERT_EQ(frame[7], 0x5A);                                        // payload 从第 7 字节起

    // 剥回去必须与原始 raw 一致
    std::vector<uint8_t> back;
    MZ_ASSERT_TRUE(adtsToRawAac(frame.data(), frame.size(), &back));
    MZ_ASSERT_EQ(back.size(), raw.size());

    // 拒绝：AOT=5（HE-AAC 不能用 ADTS 的 2 bit profile 表示）/ ASC 太短 / 帧长超过 13 bit
    const uint8_t he_asc[2] = {0x2B, 0x92};   // AOT=5
    MZ_ASSERT_FALSE(rawAacToAdts(he_asc, 2, raw.data(), raw.size(), &frame));
    MZ_ASSERT_FALSE(rawAacToAdts(asc, 1, raw.data(), raw.size(), &frame));
    std::vector<uint8_t> huge(8191, 0x11);
    MZ_ASSERT_FALSE(rawAacToAdts(asc, 2, huge.data(), huge.size(), &frame));

    uint8_t header7[7] = {0};
    MZ_ASSERT_FALSE(buildAdtsHeader(1, 13, 1, 10, header7));   // 表外采样率索引
    MZ_ASSERT_FALSE(buildAdtsHeader(1, 4, 0, 10, header7));    // channel_configuration = 0（PCE）
}

MZ_TEST(ffmpeg_codec_matrix_transcode_branch_needs_real_capability) {
    // 【M10-b】HEVC → FLV 不是"永远不行"也不是"永远能转"：取决于**本机能力**
    //   · 没有 HEVC 解码器 / 没有 H264 编码器 → Unsupported（旧的冻结行为）
    //   · 两者都有 → Transcode（M10-b 的转码路径）
    TranscodeCaps none;
    MZ_ASSERT_EQ(static_cast<int>(decideOutput(AV_CODEC_ID_HEVC, AV_CODEC_ID_AAC, "flv", none)),
                 static_cast<int>(CopyOrTranscode::Unsupported));

    TranscodeCaps only_decode;
    only_decode.hevc_decode = true;
    MZ_ASSERT_EQ(
        static_cast<int>(decideOutput(AV_CODEC_ID_HEVC, AV_CODEC_ID_AAC, "flv", only_decode)),
        static_cast<int>(CopyOrTranscode::Unsupported));

    TranscodeCaps both;
    both.hevc_decode = true;
    both.h264_encode = true;
    MZ_ASSERT_TRUE(both.hevc_to_h264());
    MZ_ASSERT_EQ(static_cast<int>(decideOutput(AV_CODEC_ID_HEVC, AV_CODEC_ID_AAC, "flv", both)),
                 static_cast<int>(CopyOrTranscode::Transcode));

    // H264 源不受"能力"影响：有转码能力也仍是 Remux（不转码就别乱转）
    MZ_ASSERT_EQ(static_cast<int>(decideOutput(AV_CODEC_ID_H264, AV_CODEC_ID_AAC, "flv", both)),
                 static_cast<int>(CopyOrTranscode::Remux));

    // 本机探测出来的能力：与真实 avcodec 一致（这台机器上 libx264 + hevc 解码器都有）
    const TranscodeCaps real = detectTranscodeCaps();
    MZ_ASSERT_TRUE(real.h264_encode);
    MZ_ASSERT_EQ(static_cast<int>(decideOutput(AV_CODEC_ID_HEVC, AV_CODEC_ID_AAC, "flv", real)),
                 static_cast<int>(real.hevc_to_h264() ? CopyOrTranscode::Transcode
                                                      : CopyOrTranscode::Unsupported));
}
