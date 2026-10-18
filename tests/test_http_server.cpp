/*
 * HttpServer / HttpSession 单元测试（M3-b，分组 `ntimed_http`）
 * ============================================================================
 * 覆盖维度（AI_COLLAB §3.4：正常 / 空 / 满 / 断开 / 超大）：
 *   正常  测试页、自定义路由、兜底路由、keep-alive 复用、管线化、CORS
 *   空    未知路径（404）、无 body 请求
 *   满    请求行超长（414）、头部超长（431）、非 GET（405）
 *   断开  Connection: close / 畸形请求 **必须真的关**（读到 EOF）；404/405/500/501 必须**保活**
 *         （v0.1.1 起每一步都断言"这条连接此刻是开还是关"，见 stillOpen / waitEof）
 *   超大  5KB URI、40KB 头部
 *
 * 为什么能进 TSAN 严格组：只用 socket 超时 + sleepMs 轮询（**不用** std::condition_variable
 * 的超时接口），不含已知误报机制；断言全部在测试主线程。
 * ============================================================================
 */

#include "test_main.h"

#include "core/util.h"
#include "http/http_server.h"
#include "network/event_poller.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <map>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using namespace mzmedia;

namespace {

/// 阻塞连接到 127.0.0.1:port（带 recv 超时，避免测试挂死）
int connectTo(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

struct Response {
    int status = 0;
    size_t declared_length = 0;
    bool has_content_length = false;
    std::string body;
    std::string raw;
    std::map<std::string, std::string> headers;   // 键统一小写

    bool has(const std::string &name) const { return headers.count(name) != 0; }
    std::string get(const std::string &name) const {
        auto it = headers.find(name);
        return it == headers.end() ? std::string() : it->second;
    }
};

/// 读一个完整响应（按 Content-Length 判断结束；M3-b 没有 chunked）
/// @param carry 上次调用**多读出来**的字节（管线化时两个响应可能挤在同一个 recv 里）：
///        进来先接上，出去时把本次没消费完的留给下一次 —— 这就是客户端的"分帧"
bool readResponse(int fd, Response *out, int timeout_ms = 2000, std::string *carry = nullptr) {
    out->raw.clear();
    out->headers.clear();
    out->body.clear();
    if (carry != nullptr) {
        out->raw.swap(*carry);   // 接上上次剩下的
    }
    const uint64_t deadline = getCurrentMillisecond() + static_cast<uint64_t>(timeout_ms);
    size_t head_end = std::string::npos;
    char buf[4096];
    while (getCurrentMillisecond() < deadline) {
        head_end = out->raw.find("\r\n\r\n");
        if (head_end != std::string::npos) {
            break;
        }
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        out->raw.append(buf, static_cast<size_t>(n));
    }
    if (head_end == std::string::npos) {
        return false;
    }

    // 状态行："HTTP/1.1 200 OK"
    const size_t sp = out->raw.find(' ');
    if (sp == std::string::npos) {
        return false;
    }
    out->status = std::atoi(out->raw.c_str() + sp + 1);

    // 头部
    size_t pos = out->raw.find("\r\n") + 2;
    while (pos < head_end) {
        const size_t eol = out->raw.find("\r\n", pos);
        if (eol == std::string::npos || eol > head_end) {
            break;
        }
        const std::string line = out->raw.substr(pos, eol - pos);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            for (char &c : name) {
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            }
            size_t v = colon + 1;
            while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
            out->headers[name] = line.substr(v);
        }
        pos = eol + 2;
    }
    if (out->has("content-length")) {
        out->has_content_length = true;
        out->declared_length = static_cast<size_t>(std::atol(out->get("content-length").c_str()));
    }

    const size_t body_start = head_end + 4;
    const bool is_chunked = out->get("transfer-encoding").find("chunked") != std::string::npos;
    size_t consumed = body_start;

    if (is_chunked) {
        // 按块解码：<hex 长度>CRLF <数据>CRLF ... 0CRLFCRLF
        std::string decoded;
        size_t pos = body_start;
        for (;;) {
            size_t eol = out->raw.find("\r\n", pos);
            while (eol == std::string::npos && getCurrentMillisecond() < deadline) {
                const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) {
                    break;
                }
                out->raw.append(buf, static_cast<size_t>(n));
                eol = out->raw.find("\r\n", pos);
            }
            if (eol == std::string::npos) {
                break;   // 超时或对端断开
            }
            const size_t chunk_len =
                static_cast<size_t>(std::strtoul(out->raw.substr(pos, eol - pos).c_str(), nullptr, 16));
            if (chunk_len == 0) {
                consumed = eol + 4;   // "0\r\n\r\n"
                break;
            }
            const size_t need = eol + 2 + chunk_len + 2;
            while (out->raw.size() < need && getCurrentMillisecond() < deadline) {
                const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) {
                    break;
                }
                out->raw.append(buf, static_cast<size_t>(n));
            }
            if (out->raw.size() < need) {
                break;
            }
            decoded.append(out->raw, eol + 2, chunk_len);
            pos = need;
        }
        out->body = decoded;
    } else {
        while (getCurrentMillisecond() < deadline) {
            const size_t have = out->raw.size() - body_start;
            if (out->has_content_length && have >= out->declared_length) {
                break;
            }
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            out->raw.append(buf, static_cast<size_t>(n));
        }
        if (out->has_content_length) {
            consumed = body_start + out->declared_length;
        } else {
            consumed = out->raw.size();
        }
        if (out->raw.size() >= body_start) {
            out->body = out->raw.substr(body_start, consumed > body_start ? consumed - body_start : 0);
        }
    }
    if (out->raw.size() > consumed) {
        if (carry != nullptr) {
            *carry = out->raw.substr(consumed);   // 多读的（管线化的下一个响应）留给下一次
        }
        out->raw.resize(consumed);
    }
    return true;
}

