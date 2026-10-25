/*
 * mzmedia：完整的 HTTP-FLV 流媒体服务器（M6-c，v0.1 的应用入口）
 * ============================================================================
 * 形状来源：docs/DESIGN_M6.md §3.7；上位需求 `docs/SPEC.md`
 *   FR-7.2「可执行文件**零参数**启动，启动后打印可用的播放 URL」
 *   FR-7.1「支持命令行配置监听端口与媒体目录」（v0.1 用命令行参数，不引入配置文件）
 *   FR-7.1「命令行参数：监听端口、媒体文件路径、日志级别、是否循环、转码开关」
 *   （v0.1 的落地：--port / --media-root / --log-level / --loop；转码开关属 v0.2）
 *   FR-6.2「日志分级、异步、滚动文件（默认 logs/mzmedia.log）」
 *   NFR-1「首帧 < 1s」（验收见 scripts/flv_http_test.sh）
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
    "  --transcode         允许转码（默认关；开启后 URL 加 ?transcode=1[&h=720] 才真的转）\n"
    "  --transcode-height <n>  默认目标高度（720）：没给 h= 时当「上限」用（源更高才缩）；\n"
    "                      0 = 只重编码不缩放；显式 h= 须偶数且不高过源\n"
    "  --transcode-max <n> 同时最多几路源在转码（默认 nproc-1；0 不是「不限」，是非法）\n"
    "  --rtsp <url>[#name] 以 RTSP 为输入（可重复；name 缺省 rtsp1、rtsp2…，用 /live/<name>.flv 拉）\n"
    "  --rtsp-timeout-ms <n>  网络流读超时（默认 10000；> 这个时间没有包就判定断流）\n"
    "  --rtsp-transport <t>   auto|tcp|udp（默认 auto = UDP 优先、被拒自动回退 TCP）\n"
    "  --hls-lease-ms <n>  HLS 拉流请求的租约（默认 60000；到期且没人拉就释放源，FR-1.3）\n"
    "  --log-level <l>     trace|debug|info|warn|error|fatal（默认 info）\n"
    "  --log-file <path>   日志文件（默认 logs/mzmedia.log，64MB×5 滚动；FR-6.2）\n"
    "  --help              显示本帮助\n"
    "\n"
    "示例：\n"
    "  mzmedia --loop\n"
    "  curl -N http://127.0.0.1:8080/live/sample.flv | ffplay -\n"
    "  mzmedia --transcode --transcode-height 480\n"
    "  mzmedia --rtsp rtsp://127.0.0.1:8554/live#cam1\n"
    "  ffplay -f flv http://127.0.0.1:8080/live/cam1.flv\n"
    "  curl -N 'http://127.0.0.1:8080/live/sample.flv?transcode=1&h=480' | ffplay -\n"
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
 * 媒体名 → 文件路径（M6-b；M9-b 抽出来给 `/hls/` 复用 —— 两处必须是**同一套**白名单）
 * @param name 形如 `sample`（默认 .mp4）或 `sample.h264`（带白名单扩展名）
 * @return false = 名字/扩展名非法（原因写进 `why`，调用方回 404 时可读）
 */
bool mediaNameToPath(const std::string &media_root, const std::string &name, std::string *full_path,
                     std::string *why) {
    if (!validMediaName(name)) {
        *why = "媒体名只允许 [A-Za-z0-9_.-]（不带扩展名时默认 .mp4）";
        return false;
    }
    // 有没有扩展名：没有就补 .mp4（M6-b 的行为保持兼容）
    std::string file = name;
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) {
        file += ".mp4";
    } else if (!allowedExtension(name.substr(dot))) {
        *why = "扩展名不在白名单（.mp4/.h264/.mkv/.ts/.aac/.flv）";
        return false; // 明确拒绝，不"试试看"
    }
    *full_path = media_root + "/" + file;
    return true;
}

