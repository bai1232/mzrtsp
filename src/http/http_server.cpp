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

/// 「发完再关」的最长等待（M6-b）：够本地把最后一块写完，又不会让连接挂太久
constexpr uint32_t kFlushCloseWaitMs = 5000;

/**
 * 内置测试页（SC-1 的落地）：**不依赖外部 CDN** —— 播放器用的是**入库**的 flv.js
 * （`third_party/flv.js/flv.min.js`，Apache-2.0），由 app 通过 `/flv.min.js` 提供
 * （见 `src/main.cpp` 的 `--web-root`）。所以离线也能播，而不是"页面能开、播放器加载不出来"。
 *
 * 页面里刻意做了一件事：**flv.js 没加载出来时明确写在页面上**。
 * 否则用户看到的是一个"点了没反应"的播放器，无从判断是页面坏了还是流坏了。
 */
// M6-c：标题保持 `<title>mzmedia</title>` —— `scripts/http_test.sh` 用它判断"是我们的内置页"，
// 这是既有的对外契约，不能因为改了页面就悄悄失效
const char *kTestPage = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>mzmedia</title>
<style>
 body { font-family: sans-serif; margin: 24px; max-width: 900px; }
 input { padding: 4px; }
 button { padding: 4px 12px; margin-left: 4px; }
 #state { margin-left: 12px; color: #555; }
 #err { color: #b00; white-space: pre-wrap; }
 video { background: #000; width: 640px; max-width: 100%; }
 pre { background: #f6f6f6; padding: 8px; overflow-x: auto; }
</style>
</head>
<body>
<h1>mzmedia</h1>
<p>HTTP-FLV 播放页。媒体文件放在服务端 <code>--media-root</code> 目录下：填 <code>sample</code>
   就会去拉 <code>/live/sample.flv</code>（对应 <code>sample.mp4</code>；
   填 <code>sample.h264</code> 就是 H264 裸流，服务端会现场构造 avcC 并转 AVCC）。</p>
<p>
  <label>媒体名：<input id="name" value="sample" size="20"></label>
  <button id="play">播放</button>
  <button id="stop">停止</button>
  <span id="state">未开始</span>
</p>
<video id="v" controls autoplay muted playsinline></video>
<p id="err"></p>
<details open><summary>/api/stats（每 2 秒刷新）</summary><pre id="stats">…</pre></details>
<p>其它接口：<code>GET /api/stats</code>（运行统计）· <code>GET /stream</code>（chunked 演示）·
   <code>GET /hls/N.m3u8</code>（HLS，v0.2 接入）。服务器加 <code>--loop</code> 启动则循环推流。</p>
<script src="/flv.min.js"></script>
<script>
(function () {
  var stateEl = document.getElementById('state');
  var errEl = document.getElementById('err');
  var video = document.getElementById('v');
  var player = null;

  function setState(s) { stateEl.textContent = s; }
  function setErr(s) { errEl.textContent = s || ''; }

  if (!window.flvjs) {
    setState('flv.js 未加载');
    setErr('没有取到 /flv.min.js：浏览器播 FLV 依赖它。\n' +
           '请用 bin/mzmedia 启动（它的 --web-root 默认指向 third_party/flv.js），' +
           '或确认该文件存在且路径正确。');
    return;
  }
  setState('flv.js ' + flvjs.version + ' 已就绪');

  function stop() {
    if (player) {
      try { player.unload(); player.detachMediaElement(); player.destroy(); } catch (e) { /* 已停 */ }
      player = null;
    }
    setState('已停止');
  }

  function play() {
    stop();
    setErr('');
    var name = document.getElementById('name').value.trim();
    if (!name) { setErr('请填媒体名'); return; }
    if (!flvjs.isSupported()) {
      setErr('这个浏览器不支持 MSE（flv.js 依赖它）。Chrome/Firefox/Edge 桌面版可以。');
      return;
    }
    var url = '/live/' + name + '.flv';
    player = flvjs.createPlayer({ type: 'flv', isLive: false, url: url },
                                { enableStashBuffer: false, stashInitialSize: 128 });
    player.attachMediaElement(video);
    player.on(flvjs.Events.ERROR, function (type, detail) {
      setState('出错');
      setErr('播放出错：' + type + ' / ' + detail +
             '\n常见原因：文件名不对（应能在服务端 --media-root 下找到）、' +
             '不是 H264+AAC、或服务器没回 200。');
    });
    player.load();
    var p = player.play();
    if (p && p.catch) {
      p.catch(function (e) { setErr('自动播放被浏览器策略拦住，请点视频上的播放键：' + e); });
    }
    setState('正在播放 ' + url);
  }

  document.getElementById('play').onclick = play;
  document.getElementById('stop').onclick = stop;
  document.getElementById('name').addEventListener('keydown', function (e) {
    if (e.key === 'Enter') { play(); }
  });

  function refreshStats() {
    fetch('/api/stats').then(function (r) { return r.text(); }).then(function (t) {
      document.getElementById('stats').textContent = t;
    }).catch(function (e) {
      document.getElementById('stats').textContent = '取统计失败：' + e;
    });
  }
  refreshStats();
  setInterval(refreshStats, 2000);
})();
</script>
</body>
</html>
)HTML";

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
            // M6-b：异步流式响应的「结束并关闭」出口。
            // 它**不依赖 HttpResponse 对象**（handler 一返回 resp 就没了），所以能挂到
            // FlvSender 的 sink 上，在流结束时才调用：发结束块 + 等发送队列排空再关连接
            resp.setEndStream([this]() -> bool {
                if (send("0\r\n\r\n", 5) < 0) {
                    return false; // 连接已坏
                }
                return shutdownAfterFlush(kFlushCloseWaitMs);
            });
            // M6-c：**异常终止**出口。流是残缺的（订阅者 broken / 写失败）时，不能发结束块
            // —— 那等于替客户端掩盖"数据丢了"。直接关连接，客户端会明确报传输被截断
            resp.setAbortFn([this]() -> bool {
                // 走到这里有且只有两种原因：客户端断开导致写出失败（`SinkFailed`）、
                // 或订阅者 broken（`Aborted`，按 FR-5.2 的丢帧策略处理）。
                // 两者都**不是服务器故障**：前者是客户端走了，后者的真信号在 FlvSender 的告警
                // 与 /api/stats 的 dropped/broken 计数里。所以记 Warn ——
                // 记 Error 会让 NFR-3 的"错误日志 0 条"永远不成立（M6-d 并发脚本实测）。
                WarnP("HttpServer: 流式响应异常终止 → 直接关闭连接（不发结束块）");
                shutdown(SockException(SockException::ErrType::Shutdown, 0,
                                       "stream aborted (broken/failed)"));
                return true;
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

        // M6-b：附件交给**会话**托管 —— 流式响应的发送器/订阅者靠它活到连接结束
        // （由会话在析构时统一释放：连接一断就退订，NFR-6）
        for (auto &resource : resp.takeResources()) {
            _held_resources.push_back(std::move(resource));
        }

        if (resp.chunked()) {
            // 【M6-d 修的 bug】异步流式响应 = **服务端单向推流**，客户端不会再发数据。
            // 读空闲检测（FR-4.4 默认 60s）会把正在观看的连接掐掉 —— 实测 5 分钟并发验收
            // 在 **t=61s** 时 10 路连接全部被关（M6-c 的 3~5 秒验收看不到它，NFR-3 的 1 小时更不可能）。
            // 关掉读侧检测 ≠ 放弃检测：**写阻塞超时（30s）+ TCP keepalive** 仍负责发现死连接
            // （死客户端不再读 → 发送队列积压 → send_blocked 触发）。
            if (resp.chunkedAsync()) {
                (void) setRecvIdleTimeout(0);
            }
            // 处理器自己把头和块发出去了：这里只兜底收尾
            // （忘了发结束块会让客户端一直等 —— 那是"静默的挂住"，必须吵出来）
            // 注意：**异步流式**（M6-b 的 FLV）不兜底 —— 它的结束块由处理器在流结束时自己发，
            // 这里替它结束会把流提前掐断
            if (!resp.chunkedEnded() && !resp.chunkedAsync()) {
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
    /// M6-b：连接级附件（流式响应的发送器/订阅者）—— 会话析构时统一释放
    std::vector<std::shared_ptr<void>> _held_resources;
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
