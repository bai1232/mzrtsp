#include "core/thread_pool.h"

#include <cstdio>
#include <exception>
#include <utility>

#include "core/util.h"

namespace mzmedia {

size_t ThreadPool::defaultThreadCount() {
    const unsigned hw = std::thread::hardware_concurrency();
    return hw == 0 ? 1 : static_cast<size_t>(hw);
}

ThreadPool::ThreadPool(size_t thread_count, size_t max_task_size)
        : _queue(new TaskQueue(max_task_size)) {
    const size_t count = (thread_count == 0) ? defaultThreadCount() : thread_count;
    _running.store(true);
    _threads.reserve(count);

    try {
        for (size_t i = 0; i < count; ++i) {
            // 线程名便于 top -H / gdb 定位
            _threads.emplace_back([this, i]() { workerLoop(i); });
        }
    } catch (...) {
        // 防御式处理：线程创建失败（资源不足）时，若把 joinable 的 std::thread
        // 留给析构函数，std::thread 的析构会直接 std::terminate 掉整个进程。
        // 因此先把已创建的线程收干净，再让异常继续向上抛。
        _running.store(false);
        _queue->abort();
        for (auto &thread : _threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        std::fprintf(stderr, "[mzmedia][ThreadPool] 创建工作线程失败，已回收已启动的线程\n");
        throw;
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

bool ThreadPool::async(Task task) {
    return async(std::move(task), 0);
}

bool ThreadPool::async(Task task, uint32_t timeout_ms) {
    if (!_running.load()) {
        return false;   // 已停机：明确拒绝，不静默入队
    }
    return _queue->push(std::move(task), timeout_ms);
}

void ThreadPool::shutdown() {
    _running.store(false);
    if (_threads.empty()) {
        return;
    }
    // abort 只负责"不再收新任务 + 唤醒所有等待者"；
    // TaskQueue::pop 会把已入队任务取完才返回 false，所以这里是优雅停机而不是丢弃。
    _queue->abort();
    for (auto &thread : _threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

bool ThreadPool::running() const {
    return _running.load();
}

size_t ThreadPool::threadCount() const {
    return _threads.size();
}

size_t ThreadPool::pendingCount() const {
    return _queue->size();
}

uint64_t ThreadPool::completedCount() const {
    return _completed.load();
}

uint64_t ThreadPool::rejectedCount() const {
    return _queue->rejectedCount();
}

void ThreadPool::workerLoop(size_t index) {
    setThreadName(strFormat("mzmedia-pool-%zu", index).c_str());

    Task task;
    while (_queue->pop(task)) {
        try {
            task();
        } catch (const std::exception &e) {
            // 任务异常绝不能弄死工作线程，否则线程池会静默降级
            std::fprintf(stderr, "[mzmedia][ThreadPool] 任务抛异常（已忽略，线程继续）: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "[mzmedia][ThreadPool] 任务抛出未知异常（已忽略，线程继续）\n");
        }
        _completed.fetch_add(1);
    }
}

} // namespace mzmedia
