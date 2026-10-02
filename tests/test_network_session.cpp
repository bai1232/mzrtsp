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
 * ============================================================================
 */

#include "test_main.h"

#include "core/util.h"
#include "network/session.h"
#include "network/tcp_server.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <dirent.h>
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

/// 记录关闭原因类型的会话（用例断言"onError 为超时/超限"就靠它）
class RecordingSession : public Session {
public:
    RecordingSession(const Socket::Ptr &sock, const EventPoller::Ptr &poller, std::atomic<int> *out)
        : Session(sock, poller), _out(out) {}

protected:
    void onError(const SockException &err) override {
        _out->store(static_cast<int>(err.type()));   // 回调在 poller 线程：只记录，不断言
        Session::onError(err);
    }

private:
    std::atomic<int> *_out;
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

} // namespace

// ---------------------------------------------------------------------------
// 正常：回显往返
// ---------------------------------------------------------------------------

MZ_TEST(ntimed_echo_roundtrip) {
    auto poller = EventPoller::create("test-session-echo");
    std::atomic<int> last_err{-1};
    auto server = std::make_shared<TcpServer>(poller);
    MZ_ASSERT_TRUE(server->setSessionCreator([&last_err](const Socket::Ptr &sock) -> Session::Ptr {
        return std::make_shared<EchoSession>(sock, EventPollerPool::Instance().getPoller(), &last_err);
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
    auto server = std::make_shared<TcpServer>(poller);
    MZ_ASSERT_TRUE(server->setSessionCreator([&last_err](const Socket::Ptr &sock) -> Session::Ptr {
        // 发送队列上限保持默认 8MB：本例测的是**时间**防线（50ms 没写成功就断）。
        // 发送缓冲压到 8KB，保证 1MB 一定写不完（否则结论取决于内核缓冲大小）
        sock->setSendBufSize(8 * 1024);
        return std::make_shared<BlastSession>(sock, EventPollerPool::Instance().getPoller(), &last_err);
    }));
    MZ_ASSERT_TRUE(server->setSessionTimeout(0, 50));
    MZ_ASSERT_TRUE(server->start(0));

    const int cli = connectTo(server->port());
    MZ_ASSERT_GT(cli, 0);
    MZ_ASSERT_EQ(::send(cli, "x", 1, 0), 1);   // 让服务器回 1MB，但**不读**
    MZ_ASSERT_TRUE(waitFor([&server]() { return server->totalIdleTimeout(); }, 1, kTimeoutMs));
    MZ_ASSERT_EQ(last_err.load(), static_cast<int>(SockException::ErrType::Timeout));
    MZ_ASSERT_EQ(server->sessionCount(), 0u);

    ::close(cli);
    server->shutdown();
    poller->shutdown();
}

// ---------------------------------------------------------------------------
// §4.3：发送队列上限（字节级防线）—— 只连不读不能把内存涨到 OOM
// ---------------------------------------------------------------------------

MZ_TEST(ntimed_session_send_overflow) {
    auto poller = EventPoller::create("test-session-send-overflow");
    std::atomic<int> last_err{-1};
    auto server = std::make_shared<TcpServer>(poller);
    MZ_ASSERT_TRUE(server->setSessionCreator([&last_err](const Socket::Ptr &sock) -> Session::Ptr {
        sock->setSendBufSize(8 * 1024);   // 同上：让"发不完"成为确定事实
        auto session = std::make_shared<BlastSession>(sock, EventPollerPool::Instance().getPoller(), &last_err);
        // 上限压到 64KB：对端不读 → 必然有超过 64KB 进不了 socket，直接判超限
        session->setMaxSendBuffer(64 * 1024);
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

    ::close(cli);
    server->shutdown();
    poller->shutdown();
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
    auto server = std::make_shared<TcpServer>(poller);
    MZ_ASSERT_TRUE(server->setSessionCreator([&last_err](const Socket::Ptr &sock) -> Session::Ptr {
        // RecordingSession 只重写 onError，不重写 onRecv → 走基类守卫
        return std::make_shared<RecordingSession>(sock, EventPollerPool::Instance().getPoller(), &last_err);
    }));
    MZ_ASSERT_TRUE(server->setSessionTimeout(0, 0));
    MZ_ASSERT_TRUE(server->start(0));

    const int cli = connectTo(server->port());
    MZ_ASSERT_GT(cli, 0);
    MZ_ASSERT_EQ(::send(cli, "x", 1, 0), 1);
    MZ_ASSERT_TRUE(waitPeerClosed(cli, kWaitMs));   // 服务器必须主动断开
    MZ_ASSERT_EQ(last_err.load(), static_cast<int>(SockException::ErrType::RecvFailed));

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
