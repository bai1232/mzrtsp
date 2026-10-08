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

#### M4-a FFmpeg 封装（AvPtr / time_base / Demuxer）
- 新增 `src/ffmpeg/av_ptr.h`：`AvPtr<T, void(*)(T**)>` RAII（format/codec ctx、packet、frame、dict）；
  工厂失败返回 nullptr + ErrorP（不抛不静默）；open 失败时显式 `avformat_free_context`（否则每次失败漏一个 ctx）
- 新增 `src/ffmpeg/time_base.h`（**不依赖 FFmpeg**）：整数换算 + `__int128` 溢出判定（失败返回 false，
  不截断）；`MonotonicGuard` 把时间戳回退钳住并**计数**（播放器把 dts 回退当跳帧）
- 新增 `src/ffmpeg/demuxer.h/.cpp`：`open` → 报流信息（codec/宽高/采样率/时基/帧率）→ `readPacket`；
  `ReadResult{Packet,Eof,Error}`（EOF 与错误必须分开）；四条硬约束：`max_streams`（超限拒绝打开）、
  `max_packet_size`（超限拒绝 + 计数）、open/read 超时（**interrupt_callback** 真正中断）、
  时间戳换算/缺失与回退全部计数
- 新增 `scripts/make_samples.sh`：用本机 ffmpeg **现场生成**样本（mp4 / h264 裸流 / 随机字节），
  不入库（`.gitignore` 加 `/samples/`）
- CMake：M4 起 **FFmpeg 为必需依赖**（缺 libav* 直接 FATAL_ERROR 报错，不悄悄少编模块）
- 测试：`tests/test_ffmpeg.cpp` **11 用例 / 1024 断言**（新分组 `ffmpeg`，进 TSAN 严格组）；
  覆盖 正常/空/满/断开/超大 五维（清单见提交说明）
- 实现中修的两个自测问题：① ctest 的 cwd 是 `build/tests/`，样本候选路径少一级；
  ② 框架断言**不中断执行**，断言失败后解引用空指针 → 段错误（改为显式 `return` 早退）

#### M4-b CodecMatrix + H264 SPS/PPS
- 新增 `src/ffmpeg/codec_matrix.h/.cpp`：`decideOutput(video, audio, output) → Remux/Transcode/Unsupported`。
  H264+AAC→FLV = Remux；**HEVC→FLV = Unsupported**（播放端不支持，SPEC 已冻结）；MPEG2/MP3 → Transcode；
  未知输出格式 → Unsupported + ErrorP（**绝不默认 Remux**：假成功最贵）
- 新增 `src/ffmpeg/h264_util.h/.cpp`：Annex-B 起始码切分（不做 emulation 反转义）、SPS/PPS 提取
  （缺任一 → 明确失败）、SPS 分辨率解析（exp-Golomb 全整数，处理 high profile 的 scaling list 与
  frame_cropping，带合理上界 16384）
- **M4 的第二条验收达成**：裸流样本提取出 SPS(22B)+PPS(4B) 并解析出 320x240，与 `Demuxer` 对同一份
  裸流的结果逐项一致（两套独立路径交叉校验）
- 契约澄清（写进头文件）：SPS 无校验和，随机字节可能凑出"语法合法"的结果 → 解析器只承诺语法解析，
  **调用方必须交叉校验**；实测 0xff 填充会被解析成 16x16，故用例改用"真解析不了"的数据（长串 0/截断/空指针）
- 测试：`tests/test_ffmpeg.cpp` 增至 **19 用例 / 1060 断言**（M4-a 11 + M4-b 8）

#### 文档：复盘总结骨架（`docs/RETROSPECTIVE.md`）
- 新增 `docs/RETROSPECTIVE.md`：① 技术坑库（一坑一格：现象 → 根因 → 修复 → 防回归用例）
  ② 测量工具与误判（复用价值最高的一节）③ 决策与代价 ④ AI 协作复盘 ⑤ 简历素材
  ⑥ 当前快照（未做 / 假设 / 风险）；每条尽量附 `文件:行号` 或可复现命令
- 实测纠正：单进程全量跑为 **171 用例 / 57995 断言**（此前口报的 133 是过期数字）
- 实测发现（见 `docs/RETROSPECTIVE.md` §2.4）：`util_file_relative` / `logger_file_relative_path`
  收尾用 `chdir("/")` 而不是切回进入前的目录 → 同进程后续用例的相对路径全部失效，使 8 个依赖
  样本的 ffmpeg 用例在**单进程全量跑**时假红；`ctest` 因每个分组是独立进程而全绿，把这个问题
  掩盖了 4 个里程碑

#### 测试：用例之间必须隔离（修掉 4 个里程碑的假红）
- `tests/test_main.h`：新增 `::mztest::ScopedCwd`（RAII 还原工作目录）与**框架级不变量**——
  用例结束时工作目录被改动 ⇒ **当场点名该用例**并立刻还原（原先由后面某个无辜用例背锅）
