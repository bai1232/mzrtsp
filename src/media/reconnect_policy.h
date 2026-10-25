/*
 * ReconnectPolicy：断流重连的**退避曲线**（v0.3-a / M11-a2）
 * ============================================================================
 * 它只算一件事：**下一次重连该等多久**。不碰网络、不碰线程、不碰 FFmpeg —— 于是它可以用
 * 纯逻辑单测（`rtsp_reconnect_policy_backoff_sequence`），而 `SourceSession` 里只剩
 * "什么时候调它"。
 *
 * 曲线（`DESIGN_M11.md` §4.4）：
 *   1s → 2s → 4s → 8s → 16s → 上限（默认 30s，之后一直是上限）
 *   · **不加随机抖动**：这个规模抖动收益不明显，而"可测"更重要（用例要能断言序列）；
 *   · **读到第一个包才算成功**（`onSuccess()`）→ 退避复位为 1s；
 *   · `max_attempts == 0` = 不设上限（默认：对端一直不回来就一直退避重试，但每次都有计数与日志）。
 *
 * 有界性（AI_COLLAB §4.5：0 不是"不限"）：
 *   `initial_ms ∈ [100, 60000]`、`max_ms ∈ [initial, 300000]`、`max_attempts ∈ [0, 100000]`；
 *   非法配置**拒绝**并保持原值（不静默改成"看起来合理"的数）。
 * ============================================================================
 */

#pragma once

#include <cstdint>

namespace mzmedia {

class ReconnectPolicy {
public:
    struct Config {
        int64_t initial_ms = 1000; ///< 第一次重连前等多久
        int64_t max_ms = 30000;    ///< 退避上限（封顶后一直等这么久）
        int max_attempts = 0;      ///< 0 = 不设上限（只在进程退出/源被停时结束）
    };

    ReconnectPolicy() = default;
    explicit ReconnectPolicy(const Config &config) {
        (void) setConfig(config);
    }

    /// @return false = 参数非法（保持原值不变）
    bool setConfig(const Config &config);
    Config config() const {
        return _config;
    }

    /**
     * 记一次**失败**，返回"这次该等多久"
     * @return -1 = 已经达到 `max_attempts`（调用方应放弃并明确失败）
     * @note 第一次调用返回 `initial_ms`，之后每次翻倍，到 `max_ms` 封顶
     */
    int64_t nextDelayMs();

    /// 记一次**成功**（读到第一个包）→ 退避复位（下次又从 `initial_ms` 开始）
    void onSuccess();

    /// 已经尝试过多少次（stats 用）
    uint64_t attempts() const {
        return _attempts;
    }
    /// 下一次失败会等多久（还没失败过时 = `initial_ms`；达到上限后 = -1）
    int64_t nextDelayIfFailedMs() const;

private:
    static constexpr int64_t kMinInitialMs = 100;
    static constexpr int64_t kMaxInitialMs = 60000;
    static constexpr int64_t kHardMaxMs = 300000;
    static constexpr int kHardMaxAttempts = 100000;

    Config _config;
    uint64_t _attempts = 0;
    int64_t _last_delay_ms = 0; ///< 上一次等的时间（0 = 还没失败过）
};

} // namespace mzmedia
