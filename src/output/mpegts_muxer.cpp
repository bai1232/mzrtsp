/*
 * MpegTsMuxer 实现（M9 / FR-3.2 / FR-3.4）
 * ============================================================================
 * 只做"字节":PAT/PMT（PSI）、PES、188 字节 TS 包、PCR、时间戳 90kHz。
 * 不做切片（`HlsWriter` 做）、不做网络（`output/` 层不依赖 `http/`）。
 *
 * 每个容易写错的地方都在这里注明"为什么"，因为它们都属于"能编译、能跑、看起来正常，
 * 但播放器就是不动"的那类错误（与 FLV 的 tag 布局同一性质）。
 * ============================================================================
 */

#include "output/mpegts_muxer.h"

#include "core/logger.h"
#include "ffmpeg/aac_util.h"
#include "ffmpeg/h264_util.h"

#include <cstring>

namespace mzmedia {

namespace {

constexpr uint8_t kStreamIdVideo = 0xE0;
constexpr uint8_t kStreamIdAudio = 0xC0;
constexpr uint8_t kStreamTypeH264 = 0x1B;
constexpr uint8_t kStreamTypeAacAdts = 0x0F;
constexpr uint64_t kTsClockHz = 90000;
constexpr uint64_t kTimestampMask = (1ull << 33) - 1;   // PTS/DTS/PCR_base 都是 33 bit

/**
 * 33 bit 时间戳 → PES 的 5 字节编码
 * @param prefix PTS-only = 0x02（'0010'）；PTS+DTS 时 PTS = 0x03、DTS = 0x01
 * @note 每个字节里的 marker bit 必须为 1（写错了 ffmpeg 会报 "Invalid timestamps"）
 */
void appendTimestamp(std::vector<uint8_t> *out, uint64_t ts, uint8_t prefix) {
    ts &= kTimestampMask;
    out->push_back(static_cast<uint8_t>((prefix << 4) | ((ts >> 30) & 0x07)));
    out->push_back(static_cast<uint8_t>((ts >> 22) & 0xFF));
    out->push_back(static_cast<uint8_t>((((ts >> 15) & 0x7F) << 1) | 0x01));
    out->push_back(static_cast<uint8_t>((ts >> 7) & 0xFF));
    out->push_back(static_cast<uint8_t>(((ts & 0x7F) << 1) | 0x01));
}

/// PCR 的 6 字节编码：PCR_base(33 bit, 90kHz) + 保留位 + PCR_ext(9 bit, 27MHz/300)
void appendPcr(std::string *out, uint64_t base90k) {
    const uint64_t base = base90k & kTimestampMask;
    const uint64_t ext = 0;   // 我们的时间轴是整数毫秒 → 90kHz 上精确；27MHz 的余数恒为 0
    out->push_back(static_cast<char>((base >> 25) & 0xFF));
    out->push_back(static_cast<char>((base >> 17) & 0xFF));
    out->push_back(static_cast<char>((base >> 9) & 0xFF));
    out->push_back(static_cast<char>((base >> 1) & 0xFF));
    out->push_back(static_cast<char>((((base & 0x01) << 7) | 0x7E | ((ext >> 8) & 0x01))));
    out->push_back(static_cast<char>(ext & 0xFF));
}

} // namespace

MpegTsMuxer::MpegTsMuxer(const Streams &streams)
    : _streams(streams) {}

MpegTsMuxer::Result MpegTsMuxer::fail(Result result, const std::string &message) {
    _last_error = message;
    ErrorP("MpegTsMuxer: %s（%s）", resultName(result), message.c_str());
    return result;
}

const char *MpegTsMuxer::resultName(Result result) {
    switch (result) {
    case Result::Ok:
        return "Ok";
    case Result::NoStreams:
        return "NoStreams";
    case Result::UnsupportedVideoCodec:
        return "UnsupportedVideoCodec";
    case Result::UnsupportedAudioCodec:
        return "UnsupportedAudioCodec";
    case Result::MissingExtradata:
        return "MissingExtradata";
    case Result::InvalidPacket:
        return "InvalidPacket";
    case Result::TooLarge:
        return "TooLarge";
    case Result::NotPrepared:
        return "NotPrepared";
    }
    return "unknown";
}

MpegTsMuxer::Result MpegTsMuxer::prepare() {
    _prepared = false;
    _sps.clear();
    _pps.clear();
    _last_error.clear();

    if (_streams.video == nullptr && _streams.audio == nullptr) {
        return fail(Result::NoStreams, "既没有视频也没有音频（HLS 不接受空流）");
    }
    if (_streams.video != nullptr) {
        if (_streams.video->codec_id != AV_CODEC_ID_H264) {
            return fail(Result::UnsupportedVideoCodec,
                        "TS 切片只支持 H264（当前 " + _streams.video->codec_name + "）");
        }
        const auto &extra = _streams.video->extradata;
        if (!extra || extra->empty()) {
            return fail(Result::MissingExtradata, "H264 缺 avcC（没有初始化数据就补不了 SPS/PPS）");
        }
        // avcC：第 5 字节低 2 位 = lengthSizeMinusOne
        _nal_length_size = static_cast<size_t>(((*extra)[4] & 0x03) + 1);
        if (!parseAvcC(extra->data(), extra->size(), &_sps, &_pps)) {
            return fail(Result::MissingExtradata, "avcC 里取不出 SPS/PPS（初始化数据坏了）");
        }
    }
    if (_streams.audio != nullptr) {
        if (_streams.audio->codec_id != AV_CODEC_ID_AAC) {
            return fail(Result::UnsupportedAudioCodec,
                        "TS 切片只支持 AAC 音频（当前 " + _streams.audio->codec_name + "）");
        }
        const auto &extra = _streams.audio->extradata;
        if (!extra || extra->size() < 2) {
            return fail(Result::MissingExtradata,
                        "AAC 缺 AudioSpecificConfig（TS 要靠它给每帧加 ADTS 头）");
        }
        // ASC 允许带扩展：实测 MP4 的 ASC 是 **5 字节**（`12 08 56 e5 00` = 核心 2 字节 + SBR 扩展）。
        // ADTS 头只能表达核心字段（AOT/sfi/cc），扩展部分表达不了 → **Warn 出来**，不静默丢。
        if (extra->size() > 2) {
            WarnP("MpegTsMuxer: ASC 有 %zu 字节（含扩展），ADTS 头只用到前 2 字节：%02X %02X",
                  extra->size(), static_cast<unsigned>((*extra)[0]),
                  static_cast<unsigned>((*extra)[1]));
        }
        _audio_asc[0] = (*extra)[0];
        _audio_asc[1] = (*extra)[1];
    }
    _prepared = true;
    return Result::Ok;
}

uint32_t MpegTsMuxer::crc32Mpeg(const uint8_t *data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04C11DB7u) : (crc << 1);
        }
    }
    return crc;
}