- 修复真凶 2 个：`util_file_relative` / `logger_file_relative_path` 改用 `ScopedCwd`
- `tests/test_ffmpeg.cpp`：样本查找改为**与工作目录无关**（从当前目录向上最多 8 层，
  原先硬编码 2 级相对路径）；找不到样本仍**明确失败**，绝不静默跳过
- 新增用例 2 个（总数 **173**）：`selftest_scoped_cwd_restores`（锁守卫本身）、
  `ffmpeg_samples_found_from_nested_cwd`（**变异验证**：把搜索层数改成 2 即红，
  且报错正是原始症状"找不到样本"）
- 效果：单进程全量跑 **8 红 → 8 个假红全部消失**，凶手从"8 个无辜用例"变成"精确 2 个"
- 遗留（另案，不改代码）：`timer_precision` 的计时门禁在**单进程全量上下文**下会红
  （单独跑 6/6 绿；差 1ms）→ 计时结论以分组门禁为准，规矩记入 `docs/TESTING.md` §2.2

#### M5-a 媒体分发（MediaPacket / FrameQueue / GopCache / MediaSource）
- 新增 `src/media/`：`MediaPacket`（**不可变**编码包，零拷贝分发的载体）、`FrameQueue`（每订阅者
  一个的有界队列，FR-5.1）、`GopCache`（最近 1 个 GOP，供中途接入）、`MediaSource` + `Subscriber`
  （一源多消费者、`weak_ptr` 自动注销，NFR-6）
- **单线程契约**（本批刻意不引入线程）：策略与线程正交，用例才能确定性覆盖；跨线程投递留 M5-b
- 关键策略：**音频与视频关键帧绝不丢**（`MediaPacket::droppable()`）；`push` 返回**四态**
  （`Accepted` / `DroppedToMakeRoom` / `DroppedIncoming` / `RejectedNoSpace`）——"丢掉新包"与
  "不可丢的包进不去"分开，后者置位 `Subscriber::broken()` 由连接层断开（FR-5.2）
- 上限不可绕过：`setLimits(0)` 或超过硬上限一律被拒**且保持原值**（FR-5.1 + AI_COLLAB §4.6）
- 新接入订阅者灌 GOP 缓存，**保证第一条是视频关键帧**；关键帧比队列上限还大 → **拒绝接入**
  （宁可拒接，也不让对端从 GOP 中间开始花屏）
- 测试：`tests/test_media.cpp` **12 用例 / 340 断言**（新分组 `media`，单线程 → 进 TSAN 严格组）；
  覆盖 正常/空/满/断开/超大 五维（清单见 `docs/DESIGN_M5.md` §5）
- 变异验证（改坏即红）：① 让音频变成可丢 → 3 个用例红；② 不灌 GOP 缓存 → 接入用例红
- 文档：新增 `docs/DESIGN_M5.md`；`docs/ROADMAP.md` 的 M5 行与 `docs/TESTING.md` §7 补齐
  **FR-5.1 / FR-5.2 / FR-5.3 / NFR-6** 的编号引用（三方对齐机检通过）
- 测试总数：**185 用例**（此前 173）
- 实测修正（另案，只改文档）：`timer_precision` 在**并行** `ctest -j4` 下会红（4 个测试进程抢 CPU），
  串行/单跑绿 → 计时门禁规矩改为"**串行、无干扰**下取结论"，见 `docs/TESTING.md` §2.2

#### M5-b 媒体层线程打通（FrameQueue SPSC + 唤醒合并 + SourcePump 源线程）
- `FrameQueue` 加锁成 **SPSC**（源线程 push / 消费者线程 pop）；取值接口改为返回**快照**
  （原来返回 `const Stats&`，跨线程读就是数据竞争），锁内不做任何外部调用
- `Subscriber` 支持 `bindPoller()` 绑定消费者线程、`setDrainCallback()`（返回上一个，沿用全项目约定）、
  **唤醒合并** `notifyIfNeeded()`（每订阅者最多一个未决唤醒）+ `clearNotifyPending()`；
  观测：`notifyCount / notifyRejectedCount / notifyCoalescedCount`
- 新增 `SourcePump`：**源线程**反复调用可打断的读回调并推给 `MediaSource`；
  `stop()` join 线程；`stopRequested()` 供真实实现接 FFmpeg 的 `interrupt_callback`；
  **正常读完 / 读失败 / 被停止都会广播 EOS**（消费者绝不永久等待），且 `eof()` 与"被停止"分得开
- 唤醒投递被拒（poller 已退出 / 任务队列满）时**复位未决标记**，否则该订阅者会被永久卡住
- 测试：`tests/test_media_ntimed.cpp` **6 用例 / 56 断言**（新分组 `ntimed_media`，只用无超时等待 →
  进 TSAN **严格组**）+ `test_media.cpp` 增 1 个绑定语义用例；总数 **192 用例**
- 关键验证：2 万帧并发搬运**不丢不重且保序**；100 帧只产生 **1 次**跨线程唤醒而数据一条不少；
  drain 确认在**轮询线程**上执行；poller 退出后唤醒被拒可重试；源线程三种结束方式都广播 EOS
