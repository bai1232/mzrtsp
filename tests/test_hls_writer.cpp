/*
 * HlsWriter 单元测试（M9-b，分组 `hls`）
 * ============================================================================
 * 契约来源：docs/DESIGN_M9.md §3.2 + FR-3.2（2s / 窗口 6 / MEDIA-SEQUENCE / TARGETDURATION）
 *
 * 为什么能确定性测：`HlsWriter::onPacket()` 是**同步**的（只做切片决策 + 字节封装，
 * 不投递线程），所以可以用手造包精确控制"关键帧在哪、间隔多久"，不需要等墙钟。
 * 端到端（真源线程 + 真 HTTP + ffmpeg 读 m3u8）在 `scripts/hls_test.sh`。
 *
 * 覆盖维度：
 *   正常  按**视频**关键帧切、窗口 6 片滚动、MEDIA-SEQUENCE 前进、URI 形态
 *   空    没有包 → 空播放列表（只有头部，没有 #EXTINF）
 *   满    超过窗口 → 丢**整片**并计数；总量上限同样只丢整片
 *   超大  GOP 比目标长 → 切片变长且 TARGETDURATION 跟着变大（协议要求 ≥ 实际最大值）
 *   断开  EOS → 收尾当前片 + #EXT-X-ENDLIST（幂等）
 * ============================================================================
 */

#include "test_main.h"

#include "ffmpeg/aac_util.h"
#include "ffmpeg/h264_util.h"
#include "media/media_packet.h"
#include "output/hls_service.h"
#include "output/hls_writer.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace mzmedia;

