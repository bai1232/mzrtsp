/*
 * Buffer 实现（M2-3a）
 * ============================================================================
 * 三个容易写错的地方，都在这里定死：
 *   1) 扩容顺序：**先 compact 再扩容** —— 否则"读游标一直前移、容量只增不减"；
 *   2) consume 越界：**不改数据 + 返回 0 + ErrorP**，绝不"按 size() 截断"；
 *   3) readFromFd 的停止条件必须带字节上限，且要把"没读完"通过 hit_limit 说出来
 *      （ET 模式下不会再有通知，见 DESIGN_M2 §8 R1）。
 * ============================================================================
 */

#include "network/buffer.h"

#include "core/logger.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace mzmedia {

namespace {
/// 单次 read() 的块大小：太小会 syscall 爆炸，太大则每次都要预分配大块内存
constexpr size_t kReadChunk = 64 * 1024;
} // namespace

Buffer::Buffer(size_t initial_capacity) {
    if (initial_capacity > 0) {
        _buf.resize(initial_capacity);
    }
}

Buffer::Buffer(Buffer &&other) noexcept
    : _buf(std::move(other._buf)), _read_pos(other._read_pos), _write_pos(other._write_pos) {
    other._read_pos = 0;
    other._write_pos = 0;
}

Buffer &Buffer::operator=(Buffer &&other) noexcept {
    if (this != &other) {
        _buf = std::move(other._buf);
        _read_pos = other._read_pos;
        _write_pos = other._write_pos;
        other._read_pos = 0;
        other._write_pos = 0;
    }
    return *this;
}

Buffer::~Buffer() = default;

const char *Buffer::data() const {
    return _buf.data() + _read_pos;
}

size_t Buffer::size() const {
    return _write_pos - _read_pos;
}

size_t Buffer::capacity() const {
    return _buf.size();
}

bool Buffer::empty() const {
    return size() == 0;
}

size_t Buffer::readPos() const {
    return _read_pos;
}

std::string Buffer::toString() const {
    return std::string(data(), size());
}

void Buffer::compact() {
    if (_read_pos == 0) {
        return;
    }
    const size_t remaining = size();
    if (remaining > 0) {
        std::memmove(_buf.data(), _buf.data() + _read_pos, remaining);
    }
    _read_pos = 0;
    _write_pos = remaining;
}

void Buffer::ensureWritable(size_t want) {
    if (capacity() - _write_pos >= want) {
        return;
    }
    // 先 compact：把读游标前移留下的前导空洞收回来（这是"容量不会单调增长"的关键）
    if (_read_pos > 0) {
        compact();
    }
    if (capacity() - _write_pos >= want) {
        return;
    }
    const size_t need = _write_pos + want;
    const size_t grown = std::max(need, capacity() * 2);   // 翻倍增长：摊还 O(1)
    _buf.resize(grown);
}

size_t Buffer::append(const void *data, size_t len) {
    if (data == nullptr || len == 0) {
        return 0;
    }
    ensureWritable(len);
    std::memcpy(_buf.data() + _write_pos, data, len);
    _write_pos += len;
    return len;
}

size_t Buffer::append(const std::string &str) {
    return append(str.data(), str.size());
}

size_t Buffer::reserve(size_t capacity) {
    if (capacity > _buf.size()) {
        _buf.resize(capacity);
    }
    return _buf.size();
}

size_t Buffer::consume(size_t len) {
    if (len == 0) {
        return 0;
    }
    if (len > size()) {
        // 调用方解析逻辑算错了：明确拒绝，不改数据（静默截断会让后续一直错位）
        ErrorP("Buffer::consume 越界：请求 %zu 字节，实际只有 %zu 字节（未改动任何数据）",
               len, size());
        return 0;
    }
    _read_pos += len;
    if (_read_pos == _write_pos) {
        _read_pos = 0;   // 消费干净：游标直接归零，省掉一次 compact
        _write_pos = 0;
    } else if (_read_pos >= _buf.size() / 2) {
        compact();       // 前导空洞超过一半才搬，摊还成本低
    }
    return len;
}

size_t Buffer::clear() {
    const size_t discarded = size();
    _read_pos = 0;
    _write_pos = 0;
    return discarded;
}

size_t Buffer::release() {
    const size_t freed = _buf.size();
    _buf.clear();
    _buf.shrink_to_fit();
    _read_pos = 0;
    _write_pos = 0;
    return freed;
}