void MpegTsMuxer::writeSection(uint16_t pid, uint8_t *cc, const std::vector<uint8_t> &section,
                               std::string *out) {
    std::string pkt;
    pkt.reserve(kTsPacketSize);
    pkt.push_back(static_cast<char>(0x47));
    pkt.push_back(static_cast<char>(0x40 | ((pid >> 8) & 0x1F)));   // PUSI = 1
    pkt.push_back(static_cast<char>(pid & 0xFF));
    pkt.push_back(static_cast<char>(0x10 | (*cc & 0x0F)));          // AFC=01（纯负载）
    *cc = static_cast<uint8_t>((*cc + 1) & 0x0F);
    pkt.push_back(static_cast<char>(0x00));                         // pointer_field
    pkt.append(reinterpret_cast<const char *>(section.data()), section.size());
    // 剩余补 0xFF（PSI 的填充值，不是"垃圾"）
    const size_t used = 4 + 1 + section.size();
    pkt.append(kTsPacketSize - used, static_cast<char>(0xFF));
    *out += pkt;
    ++_ts_packets;
    _bytes_written += pkt.size();
}

MpegTsMuxer::Result MpegTsMuxer::writePatPmt(std::string *out) {
    if (!_prepared) {
        return fail(Result::NotPrepared, "没 prepare 就写 PAT/PMT");
    }
    if (out == nullptr) {
        return fail(Result::InvalidPacket, "writePatPmt 的 out 为空");
    }

    // ---- PAT：一个 program（1）→ PMT PID ----
    std::vector<uint8_t> pat;
    pat.push_back(0x00);   // table_id
    pat.push_back(0xB0);   // section_syntax_indicator=1 | '0' | reserved '11' | section_length 高 4 位
    pat.push_back(0x0D);   // section_length = 13（含 CRC；transport_stream_id 起的字节数）
    pat.push_back(0x00);   // transport_stream_id 高
    pat.push_back(0x01);   // transport_stream_id 低（任意，但同一切片内一致）
    pat.push_back(0xC1);   // reserved '11' | version=0 | current_next=1
    pat.push_back(0x00);   // section_number
    pat.push_back(0x00);   // last_section_number
    pat.push_back(0x00);   // program_number 高
    pat.push_back(0x01);   // program_number = 1
    pat.push_back(static_cast<uint8_t>(0xE0 | ((kPmtPid >> 8) & 0x1F)));
    pat.push_back(static_cast<uint8_t>(kPmtPid & 0xFF));
    const uint32_t pat_crc = crc32Mpeg(pat.data(), pat.size());
    pat.push_back(static_cast<uint8_t>((pat_crc >> 24) & 0xFF));
    pat.push_back(static_cast<uint8_t>((pat_crc >> 16) & 0xFF));
    pat.push_back(static_cast<uint8_t>((pat_crc >> 8) & 0xFF));
    pat.push_back(static_cast<uint8_t>(pat_crc & 0xFF));
    writeSection(0x0000, &_cc_pat, pat, out);

    // ---- PMT：PCR_PID = 视频 PID（没有视频时退化为音频 PID）----
    const uint16_t pcr_pid = (_streams.video != nullptr) ? kVideoPid : kAudioPid;
    std::vector<uint8_t> pmt;
    pmt.push_back(0x02);   // table_id
    // section_length 稍后回填（= 9 + 5×流数 + 4）
    const size_t stream_count = (_streams.video != nullptr ? 1u : 0u) + (_streams.audio != nullptr ? 1u : 0u);
    const uint16_t section_length = static_cast<uint16_t>(9 + 5 * stream_count + 4);
    pmt.push_back(static_cast<uint8_t>(0xB0 | ((section_length >> 8) & 0x0F)));
    pmt.push_back(static_cast<uint8_t>(section_length & 0xFF));
    pmt.push_back(0x00);   // program_number 高
    pmt.push_back(0x01);   // program_number = 1（与 PAT 一致）
    pmt.push_back(0xC1);   // reserved | version=0 | current_next=1
    pmt.push_back(0x00);   // section_number
    pmt.push_back(0x00);   // last_section_number
    pmt.push_back(static_cast<uint8_t>(0xE0 | ((pcr_pid >> 8) & 0x1F)));
    pmt.push_back(static_cast<uint8_t>(pcr_pid & 0xFF));
    pmt.push_back(0xF0);   // reserved '1111' | program_info_length 高 4 位
    pmt.push_back(0x00);   // program_info_length = 0
    if (_streams.video != nullptr) {
        pmt.push_back(kStreamTypeH264);
        pmt.push_back(static_cast<uint8_t>(0xE0 | ((kVideoPid >> 8) & 0x1F)));
        pmt.push_back(static_cast<uint8_t>(kVideoPid & 0xFF));
        pmt.push_back(0xF0);   // ES_info_length = 0
        pmt.push_back(0x00);
    }
    if (_streams.audio != nullptr) {
        pmt.push_back(kStreamTypeAacAdts);
        pmt.push_back(static_cast<uint8_t>(0xE0 | ((kAudioPid >> 8) & 0x1F)));
        pmt.push_back(static_cast<uint8_t>(kAudioPid & 0xFF));
        pmt.push_back(0xF0);
        pmt.push_back(0x00);
    }
    const uint32_t pmt_crc = crc32Mpeg(pmt.data(), pmt.size());
    pmt.push_back(static_cast<uint8_t>((pmt_crc >> 24) & 0xFF));
    pmt.push_back(static_cast<uint8_t>((pmt_crc >> 16) & 0xFF));
    pmt.push_back(static_cast<uint8_t>((pmt_crc >> 8) & 0xFF));
    pmt.push_back(static_cast<uint8_t>(pmt_crc & 0xFF));
    writeSection(kPmtPid, &_cc_pmt, pmt, out);
    return Result::Ok;
}

