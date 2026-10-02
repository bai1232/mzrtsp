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
#include <stdexcept>
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

    for (int i = 0; i < kRuns; ++i) {
        raw_over.push_back(rawSleepOverrun(kDelay));

        Semaphore fired(0);
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
}

// ---------------------------------------------------------------------------
// 超大维度：超长延时会被 clamp 到 INT_MAX —— 这条"静默降级"路径必须可见
// ---------------------------------------------------------------------------

MZ_TEST(timer_huge_delay_clamped) {
    auto poller = EventPoller::create("test-timer-clamp");
    const uint64_t before = poller->timeoutClampCount();

    // UINT64_MAX/2 毫秒 ≈ 1.5 亿年：deadline 远超 epoll_wait 的 int 上限
    auto task = poller->doDelayTask(UINT64_MAX / 2, []() -> uint64_t { return 0; });
    MZ_ASSERT_NOT_NULL(task.get());

    // 走一圈：sync 会写唤醒管道 → 事件循环重算超时并触发 clamp
    poller->sync([]() {});
    MZ_ASSERT_GT(poller->timeoutClampCount(), before);

    task->cancel();
    poller->shutdown();
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
