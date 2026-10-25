/*
 * Transcoder 单元测试（M10-a，分组 `transcode`）
 * ============================================================================
 * 契约来源：docs/DESIGN_M9.md（同一条"纯函数式加工"的纪律）、NFR-5、SPEC §9（不做音频转码）
 *
 * 覆盖维度：
 *   正常  用真样本转码：输出宽高/avcC 形态/帧数不丢/关键帧存在
 *   空    没有视频流 / 空包 → 明确失败
 *   满    单次产出上限（max_out_packets）非法 → 拒绝；输出包数受控
 *   断开  flush() 幂等；flush 后还能拿到残余包（不静默丢最后几帧）
 *   超大  不认识的编码（VP9）/ 缺初始化数据 / 只给宽不给高 → 全部明确拒绝
 *         【M10-b 改动】HEVC 现在**可以进**（解码成 H264 是 M10-b 的一部分），
 *         但仍然要求带初始化数据（hvcC/Annex-B）—— "HEVC 的 TS 裸流"依旧明确拒绝
 * ============================================================================
 */

#include "test_main.h"

#include "core/util.h"
#include "ffmpeg/demuxer.h"
#include "ffmpeg/h264_util.h"
#include "ffmpeg/transcoder.h"
#include "media/demuxer_producer.h"
#include "media/source_pump.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace mzmedia;

namespace {

/// 找样本（与其它用例同一算法：与 cwd 无关，向上最多 8 层）
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
    for (const auto &path : candidates) {
        if (FILE *f = ::fopen(path.c_str(), "rb"); f != nullptr) {
            ::fclose(f);
            return path;
        }
    }
    return {};
}

/// AVCC 载荷的结构检查：每个 NAL 都要被 4 字节长度前缀完整覆盖
bool isAvcc(const std::vector<uint8_t> &data, int *nal_count) {
    size_t pos = 0;
    int count = 0;
    while (pos + 4 <= data.size()) {
        const size_t len = (static_cast<size_t>(data[pos]) << 24) |
                           (static_cast<size_t>(data[pos + 1]) << 16) |
                           (static_cast<size_t>(data[pos + 2]) << 8) |
                           static_cast<size_t>(data[pos + 3]);
        pos += 4;
        if (len == 0 || pos + len > data.size()) {
            return false;
        }
        pos += len;
        ++count;
    }
    if (nal_count != nullptr) {
        *nal_count = count;
    }
    return pos == data.size() && count > 0;
}

} // namespace

