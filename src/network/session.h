/*
 * Session：连接生命周期 + 收发 + 空闲检测（M2-3b）
 * ============================================================================
 * 形状来源：docs/DESIGN_M2.md §3.6（接口）、§4.4（读路径）、§4.5（写路径）、
 *           §4.6（生命周期与 FR-4.4 超时）
 *
 * 分层：Session 是**协议无关**的连接壳子。分帧/解析由子类重写 onRecv 完成
 *       （M3 的 HttpSession 就这么长出来），所以它不认识 HTTP/FLV。
 *
 * 线程模型（§5）：除 send()/shutdown() 外，其余成员**只在 poller 线程访问**；
 *       send()/shutdown() 线程安全（内部 sync 投递到 poller 线程）。
 *
 * 生命周期（自持环）：事件回调捕获 `shared_from_this()` → 只要 fd 还注册着，Session
 *       就不会析构；shutdown() → delEvent(fd) → 回调释放 → 环断开 → 析构。
 *       必须配合 EventPoller 的**延迟删除**才安全（§4.1）。
 *
 * 返回值策略（尽量不用 void）：配置类 setter 返回"是否生效"（start() 之后再改、
 *       或值非法 → false + 日志）；状态类返回"本次是否真的改变了状态"；
 *       回调注册返回**被替换掉的上一个**；只有"被调用的钩子"保留 void（无接收方语义）。
 * ============================================================================
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "core/task_cancelable.h"
#include "network/buffer.h"
#include "network/event_poller.h"
#include "network/socket.h"

namespace mzmedia {

// 前向声明：`closeLogLevel()` 的返回值需要它。刻意**不**在这里 include `core/logger.h` ——
// 这个头被 `http/`、`network/` 广泛包含，为了一个枚举把日志实现拖进来不划算
// （`enum class` 指定了底层类型，前向声明与定义兼容）。
enum class LogLevel : int;

class Session : public std::enable_shared_from_this<Session> {
public:
    using Ptr = std::shared_ptr<Session>;
    using onReadCB = std::function<void(const Buffer::Ptr &buf)>;
    using onErrorCB = std::function<void(const SockException &err)>;
    /// is_add=true 表示会话已建立并 start 成功；false 表示已关闭（从连接表移除）
    using onManagerCB = std::function<void(const Session::Ptr &session, bool is_add)>;

    // ---- 默认值：全部是初值，先给保守值再按计数校准（AI_COLLAB §4.6）----
    static constexpr uint32_t kDefaultRecvIdleMs = 60000;     // FR-4.4 读空闲 60s
    static constexpr uint32_t kDefaultSendBlockedMs = 30000;   // FR-4.4/FR-5.2 写阻塞 30s
    static constexpr size_t kDefaultMaxRecvBuffer = 1u << 20;  // 1 MB 安全网
    static constexpr size_t kDefaultMaxSendBuffer = 8u << 20;  // 8 MB 安全网（FR-5.1 同量级）

    /**
     * @param sock   已 accept 的连接（非阻塞 / CLOEXEC 由 Socket::accept4 保证）
     * @param poller 该连接**一生绑定**的 poller（连接亲和：读写同线程、零锁）
     * @note 构造不碰 fd、不注册事件；真正开始收数据要等 start()
     */
    Session(const Socket::Ptr &sock, const EventPoller::Ptr &poller);
    virtual ~Session();

    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    // ------------------------------------------------------------------
    // 钩子注册（返回被替换掉的上一个；没有则为空 —— 便于保存/恢复/断言"没人设过"）
    // ------------------------------------------------------------------

    /// @note 子类**重写** onRecv 之后，这个回调不会再被调用（子类实现优先，无双分派歧义）
    onReadCB setOnRead(onReadCB cb);
    /// @note 基类 onError 一定会记 Error 日志；设置回调后额外通知一次
    onErrorCB setOnError(onErrorCB cb);
    onManagerCB setOnManager(onManagerCB cb);

    // ------------------------------------------------------------------
    // 生命周期
    // ------------------------------------------------------------------

    /**
     * 开始工作：注册读事件 + 启动空闲检查
     * @return true = 已启动；false = 参数非法 / poller 已退出 / 注册失败
     *         （失败时已记 Error 并自行收尾，不留下"半启动"的连接）
     * @note 可从任意线程调用（内部 sync）；TcpServer 的 factory 本来就在 poller 线程上
     */
    bool start();

    /**
     * 主动关闭：**幂等**
     * @return true = 本次真的发起了关闭；false = 之前已经关过（不是错误）
     * @param err 关闭原因（默认 None = 正常关闭）。分类计数与用例断言都依赖它
     * @note 顺序：标记 → delEvent(fd) → 停/清缓冲 → close(fd) → onError → onManager(false)
     * @note delEvent 被拒（poller 正在退出）时**仍然必须 close(fd)**，否则退出路径泄漏
     *       fd（NFR-6）；跨线程投递失败时也一样兜底 close
     */
    bool shutdown(const SockException &err = SockException());

    /**
     * 「发完再关」（M6-b）：等发送队列**排空**后再关闭连接
     * @param max_wait_ms 最长等多久；**0 被拒**（0 不等于无界）；超时则强制关 + 计数
     * @return false = 已在关闭流程 / 参数非法 / 投递失败（都会记日志）
     * @note 线程安全（内部 sync 投递到 poller 线程）
     * @note 用途：HTTP-FLV 播完后主动收尾；也可给任何"还有排队数据就关"的路径用
     *       （直接 shutdown 会把已入队的数据丢掉 = 静默丢数据）
     */
    bool shutdownAfterFlush(uint32_t max_wait_ms = 5000);
    /// 「发完再关」因超时被强制关闭的次数（观测）
    uint64_t flushCloseTimeoutCount() const;

    bool isShutdown() const;
    /// 关闭原因；**只在 poller 线程读**（TcpServer 的移除通知里、以及用例里）
    const SockException &lastError() const;

    /**
     * 【M6-d】关闭时该按什么级别记日志
     * @note 正常收尾（`None` / `PeerClosed` / `Shutdown`）→ `Info`；其余（超时 / 溢出 / send-recv 失败 / …）→ `Error`
     * @note 做成 **public static 纯函数**是为了**可测**：日志级别散落在 `emitError()` 里就没法断言，
     *       而"NFR-3 错误日志 0 条"这条验收完全依赖它（10 路客户端正常断开 = 10 条 Error 的话，
     *       这条需求就没法测了）。用例：`ntimed_session_close_log_level`
     */
    static LogLevel closeLogLevel(const SockException &err);

    // ------------------------------------------------------------------
    // 发送
    // ------------------------------------------------------------------

    /**
     * 发送数据
     * @return 本次**立即**写出的字节数（0..len；EAGAIN 记为 0，不是失败）；
     *         未写完的部分进发送队列，由 EPOLLOUT 继续 flush；
     *         -1 = 已经关闭 / 出错（错误已通过 onError 通知）
     * @note 线程安全：非 poller 线程会 sync 投递后返回该结果，
     *       因此返回值是"投递那一刻立即写出的量"，不代表最终送达
     * @note 发送队列超过 maxSendBuffer() → 以 SendOverflow 关闭（慢客户端）：
     *       只连不读的对端能让内存无限涨，**光靠 30s 时间兜底限制不了字节数**（§4.3）
     */
    ssize_t send(const void *data, size_t len);
    ssize_t send(const std::string &data);

    // ------------------------------------------------------------------
    // 配置（都应在 start() 之前设置；返回 false = 值未生效，已记日志）
    // ------------------------------------------------------------------

    bool setRecvIdleTimeout(uint32_t ms);    // 0 = 关闭该方向检测
    bool setSendBlockedTimeout(uint32_t ms);
    bool setMaxRecvBuffer(size_t bytes);     // 0 被拒（有界性不可协商）
    bool setMaxSendBuffer(size_t bytes);     // 0 被拒
    uint32_t recvIdleTimeout() const;
    uint32_t sendBlockedTimeout() const;
    size_t maxRecvBuffer() const;
    size_t maxSendBuffer() const;

    // ------------------------------------------------------------------
    // 观测（全部线程安全）
    // ------------------------------------------------------------------

    const EventPoller::Ptr &poller() const;
    Socket::Ptr socket() const;
    int fd() const;
    std::string peerIP() const;
    uint16_t peerPort() const;
    uint64_t bytesIn() const;
    uint64_t bytesOut() const;
    /// 距上次成功收到 / 写出数据的毫秒数（单调时钟）
    uint64_t lastRecvMs() const;
    uint64_t lastSendMs() const;
    /// 发送队列当前积压字节数（观测"慢客户端"）
    size_t pendingSendBytes() const;

