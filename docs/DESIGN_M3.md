# M3 设计：HTTP 层（v0.1.0 的对外门面）

> 本文件是 M3 的设计与决策记录。形状约定：**权威契约以 `src/http/*.h` 为准**，
> 本节描述"为什么"。行为/形状变更必须同批同步本文件（AI_COLLAB §3.8）。

## 1. 范围与验收

| 范围内（v0.1） | 明确不做 |
|---|---|
| `HttpParser`（HTTP/1.x **请求头**解析） | HTTP/2、HTTP/3 |
| `HttpSession : Session`（分帧 + keep-alive） | WebSocket（v0.1 不做，SPEC 已冻结） |
| 路由：`GET /`、`/live/<name>.flv`、`/hls/*`、`/api/stats` | POST/文件上传（收到就 405，不假装支持） |
| CORS（FR-4.3）、404/405/500 规范响应（FR-4.5） | 多区间 Range（multipart/byteranges） |
| `chunked` **流式**响应（FR-4.2，M6 的 FLV 靠它） | TLS / HTTP 认证 |
| 单区间 `Range`（206，HLS 切片与静态文件） | 反向代理、压缩（gzip） |

**验收**（`ROADMAP.md` M3 行）：`curl` 取到内置测试页；`chunked` 响应可被 `curl -N` 流式接收。

## 2. 分层位置

```
core → network（EventPoller/Session/TcpServer/Buffer） → http → ffmpeg → media → output
```

- `http` 可以依赖 `network` 与 `core`；**反向依赖禁止**（`network` 不认识 HTTP）。
- `HttpParser` 刻意**只依赖 core**：它只吃 `(data,len)`，不认识 socket/Buffer/poller。
  好处：单测极快、无并发、可以进 TSAN 严格组；也让 M4/M5 复用（RTSP 头解析同构）。

## 3. `HttpParser` 契约（M3-a，已实现）

**为什么自研**：SPEC §7 要求网络底座自研、零第三方依赖；且"缓冲/解析"正是 NFR-7 点名要测的东西。

```cpp
enum class Status { NeedMoreData, Complete, Error };     // 半包**不是**错误
enum class Error  { None, LineTooLong, UriTooLong, HeadersTooLarge, TooManyHeaders,
                    BadRequestLine, BadHeaderLine, BadVersion, NulByte };
enum class Method { Get, Post, Put, Delete, Head, Options, Unsupported };  // 不认识的**不报错**
enum class Version { Http10, Http11 };
```

| 上限（初值，可配） | 值 | 超限后果 |
|---|---|---|
| `max_request_line` | 8 KB | `LineTooLong` → 414 |
| `max_uri` | 4 KB | `UriTooLong` → 414 |
| `max_header_bytes` | 32 KB | `HeadersTooLarge` → 431 |
| `max_headers` | 100 | `TooManyHeaders` → 431 |
| 累计缓冲硬上限 | `max_header_bytes + max_request_line`（40 KB） | 立刻拒绝；**判错后不再收数据** |

**调用约定**（半包/粘包是常态）：

```cpp
Status st = parser.parse(data, len);
if (st == Complete) { 用结果（path/header…）; }
std::string rest = parser.reset();          // ★ 结果在 reset 前读完；reset 交还剩余字节
while (!rest.empty()) { st = parser.parse(rest); if (st != Complete) break; 处理; rest = parser.reset(); }
```

**严格性取舍**（写下来避免被当 bug）：
- 头结束符只认 `\r\n\r\n`；**裸 LF 立刻 `BadHeaderLine`**（否则要等攒到 40KB 才报"超长"，把排查带偏）
- 头部名字必须是 token（`Host : x` 这种畸形被拒）；值两端 OWS 裁剪、中间原样保留
- 头部里出现 NUL → `NulByte`（二进制垃圾/攻击载荷）
- 同名列保留多条；`header()` 大小写不敏感、返回**第一条**

## 4. 关键机制（M3-b / M3-c）

| 机制 | 做法 | 为什么 |
|---|---|---|
| 分帧 | `HttpSession::onRecv` 把 `Buffer` 交给 `HttpParser`；循环 `reset()` 直到 `NeedMoreData` | 复用 Session 已验证的读路径（ET 读到 EAGAIN、上限、流控），HTTP 只做解析 |
| keep-alive | HTTP/1.1 默认长连接；`Connection: close` 关；空闲用 Session 的 `recv_idle`（60s） | 不另造超时机制；`onIdle` 已由协议层可重写 |
| 错误响应 | `LineTooLong/UriTooLong`→414、`HeadersTooLarge/TooManyHeaders`→431、其余畸形→400、未知方法→405 | 让客户端能自助定位，且日志里有 `errorName()` |
| chunked 流式（M3-c） | `Transfer-Encoding: chunked` + 持续 `send()`；**注意 `send()` 返回 0 不是失败**（已受理未写出） | 与 Session 发送队列/背压对接；FR-5.1/5.2 的帧级策略留 M5 |
| Range | 只支持单区间 `bytes=a-b`；多区间 → **返回 200 并记一条 Warn** | 半吊子 206 比明确不支持更危险 |
| `/api/stats` | JSON：暴露 M2 的计数（会话/accept/拒绝/空闲超时/收超限/发超限/丢弃）+ M3 的（请求数/4xx/5xx/超限拒绝数） | 可观测性是 FR-4.x/NFR-x 的验收基础 |