void MpegTsMuxer::writePes(uint16_t pid, uint8_t *cc, const uint8_t *pes, size_t len, bool pcr,
                           uint64_t pcr_base90k, std::string *out) {
    size_t pos = 0;
    bool first = true;
    while (pos < len) {
        const size_t remaining = len - pos;
        const bool with_pcr = first && pcr;

        // adaptation field 里除"长度字节"以外的内容：**flags(1)** +（有 PCR 时再 6）。
        // ★ 这里踩过一次：原先无 PCR 时把 af_content 算成 0，于是 af_len 少算 1，
        //   每个"靠 AF 填充的包"多写 1 字节 → 138 个包 = 138 字节错位 → 整条流全花
        //   （ffmpeg 报 corrupted macroblock / channel element not allocated）。
        const size_t af_content = (with_pcr ? 1u + 6u : 1u);
        bool use_af = with_pcr;
        // 纯负载能装 184；带 AF 时 4 + 1 + af_len + payload = 188 → payload = 183 - af_len，
        // 而 af_len = af_content + stuffing，所以"不填充时"的容量是 183 - af_content。
        size_t payload_cap = use_af ? (183 - af_content) : 184;
        if (!use_af && remaining < 184) {
            use_af = true;   // 最后一块不满：必须用 AF 吸收空洞（TS 负载不能随便补 0xFF）
            payload_cap = 183 - af_content;   // af_content = 1（只有 flags 字节）
        }
        const size_t take = (remaining < payload_cap) ? remaining : payload_cap;
        const size_t stuff = payload_cap - take;

        std::string pkt;
        pkt.reserve(kTsPacketSize);
        pkt.push_back(static_cast<char>(0x47));
        pkt.push_back(static_cast<char>((first ? 0x40 : 0x00) | ((pid >> 8) & 0x1F)));   // PUSI
        pkt.push_back(static_cast<char>(pid & 0xFF));
        const uint8_t afc = use_af ? 0x30 : 0x10;   // 11 = AF+payload；01 = 仅 payload
        pkt.push_back(static_cast<char>(afc | (*cc & 0x0F)));
        *cc = static_cast<uint8_t>((*cc + 1) & 0x0F);

        if (use_af) {
            const size_t af_len = af_content + stuff;
            pkt.push_back(static_cast<char>(af_len & 0xFF));
            if (with_pcr) {
                pkt.push_back(static_cast<char>(0x10));   // PCR_flag
                appendPcr(&pkt, pcr_base90k);
            } else {
                pkt.push_back(static_cast<char>(0x00));   // 没有 flags，全是填充
            }
            if (stuff > 0) {
                pkt.append(stuff, static_cast<char>(0xFF));
            }
        }
        if (take > 0) {
            pkt.append(reinterpret_cast<const char *>(pes + pos), take);
        }
        pos += take;
        first = false;
        *out += pkt;
        ++_ts_packets;
        _bytes_written += pkt.size();
    }
}

