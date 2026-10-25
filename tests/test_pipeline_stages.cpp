/*
 * 流水线分段测试（v0.2.3 重构，分组 `pipeline`）
 * ============================================================================
 * 重构把 `DemuxerProducer` 拆成了三条可独立测的东西：
 *   · `SourceSession`      —— 读：打开/读包/EOF/循环重开/时间戳偏移；
 *   · `PacketStage`        —— 形状：Raw / AnnexB / Adts / Transcode；
 *   · `DemuxerProducer`    —— 只负责搭链与驱动（它自己的行为仍由 `producer` 组覆盖）。
 *
 * 为什么要有这一组：以前这些逻辑全在一个类里，只能"整条链一起测"（改一处就得跑 15 个用例）；
 * 现在每一段可以**单独**证明。这也正是重构的收益——**可测面积变大了**。
 *
 * 覆盖维度（正常 / 空 / 满 / 断开 / 超大）：
 *   正常  Raw 透传保时间戳与载荷；AnnexB 建 avcC 并逐包转 AVCC；Adts 建 ASC 并剥头
 *   空    没有 extradata 的 AAC 源 → Adts 阶段会去探针（缺样本时明确失败）
 *   满    转码单次产出上限由 `max_out_packets` 兜底（`transcode` 组已覆盖，这里只验 flush 幂等）
 *   断开  非循环 EOF → 冲残余一次；Session 的 EOF 是**稳定终态**
 *   超大  转码的 `ownsTimeline()` 必须为真（否则生产者会再加一次趟偏移 → 双计）
 * ============================================================================
 */

#include "test_main.h"

#include "core/util.h"
#include "ffmpeg/aac_util.h"
#include "ffmpeg/demuxer.h"
#include "ffmpeg/h264_util.h"
#include "media/media_packet.h"
#include "media/packet_stage.h"
#include "media/source_session.h"
#include "media/stage_passthrough.h"
#include "media/stage_transcode.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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

std::vector<uint8_t> fakeSps() {
    return {0x67, 0x42, 0xC0, 0x0D, 0xDA, 0x05, 0x07, 0xEC};
}
std::vector<uint8_t> fakePps() {
    return {0x68, 0xCE, 0x0F, 0xC8};
}

/// 造一个 H264 视频流信息（extradata = Annex-B 的 SPS/PPS）
mzmedia::StreamInfo makeAnnexBVideo() {
    mzmedia::StreamInfo info;
    info.index = 0;
    info.codec_id = AV_CODEC_ID_H264;
    info.codec_name = "h264";
    info.is_video = true;
    info.width = 320;
    info.height = 240;
    info.frame_rate = mzmedia::TimeBase{25, 1};
    std::vector<uint8_t> extra;
    const std::vector<uint8_t> sps = fakeSps();
    const std::vector<uint8_t> pps = fakePps();
    extra.insert(extra.end(), {0x00, 0x00, 0x00, 0x01});
    extra.insert(extra.end(), sps.begin(), sps.end());
    extra.insert(extra.end(), {0x00, 0x00, 0x00, 0x01});
    extra.insert(extra.end(), pps.begin(), pps.end());
    info.extradata = std::make_shared<const std::vector<uint8_t>>(extra);
    return info;
}

mzmedia::MediaPacket::Ptr makePacket(mzmedia::MediaKind kind, int index,
                                     std::vector<uint8_t> bytes, bool key, int64_t dts,
                                     int64_t pts) {
    auto payload = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
    return mzmedia::MediaPacket::create(kind, index, std::move(payload), key, dts, pts);
}

} // namespace

// ---------------------------------------------------------------------------
// RawStage：透传（时间戳/载荷/关键帧都不动）
// ---------------------------------------------------------------------------