bool sendAll(int fd, const std::string &data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

/// 【v0.1.1】等对端**真的关闭**（recv 返回 0 = 收到 FIN）
/// @return true = 读到了 EOF；false = 超时/出错（= 服务端没关，用例应当红）
/// @note 判据是"关没关"，**不是延迟**：所以给宽上限（默认 2s），不拿它当计时门禁
///       （TESTING.md §2.2 的教训）。服务端"发完再关"要排空队列，慢一点是正常的。
bool waitEof(int fd, int timeout_ms = 2000) {
    const uint64_t deadline = getCurrentMillisecond() + static_cast<uint64_t>(timeout_ms);
    char buf[4096];
    for (;;) {
        const uint64_t now = getCurrentMillisecond();
        if (now >= deadline) {
            return false;
        }
        const uint64_t left = deadline - now;
        struct timeval tv;
        tv.tv_sec = static_cast<time_t>(left / 1000);
        tv.tv_usec = static_cast<suseconds_t>((left % 1000) * 1000);
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n == 0) {
            return true;   // FIN
        }
        if (n > 0) {
            continue;      // 还有尾巴数据：收干净再继续等 EOF（不能当失败）
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return false;  // 真错误
        }
    }
}

/// 【v0.1.1】断言连接**还开着**：在 grace_ms 观察窗内既不许出现 EOF，也不该有多余数据
/// 为什么需要它：只加"畸形后必须 EOF"会漏掉另一类回归 —— 把该保活的连接也关了。
/// 为什么要观察窗：服务端的关闭是"排空后再关"，FIN 不是同一瞬间到达；
///   只做一次 `recv(MSG_DONTWAIT)` 会**放过**误关（变异验证实测：`get_test_page` 侥幸绿）。
///   grace_ms 只用来"给误关留出暴露时间"，判据仍是"关没关"，不是延迟门禁（TESTING.md §2.2）。
bool stillOpen(int fd, int grace_ms = 50) {
    for (;;) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        const int rc = ::poll(&pfd, 1, grace_ms);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;   // poll 失败：无法证明连接还活着
        }
        if (rc == 0) {
            return true;    // 观察窗内没人来敲门，也没 FIN → 连接还开着
        }
        char buf[4096];
        const ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n == 0) {
            return false;   // EOF = 被关了（= 修过头）
        }
        if (n < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK;
        }
        // n > 0：还有没读的数据，读干净再等下一轮
    }
}

/// 起一个 HttpServer（带 poller），port=0 由内核分配
struct ServerFixture {
    EventPoller::Ptr poller;
    std::shared_ptr<HttpServer> server;

    /// @param configure 在 start() **之前**调用：路由/CORS/上限等必须在此之前配置
    ///        （start 之后再配会返回 false 且不生效 —— 契约见 DESIGN_M3 §8）
    bool setup(const std::function<void(HttpServer &)> &configure = nullptr) {
        poller = EventPoller::create("test-http");
        server = std::make_shared<HttpServer>(poller);
        if (!server->setSessionTimeout(0, 0)) {   // 不设空闲检测，避免干扰
            return false;
        }
        if (configure) {
            configure(*server);
        }
        return server->start(0);
    }
    ~ServerFixture() {
        if (server) {
            server->shutdown();
        }
        if (poller) {
            poller->shutdown();
        }
    }
};

} // namespace

// ---------------------------------------------------------------------------
// 正常
// ---------------------------------------------------------------------------

MZ_TEST(ntimed_http_get_test_page) {
    ServerFixture fx;
    MZ_ASSERT_TRUE(fx.setup());
    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);
    MZ_ASSERT_TRUE(sendAll(cli, "GET / HTTP/1.1\r\nHost: x\r\n\r\n"));

    Response resp;
    MZ_ASSERT_TRUE(readResponse(cli, &resp));
    MZ_ASSERT_EQ(resp.status, 200);
    MZ_ASSERT_TRUE(resp.get("content-type").find("text/html") != std::string::npos);
    MZ_ASSERT_TRUE(resp.body.find("mzmedia") != std::string::npos);
    MZ_ASSERT_TRUE(resp.has_content_length);
    MZ_ASSERT_EQ(resp.declared_length, resp.body.size());   // 头与实际一致（自动补 Content-Length）
    MZ_ASSERT_STR_EQ(resp.get("access-control-allow-origin"), std::string("*"));   // FR-4.3
    MZ_ASSERT_STR_EQ(resp.get("server"), std::string("mzmedia"));
    MZ_ASSERT_FALSE(resp.has("connection"));   // 长连接默认不发 Connection 头
    MZ_ASSERT_EQ(fx.server->totalRequests(), 1u);
    MZ_ASSERT_EQ(fx.server->total4xx(), 0u);
    // ★ v0.1.1：没声明 close 的响应必须**保活**（防"修过头把所有连接都关了"）
    MZ_ASSERT_TRUE(stillOpen(cli));
    ::close(cli);
}