size_t Buffer::find(char ch, size_t from) const {
    if (from >= size()) {
        return npos;
    }
    const char *begin = data() + from;
    const void *hit = std::memchr(begin, ch, size() - from);
    if (hit == nullptr) {
        return npos;
    }
    return static_cast<size_t>(static_cast<const char *>(hit) - data());
}

size_t Buffer::find(const void *needle, size_t needle_len, size_t from) const {
    if (needle == nullptr || needle_len == 0) {
        return from <= size() ? from : npos;
    }
    if (from >= size() || needle_len > size() - from) {
        return npos;
    }
    const char *hay = data();
    const size_t last = size() - needle_len;   // 最后一个可能的起点
    for (size_t i = from; i <= last; ++i) {
        if (hay[i] == static_cast<const char *>(needle)[0] &&
            std::memcmp(hay + i, needle, needle_len) == 0) {
            return i;
        }
    }
    return npos;
}

bool Buffer::startWith(const void *prefix, size_t len) const {
    if (len == 0) {
        return true;
    }
    if (prefix == nullptr || len > size()) {
        return false;
    }
    return std::memcmp(data(), prefix, len) == 0;
}

bool Buffer::endWith(const void *suffix, size_t len) const {
    if (len == 0) {
        return true;
    }
    if (suffix == nullptr || len > size()) {
        return false;
    }
    return std::memcmp(data() + size() - len, suffix, len) == 0;
}

ssize_t Buffer::readFromFd(int fd, size_t max_bytes, bool *hit_limit, int *err) {
    if (hit_limit != nullptr) {
        *hit_limit = false;
    }
    if (max_bytes == 0) {
        // 非法调用：放行会变成"永远读不完"的忙等，必须显式拒绝
        ErrorP("Buffer::readFromFd 收到 max_bytes == 0 的非法调用（拒绝执行）");
        if (hit_limit != nullptr) {
            *hit_limit = true;
        }
        if (err != nullptr) {
            *err = EINVAL;
        }
        return -1;
    }

    size_t total = 0;
    while (total < max_bytes) {
        const size_t want = std::min(kReadChunk, max_bytes - total);
        ensureWritable(want);
        const ssize_t n = ::read(fd, _buf.data() + _write_pos, want);
        if (n > 0) {
            _write_pos += static_cast<size_t>(n);
            total += static_cast<size_t>(n);
            continue;   // ET：必须一直读到 EAGAIN
        }
        if (n == 0) {
            // 对端关闭（EOF）：先把已读到的交给调用方，下次调用再返回 0
            return static_cast<ssize_t>(total);
        }
        if (errno == EINTR) {
            continue;   // 被信号打断：不是错误
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return static_cast<ssize_t>(total);   // 读空了：正常结束（不是错误）
        }
        // 真错误：要吵，不能"读到了一些就当成功"（那会把连接错误吞掉）
        if (err != nullptr) {
            *err = errno;
        }
        ErrorP("Buffer::readFromFd read() 失败 (fd=%d, errno=%d %s)", fd, errno, std::strerror(errno));
        return -1;
    }

    // 达到上限而停下：socket 里可能还有数据，必须让调用方知道（ET 下不会再有通知）
    if (hit_limit != nullptr) {
        *hit_limit = true;
    }
    return static_cast<ssize_t>(total);
}

ssize_t Buffer::writeToFd(int fd, int *err) {
    if (empty()) {
        return 0;
    }
    ssize_t n = -1;
    for (;;) {
        n = ::write(fd, data(), size());
        if (n >= 0 || errno != EINTR) {
            break;
        }
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;   // 写不出去：不是错误，等 EPOLLOUT
        }
        if (err != nullptr) {
            *err = errno;
        }
        ErrorP("Buffer::writeToFd write() 失败 (fd=%d, errno=%d %s)", fd, errno, std::strerror(errno));
        return -1;
    }
    // 部分写：消费已写出的部分，余下留在缓冲里等下次 EPOLLOUT
    const size_t written = static_cast<size_t>(n);
    const size_t consumed = consume(std::min(written, size()));
    if (consumed != written) {
        // 不该发生：说明 size() 在 write 期间变了（并发/别名）。吵出来，别装作没事
        ErrorP("Buffer::writeToFd 消费量与写出量不一致：写出 %zu，消费 %zu", written, consumed);
    }
    return static_cast<ssize_t>(written);
}

} // namespace mzmedia
