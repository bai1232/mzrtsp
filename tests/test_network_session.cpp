/*
 * Session / TcpServer 单元测试（M2-3b，分组 `ntimed`）
 * ============================================================================
 * 覆盖：回显往返、读空闲超时（FR-4.4）、写阻塞超时（FR-4.4/FR-5.2）、
 *       接收缓冲超限、发送队列超限、连接数上限（FR-4.4）、断开后 fd 回落（NFR-6）。
 *
 * 为什么叫 `ntimed_*` 并单独分组：这些用例要"等连接建立 / 等超时发生"，
 * 天然带**时间维度**（sleepMs 轮询 + 客户端阻塞 recv 超时）。按 §6 的约定，
 * 这一组与其它组分开跑 TSAN，避免把"等待"引入的噪声算到库代码头上。
 *
 * 断言纪律：poller 线程上的回调（onRecv/onError）**绝不能断言**，只记录到 atomic，
 * 断言回到测试主线程做。
 *
 * 【v0.1.2】除"回显内容对不对"之外，每条用例还要断言两类**可观测事实**：
 *   1) 记账：bytesIn / bytesOut / pendingSendBytes / flushCloseTimeoutCount
 *      （存活期间用 `SessionWatch` 读，关闭时刻用 `SessionSnapshot` 在 onError 里快照）；
 *   2) 连接**是否真的关了**：客户端读到 EOF（`waitPeerClosed`），而不是只看"计数涨了"。
 * 为什么必须持强引用才能测"真关"：不持有时会话一摘表就析构，`Socket::~Socket` 会**兜底**
 * close(fd) —— 于是删掉 `shutdownImpl` 里的显式 `close()` 用例仍然全绿（变异实测）。
 * 见 `SessionWatch::holdStrong()` 与本文件里标了 ★ 的断言。
 * ============================================================================
 */

#include "test_main.h"

#include "core/logger.h"  // M6-d：Session::closeLogLevel 返回 LogLevel
#include "core/util.h"
#include "network/session.h"
#include "network/tcp_server.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace mzmedia;

namespace {

constexpr int kWaitMs = 1000;   // 常规等待上限
constexpr int kTimeoutMs = 3000; // 超时类用例的等待上限

/// 阻塞连接到 127.0.0.1:port（带 recv 超时，避免测试挂死）；失败返回 -1
int connectTo(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    // 收缓冲压到 8KB：不然"只连不读"的用例里，1MB 可能被内核缓冲区整个吃掉，
    // 服务器根本没机会进发送队列（测的是库，不该由内核缓冲区大小决定结论）
    int rcvbuf = 8 * 1024;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

/// 等到"对端已关闭"：recv 返回 0（真关闭）返回 true；超时/出错返回 false
bool waitPeerClosed(int fd, int timeout_ms) {
    const uint64_t deadline = getCurrentMillisecond() + static_cast<uint64_t>(timeout_ms);
    char buf[64];
    while (getCurrentMillisecond() < deadline) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n == 0) {
            return true;   // 收到 EOF
        }
        if (n > 0) {
            continue;      // 还有数据没读完，继续（回显场景会走到这里）
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            sleepMs(10);
            continue;
        }
        return false;      // 真出错
    }
    return false;
}

/// 轮询等待某个计数器达到期望值（等"异步发生的事"）
template<typename F>
bool waitFor(F getter, uint64_t expect, int timeout_ms) {
    const uint64_t deadline = getCurrentMillisecond() + static_cast<uint64_t>(timeout_ms);
    while (getCurrentMillisecond() < deadline) {
        if (getter() == expect) {
            return true;
        }
        sleepMs(10);
    }
    return getter() == expect;
}

size_t countOpenFds() {
    size_t n = 0;
    DIR *dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) {
        return 0;
    }
    while (::readdir(dir) != nullptr) {
        ++n;
    }
    ::closedir(dir);
    return n;   // 含 . / .. 与目录自身的 fd：作为**相对**基线足够
}

/**
 * 【v0.1.2】会话**关闭时刻**的计数快照（在 onError 里取，那时对象还完整）
 *
 * 为什么需要：`Session` 由 `TcpServer` 用 `weak_ptr` 持有，用例拿不到强引用；
 * 连接一断对象就没了。于是"关闭前一刻的计数"只能在 onError 这个唯一可观测点快照下来。
 * 字段全用 atomic：写发生在轮询线程、读发生在用例主线程，**不能让 TSAN 报数据竞争**
 *（`ntimed` 是 TSAN 严格组，必须 0 报告）。
 */
struct SessionSnapshot {
    std::atomic<bool> valid{false};          // ★ 先看它：没发生过快照时下面那些 0 全是假绿
    std::atomic<uint64_t> bytes_in{0};
    std::atomic<uint64_t> bytes_out{0};
    std::atomic<size_t> pending_send{0};
    std::atomic<uint64_t> flush_timeouts{0};
};

/**
 * 【v0.1.2】抓"当前这条连接的会话弱引用"，在会话**存活期间**读它的观测值
 * @note attach 发生在轮询线程（accept 时），读发生在用例主线程 → weak_ptr 本身要加锁
 */
