/*
 * RTSP **输出**（v0.4-a / R1）单测：协议文本与 RTP 打包
 * ============================================================================
 * 这一组只测"不依赖网络、可逐字节断言"的三块：
 *   1) SDP 生成（含 RFC 4566 必填的 `c=` 行、sprop/config 与源一致）；
 *   2) AVCC（长度前缀）→ NAL 的零拷贝遍历；
 *   3) RTP 打包（单片 / FU-A 分片数、marker、seq、时间戳原样带入）。
 *
 * 什么会让它们红：
 *   · SDP 少了 `c=` / `sprop-parameter-sets` 与 avcC 不一致 → 真播放器会挑；
 *   · FU-A 分片数按 `nal_len` 而不是 `nal_len - 1` 算 → 多一片、末片负载错位；
 *   · marker 在中间 NAL 上置位 → 接收端把半帧当整帧。
 * ============================================================================
 */

#include "test_main.h"

#include "ffmpeg/h264_util.h"
#include "output/rtp_packetizer.h"
#include "output/rtsp_request.h"
#include "output/rtsp_timing.h"
#include "output/sdp_builder.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

/// 造一段 avcC：version/profile/compat/level/lengthSize=4 + SPS + PPS
std::vector<uint8_t> makeAvcC() {
    const uint8_t sps[] = {0x67, 0x42, 0xC0, 0x1E, 0x8C, 0x8D, 0x40};
    const uint8_t pps[] = {0x68, 0xCE, 0x38, 0x80};
    std::vector<uint8_t> out;
    out.push_back(0x01);       // configurationVersion
    out.push_back(0x42);       // AVCProfileIndication
    out.push_back(0xC0);       // profile_compatibility
    out.push_back(0x1E);       // AVCLevelIndication
    out.push_back(0xFF);       // 111111 + lengthSizeMinusOne=3 → 4 字节
    out.push_back(0xE1);       // 111 + numOfSPS=1
    out.push_back(0);
    out.push_back(static_cast<uint8_t>(sizeof(sps)));
    out.insert(out.end(), std::begin(sps), std::end(sps));
    out.push_back(1); // numOfPPS
    out.push_back(0);
    out.push_back(static_cast<uint8_t>(sizeof(pps)));
    out.insert(out.end(), std::begin(pps), std::end(pps));
    return out;
}

/// 造一段 AVCC 载荷：两个 NAL（长度前缀 4 字节）
std::vector<uint8_t> makeAvccPayload() {
    const std::vector<uint8_t> nal1 = {0x65, 0xAA, 0xBB, 0xCC};       // IDR
    const std::vector<uint8_t> nal2 = {0x41, 0x11, 0x22};             // 非 IDR
    std::vector<uint8_t> out;
    auto put = [&out](uint32_t v) {
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(v & 0xFF));
    };
    put(static_cast<uint32_t>(nal1.size()));
    out.insert(out.end(), nal1.begin(), nal1.end());
    put(static_cast<uint32_t>(nal2.size()));
    out.insert(out.end(), nal2.begin(), nal2.end());
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// SDP
// ---------------------------------------------------------------------------

MZ_TEST(rtspout_sdp_has_connection_line_and_params) {
    mzmedia::StreamInfo video;
    video.index = 0;
    video.is_video = true;
    video.width = 320;
    video.height = 240;
    auto avcc = makeAvcC();
    video.extradata = std::make_shared<std::vector<uint8_t>>(avcc);

    mzmedia::SdpBuilder::Streams streams;
    streams.video = &video;

    std::string error;
    const std::string sdp = mzmedia::SdpBuilder::build(streams, "sample", "192.168.1.10", &error);
    MZ_ASSERT_TRUE(!sdp.empty());
    if (sdp.empty()) {
        return;
    }
    // RFC 4566：会话级必须有 c=（否则 VLC/NVR 可能直接拒）
    MZ_ASSERT_TRUE(sdp.find("c=IN IP4 192.168.1.10\r\n") != std::string::npos);
    MZ_ASSERT_TRUE(sdp.find("m=video 0 RTP/AVP 96\r\n") != std::string::npos);
    MZ_ASSERT_TRUE(sdp.find("a=rtpmap:96 H264/90000\r\n") != std::string::npos);
    // packetization-mode=1（FU-A 需要）+ sprop 来自 avcC + profile-level-id
    MZ_ASSERT_TRUE(sdp.find("packetization-mode=1") != std::string::npos);
    MZ_ASSERT_TRUE(sdp.find("sprop-parameter-sets=") != std::string::npos);
    MZ_ASSERT_TRUE(sdp.find("profile-level-id=42C01E") != std::string::npos);
    MZ_ASSERT_TRUE(sdp.find("a=control:trackID=0\r\n") != std::string::npos);
    // 没有音频轨时不该出现 m=audio
    MZ_ASSERT_TRUE(sdp.find("m=audio") == std::string::npos);
}

