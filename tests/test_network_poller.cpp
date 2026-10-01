/*
 * EventPoller / PipeWrap 单元测试
 * ============================================================================
 * 等待策略（重要，和 TSAN 有关）：
 *   本文件"等事件发生"一律用 **Semaphore::wait()（无超时）**，不用 tryWait(ms)。
 *   原因：glibc 2.35 把 std::condition_variable 的超时接口实现为
 *   pthread_cond_clockwait，而 GCC 11 的 libtsan 没有该拦截器 → 带超时的等待会被
 *   TSAN 误报（详见 docs/TESTING.md §8）。
 *   用阻塞等待可以让本组留在 TSAN **严格组**（要求 0 报告）。
 *   代价：实现坏掉时用例会挂住，由 ctest 的 TIMEOUT(120s) 判失败。
 *   —— 本文件刻意不出现任何 wait_for / tryWait(ms)。
 *
 * 断言纪律：不允许在工作线程（含 poller 线程）里调用 MZ_ASSERT_*，
 * 线程里只写 std::atomic / 受信号量同步的普通变量，断言回到测试主线程。
 * ============================================================================
 */

#include "test_main.h"

#include "core/util.h"
#include "network/event_poller.h"
#include "network/pipe_wrap.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <stdexcept>
#include <thread>

using namespace mzmedia;

// ---------------------------------------------------------------------------
// 创建 / 上下文 / 观测
// ---------------------------------------------------------------------------

MZ_TEST(poller_create_and_context) {
    auto poller = EventPoller::create("test-poller");
    MZ_ASSERT_NOT_NULL(poller.get());
    MZ_ASSERT_FALSE(poller->isCurrentThread());
    MZ_ASSERT_FALSE(poller->exiting());
    MZ_ASSERT_EQ(poller->fdCount(), 0u);
    MZ_ASSERT_EQ(poller->timerCount(), 0u);
    MZ_ASSERT_EQ(poller->pendingTaskCount(), 0u);
    MZ_ASSERT_STR_EQ(poller->threadName(), "test-poller");
    // 非 poller 线程上取不到"当前 poller"
    MZ_ASSERT_NULL(EventPoller::getCurrentPoller().get());

    poller->shutdown();
    MZ_ASSERT_TRUE(poller->exiting());
    poller->shutdown();   // 幂等
}

MZ_TEST(poller_add_event_invalid) {
    auto poller = EventPoller::create("test-invalid");

    MZ_ASSERT_EQ(poller->addEvent(-1, EventPoller::EventRead, [](int) {}), -1);
    MZ_ASSERT_EQ(poller->addEvent(3, EventPoller::EventRead, nullptr), -1);   // 空回调

    const uint64_t before = poller->epollErrorCount();
    MZ_ASSERT_EQ(poller->addEvent(-1, EventPoller::EventRead, [](int) {}), -1);
    // §4：失败必须被计数，不能只有日志
    MZ_ASSERT_GT(poller->epollErrorCount(), before);
    MZ_ASSERT_EQ(poller->fdCount(), 0u);

    poller->shutdown();
}

// ---------------------------------------------------------------------------
// 注册 / 注销 / 修改
// ---------------------------------------------------------------------------

