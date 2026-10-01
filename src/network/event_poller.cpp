#include "network/event_poller.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/logger.h"
#include "core/util.h"

namespace mzmedia {

namespace {

constexpr int kMaxEvents = 1024;

/// 把 epoll 事件翻译成 PollEvent 位
int translateEvents(uint32_t events) {
    int ret = 0;
    if (events & EPOLLIN) {
        ret |= EventPoller::EventRead;
    }
    if (events & EPOLLOUT) {
        ret |= EventPoller::EventWrite;
    }
    if (events & (EPOLLERR | EPOLLHUP)) {
        // 同时给出读事件：让回调有机会先把缓冲区读干净再关闭。
        // 错误绝不能静默丢弃，否则上层只能靠超时发现连接已断。
        ret |= EventPoller::EventRead | EventPoller::EventError;
    }
    if (ret == 0) {
        ret = EventPoller::EventError;   // 未知事件类型也不能静默
    }
    return ret;
}

uint32_t toEpollEvents(int event) {
    uint32_t ret = 0;
    if (event & EventPoller::EventRead) {
        ret |= EPOLLIN;
    }
    if (event & EventPoller::EventWrite) {
        ret |= EPOLLOUT;
    }
    // 决策(v0.1)：默认边缘触发（ET）；带 EventLT 时用水平触发
    if (!(event & EventPoller::EventLT)) {
        ret |= EPOLLET;
    }
    return ret;
}

/// 单调时钟毫秒 → epoll_wait 的 int 超时；超范围时 clamp（调用方必须把 clamp 记成可见事件）
int clampTimeout(int64_t ms, bool *clamped) {
    if (ms < 0) {
        return -1;   // 无定时器：无限等待
    }
    if (ms > INT_MAX) {
        if (clamped != nullptr) {
            *clamped = true;
        }
        return INT_MAX;
    }
    return static_cast<int>(ms);
}

/// 当前线程所属的 poller（runLoop 里设置）
thread_local std::weak_ptr<EventPoller> g_current_poller;

std::atomic<bool> g_pool_created{false};
std::atomic<size_t> g_pool_size{0};

} // namespace

// ---------------------------------------------------------------------------
// EventPoller
// ---------------------------------------------------------------------------

EventPoller::Ptr EventPoller::create(const std::string &name) {
    Ptr poller(new EventPoller(name));
    poller->start();
    return poller;
}

EventPoller::Ptr EventPoller::getCurrentPoller() {
    return g_current_poller.lock();
}

EventPoller::EventPoller(const std::string &name) : _name(name) {
    _epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (_epoll_fd < 0) {
        _epoll_error_count.fetch_add(1);
        ErrorP("EventPoller[%s]: epoll_create1 失败 (errno=%d)", _name.c_str(), errno);
    }
    addEventPipe();
}

void EventPoller::start() {
    _thread = std::thread([this]() { runLoop(); });
    _thread_id = _thread.get_id();
    // 等 runLoop 真正进入循环：否则"创建后立刻投递任务"可能丢在启动窗口里
    _sem_started.wait();
}

void EventPoller::addEventPipe() {
    if (!_pipe.valid() || _epoll_fd < 0) {
        _epoll_error_count.fetch_add(1);
        ErrorP("EventPoller[%s]: 唤醒管道不可用，跨线程投递将无法唤醒", _name.c_str());
        return;
    }
    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    // 唤醒管道用**水平触发**（不加 EPOLLET）：只要还有未读数据就会一直报告，容错更好；
    // 每条循环末尾都会 drain()，所以不会忙等。
    ev.events = EPOLLIN;
    ev.data.fd = _pipe.readFD();
    if (::epoll_ctl(_epoll_fd, EPOLL_CTL_ADD, _pipe.readFD(), &ev) != 0) {
        _epoll_error_count.fetch_add(1);
        ErrorP("EventPoller[%s]: 注册唤醒管道失败 (errno=%d)", _name.c_str(), errno);
    }
}

void EventPoller::runLoop() {
    setThreadName(_name.c_str());
    g_current_poller = shared_from_this();
    _sem_started.post();

    std::vector<struct epoll_event> events(kMaxEvents);
    while (!_exit.load()) {
        bool clamped = false;
        const int timeout = clampTimeout(minDelayInLoop(), &clamped);
        if (clamped) {
            // §4.5：静默降级也算失败 —— 截断必须计数 + 告警
            _timeout_clamp_count.fetch_add(1);
            WarnP("EventPoller[%s]: 定时器延时超过 INT_MAX ms，epoll_wait 超时被截断为 %d ms",
                  _name.c_str(), INT_MAX);
        }

        const int n = ::epoll_wait(_epoll_fd, events.data(), kMaxEvents, timeout);
        if (n < 0) {
            if (errno == EINTR) {
                continue;   // 被信号打断：不是错误
            }
            _epoll_error_count.fetch_add(1);
            ErrorP("EventPoller[%s]: epoll_wait 失败 (errno=%d)，退出事件循环", _name.c_str(), errno);
            break;          // 事件循环坏了：明确退出，不静默空转
        }

        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            if (fd == _pipe.readFD()) {
                onPipeEvent();
                continue;
            }
            // 本批次内已删除的 fd：跳过（它可能已被 close，不能再碰）
            if (_deleted_cbs.count(fd) != 0) {
                continue;
            }
            auto it = _event_map.find(fd);
            if (it == _event_map.end()) {
                continue;   // 已注销：正常情况（延迟删除的残留）
            }
            const PollEventCB cb = it->second;   // 拷贝一份：回调里可能 delEvent
            if (cb) {
                cb(translateEvents(events[i].events));
            }
        }

        collectDeletedEvents();
        processDelayTask();
        // 兜底：本轮任务里新投递的（管道是 LT，下一轮 epoll_wait 也会立刻返回）
        runPendingTasks();
        // ★ 必须放在最后：跨线程的 delEvent 是通过"任务"执行的，
        //   若在这里之前清理，本轮登记的待删除项就要等到下一轮才生效 ——
        //   而下一轮可能因为管道已读空而长时间阻塞在 epoll_wait 上，
        //   导致 complete_cb 迟迟不回调（调用方会以为删除失败）。
        collectDeletedEvents();
    }

