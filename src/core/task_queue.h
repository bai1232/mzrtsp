/*
 * 有界阻塞任务队列（生产者-消费者）
 * ============================================================================
 * 语义约定（实现与测试都以此为准）：
 *
 *   1. 【有界】max_size > 0 时队列最多容纳 max_size 个任务；满时 push 最多等待
 *      timeout_ms 再决定"入队失败"，**绝不无限阻塞生产者**。
 *      max_size == 0 表示无界（不建议用于生产环境，仅测试或特殊场景）。
 *
 *   2. 【优雅终止】abort() 之后：
 *        - push() 立即返回 false（不再接受新任务）
 *        - pop() **不丢弃已入队的任务**：会继续取完，只有"已 abort 且队列为空"
 *          才返回 false。这保证 ThreadPool 的优雅停机不会丢任务。
 *
 *   3. 【不阻塞则失败】所有可能阻塞的接口都有带超时的版本，超时返回 false，
 *      调用方**必须**检查返回值 —— 不检查就等于静默丢任务。
 *
 *   4. 被拒绝的任务累加 rejectedCount()，便于观测"生产者是否跑得比消费者快"。
 *
 *   5. 刻意不提供 clear()/shutdownNow()：本项目线程池只做优雅停机，
 *      即时丢弃语义等真正需要时再加，避免出现"两种停机语义"让人误用。
 * ============================================================================
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>

namespace mzmedia {

class TaskQueue {
public:
    using Task = std::function<void()>;

    /**
     * @param max_size 队列容量上限，0 表示无界
     */
    explicit TaskQueue(size_t max_size = 0);

    TaskQueue(const TaskQueue &) = delete;
    TaskQueue &operator=(const TaskQueue &) = delete;

    /**
     * 入队
     * @param task       任务；空任务（空 std::function）会被直接拒绝
     * @param timeout_ms 队列满时最多等待多久；0 表示不等待、立即失败
     * @return 成功入队返回 true；被拒绝（满 / 已 abort / 空任务）返回 false
     */
    bool push(Task task, uint32_t timeout_ms = 0);

    /**
     * 阻塞取出，直到有任务可取，或"已 abort 且队列为空"
     * @return 取到任务返回 true；仅在 abort 且队列空时返回 false
     */
    bool pop(Task &task);

    /**
     * 带超时取出
     * @param timeout_ms 0 表示只尝试一次、不阻塞
     * @return 取到任务返回 true；超时或"已 abort 且队列为空"返回 false
     */
    bool pop(Task &task, uint32_t timeout_ms);

    /**
     * 终止队列：唤醒所有等待者、不再接受新任务。
     * 已入队的任务仍可被 pop() 取走。
     */
    void abort();

    bool aborted() const;

    bool empty() const;
    size_t size() const;
    size_t maxSize() const;

    /// 被拒绝的任务数（队列满 / 已 abort / 空任务）
    uint64_t rejectedCount() const;

private:
    mutable std::mutex _mtx;
    std::condition_variable _cv_not_empty;
    std::condition_variable _cv_not_full;
    std::list<Task> _queue;
    const size_t _max_size;
    bool _abort = false;
    std::atomic<uint64_t> _rejected{0};
};

} // namespace mzmedia
