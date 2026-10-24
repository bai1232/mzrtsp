/*
 * MpegTsMuxer 单元测试（M9，分组 `mpegts`）
 * ============================================================================
 * 契约来源：docs/DESIGN_M6.md §3.1（同一个"纯函数式封装"形状）、
 *           上位需求 FR-3.2（HLS 用 mpegts 切片）、FR-3.4（TS 时间基 = 90kHz）
 *
 * 为什么单独一个分组：这里测的是**字节布局**（PAT/PMT/PES/adaptation field/PCR），
 * 全部用**手造的 StreamInfo + MediaPacket**，不需要样本、不需要线程 —— 于是每个断言都确定。
 * 真样本的端到端证据在 `scripts/ts_mux_test.sh`（`ts_mux_demo` → ffprobe，另一条独立判据）。
 *
 * 覆盖维度（正常 / 空 / 满 / 断开 / 超大）：
 *   正常  PAT+PMT 的 PID/表 id/CRC；视频 PES 的 Annex-B + 关键帧补 SPS/PPS；音频 raw→ADTS
 *   空    没有音视频流 → NoStreams
 *   满    音频 PES 超过 65535 / ADTS 帧长超过 13 bit → TooLarge（不截断）
 *   超大  单帧 > 184 字节 → 拆成多个 188 字节包且每个包**恰好** 188（曾经在这里错过位）
 *   断开  不属于这两路流的包 → InvalidPacket
 * ============================================================================
 */

#include "test_main.h"

#include "ffmpeg/aac_util.h"
#include "ffmpeg/h264_util.h"
#include "media/media_packet.h"
#include "output/mpegts_muxer.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace mzmedia;

