/*
 * Socket / SockException 实现（M2-3a）
 * ============================================================================
 * 约定（写在实现里，避免调用方猜）：
 *   - EAGAIN / EWOULDBLOCK / EINTR 不是错误：**不打 Error 日志**（否则一个慢客户端
 *     就能刷爆日志），由调用方用 isEagain() 判断后去等事件；
 *   - 其余失败一律 Error/Warn + 返回失败；
 *   - send() 带 MSG_NOSIGNAL（否则对端断开时 write 会送 SIGPIPE 把进程干掉）；
 *   - accept 用 accept4 直接带上 NONBLOCK|CLOEXEC。
 * ============================================================================
 */

#include "network/socket.h"

#include "core/logger.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

namespace mzmedia {

namespace {

std::string makeErrMsg(const std::string &msg, int err_code) {
    if (!msg.empty()) {
        return msg;
    }
    return "socket error (errno=" + std::to_string(err_code) + ")";
}

bool setIntOpt(int fd, int level, int opt, int value, const char *name) {
    if (::setsockopt(fd, level, opt, &value, sizeof(value)) != 0) {
        WarnP("Socket::%s 失败 (fd=%d, errno=%d %s)", name, fd, errno, std::strerror(errno));
        return false;
    }
    return true;
}

std::string addrToIp(const struct sockaddr_storage &ss) {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (ss.ss_family == AF_INET) {
        const auto *in4 = reinterpret_cast<const struct sockaddr_in *>(&ss);
        if (::inet_ntop(AF_INET, &in4->sin_addr, buf, sizeof(buf)) == nullptr) {
            return {};
        }
        return buf;
    }
    if (ss.ss_family == AF_INET6) {
        const auto *in6 = reinterpret_cast<const struct sockaddr_in6 *>(&ss);
        if (::inet_ntop(AF_INET6, &in6->sin6_addr, buf, sizeof(buf)) == nullptr) {
            return {};
        }
        return buf;
    }
    return {};
}

uint16_t addrToPort(const struct sockaddr_storage &ss) {
    if (ss.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<const struct sockaddr_in *>(&ss)->sin_port);
    }
    if (ss.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const struct sockaddr_in6 *>(&ss)->sin6_port);
    }
    return 0;
}

