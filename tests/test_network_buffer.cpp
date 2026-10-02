/*
 * Buffer 单元测试（M2-3a，分组 `buffer`）
 * ============================================================================
 * 覆盖维度（AI_COLLAB §3.4：正常 / 空 / 满 / 断开 / 超大）：
 *   正常  append/consume/find/affix、fd 读到 EAGAIN、部分写
 *   空    空缓冲、append(nullptr/0)、空缓冲 writeToFd
 *   满    消费越界被拒、读到达上限（hit_limit）、写满发送缓冲（部分写）
 *   断开  EOF（对端关闭）
 *   超大  1000/5000 字节、4KB 级反复收发不涨容量
 *
 * 为什么这一组能干进 TSAN 严格组：本文件只用**单线程 + 非阻塞 fd**（socketpair），
 * 不等待、不睡眠，没有任何跨线程投递。fd 的阻塞语义用非阻塞 + EAGAIN 显式覆盖，
 * 所以不需要"等超时"这类带超时的等待（那会把分组踢进误报组）。
 * ============================================================================
 */

#include "test_main.h"

#include "network/buffer.h"
#include "network/socket.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

using namespace mzmedia;

namespace {

/// 建一对已连接、**非阻塞**的 socket（用 socketpair：比 pipe 多了双向语义，
/// 且 write 的部分写行为与 TCP 一致，适合测 writeToFd）
bool makeSocketPair(int *a, int *b) {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        const int flags = ::fcntl(fds[i], F_GETFL, 0);
        if (flags < 0 || ::fcntl(fds[i], F_SETFL, flags | O_NONBLOCK) != 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            return false;
        }
    }
    *a = fds[0];
    *b = fds[1];
    return true;
}

