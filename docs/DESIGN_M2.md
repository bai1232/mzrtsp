# M2 设计：网络层（Network）

> 配套：[SPEC.md](SPEC.md)（需求）· [ARCHITECTURE.md](ARCHITECTURE.md)（整体架构）· [TESTING.md](TESTING.md)（验证）· [ROADMAP.md](ROADMAP.md)（任务分解）
> 本文件是 M2 的**设计记录**：形状（返回值 / 错误语义 / 线程模型）在这一层定死，实现细节留给编码阶段。

## 1. 范围

| 交付 | 不交付（留后续） |
|---|---|
| `PipeWrap`、`SockException`、`Buffer`、`Socket` | HTTP 解析与路由（M3） |
| `EventPoller`（epoll，ET 默认 + LT 可切）、`EventPollerPool` | FFmpeg 相关（M4） |
| 定时器（`doDelayTask`） | `TcpClient`（M3 回源 / RTSP 输入时才需要） |
| `Session`（含空闲检测与心跳钩子）、`TcpServer` | UDP（v0.2 视需要） |
| `examples/echo_server.cpp` + `scripts/echo_test.sh` | TLS（明确不做） |

**验收标准**（对应 `docs/ROADMAP.md:12`）：`echo` 示例用 `nc` 回显 100MB 无错；定时器精度 ±5ms；**空闲超时按 FR-4.4 触发，断开后 fd 回落（NFR-6）**。

## 2. 分层位置与依赖方向

```
        App
         │
        Http          Stats
         │              ▲
      Output            │（被各层写入）
         │
       Media
         │
   ┌─────┴─────┐
 Network   FFmpeg 封装        ← 两者并列，互不依赖，在 Media 处汇合
   └─────┬─────┘
       Core
```

**M2 的依赖约束（硬）**：`network/` 只依赖 `core/`；`core/` **不得**反向依赖 `network/`。

```bash
# 违规检查（应为空）
grep -rn 'include "network/' src/core/
```

> ⚠️ `docs/ARCHITECTURE.md` §1 的分层图与 `src/mzmedia.h` 的注释**目前互相矛盾**（对 Http 的相对位置判断相反，且都把 FFmpeg 封装层画在 Network 之上，暗示它依赖网络 —— 这是错的）。本图是修正后的版本；同步那两处已列入"未决事项"（§10）。

## 3. 组件与接口

### 3.1 `PipeWrap`（跨线程唤醒）

```cpp
class PipeWrap {
public:
    PipeWrap();                 // pipe2(O_NONBLOCK | O_CLOEXEC)，失败则 fd 为 -1
    ~PipeWrap();                // close 两个 fd
    PipeWrap(const PipeWrap &) = delete;
    PipeWrap &operator=(const PipeWrap &) = delete;

    int writeFD() const;
    int readFD() const;
    /// 写 1 字节唤醒对端；返回写入字节数，-1 表示出错（EAGAIN 已被吞掉，不视为错误）
    int notify();
    /// 把管道里已积累的字节读空（必须读空，否则 ET 模式下不再有事件）
    void drain();
private:
    int _fds[2] = {-1, -1};
};
```

**为什么用 `pipe2` 而不是 `eventfd`**：`eventfd` 更省一个 fd，但管道能顺便携带负载（未来若要传递任务 id 不必再改结构）。代价是多占 1 个 fd。
**为什么不用条件变量**：投递任务是"跨线程 + 需要唤醒阻塞在 `epoll_wait` 的线程"，条件变量唤不醒 `epoll_wait`。

### 3.2 `SockException`（错误模型）

```cpp
class SockException : public std::runtime_error {
public:
    SockException(int err = 0, const std::string &msg = "");
    int errCode() const;      // errno；0 表示无 errno（例如自定义错误）
    bool isEof() const;       // 对端正常关闭（read 返回 0）
};
```

**约定**：`EAGAIN` / `EWOULDBLOCK` / `EINTR` **不是错误**，不得构造成 `SockException` 往上抛；它们由调用点直接处理（重试或等待下次事件）。

### 3.3 `Buffer`（线性可扩容）

接口见下（形状已定死）：

