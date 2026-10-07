/*
 * flv_http_server：最小 HTTP-FLV 服务器（M6-b）
 * ============================================================================
 * 用法：flv_http_server [--port 8080] [--media-root samples]
 *
 *   curl -N http://127.0.0.1:8080/live/sample.flv > /tmp/x.flv     # 存盘
 *   ffplay http://127.0.0.1:8080/live/sample.flv                   # 直接看画面
 *
 * 它把三件事接起来（这就是 M6-b 的全部内容）：
 *   HTTP 路由 `/live/<name>.flv` → `SourceManager::acquire`（懒启动 / 复用同一个源）
 *   → 每个连接一个 `Subscriber` + `FlvSender`（per-client 时间戳基准、每连接重发 sequence header）
 *   → 通过 `HttpResponse` 的 chunked 出口持续推流；源 EOS 时发结束块并**等排空再关**。
 *
 * 生命周期的关键一步：`FlvSender` 交给 `resp.holdResource()`，由**会话**在连接关闭时释放 ——
 * 于是订阅者被退订（NFR-6），而 drain 回调里的弱引用自然失效，不会在连接断掉之后还去写 socket。
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

extern "C" void onSignal(int) {
    g_exit.store(true);
}

/// 只放行 `[A-Za-z0-9_-]`：**不放行 `/` 与 `.`** → 从根上杜绝目录穿越
bool validName(const std::string &name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/// `/live/sample.flv` → `sample`（只接受 `.flv` 后缀）
bool parseLiveName(const std::string &path, std::string *name) {
    const std::string prefix = "/live/";
    const std::string suffix = ".flv";
    if (path.rfind(prefix, 0) != 0) {
        return false;
    }
    const std::string rest = path.substr(prefix.size());
    if (rest.size() <= suffix.size() ||
        rest.compare(rest.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }
    *name = rest.substr(0, rest.size() - suffix.size());
    return validName(*name);
}

} // namespace

int main(int argc, char **argv) {
    uint16_t port = 8080;
    std::string media_root = "samples";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--media-root" && i + 1 < argc) {
            media_root = argv[++i];
        } else {
            std::printf("用法：%s [--port 8080] [--media-root samples]\n", argv[0]);
            return 1;
        }
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    auto poller = EventPoller::create("flv-http");
    auto server = std::make_shared<HttpServer>(poller);
    auto manager = SourceManager::create(poller);

    // 统计接线（M5-d 留的钩子）：/api/stats 里带上源与订阅者
    (void) server->setExtraStatsProvider([manager] { return manager->dumpStatsJson(); });

    // ★ M6-b 的核心：把 /live/<name>.flv 接到媒体层。
    //   内置的 501 占位是在 `HttpServer::start()` 里带 `findHandler` 守卫注册的，
    //   所以在这里注册就能覆盖它（见 docs/DESIGN_M6.md §3.4）
    const bool route_ok = server->setPrefixRoute(
        "/live/",
        [poller, manager, media_root](const HttpParser &req, HttpResponse &resp) {
            std::string name;
            if (!parseLiveName(req.path(), &name)) {
                resp.setStatus(404);
                resp.setBody("404 Not Found：路径应为 /live/<name>.flv，名字只允许 [A-Za-z0-9_-]\n");
                return;
            }
            const std::string path = media_root + "/" + name + ".mp4";
            if (!fileExists(path)) {
                resp.setStatus(404);
                resp.setBody("404 Not Found：没有这个媒体文件\n");
                return;
            }

            // 懒启动 / 复用：同名请求共享同一个源（一源多消费者）
            MediaSource::Ptr source = manager->acquire(path);
            const Demuxer *demuxer = manager->demuxerFor(path);
            if (!source || demuxer == nullptr) {
                resp.setStatus(500);
                resp.setBody("500 Internal Server Error：源打不开或拿不到流信息\n");
                return;
            }

            // 顺序很讲究：**先建 sender**（它内部会校验编码/初始化数据），**再**发响应头 ——
            // 否则会出现"已经回了 200、才发现这路流封不了"的半成品响应
            auto subscriber = source->subscribe();
            if (!subscriber) {
                resp.setStatus(503);
                resp.setBody("503 Service Unavailable：订阅失败（人数上限 / 关键帧放不下）\n");
                return;
            }
            (void) subscriber->bindPoller(poller); // 消费线程 = 本连接所属的 poller 线程

            FlvSender::Sink sink;
            // ★ 必须用 **chunkWriter()**（带分块帧头），不能用裸 sender()：
            //   裸出口会把 FLV 字节直接灌进 chunked 流 → 客户端报 "Malformed encoding"
            sink.write = resp.chunkWriter();
            const auto end_stream = resp.endStreamFn();
            sink.end = [end_stream]() {
                if (end_stream) {
                    (void) end_stream(); // 发结束块 + 等发送队列排空再关连接
                }
            };
            FlvSender::Ptr sender = FlvSender::create(
                sink, FlvMuxer::Streams{demuxer->firstVideo(), demuxer->firstAudio()},
                std::move(subscriber));
            if (!sender) {
                resp.setStatus(415);
                resp.setBody("415 Unsupported Media Type：这路流封不了 FLV（需 H264 + avcC）\n");
                return;
            }

            resp.setContentType("video/x-flv");
            resp.setHeader("Cache-Control", "no-cache");
            resp.setKeepAlive(false); // 播完就关（配合 shutdownAfterFlush）
            if (!resp.beginChunked()) {
                resp.setStatus(500);
                resp.setBody("500 Internal Server Error：chunked 启动失败\n");
                return;
            }
            (void) resp.setChunkedAsync(); // 异步流：结束块由 FlvSender 自己发，框架别兜底
            resp.holdResource(sender);     // 连接关闭时释放 → 退订（NFR-6）

            // FR-3.1：连接建立后**立即**发 FLV Header + sequence header（不等第一帧）
            const FlvSender::Result started = sender->start();
            if (started != FlvSender::Result::Ok) {
                WarnP("FLV 启动失败：%s —— %s（响应头已发出，只能结束连接）",
                      FlvSender::resultName(started), sender->lastError().c_str());
                return;
            }
        });
    if (!route_ok) {
        std::printf("注册 /live/ 路由失败\n");
        return 1;
    }

    if (!server->start(port)) {
        std::printf("启动失败：端口 %u 被占用？\n", static_cast<unsigned>(port));
        return 1;
    }
    InfoP("flv_http_server 监听 0.0.0.0:%u（媒体目录 %s）",
          static_cast<unsigned>(server->port()), media_root.c_str());
    InfoP("拉流：curl -N http://127.0.0.1:%u/live/sample.flv | ffplay -",
          static_cast<unsigned>(server->port()));

    while (!g_exit.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        InfoP("统计：%s", manager->dumpStatsJson().c_str());
    }

    server->shutdown();
    InfoP("已退出");
    return 0;
}
