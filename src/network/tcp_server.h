/*
 * TcpServer：监听 + accept + 会话表（M2-3b）
 * ============================================================================
 * 形状来源：docs/DESIGN_M2.md §3.7（接口）、§4.6（FR-4.4 的四类计数）、§5（线程模型）
 *
 * 连接亲和：accept 在**本服务器自己的 poller** 上完成，新连接默认也留在该 poller
 *           （一个连接一生在同一个线程 → 读写零锁）。
 *           "把新连接分散到别的 poller"是 §10 未决事项，等 M2-3 实测多核扩展性再定。
 *
 * 所有权：会话表用 **weak_ptr**（不持强引用），强引用来自 Session 自己的事件回调
 *         （§4.6 自持环）。所以"会话泄漏"和"服务器持着会话导致释放不掉"这两类问题
 *         在结构上就不存在。
 *
 * 生命周期硬约束：**TcpServer 必须先于它的会话销毁**。shutdown()（析构会自动调用）：
 *   ① 注销 listen fd（此后不会再有 accept 回调）；
 *   ② 逐个 shutdown 会话（在 poller 线程上跑完收尾，摘表 + 分类计数）。
 *   内部回调捕获 `this`，所以这个"关停即排水"的顺序是安全性的前提，不是可选优化（§8 R2/R9）。
 *
 * 返回值策略（尽量不用 void）：配置类 setter 返回"是否生效"（start() 之后/非法值 → false）；
 *   注册回调返回**被替换掉的上一个**；forEachSession 返回遍历到的会话数。
 * ============================================================================
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>

#include "network/event_poller.h"
#include "network/session.h"
#include "network/socket.h"

namespace mzmedia {

class TcpServer {
public:
    using Ptr = std::shared_ptr<TcpServer>;
    /// 工厂在 **poller 线程**上执行，因此内部可直接用 EventPoller::getCurrentPoller()
    using SessionCreator = std::function<Session::Ptr(const Socket::Ptr &)>;
    using SessionCloseCB = std::function<void(const Session::Ptr &)>;

    /// FR-4.4 的连接上限初值；**0 不是"不限"**，见 setMaxSessionCount
    static constexpr size_t kDefaultMaxSessionCount = 64;
    static constexpr int kDefaultBacklog = 1024;

    /// @param poller 空 = 从 EventPollerPool 取一个（多 Reactor 的一条腿）
    explicit TcpServer(const EventPoller::Ptr &poller = nullptr);
    /// 析构会调用 shutdown()（见文件头"生命周期硬约束"）
    ~TcpServer();

    TcpServer(const TcpServer &) = delete;
    TcpServer &operator=(const TcpServer &) = delete;

    // ------------------------------------------------------------------
    // 启停
    // ------------------------------------------------------------------

    /**
     * 建 socket → bind → listen → 注册 accept 事件
     * @param port    0 = 由内核分配（之后用 port() 读回，测试友好）
     * @param bind_ip 默认监听所有网卡
     * @return true = 已开始监听；false = 失败（**已记日志，不抛异常**，调用方必须检查）
     * @note 没设置 setSessionCreator 时**明确失败**：否则会"能连上但数据被丢"，
     *       那是最难查的一类问题（AI_COLLAB §4.5）
     */
    bool start(uint16_t port, const std::string &bind_ip = "0.0.0.0");

    /// 停止监听并断开所有会话
    /// @return true = 本次真的执行了关停；false = 之前已经关过（幂等）
    bool shutdown();
    bool listening() const;

    // ------------------------------------------------------------------
    // 配置（返回 false = 未生效：已 start / 值非法；均已记日志）
    // ------------------------------------------------------------------

    bool setSessionCreator(SessionCreator creator);
    bool setBacklog(int backlog);
    /**
     * 连接数上限（FR-4.4，默认 64）
     * @note 传 0 会被**拒绝**：有界性是硬要求，不能通过配置把上限关掉
     *       （同 EventPoller::setMaxPendingTasks(0) 的处理）
     */
    bool setMaxSessionCount(size_t max);
    size_t maxSessionCount() const;
    /// FR-4.4：读空闲 / 写阻塞超时（毫秒，0 = 关闭该方向检测）。
    /// 这两个值由 TcpServer 在 creator 返回后**统一注入**每个新会话
    /// （所以不要在 creator 里改它们，会被覆盖；想按会话微调就改缓冲上限）
    bool setSessionTimeout(uint32_t recv_idle_ms, uint32_t send_blocked_ms);
    bool setReusePort(bool enable);

    // ------------------------------------------------------------------
    // 观测（全部线程安全）
    // ------------------------------------------------------------------

    uint16_t port() const;
    size_t sessionCount() const;
    uint64_t totalAccepted() const;
    /// FR-4.4：因连接数上限被拒的次数
    uint64_t totalRejected() const;
    /// FR-4.4：因读空闲 / 写阻塞超时被断开的次数
    uint64_t totalIdleTimeout() const;
    uint64_t totalRecvOverflow() const;
    /// 发送队列超限被断开的次数（慢客户端：字节级防线，区别于 30s 的时间级防线）
    uint64_t totalSendOverflow() const;
    /// accept 失败次数（EMFILE/ENFILE 等；fd 耗尽的信号）
    uint64_t totalAcceptError() const;
    const EventPoller::Ptr &poller() const;

    // ------------------------------------------------------------------
    // 会话遍历 / 通知
    // ------------------------------------------------------------------

    /// @return 被替换掉的上一个回调（便于保存/恢复）
    SessionCloseCB setOnSessionClose(SessionCloseCB cb);
    /// 可从任意线程调用（内部 sync 到 poller 线程遍历）
    /// @return 遍历到的会话数
    size_t forEachSession(const std::function<void(const Session::Ptr &)> &cb) const;

private:
    /// 轮询线程上的 accept 循环（ET：一直 accept 到 EAGAIN）
    void onAccept();
    /// 会话表增删 + 按关闭原因分桶计数（在 poller 线程）
    void onSessionManager(const Session::Ptr &session, bool is_add);
    void countCloseReason(const SockException &err);

    EventPoller::Ptr _poller;
    Socket::Ptr _listen_sock;          // 监听 fd（poller 线程访问）
    SessionCreator _creator;
    SessionCloseCB _on_session_close;

    // ---- 跨线程可读 ----
    std::atomic<bool> _listening{false};
    std::atomic<bool> _shutdown_flag{false};
    std::atomic<uint16_t> _port{0};
    std::atomic<int> _backlog{kDefaultBacklog};
    std::atomic<size_t> _max_session_count{kDefaultMaxSessionCount};
    std::atomic<uint32_t> _recv_idle_ms{Session::kDefaultRecvIdleMs};
    std::atomic<uint32_t> _send_blocked_ms{Session::kDefaultSendBlockedMs};
    std::atomic<bool> _reuse_port{false};
    std::atomic<size_t> _session_count{0};
    std::atomic<uint64_t> _total_accepted{0};
    std::atomic<uint64_t> _total_rejected{0};
    std::atomic<uint64_t> _total_idle_timeout{0};
    std::atomic<uint64_t> _total_recv_overflow{0};
    std::atomic<uint64_t> _total_send_overflow{0};
    std::atomic<uint64_t> _total_accept_error{0};

    /// 会话表：**weak_ptr**，只在 poller 线程访问
    std::list<std::weak_ptr<Session>> _sessions;
};

} // namespace mzmedia
