/*
 * Demuxer：解封装（M4-a；M5-c 增加"外部中止标志"）
 * ============================================================================
 * 形状来源：docs/DESIGN_M4.md §3.3、docs/DESIGN_M5.md §3.6
 *
 * 只做三件事：打开 → 报流信息 → 读包。**不解码**（解码在 M5+）。
 *
 * 四条硬约束（§4.3/§4.5）：
 *   1) `max_streams`：流数上限（异常文件不能把内存拉爆）；
 *   2) `max_packet_size`：单包上限（超限 → **拒绝并报错**，不缓存不截断）；
 *   3) `open/read` 超时：用 FFmpeg 的 `interrupt_callback` **真正中断**，
 *      不是"读完了再比时间"（那样卡死时根本回不来）；
 *   4) 时间戳：换算 + 单调守卫都要**可见**（计数），绝不静默改数据。
 *
 * 【M5-c 新增】`setAbortFlag()`：把外部的"要停了"标志接进同一个 interrupt_callback。
 *   为什么需要：`SourcePump::stop()` 会 join 源线程；如果源正卡在 `av_read_frame` 里
 *   （大文件慢盘、v0.3 的网络 URL），只靠"读超时"要等满 5s 才回来。
 *   接上标志后 stop() 立刻生效 —— 这也是 DESIGN_M4 §7 选 interrupt_callback 而不是
 *   "另起线程强制关"的原因。
 *
 * 本头文件会把 libav* 的头带进来 —— 这是 M4 的边界（use `mzmedia.h` 的使用方无感）。
 * ============================================================================
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "ffmpeg/av_ptr.h"
#include "ffmpeg/time_base.h"

namespace mzmedia {

/// 一路流的结构化信息（判"能不能直接 remux"就靠它，对应 docs/CODEC_MATRIX.md）
struct StreamInfo {
    int index = -1;
    AVCodecID codec_id = AV_CODEC_ID_NONE;
    std::string codec_name;
    bool is_video = false;
    bool is_audio = false;
    int width = 0;
    int height = 0;
    int sample_rate = 0;
    int channels = 0;
    TimeBase time_base;    // 该流的时间基
    TimeBase frame_rate;   // 帧率（num/den，未提供时为 0/0）
};

class Demuxer {
public:
    struct Limits {
        int max_streams = 16;
        int64_t max_packet_size = 8 * 1024 * 1024;
        int open_timeout_ms = 5000;
        int read_timeout_ms = 5000;
    };
    enum class ReadResult { Packet, Eof, Error };

    Demuxer();
    explicit Demuxer(const Limits &limits);
    ~Demuxer();
    Demuxer(const Demuxer &) = delete;
    Demuxer &operator=(const Demuxer &) = delete;

    /// @return false = 打不开 / 拿不到流信息 / 超时 / 流数超限（原因见 lastError()，已记日志）
    bool open(const std::string &path);
    bool opened() const;
    const std::string &path() const;

    /**
     * 绑定外部中止标志（M5-c）
     * @param flag 由调用方保证**生命周期长于本对象**；传 nullptr = 取消绑定
     * @return **上一个**标志（沿用全项目"注册返回上一个"的约定）
     * @note 标志是 atomic<bool>，可从任意线程置位；interrupt_callback 里读它
     */
    const std::atomic<bool> *setAbortFlag(const std::atomic<bool> *flag);

    const std::vector<StreamInfo> &streams() const;
    const StreamInfo *firstVideo() const;   // 没有则 nullptr
    const StreamInfo *firstAudio() const;

    /// @return Packet = packet() 有效；Eof = 正常结束；Error = 出错（见 lastError()）
    /// @note **Eof 与 Error 必须分开**：混成一个 bool 会静默提前结束播放
    ReadResult readPacket();
    const AVPacket *packet() const;
    int packetStreamIndex() const;
    /// 当前包的时间戳（毫秒，已做换算 + 单调钳制）
    /// @return false = 换算出错（已计数；输出保持上一次的值）
    bool packetTimestampsMs(int64_t *pts_ms, int64_t *dts_ms);

    // 观测（全部是"能证明某件事发生过"的计数）
    uint64_t totalPackets() const;
    uint64_t totalBytes() const;
    uint64_t rejectedPackets() const;   // 超过 max_packet_size
    uint64_t rescaleFailures() const;   // 时间戳换算失败/缺失
    uint64_t monotonicClamped() const;  // 时间戳回退被钳制的次数
    const std::string &lastError() const;

private:
    static int interruptCb(void *opaque);

    Limits _limits;
    AvFormatCtx _fmt;
    std::string _path;
    std::vector<StreamInfo> _streams;
    AvPacket _pkt;
    std::vector<MonotonicGuard> _pts_guard;   // 按流索引
    std::vector<MonotonicGuard> _dts_guard;
    const std::atomic<bool> *_abort = nullptr; // 外部中止标志（M5-c）
    int _pkt_stream_index = -1;
    int64_t _pkt_pts_ms = 0;
    int64_t _pkt_dts_ms = 0;
    bool _eof = false;
    int64_t _deadline_ms = 0;   // interrupt_callback 用它判超时
    std::string _last_error;
    uint64_t _total_packets = 0;
    uint64_t _total_bytes = 0;
    uint64_t _rejected_packets = 0;
    uint64_t _rescale_failures = 0;
};

} // namespace mzmedia