MZ_TEST(poller_add_del_modify) {
    auto poller = EventPoller::create("test-ctl");
    int fds[2] = {-1, -1};
    MZ_ASSERT_EQ(::pipe2(fds, O_NONBLOCK | O_CLOEXEC), 0);

    MZ_ASSERT_EQ(poller->addEvent(fds[0], EventPoller::EventRead, [](int) {}), 0);
    MZ_ASSERT_EQ(poller->fdCount(), 1u);
    // 重复注册同一 fd 必须失败（否则会覆盖已有回调，静默丢监听）
    MZ_ASSERT_EQ(poller->addEvent(fds[0], EventPoller::EventRead, [](int) {}), -1);

    // 修改事件：complete_cb 在轮询线程同步调用
    int mod_ok = -1;
    const int mod_ret = poller->modifyEvent(
            fds[0], EventPoller::EventRead | EventPoller::EventLT,
            [&mod_ok](bool ok) { mod_ok = ok ? 1 : 0; });
    MZ_ASSERT_EQ(mod_ret, 0);
    MZ_ASSERT_EQ(mod_ok, 1);

    // 注销：complete_cb 是**延迟**调用的（本轮事件处理完之后），所以必须等它
    Semaphore deleted(0);
    int del_ok = -1;
    MZ_ASSERT_EQ(poller->delEvent(fds[0], [&deleted, &del_ok](bool ok) {
        del_ok = ok ? 1 : 0;
        deleted.post();
    }), 0);
    MZ_ASSERT_EQ(poller->fdCount(), 0u);   // fd 计数是同步更新的
    deleted.wait();
    MZ_ASSERT_EQ(del_ok, 1);

    // 重复删除：失败且不会让 fdCount 下溢
    MZ_ASSERT_EQ(poller->delEvent(fds[0]), -1);
    MZ_ASSERT_EQ(poller->fdCount(), 0u);

    ::close(fds[0]);
    ::close(fds[1]);
    poller->shutdown();
}

MZ_TEST(poller_pipe_event_callback) {
    auto poller = EventPoller::create("test-event");
    int fds[2] = {-1, -1};
    MZ_ASSERT_EQ(::pipe2(fds, O_NONBLOCK | O_CLOEXEC), 0);

    Semaphore fired(0);
    std::atomic<int> got_event{0};
    MZ_ASSERT_EQ(poller->addEvent(fds[0], EventPoller::EventRead,
                                 [&fired, &got_event](int event) {
                                     got_event.store(event);
                                     fired.post();
                                 }),
                 0);

    const char payload = 'x';
    MZ_ASSERT_EQ(::write(fds[1], &payload, 1), 1);

    fired.wait();   // 阻塞等待（无超时 → 留在 TSAN 严格组）
    MZ_ASSERT_TRUE((got_event.load() & EventPoller::EventRead) != 0);

    // 等 complete_cb 之后再 close：确保后续批次不会再派发该 fd
    Semaphore deleted(0);
    MZ_ASSERT_EQ(poller->delEvent(fds[0], [&deleted](bool) { deleted.post(); }), 0);
    deleted.wait();

    ::close(fds[0]);
    ::close(fds[1]);
    poller->shutdown();
}

MZ_TEST(poller_del_event_in_callback) {
    auto poller = EventPoller::create("test-del-in-cb");
    int fds[2] = {-1, -1};
    MZ_ASSERT_EQ(::pipe2(fds, O_NONBLOCK | O_CLOEXEC), 0);

    Semaphore cb_fired(0);
    Semaphore deleted(0);
    // 回调里删除自己：这正是"延迟删除"要保护的场景
    // （立即 erase 会让正在执行的回调对象被析构 → use-after-free）
    const int add_ret = poller->addEvent(fds[0], EventPoller::EventRead,
                                        [&poller, &cb_fired, &deleted, &fds](int) {
                                            cb_fired.post();
                                            poller->delEvent(fds[0], [&deleted](bool) { deleted.post(); });
                                        });
    MZ_ASSERT_EQ(add_ret, 0);

    const char payload = 'y';
    MZ_ASSERT_EQ(::write(fds[1], &payload, 1), 1);

    cb_fired.wait();
    deleted.wait();
    MZ_ASSERT_EQ(poller->fdCount(), 0u);

    ::close(fds[0]);
    ::close(fds[1]);
    poller->shutdown();
}

// ---------------------------------------------------------------------------
// 跨线程投递
// ---------------------------------------------------------------------------

MZ_TEST(poller_async_no_loss) {
    constexpr int kCount = 100000;
    auto poller = EventPoller::create("test-async");
    std::atomic<int> executed{0};
    Semaphore done(0);

    for (int i = 0; i < kCount; ++i) {
        if (!poller->async([&executed]() { executed.fetch_add(1); })) {
            MZ_FAIL("async 被拒绝（poller 不应在运行中拒绝任务）");
            break;
        }
    }
    MZ_ASSERT_TRUE(poller->async([&done]() { done.post(); }));
    done.wait();   // 队列是 FIFO 单线程执行：最后一个任务跑完 => 前面都跑完了

    MZ_ASSERT_EQ(executed.load(), kCount);
    MZ_ASSERT_EQ(poller->pendingTaskCount(), 0u);

    poller->shutdown();
}

