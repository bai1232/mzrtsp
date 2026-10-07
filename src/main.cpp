/*
 * mzmedia：完整的 HTTP-FLV 流媒体服务器（M6-c，v0.1 的应用入口）
 * ============================================================================
 * 形状来源：docs/DESIGN_M6.md §3.7；上位需求 `docs/SPEC.md`
 *   FR-7.2「可执行文件**零参数**启动，启动后打印可用的播放 URL」
 *   FR-7.1「支持命令行配置监听端口与媒体目录」（v0.1 用命令行参数，不引入配置文件）
 *   FR-7.3「日志级别可配」；NFR-1「首帧 < 1s」（验收见 scripts/flv_http_test.sh）
 *
 * 用法（零参数即可跑起来）：
 *   ./bin/mzmedia                                    # 0.0.0.0:8080 + ./samples
 *   ./bin/mzmedia --port 9000 --media-root /data     # 显式配置
 *   ./bin/mzmedia --loop                             # 循环推流（浏览器里能一直看）
 *   ./bin/mzmedia --speed 8                          # 8 倍速（压测/调试用）
 *   ./bin/mzmedia --log-level debug                  # 日志级别
 *   ./bin/mzmedia --help
 *
 * 浏览器打开 http://127.0.0.1:8080/ 就是播放页（**flv.js 已入库**，离线也能播；
 * 由 `/flv.min.js` 提供，路径见 `--web-root`）。
 *
 * app 只做"接线"，不做媒体逻辑 —— 三条边界写死在这里：
 *   1) **路径安全**：`/live/<name>.flv` 的 name 只允许 `[A-Za-z0-9_.-]`，
 *      显式拒绝 `..` 与以 `.` 开头；扩展名走白名单（`.mp4/.h264/.mkv/.ts`），
 *      没有扩展名时默认 `.mp4` —— 从根上杜绝目录穿越（不靠"过滤掉 /"这种补丁）；
 *   2) **先建 sender 再回 200**：`FlvSender::create()` 会校验编码与初始化数据，
 *      失败就回 415；否则客户端会拿到一个"响应头都对、流却封不出来"的半成品；
 *   3) **生命周期挂连接**：sender 交给 `resp.holdResource()`，由会话在连接关闭时释放
 *      → 订阅者退订（NFR-6），drain 回调里的弱引用随之失效（不会写已断的 socket）。
 * ============================================================================
 */

#include "mzmedia.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace mzmedia;

