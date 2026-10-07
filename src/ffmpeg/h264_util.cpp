/*
 * h264_util 实现（M4-b）
 * ============================================================================
 * 位读取用 exp-Golomb（H.264 的基本编码单元）。三条纪律：
 *   1) 每一次读取都检查还有没有位 —— 越界立即失败（不读脏内存）；
 *   2) `parseSpsDimension` 只认自己需要的那几个字段，但**必须按规范顺序跳过**前面的
 *      可选字段（high profile 的 scaling list 最容易漏，漏了后面全错位）；
 *   3) 不返回"看起来合理"的假值：解析不出来就是 false。
 * ============================================================================
 */

#include "ffmpeg/h264_util.h"

#include <cstring>

namespace mzmedia {

namespace {

/// 极简位读取器（MSB first）
class BitReader {
public:
    BitReader(const uint8_t *data, size_t size) : _data(data), _size(size) {}

    bool readBits(int n, uint32_t *out) {
        if (n < 0 || n > 32 || _bit_pos + static_cast<size_t>(n) > _size * 8) {
            return false;
        }
        uint32_t value = 0;
        for (int i = 0; i < n; ++i) {
            const size_t byte = _bit_pos >> 3;
            const int shift = 7 - static_cast<int>(_bit_pos & 7);
            value = (value << 1) | ((_data[byte] >> shift) & 1u);
            ++_bit_pos;
        }
        *out = value;
        return true;
    }

    /// 无符号 exp-Golomb：前导零个数 + 1 位 1，再读同样多位的后缀
    bool readUE(uint32_t *out) {
        int zeros = 0;
        for (;;) {
            uint32_t bit = 0;
            if (!readBits(1, &bit)) {
                return false;
            }
            if (bit != 0) {
                break;
            }
            if (++zeros > 32) {
                return false;   // 畸形：前导零不可能这么多
            }
        }
        uint32_t suffix = 0;
        if (zeros > 0 && !readBits(zeros, &suffix)) {
            return false;
        }
        if (zeros == 32) {
            return false;
        }
        *out = (1u << zeros) - 1u + suffix;
        return true;
    }

    /// 有符号 exp-Golomb
    bool readSE(int32_t *out) {
        uint32_t code = 0;
        if (!readUE(&code)) {
            return false;
        }
        const int32_t k = static_cast<int32_t>((code + 1) / 2);
        *out = (code % 2 == 1) ? k : -k;
        return true;
    }

private:
    const uint8_t *_data;
    size_t _size;
    size_t _bit_pos = 0;
};

/// 跳过 scaling list（high profile 才出现）——漏掉它后面所有字段都会错位
bool skipScalingList(BitReader &br, int size_of_list) {
    int last_scale = 8;
    int next_scale = 8;
    for (int j = 0; j < size_of_list; ++j) {
        if (next_scale != 0) {
            int32_t delta_scale = 0;
            if (!br.readSE(&delta_scale)) {
                return false;
            }
            next_scale = (last_scale + delta_scale + 256) % 256;
        }
        last_scale = (next_scale == 0) ? last_scale : next_scale;
    }
    return true;
}

constexpr uint8_t kNalSps = 7;
constexpr uint8_t kNalPps = 8;

/// 单个 NAL 的硬上限：与 `Demuxer::Limits::max_packet_size` 同量级。
/// 一个 NAL 比整个包上限还大 = 数据已经不对了，宁可失败也不要"照长度写出去"
constexpr size_t kMaxNalSize = 8u * 1024u * 1024u;

void appendU16Be(std::vector<uint8_t> *out, size_t value) {
    out->push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    out->push_back(static_cast<uint8_t>(value & 0xFFu));
}

void appendU32Be(std::vector<uint8_t> *out, size_t value) {
    out->push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
    out->push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
    out->push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    out->push_back(static_cast<uint8_t>(value & 0xFFu));
}

} // namespace

size_t splitAnnexBNals(const uint8_t *data, size_t size, std::vector<H264Nal> *out) {
    if (data == nullptr || out == nullptr) {
        return 0;
    }
    out->clear();
    size_t i = 0;
    // 找第一个起始码
    size_t start = 0;
    bool found = false;
    while (i + 2 < size) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            start = i + 3;
            i = start;
            found = true;
            break;
        }
        ++i;
    }
    if (!found) {
        return 0;
    }
    while (i < size) {
        if (i + 2 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            // 当前 NAL 结束于 i（去掉尾部可能多出的一个 0 字节）
            size_t end = i;
            while (end > start && data[end - 1] == 0) {
                --end;
            }
            if (end > start) {
                H264Nal nal;
                nal.data = data + start;
                nal.size = end - start;
                nal.type = static_cast<uint8_t>(nal.data[0] & 0x1Fu);
                out->push_back(nal);
            }
            start = i + 3;
            i = start;
            continue;
        }
        ++i;
    }
    // 最后一个 NAL
    size_t end = size;
    while (end > start && data[end - 1] == 0) {
        --end;
    }
    if (end > start) {
        H264Nal nal;
        nal.data = data + start;
        nal.size = end - start;
        nal.type = static_cast<uint8_t>(nal.data[0] & 0x1Fu);
        out->push_back(nal);
    }
    return out->size();
}

