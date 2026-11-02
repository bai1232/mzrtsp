/*
 * SubscriberQuota：**跨源**的观众总名额（工业上的"全局连接/观众数上限"）
 * ============================================================================
 * 为什么需要它（单靠每源的 `max_subscribers` 不够）：
 *   每源上限 × 源数 才是真上限。100 路源 × 每源 512 人 = 51200 个订阅者，
 *   每个订阅者的队列上限是"码率 × 延迟预算"（默认 2 MB）→ 光是队列就能吃掉 100 GB。
 *   所以工业产品都是**两个旋钮**：
 *     · 每源上限（`MediaSource::Limits::max_subscribers`）—— 保护**单条流**；
 *     · 全局上限（本类）—— 保护**进程/机器**。
 *
 * 语义约定（与项目其它"上限"一致）：
 *   · `limit == 0` 不是"无限"，而是**不启用全局上限**（只有单测/嵌入式这么用；应用层必填）；
 *   · `tryAcquire()` 与 `release()` **必须配对**：本项目把"释放"绑在 `Subscriber` 的析构上
 *     （而不是绑在 `unsubscribe()` 上），这样"订阅者被丢弃 / 源析构 / 惰性清理"任何一条路径
 *     都只会释放一次 —— 名额泄漏是这类计数器最典型的 bug，不能靠调用方记得还。
 *
 * 线程安全：全部原子操作，任意线程可调（源线程 pushPacket 里也会读 inUse 做统计）。
 * ============================================================================
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace mzmedia {

class SubscriberQuota {
public:
    using Ptr = std::shared_ptr<SubscriberQuota>;

    /// @param max_total 全局名额上限；0 = 不启用（**不是**无限大，语义见文件头）
    explicit SubscriberQuota(size_t max_total = 0) : _limit(max_total) {}

    /// 申请一个名额；@return false = 已满（调用方必须把它当成"拒绝接入"，不能静默排队）
    bool tryAcquire() {
        const size_t limit = _limit;
        if (limit == 0) {
            ++_in_use;
            return true;
        }
        size_t current = _in_use.load(std::memory_order_relaxed);
        while (true) {
            if (current >= limit) {
                ++_rejected;
                return false;
            }
            if (_in_use.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel)) {
                return true;
            }
        }
    }

    /// 归还一个名额（**必须与一次成功的 `tryAcquire()` 严格配对**）
    void release() {
        size_t current = _in_use.load(std::memory_order_relaxed);
        while (true) {
            if (current == 0) {
                // 归还多于申请 = 记账 bug。不崩（生产里宁可多算也别退服），但必须有痕迹：
                // 这里只做计数，日志交给调用方（本类不依赖 logger，便于单测）
                ++_over_releases;
                return;
            }
            if (_in_use.compare_exchange_weak(current, current - 1, std::memory_order_acq_rel)) {
                return;
            }
        }
    }

    size_t limit() const {
        return _limit;
    }
    size_t inUse() const {
        return _in_use.load();
    }
    uint64_t rejected() const {
        return _rejected.load();
    }
    /// 归还次数多于申请次数的次数（正常应为 0；非 0 说明配额记账有 bug）
    uint64_t overReleases() const {
        return _over_releases.load();
    }

private:
    const size_t _limit;
    std::atomic<size_t> _in_use{0};
    std::atomic<uint64_t> _rejected{0};
    std::atomic<uint64_t> _over_releases{0};
};

} // namespace mzmedia
