/*
 * task_queue 单元测试
 *
 * 注意：不允许在工作线程里调用 MZ_ASSERT_*（框架的计数器不是线程安全的），
 * 线程里只写 std::atomic / 普通变量，断言一律回到测试主线程。
 */

#include "test_main.h"

#include "core/task_queue.h"
#include "core/util.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace mzmedia;

MZ_TEST(queue_max_size_and_size) {
    TaskQueue unbounded;
    MZ_ASSERT_EQ(unbounded.maxSize(), 0u);   // 0 = 无界
    MZ_ASSERT_TRUE(unbounded.empty());
    MZ_ASSERT_FALSE(unbounded.aborted());
    MZ_ASSERT_EQ(unbounded.rejectedCount(), 0u);

    TaskQueue bounded(4);
    MZ_ASSERT_EQ(bounded.maxSize(), 4u);
}

MZ_TEST(queue_fifo_order) {
    TaskQueue queue(8);
    std::vector<int> order;
    for (int i = 0; i < 5; ++i) {
        MZ_ASSERT_TRUE(queue.push([&order, i]() { order.push_back(i); }));
    }
    MZ_ASSERT_EQ(queue.size(), 5u);

    TaskQueue::Task task;
    int executed = 0;
    while (queue.pop(task, 100)) {
        task();
        ++executed;
    }
    MZ_ASSERT_EQ(executed, 5);
    MZ_ASSERT_EQ(order.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        MZ_ASSERT_EQ(order[static_cast<size_t>(i)], i);
    }
    MZ_ASSERT_TRUE(queue.empty());
}

MZ_TEST(queue_rejects_null_task) {
    TaskQueue queue(4);
    TaskQueue::Task empty_task;   // 空 std::function
    MZ_ASSERT_FALSE(queue.push(empty_task));
    MZ_ASSERT_EQ(queue.rejectedCount(), 1u);
    MZ_ASSERT_TRUE(queue.empty());
}

MZ_TEST(queue_reject_when_full_without_waiting) {
    TaskQueue queue(2);
    MZ_ASSERT_TRUE(queue.push([]() {}));
    MZ_ASSERT_TRUE(queue.push([]() {}));

    // 满且不等待：必须立即失败，绝不能阻塞生产者
    const uint64_t begin = getCurrentMillisecond();
    MZ_ASSERT_FALSE(queue.push([]() {}));
    MZ_ASSERT_LT(getCurrentMillisecond() - begin, 100u);

    MZ_ASSERT_EQ(queue.rejectedCount(), 1u);
    MZ_ASSERT_EQ(queue.size(), 2u);
}

MZ_TEST(qtimed_push_waits_for_space) {
    TaskQueue queue(1);
    MZ_ASSERT_TRUE(queue.push([]() {}));   // 填满

    std::atomic<bool> consumed{false};
    std::thread consumer([&queue, &consumed]() {
        sleepMs(50);
        TaskQueue::Task task;
        consumed.store(queue.pop(task, 1000));
    });

    const uint64_t begin = getCurrentMillisecond();
    // 生产者最多等 1000ms：消费者腾出空位后应当成功
    MZ_ASSERT_TRUE(queue.push([]() {}, 1000));
    const uint64_t elapsed = getCurrentMillisecond() - begin;
    MZ_ASSERT_GE(elapsed, 40u);   // 确实是等了一会儿才成功

    consumer.join();
    MZ_ASSERT_TRUE(consumed.load());
}

MZ_TEST(queue_pop_timeout) {
    TaskQueue queue(4);
    TaskQueue::Task task;

    const uint64_t begin = getCurrentMillisecond();
    MZ_ASSERT_FALSE(queue.pop(task, 30));
    const uint64_t elapsed = getCurrentMillisecond() - begin;
    MZ_ASSERT_GE(elapsed, 25u);
    MZ_ASSERT_LT(elapsed, 500u);

    // timeout_ms == 0：只尝试一次，不阻塞
    const uint64_t begin2 = getCurrentMillisecond();
    MZ_ASSERT_FALSE(queue.pop(task, 0));
    MZ_ASSERT_LT(getCurrentMillisecond() - begin2, 50u);
}

MZ_TEST(queue_abort_wakes_blocked_popper) {
    TaskQueue queue(4);
    std::atomic<bool> popped{true};
    std::thread popper([&queue, &popped]() {
        TaskQueue::Task task;
        popped.store(queue.pop(task));   // 阻塞等待，直到 abort
    });

    sleepMs(30);
    queue.abort();
    popper.join();   // 必须被唤醒；若实现漏了 notify_all，这里会永久挂住

    MZ_ASSERT_FALSE(popped.load());
    MZ_ASSERT_TRUE(queue.aborted());
}

MZ_TEST(qtimed_abort_wakes_blocked_pusher) {
    // 生产者卡在"等空位"时也必须被 abort 唤醒，否则优雅停机可能挂住生产者
    TaskQueue queue(1);
    MZ_ASSERT_TRUE(queue.push([]() {}));

    std::atomic<bool> pushed{true};
    std::thread producer([&queue, &pushed]() {
        pushed.store(queue.push([]() {}, 5000));   // 队列满，会一直等
    });

    sleepMs(30);
    const uint64_t begin = getCurrentMillisecond();
    queue.abort();
    producer.join();

    MZ_ASSERT_FALSE(pushed.load());
    MZ_ASSERT_LT(getCurrentMillisecond() - begin, 1000u);   // 立刻被唤醒，而不是等满 5s
}

MZ_TEST(queue_abort_drains_pending_tasks) {
    // 优雅终止的关键：abort 之后已入队的任务仍要能取完，否则线程池会丢任务
    TaskQueue queue(8);
    std::atomic<int> count{0};
    for (int i = 0; i < 3; ++i) {
        MZ_ASSERT_TRUE(queue.push([&count]() { count.fetch_add(1); }));
    }

    queue.abort();
    MZ_ASSERT_FALSE(queue.push([]() {}));   // abort 后拒绝新任务

    TaskQueue::Task task;
    int drained = 0;
    while (queue.pop(task, 100)) {
        task();
        ++drained;
    }

    MZ_ASSERT_EQ(drained, 3);
    MZ_ASSERT_EQ(count.load(), 3);
    MZ_ASSERT_TRUE(queue.empty());
}
