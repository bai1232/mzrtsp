#include "core/semaphore.h"

#include <chrono>

namespace mzmedia {

Semaphore::Semaphore(size_t initial_count) : _count(initial_count) {}

void Semaphore::post(size_t n) {
    if (n == 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lck(_mtx);
        _count += n;
    }
    // 必须 notify_all：post(n) 可能同时满足多个等待者
    _cv.notify_all();
}

void Semaphore::wait() {
    std::unique_lock<std::mutex> lck(_mtx);
    _cv.wait(lck, [this]() { return _count > 0; });
    --_count;
}

bool Semaphore::tryWait(uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lck(_mtx);
    if (_count > 0) {
        --_count;
        return true;
    }
    if (timeout_ms == 0) {
        return false;   // 非阻塞尝试
    }
    const bool has_resource = _cv.wait_for(lck, std::chrono::milliseconds(timeout_ms),
                                           [this]() { return _count > 0; });
    if (!has_resource) {
        return false;
    }
    --_count;
    return true;
}

size_t Semaphore::count() const {
    std::lock_guard<std::mutex> lck(_mtx);
    return _count;
}

} // namespace mzmedia
