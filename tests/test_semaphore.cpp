/*
 * semaphore 单元测试
 *
 * 注意：测试框架的断言计数器不是线程安全的，**不允许在工作线程里调用 MZ_ASSERT_***，
 * 线程里只记录状态（用 std::atomic / 普通 bool），断言统一放回测试主线程。
 */

#include "test_main.h"

#include "core/semaphore.h"
#include "core/util.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace mzmedia;

MZ_TEST(semaphore_count_semantics) {
    Semaphore sem(0);
    MZ_ASSERT_EQ(sem.count(), 0u);
    MZ_ASSERT_FALSE(sem.tryWait(0));   // 没有资源：非阻塞尝试失败

    sem.post();
    MZ_ASSERT_EQ(sem.count(), 1u);
    MZ_ASSERT_TRUE(sem.tryWait(0));
    MZ_ASSERT_EQ(sem.count(), 0u);
    MZ_ASSERT_FALSE(sem.tryWait(0));

    // post(n) 一次性释放多个
    sem.post(3);
    MZ_ASSERT_EQ(sem.count(), 3u);
    MZ_ASSERT_TRUE(sem.tryWait(0));
    MZ_ASSERT_TRUE(sem.tryWait(0));
    MZ_ASSERT_TRUE(sem.tryWait(0));
    MZ_ASSERT_FALSE(sem.tryWait(0));

    // post(0) 不改变计数
    sem.post(0);
    MZ_ASSERT_EQ(sem.count(), 0u);
}

MZ_TEST(semaphore_initial_count) {
    Semaphore sem(2);
    MZ_ASSERT_EQ(sem.count(), 2u);
    MZ_ASSERT_TRUE(sem.tryWait(0));
    MZ_ASSERT_TRUE(sem.tryWait(0));
    MZ_ASSERT_FALSE(sem.tryWait(0));
    MZ_ASSERT_EQ(sem.count(), 0u);
}

MZ_TEST(semaphore_timeout) {
    Semaphore sem(0);
    const uint64_t begin = getCurrentMillisecond();
    MZ_ASSERT_FALSE(sem.tryWait(30));
    const uint64_t elapsed = getCurrentMillisecond() - begin;
    MZ_ASSERT_GE(elapsed, 25u);   // 确实等到了超时（允许少量调度误差）
    MZ_ASSERT_LT(elapsed, 500u);
    MZ_ASSERT_EQ(sem.count(), 0u);
}

MZ_TEST(semaphore_blocking_wakeup) {
    Semaphore sem(0);
    std::atomic<bool> woken{false};
    std::thread waiter([&sem, &woken]() {
        sem.wait();   // 无限等待，靠 post 唤醒
        woken.store(true);
    });

    // 先确认 waiter 还没被唤醒（否则测的是"先 post 后 wait"，没有意义）
    MZ_ASSERT_FALSE(sem.tryWait(20));
    MZ_ASSERT_FALSE(woken.load());

    sem.post();
    waiter.join();
    MZ_ASSERT_TRUE(woken.load());
    MZ_ASSERT_EQ(sem.count(), 0u);
}

MZ_TEST(semaphore_multi_waiter_notify_all) {
    // 3 个等待者 + post(3)：必须全部被唤醒。
    // 如果内部实现用 notify_one()，就会有线程永远醒不来 —— 本用例守住这一点。
    constexpr int kWaiters = 3;
    Semaphore sem(0);
    std::atomic<int> woken{0};

    std::vector<std::thread> waiters;
    waiters.reserve(kWaiters);
    for (int i = 0; i < kWaiters; ++i) {
        waiters.emplace_back([&sem, &woken]() {
            sem.wait();
            woken.fetch_add(1);
        });
    }

    sleepMs(50);                          // 让等待者都进入等待
    MZ_ASSERT_EQ(woken.load(), 0);
    sem.post(static_cast<size_t>(kWaiters));
    for (auto &thread : waiters) {
        thread.join();
    }
    MZ_ASSERT_EQ(woken.load(), kWaiters);
    MZ_ASSERT_EQ(sem.count(), 0u);
}
