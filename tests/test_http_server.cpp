/*
 * HttpServer / HttpSession 单元测试（M3-b，分组 `ntimed_http`）
 * ============================================================================
 * 覆盖维度（AI_COLLAB §3.4：正常 / 空 / 满 / 断开 / 超大）：
 *   正常  测试页、自定义路由、兜底路由、keep-alive 复用、管线化、CORS
 *   空    未知路径（404）、无 body 请求
 *   满    请求行超长（414）、头部超长（431）、非 GET（405）
 *   断开  Connection: close、畸形请求后服务端仍存活
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
#include <cstring>
#include <map>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
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
    MZ_ASSERT_EQ(fx.server->total5xx(), 1u);
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
    MZ_ASSERT_EQ(fx.server->totalRequests(), 2u);
    MZ_ASSERT_EQ(fx.server->tcp().totalAccepted(), 1u);   // 只建立了 1 条连接
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
    MZ_ASSERT_EQ(fx.server->totalMalformed(), 1u);
    MZ_ASSERT_EQ(fx.server->total4xx(), 1u);
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
    MZ_ASSERT_EQ(fx.server->totalMalformed(), 1u);
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
    MZ_ASSERT_EQ(fx.server->totalMalformed(), 1u);
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
    MZ_ASSERT_EQ(fx.server->totalMalformed(), 0u);   // 合法请求、只是不支持：不算畸形
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