/**
 * `/live/<name>.flv` → 媒体文件的绝对/相对路径
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
    std::string why;
    if (!mediaNameToPath(media_root, rest, full_path, &why)) {
        return false;
    }
    *name = rest;
    return true;
}

/**
 * `/hls/<name>.m3u8` 与 `/hls/<name>-<seq>.ts` → (name, 媒体路径, seq, 是不是播放列表)
 * @note 名字规则与 `/live/` **完全一致**（同一套 `mediaNameToPath`）：
 *       不带扩展名默认 `.mp4`，也支持 `.h264/.mkv/.ts/.aac/.flv`
 */
bool resolveHlsPath(const std::string &media_root, const std::string &request_path,
                    std::string *name, std::string *full_path, uint64_t *seq, bool *is_playlist,
                    std::string *why) {
    const std::string prefix = "/hls/";
    if (request_path.rfind(prefix, 0) != 0) {
        *why = "路径必须以 /hls/ 开头";
        return false;
    }
    std::string rest = request_path.substr(prefix.size());
    *is_playlist = false;
    if (rest.size() > 5 && rest.compare(rest.size() - 5, 5, ".m3u8") == 0) {
        rest = rest.substr(0, rest.size() - 5);
        *is_playlist = true;
    } else if (rest.size() > 3 && rest.compare(rest.size() - 3, 3, ".ts") == 0) {
        rest = rest.substr(0, rest.size() - 3);
        const size_t dash = rest.rfind('-');
        if (dash == std::string::npos || dash + 1 >= rest.size()) {
            *why = "切片路径应为 /hls/<name>-<seq>.ts";
            return false;
        }
        const std::string digits = rest.substr(dash + 1);
        if (digits.find_first_not_of("0123456789") != std::string::npos) {
            *why = "切片序号必须是纯数字";
            return false;
        }
        *seq = std::strtoull(digits.c_str(), nullptr, 10);
        rest = rest.substr(0, dash);
    } else {
        *why = "HLS 只提供 /hls/<name>.m3u8 与 /hls/<name>-<seq>.ts";
        return false;
    }
    if (rest.empty()) {
        *why = "缺媒体名";
        return false;
    }
    if (!mediaNameToPath(media_root, rest, full_path, why)) {
        return false;
    }
    *name = rest;
    return true;
}

/**
 * 【M10-b / FR-7.1】解析 `/live/` 的转码查询参数：`?transcode=1&h=720`
 * @param spec 出参（成功时填好；`enabled == false` 表示拉原始码流）
 * @param why  失败原因（调用方回 400）
 * @return false = 参数非法（**未知参数一律拒**：静默忽略一个拼错的参数最难查）
 */
bool parseTranscodeQuery(const std::string &query, SourceManager::TranscodeSpec *spec,
                         std::string *why) {
    *spec = SourceManager::TranscodeSpec{};
    if (query.empty()) {
        return true; // 不带参数 = 拉原始码流（默认行为，与 v0.1 一致）
    }
    bool has_transcode = false;
    bool transcode_on = false;
    bool has_h = false;
    int height = 0;
    size_t pos = 0;
    while (pos < query.size()) {
        const size_t amp = query.find('&', pos);
        const size_t end = (amp == std::string::npos) ? query.size() : amp;
        const std::string pair = query.substr(pos, end - pos);
        pos = end + 1; // 跳过 '&'
        if (pair.empty()) {
            *why = "查询串里有空参数项（形如 `&&` 或结尾的 `&`）";
            return false;
        }
        const size_t eq = pair.find('=');
        const std::string key = pair.substr(0, eq);
        const std::string value = (eq == std::string::npos) ? std::string() : pair.substr(eq + 1);
        if (key == "transcode") {
            if (has_transcode) {
                *why = "transcode 参数重复";
                return false;
            }
            has_transcode = true;
            if (value == "1" || value == "true" || value == "yes" || value == "on") {
                transcode_on = true;
            } else if (value == "0" || value == "false" || value == "no" || value == "off") {
                transcode_on = false;
            } else {
                *why = "transcode 只接受 1/0（true/false/yes/no/on/off），当前「" + value + "」";
                return false;
            }
        } else if (key == "h") {
            if (has_h) {
                *why = "h 参数重复";
                return false;
            }
            has_h = true;
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) {
                *why = "h 必须是正整数（目标高度像素），当前「" + value + "」";
                return false;
            }
            const long parsed = std::strtol(value.c_str(), nullptr, 10);
            if (parsed < 120 || parsed > 2160) {
                *why = "h 超出 [120, 2160]（有界：再小没意义，再大等于把 CPU 打满）";
                return false;
            }
            if ((parsed & 1) != 0) {
                *why = "h 必须是偶数（yuv420p 的色度下采样要求），当前 " + value;
                return false;
            }
            height = static_cast<int>(parsed);
        } else {
            *why = "未知查询参数「" + key + "」（/live/ 只支持 transcode 与 h）";
            return false;
        }
    }
    if (has_h && !transcode_on) {
        // 给了高度但没要求转码：多半是漏了 transcode=1。**不猜**，让用户把话说全
        *why = "h 只在 transcode=1 时有效（不转码就没有「目标高度」这回事）";
        return false;
    }
    spec->enabled = transcode_on;
    spec->out_height = height;
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