/// 把 (ip, port) 填成 sockaddr；同时给出长度。返回 false = 地址不合法
bool fillAddr(const std::string &ip, uint16_t port, struct sockaddr_storage *ss, socklen_t *len) {
    std::memset(ss, 0, sizeof(*ss));
    struct sockaddr_in in4;
    std::memset(&in4, 0, sizeof(in4));
    if (::inet_pton(AF_INET, ip.c_str(), &in4.sin_addr) == 1) {
        in4.sin_family = AF_INET;
        in4.sin_port = htons(port);
        std::memcpy(ss, &in4, sizeof(in4));
        *len = sizeof(in4);
        return true;
    }
    struct sockaddr_in6 in6;
    std::memset(&in6, 0, sizeof(in6));
    if (::inet_pton(AF_INET6, ip.c_str(), &in6.sin6_addr) == 1) {
        in6.sin6_family = AF_INET6;
        in6.sin6_port = htons(port);
        std::memcpy(ss, &in6, sizeof(in6));
        *len = sizeof(in6);
        return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// SockException
// ---------------------------------------------------------------------------

SockException::SockException(int err_code, const std::string &msg)
    : std::runtime_error(makeErrMsg(msg, err_code)), _type(ErrType::None), _err_code(err_code) {}

SockException::SockException(ErrType type, int err_code, const std::string &msg)
    : std::runtime_error(makeErrMsg(msg, err_code)), _type(type), _err_code(err_code) {}

SockException::ErrType SockException::type() const {
    return _type;
}

const char *SockException::typeName() const {
    switch (_type) {
    case ErrType::None: return "none";
    case ErrType::PeerClosed: return "peer-closed";
    case ErrType::Timeout: return "timeout";
    case ErrType::RecvOverflow: return "recv-overflow";
    case ErrType::SendOverflow: return "send-overflow";
    case ErrType::Rejected: return "rejected";
    case ErrType::AcceptError: return "accept-error";
    case ErrType::SendFailed: return "send-failed";
    case ErrType::RecvFailed: return "recv-failed";
    case ErrType::Shutdown: return "shutdown";
    }
    return "unknown";
}

int SockException::errCode() const {
    return _err_code;
}

bool SockException::isEof() const {
    return _type == ErrType::PeerClosed;
}

// ---------------------------------------------------------------------------
// Socket
// ---------------------------------------------------------------------------

Socket::Socket(int fd) : _fd(fd) {}

Socket::~Socket() {
    close();   // 析构不能返回错误：内部已记日志
}

Socket::Socket(Socket &&other) noexcept : _fd(other._fd) {
    other._fd = -1;
}

Socket &Socket::operator=(Socket &&other) noexcept {
    if (this != &other) {
        close();
        _fd = other._fd;
        other._fd = -1;
    }
    return *this;
}

int Socket::rawFD() const {
    return _fd;
}

bool Socket::valid() const {
    return _fd >= 0;
}

bool Socket::close() {
    if (_fd < 0) {
        return false;   // 幂等：这次没关掉任何东西
    }
    const int fd = _fd;
    _fd = -1;
    if (::close(fd) != 0 && errno != EINTR) {
        // Linux 上 close 即使在 EINTR 时也已关闭 fd，因此不重试（重试会关错对象）
        WarnP("Socket::close 失败 (fd=%d, errno=%d %s)", fd, errno, std::strerror(errno));
        return false;
    }
    return true;
}

bool Socket::setNonBlock(bool enable) {
    if (!valid()) {
        return false;
    }
    const int flags = ::fcntl(_fd, F_GETFL, 0);
    if (flags < 0) {
        WarnP("Socket::setNonBlock fcntl(F_GETFL) 失败 (fd=%d, errno=%d)", _fd, errno);
        return false;
    }
    const int want = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(_fd, F_SETFL, want) != 0) {
        WarnP("Socket::setNonBlock fcntl(F_SETFL) 失败 (fd=%d, errno=%d)", _fd, errno);
        return false;
    }
    return true;
}

bool Socket::setNoDelay(bool enable) {
    return valid() && setIntOpt(_fd, IPPROTO_TCP, TCP_NODELAY, enable ? 1 : 0, "setNoDelay");
}

bool Socket::setReuseAddr(bool enable) {
    return valid() && setIntOpt(_fd, SOL_SOCKET, SO_REUSEADDR, enable ? 1 : 0, "setReuseAddr");
}

bool Socket::setReusePort(bool enable) {
#ifdef SO_REUSEPORT
    return valid() && setIntOpt(_fd, SOL_SOCKET, SO_REUSEPORT, enable ? 1 : 0, "setReusePort");
#else
    WarnP("Socket::setReusePort 本平台不支持 SO_REUSEPORT");
    (void) enable;
    return false;
#endif
}

bool Socket::setKeepAlive(bool enable) {
    return valid() && setIntOpt(_fd, SOL_SOCKET, SO_KEEPALIVE, enable ? 1 : 0, "setKeepAlive");
}

bool Socket::setKeepAliveParams(int idle_sec, int interval_sec, int count) {
    if (!valid()) {
        return false;
    }
    if (idle_sec <= 0 || interval_sec <= 0 || count <= 0) {
        ErrorP("Socket::setKeepAliveParams 参数非法 (idle=%d interval=%d count=%d)：拒绝执行",
               idle_sec, interval_sec, count);
        return false;
    }
#ifdef TCP_KEEPIDLE
    bool ok = setIntOpt(_fd, SOL_SOCKET, SO_KEEPALIVE, 1, "setKeepAlive");
    ok = setIntOpt(_fd, IPPROTO_TCP, TCP_KEEPIDLE, idle_sec, "setKeepAliveParams(idle)") && ok;
    ok = setIntOpt(_fd, IPPROTO_TCP, TCP_KEEPINTVL, interval_sec, "setKeepAliveParams(interval)") && ok;
    ok = setIntOpt(_fd, IPPROTO_TCP, TCP_KEEPCNT, count, "setKeepAliveParams(count)") && ok;
    return ok;
#else
    ErrorP("Socket::setKeepAliveParams 本平台不支持 TCP_KEEPIDLE/KEEPINTVL/KEEPCNT");
    return false;
#endif
}

bool Socket::setSendBufSize(int bytes) {
    if (bytes <= 0) {
        ErrorP("Socket::setSendBufSize 收到非正数 %d：拒绝执行", bytes);
        return false;
    }
    return valid() && setIntOpt(_fd, SOL_SOCKET, SO_SNDBUF, bytes, "setSendBufSize");
}

bool Socket::setRecvBufSize(int bytes) {
    if (bytes <= 0) {
        ErrorP("Socket::setRecvBufSize 收到非正数 %d：拒绝执行", bytes);
        return false;
    }
    return valid() && setIntOpt(_fd, SOL_SOCKET, SO_RCVBUF, bytes, "setRecvBufSize");
}

bool Socket::bind(const std::string &ip, uint16_t port) {
    if (!valid()) {
        return false;
    }
    struct sockaddr_storage ss;
    socklen_t len = 0;
    if (!fillAddr(ip, port, &ss, &len)) {
        ErrorP("Socket::bind 地址非法: %s", ip.c_str());
        return false;
    }
    if (::bind(_fd, reinterpret_cast<struct sockaddr *>(&ss), len) != 0) {
        ErrorP("Socket::bind 失败 (%s:%u, errno=%d %s)", ip.c_str(), port, errno, std::strerror(errno));
        return false;
    }
    return true;
}

bool Socket::listen(int backlog) {
    if (!valid()) {
        return false;
    }
    if (backlog <= 0) {
        ErrorP("Socket::listen 收到非正数 backlog=%d：拒绝执行", backlog);
        return false;
    }
    if (::listen(_fd, backlog) != 0) {
        ErrorP("Socket::listen 失败 (fd=%d, backlog=%d, errno=%d %s)", _fd, backlog, errno,
               std::strerror(errno));
        return false;
    }
    return true;
}

int Socket::accept(std::string *peer_ip, uint16_t *peer_port, int *err) {
    if (!valid()) {
        if (err != nullptr) {
            *err = EBADF;
        }
        return -1;
    }
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    const int new_fd = ::accept4(_fd, reinterpret_cast<struct sockaddr *>(&ss), &len,
                                 SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (new_fd < 0) {
        if (err != nullptr) {
            *err = errno;
        }
        if (!isEagain(errno)) {
            // EAGAIN 不是错误、且会反复出现，所以不打日志（避免刷屏）
            ErrorP("Socket::accept 失败 (fd=%d, errno=%d %s)", _fd, errno, std::strerror(errno));
        }
        return -1;
    }
    if (peer_ip != nullptr) {
        *peer_ip = addrToIp(ss);
    }
    if (peer_port != nullptr) {
        *peer_port = addrToPort(ss);
    }
    if (err != nullptr) {
        *err = 0;
    }
    return new_fd;
}

ssize_t Socket::send(const void *data, size_t len, int flags) {
    if (!valid() || data == nullptr) {
        return -1;
    }
    // MSG_NOSIGNAL：对端已关闭时 write 会送 SIGPIPE，默认动作是**干掉整个进程**
    const ssize_t n = ::send(_fd, data, len, flags | MSG_NOSIGNAL);
    if (n < 0 && !isEagain(errno)) {
        // EAGAIN 不是错误（调用方要去等 EPOLLOUT），不能打日志：否则一个慢客户端就能刷屏
        ErrorP("Socket::send 失败 (fd=%d, errno=%d %s)", _fd, errno, std::strerror(errno));
    }
    return n;
}

ssize_t Socket::recv(void *buf, size_t len, int flags) {
    if (!valid() || buf == nullptr) {
        return -1;
    }
    const ssize_t n = ::recv(_fd, buf, len, flags);
    if (n < 0 && !isEagain(errno)) {
        ErrorP("Socket::recv 失败 (fd=%d, errno=%d %s)", _fd, errno, std::strerror(errno));
    }
    return n;
}

std::string Socket::localIP() const {
    if (!valid()) {
        return {};
    }
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (::getsockname(_fd, reinterpret_cast<struct sockaddr *>(&ss), &len) != 0) {
        return {};
    }
    return addrToIp(ss);
}

uint16_t Socket::localPort() const {
    if (!valid()) {
        return 0;
    }
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (::getsockname(_fd, reinterpret_cast<struct sockaddr *>(&ss), &len) != 0) {
        return 0;
    }
    return addrToPort(ss);
}

std::string Socket::peerIP() const {
    if (!valid()) {
        return {};
    }
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (::getpeername(_fd, reinterpret_cast<struct sockaddr *>(&ss), &len) != 0) {
        return {};
    }
    return addrToIp(ss);
}

uint16_t Socket::peerPort() const {
    if (!valid()) {
        return 0;
    }
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (::getpeername(_fd, reinterpret_cast<struct sockaddr *>(&ss), &len) != 0) {
        return 0;
    }
    return addrToPort(ss);
}

Socket::Ptr Socket::create(Domain domain, int type) {
    const int fd = ::socket(static_cast<int>(domain), type | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        ErrorP("Socket::create 失败 (domain=%d, type=%d, errno=%d %s)", static_cast<int>(domain),
               type, errno, std::strerror(errno));
        return nullptr;
    }
    return std::make_shared<Socket>(fd);
}

bool Socket::isEagain(int err) {
    return err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
}

} // namespace mzmedia
