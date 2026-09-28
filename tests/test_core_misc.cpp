/*
 * Core 杂项组件单元测试：Ticker / onceToken / TaskCancelable / ThreadGroup / NoticeCenter
 *
 * 注意：不允许在工作线程里调用 MZ_ASSERT_*（框架计数器不是线程安全的），
 * 线程里只写 std::atomic / 受锁保护的变量，断言一律回到测试主线程。
 */

#include "test_main.h"

#include "core/notice_center.h"
#include "core/once_token.h"
#include "core/task_cancelable.h"
#include "core/thread_group.h"
#include "core/ticker.h"
#include "core/util.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace mzmedia;

// ---------------------------------------------------------------------------
// Ticker
// ---------------------------------------------------------------------------

MZ_TEST(core_ticker_elapsed) {
    Ticker ticker;
    sleepMs(30);

    const uint64_t ms = ticker.elapsedTime();
    MZ_ASSERT_GE(ms, 25u);    // 允许少量调度误差
    MZ_ASSERT_LT(ms, 500u);
    MZ_ASSERT_GE(ticker.elapsedTimeInMicro(), 25000u);
    MZ_ASSERT_GE(ticker.elapsedTimeInSecond(), 0.025);

    // 单调时钟：只会前进
    const uint64_t first = ticker.elapsedTimeInMicro();
    sleepMs(5);
    MZ_ASSERT_GT(ticker.elapsedTimeInMicro(), first);
}

MZ_TEST(core_ticker_reset) {
    Ticker ticker;
    sleepMs(30);
    MZ_ASSERT_GE(ticker.elapsedTime(), 25u);

    ticker.reset();
    MZ_ASSERT_LT(ticker.elapsedTime(), 30u);   // reset 之后重新开始计时
}

// ---------------------------------------------------------------------------
// onceToken
// ---------------------------------------------------------------------------

MZ_TEST(core_once_token) {
    int constructed = 0;
    int destructed = 0;
    {
        onceToken token([&constructed]() { ++constructed; }, [&destructed]() { ++destructed; });
        MZ_ASSERT_EQ(constructed, 1);
        MZ_ASSERT_EQ(destructed, 0);   // 还未离开作用域
    }
    MZ_ASSERT_EQ(constructed, 1);
    MZ_ASSERT_EQ(destructed, 1);       // 析构时触发

    // dismiss 之后不再触发析构回调
    {
        onceToken token(nullptr, [&destructed]() { ++destructed; });
        token.dismiss();
    }
    MZ_ASSERT_EQ(destructed, 1);

    // 只给构造回调也允许
    int only_construct = 0;
    {
        onceToken token([&only_construct]() { ++only_construct; });
    }
    MZ_ASSERT_EQ(only_construct, 1);
}

// ---------------------------------------------------------------------------
// TaskCancelable
// ---------------------------------------------------------------------------

MZ_TEST(core_task_cancelable) {
    int calls = 0;
    auto task = std::make_shared<TaskCancelableImp<int(int)>>([&calls](int value) {
        ++calls;
        return value * 2;
    });

    MZ_ASSERT_FALSE(task->isCanceled());
    MZ_ASSERT_EQ((*task)(21), 42);
    MZ_ASSERT_EQ(calls, 1);

    // 到期时刻（M2 定时器排序用）
    MZ_ASSERT_EQ(task->deadline(), 0u);
    task->setDeadline(12345);
    MZ_ASSERT_EQ(task->deadline(), 12345u);

    // 取消后：不再执行函数体，返回默认值
    task->cancel();
    MZ_ASSERT_TRUE(task->isCanceled());
    MZ_ASSERT_EQ((*task)(21), 0);
    MZ_ASSERT_EQ(calls, 1);   // 关键：函数体没有被再调用

    // 通过基类指针操作（M2 的定时器就是这样持有任务的）
    TaskCancelable::Ptr base = task;
    MZ_ASSERT_TRUE(base->isCanceled());
    base->cancel();           // 幂等
    MZ_ASSERT_TRUE(base->isCanceled());
    MZ_ASSERT_EQ(calls, 1);
}

MZ_TEST(core_task_cancelable_void) {
    int calls = 0;
    TaskCancelableImp<void()> task([&calls]() { ++calls; });

    task();
    MZ_ASSERT_EQ(calls, 1);

    task.cancel();
    task();                   // 取消后调用不得执行函数体
    MZ_ASSERT_EQ(calls, 1);
}

// ---------------------------------------------------------------------------
// ThreadGroup
// ---------------------------------------------------------------------------

MZ_TEST(core_thread_group) {
    ThreadGroup group;
    MZ_ASSERT_TRUE(group.empty());
    MZ_ASSERT_EQ(group.size(), 0u);

    std::atomic<int> count{0};
    for (int i = 0; i < 4; ++i) {
        group.createThread([&count]() { count.fetch_add(1); });
    }
    MZ_ASSERT_FALSE(group.empty());
    MZ_ASSERT_EQ(group.size(), 4u);

    group.joinAll();
    MZ_ASSERT_EQ(count.load(), 4);
    group.joinAll();               // 幂等
    MZ_ASSERT_EQ(group.size(), 4u);
}