struct SessionWatch {
    void attach(const std::shared_ptr<Session> &session) {
        std::lock_guard<std::mutex> lck(_mtx);
        _weak = session;
    }
    bool alive() const {
        std::lock_guard<std::mutex> lck(_mtx);
        return !_weak.expired();
    }
    /**
     * 【v0.1.2】**替服务端保持一个强引用**（模拟"会话被别处持有"的真实场景：
     * M6 的 `HttpResponse::holdResource()` 就把 FLV 的 Sender/订阅者挂在连接上）。
     * 为什么需要：不持有强引用时，会话一被摘表就会析构，而 `Socket` 的析构又**兜底** close(fd) ——
     * 于是"关闭流程到底有没有主动关 fd"这件事**根本区分不出来**（实测：删掉 shutdownImpl 里的
     * `sock->close()`，用例仍然全绿）。持有强引用后，fd 只能靠显式关闭，泄漏才会暴露。
     */
    void holdStrong(const std::shared_ptr<Session> &session) {
        std::lock_guard<std::mutex> lck(_mtx);
        _weak = session;
        _strong = session;
    }
    void releaseStrong() {
        std::lock_guard<std::mutex> lck(_mtx);
        _strong.reset();
    }
    uint64_t bytesIn() const { auto s = lockSession(); return s ? s->bytesIn() : 0; }
    uint64_t bytesOut() const { auto s = lockSession(); return s ? s->bytesOut() : 0; }
    size_t pendingSend() const { auto s = lockSession(); return s ? s->pendingSendBytes() : 0; }

private:
    std::shared_ptr<Session> lockSession() const {
        std::lock_guard<std::mutex> lck(_mtx);
        return _weak.lock();
    }
    mutable std::mutex _mtx;
    std::weak_ptr<Session> _weak;
    std::shared_ptr<Session> _strong;   // 只有 holdStrong() 才会填
};

/// 记录关闭原因类型的会话（用例断言"onError 为超时/超限"就靠它）
class RecordingSession : public Session {
public:
    RecordingSession(const Socket::Ptr &sock, const EventPoller::Ptr &poller, std::atomic<int> *out,
                     SessionSnapshot *snap = nullptr)
        : Session(sock, poller), _out(out), _snap(snap) {}

protected:
    void onError(const SockException &err) override {
        _out->store(static_cast<int>(err.type()));   // 回调在 poller 线程：只记录，不断言
        if (_snap != nullptr) {
            // 顺序要注意：先填字段、最后置 valid（全是 seq_cst atomic，读侧先看 valid 就一定能看到前面的值）
            _snap->bytes_in.store(bytesIn());
            _snap->bytes_out.store(bytesOut());
            _snap->pending_send.store(pendingSendBytes());
            _snap->flush_timeouts.store(flushCloseTimeoutCount());
            _snap->valid.store(true);
        }
        Session::onError(err);
    }

private:
    std::atomic<int> *_out;
    SessionSnapshot *_snap;
};

/// 回显（示例程序里的同款写法：重写 onRecv）
class EchoSession : public RecordingSession {
public:
    EchoSession(const Socket::Ptr &sock, const EventPoller::Ptr &poller, std::atomic<int> *out)
        : RecordingSession(sock, poller, out) {}

protected:
    void onRecv(const Buffer::Ptr &buf) override {
        send(buf->data(), buf->size());
    }
};

/// 只消费不回显（"协议层已处理完"）：用于测接收侧的安全网
class SinkSession : public RecordingSession {
public:
    SinkSession(const Socket::Ptr &sock, const EventPoller::Ptr &poller, std::atomic<int> *out)
        : RecordingSession(sock, poller, out) {}

protected:
    void onRecv(const Buffer::Ptr &buf) override {
        buf->consume(buf->size());   // 消费干净：表示协议层处理完了
    }
};

/// 收到 1 字节就回 1MB：用于把**发送队列**顶到上限（慢客户端）
class BlastSession : public RecordingSession {
public:
    BlastSession(const Socket::Ptr &sock, const EventPoller::Ptr &poller, std::atomic<int> *out)
        : RecordingSession(sock, poller, out), _blob(1u << 20, 'B') {}

protected:
    void onRecv(const Buffer::Ptr &) override {
        send(_blob.data(), _blob.size());
    }

private:
    std::string _blob;
};

/// 「发完再关」用：收到 1 字节就回一段固定数据，然后请求 shutdownAfterFlush
class FlushCloseSession : public RecordingSession {
public:
    FlushCloseSession(const Socket::Ptr &sock, const EventPoller::Ptr &poller, std::atomic<int> *out,
                      size_t bytes, uint32_t wait_ms, SessionSnapshot *snap = nullptr)
        : RecordingSession(sock, poller, out, snap), _blob(bytes, 'F'), _wait_ms(wait_ms) {}

protected:
    void onRecv(const Buffer::Ptr &) override {
        send(_blob.data(), _blob.size());      // 先入队（很可能写不完）
        (void) shutdownAfterFlush(_wait_ms);   // 排空之后再关；到点没排空则强制关 + 计数
    }

private:
    std::string _blob;
    uint32_t _wait_ms;
};

} // namespace

// ---------------------------------------------------------------------------
// 正常：回显往返
// ---------------------------------------------------------------------------