MZ_TEST(ntimed_http_404_unknown_path) {
    ServerFixture fx;
    MZ_ASSERT_TRUE(fx.setup());
    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);
    MZ_ASSERT_TRUE(sendAll(cli, "GET /no/such/path HTTP/1.1\r\n\r\n"));
    Response resp;
    MZ_ASSERT_TRUE(readResponse(cli, &resp));
    MZ_ASSERT_EQ(resp.status, 404);
    MZ_ASSERT_TRUE(resp.body.find("404") != std::string::npos);
    MZ_ASSERT_EQ(fx.server->total4xx(), 1u);
    MZ_ASSERT_TRUE(stillOpen(cli));   // 404 是合法请求 → 必须保活（不能因为"是错误码"就关）
    ::close(cli);
}

MZ_TEST(ntimed_http_501_live_route) {
    ServerFixture fx;
    MZ_ASSERT_TRUE(fx.setup());
    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);
    MZ_ASSERT_TRUE(sendAll(cli, "GET /live/a.flv HTTP/1.1\r\n\r\n"));
    Response resp;
    MZ_ASSERT_TRUE(readResponse(cli, &resp));
    MZ_ASSERT_EQ(resp.status, 501);   // 路由存在但没有实现（M6 才有）
    MZ_ASSERT_TRUE(resp.body.find("Not Implemented") != std::string::npos);
    MZ_ASSERT_EQ(fx.server->total5xx(), 1u);
    MZ_ASSERT_TRUE(stillOpen(cli));   // 501 也是合法请求 → 保活
    ::close(cli);
}

MZ_TEST(ntimed_http_custom_route_and_fallback) {
    ServerFixture fx;
    bool route_ok = false;
    bool fallback_ok = false;
    MZ_ASSERT_TRUE(fx.setup([&](HttpServer &s) {   // 路由必须在 start() 之前配置
        route_ok = s.setRoute("/hello", [](const HttpParser &req, HttpResponse &resp) {
            resp.setContentType("text/plain");
            resp.setBody(std::string("hello ") + req.path());
        });
        fallback_ok = s.setFallback([](const HttpParser &req, HttpResponse &resp) {
            resp.setStatus(200);
            resp.setBody(std::string("fallback:") + req.path());
        });
    }));
    MZ_ASSERT_TRUE(route_ok);
    MZ_ASSERT_TRUE(fallback_ok);

    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);
    MZ_ASSERT_TRUE(sendAll(cli, "GET /hello HTTP/1.1\r\n\r\n"));
    Response resp;
    MZ_ASSERT_TRUE(readResponse(cli, &resp));
    MZ_ASSERT_EQ(resp.status, 200);
    MZ_ASSERT_STR_EQ(resp.body, std::string("hello /hello"));

    // 同一连接上再发一个：走兜底
    MZ_ASSERT_TRUE(sendAll(cli, "GET /other HTTP/1.1\r\n\r\n"));
    MZ_ASSERT_TRUE(readResponse(cli, &resp));
    MZ_ASSERT_EQ(resp.status, 200);
    MZ_ASSERT_STR_EQ(resp.body, std::string("fallback:/other"));
    MZ_ASSERT_EQ(fx.server->totalRequests(), 2u);
    MZ_ASSERT_TRUE(stillOpen(cli));   // 两次都保活
    ::close(cli);
}

MZ_TEST(ntimed_http_handler_exception_500) {
    ServerFixture fx;
    bool routed = false;
    MZ_ASSERT_TRUE(fx.setup([&](HttpServer &s) {
        routed = s.setRoute("/boom", [](const HttpParser &, HttpResponse &) {
            throw std::runtime_error("handler 故意抛异常");
        });
    }));
    MZ_ASSERT_TRUE(routed);
    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);
    MZ_ASSERT_TRUE(sendAll(cli, "GET /boom HTTP/1.1\r\n\r\n"));
    Response resp;
    MZ_ASSERT_TRUE(readResponse(cli, &resp));
    MZ_ASSERT_EQ(resp.status, 500);
    MZ_ASSERT_TRUE(resp.body.find("Internal Server Error") != std::string::npos);
    MZ_ASSERT_EQ(fx.server->total5xx(), 1u);
    MZ_ASSERT_TRUE(stillOpen(cli));   // 500 不是解析错误 → 连接要保活
    ::close(cli);
}