bool extractSpsPps(const uint8_t *data, size_t size, std::vector<uint8_t> *sps,
                   std::vector<uint8_t> *pps) {
    if (data == nullptr || sps == nullptr || pps == nullptr) {
        return false;
    }
    sps->clear();
    pps->clear();
    std::vector<H264Nal> nals;
    if (splitAnnexBNals(data, size, &nals) == 0) {
        return false;
    }
    for (const auto &nal : nals) {
        if (nal.type == kNalSps && sps->empty()) {
            sps->assign(nal.data, nal.data + nal.size);
        } else if (nal.type == kNalPps && pps->empty()) {
            pps->assign(nal.data, nal.data + nal.size);
        }
        if (!sps->empty() && !pps->empty()) {
            return true;
        }
    }
    return false;   // 缺任一 → 明确失败
}

bool parseSpsDimension(const uint8_t *sps, size_t size, int *width, int *height) {
    if (sps == nullptr || size < 4 || width == nullptr || height == nullptr) {
        return false;
    }
    BitReader br(sps + 1, size - 1);   // 跳过 NAL header
    uint32_t profile_idc = 0;
    uint32_t tmp = 0;
    if (!br.readBits(8, &profile_idc) || !br.readBits(8, &tmp) || !br.readBits(8, &tmp)) {
        return false;   // profile / constraint flags / level
    }
    if (!br.readUE(&tmp)) {   // seq_parameter_set_id
        return false;
    }

    bool separate_colour_plane = false;
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 || profile_idc == 244 ||
        profile_idc == 44 || profile_idc == 83 || profile_idc == 86 || profile_idc == 118 ||
        profile_idc == 128 || profile_idc == 138 || profile_idc == 139 || profile_idc == 134 ||
        profile_idc == 135) {
        uint32_t chroma_format_idc = 0;
        if (!br.readUE(&chroma_format_idc)) {
            return false;
        }
        if (chroma_format_idc == 3) {
            uint32_t flag = 0;
            if (!br.readBits(1, &flag)) {
                return false;
            }
            separate_colour_plane = (flag != 0);
        }
        if (!br.readUE(&tmp) || !br.readUE(&tmp)) {   // bit_depth_luma/chroma
            return false;
        }
        uint32_t qpprime = 0;
        uint32_t scaling_present = 0;
        if (!br.readBits(1, &qpprime) || !br.readBits(1, &scaling_present)) {
            return false;
        }
        if (scaling_present != 0) {
            const int lists = (chroma_format_idc != 3) ? 8 : 12;
            for (int i = 0; i < lists; ++i) {
                uint32_t list_present = 0;
                if (!br.readBits(1, &list_present)) {
                    return false;
                }
                if (list_present != 0 && !skipScalingList(br, i < 6 ? 16 : 64)) {
                    return false;
                }
            }
        }
    }

    if (!br.readUE(&tmp)) {   // log2_max_frame_num_minus4
        return false;
    }
    uint32_t pic_order_cnt_type = 0;
    if (!br.readUE(&pic_order_cnt_type)) {
        return false;
    }
    if (pic_order_cnt_type == 0) {
        if (!br.readUE(&tmp)) {
            return false;
        }
    } else if (pic_order_cnt_type == 1) {
        uint32_t flag = 0;
        int32_t se = 0;
        if (!br.readBits(1, &flag) || !br.readSE(&se) || !br.readSE(&se) || !br.readUE(&tmp)) {
            return false;
        }
        for (uint32_t i = 0; i < tmp; ++i) {
            if (!br.readSE(&se)) {
                return false;
            }
        }
    }
    if (!br.readUE(&tmp)) {   // max_num_ref_frames
        return false;
    }
    uint32_t gaps = 0;
    if (!br.readBits(1, &gaps)) {
        return false;
    }

    uint32_t width_mbs = 0;
    uint32_t height_map_units = 0;
    if (!br.readUE(&width_mbs) || !br.readUE(&height_map_units)) {
        return false;
    }
    uint32_t frame_mbs_only = 0;
    if (!br.readBits(1, &frame_mbs_only)) {
        return false;
    }
    int w = static_cast<int>(width_mbs + 1) * 16;
    int h = static_cast<int>(height_map_units + 1) * 16 * (frame_mbs_only != 0 ? 1 : 2);

    if (frame_mbs_only == 0) {
        uint32_t mb_adaptive = 0;
        if (!br.readBits(1, &mb_adaptive)) {
            return false;
        }
    }
    uint32_t direct_8x8 = 0;
    if (!br.readBits(1, &direct_8x8)) {
        return false;
    }
    uint32_t cropping = 0;
    if (!br.readBits(1, &cropping)) {
        return false;
    }
    if (cropping != 0) {
        uint32_t left = 0, right = 0, top = 0, bottom = 0;
        if (!br.readUE(&left) || !br.readUE(&right) || !br.readUE(&top) || !br.readUE(&bottom)) {
            return false;
        }
        // 裁剪单位：色度采样相关（4:2:0 → 2x2；4:2:2 → 2x1；4:4:4/单色 → 1x1）
        const int chroma_array_type = separate_colour_plane ? 0 : 1;
        const int sub_width = (chroma_array_type == 0) ? 1 : 2;
        const int sub_height = (chroma_array_type == 0) ? ((frame_mbs_only != 0) ? 1 : 2) : 2;
        w -= static_cast<int>(sub_width * (left + right));
        h -= static_cast<int>(sub_height * (top + bottom));
    }
    // 合理性上界：SPS 没有校验和，**随机字节也可能凑出"语法合法"的 SPS** ——
    // 所以除了 >0 还要挡住"不可能的分辨率"（否则解析器会对垃圾数据返回假成功）
    constexpr int kMaxDimension = 16384;   // 8K 级别足够，v0.1 用不到更大
    if (w <= 0 || h <= 0 || w > kMaxDimension || h > kMaxDimension) {
        return false;
    }
    *width = w;
    *height = h;
    return true;
}

