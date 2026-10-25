/*
 * 纯转换阶段的实现（逻辑逐字搬自 v0.2.2 的 `DemuxerProducer`）
 * ============================================================================
 * 搬运时刻意**没有改写任何判据**：每条错误的措辞、每个上限、每处"不猜"的理由都保留。
 * 时间戳：输出包直接沿用输入包的 dts/pts（**不加趟偏移** —— 那是生产者的链尾工作）。
 * ============================================================================
 */

#include "media/stage_passthrough.h"

#include "core/logger.h"
#include "ffmpeg/aac_util.h"
#include "ffmpeg/h264_util.h"

#include <utility>

namespace mzmedia {

namespace {

/// 用同一份载荷/键/时间戳造一个包（转换**失败**时返回 nullptr，调用方填 error）
MediaPacket::Ptr cloneWithPayload(const MediaPacket &in, std::shared_ptr<const std::vector<uint8_t>> payload) {
    return MediaPacket::create(in.kind(), in.streamIndex(), std::move(payload), in.isKeyFrame(),
                               in.dtsMs(), in.ptsMs());
}

} // namespace

// ===========================================================================
// RawStage：原样透传
// ===========================================================================

bool RawStage::prepare(const StreamInfo &src, const StageContext &ctx, StreamInfo *out,
                       std::string *error) {
    (void) ctx;
    (void) error;
    *out = src; // 编码 / 宽高 / extradata 都不改
    return true;
}

bool RawStage::process(const MediaPacket &in, std::vector<MediaPacket::Ptr> *out,
                       std::string *error) {
    // 零转换：直接把同一个共享载荷交出去（**不复制**；所有订阅者共享同一块内存）
    auto pkt = cloneWithPayload(in, in.payload());
    if (!pkt) {
        *error = "MediaPacket::create 失败（参数非法）";
        return false;
    }
    out->push_back(std::move(pkt));
    return true;
}

// ===========================================================================
// AnnexBStage：Annex-B → AVCC（+ 现场构造 avcC）
// ===========================================================================

bool AnnexBStage::prepare(const StreamInfo &src, const StageContext &ctx, StreamInfo *out,
                          std::string *error) {
    (void) ctx;
    *out = src;
    const bool has_extra = src.extradata && !src.extradata->empty();
    if (!has_extra || src.codec_id != AV_CODEC_ID_H264 ||
        !looksLikeAnnexB(src.extradata->data(), src.extradata->size())) {
        return true; // 不是 Annex-B（比如 MP4 的 avcC）：本阶段什么都不用做
    }
    // H264 裸流（.h264 / TS）：初始化数据是 Annex-B 的 SPS/PPS → 现场构造 avcC
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    if (!extractSpsPps(src.extradata->data(), src.extradata->size(), &sps, &pps)) {
        *error = "H264 输入是 Annex-B，但初始化数据里没有同时出现 SPS 与 PPS → "
                 "构造不出 avcC（明确失败，不用裸 SPS/PPS 冒充初始化头）";
        return false;
    }
    std::vector<uint8_t> avcc;
    if (!buildAvcC(sps.data(), sps.size(), pps.data(), pps.size(), &avcc)) {
        *error = "SPS(" + std::to_string(sps.size()) + "B)/PPS(" + std::to_string(pps.size()) +
                 "B) 构造 avcC 失败（参数非法或超长）";
        return false;
    }
    out->extradata = std::make_shared<const std::vector<uint8_t>>(std::move(avcc));
    InfoP("AnnexBStage: 输入是 H264 裸流（Annex-B）→ 构造 avcC %zu 字节，发包时转 AVCC",
          out->extradata->size());
    return true;
}

bool AnnexBStage::process(const MediaPacket &in, std::vector<MediaPacket::Ptr> *out,
                          std::string *error) {
    auto avcc = std::make_shared<std::vector<uint8_t>>();
    if (!annexBToAvcc(in.data(), in.size(), avcc.get())) {
        *error = "Annex-B → AVCC 转换失败（包内没有合法起始码 / NAL 超长）";
        return false;
    }
    auto pkt = cloneWithPayload(in, std::move(avcc));
    if (!pkt) {
        *error = "MediaPacket::create 失败（参数非法）";
        return false;
    }
    ++_converted;
    out->push_back(std::move(pkt));
    return true;
}

// ===========================================================================
// AdtsStage：ADTS → raw AAC（+ 一次性探针造 ASC）
// ===========================================================================

bool AdtsStage::prepare(const StreamInfo &src, const StageContext &ctx, StreamInfo *out,
                        std::string *error) {
    *out = src;
    const bool has_extra = src.extradata && !src.extradata->empty();
    if (has_extra || src.codec_id != AV_CODEC_ID_AAC) {
        return true; // MP4 的 AAC 自带 ASC：本阶段什么都不用做
    }
    // ---- TS 的 AAC 是 ADTS：没有 extradata，必须**现场造 ASC** ----
    // 为什么用**一次性探针**而不是用主解封装读一帧：
    //   主 Demuxer 的读游标一旦前进，读到的那些包就必须原样留到后面再交付（否则首帧丢失），
    //   要实现就得引入"重放队列"；而 ASC 又必须在 open 之内就绪。探针把两个约束都绕开了。
    // 代价：打开同一个源两次（本地文件是一次 open + 读几个包，毫秒级）。
    Demuxer probe(ctx.demux);
    if (!probe.open(ctx.path)) {
        *error = "探测 ADTS 头失败：无法用探针打开 " + ctx.path + "（" + probe.lastError() + "）";
        return false;
    }
    const StreamInfo *probe_audio = probe.firstAudio();
    if (probe_audio == nullptr) {
        (void) probe.close();
        *error = "探测 ADTS 头失败：探针没有看到音频流（与主解封装不一致）";
        return false;
    }
    const int want_index = probe_audio->index;

    // 有界：最多看这么多包（正常 TS 里音频包间隔只有几个包；找不到就是源本身异常）
    constexpr int kMaxProbePackets = 512;
    AdtsHeader header;
    bool found = false;
    for (int i = 0; i < kMaxProbePackets; ++i) {
        const Demuxer::ReadResult result = probe.readPacket();
        if (result == Demuxer::ReadResult::Eof) {
            break;
        }
        if (result == Demuxer::ReadResult::Error) {
            *error = "探测 ADTS 头失败：探针读包出错（" + probe.lastError() + "）";
            (void) probe.close();
            return false;
        }
        if (probe.packetStreamIndex() != want_index) {
            continue;
        }
        const AVPacket *pkt = probe.packet();
        if (pkt == nullptr || pkt->data == nullptr || pkt->size <= 0) {
            continue;
        }
        if (!parseAdtsHeader(pkt->data, static_cast<size_t>(pkt->size), &header)) {
            *error = "探测 ADTS 头失败：TS 的 AAC 包不是合法 ADTS 帧（同步字 / 表外采样率 / 帧长非法）"
                     " → 造不出 AudioSpecificConfig（明确拒绝，不猜）";
            (void) probe.close();
            return false;
        }
        found = true;
        break;
    }
    (void) probe.close();
    if (!found) {
        *error = "探测 ADTS 头失败：前 " + std::to_string(kMaxProbePackets) +
                 " 个包里没有 AAC 音频帧 → 造不出 AudioSpecificConfig（明确拒绝，不猜）";
        return false;
    }

    uint8_t asc[2] = {0, 0};
    if (!buildAudioSpecificConfig(header, asc)) {
        *error = "由 ADTS 头构造 AudioSpecificConfig 失败（字段超出位宽）";
        return false;
    }
    // 交叉校验（两套独立路径互相印证，与 M4-b 的 SPS 交叉校验同一思路）：
    //   · 采样率**必须**一致 —— 不一致说明两套读法有一边错了，继续下去会产出音调不对的流；
    //   · 声道数只 Warn（ASC 用 ADTS 的 channel_configuration 才是权威）
    const uint32_t adts_rate = aacSampleRateFromIndex(header.sampling_frequency_index);
    if (src.sample_rate > 0 && adts_rate != static_cast<uint32_t>(src.sample_rate)) {
        *error = "ADTS 头声明的采样率 " + std::to_string(adts_rate) + " Hz 与解封装报的 " +
                 std::to_string(src.sample_rate) + " Hz 不一致 → 明确拒绝（不猜用哪个）";
        return false;
    }
    if (src.channels > 0 && static_cast<int>(header.channel_configuration) != src.channels) {
        WarnP("AdtsStage: ADTS 的 channel_configuration=%u 与解封装报的 channels=%d 不同"
              "（7.1 的 channel_configuration=7 对应 8 声道，属于正常差异）→ 以 ADTS 为准",
              static_cast<unsigned>(header.channel_configuration), src.channels);
    }
    out->extradata = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{asc[0], asc[1]});
    InfoP("AdtsStage: 音频是 ADTS（TS）→ 现场构造 AudioSpecificConfig %02X %02X"
          "（profile=%u 采样率索引=%u 声道配置=%u，头 %u 字节）→ 发包时逐包剥 ADTS 头",
          static_cast<unsigned>(asc[0]), static_cast<unsigned>(asc[1]),
          static_cast<unsigned>(header.profile),
          static_cast<unsigned>(header.sampling_frequency_index),
          static_cast<unsigned>(header.channel_configuration),
          static_cast<unsigned>(header.header_bytes));
    return true;
}

bool AdtsStage::process(const MediaPacket &in, std::vector<MediaPacket::Ptr> *out,
                        std::string *error) {
    auto raw = std::make_shared<std::vector<uint8_t>>();
    if (!adtsToRawAac(in.data(), in.size(), raw.get())) {
        *error = "ADTS → raw AAC 转换失败（帧不完整 / 一帧多块 / 同步字丢失）";
        return false;
    }
    auto pkt = cloneWithPayload(in, std::move(raw));
    if (!pkt) {
        *error = "MediaPacket::create 失败（参数非法）";
        return false;
    }
    ++_converted;
    out->push_back(std::move(pkt));
    return true;
}

} // namespace mzmedia