namespace {

std::vector<uint8_t> fakeSps() {
    return {0x67, 0x42, 0xC0, 0x0D, 0xDA, 0x05, 0x07, 0xEC};
}
std::vector<uint8_t> fakePps() {
    return {0x68, 0xCE, 0x0F, 0xC8};
}

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

/// AVCC 载荷（4 字节长度前缀 + NAL）
std::vector<uint8_t> avccPayload(uint8_t nal_header, size_t payload) {
    std::vector<uint8_t> nal(payload, 0x77);
    nal[0] = nal_header;
    std::vector<uint8_t> out;
    out.reserve(4 + nal.size());
    out.push_back(static_cast<uint8_t>((nal.size() >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((nal.size() >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((nal.size() >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(nal.size() & 0xFF));
    out.insert(out.end(), nal.begin(), nal.end());
    return out;
}

MediaPacket::Ptr videoPacket(int index, bool key, int64_t dts_ms, int64_t pts_ms) {
    auto payload = std::make_shared<std::vector<uint8_t>>(
        avccPayload(key ? 0x65 : 0x41, key ? 200 : 100));
    return MediaPacket::create(MediaKind::Video, index, std::move(payload), key, dts_ms, pts_ms);
}

MediaPacket::Ptr audioPacket(int index, int64_t dts_ms) {
    auto payload = std::make_shared<std::vector<uint8_t>>(120, 0x5A);
    // ★ 音频包的 key 标志恒为 true（AV_PKT_FLAG_KEY）—— 这正是"切点必须只看视频关键帧"的理由
    return MediaPacket::create(MediaKind::Audio, index, std::move(payload), true, dts_ms, dts_ms);
}

HlsWriter::Config makeConfig() {
    HlsWriter::Config config;
    config.name = "movie.mp4";
    config.target_duration_ms = 2000;
    config.window = 6;
    config.max_slice_ms = 10000;
    config.max_slice_bytes = 1u << 20;
    config.max_total_bytes = 8u << 20;
    return config;
}

size_t countLines(const std::string &text, const std::string &needle) {
    size_t n = 0;
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

} // namespace

MZ_TEST(hls_cuts_only_on_video_keyframes) {
    // 能证明什么：**音频包的 key 标志不参与切点判断**。
    //   音频每包都带 key 标志；若拿它当切点，会在音频的时间戳上切 → 切片时长错 + 跨片时间戳回退。
    // 什么情况会红：把 `packet.isKeyFrame()` 直接当切点（不判 kind）→ 第一片时长会等于
    //   音频包的 dts（2200）而不是视频关键帧的 dts（4000）。
    StreamInfo video = makeVideoInfo();
    StreamInfo audio = makeAudioInfo();
    HlsWriter::Ptr writer =
        HlsWriter::create(makeConfig(), MpegTsMuxer::Streams{&video, &audio});
    MZ_ASSERT_TRUE(writer != nullptr);
    if (!writer) {
        return;
    }

    MZ_ASSERT_EQ(writer->onPacket(*videoPacket(video.index, true, 0, 0)), HlsWriter::Result::Ok);
    // 音频包（key=true）在 2200ms —— 不允许它触发切点
    MZ_ASSERT_EQ(writer->onPacket(*audioPacket(audio.index, 2200)), HlsWriter::Result::Ok);
    // 视频关键帧在 4000ms → 这里才是切点
    MZ_ASSERT_EQ(writer->onPacket(*videoPacket(video.index, true, 4000, 4000)), HlsWriter::Result::Ok);
    writer->finish();

    MZ_ASSERT_EQ(writer->slicesProduced(), 2u);
    const std::string playlist = writer->playlist();
    MZ_ASSERT_EQ(countLines(playlist, "#EXTINF:"), 2u);
    // 第一片时长应是 4000ms（关键帧间隔），不是 2200ms（音频）
    MZ_ASSERT_TRUE(playlist.find("#EXTINF:4.000,") != std::string::npos);
}

MZ_TEST(hls_window_slides_and_media_sequence_advances) {
    // 能证明什么：窗口只保留 6 片、被淘汰的是**整片**、MEDIA-SEQUENCE 跟着前进（FR-3.2）。
    // 什么情况会红：窗口不封顶（内存涨）、或 MEDIA-SEQUENCE 固定为 0（播放器会重复请求老片）。
    StreamInfo video = makeVideoInfo();
    HlsWriter::Ptr writer = HlsWriter::create(makeConfig(), MpegTsMuxer::Streams{&video, nullptr});
    MZ_ASSERT_TRUE(writer != nullptr);
    if (!writer) {
        return;
    }
    // 每 2s 一个关键帧 → 每片恰好 2.000s；造 8 片
    for (int i = 0; i < 9; ++i) {
        const int64_t dts = static_cast<int64_t>(i) * 2000;
        MZ_ASSERT_EQ(writer->onPacket(*videoPacket(video.index, true, dts, dts)),
                     HlsWriter::Result::Ok);
    }
    MZ_ASSERT_EQ(writer->sliceCount(), 6u);          // 窗口封顶
    MZ_ASSERT_GT(writer->slicesDropped(), 0u);       // 有整片被淘汰（计数可见）
    MZ_ASSERT_EQ(writer->slicesProduced(), 8u);      // 切点是第 1..8 个关键帧（第 9 个开启新片）

    const std::string playlist = writer->playlist();
    MZ_ASSERT_EQ(countLines(playlist, "#EXTINF:"), 6u);
    // MEDIA-SEQUENCE = 窗口里第一片的序号（已淘汰 2 片 → 从 2 开始）
    MZ_ASSERT_TRUE(playlist.find("#EXT-X-MEDIA-SEQUENCE:2") != std::string::npos);
    MZ_ASSERT_TRUE(playlist.find("#EXT-X-TARGETDURATION:2") != std::string::npos);
    // URI 形态必须能被 /hls/ 路由解析
    MZ_ASSERT_TRUE(playlist.find("/hls/movie.mp4-2.ts") != std::string::npos);
    MZ_ASSERT_TRUE(playlist.find("/hls/movie.mp4-7.ts") != std::string::npos);
    // 滑出窗口的切片必须取不到（HTTP 侧据此回 404 —— 正常的"老请求"）
    MZ_ASSERT_TRUE(writer->findSlice(0) == nullptr);
    MZ_ASSERT_TRUE(writer->findSlice(2) != nullptr);
    if (auto slice = writer->findSlice(2)) {
        MZ_ASSERT_EQ(slice->seq, 2u);
        // 每片都必须以 TS 同步字开头且 188 对齐（PAT/PMT 在片头）
        MZ_ASSERT_GT(slice->bytes.size(), 0u);
        MZ_ASSERT_EQ(slice->bytes.size() % MpegTsMuxer::kTsPacketSize, 0u);
        MZ_ASSERT_EQ(static_cast<uint8_t>(slice->bytes[0]), 0x47u);
    }
}

MZ_TEST(hls_target_duration_covers_long_gop) {
    // 能证明什么：GOP 比目标长时，切片就变长，而 `#EXT-X-TARGETDURATION` 必须 >= 实际最大时长。
    // 什么情况会红：把 TARGETDURATION 写死成 2（播放器会判"切片比声明长"→ 卡住/重试）。
    StreamInfo video = makeVideoInfo();
    HlsWriter::Config config = makeConfig();
    HlsWriter::Ptr writer = HlsWriter::create(config, MpegTsMuxer::Streams{&video, nullptr});
    MZ_ASSERT_TRUE(writer != nullptr);
    if (!writer) {
        return;
    }
    // 关键帧间隔 3.5s：切点只在关键帧上 → 每片 3.5s
    for (int i = 0; i < 3; ++i) {
        const int64_t dts = static_cast<int64_t>(i) * 3500;
        (void) writer->onPacket(*videoPacket(video.index, true, dts, dts));
    }
    writer->finish();
    const std::string playlist = writer->playlist();
    MZ_ASSERT_TRUE(playlist.find("#EXTINF:3.500,") != std::string::npos);
    MZ_ASSERT_TRUE(playlist.find("#EXT-X-TARGETDURATION:4") != std::string::npos);   // ceil(3.5)
}

MZ_TEST(hls_endlist_appears_once_at_eos) {
    // 能证明什么：源结束后播放列表有 `#EXT-X-ENDLIST`，且 `finish()` 幂等（drain 可能被多调）。
    // 什么情况会红：没有 ENDLIST（播放器一直等"下一片"）、或重复追加（播放列表非法）。
    StreamInfo video = makeVideoInfo();
    HlsWriter::Ptr writer = HlsWriter::create(makeConfig(), MpegTsMuxer::Streams{&video, nullptr});
    MZ_ASSERT_TRUE(writer != nullptr);
    if (!writer) {
        return;
    }
    (void) writer->onPacket(*videoPacket(video.index, true, 0, 0));
    (void) writer->onPacket(*videoPacket(video.index, false, 40, 40));
    writer->finish();
    writer->finish();   // 幂等
    const std::string playlist = writer->playlist();
    MZ_ASSERT_EQ(countLines(playlist, "#EXT-X-ENDLIST"), 1u);
    MZ_ASSERT_EQ(writer->slicesProduced(), 1u);   // EOS 收尾把 open 的那片入库
    MZ_ASSERT_TRUE(writer->finished());
}

MZ_TEST(hls_empty_playlist_before_first_keyframe) {
    // 能证明什么：还没收到视频关键帧时，播放列表只有头部（没有 #EXTINF）—— 播放器会等。
    // 什么情况会红：把"非关键帧包"当成切片起点（会产出不可解码的第一片）。
    StreamInfo video = makeVideoInfo();
    HlsWriter::Ptr writer = HlsWriter::create(makeConfig(), MpegTsMuxer::Streams{&video, nullptr});
    MZ_ASSERT_TRUE(writer != nullptr);
    if (!writer) {
        return;
    }
    (void) writer->onPacket(*videoPacket(video.index, false, 0, 0));   // 非关键帧：丢弃
    const std::string playlist = writer->playlist();
    MZ_ASSERT_EQ(countLines(playlist, "#EXTINF:"), 0u);
    MZ_ASSERT_TRUE(playlist.find("#EXTM3U") == 0);
    MZ_ASSERT_EQ(writer->skippedBeforeKey(), 1u);
    MZ_ASSERT_EQ(writer->slicesProduced(), 0u);
}

MZ_TEST(hls_rejects_bad_config_and_streams) {
    // 能证明什么：非法配置/不支持的编码一律**明确拒绝**（0 不是"不限"）。
    StreamInfo video = makeVideoInfo();
    {
        HlsWriter::Config bad = makeConfig();
        bad.window = 0;
        MZ_ASSERT_TRUE(HlsWriter::create(bad, MpegTsMuxer::Streams{&video, nullptr}) == nullptr);
    }
    {
        HlsWriter::Config bad = makeConfig();
        bad.target_duration_ms = 0;
        MZ_ASSERT_TRUE(HlsWriter::create(bad, MpegTsMuxer::Streams{&video, nullptr}) == nullptr);
    }
    {
        HlsWriter::Config bad = makeConfig();
        bad.name.clear();
        MZ_ASSERT_TRUE(HlsWriter::create(bad, MpegTsMuxer::Streams{&video, nullptr}) == nullptr);
    }
    {
        StreamInfo hevc = makeVideoInfo();
        hevc.codec_id = AV_CODEC_ID_HEVC;
        hevc.codec_name = "hevc";
        MZ_ASSERT_TRUE(HlsWriter::create(makeConfig(), MpegTsMuxer::Streams{&hevc, nullptr}) == nullptr);
    }
    {
        MZ_ASSERT_TRUE(HlsWriter::create(makeConfig(), MpegTsMuxer::Streams{nullptr, nullptr}) ==
                       nullptr);
    }
}

MZ_TEST(hls_slice_uri_carries_transcode_query) {
    // 【M10-c】转码流的切片 URI 必须带上同一份规格（`?transcode=1&h=120`）。
    // 为什么这条是硬要求：播放器**只认播放列表里给出的 URI**。少带一个参数，
    // 播放器就会去请求直通切片 —— 画面突然变回原分辨率（甚至来回跳），服务端不会报错。
    // 什么情况会红：`uri_query` 没拼进切片 URI（大小写/分隔符写错也算）。
    StreamInfo video = makeVideoInfo();
    HlsWriter::Config config = makeConfig();
    config.name = "tx.mp4";
    config.uri_query = "?transcode=1&h=120";
    HlsWriter::Ptr writer = HlsWriter::create(config, MpegTsMuxer::Streams{&video, nullptr});
    MZ_ASSERT_TRUE(writer != nullptr);
    if (!writer) {
        return;
    }
    (void) writer->onPacket(*videoPacket(video.index, true, 0, 0));
    (void) writer->onPacket(*videoPacket(video.index, true, 2000, 2000));
    writer->finish();

    const std::string playlist = writer->playlist();
    // 每一条切片 URI 都要带规格（不能只带第一条）
    size_t pos = 0;
    int uri_count = 0;
    while ((pos = playlist.find("/hls/tx.mp4-", pos)) != std::string::npos) {
        const size_t eol = playlist.find('\n', pos);
        const std::string line = playlist.substr(pos, std::string::npos == eol ? std::string::npos
                                                                             : eol - pos);
        MZ_ASSERT_TRUE(line.find(".ts?transcode=1&h=120") != std::string::npos);
        ++uri_count;
        pos = eol == std::string::npos ? playlist.size() : eol;
    }
    MZ_ASSERT_EQ(uri_count, 2);   // 两片，各一条
    // 不带 uri_query 的播放列表里不能出现参数（反向对照）
    StreamInfo video2 = makeVideoInfo();
    HlsWriter::Config plain = makeConfig();
    plain.name = "plain.mp4";
    HlsWriter::Ptr plain_writer = HlsWriter::create(plain, MpegTsMuxer::Streams{&video2, nullptr});
    MZ_ASSERT_TRUE(plain_writer != nullptr);
    if (plain_writer) {
        (void) plain_writer->onPacket(*videoPacket(video2.index, true, 0, 0));
        (void) plain_writer->onPacket(*videoPacket(video2.index, true, 2000, 2000));
        plain_writer->finish();
        MZ_ASSERT_TRUE(plain_writer->playlist().find('?') == std::string::npos);
    }
}

MZ_TEST(hls_service_stream_key_distinguishes_transcode) {
    // 【M10-c】HLS 的流键与 SourceManager 的源键同构：
    //   不转码 = 名字本身；转码 = `名字|规格`。两条流必须能共存（同名不同规格是两条流）。
    SourceManager::TranscodeSpec none;
    MZ_ASSERT_STR_EQ(HlsService::streamKey("movie.mp4", none), "movie.mp4");

    SourceManager::TranscodeSpec h120;
    h120.enabled = true;
    h120.out_height = 120;
    MZ_ASSERT_STR_EQ(HlsService::streamKey("movie.mp4", h120), "movie.mp4|h=120");

    SourceManager::TranscodeSpec src;
    src.enabled = true;   // 只重编码，不缩放
    MZ_ASSERT_STR_EQ(HlsService::streamKey("movie.mp4", src), "movie.mp4|src");

    // 与源键一致：HlsService 与 SourceManager 用的必须是同一套描述（否则一处转一处不转）
    MZ_ASSERT_STR_EQ(HlsService::streamKey("movie.mp4", h120),
                     "movie.mp4|" + h120.describe());
}
