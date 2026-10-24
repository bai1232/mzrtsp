/*
 * aac_util 实现（M8 / FR-2.2）：ADTS 头解析、ASC 构造、ADTS → raw AAC
 * ============================================================================
 * 为什么全部是整数位运算：ADTS 头是**定长位域**，没有浮点、没有正文，纯函数可完全单测。
 * 失败一律返回 false —— 这是 CODEC_MATRIX §5 那条「必须按目标容器转换」的具体落地：
 * 宁可明确拒绝（415 + 原因），也不要产出一路"能播但没声音/花屏"的流。
 * ============================================================================
 */

#include "ffmpeg/aac_util.h"

namespace mzmedia {

namespace {

// ISO/IEC 13818-7 表：sampling_frequency_index → 采样率
constexpr uint32_t kSampleRates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                       22050, 16000, 12000, 11025, 8000,  7350};

} // namespace

uint32_t aacSampleRateFromIndex(uint8_t index) {
    if (index >= 13) {
        return 0;   // 13/14 保留、15 = 显式采样率（本实现不支持）：一律 0，不当成某个值
    }
    return kSampleRates[index];
}

int aacIndexFromSampleRate(uint32_t sample_rate) {
    if (sample_rate == 0) {
        return -1;
    }
    for (uint8_t i = 0; i < 13; ++i) {
        if (kSampleRates[i] == sample_rate) {
            return i;
        }
    }
    return -1;   // 表外采样率（如 7350 以外的非标准值）不猜
}

bool parseAdtsHeader(const uint8_t *data, size_t size, AdtsHeader *out) {
    if (data == nullptr || out == nullptr || size < 7) {
        return false;
    }
    // syncword 12 位 = 0xFFF
    if (data[0] != 0xFF || (data[1] & 0xF0) != 0xF0) {
        return false;
    }
    const uint8_t layer = static_cast<uint8_t>((data[1] >> 1) & 0x03);
    if (layer != 0) {
        return false;   // ADTS 规定 layer 必须为 0
    }

    AdtsHeader h;
    h.mpeg2 = (data[1] & 0x08) != 0;
    h.protection_absent = (data[1] & 0x01) != 0;
    h.profile = static_cast<uint8_t>((data[2] >> 6) & 0x03);
    h.sampling_frequency_index = static_cast<uint8_t>((data[2] >> 2) & 0x0F);
    h.channel_configuration = static_cast<uint8_t>(((data[2] & 0x01) << 2) | ((data[3] >> 6) & 0x03));
    h.frame_length = static_cast<uint16_t>(((data[3] & 0x03) << 11) | (data[4] << 3) |
                                          ((data[5] >> 5) & 0x07));
    h.header_bytes = h.protection_absent ? 7 : 9;
    h.raw_blocks = static_cast<uint8_t>(data[6] & 0x03);

    if (size < h.header_bytes) {
        return false;   // 有 CRC 但连 9 字节都没有
    }
    if (aacSampleRateFromIndex(h.sampling_frequency_index) == 0) {
        return false;   // 表外采样率索引：不猜
    }
    if (h.channel_configuration == 0) {
        return false;   // 声道由 PCE 定义（流内配置），本实现不支持
    }
    if (h.frame_length < h.header_bytes) {
        return false;   // 帧长连头都装不下
    }
    *out = h;
    return true;
}

bool buildAudioSpecificConfig(const AdtsHeader &header, uint8_t out[2]) {
    if (out == nullptr) {
        return false;
    }
    const uint32_t aot = static_cast<uint32_t>(header.profile) + 1;   // profile = AOT - 1
    if (aot > 31 || header.sampling_frequency_index > 12 || header.channel_configuration > 7) {
        return false;   // 超出 ASC 位宽：不截断
    }
    // ASC = AOT(5) | samplingFrequencyIndex(4) | channelConfiguration(4) = 13 bit
    //   例：AAC-LC / 44100 / 立体声 → 12 10
    out[0] = static_cast<uint8_t>((aot << 3) | ((header.sampling_frequency_index >> 1) & 0x07));
    out[1] = static_cast<uint8_t>(((header.sampling_frequency_index & 0x01) << 7) |
                                  (header.channel_configuration << 3));
    return true;
}

