/*
 * Throttle 实现（M5-d）
 * ============================================================================
 * 计时公式：应当已过去 `(dts - first_dts) / speed` 毫秒（相对本次 reset 的墙钟）。
 * 已经超过就直接放行（**不早发、也不补偿**）——补偿会让"落后"的源疯狂追赶，
 * 反而把下游又冲爆一次；宁可慢一点，也不要突发。
 * ============================================================================
 */

#include "media/throttle.h"

#include "core/logger.h"
#include "core/util.h"

#include <cmath>

namespace mzmedia {

bool Throttle::setConfig(const Config &config) {
    if (!std::isfinite(config.speed) || config.speed <= 0.0) {
        WarnL << "Throttle::setConfig 被拒：speed 必须是正数且有限（传入 " << config.speed << "）";
        return false;
    }
    if (config.speed > kHardMaxSpeed) {
        WarnL << "Throttle::setConfig 被拒：speed 超过硬上限 " << kHardMaxSpeed;
        return false;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _config = config;
    return true;
}

Throttle::Config Throttle::config() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _config;
}

void Throttle::reset() {
    _start_wall_ms = 0;
    _first_dts_ms = 0;
    _started = false;
}

bool Throttle::pace(int64_t dts_ms, const std::atomic<bool> &abort) {
    Config config;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        config = _config;
    }

    ++_pace_count;
    if (abort.load()) {
        ++_aborted_count;
        return false;
    }
    if (!config.enabled) {
        return true; // 显式关闭（压测路径）：立即放行
    }

    if (!_started) {
        // 首个包立即可发：基准就是它，不等待
        _start_wall_ms = static_cast<int64_t>(getCurrentMillisecond());
        _first_dts_ms = dts_ms;
        _started = true;
        return true;
    }

    const double due = static_cast<double>(dts_ms - _first_dts_ms) / config.speed;
    if (due <= 0.0) {
        return true; // 时间戳回退/重复：不早发，也不倒着等
    }

    const int64_t elapsed = static_cast<int64_t>(getCurrentMillisecond()) - _start_wall_ms;
    int64_t remaining = static_cast<int64_t>(due) - elapsed;
    int64_t waited = 0;
    while (remaining > 0) {
        if (abort.load()) {
            _waited_ms += waited;
            ++_aborted_count;
            return false; // 立刻让调用方退出，不睡满
        }
        const int64_t slice = remaining > kMaxSliceMs ? kMaxSliceMs : remaining;
        sleepMs(static_cast<uint32_t>(slice));
        waited += slice;
        remaining -= slice;
    }
    _waited_ms += waited;
    return !abort.load();
}

} // namespace mzmedia
