/*
 * Socket / SockException：fd 的 RAII 与系统调用封装（M2-3a）
 * ============================================================================
 * 形状来源：docs/DESIGN_M2.md §3.2（错误模型）与 §3.4（fd RAII + 系统调用封装）
 *
 * 分层：本文件**不认识 EventPoller**（不知道 epoll、不知道线程），只做 fd 的
 *       生命周期与 syscall 封装。事件注册由 Session/TcpServer 通过 poller 完成，
 *       这样 M3 的 TcpClient（回源 / RTSP 拉流）能直接复用。
 *
 * 失败一律返回 bool / -1 + errno，并在**内部记日志**：调用方不需要解析 errno，
 * 但也绝不会"什么都不知道地继续跑"（AI_COLLAB §4.1）。
 *
 * 返回值策略（尽量不用 void）：本类所有有意义的操作都有返回值 —— 连 close() 都返回
 *   "本次是否真的关掉了 fd"（幂等语义可判定）。只有析构函数是 void（语言要求）。
 * ============================================================================
 */

#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>

namespace mzmedia {

/**
 * 连接级错误
 *
 * 为什么除了 errno 还要一个 ErrType：
 *   FR-4.4 的四类断开（读空闲超时 / 写阻塞 / 接收缓冲超限 / 连接数被拒）在 errno 里
 *   都是 0 或者笼统的 ETIMEDOUT，**光靠 errno 无法分类**；而 TcpServer 的四个计数
 *   （totalIdleTimeout / totalRecvOverflow / totalRejected / totalAcceptError）与
 *   §7 的用例断言（"onError 为超时"）都需要"原因本身"。
 */
class SockException : public std::runtime_error {
public:
    enum class ErrType {
        None = 0,       // 正常关闭（本地 shutdown 或对端 EOF）
        PeerClosed,     // 对端关闭/挂断（read 返回 0，或 epoll 报 EPOLLHUP/EPOLLERR）
        Timeout,        // 读空闲 / 写阻塞超时（FR-4.4）
        RecvOverflow,   // 接收缓冲超限被断开
        SendOverflow,   // 发送队列超限（慢客户端，FR-5.2 的队列上限）
        Rejected,       // 连接数上限被拒（FR-4.4）
        AcceptError,    // accept 失败（EMFILE/ENFILE 等）
        SendFailed,     // send 出错
        RecvFailed,     // recv 出错
        Shutdown,       // 本地正在/已经关闭
    };

    /// 只用 errno 构造（ErrType = None）。刻意**不给 err_code 默认值**：
    /// 否则 `SockException()` 会在两个构造函数之间歧义（SockException() 走下面那个）
    explicit SockException(int err_code, const std::string &msg = "");
    SockException(ErrType type = ErrType::None, int err_code = 0, const std::string &msg = "");

    ErrType type() const;
    /// 日志可读名（"timeout" / "recv-overflow" / ...）
    const char *typeName() const;
    int errCode() const;   // errno；0 = 无（自定义错误）
    bool isEof() const;    // 对端正常关闭

private:
    ErrType _type;
    int _err_code;
};

/**
 * Socket：单个 fd 的 RAII 容器 + syscall 封装
 *
 * 线程模型：**只在 poller 线程使用**（accept/read/write 都在事件回调里）；
 *           `close()` 也应在 poller 线程调用（Session::shutdown 负责这件事）。
 * SIGPIPE：`send()` 一律带 MSG_NOSIGNAL，因此**不需要**在进程里忽略 SIGPIPE。
 *
 * @note "对端消失"的 errno 判定在 `core/util.h` 的 `isPeerGoneErrno()`（M6-d：
 *       `Socket` / `Buffer` / `Session` 三处都要用，放这里会让低层被迫包含本头）。
 */
class Socket {
public:
    using Ptr = std::shared_ptr<Socket>;
    enum class Domain { IPv4 = AF_INET, IPv6 = AF_INET6 };

