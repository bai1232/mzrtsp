/*
 * TcpServer 实现（M2-3b）
 * ============================================================================
 * accept 循环的三条纪律：
 *   1) ET 模式必须**一直 accept 到 EAGAIN**，否则积压的连接要等下一次事件才被处理；
 *   2) 达到连接上限时**真的关掉 fd**（"接受后再拒绝"会白占一个 fd，还可能被对端当成已连上）；
 *   3) accept 失败要分两类：EAGAIN 是正常（没更多连接），EMFILE/ENFILE 是 fd 耗尽信号，
 *      必须计数 + 报错（§8 R5）。
 * ============================================================================
 */

#include "network/tcp_server.h"

#include "core/logger.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <utility>
#include <vector>

namespace mzmedia {

TcpServer::TcpServer(const EventPoller::Ptr &poller) : _poller(poller) {
    if (!_poller) {
        _poller = EventPollerPool::Instance().getPoller();
    }
}

TcpServer::~TcpServer() {
    shutdown();   // 必须在析构里排水：内部回调捕获 this（见文件头"生命周期硬约束"）
}

bool TcpServer::start(uint16_t port, const std::string &bind_ip) {
    if (_listening.load()) {
        WarnP("TcpServer::start 重复调用：忽略");
        return false;
    }
    if (!_creator) {
        ErrorP("TcpServer::start 未设置 setSessionCreator：拒绝启动"
               "（否则会「能连上但数据被丢」，那是更难查的问题）");
        return false;
    }
    if (!_poller) {
        ErrorP("TcpServer::start 没有可用的 poller");
        return false;
    }
    if (!_poller->isCurrentThread()) {
        try {
            return _poller->sync([this, port, bind_ip]() { return start(port, bind_ip); });
        } catch (const std::exception &e) {
            ErrorP("TcpServer::start 投递到轮询线程失败: %s", e.what());
            return false;
        }
    }

    auto sock = Socket::create(Socket::Domain::IPv4);
    if (!sock) {
        return false;
    }
    if (!sock->setReuseAddr(true)) {
        return false;
    }
    if (_reuse_port.load() && !sock->setReusePort(true)) {
        return false;
    }
    if (!sock->bind(bind_ip, port)) {
        return false;
    }
    if (!sock->listen(_backlog.load())) {
        return false;
    }
    if (!sock->setNonBlock(true)) {
        return false;
    }

    const int fd = sock->rawFD();
    if (_poller->addEvent(fd, EventPoller::EventRead, [this](int) { onAccept(); }) != 0) {
        ErrorP("TcpServer::start 注册 accept 事件失败 (fd=%d)", fd);
        sock->close();
        return false;
    }
    _port.store(sock->localPort());
    _listen_sock = sock;
    _listening.store(true);
    return true;
}

bool TcpServer::shutdown() {
    if (_shutdown_flag.exchange(true)) {
        return false;   // 幂等
    }
    if (_poller && !_poller->isCurrentThread()) {
        try {
            _poller->sync([this]() { shutdown(); });
        } catch (const std::exception &e) {
            ErrorP("TcpServer::shutdown 投递到轮询线程失败 (%s)：只做本地清理", e.what());
        }
        return true;
    }

    // 1) 先注销 listen fd：此后不会再进 onAccept（回调里的 this 因此安全）
    if (_listen_sock && _listen_sock->valid()) {
        const int fd = _listen_sock->rawFD();
        if (_poller->delEvent(fd, nullptr) != 0) {
            WarnP("TcpServer::shutdown delEvent(listen fd=%d) 未被受理", fd);
        }
        _listen_sock->close();
    }
    _listening.store(false);

    // 2) 逐个断开会话。先拷贝一份强引用：shutdown 会经 onSessionManager 改动 _sessions，
    //    直接边遍历边改会失效（这类错很隐蔽，而且只在关停时出现）
    std::vector<Session::Ptr> alive;
    for (const auto &weak : _sessions) {
        if (auto session = weak.lock()) {
            alive.push_back(session);
        }
    }
    _sessions.clear();
    for (const auto &session : alive) {
        session->shutdown(SockException(SockException::ErrType::Shutdown, 0, "服务器关闭"));
    }
    _session_count.store(0);
    return true;
}

bool TcpServer::listening() const {
    return _listening.load();
}

void TcpServer::onAccept() {
    if (_shutdown_flag.load() || !_listen_sock) {
        return;
    }
    for (;;) {   // ET：一直 accept 到 EAGAIN
        std::string peer_ip;
        uint16_t peer_port = 0;
        int err = 0;
        const int fd = _listen_sock->accept(&peer_ip, &peer_port, &err);
        if (fd < 0) {
            if (Socket::isEagain(err)) {
                break;   // 没有更多连接了：正常结束
            }
            _total_accept_error.fetch_add(1);
            ErrorP("TcpServer::onAccept accept 失败 (errno=%d %s)：可能是 fd 耗尽", err,
                   std::strerror(err));
            break;
        }

        const size_t limit = _max_session_count.load();
        if (limit > 0 && _session_count.load() >= limit) {
            _total_rejected.fetch_add(1);
            WarnP("TcpServer: 连接数已达上限 %zu，拒绝 %s:%u", limit, peer_ip.c_str(),
                  static_cast<unsigned>(peer_port));
            ::close(fd);   // 拒绝就要真的拒绝（不占着 fd，也不让对端误以为连上了）
            continue;      // 继续 accept：把本轮积压清干净，否则 ET 下不会再被通知
        }

        auto sock = std::make_shared<Socket>(fd);
        sock->setNoDelay(true);
        // FR-4.4 兜底：内核 KeepAlive（90s 内判死半开连接）。注意它**查不出对端进程卡死**，
        // 所以下面的应用层空闲检测仍然是必须的
        sock->setKeepAliveParams(60, 10, 3);

        Session::Ptr session;
        try {
            session = _creator(sock);
        } catch (const std::exception &e) {
            ErrorP("TcpServer: session creator 抛异常: %s", e.what());
        }
        if (!session) {
            ErrorP("TcpServer: session creator 返回空，关闭该连接 (%s:%u)", peer_ip.c_str(),
                   static_cast<unsigned>(peer_port));
            sock->close();
            continue;
        }

        // 超时统一注入（creator 之后，见头文件说明）
        session->setRecvIdleTimeout(_recv_idle_ms.load());
        session->setSendBlockedTimeout(_send_blocked_ms.load());
        session->setOnManager([this](const Session::Ptr &s, bool is_add) { onSessionManager(s, is_add); });

        _sessions.push_back(session);   // weak_ptr：不持强引用
        _session_count.fetch_add(1);
        _total_accepted.fetch_add(1);
        if (!session->start()) {
            // start 失败时 Session 自己已经走完关闭流程 → onSessionManager(false)
            // 已经把它摘表 + 减计数，这里不需要重复处理
            ErrorP("TcpServer: session start 失败 (%s:%u)，连接已关闭", peer_ip.c_str(),
                   static_cast<unsigned>(peer_port));
        }
    }
}

void TcpServer::onSessionManager(const Session::Ptr &session, bool is_add) {
    if (!session) {
        return;
    }
    if (is_add) {
        return;   // 建立时已经在表里了（我们在 start 之前就 push，保证 start 失败也能摘掉）
    }
    // 摘表：顺手清掉所有已失效的 weak_ptr（连接太多时这是免费的清理时机）
    _sessions.remove_if([&session](const std::weak_ptr<Session> &weak) {
        auto locked = weak.lock();
        return !locked || locked == session;
    });
    const size_t now = _session_count.load();
    if (now > 0) {
        _session_count.fetch_sub(1);
    }
    countCloseReason(session->lastError());
    if (_on_session_close) {
        try {
            _on_session_close(session);
        } catch (const std::exception &e) {
            ErrorP("TcpServer::onSessionClose 回调抛异常: %s", e.what());
        }
    }
}

void TcpServer::countCloseReason(const SockException &err) {
    switch (err.type()) {
    case SockException::ErrType::Timeout:
        _total_idle_timeout.fetch_add(1);
        break;
    case SockException::ErrType::RecvOverflow:
        _total_recv_overflow.fetch_add(1);
        break;
    case SockException::ErrType::SendOverflow:
        _total_send_overflow.fetch_add(1);
        break;
    case SockException::ErrType::Rejected:
        _total_rejected.fetch_add(1);
        break;
    case SockException::ErrType::AcceptError:
        _total_accept_error.fetch_add(1);
        break;
    default:
        break;   // 正常关闭 / 对端关闭：不计数（它们不是"异常断开"）
    }
}

bool TcpServer::setSessionCreator(SessionCreator creator) {
    if (_listening.load()) {
        WarnP("TcpServer::setSessionCreator 在 start() 之后调用：不生效");
        return false;
    }
    if (!creator) {
        ErrorP("TcpServer::setSessionCreator 收到空工厂：拒绝（否则连上来也没人处理数据）");
        return false;
    }
    _creator = std::move(creator);
    return true;
}

bool TcpServer::setBacklog(int backlog) {
    if (_listening.load() || backlog <= 0) {
        WarnP("TcpServer::setBacklog(%d) 未生效（已 start 或值非法）", backlog);
        return false;
    }
    _backlog.store(backlog);
    return true;
}

bool TcpServer::setMaxSessionCount(size_t max) {
    if (_listening.load()) {
        WarnP("TcpServer::setMaxSessionCount 在 start() 之后调用：不生效");
        return false;
    }
    if (max == 0) {
        // 0 不是"不限"：有界性不可协商（同 EventPoller::setMaxPendingTasks(0)）
        ErrorP("TcpServer::setMaxSessionCount(0) 被拒：0 会被误读成「不限」，"
               "而有界性不允许关掉（默认 %zu）", kDefaultMaxSessionCount);
        return false;
    }
    _max_session_count.store(max);
    return true;
}

size_t TcpServer::maxSessionCount() const {
    return _max_session_count.load();
}

bool TcpServer::setSessionTimeout(uint32_t recv_idle_ms, uint32_t send_blocked_ms) {
    if (_listening.load()) {
        WarnP("TcpServer::setSessionTimeout 在 start() 之后调用：不生效");
        return false;
    }
    _recv_idle_ms.store(recv_idle_ms);
    _send_blocked_ms.store(send_blocked_ms);
    return true;
}

bool TcpServer::setReusePort(bool enable) {
    if (_listening.load()) {
        WarnP("TcpServer::setReusePort 在 start() 之后调用：不生效");
        return false;
    }
    _reuse_port.store(enable);
    return true;
}

uint16_t TcpServer::port() const {
    return _port.load();
}

size_t TcpServer::sessionCount() const {
    return _session_count.load();
}

uint64_t TcpServer::totalAccepted() const {
    return _total_accepted.load();
}

uint64_t TcpServer::totalRejected() const {
    return _total_rejected.load();
}

uint64_t TcpServer::totalIdleTimeout() const {
    return _total_idle_timeout.load();
}

uint64_t TcpServer::totalRecvOverflow() const {
    return _total_recv_overflow.load();
}

uint64_t TcpServer::totalSendOverflow() const {
    return _total_send_overflow.load();
}

uint64_t TcpServer::totalAcceptError() const {
    return _total_accept_error.load();
}

const EventPoller::Ptr &TcpServer::poller() const {
    return _poller;
}

TcpServer::SessionCloseCB TcpServer::setOnSessionClose(SessionCloseCB cb) {
    return std::exchange(_on_session_close, std::move(cb));
}

size_t TcpServer::forEachSession(const std::function<void(const Session::Ptr &)> &cb) const {
    if (!cb) {
        return 0;
    }
    auto walk = [this, &cb]() -> size_t {
        size_t count = 0;
        for (const auto &weak : _sessions) {
            if (auto session = weak.lock()) {
                cb(session);
                ++count;
            }
        }
        return count;
    };
    if (_poller && !_poller->isCurrentThread()) {
        try {
            return _poller->sync(walk);
        } catch (const std::exception &e) {
            ErrorP("TcpServer::forEachSession 投递到轮询线程失败: %s", e.what());
            return 0;
        }
    }
    return walk();
}

} // namespace mzmedia
