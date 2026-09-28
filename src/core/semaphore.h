/*
 * 计数信号量（Semaphore）
 * ============================================================================
 * 用途：线程间"有资源可用"的计数同步。与 std::condition_variable 相比，
 *       信号量自带计数，不需要调用方自己维护"资源够不够"的谓词。
 *
 * 与条件变量的取舍（重要）：
 *   条件变量必须配合一个受互斥量保护的**谓词**使用，否则会丢唤醒；
 *   信号量把计数封装在内部，用法更简单，代价是无法表达复杂谓词。
 *   需要"等待某个复杂状态成立"（如队列非空且未中止）时用条件变量；
 *   只需要"等待 N 个资源可用"时用信号量。
 * ============================================================================
 */

#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace mzmedia {

class Semaphore {
public:
    /**
     * @param initial_count 初始计数（0 表示一开始没有资源可用）
     */
    explicit Semaphore(size_t initial_count = 0);

    Semaphore(const Semaphore &) = delete;
    Semaphore &operator=(const Semaphore &) = delete;

    /**
     * 释放 n 个资源
     *
     * 注意：内部用 notify_all() 而不是 notify_one()。
     * 这是计数信号量，post(5) 可能要让 5 个等待者全部醒来；
     * 用 notify_one() 会导致只唤醒 1 个，其余线程要等下一次 post 才能醒 ——
     * 典型的"丢唤醒"问题。
     * @param n 释放数量，0 表示什么都不做
     */
    void post(size_t n = 1);

    /**
     * 无限期等待并获得 1 个资源
     * @note 用条件变量 + 谓词实现，不存在虚假唤醒问题
     */
    void wait();

    /**
     * 在 timeout_ms 内尝试获得 1 个资源
     * @param timeout_ms 0 表示只尝试一次、不阻塞
     * @return 获得成功返回 true；超时返回 false
     */
    bool tryWait(uint32_t timeout_ms);

    /**
     * 当前可用资源数（近似值，仅用于观测与断言）
     */
    size_t count() const;

private:
    mutable std::mutex _mtx;
    std::condition_variable _cv;
    size_t _count;
};

} // namespace mzmedia
