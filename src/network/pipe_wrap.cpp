#include "network/pipe_wrap.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>

#include "core/logger.h"

namespace mzmedia {

PipeWrap::PipeWrap() : _fds{-1, -1} {
    // O_CLOEXEC：exec 出来的子进程不继承（否则 fork+exec 后管道读端被占住）
    if (::pipe2(_fds, O_NONBLOCK | O_CLOEXEC) != 0) {
        _fds[0] = -1;
        _fds[1] = -1;
        ErrorP("PipeWrap: pipe2 创建失败 (errno=%d)", errno);
    }
}

PipeWrap::~PipeWrap() {
    if (_fds[0] >= 0) {
        ::close(_fds[0]);
    }
    if (_fds[1] >= 0) {
        ::close(_fds[1]);
    }
}

int PipeWrap::notify() {
    if (!valid()) {
        return -1;
    }
    const char payload = 'w';
    const ssize_t n = ::write(_fds[1], &payload, 1);
    if (n == 1) {
        return 1;
    }
    // EAGAIN 不是错误：管道缓冲区满，说明"已经有通知在排队"，对端一定会被唤醒
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return 1;
    }
    ErrorP("PipeWrap: 写唤醒管道失败 (errno=%d)", errno);
    return -1;
}

size_t PipeWrap::drain() {
    if (_fds[0] < 0) {
        return 0;
    }
    size_t total = 0;
    char buf[64];
    while (true) {
        const ssize_t n = ::read(_fds[0], buf, sizeof(buf));
        if (n > 0) {
            total += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;   // 被信号打断：重试，不算失败
        }
        break;          // EAGAIN（读空）或真正的错误
    }
    return total;
}

} // namespace mzmedia