MZ_TEST(ntimed_echo_roundtrip) {
    auto poller = EventPoller::create("test-session-echo");
    std::atomic<int> last_err{-1};
    SessionWatch watch;   // 【v0.1.2】观测存活会话的记账值
    auto server = std::make_shared<TcpServer>(poller);
    MZ_ASSERT_TRUE(server->setSessionCreator([&last_err, &watch](const Socket::Ptr &sock) -> Session::Ptr {
        auto session =
            std::make_shared<EchoSession>(sock, EventPollerPool::Instance().getPoller(), &last_err);
        watch.attach(session);
        return session;
    }));
    MZ_ASSERT_TRUE(server->setSessionTimeout(0, 0));   // 关掉空闲检测，别干扰本用例
    MZ_ASSERT_TRUE(server->start(0));
    MZ_ASSERT_GT(server->port(), 0);

    const int cli = connectTo(server->port());
    MZ_ASSERT_GT(cli, 0);

    const std::string msg = "hello, mzmedia";
    MZ_ASSERT_EQ(::send(cli, msg.data(), msg.size(), 0), static_cast<ssize_t>(msg.size()));

    std::string got(msg.size(), '\0');
    MZ_ASSERT_EQ(::recv(cli, &got[0], got.size(), 0), static_cast<ssize_t>(msg.size()));
    MZ_ASSERT_STR_EQ(got, msg);
    MZ_ASSERT_EQ(server->totalAccepted(), 1u);
    MZ_ASSERT_EQ(server->sessionCount(), 1u);

    // ★【v0.1.2】记账断言（此前一条都没有：回显内容对得上，但"读了多少、写了多少、队列排空没"没人验）
    // 抓什么：bytesIn / bytesOut 的记账回归；以及 pendingSendBytes 不归零（数据进了队列再没人写出去）
    // 后果：/api/stats 的吞吐与"慢客户端"判断全错（FR-6.1）；队列不排空 = 内存慢性上涨（NFR-3 的 RSS 斜率中招）
    // 防线在哪：session.cpp 读路径的 `_bytes_in.fetch_add`、写路径的 `_bytes_out/_pending_send_bytes`、
    //          以及 onWriteEvent 里"EAGAIN 之后靠 EPOLLOUT 续写"那段
    // 变异：删掉 `_bytes_out.fetch_add` → bytes_out 断言红；删掉 EPOLLOUT 续写 → pendingSend 不归零
    MZ_ASSERT_TRUE(watch.alive());   // 先确认会话还活着，否则下面三个 0 都是假绿（观测对象没了）
    MZ_ASSERT_EQ(watch.bytesIn(), static_cast<uint64_t>(msg.size()));
    // ⚠️ bytesOut 必须用"**最终一致**"断言，不能立即相等：
    //   `Session::send` 是「先 writeToFd 把字节交给内核、**之后**才 `_bytes_out.fetch_add`」
    //   （session.cpp 的写路径）。所以客户端 recv 返回的那一刻，服务端的计数可能还没加上 ——
    //   立即断言 = 把两个线程的调度顺序当前提（单进程全量跑时实测翻红一次，组跑时侥幸绿）。
    //   这正是 TESTING.md §2.2 与本项目 M7 那条教训（"别拿插桩/调度时序当前提"）的同一形状。
    MZ_ASSERT_TRUE(waitFor([&watch]() { return watch.bytesOut(); },
                           static_cast<uint64_t>(msg.size()), kWaitMs));
    MZ_ASSERT_TRUE(waitFor([&watch]() { return static_cast<uint64_t>(watch.pendingSend()); }, 0, kWaitMs));

    ::close(cli);   // 对端关闭 → 会话应被摘表
    MZ_ASSERT_TRUE(waitFor([&server]() { return server->sessionCount(); }, 0, kWaitMs));
    MZ_ASSERT_EQ(server->totalAccepted(), 1u);

    server->shutdown();
    poller->shutdown();
}

// ---------------------------------------------------------------------------
// FR-4.4：读空闲超时
// ---------------------------------------------------------------------------

MZ_TEST(ntimed_session_idle_timeout) {
    auto poller = EventPoller::create("test-session-idle");
    std::atomic<int> last_err{-1};
    auto server = std::make_shared<TcpServer>(poller);
    MZ_ASSERT_TRUE(server->setSessionCreator([&last_err](const Socket::Ptr &sock) -> Session::Ptr {
        return std::make_shared<RecordingSession>(sock, EventPollerPool::Instance().getPoller(), &last_err);
    }));
    // recv_idle=50ms：检查周期 = clamp(50/10, 1s, 30s) = 1s → 大约 1s 后断开
    MZ_ASSERT_TRUE(server->setSessionTimeout(50, 0));
    MZ_ASSERT_TRUE(server->start(0));

    const int cli = connectTo(server->port());
    MZ_ASSERT_GT(cli, 0);
    // 连上就**什么都不发**：等着被空闲检测杀掉
    MZ_ASSERT_TRUE(waitPeerClosed(cli, kTimeoutMs));
    MZ_ASSERT_TRUE(waitFor([&server]() { return server->totalIdleTimeout(); }, 1, kWaitMs));
    MZ_ASSERT_EQ(server->sessionCount(), 0u);
    MZ_ASSERT_EQ(last_err.load(), static_cast<int>(SockException::ErrType::Timeout));

    ::close(cli);
    server->shutdown();
    poller->shutdown();
}

// ---------------------------------------------------------------------------
// FR-4.4 / FR-5.2：写阻塞超时（时间级防线）
// ---------------------------------------------------------------------------