- 实现中修正：`MediaSource::Ptr` 别名缺失（M5-a 没人用到，M5-b 才需要）
- 批计划调整（已同步 `ROADMAP` / `DESIGN_M5` §1）：M5-c = `SourceManager` 懒启动/空闲释放 + 接 `Demuxer`；
  M5-d = 节流 FR-3.5 + 30s 写阻塞串通 + `/api/stats` 计数（FR-6.1）

#### M5-b′ 规格变更落地（FR-5.1 / FR-5.2 修订 + 上限推导 + 统计最小版）
- **FR-5.1 修订**：队列上限**只按字节**（删掉"64 帧"），值 = **码率上限 × 延迟额度**
  （初值 8 Mbps × 2 s = 2,000,000 B）。帧数与字节数是同一件事的两种量纲，而真正的参数是时间：
  4K 下 64 帧不到 1 秒（帧数上限先到、延迟失真），低码率下 64 帧可能几十秒（内存白占）
- **FR-5.2 修订**：改为**音视频成对丢**（丢"队头最旧的一段"，音频与视频一起走），
  **唯一不可丢的是视频关键帧**。原"音频一律不可丢"会导致拥塞时视频跳着走、音频连续放 → 音画错位
- `GopCache` 上限同样改为**推导值** = 码率上限 × 最大 GOP 时长（初值 2MB），不再拍 8MB；
  与 `FrameQueue` 的"可丢集合"**统一**（两处唯一不可丢的都是视频关键帧）
- `MediaSource::Limits` 只收**物理量**：`max_bitrate_bps` / `latency_budget_ms` / `max_gop_ms` /
  `max_subscribers`，推导入口只有 `queueMaxBytes()` / `gopMaxBytes()` 两处
- **每源上限用"人数"不用"带宽"**：带宽是事后统计量、接连接时不可预判，当准入等于没有上限；
  `outputBitrateBudgetBps()` 只进观测
- **统计最小版（FR-6.1）**：计数器全部 `std::atomic`，新增 `dumpStats()` 一行文本，**任意线程可调**
- `Subscriber` 头文件里给"**独立时间戳基准**"留了占位注释（签名 + M6 落点），不写死代码
- 测试：`test_media.cpp` 增至 **14 用例**（新增 `media_source_dump_stats_minimal`；策略断言按修订版更新），
  `ntimed_media` 6 个不变 → **全库 193 用例**
- 已同步文档：`docs/SPEC.md`（FR-5.1/5.2）、`docs/ROADMAP.md`、`docs/TESTING.md`、`docs/DESIGN_M5.md`
  （§1 批计划、§3 契约、§4.2/§4.5、§5 用例、§7 决策、§8 未决 2/3/4/6 关闭）
- 遗留：**包数防呆硬顶未加**（只按字节后，极小包洪泛会放大记账外内存开销；见 `DESIGN_M5.md` §6 风险 11 / §8 未决 7）

#### M5-c 真接 Demuxer + 源管理（懒启动 / 空闲释放）
- 新增 `DemuxerProducer`：把 M4 的 `Demuxer` 包成 `SourcePump` 的读回调（`AVPacket → MediaPacket`）。
  **每包一次拷贝**（全链路唯一一处：Demuxer 的 packet 下一帧就被复用）；扇出到 N 个客户端仍零拷贝。
  非音视频流（字幕/数据）跳过但**计数**；时间戳换算失败**仍然发包**只累加计数
- 新增 `SourceManager`：**懒启动**（第一次 acquire 才开文件、才起源线程）、**复用**（同一 path 同一
  `MediaSource`）、**空闲释放**（最后一个句柄放手后计时，到点停线程 + 关文件）
  - 句柄用「自定义 deleter + 捕获强引用」：`releaseAll()` 之后句柄依然有效，不悬垂
  - IO 在锁外做（open 可能超时，占锁 = 阻塞事件循环）；停线程在锁外做（join 耗时）
  - 计时器只能在轮询线程挂 → `async()` 投递；**挂不上就退化为立即释放**（绝不静默"永不释放"）
  - `idle_release_ms = 0` 是合法档位：句柄一放就释放（不缓存源），也让用例能完全确定性
  - 未提供 poller → 明确退化为"释放即回收"，而不是"看着在缓存、其实永不释放"
- `Demuxer` 增加 `setAbortFlag()`：`interrupt_callback` 除超时外再检查外部中止标志 ——
  这样 `SourcePump::stop()` 才能真正打断正在进行的 `av_read_frame`，而不是干等 join
  （`SourcePump` 相应暴露 `stopFlag()`；**唯一一处改 M4 代码**，已同步 `DESIGN_M4`）
- 测试：`tests/test_source_manager.cpp` **8 用例 / 360 断言**（新分组 `srcmgr`，进 TSAN 严格组）；
  真读 `samples/sample.mp4`（320x240 H264 + AAC）到 EOF，断言视频 dts 单调、关键帧 1 个、音视频包都 > 0