    // 退出清理：定时器与事件都不再触发
    _event_map.clear();
    _deleted_cbs.clear();
    _delay_task_map.clear();
    _timer_count.store(0);
    _fd_count.store(0);
    {
        // 退出时被丢弃的"已受理但未执行"任务：静默丢弃也是失败（§4.5），
        // 因此计数 + Warn，让它可被发现
        std::lock_guard<std::mutex> lck(_mtx_task);
        if (!_list_task.empty()) {
            _dropped_on_exit_count.fetch_add(_list_task.size());
            WarnP("EventPoller[%s]: 退出时丢弃了 %zu 个已受理但未执行的任务",
                  _name.c_str(), _list_task.size());
            _list_task.clear();
            _pending_task_count.store(0);
        }
    }
}

void EventPoller::onPipeEvent() {
    _pipe.drain();
    runPendingTasks();
}

void EventPoller::runPendingTasks() {
    std::list<Task> todo;
    {
        std::lock_guard<std::mutex> lck(_mtx_task);
        if (_list_task.empty()) {
            return;
        }
        todo.swap(_list_task);
    }
    _pending_task_count.fetch_sub(todo.size());

    for (auto &task : todo) {
        try {
            task();
        } catch (const std::exception &e) {
            // 任务异常不能弄死事件循环（否则整个服务停摆）
            ErrorP("EventPoller[%s]: 投递的任务抛异常（已忽略）: %s", _name.c_str(), e.what());
        } catch (...) {
            ErrorP("EventPoller[%s]: 投递的任务抛出未知异常（已忽略）", _name.c_str());
        }
    }
}

void EventPoller::collectDeletedEvents() {
    if (_deleted_cbs.empty()) {
        return;
    }
    auto cbs = std::move(_deleted_cbs);
    _deleted_cbs.clear();
    for (const auto &item : cbs) {
        _event_map.erase(item.first);
        if (item.second) {
            item.second(true);
        }
    }
}

