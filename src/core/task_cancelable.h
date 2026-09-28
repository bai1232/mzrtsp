/*
 * TaskCancelable：可取消的任务包装
 * ============================================================================
 * 解决的问题：把一个任务交给别人（定时器、线程池）之后，如何反悔？
 *
 * 取消语义（重要，别误解）：
 *   - cancel() 只保证"**尚未开始执行**的任务不会再被执行"，`operator()` 会直接
 *     返回默认值 R()；
 *   - cancel() **不能中断正在执行的任务**。如果任务需要尽快退出，必须在任务体
 *     内部自己轮询 isCanceled()；
 *   - cancel() 幂等，可重复调用。
 *
 * 用法（M2 的定时器就依赖它）：
 * @code
 *   auto task = std::make_shared<TaskCancelableImp<uint64_t()>>([]() -> uint64_t {
 *       InfoL << "定时器触发";
 *       return 1000;      // 返回下次触发的延时（毫秒），0 表示不再重复
 *   });
 *   task->setDeadline(getCurrentMillisecond() + 1000);
 *   poller->doDelayTask(task);
 *   task->cancel();       // 反悔
 * @endcode
 *
 * 模板参数是**函数签名**，不是分开的返回值与参数：
 *   TaskCancelableImp<void()>、TaskCancelableImp<int(int)>、TaskCancelableImp<uint64_t()>
 * ============================================================================
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace mzmedia {

class TaskCancelable {
public:
    using Ptr = std::shared_ptr<TaskCancelable>;
    virtual ~TaskCancelable() = default;
    virtual void cancel() = 0;
    virtual bool isCanceled() const = 0;
};

template<typename Func>
class TaskCancelableImp;

template<typename R, typename... Args>
class TaskCancelableImp<R(Args...)>
        : public TaskCancelable,
          public std::enable_shared_from_this<TaskCancelableImp<R(Args...)>> {
public:
    using Ptr = std::shared_ptr<TaskCancelableImp>;
    using func_type = std::function<R(Args...)>;

    explicit TaskCancelableImp(func_type fun) : _fun(std::move(fun)) {}

    R operator()(Args... args) {
        if (_canceled.load()) {
            return R();   // R 为 void 时 `return R();` 同样合法
        }
        return _fun(std::forward<Args>(args)...);
    }

    void cancel() override { _canceled.store(true); }

    bool isCanceled() const override { return _canceled.load(); }

    /**
     * 绝对到期时刻（毫秒，基于单调时钟）
     *
     * 仅由定时器读取，用于把任务按到期时间排序；用 atomic 是为了避免
     * "创建线程写、定时器线程读"这种跨线程访问出现数据竞争（TSAN 会报）。
     */
    void setDeadline(uint64_t deadline) { _deadline.store(deadline); }
    uint64_t deadline() const { return _deadline.load(); }

private:
    std::atomic<bool> _canceled{false};
    std::atomic<uint64_t> _deadline{0};
    func_type _fun;
};

} // namespace mzmedia