MZ_TEST(pipeline_raw_stage_passes_through_untouched) {
    mzmedia::RawStage stage;
    const mzmedia::StreamInfo src = makeAnnexBVideo();
    mzmedia::StageContext ctx;
    mzmedia::StreamInfo out;
    std::string error;
    MZ_ASSERT_TRUE(stage.prepare(src, ctx, &out, &error));
    MZ_ASSERT_EQ(out.width, 320);
    MZ_ASSERT_FALSE(stage.ownsTimeline());     // 转换类**不**拥有时间轴
    MZ_ASSERT_EQ(stage.convertedPackets(), 0u); // 透传不算"转换"

    auto in = makePacket(mzmedia::MediaKind::Video, 0, {0xAB, 0xCD}, true, 100, 140);
    MZ_ASSERT_NOT_NULL(in.get());
    std::vector<mzmedia::MediaPacket::Ptr> produced;
    MZ_ASSERT_TRUE(stage.process(*in, &produced, &error));
    MZ_ASSERT_EQ(produced.size(), 1u);
    if (!produced.empty()) {
        MZ_ASSERT_EQ(produced[0]->dtsMs(), 100);
        MZ_ASSERT_EQ(produced[0]->ptsMs(), 140);
        MZ_ASSERT_TRUE(produced[0]->isKeyFrame());
        MZ_ASSERT_EQ(produced[0]->size(), 2u);
        // 零拷贝：同一个共享载荷（订阅者共享同一块内存）
        MZ_ASSERT_TRUE(produced[0]->payload() == in->payload());
    }
}

// ---------------------------------------------------------------------------
// AnnexBStage：建 avcC + 逐包转 AVCC
// ---------------------------------------------------------------------------

MZ_TEST(pipeline_annexb_stage_builds_avcc_snapshot) {
    mzmedia::AnnexBStage stage;
    const mzmedia::StreamInfo src = makeAnnexBVideo();
    mzmedia::StageContext ctx;
    mzmedia::StreamInfo out;
    std::string error;
    MZ_ASSERT_TRUE(stage.prepare(src, ctx, &out, &error));
    MZ_ASSERT_NOT_NULL(out.extradata.get());
    if (out.extradata) {
        MZ_ASSERT_TRUE(!out.extradata->empty());
        MZ_ASSERT_EQ(out.extradata->at(0), 0x01u); // avcC 的 configurationVersion
    }
    MZ_ASSERT_EQ(out.width, 320);   // 宽高照抄
    MZ_ASSERT_EQ(out.height, 240);

    // Annex-B 包（起始码 00 00 00 01 + IDR）→ AVCC（4 字节长度 + NAL）
    const std::vector<uint8_t> annexb = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x99, 0xAA};
    auto in = makePacket(mzmedia::MediaKind::Video, 0, annexb, true, 40, 80);
    std::vector<mzmedia::MediaPacket::Ptr> produced;
    MZ_ASSERT_TRUE(stage.process(*in, &produced, &error));
    MZ_ASSERT_EQ(produced.size(), 1u);
    MZ_ASSERT_EQ(stage.convertedPackets(), 1u);
    if (!produced.empty()) {
        // 输出必须是 4 字节长度前缀（AVCC），不是起始码
        const std::vector<uint8_t> &data = *produced[0]->payload();
        MZ_ASSERT_EQ(data.size(), 8u);
        MZ_ASSERT_EQ(data[0], 0x00u);
        MZ_ASSERT_EQ(data[1], 0x00u);
        MZ_ASSERT_EQ(data[2], 0x00u);
        MZ_ASSERT_EQ(data[3], 0x04u); // 长度 = 4
        MZ_ASSERT_EQ(data[4], 0x65u); // NAL 原样
        MZ_ASSERT_EQ(produced[0]->dtsMs(), 40);
        MZ_ASSERT_EQ(produced[0]->ptsMs(), 80);
    }
}

// ---------------------------------------------------------------------------
// AdtsStage：由真实 TS 造 ASC + 逐包剥 ADTS 头
// ---------------------------------------------------------------------------