```cpp
class Buffer {
public:
    using Ptr = std::shared_ptr<Buffer>;
    static constexpr size_t npos = static_cast<size_t>(-1);

    explicit Buffer(size_t initial_capacity = 0);
    Buffer(const Buffer &) = delete;
    Buffer(Buffer &&) = default;

    const char *data() const;
    size_t size() const;
    size_t capacity() const;
    bool empty() const;

    void append(const void *data, size_t len);
    void append(const std::string &str);
    void reserve(size_t capacity);

    void consume(size_t len);   // len 超长时按 size() 截断
    void clear();               // 数据清空、容量保留
    void release();             // 容量释放

    size_t find(char ch, size_t from = 0) const;                          // 返回偏移
    size_t find(const void *needle, size_t needle_len, size_t from = 0) const;
    bool startWith(const void *prefix, size_t len) const;
    bool endWith(const void *suffix, size_t len) const;

    /// 循环 read() 直到 EAGAIN —— ET 模式下必须一次读空
    /// @return >0 新增字节数；0 = 对端关闭(EOF)；-1 = 出错（*err 带 errno）
    ssize_t readFromFd(int fd, int *err = nullptr);

    /// 尽量写出可读区；部分写时消费已写部分
    /// @return >0 本次写出字节数（可能 < size()）；-1 = 出错（*err 带 errno）
    ssize_t writeToFd(int fd, int *err = nullptr);

    std::string toString() const;
};
```

**不变量**：`0 ≤ _read_pos ≤ _write_pos ≤ _buf.size()`，`size() == _write_pos - _read_pos`。

| 决策 | 做法 | 为什么 |
|---|---|---|
| 扩容 | 空间不足时先 **compact**（剩余数据搬到头部），仍不够再 `resize(max(need, capacity*2))` | 否则"读游标一直前移、容量只增不减" |
| 清空 | 游标归零、**保留容量** | 长连接反复收发不要反复 malloc |
| `find` 返回偏移 | 不返回指针 | 扩容后 `data()` 会失效，偏移量天然安全 |
| 上限 | **Buffer 不设上限**，上限策略在 `Session`（见 §5.4） | 容器职责单一；"多大算攻击"是协议层策略 |

### 3.4 `Socket`（fd RAII + 系统调用封装）

```cpp
class Socket {
public:
    using Ptr = std::shared_ptr<Socket>;
    enum class Domain { IPv4 = AF_INET, IPv6 = AF_INET6 };

    explicit Socket(int fd = -1);
    ~Socket();                                   // close(fd)
    Socket(const Socket &) = delete;
    Socket(Socket &&) noexcept;

    int rawFD() const;
    bool valid() const;
    void close();

    // 全部返回 bool：失败已记日志，调用方按需处理（禁止静默失败）
    bool setNonBlock(bool enable);
    bool setNoDelay(bool enable);                // TCP_NODELAY
    bool setReuseAddr(bool enable);
    bool setKeepAlive(bool enable);
    bool setKeepAliveParams(int idle_sec, int interval_sec, int count);   // Linux TCP_KEEP*
    bool setSendBufSize(int bytes);
    bool setRecvBufSize(int bytes);

    bool bind(const std::string &ip, uint16_t port);
    bool listen(int backlog);
    /// 返回新连接的 fd；-1 = 失败（*err 带 errno，EMFILE/ENFILE 会单独计数）
    int accept(std::string *peer_ip = nullptr, uint16_t *peer_port = nullptr);

    /// @return >0 已发送字节数；-1 = 出错（*err 带 errno，EAGAIN 需调用方区分）
    ssize_t send(const void *data, size_t len, int flags = 0);
    ssize_t recv(void *buf, size_t len, int flags = 0);

    std::string localIP() const;
    uint16_t localPort() const;
    std::string peerIP() const;
    uint16_t peerPort() const;

    static Socket::Ptr create(Domain domain, int type = SOCK_STREAM);
    static bool isEagain(int err);               // EAGAIN / EWOULDBLOCK / EINTR
};
```

### 3.5 `EventPoller`（epoll 事件循环 + 定时器 + 跨线程投递）

