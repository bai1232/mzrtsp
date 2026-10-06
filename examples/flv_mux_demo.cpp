/*
 * flv_mux_demo：把媒体文件 remux 成 FLV 落盘（M6-a 的验收工具）
 * ============================================================================
 * 用法：flv_mux_demo <输入文件> [输出.flv]
 *
 * 为什么要有它：M6-a 只做"纯函数式封装"，没有网络层可验。这个 example 把
 * 「demux → 封成 FLV → 落盘」打通，**交给 `scripts/flv_mux_test.sh` 用 ffprobe 校验**：
 * 编出来的 FLV 到底是不是 h264 + aac、分辨率对不对。
 * 这是"字节拼得对不对"最硬的证据（比任何单元断言都硬）。
 * ============================================================================
 */

#include "mzmedia.h"

#include <cstdio>
#include <string>

using namespace mzmedia;

int main(int argc, char **argv) {
    if (argc < 2) {
        std::printf("用法：%s <输入文件> [输出.flv]\n", argv[0]);
        return 1;
    }
    const std::string input = argv[1];
    const std::string output = (argc >= 3) ? argv[2] : "/tmp/mzmedia_demo.flv";

    // 读包统一走 DemuxerProducer（M5-c）：它已经处理好了"AVPacket → MediaPacket"、
    // 时间戳换算、非音视频流跳过与计数 —— 这里不重复造一遍
    DemuxerProducer producer;
    if (!producer.open(input)) {
        std::printf("打开失败：%s\n", producer.lastError().c_str());
        return 1;
    }

    const Demuxer &demuxer = producer.demuxer();
    const StreamInfo *video = demuxer.firstVideo();
    const StreamInfo *audio = demuxer.firstAudio();
    std::printf("输入：%s\n", input.c_str());
    std::printf("  视频：%s %dx%d%s\n", video != nullptr ? video->codec_name.c_str() : "(无)",
                video != nullptr ? video->width : 0, video != nullptr ? video->height : 0,
                (video != nullptr && video->extradata) ? "（带 avcC）" : "");
    std::printf("  音频：%s%s\n", audio != nullptr ? audio->codec_name.c_str() : "(无)",
                (audio != nullptr && audio->extradata) ? "（带 AudioSpecificConfig）" : "");

    FlvMuxer muxer(FlvMuxer::Streams{video, audio});
    const FlvMuxer::Result prepared = muxer.prepare();
    if (prepared != FlvMuxer::Result::Ok) {
        // 明确失败：不产出半成品文件（"能连上但一片黑"是最难查的问题）
        std::printf("FLV 封装无法开始：%s —— %s\n", FlvMuxer::resultName(prepared),
                    muxer.lastError().c_str());
        return 1;
    }

    std::string flv;
    (void) muxer.writeHeader(&flv);
    const FlvMuxer::Result seq = muxer.writeSequenceHeaders(&flv);
    if (seq != FlvMuxer::Result::Ok) {
        std::printf("写 sequence header 失败：%s —— %s\n", FlvMuxer::resultName(seq),
                    muxer.lastError().c_str());
        return 1;
    }

    uint64_t video_packets = 0;
    uint64_t audio_packets = 0;
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
        const FlvMuxer::Result written = muxer.writePacket(*packet, &flv);
        if (written != FlvMuxer::Result::Ok) {
            std::printf("封装失败：%s —— %s\n", FlvMuxer::resultName(written),
                        muxer.lastError().c_str());
            return 1;
        }
        if (packet->kind() == MediaKind::Video) {
            ++video_packets;
        } else {
            ++audio_packets;
        }
    }

    if (!saveFile(output, flv)) {
        std::printf("写文件失败：%s\n", output.c_str());
        return 1;
    }
    std::printf("已写出：%s（%zu 字节 / %llu 个 tag：视频 %llu 包、音频 %llu 包，时间戳基准 %lld ms）\n",
                output.c_str(), flv.size(), static_cast<unsigned long long>(muxer.tagCount()),
                static_cast<unsigned long long>(video_packets),
                static_cast<unsigned long long>(audio_packets),
                static_cast<long long>(muxer.timestampBaseMs()));
    return 0;
}
