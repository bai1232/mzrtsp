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
    /**
     * 额外的统计片段提供者（FR-6.1，M5-d）
     * @param provider 返回值应当是**合法 JSON 对象片段**（不含最外层花括号），
     *                 例如 `"media_source":{...}`；`/api/stats` 会把它拼进同一个 JSON 对象
     * @return **上一个**提供者（沿用"注册返回上一个"的约定）
     * @note 为什么用钩子而不是让 `HttpServer` 依赖 `media`：分层方向是 http ← 上层装配
     *       （M7 的 main 把 `SourceManager::dumpStatsJson()` 接进来），http 层不拖上 media
     */
    std::function<std::string()> setExtraStatsProvider(std::function<std::string()> provider);
    bool setMaxSessionCount(size_t max);
    bool setCorsEnabled(bool enable);

    // ---- 观测 ----
    uint64_t totalRequests() const;
    uint64_t total4xx() const;
    uint64_t total5xx() const;
    uint64_t totalMalformed() const;   // 解析层拒掉的（含 400/414/431）
    /**
     * 【v0.1.1】「发完再关」排程失败的次数（应当恒为 0）
     * @note 什么时候会涨：`Session::shutdownAfterFlush()` 返回 false 且连接**还没在关**
     *       （poller 已退出 / 截止定时器建不出来）。那时必须立刻硬关连接，
     *       否则会留下"活着但永远不回话"的连接 —— 比不关更糟。计数 + Warn 让它可观测。
     */
    uint64_t totalCloseFallback() const;
    /// 网络层计数（会话数 / accept / 空闲超时 / 收超限 / 发超限）
    TcpServer &tcp();

private:
    friend class HttpSession;
    /// 内部：给会话用（会话在 http_server.cpp 里定义）
    void onRequestHandled(int status);
    void onMalformedRequest();
    /// 【v0.1.1】会话报告：「发完再关」排程失败（计数，由会话在硬关前调用）
    void onCloseFallback();
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
    /// 追加统计片段提供者（M5-d）：只在 start() 之前设置，之后读（与路由同一纪律）
    std::function<std::string()> _extra_stats_provider;

    std::atomic<uint64_t> _total_requests{0};
    std::atomic<uint64_t> _total_4xx{0};
    std::atomic<uint64_t> _total_5xx{0};
    std::atomic<uint64_t> _total_malformed{0};
    /// 【v0.1.1】「发完再关」排程失败次数（恒为 0 才正常；见 totalCloseFallback 的说明）
    std::atomic<uint64_t> _total_close_fallback{0};
    /// 【M7】进程/服务启动时刻（单调时钟毫秒）：`/api/stats` 的 `uptime_ms`（FR-6.1 要求"运行时长"）
    uint64_t _started_at_ms = 0;
};

} // namespace mzmedia
