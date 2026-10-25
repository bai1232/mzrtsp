/*
 * PacketStage：把"一路流的包"加工成"输出层能吃的包"（v0.2.3 重构抽出）
 * ============================================================================
 * 为什么要有这层：`DemuxerProducer` 在 v0.1–v0.2 期间长成了一条"隐式流水线"——
 * 读包、跳过非音视频、时间戳偏移、Annex-B→AVCC、ADTS→raw AAC、转码编排、循环接缝对齐
 * 全挤在同一个类里（789 行 / 22 个成员函数）。结果是**每加一种输入形状或能力都要改那个类**
 * （M8 输入形状 +125 行、M10-b 转码 +260 行），违反开闭原则。
 *
 * 现在切成两半：
 *   · `SourceSession`：负责"**一次源的读**"（打开/读包/跳过/EOF/循环重开/时间戳与偏移）；
 *   · `PacketStage`：负责"**一路流的形状**"（一种实现 = 一种加工），这里定义的接口。
 *
 * 一条链（`std::vector<PacketStage::Ptr>`）从"源形状"加工到"输出形状"：
 *   MP4（avcC+AVCC）        → [Raw]
 *   H264 裸流 / TS 的 H264   → [AnnexB]（或 [AnnexB, Transcode]）
 *   MP4 + 转码               → [Raw, Transcode]
 *   任意 H264 + 转码         → [..., Transcode]
 *   加"滤镜/水印"（v0.3）    → 再插一个阶段，**不改这里、不改 producer**
 *
 * 时间戳契约（**别改**）：
 *   · 链上任何阶段都只处理**原始**时间戳，不许"顺手加趟偏移"；
 *   · 趟偏移由 `DemuxerProducer` 在**链尾**统一加，**除非**链里有阶段 `ownsTimeline()`
 *     （转码自己生成连续时间轴：帧序号 × 步长 + 接缝修正；再加趟偏移会双计）。
 * ============================================================================
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ffmpeg/demuxer.h" // StreamInfo / Demuxer::Limits
#include "media/media_packet.h"

namespace mzmedia {

/// 阶段需要的**只读**上下文（ADTS 探针要重新打开同一个源；其它阶段用不到）
struct StageContext {
    std::string path;      ///< 源路径（探针用）
    Demuxer::Limits demux; ///< 解封装上限（探针跟着源走）
};

/**
 * 一路流的加工阶段（**无状态或自持状态**，不碰线程、不碰网络、不碰文件）
 * @note 只处理**一种流**（视频或音频）：`DemuxerProducer` 各持一条链。
 */
class PacketStage {
public:
    using Ptr = std::unique_ptr<PacketStage>;
    virtual ~PacketStage() = default;

    /// 名字（日志/诊断用）
    virtual const char *name() const = 0;

    /**
     * 打开期：拿**源**流快照，产出**输出**流快照
     * @param src 解封装给出的流信息（只读；里面可能是 Annex-B / ADTS / hvcC 等"输入形状"）
     * @param out 输出流信息：编码/宽高/extradata 可能被改写（转码会改宽高与 avcC）
     * @return false = 明确失败（`error` 是人话；**不猜**、不降级）
     */
    virtual bool prepare(const StreamInfo &src, const StageContext &ctx, StreamInfo *out,
                         std::string *error) = 0;

    /**
     * 处理一个包（MediaPacket → 0..N 个 MediaPacket）
     * @param in 输入包（时间戳是**原始**的；本阶段输出也必须保持原始时间戳，
     *              除非它 `ownsTimeline()`）
     * @param out 产物（顺序即输出顺序）
     * @note 转换类 1:1；转码类可能 0（解码器攒帧）或 N（编码器补齐）
     */
    virtual bool process(const MediaPacket &in, std::vector<MediaPacket::Ptr> *out,
                         std::string *error) = 0;

    /**
     * 本阶段是否**自己拥有时间轴**（转码 = true）
     * @note 生产者据此决定"要不要在链尾加趟偏移"：转换类不加就跟不上循环重开的时间轴，
     *       转码类加了会**双计**（v0.2.1 实测：视频跑到 14s、音频 8s）
     */
    virtual bool ownsTimeline() const {
        return false;
    }

    /**
     * 源结束（**非循环**）时把攒着的残余吐出来
     * @note 转码必须实现（漏了就是少最后几帧且不报错）；转换类是空实现
     */
    virtual bool flush(std::vector<MediaPacket::Ptr> *out, std::string *error) {
        (void) out;
        (void) error;
        return true;
    }

    /**
     * 循环重开的接缝：`pass_length_ms` = 这一趟的**真实长度**（`pass_end − pass_start`）
     * @note 转码用它对齐自造的时间轴；转换类忽略（它们的时间戳由生产者按偏移算，天然连续）
     */
    virtual void onSeam(int64_t pass_length_ms) {
        (void) pass_length_ms;
    }

    /// 本阶段"转换过"的包数（Raw/转码为 0；AnnexB/Adts 是处理过的包数）——stats 用
    virtual uint64_t convertedPackets() const {
        return 0;
    }

    /// JSON 片段（**不含**外层花括号）；默认空，转码会填 speed 等
    virtual std::string statsJson() const {
        return {};
    }
};

} // namespace mzmedia