MZ_TEST(rtspout_sdp_requires_ip_and_avcc) {
    mzmedia::StreamInfo video;
    video.is_video = true;
    mzmedia::SdpBuilder::Streams streams;
    streams.video = &video;
    std::string error;

    // 没有 avcC → 明确失败（不生成一个客户端解码不了的 SDP）
    MZ_ASSERT_TRUE(mzmedia::SdpBuilder::build(streams, "x", "10.0.0.1", &error).empty());
    MZ_ASSERT_TRUE(!error.empty());

    // 没有 c= 用的 IP → 同样失败
    video.extradata = std::make_shared<std::vector<uint8_t>>(makeAvcC());
    MZ_ASSERT_TRUE(mzmedia::SdpBuilder::build(streams, "x", "", &error).empty());
}

// ---------------------------------------------------------------------------
// AVCC → NAL（零拷贝）
// ---------------------------------------------------------------------------

MZ_TEST(rtspout_avcc_to_nalus_is_zero_copy) {
    const std::vector<uint8_t> payload = makeAvccPayload();
    std::vector<std::pair<const uint8_t *, size_t>> nals;
    const bool ok = mzmedia::avccToNalus(payload.data(), payload.size(), 4,
                                         [&nals](const uint8_t *nal, size_t len) {
                                             nals.emplace_back(nal, len);
                                             return true;
                                         });
    MZ_ASSERT_TRUE(ok);
    MZ_ASSERT_EQ(nals.size(), 2u);
    if (nals.size() == 2u) {
        MZ_ASSERT_EQ(nals[0].second, 4u); // IDR 长度
        MZ_ASSERT_EQ(nals[1].second, 3u);
        // 零拷贝：指针落在原缓冲内部
        MZ_ASSERT_TRUE(nals[0].first >= payload.data());
        MZ_ASSERT_TRUE(nals[0].first < payload.data() + payload.size());
        MZ_ASSERT_EQ(nals[0].first[0], 0x65);
    }
    // 结构坏了要明确失败（前缀为 0）
    const uint8_t bad[] = {0, 0, 0, 0, 0x65};
    MZ_ASSERT_FALSE(mzmedia::avccToNalus(bad, sizeof(bad), 4,
                                         [](const uint8_t *, size_t) { return true; }));
}

// ---------------------------------------------------------------------------
// RTP 打包
// ---------------------------------------------------------------------------

MZ_TEST(rtspout_rtp_single_nal_header) {
    mzmedia::RtpPacketizer::Config config;
    config.payload_type = 96;
    config.ssrc = 0x11223344;
    config.mtu = 1400;
    mzmedia::RtpPacketizer packetizer(config);

    const uint8_t nal[] = {0x65, 0x01, 0x02, 0x03};
    std::vector<std::string> packets;
    packetizer.packetizeNal(nal, sizeof(nal), 90000, true, &packets);
    MZ_ASSERT_EQ(packets.size(), 1u);
    if (packets.empty()) {
        return;
    }
    const auto &packet = packets[0];
    MZ_ASSERT_EQ(static_cast<uint8_t>(packet[0]), 0x80); // V=2
    MZ_ASSERT_EQ(static_cast<uint8_t>(packet[1]), 0x80 | 96); // marker + PT
    MZ_ASSERT_EQ(packet.size(), 12u + sizeof(nal));
    // 时间戳原样带入（大端）
    const uint32_t ts = (static_cast<uint8_t>(packet[4]) << 24) |
                        (static_cast<uint8_t>(packet[5]) << 16) |
                        (static_cast<uint8_t>(packet[6]) << 8) |
                        static_cast<uint8_t>(packet[7]);
    MZ_ASSERT_EQ(ts, 90000u);
    MZ_ASSERT_TRUE(std::memcmp(packet.data() + 12, nal, sizeof(nal)) == 0);
}

