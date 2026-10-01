/*
 * PipeWrap：跨线程唤醒用的管道
 * ============================================================================
 * 用途：EventPoller 阻塞在 epoll_wait 时，其它线程要投递任务给它，必须有个东西
 *       能把 epoll_wait 叫醒。pipe 的读端注册进 epoll，写端用来通知。
 *
 * 决策(v0.1)：用 pipe2(O_NONBLOCK | O_CLOEXEC) 而不是 eventfd。
 *   eventfd 少占 1 个 fd，但 pipe 能顺便携带少量负载（未来若要传任务 id 不必改结构）。
 *   排除条件变量：它唤不醒 epoll_wait。
 * ============================================================================
 */

#pragma once

#include <cstddef>

namespace mzmedia {

class PipeWrap {
public:
    PipeWrap();
    ~PipeWrap();

    PipeWrap(const PipeWrap &) = delete;
    PipeWrap &operator=(const PipeWrap &) = delete;

    /// 读端 fd（注册到 epoll 用的是它）；创建失败时为 -1
    int readFD() const { return _fds[0]; }
    /// 写端 fd（notify 用的是它）
    int writeFD() const { return _fds[1]; }
    bool valid() const { return _fds[0] >= 0 && _fds[1] >= 0; }

    /**
     * 写 1 字节唤醒对端
     * @return 1 = 成功；-1 = 失败（errno 已记录到日志，EAGAIN 视为成功，因为缓冲区已满说明通知已生效）
     */
    int notify();

    /**
     * 把管道里积累的字节读空
     * @return 本次读出的字节数
     * @note 必须读空：读端用 ET 注册时，残留数据会吞掉后续通知
     */
    size_t drain();

private:
    int _fds[2];
};

} // namespace mzmedia
