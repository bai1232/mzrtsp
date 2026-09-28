#include "core/task_queue.h"

#include <chrono>
#include <utility>

namespace mzmedia {

TaskQueue::TaskQueue(size_t max_size) : _max_size(max_size) {}

bool TaskQueue::push(Task task, uint32_t timeout_ms) {
    if (!task) {
        // 空任务入队后执行会抛 std::bad_function_call，直接拒绝更安全
        _rejected.fetch_add(1);
        return false;
    }

    std::unique_lock<std::mutex> lck(_mtx);
    if (_abort) {
        _rejected.fetch_add(1);
        return false;
    }

    const bool full = (_max_size > 0) && (_queue.size() >= _max_size);
    if (full) {
        if (timeout_ms == 0) {
            _rejected.fetch_add(1);
            return false;   // 不等待：立即失败
        }
        // 等待消费者腾出空位；wait_for 会先检查谓词，不会误判
        const bool has_space = _cv_not_full.wait_for(
                lck, std::chrono::milliseconds(timeout_ms), [this]() {
                    return _abort || _max_size == 0 || _queue.size() < _max_size;
                });
        if (!has_space || _abort || ((_max_size > 0) && (_queue.size() >= _max_size))) {
            _rejected.fetch_add(1);
            return false;
        }
    }

    _queue.emplace_back(std::move(task));
    lck.unlock();
    _cv_not_empty.notify_one();
    return true;
}

bool TaskQueue::pop(Task &task) {
    std::unique_lock<std::mutex> lck(_mtx);
    _cv_not_empty.wait(lck, [this]() { return _abort || !_queue.empty(); });
    if (_queue.empty()) {
        // 只有"已 abort 且队列为空"才会到这里：保证 abort 之前入队的任务都被处理完
        return false;
    }
    task = std::move(_queue.front());
    _queue.pop_front();
    lck.unlock();
    _cv_not_full.notify_one();
    return true;
}

bool TaskQueue::pop(Task &task, uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lck(_mtx);
    const bool ready = _cv_not_empty.wait_for(lck, std::chrono::milliseconds(timeout_ms),
                                              [this]() { return _abort || !_queue.empty(); });
    if (!ready || _queue.empty()) {
        return false;
    }
    task = std::move(_queue.front());
    _queue.pop_front();
    lck.unlock();
    _cv_not_full.notify_one();
    return true;
}

void TaskQueue::abort() {
    {
        std::lock_guard<std::mutex> lck(_mtx);
        _abort = true;
    }
    // 两边的等待者都要叫醒：生产者可能卡在"等空位"，消费者可能卡在"等任务"
    _cv_not_empty.notify_all();
    _cv_not_full.notify_all();
}

bool TaskQueue::aborted() const {
    std::lock_guard<std::mutex> lck(_mtx);
    return _abort;
}

bool TaskQueue::empty() const {
    std::lock_guard<std::mutex> lck(_mtx);
    return _queue.empty();
}

size_t TaskQueue::size() const {
    std::lock_guard<std::mutex> lck(_mtx);
    return _queue.size();
}

size_t TaskQueue::maxSize() const {
    return _max_size;
}

uint64_t TaskQueue::rejectedCount() const {
    return _rejected.load();
}

} // namespace mzmedia