MZ_TEST(rtspout_rtp_fua_fragment_count_and_reassembly) {
    mzmedia::RtpPacketizer::Config config;
    config.ssrc = 0xAABBCCDD;
    config.mtu = 1400;
    mzmedia::RtpPacketizer packetizer(config);

    // 4096 字节的 IDR：分片数必须是 ceil((4096-1)/1386) = 3
    std::vector<uint8_t> nal(4096, 0x5A);
    nal[0] = 0x65;
    MZ_ASSERT_EQ(packetizer.fragmentsFor(nal.size()), 3u);

    std::vector<std::string> packets;
    packetizer.packetizeNal(nal.data(), nal.size(), 12345, true, &packets);
    MZ_ASSERT_EQ(packets.size(), 3u);
    if (packets.size() != 3u) {
        return;
    }
    // 首片 S=1/E=0、中间片都是 0、末片 E=1 且 **marker 只在末片**
    for (size_t i = 0; i < packets.size(); ++i) {
        const auto &pkt = packets[i];
        const bool marker = (static_cast<uint8_t>(pkt[1]) & 0x80) != 0;
        MZ_ASSERT_EQ(marker, i + 1 == packets.size());
        MZ_ASSERT_EQ(static_cast<int>(static_cast<uint8_t>(pkt[12]) & 0x1F), 28); // FU-A type
        const uint8_t fu = static_cast<uint8_t>(pkt[13]);
        MZ_ASSERT_EQ((fu & 0x80) != 0, i == 0);                      // S
        MZ_ASSERT_EQ((fu & 0x40) != 0, i + 1 == packets.size());     // E
        MZ_ASSERT_EQ(static_cast<int>(fu & 0x1F), 5);                 // 原 NAL type 带过来
    }
    // 反向重组：片 2..N 的负载 + 原 NAL 头 == 原 NAL
    std::vector<uint8_t> rebuilt;
    rebuilt.push_back(nal[0]);
    for (const auto &pkt : packets) {
        rebuilt.insert(rebuilt.end(), pkt.begin() + 14, pkt.end());
    }
    MZ_ASSERT_EQ(rebuilt.size(), nal.size());
    MZ_ASSERT_TRUE(std::memcmp(rebuilt.data(), nal.data(), nal.size()) == 0);
}

