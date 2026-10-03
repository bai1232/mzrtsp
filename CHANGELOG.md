# 变更记录

本文件遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 风格，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)，发布要求见 [docs/VERSIONING.md](docs/VERSIONING.md)。

## [Unreleased]

### Added
- 项目立项：需求规格、架构设计、能力矩阵、路线图、测试与验收方案
- `docs/VERSIONING.md`：版本与发布强制要求（每版必推 Git、必打 tag、语义化版本、可回滚）

#### M1 Core 层（已完成）
- 构建系统：CMake（C++17、`-Wall -Wextra` 零抑制、默认 Debug）、ASAN / TSAN 开关、
  伞头 `src/mzmedia.h`、按分组注册的 ctest
- `core/util.h`：字符串、时间（系统/单调时钟分离）、线程进程、文件（递归建目录、
  整文件读写——`saveFile` 为原子写）
- `core/logger.h`：分级过滤、异步队列（满则丢弃并计数）、控制台着色、
  按大小滚动文件、流式 `InfoL` 与 printf 风格 `InfoP`、`isOpen()` 失败可见性
- `core/task_queue.h`：有界阻塞队列，优雅终止（abort 后仍取完已入队任务）
- `core/thread_pool.h`：固定大小线程池，优雅停机、异常隔离、拒绝计数
- `core/semaphore.h`：计数信号量
- `core/ticker.h` / `core/once_token.h` / `core/task_cancelable.h` /
  `core/thread_group.h` / `core/notice_center.h`：耗时统计、作用域收尾、
  可取消任务、线程组、进程内事件总线
- `examples/core_demo.cpp`：Core 层可运行示例
- 测试：61 个用例 / 779 条断言；`scripts/tsan.sh` 并发检查（含已知误报的分组与签名判定）

#### M2 Network 层（进行中）
- `network/pipe_wrap.h`：`pipe2(O_NONBLOCK | O_CLOEXEC)` 唤醒管道；`notify()` 可区分失败
  （写满管道导致的 `EAGAIN` 视为成功，不算丢事件）
- `network/event_poller.h`：epoll 事件循环（ET 默认，`EventLT` 可按 fd 切换）、
  延迟删除（回调里删自己/退出不 UAF）、`async` / `sync` 跨线程投递
  （**有界队列** 65536，满则拒绝 + 计数，不做无上限堆积）
- 定时器：`doDelayTask`（multimap 最小堆、可取消、循环任务、单调时钟）、超长延时
  clamp 到 `INT_MAX` 且**计数可见**、`EventPollerPool`（单 Reactor × N）
- M2-2 定时器专项测试：`tests/test_network_timer.cpp`（10 个用例，全组只用无超时等待，
  因此进 TSAN **严格组**）；新增 ctest 分组 `timer`
- M2-2 实测结论：本宿主唤醒延迟为 **0~14ms 且与延时长短无关**（裸 `nanosleep`
  10ms/100ms/1s 档实测 +6 / +10 / +13），故定时器精度门禁由"绝对 ±10ms"改为
  **"0 早触发 + 相对同进程裸基线增量 ≤5ms"**（见 `docs/DESIGN_M2.md` §7.1 / §8 R11）；
  TSAN 构建下该容差按编译期判定放宽到 25ms（TSAN 只插桩库代码、不插桩内核 `nanosleep`），
  并在测试输出里打印"门禁已放宽"
- 修复：超长延时 clamp 的 Warn 会在每轮事件循环重复打印（改为进入截断状态时打一次，
  计数仍逐次累加）
- 修复：`minDelayInLoop` 在超长延时（deadline 越过 int64 上界）时差值为负，被
  `clampTimeout` 当成"没有定时器"→ `epoll_wait` 永久等待 → 超长定时器**静默永不触发**；
  改为饱和到 `INT64_MAX`（边界用例 `timer_huge_delay_clamped` 修前为红）