```cpp
class EventPoller : public std::enable_shared_from_this<EventPoller> {
public:
    using Ptr = std::shared_ptr<EventPoller>;
    using Task = TaskQueue::Task;
    using PollEventCB = std::function<void(int event)>;
    using DelayTask = TaskCancelableImp<uint64_t()>;      // 返回下次延时，0 = 不再重复

    enum PollEvent {
        EventRead   = 1 << 0,
        EventWrite  = 1 << 1,
        EventError  = 1 << 2,
        EventLT     = 1 << 3,       // 该 fd 使用水平触发（默认 ET）
    };

    /// 创建并**立即启动**轮询线程（构造完成后线程已进入 runLoop）
    static Ptr create(const std::string &name = "EventPoller");
    /// 当前线程所属的 poller；不在 poller 线程上时返回 nullptr
    static Ptr getCurrentPoller();
    ~EventPoller();                 // 自动 shutdown

    // ---- 事件注册（0 = 成功；-1 = 失败并记日志）----
    int addEvent(int fd, int event, PollEventCB cb);
    /// 可在回调内部调用（延迟删除，见 §4.1）
    int delEvent(int fd, std::function<void(bool)> complete_cb = nullptr);
    int modifyEvent(int fd, int event, std::function<void(bool)> complete_cb = nullptr);
    size_t fdCount() const;

    // ---- 跨线程 ----
    /// 投递任务；若当前已在轮询线程且 may_sync=true，则**直接执行**（保证时序）
    bool async(Task task, bool may_sync = true);
    /// 同步取值：同线程内联执行（避免自等死锁）；异常会传播到调用方
    template<typename F> auto sync(F &&func) -> decltype(func());

    // ---- 定时器 ----
    /// @param delay_ms 首次延时；任务返回非 0 则按该值重复
    DelayTask::Ptr doDelayTask(uint64_t delay_ms, std::function<uint64_t()> task);

    // ---- 观测 ----
    bool isCurrentThread() const;
    const std::string &threadName() const;
    std::thread::id threadId() const;
    size_t timerCount() const;
    int64_t getMinDelay() const;    // -1 = 无定时器（无限等待）

    void shutdown();                // 幂等
};

class EventPollerPool {
public:
    static EventPollerPool &Instance();
    /// 必须在首次 Instance() 之前调用；0 = hardware_concurrency()
    static void setPoolSize(size_t size);
    EventPoller::Ptr getFirstPoller();
    /// prefer_current=true 时，若当前线程就是某个 poller 线程则返回它（避免跨线程投递）
    EventPoller::Ptr getPoller(bool prefer_current = true);
    size_t size() const;
};
```

**形状约定（定死，不后改）**

| 项 | 约定 |
|---|---|
| 失败返回 | `addEvent/modifyEvent/delEvent` 返回 `int`（0/-1）；`async` 返回 `bool`（**队列满或已退出时为 false**，调用方必须检查） |
| 队列上限 | `EventPoller` 的任务队列**有上限**（初值 65536 ≈ 3 MB，`setMaxPendingTasks()` 可调，**传 0 被拒绝**）；满了 **拒绝 + `asyncRejectedCount()` + Warn**。控制面接口走 `sync`，满时抛异常而非静默丢 |
| 错误报告 | 内部失败一律**记日志**（含 fd 与 errno）；调用方不需要解析 errno |
| 线程 | `addEvent/modifyEvent/delEvent` 可从任意线程调（内部投递）；`async(may_sync=true)` 在同线程直接执行 |
| `delEvent` 的 `complete_cb` | 参数是 `bool success`；**在 poller 线程上、删除完成后**调用（回调内不得再 delayed-delete 同一 fd） |
| 定时器线程安全 | `doDelayTask` 可从任意线程调；`DelayTask::cancel()` 线程安全（`atomic`） |

### 3.6 `Session`（连接生命周期 + 收发）