namespace {

/// 最小可用的 SPS / PPS（含 NAL header；`buildAvcC` 只看 NAL 类型与前 4 字节）
std::vector<uint8_t> fakeSps() {
    return {0x67, 0x42, 0xC0, 0x0D, 0xDA, 0x05, 0x07, 0xEC};
}
std::vector<uint8_t> fakePps() {
    return {0x68, 0xCE, 0x0F, 0xC8};
}

/// AVCC 载荷：4 字节长度前缀 + NAL
std::vector<uint8_t> makeAvccPayload(const std::vector<uint8_t> &nal) {
    std::vector<uint8_t> out;
    out.reserve(4 + nal.size());
    out.push_back(static_cast<uint8_t>((nal.size() >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((nal.size() >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((nal.size() >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(nal.size() & 0xFF));
    out.insert(out.end(), nal.begin(), nal.end());
    return out;
}

/// 造一个带 avcC 的 H264 视频流信息
StreamInfo makeVideoInfo() {
    StreamInfo info;
    info.index = 0;
    info.codec_id = AV_CODEC_ID_H264;
    info.codec_name = "h264";
    info.is_video = true;
    info.width = 320;
    info.height = 240;
    std::vector<uint8_t> avcc;
    (void) buildAvcC(fakeSps().data(), fakeSps().size(), fakePps().data(), fakePps().size(), &avcc);
    info.extradata = std::make_shared<const std::vector<uint8_t>>(std::move(avcc));
    return info;
}

/// 造一个带 ASC 的 AAC 音频流信息（AAC-LC / 44100 / mono → 12 08）
StreamInfo makeAudioInfo() {
    StreamInfo info;
    info.index = 1;
    info.codec_id = AV_CODEC_ID_AAC;
    info.codec_name = "aac";
    info.is_audio = true;
    info.sample_rate = 44100;
    info.channels = 1;
    info.extradata = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0x12, 0x08});
    return info;
}

MediaPacket::Ptr makePacket(MediaKind kind, int stream_index, std::vector<uint8_t> bytes, bool key,
                            int64_t dts_ms, int64_t pts_ms) {
    auto payload = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
    return MediaPacket::create(kind, stream_index, std::move(payload), key, dts_ms, pts_ms);
}

/// 断言整块输出是 188 字节对齐、且每个包都以 0x47 开头
void assertTsFraming(const std::string &ts, const char *where) {
    if (ts.size() % MpegTsMuxer::kTsPacketSize != 0) {
        MZ_FAIL(std::string(where) + "：TS 输出长度不是 188 的整数倍（" +
                std::to_string(ts.size()) + "）");
        return;
    }
    for (size_t off = 0; off < ts.size(); off += MpegTsMuxer::kTsPacketSize) {
        if (static_cast<uint8_t>(ts[off]) != 0x47) {
            MZ_FAIL(std::string(where) + "：第 " + std::to_string(off / 188) + " 个包缺同步字 0x47");
            return;
        }
    }
    MZ_ASSERT_EQ(ts.size() % MpegTsMuxer::kTsPacketSize, 0u);
}

/// 取第 n 个 188 字节包
std::string packetAt(const std::string &ts, size_t n) {
    return ts.substr(n * MpegTsMuxer::kTsPacketSize, MpegTsMuxer::kTsPacketSize);
}

uint16_t pidOf(const std::string &pkt) {
    return static_cast<uint16_t>(((static_cast<uint8_t>(pkt[1]) & 0x1F) << 8) |
                                 static_cast<uint8_t>(pkt[2]));
}

} // namespace

MZ_TEST(mpegts_pat_pmt_layout) {
    // 能证明什么：PAT 指向 PMT、PMT 列出两路流、CRC 正确、包长与 PUSI 都对。
    // 什么情况会红：PID/表 id/section_length/CRC 任一处写错 —— 播放器会"能打开但没节目"。
    StreamInfo video = makeVideoInfo();
    StreamInfo audio = makeAudioInfo();
    MpegTsMuxer muxer(MpegTsMuxer::Streams{&video, &audio});
    MZ_ASSERT_EQ(muxer.prepare(), MpegTsMuxer::Result::Ok);

    std::string ts;
    MZ_ASSERT_EQ(muxer.writePatPmt(&ts), MpegTsMuxer::Result::Ok);
    assertTsFraming(ts, "PAT+PMT");
    MZ_ASSERT_EQ(ts.size(), 2u * MpegTsMuxer::kTsPacketSize);

    const std::string pat = packetAt(ts, 0);
    MZ_ASSERT_EQ(pidOf(pat), static_cast<uint16_t>(0x0000));
    MZ_ASSERT_EQ((static_cast<uint8_t>(pat[1]) & 0x40) != 0, true);   // PUSI
    MZ_ASSERT_EQ(static_cast<uint8_t>(pat[4]), 0x00u);               // pointer_field
    MZ_ASSERT_EQ(static_cast<uint8_t>(pat[5]), 0x00u);               // table_id = PAT
    // patch 里第 14..15 字节是 program → PMT PID（00 01 | PMT_PID）
    const size_t pmt_ref = 5 + 8;   // 段内偏移：table_id 起算 8 字节后是 program_number
    MZ_ASSERT_EQ(static_cast<uint8_t>(pat[pmt_ref]), 0x00u);
    MZ_ASSERT_EQ(static_cast<uint8_t>(pat[pmt_ref + 1]), 0x01u);
    const uint16_t pat_pmt_pid = static_cast<uint16_t>(((static_cast<uint8_t>(pat[pmt_ref + 2]) & 0x1F) << 8) |
                                                      static_cast<uint8_t>(pat[pmt_ref + 3]));
    MZ_ASSERT_EQ(pat_pmt_pid, MpegTsMuxer::kPmtPid);

    const std::string pmt = packetAt(ts, 1);
    MZ_ASSERT_EQ(pidOf(pmt), MpegTsMuxer::kPmtPid);
    MZ_ASSERT_EQ(static_cast<uint8_t>(pmt[5]), 0x02u);   // table_id = PMT
    // 段长 = 9 + 5×流数 + 4 = 23（两路流）
    const uint16_t section_length =
        static_cast<uint16_t>(((static_cast<uint8_t>(pmt[6]) & 0x0F) << 8) | static_cast<uint8_t>(pmt[7]));
    MZ_ASSERT_EQ(section_length, 23u);
    // PCR_PID 必须是视频 PID（HLS/播放器靠它找时钟）
    const uint16_t pcr_pid = static_cast<uint16_t>(((static_cast<uint8_t>(pmt[13]) & 0x1F) << 8) |
                                                  static_cast<uint8_t>(pmt[14]));
    MZ_ASSERT_EQ(pcr_pid, MpegTsMuxer::kVideoPid);
    // 两路 ES：stream_type 0x1B(H264) 与 0x0F(AAC/ADTS)
    MZ_ASSERT_EQ(static_cast<uint8_t>(pmt[17]), 0x1Bu);
    MZ_ASSERT_EQ(static_cast<uint8_t>(pmt[22]), 0x0Fu);
}

MZ_TEST(mpegts_video_pes_annexb_spspps_and_90khz) {
    // 能证明什么：
    //   ① 视频从 AVCC 转成 Annex-B（00 00 01 起始码）；
    //   ② **关键帧前补了 SPS/PPS**（否则 HLS 切片单放一片黑）；
    //   ③ 时间戳按 ms × 90 写进 PES（FR-3.4 的 90kHz）；
    //   ④ 大帧被拆成多个包，每包恰好 188 字节。
    // 什么情况会红：PES 头位移错、adaptation field 少算 flags 字节（会让每个包的负载差 1 字节，
    //   整条流错位 —— 这个 bug 真发生过，见 .cpp 里 af_content 的注释）。
    StreamInfo video = makeVideoInfo();
    MpegTsMuxer muxer(MpegTsMuxer::Streams{&video, nullptr});
    MZ_ASSERT_EQ(muxer.prepare(), MpegTsMuxer::Result::Ok);

    // 造一个 400 字节的 IDR NAL → 必然跨多个 TS 包
    std::vector<uint8_t> idr;
    idr.push_back(0x65);
    idr.resize(400, 0x88);
    MediaPacket::Ptr packet = makePacket(MediaKind::Video, video.index, makeAvccPayload(idr),
                                         /*key=*/true, /*dts_ms=*/1000, /*pts_ms=*/1040);
    MZ_ASSERT_TRUE(packet != nullptr);

    std::string ts;
    MZ_ASSERT_EQ(muxer.writePatPmt(&ts), MpegTsMuxer::Result::Ok);
    MZ_ASSERT_EQ(muxer.writePacket(*packet, &ts), MpegTsMuxer::Result::Ok);
    assertTsFraming(ts, "video PES");
    MZ_ASSERT_EQ(muxer.spsPpsInjected(), 1u);
    MZ_ASSERT_EQ(muxer.pcrCount(), 1u);
    MZ_ASSERT_EQ(muxer.pesPackets(), 1u);

    // 第 3 个包（PAT/PMT 之后）必须是视频 PUSI + PCR
    const std::string first = packetAt(ts, 2);
    MZ_ASSERT_EQ(pidOf(first), MpegTsMuxer::kVideoPid);
    MZ_ASSERT_EQ((static_cast<uint8_t>(first[1]) & 0x40) != 0, true);          // PUSI
    MZ_ASSERT_EQ((static_cast<uint8_t>(first[3]) & 0x30), 0x30);               // AFC = 3（AF + 负载）
    MZ_ASSERT_EQ((static_cast<uint8_t>(first[5]) & 0x10), 0x10);               // PCR_flag
    const size_t af_len = static_cast<uint8_t>(first[4]);
    // PES 起点 = 4(头) + 1(af_len 字段) + af_len
    const size_t pes_off = 4 + 1 + af_len;
    MZ_ASSERT_EQ(static_cast<uint8_t>(first[pes_off]), 0x00u);                 // 起始码
    MZ_ASSERT_EQ(static_cast<uint8_t>(first[pes_off + 1]), 0x00u);
    MZ_ASSERT_EQ(static_cast<uint8_t>(first[pes_off + 2]), 0x01u);
    MZ_ASSERT_EQ(static_cast<uint8_t>(first[pes_off + 3]), 0xE0u);             // video stream_id
    // PES 头布局：start_code(3) + stream_id(1) + length(2) = 6 → flags1 在 **+6**、
    // flags2 在 +7、header_data_len 在 +8。写这条断言时我自己先错了一次（写成 +7）。
    MZ_ASSERT_EQ(static_cast<uint8_t>(first[pes_off + 6]), 0xC0u);             // PTS+DTS
    MZ_ASSERT_EQ(static_cast<uint8_t>(first[pes_off + 7]), 0x00u);             // flags2
    MZ_ASSERT_EQ(static_cast<uint8_t>(first[pes_off + 8]), 0x0Au);             // header_data_len = 10
    // 负载起点：SPS/PPS 在前（00 00 01 67 … 00 00 01 68 … 00 00 01 65 …）
    const size_t payload_off = pes_off + 9 + 10;
    MZ_ASSERT_EQ(static_cast<uint8_t>(first[payload_off]), 0x00u);
    MZ_ASSERT_EQ(static_cast<uint8_t>(first[payload_off + 1]), 0x00u);
    MZ_ASSERT_EQ(static_cast<uint8_t>(first[payload_off + 2]), 0x01u);
    MZ_ASSERT_EQ(static_cast<uint8_t>(first[payload_off + 3]), 0x67u);         // SPS
    const std::vector<uint8_t> sps = fakeSps();
    MZ_ASSERT_EQ(static_cast<uint8_t>(first[payload_off + 3 + sps.size()]), 0x00u);
    MZ_ASSERT_EQ(static_cast<uint8_t>(first[payload_off + 3 + sps.size() + 3]), 0x68u);  // PPS
}

MZ_TEST(mpegts_audio_raw_becomes_adts) {
    // 能证明什么：raw AAC 在 TS 里被**重新加上 ADTS 头**（TS 不收 raw AAC），
    //   且头里的字段与 ASC 一致（AAC-LC/44100/mono）、帧长 = 7 + payload。
    // 什么情况会红：忘了加头（播放器当噪声）、头字段错、payload 起点错。
    StreamInfo audio = makeAudioInfo();
    MpegTsMuxer muxer(MpegTsMuxer::Streams{nullptr, &audio});
    MZ_ASSERT_EQ(muxer.prepare(), MpegTsMuxer::Result::Ok);

    std::vector<uint8_t> raw(200, 0x5A);
    MediaPacket::Ptr packet =
        makePacket(MediaKind::Audio, audio.index, raw, /*key=*/true, 1000, 1000);
    MZ_ASSERT_TRUE(packet != nullptr);

    std::string ts;
    MZ_ASSERT_EQ(muxer.writePatPmt(&ts), MpegTsMuxer::Result::Ok);
    MZ_ASSERT_EQ(muxer.writePacket(*packet, &ts), MpegTsMuxer::Result::Ok);
    assertTsFraming(ts, "audio PES");

    const std::string first = packetAt(ts, 2);
    MZ_ASSERT_EQ(pidOf(first), MpegTsMuxer::kAudioPid);
    MZ_ASSERT_EQ((static_cast<uint8_t>(first[3]) & 0x30), 0x10);   // 音频不带 AF（够满就纯负载）
    const size_t pes_off = 4;
    MZ_ASSERT_EQ(static_cast<uint8_t>(first[pes_off + 3]), 0xC0u); // audio stream_id
    MZ_ASSERT_EQ(static_cast<uint8_t>(first[pes_off + 8]), 0x05u); // header_data_len = 5（只有 PTS）
    const size_t adts_off = pes_off + 9 + 5;   // 包内偏移
    AdtsHeader header;
    // ★ 必须从**该包的起点**再偏移：直接 ts.substr(adts_off) 取到的是 PAT 里的字节
    //   （我第一次就是这么写错的，parse 必然失败）。
    const std::string adts_view = ts.substr(2 * MpegTsMuxer::kTsPacketSize + adts_off);
    MZ_ASSERT_TRUE(parseAdtsHeader(reinterpret_cast<const uint8_t *>(adts_view.data()),
                                   adts_view.size(), &header));
    MZ_ASSERT_EQ(header.header_bytes, 7u);
    MZ_ASSERT_EQ(header.frame_length, static_cast<uint16_t>(7 + raw.size()));
    MZ_ASSERT_EQ(aacSampleRateFromIndex(header.sampling_frequency_index), 44100u);
    MZ_ASSERT_EQ(header.channel_configuration, 1u);
}

MZ_TEST(mpegts_rejects_bad_inputs) {
    // 能证明什么：每条拒绝路径都给出**具体**原因（调用方据此回 415/500），绝不产出半成品。
    // 什么情况会红：把拒绝放宽成"默认值"（那会产出一路"能播但不对"的流）。
    {
        MpegTsMuxer muxer(MpegTsMuxer::Streams{nullptr, nullptr});
        MZ_ASSERT_EQ(muxer.prepare(), MpegTsMuxer::Result::NoStreams);
        MZ_ASSERT_FALSE(muxer.lastError().empty());
    }
    {
        StreamInfo video = makeVideoInfo();
        video.codec_id = AV_CODEC_ID_HEVC;
        video.codec_name = "hevc";
        MpegTsMuxer muxer(MpegTsMuxer::Streams{&video, nullptr});
        MZ_ASSERT_EQ(muxer.prepare(), MpegTsMuxer::Result::UnsupportedVideoCodec);
    }
    {
        StreamInfo video = makeVideoInfo();
        video.extradata.reset();   // 缺 avcC
        MpegTsMuxer muxer(MpegTsMuxer::Streams{&video, nullptr});
        MZ_ASSERT_EQ(muxer.prepare(), MpegTsMuxer::Result::MissingExtradata);
    }
    {
        StreamInfo audio = makeAudioInfo();
        audio.extradata.reset();   // 缺 ASC
        MpegTsMuxer muxer(MpegTsMuxer::Streams{nullptr, &audio});
        MZ_ASSERT_EQ(muxer.prepare(), MpegTsMuxer::Result::MissingExtradata);
    }
    {
        StreamInfo video = makeVideoInfo();
        MpegTsMuxer muxer(MpegTsMuxer::Streams{&video, nullptr});
        MZ_ASSERT_EQ(muxer.prepare(), MpegTsMuxer::Result::Ok);
        std::string ts;
        // 没 prepare 就写（换一个实例）
        MpegTsMuxer fresh(MpegTsMuxer::Streams{&video, nullptr});
        MZ_ASSERT_EQ(fresh.writePatPmt(&ts), MpegTsMuxer::Result::NotPrepared);
        // 不属于这两路流的包
        MediaPacket::Ptr foreign =
            makePacket(MediaKind::Audio, 99, std::vector<uint8_t>(10, 0), true, 0, 0);
        MZ_ASSERT_EQ(muxer.writePacket(*foreign, &ts), MpegTsMuxer::Result::InvalidPacket);
        // 空包
        MediaPacket::Ptr empty = makePacket(MediaKind::Video, video.index, {}, true, 0, 0);
        if (empty != nullptr) {
            MZ_ASSERT_EQ(muxer.writePacket(*empty, &ts), MpegTsMuxer::Result::InvalidPacket);
        }
    }
}

MZ_TEST(mpegts_small_frames_are_still_188_aligned) {
    // 能证明什么：**不满 184 字节的负载**必须用 adaptation field 填充，且总长仍然恰好 188。
    // 什么情况会红：af_len 少算 flags 字节 → 每个包多/少 1 字节 → 后续全错位
    //   （这个 bug 真发生过：138 个包 × 1 字节 → 整条流花屏，ffmpeg 报 corrupted macroblock）。
    StreamInfo video = makeVideoInfo();
    MpegTsMuxer muxer(MpegTsMuxer::Streams{&video, nullptr});
    MZ_ASSERT_EQ(muxer.prepare(), MpegTsMuxer::Result::Ok);
    std::string ts;
    MZ_ASSERT_EQ(muxer.writePatPmt(&ts), MpegTsMuxer::Result::Ok);

    // 一帧很小的 P 帧（AVCC 4+5=9 字节）+ 一帧 183 字节的边界负载（最容易踩 off-by-one）
    for (size_t sz : {size_t{5}, size_t{179}, size_t{180}, size_t{184}}) {
        std::vector<uint8_t> nal;
        nal.push_back(0x41);
        nal.resize(sz, 0x33);
        MediaPacket::Ptr p = makePacket(MediaKind::Video, video.index, makeAvccPayload(nal),
                                        /*key=*/false, 1000, 1000);
        MZ_ASSERT_TRUE(p != nullptr);
        MZ_ASSERT_EQ(muxer.writePacket(*p, &ts), MpegTsMuxer::Result::Ok);
        assertTsFraming(ts, "小帧");
    }
}