// ---------------------------------------------------------------------------
// M6-c：Annex-B → AVCC + avcC 构造
// ---------------------------------------------------------------------------

bool looksLikeAnnexB(const uint8_t *data, size_t size) {
    if (data == nullptr || size < 3) {
        return false;
    }
    if (data[0] != 0 || data[1] != 0) {
        return false;
    }
    if (data[2] == 1) {
        return true; // 3 字节起始码 00 00 01
    }
    return size >= 4 && data[2] == 0 && data[3] == 1; // 4 字节 00 00 00 01
}

bool annexBToAvcc(const uint8_t *data, size_t size, std::vector<uint8_t> *out) {
    if (data == nullptr || out == nullptr) {
        return false;
    }
    out->clear();
    if (size == 0) {
        return false;
    }
    std::vector<H264Nal> nals;
    if (splitAnnexBNals(data, size, &nals) == 0) {
        return false; // 没有起始码：这不是 Annex-B（**报错**，别猜）
    }
    for (const auto &nal : nals) {
        if (nal.size == 0 || nal.size > kMaxNalSize) {
            out->clear();
            return false;
        }
        appendU32Be(out, nal.size);
        out->insert(out->end(), nal.data, nal.data + nal.size);
    }
    return !out->empty();
}

bool buildAvcC(const uint8_t *sps, size_t sps_size, const uint8_t *pps, size_t pps_size,
               std::vector<uint8_t> *out) {
    if (sps == nullptr || pps == nullptr || out == nullptr) {
        return false;
    }
    // 最短合法 SPS 也要能拿出 profile/compat/level 三个字节（+ NAL header）
    if (sps_size < 4 || sps_size > 0xFFFFu || pps_size == 0 || pps_size > 0xFFFFu) {
        return false;
    }
    if ((sps[0] & 0x1Fu) != kNalSps || (pps[0] & 0x1Fu) != kNalPps) {
        return false; // 传进来的不是 SPS/PPS：宁可失败，也不构造一个假头
    }

    out->clear();
    out->reserve(11 + sps_size + pps_size);
    out->push_back(1);        // configurationVersion
    out->push_back(sps[1]);   // AVCProfileIndication
    out->push_back(sps[2]);   // profile_compatibility
    out->push_back(sps[3]);   // AVCLevelIndication
    out->push_back(0xFF);     // 6 bits reserved(111111) + lengthSizeMinusOne(11) → NAL 长度 4 字节
    out->push_back(0xE1);     // 3 bits reserved(111) + numOfSequenceParameterSets(1)
    appendU16Be(out, sps_size);
    out->insert(out->end(), sps, sps + sps_size);
    out->push_back(1);        // numOfPictureParameterSets
    appendU16Be(out, pps_size);
    out->insert(out->end(), pps, pps + pps_size);
    return true;
}

} // namespace mzmedia