- 关键验证：懒启动（acquire 前源数为 0）；复用（两次 acquire 同一指针、`totalCreated == 1`）；
  空闲窗口内再次 acquire 会**取消计时**（等过原定时长源仍在）；打不开 → nullptr + 计数 + `lastError`；
  `releaseAll()` 后句柄不悬垂、再 acquire 会重建（`totalCreated == 2`）
- 变异验证：①「打不开也照样注册源」→ `open_failure_not_silent` + `lazy_start_and_reuse` **红**（有效）；
  ②「去掉空闲到点的 handles 复查那一行」→ **用例仍全绿**：说明该行是**冗余兜底**
  （`acquire` 会 cancel 计时器、`armIdleTimer` 也会查 `handles`），当前**不可达**但保留，
  以防将来某条路径忘记 cancel —— 这类"存活变异"要如实记录，不能当成"验证通过"
- 三方对齐补齐：`ROADMAP` / `TESTING` 补 **FR-1.2**（按需启动、不支持 seek）、**FR-2.1**（真读 MP4）、
  **FR-6.3**（源创建/释放落日志）的编号引用
- **TSAN 抓到真 bug 并修掉（本批最重要的产出）**：接上真实源线程后，调用方会在源线程推流的同时
  `subscribe()` —— 而 `MediaSource` 的订阅管理当时**完全没有同步**，TSAN 报了 **13 处 data race**
  （全部指向 `subscribe()` 里读 `GopCache` 那一行）。修法：订阅集合 / `GopCache` / `_ended` / `_next_id`
  统一由 `_mutex` 保护；**唤醒一律移到锁外**（`async` 在"调用者就是轮询线程"时会内联执行，
  内联的 drain 回调可能再次进入 `MediaSource` → 持锁调用即死锁）；唤醒清单用**局部** vector
  （成员暂存会被两个线程的调用互相清空）。`Subscriber::broken()` 改 atomic
- 附带：`DemuxerProducer` 的计数改 atomic、`lastError()` 走锁（观测方可能在别的线程）
- 命名坑：用例曾叫 `srcmgr_idle_release_i`**`mmedia`**`te` → 含 "media" 子串，会被纯逻辑的
  `media` 分组过滤到、把该组拖成"依赖样本"；已改名 `srcmgr_idle_release_without_cache`
  （子串匹配的坑，已记入 `DESIGN_M5` §5.3）
- 遗留：**循环播放（FR-1.2 的"可选循环"）留 M6**（要"读完重开 + 时间戳重置"，与源生命周期纠缠）

#### M5-d 节流（FR-3.5）+ 统计接线（FR-6.1）—— M5 收尾
- 新增 `Throttle`：按 `MediaPacket::dtsMs()` 与**单调墙钟**对齐推送（FR-3.5 "不得以磁盘速度全速灌入"）
  - 默认 **开**、`speed = 1.0`；`speed` 可调（8.0 = 2 秒样本约 250ms 读完），`enabled = false` 只给压测
  - **可被打断**：内部按 ≤50ms 的分片睡，中止标志一置位立刻返回 false（否则 `SourcePump::stop()`
    的 join 要等满一拍）；**不补偿**落后（补偿会让源疯狂追赶，把下游再冲爆一次）
  - `setConfig` 拒绝 0 / 负数 / NaN / inf / 超硬上限（1000 倍速），且保持原值
- `SourcePump` 集成节流；`SourceManager::Config` 增加 `throttle`（**必须在 start() 之前配**，
  否则真样本只能按 1 倍速跑，用例没法快）
- 统计接线（FR-6.1）：`MediaSource::dumpStatsJson()` / `SourceManager::dumpStatsJson()` 返回
  **合法 JSON 对象片段**（键名做前缀）；`HttpServer::setExtraStatsProvider()` 单钩子把它们拼进
  `/api/stats`（**http 层不依赖 media**，接线由 M7 的 main 做）
  - 顺带修掉隐患：`/api/stats` 原来用 `snprintf` + **固定 640 字节缓冲**，追加片段后长度不可控 →
    改成 `std::string` 拼接（固定缓冲会**静默截断**，违反 AI_COLLAB §4.5）
- `broken()` 本批**只做巡检**（`brokenSubscriberIds()` + 进 JSON）；**真正断连留 M6**（现在没有连接对象可断，
  媒体层自己踢人会让"谁断的"不可追溯）
- 新增/更新用例：`media` 组 +4（节流：按墙钟等待 / 中止立刻返回 / 非法 speed 被拒 / 关闭即放行）、
  `srcmgr` 组 +2（统计 JSON / **8 倍速节流实测**）、`ntimed_http` +1（钩子进 JSON 且括号配平）→ **全库 208 用例**
- **修掉一个观测口径漏洞**（用例抓到的）：`SourceManager::releaseEntry` 原来"先摘条目 → join → 再计数"，
  节流让 join 变慢（源线程正睡在 50ms 分片里）后，外部会读到"`sourceCount()==0` 但计数仍是 0"的中间态；
  现在**计数与摘条目在同一临界区**完成