MZ_TEST(ntimed_http_cors_can_be_disabled) {
    ServerFixture fx;
    bool cors_ok = false;
    MZ_ASSERT_TRUE(fx.setup([&](HttpServer &s) { cors_ok = s.setCorsEnabled(false); }));
    MZ_ASSERT_TRUE(cors_ok);   // start() 之前配置 → 生效
    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);
    MZ_ASSERT_TRUE(sendAll(cli, "GET / HTTP/1.1\r\n\r\n"));
    Response resp;
    MZ_ASSERT_TRUE(readResponse(cli, &resp));
    MZ_ASSERT_EQ(resp.status, 200);
    MZ_ASSERT_FALSE(resp.has("access-control-allow-origin"));   // 关掉之后必须没有 CORS 头
    MZ_ASSERT_TRUE(stillOpen(cli));
    ::close(cli);
}

// ---------------------------------------------------------------------------
// 连接语义：keep-alive / close / 管线化
// ---------------------------------------------------------------------------

MZ_TEST(ntimed_http_keepalive_two_requests) {
    ServerFixture fx;
    MZ_ASSERT_TRUE(fx.setup());
    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);

    // 第 1 个请求
    MZ_ASSERT_TRUE(sendAll(cli, "GET / HTTP/1.1\r\n\r\n"));
    Response resp;
    MZ_ASSERT_TRUE(readResponse(cli, &resp));
    MZ_ASSERT_EQ(resp.status, 200);
    MZ_ASSERT_FALSE(resp.has("connection"));   // HTTP/1.1 默认长连接

    // 第 2 个请求（复用同一连接）
    MZ_ASSERT_TRUE(sendAll(cli, "GET /nope HTTP/1.1\r\n\r\n"));
    MZ_ASSERT_TRUE(readResponse(cli, &resp));
    MZ_ASSERT_EQ(resp.status, 404);
    MZ_ASSERT_TRUE(resp.body.find("404 Not Found") != std::string::npos);   // 可观察量：body
    MZ_ASSERT_EQ(fx.server->totalRequests(), 2u);
    MZ_ASSERT_EQ(fx.server->tcp().totalAccepted(), 1u);   // 只建立了 1 条连接
    MZ_ASSERT_TRUE(stillOpen(cli));   // ★ 两条都保活 → 不能因为"修 close"把它们关了
    ::close(cli);
}

MZ_TEST(ntimed_http_connection_close_honored) {
    ServerFixture fx;
    MZ_ASSERT_TRUE(fx.setup());
    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);
    MZ_ASSERT_TRUE(sendAll(cli, "GET / HTTP/1.1\r\nConnection: close\r\n\r\n"));
    Response resp;
    MZ_ASSERT_TRUE(readResponse(cli, &resp));
    MZ_ASSERT_EQ(resp.status, 200);
    MZ_ASSERT_STR_EQ(resp.get("connection"), std::string("close"));   // 必须如实告诉客户端
    // ★ v0.1.1：**声明 close 就必须真的关**（RFC 7230 §6.6）。
    //   修前 = 红：头里写着 close，服务端却不关，客户端只能等到 60s 读空闲（FR-4.4）。
    MZ_ASSERT_TRUE(waitEof(cli));
    MZ_ASSERT_EQ(fx.server->totalCloseFallback(), 0u);
    ::close(cli);
}

MZ_TEST(ntimed_http_pipelined_same_packet) {
    ServerFixture fx;
    MZ_ASSERT_TRUE(fx.setup());
    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);
    // 一个包里两个请求：响应必须**按顺序**回来
    MZ_ASSERT_TRUE(sendAll(cli, "GET / HTTP/1.1\r\n\r\nGET /nope HTTP/1.1\r\n\r\n"));
    Response first;
    Response second;
    std::string carry;   // ★ 两个响应可能挤在一次 recv 里，必须把多读的带给下一次
    MZ_ASSERT_TRUE(readResponse(cli, &first, 2000, &carry));
    MZ_ASSERT_TRUE(readResponse(cli, &second, 2000, &carry));
    MZ_ASSERT_EQ(first.status, 200);
    MZ_ASSERT_EQ(second.status, 404);
    MZ_ASSERT_EQ(fx.server->totalRequests(), 2u);
    MZ_ASSERT_TRUE(stillOpen(cli));   // 管线化没声明 close → 两条响应之后仍然保活
    ::close(cli);
}

// ---------------------------------------------------------------------------
// 畸形 / 超限（都要有明确状态码，且服务端不能被打死）
// ---------------------------------------------------------------------------

MZ_TEST(ntimed_http_400_malformed) {
    ServerFixture fx;
    MZ_ASSERT_TRUE(fx.setup());
    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);
    MZ_ASSERT_TRUE(sendAll(cli, "GET /\r\n\r\n"));   // 缺版本段
    Response resp;
    MZ_ASSERT_TRUE(readResponse(cli, &resp));
    MZ_ASSERT_EQ(resp.status, 400);
    MZ_ASSERT_STR_EQ(resp.get("connection"), std::string("close"));   // 畸形：回完就关
    // 可观察量：body 必须说清是**哪一类**畸形（客户端自助定位，不靠猜）
    MZ_ASSERT_TRUE(resp.body.find("HTTP 400 Bad Request") == 0);
    MZ_ASSERT_TRUE(resp.body.find("bad-request-line") != std::string::npos);
    MZ_ASSERT_EQ(fx.server->totalMalformed(), 1u);
    MZ_ASSERT_EQ(fx.server->total4xx(), 1u);
    // ★ v0.1.1：光有 close **头**不够，必须真的读到 EOF。
    //   修前 = 红（服务端不关；客户端按 close 语义永远等不到 FIN）。
    MZ_ASSERT_TRUE(waitEof(cli));
    MZ_ASSERT_EQ(fx.server->totalCloseFallback(), 0u);   // 应走"排空后再关"，不是硬关兜底
    ::close(cli);
}