MZ_TEST(ntimed_session_send_blocked) {
    auto poller = EventPoller::create("test-session-send-blocked");
    std::atomic<int> last_err{-1};
    SessionWatch watch;   // 【v0.1.2】持强引用：让"显式关 fd"成为唯一能让对端收到 EOF 的途径
    auto server = std::make_shared<TcpServer>(poller);
    MZ_ASSERT_TRUE(server->setSessionCreator([&last_err, &watch](const Socket::Ptr &sock) -> Session::Ptr {
        // 发送队列上限保持默认 8MB：本例测的是**时间**防线（50ms 没写成功就断）。
        // 发送缓冲压到 8KB，保证 1MB 一定写不完（否则结论取决于内核缓冲大小）
        sock->setSendBufSize(8 * 1024);
        auto session = std::make_shared<BlastSession>(sock, EventPollerPool::Instance().getPoller(), &last_err);
        watch.holdStrong(session);   // ★ 见 holdStrong() 的注释：不这样断言就测不出 fd 泄漏
        return session;
    }));
    MZ_ASSERT_TRUE(server->setSessionTimeout(0, 50));
    MZ_ASSERT_TRUE(server->start(0));

    const int cli = connectTo(server->port());
    MZ_ASSERT_GT(cli, 0);
    MZ_ASSERT_EQ(::send(cli, "x", 1, 0), 1);   // 让服务器回 1MB，但**不读**
    MZ_ASSERT_TRUE(waitFor([&server]() { return server->totalIdleTimeout(); }, 1, kTimeoutMs));
    MZ_ASSERT_EQ(last_err.load(), static_cast<int>(SockException::ErrType::Timeout));
    MZ_ASSERT_EQ(server->sessionCount(), 0u);

    // ★【v0.1.2】"计数涨了" ≠ "连接真的关了"。
    // 抓什么：超时只改了计数 / 只摘了会话表，却没 close(fd) → 客户端永远等不到 EOF，fd 泄漏（NFR-6）
    // 后果：10 路慢客户端 = 10 个 fd 长期不回收；连接数上限（FR-4.4 默认 64）会被"假活"连接吃满
    // 防线在哪：Session::shutdownImpl（先 delEvent 等 complete_cb，再 sock->close()）
    // 变异：把 shutdownImpl 里的 sock->close() 去掉 → 本断言红（前面三条仍然绿）
    // 注：本用例**替服务端持着强引用**（watch.holdStrong），所以会话不会因引用计数归零而析构
    //     —— 客户端能收到 EOF 只可能来自 shutdownImpl 里那次**显式 close**。
    MZ_ASSERT_TRUE(waitPeerClosed(cli, kTimeoutMs));

    // ★【v0.1.2】方向断言：本用例测的是**时间**防线，就必须证明"字节防线没有先触发"
    // 抓什么：两条防线的判定写反/写串（例如把 Timeout 记成 SendOverflow，或超限阈值被压到 8KB 附近）
    // 后果：/api/stats 里 idleTimeout 与 sendOverflow 失去区分度 → 排障会去查错方向
    // 防线在哪：Session 的 50ms 时间判定与 maxSendBuffer 字节判定是**两条独立路径**
    // 变异：把 setMaxSendBuffer 压到 1KB → 本断言红（totalSendOverflow 变 1）
    MZ_ASSERT_EQ(server->totalSendOverflow(), 0u);
    MZ_ASSERT_EQ(server->totalRecvOverflow(), 0u);

    ::close(cli);
    server->shutdown();
    poller->shutdown();
    watch.releaseStrong();   // 会话已关：放掉测试侧的强引用，避免析构顺序被测试干扰
}

// ---------------------------------------------------------------------------
// §4.3：发送队列上限（字节级防线）—— 只连不读不能把内存涨到 OOM
// ---------------------------------------------------------------------------

MZ_TEST(ntimed_session_send_overflow) {
    auto poller = EventPoller::create("test-session-send-overflow");
    std::atomic<int> last_err{-1};
    SessionWatch watch;   // 【v0.1.2】同上：持强引用，fd 只能靠显式关闭释放
    auto server = std::make_shared<TcpServer>(poller);
    MZ_ASSERT_TRUE(server->setSessionCreator([&last_err, &watch](const Socket::Ptr &sock) -> Session::Ptr {
        sock->setSendBufSize(8 * 1024);   // 同上：让"发不完"成为确定事实
        auto session = std::make_shared<BlastSession>(sock, EventPollerPool::Instance().getPoller(), &last_err);
        // 上限压到 64KB：对端不读 → 必然有超过 64KB 进不了 socket，直接判超限
        session->setMaxSendBuffer(64 * 1024);
        watch.holdStrong(session);
        return session;
    }));
    MZ_ASSERT_TRUE(server->setSessionTimeout(0, 0));   // 关掉时间防线，只验证字节防线
    MZ_ASSERT_TRUE(server->start(0));

    const int cli = connectTo(server->port());
    MZ_ASSERT_GT(cli, 0);
    MZ_ASSERT_EQ(::send(cli, "x", 1, 0), 1);
    MZ_ASSERT_TRUE(waitFor([&server]() { return server->totalSendOverflow(); }, 1, kWaitMs));
    MZ_ASSERT_EQ(last_err.load(), static_cast<int>(SockException::ErrType::SendOverflow));
    MZ_ASSERT_EQ(server->totalIdleTimeout(), 0u);   // 不是被时间防线断的

    // ★【v0.1.2】字节防线也必须**真的关连接**（前面只验了"计数涨了、类型对了"）
    // 抓什么：`send()` 判超限后只把错误记进 last_err/SendOverflow，却忘了走关闭流程
    // 后果：慢客户端既不读也不断，占着 8MB 队列 + 一个 fd；100 个这样的客户端 = OOM 前先耗尽连接上限
    // 防线在哪：Session::send 里"队列超 maxSendBuffer → 以 SendOverflow 关闭"这条路径
    // 变异：把那条 return -1 改成"只计数不断开" → 本断言红（waitPeerClosed 超时）
    MZ_ASSERT_TRUE(waitPeerClosed(cli, kTimeoutMs));
    MZ_ASSERT_EQ(server->sessionCount(), 0u);
    MZ_ASSERT_EQ(server->totalSendOverflow(), 1u);   // 恰好一次：不是"关了两次/计了两次"

    ::close(cli);
    server->shutdown();
    poller->shutdown();
    watch.releaseStrong();
}

// ---------------------------------------------------------------------------
// §4.3：接收缓冲上限（安全网）
// ---------------------------------------------------------------------------