- 变异验证：① 让节流形同虚设 → `media`/`srcmgr` 相关用例红（有效）；② 让 `/api/stats` 不拼追加片段 →
  `ntimed_http_api_stats_extra_provider` 红（有效）
- 用例加固：空闲释放的两个用例超时从 3s 放宽到 **10s**，超时时 `MZ_FAIL` 打印 `dumpStats()`
  （实测：ASAN ctest 满载时曾偶发超时、单独跑 3/3 绿 —— 先给宽超时 + 留诊断，**不是**"再跑一次就好了"）

#### M6-a FLV 封装（`FlvMuxer`，纯函数式）+ ffprobe 验收
- 新增 `src/output/flv_muxer.h/.cpp`：FLV Header、AVC/AAC sequence header、视频/音频 tag、
  `CompositionTime = pts - dts`（24 位有符号）、**每客户端一份的时间戳基准**、32 位自然回绕
  - **没有初始化头就不开工**：H264 缺 avcC / AAC 缺 AudioSpecificConfig → `prepare()` 直接失败并给原因
  - **本批只吃 MP4/avcC**：初始化数据看着像 Annex-B（首字节 0x00）→ `AnnexBNotSupported` **明确拒绝**
    （不做转换、不出坏流；转换归 M6-c）—— 视频 NALU 直接搬，因为 MP4 的包本来就是 length-prefixed
  - 早于基准的包**钳到 0 + 计数 + 首次告警**（不让 32 位回绕出"40 亿"这种时间戳）
  - 外来流不静默丢（`InvalidPacket`）；tag 超 24 位长度上限 → `TooLarge`
- `StreamInfo` 增加 `extradata`：`Demuxer` 从 `AVCodecParameters` **拷贝**带出（已同步 `DESIGN_M4`）
- 新增 `examples/flv_mux_demo.cpp`（demux → 封成 FLV 落盘）与 `scripts/flv_mux_test.sh`（**M6-a 验收**）：
  产物 **55,623 字节 / 140 tag（视频 50 + 音频 88，时间戳基准 -23 ms）**，`ffprobe` 认成
  **h264 320x240 + aac**、解码出 **50 帧 / 2 秒**、`ffmpeg -f null -` 完整解码**无 error** → **8/8 通过**
- 测试：`tests/test_flv_muxer.cpp` **10 用例 / 515 断言**（新分组 `flv`，进 TSAN 严格组），
  全部**字节级**断言（tag 类型 / dataSize / 时间戳 / CompositionTime / PreviousTagSize / 负载与 extradata 逐字节比对）
- 变异验证：① FLV header 的 audio/video flags 全 0 → 红；② 视频包 `AVCPacketType` 写成 0 → 红
- 修掉一个**测试自身**的坑：`static_cast<int>(out[i])` 在 x86 上会符号扩展（0xAF → -81）→ 统一走 `byteAt()`
- 文档：新增 `docs/DESIGN_M6.md`（FLV 逐字段布局、per-client 基准、sequence header 重发纪律、验收方式）；
  `ROADMAP` / `TESTING` / `README` / `DESIGN_M4` 同批同步
- 门禁：串行 ctest **20/20**、ASAN **20/20**、TSAN **290 用例 0 报告**、单进程全量 **217/217**、零警告
- 遗留：`flv.js` 入库（M6-c，含许可证声明）、`onMetaData` / AVC end-of-sequence 是否发（M6-c 实测后定）、
  Annex-B→AVCC 转换（M6-c）

#### M6-b HTTP-FLV 接线（`FlvSender` + `/live/` + 发完再关）
- 新增 `src/output/flv_sender.h/.cpp`：**每连接一个**，把订阅者队列封成 FLV 写出去
  - 写出目标抽象成 `Sink`（**不依赖 `http/`**）→ 用**假 sink** 做确定性单测（无网络/线程/时序）
  - `create()` 内部 `FlvMuxer::prepare()` 并**自己注册 drain 回调**（只持弱引用：不成环、无需调用方再拿订阅者句柄）
  - `onDrain()`：队列取空 → 封 tag → 攒到 32KB 写一块；**`broken()` → Aborted**（关键帧进不去，继续发只会花屏）、**EOS → `finished()` + `end()`**（幂等）
- HTTP 侧三处能力（都为"异步流式"服务）：
  - `HttpResponse::chunkWriter()`：带**分块帧头**的写出函数，且**只捕获 Sender 的拷贝**（handler 返回后仍可用）
  - `HttpResponse::setChunkedAsync()`：声明异步流 → 框架不再兜底 `endChunked()`（否则会把流提前掐断）
  - `HttpResponse::holdResource()` + `HttpSession::_held_resources`：把 `FlvSender` 挂到**连接**上，
    连接关闭统一释放 → 退订（NFR-6），drain 回调的弱引用随之失效（不会在连接断后写 socket）
- 新增 `Session::shutdownAfterFlush(max_wait_ms)`（"发完再关"，`DESIGN_M3` §9 未决 5 关闭）：
  - 队列**排空即关**（挂在写路径的"队列空"分支，事件驱动）；到点仍排不空 → **强制关 + `flushCloseTimeoutCount()` + Warn**
  - `max_wait_ms = 0` 被拒（0 不等于无界）；关闭时取消截止定时器