MZ_TEST(pipeline_adts_stage_builds_asc_and_strips_header) {
    const std::string path = samplePath("sample.ts");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }
    mzmedia::Demuxer probe;
    MZ_ASSERT_TRUE(probe.open(path));
    const mzmedia::StreamInfo *audio = probe.firstAudio();
    MZ_ASSERT_NOT_NULL(audio);
    if (audio == nullptr) {
        return;
    }
    // TS 的 AAC 没有 extradata（每帧自带 ADTS 头）—— 这正是本阶段要处理的情况
    MZ_ASSERT_TRUE(!audio->extradata || audio->extradata->empty());

    mzmedia::AdtsStage stage;
    mzmedia::StageContext ctx;
    ctx.path = path;
    mzmedia::StreamInfo out;
    std::string error;
    MZ_ASSERT_TRUE(stage.prepare(*audio, ctx, &out, &error));
    MZ_ASSERT_NOT_NULL(out.extradata.get());
    if (out.extradata) {
        MZ_ASSERT_EQ(out.extradata->size(), 2u);
        MZ_ASSERT_EQ(out.extradata->at(0), 0x12u); // ASC：AAC-LC / 44100
        MZ_ASSERT_EQ(out.extradata->at(1), 0x08u);
    }

    // 从真实 TS 里取一个 ADTS 音频包，交给阶段 → 必须剥掉头
    mzmedia::MediaPacket::Ptr adts_packet;
    for (int i = 0; i < 64 && !adts_packet; ++i) {
        MZ_ASSERT_EQ(probe.readPacket(), mzmedia::Demuxer::ReadResult::Packet);
        if (probe.packetStreamIndex() != audio->index) {
            continue;
        }
        const AVPacket *pkt = probe.packet();
        if (pkt == nullptr || pkt->data == nullptr || pkt->size <= 0) {
            continue;
        }
        std::vector<uint8_t> bytes(pkt->data, pkt->data + pkt->size);
        adts_packet = makePacket(mzmedia::MediaKind::Audio, audio->index, std::move(bytes), true,
                                 100, 100);
    }
    MZ_ASSERT_NOT_NULL(adts_packet.get());
    if (!adts_packet) {
        return;
    }
    std::vector<mzmedia::MediaPacket::Ptr> produced;
    MZ_ASSERT_TRUE(stage.process(*adts_packet, &produced, &error));
    MZ_ASSERT_EQ(produced.size(), 1u);
    MZ_ASSERT_EQ(stage.convertedPackets(), 1u);
    if (!produced.empty()) {
        MZ_ASSERT_TRUE(produced[0]->size() < adts_packet->size()); // 剥头 → 短了
        const std::vector<uint8_t> &data = *produced[0]->payload();
        // 输出里不能再有 ADTS 同步字（FF Fx）
        const bool sync = data.size() >= 2 && data[0] == 0xFF && (data[1] & 0xF0) == 0xF0;
        MZ_ASSERT_FALSE(sync);
        MZ_ASSERT_EQ(produced[0]->dtsMs(), 100);
    }
}

// ---------------------------------------------------------------------------
// SourceSession：读到 EOF（稳定终态）、计数、close 幂等
// ---------------------------------------------------------------------------

MZ_TEST(pipeline_source_session_reads_to_eof) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }
    mzmedia::SourceSession session(mzmedia::SourceSession::Config{});
    MZ_ASSERT_TRUE(session.open(path));
    MZ_ASSERT_TRUE(session.opened());
    MZ_ASSERT_STR_EQ(session.path(), path);

    int packets = 0;
    while (true) {
        mzmedia::SourceSession::Packet pkt;
        std::string error;
        const mzmedia::SourceSession::Result r = session.read(&pkt, &error);
        if (r == mzmedia::SourceSession::Result::EndOfStream) {
            break;
        }
        MZ_ASSERT_EQ(r, mzmedia::SourceSession::Result::Packet);
        if (r != mzmedia::SourceSession::Result::Packet) {
            MZ_FAIL("读包失败：" + error);
            return;
        }
        MZ_ASSERT_NOT_NULL(pkt.payload.get());
        if (pkt.payload) {
            MZ_ASSERT_TRUE(!pkt.payload->empty());
        }
        ++packets;
    }
    MZ_ASSERT_EQ(packets, 138); // 50 视频 + 88 音频（与 v0.1 起的样本一致）
    MZ_ASSERT_EQ(session.totalPackets(), 138u);
    MZ_ASSERT_GT(session.totalBytes(), 0u);
    MZ_ASSERT_EQ(session.skippedUnknownStreams(), 0u);
    MZ_ASSERT_EQ(session.loopCount(), 0u);
    MZ_ASSERT_EQ(session.loopOffsetMs(), 0);

    // EOF 是**稳定终态**：再读仍然是 EndOfStream（不是 Error）
    mzmedia::SourceSession::Packet again;
    std::string error;
    MZ_ASSERT_EQ(session.read(&again, &error), mzmedia::SourceSession::Result::EndOfStream);
    MZ_ASSERT_TRUE(session.close());
    MZ_ASSERT_FALSE(session.opened());
    MZ_ASSERT_FALSE(session.close()); // 幂等：本来就没打开
}

// ---------------------------------------------------------------------------
// SourceSession：循环重开的时间轴（首包 dts 为负的源也不许倒退）
// ---------------------------------------------------------------------------