MZ_TEST(ntimed_session_recv_overflow) {
    auto poller = EventPoller::create("test-session-recv-overflow");
    std::atomic<int> last_err{-1};
    auto server = std::make_shared<TcpServer>(poller);
    MZ_ASSERT_TRUE(server->setSessionCreator([&last_err](const Socket::Ptr &sock) -> Session::Ptr {
        auto session = std::make_shared<SinkSession>(sock, EventPollerPool::Instance().getPoller(), &last_err);
        // 单次交付上限 4096：客户端一次灌 1MB ⇒ 单事件要交付 256 次 > 64 次阈值 ⇒ 判超限
        session->setMaxRecvBuffer(4096);
        return session;
    }));
    MZ_ASSERT_TRUE(server->setSessionTimeout(0, 0));
    MZ_ASSERT_TRUE(server->start(0));

    const int cli = connectTo(server->port());
    MZ_ASSERT_GT(cli, 0);
    const std::string flood(1u << 20, 'F');   // 1MB：4096 × 64 次 = 256KB 就会被判超限
    ssize_t sent = 0;
    while (sent < static_cast<ssize_t>(flood.size())) {   // 一次 send 可能写不完
        const ssize_t n = ::send(cli, flood.data() + sent, flood.size() - static_cast<size_t>(sent), 0);
        if (n <= 0) {
            break;   // 服务器已经断开：正常（它就是在超限时断的）
        }
        sent += n;
    }
    MZ_ASSERT_GT(sent, 0);

    MZ_ASSERT_TRUE(waitFor([&server]() { return server->totalRecvOverflow(); }, 1, kWaitMs));
    MZ_ASSERT_EQ(last_err.load(), static_cast<int>(SockException::ErrType::RecvOverflow));

    ::close(cli);
    server->shutdown();
    poller->shutdown();
}

// ---------------------------------------------------------------------------
// FR-4.4：连接数上限（拒绝要真的拒绝，且要计数）
// ---------------------------------------------------------------------------

MZ_TEST(ntimed_max_session_count) {
    auto poller = EventPoller::create("test-session-max");
    std::atomic<int> last_err{-1};
    auto server = std::make_shared<TcpServer>(poller);
    MZ_ASSERT_TRUE(server->setSessionCreator([&last_err](const Socket::Ptr &sock) -> Session::Ptr {
        return std::make_shared<RecordingSession>(sock, EventPollerPool::Instance().getPoller(), &last_err);
    }));
    MZ_ASSERT_TRUE(server->setSessionTimeout(0, 0));
    MZ_ASSERT_TRUE(server->setMaxSessionCount(1));
    MZ_ASSERT_EQ(server->maxSessionCount(), 1u);
    MZ_ASSERT_TRUE(server->start(0));

    const int first = connectTo(server->port());
    MZ_ASSERT_GT(first, 0);
    MZ_ASSERT_TRUE(waitFor([&server]() { return server->sessionCount(); }, 1, kWaitMs));

    const int second = connectTo(server->port());   // 第 2 个：应被拒绝
    MZ_ASSERT_GT(second, 0);                        // connect 会成功（内核已握手）
    MZ_ASSERT_TRUE(waitPeerClosed(second, kWaitMs));  // 但立刻被服务器关掉
    MZ_ASSERT_TRUE(waitFor([&server]() { return server->totalRejected(); }, 1, kWaitMs));
    MZ_ASSERT_EQ(server->sessionCount(), 1u);        // 只有第 1 个还活着

    // ★【v0.1.2】被拒的连接**不能**计入 totalAccepted（之前只验了"拒绝了"，没验"没被算成接入"）
    // 抓什么：拒绝计数语义漂移 —— 例如把 `_total_accepted.fetch_add` 挪到限额检查之前，
    //         或拒绝路径顺手复用了 accepted 的累加
    // 后果：/api/stats 的 accepted 把被拒连接也算进去 → 容量规划与告警全部失真；
    //       FR-4.4 "默认 64 上限"的验收（第 65 条必须被拒）在数字上再也说不清
    // 防线在哪：tcp_server.cpp 的 onAccept：限额检查 → 拒绝并 close(fd) → continue，
    //           `_total_accepted.fetch_add(1)` 在会话真正建好**之后**
    // 变异：把 accepted 的累加挪到限额检查之前 → 本断言红（实际 2）
    MZ_ASSERT_EQ(server->totalAccepted(), 1u);
    MZ_ASSERT_EQ(server->totalRejected(), 1u);

    ::close(second);
    ::close(first);
    server->shutdown();
    poller->shutdown();
}

// ---------------------------------------------------------------------------
// NFR-6：断开之后 fd 要回落（不泄漏）
// ---------------------------------------------------------------------------

MZ_TEST(ntimed_fd_recycle) {
    auto poller = EventPoller::create("test-fd-recycle");
    std::atomic<int> last_err{-1};
    auto server = std::make_shared<TcpServer>(poller);
    MZ_ASSERT_TRUE(server->setSessionCreator([&last_err](const Socket::Ptr &sock) -> Session::Ptr {
        return std::make_shared<EchoSession>(sock, EventPollerPool::Instance().getPoller(), &last_err);
    }));
    MZ_ASSERT_TRUE(server->setSessionTimeout(0, 0));
    MZ_ASSERT_TRUE(server->start(0));

    // 先跑一轮把"每条连接的那几个 fd"预分配出来，再取基线（否则第一轮会把基线抬高）
    {
        const int warm = connectTo(server->port());
        MZ_ASSERT_GT(warm, 0);
        MZ_ASSERT_TRUE(waitFor([&server]() { return server->sessionCount(); }, 1, kWaitMs));
        ::close(warm);
        MZ_ASSERT_TRUE(waitFor([&server]() { return server->sessionCount(); }, 0, kWaitMs));
    }
    const size_t baseline = countOpenFds();

    constexpr int kRounds = 20;
    for (int i = 0; i < kRounds; ++i) {
        const int cli = connectTo(server->port());
        MZ_ASSERT_GT(cli, 0);
        MZ_ASSERT_EQ(::send(cli, "ping", 4, 0), 4);
        char buf[8] = {0};
        MZ_ASSERT_EQ(::recv(cli, buf, sizeof(buf), 0), 4);
        ::close(cli);
    }

    MZ_ASSERT_TRUE(waitFor([&server]() { return server->sessionCount(); }, 0, kTimeoutMs));
    MZ_ASSERT_EQ(server->totalAccepted(), static_cast<uint64_t>(kRounds + 1));
    // fd 回落到基线（允许 ±2 的抖动：accept 队列/内核延迟释放）
    MZ_ASSERT_LE(countOpenFds(), baseline + 2);

    server->shutdown();
    poller->shutdown();
}