- 新增 `SourceManager::demuxerFor(path)`（输出层要拿 `StreamInfo` 写 sequence header）
- 新增 `examples/flv_http_server.cpp`（`--port` / `--media-root`；名字只收 `[A-Za-z0-9_-]` → 杜绝目录穿越）
  与 `scripts/flv_http_test.sh`（**M6-b 验收**）
- **验收实测（15/15 通过）**：`curl -N` 拿到 **55,623 字节**（与 M6-a 落盘产物**字节数一致**）、
  `ffprobe` 认 h264 **320x240** + aac、解码 **50 帧**、`ffmpeg -f null -` 无 error；
  `Content-Type: video/x-flv` / chunked / CORS 齐备；不存在与穿越均 404；断开后 `subscribers=0`
- 测试：`tests/test_flv_sender.cpp` **8 用例**（归入 `flv` 分组）+ `ntimed_session_shutdown_after_flush`
  （两个场景：**256KB 一个字节不丢后才 EOF**；只连不读 → 到点强制关）→ 全库 **226 用例**
- **过程中被验收/用例抓到的三个真问题**：
  ① 用裸 `sender()` 当 chunked 出口 → `curl: (56) Malformed encoding`（必须用 `chunkWriter()`）；
  ② "发完再关"的钩子挂在写循环**之后** → 那个循环只在队列空/出错时退出 → **死代码**（改挂"队列空"分支）；
  ③ `dumpStatsJson()` 不做惰性清理 → 源 EOS 后统计永远停在旧值、NFR-6 验收失败（改成先 prune）
- 文档：`DESIGN_M6`（§3.4/§3.5 契约、§5.3 用例、§7 决策 8 条）、`DESIGN_M2` §9（发完再关）、
  `DESIGN_M3` §9 未决 5 关闭、`ROADMAP`、`TESTING`（FR-4.1 + NFR-1）、本文件
- 门禁：串行 ctest **20/20**、ASAN **20/20**、TSAN **299 用例 0 报告**（`flv` 18 / `ntimed` 38 均 0 报告）、
  单进程全量 **226/226**、零警告

#### M6-c 应用入口 + 浏览器播放页 + 裸流输入 + 循环播放（**v0.1.0 端到端可播**）

- **`src/main.cpp`（新）→ `build/bin/mzmedia`**：零参数启动（**FR-7.2**，默认 `0.0.0.0:8080` + `./samples`，
  横幅直接打印播放页 / 拉流 / 统计 URL）；`--port` / `--media-root` / `--web-root` / `--loop` / `--speed` /
  `--log-level`（**FR-7.1** / **FR-7.3**）/ `--help`；**未知参数报错退出**（不静默忽略）。
  它只做接线：路由、配置、生命周期 —— 不认识 FLV 字节，也不碰 `Demuxer`
- **浏览器播放页**：`HttpServer` 的内建 `kTestPage` 升级为**播放器页**（`<video>` + 名字输入 + 播放/停止 +
  状态 + 错误区 + `/api/stats` 每 2s 刷新）；`flv.js` **1.6.2 入库**
  （`third_party/flv.js/`：`flv.min.js` + `LICENSE`(Apache-2.0) + `README.md`(来源/版本/MD5)），
  由 `GET /flv.min.js` 提供 → **离线可用**；flv.js 取不到时页面**明确写出原因**（不是"点了没反应"）
- **Annex-B → AVCC + avcC 构造（FR-2.1 补齐）**：`h264_util` 新增 `looksLikeAnnexB()` /
  `annexBToAvcc()` / `buildAvcC()`（纯函数）；转换落在 `DemuxerProducer`（解封装层）——
  转完与 MP4 输入**完全同形**，`FlvMuxer` 一行未改。缺 SPS/PPS 或包内无起始码 → **明确失败**（不猜、不原样拷）
- **循环播放 `--loop`（默认关）**：`Demuxer` 新增 `close()` 供重开；`DemuxerProducer::Config::loop`
  → 读到 EOF 重开同一路径，**偏移累加**（末帧 dts + 帧长**向上取整** + 1ms 间隙）→ 时间轴连续且严格递增；
  每趟校验流参数（被换掉的文件 → 明确失败）、空趟不再重开（防死循环）
- **`Sink::abort` + `HttpResponse::abortFn()`**：`Aborted`/`SinkFailed` 由 HTTP 侧**硬关连接**
  （不发结束块）——`end` = 正常结束、`abort` = 流残缺，两个语义不混
- **`SourceManager`**：`muxerStreamsFor()` 取代 `demuxerFor()`（返回**源自己持有的快照**，
  Annex-B 时已是 avcC）；`/api/stats` 增加每源 `producer{packets,annex_b_video,annex_b_packets,loops,loop_offset_ms,loop_failures}`
  与 `idle_deferred`