#### M2-2b 调用上下文补测（并发 / 重入 / 退出中）
- 用例：`timer_concurrent_submit`（4 线程 × 250 并发投递）、`timer_same_deadline`（同 deadline
  不丢不饿死，顺序不作契约）、`timer_cancel_after_fire`（已触发后再 cancel，高频路径）、
  `timer_reentrant_submit`（回调里再投）、`timer_reentrant_zero_delay`（0 延时自投链，只观测）、
  `timer_submit_after_shutdown`（退出过程中投递）
- 修复：`doDelayTask` 在轮询线程内不检查退出，返回一个"永不触发"的非空 handle（静默降级）
  → 与跨线程路径统一为 `nullptr` + `rejectedTimerCount()`（用例 `timer_submit_after_shutdown` 修前为红）
- 修复：退出时静默丢弃未触发定时器 → `droppedTimerOnExitCount()` + Warn
- 新增观测：`delayBatchMax()` / `delayBatchCount()`。实测：1000 个同刻到期 = 1 批 1000 个；
  20 步 0 延时自投链 = 1 批 20 个（性质已量化，暂不限制，见 `DESIGN_M2` §8 R12 / §10 未决 8）
- 修复（测试框架）：汇总行把"失败"打成**断言**数却与"通过"的**用例**数并列，出现
  "用例 18 个（通过 16 / 失败 3）"这种自相矛盾的输出；现在两处都写明单位
- 修复（真实竞态）：`timer_precision` 把 `Semaphore` 声明在循环里，上一轮的 `~Semaphore()`
  与轮询线程仍在进行的 `post()` 并发 —— TSAN 严格组抓到（`pthread_cond_destroy` vs
  `pthread_cond_broadcast`）；生存期提到 `poller->shutdown()` 之后，规矩记入 `TESTING.md` §8.7
#### M2-3a Buffer / Socket（含 `ErrType`）
- `network/buffer.h/.cpp`：线性可扩容缓冲（compact 回收前导空间、读游标、`find` 返回偏移）。
  **返回值策略：没有 void 公开接口** —— `consume` 返回实际消费量（越界 → 0 + ErrorP，不截断）、
  `clear/release` 返回被处理的字节数、`append/reserve` 返回实际量
- `readFromFd(fd, max_bytes, hit_limit, err)`：上限**必填**（原签名让 Session 无法在循环中途
  设限，与"上限在 Session"自相矛盾）；`hit_limit` 显式告知"没读完"（ET 下漏了会永久假死）；
  非法 `max_bytes==0` 拒绝执行
- `network/socket.h/.cpp`：fd RAII + syscall 封装；`accept4` 带 NONBLOCK|CLOEXEC；
  `send` 带 MSG_NOSIGNAL；`EAGAIN/EWOULDBLOCK/EINTR` 不记日志（不是错误）；
  `SockException` 新增 `ErrType`（FR-4.4 四类断开要靠原因分桶计数）
- 测试：`tests/test_network_buffer.cpp` 12 个用例（socketpair + 非阻塞，无等待 → TSAN 严格组），
  新增 ctest 分组 `buffer`；关键契约 `hit_limit` 已做变异验证（改坏即红）
- 测试总数：**110 个用例 / 2597 条断言**（M1 61 + M2 49：poller 13 + timer 16 + buffer 12 + ntimed 8）

#### M2-3b Session / TcpServer / echo 示例（已完成）
- 新增 `network/session.h/.cpp`、`network/tcp_server.h/.cpp`、`examples/echo_server.cpp`、
  `scripts/echo_test.sh`；ctest 新增分组 `ntimed`（带时间维度，与严格组分开跑 TSAN）
- 8 个用例全绿：回显往返、读空闲超时（FR-4.4）、写阻塞超时（FR-4.4/FR-5.2）、发送队列
  超限、接收超限、连接数上限、断开后 fd 回落（NFR-6）、`onRecv` 未实现必须报错关闭
