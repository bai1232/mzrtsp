/*
 * Ticker：测耗时的小工具
 * ============================================================================
 * 设计要点：
 *   内部使用 **steady_clock（单调时钟）**，不是 system_clock。
 *   原因：system_clock 会被 NTP 校时/手动改时间拨动，用它测耗时会得到负数或
 *   离谱的值；单调时钟只会前进，专门用于"测量时间间隔"。
 *
 * 用法：
 * @code
 *   Ticker ticker;                 // 构造即开始计时
 *   doSomething();
 *   InfoP("耗时 %llu ms", (unsigned long long)ticker.elapsedTime());
 * @endcode
 * ============================================================================
 */

#pragma once

#include <chrono>
#include <cstdint>

namespace mzmedia {

class Ticker {
public:
    Ticker() : _created(clock::now()) {}

    /// 重新开始计时
    void reset() { _created = clock::now(); }

    /// 距上次 reset（或构造）的毫秒数
    uint64_t elapsedTime() const {
        return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - _created)
                        .count());
    }

    /// 距上次 reset（或构造）的微秒数（适合测很短的代码段）
    uint64_t elapsedTimeInMicro() const {
        return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - _created)
                        .count());
    }

    /// 距上次 reset（或构造）的秒数（浮点，便于打印）
    double elapsedTimeInSecond() const {
        return static_cast<double>(elapsedTimeInMicro()) / 1000000.0;
    }

private:
    using clock = std::chrono::steady_clock;
    clock::time_point _created;
};

// 兼容 ZLToolKit 的命名
using TimeTicker = Ticker;

} // namespace mzmedia
