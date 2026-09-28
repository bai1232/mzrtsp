/*
 * thread_pool 单元测试
 *
 * 注意：不允许在工作线程里调用 MZ_ASSERT_*（框架的计数器不是线程安全的），
 * 线程里只写 std::atomic，断言一律回到测试主线程。
 */

#include "test_main.h"

#include "core/semaphore.h"
#include "core/task_queue.h"
#include "core/thread_pool.h"
#include "core/util.h"

#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace mzmedia;

MZ_TEST(pool_default_thread_count) {
    const size_t expected = ThreadPool::defaultThreadCount();
    MZ_ASSERT_GE(expected, 1u);

    ThreadPool pool(0, 64);   // 0 = 用默认线程数
    MZ_ASSERT_EQ(pool.threadCount(), expected);
    MZ_ASSERT_TRUE(pool.running());
    MZ_ASSERT_EQ(pool.completedCount(), 0u);
    MZ_ASSERT_EQ(pool.rejectedCount(), 0u);

    pool.shutdown();
    MZ_ASSERT_FALSE(pool.running());
    MZ_ASSERT_EQ(pool.pendingCount(), 0u);
}

MZ_TEST(pool_runs_all_tasks) {
    constexpr int kCount = 2000;
    std::atomic<int> ran{0};

    ThreadPool pool(4, 8192);
    MZ_ASSERT_EQ(pool.threadCount(), 4u);

    for (int i = 0; i < kCount; ++i) {
        if (!pool.async([&ran]() { ran.fetch_add(1); })) {
            MZ_FAIL("任务被意外拒绝（队列上限 8192 > 2000）");
            break;
        }
    }

    pool.shutdown();   // 优雅停机：必须等队列中的任务全部执行完
    MZ_ASSERT_EQ(ran.load(), kCount);
    MZ_ASSERT_EQ(pool.completedCount(), static_cast<uint64_t>(kCount));
    MZ_ASSERT_EQ(pool.pendingCount(), 0u);
    MZ_ASSERT_EQ(pool.rejectedCount(), 0u);
}

MZ_TEST(pool_task_exception_is_contained) {
    std::atomic<int> ran{0};
    ThreadPool pool(2, 64);
    MZ_ASSERT_EQ(pool.threadCount(), 2u);

    MZ_ASSERT_TRUE(pool.async([]() { throw std::runtime_error("task boom"); }));
    for (int i = 0; i < 20; ++i) {
        MZ_ASSERT_TRUE(pool.async([&ran]() { ran.fetch_add(1); }));
    }

    pool.shutdown();

    // 抛异常的任务不得影响其它任务，也不得弄死工作线程
    MZ_ASSERT_EQ(ran.load(), 20);
    MZ_ASSERT_EQ(pool.completedCount(), 21u);
}

MZ_TEST(pool_graceful_shutdown_drains_queue) {
    constexpr int kCount = 400;
    std::atomic<int> ran{0};

    ThreadPool pool(2, 4096);
    for (int i = 0; i < kCount; ++i) {
        MZ_ASSERT_TRUE(pool.async([&ran]() {
            sleepMs(1);
            ran.fetch_add(1);
        }));
    }

    // 立刻停机：此时队列里还有大量任务，优雅停机必须把它们跑完（不能丢）
    pool.shutdown();

    MZ_ASSERT_EQ(ran.load(), kCount);
    MZ_ASSERT_EQ(pool.pendingCount(), 0u);
    MZ_ASSERT_FALSE(pool.async([]() {}));   // 停机后不再接受任务
}

MZ_TEST(ptimed_rejects_when_full) {
    // 1 个工作线程 + 队列上限 1：把线程占住、队列填满，第三次投递必然被拒绝
    Semaphore started(0);
    Semaphore release(0);
    std::atomic<int> ran{0};

    ThreadPool pool(1, 1);
    MZ_ASSERT_EQ(pool.threadCount(), 1u);

    MZ_ASSERT_TRUE(pool.async([&started, &release, &ran]() {
        started.post();
        release.wait();   // 把线程卡住
        ran.fetch_add(1);
    }));
    MZ_ASSERT_TRUE(started.tryWait(2000));   // 确保任务已被取走，队列已空、线程忙碌

    MZ_ASSERT_TRUE(pool.async([&ran]() { ran.fetch_add(1); }));    // 填满队列（上限 1）
    MZ_ASSERT_FALSE(pool.async([&ran]() { ran.fetch_add(1); }));   // 满 → 立即拒绝
    MZ_ASSERT_GT(pool.rejectedCount(), 0u);

    // 带超时的投递：等待期间没人会释放，所以同样应该失败（而不是无限阻塞）
    MZ_ASSERT_FALSE(pool.async([&ran]() { ran.fetch_add(1); }, 50));

    release.post();
    pool.shutdown();   // 队列里那 1 个任务仍会被执行

    MZ_ASSERT_EQ(ran.load(), 2);   // 只有未被拒绝的两个任务真正执行了
}

MZ_TEST(pool_shutdown_is_idempotent) {
    ThreadPool pool(2, 16);
    MZ_ASSERT_TRUE(pool.async([]() {}));

    pool.shutdown();
    pool.shutdown();   // 重复调用不得崩溃、不得死锁

    MZ_ASSERT_FALSE(pool.running());
    MZ_ASSERT_FALSE(pool.async([]() {}));
}