- 修掉 4 个真缺陷（都被新用例/验收脚本抓到）：
  ① `close(fd)` 必须等 `delEvent` 的 `complete_cb`（否则 fd 号立刻被内核复用 → 新连接的
     `addEvent` 被判"重复注册" → 连接全废）
  ② 读写共用一个事件回调，**EPOLLOUT 必须按位分发**（原先 `onWriteEvent` 从未被执行，
     积压数据只能靠后续读操作顺带写出）
  ③ ET 写路径必须**循环写到 EAGAIN**（只写一次会让尾部数据永远发不出去）
  ④ 收发**流控**：发送队列到高水位就停止收数据（否则一个读事件收 64MB 会顶爆发送上限）
- 测试可用 `MZ_TEST_LOG=1` 打开库日志（Logger 默认无 writer，等于静默）
- ✅ 8MB 端到端回显 `cmp` 逐字节一致；8 路并发一致；空闲超时与 fd 回落通过
- ❌→✅ **曾经的 100MB 内容损坏已定位并修复**（`Session::send` 在发送队列非空时仍直写
  socket → 块级乱序）：修法见下一条，判据换成确定性用例 `ntimed_send_order_with_backlog`
- 修复：`Session::send` **只在发送队列为空时**才直接写 socket；队列非空时整段入队（FIFO 保序）。
  原先"能塞就塞"会让新块插到队列里旧块前面 —— 字节数不变、内容错位（socket 是 FIFO，
  顺序就是正确性）。确定性用例证据：修前首个 `'B'` 出现在偏移 **16384**（= 发送缓冲大小）、
  修后正好在 **1048576**（1MB 边界）且其后不再出现 `'A'`
- 验收：`SIZE_MB=100 ./scripts/echo_test.sh` 连跑，**有效 2 次全部四项通过**（`cmp` 逐字节
  一致 + 8 路并发 + 空闲超时 + fd 回落）；第 2 次因脚本自身启动竞态（上一轮服务器未释放
  19000 端口）而中止，属脚本缺陷，待修
- 教训（已写进 `DESIGN_M2` §8 R13）：今天两次错误结论都来自**测量工具本身** —— 异步日志
  未落盘导致"插队=0"的假象、进程内探针把 reader 线程建在发送之后导致 7.5MB 就 send-overflow。
  因此顺序类判据改用**确定性用例**，日志只作辅助


#### M3-a HTTP 请求解析器
- 新增 `src/http/http_parser.h/.cpp`：**I/O 无关**的 HTTP/1.x 请求头解析状态机（自研、零依赖）
- 上限是硬约束（§4.3）：请求行 8KB / URI 4KB / 头部总量 32KB / 头部 100 条；累计缓冲另有
  40KB 硬上限，**判错后不再收数据**（内存有界）；超限原因细分到枚举 + `totalRejected` 计数
- 半包/粘包：`NeedMoreData` 不是错误；`reset()` 把同包里多出的字节（下一个请求 / body）
  交还调用方 → 天然支持管线化；结果需在 `reset()` 之前读完（契约写在头文件）
- 严格性取舍（都写进文档避免被当 bug）：只认 CRLF、**裸 LF 立刻 `BadHeaderLine`**（否则
  要攒到 40KB 才报"超长"，把排查带偏）、头名必须是 token、NUL 字节直接拒、
  未知方法**不算错**（解析成功，由服务器回 405）
- 测试：`tests/test_http_parser.cpp` **19 个用例 / 150 断言**（正常/空/超长/畸形/NUL/管线化），
  进 TSAN 严格组；新增 ctest 分组 `http`
- 文档：新增 `docs/DESIGN_M3.md`（范围/分层/解析器契约与上限/机制/分批/风险/决策/未决）
- 测试总数：**130 个用例**（M1 61 + M2 49 + M3-a 19）

#### M3-b HTTP 服务端（HttpSession / 路由 / 错误码 / CORS）
- 新增 `src/http/http_response.h/.cpp`：响应构造 + 序列化（纯函数，可单测）；自动补
  `Content-Length`（头与实际不一致是最难查的一类问题）；只在要关闭时发 `Connection: close`
