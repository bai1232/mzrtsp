/*
 * http_server：M3-b 的可运行 HTTP 服务（ROADMAP 的 curl 验收靠它）
 * ============================================================================
 * 用法：
 *   ./http_server [--port 8080] [--recv-idle-ms 60000]
 *
 * 它只装一条自定义路由 `/hello`（演示怎么加路由），其余走内建：
 *   GET /             → 内置测试页
 *   GET /hello        → 自定义路由（示例）
 *   GET /live/N.flv   → 501（M6 接入 HTTP-FLV）
 *   GET /hls/...      → 501（M4–M6 接入 HLS）
 *   其它              → 404；非 GET → 405
 * 配套：scripts/http_test.sh（curl 端到端验收）
 * ============================================================================
 */

#include "mzmedia.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace mzmedia;

namespace {

std::atomic<bool> g_exit{false};

void onSignal(int) {
    g_exit.store(true);
}

} // namespace

int main(int argc, char **argv) {
    uint16_t port = 8080;
    uint32_t recv_idle_ms = Session::kDefaultRecvIdleMs;

    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        auto value = [&argc, &argv, &i](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s 缺少参数\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (key == "--port") {
            port = static_cast<uint16_t>(std::atoi(value("--port")));
        } else if (key == "--recv-idle-ms") {
            recv_idle_ms = static_cast<uint32_t>(std::atol(value("--recv-idle-ms")));
        } else {
            std::fprintf(stderr, "未知参数：%s\n", key.c_str());
            return 2;
        }
    }

    Logger::Instance().add(std::make_shared<ConsoleWriter>());
    ::signal(SIGINT, onSignal);
    ::signal(SIGTERM, onSignal);

    auto server = std::make_shared<HttpServer>();
    // 路由/CORS/上限都必须在 start() **之前**配置（start 之后再配会返回 false 且不生效）
    if (!server->setSessionTimeout(recv_idle_ms, 30000) ||
        !server->setRoute("/hello", [](const HttpParser &req, HttpResponse &resp) {
            resp.setContentType("text/plain; charset=utf-8");
            resp.setBody(std::string("hello from mzmedia, path=") + req.path() + "\n");
        })) {
        return 1;
    }
    if (!server->start(port)) {
        return 1;
    }
    InfoP("http_server 监听 0.0.0.0:%u（curl http://127.0.0.1:%u/）",
          static_cast<unsigned>(server->port()), static_cast<unsigned>(server->port()));

    while (!g_exit.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        InfoP("统计：请求 %llu | 4xx %llu | 5xx %llu | 畸形 %llu | 会话 %zu",
              static_cast<unsigned long long>(server->totalRequests()),
              static_cast<unsigned long long>(server->total4xx()),
              static_cast<unsigned long long>(server->total5xx()),
              static_cast<unsigned long long>(server->totalMalformed()),
              server->tcp().sessionCount());
    }

    server->shutdown();
    InfoP("已退出");
    return 0;
}