MpegTsMuxer::Result MpegTsMuxer::writePacket(const MediaPacket &packet, std::string *out) {
    if (!_prepared) {
        return fail(Result::NotPrepared, "没 prepare 就写包");
    }
    if (out == nullptr) {
        return fail(Result::InvalidPacket, "writePacket 的 out 为空");
    }
    if (packet.size() == 0) {
        return fail(Result::InvalidPacket, "空包（size == 0）");
    }

    const bool is_video = (_streams.video != nullptr && packet.streamIndex() == _streams.video->index);
    const bool is_audio = (_streams.audio != nullptr && packet.streamIndex() == _streams.audio->index);
    if (!is_video && !is_audio) {
        return fail(Result::InvalidPacket, "包不属于这两路流（streamIndex 对不上）");
    }

    const uint64_t pts90k = static_cast<uint64_t>(packet.ptsMs()) * kTsClockHz / 1000;
    const uint64_t dts90k = static_cast<uint64_t>(packet.dtsMs()) * kTsClockHz / 1000;
    _last_dts90k = dts90k & kTimestampMask;
    ++_pes_packets;

    if (is_video) {
        // ① AVCC → Annex-B（TS 要起始码）
        std::vector<uint8_t> annexb;
        if (!avccToAnnexB(packet.data(), packet.size(), _nal_length_size, &annexb)) {
            return fail(Result::InvalidPacket, "视频包 AVCC → Annex-B 失败（长度前缀/length_size 不对）");
        }
        // ② 关键帧前补 SPS/PPS：**每个切片都要能独立解码**，而 MP4 的包里没有 SPS/PPS
        if (packet.isKeyFrame() && !_sps.empty() && !_pps.empty()) {
            std::vector<uint8_t> with_headers;
            with_headers.reserve(6 + _sps.size() + _pps.size() + annexb.size());
            with_headers.push_back(0x00);
            with_headers.push_back(0x00);
            with_headers.push_back(0x01);
            with_headers.insert(with_headers.end(), _sps.begin(), _sps.end());
            with_headers.push_back(0x00);
            with_headers.push_back(0x00);
            with_headers.push_back(0x01);
            with_headers.insert(with_headers.end(), _pps.begin(), _pps.end());
            with_headers.insert(with_headers.end(), annexb.begin(), annexb.end());
            annexb = std::move(with_headers);
            ++_sps_pps_injected;
        }

        // ③ 组 PES：视频 PES_packet_length 写 0（不限制，规范只允许视频这么做）
        const bool with_dts = (pts90k != dts90k);
        const size_t header_data_len = with_dts ? 10 : 5;
        std::vector<uint8_t> pes;
        pes.reserve(9 + header_data_len + annexb.size());
        pes.push_back(0x00);
        pes.push_back(0x00);
        pes.push_back(0x01);
        pes.push_back(kStreamIdVideo);
        pes.push_back(0x00);
        pes.push_back(0x00);
        pes.push_back(static_cast<uint8_t>(with_dts ? 0xC0 : 0x80));
        pes.push_back(0x00);
        pes.push_back(static_cast<uint8_t>(header_data_len));
        appendTimestamp(&pes, pts90k, static_cast<uint8_t>(with_dts ? 0x03 : 0x02));
        if (with_dts) {
            appendTimestamp(&pes, dts90k, 0x01);
        }
        pes.insert(pes.end(), annexb.begin(), annexb.end());

        // ④ PCR：关键帧插一次（`pcrCount()` 可见；间隔由调用方用 lastDts90k() 判断）
        const bool need_pcr = packet.isKeyFrame();
        if (need_pcr) {
            ++_pcr_count;
        }
        writePes(kVideoPid, &_cc_video, pes.data(), pes.size(), need_pcr, dts90k, out);
        return Result::Ok;
    }

    // ---- 音频：raw AAC → ADTS ----
    std::vector<uint8_t> adts;
    if (!rawAacToAdts(_audio_asc, sizeof(_audio_asc), packet.data(), packet.size(), &adts)) {
        return fail(Result::TooLarge,
                    "raw AAC → ADTS 失败（ASC 非法 / AOT 不能用 ADTS 表示 / 帧长超过 13 bit）");
    }
    const size_t pes_payload_len = 3 + 5 + adts.size();   // flags+flags2+hdr_len + PTS + 负载
    if (pes_payload_len > 0xFFFFu) {
        return fail(Result::TooLarge, "音频 PES 超过 65535 字节（PES_packet_length 装不下）");
    }
    std::vector<uint8_t> pes;
    pes.reserve(6 + pes_payload_len);
    pes.push_back(0x00);
    pes.push_back(0x00);
    pes.push_back(0x01);
    pes.push_back(kStreamIdAudio);
    pes.push_back(static_cast<uint8_t>((pes_payload_len >> 8) & 0xFF));
    pes.push_back(static_cast<uint8_t>(pes_payload_len & 0xFF));
    pes.push_back(0x80);   // 只带 PTS（音频 dts == pts）
    pes.push_back(0x00);
    pes.push_back(0x05);
    appendTimestamp(&pes, pts90k, 0x02);
    pes.insert(pes.end(), adts.begin(), adts.end());
    writePes(kAudioPid, &_cc_audio, pes.data(), pes.size(), false, 0, out);
    return Result::Ok;
}

} // namespace mzmedia