```cpp
class Session : public std::enable_shared_from_this<Session> {
public:
    using Ptr = std::shared_ptr<Session>;
    using onReadCB  = std::function<void(const Buffer::Ptr &buf)>;
    using onErrorCB = std::function<void(const SockException &err)>;
    using onManagerCB = std::function<void(const Session::Ptr &session, bool is_add)>;

    Session(const Socket::Ptr &sock, const EventPoller::Ptr &poller);
    virtual ~Session();

    void setOnRead(onReadCB cb);
    void setOnError(onErrorCB cb);
    void setOnManager(onManagerCB cb);

    /// 线程安全：非 poller 线程会经 sync() 投递到 poller 线程执行
    /// @return 立即写出的字节数；数据未写完时余下进发送队列（不是失败）
    virtual ssize_t send(const void *data, size_t len);
    virtual ssize_t send(const std::string &data);

    /// 主动关闭（线程安全）；幂等
    void shutdown();

    // ---- FR-4.4：空闲检测 ----
    /// 0 = 关闭该方向检测。默认值由 TcpServer::setSessionTimeout 注入
    void setRecvIdleTimeout(uint32_t ms);
    void setSendBlockedTimeout(uint32_t ms);
    uint32_t recvIdleTimeout() const;
    uint32_t sendBlockedTimeout() const;

    /// 接收缓冲上限（安全网，不是协议上限）：超限 → onError + 计数 + 关闭
    void setMaxRecvBuffer(size_t bytes);          // 默认 1 MB（初值，待实测校准）

    // ---- 观测 ----
    const EventPoller::Ptr &poller() const;
    Socket::Ptr socket() const;
    std::string peerIP() const;
    uint16_t peerPort() const;
    uint64_t bytesIn() const;
    uint64_t bytesOut() const;
    uint64_t lastRecvMs() const;      // 距上次收到数据的毫秒数
    uint64_t lastSendMs() const;

protected:
    /// 子类重写做分帧；buf 里可能是半条消息，也可能含多条 —— 自己 find + consume
    virtual void onRecv(const Buffer::Ptr &buf);
    virtual void onError(const SockException &err);
    /// 空闲超时回调：**默认实现是关闭连接**；协议层重写它就能变成"发心跳"
    virtual void onIdle(uint32_t idle_ms);

private:
    void attachEvent();
    void onReadEvent(int event);
    void emitError(const SockException &err);
    void updateEpollOut(bool enable);
    void startIdleChecker();
    // ... 成员见 §4.6
};
```

### 3.7 `TcpServer`

```cpp
class TcpServer {
public:
    using Ptr = std::shared_ptr<TcpServer>;
    /// 工厂在 **poller 线程**上执行，因此内部可用 EventPoller::getCurrentPoller()
    using SessionCreator = std::function<Session::Ptr(const Socket::Ptr &)>;

    explicit TcpServer(const EventPoller::Ptr &poller = nullptr);   // 空 = 从 Pool 取
    ~TcpServer();

    /// @param port 0 = 由内核分配（随后用 port() 读回，测试友好）
    /// @return bind/listen 是否成功（失败已记日志，不抛异常）
    bool start(uint16_t port, const std::string &bind_ip = "0.0.0.0");
    void shutdown();                                     // 幂等

    // 配置（必须在 start() 之前）
    void setSessionCreator(SessionCreator creator);
    void setBacklog(int backlog);                        // 默认 1024
    void setMaxSessionCount(size_t max);                 // 默认 0 = 不限（FR-4.4 的"连接上限"）
    void setSessionTimeout(uint32_t recv_idle_ms, uint32_t send_blocked_ms);   // FR-4.4
    void setReusePort(bool enable);

    // 观测
    uint16_t port() const;
    bool listening() const;
    size_t sessionCount() const;
    uint64_t totalAccepted() const;
    uint64_t totalRejected() const;      // FR-4.4：因连接数上限被拒
    uint64_t totalIdleTimeout() const;   // FR-4.4：因空闲/写阻塞超时被断开
    uint64_t totalRecvOverflow() const;  // 接收缓冲超限被断开
    uint64_t totalAcceptError() const;   // accept 失败（如 EMFILE）
    const EventPoller::Ptr &poller() const;

    void setOnSessionClose(std::function<void(const Session::Ptr &)> cb);
    void forEachSession(const std::function<void(const Session::Ptr &)> &cb) const;
};
```

## 4. 关键机制

### 4.1 事件循环 + 延迟删除

```cpp
while (!_exit) {
    const int timeout = clampToInt(getMinDelay());     // -1 = 无限等待
    const int n = epoll_wait(_epoll_fd, events, kMaxEvents, timeout);
    if (n < 0) { if (errno == EINTR) continue; ErrorP(...); break; }

    for (int i = 0; i < n; ++i) {
        if (events[i].data.fd == _pipe.readFD()) { onPipeEvent(); continue; }

        auto it = _event_map.find(events[i].data.fd);
        if (it == _event_map.end()) continue;

        // 拷贝一份 shared_ptr：回调里可能 delEvent，只有拷贝能保证回调对象
        // 在本次调用期间不被析构
        auto cb = it->second;
        (*cb)(translate(events[i].events));
    }

    // 延迟删除：回调栈上直接 erase 会让回调对象析构 → Session 在回调执行中
    // 被销毁 → use-after-free。所以本线程内的 delEvent 只登记，循环末尾统一清
    if (!_expired_fds.empty()) { /* erase 并回调 complete_cb(true) */ }

    flushDelayTask(getCurrentMillisecond());
    onPipeEvent();      // 处理跨线程投递的任务
}
```

