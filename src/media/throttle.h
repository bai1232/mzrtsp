/*
 * Throttle：按源时间轴与**墙钟**对齐推送（M5-d，FR-3.5）
 * ============================================================================
 * 形状来源：docs/DESIGN_M5.md §3.8；上位需求 `docs/SPEC.md` **FR-3.5**
 *   「文件输入按源时间轴与**墙钟对齐**推送，不得以磁盘速度全速灌入」
 *
 * 为什么必须存在：磁盘比网络快几个数量级。不节流的话，一个 2 秒的样本会在几毫秒内
 * 读完并塞满每个订阅者的队列（然后按 FR-5.1/5.2 大面积丢帧）——对观看者来说不是"流畅播放"，
 * 而是"瞬间冲完 + 一堆丢帧"。节流之后，源的产出速率才等于真实播放速率。
 *
 * 时基：用 `MediaPacket::dtsMs()`（M4 已换算成毫秒并做过单调钳制）与**单调时钟**做差，
 *   不用系统时钟（会被 NTP 调整影响）。
 *
 * 三条要求：
 *   1) **可被打断**：`pace()` 必须能被中止标志叫停（否则 `SourcePump::stop()` 的 join 要等满一拍）；
 *   2) **不早发**：只在"已经落后于应当的时刻"时才等，绝不提前放行（差值 ≤ 0 直接过）;
 *   3) **不减数据**：节流只影响"什么时候发"，不丢任何包（丢包是 FR-5.1/5.2 的事，两者不要混）。
 *
 * `speed` 是给压测/单测用的倍速：`speed = 8` 表示按 8 倍速播放（2 秒样本 ≈ 250ms 读完）。
 * `enabled = false` 表示完全不节流（只给压测用，日常路径应当开着 —— FR-1.2 要的就是节流推送）。
 * ============================================================================
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace mzmedia {

class Throttle {
public:
    struct Config {
        bool enabled = true;  // FR-1.2/FR-3.5：默认**开**
        double speed = 1.0;   // 1.0 = 实时；>1 = 快放（压测/单测）
    };

    Throttle() = default;
    explicit Throttle(const Config &config) {
        (void) setConfig(config);
    }

    /// @return false = 参数非法（speed ≤ 0 / NaN / inf / 超过硬上限），保持原值不变
    bool setConfig(const Config &config);
    Config config() const;

    /// 开始一段流（`SourcePump::start` 时调用）：重置墙钟与首个 dts 基准
    /// @note 只由源线程调用
    void reset();

    /**
     * 按 dts 等到"该发"的时刻
     * @param dts_ms 当前包的解码时间戳（毫秒）
     * @param abort 中止标志（通常是 `SourcePump::stopFlag()`）
     * @return false = 被中止（调用方应当立刻退出，不要再读下一包）
     * @note 只由源线程调用；内部按 ≤50ms 的片睡，保证中止能及时生效
     */
    bool pace(int64_t dts_ms, const std::atomic<bool> &abort);

    // 观测
    int64_t waitedMs() const {
        return _waited_ms.load();
    }
    uint64_t paceCount() const {
        return _pace_count.load();
    }
    uint64_t abortedCount() const {
        return _aborted_count.load();
    }

private:
    static constexpr double kHardMaxSpeed = 1000.0; // 1000 倍速封顶（再快就不是"节流"了）
    static constexpr int64_t kMaxSliceMs = 50;      // 单次睡眠上限：让中止/退出保持灵敏

    mutable std::mutex _mutex; // 保护 _config
    Config _config;

    // 以下只由源线程读写（不加锁）
    int64_t _start_wall_ms = 0;
    int64_t _first_dts_ms = 0;
    bool _started = false;

    std::atomic<int64_t> _waited_ms{0};
    std::atomic<uint64_t> _pace_count{0};
    std::atomic<uint64_t> _aborted_count{0};
};

} // namespace mzmedia
