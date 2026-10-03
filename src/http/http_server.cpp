/*
 * HttpServer 实现（M3-b）
 * ============================================================================
 * 请求处理流程（一条路径，没有分叉的"特例"）：
 *   onRecv → HttpParser → Complete → 路由 → 填 HttpResponse → 序列化 → Session::send
 *                     ↘ Error    → 400/414/431 + Connection: close
 *                     ↘ NeedMoreData → 等下一次数据
 *
 * 三个刻意的决定：
 *   1) **异常不穿透**：handler 抛异常 → 500 + ErrorP（否则异常会打到事件循环里，
 *      把整个 poller 线程带走）；
 *   2) 非 GET 一律 405（带 Allow: GET）—— v0.1 不需要 POST，不假装支持；
 *   3) 畸形请求回完就 `Connection: close`，不再解析后续字节（攻击面收敛）。
 * ============================================================================
 */

#include "http/http_server.h"

#include "core/logger.h"

#include <exception>
#include <string>
#include <utility>

namespace mzmedia {

namespace {

/// 内置测试页（SC-1 的落地）：**不依赖外部 CDN**，离线可用
const char *kTestPage =
    "<!DOCTYPE html>\n"
    "<html lang=\"zh-CN\">\n"
    "<head><meta charset=\"utf-8\"><title>mzmedia</title></head>\n"
    "<body>\n"
    "<h1>mzmedia</h1>\n"
    "<p>HTTP 层已就绪（M3-b）。FLV 播放页将在 M6 接入。</p>\n"
    "<ul>\n"
    "  <li>GET /            —— 本页</li>\n"
    "  <li>GET /api/stats    —— 运行统计（M3-c）</li>\n"
    "  <li>GET /live/N.flv   —— HTTP-FLV（M6）</li>\n"
    "</ul>\n"
    "</body>\n"
    "</html>\n";

/// 解析错误 → HTTP 状态码（让客户端能自助定位；具体原因见响应 body）
int statusForParseError(HttpParser::Error err) {
    switch (err) {
    case HttpParser::Error::LineTooLong:
    case HttpParser::Error::UriTooLong:
        return 414;
    case HttpParser::Error::HeadersTooLarge:
    case HttpParser::Error::TooManyHeaders:
        return 431;
    default:
        return 400;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// HttpSession（只在 .cpp 里，M6 需要时再提升为公开类）
// ---------------------------------------------------------------------------

class HttpSession : public Session {
public:
    HttpSession(const Socket::Ptr &sock, const EventPoller::Ptr &poller, HttpServer *owner)
        : Session(sock, poller), _owner(owner), _parser(owner->_limits) {}

protected:
    void onRecv(const Buffer::Ptr &buf) override {
        HttpParser::Status st = _parser.parse(buf->data(), buf->size());
        for (;;) {
            if (st == HttpParser::Status::Error) {
                _owner->onMalformedRequest();
                sendParseError(statusForParseError(_parser.error()), _parser.errorName());
                return;   // 不再解析后续字节：畸形请求的响应带 Connection: close
            }
            if (st == HttpParser::Status::NeedMoreData) {
                return;
            }
            handleOneRequest();
            if (isShutdown()) {
                return;   // 处理/发送过程中连接已关：不再碰任何状态
            }
            const std::string rest = _parser.reset();   // 结果已在 handleOneRequest 里用完
            if (rest.empty()) {
                return;
            }
            st = _parser.parse(rest);   // 粘包 / 管线化：接着解析
        }
    }

private:
    void handleOneRequest() {
        // 从"带基础头（Server/CORS）的 200"起步，处理器可以覆盖任何一项
        HttpResponse resp = _owner->makeResponse(200);
        if (_parser.method() != HttpParser::Method::Get) {
            resp = _owner->makeResponse(405);
            resp.setHeader("Allow", "GET");
            resp.setBody("405 Method Not Allowed（v0.1 只支持 GET）\n");
        } else if (HttpHandler handler = _owner->findHandler(_parser.path())) {
            try {
                handler(_parser, resp);
            } catch (const std::exception &e) {
                ErrorP("HttpServer: handler 抛异常（%s），回 500", e.what());
                resp = _owner->makeResponse(500);
                resp.setBody("500 Internal Server Error\n");
            } catch (...) {
                ErrorP("HttpServer: handler 抛未知异常，回 500");
                resp = _owner->makeResponse(500);
                resp.setBody("500 Internal Server Error\n");
            }
        } else {
            resp = _owner->makeResponse(404);
            resp.setBody("404 Not Found\n");
        }

        resp.setKeepAlive(_parser.keepAlive());
        _owner->onRequestHandled(resp.status());

        const std::string wire = resp.serialize();
        // 返回 0 不是失败（数据已入队、由 EPOLLOUT 续写）；这里只关心连接是否已关闭
        send(wire.data(), wire.size());
    }

    void sendParseError(int status, const char *reason) {
        HttpResponse resp = _owner->makeResponse(status);
        resp.setKeepAlive(false);
        resp.setBody(std::string("HTTP ") + std::to_string(status) + " " +
                     HttpResponse::reasonPhrase(status) + " (" + reason + ")\n");
        _owner->onRequestHandled(status);
        const std::string wire = resp.serialize();
        send(wire.data(), wire.size());
    }

    HttpServer *_owner;
    HttpParser _parser;
};

// ---------------------------------------------------------------------------
// HttpServer
// ---------------------------------------------------------------------------

HttpServer::HttpServer(const EventPoller::Ptr &poller) : _tcp(std::make_shared<TcpServer>(poller)) {}

HttpServer::~HttpServer() {
    _tcp->shutdown();   // 会话先于服务器销毁（TcpServer 的硬约束）
}

bool HttpServer::start(uint16_t port, const std::string &bind_ip) {
    if (_started) {
        WarnP("HttpServer::start 重复调用：忽略");
        return false;
    }
    // 默认路由：只在用户没占用该路径时安装（可预期）
    if (!findHandler("/")) {
        setRoute("/", [](const HttpParser &, HttpResponse &resp) {
            resp.setContentType("text/html; charset=utf-8");
            resp.setBody(kTestPage);
        });
    }
    if (!findHandler("/live/x.flv")) {
        setPrefixRoute("/live/", [](const HttpParser &, HttpResponse &resp) {
            resp.setStatus(501);
            resp.setBody("501 Not Implemented：HTTP-FLV 将在 M6 接入\n");
        });
    }
    if (!findHandler("/hls/x.m3u8")) {
        setPrefixRoute("/hls/", [](const HttpParser &, HttpResponse &resp) {
            resp.setStatus(501);
            resp.setBody("501 Not Implemented：HLS 将在 M4–M6 接入\n");
        });
    }

    if (!_tcp->setSessionCreator([this](const Socket::Ptr &sock) -> Session::Ptr {
            // 工厂在 poller 线程上执行 → getPoller() 拿到当前 poller，保持连接亲和
            return std::make_shared<HttpSession>(sock, EventPollerPool::Instance().getPoller(), this);
        })) {
        return false;
    }
    if (!_tcp->start(port, bind_ip)) {
        return false;
    }
    _started = true;
    InfoP("HttpServer 监听 %s:%u（CORS=%s）", bind_ip.c_str(), static_cast<unsigned>(_tcp->port()),
          _cors_enabled ? "on" : "off");
    return true;
}

bool HttpServer::shutdown() {
    return _tcp->shutdown();
}

bool HttpServer::listening() const {
    return _tcp->listening();
}

uint16_t HttpServer::port() const {
    return _tcp->port();
}

bool HttpServer::setRoute(const std::string &path, HttpHandler handler) {
    if (_started) {
        WarnP("HttpServer::setRoute 在 start() 之后调用：不生效");
        return false;
    }
    if (path.empty() || path[0] != '/' || !handler) {
        ErrorP("HttpServer::setRoute 参数非法（path 必须以 '/' 开头且 handler 非空）");
        return false;
    }
    for (auto &kv : _exact_routes) {
        if (kv.first == path) {
            kv.second = std::move(handler);   // 覆盖
            return true;
        }
    }
    _exact_routes.emplace_back(path, std::move(handler));
    return true;
}

bool HttpServer::setPrefixRoute(const std::string &prefix, HttpHandler handler) {
    if (_started) {
        WarnP("HttpServer::setPrefixRoute 在 start() 之后调用：不生效");
        return false;
    }
    if (prefix.empty() || prefix[0] != '/' || !handler) {
        ErrorP("HttpServer::setPrefixRoute 参数非法（prefix 必须以 '/' 开头且 handler 非空）");
        return false;
    }
    for (auto &kv : _prefix_routes) {
        if (kv.first == prefix) {
            kv.second = std::move(handler);
            return true;
        }
    }
    _prefix_routes.emplace_back(prefix, std::move(handler));
    return true;
}

bool HttpServer::setFallback(HttpHandler handler) {
    if (_started || !handler) {
        WarnP("HttpServer::setFallback 未生效（已 start 或 handler 为空）");
        return false;
    }
    _fallback = std::move(handler);
    return true;
}

bool HttpServer::setParserLimits(const HttpParser::Limits &limits) {
    if (_started) {
        WarnP("HttpServer::setParserLimits 在 start() 之后调用：不生效");
        return false;
    }
    _limits = limits;
    return true;
}

bool HttpServer::setSessionTimeout(uint32_t recv_idle_ms, uint32_t send_blocked_ms) {
    return _tcp->setSessionTimeout(recv_idle_ms, send_blocked_ms);
}

bool HttpServer::setMaxSessionCount(size_t max) {
    return _tcp->setMaxSessionCount(max);
}

bool HttpServer::setCorsEnabled(bool enable) {
    if (_started) {
        WarnP("HttpServer::setCorsEnabled 在 start() 之后调用：不生效");
        return false;
    }
    _cors_enabled = enable;
    return true;
}

uint64_t HttpServer::totalRequests() const {
    return _total_requests.load();
}

uint64_t HttpServer::total4xx() const {
    return _total_4xx.load();
}

uint64_t HttpServer::total5xx() const {
    return _total_5xx.load();
}

uint64_t HttpServer::totalMalformed() const {
    return _total_malformed.load();
}

TcpServer &HttpServer::tcp() {
    return *_tcp;
}

void HttpServer::onRequestHandled(int status) {
    _total_requests.fetch_add(1);
    if (status >= 500) {
        _total_5xx.fetch_add(1);
    } else if (status >= 400) {
        _total_4xx.fetch_add(1);
    }
}

void HttpServer::onMalformedRequest() {
    _total_malformed.fetch_add(1);
}

HttpHandler HttpServer::findHandler(const std::string &path) const {
    for (const auto &kv : _exact_routes) {
        if (kv.first == path) {
            return kv.second;
        }
    }
    for (const auto &kv : _prefix_routes) {
        if (path.compare(0, kv.first.size(), kv.first) == 0) {
            return kv.second;
        }
    }
    return _fallback;   // 可能是空的：调用方按 404 处理
}

HttpResponse HttpServer::makeResponse(int status) const {
    HttpResponse resp(status);
    resp.setHeader("Server", "mzmedia");
    resp.setContentType("text/plain; charset=utf-8");
    if (_cors_enabled) {
        resp.setHeader("Access-Control-Allow-Origin", "*");   // FR-4.3
    }
    return resp;
}

} // namespace mzmedia