**触发模式**：注册 fd 时默认 `EPOLLET`；`event & EventLT` 时去掉 `EPOLLET` 并用水平触发。

### 4.2 跨线程投递：`async` / `sync`

```cpp
bool async(Task task, bool may_sync) {
    if (may_sync && isCurrentThread()) { task(); return true; }   // 同线程直接执行，保序
    { std::lock_guard lck(_mtx_task); _list_task.push_back(std::move(task)); }
    _pipe.notify();                                               // 唤醒 epoll_wait
    return true;
}
```

`sync` 用 `std::packaged_task` + `std::future`；若已在 poller 线程则**内联执行**（否则自等 → 死锁）。

### 4.3 定时器：事件线程内零锁

`std::multimap<uint64_t /*绝对到期毫秒*/, DelayTask::Ptr>`。插入通过 `async` 投递到 poller 线程 → **整个 map 只被一个线程访问，无需任何锁**（代价是多一次唤醒）。

```cpp
DelayTask::Ptr doDelayTask(uint64_t delay_ms, std::function<uint64_t()> task) {
    auto ret = std::make_shared<DelayTask>(std::move(task));
    ret->setDeadline(getCurrentMillisecond() + delay_ms);
    async([this, ret] { _delay_task_map.emplace(ret->deadline(), ret); });
    return ret;
}
// flush：取出到期项 → 先判 isCanceled() → 执行 → 返回非 0 则按新 deadline 重新入堆
```

**时钟**：全部用 `getCurrentMillisecond()`（单调时钟）。**绝不使用墙钟** —— NTP 校时会让定时器乱序。

**溢出边界**：`epoll_wait` 的 timeout 是 `int` 毫秒 → 超过 `INT_MAX` 时 clamp；定时器本身用 `uint64_t`。

### 4.4 读路径：ET 必须读到 `EAGAIN`

```cpp
void Session::onReadEvent(int) {
    while (true) {
        int err = 0;
        const ssize_t n = _read_buffer->readFromFd(_sock->rawFD(), &err);
        if (n > 0) {
            _last_recv_ms.store(getCurrentMillisecond());        // FR-4.4 活跃时间
            _bytes_in += n;
            if (_read_buffer->size() > _max_recv_buffer) {        // §5.4 安全网
                emitError(SockException(Err_buffer_overflow, "接收缓冲超限"));
                return;
            }
            onRecv(_read_buffer);                                // 交子类分帧（自己 consume）
            continue;
        }
        if (Socket::isEagain(err)) return;                       // 读空：正常退出
        if (n == 0) { emitError(SockException(Err_eof, "对端关闭")); return; }
        emitError(SockException(err));
        return;
    }
}
```

### 4.5 写路径：发送队列 + `EPOLLOUT` 严格配对

```cpp
ssize_t Session::send(const void *data, size_t len) {
    if (!_poller->isCurrentThread()) {                            // 线程安全
        return _poller->sync([&] { return send(data, len); });
    }
    ssize_t sent = _sock->send(data, len);
    if (sent < 0 && Socket::isEagain(errno)) sent = 0;
    if (sent < 0) { emitError(SockException(errno)); return sent; }

    if (static_cast<size_t>(sent) < len) {                        // 部分写 → 余下入队
        _send_queue.append(static_cast<const char *>(data) + sent, len - static_cast<size_t>(sent));
        updateEpollOut(true);                                     // ★ 挂 EPOLLOUT
    }
    _last_send_ms.store(getCurrentMillisecond());
    _bytes_out += static_cast<uint64_t>(sent);
    return sent;
}

void Session::onWriteEvent() {                                    // EPOLLOUT 到来 → 继续 flush
    const ssize_t n = _send_queue.writeToFd(_sock->rawFD());
    if (n > 0) { _last_send_ms.store(getCurrentMillisecond()); _bytes_out += n; }
    if (_send_queue.empty()) updateEpollOut(false);               // ★ 摘 EPOLLOUT
}
```

**挂/摘必须成对，且只收敛在 `updateEpollOut(bool)` 一个函数里。** 这是 LT 下 100% CPU、ET 下"永远发不出去"的共同根源。

### 4.6 Session 生命周期与超时（FR-4.4 落地）

**生命周期**：

