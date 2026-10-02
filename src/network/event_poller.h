/*
 * EventPoller：epoll 事件循环 + 定时器 + 跨线程任务投递
 * ============================================================================
 * 形状（在设计文档 docs/DESIGN_M2.md §3.5 定死，不后改）：
 *   - 注册类接口返回 int：0 = 成功，-1 = 失败（失败一律记日志 + 计数）
 *   - async 返回 bool；sync 在投递失败时抛 std::runtime_error（不能静默）
 *   - 事件回调只在轮询线程执行；注册类接口可从任意线程调用
 *   - EAGAIN / EWOULDBLOCK / EINTR 不是错误
 *
 * 失败可见性（AI_COLLAB.md §4）：
 *   所有可能失败的操作都有观测出口 —— epollErrorCount / timerCount /
 *   timeoutClampCount / rejectedTimerCount / pendingTaskCount。
 *
 * 线程不变式：
 *   _event_map / _delay_task_map 只允许在轮询线程访问；
 *   跨线程调用通过 async/sync 投递到轮询线程执行。
 * ============================================================================
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/semaphore.h"
#include "core/task_cancelable.h"
#include "core/task_queue.h"
#include "network/pipe_wrap.h"

namespace mzmedia {

class EventPoller : public std::enable_shared_from_this<EventPoller> {
public:
    using Ptr = std::shared_ptr<EventPoller>;
    using Task = TaskQueue::Task;
    using PollEventCB = std::function<void(int event)>;
    using PollCompleteCB = std::function<void(bool success)>;
    /// 定时任务：返回下次延时（毫秒），0 表示不再重复
    using DelayTask = TaskCancelableImp<uint64_t()>;

    enum PollEvent {
        EventRead = 1 << 0,
        EventWrite = 1 << 1,
        EventError = 1 << 2,
        /// 该 fd 使用水平触发（默认 ET）
        EventLT = 1 << 3,
    };

    /**
     * 创建并**立即启动**轮询线程（返回时线程已在 runLoop 中）
     * @param name 线程名（最长 15 字符，超出截断）
     */
    static Ptr create(const std::string &name = "EventPoller");

    /// 当前线程所属的 poller；不在任何 poller 线程上时返回 nullptr
    static Ptr getCurrentPoller();

    ~EventPoller();

    EventPoller(const EventPoller &) = delete;
    EventPoller &operator=(const EventPoller &) = delete;

    // ------------------------------------------------------------------
    // 事件注册（可从任意线程调用）
    // ------------------------------------------------------------------

    /**
     * 注册 fd 的事件监听
     * @param event PollEvent 位或（EventRead | EventWrite | EventLT）
     * @return 0 = 成功；-1 = 失败（fd 非法 / epoll_ctl 失败 / poller 已退出）
     * @note 同一个 fd 重复注册会返回 -1（避免覆盖已有回调）
     */
    int addEvent(int fd, int event, PollEventCB cb);

    /**
     * 注销 fd 的事件监听
     * @param complete_cb 删除完成回调（在轮询线程上调用，参数为是否成功）；可为空
     * @return 0 = 已受理；-1 = fd 未注册 / poller 已退出
     * @note **必须等 complete_cb 或被 sync 确认后再 close(fd)**，否则本轮 epoll
     *       批次里可能还有该 fd 的事件，回调会操作已关闭的 fd
     */
    int delEvent(int fd, PollCompleteCB complete_cb = nullptr);

    /**
     * 修改监听的事件（保留原回调）
     * @return 0 = 成功；-1 = fd 未注册 / poller 已退出
     */
    int modifyEvent(int fd, int event, PollCompleteCB complete_cb = nullptr);

    /// 当前注册的 fd 数量
    size_t fdCount() const { return _fd_count.load(); }

    // ------------------------------------------------------------------
    // 跨线程
    // ------------------------------------------------------------------

    /**
     * 投递任务到轮询线程
     * @param may_sync true 且当前已经在轮询线程时，**直接执行**（保证时序）
     * @return 成功受理返回 true；**poller 已退出或队列已满**时返回 false（调用方必须检查）
     * @note 队列**有上限**（`maxPendingTasks()`，初值 65536 ≈ 3 MB）：满了就拒绝 + 计数，
     *       绝不无限增长。无上限在内存耗尽时会让整个进程死掉，那时所有待执行的
     *       delEvent 一起丢 —— 反而更彻底地破坏 fd 回收。
     *       可观测（pendingTaskCount / asyncRejectedCount）用于**事后校准**，
     *       不能替代上限本身。
     * @note 控制面接口（addEvent/delEvent/modifyEvent）走的是 sync 而不是 async：
     *       队列满时它们会**抛异常**（loud），不会被静默丢弃。
     */
    bool async(Task task, bool may_sync = true);

    /**
     * 投递任务并等待其返回值
     * @return 任务返回值
     * @throws std::runtime_error poller 已退出、无法投递时（不能静默返回默认值）
     */
    template<typename F>
    auto sync(F &&func) -> decltype(func()) {
        using ret_type = decltype(func());
        if (isCurrentThread()) {
            return func();
        }
        auto task = std::make_shared<std::packaged_task<ret_type()>>(std::forward<F>(func));
        auto future = task->get_future();
        if (!async([task]() { (*task)(); })) {
            throw std::runtime_error("EventPoller 已退出，sync 无法投递任务");
        }
        return future.get();
    }

    // ------------------------------------------------------------------
    // 定时器
    // ------------------------------------------------------------------

    /**
     * 延时任务
     * @param delay_ms 首次延时（毫秒）；0 表示下个事件循环周期执行
     * @param task     任务体：返回非 0 表示按该值重复，返回 0 表示结束
     * @return 可取消的任务句柄；**poller 已退出时返回 nullptr 并计数**（调用方必须检查）
     * @note 定时全部基于单调时钟（getCurrentMillisecond），不受系统改时间影响
     */
    DelayTask::Ptr doDelayTask(uint64_t delay_ms, std::function<uint64_t()> task);

    // ------------------------------------------------------------------
    // 观测（全部线程安全）
    // ------------------------------------------------------------------

    bool isCurrentThread() const;
    const std::string &threadName() const { return _name; }
    std::thread::id threadId() const { return _thread_id; }
    bool exiting() const { return _exit.load(); }

    /// 当前定时器数量（观测堆积/泄漏）
    size_t timerCount() const { return _timer_count.load(); }
    /// 排队等待执行的跨线程任务数
    size_t pendingTaskCount() const { return _pending_task_count.load(); }
    /// 任务队列上限（初值 65536，待实测校准）
    size_t maxPendingTasks() const { return _max_pending_tasks.load(); }
    /// 因队列满被拒绝的任务数（背压信号：持续增长说明该调大上限）
    uint64_t asyncRejectedCount() const { return _async_rejected_count.load(); }
    /// 退出时被丢弃的"已受理但未执行"任务数（静默丢弃也算失败，必须可见）
    uint64_t droppedOnExitCount() const { return _dropped_on_exit_count.load(); }

    /**
     * 设置任务队列上限
     * @note 传 0 会被**拒绝**（不允许把上限关掉）：只打 Warn 并保持当前值
     */
    void setMaxPendingTasks(size_t limit);
    /// 因超过 INT_MAX 被截断的 epoll_wait 超时次数（静默降级必须可见）
    uint64_t timeoutClampCount() const { return _timeout_clamp_count.load(); }
    /// 退出时被丢弃的**未触发**定时器数（只统计未被 cancel 的；静默丢弃也算失败）
    uint64_t droppedTimerOnExitCount() const { return _dropped_timer_on_exit_count.load(); }
    /// processDelayTask 处理的**非空**批次数（观测"同刻大量到期"的频度）
    uint64_t delayBatchCount() const { return _delay_batch_count.load(); }
    /// 单批处理定时器数的高水位（观测 0 延时自投链 / 定时器风暴，见 §8 R12）
    size_t delayBatchMax() const { return _delay_batch_max.load(); }
    /// epoll_ctl / epoll_wait 失败次数
    uint64_t epollErrorCount() const { return _epoll_error_count.load(); }
    /// 已退出导致 doDelayTask 被拒绝的次数
    uint64_t rejectedTimerCount() const { return _rejected_timer_count.load(); }

    /**
     * 停止轮询线程并回收资源（幂等）
     * @note 不要在轮询线程内销毁 EventPoller；EventPollerPool 持有的实例不会析构
     */
    void shutdown();

