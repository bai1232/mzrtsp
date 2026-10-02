/*
 * 定时器专项测试（M2-2）
 * ============================================================================
 * 对应 docs/DESIGN_M2.md §6 的 M2-2 与 §7 的测试计划。
 *
 * 等待策略（与 TSAN 有关，务必保持）：
 *   本文件"等定时器触发"一律用 **Semaphore::wait()（无超时）**；
 *   "验证一段时间内没有发生" 用 **sleepMs()**；测时间用单调时钟
 *   `getCurrentMillisecond()`。**刻意不出现任何 wait_for / tryWait(ms)**，
 *   因此本组可以留在 TSAN 严格组（要求 0 报告）。
 *   代价：实现坏掉时用例会挂住，由 ctest 的 TIMEOUT(120s) 判失败。
 *
 * 生存期纪律：等待用的 Semaphore 必须**活得比轮询线程长** —— 只能在
 *   `poller->shutdown()`（内部 join）之后销毁。若声明在循环里，上一轮的
 *   `~Semaphore()`（pthread_cond_destroy）会与轮询线程仍在进行的 `post()`
 *   （pthread_cond_broadcast）并发，这是**真实竞态**，TSAN 严格组抓到过一次
 *   （规矩与实测报告见 docs/TESTING.md §8.7）。
 *
 * 断言纪律：回调都在轮询线程执行，**不允许在回调里调用 MZ_ASSERT_***；
 * 回调只写 std::atomic / 受信号量同步的普通变量，断言回到测试主线程。
 *
 * 精度门禁为什么长这样（本机实测数据，见 docs/DESIGN_M2.md §8 风险 R11）：
 *   本宿主机的"睡醒延迟"是 **0~14ms 的随机量，且与延时长短无关** ——
 *   裸 nanosleep 实测超出量：10ms 档 +6~+7、100ms 档 +9~+11、1s 档 +4~+13。
 *   即：±10ms 的**绝对**门禁连一个裸系统调用都过不了，与我们的代码无关。
 *   所以 timer_precision 拆成两条：
 *     ① 绝对下限：任何一次都不允许早触发（本库自己的保证，硬断言）
 *     ② 相对增量：最小值与**同进程裸 nanosleep 基线**的最小值对拍，差值 ≤5ms
 *        用最小值对拍的理由：系统性偏置（向上取整到 tick、单位算错）会把整个
 *        分布连同最小值一起平移，因此最小值能抓到偏置；而随机 tick 噪声只会
 *        抬高个别样本，抓不到最小值。用平均值/最大值对拍会被本机噪声随机翻红。
 * ============================================================================
 */

#include "test_main.h"

#include "core/util.h"
#include "network/event_poller.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace mzmedia;

// 是否在 ThreadSanitizer 构建下运行（编译期判定）。
// 为什么要区分：TSAN 会给**库代码**插桩（跨线程投递、mutex、map 操作），
// 但不会给内核里的 nanosleep 插桩，于是"库 vs 裸基线"的差值里混进了插桩开销
// （实测 TSAN 下 min 差值 ~3ms，且抖动更大）。计时门禁因此在 TSAN 下放宽，
// 正式门禁以普通构建为准。
#if defined(__SANITIZE_THREAD__)
#define MZ_TIMER_UNDER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define MZ_TIMER_UNDER_TSAN 1
#endif
#endif
#ifndef MZ_TIMER_UNDER_TSAN
#define MZ_TIMER_UNDER_TSAN 0
#endif