MZ_TEST(pipeline_source_session_loop_offsets_negative_start) {
    const std::string path = samplePath("bf.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }
    mzmedia::SourceSession::Config config;
    config.loop = true;
    mzmedia::SourceSession session(config);
    MZ_ASSERT_TRUE(session.open(path));

    int64_t last_video_dts = 0;
    int video = 0;
    bool monotonic = true;
    int guard = 0;
    while (guard++ < 500 && video < 60) {
        mzmedia::SourceSession::Packet pkt;
        std::string error;
        const mzmedia::SourceSession::Result r = session.read(&pkt, &error);
        MZ_ASSERT_EQ(r, mzmedia::SourceSession::Result::Packet); // 循环模式不该 EOS
        if (r != mzmedia::SourceSession::Result::Packet) {
            MZ_FAIL("循环读包失败：" + error);
            return;
        }
        if (pkt.kind != mzmedia::MediaKind::Video) {
            continue;
        }
        // Session 交的是**原始** dts（每趟从源起点重新开始），趟偏移由生产者加。
        // 所以这里要按生产者的口径算"生效时间戳"，才等价于观众看到的时间轴：
        //   生效 dts = 原始 dts + 当前趟偏移
        const int64_t effective = pkt.raw_dts_ms + session.loopOffsetMs();
        if (video > 0 && effective <= last_video_dts) {
            monotonic = false;
        }
        last_video_dts = effective;
        ++video;
    }
    MZ_ASSERT_GT(session.loopCount(), 0u);          // 确实跨了接缝
    MZ_ASSERT_GT(session.loopOffsetMs(), 0);        // 偏移已经累加
    MZ_ASSERT_TRUE(monotonic);
    (void) session.close();
}

// ---------------------------------------------------------------------------
// TranscodeStage：拥有时间轴（生产者据此**不加**趟偏移）+ flush 幂等
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 【M11-a2】重连的时间轴接续公式（纯函数）：重连处最容易"悄悄错"的地方
//
// 什么让它们红：
//   · 公式漏掉 `frame_gap` → 新一段第一个包与上一包**同一个 dts**（第二个断言）；
//   · 公式把"对端接着发"当异常 → 时间轴被拉到未来，播放器等一大截（第三个断言）；
//   · `has_last == false` 时误用 0 当基准 → 偏移算成 −raw_first（第四个断言）。
// ---------------------------------------------------------------------------

MZ_TEST(pipeline_timeline_offset_restarts_from_zero) {
    // 实测场景：上一包生效 dts=15920，对端重连后从 200ms 重新开始 → 偏移 15760
    const int64_t offset = mzmedia::timelineOffsetForReconnect(15920, true, 200, 40);
    MZ_ASSERT_EQ(offset, 15760);
    MZ_ASSERT_EQ(200 + offset, 15960); // = 上一包 + 一帧，**不是**重复的 15920
}

MZ_TEST(pipeline_timeline_offset_continues_after_outage) {
    // 对端自己接着走钟（断流 460 ms）：公式把它**拉回来**接到上一包之后（不保留空档）
    // 实测场景：上一包 15920，新一段原始 dts=16380，一帧 40 → 偏移 −420
    const int64_t offset = mzmedia::timelineOffsetForReconnect(15920, true, 16380, 40);
    MZ_ASSERT_EQ(offset, -420);
    MZ_ASSERT_EQ(16380 + offset, 15960);
}

MZ_TEST(pipeline_timeline_offset_without_previous_packet) {
    // 没有上一包时基准取"新一段自己的首个 dts"，于是只前进一帧（因此这个函数**只能**用在接续处）
    MZ_ASSERT_EQ(mzmedia::timelineOffsetForReconnect(0, false, 200, 40), 40);
    MZ_ASSERT_EQ(mzmedia::timelineOffsetForReconnect(0, false, 0, 1), 1);
}

MZ_TEST(pipeline_net_state_names_are_stable) {
    // stats 里这几个字符串是**对外契约**（脚本/看板按它判断），改名要当成破坏性变更
    MZ_ASSERT_TRUE(std::string(mzmedia::SourceSession::netStateName(
                       mzmedia::SourceSession::NetState::Idle)) == "idle");
    MZ_ASSERT_TRUE(std::string(mzmedia::SourceSession::netStateName(
                       mzmedia::SourceSession::NetState::Playing)) == "playing");
    MZ_ASSERT_TRUE(std::string(mzmedia::SourceSession::netStateName(
                       mzmedia::SourceSession::NetState::Reconnecting)) == "reconnecting");
    MZ_ASSERT_TRUE(std::string(mzmedia::SourceSession::netStateName(
                       mzmedia::SourceSession::NetState::Failed)) == "failed");
    // 四个状态**互不相同**（否则观测出"两种状态同一个名字"）
    MZ_ASSERT_TRUE(std::string(mzmedia::SourceSession::netStateName(
                       mzmedia::SourceSession::NetState::Failed)) !=
                   std::string(mzmedia::SourceSession::netStateName(
                       mzmedia::SourceSession::NetState::Reconnecting)));
}

MZ_TEST(pipeline_transcode_stage_owns_timeline_and_flushes) {
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }
    mzmedia::Demuxer probe;
    MZ_ASSERT_TRUE(probe.open(path));
    const mzmedia::StreamInfo *video = probe.firstVideo();
    MZ_ASSERT_NOT_NULL(video);
    if (video == nullptr) {
        return;
    }
    mzmedia::TranscodeStage::Config config;
    config.out_height = 120;
    mzmedia::TranscodeStage stage(config);
    mzmedia::StageContext ctx;
    mzmedia::StreamInfo out;
    std::string error;
    MZ_ASSERT_TRUE(stage.prepare(*video, ctx, &out, &error));
    // ★ 这条是"不许双计"的契约：转码自己生成时间轴，生产者不能再加趟偏移
    MZ_ASSERT_TRUE(stage.ownsTimeline());
    MZ_ASSERT_EQ(out.width, 160);   // 320x240 → 160x120
    MZ_ASSERT_EQ(out.height, 120);
    MZ_ASSERT_NOT_NULL(out.extradata.get());

    // 喂完样本的所有视频包，然后 flush：进出包数必须相等（一帧不少）
    int fed = 0;
    std::vector<mzmedia::MediaPacket::Ptr> produced;
    while (true) {
        const mzmedia::Demuxer::ReadResult r = probe.readPacket();
        if (r == mzmedia::Demuxer::ReadResult::Eof) {
            break;
        }
        MZ_ASSERT_EQ(r, mzmedia::Demuxer::ReadResult::Packet);
        if (probe.packetStreamIndex() != video->index) {
            continue;
        }
        const AVPacket *pkt = probe.packet();
        if (pkt == nullptr || pkt->data == nullptr || pkt->size <= 0) {
            continue;
        }
        int64_t pts = 0;
        int64_t dts = 0;
        (void) probe.packetTimestampsMs(&pts, &dts);
        std::vector<uint8_t> bytes(pkt->data, pkt->data + pkt->size);
        auto in = makePacket(mzmedia::MediaKind::Video, video->index, std::move(bytes),
                             (pkt->flags & AV_PKT_FLAG_KEY) != 0, dts, pts);
        MZ_ASSERT_TRUE(stage.process(*in, &produced, &error));
        ++fed;
    }
    MZ_ASSERT_GT(fed, 0);
    std::vector<mzmedia::MediaPacket::Ptr> flushed;
    MZ_ASSERT_TRUE(stage.flush(&flushed, &error));
    MZ_ASSERT_TRUE(stage.flush(&flushed, &error)); // 幂等
    for (auto &pkt : flushed) {
        produced.push_back(std::move(pkt));
    }
    MZ_ASSERT_EQ(stage.inputs(), static_cast<uint64_t>(fed));
    MZ_ASSERT_EQ(stage.outputs(), static_cast<uint64_t>(fed)); // **一帧不少**
    MZ_ASSERT_EQ(produced.size(), static_cast<size_t>(fed));
    MZ_ASSERT_GT(stage.outMediaMs(), 0u);
    MZ_ASSERT_GT(stage.costMs(), 0);
    MZ_ASSERT_GT(stage.speed(), 0.0);
    // statsJson 是 `/api/stats` 的片段：必须能被 JSON 解析器吃下（这里只查关键字段）
    const std::string stats = stage.statsJson();
    MZ_ASSERT_TRUE(stats.find("\"enabled\":true") != std::string::npos);
    MZ_ASSERT_TRUE(stats.find("\"speed\":") != std::string::npos);
}
