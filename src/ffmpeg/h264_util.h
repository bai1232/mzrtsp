/*
 * h264_util：Annex-B 切分 + SPS/PPS 提取 + SPS 分辨率解析（M4-b）
 * ============================================================================
 * 形状来源：docs/DESIGN_M4.md §4（M4 的第二条验收：裸流能提 SPS/PPS 并报出分辨率）
 *
 * 为什么自己解 SPS：FLV 的 `AVCDecoderConfigurationRecord` 要的是**不带起始码**的
 * SPS/PPS，而裸流是 Annex-B（带 00 00 01）；同时 M6 要用分辨率决定播放器尺寸。
 * 自己解的好处是**可单测**（不需要解码器、不需要样本也能造数据）。
 *
 * 全整数实现（exp-Golomb），失败一律返回 false —— **不猜、不返回 0x0 假成功**。
 * ============================================================================
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mzmedia {

/// Annex-B 里的一个 NAL（data/size **不含**起始码；type 已剥掉 NAL header 的高位）
struct H264Nal {
    const uint8_t *data = nullptr;
    size_t size = 0;
    uint8_t type = 0;
};

/// 按起始码（00 00 01 / 00 00 00 01）切分
/// @return NAL 个数（0 = 没有一个合法起始码）
/// @note 只切分，不做 emulation prevention 反转义（那是解码器的事）
size_t splitAnnexBNals(const uint8_t *data, size_t size, std::vector<H264Nal> *out);

/// 提取第一个 SPS(type=7) 与 PPS(type=8)（**含**各自 NAL header 字节，不含起始码）
/// @return true = 两个都找到了；false = 缺任意一个（调用方必须处理，不能假设存在）
bool extractSpsPps(const uint8_t *data, size_t size, std::vector<uint8_t> *sps,
                   std::vector<uint8_t> *pps);

/// 解析 SPS 得到显示宽高（已按 frame_cropping 修正）
/// @param sps 含 NAL header（0x67...）的 SPS 字节
/// @return true = **语法上**解析成功；false = 解析不了（数据太短/exp-Golomb 越界/尺寸不在合理范围）
/// @warning SPS **没有校验和**：随机字节也可能凑出"语法合法"的结果（此时分辨率不可信）。
///          所以**调用方必须与解封装/E码流得到的信息交叉校验**，不能只看这里的返回值 ——
///          M4-b 的用例就是这么做的（裸流解析结果 vs Demuxer 的 width/height）。
bool parseSpsDimension(const uint8_t *sps, size_t size, int *width, int *height);

} // namespace mzmedia
