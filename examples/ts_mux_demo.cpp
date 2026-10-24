/*
 * ts_mux_demo：把媒体文件 remux 成 MPEG-TS 落盘（M9 的验收工具）
 * ============================================================================
 * 用法：ts_mux_demo <输入文件> [输出.ts]
 *
 * 为什么要有它：`MpegTsMuxer` 是纯函数式的（只产出字节），单元断言只能证明"我以为的布局对"。
 * 这个 example 把「demux → PES → TS 包 → 落盘」打通，**交给 scripts/ts_mux_test.sh 用 ffprobe 校验**：
 * 编出来的 TS 到底能不能被独立解码器认出 h264 + aac。这是"字节拼得对不对"最硬的证据。
 * ============================================================================
 */

#include "mzmedia.h"

#include <cstdio>
#include <string>

using namespace mzmedia;

int main(int argc, char **argv) {
    if (argc < 2) {
        std::printf("用法：%s <输入文件> [输出.ts]\n", argv[0]);
        return 1;
    }
    const std::string input = argv[1];
    const std::string output = (argc >= 3) ? argv[2] : "/tmp/mzmedia_demo.ts";

    DemuxerProducer producer;
    if (!producer.open(input)) {
        std::printf("打开失败：%s\n", producer.lastError().c_str());
        return 1;
    }
    const StreamInfo *video = producer.muxerVideo();
    const StreamInfo *audio = producer.muxerAudio();
    std::printf("输入：%s\n  视频：%s\n  音频：%s\n", input.c_str(),
                video != nullptr ? (video->codec_name + (video->extradata ? "（带 avcC）" : "")).c_str()
                                 : "(无)",
                audio != nullptr ? (audio->codec_name + (audio->extradata ? "（带 ASC）" : "")).c_str()
                                 : "(无)");

    MpegTsMuxer muxer(MpegTsMuxer::Streams{video, audio});
    const MpegTsMuxer::Result prepared = muxer.prepare();
    if (prepared != MpegTsMuxer::Result::Ok) {
        std::printf("TS 封装无法开始：%s —— %s\n", MpegTsMuxer::resultName(prepared),
                    muxer.lastError().c_str());
        return 1;
    }

    std::string ts;
    if (muxer.writePatPmt(&ts) != MpegTsMuxer::Result::Ok) {
        std::printf("写 PAT/PMT 失败：%s\n", muxer.lastError().c_str());
        return 1;
    }

    uint64_t video_packets = 0;
    uint64_t audio_packets = 0;
    uint64_t key_frames = 0;
    while (true) {
        MediaPacket::Ptr packet;
        std::string error;
        const SourcePump::ReadResult result = producer.read(&packet, &error);
        if (result == SourcePump::ReadResult::EndOfStream) {
            break;
        }
        if (result == SourcePump::ReadResult::Error) {
            std::printf("读包失败：%s\n", error.c_str());
            return 1;
        }
        const MpegTsMuxer::Result written = muxer.writePacket(*packet, &ts);
        if (written != MpegTsMuxer::Result::Ok) {
            std::printf("封装失败：%s —— %s\n", MpegTsMuxer::resultName(written),
                        muxer.lastError().c_str());
            return 1;
        }
        if (packet->kind() == MediaKind::Video) {
            ++video_packets;
            if (packet->isKeyFrame()) {
                ++key_frames;
            }
        } else {
            ++audio_packets;
        }
    }

    if (!saveFile(output, ts)) {
        std::printf("写文件失败：%s\n", output.c_str());
        return 1;
    }
    std::printf("已写出：%s（%zu 字节 / %llu 个 TS 包；视频 %llu 包、音频 %llu 包、关键帧 %llu；"
                "补 SPS/PPS %llu 次、PCR %llu 次）\n",
                output.c_str(), ts.size(), static_cast<unsigned long long>(muxer.tsPackets()),
                static_cast<unsigned long long>(video_packets),
                static_cast<unsigned long long>(audio_packets),
                static_cast<unsigned long long>(key_frames),
                static_cast<unsigned long long>(muxer.spsPpsInjected()),
                static_cast<unsigned long long>(muxer.pcrCount()));
    return 0;
}