## 5. 分批与验收

| 批次 | 内容 | 验收 |
|---|---|---|
| **M3-a** | `HttpParser` + `tests/test_http_parser.cpp`（19 用例） | 正常/半包/粘包/超长/畸形/NUL 全部确定性通过；进 TSAN 严格组 |
| **M3-b** | `HttpSession`、路由、`/`（测试页）、404/405/500、CORS | `curl -i http://127.0.0.1:P/` 拿到测试页；错误码规范；连接级用例 |
| **M3-c** | chunked 流式、Range（206）、`/api/stats` | `curl -N` 流式不中断；`curl -r 0-9` 拿到 206 与 `Content-Range`；stats 各类计数可见 |

## 6. 测试计划

| 分组 | 位置 | 说明 |
|---|---|---|
| `http` | `tests/test_http_parser.cpp` | 纯解析，单线程无等待 → **TSAN 严格组** |
| `ntimed_http_*` | `tests/test_http_server.cpp`（M3-b） | 连接级（keep-alive、超时、chunked 流式、Range、错误码）→ 已知误报组（带时间维度） |
| 脚本 | `scripts/http_test.sh`（M3-b/c） | `curl` 端到端：测试页、`-N` 流式、`-r` Range、`404/405`、`/api/stats` |

## 7. 风险清单

| # | 风险 | 触发条件 | 应对 |
|---|---|---|---|
| 1 | 头攻击面（超长/畸形/巨量头部） | 恶意客户端 | 四级上限 + 硬上限 + 判错即停 + 计数（已实现） |
| 2 | 裸 LF 被当成"攒着等"，拖到 40KB 才报超长 | 非标准客户端 | 立刻 `BadHeaderLine`（已实现） |
| 3 | chunked 与发送队列背压交互 | 慢客户端 + 流式响应 | 复用 Session 的字节级/时间级防线；`send()` 返回 0 按"已受理"处理 |
| 4 | keep-alive 与空闲超时冲突 | 客户端长间隔复用连接 | 60s `recv_idle` 可配；`totalIdleTimeout()` 可观测 |
| 5 | Range 边界（`bytes=-0`、超出文件长度、`a>b`） | 恶意/异常请求 | 单区间 + 边界用例；不合法 → 200 或 416 并在用例里锁死 |
| 6 | 测试页依赖外部 CDN 的 flv.js | 离线环境 | 测试页内联最小播放器 or 本地静态资源（M3-b 定） |

## 8. 决策记录

| 决策 | 选择 | 排除的选项与原因 |
|---|---|---|
| 解析器 | **自研状态机** | 排除第三方（SPEC 要求自研底座、零依赖；且 NFR-7 点名要测解析） |
| 解析器依赖 | 只依赖 core，吃 `(data,len)` | 排除"直接吃 `Buffer`"（会给 http 引入 network 依赖，也难单测纯函数） |
| 上限 | 请求行 8KB / URI 4KB / 头 32KB / 100 条 | 排除"不设上限"（把内存交给对端）；排除"照抄某个服务器的大值"（无依据） |
| 行结束符 | 只认 CRLF，裸 LF 立刻拒 | 排除"容忍 LF"（要额外状态，且会把畸形流量放进解析器） |
| 未知方法 | 解析成功（`Unsupported`），服务器回 405 | 排除"解析层直接拒"（合法但未实现的方法应该 405/501，不是 400） |
| `reset()` 语义 | 交还剩余字节 + **清掉上一次结果** | 排除"结果跨 reset 保留"（下一次 parse 会覆盖，留着只会让人误用） |

## 9. 未决事项

| # | 事项 | 何时定 |
|---|---|---|
| 1 | 四个上限的初值是否合适（8/4/32KB/100） | M3-b 跑起来后按 `totalRejected` 与真实请求观察 |
| 2 | 是否支持 POST / body（v0.1 只需要 GET） | M3-b 定：若支持，body 上限与 413 计数一起加 |
| 3 | 测试页是否内联播放器（离线可用） | M3-b |
| 4 | `/api/stats` 是否需要鉴权/只绑 127.0.0.1 | M3-c（默认不做鉴权，SPEC 未要求） |
