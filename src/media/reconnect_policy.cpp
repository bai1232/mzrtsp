/*
 * ReconnectPolicy 实现（v0.3-a / M11-a2）
 * ============================================================================
 * 退避曲线：1s → 2s → 4s → 8s → 16s → 上限（默认 30s，之后一直上限）。
 * 每次失败调用一次 `nextDelayMs()`；读到第一个包调 `onSuccess()` 复位。
 * ============================================================================
 */

#include "media/reconnect_policy.h"

#include "core/logger.h"

namespace mzmedia {

bool ReconnectPolicy::setConfig(const Config &config) {
    if (config.initial_ms < kMinInitialMs || config.initial_ms > kMaxInitialMs) {
        WarnL << "ReconnectPolicy: initial_ms=" << config.initial_ms << " 不在 [" << kMinInitialMs
              << ", " << kMaxInitialMs << "] 内（0 不是「立即」）→ 拒绝";
        return false;
    }
    if (config.max_ms < config.initial_ms || config.max_ms > kHardMaxMs) {
        WarnL << "ReconnectPolicy: max_ms=" << config.max_ms << " 必须在 [initial_ms="
              << config.initial_ms << ", " << kHardMaxMs << "] 内 → 拒绝";
        return false;
    }
    if (config.max_attempts < 0 || config.max_attempts > kHardMaxAttempts) {
        WarnL << "ReconnectPolicy: max_attempts=" << config.max_attempts << " 必须在 [0, "
              << kHardMaxAttempts << "] 内（0 = 不设上限）→ 拒绝";
        return false;
    }
    _config = config;
    _attempts = 0;
    _last_delay_ms = 0;
    return true;
}

int64_t ReconnectPolicy::nextDelayMs() {
    // 已经用完了允许的次数：明确放弃（调用方据此结束这一路，而不是无限重试）
    if (_config.max_attempts > 0 && _attempts >= static_cast<uint64_t>(_config.max_attempts)) {
        return -1;
    }
    int64_t delay = 0;
    if (_attempts == 0) {
        delay = _config.initial_ms; // 第一次
    } else {
        // 之后每次翻倍；用"上一次的值"翻倍，避免浮点/位移溢出
        delay = (_last_delay_ms > 0) ? _last_delay_ms * 2 : _config.initial_ms;
        if (delay > _config.max_ms) {
            delay = _config.max_ms; // 封顶
        }
    }
    ++_attempts;
    _last_delay_ms = delay;
    return delay;
}

void ReconnectPolicy::onSuccess() {
    _attempts = 0;
    _last_delay_ms = 0;
}

int64_t ReconnectPolicy::nextDelayIfFailedMs() const {
    if (_config.max_attempts > 0 && _attempts >= static_cast<uint64_t>(_config.max_attempts)) {
        return -1;
    }
    if (_attempts == 0 || _last_delay_ms <= 0) {
        return _config.initial_ms;
    }
    const int64_t doubled = _last_delay_ms * 2;
    return (doubled > _config.max_ms) ? _config.max_ms : doubled;
}

} // namespace mzmedia
