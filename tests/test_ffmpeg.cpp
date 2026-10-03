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
#include "ffmpeg/demuxer.h"
#include "ffmpeg/time_base.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using namespace mzmedia;

namespace {

/// 找样本：优先 $MZ_SAMPLES_DIR，其次 samples/（仓库根运行）、../samples/（build 目录运行）
std::string samplePath(const std::string &name) {
    std::vector<std::string> candidates;
    if (const char *env = ::getenv("MZ_SAMPLES_DIR"); env != nullptr) {
        candidates.push_back(std::string(env) + "/" + name);
    }
    candidates.push_back("samples/" + name);
    candidates.push_back("../samples/" + name);      // ctest 的 cwd 是 build/
    candidates.push_back("../../samples/" + name);     // ctest 的 cwd 是 build/tests/
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
