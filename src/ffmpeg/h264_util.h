/*
 * h264_util：Annex-B 切分 + SPS/PPS 提取 + SPS 分辨率解析（M4-b）+ Annex-B→AVCC（M6-c）
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

// ---------------------------------------------------------------------------
// M6-c：Annex-B → AVCC（FLV 与 MP4 要的都是 length-prefixed 形状）
//
// 为什么必须做：H264 裸流（.h264 / TS 里的 H264）是 Annex-B —— 用 `00 00 01` 起始码分隔 NAL；
//   而 FLV 的视频 tag 要求 **4 字节大端长度** 前缀，且初始化头必须是 avcC（不是裸的 SPS/PPS）。
//   两者不能混：把 Annex-B 直接塞进 FLV，播放端会把它当长度前缀解析 → 立刻花屏/报错
//   （这种"错得很安静"的流最难查，所以 M6-a 的选择是**明确拒绝**，M6-c 在这里补齐转换）。
//
// 三个函数都是**纯函数**（只用入参、只写 out）：不需要解码器、不需要样本文件就能单测。
// ---------------------------------------------------------------------------

/// 判断数据是否"像 Annex-B"（以 `00 00 01` / `00 00 00 01` 开头）
/// @note 只做判定，**不修数据**。判据可靠的原因：avcC 的首字节固定是 1
///       （`configurationVersion`），不会与起始码混淆（`FlvMuxer::prepare()` 用同一判据）
bool looksLikeAnnexB(const uint8_t *data, size_t size);

/**
 * Annex-B → AVCC：把每个 NAL 的起始码换成 4 字节大端长度
 * @param out **会被清空**后重填（一个包一次调用，语义唯一；不做"追加"这种能写错的接口）
 * @return true = 至少转换出一个 NAL；false = 没有一个合法起始码 / NAL 长度超硬上限
 * @note 失败时 out 是空的 —— **绝不"原样拷一份"冒充成功**（那正是花屏的成因）
 */
bool annexBToAvcc(const uint8_t *data, size_t size, std::vector<uint8_t> *out);

/**
 * 用 SPS/PPS 构造 `AVCDecoderConfigurationRecord`（= FLV 的 AVC sequence header 内容）
 * @param sps/pps 含各自 NAL header（0x67 / 0x68），**不含**起始码，长度 ≤ 65535
 * @return true = 构造成功；false = 参数非法（空 / 超长 / nal_unit_type 不是 7 / 8）
 * @note 只写基础字段（version/profile/compat/level/lengthSize=4/SPS/PPS）。
 *       high profile 的扩展字段（chroma_format_idc 等）**刻意不写**：FLV 播放端从 SPS 里自己读，
 *       M6-c 的验收就是用 `ffprobe`/`ffmpeg` 真解一遍，而不是靠"字段看起来齐了"自我确认
 */
bool buildAvcC(const uint8_t *sps, size_t sps_size, const uint8_t *pps, size_t pps_size,
               std::vector<uint8_t> *out);

} // namespace mzmedia