- 新增 `src/http/http_server.h/.cpp`：`HttpServer` = `TcpServer` + 路由 + 错误语义 + CORS + 观测；
  内部 `HttpSession`（重写 `onRecv`：`HttpParser` 分帧 → 路由 → 序列化 → `Session::send`，
  循环 `reset()` 支持粘包/管线化）
- 错误语义：400（畸形）/414（行或 URI 超长）/431（头部超限）/404（无路由）/405（非 GET，
  带 `Allow: GET`）/500（handler 抛异常，**异常不穿透事件循环**）/501（媒体路由，M6 实现）
- CORS：所有响应带 `Access-Control-Allow-Origin: *`（FR-4.3）；基础头由 `makeResponse()`
  统一给，处理器可覆盖
- 新增 `examples/http_server.cpp`（可运行示例 + curl 验收载体）、`scripts/http_test.sh`（8 项）
- 测试：`tests/test_http_server.cpp` **14 个用例 / 118 断言**，进 TSAN 严格组（新增分组
  `ntimed_http`）；覆盖 正常/空/满/断开/超大 五维（清单见提交说明）
- 验收：`./scripts/http_test.sh` **8/8 全过**（测试页 200 且 Content-Length 一致、CORS、
  自定义路由、404、405+Allow、501、400、keep-alive 复用两次请求）
- 实现中修正的两点：①处理器自建的响应漏了 CORS/Server 头 → 改为从 `makeResponse(200)` 起步；
  ②测试助手一次 `recv` 会把管线化的第二个响应一起读走 → 加 `carry`（客户端的"分帧"）

#### M3-c chunked 流式 / Range / stats
- `HttpResponse` 增加 chunked：`setSender`/`beginChunked`/`sendChunk`/`endChunked`；分块模式
  **绝不发 `Content-Length`**，结束块显式发（处理器忘了由 `HttpSession` 兜底 + Warn）
- `/stream` 演示路由（M6 的 FLV 就是这个形状）；`send()` 返回 0 视为成功（已入队）
- 单区间 `Range`：`bytes=a-b` / `bytes=a-` → 206 + `Content-Range`；起点越界 → 416；
  多区间 / 后缀 `-N` / 畸形 → 忽略回 200 + Warn
- `/api/stats`：JSON（M3 的 requests/4xx/5xx/malformed + M2 的会话/accept/拒绝/空闲超时/
  收超限/发超限/accept 错误）
- 测试：`tests/test_http_server.cpp` 增至 **21 用例 / 193 断言**（chunked 2 + Range 4 + stats 1），
  测试助手补 **chunked 解码**；脚本增至 **11 组**

#### 工具链：TSAN 误报根治（g++-12）
- 装上 `g++-12`（12.3.0）后实测：**全部组 0 报告**，结论"TSAN 全绿，无任何报告"
  （含此前稳定误报的 `qtimed`/`ptimed` 与间歇误报的 `semaphore`）
- `scripts/tsan.sh`：自动优先 `g++-12`、换编译器时自动清理 `build-tsan`、未装时打印提示；
  FP 分组逻辑保留以兼容 GCC 11 环境

### 说明
- `v0.1.0` 尚未发布。按 `VERSIONING.md`，tag 只能打在**可独立构建且测试通过**的提交上。
- M1（Core 层）已完成并推送；后续进入 M2（网络层：EventPoller / TcpServer / Session）。

## 版本规划

| 版本 | 主题 | 状态 |
|---|---|---|
| `v0.1.0` | MP4 / H264 裸流 → HTTP-FLV（remux，多客户端共享） | 🚧 开发中 |
| `v0.2.0` | MKV / TS 输入、HLS 输出、转码 | ⏳ 计划 |
| `v0.3.0` | RTSP 输入、解码处理（滤镜/缩放/水印） | ⏳ 计划 |