MZ_TEST(transcode_rejects_bad_inputs) {
    // 能证明什么：每条拒绝路径都给出**具体** Result/日志，绝不产出半成品。
    // 什么情况会红：把校验放宽成"默认值"（例如不检查 avcC、或允许 0 = 无界）。
    StreamInfo video;
    video.index = 0;
    video.codec_id = AV_CODEC_ID_H264;
    video.codec_name = "h264";
    video.is_video = true;
    video.width = 320;
    video.height = 240;
    std::vector<uint8_t> sps = {0x67, 0x42, 0xC0, 0x0D, 0xDA, 0x05};
    std::vector<uint8_t> pps = {0x68, 0xCE, 0x0F, 0xC8};
    std::vector<uint8_t> avcc;
    MZ_ASSERT_TRUE(buildAvcC(sps.data(), sps.size(), pps.data(), pps.size(), &avcc));
    video.extradata = std::make_shared<const std::vector<uint8_t>>(avcc);

    Transcoder::Config ok_config;
    // ① 不认识的编码（VP9）：明确拒绝。**注意**：HEVC 在 M10-b 起是**合法输入**
    //    （解码 HEVC 再编 H264 就是"转码"本身），所以这里不再拿 HEVC 当作拒绝样本；
    //    HEVC 的边界改成"**没有初始化数据**就必须拒绝"（TS 裸流那条路）。
    StreamInfo vp9 = video;
    vp9.codec_id = AV_CODEC_ID_VP9;
    vp9.codec_name = "vp9";
    MZ_ASSERT_TRUE(Transcoder::create(vp9, ok_config) == nullptr);

    // ①b HEVC 但缺初始化数据（hvcC）：明确拒绝 —— 没它解不了码，猜不得
    StreamInfo hevc_no_extra = video;
    hevc_no_extra.codec_id = AV_CODEC_ID_HEVC;
    hevc_no_extra.codec_name = "hevc";
    hevc_no_extra.extradata.reset();
    MZ_ASSERT_TRUE(Transcoder::create(hevc_no_extra, ok_config) == nullptr);
    // ② 缺 avcC
    StreamInfo no_extra = video;
    no_extra.extradata.reset();
    MZ_ASSERT_TRUE(Transcoder::create(no_extra, ok_config) == nullptr);
    // ③ 只给宽不给高
    Transcoder::Config half;
    half.out_width = 640;
    MZ_ASSERT_TRUE(Transcoder::create(video, half) == nullptr);
    // ④ 0 不是无界
    Transcoder::Config zero_out;
    zero_out.max_out_packets = 0;
    MZ_ASSERT_TRUE(Transcoder::create(video, zero_out) == nullptr);
    Transcoder::Config zero_gop;
    zero_gop.gop_frames = 0;
    MZ_ASSERT_TRUE(Transcoder::create(video, zero_gop) == nullptr);
    // ⑤ 合法配置能建起来，且输出流信息是"输出层要的形状"
    Transcoder::Ptr good = Transcoder::create(video, ok_config);
    MZ_ASSERT_TRUE(good != nullptr);
    if (good) {
        MZ_ASSERT_EQ(good->videoInfo().width, 320);
        MZ_ASSERT_EQ(good->videoInfo().height, 240);
        MZ_ASSERT_EQ(good->videoInfo().index, video.index); // 同一个 stream index
        MZ_ASSERT_TRUE(good->videoInfo().extradata != nullptr);
        if (good->videoInfo().extradata != nullptr) {
            // avcC 的首字节必须是 configurationVersion = 1（Annex-B 会以 0x00 开头）
            MZ_ASSERT_EQ((*good->videoInfo().extradata)[0], 0x01u);
        }
        MZ_ASSERT_FALSE(good->scaling()); // 同尺寸同像素格式 → 不建 sws
    }
}