MZ_TEST(ntimed_http_414_uri_too_long) {
    ServerFixture fx;
    MZ_ASSERT_TRUE(fx.setup());
    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);
    const std::string req = "GET /" + std::string(5 * 1024, 'a') + " HTTP/1.1\r\n\r\n";
    MZ_ASSERT_TRUE(sendAll(cli, req));
    Response resp;
    MZ_ASSERT_TRUE(readResponse(cli, &resp));
    MZ_ASSERT_EQ(resp.status, 414);
    MZ_ASSERT_STR_EQ(resp.get("connection"), std::string("close"));   // v0.1.1：原来连头都没断言
    MZ_ASSERT_TRUE(resp.body.find("HTTP 414 URI Too Long") == 0);
    MZ_ASSERT_TRUE(resp.body.find("uri-too-long") != std::string::npos);
    MZ_ASSERT_EQ(fx.server->totalMalformed(), 1u);
    MZ_ASSERT_EQ(fx.server->total4xx(), 1u);
    MZ_ASSERT_TRUE(waitEof(cli));   // ★ 超限属于解析错误 → 必须断开
    ::close(cli);
}

MZ_TEST(ntimed_http_431_headers_too_large) {
    ServerFixture fx;
    MZ_ASSERT_TRUE(fx.setup());
    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);
    std::string req = "GET / HTTP/1.1\r\n";
    while (req.size() < 40 * 1024) {
        req += "X-Filler: 0123456789012345678901234567890123456789\r\n";
    }
    req += "\r\n";
    MZ_ASSERT_TRUE(sendAll(cli, req));
    Response resp;
    MZ_ASSERT_TRUE(readResponse(cli, &resp));
    MZ_ASSERT_EQ(resp.status, 431);
    MZ_ASSERT_STR_EQ(resp.get("connection"), std::string("close"));
    MZ_ASSERT_TRUE(resp.body.find("HTTP 431 Request Header Fields Too Large") == 0);
    MZ_ASSERT_TRUE(resp.body.find("headers-too-large") != std::string::npos);
    MZ_ASSERT_EQ(fx.server->totalMalformed(), 1u);
    MZ_ASSERT_EQ(fx.server->total4xx(), 1u);
    MZ_ASSERT_TRUE(waitEof(cli));   // ★ 超限属于解析错误 → 必须断开
    ::close(cli);
}

MZ_TEST(ntimed_http_405_method_not_allowed) {
    ServerFixture fx;
    MZ_ASSERT_TRUE(fx.setup());
    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);
    MZ_ASSERT_TRUE(sendAll(cli, "POST / HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc"));
    Response resp;
    MZ_ASSERT_TRUE(readResponse(cli, &resp));
    MZ_ASSERT_EQ(resp.status, 405);
    MZ_ASSERT_STR_EQ(resp.get("allow"), std::string("GET"));   // 告诉客户端支持什么
    MZ_ASSERT_EQ(fx.server->total4xx(), 1u);
    MZ_ASSERT_TRUE(resp.body.find("405 Method Not Allowed") == 0);
    MZ_ASSERT_EQ(fx.server->totalMalformed(), 0u);   // 合法请求、只是不支持：不算畸形
    MZ_ASSERT_TRUE(stillOpen(cli));   // ★ 405 不是解析错误 → 不能顺手关（防修过头）
    ::close(cli);
}

MZ_TEST(ntimed_http_server_survives_malformed) {
    ServerFixture fx;
    MZ_ASSERT_TRUE(fx.setup());
    // 先来一条畸形连接（服务端会回 400 并断）
    const int bad = connectTo(fx.server->port());
    MZ_ASSERT_GT(bad, 0);
    MZ_ASSERT_TRUE(sendAll(bad, "GET / HTTP/1.1\r\nBadHeaderNoColon\r\n\r\n"));
    Response resp;
    MZ_ASSERT_TRUE(readResponse(bad, &resp));
    MZ_ASSERT_EQ(resp.status, 400);
    MZ_ASSERT_TRUE(waitEof(bad));   // 畸形连接必须被服务端关掉（不是靠客户端自己关）
    ::close(bad);

    // 服务端必须还能正常服务新连接
    const int good = connectTo(fx.server->port());
    MZ_ASSERT_GT(good, 0);
    MZ_ASSERT_TRUE(sendAll(good, "GET / HTTP/1.1\r\n\r\n"));
    MZ_ASSERT_TRUE(readResponse(good, &resp));
    MZ_ASSERT_EQ(resp.status, 200);
    MZ_ASSERT_EQ(fx.server->totalRequests(), 2u);
    ::close(good);
}