    /// @param fd 接管所有权（析构时 close）；-1 表示空对象
    explicit Socket(int fd = -1);
    ~Socket();
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    Socket(Socket &&other) noexcept;
    Socket &operator=(Socket &&other) noexcept;

    int rawFD() const;
    bool valid() const;
    /// @return true = 本次真的关闭了 fd；false = 本来就是无效对象，或 close 真失败（已记日志）
    /// @note 幂等：重复调用返回 false（"这次没关东西"），不是错误
    bool close();

    // ------------------------------------------------------------------
    // 选项（全部返回 bool：失败已记日志；调用方按需处理，不静默）
    // ------------------------------------------------------------------

    bool setNonBlock(bool enable);
    bool setNoDelay(bool enable);                                        // TCP_NODELAY
    bool setReuseAddr(bool enable);
    bool setReusePort(bool enable);
    bool setKeepAlive(bool enable);
    /// Linux TCP_KEEPIDLE / TCP_KEEPINTVL / TCP_KEEPCNT（任一参数 <= 0 → 拒绝 + 日志）
    /// 兜底防"半开连接"泄漏 fd（NFR-3/NFR-6）。注意内核探测**查不出对端进程卡死**，
    /// 所以应用层空闲检测仍然是必须的（DESIGN_M2 §4.6）
    bool setKeepAliveParams(int idle_sec, int interval_sec, int count);
    bool setSendBufSize(int bytes);   // bytes <= 0 → 拒绝
    bool setRecvBufSize(int bytes);

    // ------------------------------------------------------------------
    // 服务端：bind / listen / accept
    // ------------------------------------------------------------------

    bool bind(const std::string &ip, uint16_t port);
    bool listen(int backlog);   // backlog <= 0 → 拒绝

    /**
     * 接受一个连接
     * @param peer_ip / peer_port 非空时写出对端地址
     * @param err 非空时写出 errno
     * @return >=0 新连接的 fd（**调用方负责接管所有权**，通常立刻包成 Socket::Ptr）；
     *         -1 失败。EAGAIN（没有更多连接了）与真错误都返回 -1，
     *         **必须靠 *err 区分**：ET 下要一直 accept 到 EAGAIN（同 §4.4 的读路径）
     * @note 用 accept4 带上 SOCK_NONBLOCK | SOCK_CLOEXEC：新连接默认非阻塞，
     *       避免"忘了设非阻塞 → 读到阻塞住整个事件循环"这类经典事故
     */
    int accept(std::string *peer_ip = nullptr, uint16_t *peer_port = nullptr, int *err = nullptr);

    // ------------------------------------------------------------------
    // 数据收发
    // ------------------------------------------------------------------

    /// @return >0 已发送字节数；-1 出错（*err 带 errno；**EAGAIN 不是错误**，
    ///         调用方用 isEagain 判断后去等 EPOLLOUT）
    ssize_t send(const void *data, size_t len, int flags = 0);
    /// @return >0 收到的字节数；0 = 对端关闭（EOF）；-1 出错（同样要区分 EAGAIN）
    ssize_t recv(void *buf, size_t len, int flags = 0);

    // ------------------------------------------------------------------
    // 地址
    // ------------------------------------------------------------------

    std::string localIP() const;
    uint16_t localPort() const;
    std::string peerIP() const;
    uint16_t peerPort() const;

    /// 创建 socket（已带 CLOEXEC；**非阻塞由调用方显式 setNonBlock**，
    /// 因为 bind/listen 的顺序与是否阻塞在不同场景下不一样）
    /// @return nullptr = 失败（已记 Error）
    static Socket::Ptr create(Domain domain, int type = SOCK_STREAM);

    /// EAGAIN / EWOULDBLOCK / EINTR —— 它们**不是错误**，不得包成 SockException（§3.2）
    static bool isEagain(int err);

private:
    int _fd;
};

} // namespace mzmedia
