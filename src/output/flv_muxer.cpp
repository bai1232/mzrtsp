/*
 * FlvMuxer 实现（M6-a）
 * ============================================================================
 * FLV 的字节布局（写错了就是"能连上但播不出"，所以这里逐字段写清楚）：
 *
 *   FLV Header（9B）   'F''L''V' | version=1 | flags(0x01 视频 / 0x04 音频) | DataOffset=9(u32)
 *   PreviousTagSize0   0 (u32)
 *   Tag（重复）        type(1) | dataSize(u24) | timestamp(u24 低) | timestampExt(1 高) | streamId=0(u24)
 *                      | data | PreviousTagSize = 11 + dataSize (u32)
 *
 *   视频 tag data      (frameType<<4 | 7=AVC) | AVCPacketType(0=seq header,1=NALU) | CompositionTime(int24)
 *                      | payload（seq header 时是 avcC；NALU 时是 **length-prefixed** 数据）
 *   音频 tag data      (10=AAC<<4 | rate<<2 | size<<1 | type) | AACPacketType(0=seq header,1=raw) | payload
 *
 * 为什么视频 NALU 可以直接搬：MP4 里的 H264 包本来就是 length-prefixed（AVCC），与 FLV 要求一致；
 * Annex-B（起始码）才需要转换 —— 那正是 M6-a 明确不做、并且**明确拒绝**的输入。
 * ============================================================================
 */

#include "output/flv_muxer.h"

#include "core/logger.h"

#include <cstdio>

namespace mzmedia {

namespace {

constexpr uint8_t kTagAudio = 8;
constexpr uint8_t kTagVideo = 9;
constexpr uint8_t kCodecIdAvc = 7;
constexpr uint8_t kSoundFormatAac = 10;
constexpr size_t kTagHeaderSize = 11;
constexpr size_t kTagSuffixSize = 4;          // PreviousTagSize
constexpr size_t kMaxTagDataSize = 0xFFFFFFu; // dataSize 是 24 位
constexpr int64_t kInt24Min = -8388608;
constexpr int64_t kInt24Max = 8388607;

void appendU24(std::string *out, uint32_t value) {
    out->push_back(static_cast<char>((value >> 16) & 0xFFu));
    out->push_back(static_cast<char>((value >> 8) & 0xFFu));
    out->push_back(static_cast<char>(value & 0xFFu));
}

void appendU32(std::string *out, uint32_t value) {
    out->push_back(static_cast<char>((value >> 24) & 0xFFu));
    out->push_back(static_cast<char>((value >> 16) & 0xFFu));
    out->push_back(static_cast<char>((value >> 8) & 0xFFu));
    out->push_back(static_cast<char>(value & 0xFFu));
}

} // namespace

FlvMuxer::FlvMuxer(const Streams &streams)
    : _streams(streams) {}

const char *FlvMuxer::resultName(Result result) {
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
    case Result::AnnexBNotSupported:
        return "AnnexBNotSupported";
    case Result::NotPrepared:
        return "NotPrepared";
    case Result::InvalidPacket:
        return "InvalidPacket";
    case Result::TooLarge:
        return "TooLarge";
    }
    return "Unknown";
}

FlvMuxer::Result FlvMuxer::fail(Result result, const std::string &message) {
    _last_error = message;
    WarnP("FlvMuxer: %s（%s）", resultName(result), message.c_str());
    return result;
}

