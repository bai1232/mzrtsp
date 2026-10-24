/*
 * transcode_demo：把文件转码成 FLV（M10-a 的验收工具，NFR-5 的 speed 就靠它量）
 * ============================================================================
 * 用法：transcode_demo <输入> [输出.flv] [输出高度]
 *   例：transcode_demo samples/sample1080.mp4 /tmp/out720.flv 720
 *       transcode_demo samples/sample.mp4     /tmp/out.flv     0   # 0 = 不缩放（只重编码）
 *
 * 它走的是**真实链路**（与 M10-b 要接进服务端的那条一样）：
 *   DemuxerProducer（解封装，得到 AVCC 视频包 + raw AAC 音频包）
 *     → Transcoder（解码 → libswscale 缩放 → x264 编码 → 现场构造 avcC）
 *     → FlvMuxer（视频用转码后的流信息；**音频原样透传** —— SPEC §9 不做音频转码）
 *     → 落盘 .flv
 *
 * 为什么要落成 FLV 而不是裸 H.264：FLV 能同时承载"转码后的视频 + 透传的音频"，
 * 而且项目里已有一套**经过验收的** FLV 校验方法（ffprobe + ffmpeg -f null -）。
 * 换句话说：转码对不对，交给独立裁判，而不是我自己的断言。
 * ============================================================================
 */

#include "mzmedia.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace mzmedia;

