/*
 * NoticeCenter：进程内事件总线（发布/订阅）
 * ============================================================================
 * 解决的问题：模块之间不想互相持有指针（例如"媒体源产出了一帧"要通知
 * 多个订阅者，但源不应该知道谁在听）。发布者只管 emitEvent，订阅者自己 addListener。
 *
 * 线程安全与重入（关键设计）：
 *   emitEvent 在锁内**只拷贝监听器列表**，真正的回调在**锁外**执行。因此：
 *     - 回调里可以安全地再次 emitEvent（同一个事件也行）；
 *     - 回调里可以 addListener / delListener，不会自死锁；
 *     - 回调耗时较长也不会阻塞其他线程注册监听器；
 *   代价是"发布瞬间的监听器快照"语义：本次发布中新注册的监听器**不会**被调用。
 *
 * 类型契约（必须遵守）：
 *   事件的参数类型是 addListener 与 emitEvent 之间的**口头约定**，两边写的模板
 *   参数必须完全一致。emitEvent 内部按此类型做 static_cast，类型对不上是未定义行为
 *   （这与 ZLToolKit 的做法一致，属于"用编译期显式类型换运行期零哈希开销"的取舍）。
 *
 * 用法：
 * @code
 *   static int kTag = 0;   // tag 用来批量注销，通常传对象裸指针或静态地址
 *   NoticeCenter::Instance().addListener<int, const std::string &>(
 *       &kTag, "on_event", [](const int code, const std::string &msg) { ... });
 *   NoticeCenter::Instance().emitEvent<int, const std::string &>("on_event", 200, std::string("ok"));
 *   NoticeCenter::Instance().delListener(&kTag);   // 注销该 tag 的所有监听
 * @endcode
 * ============================================================================
 */

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mzmedia {

class NoticeCenter {
public:
    /**
     * 全局单例
     *
     * 与 Logger 相同的单例策略：**故意泄漏**（内部 new 出来不 delete）。
     * 原因：事件可能在其它静态对象/后台线程的析构路径里被发布，若用栈上静态对象，
     * 析构顺序不可控会造成 use-after-free。
     */
    static NoticeCenter &Instance();

    NoticeCenter(const NoticeCenter &) = delete;
    NoticeCenter &operator=(const NoticeCenter &) = delete;

    /**
     * 注册监听器
     * @param tag   注销凭证（通常传对象地址或静态变量地址）；同一个 tag 可注册多个事件
     * @param event 事件名
     * @param fun   回调；**模板参数 Args 必须与 emitEvent 处完全一致**
     */
    template<typename... Args, typename Fun>
    void addListener(void *tag, const std::string &event, Fun &&fun) {
        using listener_type = Listener<Args...>;
        using func_type = typename listener_type::func_type;
        auto listener = std::make_shared<listener_type>(func_type(std::forward<Fun>(fun)));
        std::lock_guard<std::mutex> lck(_mtx);
        _listeners[event].push_back(Entry{tag, listener});
    }

    /**
     * 发布事件（同步调用所有监听器）
     * @note 参数支持左值与临时值：内部按 decay 后的类型以 const 引用接收
     */
    template<typename... Args>
    void emitEvent(const std::string &event, const typename std::decay<Args>::type &...args) {
        const auto listeners = snapshot(event);
        for (const auto &entry : listeners) {
            static_cast<Listener<Args...> *>(entry.listener.get())->call(args...);
        }
    }

    /**
     * 注销监听器
     * @param tag   注册时传入的凭证
     * @param event 为空表示注销该 tag 在**所有事件**上的监听
     */
    void delListener(void *tag, const std::string &event = "");

    /// 指定事件的监听器数量
    size_t listenerCount(const std::string &event) const;

    /// 全部监听器数量
    size_t listenerCount() const;

    /// 清空所有监听器（测试与退出清理用）
    void clear();

private:
    NoticeCenter() = default;
    ~NoticeCenter() = default;

    struct IListener {
        virtual ~IListener() = default;
    };

    template<typename... Args>
    struct Listener : IListener {
        using func_type = std::function<void(const typename std::decay<Args>::type &...)>;

        explicit Listener(func_type fun) : _fun(std::move(fun)) {}

        void call(const typename std::decay<Args>::type &...args) { _fun(args...); }

        func_type _fun;
    };

    struct Entry {
        void *tag;
        std::shared_ptr<IListener> listener;
    };

    /// 在锁内拷贝一份监听器列表，供锁外回调
    std::vector<Entry> snapshot(const std::string &event) const;

    mutable std::mutex _mtx;
    std::unordered_map<std::string, std::vector<Entry>> _listeners;
};

} // namespace mzmedia
