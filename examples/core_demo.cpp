/*
 * core_demo —— M1 Core 层各组件的可运行示例
 * ============================================================================
 * 构建与运行：
 *   cmake -B build && cmake --build build -j
 *   ./build/bin/core_demo
 *
 * 覆盖：日志（控制台 + 滚动文件）、事件总线、线程池 + 信号量、Ticker、
 *       onceToken、可取消任务、线程组。
 *
 * 注意这一行就够：examples/ 属于"使用方"，按约定用伞头包含。
 * ============================================================================
 */

#include "mzmedia.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>

using namespace mzmedia;

namespace {

struct DemoReceiver {
    int count = 0;
    std::string last;
};

} // namespace

int main() {
    // -------------------------------------------------------------------
    // 1) 日志：控制台 + 文件（文件按大小滚动，父目录自动创建）
    // -------------------------------------------------------------------
    Logger::Instance().add(std::make_shared<ConsoleWriter>());
    Logger::Instance().add(std::make_shared<FileWriter>("logs/core_demo.log"));
    Logger::Instance().setLevel(LogLevel::Debug);
    InfoL << "core_demo 启动";

    // -------------------------------------------------------------------
    // 2) 事件总线：先注册监听，再发布事件
    // -------------------------------------------------------------------
    static int kEventTag = 0;
    DemoReceiver receiver;
    NoticeCenter::Instance().addListener<int, const std::string &>(
            &kEventTag, "demo_event", [&receiver](const int code, const std::string &msg) {
                ++receiver.count;
                receiver.last = msg;
                InfoP("  收到事件: code=%d msg=%s", code, msg.c_str());
            });

    NoticeCenter::Instance().emitEvent<int, const std::string &>(
            "demo_event", 200, std::string("hello"));
    InfoP("  事件回调次数=%d 最后消息=%s", receiver.count, receiver.last.c_str());

    // -------------------------------------------------------------------
    // 3) 线程池 + 信号量：投递任务并等全部完成
    // -------------------------------------------------------------------
    {
        constexpr int kTaskCount = 8;
        Semaphore done(0);
        std::atomic<int> sum{0};

        ThreadPool pool(2, 64);
        for (int i = 1; i <= kTaskCount; ++i) {
            const bool accepted = pool.async([i, &sum, &done]() {
                sum.fetch_add(i);
                done.post();   // 通知"我完成了"
            });
            if (!accepted) {
                WarnP("  任务 %d 被拒绝（队列已满）", i);
            }
        }
        for (int i = 0; i < kTaskCount; ++i) {
            done.wait();
        }

        const int expected = kTaskCount * (kTaskCount + 1) / 2;
        InfoP("线程池完成 %d 个任务: sum=%d（期望 %d）", kTaskCount, sum.load(), expected);
        pool.shutdown();
        InfoP("  线程池统计: 线程数=%zu 已完成=%llu 被拒绝=%llu", pool.threadCount(),
              static_cast<unsigned long long>(pool.completedCount()),
              static_cast<unsigned long long>(pool.rejectedCount()));
    }

    // -------------------------------------------------------------------
    // 4) Ticker + onceToken：测一段耗时，并在离开作用域时自动收尾
    // -------------------------------------------------------------------
    {
        Ticker ticker;
        onceToken token([]() { DebugL << "  进入计时作用域"; },
                        [&ticker]() {
                            InfoP("  计时作用域耗时 %llu ms",
                                  static_cast<unsigned long long>(ticker.elapsedTime()));
                        });
        sleepMs(20);
    }

    // -------------------------------------------------------------------
    // 5) 可取消任务（M2 的定时器就依赖它）
    // -------------------------------------------------------------------
    {
        int calls = 0;
        auto task = std::make_shared<TaskCancelableImp<uint64_t()>>([&calls]() -> uint64_t {
            ++calls;
            InfoP("  任务被执行（第 %d 次）", calls);
            return 0;   // 返回 0 表示"不再重复"
        });

        (*task)();
        task->setDeadline(getCurrentMillisecond() + 1000);
        InfoP("  任务到期时刻=%llu 已取消=%d", static_cast<unsigned long long>(task->deadline()),
              static_cast<int>(task->isCanceled()));

        task->cancel();
        (*task)();   // 取消后不会执行函数体
        InfoP("  取消后调用次数=%d（期望 1）", calls);
    }

    // -------------------------------------------------------------------
    // 6) 线程组：命名线程 + joinAll
    // -------------------------------------------------------------------
    {
        ThreadGroup group;
        std::atomic<int> counter{0};
        for (int i = 0; i < 3; ++i) {
            group.createThread([&counter]() { counter.fetch_add(1); }, "mz-demo-t");
        }
        group.joinAll();
        InfoP("线程组执行完毕: 线程数=%zu 计数=%d（期望 3）", group.size(), counter.load());
    }

    // -------------------------------------------------------------------
    // 7) 收尾：注销监听、刷新日志
    //    （Logger 在 atexit 时也会兜底 flush，这里显式调用只为让输出立刻可见）
    // -------------------------------------------------------------------
    NoticeCenter::Instance().delListener(&kEventTag);
    InfoL << "core_demo 结束";
    Logger::Instance().flush();
    return 0;
}