namespace {

std::atomic<bool> g_exit{false};

extern "C" void onSignal(int) {
    g_exit.store(true);
}

const char *kUsage =
    "用法：mzmedia [选项]\n"
    "  （不带任何选项即可启动：0.0.0.0:8080，媒体目录 ./samples）\n"
    "\n"
    "  --port <n>          监听端口（默认 8080）\n"
    "  --media-root <dir>  媒体文件目录（默认 samples）；/live/<name>.flv 从这里找文件\n"
    "  --web-root <dir>    静态网页目录（默认自动找 third_party/flv.js，提供 /flv.min.js）\n"
    "  --loop              循环推流：读到文件尾重开并把时间戳接着往后接（浏览器可长时间观看）\n"
    "  --speed <x>         推送倍速（默认 1.0；压测/调试用，> 1 会加快）\n"
    "  --log-level <l>     trace|debug|info|warn|error|fatal（默认 info）\n"
    "  --help              显示本帮助\n"
    "\n"
    "示例：\n"
    "  mzmedia --loop\n"
    "  curl -N http://127.0.0.1:8080/live/sample.flv | ffplay -\n"
    "  ffprobe -f flv http://127.0.0.1:8080/live/sample.mp4.flv   # H264 裸流也能拉\n";

/// 只放行 `[A-Za-z0-9_.-]`，并显式拒绝 `..` / 以 '.' 开头 / 空 / 超长
/// @note 不靠"把 / 过滤掉"：白名单 + 显式拒绝 才是可审计的（目录穿越从根上不成立）
bool validMediaName(const std::string &name) {
    if (name.empty() || name.size() > 96) {
        return false;
    }
    if (name == "." || name.front() == '.') {
        return false;
    }
    if (name.find("..") != std::string::npos) {
        return false;
    }
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/// 允许的容器/裸流扩展名（白名单）
bool allowedExtension(const std::string &ext) {
    return ext == ".mp4" || ext == ".h264" || ext == ".mkv" || ext == ".ts" || ext == ".aac" ||
           ext == ".flv";
}

/**
 * `/live/<name>.flv` → 媒体文件的绝对/相对路径
 * @param name 形如 `sample`（默认 .mp4）或 `sample.h264`（带白名单扩展名）
 * @return false = 路径非法（原因由调用方回 404，并给出可读提示）
 */
bool resolveMediaPath(const std::string &media_root, const std::string &request_path,
                      std::string *name, std::string *full_path) {
    const std::string prefix = "/live/";
    const std::string suffix = ".flv";
    if (request_path.rfind(prefix, 0) != 0) {
        return false;
    }
    std::string rest = request_path.substr(prefix.size());
    if (rest.size() <= suffix.size() ||
        rest.compare(rest.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false; // 必须是 /live/xxx.flv
    }
    rest = rest.substr(0, rest.size() - suffix.size());
    if (!validMediaName(rest)) {
        return false;
    }
    // 有没有扩展名：没有就补 .mp4（M6-b 的行为保持兼容）
    std::string file = rest;
    const size_t dot = rest.rfind('.');
    if (dot == std::string::npos) {
        file += ".mp4";
    } else if (!allowedExtension(rest.substr(dot))) {
        return false; // 扩展名不在白名单：明确拒绝，不"试试看"
    }
    *name = rest;
    *full_path = media_root + "/" + file;
    return true;
}

/// 日志级别名 → 枚举；@return false = 不认识（调用方报错退出）
bool parseLogLevel(const std::string &text, LogLevel *out) {
    if (text == "trace") {
        *out = LogLevel::Trace;
    } else if (text == "debug") {
        *out = LogLevel::Debug;
    } else if (text == "info") {
        *out = LogLevel::Info;
    } else if (text == "warn") {
        *out = LogLevel::Warn;
    } else if (text == "error") {
        *out = LogLevel::Error;
    } else if (text == "fatal") {
        *out = LogLevel::Fatal;
    } else {
        return false;
    }
    return true;
}

/// 找 flv.min.js：先按命令行给的目录，再按"当前目录 / 可执行文件旁边的仓库布局"
std::string findWebRoot(const std::string &explicit_root) {
    std::vector<std::string> candidates;
    if (!explicit_root.empty()) {
        candidates.push_back(explicit_root);
    } else {
        candidates.push_back("third_party/flv.js");                 // 在仓库根目录直接跑
        candidates.push_back(exeDir() + "/../third_party/flv.js");   // build/bin/mzmedia
        candidates.push_back(exeDir() + "/../../third_party/flv.js");
    }
    for (const auto &dir : candidates) {
        const std::string file = dir + "/flv.min.js";
        if (fileExists(file)) {
            return dir;
        }
    }
    return explicit_root; // 没找到就如实返回（启动时会 Warn，页面上会显示原因）
}

} // namespace

int main(int argc, char **argv) {
    uint16_t port = 8080;
    std::string media_root = "samples";
    std::string web_root;
    bool loop = false;
    double speed = 1.0;
    LogLevel level = LogLevel::Info;

    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        const auto value = [&argc, &argv, &i, &key]() -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 %s 缺少取值\n\n%s", key.c_str(), kUsage);
                std::exit(2);
            }
            return argv[++i];
        };
        if (key == "--port") {
            const int parsed = std::atoi(value());
            if (parsed <= 0 || parsed > 65535) {
                std::fprintf(stderr, "--port 非法：%s（应为 1..65535）\n", argv[i]);
                return 2;
            }
            port = static_cast<uint16_t>(parsed);
        } else if (key == "--media-root") {
            media_root = value();
        } else if (key == "--web-root") {
            web_root = value();
        } else if (key == "--loop") {
            loop = true;
        } else if (key == "--speed") {
            speed = std::atof(value());
            if (!(speed > 0.0) || speed > 1000.0) {
                std::fprintf(stderr, "--speed 非法：必须 (0, 1000]（0 不是「不限」）\n");
                return 2;
            }
        } else if (key == "--log-level") {
            if (!parseLogLevel(value(), &level)) {
                std::fprintf(stderr, "--log-level 非法：%s\n", argv[i]);
                return 2;
            }
        } else if (key == "--help" || key == "-h") {
            std::fputs(kUsage, stdout);
            return 0;
        } else {
            // 未知参数**不许静默忽略**：打错一个字母就"看起来启动成功了"是最难查的
            std::fprintf(stderr, "未知参数：%s\n\n%s", key.c_str(), kUsage);
            return 2;
        }
    }

    Logger::Instance().add(std::make_shared<ConsoleWriter>());
    Logger::Instance().setLevel(level); // FR-7.3
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    const std::string flv_js_root = findWebRoot(web_root);
    if (flv_js_root.empty() || !fileExists(flv_js_root + "/flv.min.js")) {
        WarnP("找不到 flv.min.js（--web-root %s）→ 网页会显示「flv.js 未加载」，"
              "但 /live/*.flv 拉流仍然可用",
              web_root.empty() ? "(未指定)" : web_root.c_str());
    }
    if (!isDir(media_root)) {
        WarnP("媒体目录不存在：%s → /live/ 会回 404（先用 scripts/make_samples.sh 生成样本）",
              media_root.c_str());
    }

    auto poller = EventPoller::create("mzmedia");
    auto server = std::make_shared<HttpServer>(poller);
    auto manager = SourceManager::create(poller);

    // 媒体层配置：循环重开（--loop）与推送倍速（--speed）都在这里落地
    SourceManager::Config manager_config;
    manager_config.idle_release_ms = 60000; // ARCHITECTURE §7：无人观看 60s 后释放
    manager_config.throttle.speed = speed;
    manager_config.producer.loop = loop;
    if (!manager->setConfig(manager_config)) {
        ErrorP("SourceManager 配置非法（speed=%f loop=%d）", speed, loop ? 1 : 0);
        return 1;
    }

    // FR-6.1：/api/stats 里带上媒体层的源与订阅统计
    (void) server->setExtraStatsProvider([manager] { return manager->dumpStatsJson(); });

    // 内置 501 占位是在 HttpServer::start() 里带 findHandler 守卫注册的，
    // 所以**在 start() 之前**注册就能覆盖它（见 docs/DESIGN_M6.md §3.4）
    if (!server->setPrefixRoute(
            "/live/",
            [poller, manager, media_root](const HttpParser &req, HttpResponse &resp) {
                std::string name;
                std::string path;
                if (!resolveMediaPath(media_root, req.path(), &name, &path)) {
                    resp.setStatus(404);
                    resp.setBody("404 Not Found：路径应为 /live/<name>.flv；name 允许 "
                                 "[A-Za-z0-9_.-]（不带扩展名时默认 .mp4，"
                                 "也支持 .h264/.mkv/.ts/.aac）\n");
                    return;
                }
                if (!fileExists(path)) {
                    resp.setStatus(404);
                    resp.setBody("404 Not Found：没有这个媒体文件（" + name + "）\n");
                    return;
                }

                // 懒启动 / 复用：同名请求共享同一个源（一源多消费者）
                MediaSource::Ptr source = manager->acquire(path);
                if (!source) {
                    resp.setStatus(500);
                    resp.setBody("500 Internal Server Error：源打不开（" + manager->lastError() +
                                 "）\n");
                    return;
                }
                const SourceManager::MuxerStreams streams = manager->muxerStreamsFor(path);
                if (streams.video == nullptr && streams.audio == nullptr) {
                    resp.setStatus(500);
                    resp.setBody("500 Internal Server Error：这个源没有音视频流\n");
                    return;
                }

                // 顺序讲究：**先建 sender**（它内部校验编码/初始化数据），**再**发响应头
                auto subscriber = source->subscribe();
                if (!subscriber) {
                    resp.setStatus(503);
                    resp.setBody("503 Service Unavailable：订阅失败（人数上限 / 关键帧放不下）\n");
                    return;
                }
                (void) subscriber->bindPoller(poller); // 消费线程 = 连接所属的 poller 线程

                FlvSender::Sink sink;
                // ★ 必须用 chunkWriter()（带分块帧头）：裸 sender() 会把 FLV 字节直接灌进
                //   chunked 流，客户端报 "Malformed encoding"（M6-b 踩过）
                sink.write = resp.chunkWriter();
                const auto end_stream = resp.endStreamFn();
                const auto abort_stream = resp.abortFn();
                sink.end = [end_stream]() {
                    if (end_stream) {
                        (void) end_stream(); // 正常结束：发结束块 + 等排空再关
                    }
                };
                // M6-c：订阅者 broken / 写失败 → 硬关连接（不发结束块，客户端能看到"被截断"）
                sink.abort = [abort_stream]() {
                    if (abort_stream) {
                        (void) abort_stream();
                    }
                };

                FlvSender::Ptr sender = FlvSender::create(
                    sink, FlvMuxer::Streams{streams.video, streams.audio}, std::move(subscriber));
                if (!sender) {
                    resp.setStatus(415);
                    resp.setBody("415 Unsupported Media Type：这路流封不了 FLV"
                                 "（v0.1 需要 H264 + AAC；H264 裸流需带 SPS/PPS）\n");
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
                }
            })) {
        std::fprintf(stderr, "注册 /live/ 路由失败\n");
        return 1;
    }

    // 播放器本体：入库的 flv.js（离线可用）。它不在媒体层里，属于"网页资源"
    if (!server->setRoute("/flv.min.js", [flv_js_root](const HttpParser &, HttpResponse &resp) {
            std::string error;
            const std::string body = loadFile(flv_js_root + "/flv.min.js", &error);
            if (body.empty()) {
                resp.setStatus(404);
                resp.setContentType("text/plain; charset=utf-8");
                resp.setBody("404 Not Found：flv.min.js（" + error + "）\n");
                return;
            }
            resp.setContentType("application/javascript; charset=utf-8");
            resp.setHeader("Cache-Control", "public, max-age=600");
            resp.setBody(body);
        })) {
        std::fprintf(stderr, "注册 /flv.min.js 路由失败\n");
        return 1;
    }

    if (!server->start(port)) {
        std::fprintf(stderr, "启动失败：端口 %u 被占用？\n", static_cast<unsigned>(port));
        return 1;
    }

    // FR-7.2：零参数启动后必须能直接看出"去哪儿看"
    std::printf("\n");
    std::printf("mzmedia 已启动\n");
    std::printf("  播放页    : http://127.0.0.1:%u/          （浏览器打开，点「播放」）\n",
                static_cast<unsigned>(server->port()));
    std::printf("  HTTP-FLV  : http://127.0.0.1:%u/live/<name>.flv\n",
                static_cast<unsigned>(server->port()));
    std::printf("  统计      : http://127.0.0.1:%u/api/stats\n",
                static_cast<unsigned>(server->port()));
    std::printf("  媒体目录  : %s%s\n", media_root.c_str(), isDir(media_root) ? "" : "（不存在！）");
    std::printf("  推送      : %gx%s\n", speed, loop ? "，循环推流" : "");
    std::printf("  命令行拉流: curl -N http://127.0.0.1:%u/live/sample.flv | ffplay -\n",
                static_cast<unsigned>(server->port()));
    std::printf("  Ctrl-C 退出\n\n");
    std::fflush(stdout);

    while (!g_exit.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    server->shutdown(); // 先关服务器（连接全断）→ 订阅者释放 → 源按空闲规则回收
    InfoP("已退出");
    return 0;
}