FlvMuxer::Result FlvMuxer::prepare() {
    if (_streams.video == nullptr && _streams.audio == nullptr) {
        return fail(Result::NoStreams, "既没有视频流也没有音频流");
    }

    if (_streams.video != nullptr) {
        const StreamInfo &video = *_streams.video;
        if (video.codec_id != AV_CODEC_ID_H264) {
            return fail(Result::UnsupportedVideoCodec,
                        "视频编码不是 H264（v0.1 只支持 H264）：" + video.codec_name);
        }
        if (!video.extradata || video.extradata->empty()) {
            return fail(Result::MissingExtradata,
                        "H264 缺 avcC（AVCDecoderConfigurationRecord）→ 写不出 AVC sequence header");
        }
        const std::vector<uint8_t> &extra = *video.extradata;
        if (extra.size() < 7 || extra[0] != 1) {
            // avcC 的第一字节是 configurationVersion(=1)；0x00 起头基本都是 Annex-B 的起始码
            if (extra[0] == 0) {
                return fail(Result::AnnexBNotSupported,
                            "H264 初始化数据看着是 Annex-B（H264 裸流）→ M6-a 不做转换，"
                            "请用 MP4 输入（转换见 M6-c）");
            }
            return fail(Result::MissingExtradata, "H264 初始化数据不是合法的 avcC（首字节应为 1）");
        }
    }

    if (_streams.audio != nullptr) {
        const StreamInfo &audio = *_streams.audio;
        if (audio.codec_id != AV_CODEC_ID_AAC) {
            return fail(Result::UnsupportedAudioCodec,
                        "音频编码不是 AAC（v0.1 只支持 AAC 转发）：" + audio.codec_name);
        }
        if (!audio.extradata || audio.extradata->empty()) {
            return fail(Result::MissingExtradata, "AAC 缺 AudioSpecificConfig → 写不出 AAC sequence header");
        }
    }

    _prepared = true;
    return Result::Ok;
}

void FlvMuxer::setTimestampBaseMs(int64_t dts_ms) {
    _base_dts_ms = dts_ms;
    _base_fixed = true;
}

bool FlvMuxer::writeHeader(std::string *out) const {
    if (out == nullptr) {
        return false;
    }
    const uint8_t flags = static_cast<uint8_t>((_streams.video != nullptr ? 0x01u : 0x00u) |
                                               (_streams.audio != nullptr ? 0x04u : 0x00u));
    out->push_back('F');
    out->push_back('L');
    out->push_back('V');
    out->push_back(1); // version
    out->push_back(static_cast<char>(flags));
    appendU32(out, 9); // DataOffset
    appendU32(out, 0); // PreviousTagSize0
    return true;
}

FlvMuxer::Result FlvMuxer::writeSequenceHeaders(std::string *out) {
    if (!_prepared) {
        return fail(Result::NotPrepared, "writeSequenceHeaders 之前必须先 prepare()");
    }
    if (out == nullptr) {
        return fail(Result::InvalidPacket, "out 为空");
    }

    if (_streams.video != nullptr) {
        std::string data;
        data.push_back(static_cast<char>(0x17)); // frameType=1(关键帧) | codecId=7(AVC)
        data.push_back(static_cast<char>(0x00)); // AVCPacketType = 0（sequence header）
        appendU24(&data, 0);                     // CompositionTime = 0
        data.append(reinterpret_cast<const char *>(_streams.video->extradata->data()),
                    _streams.video->extradata->size());
        const Result result = appendTag(kTagVideo, 0, data, out);
        if (result != Result::Ok) {
            return result;
        }
    }

    if (_streams.audio != nullptr) {
        std::string data;
        // AAC 的 SoundRate 在 FLV 里**恒为 3**（44kHz）、SoundSize=1(16bit)、SoundType=1(stereo)
        data.push_back(static_cast<char>((kSoundFormatAac << 4) | (3u << 2) | (1u << 1) | 1u));
        data.push_back(static_cast<char>(0x00)); // AACPacketType = 0（sequence header）
        data.append(reinterpret_cast<const char *>(_streams.audio->extradata->data()),
                    _streams.audio->extradata->size());
        const Result result = appendTag(kTagAudio, 0, data, out);
        if (result != Result::Ok) {
            return result;
        }
    }
    return Result::Ok;
}

uint32_t FlvMuxer::timestampFor(int64_t dts_ms) {
    if (!_base_fixed) {
        _base_dts_ms = dts_ms; // 首个包定基准（每个客户端一份）
        _base_fixed = true;
    }
    if (dts_ms < _base_dts_ms) {
        // 早于基准：**不能**让 uint32 回绕出 40 亿这种值（播放端会直接崩/黑屏），
        // 钳到 0 并计数 + 首次告警（静默回绕属于最难查的问题）
        ++_clamped_before_base;
        if (_clamped_before_base == 1) {
            WarnP("FlvMuxer: 有包早于时间戳基准（dts=%lld < base=%lld）→ 时间戳钳到 0 并计数",
                  static_cast<long long>(dts_ms), static_cast<long long>(_base_dts_ms));
        }
        _last_timestamp = 0;
        return 0;
    }
    // FLV 时间戳是 32 位毫秒：按规范自然回绕（长片/长时间运行会绕）
    const uint32_t timestamp = static_cast<uint32_t>(static_cast<uint64_t>(dts_ms - _base_dts_ms));
    _last_timestamp = timestamp;
    return timestamp;
}

