/*
 * DemuxerProducer 实现（M5-c）
 * ============================================================================
 * `read()` 的循环只在一个地方兜圈：**跳过非音视频流**。
 * 其余情况一律"一步给出结论"（有包 / 读完 / 出错），因为 `SourcePump` 的退出语义
 * 依赖这三个结局被如实转达 —— 把"跳过"伪装成"读完"会让源提前结束。
 *
 * 线程说明：`read()` 由源线程调用；计数是 atomic、`lastError()` 走锁，
 * 因此观测方（HTTP 线程之类的）读它们是安全的。
 * ============================================================================
 */

#include "media/demuxer_producer.h"

#include "core/logger.h"

#include <utility>
#include <vector>

namespace mzmedia {

DemuxerProducer::DemuxerProducer(const Config &config)
    : _config(config)
    , _demux(config.demux) {}

void DemuxerProducer::setError(const std::string &message) {
    std::lock_guard<std::mutex> lock(_mutex);
    _last_error = message;
}

std::string DemuxerProducer::lastError() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _last_error;
}

bool DemuxerProducer::open(const std::string &path) {
    if (path.empty()) {
        setError("path 为空");
        ErrorL << "DemuxerProducer::open 失败：" << lastError();
        return false;
    }
    if (_demux.opened()) {
        setError("已经打开过（需要换文件请新建 producer）");
        ErrorL << "DemuxerProducer::open 失败：" << lastError();
        return false;
    }
    if (!_demux.open(path)) {
        setError(_demux.lastError()); // 不吞掉下层原因
        return false;
    }
    _path = path;
    return true;
}

const StreamInfo *DemuxerProducer::findStream(int index) const {
    for (const auto &info : _demux.streams()) {
        if (info.index == index) {
            return &info;
        }
    }
    return nullptr;
}

SourcePump::ReadResult DemuxerProducer::read(MediaPacket::Ptr *packet, std::string *error) {
    if (packet == nullptr || error == nullptr) {
        setError("read 的出参为空（调用方 bug）");
        ErrorL << "DemuxerProducer::read 失败：" << lastError();
        return SourcePump::ReadResult::Error;
    }
    if (!_demux.opened()) {
        setError("尚未 open 就读包");
        ErrorL << "DemuxerProducer::read 失败：" << lastError();
        return SourcePump::ReadResult::Error;
    }

    while (true) {
        const Demuxer::ReadResult result = _demux.readPacket();
        if (result == Demuxer::ReadResult::Eof) {
            return SourcePump::ReadResult::EndOfStream;
        }
        if (result == Demuxer::ReadResult::Error) {
            setError(_demux.lastError()); // 如实转达（含超时/超限/损坏）
            return SourcePump::ReadResult::Error;
        }

        const AVPacket *pkt = _demux.packet();
        if (pkt == nullptr || pkt->data == nullptr || pkt->size <= 0) {
            setError("读到空包（data 为空或 size <= 0）");
            ErrorL << "DemuxerProducer::read 失败：" << lastError();
            return SourcePump::ReadResult::Error;
        }

        const int stream_index = _demux.packetStreamIndex();
        const StreamInfo *info = findStream(stream_index);
        MediaKind kind = MediaKind::Video;
        if (info != nullptr && info->is_video) {
            kind = MediaKind::Video;
        } else if (info != nullptr && info->is_audio) {
            kind = MediaKind::Audio;
        } else {
            // 字幕/数据流：媒体层不认，跳过但**计数**（静默跳过 = 悄悄丢数据）
            ++_skipped_unknown;
            continue;
        }

        int64_t pts_ms = 0;
        int64_t dts_ms = 0;
        if (!_demux.packetTimestampsMs(&pts_ms, &dts_ms)) {
            // 换算失败仍然发（Demuxer 保持上一次的值并做了单调钳制）；丢包比时间戳不精确更糟
            ++_ts_failures;
        }

        // 全链路**唯一**一次拷贝：Demuxer 的 AVPacket 下一帧就被复用
        auto payload = std::make_shared<std::vector<uint8_t>>(
            pkt->data, pkt->data + static_cast<size_t>(pkt->size));
        const bool key_frame = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        MediaPacket::Ptr media_packet =
            MediaPacket::create(kind, stream_index, std::move(payload), key_frame, dts_ms, pts_ms);
        if (!media_packet) {
            setError("MediaPacket::create 失败（参数非法）");
            ErrorL << "DemuxerProducer::read 失败：" << lastError();
            return SourcePump::ReadResult::Error;
        }

        ++_packets;
        _bytes += media_packet->size();
        *packet = std::move(media_packet);
        return SourcePump::ReadResult::Packet;
    }
}

} // namespace mzmedia
