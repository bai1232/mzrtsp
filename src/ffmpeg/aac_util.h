/*
 * aac_util：ADTS 头解析 + AudioSpecificConfig 构造 + ADTS → raw AAC（M8 / FR-2.2）
 * ============================================================================
 * 形状来源：docs/DESIGN_M4.md §3.3、docs/CODEC_MATRIX.md §5「AAC 封装」
 *
 * 为什么需要这一层：
 *   MP4/MKV 里 AAC 是 **raw AAC + AudioSpecificConfig**（ASC，2 字节，放在 extradata 里）；
 *   MPEG-TS 里 AAC 是 **ADTS**（每帧自带 7/9 字节头，采样率/声道在头里自描述），
 *   `avformat` 对 TS 的 AAC **不给 extradata**。而 FLV 要的是「ASC 初始化头 + raw AAC 帧」，
 *   所以 TS 输入必须做一次**轻量转换**（CODEC_MATRIX 的 REPACK 档，不解码）：
 *     ① 从首帧 ADTS 头造出 2 字节 ASC（给 FLV 的 AAC sequence header）；
 *     ② 每帧剥掉 ADTS 头（给 FLV 的 raw AAC tag）。
 *
 * 为什么做成纯函数、且**不 include 任何 libav\*** ：ADTS 是纯字节语法，可脱库单测
 * （手造字节即可覆盖有 CRC / 无 CRC / 表外采样率 / 畸形长度），不必依赖样本文件与解码器。
 *
 * **不猜**：表外采样率索引、非法 profile、帧长越界一律返回 false（调用方必须明确失败）。
 * ============================================================================
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mzmedia {

/**
 * 一个 ADTS 帧的头（**只含头字段**，payload 不在里面）
 * 布局（ISO/IEC 13818-7）：
 *   byte0      : 0xFF           （syncword 高 8 位）
 *   byte1      : 0xF?           （syncword 低 4 位 + ID(1) + layer(2) + protection_absent(1)）
 *   byte2      : profile(2) | sampling_frequency_index(4) | private(1) | channel_config 高位(1)
 *   byte3      : channel_config 低 2 位 | ... | frame_length 高 2 位
 *   byte4      : frame_length 中 8 位
 *   byte5      : frame_length 低 3 位 | buffer_fullness 高 5 位
 *   byte6      : buffer_fullness 低 6 位 | number_of_raw_data_blocks_in_frame(2)
 *   byte7/8    : CRC（仅 protection_absent == false 时存在）
 */
struct AdtsHeader {
    bool protection_absent = true;       ///< true = 无 CRC（头 7 字节）；false = 有 CRC（头 9 字节）
    bool mpeg2 = false;                  ///< ID 位：true = MPEG-2 ADTS
    uint8_t profile = 0;                 ///< 2 bit：**AAC object type - 1**（AAC-LC = 1）
    uint8_t sampling_frequency_index = 0;///< 4 bit：0..12 有效（15 = 显式采样率，本实现不支持）
    uint8_t channel_configuration = 0;   ///< 3 bit：0 = 由 PCE 定义（本实现不支持）
    uint16_t frame_length = 0;           ///< 整帧长度（**含**头与 CRC）
    uint8_t header_bytes = 7;            ///< 7 或 9
    uint8_t raw_blocks = 0;              ///< number_of_raw_data_blocks_in_frame（0 = 1 个块）
};

/// ADTS 采样率索引 → 采样率（Hz）。表外（含 15）返回 **0**（不猜、不返回默认值）
uint32_t aacSampleRateFromIndex(uint8_t index);

/// 采样率（Hz）→ ADTS 采样率索引。表外返回 **-1**
int aacIndexFromSampleRate(uint32_t sample_rate);

/**
 * 解析 ADTS 头
 * @param data/size 至少 7 字节（protection_absent == false 时至少 9 字节）
 * @return false = 语法不合法（同步字不对 / layer 非 0 / 表外索引 / channel_configuration == 0 /
 *                 帧长越界…）。**调用方必须当失败处理**
 * @note 只承诺**语法**：ADTS 没有校验和，随机字节可能凑出语法合法的头；
 *       调用方应交叉校验（例如与 `Demuxer` 报出的 `sample_rate` / `channels` 比对）
 */
bool parseAdtsHeader(const uint8_t *data, size_t size, AdtsHeader *out);

/**
 * 由 ADTS 头构造 2 字节 `AudioSpecificConfig`
 *   ASC = audioObjectType(5) | samplingFrequencyIndex(4) | channelConfiguration(4)  → 13 bit
 *   其中 audioObjectType = profile + 1（AAC-LC → 2）
 * @return false = 字段超出 ASC 的位宽（profile + 1 > 31 等），不静默截断
 */
bool buildAudioSpecificConfig(const AdtsHeader &header, uint8_t out[2]);

/**
 * ADTS → raw AAC：逐帧剥掉头（含 CRC），把 payload 顺序拼起来
 * @param data/size 必须是**完整的 ADTS 帧列表**（半帧 → false）
 * @return false = 同步字丢失 / 帧长越界 / 帧不完整（**不静默丢弃半帧**）
 * @note `raw_blocks != 0`（一帧多块）本实现**不支持**：明确失败，不假装转过
 */
bool adtsToRawAac(const uint8_t *data, size_t size, std::vector<uint8_t> *out);

/**
 * 【M9】构造 7 字节 ADTS 头（`protection_absent = 1`，不带 CRC）
 * @param payload_size ADTS 头**之后**的字节数（整帧长 = 7 + payload_size，13 bit 上限 8191）
 * @return false = 采样率索引表外 / channel_configuration 不在 1..7 / 帧长超过 8191
 */
bool buildAdtsHeader(uint8_t profile, uint8_t sampling_frequency_index, uint8_t channel_configuration,
                     size_t payload_size, uint8_t out[7]);

/**
 * 【M9】**raw AAC → ADTS 帧**（方向与 `adtsToRawAac` 相反）
 * @param asc 2 字节 `AudioSpecificConfig`（来自 `StreamInfo::extradata`）
 * @return false = ASC 长度不是 2 / AOT 无法用 ADTS 的 2 bit profile 表示（如 HE-AAC AOT=5）/
 *         采样率索引表外 / 帧长超上限
 * @note 为什么需要：`MediaPacket` 里的音频是 **raw AAC**（FLV 要），而 MPEG-TS/HLS 要 **ADTS**
 *       （每帧自带头）。HLS 切片由输出层重新加头，输入层不用动。
 */
bool rawAacToAdts(const uint8_t *asc, size_t asc_size, const uint8_t *raw, size_t raw_size,
                  std::vector<uint8_t> *out);

} // namespace mzmedia
