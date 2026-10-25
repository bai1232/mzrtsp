/*
 * HlsWriter：一源一份的 HLS 切片器（M9-b / FR-3.2 / FR-4.1 / FR-4.2）
 * ============================================================================
 * 形状来源：docs/DESIGN_M9.md §3.2（形状先定死）；上位需求 `docs/SPEC.md`
 *   FR-3.2「mpegts 切片 **2s**、**滑动窗口 6 片**、`.m3u8` 含 `#EXT-X-MEDIA-SEQUENCE`
 *          与 `#EXT-X-TARGETDURATION`」
 *   FR-1.3「转码/封装任务 lazy 启动 + 空闲 60s 释放」（HLS 的"客户端"是**拉流请求**，见 §决策）
 *
 * 与 `FlvSender` 的根本区别：**FLV 是每连接一份输出，HLS 是一源一份、所有客户端共享**
 * （切片是文件，播放器自己按 m3u8 去取）。所以本类是"源的输出侧"，不是"连接的输出侧"：
 * 一个源只有一个 `HlsWriter`，它把包切成片放进内存 ring，HTTP 层只读 ring。
 *
 * 四条硬约束：
 *   1) **切片必须切在关键帧上**（只要源有视频）——从非关键帧开始的切片播放端解不了；
 *      因此 `target_duration` 是**目标**而不是硬边界：GOP 比它长时切片会更长（计数 + Warn，不静默）。
 *   2) **每个切片开头写 PAT + PMT**（`MpegTsMuxer::writePatPmt`）——切片要能独立解码/起播。
 *   3) **有界**：ring 里最多 `window` 片、单片 ≤ `max_slice_bytes`、总量 ≤ `max_total_bytes`；
 *      超出策略是**丢最旧的整片**并计数（不能丢半片：那会产出坏切片）。
 *   4) `#EXT-X-TARGETDURATION` 取**已产出切片的实测最大时长**的向上取整（协议要求 ≥ 实际最大值），
 *      窗口为空时退化为 `ceil(target_duration)`。
 *
 * 线程：`onPacket`/`finish` 在**订阅者的 poller 线程**上调用；`playlist()`/`findSlice()`
 * 在**任意 HTTP 线程**上调用 → 内部一把锁（只在两侧各持一小段时间，不跨 I/O）。
 * ============================================================================
 */

#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "media/media_packet.h"
#include "output/mpegts_muxer.h"

namespace mzmedia {

class HlsWriter : public std::enable_shared_from_this<HlsWriter> {
public:
    using Ptr = std::shared_ptr<HlsWriter>;

    struct Config {
        /// 播放列表里的 URI 前缀：切片地址 = `/hls/<name>-<seq>.ts`
        std::string name;
        /// 【M10-c】切片 URI 的查询串后缀（形如 `?transcode=1&h=120`；空 = 不带）
        /// @note 为什么要写在 URI 里：播放器只认播放列表里给出的地址，
        ///       转码规格必须在**每个切片**的 URL 上重复一遍，否则切片请求会退化成直通
        std::string uri_query;
        uint32_t target_duration_ms = 2000;   ///< 目标切片时长（FR-3.2 的 2s）
        size_t window = 6;                    ///< 滑动窗口片数（FR-3.2 的 6 片）
        /// 单片时长硬顶：GOP 比它长时**切不了**（切在非关键帧 = 坏片）→ 计数 + Warn，继续等关键帧
        uint32_t max_slice_ms = 10000;
        size_t max_slice_bytes = 8u << 20;    ///< 单片字节上限（码率 × 目标时长的量级）
        size_t max_total_bytes = 48u << 20;   ///< 总量上限（window × 单片上限）
    };

    /// 一片（已完成的切片；**open 中的那一片不在 ring 里**）
    struct Slice {
        uint64_t seq = 0;
        int64_t duration_ms = 0;
        std::string bytes;
    };

    enum class Result : uint8_t {
        Ok = 0,
        BadStreams = 1,      ///< 编码/初始化数据不满足 TS 封装
        InvalidPacket = 2,   ///< 空包 / 不属于这两路流
        TooLarge = 3,        ///< 单片超过硬上限（已计数；继续等关键帧）
    };

    /// @return nullptr = 流参数不满足 TS 封装（原因已记日志，见 `MpegTsMuxer::lastError()`）
    static Ptr create(const Config &config, MpegTsMuxer::Streams streams);
    static const char *resultName(Result result);

    /**
     * 喂一个包（**同步**：不投递线程、不阻塞）
     * @note 有视频时：**第一个切片从关键帧开始**（之前的包丢弃并计数）；
     *       后续在"关键帧 + 当前片已达目标时长"处切下一片。
     *       无视频时（纯音频）：按时间切。
     */
    Result onPacket(const MediaPacket &packet);

    /// 源 EOS：把 open 的那片收尾（时长 > 0 才入 ring）并置 `#EXT-X-ENDLIST`
    void finish();

    // ---- HTTP 侧（任意线程）----
    std::string playlist() const;
    /// @return nullptr = 不在当前窗口（播放器拿到的旧 m3u8 会请求老切片 —— 这是**正常**的 404）
    std::shared_ptr<const Slice> findSlice(uint64_t seq) const;

    // ---- 观测 ----
    bool finished() const {
        std::lock_guard<std::mutex> lck(_mutex);
        return _finished;
    }
    size_t sliceCount() const {
        std::lock_guard<std::mutex> lck(_mutex);
        return _ring.size();
    }
    uint64_t slicesProduced() const {
        std::lock_guard<std::mutex> lck(_mutex);
        return _slices_produced;
    }
    uint64_t slicesDropped() const {
        std::lock_guard<std::mutex> lck(_mutex);
        return _slices_dropped;
    }
    size_t bytesInRing() const {
        std::lock_guard<std::mutex> lck(_mutex);
        return _bytes_in_ring;
    }
    /// 超过 `max_slice_ms` 仍等不到关键帧的次数（GOP 过长 → 切片变长）
    uint64_t oversizeSlices() const {
        std::lock_guard<std::mutex> lck(_mutex);
        return _oversize_slices;
    }
    /// 首个切片之前被丢弃的包数（正常为 0；>0 说明源不是从关键帧开始）
    uint64_t skippedBeforeKey() const {
        std::lock_guard<std::mutex> lck(_mutex);
        return _skipped_before_key;
    }
    std::string lastError() const {
        std::lock_guard<std::mutex> lck(_mutex);
        return _last_error;
    }

private:
    HlsWriter(const Config &config, MpegTsMuxer::Streams streams);
    Result fail(Result result, const std::string &message);
    /// 把当前 open 的片收尾（`next_first_dts_ms` 用来算时长；<0 表示用末包时间估）
    void closeCurrentLocked(int64_t next_first_dts_ms);
    void enforceLimitsLocked();

    mutable std::mutex _mutex;
    Config _config;
    MpegTsMuxer _muxer;
    bool _has_video = false;

    // ---- 以下都在 _mutex 内访问 ----
    std::deque<Slice> _ring;
    std::string _current;          ///< open 中的那一片的字节
    bool _current_open = false;
    int64_t _current_first_dts_ms = 0;
    int64_t _current_last_dts_ms = 0;
    int64_t _frame_duration_ms = 40;
    uint64_t _next_seq = 0;
    uint64_t _slices_produced = 0;
    uint64_t _slices_dropped = 0;
    size_t _bytes_in_ring = 0;
    uint64_t _oversize_slices = 0;
    uint64_t _skipped_before_key = 0;
    bool _finished = false;
    std::string _last_error;
};

} // namespace mzmedia
