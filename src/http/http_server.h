/*
 * HttpServer：M3-b 的对外入口（TcpServer + HTTP 语义）
 * ============================================================================
 * 形状来源：docs/DESIGN_M3.md §4/§5
 *
 * 它只做四件事，其余都复用 M2：
 *   1) 把每个连接包成 HttpSession（重写 onRecv 做分帧）；
 *   2) 路由：精确路径 + 前缀（/live/、/hls/）+ 兜底；
 *   3) 错误语义：解析错 → 400/414/431；不支持的方法 → 405（带 Allow）；无路由 → 404；
 *      handler 抛异常 → 500（**不让异常穿透到事件循环**）；
 *   4) 观测：请求数 / 4xx / 5xx / 畸形数（+ 透传 TcpServer 的网络计数）。
 *
 * CORS：默认所有响应带 `Access-Control-Allow-Origin: *`（FR-4.3，flv.js 跨域要它）。
 *
 * 命名：ROADMAP 里写的是 `HttpConnection`，这里是 `HttpSession` —— 因为 M2 的基类叫
 * `Session`，同一架构里保持一个词（决策记录见 DESIGN_M3 §8）。
 * ============================================================================
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "http/http_parser.h"
#include "http/http_response.h"
#include "network/event_poller.h"
#include "network/tcp_server.h"

namespace mzmedia {

/// 请求处理函数：拿到已解析的请求，填响应。
/// 抛异常 → 服务器回 500 并计数（不会把事件循环带崩）
using HttpHandler = std::function<void(const HttpParser &req, HttpResponse &resp)>;

class HttpServer {
public:
    using Ptr = std::shared_ptr<HttpServer>;

    explicit HttpServer(const EventPoller::Ptr &poller = nullptr);
    ~HttpServer();

    HttpServer(const HttpServer &) = delete;
    HttpServer &operator=(const HttpServer &) = delete;

    // ---- 启停（返回 false 时已记日志，调用方必须检查）----
    bool start(uint16_t port, const std::string &bind_ip = "0.0.0.0");
    bool shutdown();
    bool listening() const;
    uint16_t port() const;

    // ---- 配置（必须 start() 之前；返回 false = 未生效）----
    /// 精确路径路由（默认只对 GET 生效；其它方法一律 405）
    bool setRoute(const std::string &path, HttpHandler handler);
    /// 前缀路由（如 "/live/"）：handler 里用 req.path() 自己判断
    bool setPrefixRoute(const std::string &prefix, HttpHandler handler);
    /// 都没匹配上时的兜底（默认 = 404 文本）
    bool setFallback(HttpHandler handler);
    bool setParserLimits(const HttpParser::Limits &limits);
    bool setSessionTimeout(uint32_t recv_idle_ms, uint32_t send_blocked_ms);
    bool setMaxSessionCount(size_t max);
    bool setCorsEnabled(bool enable);

    // ---- 观测 ----
    uint64_t totalRequests() const;
    uint64_t total4xx() const;
    uint64_t total5xx() const;
    uint64_t totalMalformed() const;   // 解析层拒掉的（含 400/414/431）
    /// 网络层计数（会话数 / accept / 空闲超时 / 收超限 / 发超限）
    TcpServer &tcp();

private:
    friend class HttpSession;
    /// 内部：给会话用（会话在 http_server.cpp 里定义）
    void onRequestHandled(int status);
    void onMalformedRequest();
    HttpHandler findHandler(const std::string &path) const;
    /// 建一个已填好 CORS 头的响应
    HttpResponse makeResponse(int status) const;
    bool sendError(class HttpSession &session, int status, const std::string &detail);

    std::shared_ptr<TcpServer> _tcp;
    std::vector<std::pair<std::string, HttpHandler>> _exact_routes;
    std::vector<std::pair<std::string, HttpHandler>> _prefix_routes;
    HttpHandler _fallback;
    HttpParser::Limits _limits;
    bool _cors_enabled = true;
    bool _started = false;

    std::atomic<uint64_t> _total_requests{0};
    std::atomic<uint64_t> _total_4xx{0};
    std::atomic<uint64_t> _total_5xx{0};
    std::atomic<uint64_t> _total_malformed{0};
};

} // namespace mzmedia