FlvMuxer::Result FlvMuxer::appendTag(uint8_t type, uint32_t timestamp_ms, const std::string &data,
                                     std::string *out) {
    if (out == nullptr) {
        return fail(Result::InvalidPacket, "out 为空");
    }
    if (data.size() > kMaxTagDataSize) {
        return fail(Result::TooLarge,
                    "tag 数据 " + std::to_string(data.size()) + " 字节超过 24 位长度上限");
    }

    const uint32_t data_size = static_cast<uint32_t>(data.size());
    out->push_back(static_cast<char>(type));
    appendU24(out, data_size);
    appendU24(out, timestamp_ms & 0xFFFFFFu);                       // 低 24 位
    out->push_back(static_cast<char>((timestamp_ms >> 24) & 0xFFu)); // 高 8 位
    appendU24(out, 0);                                              // StreamID = 0
    out->append(data);
    appendU32(out, static_cast<uint32_t>(kTagHeaderSize + data_size)); // PreviousTagSize

    ++_tag_count;
    _bytes_written += kTagHeaderSize + data_size + kTagSuffixSize;
    return Result::Ok;
}

FlvMuxer::Result FlvMuxer::writeVideoTag(const MediaPacket &packet, std::string *out) {
    const int64_t composition = packet.ptsMs() - packet.dtsMs();
    if (composition < kInt24Min || composition > kInt24Max) {
        return fail(Result::InvalidPacket,
                    "CompositionTime 超出 24 位有符号范围（pts-dts=" + std::to_string(composition) + "ms）");
    }

    std::string data;
    const uint8_t frame_type = packet.isKeyFrame() ? 1u : 2u; // 1 = keyframe, 2 = inter frame
    data.push_back(static_cast<char>((frame_type << 4) | kCodecIdAvc));
    data.push_back(static_cast<char>(0x01)); // AVCPacketType = 1（NALU）
    appendU24(&data, static_cast<uint32_t>(composition) & 0xFFFFFFu);
    // MP4 的包本来就是 length-prefixed（AVCC），与 FLV 要求一致 → 直接搬，不做转换
    data.append(reinterpret_cast<const char *>(packet.data()), packet.size());
    return appendTag(kTagVideo, timestampFor(packet.dtsMs()), data, out);
}

FlvMuxer::Result FlvMuxer::writeAudioTag(const MediaPacket &packet, std::string *out) {
    std::string data;
    data.push_back(static_cast<char>((kSoundFormatAac << 4) | (3u << 2) | (1u << 1) | 1u));
    data.push_back(static_cast<char>(0x01)); // AACPacketType = 1（raw AAC）
    data.append(reinterpret_cast<const char *>(packet.data()), packet.size());
    return appendTag(kTagAudio, timestampFor(packet.dtsMs()), data, out);
}

FlvMuxer::Result FlvMuxer::writePacket(const MediaPacket &packet, std::string *out) {
    if (!_prepared) {
        return fail(Result::NotPrepared, "writePacket 之前必须先 prepare()");
    }
    if (out == nullptr) {
        return fail(Result::InvalidPacket, "out 为空");
    }
    const int index = packet.streamIndex();
    if (_streams.video != nullptr && index == _streams.video->index) {
        return writeVideoTag(packet, out);
    }
    if (_streams.audio != nullptr && index == _streams.audio->index) {
        return writeAudioTag(packet, out);
    }
    // 不属于这两路流的包**不能静默丢**（丢数据要能被发现）
    return fail(Result::InvalidPacket,
                "包不属于这两路流（stream_index=" + std::to_string(index) + "）");
}

} // namespace mzmedia
