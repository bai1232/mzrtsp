/*
 * 固定大小线程池
 * ============================================================================
 * 语义约定：
 *   1. 构造即启动 thread_count 个工作线程；thread_count == 0 表示
 *      取 hardware_concurrency()（至少 1）。
 *   2. 【不阻塞业务线程】async() 默认不等待：队列满时**立即返回 false**，
 *      并累加 rejectedCount()。带 timeout_ms 的版本会最多等一会儿。
 *      **刻意不做"隐式同步执行"**（即队列满就在调用线程跑）：那会让 async 有时
 *      异步有时同步，调用方无法预期，排查问题时极其隐蔽。
 *   3. 【优雅停机】shutdown() = 停止接收新任务 + 把已入队任务全部执行完 + join 线程。
 *      幂等，可重复调用；析构函数会自动调用。
 *   4. 【异常隔离】单个任务抛异常只会被记录，工作线程继续运行 ——
 *      线程一旦死掉，线程池会静默降级甚至完全停摆。
 * ============================================================================
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "core/task_queue.h"

namespace mzmedia {

class ThreadPool {
public:
    using Ptr = std::shared_ptr<ThreadPool>;
    using Task = TaskQueue::Task;

    /**
     * @param thread_count  工作线程数，0 表示 hardware_concurrency()（至少 1）
     * @param max_task_size 任务队列上限，0 表示无界
     */
    explicit ThreadPool(size_t thread_count = 0, size_t max_task_size = 4096);

    ~ThreadPool();

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    /**
     * 投递任务（队列满时立即返回 false，不阻塞）
     */
    bool async(Task task);

    /**
     * 投递任务，队列满时最多等待 timeout_ms
     * @return 成功入队返回 true；被拒绝返回 false（调用方必须检查）
     */
    bool async(Task task, uint32_t timeout_ms);

    /**
     * 优雅停机：停止接收新任务、执行完队列中剩余任务、回收全部工作线程。
     * 幂等；析构时会自动调用。
     */
    void shutdown();

    /// 是否还在接收任务（shutdown 之后为 false）
    bool running() const;

    /// 工作线程数（构造时确定，停机后仍然返回该值）
    size_t threadCount() const;

    /// 队列中待执行的任务数
    size_t pendingCount() const;

    /// 已完成（含抛异常）的任务数
    uint64_t completedCount() const;

    /// 被拒绝的任务数
    uint64_t rejectedCount() const;

    /// 默认线程数：hardware_concurrency()，为 0 时兜底为 1
    static size_t defaultThreadCount();

private:
    void workerLoop(size_t index);

    std::unique_ptr<TaskQueue> _queue;
    std::vector<std::thread> _threads;
    std::atomic<bool> _running{false};
    std::atomic<uint64_t> _completed{0};
};

} // namespace mzmedia