MZ_TEST(poller_async_inline_on_poller_thread) {
    auto poller = EventPoller::create("test-inline");
    std::atomic<bool> was_current{false};
    std::atomic<bool> inner_ran{false};
    std::atomic<bool> inner_ran_when_async_returned{false};
    Semaphore done(0);

    const bool accepted = poller->async([&]() {
        was_current.store(poller->isCurrentThread());
        // 已在轮询线程：may_sync=true 时应**直接执行**，而不是入队
        poller->async([&inner_ran]() { inner_ran.store(true); });
        inner_ran_when_async_returned.store(inner_ran.load());
        done.post();
    });
    MZ_ASSERT_TRUE(accepted);
    done.wait();

    MZ_ASSERT_TRUE(was_current.load());
    MZ_ASSERT_TRUE(inner_ran.load());
    MZ_ASSERT_TRUE(inner_ran_when_async_returned.load());   // 证明是内联执行

    poller->shutdown();
}

MZ_TEST(poller_sync_value) {
    auto poller = EventPoller::create("test-sync");
    std::atomic<bool> ran_on_poller{false};

    const int value = poller->sync([&]() {
        ran_on_poller.store(poller->isCurrentThread());
        return 42;
    });
    MZ_ASSERT_EQ(value, 42);
    MZ_ASSERT_TRUE(ran_on_poller.load());

    // 在轮询线程内部再 sync：必须内联执行，否则自等死锁
    const int nested = poller->sync([&]() { return poller->sync([]() { return 7; }); });
    MZ_ASSERT_EQ(nested, 7);

    poller->shutdown();
}

// ---------------------------------------------------------------------------
// 定时器
// ---------------------------------------------------------------------------

MZ_TEST(poller_timer_fires) {
    auto poller = EventPoller::create("test-timer");
    Semaphore fired(0);
    const uint64_t begin = getCurrentMillisecond();

    auto task = poller->doDelayTask(20, [&fired]() -> uint64_t {
        fired.post();
        return 0;   // 不重复
    });
    MZ_ASSERT_NOT_NULL(task.get());

    fired.wait();
    const uint64_t elapsed = getCurrentMillisecond() - begin;
    MZ_ASSERT_GE(elapsed, 15u);   // 允许少量调度误差
    MZ_ASSERT_LT(elapsed, 500u);
    MZ_ASSERT_FALSE(task->isCanceled());

    sleepMs(20);
    MZ_ASSERT_EQ(poller->timerCount(), 0u);   // 一次性任务执行后应出堆
    poller->shutdown();
}

MZ_TEST(poller_timer_repeat_and_cancel) {
    auto poller = EventPoller::create("test-timer-repeat");
    Semaphore tick(0);
    std::atomic<int> count{0};

    auto task = poller->doDelayTask(10, [&tick, &count]() -> uint64_t {
        count.fetch_add(1);
        tick.post();
        return 10;   // 每 10ms 重复
    });
    MZ_ASSERT_NOT_NULL(task.get());

    for (int i = 0; i < 3; ++i) {
        tick.wait();
    }
    const int snapshot = count.load();
    MZ_ASSERT_GE(snapshot, 3);

    task->cancel();
    sleepMs(80);
    // 取消后不再增长（允许取消瞬间已有一次在途执行）
    MZ_ASSERT_LE(count.load(), snapshot + 1);
    MZ_ASSERT_TRUE(task->isCanceled());

    poller->shutdown();
}

// ---------------------------------------------------------------------------
// 退出后的行为（§4：必须明确拒绝，不能假装成功）
// ---------------------------------------------------------------------------