namespace {

// 实测数值必须打出来（AI_COLLAB §4：精度这种"只有数字能说明问题"的用例，
// 不能只给一个 PASS）。
// 刻意用 printf 而不是 InfoP：Logger 默认**没有任何 writer**，日志会被静默丢掉，
// 而测试框架自己的输出走的就是 printf。
void mzReport(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void mzReport(const char *fmt, ...) {
    va_list args;
    std::printf("    [ INFO ] ");
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    std::fflush(stdout);
}
/// 裸 nanosleep 基线：返回实际耗时超出请求值的毫秒数（可能为 0）
int64_t rawSleepOverrun(uint64_t delay_ms) {
    const struct timespec req = {static_cast<time_t>(delay_ms / 1000),
                                 static_cast<long>(delay_ms % 1000) * 1000000L};
    const uint64_t begin = getCurrentMillisecond();
    nanosleep(&req, nullptr);
    return static_cast<int64_t>(getCurrentMillisecond() - begin - delay_ms);
}

int64_t minOf(const std::vector<int64_t> &v) {
    return *std::min_element(v.begin(), v.end());
}

int64_t maxOf(const std::vector<int64_t> &v) {
    return *std::max_element(v.begin(), v.end());
}

int64_t avgOf(const std::vector<int64_t> &v) {
    int64_t sum = 0;
    for (int64_t x : v) {
        sum += x;
    }
    return sum / static_cast<int64_t>(v.size());
}

} // namespace

// ---------------------------------------------------------------------------
// 精度（正常维度）
// ---------------------------------------------------------------------------

MZ_TEST(timer_precision) {
    constexpr int kRuns = 10;
    constexpr uint64_t kDelay = 100;   // ms

    auto poller = EventPoller::create("test-timer-precision");
    std::vector<int64_t> lib_over;   // 库定时器超出量
    std::vector<int64_t> raw_over;   // 裸 nanosleep 超出量（宿主基线）

    // fired 必须**在循环外**、活得比轮询线程长：
    // 若声明在循环里，上一轮的 ~Semaphore()（pthread_cond_destroy）会与轮询线程
    // 仍在进行的 post()（pthread_cond_broadcast）并发 —— 这是**真实竞态**，
    // TSAN 严格组抓到过（main 线程 destroy vs poller 线程 broadcast）。
    // 规矩：等待用的 Semaphore 只有在 poller->shutdown()（内部 join）之后才能销毁。
    Semaphore fired(0);

    for (int i = 0; i < kRuns; ++i) {
        raw_over.push_back(rawSleepOverrun(kDelay));

        const uint64_t begin = getCurrentMillisecond();
        auto task = poller->doDelayTask(kDelay, [&fired]() -> uint64_t {
            fired.post();
            return 0;   // 一次性
        });
        MZ_ASSERT_NOT_NULL(task.get());
        fired.wait();

        const int64_t over = static_cast<int64_t>(getCurrentMillisecond() - begin - kDelay);
        lib_over.push_back(over);
        // ① 绝不允许早触发：deadline 由同一个单调时钟基于调用时刻算出，
        //    因此 elapsed >= delay 是本库必须给出的硬保证
        MZ_ASSERT_GE(over, 0);
    }

    // 把两组数都打出来：门禁失败时能看到"到底是库的问题还是宿主的问题"
    mzReport("timer_precision: 库 %llu ms × %d → 超出量 min=%lld avg=%lld max=%lld ms",
             static_cast<unsigned long long>(kDelay), kRuns,
             static_cast<long long>(minOf(lib_over)), static_cast<long long>(avgOf(lib_over)),
             static_cast<long long>(maxOf(lib_over)));
    mzReport("timer_precision: 裸 nanosleep 基线 → 超出量 min=%lld avg=%lld max=%lld ms"
             "（宿主唤醒延迟，与延时长短无关）",
             static_cast<long long>(minOf(raw_over)), static_cast<long long>(avgOf(raw_over)),
             static_cast<long long>(maxOf(raw_over)));

    // ② 库相对宿主基线不引入系统性额外延迟
    const int64_t tolerance = MZ_TIMER_UNDER_TSAN ? 25 : 5;
    mzReport("timer_precision: 门禁 = min(库) ≤ min(裸) + %lld ms%s",
             static_cast<long long>(tolerance),
             MZ_TIMER_UNDER_TSAN ? "（TSAN 构建：库路径被插桩，门禁放宽，正式门禁见普通构建）" : "");
    MZ_ASSERT_LE(minOf(lib_over), minOf(raw_over) + tolerance);
    // ③ 灾难性回归守卫：宿主噪声上限实测 ~14ms，这里放到 50ms（TSAN 下 100ms）
    //    只为"不会误红"，真正的精度结论由 ② 给出
    MZ_ASSERT_LE(maxOf(lib_over), MZ_TIMER_UNDER_TSAN ? 100 : 50);

    MZ_ASSERT_EQ(poller->timerCount(), 0u);
    poller->shutdown();
}

MZ_TEST(timer_zero_delay) {
    auto poller = EventPoller::create("test-timer-zero");
    Semaphore fired(0);
    const uint64_t begin = getCurrentMillisecond();

    auto task = poller->doDelayTask(0, [&fired]() -> uint64_t {
        fired.post();
        return 0;
    });
    MZ_ASSERT_NOT_NULL(task.get());
    fired.wait();

    // 0 延时不应退化成"等一个完整 tick"
    MZ_ASSERT_LT(getCurrentMillisecond() - begin, 50u);
    poller->shutdown();
}

// ---------------------------------------------------------------------------
// 空维度：没有任何定时器时，epoll_wait 会无限等待，跨线程投递必须仍能唤醒
// ---------------------------------------------------------------------------

MZ_TEST(timer_no_timer_path) {
    auto poller = EventPoller::create("test-timer-none");
    MZ_ASSERT_EQ(poller->timerCount(), 0u);

    std::atomic<bool> ran{false};
    // 此时事件线程正阻塞在"无限超时"的 epoll_wait 上
    poller->sync([&ran]() { ran.store(true); });

    MZ_ASSERT_TRUE(ran.load());
    MZ_ASSERT_EQ(poller->timerCount(), 0u);
    poller->shutdown();
}

// ---------------------------------------------------------------------------
// 满维度：一次投大量定时器
// ---------------------------------------------------------------------------

MZ_TEST(timer_many_1000) {
    constexpr int kCount = 1000;
    auto poller = EventPoller::create("test-timer-many");

    std::atomic<int> fired{0};
    Semaphore done(0);
    for (int i = 0; i < kCount; ++i) {
        auto task = poller->doDelayTask(10, [&fired, &done]() -> uint64_t {
            if (fired.fetch_add(1) + 1 == kCount) {
                done.post();
            }
            return 0;
        });
        MZ_ASSERT_NOT_NULL(task.get());
    }

    done.wait();
    MZ_ASSERT_EQ(fired.load(), kCount);
    MZ_ASSERT_EQ(poller->timerCount(), 0u);   // 全部出堆，无残留
    // 1000 个同刻到期 ≈ "100 路客户端秒级校验同时到点"的放大版：
    // 观测它们被分成几批（批次越大，单次 processDelayTask 占用的时间越长）
    mzReport("timer_many_1000: 单批最大 %zu 个 / 非空批次数 %llu",
             poller->delayBatchMax(), static_cast<unsigned long long>(poller->delayBatchCount()));
    poller->shutdown();
}

// ---------------------------------------------------------------------------
// 断开维度：关闭时未到期的定时器不再触发
// ---------------------------------------------------------------------------

MZ_TEST(timer_shutdown_cancels) {
    auto poller = EventPoller::create("test-timer-shutdown");
    std::atomic<int> fired{0};

    auto task = poller->doDelayTask(1000, [&fired]() -> uint64_t {
        fired.fetch_add(1);
        return 0;
    });
    MZ_ASSERT_NOT_NULL(task.get());

    // 用 sync 冲刷而不是"睡 20ms 再赌它已入堆"：跨线程任务是 FIFO 的，
    // sync 返回即保证之前的插入任务已经执行完（本宿主 10ms 级的调度抖动
    // 会让固定睡眠变成 flaky 用例）
    poller->sync([]() {});
    MZ_ASSERT_EQ(poller->timerCount(), 1u);

    poller->shutdown();
    sleepMs(50);
    MZ_ASSERT_EQ(fired.load(), 0);
    MZ_ASSERT_EQ(poller->timerCount(), 0u);   // 退出清理已清空
    // 未触发的定时器在退出时被丢弃 —— 必须可见。
    // （未执行的"任务"有 droppedOnExitCount + Warn，定时器原先一条计数都没有 = 静默丢弃）
    MZ_ASSERT_EQ(poller->droppedTimerOnExitCount(), 1u);
}

// ---------------------------------------------------------------------------
// 超大维度：超长延时会被 clamp 到 INT_MAX —— 这条"静默降级"路径必须可见
// ---------------------------------------------------------------------------

MZ_TEST(timer_huge_delay_clamped) {
    // 两个边界值都必须被 clamp 且**可见**：
    //   a) UINT64_MAX/2 → deadline - now 恰好等于 INT64_MAX（int64 上界，仍不溢出）
    //   b) UINT64_MAX/2 + 1000 → 再往上就越过 int64 上界。若实现是
    //      `static_cast<int64_t>(next - now)`，这里会溢出成**负数**，被
    //      clampTimeout 当成"没有定时器" → epoll_wait 永久等待 →
    //      超长定时器**静默永不触发**，且计数与告警都不涨（违反 §4.5）
    //
    // 每个值各用一个**独立的 poller**：超长定时器一旦入堆就不会出来（deadline 在
    // 1.5 亿年后，cancel 只是标记，仍要等到期才出堆），留在同一个 poller 里会让
    // "计数有没有涨"被前一个定时器每轮的 clamp 顶上去 —— 断言就失去区分度了
    // （这个坑是真实踩到的：同 poller 版本对溢出漏洞是绿的）。
    const uint64_t delays[2] = {UINT64_MAX / 2, UINT64_MAX / 2 + 1000};
    for (uint64_t delay : delays) {
        auto poller = EventPoller::create("test-timer-clamp");
        const uint64_t before = poller->timeoutClampCount();   // 该 poller 上还没有任何定时器

        auto task = poller->doDelayTask(delay, []() -> uint64_t { return 0; });
        MZ_ASSERT_NOT_NULL(task.get());

        // 两次 sync：第一次只保证"插入已完成"（插入与它可能落在同一批任务里被执行），
        // 第二次才保证事件循环又走完至少一轮 —— 截断计数是轮首算超时时加的
        poller->sync([]() {});
        poller->sync([]() {});

        MZ_ASSERT_GT(poller->timeoutClampCount(), before);
        task->cancel();
        poller->shutdown();
    }
}

// ---------------------------------------------------------------------------
// 结构正确性：多个定时器必须按到期时间顺序触发（multimap 最小堆）
// ---------------------------------------------------------------------------

MZ_TEST(timer_order_by_deadline) {
    auto poller = EventPoller::create("test-timer-order");

    std::vector<int> order;
    std::atomic<int> count{0};
    Semaphore done(0);

    // 故意乱序投递：300ms、100ms、200ms。
    // 档位间隔取 100ms 而不是 10ms：每个 deadline 都是在**调用线程**上按"当时时刻"
    // 算出来的，若两次投递之间被宿主调度打断超过档位间隔，期望顺序本身就不成立了
    // （本宿主有 10ms 级的调度抖动，10ms 间隔会真的翻红）。
    const int delays[3] = {300, 100, 200};
    for (int i = 0; i < 3; ++i) {
        auto task = poller->doDelayTask(static_cast<uint64_t>(delays[i]),
                                        [&order, &count, &done, i]() -> uint64_t {
                                            // 回调都在轮询线程 → 不需要锁
                                            order.push_back(i);
                                            if (count.fetch_add(1) + 1 == 3) {
                                                done.post();
                                            }
                                            return 0;
                                        });
        MZ_ASSERT_NOT_NULL(task.get());
    }

    done.wait();
    MZ_ASSERT_EQ(order.size(), 3u);
    MZ_ASSERT_EQ(order[0], 1);   // 100ms
    MZ_ASSERT_EQ(order[1], 2);   // 200ms
    MZ_ASSERT_EQ(order[2], 0);   // 300ms
    poller->shutdown();
}

// ---------------------------------------------------------------------------
// 竞态维度：到期瞬间取消 —— 只断言"不崩 + 无残留"，不做时序断言（否则必然 flaky）
// ---------------------------------------------------------------------------

MZ_TEST(timer_cancel_race) {
    constexpr int kRounds = 200;
    auto poller = EventPoller::create("test-timer-cancel-race");
    std::atomic<int> fired{0};

    for (int i = 0; i < kRounds; ++i) {
        auto task = poller->doDelayTask(1, [&fired]() -> uint64_t {
            fired.fetch_add(1);
            return 0;
        });
        if (!task) {
            MZ_FAIL("doDelayTask 被拒绝（poller 不应在运行中拒绝）");
            break;
        }
        task->cancel();   // 与 1ms 到期竞态：要么已执行，要么被取消
    }

    sleepMs(100);   // 让所有在途的定时器落定（宿主 tick 量化下 1ms 可能实际 10ms+）
    MZ_ASSERT_LE(fired.load(), kRounds);
    MZ_ASSERT_EQ(poller->timerCount(), 0u);   // 取消的也会在到期时出堆，不留残留
    poller->shutdown();
}

// ---------------------------------------------------------------------------
// 循环任务：返回非 0 重复、返回 0 结束，次数必须准确
// ---------------------------------------------------------------------------

MZ_TEST(timer_repeat_count) {
    constexpr int kExpected = 5;
    auto poller = EventPoller::create("test-timer-repeat");

    std::atomic<int> fired{0};
    Semaphore enough(0);
    const uint64_t begin = getCurrentMillisecond();

    auto task = poller->doDelayTask(20, [&fired, &enough]() -> uint64_t {
        const int now = fired.fetch_add(1) + 1;
        if (now >= kExpected) {
            enough.post();
            return 0;   // 返回 0 = 结束
        }
        return 20;      // 非 0 = 再等 20ms
    });
    MZ_ASSERT_NOT_NULL(task.get());

    enough.wait();
    sleepMs(100);   // 若"返回 0 就结束"没生效，计数会继续往上涨
    MZ_ASSERT_EQ(fired.load(), kExpected);

    // 每次重复的 deadline 都从"上一次回调返回之后"起算 → 5 次触发的总时长 ≥ 4×20ms。
    // 这里刻意不断言上界：上界由宿主唤醒延迟决定，断言它就是把宿主抖动当被测指标。
    const uint64_t elapsed = getCurrentMillisecond() - begin;
    MZ_ASSERT_GE(elapsed, 80u);
    mzReport("timer_repeat_count: 20ms 循环任务恰好触发 %d 次，实际耗时 %llu ms（下限 80ms）",
             kExpected, static_cast<unsigned long long>(elapsed));

    MZ_ASSERT_EQ(poller->timerCount(), 0u);   // 结束后不留条目
    poller->shutdown();
}

// ---------------------------------------------------------------------------
// 调用上下文维度（M2-2b 补）
//
// 为什么单列一组：上面 10 条用例全部是"从测试线程、一次性、不在回调里"投递的，
// 而真实服务器里定时器是在**回调里、多生产者、退出过程中**投的。这一组按
// "谁投 / 什么时候投"来测，正是 DESIGN_M2 §7.2 那张矩阵要补的那一维。
// ---------------------------------------------------------------------------

MZ_TEST(timer_concurrent_submit) {
    // 多生产者并发投递。固定 4 线程：测试要可复现，不随机器核数变。
    // 结构上 multimap 只被轮询线程碰，所以这里真正考验的是**任务队列在 N 生产者下
    // 的完整性**（以及计数不会漂）
    constexpr int kThreads = 4;
    constexpr int kPerThread = 250;
    constexpr int kTotal = kThreads * kPerThread;

    auto poller = EventPoller::create("test-timer-concurrent");
    std::atomic<int> fired{0};
    std::atomic<int> rejected{0};
    Semaphore done(0);
    std::vector<std::thread> producers;

    for (int t = 0; t < kThreads; ++t) {
        producers.emplace_back([&poller, &fired, &rejected, &done]() {
            for (int i = 0; i < kPerThread; ++i) {
                auto task = poller->doDelayTask(10, [&fired, &done]() -> uint64_t {
                    if (fired.fetch_add(1) + 1 == kTotal) {
                        done.post();
                    }
                    return 0;
                });
                if (!task) {
                    rejected.fetch_add(1);   // 工作线程里不断言，只记录
                }
            }
        });
    }
    for (auto &producer : producers) {
        producer.join();
    }
    done.wait();

    MZ_ASSERT_EQ(rejected.load(), 0);
    MZ_ASSERT_EQ(fired.load(), kTotal);
    MZ_ASSERT_EQ(poller->timerCount(), 0u);
    MZ_ASSERT_EQ(poller->rejectedTimerCount(), 0u);
    MZ_ASSERT_EQ(poller->asyncRejectedCount(), 0u);
    MZ_ASSERT_EQ(poller->pendingTaskCount(), 0u);
    mzReport("timer_concurrent_submit: %d 线程 × %d 个 = %d 全部触发；单批最大 %zu / 非空批次数 %llu",
             kThreads, kPerThread, kTotal, poller->delayBatchMax(),
             static_cast<unsigned long long>(poller->delayBatchCount()));
    poller->shutdown();
}

MZ_TEST(timer_same_deadline) {
    // 同 deadline 的一批定时器：这里**只锁"不丢、不饿死"**，不锁顺序。
    // 顺序不写成断言的原因：deadline 是 doDelayTask 内部按 now+delay 算的，
    // 100 次投递只要跨了 1ms，插入序就与 deadline 序不一致；跨线程更是如此。
    // （multimap 对等价键保证插入序，但"能造出等价键"这件事本身不可依赖）
    constexpr int kDeadlineCount = 100;
    auto poller = EventPoller::create("test-timer-same-deadline");
    std::atomic<int> fired{0};
    Semaphore done(0);

    for (int i = 0; i < kDeadlineCount; ++i) {
        auto task = poller->doDelayTask(50, [&fired, &done]() -> uint64_t {
            if (fired.fetch_add(1) + 1 == kDeadlineCount) {
                done.post();
            }
            return 0;
        });
        MZ_ASSERT_NOT_NULL(task.get());
    }

    done.wait();
    MZ_ASSERT_EQ(fired.load(), kDeadlineCount);
    MZ_ASSERT_EQ(poller->timerCount(), 0u);
    mzReport("timer_same_deadline: %d 个 50ms 定时器 → 单批最大 %zu / 非空批次数 %llu",
             kDeadlineCount, poller->delayBatchMax(),
             static_cast<unsigned long long>(poller->delayBatchCount()));
    poller->shutdown();
}

MZ_TEST(timer_cancel_after_fire) {
    // 一次性定时器**已经触发**（已出堆）之后再 cancel —— 必须是安全的 no-op。
    // 这是高频正常路径：空闲超时 → onIdle → shutdown → cancel 自己那个检查任务；
    // 100 路客户端里每次超时都会走一遍
    auto poller = EventPoller::create("test-timer-cancel-after-fire");
    std::atomic<int> fired{0};
    Semaphore fired_once(0);

    auto task = poller->doDelayTask(10, [&fired, &fired_once]() -> uint64_t {
        fired.fetch_add(1);
        fired_once.post();
        return 0;
    });
    MZ_ASSERT_NOT_NULL(task.get());

    fired_once.wait();
    MZ_ASSERT_EQ(poller->timerCount(), 0u);   // 已经出堆

    task->cancel();   // 对已触发过的任务取消：handle 仍持有对象（shared_ptr），不得 UAF
    sleepMs(50);
    MZ_ASSERT_EQ(fired.load(), 1);            // 不会"复活"再触发一次
    MZ_ASSERT_TRUE(task->isCanceled());
    MZ_ASSERT_EQ(poller->timerCount(), 0u);   // 也不会留下条目
    poller->shutdown();
}

MZ_TEST(timer_reentrant_submit) {
    // 回调里再投定时器（重入投递）—— M3 的 HTTP keep-alive、协议层心跳都这么用。
    // 安全性来自 processDelayTask 先 erase 再执行回调、且每轮重新取 begin()
    constexpr int kChain = 5;
    auto poller = EventPoller::create("test-timer-reentrant");
    std::atomic<int> fired{0};
    std::atomic<int> rejected{0};
    Semaphore done(0);

    std::function<void()> arm;   // 自引用：每一步在回调里再投下一步
    arm = [&poller, &fired, &rejected, &done, &arm]() {
        const int step = fired.fetch_add(1) + 1;
        if (step >= kChain) {
            done.post();
            return;   // 链条结束
        }
        auto next = poller->doDelayTask(10, [&arm]() -> uint64_t {
            arm();
            return 0;
        });
        if (!next) {
            rejected.fetch_add(1);   // 回调里不断言，只记录
        }
    };

    auto first = poller->doDelayTask(10, [&arm]() -> uint64_t {
        arm();
        return 0;
    });
    MZ_ASSERT_NOT_NULL(first.get());

    done.wait();
    sleepMs(50);   // 链条若没真的结束，计数还会继续涨
    MZ_ASSERT_EQ(fired.load(), kChain);
    MZ_ASSERT_EQ(rejected.load(), 0);
    MZ_ASSERT_EQ(poller->timerCount(), 0u);
    poller->shutdown();
}

MZ_TEST(timer_reentrant_zero_delay) {
    // 0 延时自投链：M2-2b 只**观测**，不限制（决策见 DESIGN_M2 §9）。
    // 性质：新条目 deadline == now → 会被**同一个 processDelayTask 循环**处理完，
    // 期间既不派发 epoll 事件、也不检查 _exit（所以自投链能拖住退出）。
    // 这里不断言"必须在同一批"——那是性质不是契约，只把观测值打出来
    constexpr int kSteps = 20;
    auto poller = EventPoller::create("test-timer-zero-chain");
    std::atomic<int> fired{0};
    std::atomic<int> rejected{0};
    Semaphore done(0);

    std::function<void()> arm;
    arm = [&poller, &fired, &rejected, &done, &arm]() {
        const int step = fired.fetch_add(1) + 1;
        if (step >= kSteps) {
            done.post();
            return;
        }
        auto next = poller->doDelayTask(0, [&arm]() -> uint64_t {
            arm();
            return 0;
        });
        if (!next) {
            rejected.fetch_add(1);
        }
    };

    auto first = poller->doDelayTask(0, [&arm]() -> uint64_t {
        arm();
        return 0;
    });
    MZ_ASSERT_NOT_NULL(first.get());

    done.wait();
    MZ_ASSERT_EQ(fired.load(), kSteps);
    MZ_ASSERT_EQ(rejected.load(), 0);
    MZ_ASSERT_EQ(poller->timerCount(), 0u);
    mzReport("timer_reentrant_zero_delay: %d 步 0 延时自投链 → 单批最大 %zu / 非空批次数 %llu"
             "（单批 ≈ 步数 说明这段时间内不派发 I/O）",
             kSteps, poller->delayBatchMax(),
             static_cast<unsigned long long>(poller->delayBatchCount()));
    poller->shutdown();
}

MZ_TEST(timer_submit_after_shutdown) {
    // 契约：poller 已退出 → doDelayTask 返回 nullptr + rejectedTimerCount()（与跨线程
    // 路径语义统一）。
    // **修前这条是红的**：轮询线程路径不检查 _exit，返回一个非空 handle，但它永远不会
    // 触发，也没有任何计数/告警 —— 静默降级（AI_COLLAB §4.5）。
    // 确定性造法：在轮询线程的 sync 任务里先 shutdown()（只置位 + 唤醒，不 join 自己），
    // 再投定时器
    auto poller = EventPoller::create("test-timer-after-exit");
    std::atomic<int> fired{0};
    std::atomic<bool> got_null{false};

    poller->sync([&poller, &fired, &got_null]() {
        poller->shutdown();
        auto task = poller->doDelayTask(10, [&fired]() -> uint64_t {
            fired.fetch_add(1);
            return 0;
        });
        got_null.store(task == nullptr);   // 轮询线程里不断言，只记录
    });

    MZ_ASSERT_TRUE(got_null.load());
    MZ_ASSERT_GT(poller->rejectedTimerCount(), 0u);
    MZ_ASSERT_EQ(fired.load(), 0);   // 被拒的定时器绝不会触发
}

// ---------------------------------------------------------------------------
// 失败路径：定时任务抛异常后不再重复
// ---------------------------------------------------------------------------

MZ_TEST(timer_exception_stops_repeat) {
    auto poller = EventPoller::create("test-timer-exception");
    std::atomic<int> fired{0};
    Semaphore second(0);

    auto task = poller->doDelayTask(10, [&fired, &second]() -> uint64_t {
        const int now = fired.fetch_add(1) + 1;
        if (now >= 2) {
            second.post();
            throw std::runtime_error("timer boom");
        }
        return 10;   // 第 1 次返回 10 → 重复
    });
    MZ_ASSERT_NOT_NULL(task.get());

    second.wait();   // 第 2 次触发（并在这里抛异常）
    sleepMs(50);     // 若异常后仍然重复，计数会继续涨
    MZ_ASSERT_EQ(fired.load(), 2);
    MZ_ASSERT_EQ(poller->timerCount(), 0u);   // 异常后不再入堆
    poller->shutdown();
}