| 环节 | 机制 |
|---|---|
| 保活 | `attachEvent()` 时事件回调里捕获 `shared_from_this()` → 只要 fd 还注册着，Session 不会被析构 |
| 释放 | `shutdown()` → `delEvent(fd)` → 回调释放 → 自持环断开 → Session 析构（配合 §4.1 的延迟删除才安全） |
| 统计 | `TcpServer` 用 `weak_ptr` 存会话表，靠 `onManager(session, is_add)` 增减，**不持有强引用** |

**超时（FR-4.4 / FR-5.2）**：

```
活跃时间戳（都是单调时钟，atomic 以便跨线程查询）
  _last_recv_ms：每次 read 到 >0 字节时刷新（半个包也算活跃）
  _last_send_ms：每次成功写出 >0 字节时刷新

检测：每会话一个循环检查任务（doDelayTask 的返回值即"下次再查"）
  周期 = clamp(max(recv_idle, send_blocked) / 10, 1s, 30s)
  recv_idle 超时    → onIdle(recv_idle)      ← 默认实现 = 关闭连接
  send_blocked 超时 → onIdle(send_blocked)   ← 条件：发送队列非空且久未写出成功
```

| 决策 | 值 | 为什么 |
|---|---|---|
| TCP KeepAlive | **开**，idle 60s / interval 10s / count 3（约 90s 判死） | 兜底防"半开连接"泄漏 fd（NFR-3 / NFR-6）。注意：内核 KeepAlive **探测不到对端进程卡死**，必须有应用层检测 |
| `send_blocked_ms` | **30000**（FR-5.2 的"30s 断开"） | 慢客户端 |
| `recv_idle_ms` | **60000**（FR-4.4 的"读空闲 60s"） | 初期先用 SPEC 值；标记为初值，实测后校准 |
| 连接上限 | **64**（FR-4.4） | 同上 |
| 接收缓冲上限 | **1 MB** | 安全网（协议不应依赖它）；`Buffer` 自身不设限 |

> 以上数值全部是**初值**（按 `AI_COLLAB.md` §4.6：保守 / 可观测 / 有出处标记）。**可观测性由计数提供**（见下表），没有计数就不可能有依据校准。

**计数（超限 / 超时必须可见）**

| 计数 | 位置 | 含义 |
|---|---|---|
| `TcpServer::totalRejected()` | TcpServer | 因连接数上限被拒 |
| `TcpServer::totalIdleTimeout()` | TcpServer | 因空闲/写阻塞超时被断开 |
| `TcpServer::totalRecvOverflow()` | TcpServer | 接收缓冲超限被断开 |
| `TcpServer::totalAcceptError()` | TcpServer | `accept` 失败（EMFILE 等） |
| `EventPoller::timerCount()` | EventPoller | 当前定时器数（观测泄漏/堆积） |
| `EventPoller::asyncRejectedCount()` | EventPoller | 任务队列满导致的投递被拒（背压信号） |
| `EventPoller::droppedOnExitCount()` | EventPoller | 退出时丢弃的"已受理但未执行"任务 |

**心跳钩子**：网络层只负责"检测 + 调用 `onIdle()`"；**发什么包由协议层重写 `onIdle()` 决定**（RTSP 用 `OPTIONS`/`GET_PARAMETER`、WebSocket 用 ping 帧）。网络层自己往连接里塞空包是非法流量。

## 5. 线程模型

| 线程 | 数量 | 职责 | 禁止 |
|---|---|---|---|
| EventPoller 线程 | `hardware_concurrency()`（本机 4） | accept、读写、定时器、会话管理 | 绝不阻塞（含不睡眠、不做磁盘 IO） |
| 其它线程（调用方） | 任意 | 调 `send()` / `async()` / `doDelayTask()` | 不得直接 `addEvent/delEvent` 以外的 fd 操作 |

| 对象 | 谁在哪个线程访问 |
|---|---|
| `Session` / `TcpServer` / `Buffer` / `Socket` | **只在 poller 线程**；`send()` 提供跨线程入口（内部 `sync`） |
| `EventPoller` 自身 | `addEvent/modifyEvent/delEvent/async/doDelayTask` 任意线程；事件回调只在轮询线程 |
| `EventPollerPool` | `getPoller` 任意线程（内部锁） |

**连接亲和**：一个连接从建立到关闭绑定同一个 poller → 读写同线程、**零锁**。

## 6. 分批计划与验收