MZ_TEST(ntimed_http_malformed_single_response) {
    // ★ v0.1.1 修的第二个 bug：**一条连接只回一次 4xx**。
    //   修前：解析判错后连接不关，同一连接每读到一个报文就再回一个 400
    //   —— 实测连发 5 条畸形 → 收到 5 个 400、`malformed` 计成 5（日志却只有 1 条）。
    ServerFixture fx;
    MZ_ASSERT_TRUE(fx.setup());
    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);

    // 一个包里塞 3 条畸形请求（缺版本段）：只允许回 1 个响应
    MZ_ASSERT_TRUE(sendAll(cli, "GET /\r\n\r\nGET /\r\n\r\nGET /\r\n\r\n"));
    Response resp;
    MZ_ASSERT_TRUE(readResponse(cli, &resp));
    MZ_ASSERT_EQ(resp.status, 400);
    MZ_ASSERT_TRUE(resp.body.find("HTTP 400 Bad Request") == 0);
    MZ_ASSERT_EQ(fx.server->totalRequests(), 1u);
    MZ_ASSERT_EQ(fx.server->totalMalformed(), 1u);
    MZ_ASSERT_EQ(fx.server->total4xx(), 1u);
    // 回完就关：再 recv 必须是 EOF（不是"又来一个 400"）
    MZ_ASSERT_TRUE(waitEof(cli));

    // 服务端没被搞坏：新连接照常工作
    const int again = connectTo(fx.server->port());
    MZ_ASSERT_GT(again, 0);
    MZ_ASSERT_TRUE(sendAll(again, "GET /no/such HTTP/1.1\r\n\r\n"));
    MZ_ASSERT_TRUE(readResponse(again, &resp));
    MZ_ASSERT_EQ(resp.status, 404);
    MZ_ASSERT_EQ(fx.server->totalRequests(), 2u);
    ::close(again);
    ::close(cli);
}


// ---------------------------------------------------------------------------
// M3-c：chunked 流式 / Range / stats
// ---------------------------------------------------------------------------

MZ_TEST(ntimed_http_chunked_stream) {
    ServerFixture fx;
    MZ_ASSERT_TRUE(fx.setup());
    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);
    MZ_ASSERT_TRUE(sendAll(cli, "GET /stream HTTP/1.1\r\n\r\n"));

    Response resp;
    std::string carry;
    MZ_ASSERT_TRUE(readResponse(cli, &resp, 2000, &carry));
    MZ_ASSERT_EQ(resp.status, 200);
    // ★ 分块模式的两条硬性约定：必须声明 chunked、**不能**有 Content-Length
    MZ_ASSERT_TRUE(resp.get("transfer-encoding").find("chunked") != std::string::npos);
    MZ_ASSERT_FALSE(resp.has("content-length"));
    MZ_ASSERT_STR_EQ(resp.body, std::string("chunk-1\nchunk-2\nchunk-3\n"));
    ::close(cli);
}

MZ_TEST(ntimed_http_chunked_keeps_connection_usable) {
    ServerFixture fx;
    MZ_ASSERT_TRUE(fx.setup());
    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);
    // 分块响应之后，同一条连接上还能正常发下一个请求（分帧没有把连接搞坏）
    MZ_ASSERT_TRUE(sendAll(cli, "GET /stream HTTP/1.1\r\n\r\n"));
    Response resp;
    std::string carry;
    MZ_ASSERT_TRUE(readResponse(cli, &resp, 2000, &carry));
    MZ_ASSERT_EQ(resp.status, 200);

    MZ_ASSERT_TRUE(sendAll(cli, "GET /nope HTTP/1.1\r\n\r\n"));
    MZ_ASSERT_TRUE(readResponse(cli, &resp, 2000, &carry));
    MZ_ASSERT_EQ(resp.status, 404);
    MZ_ASSERT_EQ(fx.server->totalRequests(), 2u);
    MZ_ASSERT_EQ(fx.server->tcp().totalAccepted(), 1u);   // 同一条连接
    MZ_ASSERT_TRUE(stillOpen(cli));   // ★ 同步 chunked + 保活 → 不能被"发完再关"误关
    ::close(cli);
}

MZ_TEST(ntimed_http_range_single) {
    ServerFixture fx;
    MZ_ASSERT_TRUE(fx.setup());
    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);

    // 先拿全量，作为切片比较的基准（不在用例里重复页面内容）
    MZ_ASSERT_TRUE(sendAll(cli, "GET / HTTP/1.1\r\n\r\n"));
    Response full;
    std::string carry;
    MZ_ASSERT_TRUE(readResponse(cli, &full, 2000, &carry));
    MZ_ASSERT_EQ(full.status, 200);
    MZ_ASSERT_STR_EQ(full.get("accept-ranges"), std::string("bytes"));
    const size_t total = full.body.size();

    MZ_ASSERT_TRUE(sendAll(cli, "GET / HTTP/1.1\r\nRange: bytes=0-9\r\n\r\n"));
    Response part;
    MZ_ASSERT_TRUE(readResponse(cli, &part, 2000, &carry));
    MZ_ASSERT_EQ(part.status, 206);
    MZ_ASSERT_EQ(part.body.size(), 10u);
    MZ_ASSERT_STR_EQ(part.body, full.body.substr(0, 10));
    MZ_ASSERT_STR_EQ(part.get("content-range"),
                     std::string("bytes 0-9/") + std::to_string(total));
    MZ_ASSERT_EQ(part.declared_length, part.body.size());   // 206 的 Content-Length 也要自洽
    ::close(cli);
}