MZ_TEST(rtspout_aac_au_header) {
    mzmedia::RtpPacketizer::Config config;
    config.payload_type = 97;
    config.ssrc = 1;
    mzmedia::RtpPacketizer packetizer(config);
    const uint8_t au[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    std::vector<std::string> packets;
    packetizer.packetizeAac(au, sizeof(au), 1024, &packets);
    MZ_ASSERT_EQ(packets.size(), 1u);
    if (packets.empty()) {
        return;
    }
    const auto &packet = packets[0];
    MZ_ASSERT_EQ(static_cast<uint8_t>(packet[12]), 0x00); // AU-headers-length 高 8 位
    MZ_ASSERT_EQ(static_cast<uint8_t>(packet[13]), 0x10); // 16 bits
    const uint16_t size13 = (static_cast<uint16_t>(static_cast<uint8_t>(packet[14])) << 8) |
                            static_cast<uint8_t>(packet[15]);
    MZ_ASSERT_EQ(static_cast<size_t>(size13 >> 3), sizeof(au));
}

// ---------------------------------------------------------------------------
// RTSP 请求解析（服务端收到的那些方法）
// ---------------------------------------------------------------------------

MZ_TEST(rtspout_request_setup_and_transport) {
    const std::string text =
        "SETUP rtsp://127.0.0.1:8554/live/sample/trackID=0 RTSP/1.0\r\n"
        "CSeq: 3\r\n"
        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
        "\r\n";
    mzmedia::RtspRequest request;
    MZ_ASSERT_EQ(request.parse(text.data(), text.size()), mzmedia::RtspRequest::Status::Complete);
    MZ_ASSERT_TRUE(request.method() == mzmedia::RtspRequest::Method::Setup);
    MZ_ASSERT_STR_EQ(request.cseq(), "3");
    MZ_ASSERT_STR_EQ(mzmedia::RtspRequest::pathName(request.uri()), "sample");

    int rtp_ch = -1;
    int rtcp_ch = -1;
    MZ_ASSERT_TRUE(mzmedia::RtspRequest::parseInterleaved(request.header("transport"), &rtp_ch, &rtcp_ch));
    MZ_ASSERT_EQ(rtp_ch, 0);
    MZ_ASSERT_EQ(rtcp_ch, 1);

    // 半包：只有请求行 → 必须 NeedMoreData（不是错误，也不能当完整请求处理）
    const std::string partial = "OPTIONS rtsp://127.0.0.1/live/x RTSP/1.0\r\nCSeq: 1\r\n";
    MZ_ASSERT_EQ(request.parse(partial.data(), partial.size()),
                 mzmedia::RtspRequest::Status::NeedMoreData);

    // 版本不对 → Error（不回 200）
    const std::string bad = "PLAY rtsp://127.0.0.1/live/x HTTP/1.1\r\n\r\n";
    MZ_ASSERT_EQ(request.parse(bad.data(), bad.size()), mzmedia::RtspRequest::Status::Error);
}

MZ_TEST(rtspout_request_rejects_bad_name) {
    // 路径穿越/非法字符一律取不到名字（与 /live/ 同一套规则，防注入）
    MZ_ASSERT_STR_EQ(mzmedia::RtspRequest::pathName("rtsp://h/live/../etc/passwd"), "");
    MZ_ASSERT_STR_EQ(mzmedia::RtspRequest::pathName("rtsp://h/live/a%2e%2e/b"), "");
    // UDP 的 SETUP 没有 interleaved → 走 461 分支
    int rtp_ch = 0;
    int rtcp_ch = 0;
    MZ_ASSERT_FALSE(mzmedia::RtspRequest::parseInterleaved("RTP/AVP;unicast;client_port=5000-5001",
                                                          &rtp_ch, &rtcp_ch));
}

// ---------------------------------------------------------------------------
// 【R2】时间戳与 RTP-Info（纯函数）
// ---------------------------------------------------------------------------

MZ_TEST(rtspout_audio_ts_is_sample_count_from_pts) {
    // 音频 RTP 时间戳是**采样数**：0 ms → 起始偏移（1 s 采样数），1000 ms → +44100
    const uint32_t zero = mzmedia::RtspTiming::audioRtpTs(0, 44100);
    MZ_ASSERT_EQ(zero, 44100u);
    MZ_ASSERT_EQ(mzmedia::RtspTiming::audioRtpTs(1000, 44100), zero + 44100u);
    // 23 ms（AAC 一帧 1024 采样 ≈ 23.22 ms）→ +1024 左右（取整误差 ±1）
    const uint32_t frame = mzmedia::RtspTiming::audioRtpTs(23, 44100) - zero;
    MZ_ASSERT_TRUE(frame >= 1013u && frame <= 1015u);
    // 负 pts → 钳到起始偏移（不产生负数）
    MZ_ASSERT_EQ(mzmedia::RtspTiming::audioRtpTs(-50, 44100), zero);
}

MZ_TEST(rtspout_video_ts_uses_pts_with_offset) {
    // 视频 = PTS×90 + 1 s 偏移；0 ms → 90000，40 ms → +3600
    MZ_ASSERT_EQ(mzmedia::RtspTiming::videoRtpTs(0), 90000u);
    MZ_ASSERT_EQ(mzmedia::RtspTiming::videoRtpTs(40), 90000u + 3600u);
    MZ_ASSERT_EQ(mzmedia::RtspTiming::videoRtpTs(-100), 90000u); // 负值钳 0
}

MZ_TEST(rtspout_rtp_info_lists_each_track) {
    std::vector<mzmedia::RtspTiming::TrackInfo> tracks;
    tracks.push_back({0, 120, 90000});
    tracks.push_back({1, 45, 44100});
    const std::string info =
        mzmedia::RtspTiming::buildRtpInfo("rtsp://127.0.0.1:8554/live/sample", tracks);
    MZ_ASSERT_TRUE(info ==
                   "RTP-Info: url=rtsp://127.0.0.1:8554/live/sample/trackID=0;seq=120;rtptime=90000,"
                   "url=rtsp://127.0.0.1:8554/live/sample/trackID=1;seq=45;rtptime=44100\r\n");
}

MZ_TEST(rtspout_sdp_advertises_audio_track) {
    mzmedia::StreamInfo video;
    video.is_video = true;
    video.extradata = std::make_shared<std::vector<uint8_t>>(makeAvcC());
    mzmedia::StreamInfo audio;
    audio.is_audio = true;
    audio.sample_rate = 44100;
    audio.channels = 1;
    audio.extradata = std::make_shared<std::vector<uint8_t>>(
        std::vector<uint8_t>{0x12, 0x08}); // AAC-LC 44.1k mono 的 ASC

    mzmedia::SdpBuilder::Streams streams;
    streams.video = &video;
    streams.audio = &audio;
    std::string error;
    const std::string sdp = mzmedia::SdpBuilder::build(streams, "s", "10.0.0.2", &error);
    MZ_ASSERT_TRUE(!sdp.empty());
    MZ_ASSERT_TRUE(sdp.find("m=audio 0 RTP/AVP 97\r\n") != std::string::npos);
    MZ_ASSERT_TRUE(sdp.find("a=rtpmap:97 MPEG4-GENERIC/44100/1\r\n") != std::string::npos);
    MZ_ASSERT_TRUE(sdp.find("mode=AAC-hbr") != std::string::npos);
    MZ_ASSERT_TRUE(sdp.find("config=1208") != std::string::npos);
    MZ_ASSERT_TRUE(sdp.find("a=control:trackID=1\r\n") != std::string::npos);
}