private:
    explicit EventPoller(const std::string &name);
    void start();
    void runLoop();
    void onPipeEvent();
    void runPendingTasks();
    void processDelayTask();
    void collectDeletedEvents();
    void addEventPipe();
    int addEventInLoop(int fd, int event, const PollEventCB &cb);
    int delEventInLoop(int fd, const PollCompleteCB &cb);
    int modifyEventInLoop(int fd, int event, const PollCompleteCB &cb);
    int64_t minDelayInLoop() const;

    const std::string _name;
    int _epoll_fd = -1;
    PipeWrap _pipe;
    std::thread _thread;
    std::thread::id _thread_id;
    Semaphore _sem_started{0};

    std::atomic<bool> _exit{false};
    std::atomic<size_t> _fd_count{0};
    std::atomic<size_t> _timer_count{0};
    std::atomic<size_t> _pending_task_count{0};
    /// 队列上限初值(v0.1)：65536 个任务 ≈ 3 MB（std::function 32B + list 节点 ≈ 48B/任务），
    /// 待实测校准。取值偏松：太小会拒掉合法突发，而 3 MB 的上界可以忽略。
    std::atomic<size_t> _max_pending_tasks{65536};
    std::atomic<uint64_t> _async_rejected_count{0};
    std::atomic<uint64_t> _dropped_on_exit_count{0};
    std::atomic<uint64_t> _timeout_clamp_count{0};
    /// 退出时丢弃的未触发定时器数（只统计未被 cancel 的）
    std::atomic<uint64_t> _dropped_timer_on_exit_count{0};
    /// 非空批次计数 + 单批高水位（观测"同刻到期/自投链"，见 §8 R12）
    std::atomic<uint64_t> _delay_batch_count{0};
    std::atomic<size_t> _delay_batch_max{0};
    std::atomic<uint64_t> _epoll_error_count{0};
    std::atomic<uint64_t> _rejected_timer_count{0};

    /// 跨线程任务队列（_mtx_task 保护）
    std::mutex _mtx_task;
    std::list<Task> _list_task;

    // ---- 以下只在轮询线程访问，因此无需加锁 ----
    std::unordered_map<int, PollEventCB> _event_map;
    /// 延迟删除：本轮事件处理完才真正从 _event_map 移除（见 .cpp 里的说明）
    std::map<int, PollCompleteCB> _deleted_cbs;
    std::multimap<uint64_t, DelayTask::Ptr> _delay_task_map;
};

/**
 * EventPollerPool：N 个 EventPoller（默认 hardware_concurrency()）
 *
 * 用途：多 Reactor —— 每个 poller 一个线程，新连接绑定到其中一个，
 *       之后该连接的读写与定时器全部在同一个线程上，因此无需加锁。
 */
class EventPollerPool {
public:
    static EventPollerPool &Instance();

    /**
     * 设置池大小（**必须在首次 Instance() 之前调用**）
     * @param size 0 = hardware_concurrency()
     * @note 池已创建后再调用会被忽略并打 Warn（不能静默）
     */
    static void setPoolSize(size_t size);

    /**
     * 取一个 poller
     * @param prefer_current true 时若当前线程就是某个 poller 线程，直接返回它
     *                       （避免跨线程投递，这是"连接亲和"的关键）
     */
    EventPoller::Ptr getPoller(bool prefer_current = true);

    EventPoller::Ptr getFirstPoller();
    size_t size() const { return _pollers.size(); }

private:
    EventPollerPool();

    std::vector<EventPoller::Ptr> _pollers;
};

} // namespace mzmedia
