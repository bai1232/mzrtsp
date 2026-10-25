/*
 * 纯转换阶段：Raw（透传）/ AnnexB（→AVCC）/ Adts（→raw AAC + ASC）
 * ============================================================================
 * 三者都**不碰时间戳**（只把输入包的 dts/pts 原样写进输出包），也不碰线程/网络/文件。
 * 逻辑逐字搬自 v0.2.2 的 `DemuxerProducer`：每条 `error` 的措辞、每个上限、每处"不猜"的
 * 理由都保留 —— 这样"重构不改变行为"才可验证（`scripts/refactor_equiv_check.sh` 做字节比对）。
 * ============================================================================
 */

#pragma once

#include "media/packet_stage.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mzmedia {

/// 原样透传（MP4 的 avcC + AVCC 不需要任何加工）
class RawStage final : public PacketStage {
public:
    const char *name() const override {
        return "raw";
    }
    bool prepare(const StreamInfo &src, const StageContext &ctx, StreamInfo *out,
                 std::string *error) override;
    bool process(const MediaPacket &in, std::vector<MediaPacket::Ptr> *out,
                 std::string *error) override;
};

/**
 * Annex-B（起始码分隔）→ AVCC + 现场构造 avcC
 * @note 为什么放在这里而不是 `output/`：**"输入是什么形状"是解封装层的知识**，
 *       输出层只管"我要 avcC"；转换后与 MP4 输入完全同形，`FlvMuxer`/`MpegTsMuxer` 一行都不用改。
 */
class AnnexBStage final : public PacketStage {
public:
    const char *name() const override {
        return "annexb";
    }
    bool prepare(const StreamInfo &src, const StageContext &ctx, StreamInfo *out,
                 std::string *error) override;
    bool process(const MediaPacket &in, std::vector<MediaPacket::Ptr> *out,
                 std::string *error) override;
    uint64_t convertedPackets() const override {
        return _converted;
    }

private:
    uint64_t _converted = 0;
};

/**
 * ADTS（每帧带 7/9 字节自描述头）→ raw AAC + 由首帧 ADTS 头构造 AudioSpecificConfig
 * @note 两个约束决定了它的形状（M8 踩出来的）：
 *       ① ASC **必须在 `prepare()`（= open）之内就绪** —— 连接会拿流快照的裸指针写
 *          AAC sequence header，不能"等第一个包到了再造"；
 *       ② 而造 ASC 需要**真读一帧**（ADTS 头里的 profile/采样率/声道才是权威）。
 *       做法：用**一次性探针 Demuxer** 打开同一个源、读到首个音频包就造 ASC 并关掉，
 *       主解封装的读游标**一个字节都没动**（否则首帧就得进"重放队列"）。
 */
class AdtsStage final : public PacketStage {
public:
    const char *name() const override {
        return "adts";
    }
    bool prepare(const StreamInfo &src, const StageContext &ctx, StreamInfo *out,
                 std::string *error) override;
    bool process(const MediaPacket &in, std::vector<MediaPacket::Ptr> *out,
                 std::string *error) override;
    uint64_t convertedPackets() const override {
        return _converted;
    }

private:
    uint64_t _converted = 0;
};

} // namespace mzmedia