protected:
    // ------------------------------------------------------------------
    // 子类重写点
    // ------------------------------------------------------------------

    /**
     * 收到数据（可能含半条消息，也可能一次到多条）——自己 find + consume
     * @param buf 本读事件新建的缓冲，**所有权移交给你**（可以留存；不要假设会被复用）
     *
     * 基类默认实现：设了 setOnRead 就转调；**没设也没重写 → ErrorP + 以 RecvFailed 关闭**。
     * 绝不静默丢数据（AI_COLLAB §4.5）。
     */
    virtual void onRecv(const Buffer::Ptr &buf);

    /// 出错 / 关闭通知。基类默认实现：转调 setOnError 回调（日志在 emitError 里统一打）
    virtual void onError(const SockException &err);

    /**
     * 空闲超时钩子（FR-4.4 + 心跳）
     * @param idle_ms 触发的是哪个阈值（recv_idle 或 send_blocked）
     * 基类默认实现 = shutdown(Timeout)。**协议层重写它就能把"关连接"换成"发心跳"**
     * （RTSP 用 OPTIONS/GET_PARAMETER、WebSocket 用 ping 帧）；网络层自己往连接里塞
     * 空包是非法流量，所以只给钩子。
     */
    virtual void onIdle(uint32_t idle_ms);