MZ_TEST(poller_shutdown_rejects_new_work) {
    auto poller = EventPoller::create("test-shutdown");
    poller->shutdown();
    MZ_ASSERT_TRUE(poller->exiting());

    MZ_ASSERT_FALSE(poller->async([]() {}));
    MZ_ASSERT_NULL(poller->doDelayTask(10, []() -> uint64_t { return 0; }).get());
    MZ_ASSERT_GT(poller->rejectedTimerCount(), 0u);
    MZ_ASSERT_EQ(poller->addEvent(0, EventPoller::EventRead, [](int) {}), -1);
    // sync 投递不了时必须抛异常，不能静默返回默认值
    MZ_ASSERT_THROW(poller->sync([]() {}), std::runtime_error);

    poller->shutdown();   // 幂等
}

// ---------------------------------------------------------------------------
// EventPollerPool
// ---------------------------------------------------------------------------

MZ_TEST(poller_pool_get_poller) {
    EventPollerPool &pool = EventPollerPool::Instance();
    const unsigned hw = std::thread::hardware_concurrency();
    const size_t expected = hw == 0 ? 1 : static_cast<size_t>(hw);
    MZ_ASSERT_EQ(pool.size(), expected);

    auto first = pool.getFirstPoller();
    MZ_ASSERT_NOT_NULL(first.get());
    MZ_ASSERT_NOT_NULL(pool.getPoller().get());

    // 在轮询线程内部：prefer_current=true 必须返回"当前线程的" poller（连接亲和的基础）
    // 注意：getCurrentPoller() 只能在 poller 线程上读取，所以比较必须在 sync 的任务里做
    std::atomic<bool> current_is_first{false};
    auto same = first->sync([&]() {
        current_is_first.store(EventPoller::getCurrentPoller().get() == first.get());
        return EventPollerPool::Instance().getPoller(true);
    });
    MZ_ASSERT_TRUE(same.get() == first.get());
    MZ_ASSERT_TRUE(current_is_first.load());

    // 池已创建后再 setPoolSize 必须被忽略（且有 Warn 日志，不能静默生效）
    EventPollerPool::setPoolSize(1);
    MZ_ASSERT_EQ(pool.size(), expected);
}

// ---------------------------------------------------------------------------
// 任务队列上限（§4：有界 + 拒绝 + 可见）
// ---------------------------------------------------------------------------

MZ_TEST(poller_async_rejected_when_full) {
    auto poller = EventPoller::create("test-async-full");
    poller->setMaxPendingTasks(4);   // 默认 65536，这里调小以便构造满队列
    MZ_ASSERT_EQ(poller->maxPendingTasks(), 4u);

    // 用阻塞的**定时任务**卡住轮询线程：定时任务在 processDelayTask 里跑，
    // 位于 runPendingTasks **之前**，所以此时 _list_task 不会被整批取走
    Semaphore entered(0);
    Semaphore blocker(0);
    auto timer = poller->doDelayTask(0, [&entered, &blocker]() -> uint64_t {
        entered.post();
        blocker.wait();
        return 0;
    });
    MZ_ASSERT_NOT_NULL(timer.get());
    entered.wait();

    // 灌满队列
    MZ_ASSERT_TRUE(poller->async([]() {}));
    MZ_ASSERT_TRUE(poller->async([]() {}));
    MZ_ASSERT_TRUE(poller->async([]() {}));
    MZ_ASSERT_TRUE(poller->async([]() {}));
    MZ_ASSERT_EQ(poller->pendingTaskCount(), 4u);

    // 第 5 个必须被拒绝，而不是无限增长
    const uint64_t rejected_before = poller->asyncRejectedCount();
    MZ_ASSERT_FALSE(poller->async([]() {}));
    MZ_ASSERT_GT(poller->asyncRejectedCount(), rejected_before);
    MZ_ASSERT_EQ(poller->pendingTaskCount(), 4u);   // 被拒绝的任务不占队列

    // 不允许把上限关掉（0 会被拒绝并告警）
    const size_t limit_before = poller->maxPendingTasks();
    poller->setMaxPendingTasks(0);
    MZ_ASSERT_EQ(poller->maxPendingTasks(), limit_before);

    blocker.post();   // 放行
    poller->shutdown();
}
