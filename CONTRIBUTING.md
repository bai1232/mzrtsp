# 贡献指南

## 开发环境

| 项 | 要求 |
|---|---|
| 系统 | Linux x86_64（网络层基于 epoll） |
| 编译器 | GCC ≥ 9（C++17）或 Clang ≥ 10 |
| 构建 | CMake ≥ 3.15（`ninja` 可选） |
| 依赖 | FFmpeg 4.4.x 开发库（`libavformat-dev` / `libavcodec-dev` / `libavutil-dev` / `libswresample-dev`） |

```bash
sudo apt-get install -y libavformat-dev libavcodec-dev libavutil-dev libswresample-dev
# v0.2 起转码/滤镜还需
sudo apt-get install -y libswscale-dev libavfilter-dev
```

## 构建与测试

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release   # 或 Debug
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

## 代码风格

| 项 | 约定 |
|---|---|
| 命名空间 | `mzmedia` |
| 类名 | 大驼峰，如 `EventPoller`、`MediaSource` |
| 函数 / 变量 | 小驼峰，如 `addEvent`、`maxSize` |
| 成员变量 | 前缀下划线，如 `_eventFd`、`_subscribers` |
| 常量 | `k` 前缀或全大写，如 `kMaxQueueSize` |
| 头文件保护 | `#pragma once` |
| 缩进 | 4 空格，禁止 Tab |
| 行宽 | 建议 ≤ 120 字符 |
| 注释 | **中文**，解释「为什么」而不是「做什么」 |
| 异常 | 不用异常做流程控制；错误用返回值 + 日志 |

## 编码约定（重要）

1. **FFmpeg 对象一律用 RAII 封装**（`AvPtr` 系列）。**禁止**裸 `new`/`malloc` FFmpeg 上下文，任何泄漏都会违反 NFR-3。
2. **禁止阻塞事件线程**：`demux`/`transcode`/文件 IO 只能在源线程或线程池中执行。
3. **跨线程不直接操作连接对象**：通过 `EventPoller::async()` 投递。
4. **时间戳换算用整数有理运算**，不要用浮点累加（会产生漂移并破坏单调性）。
5. **新增输入/输出格式不得修改核心**（`media/`、`network/`）。若确实需要改核心，先提 issue 讨论架构。
6. 头文件尽量自包含；跨模块依赖只能"上层依赖下层"（见 `docs/ARCHITECTURE.md`）。

## 提交前检查清单

- [ ] `cmake --build build -j` 通过，**`-Wall -Wextra` 零警告**
- [ ] `ctest` 全部通过
- [ ] 新增/修改的公开接口已在 `docs/` 中同步
- [ ] 涉及时间戳/封装/FFmpeg 的改动，已跑过对应的 `ffprobe` 集成校验
- [ ] 提交信息符合 Conventional Commits（见 `docs/GIT_WORKFLOW.md`）
- [ ] 未提交构建产物、日志、媒体文件

## 文档要求

- **规格变更必须先改 `docs/SPEC.md`**，再改代码；不允许实现与规格不一致
- 新增能力要同步更新：`docs/ROADMAP.md`、`docs/CODEC_MATRIX.md`（如涉及编码组合）、`CHANGELOG.md`
- 发版流程严格遵循 `docs/VERSIONING.md` 的强制要求

## 报告问题

请附上：输入文件信息（`ffprobe` 输出）、完整启动参数、`logs/` 中的相关日志、`/api/stats` 快照、复现步骤。
