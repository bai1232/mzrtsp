/*
 * ThreadGroup：一组线程的管家
 * ============================================================================
 * 与 ThreadPool 的区别：
 *   - ThreadPool：固定线程数 + 任务队列，任务由池调度；
 *   - ThreadGroup：**每创建一个线程就跑一个指定的函数**，通常用来管理
 *     "长期存在的后台线程"（例如 M2 的事件循环线程、M5 的媒体源线程）。
 *
 * 设计要点：
 *   1. 用 std::list 存放线程 —— list 的节点地址稳定，createThread 返回的
 *      `std::thread&` 在后续插入后依然有效；
 *   2. 析构时自动 joinAll()：如果留着 joinable 的 std::thread 不管，
 *      它的析构函数会直接 std::terminate；
 *   3. joinAll() 幂等（已 join 过的线程 joinable() 为 false，会跳过）。
 *
 * 使用约束：
 *   - joinAll() 会持锁等待，**不要在工作线程里调用 joinAll()**（会等待自己 → 死锁）；
 *   - joinAll() 期间不要再从其他线程 createThread()（会被锁挡住，属于预期行为）。
 * ============================================================================
 */

#pragma once

#include <cstddef>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "core/util.h"

namespace mzmedia {

class ThreadGroup {
public:
    ThreadGroup() = default;
    ~ThreadGroup() { joinAll(); }

    ThreadGroup(const ThreadGroup &) = delete;
    ThreadGroup &operator=(const ThreadGroup &) = delete;

    /**
     * 创建并启动一个线程
     * @param func 线程体
     * @param name 线程名（便于 top -H / gdb 定位；最长 15 字符，超出会被截断）
     * @return 该线程的引用（可用于单独 join），注意线程会被本对象持有
     */
    template<typename F>
    std::thread &createThread(F &&func, const std::string &name = "") {
        std::lock_guard<std::mutex> lck(_mtx);
        _threads.emplace_back([fn = std::forward<F>(func), name]() {
            if (!name.empty()) {
                setThreadName(name.c_str());
            }
            fn();
        });
        return _threads.back();
    }

    /// 等待全部线程结束；幂等
    void joinAll() {
        std::lock_guard<std::mutex> lck(_mtx);
        for (auto &thread : _threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    /// 已管理的线程数（join 之后仍返回该值，便于统计）
    size_t size() const {
        std::lock_guard<std::mutex> lck(_mtx);
        return _threads.size();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lck(_mtx);
        return _threads.empty();
    }

private:
    mutable std::mutex _mtx;
    std::list<std::thread> _threads;
};

} // namespace mzmedia