- **验收实测（49/49 通过，脚本重写）**：播放页与 `/flv.min.js` 可取（**与入库文件逐字节一致**、版本 1.6.2）；
  MP4 → **55,623 字节**合法 FLV；**H264 裸流也能拉**（`ffprobe` + `ffmpeg` + python 逐 tag 复查 AVCC 结构）；
  **NFR-1 首帧 20ms 级**（TTFB 3.3ms）；响应头 / 404（含非法扩展名与"缺 `.flv` 后缀"）；
  断开后 `subscribers=0`；**源结束后再拉 12ms 正常结束**；`--loop --speed 4` 拉 3 秒 → **11 秒媒体时长 /
  297 帧 / loops=6 / 时间戳跨趟单调 / 解码无 error**
- 测试：`tests/test_ffmpeg.cpp` +7（avcC/Annex-B 纯函数，含真样本往返）、
  `tests/test_demuxer_producer.cpp` +6（新分组 **`producer`**）、`flv` +2、`srcmgr` +2 → 全库 **243 用例**
- **过程中被抓到的两个真问题（都是"接起来才暴露"，单测全绿、脚本一跑就红）**：
  ① **源读完之后再拉同一路，连接一直挂着不动**（晚到订阅者的队列已被 `markEndOfStream()`，
  但那一刻 drain 回调还没注册 → 永远没有唤醒）→ `FlvSender::start()` 发完头**立刻搬一次队列**；
  ② **循环播放每 2 秒被 ffmpeg 报一次 `non monotonically increasing dts`**（整数毫秒表达不了
  AAC 的 23.22ms 帧长，第二趟首包与第一趟末包撞在同一毫秒）→ 帧长向上取整 + 1ms 间隙
- **读代码时发现并修掉的一个真问题**：`SourceManager` 的空闲释放**只看句柄数**，
  而应用层 handler 一返回就放掉句柄 → **有人正在看的源也会在 60s 后被释放、观看者突然"播完"**
  → 改为"有订阅者就推迟 + 按 `deferred_recheck_ms` 复查"（三个入口同一个守卫），
  新增计数 `idle_deferred`，用例 `srcmgr_idle_release_waits_for_subscribers`
- **验证了设计取舍（反证）**：曾为"循环重开时重置节流基准"加了 `ReadResult::PacketNewSegment`，
  实测 `--loop --speed 4` 拉 3 秒加/不加都是约 12.0 秒媒体时长 → **无可观测差别 = 验证不了的复杂度，删除**
- 文档：`DESIGN_M6`（新增 §3.6/§3.7 契约、§4.6/§4.7/§4.8 机制、§5.4 用例、13 条风险、9 条决策）、
  `ARCHITECTURE`（App 层实际形态、`third_party` 特例、源释放判据）、`TESTING`（§3 集成改成跑脚本、
  §3.1 NFR-1 量法、§7 需求对照、§8.6 基线刷新）、`ROADMAP`（M6 行）、`README`（快速开始 +
  依赖表述 + 许可证）、`third_party/flv.js/README.md`（新）
- 门禁：串行 ctest **21/21**、ASAN **21/21**、TSAN **317 用例 0 报告**（新 `producer` 组 7 例 0 报告）、
  单进程全量 **243/243**（61,254 断言）、零警告；`scripts/{http,flv_mux,flv_http}_test.sh` 全绿
- 删除：`examples/flv_http_server.cpp`（升级为 `src/main.cpp`）
- 遗留：**M6-d**（10 路并发 / 5 分钟验收 + 拿真实数据回填 M5 未决）、
  MKV/TS 输入仍无样本实测、high profile 裸流的 avcC 扩展字段、`onMetaData`/AVC end-of-sequence 仍不发

#### M6-d 并发 / 长跑验收 + 回填 M5 未决（**M6 收尾**）

- 新增 `scripts/concurrent_test.sh`（**一个参数化脚本**）：`--clients/--duration/--quick/--kill-half/
  --overload/--speed/--no-loop/--media-root/--port`；客户端 **5×curl（纯拉流）+ 5×ffmpeg（边拉边真解码）**、
  **错开 200ms 接入**；每 5s 采样 RSS/fd/服务端 CPU/`/api/stats`；客户端被掐断的尾包**先切到完整 tag 再校验**
- **判据分两层**（刻意的）：**功能性硬门禁**（每路码流完整可解 / 同时在线 = N 路 / 断开后 100% 回收 /
  错误日志 0 条 / `--kill-half` 不影响其余 / `--overload` 的接受与拒绝数对得上 `max_subscribers`）；
  **性能只记基线**（5 分钟的 RSS 斜率噪声大，拿它当门禁会随机翻红）
- `/api/stats` 增加 `media_source.wakeup{notify, coalesced, notify_rejected, popped}`（聚合**当前在册**订阅者）
  —— 这是回填 `DESIGN_M5` §8 #9（唤醒合并够不够）**唯一**的数据来源；键名特意与 `counters.rejected`（订阅被拒）
  区分开（写脚本时踩到过同名抓错值）
