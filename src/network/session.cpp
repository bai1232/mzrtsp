/*
 * Session 实现（M2-3b）
 * ============================================================================
 * 四条不变式（写的时候按这个顺序想，就不容易出错）：
 *   1) **self-hold**：事件回调捕获 shared_from_this()；shutdown 用 delEvent 断环。
 *      所以任何"回调里删自己"的场景都必须走延迟删除（EventPoller §4.1）。
 *   2) **读路径**：一次事件必须读到 EAGAIN（ET），但要带上限；hit_limit / eof 必须被处理，
 *      否则要么内存被对端控制、要么连接永久假死。
 *   3) **写路径**：EPOLLOUT 的挂/摘只收敛在 updateEpollOut()，且"部分写"要消费已写部分。
 *   4) **关闭路径**：幂等；delEvent 被拒也必须 close(fd)（NFR-6 不泄漏 fd）。
 * ============================================================================
 */

#include "network/session.h"

#include "core/logger.h"
#include "core/util.h"

#include <algorithm>
#include <cerrno>
#include <utility>

namespace mzmedia {

namespace {
/// 单个读事件里最多交付几次（每次 ≤ maxRecvBuffer）：默认 1MB × 64 = 64MB/事件。
/// 它同时是"对端灌得太凶"的判定阈值（超过就按 RecvOverflow 断开 + 计数）
constexpr int kMaxReadsPerEvent = 64;

/// 关闭前最多推迟几个空闲检查周期：留给"把已排队的数据发完"。
/// 为什么需要：读空闲超时/对端 EOF 时若发送队列还有数据，立刻断开就是**静默丢数据**；
/// 但也不能无限推迟（慢客户端会永远挂着），所以有上限。
constexpr int kMaxCloseDefer = 3;

/// 空闲检查周期：clamp(max(recv_idle, send_blocked) / 10, 1s, 30s)
uint32_t idleCheckPeriod(uint32_t recv_idle_ms, uint32_t send_blocked_ms) {
    const uint32_t max_ms = std::max(recv_idle_ms, send_blocked_ms);
    if (max_ms == 0) {
        return 0;   // 两个方向都关闭检测：不起定时器
    }
    return std::max<uint32_t>(1000, std::min<uint32_t>(30000, max_ms / 10));
}
} // namespace

Session::Session(const Socket::Ptr &sock, const EventPoller::Ptr &poller)
    : _sock(sock), _poller(poller), _send_queue(std::make_shared<Buffer>()) {}

Session::~Session() = default;

// ---------------------------------------------------------------------------
// 钩子注册
// ---------------------------------------------------------------------------

Session::onReadCB Session::setOnRead(onReadCB cb) {
    return std::exchange(_on_read, std::move(cb));
}

Session::onErrorCB Session::setOnError(onErrorCB cb) {
    return std::exchange(_on_error, std::move(cb));
}

Session::onManagerCB Session::setOnManager(onManagerCB cb) {
    return std::exchange(_on_manager, std::move(cb));
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool Session::start() {
    if (!_sock || !_sock->valid() || !_poller) {
        ErrorP("Session::start 参数非法（socket/poller 为空）");
        return false;
    }
    if (!_poller->isCurrentThread()) {
        // 跨线程：投递到 poller 线程再启动（事件必须注册在连接自己那条线程上）
        try {
            return _poller->sync([this]() { return start(); });
        } catch (const std::exception &e) {
            ErrorP("Session::start 投递到轮询线程失败: %s", e.what());
            return false;
        }
    }
    if (_closing.load()) {
        WarnP("Session::start 在已关闭的会话上调用：忽略");
        return false;
    }

    const uint64_t now = getCurrentMillisecond();
    _last_recv_ms.store(now);
    _last_send_ms.store(now);

    auto self = shared_from_this();   // ★ 自持环：fd 注册期间 Session 不会被析构
    const int fd = _sock->rawFD();
    if (_poller->addEvent(fd, EventPoller::EventRead, [this, self](int event) { onEvent(event); }) != 0) {
        ErrorP("Session::start 注册读事件失败 (fd=%d)", fd);
        // 不留"半启动"的连接：直接走关闭流程（onError + onManager(false) 都会发出去）
        shutdownImpl(SockException(SockException::ErrType::Shutdown, 0, "注册读事件失败"));
        return false;
    }
    _event = EventPoller::EventRead;

    if (!startIdleChecker()) {
        WarnP("Session::start 空闲检查定时器未启动（poller 可能正在退出）");
    }
    if (_on_manager) {
        _on_manager(self, true);
    }
    return true;
}

bool Session::shutdown(const SockException &err) {
    if (_closing.exchange(true)) {
        return false;   // 幂等：之前已经关过
    }
    if (_poller && !_poller->isCurrentThread()) {
        try {
            _poller->sync([this, err]() { shutdownImpl(err); });
        } catch (const std::exception &e) {
            // 投递失败（poller 已退出）：**仍然必须 close(fd)**，否则 fd 泄漏
            ErrorP("Session::shutdown 投递到轮询线程失败 (%s)：直接关闭 fd", e.what());
            if (_sock) {
                _sock->close();
            }
            return false;
        }
        return true;
    }
    shutdownImpl(err);
    return true;
}

void Session::shutdownImpl(const SockException &err) {
    _close_reason = err;

    // 1) 先注销事件，并且**等注销完成后才 close(fd)**。
    //    踩过的坑：delEvent 是"延迟删除"（本轮 epoll 批次处理完才真正摘掉），
    //    若在这里立刻 close(fd)，内核会马上把这个 fd 号复用给下一条连接，
    //    而 poller 里旧条目还在 → 新连接的 addEvent 被"重复注册"拒绝 → 连接全废。
    //    这正是 EventPoller::delEvent 注释里要求"等 complete_cb 再 close"的原因。
    const int fd = _sock ? _sock->rawFD() : -1;
    if (_poller && fd >= 0) {
        Socket::Ptr sock = _sock;   // 保活到回调执行
        if (_poller->delEvent(fd, [sock](bool success) {
                if (!success) {
                    WarnP("Session: delEvent 的 complete_cb 报告失败 (fd=%d)", sock->rawFD());
                }
                sock->close();   // ★ 确认摘掉之后才关：fd 号不会被别人抢走
            }) != 0) {
            // poller 正在退出等：投递不进去，只能就地关掉（否则这个 fd 就泄漏了）
            WarnP("Session::shutdown delEvent 未被受理 (fd=%d)：就地关闭 fd", fd);
            sock->close();
        }
    } else if (_sock) {
        _sock->close();
    }

    // 2) 停空闲检查 + 释放发送队列（内存要还回去，别等对象析构）
    if (_idle_task) {
        _idle_task->cancel();
        _idle_task.reset();
    }
    // 「发完再关」的截止定时器也要停：否则关闭后还会回调一次（白跑一趟 + 计数污染）
    if (_flush_deadline_task) {
        _flush_deadline_task->cancel();
        _flush_deadline_task.reset();
    }
    _flush_close_pending.store(false);
    _send_queue->release();
    _pending_send_bytes.store(0);

    // 3) 注意：**不在这里 close(fd)** —— 已经交给上面的 complete_cb 或就地回退分支处理。
    //    若 poller 在回调执行前退出（退出清理会丢弃待触发的 complete_cb），
    //    Socket 的析构函数仍会兜底 close（Socket::close 幂等，不会误关被复用的 fd）。

    // 4) 通知：先 onError（此时会话还在服务器表里），再 onManager(false)（摘表 + 分类计数）
    onError(_close_reason);
    if (_on_manager) {
        try {
            _on_manager(shared_from_this(), false);
        } catch (const std::exception &e) {
            ErrorP("Session::onManager(false) 回调抛异常: %s", e.what());
        }
    }
}

void Session::emitError(const SockException &err) {
    if (_closing.exchange(true)) {
        return;   // 已经在关闭：不重复通知
    }
    // 统一在这里记日志：子类覆盖 onError 也不会丢掉"有人报过错"这件事
    ErrorP("Session[%s:%u] 关闭：%s (errno=%d)",
           _sock ? _sock->peerIP().c_str() : "-", static_cast<unsigned>(_sock ? _sock->peerPort() : 0),
           err.typeName(), err.errCode());
    shutdownImpl(err);
}

bool Session::isShutdown() const {
    return _closing.load();
}

const SockException &Session::lastError() const {
    return _close_reason;
}

// ---------------------------------------------------------------------------
// 事件
// ---------------------------------------------------------------------------

void Session::onEvent(int event) {
    if (_closing.load()) {
        return;   // 正在关闭：不再处理任何事件
    }
    if ((event & EventPoller::EventError) != 0) {
        emitError(SockException(SockException::ErrType::RecvFailed, 0, "epoll 报告错误事件"));
        return;
    }
    // 先写后读：先把积压发出去腾出空间，再收新数据（对回显/代理类服务更友好）
    if ((event & EventPoller::EventWrite) != 0) {
        onWriteEvent();
        if (_closing.load()) {
            return;
        }
    }
    if ((event & EventPoller::EventRead) != 0) {
        onReadEvent();
    }
}

void Session::onReadEvent() {
    if (_closing.load()) {
        return;
    }

    // 每个读事件可能要多读几轮：maxRecvBuffer 是**单次交付上限**（一次 onRecv 最多
    // 这么多字节），不是"连接累计上限"。ET 模式要求一次事件读到 EAGAIN，所以读满一块
    // 就必须接着读 —— 否则 8MB 的正常传输会被误判成攻击（这个坑是跑验收脚本时踩到的：
    // 8MB 回显被 1MB 上限切成了 recv-overflow）。
    const size_t cap = _max_recv_buffer.load();
    bool eof = false;
    for (int reads = 0; reads < kMaxReadsPerEvent; ++reads) {
        // 读写流控（echo/代理类服务的命门）：发送队列积压到高水位就先别收，
        // 否则"收得多、发得慢"会把积压全转成我们的内存，一个事件就能顶爆发送上限。
        // 恢复由 onWriteEvent 在降到低水位时做（通过 modifyEvent 重新挂 EPOLLIN）。
        if (_pending_send_bytes.load() >= _max_send_buffer.load() / 2) {
            setReadPaused(true);
            return;
        }
        // 每个 onRecv 一块新 Buffer：所有权交给协议层，协议层可以留存（不能复用同一个）
        Buffer::Ptr buf = std::make_shared<Buffer>();
        bool hit_limit = false;
        int err = 0;
        const ssize_t n = buf->readFromFd(_sock->rawFD(), cap, &eof, &hit_limit, &err);
        if (n < 0) {
            emitError(SockException(SockException::ErrType::RecvFailed, err, "读失败"));
            return;
        }
        if (n == 0) {
            if (eof) {
                // 对端半关闭（它可能还在等我们的回显）：队列里还有数据就先别断，
                // 交给空闲检查去收尾（有上限）。否则立刻按 PeerClosed 关闭。
                _read_eof = true;
                if (_pending_send_bytes.load() > 0 && _idle_task) {
                    _close_defer = 0;
                } else {
                    emitError(SockException(SockException::ErrType::PeerClosed, 0, "对端关闭"));
                }
            }
            return;   // 否则是 EAGAIN：本轮读空了（ET 的正确结果）
        }

        _bytes_in.fetch_add(static_cast<uint64_t>(n));
        _last_recv_ms.store(getCurrentMillisecond());
        _close_defer = 0;   // 还有数据进来 = 有进展，推迟计数归零
        onRecv(buf);   // 所有权移交；可能在这里被关闭，所以之后只做"是否结束"的判断
        if (_closing.load()) {
            return;
        }
        if (!hit_limit) {
            return;   // socket 已读空
        }
    }
    // 单事件里连续读满 kMaxReadsPerEvent 次仍未读空：对端灌得比我们能处理的快得多，
    // 按安全网断开（并让 TcpServer 的 totalRecvOverflow 计数）
    emitError(SockException(SockException::ErrType::RecvOverflow, 0, "单事件接收量超限"));
}

void Session::onWriteEvent() {
    if (_closing.load()) {
        return;
    }
    // **ET 下必须一直写到 EAGAIN**（或写空）：只写一次的话，如果这次没写空又没到 EAGAIN，
    // 就不会再有 EPOLLOUT 通知 → 尾部数据永远发不出去（表现是"回显少了几百 KB"，
    // 直到空闲超时把连接关掉）。这是与读路径对称的要求，漏掉过一次，被验收脚本抓到。
    for (;;) {
        if (_send_queue->empty()) {
            updateEpollOut(false);   // 没有积压：摘掉 EPOLLOUT（挂摘必须配对）
            if (_read_paused) {
                setReadPaused(false);   // 队列空了：恢复收（也让内核缓冲里的数据继续进来）
            }
            // 「发完再关」（M6-b）：队列刚排空 —— 这就是关连接最好的时机
            // （事件驱动，不必等空闲检查的周期，那个周期可能是 6 秒）
            // 注意：它可能在这里发起 shutdown，所以调用之后立刻返回，不再碰本对象
            maybeCloseAfterFlush();
            return;
        }
        if (_read_paused && _pending_send_bytes.load() < _max_send_buffer.load() / 4) {
            setReadPaused(false);   // 降到低水位：恢复收
        }
        int err = 0;
        const ssize_t n = _send_queue->writeToFd(_sock->rawFD(), &err);
        if (n < 0) {
            emitError(SockException(SockException::ErrType::SendFailed, err, "写失败"));
            return;
        }
        if (n == 0) {
            return;   // EAGAIN：写不动了，EPOLLOUT 仍挂着，等下次通知
        }
        _bytes_out.fetch_add(static_cast<uint64_t>(n));
        _pending_send_bytes.fetch_sub(static_cast<size_t>(n));
        _last_send_ms.store(getCurrentMillisecond());
    }
}

bool Session::updateEpollOut(bool enable) {
    const int want = enable ? (_event | EventPoller::EventWrite) : (_event & ~EventPoller::EventWrite);
    if (want == _event) {
        return true;   // 已经是目标状态：不折腾 epoll_ctl
    }
    if (_poller->modifyEvent(_sock->rawFD(), want, nullptr) != 0) {
        // 改不动就断：绝不能停在"以为挂了 EPOLLOUT 其实没挂"的状态（数据永远发不出去）
        ErrorP("Session::updateEpollOut modifyEvent 失败 (fd=%d, want=%d)", _sock->rawFD(), want);
        emitError(SockException(SockException::ErrType::SendFailed, 0, "modifyEvent 失败"));
        return false;
    }
    _event = want;
    return true;
}

bool Session::setReadPaused(bool paused) {
    if (_read_paused == paused) {
        return true;
    }
    const int want = paused ? (_event & ~EventPoller::EventRead) : (_event | EventPoller::EventRead);
    if (_poller->modifyEvent(_sock->rawFD(), want, nullptr) != 0) {
        ErrorP("Session::setReadPaused modifyEvent 失败 (fd=%d, paused=%d)", _sock->rawFD(), paused);
        emitError(SockException(SockException::ErrType::RecvFailed, 0, "流控 modifyEvent 失败"));
        return false;
    }
    _event = want;
    _read_paused = paused;
    return true;
}

// ---------------------------------------------------------------------------
// 发送
// ---------------------------------------------------------------------------

ssize_t Session::send(const void *data, size_t len) {
    if (data == nullptr || len == 0) {
        return 0;
    }
    if (_closing.load()) {
        return -1;   // 已关闭：明确失败，不假装成功
    }
    if (!_poller->isCurrentThread()) {
        try {
            return _poller->sync([this, data, len]() { return send(data, len); });
        } catch (const std::exception &e) {
            ErrorP("Session::send 投递到轮询线程失败: %s", e.what());
            return -1;
        }
    }
    if (_closing.load()) {
        return -1;
    }

    // 队列非空时**绝不能**直写 socket：socket 是 FIFO，新数据会插到队列里旧数据的前面
    // → 客户端收到块级乱序（字节数不变、内容错位）。由 ntimed_send_order_with_backlog
    // 确定性复现：首个 'B' 出现在 16KB 处（= 发送缓冲大小）而不是 1MB 处。
    ssize_t sent = 0;
    if (_send_queue->empty()) {
        sent = _sock->send(data, len);
        if (sent < 0) {
            if (Socket::isEagain(errno)) {
                sent = 0;   // EAGAIN 不是错误：转成"这次写出 0 字节"，余下进队列
            } else {
                emitError(SockException(SockException::ErrType::SendFailed, errno, "send 失败"));
                return -1;
            }
        }
    }

    const size_t rest = len - static_cast<size_t>(sent);
    if (rest > 0) {
        // 先判上限再入队：入队后再判等于先把内存涨上去再报警
        if (_send_queue->size() + rest > _max_send_buffer.load()) {
            emitError(SockException(SockException::ErrType::SendOverflow, 0, "发送队列超限"));
            return sent;   // 已写出的部分照实返回（数据没丢，只是连接要断）
        }
        _send_queue->append(static_cast<const char *>(data) + sent, rest);
        _pending_send_bytes.fetch_add(rest);
        updateEpollOut(true);
    }
    if (sent > 0) {
        _bytes_out.fetch_add(static_cast<uint64_t>(sent));
        _last_send_ms.store(getCurrentMillisecond());
    }
    return sent;
}

ssize_t Session::send(const std::string &data) {
    return send(data.data(), data.size());
}

// ---------------------------------------------------------------------------
// 空闲检测（FR-4.4）
// ---------------------------------------------------------------------------

bool Session::startIdleChecker() {
    const uint32_t period = idleCheckPeriod(_recv_idle_ms.load(), _send_blocked_ms.load());
    if (period == 0) {
        return true;   // 两个方向都关闭检测：合法配置
    }
    _idle_period_ms = period;
    _idle_task = _poller->doDelayTask(period, [this]() -> uint64_t { return checkIdle(); });
    if (!_idle_task) {
        ErrorP("Session: 空闲检查定时器创建失败（poller 已退出？）");
        return false;
    }
    return true;
}

uint64_t Session::checkIdle() {
    if (_closing.load()) {
        return 0;   // 已关闭：不再重复
    }
    const uint64_t now = getCurrentMillisecond();
    const uint32_t recv_idle = _recv_idle_ms.load();
    const uint32_t send_blocked = _send_blocked_ms.load();
    const bool draining = _pending_send_bytes.load() > 0;   // 还有排队数据没发完

    // 读侧已经 EOF（对端半关闭）：把回显发完再断，最多推迟 kMaxCloseDefer 个周期
    if (_read_eof) {
        if (!draining || _close_defer >= kMaxCloseDefer) {
            emitError(SockException(SockException::ErrType::PeerClosed, 0, "对端关闭"));
            return 0;
        }
        ++_close_defer;
        return _idle_period_ms;
    }

    if (recv_idle > 0 && now - _last_recv_ms.load() >= recv_idle) {
        // 读空闲超时：队列里还有数据就先把它们发出去（同样只推迟有限次），
        // 否则"读空闲"会顺手把没发完的数据丢掉 —— 静默丢数据（§4.5）
        if (draining && _close_defer < kMaxCloseDefer) {
            ++_close_defer;
            return _idle_period_ms;
        }
        onIdle(recv_idle);   // 默认实现 = 关闭；协议层可重写为发心跳
        if (_closing.load()) {
            return 0;
        }
    }
    // 写阻塞：只有"还有积压且很久没写成功"时才算（否则空闲连接会被误杀）
    if (send_blocked > 0 && draining && now - _last_send_ms.load() >= send_blocked) {
        onIdle(send_blocked);   // 慢客户端：这里**不推迟**，推迟就没意义了
        if (_closing.load()) {
            return 0;
        }
    }
    return _idle_period_ms;   // 继续周期性检查
}

// ---------------------------------------------------------------------------
// 默认钩子实现
// ---------------------------------------------------------------------------

void Session::onRecv(const Buffer::Ptr &buf) {
    if (_on_read) {
        _on_read(buf);
        return;
    }
    // 既没设回调也没重写：不能静默丢数据（§4.5），报错并关闭
    ErrorP("Session::onRecv 未被重写且未设置 onRead 回调：丢弃 %zu 字节并关闭连接",
           buf ? buf->size() : 0);
    shutdown(SockException(SockException::ErrType::RecvFailed, 0, "onRecv 未实现"));
}

void Session::onError(const SockException &err) {
    if (_on_error) {
        try {
            _on_error(err);
        } catch (const std::exception &e) {
            ErrorP("Session::onError 回调抛异常: %s", e.what());
        }
    }
}

void Session::onIdle(uint32_t idle_ms) {
    const char *what = (idle_ms == _recv_idle_ms.load()) ? "读空闲超时" : "写阻塞超时";
    shutdown(SockException(SockException::ErrType::Timeout, 0, what));
}

// ---------------------------------------------------------------------------
// 配置 / 观测
// ---------------------------------------------------------------------------

bool Session::setRecvIdleTimeout(uint32_t ms) {
    if (_idle_task) {
        WarnP("Session::setRecvIdleTimeout 在 start() 之后调用：不生效（检查周期已建立）");
        return false;
    }
    _recv_idle_ms.store(ms);
    return true;
}

bool Session::setSendBlockedTimeout(uint32_t ms) {
    if (_idle_task) {
        WarnP("Session::setSendBlockedTimeout 在 start() 之后调用：不生效");
        return false;
    }
    _send_blocked_ms.store(ms);
    return true;
}

bool Session::setMaxRecvBuffer(size_t bytes) {
    if (bytes == 0) {
        // 0 不是"不限"，而是把安全网关掉：拒绝（有界性不可协商）
        ErrorP("Session::setMaxRecvBuffer(0) 被拒：0 表示关闭上限，而有界性不允许关掉");
        return false;
    }
    _max_recv_buffer.store(bytes);
    return true;
}

bool Session::setMaxSendBuffer(size_t bytes) {
    if (bytes == 0) {
        ErrorP("Session::setMaxSendBuffer(0) 被拒：0 表示关闭上限，而有界性不允许关掉");
        return false;
    }
    _max_send_buffer.store(bytes);
    return true;
}

uint32_t Session::recvIdleTimeout() const {
    return _recv_idle_ms.load();
}

uint32_t Session::sendBlockedTimeout() const {
    return _send_blocked_ms.load();
}

size_t Session::maxRecvBuffer() const {
    return _max_recv_buffer.load();
}

size_t Session::maxSendBuffer() const {
    return _max_send_buffer.load();
}

const EventPoller::Ptr &Session::poller() const {
    return _poller;
}

Socket::Ptr Session::socket() const {
    return _sock;
}

int Session::fd() const {
    return _sock ? _sock->rawFD() : -1;
}

std::string Session::peerIP() const {
    return _sock ? _sock->peerIP() : std::string();
}

uint16_t Session::peerPort() const {
    return _sock ? _sock->peerPort() : 0;
}

uint64_t Session::bytesIn() const {
    return _bytes_in.load();
}

uint64_t Session::bytesOut() const {
    return _bytes_out.load();
}

uint64_t Session::lastRecvMs() const {
    const uint64_t last = _last_recv_ms.load();
    return last == 0 ? 0 : getCurrentMillisecond() - last;
}

uint64_t Session::lastSendMs() const {
    const uint64_t last = _last_send_ms.load();
    return last == 0 ? 0 : getCurrentMillisecond() - last;
}

size_t Session::pendingSendBytes() const {
    // 读原子镜像，**不要**直接读 _send_queue（Buffer 不是线程安全的）
    return _pending_send_bytes.load();
}

// ---------------------------------------------------------------------------
// 「发完再关」（M6-b）
// ---------------------------------------------------------------------------

bool Session::shutdownAfterFlush(uint32_t max_wait_ms) {
    if (max_wait_ms == 0) {
        // 0 不等于无界：没有截止时刻就会永远挂着（有界性不可协商）
        WarnP("Session::shutdownAfterFlush 被拒：max_wait_ms 不能为 0");
        return false;
    }
    if (_closing.load()) {
        return false; // 已经在关，不用再排
    }

    bool ok = false;
    try {
        _poller->sync([this, max_wait_ms, &ok] {
            if (_closing.load() || _flush_close_pending.exchange(true)) {
                ok = false;
                return;
            }
            _flush_close_deadline_ms.store(getCurrentMillisecond() + max_wait_ms);
            // 截止时刻用**专用一次性定时器**：不能指望空闲检查的周期（可能是 6 秒）
            _flush_deadline_task = _poller->doDelayTask(max_wait_ms, [this]() -> uint64_t {
                onFlushDeadline();
                return 0;
            });
            if (!_flush_deadline_task) {
                WarnP("Session::shutdownAfterFlush：截止定时器创建失败（poller 已退出）→ 立即关闭");
                _flush_close_pending.store(false);
                shutdown(SockException(SockException::ErrType::None, 0, "发完再关（定时器失败）"));
                ok = false;
                return;
            }
            ok = true;
            // 队列可能本来就是空的（例如 FLV 的最后一块已经写完）→ 立刻推进一次
            maybeCloseAfterFlush();
        });
    } catch (const std::exception &e) {
        ErrorP("Session::shutdownAfterFlush 投递失败：%s", e.what());
        return false;
    }
    return ok;
}

uint64_t Session::flushCloseTimeoutCount() const {
    return _flush_close_timeout_count.load();
}

void Session::maybeCloseAfterFlush() {
    if (!_flush_close_pending.load() || _closing.load()) {
        return;
    }
    if (_pending_send_bytes.load() > 0) {
        return; // 还有排队数据：等下一次写事件（或截止定时器）
    }
    _flush_close_pending.store(false);
    if (_flush_deadline_task) {
        _flush_deadline_task->cancel();
        _flush_deadline_task.reset();
    }
    shutdown(SockException(SockException::ErrType::None, 0, "发完再关"));
}

void Session::onFlushDeadline() {
    if (!_flush_close_pending.load() || _closing.load()) {
        return;
    }
    // 超时：强制关，但**吵出来**（计数 + 日志）—— 静默丢尾部数据不可接受
    ++_flush_close_timeout_count;
    WarnP("Session: 「发完再关」超时（仍有 %zu 字节未写出）→ 强制关闭",
          _pending_send_bytes.load());
    _flush_close_pending.store(false);
    shutdown(SockException(SockException::ErrType::SendFailed, 0, "发完再关超时"));
}

} // namespace mzmedia