int main(int argc, char **argv) {
    if (argc < 2) {
        std::printf("用法：%s <输入> [输出.flv] [输出高度=0 表示不缩放]\n", argv[0]);
        return 1;
    }
    const std::string input = argv[1];
    const std::string output = (argc >= 3) ? argv[2] : "/tmp/mzmedia_transcode.flv";
    const int out_height = (argc >= 4) ? std::atoi(argv[3]) : 0;

    DemuxerProducer producer;
    if (!producer.open(input)) {
        std::printf("打开失败：%s\n", producer.lastError().c_str());
        return 1;
    }
    const StreamInfo *src_video = producer.muxerVideo();
    const StreamInfo *audio = producer.muxerAudio();
    if (src_video == nullptr) {
        std::printf("没有视频流：%s\n", input.c_str());
        return 1;
    }

    Transcoder::Config config;
    config.gop_frames = 50;
    if (out_height > 0) {
        // 等比缩放：宽度按源宽高比算，并保证是偶数（yuv420p 要求）
        int width = static_cast<int>(static_cast<int64_t>(src_video->width) * out_height /
                                     (src_video->height > 0 ? src_video->height : 1));
        width = (width & 1) ? (width - 1) : width;
        config.out_width = width;
        config.out_height = out_height;
    }
    Transcoder::Ptr transcoder = Transcoder::create(*src_video, config);
    if (!transcoder) {
        std::printf("转码器建不起来（见上面的 Error 日志）\n");
        return 1;
    }
    const StreamInfo &dst_video = transcoder->videoInfo();
    std::printf("输入：%s\n  视频：%dx%d（%s） → 输出：%dx%d（avcC %zu 字节）\n", input.c_str(),
                src_video->width, src_video->height, src_video->codec_name.c_str(), dst_video.width,
                dst_video.height, dst_video.extradata ? dst_video.extradata->size() : 0u);
    std::printf("  音频：%s（**原样透传**，SPEC §9 不做音频转码）\n",
                audio != nullptr ? audio->codec_name.c_str() : "(无)");

    // FlvMuxer 只认"形状"：视频是 H264+avcC（转码器现场构造的），音频是 AAC+ASC（源流信息）
    FlvMuxer muxer(FlvMuxer::Streams{&dst_video, audio});
    const FlvMuxer::Result prepared = muxer.prepare();
    if (prepared != FlvMuxer::Result::Ok) {
        std::printf("FLV 封装无法开始：%s —— %s\n", FlvMuxer::resultName(prepared),
                    muxer.lastError().c_str());
        return 1;
    }
    std::string flv;
    (void) muxer.writeHeader(&flv);
    if (muxer.writeSequenceHeaders(&flv) != FlvMuxer::Result::Ok) {
        std::printf("写 sequence header 失败：%s\n", muxer.lastError().c_str());
        return 1;
    }

    uint64_t video_in = 0;
    uint64_t audio_in = 0;
    uint64_t video_out = 0;
    const uint64_t begin_ms = getCurrentMillisecond();
    std::vector<Transcoder::OutPacket> outs;

    auto write_transcoded = [&](const std::vector<Transcoder::OutPacket> &packets) -> bool {
        for (const Transcoder::OutPacket &item : packets) {
            MediaPacket::Ptr media = MediaPacket::create(MediaKind::Video, dst_video.index, item.data,
                                                         item.key, item.dts_ms, item.pts_ms);
            if (!media) {
                std::printf("MediaPacket::create 失败\n");
                return false;
            }
            if (muxer.writePacket(*media, &flv) != FlvMuxer::Result::Ok) {
                std::printf("FLV 封装失败：%s\n", muxer.lastError().c_str());
                return false;
            }
            ++video_out;
        }
        return true;
    };

    for (;;) {
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
        if (packet->kind() == MediaKind::Audio) {
            // 音频透传：一个字节都不改（时间戳/形状都是输出层要的样子）
            if (muxer.writePacket(*packet, &flv) != FlvMuxer::Result::Ok) {
                std::printf("音频透传失败：%s\n", muxer.lastError().c_str());
                return 1;
            }
            ++audio_in;
            continue;
        }
        ++video_in;
        outs.clear();
        const Transcoder::Result tret = transcoder->transcode(
            packet->data(), packet->size(), packet->dtsMs(), packet->ptsMs(), &outs);
        if (tret != Transcoder::Result::Ok) {
            std::printf("转码失败：%s（%s）\n", Transcoder::resultName(tret),
                        transcoder->lastError().c_str());
            return 1;
        }
        if (!write_transcoded(outs)) {
            return 1;
        }
    }
    // EOF：把解码器/编码器里攒着的帧全部冲出来（漏掉这一步会少最后几帧，而且不报错）
    outs.clear();
    if (transcoder->flush(&outs) != Transcoder::Result::Ok) {
        std::printf("flush 失败：%s\n", transcoder->lastError().c_str());
        return 1;
    }
    if (!write_transcoded(outs)) {
        return 1;
    }
    const uint64_t elapsed_ms = getCurrentMillisecond() - begin_ms;

    if (!saveFile(output, flv)) {
        std::printf("写文件失败：%s\n", output.c_str());
        return 1;
    }

    // NFR-5 的口径：speed = 已输出媒体时长 / 已耗墙钟（>1 表示比实时快）
    const double media_ms = static_cast<double>(transcoder->outMediaMs());
    const double wall_ms = static_cast<double>(elapsed_ms > 0 ? elapsed_ms : 1);
    const double speed = media_ms / wall_ms;
    const double fps = wall_ms > 0.0 ? static_cast<double>(transcoder->encodedPackets()) * 1000.0 / wall_ms : 0.0;
    std::printf("已写出：%s（%zu 字节）\n", output.c_str(), flv.size());
    std::printf("  输入：视频包 %llu / 音频包 %llu（透传）\n",
                static_cast<unsigned long long>(video_in), static_cast<unsigned long long>(audio_in));
    std::printf("  转码：解码帧 %llu（其中缩放 %llu）→ 编码包 %llu（关键帧 %llu）\n",
                static_cast<unsigned long long>(transcoder->decodedFrames()),
                static_cast<unsigned long long>(transcoder->scaledFrames()),
                static_cast<unsigned long long>(video_out),
                static_cast<unsigned long long>(transcoder->keyPackets()));
    std::printf("  墙钟 %llu ms / 媒体 %llu ms → speed=%.2fx（%.1f 帧/s）\n",
                static_cast<unsigned long long>(elapsed_ms),
                static_cast<unsigned long long>(transcoder->outMediaMs()), speed, fps);
    std::printf("  机器可读：OUT_W=%d OUT_H=%d SPEED=%.2f FPS=%.1f FRAMES=%llu\n", dst_video.width,
                dst_video.height, speed, fps, static_cast<unsigned long long>(video_out));
    return 0;
}
