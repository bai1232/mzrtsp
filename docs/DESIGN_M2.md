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

**验收标准**（对应 `docs/ROADMAP.md:12`）：`echo` 示例用 `nc` 回显 100MB 无错；定时器精度 **0 早触发 + 相对宿主裸 `nanosleep` 基线增量 ≤5ms**（原稿写"绝对 ±10ms"，实测在本宿主不可达，口径修正见 §7.1 / §8 R11）；**空闲超时按 FR-4.4 触发，断开后 fd 回落（NFR-6）**。

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
    /// ErrType 是 M2-3 新增：FR-4.4 的四类断开在 errno 里都是 0 或笼统的 ETIMEDOUT，
    /// 光靠 errno 无法分类（计数要分桶、用例要断言"onError 为超时"）
    enum class ErrType {
        None = 0, PeerClosed, Timeout, RecvOverflow, SendOverflow,
        Rejected, AcceptError, SendFailed, RecvFailed, Shutdown,
    };

    explicit SockException(int err_code, const std::string &msg = "");   // 只用 errno
    SockException(ErrType type = ErrType::None, int err_code = 0, const std::string &msg = "");

    ErrType type() const;
    const char *typeName() const;   // 日志用
    int errCode() const;            // errno；0 表示无 errno（例如自定义错误）
    bool isEof() const;             // 对端正常关闭（read 返回 0）
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
    Buffer(Buffer &&) noexcept;
    Buffer &operator=(Buffer &&) noexcept;

    const char *data() const;
    size_t size() const;
    size_t capacity() const;
    bool empty() const;
    size_t readPos() const;     // 已消费字节数（诊断 compact 是否生效）

    // 返回值策略：**没有 void 接口**（M2-3 起，AI_COLLAB "尽量不用 void"）——
    // 写/消费返回实际完成量，清空/释放返回被处理的字节数，
    // 调用方永远能判断"我要求的"和"实际发生的"差多少
    size_t append(const void *data, size_t len);   // 实际写入量（nullptr/0 → 0）
    size_t append(const std::string &str);
    size_t reserve(size_t capacity);               // 处理后的实际容量

    size_t consume(size_t len);  // 实际消费量；len > size() → **0 且不改数据 + ErrorP**
    size_t clear();              // 返回被丢弃的字节数（容量保留）
    size_t release();            // 返回被释放的容量字节数

    size_t find(char ch, size_t from = 0) const;                          // 返回偏移
    size_t find(const void *needle, size_t needle_len, size_t from = 0) const;
    bool startWith(const void *prefix, size_t len) const;
    bool endWith(const void *suffix, size_t len) const;

    /// 循环 read() 追加，直到 EAGAIN / EOF / 出错 / 达到 max_bytes
    /// @param max_bytes **必填**：没有上限的读循环等于把内存交给对端控制（§4.3）；
    ///                  §3.3 决定"上限在 Session"，原签名却让 Session 无法在循环中途设限
    /// @param hit_limit 写出"是否因为到上限而停下"：**true 表示 socket 里可能还有数据**，
    ///                  ET 下不会再有通知，调用方必须继续读或断开（§8 R1）
    /// @return >0 新增字节数；0 = EAGAIN 或 EOF；-1 = 真错误 / 非法调用(max_bytes==0)
    ssize_t readFromFd(int fd, size_t max_bytes, bool *hit_limit = nullptr, int *err = nullptr);

    /// 尽量写出可读区；部分写时消费已写部分
    /// @return >0 本次写出字节数；0 = EAGAIN（不是错误）；-1 = 真错误
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
| `consume` 越界 | **拒绝执行**（返回 0 + ErrorP），不按 `size()` 截断 | "少读了几个字节"会变成后续一直错位、极难定位的 bug |
| `readFromFd` 的上限 | 上限**作为必填参数**进来，并把"没读完"用 `hit_limit` 说出来 | 原签名做不到：Session 拿不到读循环中途的控制权 → 与"上限在 Session"自相矛盾；ET 下漏报"没读完"会永久假死 |
| 返回值风格 | **尽量不用 `void`**：写/消费返回实际量，清空/释放返回处理量 | `void` 把"我要求的 vs 实际发生的"这个差值抹掉，只能靠"约定一定全部完成"推理 |

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
    /// @return true = 本次真的关闭了 fd；false = 原本就无效 / 真失败（幂等语义可判定）
    bool close();

    // 全部返回 bool：失败已记日志，调用方按需处理（禁止静默失败）
    bool setNonBlock(bool enable);
    bool setNoDelay(bool enable);                // TCP_NODELAY
    bool setReuseAddr(bool enable);
    bool setReusePort(bool enable);
    bool setKeepAlive(bool enable);
    bool setKeepAliveParams(int idle_sec, int interval_sec, int count);   // Linux TCP_KEEP*
    bool setSendBufSize(int bytes);
    bool setRecvBufSize(int bytes);

    bool bind(const std::string &ip, uint16_t port);
    bool listen(int backlog);
    /// 返回新连接的 fd（accept4 + NONBLOCK|CLOEXEC）；-1 = 失败
    /// **EAGAIN 与真错误都返回 -1，必须靠 *err 区分**（ET 下要一直 accept 到 EAGAIN）
    int accept(std::string *peer_ip = nullptr, uint16_t *peer_port = nullptr, int *err = nullptr);

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