MZ_TEST(ntimed_http_range_open_ended) {
    ServerFixture fx;
    MZ_ASSERT_TRUE(fx.setup());
    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);
    MZ_ASSERT_TRUE(sendAll(cli, "GET / HTTP/1.1\r\n\r\n"));
    Response full;
    std::string carry;
    MZ_ASSERT_TRUE(readResponse(cli, &full, 2000, &carry));

    // bytes=5- ：从 5 到末尾
    MZ_ASSERT_TRUE(sendAll(cli, "GET / HTTP/1.1\r\nRange: bytes=5-\r\n\r\n"));
    Response part;
    MZ_ASSERT_TRUE(readResponse(cli, &part, 2000, &carry));
    MZ_ASSERT_EQ(part.status, 206);
    MZ_ASSERT_STR_EQ(part.body, full.body.substr(5));
    ::close(cli);
}

MZ_TEST(ntimed_http_range_out_of_range_416) {
    ServerFixture fx;
    MZ_ASSERT_TRUE(fx.setup());
    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);
    MZ_ASSERT_TRUE(sendAll(cli, "GET / HTTP/1.1\r\nRange: bytes=999999-\r\n\r\n"));
    Response resp;
    MZ_ASSERT_TRUE(readResponse(cli, &resp));
    MZ_ASSERT_EQ(resp.status, 416);
    MZ_ASSERT_TRUE(resp.get("content-range").find("bytes */") == 0);   // 格式：bytes */总长
    MZ_ASSERT_EQ(fx.server->total4xx(), 1u);
    ::close(cli);
}

MZ_TEST(ntimed_http_range_ignored_cases) {
    ServerFixture fx;
    MZ_ASSERT_TRUE(fx.setup());
    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);
    MZ_ASSERT_TRUE(sendAll(cli, "GET / HTTP/1.1\r\n\r\n"));
    Response full;
    std::string carry;
    MZ_ASSERT_TRUE(readResponse(cli, &full, 2000, &carry));

    // 三种"不支持但合法/畸形"的 Range：一律忽略 → 200 全量（绝不给半吊子 206）
    const char *ranges[] = {"bytes=0-1,3-4", "bytes=abc", "bytes=-5"};
    for (const char *r : ranges) {
        MZ_ASSERT_TRUE(sendAll(cli, std::string("GET / HTTP/1.1\r\nRange: ") + r + "\r\n\r\n"));
        Response resp;
        MZ_ASSERT_TRUE(readResponse(cli, &resp, 2000, &carry));
        MZ_ASSERT_EQ(resp.status, 200);
        MZ_ASSERT_EQ(resp.body.size(), full.body.size());
    }
    ::close(cli);
}

MZ_TEST(ntimed_http_api_stats) {
    ServerFixture fx;
    MZ_ASSERT_TRUE(fx.setup());
    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);

    MZ_ASSERT_TRUE(sendAll(cli, "GET /nope HTTP/1.1\r\n\r\n"));   // 先制造一次 404
    Response resp;
    std::string carry;
    MZ_ASSERT_TRUE(readResponse(cli, &resp, 2000, &carry));
    MZ_ASSERT_EQ(resp.status, 404);

    MZ_ASSERT_TRUE(sendAll(cli, "GET /api/stats HTTP/1.1\r\n\r\n"));
    MZ_ASSERT_TRUE(readResponse(cli, &resp, 2000, &carry));
    MZ_ASSERT_EQ(resp.status, 200);
    MZ_ASSERT_TRUE(resp.get("content-type").find("application/json") != std::string::npos);
    // 统计里必须能看到刚才那次 404（requests 计数在本次请求之前结算 → 1）
    MZ_ASSERT_TRUE(resp.body.find("\"requests\":1") != std::string::npos);
    MZ_ASSERT_TRUE(resp.body.find("\"4xx\":1") != std::string::npos);
    MZ_ASSERT_TRUE(resp.body.find("\"sessions\":") != std::string::npos);
    MZ_ASSERT_TRUE(resp.body.find("\"accepted\":") != std::string::npos);
    // 【M7 / FR-6.1】"运行时长"必须在 /api/stats 里，而且是真的在走（不是写死的 0）
    MZ_ASSERT_TRUE(resp.body.find("\"uptime_ms\":") != std::string::npos);
    MZ_ASSERT_TRUE(resp.body.find("\"uptime_s\":") != std::string::npos);
    sleepMs(30);
    MZ_ASSERT_TRUE(sendAll(cli, "GET /api/stats HTTP/1.1\r\n\r\n"));
    MZ_ASSERT_TRUE(readResponse(cli, &resp, 2000, &carry));
    const size_t pos = resp.body.find("\"uptime_ms\":");
    MZ_ASSERT_TRUE(pos != std::string::npos);
    if (pos != std::string::npos) {
        const uint64_t uptime = static_cast<uint64_t>(
            std::atoll(resp.body.c_str() + pos + std::string("\"uptime_ms\":").size()));
        MZ_ASSERT_GT(uptime, 0u);   // 睡了 30ms → 至少 1ms
    }
    ::close(cli);
}

