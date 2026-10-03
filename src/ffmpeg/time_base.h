/*
 * time_base：时间基换算 + 单调性守卫（M4-a）
 * ============================================================================
 * 形状来源：docs/DESIGN_M4.md §3.2
 *
 * **本文件刻意不依赖 FFmpeg**：换算是纯整数运算，单测不需要样本文件、也不需要
 * libav* 在场（这样它能进最快的那个测试分组）。
 *
 * 三条硬要求：
 *   1) 只用整数运算 + __int128 中间量：浮点累加会让长片的时间戳慢慢漂（FLV 的 dts
 *      必须单调，漂一点就会被播放器当成跳帧）；
 *   2) 失败**必须**能被发现：除零、结果溢出 int64 → 返回 false（不做"截断/钳制"式的静默降级）；
 *   3) 回退要可见：播放/封装侧需要"单调不减"，但现实里 B 帧、坏流会给回退值 ——
 *      `MonotonicGuard` 把回退钳住并**计数**，绝不静默改数据。
 * ============================================================================
 */

#pragma once

#include <cstdint>
#include <limits>

namespace mzmedia {

/// 时间基：1 个 tick = num/den 秒（默认 1000/1 = 毫秒）
struct TimeBase {
    int64_t num = 1;
    int64_t den = 1000;
};

/**
 * 把 ts 从 src 时间基换算到 dst 时间基
 * @return true = 成功；false = 时间基非法（num/den <= 0）或结果溢出 int64
 * @note 溢出判定用 __int128 精确比较，不做"就近截断"（静默错值比失败更糟）
 */
inline bool rescaleTimestamp(int64_t ts, const TimeBase &src, const TimeBase &dst, int64_t *out) {
    if (out == nullptr) {
        return false;
    }
    if (src.num <= 0 || src.den <= 0 || dst.num <= 0 || dst.den <= 0) {
        return false;
    }
    // ts * (src.num/src.den) / (dst.num/dst.den)  =  ts * src.num * dst.den / (src.den * dst.num)
    const __int128 num = static_cast<__int128>(ts) * src.num * dst.den;
    const __int128 den = static_cast<__int128>(src.den) * dst.num;
    const __int128 result = num / den;
    if (result > std::numeric_limits<int64_t>::max() || result < std::numeric_limits<int64_t>::min()) {
        return false;
    }
    *out = static_cast<int64_t>(result);
    return true;
}

/**
 * 单调性守卫：输入时间戳必须不减；回退时钳到上一个值并计数
 * @note 计数是给"这条流有问题"用的观测出口（AI_COLLAB §4.5：静默改数据 = 失败）
 */
class MonotonicGuard {
public:
    /// @return 钳制后的时间戳（保证 >= 上一次的返回值）
    int64_t apply(int64_t ts) {
        if (ts < _last) {
            ++_clamped;
            return _last;
        }
        _last = ts;
        return ts;
    }

    /// 发生过多少次回退钳制
    uint64_t monotonicClampCount() const {
        return _clamped;
    }

    /// 复位（换流/换源时用）
    void reset() {
        _last = std::numeric_limits<int64_t>::min();
        _clamped = 0;
    }

private:
    int64_t _last = std::numeric_limits<int64_t>::min();
    uint64_t _clamped = 0;
};

} // namespace mzmedia