// ---------------------------------------------------------------------------
// §4.5：没重写 onRecv 也没设回调 —— 必须报错关闭，绝不静默丢数据
// ---------------------------------------------------------------------------

MZ_TEST(ntimed_onrecv_not_implemented) {
    auto poller = EventPoller::create("test-session-no-recv");
    std::atomic<int> last_err{-1};
    SessionSnapshot snap;
    auto server = std::make_shared<TcpServer>(poller);
    MZ_ASSERT_TRUE(server->setSessionCreator([&last_err, &snap](const Socket::Ptr &sock) -> Session::Ptr {
        // RecordingSession 只重写 onError，不重写 onRecv → 走基类守卫
        return std::make_shared<RecordingSession>(sock, EventPollerPool::Instance().getPoller(), &last_err,
                                                  &snap);
    }));
    MZ_ASSERT_TRUE(server->setSessionTimeout(0, 0));
    MZ_ASSERT_TRUE(server->start(0));

    const int cli = connectTo(server->port());
    MZ_ASSERT_GT(cli, 0);
    MZ_ASSERT_EQ(::send(cli, "x", 1, 0), 1);
    MZ_ASSERT_TRUE(waitPeerClosed(cli, kWaitMs));   // 服务器必须主动断开
    MZ_ASSERT_EQ(last_err.load(), static_cast<int>(SockException::ErrType::RecvFailed));

    // ★【v0.1.2】排除假绿：确认那 1 个字节**真的被读进来了**才判"没实现 onRecv"
    // 若 bytesIn 是 0，说明数据根本没到（连接建早了/发早了），那 RecvFailed 也能"凑"出来 ——
    // 用例看着绿，实际什么都没测到。
    // 抓什么：字节计数漏加；以及"以为收到了"与"真的收到了"混为一谈
    // 后果：这类假绿会掩盖真正的"数据到了但没人处理"（§4.5 的静默丢数据）
    // 防线在哪：session.cpp 读路径 `_bytes_in.fetch_add(n)`（在交付给 onRecv 之前）
    // 变异：删掉 `_bytes_in.fetch_add` → 本断言红（实际 0）
    MZ_ASSERT_TRUE(snap.valid.load());
    MZ_ASSERT_EQ(snap.bytes_in.load(), 1u);
    MZ_ASSERT_EQ(snap.bytes_out.load(), 0u);   // 守卫路径不该写任何东西回去

    ::close(cli);
    server->shutdown();
    poller->shutdown();
}

// ---------------------------------------------------------------------------
// §4.5 / 顺序：发送队列非空时**绝不能**直写 socket（否则新数据插到旧数据前面 = 乱序）
// 这是确定性用例：把服务端发送缓冲压到 8KB，保证第 1 块发不完 → 队列非空 →
// 第 2 块若被直接写出去，客户端就会在第 1 块结束前收到第 2 块的字节。
// 不依赖 100MB、不依赖日志（今天的两次错误结论都来自"测量工具本身"，所以判据用这个）。
// ---------------------------------------------------------------------------

namespace {

/// 收到 1 字节就连续发两块不同数据
class TwoChunkSession : public RecordingSession {
public:
    TwoChunkSession(const Socket::Ptr &sock, const EventPoller::Ptr &poller, std::atomic<int> *out)
        : RecordingSession(sock, poller, out), _a(1u << 20, 'A'), _b(1u << 20, 'B') {}

protected:
    void onRecv(const Buffer::Ptr &) override {
        send(_a.data(), _a.size());   // 第 1 块：1MB 'A'（8KB 发送缓冲 → 必然发不完，余量入队）
        send(_b.data(), _b.size());   // 第 2 块：1MB 'B'（此时队列非空！）
    }

private:
    std::string _a;
    std::string _b;
};

} // namespace

