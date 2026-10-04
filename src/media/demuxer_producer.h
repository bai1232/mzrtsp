/*
 * DemuxerProducer：把 M4 的 `Demuxer` 包装成 `SourcePump` 的读回调（M5-c）
 * ============================================================================
 * 形状来源：docs/DESIGN_M5.md §3.6；上游 `docs/DESIGN_M4.md`（Demuxer）、`DESIGN_M5.md` §3.5（SourcePump）
 *
 * 它的职责只有一件：**把 AVPacket 变成 MediaPacket**，并如实转达三种结局
 * （有包 / 读完 / 出错）。不做分发、不做缓冲、不做线程 —— 那些是 `SourcePump` 与
 * `MediaSource` 的事（分工明确才能各自被确定性测试）。
 *
 * 两处必须说清楚的取舍（DESIGN_M5.md §7）：
 *   1) **每包一次拷贝**：`Demuxer::packet()` 指向它内部的 AVPacket，下一帧就被复用，
 *      所以必须把字节搬进共享载荷。这是全链路**唯一**一次拷贝；扇出到 N 个客户端仍然
 *      零拷贝（所有订阅者共享同一个 `shared_ptr`）。
 *   2) **非音视频流跳过**：字幕/数据流不喂给媒体层（媒体层只认 Video/Audio），
 *      但仍然**计数**（`skippedUnknownStreams()`）—— 静默跳过等于悄悄丢数据。
 *
 * 时间戳换算失败（`packetTimestampsMs` 返回 false）时**仍然发包**（用 Demuxer 保持的
 * 上一次值 + 单调钳制），只累加计数：丢一个包比时间戳不够精确更糟。
 * ============================================================================
 */

#pragma once

#include "ffmpeg/demuxer.h"
#include "media/media_packet.h"
#include "media/source_pump.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace mzmedia {

class DemuxerProducer {
public:
    struct Config {
        Demuxer::Limits demux; // 流数 / 单包 / open 与 read 超时
    };

    explicit DemuxerProducer(const Config &config = Config{});
    ~DemuxerProducer() = default;
    DemuxerProducer(const DemuxerProducer &) = delete;
    DemuxerProducer &operator=(const DemuxerProducer &) = delete;

    /// @return false = 打不开（原因见 lastError()，已记日志）
    bool open(const std::string &path);
    bool opened() const {
        return _demux.opened();
    }
    const std::string &path() const {
        return _path;
    }
    /// 只读访问（M6 的 FLV 封装要用它拿流信息写 AVC/AAC sequence header）
    const Demuxer &demuxer() const {
        return _demux;
    }

    /// 绑定外部中止标志（通常是 `SourcePump::stopFlag()`）；@return 上一个标志
    const std::atomic<bool> *setAbortFlag(const std::atomic<bool> *flag) {
        return _demux.setAbortFlag(flag);
    }

    /// **直接作为 `SourcePump::ReadFn` 使用**
    SourcePump::ReadResult read(MediaPacket::Ptr *packet, std::string *error);

    // 观测（**可在其他线程读**：源线程在跑，观测方可能是 HTTP 线程）
    uint64_t totalPackets() const {
        return _packets.load();
    }
    uint64_t totalBytes() const {
        return _bytes.load();
    }
    uint64_t skippedUnknownStreams() const {
        return _skipped_unknown.load();
    }
    uint64_t timestampFailures() const {
        return _ts_failures.load();
    }
    std::string lastError() const;

private:
    const StreamInfo *findStream(int index) const;
    /// 记失败原因（内部加锁：读方可能在别的线程）
    void setError(const std::string &message);

    Config _config;
    Demuxer _demux;
    std::string _path;
    mutable std::mutex _mutex; // 保护 _last_error
    std::string _last_error;
    std::atomic<uint64_t> _packets{0};
    std::atomic<uint64_t> _bytes{0};
    std::atomic<uint64_t> _skipped_unknown{0};
    std::atomic<uint64_t> _ts_failures{0};
};

} // namespace mzmedia