- **修三个真问题**（细节见 `docs/DESIGN_M6.md` §4.9、`RETROSPECTIVE.md` §2.7）：
  ① **5 分钟跑在 t=61s 时 10 路连接全部断开** —— HTTP-FLV 是服务端**单向推流**，客户端不发数据，
     FR-4.4 的"读空闲 60s"把正在观看的连接掐掉（3~5 秒的验收永远看不到，NFR-3 的 1 小时更不可能）
     → 异步流式响应（`setChunkedAsync()`）**关掉该连接的读空闲检测**（写阻塞超时 + TCP keepalive 仍兜底），
     用例 `ntimed_http_stream_survives_recv_idle`（**变异验证**：把豁免拿掉必红）；
     同时把 `Session::setRecvIdleTimeout/setSendBlockedTimeout` 改成**允许运行期修改**（检查周期不随之变密）
  ② **"对端消失"被当成服务器故障，而且四处各错一次**（`Session::emitError` 的级别、`Socket::send/recv`、
     `Buffer::readFromFd`、`onEvent` 的分支顺序）→ 一台 5 路被 kill 的客户端留下 **10 条 `[E]`**，
     NFR-3 的"错误日志 0 条"从此不可测 → `core/util` 新增纯函数 `isPeerGoneErrno()`
     （`EPIPE/ECONNRESET/ECONNABORTED/ENOTCONN/ETIMEDOUT`）+ `Session::closeLogLevel()`
     （**public static**，可测）+ 三处日志分级；**类型与计数一律不变**（既有 onError 断言与计数不受影响）
  ③ **`EPOLLHUP/EPOLLERR` 时先判错就 return** → 缓冲区里对端剩下的数据被丢掉，且原因被硬编码成
     `recv-failed (errno=0)`（排障毫无信息）→ `onEvent` 顺序改为 **先写 → 再读 → 最后判错误事件**；
     真"挂断且无数据可读"才报 `PeerClosed`。顺带 `emitError` 的日志补上 `err.what()`
     （**这条没有确定性单测**：依赖内核事件组合且 `onEvent` 是 private，证据是 poller 注释与代码的矛盾 +
     并发脚本 Error 计数 10→0，补测留 M7）
- **回填 `docs/DESIGN_M5.md` §8**（拿真实数据逐条定，含"数据不足就不改"）：#1 `max_subscribers=16` **保持**
  （实测恰好接受到 16、第 17 路被拒 503）；#7 **不加**包数硬顶（源端仅 ≈68 包/s）；#8 **不升级**
  `FrameQueue` 锁粒度（10 路时服务端单核 CPU 4.11%，锁竞争证据不存在）；#9 唤醒合并**够**
  （1.4 包/唤醒、33% 被合并）；#13 节流基准**不改**（音画偏移峰值 2.0ms，远小于一帧 40ms）；
  #5/#6/#10/#14 标注已完成
- **验收实测（10 路 × 300s，`--loop` + 2 秒样本 = 150 次循环重开）**：10/10 路码流完整可解
  （视频 7426 帧 / 预期 7500）；服务端 **CPU 单核 4.11%**（整机 1.03%；NFR-4 是"单路 <5%"，这是十路合计）；
  RSS 稳态段 232s **仅 +24 kB → 折算 0.36 MB/h**（NFR-3 要求 <5MB/h；折算口径，1 小时真跑留 M7）；
  fd 运行期恒为 30、断开后回到基线 19；丢帧 0；错误日志 **0 条**（Warn 12 条 = 对端消失）；
  `--overload 8`：接受 6 / 拒绝 2（`max_subscribers=16` 真的在生效）
- 测试：`ntimed_session_close_log_level`（23 断言：级别映射 + "只改级别不改类型"）、
  `ntimed_http_stream_survives_recv_idle`（单向推流不被读空闲掐）、`ntimed_media_*` 扩充（`wakeup` 聚合 +
  括号配平）→ 全库 **245 用例**；`concurrent_test.sh` 加入验证清单（`--quick` 10 项全绿）
- 文档：`DESIGN_M6`（§4.9 三个真问题、§5.5 用例与判据、风险 14~16、未决 11~13）、
  `DESIGN_M5`（§1 状态行 + §8 全部回填）、`TESTING`（§5/§6 改成跑脚本 + 实测基线表 + ASAN 长跑）、
  `ROADMAP`、`README`（验证清单）、`RETROSPECTIVE` §2.7、本文件

### 说明
- `v0.1.0` 尚未发布。按 `VERSIONING.md`，tag 只能打在**可独立构建且测试通过**的提交上。
- M1（Core 层）已完成并推送；后续进入 M2（网络层：EventPoller / TcpServer / Session）。

## 版本规划

| 版本 | 主题 | 状态 |
|---|---|---|
| `v0.1.0` | MP4 / H264 裸流 → HTTP-FLV（remux，多客户端共享） | 🚧 开发中 |
| `v0.2.0` | MKV / TS 输入、HLS 输出、转码 | ⏳ 计划 |
| `v0.3.0` | RTSP 输入、解码处理（滤镜/缩放/水印） | ⏳ 计划 |
