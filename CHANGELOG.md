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
- 测试总数：**83 个用例 / 1903 条断言**（M1 的 61/779 + M2 的 22）

### 说明
- `v0.1.0` 尚未发布。按 `VERSIONING.md`，tag 只能打在**可独立构建且测试通过**的提交上。
- M1（Core 层）已完成并推送；后续进入 M2（网络层：EventPoller / TcpServer / Session）。

## 版本规划

| 版本 | 主题 | 状态 |
|---|---|---|
| `v0.1.0` | MP4 / H264 裸流 → HTTP-FLV（remux，多客户端共享） | 🚧 开发中 |
| `v0.2.0` | MKV / TS 输入、HLS 输出、转码 | ⏳ 计划 |
| `v0.3.0` | RTSP 输入、解码处理（滤镜/缩放/水印） | ⏳ 计划 |