void EventPoller::processDelayTask() {
    while (!_delay_task_map.empty()) {
        auto it = _delay_task_map.begin();
        const uint64_t now = getCurrentMillisecond();   // 每轮重取：任务可能执行很久
        if (it->first > now) {
            break;
        }
        DelayTask::Ptr task = it->second;
        _delay_task_map.erase(it);
        _timer_count.fetch_sub(1);
        if (!task || task->isCanceled()) {
            continue;   // 取消的任务：静默跳过是正确行为，不是失败
        }
        uint64_t next_delay = 0;
        try {
            next_delay = (*task)();
        } catch (const std::exception &e) {
            ErrorP("EventPoller[%s]: 定时任务抛异常（不再重复）: %s", _name.c_str(), e.what());
            next_delay = 0;
        } catch (...) {
            ErrorP("EventPoller[%s]: 定时任务抛出未知异常（不再重复）", _name.c_str());
            next_delay = 0;
        }
        if (next_delay > 0 && !task->isCanceled() && !_exit.load()) {
            task->setDeadline(getCurrentMillisecond() + next_delay);
            _delay_task_map.emplace(task->deadline(), task);
            _timer_count.fetch_add(1);
        }
    }
}

int64_t EventPoller::minDelayInLoop() const {
    if (_delay_task_map.empty()) {
        return -1;
    }
    const uint64_t now = getCurrentMillisecond();
    const uint64_t next = _delay_task_map.begin()->first;
    return next <= now ? 0 : static_cast<int64_t>(next - now);
}

int EventPoller::addEvent(int fd, int event, PollEventCB cb) {
    if (fd < 0) {
        _epoll_error_count.fetch_add(1);
        ErrorP("EventPoller[%s]: addEvent 收到非法 fd=%d", _name.c_str(), fd);
        return -1;
    }
    if (!cb) {
        _epoll_error_count.fetch_add(1);
        ErrorP("EventPoller[%s]: addEvent fd=%d 的回调为空，已拒绝", _name.c_str(), fd);
        return -1;
    }
    if (isCurrentThread()) {
        return addEventInLoop(fd, event, cb);
    }
    if (_exit.load()) {
        _epoll_error_count.fetch_add(1);
        ErrorP("EventPoller[%s]: 已退出，addEvent fd=%d 被拒绝", _name.c_str(), fd);
        return -1;
    }
    int ret = -1;
    sync([this, fd, event, &cb, &ret]() { ret = addEventInLoop(fd, event, cb); });
    return ret;
}