**形状约定（本批内稳定；一旦变更，必须同一批同步本节 + 在 §9 追加变更记录）**

> 原措辞是「定死，不后改」——事实证明既做不到也不该做：M2-1 一批之内就改了两次形状
> （`async` 的返回语义、任务队列上限）。把"不后改"写成承诺，只会让人倾向于掩盖形变，
> 而不是走"改文档 + 记决策"的正当流程。

| 项 | 约定 |
|---|---|
| 失败返回 | `addEvent/modifyEvent/delEvent` 返回 `int`（0/-1）；`async` 返回 `bool`（**队列满或已退出时为 false**，调用方必须检查） |
| 队列上限 | `EventPoller` 的任务队列**有上限**（初值 65536 ≈ 3 MB，`setMaxPendingTasks()` 可调，**传 0 被拒绝**）；满了 **拒绝 + `asyncRejectedCount()` + Warn**。控制面接口走 `sync`，满时抛异常而非静默丢 |
| 错误报告 | 内部失败一律**记日志**（含 fd 与 errno）；调用方不需要解析 errno |
| 线程 | `addEvent/modifyEvent/delEvent` 可从任意线程调（内部投递）；`async(may_sync=true)` 在同线程直接执行 |
| `delEvent` 的 `complete_cb` | 参数是 `bool success`；**在 poller 线程上、删除完成后**调用（回调内不得再 delayed-delete 同一 fd） |
| 定时器线程安全 | `doDelayTask` 可从任意线程调；`DelayTask::cancel()` 线程安全（`atomic`） |
| `doDelayTask` 退出语义 | poller 已退出（`exiting()`）→ **一律 `nullptr` + `rejectedTimerCount()`**，与调用线程无关。修前只有跨线程路径检查，轮询线程内会返回永不触发的非空 handle（静默降级，M2-2b 统一） |
| 同 deadline 的顺序 | `std::multimap` 对等价键保证插入序，但 deadline 由 `doDelayTask` 内部按 `now+delay` 算出 —— **"同一 deadline"本身不可依赖**（跨毫秒/跨线程都会变），因此顺序**不作为契约**，只保证不丢、不饿死（用例 `timer_same_deadline`） |
| 退出时的未触发定时器 | 丢弃必须可见：`droppedTimerOnExitCount()` + Warn（只统计未被 cancel 的） |
| 到期批次观测 | `delayBatchCount()`（非空批次数）/ `delayBatchMax()`（单批高水位）：观测同刻大量到期与 0 延时自投链（§4.3、§8 R12） |

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

    // ---- 以下为 M2-3b 实现后的形状（权威契约以 src/network/session.h 为准）----
    /// 线程安全（非 poller 线程内部 sync 投递）
    /// @return **本次立即写出的字节数**；0 = 没直写出去（socket 满 **或队列非空**），
    ///         数据已入队、由 EPOLLOUT 续写 —— **0 不是失败**；-1 = 已关闭/出错
    ssize_t send(const void *data, size_t len);
    ssize_t send(const std::string &data);

    /// 主动关闭（线程安全、幂等）
    /// @return true = 本次真的发起关闭；false = 之前已经关过（不是错误）
    bool shutdown(const SockException &err = SockException());
    bool isShutdown() const;
    const SockException &lastError() const;   // 关闭原因（只在 poller 线程读）

    // ---- FR-4.4：空闲检测（配置类 setter 统一返回 bool）----
    /// @return false = 被拒或 start() 之后调用（值未生效，已记日志）
    /// 0 = 关闭该方向检测。默认值由 TcpServer::setSessionTimeout 注入
    bool setRecvIdleTimeout(uint32_t ms);
    bool setSendBlockedTimeout(uint32_t ms);
    uint32_t recvIdleTimeout() const;
    uint32_t sendBlockedTimeout() const;

    /// 接收上限（安全网，不是协议上限）：**单次 onRecv 交付上限**；
    /// 单事件最多交付 `kMaxReadsPerEvent`（64）块，超出 → RecvOverflow 断开 + 计数
    bool setMaxRecvBuffer(size_t bytes);          // 默认 1 MB（初值）；0 被拒
    /// 发送队列上限：超限 → SendOverflow 断开 + 计数（字节级防线，区别于 30s 的时间级防线）
    bool setMaxSendBuffer(size_t bytes);          // 默认 8 MB（初值）；0 被拒

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
    ~TcpServer();                                     // 析构调用 shutdown()（析构即排水）

    /// @param port 0 = 由内核分配（随后用 port() 读回，测试友好）
    /// @return true = 已开始监听；false = 失败（**已记日志，不抛异常**）
    ///         四种明确失败：未设 session creator / poller 不可用 / bind-listen 失败 / 重复 start
    bool start(uint16_t port, const std::string &bind_ip = "0.0.0.0");
    /// @return true = 本次真的执行了关停；false = 之前已经关过（幂等）
    bool shutdown();

    // 配置（必须 start() 之前；**全部返回 bool**：false = 被拒/太晚，值未生效）
    bool setSessionCreator(SessionCreator creator);      // 空工厂被拒
    bool setBacklog(int backlog);                        // 默认 1024
    bool setMaxSessionCount(size_t max);                 // 默认 64（FR-4.4）；**0 被拒**（不是"不限"）
    bool setSessionTimeout(uint32_t recv_idle_ms, uint32_t send_blocked_ms);   // FR-4.4
    bool setReusePort(bool enable);

    // 观测
    uint16_t port() const;
    bool listening() const;
    size_t sessionCount() const;
    uint64_t totalAccepted() const;
    uint64_t totalRejected() const;      // FR-4.4：因连接数上限被拒
    uint64_t totalIdleTimeout() const;   // FR-4.4：因空闲/写阻塞超时被断开
    uint64_t totalRecvOverflow() const;  // 单事件接收量超限被断开
    uint64_t totalSendOverflow() const;  // 发送队列超限被断开（字节级防线）
    uint64_t totalAcceptError() const;   // accept 失败（如 EMFILE）
    const EventPoller::Ptr &poller() const;

    /// @return 被替换掉的上一个回调
    SessionCloseCB setOnSessionClose(SessionCloseCB cb);
    /// @return 遍历到的会话数（可从任意线程调用，内部 sync）
    size_t forEachSession(const std::function<void(const Session::Ptr &)> &cb) const;
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
    if (_exit.load()) { ++_rejected_timer_count; return nullptr; }   // 已退出：明确拒绝
    auto ret = std::make_shared<DelayTask>(std::move(task));
    ret->setDeadline(getCurrentMillisecond() + delay_ms);
    if (isCurrentThread()) {                                         // 同线程直插（保证时序）
        _delay_task_map.emplace(ret->deadline(), ret);
        return ret;
    }
    async([this, ret] { _delay_task_map.emplace(ret->deadline(), ret); });
    return ret;
}
// flush：取出到期项 → 先判 isCanceled() → 执行 → 返回非 0 则按新 deadline 重新入堆
```

**时钟**：全部用 `getCurrentMillisecond()`（单调时钟）。**绝不使用墙钟** —— NTP 校时会让定时器乱序。

**溢出边界**：`epoll_wait` 的 timeout 是 `int` 毫秒 → 超过 `INT_MAX` 时 clamp；定时器本身用 `uint64_t`。
`minDelayInLoop()` 的差值还要**饱和**到 `INT64_MAX`：越过 int64 上界时直接 `static_cast<int64_t>`
会得到负数，被 `clampTimeout` 当成"没有定时器" → 永久等待 → 定时器静默永不触发
（M2-2 修复，用例 `timer_huge_delay_clamped` 修前为红）。

**到期批次与重入（M2-2b 观测，先不设限）**：`processDelayTask` 每轮把**所有已到期**的项处理完
才返回，期间不派发 epoll 事件、也不检查 `_exit`。因此：
- 一批同刻到期的定时器（100 路客户端的秒级校验、accept 突发后一批会话的检查任务）会连着跑完；
- 回调里 `doDelayTask(0, …)` 会被**同一个循环**接着处理，自投链能一直占着循环。

观测出口：`delayBatchMax()`（单批高水位）/ `delayBatchCount()`（非空批次数）。
实测：1000 个同刻到期 = 1 批 1000 个；100 个同 deadline = 1 批 100 个；20 步 0 延时自投链 = 1 批 20 个。
**先观测不限制**（决策见 §9）——限制每轮处理数会引入"积压时延迟一轮"的新行为，
要有真实场景证明值得（§10 未决 8）。

**退出语义**：`doDelayTask` 在**任何**线程上，只要 `exiting()` 为真就返回 `nullptr` +
`rejectedTimerCount()`。修前只有跨线程路径检查 `_exit`，轮询线程内（回调里 / shutdown 之后）
会返回一个永不触发的非空 handle —— 静默降级（M2-2b 修复，用例 `timer_submit_after_shutdown`）。
退出时未触发的定时器会被丢弃，计数 `droppedTimerOnExitCount()` + Warn：
"未执行的任务"有 `droppedOnExitCount`，定时器不能反而没有。

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
| `EventPoller::droppedTimerOnExitCount()` | EventPoller | 退出时丢弃的**未触发**定时器数（静默丢弃也算失败） |
| `EventPoller::delayBatchMax()` / `delayBatchCount()` | EventPoller | 单批处理定时器数高水位 / 非空批次数（同刻到期、0 延时自投链） |
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
| **M2-2** | 定时器（`doDelayTask` / 取消 / 循环任务）+ `getMinDelay` | 单测 `tests/test_network_timer.cpp`（10 个用例，清单见 §7.1）：**0 早触发** + 相对宿主裸基线增量 ≤5ms、取消后不触发、循环任务次数正确、取消与到期的竞态、超长延时 clamp 可见 | ROADMAP:12（精度门禁）、NFR-7 |
| **M2-3a** | `Buffer` + `Socket` / `SockException`（新增 `ErrType`） | 12 个用例（§7.3）：`readFromFd` 读到 EAGAIN、**上限契约 `hit_limit`**、部分写、EOF、越界消费被拒 | NFR-7、§4.3 |
| **M2-3b** | `Session` + `TcpServer` + 空闲检测 + `examples/echo_server` + `scripts/echo_test.sh` | `nc` 回显 100MB 校验和一致；50 并发正确；**空闲超时按 FR-4.4 触发**；断开后 fd 回落；`totalRejected / totalIdleTimeout / totalRecvOverflow / totalSendOverflow` 计数增长 | FR-4.4、FR-5.2、NFR-2、NFR-6、SC-3 |

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
| `timer_*`（M2-2 共 10 个用例） | 实现文件 `tests/test_network_timer.cpp` | 完整清单与门禁口径见 §7.1 |
| `buffer_*`（12 个用例） | 实现文件 `tests/test_network_buffer.cpp`：socketpair + **非阻塞 fd**，全程无等待 → 进 TSAN 严格组 | 逐条清单见 §7.3 |
| `ntimed_echo_100mb` | `nc` 灌 100MB 随机数据 | 回显校验和一致（`scripts/echo_test.sh`） |
| `ntimed_session_idle_timeout` | 连上不发数据，`recv_idle=50ms` | 阈内断开、`onError` 为超时、`totalIdleTimeout()` +1 |
| `ntimed_session_send_blocked` | 只连不读 + 持续 `send` | 超过 `send_blocked` 后断开、`bytesOut` 停止增长 |
| `ntimed_session_recv_overflow` | 发送超过 1MB 且服务器不消费 | 断开、`totalRecvOverflow()` +1 |
| `ntimed_fd_recycle` | 50 并发连上再断开 | `/proc/self/fd` 数目回落到基线 |

### 7.1 定时器分组（M2-2，实现文件 `tests/test_network_timer.cpp`）

`ctest -R timer` 实际跑 **18 个**用例：下表 16 个 + M2-1 的两个冒烟用例
（`poller_timer_fires` / `poller_timer_repeat_and_cancel`）——因为分组过滤是**子串匹配**，
用例名里含 `timer` 的都会被命中（无害，只是重复跑一遍）。

等待策略：本组**刻意只用无超时等待**（`Semaphore::wait()` 无超时 / `sleepMs()`），
因此留在 TSAN **严格组**（要求 0 报告），见 `scripts/tsan.sh` 的 `STRICT_GROUPS`。
代价：实现坏掉时用例会挂住，由 ctest 的 `TIMEOUT(120s)` 判失败（不引入 `wait_for`，
因为本环境 TSAN 对 `pthread_cond_clockwait` 存在已知误报）。
另一条纪律：**回调在轮询线程执行，禁止在回调里断言**（测试框架非线程安全），
回调只写 `std::atomic` 或受信号量同步的变量，断言回到测试主线程。

| 用例 | 怎么造 | 断言 |
|---|---|---|
| `timer_precision` | 10 轮，每轮先量一次裸 `nanosleep(100ms)` 基线，再量一次库定时器 | ①每轮库值都 ≥100ms（**不早触发**，硬保证）②`min(库) ≤ min(裸) + 5ms`（库不引入系统性额外延迟）③`max(库) ≤ 50ms`（灾难性回归守卫）；两组的 min/avg/max 都打印出来 |
| `timer_zero_delay` | `doDelayTask(0, …)` | 触发，且耗时 <50ms（不退化成"等一个 tick"） |
| `timer_no_timer_path` | 池里没有任何定时器时跨线程 `sync` | 无限 `epoll_wait` 能被唤醒管道叫醒 |
| `timer_many_1000` | 一次投 1000 个 10ms 定时器 | 1000 个全部触发；`timerCount()` 归 0 |
| `timer_shutdown_cancels` | 投 1000ms 定时器 → `sync` 冲刷 → `shutdown()` | 未到期不触发；退出后 `timerCount()==0`（用 `sync` 而不是"睡 20ms 赌它已入堆"，避免宿主抖动导致 flaky） |
| `timer_huge_delay_clamped` | 两个边界值，**各用一个独立 poller**：`UINT64_MAX/2`（`deadline - now` 正好是 `INT64_MAX`）与 `UINT64_MAX/2 + 1000`（越过 int64 上界） | 两个值都要让 `timeoutClampCount()` 增长（静默降级必须可见，风险 6）。**修前第二个值是红的**：负差值被当成"没有定时器" → 永久等待 → 超长定时器静默永不触发；修复办法是 `minDelayInLoop` 饱和返回 |
| `timer_order_by_deadline` | 乱序投 300 / 100 / 200ms | 触发顺序 100 → 200 → 300 |
| `timer_cancel_race` | 200 次"投 1ms 定时器后立刻 `cancel()`" | 不崩；触发次数 ≤200；`timerCount()` 归 0（取出后先判 `isCanceled()`，风险 7） |
| `timer_repeat_count` | 返回 20ms 的循环任务，第 5 次返回 0 | **恰好 5 次**（不是"跑 200ms 数次数 ±2"：本宿主 10ms 级抖动会让窗口计数 flaky）；`timerCount()` 归 0 |
| `timer_exception_stops_repeat` | 第 2 次触发时抛异常 | 计数停在 2；异常后不再入堆 |
| `timer_concurrent_submit` | 4 线程 × 250 个 `doDelayTask(10ms)`（**多生产者**） | 1000 个全部触发；`timerCount()` / `rejectedTimerCount()` / `asyncRejectedCount()` / `pendingTaskCount()` 全为 0 |
| `timer_same_deadline` | 100 个 50ms 定时器（同刻到期） | 100 个全部触发、无丢失；**顺序刻意不写成断言**（见 §3.5"同 deadline 的顺序"） |
| `timer_cancel_after_fire` | 一次性定时器**已触发**后再 `cancel()`（高频路径：超时 → onIdle → shutdown → cancel 自己） | 不崩、不复活（`fired==1`）、`isCanceled()` 为真、`timerCount()` 仍为 0 |
| `timer_reentrant_submit` | **回调里**再投下一个（链长 5，每步 10ms） | 5 步全部执行、无拒投、`timerCount()` 归 0 |
| `timer_reentrant_zero_delay` | 回调里 `doDelayTask(0, …)` 自投 20 步（**只观测**，不断言批量性质） | 20 步全部执行；打印 `delayBatchMax()`（实测 20 ⇒ 全在**同一个** `processDelayTask` 里跑完，期间不派发 I/O） |
| `timer_submit_after_shutdown` | 轮询线程内先 `shutdown()` 再投定时器（**退出过程中**） | 必须返回 `nullptr` + `rejectedTimerCount()` 增长；被拒的定时器绝不触发（**修前为红**） |

**两处刻意的设计偏离（相对 §7 初稿）**：
1. `timer_order_by_deadline` 的档位间隔从 10ms 改为 **100ms**。每个 deadline 都是在
   **调用线程**上按"当时时刻"算出来的，若两次投递之间被宿主调度打断超过档位间隔，
   期望顺序本身就不成立（本宿主有 10ms 级调度抖动，用 10ms 间隔会真翻红）。
2. `timer_repeat_count` 从"固定跑 200ms 数触发次数（允许 ±2）"改为"等到第 5 次触发"。
   前者把宿主抖动当成了被测对象的指标。

**精度门禁口径（ROADMAP:12 的验收项）**：绝对 `±10ms` 在**本宿主上不可达** ——
裸 `nanosleep` 的超出量本身就是 **0~14ms，且与延时长短无关**
（10ms / 100ms / 1s 档实测 +6 / +10 / +13），即连一个裸系统调用都过不了这个门禁。
因此门禁拆成"绝对下限（不早触发）+ 相对同进程基线增量 ≤5ms"，并且用**最小值**对拍：
系统性偏置（向上取整到 tick、单位算错）会把整个分布连同最小值一起平移 → 抓得到；
随机 tick 噪声只抬高个别样本、抓不到最小值 → 不会误红。
实测两组数据：库 `min/avg/max = 0/7/15 ms`，裸基线 `0/7/13 ms`（即库自身没有额外延迟）。

**TSAN 构建下的门禁（编译期 `__SANITIZE_THREAD__` 判定，运行时打印）**：TSAN 会给
**库代码**插桩（跨线程投递 / mutex / map 操作），却不会给内核里的 `nanosleep` 插桩，
于是"库 vs 裸基线"的差值里混进了插桩开销（实测 TSAN 下 min 差值 ~3ms 且抖动更大）。
因此 TSAN 构建下把 ② 的容差从 5ms 放宽到 25ms、③ 从 50ms 放宽到 100ms，
并在输出里明确打印"门禁已放宽，正式门禁见普通构建"——**不静默**。
计时精度以普通构建的 `ctest timer` 为准；TSAN 那一轮只负责并发正确性。
> 注意 SPEC 的 `NFR-7` 是**代码质量**（零警告 + 单测覆盖），与定时器精度无关；
> 精度目前只写在 `ROADMAP.md` 的 M2 验收里，是否升格为正式 NFR 见 §10。

**验证要求**（`AI_COLLAB.md` §3.4）：每条用例必须覆盖 正常 / 空 / 满 / 断开 / 超大输入中适用的分支；"修前必红"的用例（如 `ntimed_session_idle_timeout`）要附变异验证输出。

### 7.2 用例推导矩阵（机制 × 上下文）

**为什么加这张表**：M2-2 的初稿只按"定时器自身行为"列了 10 条用例，于是"谁投 / 在哪个线程 /
什么时候投"这一整维漏了 4 条 —— 其中 1 条（退出过程中投递）落在一个真实缺陷上。
所以"该测什么"不该由初稿列没列决定，而由下表推导：每个格子问一次
"这段代码在这个上下文里还成立吗"。

| 机制 \ 上下文 | 单生产者（测试线程） | 多生产者 / 跨线程 | 轮询线程内重入 | 退出过程中 |
|---|---|---|---|---|
| 投递 `doDelayTask` | ✔ 已有 | `timer_concurrent_submit` | `timer_reentrant_submit` | `timer_submit_after_shutdown` |
| 到期执行 | ✔ 已有 | 同上（1000 个并发投递的到期） | `timer_reentrant_zero_delay`（只观测） | ✔ 已有（`timer_shutdown_cancels`） |
| 取消 `cancel()` | ✔ 已有 | `timer_cancel_race`（跨线程取消 vs 到期） | `timer_cancel_after_fire` | ✔ 已有 |
| 重复 / 循环 | ✔ 已有 | —（重投只发生在轮询线程） | ✔ 已有（回调里重投即循环） | — |
| 异常 | ✔ 已有 | — | ✔ 已有（异常在回调里抛） | — |
| 超大延时 / 溢出 | ✔ 已有 | — | — | — |
| 同 deadline（同键） | `timer_same_deadline` | —（顺序本就不可依赖） | — | — |

"—" 表示判定为**没有差异化代码路径、不需要单独覆盖**，不是"忘了写"；每格的理由见括注。

### 7.3 Buffer / Socket 分组（M2-3a，实现文件 `tests/test_network_buffer.cpp`）

12 个用例全部**单线程 + 非阻塞 fd**（`socketpair`），不睡眠、不等待 → 留在 TSAN 严格组。
大 payload 用 FNV-1a 哈希比较而不是逐字节 dump（失败信息要能看，不能刷 5KB 的字符）。

| 用例 | 造法 | 断言 |
|---|---|---|
| `buffer_append_consume_basic` | append/consume 基本流 | 返回值 == 实际量、`size()` 一致、消费干净后读游标归零 |
| `buffer_consume_overflow_rejected` | 只有 3 字节却 `consume(10)` | 返回 0 且**数据不变**（不截断） |
| `buffer_clear_release` | 先 clear 再 release | clear 返回被丢弃量且**容量保留**；release 返回释放的容量 |
| `buffer_compact_no_growth` | 100 轮 append 1KB + 半消费 + 消费干净 | 容量仍停在初始量级（≤2×）：compact 生效 |
| `buffer_find_and_affix` | HTTP 头字符串 | `find("\r\n\r\n")` 偏移、`find(char, from)`、未找到 = `npos`、startWith/endWith |
| `buffer_append_null_and_empty` | `append(nullptr,10)` / `append("",0)` / `reserve` | 写入 0；reserve 返回实际容量、够大时原样返回 |
| `buffer_move` | move 构造 / move 赋值 | 数据随所有权转移；被移走的是**可用的空对象** |
| `buffer_read_from_fd_to_eagain` | 对端发 1000 字节 | 一次读空并返回 1000；`hit_limit=false`；再读 = 0（EAGAIN 不是错误） |
| `buffer_read_from_fd_max_bytes_contract` | 对端发 5000 字节，上限 100 | 返回 100 且 **`hit_limit=true`**；按契约循环读到 `hit_limit=false` 后总量 == 5000（**ET 关键契约**，已变异验证会红） |
| `buffer_read_from_fd_eof` | 发 64 字节后关闭对端 | 先把已到的 64 字节交出来，**下一次**才返回 0（EOF） |
| `buffer_read_from_fd_invalid_max` | `max_bytes == 0` | 返回 -1 + `hit_limit=true` + `err=EINVAL`（拒绝"永远读不完"的忙等） |
| `buffer_write_to_fd_partial` | 对端不读，灌 1MB | 部分写：写出量 < 1MB 且已写部分被消费；再写 = 0（EAGAIN）；空缓冲写 = 0 |

## 8. 风险清单

| # | 风险 | 触发条件 | 应对 |
|---|---|---|---|
| 1 | **ET 漏读 → 连接假死** | 一次读事件没读到 `EAGAIN` | 读循环强制到 `EAGAIN`；配"分批发送 + 慢读"用例 |
| 2 | **回调栈上析构 Session（UAF）** | 回调里 `shutdown()` 且无延迟删除 | §4.1 延迟删除 + 回调内拷贝 `shared_ptr`；专项用例 |
| 3 | **`EPOLLOUT` 挂摘不配对** | 加队列不挂、flush 完不摘 | 收敛到 `updateEpollOut()` 单点 |
| 4 | 部分写导致数据错乱 | 大包 + 慢读 | 发送队列 + 读游标前移；1MB 用例 |
| 5 | `EMFILE`（fd 耗尽） | 并发连接数超过 `ulimit -n` | `totalAcceptError()` 计数 + 日志；`ulimit` 写入 TESTING |
| 6 | `epoll_wait` timeout 溢出 | 定时器超过 `INT_MAX` 毫秒 | clamp 到 `INT_MAX`；`timeoutClampCount()` **逐次累加**，但 Warn 只在**进入**截断状态的那一次打（否则每轮事件循环都打 → 一条定时器刷爆日志，把真问题淹掉）。另外 `minDelayInLoop` 的 `next - now` 必须**饱和**到 `INT64_MAX`：越过 int64 上界时直接 `static_cast<int64_t>` 会得到负数，被当成"没有定时器" → 永久等待 → **静默永不触发** |
| 7 | 定时器与 `cancel` 竞态 | 到期瞬间取消 | 取出后先判 `isCanceled()`；`timer_cancel_race` 用例 |
| 8 | **空闲检测误杀正常客户端** | `recv_idle` 小于客户端心跳周期 | 保守初值（60s）+ `totalIdleTimeout()` 计数暴露；协议层可重写 `onIdle()` 改为发心跳 |
| 9 | 半开连接泄漏 fd | 对端断电/拔网线 | TCP KeepAlive（90s）+ 应用层检测；`ntimed_fd_recycle` 用例 |
| 10 | TSAN 已知误报掩盖真竞争 | 网络测试用带超时的等待 | `ntimed` 分组 + 签名判定（`scripts/tsan.sh`）；**已根治（2026-10-03）**：装 `g++-12` 后实测全部组 0 报告 |
| 11 | **绝对精度门禁在虚拟化宿主上不可达** | 宿主 tick 量化唤醒延迟 **0~14ms 且与延时无关**（裸 `nanosleep` 10ms/100ms/1s 档实测 +6/+10/+13） | 门禁改为"**0 早触发** + 相对同进程裸基线增量 ≤5ms"（§7.1）；对拍取**最小值**以剔除随机噪声、保留系统性偏置；换裸机后可收紧回绝对 ±5ms（§10.7） |
| 12 | **0 延时自投链 / 同刻大量到期会占住事件循环** | 回调里 `doDelayTask(0, …)`（被**同一个** `processDelayTask` 循环处理完，期间不派发 epoll 事件、也不检查 `_exit`）；或 100 路客户端的秒级校验同时到点 | **先观测、暂不限制**（§9 决策）：`delayBatchMax()` / `delayBatchCount()` 已能量化（实测 1000 个同刻到期 = 1 批 1000 个；20 步 0 延时自投链 = 1 批 20 个）。触发条件：线上单批远超客户端数 → 再加"每轮处理上限 + 让出循环"（§10 未决 8） |
| 13 | **大流量回显块级乱序（> 8MB）—— 已修复（M2-3b），保留此条作踩坑记录** | `Session::send` 在**发送队列非空**时仍直接 `write` socket：socket 是 FIFO，新块插到旧块前面 → 块级乱序（字节数不变、内容错位）。实测首个错位量 k ≈ 队列尾巴大小（4224 / 25536 / 750272） | 修复：**仅 `_send_queue->empty()` 时才直写**，否则整段入队（FIFO 保序）。判据换成**确定性用例** `ntimed_send_order_with_backlog`（8KB 发送缓冲 + 连续两块：修前首个 `'B'` 在 16KB 处、修后在 1MB 边界）。教训：两次错误结论都来自**测量工具本身**（异步日志未落盘 →"插队=0"假象；探针把 reader 线程建在发送之后 → 7.5MB 就 send-overflow） |

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
| 定时器精度门禁 | "0 早触发 + 相对同进程裸基线增量 ≤5ms"，对拍取**最小值** | 排除"绝对 ±10ms"（本宿主裸 `nanosleep` 自身超出量就 0~14ms，门禁与被测对象无关地红，见 §7.1）；排除"直接放宽到绝对 ±20ms"（把宿主噪声算进库的指标，库自身偏置会被掩盖）；排除"对拍平均值/最大值"（本机噪声会让它随机翻红） |
| clamp 告警频率 | Warn 只在进入"超时截断"状态时打一次；`timeoutClampCount()` 仍逐次累加 | 排除"每轮事件循环都 Warn"（超长定时器 + 频繁唤醒 = 日志洪水）；排除"只计数不告警"（违反 §4.5 可见性） |
| 超长延时的溢出处理 | `minDelayInLoop` 的差值**饱和**到 `INT64_MAX`，再交给 `clampTimeout` 截断 + 计数 + 告警 | 排除"直接 `static_cast<int64_t>(next - now)`"（deadline 越过 int64 上界 → 负数 → 被当成"没有定时器" → `epoll_wait` 永久等待 → 超长定时器静默永不触发，违反 §4.5）；排除"用 assert 拒绝超大延时"（业务错误不该 assert，违反 §4.4） |
| 定时器测试的等待方式 | 只用无超时等待（`Semaphore::wait()` / `sleepMs()`），代价是坏实现会让用例挂到 ctest TIMEOUT | 排除 `wait_for` / `tryWait(ms)`（本环境 TSAN 对 `pthread_cond_clockwait` 有已知误报，会把 timer 组从严格组里踢出去） |
| 计时门禁在 TSAN 下放宽 | 编译期判定 sanitizer，TSAN 构建容差 5ms→25ms（并在输出里打印"已放宽"） | 排除"用同一套容差"（TSAN 只插桩库代码、不插桩内核 `nanosleep`，差值里混进插桩开销 → 随机翻红，实测已红过一次）；排除"TSAN 下跳过计时断言"（静默降级，违反 §4）；排除"把计时用例从 TSAN 严格组里挪走"（同样会少掉并发覆盖） |
| `doDelayTask` 的退出语义 | 已退出一律 `nullptr` + `rejectedTimerCount()`（**含轮询线程内**调用） | 排除"轮询线程内返回非空 handle"（永不触发且无告警 = 静默降级，§4.5）；排除"直接抛异常"（在关停路径上抛异常会把关停流程复杂化） |
| 0 延时自投链 | **只观测**（`delayBatchMax` / `delayBatchCount` + 文档写明性质），不限制 | 排除"限制每轮处理上限"（引入"积压时延迟一轮"的新行为，需要真实场景证明值得）；排除"禁止 delay=0"（`0` 有合法语义：下个循环周期执行）；改判条件：线上单批远超客户端数 |
| 同 deadline 的顺序 | 不写成契约（只保证不丢、不饿死） | 排除"承诺插入序"（deadline 是内部 `now+delay` 算出的，"同 deadline"从外部不可控；承诺了就要为它负责） |
| `SockException` 加 `ErrType`（M2-3a） | 见 §3.2：errno 之外再带"原因" | 排除"只看 errno"：FR-4.4 的四类断开在 errno 里都是 0/ETIMEDOUT，无法分桶计数、也无法断言"onError 为超时" |
| `Buffer::consume` 越界（M2-3a） | 拒绝执行（返回 0 + ErrorP），不截断 | 排除"按 `size()` 截断"（静默错位，最难查）；排除"抛异常"（解析热路径上抛异常代价过大） |
| `Buffer::readFromFd` 带上限（M2-3a） | `max_bytes` **必填** + `hit_limit` 出参 | 排除"容器内部定一个固定上限"（策略不该写死在容器里）；排除"不加上限"（读循环把内存交给对端） |
| 尽量不用 `void`（M2-3a 起） | 公开接口全面给出有意义的返回值；只有"被调用的钩子"保留 void | 排除"保持 void + 文档约定一定成功"：约定被破坏时没人会发现，而返回值让调用方当场可判定 |
| `TcpServer` 连接上限默认（M2-3b） | 默认 **64**（FR-4.4），传 0 **被拒** | 排除"0 = 不限"：有界性不可协商（与 `EventPoller::setMaxPendingTasks(0)` 同一处理） |
| `Session` 发送队列上限（M2-3b） | `setMaxSendBuffer`（默认 8MB）+ `totalSendOverflow()` | 排除"只靠 30s 写阻塞兜底"：时间兜底限制不了**字节数**，只连不读的对端能把内存涨到 OOM |
| `TcpServer` 未设 session creator（M2-3b） | `start()` 明确失败（ErrorP + false） | 排除"接受连接然后丢数据"：那是最难查的一类问题 |
| `close(fd)` 的时机（M2-3b） | 只在 `delEvent` 的 `complete_cb` 里关；投递不进去才就地关 | 排除"delEvent 后立刻 close"（poller 是延迟删除，fd 号会被内核复用给新连接 → 新连接 `addEvent` 被判重复注册，整片连接失败——真踩过）；排除"不关、等析构"（poller 退出清理会丢弃 `complete_cb`，此时靠 `Socket::~Socket` 兜底） |
| 发送顺序（M2-3b 修复） | **仅当发送队列为空时才直写 socket**，队列非空整段入队（FIFO 保序） | 排除"能塞就塞"：新块会插到队列旧块前面 → **块级乱序**（字节数不变、内容错位）；确定性用例 `ntimed_send_order_with_backlog` 锁住 |
| 收发流控（M2-3b） | 发送队列 ≥ `maxSendBuffer/2` 停止收数据，< `/4` 恢复（`modifyEvent` 重挂 EPOLLIN） | 排除"只靠上限兜底"（一个读事件最多 64MB，会把 8MB 队列顶爆）；排除"收满就断"（正常批量传输会被误杀） |
| EOF / 空闲超时的关闭（M2-3b） | 若发送队列非空则**延迟关闭**，最多推迟 `kMaxCloseDefer = 3` 个检查周期，排不完照断 | 排除"立刻断"（丢掉已排队数据 = 静默丢数据）；排除"无限等"（慢客户端能把连接永远挂着） |
| 接收上限的两级语义（M2-3b） | 每块 ≤ `maxRecvBuffer`（1MB）、每事件最多 `kMaxReadsPerEvent`（64）块 | 排除"一次事件读到上限就断"——8MB 的正常回显会被误判成攻击（这个坑真踩过） |
| 尽量不用 `void` 的落地口径（M2-3b） | 配置 setter 返回"是否生效"；操作返回"实际完成量 / 状态是否改变"；注册回调返回上一个；只有"被调用的钩子"保留 void | 排除"全部强行加返回值"（钩子没有接收方语义，硬加只会让实现纠结返回什么） |

## 10. 未决事项（待定，走到对应步骤再定）

| # | 事项 | 何时定 |
|---|---|---|
| 1 | 连接是否**分散到多个 poller**（`TcpServer` 把新 fd 投递给别的 poller） | M2-3 实测多核扩展性后再定（默认不分散，简单且零跨线程投递） |
| 2 | 心跳阈值校准（60s → ？） | M2-3 拿到 `totalIdleTimeout()` 数据后 |
| 3 | 接收缓冲上限校准（1MB → ？） | 同上（看 `totalRecvOverflow()`） |
| 4 | `docs/ARCHITECTURE.md` §1 分层图 / `src/mzmedia.h` 注释的同步修正 | 下一批文档改动时一并做（本文件 §2 是修正后版本） |
| 5 | ~~TSAN 误报根治~~ | **已完成（2026-10-03）**：装 `g++-12` 12.3.0 后实测全部组 0 报告；`scripts/tsan.sh` 自动优先使用 `g++-12` |
| 6 | 定时器精度是否升格为 SPEC 的正式 NFR | 你决定（当前只写在 `ROADMAP.md` 的 M2 验收里；SPEC 的 `NFR-7` 是"代码质量"，与精度无关） |
| 7 | 精度门禁是否在裸机/物理机上收紧回**绝对 ±5ms** | 有裸机环境时（本宿主 0~14ms 唤醒延迟是虚拟化造成的，不是代码问题） |
| 8 | `processDelayTask` 是否加"每轮处理上限 + 让出事件循环" | 等 `delayBatchMax()` 在真实流量（M6 多客户端）里的数据；当前只观测 |
| 9 | ~~大流量回显（> 8MB）内容损坏的根因~~ | **已解决（M2-3b）**：`Session::send` 在发送队列非空时仍直写 socket → 块级乱序；修法与判据见 §9 决策记录、§8 R13 |