MZ_TEST(ntimed_http_api_stats_extra_provider) {
    // M5-d（FR-6.1）：上层装配可以把任意模块的统计片段接进 /api/stats。
    // 这里用假片段验证"钩子通了"，真接线由 M7 的 main 做（接 SourceManager::dumpStatsJson()）
    ServerFixture fx;
    MZ_ASSERT_TRUE(fx.setup([](HttpServer &server) {
        const auto previous = server.setExtraStatsProvider([] {
            return std::string("\"fake_module\":{\"hits\":7}");
        });
        MZ_ASSERT_TRUE(!previous); // 第一次设置：返回"上一个"（为空）
    }));

    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);

    MZ_ASSERT_TRUE(sendAll(cli, "GET /api/stats HTTP/1.1\r\n\r\n"));
    Response resp;
    std::string carry;
    MZ_ASSERT_TRUE(readResponse(cli, &resp, 2000, &carry));
    MZ_ASSERT_EQ(resp.status, 200);
    // 原有计数还在，追加片段也在（而且拼在同一个 JSON 对象里）
    MZ_ASSERT_TRUE(resp.body.find("\"requests\":") != std::string::npos);
    MZ_ASSERT_TRUE(resp.body.find("\"fake_module\":{\"hits\":7}") != std::string::npos);
    MZ_ASSERT_TRUE(resp.body.find("}\n") != std::string::npos); // 仍然正常收尾

    // start() 之后再设置：按契约**不生效**（返回空，且不会改动已生效的那个）
    const auto rejected = fx.server->setExtraStatsProvider([] { return std::string(); });
    MZ_ASSERT_TRUE(!rejected);

    ::close(cli);
}

MZ_TEST(ntimed_http_stream_survives_recv_idle) {
    // ★ M6-d 修的 bug（**5 分钟**并发验收才抓到，3~5 秒的验收永远看不到）：
    //   HTTP-FLV 是服务端**单向推流**，客户端全程不发一个字节；而 FR-4.4 的"读空闲 60s"
    //   会在第 60 秒把正在观看的连接掐掉 —— 实测 10 路 × 5 分钟的跑法在 t=61s 全部断开。
    //   修法：**异步流式响应**（`setChunkedAsync()`）关掉该连接的读空闲检测
    //   （写阻塞超时 + TCP keepalive 仍然兜底，见 DESIGN_M6 §5.5）。
    //   本用例把读空闲压到 300ms：修前连接必被关（recv 返回 0 = EOF），修后保持打开（EAGAIN）。
    ServerFixture fx;
    MZ_ASSERT_TRUE(fx.setup([](HttpServer &server) {
        MZ_ASSERT_TRUE(server.setSessionTimeout(300, 0)); // 读空闲 300ms、写阻塞不检测
        MZ_ASSERT_TRUE(server.setRoute("/drip", [](const HttpParser &, HttpResponse &resp) {
            if (!resp.beginChunked()) {
                resp.setStatus(500);
                return;
            }
            (void) resp.setChunkedAsync();   // ★ 声明"异步流式"：框架别兜底结束、读空闲也别管我
            (void) resp.sendChunk("x\n", 2);
        }));
    }));

    const int cli = connectTo(fx.server->port());
    MZ_ASSERT_GT(cli, 0);
    MZ_ASSERT_TRUE(sendAll(cli, "GET /drip HTTP/1.1\r\n\r\n"));

    // 读掉响应头（此后客户端**一个字节都不发**）
    std::string head;
    char buf[512];
    const uint64_t deadline = getCurrentMillisecond() + 1000;
    while (getCurrentMillisecond() < deadline && head.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = ::recv(cli, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        head.append(buf, static_cast<size_t>(n));
    }
    MZ_ASSERT_TRUE(head.find("200") != std::string::npos);
    MZ_ASSERT_TRUE(head.find("chunked") != std::string::npos);

    // 把已经到达的字节**全部排空**：响应头与第一块可能分两个包到达，
    // 残留的那一块会让后面的探测读到数据（曾经因此在全量上下文里变成假红）。
    // 排空之后客户端**一个字节都不发** → 之后读到的任何东西都只可能是"服务端推了"或"连接被关"。
    sleepMs(100);
    while (::recv(cli, buf, sizeof(buf), MSG_DONTWAIT) > 0) {
    }

    // 空闲检查周期会被 clamp 到 ≥1s（见 session.cpp 的 idleCheckPeriod）→ 等 1.5s 才跨过第一个检查点
    sleepMs(1500);
    // 非阻塞探测（不依赖 SO_RCVTIMEO 的语义）：
    //   修前 = 0（连接被读空闲关掉，EOF）→ 本用例为红；修后 = -1 + EAGAIN（连接还开着）
    errno = 0;
    const ssize_t n = ::recv(cli, buf, sizeof(buf), MSG_DONTWAIT);
    MZ_ASSERT_TRUE(n < 0);
    if (n < 0) {
        MZ_ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    }
    ::close(cli);
}
