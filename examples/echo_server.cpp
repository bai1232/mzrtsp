/*
 * echo_server：M2-3 的可运行示例（同时是 FR-4.4 / NFR-2 的验收工具）
 * ============================================================================
 * 用法：
 *   ./echo_server [--port 9000] [--recv-idle-ms 60000] [--max-session 64]
 *
 * 它演示了两件"正确用法"：
 *   1) **继承 Session 重写 onRecv**（协议分帧就发生在这里）；
 *      比 setOnRead 回调更适合示例，因为真实协议（HTTP/FLV）必然要分帧、要维护状态。
 *   2) 工厂里用 EventPollerPool::getPoller()（默认 prefer_current=true）拿到**当前**
 *      poller → 保持"连接亲和"：这条连接一生都在同一个线程上，读写零锁。
 *
 * 配套：scripts/echo_test.sh（灌 100MB 校验和、并发、空闲超时、fd 回落）
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

/// 回显会话：收到多少回多少
class EchoSession : public Session {
public:
    EchoSession(const Socket::Ptr &sock, const EventPoller::Ptr &poller) : Session(sock, poller) {}

protected:
    void onRecv(const Buffer::Ptr &buf) override {
        // 示例不做分帧；真实协议在这里 find + consume（处理半包与粘包）。
        // send 失败时 Session 内部已经 emitError 并关闭，这里只记一下即可。
        if (send(buf->data(), buf->size()) < 0) {
            InfoP("EchoSession: send 失败，连接即将关闭 (peer=%s:%u)", peerIP().c_str(),
                  static_cast<unsigned>(peerPort()));
        }
    }
};

} // namespace

int main(int argc, char **argv) {
    uint16_t port = 9000;
    uint32_t recv_idle_ms = Session::kDefaultRecvIdleMs;
    size_t max_session = TcpServer::kDefaultMaxSessionCount;

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
        } else if (key == "--max-session") {
            max_session = static_cast<size_t>(std::atol(value("--max-session")));
        } else {
            std::fprintf(stderr, "未知参数：%s\n", key.c_str());
            return 2;
        }
    }

    // 日志要落到控制台，否则 InfoP/ErrorP 没有任何 writer，等于静默（默认行为见 logger.h）
    Logger::Instance().add(std::make_shared<ConsoleWriter>());

    ::signal(SIGINT, onSignal);
    ::signal(SIGTERM, onSignal);

    auto server = std::make_shared<TcpServer>();
    if (!server->setSessionCreator([](const Socket::Ptr &sock) -> Session::Ptr {
            return std::make_shared<EchoSession>(sock, EventPollerPool::Instance().getPoller());
        })) {
        return 1;
    }
    if (!server->setSessionTimeout(recv_idle_ms, 30000)) {
        return 1;
    }
    if (!server->setMaxSessionCount(max_session)) {
        return 1;
    }
    if (!server->start(port)) {
        return 1;
    }
    InfoP("echo server 监听 0.0.0.0:%u（recv_idle=%ums, 写阻塞=30000ms, 连接上限=%zu）",
          static_cast<unsigned>(server->port()), recv_idle_ms, max_session);

    while (!g_exit.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        InfoP("统计：会话 %zu | 累计接受 %llu | 拒绝 %llu | 空闲超时 %llu | 收超限 %llu | "
              "发超限 %llu | accept 错误 %llu",
              server->sessionCount(), static_cast<unsigned long long>(server->totalAccepted()),
              static_cast<unsigned long long>(server->totalRejected()),
              static_cast<unsigned long long>(server->totalIdleTimeout()),
              static_cast<unsigned long long>(server->totalRecvOverflow()),
              static_cast<unsigned long long>(server->totalSendOverflow()),
              static_cast<unsigned long long>(server->totalAcceptError()));
    }

    server->shutdown();
    InfoP("已退出");
    return 0;
}