/// 大 payload 比较用哈希：失败信息要能看，绝不能把 5KB 的 'Q' 全 dump 出来
size_t hashOf(const char *data, size_t len) {
    size_t h = 1469598103934665603ull;   // FNV-1a 64 位
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<unsigned char>(data[i]);
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace

// ---------------------------------------------------------------------------
// 纯内存部分
// ---------------------------------------------------------------------------

MZ_TEST(buffer_append_consume_basic) {
    Buffer buf;
    MZ_ASSERT_TRUE(buf.empty());
    MZ_ASSERT_EQ(buf.size(), 0u);
    MZ_ASSERT_EQ(buf.capacity(), 0u);

    MZ_ASSERT_EQ(buf.append("hello", 5), 5u);
    MZ_ASSERT_EQ(buf.size(), 5u);
    MZ_ASSERT_EQ(buf.append(std::string(" world")), 6u);
    MZ_ASSERT_STR_EQ(buf.toString(), std::string("hello world"));

    MZ_ASSERT_EQ(buf.consume(6), 6u);   // 消费 "hello "
    MZ_ASSERT_EQ(buf.size(), 5u);
    MZ_ASSERT_STR_EQ(buf.toString(), std::string("world"));

    MZ_ASSERT_EQ(buf.consume(buf.size()), 5u);   // 消费干净
    MZ_ASSERT_TRUE(buf.empty());
    MZ_ASSERT_EQ(buf.readPos(), 0u);             // 消费干净后读游标归零
}

MZ_TEST(buffer_consume_overflow_rejected) {
    Buffer buf;
    buf.append("abc", 3);

    // 越界消费：拒绝执行（返回 0）且**不改动数据**，而不是"按 size() 截断"
    MZ_ASSERT_EQ(buf.consume(10), 0u);
    MZ_ASSERT_EQ(buf.size(), 3u);
    MZ_ASSERT_STR_EQ(buf.toString(), std::string("abc"));

    MZ_ASSERT_EQ(buf.consume(0), 0u);   // 0 = 什么都不做
    MZ_ASSERT_EQ(buf.size(), 3u);
}

MZ_TEST(buffer_clear_release) {
    Buffer buf(64);
    MZ_ASSERT_EQ(buf.capacity(), 64u);
    buf.append("0123456789", 10);

    MZ_ASSERT_EQ(buf.clear(), 10u);      // 返回被丢弃的字节数
    MZ_ASSERT_TRUE(buf.empty());
    MZ_ASSERT_EQ(buf.capacity(), 64u);   // 容量保留（长连接不要反复 malloc）

    MZ_ASSERT_EQ(buf.release(), 64u);    // 返回被释放的容量
    MZ_ASSERT_EQ(buf.capacity(), 0u);
}

MZ_TEST(buffer_compact_no_growth) {
    // "反复收发容量单调增长"是线性缓冲最容易踩的坑：靠 compact（读游标前移后回收前导空间）兜住
    const std::string chunk(1024, 'c');
    Buffer buf(1024);

    for (int i = 0; i < 100; ++i) {
        MZ_ASSERT_EQ(buf.append(chunk), 1024u);
        MZ_ASSERT_EQ(buf.consume(512), 512u);   // 半消费：让读游标前移、逼出 compact 分支
        MZ_ASSERT_EQ(buf.consume(512), 512u);   // 消费干净
    }

    MZ_ASSERT_TRUE(buf.empty());
    MZ_ASSERT_LE(buf.capacity(), 2048u);   // 100 轮之后容量仍然只有初始量级
}

MZ_TEST(buffer_find_and_affix) {
    const char *req = "GET /live/a.flv HTTP/1.1\r\nHost: x\r\n\r\n";
    const size_t req_len = std::strlen(req);

    Buffer buf;
    buf.append(req, req_len);

    MZ_ASSERT_TRUE(buf.startWith("GET ", 4));
    MZ_ASSERT_FALSE(buf.startWith("POST", 4));
    MZ_ASSERT_TRUE(buf.endWith("\r\n\r\n", 4));
    MZ_ASSERT_FALSE(buf.endWith("X\r\n\r\n", 5));

    // 找头部结束（HTTP 分帧的标准动作）
    const size_t hdr_end = buf.find("\r\n\r\n", 4);
    MZ_ASSERT_NE(hdr_end, Buffer::npos);
    MZ_ASSERT_EQ(hdr_end, req_len - 4);

    MZ_ASSERT_EQ(buf.find('x', 0), 32u);            // "Host: x" 里的 x
    MZ_ASSERT_EQ(buf.find('x', 33), Buffer::npos);  // 从它后面找：没有
    MZ_ASSERT_EQ(buf.find("NOPE", 4), Buffer::npos);
    MZ_ASSERT_EQ(buf.find('G', buf.size()), Buffer::npos);   // 起点越界
}

MZ_TEST(buffer_append_null_and_empty) {
    Buffer buf;
    MZ_ASSERT_EQ(buf.append(nullptr, 10), 0u);   // 空指针：写入 0 字节
    MZ_ASSERT_EQ(buf.append("", 0), 0u);
    MZ_ASSERT_TRUE(buf.empty());

    MZ_ASSERT_EQ(buf.reserve(64), 64u);          // 返回实际容量
    MZ_ASSERT_EQ(buf.reserve(16), 64u);          // 已经够大：原样返回
    MZ_ASSERT_EQ(buf.capacity(), 64u);
}

MZ_TEST(buffer_move) {
    Buffer a;
    a.append("payload", 7);

    Buffer b(std::move(a));
    MZ_ASSERT_STR_EQ(b.toString(), std::string("payload"));
    MZ_ASSERT_EQ(b.size(), 7u);
    MZ_ASSERT_TRUE(a.empty());            // 被移动后是有效的空对象
    MZ_ASSERT_EQ(a.append("x", 1), 1u);

    Buffer c;
    c = std::move(b);
    MZ_ASSERT_STR_EQ(c.toString(), std::string("payload"));
    MZ_ASSERT_TRUE(b.empty());
}

// ---------------------------------------------------------------------------
// fd 部分（非阻塞 + EAGAIN，不睡眠、不跨线程 → 可以留在 TSAN 严格组）
// ---------------------------------------------------------------------------

MZ_TEST(buffer_read_from_fd_to_eagain) {
    int a = -1, b = -1;
    MZ_ASSERT_TRUE(makeSocketPair(&a, &b));
    Socket peer(a);
    Socket me(b);

    const std::string payload(1000, 'Z');
    MZ_ASSERT_EQ(peer.send(payload.data(), payload.size()), static_cast<ssize_t>(payload.size()));

    Buffer buf;
    bool eof = true;
    bool hit = true;
    int err = 0;
    const ssize_t n = buf.readFromFd(me.rawFD(), 65536, &eof, &hit, &err);
    MZ_ASSERT_EQ(n, 1000);
    MZ_ASSERT_FALSE(hit);        // 没到上限：是把 socket 读空了
    MZ_ASSERT_EQ(hashOf(buf.data(), buf.size()), hashOf(payload.data(), payload.size()));

    // 再来一次：EAGAIN → 0（不是错误，也不是 EOF）
    hit = true;
    eof = true;
    MZ_ASSERT_EQ(buf.readFromFd(me.rawFD(), 65536, &eof, &hit, &err), 0);
    MZ_ASSERT_FALSE(hit);
    MZ_ASSERT_FALSE(eof);   // EAGAIN 不是 EOF：调用方必须能分开这两种"读出 0"
}

MZ_TEST(buffer_read_from_fd_max_bytes_contract) {
    // ★ ET 模式下最致命的一条契约：到达上限而没读完时，必须通过 hit_limit 说出来
    //   （不派发第二次读事件，看不见"没读完"就会让连接永久假死，DESIGN_M2 §8 R1）
    int a = -1, b = -1;
    MZ_ASSERT_TRUE(makeSocketPair(&a, &b));
    Socket peer(a);
    Socket me(b);

    const std::string payload(5000, 'Q');
    MZ_ASSERT_EQ(peer.send(payload.data(), payload.size()), static_cast<ssize_t>(payload.size()));

    Buffer buf;
    bool eof = false;
    bool hit = false;
    MZ_ASSERT_EQ(buf.readFromFd(me.rawFD(), 100, &eof, &hit, nullptr), 100);
    MZ_ASSERT_TRUE(hit);
    MZ_ASSERT_EQ(buf.size(), 100u);

    // 按契约继续读，直到某次不再 hit_limit
    size_t total = buf.size();
    while (hit) {
        const ssize_t m = buf.readFromFd(me.rawFD(), 100, &eof, &hit, nullptr);
        MZ_ASSERT_GE(m, 0);
        total += static_cast<size_t>(m);
    }
    MZ_ASSERT_EQ(total, 5000u);
    MZ_ASSERT_EQ(buf.size(), 5000u);
    MZ_ASSERT_EQ(hashOf(buf.data(), buf.size()), hashOf(payload.data(), payload.size()));
}

MZ_TEST(buffer_read_from_fd_eof) {
    int a = -1, b = -1;
    MZ_ASSERT_TRUE(makeSocketPair(&a, &b));
    Socket peer(a);
    Socket me(b);

    const std::string payload(64, 'E');
    MZ_ASSERT_EQ(peer.send(payload.data(), payload.size()), static_cast<ssize_t>(payload.size()));
    MZ_ASSERT_TRUE(peer.close());   // 对端关闭

    Buffer buf;
    bool eof = false;
    bool hit = true;
    // 有数据 + 对端已关闭：**数据和 EOF 要同时报出来**（否则调用方要么丢数据、要么永远不关连接）
    MZ_ASSERT_EQ(buf.readFromFd(me.rawFD(), 4096, &eof, &hit, nullptr), 64);
    MZ_ASSERT_FALSE(hit);
    MZ_ASSERT_TRUE(eof);
    eof = false;
    MZ_ASSERT_EQ(buf.readFromFd(me.rawFD(), 4096, &eof, &hit, nullptr), 0);   // 纯 EOF
    MZ_ASSERT_TRUE(eof);
    MZ_ASSERT_EQ(buf.size(), 64u);
}

MZ_TEST(buffer_read_from_fd_invalid_max) {
    int a = -1, b = -1;
    MZ_ASSERT_TRUE(makeSocketPair(&a, &b));
    Socket peer(a);
    Socket me(b);

    Buffer buf;
    bool eof = false;
    bool hit = false;
    int err = 0;
    // max_bytes == 0 是非法调用（放行会变成"永远读不完"的忙等）：必须拒绝并说清楚
    MZ_ASSERT_EQ(buf.readFromFd(me.rawFD(), 0, &eof, &hit, &err), -1);
    MZ_ASSERT_TRUE(hit);
    MZ_ASSERT_FALSE(eof);
    MZ_ASSERT_EQ(err, EINVAL);
}

MZ_TEST(buffer_write_to_fd_partial) {
    int a = -1, b = -1;
    MZ_ASSERT_TRUE(makeSocketPair(&a, &b));
    Socket peer(a);   // 故意不读：把发送缓冲灌满
    Socket me(b);

    const std::string big(1 << 20, 'W');   // 1MB 远大于 socket 默认发送缓冲
    Buffer buf;
    MZ_ASSERT_EQ(buf.append(big), static_cast<size_t>(big.size()));

    int err = 0;
    const ssize_t wrote = buf.writeToFd(me.rawFD(), &err);
    MZ_ASSERT_GT(wrote, 0);
    MZ_ASSERT_LT(wrote, static_cast<ssize_t>(big.size()));                 // 部分写
    MZ_ASSERT_EQ(buf.size(), big.size() - static_cast<size_t>(wrote));     // 已写出部分被消费
    MZ_ASSERT_EQ(err, 0);

    // 缓冲仍然满着 → 再写就是 EAGAIN：返回 0（不是错误）
    const size_t before = buf.size();
    MZ_ASSERT_EQ(buf.writeToFd(me.rawFD(), &err), 0);
    MZ_ASSERT_EQ(buf.size(), before);

    // 空缓冲时 writeToFd 也不该崩
    Buffer empty_buf;
    MZ_ASSERT_EQ(empty_buf.writeToFd(me.rawFD(), &err), 0);
}

MZ_TEST(buffer_random_differential) {
    int a = -1, b = -1;
    MZ_ASSERT_TRUE(makeSocketPair(&a, &b));
    Socket peer(a), me(b);
    Buffer buf;
    std::string model;
    uint32_t seed = 12345;
    auto rnd = [&seed]() { seed = seed * 1103515245u + 12345u; return seed >> 16; };

    for (int step = 0; step < 20000; ++step) {
        const int op = rnd() % 3;
        if (op == 0 || model.empty()) {                       // append
            const size_t n = rnd() % 4096 + 1;
            std::string chunk(n, '\0');
            for (auto &c : chunk) c = static_cast<char>(rnd());
            MZ_ASSERT_EQ(buf.append(chunk.data(), chunk.size()), chunk.size());
            model += chunk;
        } else if (op == 1) {                                 // consume（含 0 与整段）
            const size_t n = rnd() % (model.size() + 1);
            MZ_ASSERT_EQ(buf.consume(n), n);
            model.erase(0, n);
        } else {                                              // writeToFd：部分写才是关键路径
            char tmp[8192];
            while (::recv(peer.rawFD(), tmp, sizeof(tmp), MSG_DONTWAIT) > 0) { }   // 先腾空对端
            const ssize_t n = buf.writeToFd(me.rawFD(), nullptr);
            MZ_ASSERT_GE(n, 0);
            if (n > 0) model.erase(0, static_cast<size_t>(n));
        }
        MZ_ASSERT_EQ(buf.size(), model.size());               // ★ 每步比对逻辑内容
        if (buf.size() > 0) {
            MZ_ASSERT_EQ(std::memcmp(buf.data(), model.data(), model.size()), 0);
        }
        if (model.size() > 256 * 1024) {                      // 防止越滚越大
            MZ_ASSERT_EQ(buf.consume(model.size() / 2), model.size() / 2);
            model.erase(0, model.size() / 2);
        }
    }
}