| 批次 | 内容 | 验收（可执行） | 涉及的 FR/NFR |
|---|---|---|---|
| **M2-1** | `PipeWrap` + `EventPoller`（epoll / ET-LT / 延迟删除 / `async` / `sync`）+ `EventPollerPool` | 单测：pipe 读事件、`async` 投递 10 万次无丢、`sync` 取值、`delEvent` 后不再触发、**回调内 `shutdown()` 不崩**；TSAN 严格组 0 报告 | NFR-7 |
| **M2-2** | 定时器（`doDelayTask` / 取消 / 循环任务）+ `getMinDelay` | 单测：精度 ±10ms、取消后不触发、循环任务次数正确、取消与到期的竞态 | NFR-7、ROADMAP:12 的"±5ms" |
| **M2-3** | `Socket` / `Buffer` / `Session` / `TcpServer` + 空闲检测 + `examples/echo_server` | `nc` 回显 100MB 校验和一致；50 并发正确；**空闲超时按 FR-4.4 触发**；断开后 fd 回落；`totalRejected/totalIdleTimeout` 计数增长 | FR-4.4、FR-5.2、NFR-2、NFR-6、SC-3 |

**每批门禁（不变）**：零警告（`-Wall -Wextra`）+ `ctest` 全绿 + ASAN 干净 + TSAN 严格组 0 报告。

**TSAN 分组约定**：网络层"等连接建立 / 等超时发生"的用例命名 `ntimed_*`，并把 `ntimed` **与第一个此类用例同批**加进 `tests/CMakeLists.txt` 的 `MZMEDIA_TEST_GROUPS` 和 `scripts/tsan.sh` 的 `FP_GROUPS`。
> 不能提前注册：分组 0 用例会被判失败（`tests/test_main.h:243` 的防假绿机制），实测 `./build/bin/mzmedia_unittest ntimed` 退出码为 1。

## 7. 测试计划

| 用例 | 怎么造 | 断言 |
|---|---|---|
| `poller_pipe_event` | 往 pipe 写 1 字节 | 回调被触发、`drain` 后无残留 |
| `poller_async_no_loss` | 跨线程投递 10 万次 | 全部执行、计数一致 |
| `poller_sync_value` | 从非 poller 线程 `sync` 取值 | 返回值正确（不超时、不死锁） |
| `poller_del_event_in_callback` | 回调里 `delEvent` 自己 | 不崩；回调返回后对象才析构 |
| `poller_lt_vs_et` | 同一 fd 分别用 LT / ET 注册 | LT 反复触发、ET 只在新数据到达时触发 |
| `timer_precision` | 100ms 定时器，测实际间隔 | 在 `[100, 110] ms` 内 |
| `timer_cancel_race` | 到期瞬间 `cancel()` | 要么执行且返回 0、要么不执行；**不崩溃** |
| `timer_repeat_count` | 返回 20ms 的循环任务，跑 200ms | 触发约 10 次（允许 ±2） |
| `buffer_read_write_fd` | pipe 灌 1MB | `readFromFd` 读到 EAGAIN 才返回；`writeToFd` 部分写后余量正确 |
| `buffer_frame_find` | 半包 / 多包混在一起 | `find` 偏移正确、`consume` 后 `size()` 正确 |
| `ntimed_echo_100mb` | `nc` 灌 100MB 随机数据 | 回显校验和一致（`scripts/echo_test.sh`） |
| `ntimed_session_idle_timeout` | 连上不发数据，`recv_idle=50ms` | 阈内断开、`onError` 为超时、`totalIdleTimeout()` +1 |
| `ntimed_session_send_blocked` | 只连不读 + 持续 `send` | 超过 `send_blocked` 后断开、`bytesOut` 停止增长 |
| `ntimed_session_recv_overflow` | 发送超过 1MB 且服务器不消费 | 断开、`totalRecvOverflow()` +1 |
| `ntimed_fd_recycle` | 50 并发连上再断开 | `/proc/self/fd` 数目回落到基线 |

**验证要求**（`AI_COLLAB.md` §3.4）：每条用例必须覆盖 正常 / 空 / 满 / 断开 / 超大输入中适用的分支；"修前必红"的用例（如 `ntimed_session_idle_timeout`）要附变异验证输出。

## 8. 风险清单