int EventPoller::addEventInLoop(int fd, int event, const PollEventCB &cb) {
    if (_event_map.count(fd) != 0) {
        _epoll_error_count.fetch_add(1);
        ErrorP("EventPoller[%s]: fd=%d 已注册，拒绝重复添加（避免覆盖已有回调）", _name.c_str(), fd);
        return -1;
    }
    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = toEpollEvents(event);
    ev.data.fd = fd;
    if (::epoll_ctl(_epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        _epoll_error_count.fetch_add(1);
        ErrorP("EventPoller[%s]: epoll_ctl(ADD, fd=%d) 失败 (errno=%d)", _name.c_str(), fd, errno);
        return -1;
    }
    _event_map.emplace(fd, cb);
    _fd_count.fetch_add(1);
    return 0;
}

int EventPoller::delEvent(int fd, PollCompleteCB complete_cb) {
    if (fd < 0) {
        _epoll_error_count.fetch_add(1);
        ErrorP("EventPoller[%s]: delEvent 收到非法 fd=%d", _name.c_str(), fd);
        return -1;
    }
    if (isCurrentThread()) {
        return delEventInLoop(fd, complete_cb);
    }
    if (_exit.load()) {
        _epoll_error_count.fetch_add(1);
        ErrorP("EventPoller[%s]: 已退出，delEvent fd=%d 被拒绝", _name.c_str(), fd);
        return -1;
    }
    int ret = -1;
    sync([this, fd, &complete_cb, &ret]() { ret = delEventInLoop(fd, complete_cb); });
    return ret;
}

int EventPoller::delEventInLoop(int fd, const PollCompleteCB &cb) {
    if (_deleted_cbs.count(fd) != 0) {
        // 已在本轮待删集合里：不能再减一次计数，否则 _fd_count 会下溢
        ErrorP("EventPoller[%s]: fd=%d 已在本轮待删除集合中，重复 delEvent 被拒绝",
               _name.c_str(), fd);
        if (cb) {
            cb(false);
        }
        return -1;
    }
    auto it = _event_map.find(fd);
    if (it == _event_map.end()) {
        if (cb) {
            cb(false);
        }
        return -1;
    }
    // 先把 fd 从 epoll 摘掉：保证之后不再产生它的新事件
    if (::epoll_ctl(_epoll_fd, EPOLL_CTL_DEL, fd, nullptr) != 0) {
        // EBADF（fd 已被 close）或 ENOENT：不是致命错误，但必须可见
        _epoll_error_count.fetch_add(1);
        WarnP("EventPoller[%s]: epoll_ctl(DEL, fd=%d) 失败 (errno=%d)", _name.c_str(), fd, errno);
    }
    // 延迟删除：本轮 epoll 批次里可能还有该 fd 的事件，立即 erase 会让**
    // 正在执行的回调对象被析构** → use-after-free。因此只登记，循环末尾统一清。
    _deleted_cbs.emplace(fd, cb);
    _fd_count.fetch_sub(1);
    return 0;
}

int EventPoller::modifyEvent(int fd, int event, PollCompleteCB complete_cb) {
    if (fd < 0) {
        _epoll_error_count.fetch_add(1);
        ErrorP("EventPoller[%s]: modifyEvent 收到非法 fd=%d", _name.c_str(), fd);
        return -1;
    }
    if (isCurrentThread()) {
        return modifyEventInLoop(fd, event, complete_cb);
    }
    if (_exit.load()) {
        _epoll_error_count.fetch_add(1);
        ErrorP("EventPoller[%s]: 已退出，modifyEvent fd=%d 被拒绝", _name.c_str(), fd);
        return -1;
    }
    int ret = -1;
    sync([this, fd, event, &complete_cb, &ret]() {
        ret = modifyEventInLoop(fd, event, complete_cb);
    });
    return ret;
}

int EventPoller::modifyEventInLoop(int fd, int event, const PollCompleteCB &cb) {
    if (_deleted_cbs.count(fd) != 0) {
        if (cb) {
            cb(false);
        }
        return -1;
    }
    auto it = _event_map.find(fd);
    if (it == _event_map.end()) {
        if (cb) {
            cb(false);
        }
        return -1;
    }
    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = toEpollEvents(event);
    ev.data.fd = fd;
    if (::epoll_ctl(_epoll_fd, EPOLL_CTL_MOD, fd, &ev) != 0) {
        _epoll_error_count.fetch_add(1);
        ErrorP("EventPoller[%s]: epoll_ctl(MOD, fd=%d) 失败 (errno=%d)", _name.c_str(), fd, errno);
        if (cb) {
            cb(false);
        }
        return -1;
    }
    if (cb) {
        cb(true);
    }
    return 0;
}

bool EventPoller::async(Task task, bool may_sync) {
    if (!task) {
        ErrorP("EventPoller[%s]: async 收到空任务，已拒绝", _name.c_str());
        return false;
    }
    if (may_sync && isCurrentThread()) {
        task();   // 已在轮询线程：直接执行，保证时序
        return true;
    }
    {
        std::lock_guard<std::mutex> lck(_mtx_task);
        if (_exit.load()) {
            return false;   // 已退出：明确拒绝，调用方必须检查
        }
        // 队列有上限：满了拒绝 + 计数（不阻塞、不静默丢）。
        // 判定必须在锁内、emplace 之前，否则判定与插入之间的窗口会让队列超限。
        const size_t limit = _max_pending_tasks.load();
        if (_list_task.size() >= limit) {
            _async_rejected_count.fetch_add(1);
            // 每次拒绝都打 Warn：拒绝虽已由返回值与计数体现，但"谁在猛投"需要日志线索。
            // 计数持续增长才是"该调大上限"的信号。
            WarnP("EventPoller[%s]: 任务队列已满（上限 %zu），本次投递被拒绝", _name.c_str(), limit);
            return false;
        }
        _list_task.emplace_back(std::move(task));
        _pending_task_count.fetch_add(1);
    }
    _pipe.notify();
    return true;
}

void EventPoller::setMaxPendingTasks(size_t limit) {
    if (limit == 0) {
        // 不允许把上限关掉：无上限在内存耗尽时会让整个进程死掉，
        // 那时连待执行的 delEvent 也一起丢 —— 比"拒绝一个任务"糟得多
        WarnP("EventPoller[%s]: setMaxPendingTasks(0) 被拒绝（不允许无上限），保持 %zu",
              _name.c_str(), _max_pending_tasks.load());
        return;
    }
    _max_pending_tasks.store(limit);
}

EventPoller::DelayTask::Ptr EventPoller::doDelayTask(uint64_t delay_ms, std::function<uint64_t()> task) {
    if (!task) {
        _rejected_timer_count.fetch_add(1);
        ErrorP("EventPoller[%s]: doDelayTask 收到空任务，已拒绝", _name.c_str());
        return nullptr;
    }
    auto ret = std::make_shared<DelayTask>(std::move(task));
    ret->setDeadline(getCurrentMillisecond() + delay_ms);

    if (isCurrentThread()) {
        _delay_task_map.emplace(ret->deadline(), ret);
        _timer_count.fetch_add(1);
        return ret;
    }
    if (!async([this, ret]() {
            _delay_task_map.emplace(ret->deadline(), ret);
            _timer_count.fetch_add(1);
        })) {
        _rejected_timer_count.fetch_add(1);
        ErrorP("EventPoller[%s]: poller 已退出，doDelayTask 被拒绝", _name.c_str());
        return nullptr;   // 明确失败：调用方必须检查，不能以为定时器已生效
    }
    return ret;
}

bool EventPoller::isCurrentThread() const {
    return std::this_thread::get_id() == _thread_id;
}

void EventPoller::shutdown() {
    if (_exit.exchange(true)) {
        return;   // 幂等
    }
    _pipe.notify();
    if (_thread.joinable() && !isCurrentThread()) {
        _thread.join();
    }
    // 若在轮询线程内调用 shutdown：不能 join 自己，交给析构函数处理
}

EventPoller::~EventPoller() {
    shutdown();
    if (_thread.joinable()) {
        if (isCurrentThread()) {
            // 在轮询线程内销毁：detach 会留下 use-after-free 风险，属于调用方用法错误
            ErrorP("EventPoller[%s]: 在轮询线程内销毁，只能 detach（请改用 EventPollerPool）",
                   _name.c_str());
            _thread.detach();
        } else {
            _thread.join();
        }
    }
    if (_epoll_fd >= 0) {
        ::close(_epoll_fd);
        _epoll_fd = -1;
    }
}

// ---------------------------------------------------------------------------
// EventPollerPool
// ---------------------------------------------------------------------------

EventPollerPool &EventPollerPool::Instance() {
    // 故意泄漏：与 Logger / NoticeCenter 相同的策略，避免静态析构顺序问题
    static EventPollerPool *instance = new EventPollerPool();
    return *instance;
}

EventPollerPool::EventPollerPool() {
    size_t count = g_pool_size.load();
    if (count == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        count = hw == 0 ? 1 : static_cast<size_t>(hw);
    }
    _pollers.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        _pollers.emplace_back(EventPoller::create(strFormat("mzmedia-poll-%zu", i)));
    }
    g_pool_created.store(true);
    InfoP("EventPollerPool 已创建：%zu 个轮询线程", _pollers.size());
}

void EventPollerPool::setPoolSize(size_t size) {
    if (g_pool_created.load()) {
        // 池是"首次使用时创建"的单例：创建后再设置无效，必须让人看见
        WarnP("EventPollerPool::setPoolSize(%zu) 被忽略：池已创建", size);
        return;
    }
    g_pool_size.store(size);
}

EventPoller::Ptr EventPollerPool::getPoller(bool prefer_current) {
    if (prefer_current) {
        // 当前线程就是某个 poller 线程时直接返回它：避免跨线程投递（连接亲和的关键）
        if (auto current = EventPoller::getCurrentPoller()) {
            return current;
        }
    }
    EventPoller::Ptr best;
    size_t min_fd = static_cast<size_t>(-1);
    for (const auto &poller : _pollers) {
        const size_t count = poller->fdCount();
        if (count < min_fd) {
            min_fd = count;
            best = poller;
        }
    }
    return best;
}

EventPoller::Ptr EventPollerPool::getFirstPoller() {
    return _pollers.empty() ? nullptr : _pollers.front();
}

} // namespace mzmedia
