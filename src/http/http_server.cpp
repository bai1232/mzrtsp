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

#include <cstdio>
#include <cstdlib>
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

/**
 * 单区间 Range（M3-c）：只支持 `bytes=a-b` / `bytes=a-`
 * - 多区间（multipart/byteranges）、后缀区间 `-N`、畸形语法 → **忽略 Range 回 200** + Warn
 *   （刻意的取舍：半吊子 206 比"明确不支持"更危险，见 DESIGN_M3 §8）
 * - 起点越界 → 416
 * @return true = 已按 206/416 处理；false = 按 200 全量返回
 */
bool serveStaticWithRange(const HttpParser &req, HttpResponse &resp, const std::string &body,
                          const std::string &mime) {
    resp.setContentType(mime);
    resp.setHeader("Accept-Ranges", "bytes");

    const std::string *range = req.header("Range");
    if (range == nullptr || range->empty()) {
        resp.setBody(body);
        return false;
    }
    const std::string r = *range;
    const auto ignore = [&](const char *why) {
        WarnP("HttpServer: 忽略 Range（%s）: %s", why, r.c_str());
        resp.setBody(body);
        return false;
    };
    if (r.find(',') != std::string::npos) {
        return ignore("不支持多区间");
    }
    if (r.compare(0, 6, "bytes=") != 0) {
        return ignore("语法不认识");
    }
    const std::string spec = r.substr(6);
    const size_t dash = spec.find('-');
    if (dash == std::string::npos) {
        return ignore("缺 '-'");
    }
    const std::string a_str = spec.substr(0, dash);
    const std::string b_str = spec.substr(dash + 1);
    if (a_str.empty() || a_str.find_first_not_of("0123456789") != std::string::npos) {
        return ignore("起点非法（不支持后缀区间）");
    }
    if (!b_str.empty() && b_str.find_first_not_of("0123456789") != std::string::npos) {
        return ignore("终点非法");
    }
    const long long total = static_cast<long long>(body.size());
    const long long a = std::atoll(a_str.c_str());
    long long b = b_str.empty() ? total - 1 : std::atoll(b_str.c_str());
    if (a < 0 || a >= total || b < a) {
        resp.setStatus(416);
        resp.setHeader("Content-Range", "bytes */" + std::to_string(body.size()));
        resp.setBody("416 Range Not Satisfiable\n");
        return true;
    }
    if (b >= total) {
        b = total - 1;   // 终点越界：按规范裁剪到末尾
    }
    resp.setStatus(206);
    resp.setHeader("Content-Range", "bytes " + std::to_string(a) + "-" + std::to_string(b) + "/" +
                                        std::to_string(body.size()));
    resp.setBody(body.substr(static_cast<size_t>(a), static_cast<size_t>(b - a + 1)));
    return true;
}

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
            // 注入发送出口：处理器可以走 chunked 流式（M3-c 演示 / M6 的 FLV）
            resp.setSender([this](const char *data, size_t len) {
                // send() 返回 0 = 已入队未写出（**不是失败**）；只有 -1 才算失败
                return send(data, len) >= 0;
            });
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
        if (resp.chunked()) {
            // 处理器自己把头和块发出去了：这里只兜底收尾
            // （忘了发结束块会让客户端一直等 —— 那是"静默的挂住"，必须吵出来）
            if (!resp.chunkedEnded()) {
                WarnP("HttpServer: chunked 处理器未调用 endChunked()，这里补上");
                resp.endChunked();
            }
            _owner->onRequestHandled(resp.status());
            return;
        }
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
        setRoute("/", [](const HttpParser &req, HttpResponse &resp) {
            serveStaticWithRange(req, resp, kTestPage, "text/html; charset=utf-8");
        });
    }
    if (!findHandler("/api/stats")) {
        setRoute("/api/stats", [this](const HttpParser &, HttpResponse &resp) {
            TcpServer &t = *_tcp;
            // 用 std::string 拼接而不是固定缓冲：追加统计片段后长度不可控，
            // 固定缓冲会**静默截断**（AI_COLLAB §4.5：静默截断属于失败）
            std::string json;
            json.reserve(512);
            json += "{\"requests\":" + std::to_string(_total_requests.load());
            json += ",\"4xx\":" + std::to_string(_total_4xx.load());
            json += ",\"5xx\":" + std::to_string(_total_5xx.load());
            json += ",\"malformed\":" + std::to_string(_total_malformed.load());
            json += ",\"sessions\":" + std::to_string(t.sessionCount());
            json += ",\"accepted\":" + std::to_string(t.totalAccepted());
            json += ",\"rejected\":" + std::to_string(t.totalRejected());
            json += ",\"idleTimeout\":" + std::to_string(t.totalIdleTimeout());
            json += ",\"recvOverflow\":" + std::to_string(t.totalRecvOverflow());
            json += ",\"sendOverflow\":" + std::to_string(t.totalSendOverflow());
            json += ",\"acceptError\":" + std::to_string(t.totalAcceptError());

            // 上层装配进来的追加片段（M5-d：media 的源 / 订阅统计）
            if (_extra_stats_provider) {
                const std::string extra = _extra_stats_provider();
                if (!extra.empty()) {
                    json += ",";
                    json += extra;
                }
            }
            json += "}\n";

            resp.setContentType("application/json; charset=utf-8");
            resp.setBody(json);
        });
    }
    if (!findHandler("/stream")) {
        setRoute("/stream", [](const HttpParser &, HttpResponse &resp) {
            // chunked 流式演示：头先发、数据分块发 —— M6 的 FLV 就是这个形状
            if (!resp.beginChunked()) {
                resp.setStatus(500);
                resp.setBody("500 beginChunked 失败\n");
                return;
            }
            for (int i = 1; i <= 3; ++i) {
                const std::string chunk = "chunk-" + std::to_string(i) + "\n";
                if (!resp.sendChunk(chunk.data(), chunk.size())) {
                    return;   // 发送失败：连接已坏，交给 Session 的 onError/关闭路径
                }
            }
            resp.endChunked();
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

std::function<std::string()> HttpServer::setExtraStatsProvider(std::function<std::string()> provider) {
    if (_started) {
        // 与路由/上限同一纪律：运行期改它会让 /api/stats 的行为随时刻变化，难以排查
        WarnP("HttpServer::setExtraStatsProvider 在 start() 之后调用：不生效");
        return {};
    }
    std::function<std::string()> previous = _extra_stats_provider;
    _extra_stats_provider = provider;
    return previous;
}

bool HttpServer::setParserLimits(const HttpParser::Limits &limits) {    if (_started) {
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