MZ_TEST(transcode_sample_scales_without_losing_frames) {
    // 能证明什么：真样本 320x240 → 160x120 转码后
    //   ① 每个输出包都是合法 AVCC；② 帧数一帧不少（解码数 == 编码数）；
    //   ③ 关键帧存在且第一包就是关键帧；④ 时间轴单调（自己的 pts 生成器没写错）。
    // 什么情况会红：漏了 flush（少最后几帧）、时间截钳错（dts 回退）、转换错（AVCC 结构坏）。
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }
    DemuxerProducer producer;
    MZ_ASSERT_TRUE(producer.open(path));
    const StreamInfo *src_video = producer.muxerVideo();
    MZ_ASSERT_NOT_NULL(src_video);
    if (src_video == nullptr) {
        return;
    }

    Transcoder::Config config;
    config.out_width = 160;
    config.out_height = 120;
    Transcoder::Ptr transcoder = Transcoder::create(*src_video, config);
    MZ_ASSERT_TRUE(transcoder != nullptr);
    if (!transcoder) {
        return;
    }
    // 注意：sws 是**懒创建**的（第一帧解码出来才知道解码器的像素格式）→ 此刻还没建
    MZ_ASSERT_FALSE(transcoder->scaling());
    MZ_ASSERT_EQ(transcoder->videoInfo().width, 160);

    std::vector<Transcoder::OutPacket> outs;
    uint64_t in_packets = 0;
    uint64_t out_packets = 0;
    uint64_t key_packets = 0;
    int64_t last_dts = -1;
    bool monotonic = true;
    bool avcc_ok = true;
    const auto consume = [&](const std::vector<Transcoder::OutPacket> &items) {
        for (const auto &item : items) {
            int nals = 0;
            if (!isAvcc(*item.data, &nals)) {
                avcc_ok = false;
            }
            if (item.dts_ms < last_dts) {
                monotonic = false;
            }
            last_dts = item.dts_ms;
            if (item.key) {
                ++key_packets;
            }
            ++out_packets;
        }
    };

    for (;;) {
        MediaPacket::Ptr packet;
        std::string error;
        const SourcePump::ReadResult result = producer.read(&packet, &error);
        if (result == SourcePump::ReadResult::EndOfStream) {
            break;
        }
        MZ_ASSERT_TRUE(result == SourcePump::ReadResult::Packet);
        if (result != SourcePump::ReadResult::Packet || !packet) {
            return;
        }
        if (packet->kind() != MediaKind::Video) {
            continue;
        }
        ++in_packets;
        outs.clear();
        MZ_ASSERT_EQ(transcoder->transcode(packet->data(), packet->size(), packet->dtsMs(),
                                          packet->ptsMs(), &outs),
                     Transcoder::Result::Ok);
        consume(outs);
    }
    outs.clear();
    MZ_ASSERT_EQ(transcoder->flush(&outs), Transcoder::Result::Ok); // EOF 冲残余
    consume(outs);
    // flush 幂等
    outs.clear();
    MZ_ASSERT_EQ(transcoder->flush(&outs), Transcoder::Result::Ok);
    MZ_ASSERT_EQ(outs.size(), 0u);

    // 喂过帧之后，缩放器必须已经建起来（尺寸确实变了）
    MZ_ASSERT_TRUE(transcoder->scaling());
    MZ_ASSERT_EQ(transcoder->scaledFrames(), in_packets);
    MZ_ASSERT_TRUE(avcc_ok);
    MZ_ASSERT_TRUE(monotonic);
    MZ_ASSERT_EQ(out_packets, in_packets);          // 一帧不多、一帧不少（H264 无 B 帧）
    MZ_ASSERT_EQ(transcoder->encodedPackets(), out_packets);
    MZ_ASSERT_GT(key_packets, 0u);
    MZ_ASSERT_GT(transcoder->outMediaMs(), 0u);
    MZ_ASSERT_FALSE(transcoder->vfrDetected());     // CFR 样本不应报 VFR
}

MZ_TEST(transcode_empty_packet_is_rejected) {
    // 能证明什么：空包/空指针**明确失败**（而不是让解码器崩或者静默返回 0 个包）。
    const std::string path = samplePath("sample.mp4");
    MZ_ASSERT_FALSE(path.empty());
    if (path.empty()) {
        return;
    }
    DemuxerProducer producer;
    MZ_ASSERT_TRUE(producer.open(path));
    const StreamInfo *src_video = producer.muxerVideo();
    if (src_video == nullptr) {
        return;
    }
    Transcoder::Ptr transcoder = Transcoder::create(*src_video, Transcoder::Config{});
    MZ_ASSERT_TRUE(transcoder != nullptr);
    if (!transcoder) {
        return;
    }
    std::vector<Transcoder::OutPacket> outs;
    MZ_ASSERT_EQ(transcoder->transcode(nullptr, 0, 0, 0, &outs), Transcoder::Result::BadStream);
    MZ_ASSERT_EQ(transcoder->transcode(nullptr, 16, 0, 0, &outs), Transcoder::Result::BadStream);
    MZ_ASSERT_EQ(transcoder->transcode(reinterpret_cast<const uint8_t *>("x"), 0, 0, 0, &outs),
                 Transcoder::Result::BadStream);
    MZ_ASSERT_FALSE(transcoder->lastError().empty());
}