| # | 风险 | 触发条件 | 应对 |
|---|---|---|---|
| 1 | **ET 漏读 → 连接假死** | 一次读事件没读到 `EAGAIN` | 读循环强制到 `EAGAIN`；配"分批发送 + 慢读"用例 |
| 2 | **回调栈上析构 Session（UAF）** | 回调里 `shutdown()` 且无延迟删除 | §4.1 延迟删除 + 回调内拷贝 `shared_ptr`；专项用例 |
| 3 | **`EPOLLOUT` 挂摘不配对** | 加队列不挂、flush 完不摘 | 收敛到 `updateEpollOut()` 单点 |
| 4 | 部分写导致数据错乱 | 大包 + 慢读 | 发送队列 + 读游标前移；1MB 用例 |
| 5 | `EMFILE`（fd 耗尽） | 并发连接数超过 `ulimit -n` | `totalAcceptError()` 计数 + 日志；`ulimit` 写入 TESTING |
| 6 | `epoll_wait` timeout 溢出 | 定时器超过 `INT_MAX` 毫秒 | clamp 到 `INT_MAX`；超长定时器用例 |
| 7 | 定时器与 `cancel` 竞态 | 到期瞬间取消 | 取出后先判 `isCanceled()`；`timer_cancel_race` 用例 |
| 8 | **空闲检测误杀正常客户端** | `recv_idle` 小于客户端心跳周期 | 保守初值（60s）+ `totalIdleTimeout()` 计数暴露；协议层可重写 `onIdle()` 改为发心跳 |
| 9 | 半开连接泄漏 fd | 对端断电/拔网线 | TCP KeepAlive（90s）+ 应用层检测；`ntimed_fd_recycle` 用例 |
| 10 | TSAN 已知误报掩盖真竞争 | 网络测试用带超时的等待 | `ntimed` 分组 + 签名判定（`scripts/tsan.sh`）；根治靠 `apt install g++-12` |

## 9. 决策记录（为什么这么选 / 排除了什么）

| 决策 | 选择 | 排除的选项与原因 |
|---|---|---|
| Reactor 模型 | 单 Reactor × N + 连接亲和 | 排除"单线程"（多核用不上，SC-3 讲不动）；排除"主从 Reactor"（多一个 acceptor 线程，收益不足） |
| 触发模式 | ET 默认 + `EventLT` 可切 | 只做 LT 会少一个能讲深的点；ET 的风险用"读到 EAGAIN"约束掉 |
| 跨线程唤醒 | `pipe2` | 排除 `eventfd`（省 1 个 fd，但无法携带负载）；排除条件变量（唤不醒 `epoll_wait`） |
| 定时器 | `multimap` 最小堆 + 循环任务 | 排除时间轮（实现量翻倍，本项目规模不需要） |
| 写路径 | 发送队列 + 部分写 | 排除"直接 `send`"（慢客户端会丢数据） |
| Buffer | 线性可扩容 | 排除零拷贝链式（M2 工期翻倍，可在 M5 重新评估） |
| Session 所有权 | 自持 + `weak_ptr` 表 | 排除"表持有 `shared_ptr`"（容易忘记释放，且遮蔽生命周期问题） |
| 心跳 | `onIdle()` 钩子，协议层重写 | 排除"网络层内建空包"（HTTP 里塞空包是非法流量） |
| 超时阈值 | 先用 SPEC 的 64 / 60s / 30s + 计数 | 排除"先调参再实现"（没数据无从调）；排除"不设阈值"（半开连接泄漏 fd） |
| `EventPoller` 线程归属 | poller 自带线程 | 排除"由 Pool 统一管理线程"（连接亲和需要"当前线程 == 该 poller"这一不变式） |

## 10. 未决事项（待定，走到对应步骤再定）

| # | 事项 | 何时定 |
|---|---|---|
| 1 | 连接是否**分散到多个 poller**（`TcpServer` 把新 fd 投递给别的 poller） | M2-3 实测多核扩展性后再定（默认不分散，简单且零跨线程投递） |
| 2 | 心跳阈值校准（60s → ？） | M2-3 拿到 `totalIdleTimeout()` 数据后 |
| 3 | 接收缓冲上限校准（1MB → ？） | 同上（看 `totalRecvOverflow()`） |
| 4 | `docs/ARCHITECTURE.md` §1 分层图 / `src/mzmedia.h` 注释的同步修正 | 下一批文档改动时一并做（本文件 §2 是修正后版本） |
| 5 | TSAN 误报根治（装 `g++-12` 或 clang） | 你有 sudo 密码时（需要你执行 `sudo apt install g++-12`） |