bool adtsToRawAac(const uint8_t *data, size_t size, std::vector<uint8_t> *out) {
    if (data == nullptr || out == nullptr || size < 7) {
        return false;
    }
    std::vector<uint8_t> raw;
    raw.reserve(size);
    size_t pos = 0;
    size_t frames = 0;
    while (pos < size) {
        AdtsHeader h;
        if (!parseAdtsHeader(data + pos, size - pos, &h)) {
            return false;
        }
        if (h.raw_blocks != 0) {
            // 一帧多块（number_of_raw_data_blocks_in_frame != 0）：本实现不做，明确失败
            return false;
        }
        if (h.frame_length > size - pos) {
            return false;   // 半帧：宁可失败，也不静默丢尾巴
        }
        raw.insert(raw.end(), data + pos + h.header_bytes, data + pos + h.frame_length);
        pos += h.frame_length;
        ++frames;
    }
    if (frames == 0 || raw.empty()) {
        return false;
    }
    *out = std::move(raw);
    return true;
}

bool buildAdtsHeader(uint8_t profile, uint8_t sampling_frequency_index, uint8_t channel_configuration,
                     size_t payload_size, uint8_t out[7]) {
    if (out == nullptr) {
        return false;
    }
    if (aacSampleRateFromIndex(sampling_frequency_index) == 0) {
        return false;   // 表外采样率索引：不猜
    }
    if (channel_configuration == 0 || channel_configuration > 7) {
        return false;   // 0 = PCE（流内定义），本实现不支持
    }
    if (payload_size == 0 || payload_size > 8191u - 7u) {
        return false;   // frame_length 只有 13 bit
    }
    const size_t frame_length = payload_size + 7u;
    out[0] = 0xFF;
    out[1] = 0xF1;   // syncword 低 4 位=1111 | ID=0(MPEG-4) | layer=00 | protection_absent=1
    out[2] = static_cast<uint8_t>((profile << 6) | ((sampling_frequency_index & 0x0F) << 2) |
                                  ((channel_configuration >> 2) & 0x01));
    out[3] = static_cast<uint8_t>(((channel_configuration & 0x03) << 6) |
                                  ((frame_length >> 11) & 0x03));
    out[4] = static_cast<uint8_t>((frame_length >> 3) & 0xFF);
    out[5] = static_cast<uint8_t>(((frame_length & 0x07) << 5) | 0x1F);
    out[6] = 0xFC;   // buffer_fullness 低 6 位全 1（合法）| number_of_raw_data_blocks=0
    return true;
}

bool rawAacToAdts(const uint8_t *asc, size_t asc_size, const uint8_t *raw, size_t raw_size,
                  std::vector<uint8_t> *out) {
    // asc_size 允许 > 2：MP4 的 ASC 常见是 5 字节（核心 2 字节 + SBR/PS 扩展）。
    // ADTS 只能表达核心字段，扩展位被忽略 —— 调用方（MpegTsMuxer）会把这件事实 Warn 出来。
    if (asc == nullptr || raw == nullptr || out == nullptr || asc_size < 2 || raw_size == 0) {
        return false;
    }
    // ASC = AOT(5) | samplingFrequencyIndex(4) | channelConfiguration(4)
    const uint32_t aot = (static_cast<uint32_t>(asc[0]) >> 3) & 0x1F;
    const uint8_t sfi = static_cast<uint8_t>(((asc[0] & 0x07) << 1) | ((asc[1] >> 7) & 0x01));
    const uint8_t cc = static_cast<uint8_t>((asc[1] >> 3) & 0x0F);
    // ADTS 的 profile 只有 2 bit，只能表示 AOT 1..4（AAC Main/LC/SSR/LTP）；
    // HE-AAC（AOT=5）等**明确拒绝**，不硬塞成 LC（那会变成"能播但音调不对"）
    if (aot < 1 || aot > 4) {
        return false;
    }
    uint8_t header[7] = {0};
    if (!buildAdtsHeader(static_cast<uint8_t>(aot - 1), sfi, cc, raw_size, header)) {
        return false;
    }
    out->clear();
    out->reserve(7 + raw_size);
    out->insert(out->end(), header, header + 7);
    out->insert(out->end(), raw, raw + raw_size);
    return true;
}

} // namespace mzmedia