/// 找 hls.min.js（M9-c）：搜索顺序与 flv.js 相同（当前目录 → 可执行文件旁边）
std::string findHlsJsDir() {
    const std::vector<std::string> candidates = {
        "third_party/hls.js",                  // 在仓库根目录直接跑
        exeDir() + "/../third_party/hls.js",   // build/bin/mzmedia
        exeDir() + "/../../third_party/hls.js",
    };
    for (const auto &dir : candidates) {
        if (fileExists(dir + "/hls.min.js")) {
            return dir;
        }
    }
    return {};
}

} // namespace

int main(int argc, char **argv) {
    uint16_t port = 8080;
    std::string media_root = "samples";
    std::string web_root;
    std::string log_file = "logs/mzmedia.log"; // FR-6.2 的默认值
    bool loop = false;
    double speed = 1.0;
    uint32_t hls_lease_ms = 60000;   // FR-1.3 的 60s 同量级；可调是为了让验收脚本能验"租约到期释放"
    LogLevel level = LogLevel::Info;
    // 【M11-a】RTSP 输入：命令行注册"名字 → URL"（**绝不**从 URL query 取地址，避免 SSRF）
    bool rtsp_enabled = false;
    int rtsp_timeout_ms = 10000;
    DemuxerProducer::RtspConfig::Transport rtsp_transport =
        DemuxerProducer::RtspConfig::Transport::Auto;
    std::vector<std::pair<std::string, std::string>> rtsp_sources; // (name, url)
    // 【M10-b / FR-7.1】转码：默认**关**（不带 --transcode 时行为与 v0.1 完全一致）
    bool transcode_allowed = false;
    int transcode_height = 720;      // 默认把 1080p 降到 720p（NFR-5 的口径）
    // 并发上限默认 nproc-1：留一个核给解封装/网络/日志（拿不到核数就按 2 算，不猜更大的机器）
    unsigned cores = std::thread::hardware_concurrency();
    if (cores == 0) {
        cores = 2;
    }
    uint32_t transcode_max = (cores > 1) ? (cores - 1) : 1;

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
        } else if (key == "--transcode") {
            transcode_allowed = true;
        } else if (key == "--transcode-height") {
            const int parsed = std::atoi(value());
            // 这里 0 **有明确含义**（只重编码、不缩放），不是"不限"；奇数/越界一律拒
            if (parsed < 0 || parsed > 2160 ||
                (parsed > 0 && (parsed < 120 || (parsed & 1) != 0))) {
                std::fprintf(stderr,
                             "--transcode-height 非法：%s（0 = 只重编码；否则必须是 [120, 2160] 的**偶数**）\n",
                             argv[i]);
                return 2;
            }
            transcode_height = parsed;
        } else if (key == "--transcode-max") {
            const int parsed = std::atoi(value());
            if (parsed < 1 || parsed > 64) {
                std::fprintf(stderr, "--transcode-max 非法：应为 1..64（0 不是「不限」，是非法）\n");
                return 2;
            }
            transcode_max = static_cast<uint32_t>(parsed);
        } else if (key == "--rtsp") {
            const std::string raw = value();
            // 形式：<url>[#<name>]；名字只允许 [A-Za-z0-9_.-]（与 /live/ 同一套白名单）
            std::string url = raw;
            std::string name;
            const size_t hash = raw.rfind('#');
            if (hash != std::string::npos) {
                url = raw.substr(0, hash);
                name = raw.substr(hash + 1);
            }
            if (!isNetworkMediaUrl(url)) {
                std::fprintf(stderr, "--rtsp 只接受 rtsp:// 或 rtsps:// 开头的 URL：%s\n", raw.c_str());
                return 2;
            }
            if (name.empty()) {
                name = "rtsp" + std::to_string(rtsp_sources.size() + 1);
            }
            if (!validMediaName(name)) {
                std::fprintf(stderr, "--rtsp 的名字非法：%s（只允许 [A-Za-z0-9_.-]）\n", name.c_str());
                return 2;
            }
            for (const auto &item : rtsp_sources) {
                if (item.first == name) {
                    std::fprintf(stderr, "--rtsp 名字重复：%s\n", name.c_str());
                    return 2;
                }
            }
            rtsp_sources.emplace_back(name, url);
            rtsp_enabled = true;
        } else if (key == "--rtsp-timeout-ms") {
            const int parsed = std::atoi(value());
            if (parsed < 2000 || parsed > 60000) {
                std::fprintf(stderr, "--rtsp-timeout-ms 非法：应为 2000..60000（0 不是「不限」）\n");
                return 2;
            }
            rtsp_timeout_ms = parsed;
        } else if (key == "--rtsp-transport") {
            const std::string text = value();
            if (text == "auto") {
                rtsp_transport = DemuxerProducer::RtspConfig::Transport::Auto;
            } else if (text == "tcp") {
                rtsp_transport = DemuxerProducer::RtspConfig::Transport::Tcp;
            } else if (text == "udp") {
                rtsp_transport = DemuxerProducer::RtspConfig::Transport::Udp;
            } else {
                std::fprintf(stderr, "--rtsp-transport 非法：%s（auto|tcp|udp）\n", text.c_str());
                return 2;
            }
        } else if (key == "--hls-lease-ms") {
            const int parsed = std::atoi(value());
            if (parsed < 500 || parsed > 3600000) {
                // 有界性不可协商：0 不是"无限租约"，而是非法
                std::fprintf(stderr, "--hls-lease-ms 非法：应为 500..3600000 ms\n");
                return 2;
            }
            hls_lease_ms = static_cast<uint32_t>(parsed);
        } else if (key == "--log-level") {
            if (!parseLogLevel(value(), &level)) {
                std::fprintf(stderr, "--log-level 非法：%s\n", argv[i]);
                return 2;
            }
        } else if (key == "--log-file") {
            log_file = value();
            if (log_file.empty()) {
                std::fprintf(stderr, "--log-file 不能为空\n");
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
    // FR-6.2：**默认滚动文件日志** `logs/mzmedia.log`（单文件 64MB、保留最近 5 个 —— FileWriter 的默认值）
    //   · 父目录不存在会自动创建；
    //   · 打开失败**不静默**：FileWriter 内部会向 stderr 告警一次，这里再显式提示并继续只用控制台
    //     （"日志系统自己坏了"属于必须被看见的问题，不能悄悄降级）
    auto file_writer = std::make_shared<FileWriter>(log_file);
    if (!file_writer->isOpen()) {
        std::fprintf(stderr,
                     "警告：日志文件 %s 打开失败 → 本次只输出到控制台（检查目录权限/磁盘空间）\n",
                     log_file.c_str());
    }
    Logger::Instance().add(file_writer);
    Logger::Instance().setLevel(level); // FR-7.1（"日志级别"）；FR-6.2 的滚动文件见上
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    const std::string flv_js_root = findWebRoot(web_root);
    if (flv_js_root.empty() || !fileExists(flv_js_root + "/flv.min.js")) {
        WarnP("找不到 flv.min.js（--web-root %s）→ 网页会显示「flv.js 未加载」，"
              "但 /live/*.flv 拉流仍然可用",
              web_root.empty() ? "(未指定)" : web_root.c_str());
    }
    const std::string hls_js_dir = findHlsJsDir();
    if (hls_js_dir.empty()) {
        WarnP("找不到 hls.js（third_party/hls.js/hls.min.js）→ 播放页只能靠浏览器**原生** HLS"
              "（Safari/iOS）；Chrome/Firefox 播不了。HLS 拉流本身（ffplay/ffmpeg/curl）不受影响");
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
    manager_config.max_transcoding = transcode_max; // FR-1.3：转码并发是硬上限，0 会被拒
    // 【M11-a】RTSP：开关 + 网络流超时 + 传输方式
    manager_config.rtsp.enabled = rtsp_enabled;
    manager_config.rtsp.timeout_ms = rtsp_timeout_ms;
    manager_config.rtsp.transport = rtsp_transport;
    if (!manager->setConfig(manager_config)) {
        ErrorP("SourceManager 配置非法（speed=%f loop=%d max_transcoding=%u）", speed, loop ? 1 : 0,
               transcode_max);
        return 1;
    }

    // FR-6.1：/api/stats 里带上媒体层的源与订阅统计
    (void) server->setExtraStatsProvider([manager] { return manager->dumpStatsJson(); });

    // ---- M9-b：HLS（一源一份切片 + 租约式空闲释放）----
    // 租约 60s（FR-1.3 同量级）：任意 /hls/ 请求续租；到期且无订阅者 → 释放源。
    HlsService::Config hls_config;
    hls_config.writer.target_duration_ms = 2000;   // FR-3.2
    hls_config.writer.window = 6;                  // FR-3.2
    hls_config.lease_ms = hls_lease_ms;
    hls_config.sweep_ms = (hls_lease_ms < 4000 ? (hls_lease_ms / 4) : 5000);   // 短租约 → 短扫描间隔
    HlsService::Ptr hls = HlsService::create(manager, poller, hls_config);
    if (!hls) {
        std::fprintf(stderr, "HLS 初始化失败（/hls/ 将回 501）\n");
    } else {
        (void) server->setExtraStatsProvider([manager, hls] {
            return manager->dumpStatsJson() + "," + hls->dumpStatsJson();
        });
        // 必须在 start() 之前注册 /hls/：HttpServer::start() 里的 501 占位有 findHandler 守卫
        if (!server->setPrefixRoute(
                "/hls/", [manager, hls, media_root, transcode_allowed, transcode_height,
                          rtsp_sources](const HttpParser &req, HttpResponse &resp) {
                    // 【M10-c】HLS 也支持转码：`/hls/<name>.m3u8?transcode=1&h=720`
                    // 切片 URI 会带上**同一份**查询串（见 HlsService），播放器照抄即可
                    SourceManager::TranscodeSpec spec;
                    std::string query_why;
                    if (!parseTranscodeQuery(req.query(), &spec, &query_why)) {
                        resp.setStatus(400);
                        resp.setBody("400 Bad Request：" + query_why +
                                     "\n→ 形式：/hls/<name>.m3u8?transcode=1&h=720\n");
                        return;
                    }
                    if (spec.enabled) {
                        if (!transcode_allowed) {
                            resp.setStatus(501);
                            resp.setBody("501 Not Implemented：本实例没有开启转码（启动时缺 "
                                         "--transcode）\n→ 用 `mzmedia --transcode` 启动；"
                                         "或去掉 ?transcode=1 拉原始码流\n");
                            return;
                        }
                        spec.max_height = transcode_height; // 没给 h= → 上限（与 /live/ 一致）
                    }
                    std::string name;
                    std::string path;
                    uint64_t seq = 0;
                    bool is_playlist = false;
                    std::string why;
                    if (!resolveHlsPath(media_root, req.path(), &name, &path, &seq, &is_playlist,
                                        &why)) {
                        resp.setStatus(404);
                        resp.setBody("404 Not Found：" + why + "\n");
                        return;
                    }
                    // 【M11-a】RTSP 源：名字命中注册表 → 用 URL（与 /live/ 同一套规则）
                    for (const auto &item : rtsp_sources) {
                        if (item.first == name) {
                            path = item.second;
                            break;
                        }
                    }
                    std::string body;
                    std::string error;
                    if (is_playlist) {
                        if (!hls->playlist(name, path, spec, &body, &error)) {
                            resp.setStatus(500);
                            resp.setBody("500 Internal Server Error：HLS 打不开（" + error + "）\n");
                            return;
                        }
                        resp.setHeader("Cache-Control", "no-cache");
                        resp.setBody(body);
                        resp.setContentType("application/vnd.apple.mpegurl");
                        return;
                    }
                    if (!hls->slice(name, spec, seq, &body, &error)) {
                        // 老切片滑出窗口 → 404 是**正常**的（播放器拿旧 m3u8 会这样请求）
                        resp.setStatus(404);
                        resp.setBody("404 Not Found：" + error + "\n");
                        return;
                    }
                    resp.setHeader("Cache-Control", "no-cache");
                    // 切片在内存里，但要与静态文件一样支持 Range（FR-4.2）
                    (void) serveMemoryAsStatic(req, resp, body, "video/mp2t");
                })) {
            std::fprintf(stderr, "注册 /hls/ 路由失败\n");
            return 1;
        }
    }

    // 内置 501 占位是在 HttpServer::start() 里带 findHandler 守卫注册的，
    // 所以**在 start() 之前**注册就能覆盖它（见 docs/DESIGN_M6.md §3.4）
    if (!server->setPrefixRoute(
            "/live/",
            [poller, manager, media_root, transcode_allowed, transcode_height, rtsp_sources](
                const HttpParser &req, HttpResponse &resp) {
                std::string name;
                std::string path;
                if (!resolveMediaPath(media_root, req.path(), &name, &path)) {
                    resp.setStatus(404);
                    resp.setBody("404 Not Found：路径应为 /live/<name>.flv；name 允许 "
                                 "[A-Za-z0-9_.-]（不带扩展名时默认 .mp4，"
                                 "也支持 .h264/.mkv/.ts/.aac）\n");
                    return;
                }
                // 【M11-a】RTSP 源：名字命中命令行注册表 → 用 URL 取代文件路径（**跳过 fileExists**）
                bool is_rtsp = false;
                for (const auto &item : rtsp_sources) {
                    if (item.first == name) {
                        path = item.second;
                        is_rtsp = true;
                        break;
                    }
                }
                if (!is_rtsp && !fileExists(path)) {
                    resp.setStatus(404);
                    resp.setBody("404 Not Found：没有这个媒体文件（" + name + "）\n");
                    return;
                }

                // 【M10-b】转码请求：`?transcode=1[&h=720]`；不带参数 = 原始码流（默认行为）
                SourceManager::TranscodeSpec spec;
                std::string query_why;
                // `?transcode=1`：把 --transcode-height 当上限；`?transcode=1&h=480`：明确要 480p
                if (!parseTranscodeQuery(req.query(), &spec, &query_why)) {
                    resp.setStatus(400);
                    resp.setBody("400 Bad Request：" + query_why +
                                 "\n→ 形式：/live/<name>.flv?transcode=1&h=720\n");
                    return;
                }
                if (spec.enabled) {
                    if (!transcode_allowed) {
                        // **不静默降级成直通**：那会让人以为"转过了"
                        resp.setStatus(501);
                        resp.setBody(
                            "501 Not Implemented：本实例没有开启转码（启动时缺 --transcode）\n"
                            "→ 用 `mzmedia --transcode` 启动；或去掉 ?transcode=1 拉**原始**码流\n");
                        return;
                    }
                    // 没给 `h=` → 把 `--transcode-height` 当**上限**（源更高才缩，小的源保持原样），
                    // 而不是"一定要这个高度" —— 否则 240p 的源会被"默认 720p"判成放大而拒绝
                    spec.max_height = transcode_height;
                }
                // 懒启动 / 复用：同一个文件 × 同一个转码规格 共享同一个源（一源多消费者）
                const uint64_t rejected_before = manager->totalTranscodeRejected();
                MediaSource::Ptr source = manager->acquire(path, spec);
                if (!source) {
                    // 被拒的两类原因分开报：**并发已满是"稍后再试"（503）**，
                    // 编码解不了 / 参数非法是"这个源不行"（500）。
                    // 判定用**计数器差**而不是去 parse 错误文本（文本会变，计数器是契约）
                    const bool saturated = manager->totalTranscodeRejected() > rejected_before;
                    resp.setStatus(saturated ? 503 : 500);
                    resp.setBody(std::string(saturated ? "503 Service Unavailable"
                                                      : "500 Internal Server Error") +
                                 "：源打不开（" + manager->lastError() + "）\n" +
                                 (saturated ? "→ 转码并发已满，稍后重试；或去掉 ?transcode=1 拉原始码流\n"
                                            : ""));
                    return;
                }
                const SourceManager::MuxerStreams streams = manager->muxerStreamsFor(path, spec);
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
                                 "（需要 H264 + AAC；H264 裸流需带 SPS/PPS）\n"
                                 "→ 若源是 HEVC，可用 ?transcode=1 转成 H264（服务端需 --transcode）\n");
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

    // M9-c：hls.js 同理由服务端提供（离线的播放页需要它）。没入库就 404 + 明确原因，
    // 不阻塞启动 —— HLS 拉流本身不依赖它（ffplay/ffmpeg 直接吃 m3u8）。
    if (!server->setRoute("/hls.min.js", [hls_js_dir](const HttpParser &, HttpResponse &resp) {
            std::string error;
            const std::string body =
                hls_js_dir.empty() ? std::string() : loadFile(hls_js_dir + "/hls.min.js", &error);
            if (body.empty()) {
                resp.setStatus(404);
                resp.setContentType("text/plain; charset=utf-8");
                resp.setBody("404 Not Found：hls.js 未入库（third_party/hls.js/hls.min.js）"
                             + (error.empty() ? std::string() : "（" + error + "）") +
                             "\n→ 播放页将退回浏览器原生 HLS（Safari/iOS）；"
                             "HLS 拉流本身不受影响\n");
                return;
            }
            resp.setContentType("application/javascript; charset=utf-8");
            resp.setHeader("Cache-Control", "public, max-age=600");
            resp.setBody(body);
        })) {
        std::fprintf(stderr, "注册 /hls.min.js 路由失败\n");
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
    std::printf("  HLS       : http://127.0.0.1:%u/hls/<name>.m3u8\n",
                static_cast<unsigned>(server->port()));
    std::printf("  统计      : http://127.0.0.1:%u/api/stats\n",
                static_cast<unsigned>(server->port()));
    std::printf("  媒体目录  : %s%s\n", media_root.c_str(), isDir(media_root) ? "" : "（不存在！）");
    std::printf("  日志文件  : %s\n", log_file.c_str());
    std::printf("  推送      : %gx%s\n", speed, loop ? "，循环推流" : "");
    if (transcode_allowed) {
        std::printf("  转码      : 开启（默认 %dp，最多 %u 路；URL 加 ?transcode=1&h=720）\n",
                    transcode_height, transcode_max);
    } else {
        std::printf("  转码      : 关闭（--transcode 打开；HEVC 源当前会 415）\n");
    }
    std::printf("  命令行拉流: curl -N http://127.0.0.1:%u/live/sample.flv | ffplay -\n",
                static_cast<unsigned>(server->port()));
    if (!rtsp_sources.empty()) {
        std::printf("  RTSP 输入 :\n");
        for (const auto &item : rtsp_sources) {
            std::printf("    %-10s %s\n", item.first.c_str(), redactMediaUrl(item.second).c_str());
        }
        std::printf("               拉流：/live/%s.flv 或 /hls/%s.m3u8\n",
                    rtsp_sources.front().first.c_str(), rtsp_sources.front().first.c_str());
    }
    std::printf("  Ctrl-C 退出\n\n");
    std::fflush(stdout);

    while (!g_exit.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    server->shutdown(); // 先关服务器（连接全断）→ 订阅者释放 → 源按空闲规则回收
    if (hls) {
        hls->shutdown();  // M9-b：释放 HLS 的切片器/订阅者/源句柄（不等租约到期）
    }
    InfoP("已退出");
    // 退出前把日志队列刷干净：异步模式下"最后几行"可能还在队列/用户态缓冲里，
    // 不 flush 就会出现"日志文件里看不到退出原因"这种最要命的缺失（M7 实测踩到）
    Logger::Instance().flush();
    return 0;
}