private:
    /// 事件总入口：**读写共用同一个回调**，必须在这里按位分发（漏掉写位 = EPOLLOUT 永远不处理）
    void onEvent(int event);
    /// 读事件（含 ET 循环与安全网判定）
    void onReadEvent();
    void onWriteEvent();
    /// 真正执行关闭（只在 poller 线程）
    void shutdownImpl(const SockException &err);
    /// 「发完再关」的推进：发送队列已排空就关；只在 poller 线程调用
    void maybeCloseAfterFlush();
    /// 「发完再关」超时：强制关 + 计数（定时器回调，poller 线程）
    void onFlushDeadline();
    /// 出错/关闭的唯一入口：幂等 + 记日志 + shutdownImpl + onError
    void emitError(const SockException &err);
    /// **挂/摘 EPOLLOUT 的唯一入口**：不配对会导致 LT 下 100% CPU 或 ET 下永远发不出去
    bool updateEpollOut(bool enable);
    /// 读写流控：发送队列积压超过高水位就**停止收数据**，降回低水位再恢复收
    /// （不收 = 让对端在内核缓冲里等着，而不是把内存转成我们的发送队列）
    bool setReadPaused(bool paused);
    bool startIdleChecker();
    /// 空闲检查任务体；返回值即"下次再查"的毫秒数（0 = 不再查）
    uint64_t checkIdle();

    // ---- 只在 poller 线程访问 ----
    Socket::Ptr _sock;
    EventPoller::Ptr _poller;
    Buffer::Ptr _send_queue;                 // 待写数据（复用同一个 Buffer）
    int _event = EventPoller::EventRead;     // 当前注册的事件位（EPOLLOUT 配对用）
    EventPoller::DelayTask::Ptr _idle_task;  // 空闲检查循环任务
    uint32_t _idle_period_ms = 0;            // 空闲检查周期
    bool _read_eof = false;                  // 读侧已 EOF（对端半关闭）
    bool _read_paused = false;               // 因发送队列积压而暂停收数据（流控）
    int _close_defer = 0;                    // 关闭已推迟的次数（把排队数据发完，有上限）
    onReadCB _on_read;
    onErrorCB _on_error;
    onManagerCB _on_manager;
    SockException _close_reason;             // 关闭原因（只在 poller 线程写；关闭后只读）

    // ---- 跨线程可读 ----
    std::atomic<bool> _closing{false};
    std::atomic<uint64_t> _bytes_in{0};
    std::atomic<uint64_t> _bytes_out{0};
    std::atomic<uint64_t> _last_recv_ms{0};
    std::atomic<uint64_t> _last_send_ms{0};
    std::atomic<uint32_t> _recv_idle_ms{kDefaultRecvIdleMs};
    std::atomic<uint32_t> _send_blocked_ms{kDefaultSendBlockedMs};
    std::atomic<size_t> _max_recv_buffer{kDefaultMaxRecvBuffer};
    std::atomic<size_t> _max_send_buffer{kDefaultMaxSendBuffer};
    /// 发送队列积压字节数（原子镜像：观测接口要能跨线程读，不能直接读 Buffer）
    std::atomic<size_t> _pending_send_bytes{0};

    /// 「发完再关」（M6-b）：待排空标记 + 截止时刻 + 超时计数
    std::atomic<bool> _flush_close_pending{false};
    std::atomic<uint64_t> _flush_close_deadline_ms{0};
    std::atomic<uint64_t> _flush_close_timeout_count{0};
    /// 截止时刻的定时器（只在 poller 线程创建/取消）
    EventPoller::DelayTask::Ptr _flush_deadline_task;
};

} // namespace mzmedia
