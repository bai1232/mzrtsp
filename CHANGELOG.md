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

### 说明
- `v0.1.0` 尚未发布。按 `VERSIONING.md`，tag 只能打在**可独立构建且测试通过**的提交上。
- M1（Core 层）已完成并推送；后续进入 M2（网络层：EventPoller / TcpServer / Session）。

## 版本规划

| 版本 | 主题 | 状态 |
|---|---|---|
| `v0.1.0` | MP4 / H264 裸流 → HTTP-FLV（remux，多客户端共享） | 🚧 开发中 |
| `v0.2.0` | MKV / TS 输入、HLS 输出、转码 | ⏳ 计划 |
| `v0.3.0` | RTSP 输入、解码处理（滤镜/缩放/水印） | ⏳ 计划 |