MZ_TEST(ntimed_send_order_with_backlog) {
    constexpr size_t kChunk = 1u << 20;

    auto poller = EventPoller::create("test-send-order");
    std::atomic<int> last_err{-1};
    auto server = std::make_shared<TcpServer>(poller);
    MZ_ASSERT_TRUE(server->setSessionCreator([&last_err](const Socket::Ptr &sock) -> Session::Ptr {
        // 发送缓冲压到 8KB：保证第一次 send(1MB) 只写出去一小截，余下进队列
        sock->setSendBufSize(8 * 1024);
        return std::make_shared<TwoChunkSession>(sock, EventPollerPool::Instance().getPoller(), &last_err);
    }));
    MZ_ASSERT_TRUE(server->setSessionTimeout(0, 0));   // 不设空闲检测，避免干扰
    MZ_ASSERT_TRUE(server->start(0));

    const int cli = connectTo(server->port());
    MZ_ASSERT_GT(cli, 0);
    MZ_ASSERT_EQ(::send(cli, "x", 1, 0), 1);           // 触发服务端连发两块

    // 故意延迟读：让服务端把第 1 块塞进队列（队列非空的时刻就是本用例要抓的）
    sleepMs(1000);

    std::string got;
    const uint64_t deadline = getCurrentMillisecond() + 5000;
    char buf[64 * 1024];
    while (got.size() < 2 * kChunk && getCurrentMillisecond() < deadline) {
        const ssize_t n = ::recv(cli, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        got.append(buf, static_cast<size_t>(n));
    }

    MZ_ASSERT_EQ(got.size(), 2 * kChunk);

    // ★ 期望：前 1MB 全是 'A'，后 1MB 全是 'B'
    const size_t first_b = got.find('B');
    const size_t first_a_after_b = (first_b == std::string::npos) ? std::string::npos : got.find('A', first_b);
    std::printf("    [ INFO ] 发送顺序检查：总长 %zu，首个 'B' 在偏移 %zu（期望 %zu），'B' 之后又出现 'A' 的位置 %zu\n",
                got.size(), first_b, kChunk, first_a_after_b);
    MZ_ASSERT_EQ(first_b, kChunk);                                        // 第 1 个 'B' 必须在 1MB 边界
    MZ_ASSERT_EQ(got.find('A', first_b == std::string::npos ? 2 * kChunk : first_b), std::string::npos);

    ::close(cli);
    server->shutdown();
    poller->shutdown();
}

MZ_TEST(ntimed_session_shutdown_after_flush) {
    // 场景 A：客户端**一直读** —— 数据一个字节都不能少，然后才关连接
    {
        auto poller = EventPoller::create("test-session-flush-a");
        std::atomic<int> last_err{-1};
        SessionSnapshot snap;   // 【v0.1.2】关闭时刻的计数快照
        auto server = std::make_shared<TcpServer>(poller);
        MZ_ASSERT_TRUE(server->setSessionCreator([&last_err, &snap](const Socket::Ptr &sock) -> Session::Ptr {
            sock->setSendBufSize(8 * 1024); // 逼出"多次写 + EPOLLOUT 续写"
            return std::make_shared<FlushCloseSession>(sock, EventPollerPool::Instance().getPoller(),
                                                       &last_err, 256 * 1024, 3000, &snap);
        }));
        MZ_ASSERT_TRUE(server->setSessionTimeout(0, 0)); // 关掉超时，专测「发完再关」
        MZ_ASSERT_TRUE(server->start(0));

        const int cli = connectTo(server->port());
        MZ_ASSERT_GT(cli, 0);
        MZ_ASSERT_EQ(::send(cli, "x", 1, 0), 1);

        size_t got = 0;
        bool closed = false;
        char buf[16384];
        const uint64_t deadline = getCurrentMillisecond() + static_cast<uint64_t>(kTimeoutMs);
        while (getCurrentMillisecond() < deadline) {
            const ssize_t n = ::recv(cli, buf, sizeof(buf), 0);
            if (n == 0) {
                closed = true; // 对端关闭（= 我们排空后主动关的）
                break;
            }
            if (n < 0) {
                break; // 超时/出错
            }
            got += static_cast<size_t>(n);
        }
        MZ_ASSERT_EQ(got, 256u * 1024u); // ★ 一个字节都没丢
        MZ_ASSERT_TRUE(closed);          // ★ 而且是"排空之后"才关的
        MZ_ASSERT_EQ(last_err.load(), static_cast<int>(SockException::ErrType::None));

        // ★【v0.1.2】服务端视角的"排空"：关闭那一刻发送队列必须是 0，且**没有**走过强制关分支
        // 抓什么：客户端收到 256KB（看上去没事），但服务端其实是"到点强制关 + 丢尾巴"；
        //         以及 bytes_out 记账漏加（数据发出去却没记）
        // 后果：FLV/HLS 收尾时最后几片被静默丢掉（客户端表现为"播到一半没了"），
        //       而 /api/stats 的字节数与实际不符
        // 防线在哪：Session::maybeCloseAfterFlush（`_pending_send_bytes > 0` 就不关）
        // 变异：把 maybeCloseAfterFlush 里的 pending 判定去掉 → 提前关 → got 少 + pending 快照非 0
        MZ_ASSERT_TRUE(snap.valid.load());
        MZ_ASSERT_EQ(snap.pending_send.load(), 0u);
        MZ_ASSERT_EQ(snap.flush_timeouts.load(), 0u);   // 与场景 B 配对的另一半：不能记成"超时强制关"
        MZ_ASSERT_EQ(snap.bytes_out.load(), static_cast<uint64_t>(256u * 1024u));

        ::close(cli);
        server->shutdown();
        poller->shutdown();
    }

    // 场景 B：客户端**只连不读** —— 到点强制关，并且**记为失败**（静默丢尾部数据不可接受）
    {
        auto poller = EventPoller::create("test-session-flush-b");
        std::atomic<int> last_err{-1};
        SessionSnapshot snap;
        auto server = std::make_shared<TcpServer>(poller);
        MZ_ASSERT_TRUE(server->setSessionCreator([&last_err, &snap](const Socket::Ptr &sock) -> Session::Ptr {
            sock->setSendBufSize(8 * 1024);
            return std::make_shared<FlushCloseSession>(sock, EventPollerPool::Instance().getPoller(),
                                                       &last_err, 1u << 20, 100, &snap);
        }));
        MZ_ASSERT_TRUE(server->setSessionTimeout(0, 0)); // 只有"发完再关"的截止时刻在起作用
        MZ_ASSERT_TRUE(server->start(0));

        const int cli = connectTo(server->port());
        MZ_ASSERT_GT(cli, 0);
        MZ_ASSERT_EQ(::send(cli, "x", 1, 0), 1);

        const auto hit_timeout = [&last_err]() -> uint64_t {
            return last_err.load() == static_cast<int>(SockException::ErrType::SendFailed) ? 1u : 0u;
        };
        MZ_ASSERT_TRUE(waitFor(hit_timeout, 1, kTimeoutMs));
        MZ_ASSERT_EQ(server->sessionCount(), 0u);

        // ★【v0.1.2】"强制关记为失败"这条必须先被**确认发生过**，否则 last_err==SendFailed
        //         可能是别的原因（例如对端 RST）误打误撞凑出来的 —— 那就是假绿
        // 抓什么：强制关路径没有计数（下次"发完再关超时"就查不到）；以及 last_err 的归因串味
        // 后果：FLV 直播里"慢客户端被强制关"变成不可观测事件；NFR-3 的"错误日志 0 条"也失去解释力
        // 防线在哪：Session::onFlushDeadline（`++_flush_close_timeout_count` 后
        //          以 SendFailed 关闭，而不是 None —— 静默丢尾部数据不可接受）
        // 变异：把 onFlushDeadline 里的 ++ 去掉 → 本断言红（实际 0）；
        //       把 SendFailed 改成 None → hit_timeout 永不成立
        MZ_ASSERT_TRUE(snap.valid.load());
        MZ_ASSERT_EQ(snap.flush_timeouts.load(), 1u);   // 恰好一次
        // "关闭那一刻确实还有数据没发完"怎么观测？
        //   ✗ 不能用 pendingSendBytes()：`shutdownImpl` 会 `_pending_send_bytes.store(0)`
        //     （session.cpp 第 2 步"释放发送队列"），到 onError 时它**一定是 0**；
        //     这个原始数字只留在 onFlushDeadline 的 Warn 日志里（所以"有日志"不能代替"有断言"）。
        //   ✓ 用 bytes_out：1MB 的 payload 只写出去了一部分（> 0 且 < 1MB），
        //     就证明"尾部数据确实没发完就被强行关掉了"——这是静默丢数据风险的现场证据。
        MZ_ASSERT_GT(snap.bytes_out.load(), 0u);
        MZ_ASSERT_LT(snap.bytes_out.load(), static_cast<uint64_t>(1u << 20));

        ::close(cli);
        server->shutdown();
        poller->shutdown();
    }
}

MZ_TEST(ntimed_session_close_log_level) {
    // M6-d：**正常断开不该记 Error**。
    // 背景（实测）：并发脚本里 10 路客户端播完自己断开 → 服务端日志出现 10 条
    // `[E] Session[...] 关闭：peer-closed`，于是 NFR-3 的"错误日志 0 条"这条验收
    // 立刻失去意义（而且会训练读者忽略 Error）。
    // 判据做成 public static 纯函数就是为了这条用例能断言它（日志散在 emitError 里没法测）。
    using ErrType = SockException::ErrType;
    // 正常收尾 → Info
    MZ_ASSERT_EQ(Session::closeLogLevel(SockException(ErrType::None)), LogLevel::Info);
    MZ_ASSERT_EQ(Session::closeLogLevel(SockException(ErrType::PeerClosed)), LogLevel::Info);
    MZ_ASSERT_EQ(Session::closeLogLevel(SockException(ErrType::Shutdown)), LogLevel::Info);
    // 异常一律**保持 Error**（不顺手改语义：那会让"错误日志"变成主观判断）
    MZ_ASSERT_EQ(Session::closeLogLevel(SockException(ErrType::Timeout)), LogLevel::Error);
    MZ_ASSERT_EQ(Session::closeLogLevel(SockException(ErrType::RecvOverflow)), LogLevel::Error);
    MZ_ASSERT_EQ(Session::closeLogLevel(SockException(ErrType::SendOverflow)), LogLevel::Error);
    MZ_ASSERT_EQ(Session::closeLogLevel(SockException(ErrType::Rejected)), LogLevel::Error);
    MZ_ASSERT_EQ(Session::closeLogLevel(SockException(ErrType::AcceptError)), LogLevel::Error);
    MZ_ASSERT_EQ(Session::closeLogLevel(SockException(ErrType::SendFailed)), LogLevel::Error);
    MZ_ASSERT_EQ(Session::closeLogLevel(SockException(ErrType::RecvFailed)), LogLevel::Error);

    // 【同一概念的另外半边】"对端消失"也不是服务器故障：客户端被杀/关页面/断网时
    // send 会拿 EPIPE、recv 会拿 ECONNRESET。实测（并发脚本 kill 一半客户端）：
    // 5 路被 kill → 10 条 Error（send-failed + recv-failed + 应用层 abortFn）。
    MZ_ASSERT_TRUE(isPeerGoneErrno(EPIPE));
    MZ_ASSERT_TRUE(isPeerGoneErrno(ECONNRESET));
    MZ_ASSERT_TRUE(isPeerGoneErrno(ECONNABORTED));
    MZ_ASSERT_TRUE(isPeerGoneErrno(ENOTCONN));
    MZ_ASSERT_FALSE(isPeerGoneErrno(ENOBUFS)); // 缓冲区不够是**我们的**问题 → 必须留 Error
    MZ_ASSERT_FALSE(isPeerGoneErrno(0));
    MZ_ASSERT_EQ(Session::closeLogLevel(SockException(ErrType::SendFailed, EPIPE)), LogLevel::Warn);
    MZ_ASSERT_EQ(Session::closeLogLevel(SockException(ErrType::SendFailed, ECONNRESET)),
                 LogLevel::Warn);
    MZ_ASSERT_EQ(Session::closeLogLevel(SockException(ErrType::RecvFailed, ECONNRESET)),
                 LogLevel::Warn);
    MZ_ASSERT_EQ(Session::closeLogLevel(SockException(ErrType::RecvFailed, 0)), LogLevel::Error);
    MZ_ASSERT_EQ(Session::closeLogLevel(SockException(ErrType::SendFailed, ENOBUFS)),
                 LogLevel::Error);
    // 关键不变式：**只改日志级别，不改类型**（既有 onError 断言与计数依赖 type）
    const SockException peer_gone(ErrType::SendFailed, EPIPE, "写不出去");
    MZ_ASSERT_EQ(peer_gone.type(), ErrType::SendFailed);
    MZ_ASSERT_EQ(peer_gone.errCode(), EPIPE);
}