MZ_TEST(core_thread_group_named) {
    ThreadGroup group;
    std::mutex mtx;
    std::string observed;

    group.createThread(
            [&observed, &mtx]() {
                const std::string name = getThreadName();
                std::lock_guard<std::mutex> lck(mtx);
                observed = name;
            },
            "mz-demo-t");

    group.joinAll();
    MZ_ASSERT_STR_EQ(observed, "mz-demo-t");
}

// ---------------------------------------------------------------------------
// NoticeCenter
// ---------------------------------------------------------------------------

MZ_TEST(core_notice_center_basic) {
    NoticeCenter &center = NoticeCenter::Instance();
    center.clear();

    int tag = 0;
    std::string received;
    center.addListener<std::string>(&tag, "ev", [&received](const std::string &msg) { received = msg; });
    MZ_ASSERT_EQ(center.listenerCount("ev"), 1u);
    MZ_ASSERT_EQ(center.listenerCount(), 1u);

    // 支持临时值（字面量）与左值两种传参
    center.emitEvent<std::string>("ev", "hello");
    MZ_ASSERT_STR_EQ(received, "hello");

    std::string second = "world";
    center.emitEvent<std::string>("ev", second);
    MZ_ASSERT_STR_EQ(received, "world");

    // 注销后不再收到
    center.delListener(&tag, "ev");
    MZ_ASSERT_EQ(center.listenerCount("ev"), 0u);
    center.emitEvent<std::string>("ev", "ignored");
    MZ_ASSERT_STR_EQ(received, "world");

    // 没有监听者时发布不能崩
    center.emitEvent<std::string>("not-exist", "x");
    center.clear();
}

MZ_TEST(core_notice_center_multi_listener) {
    NoticeCenter &center = NoticeCenter::Instance();
    center.clear();

    int tag_a = 0;
    int tag_b = 0;
    int sum = 0;
    center.addListener<int>(&tag_a, "num", [&sum](const int &value) { sum += value; });
    center.addListener<int>(&tag_b, "num", [&sum](const int &value) { sum += value * 10; });
    MZ_ASSERT_EQ(center.listenerCount("num"), 2u);

    center.emitEvent<int>("num", 3);
    MZ_ASSERT_EQ(sum, 33);   // 3 + 30
    center.clear();
}

MZ_TEST(core_notice_center_reentrant) {
    // 关键用例：回调里再次发布 / 增删监听器都不能死锁
    // （实现是把监听器列表拷出来在锁外回调，所以这些操作都是安全的）
    NoticeCenter &center = NoticeCenter::Instance();
    center.clear();

    int tag_outer = 0;
    int tag_inner = 0;
    int tag_other = 0;
    int hits = 0;
    int other_hits = 0;

    center.addListener<std::string>(&tag_outer, "ev",
            [&center, &tag_inner, &hits](const std::string &msg) {
                ++hits;
                // ① 回调里再次发布另一个事件（重入）
                center.emitEvent<std::string>("other", msg);
                // ② 回调里新增监听器
                center.addListener<std::string>(&tag_inner, "ev", [](const std::string &) {});
                // ③ 回调里注销监听器
                center.delListener(&tag_inner, "ev");
            });
    center.addListener<std::string>(&tag_other, "other",
            [&other_hits](const std::string &) { ++other_hits; });

    center.emitEvent<std::string>("ev", std::string("payload"));

    MZ_ASSERT_EQ(hits, 1);
    MZ_ASSERT_EQ(other_hits, 1);                 // 重入发布生效
    // 快照语义：本次发布中新注册的监听器不会被调用，且它已被回调自己注销
    MZ_ASSERT_EQ(center.listenerCount(), 2u);    // tag_outer(ev) + tag_other(other)
    center.clear();
}

MZ_TEST(core_notice_center_tag_removal) {
    NoticeCenter &center = NoticeCenter::Instance();
    center.clear();

    int tag_a = 0;
    int tag_b = 0;
    center.addListener<int>(&tag_a, "e1", [](const int &) {});
    center.addListener<int>(&tag_a, "e2", [](const int &) {});
    center.addListener<int>(&tag_b, "e1", [](const int &) {});
    MZ_ASSERT_EQ(center.listenerCount(), 3u);

    center.delListener(&tag_a);   // 不带事件名 = 注销该 tag 的所有监听
    MZ_ASSERT_EQ(center.listenerCount(), 1u);
    MZ_ASSERT_EQ(center.listenerCount("e1"), 1u);
    MZ_ASSERT_EQ(center.listenerCount("e2"), 0u);

    center.delListener(&tag_b, "e1");
    MZ_ASSERT_EQ(center.listenerCount(), 0u);
    center.clear();
}

MZ_TEST(core_notice_center_clear) {
    NoticeCenter &center = NoticeCenter::Instance();
    center.clear();

    int tag = 0;
    center.addListener<int>(&tag, "e", [](const int &) {});
    MZ_ASSERT_EQ(center.listenerCount(), 1u);

    center.clear();
    MZ_ASSERT_EQ(center.listenerCount(), 0u);
    MZ_ASSERT_EQ(center.listenerCount("e"), 0u);
}
