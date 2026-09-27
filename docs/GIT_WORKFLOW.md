# Git 工作流

> 版本发布相关的**强制要求**见 [VERSIONING.md](VERSIONING.md)，本文只讲日常开发流程。

## 1. 分支模型

| 分支 | 用途 | 规则 |
|---|---|---|
| `main` | 稳定主线，**任何时刻都应可构建** | 禁止直接提交未验证的代码 |
| `feature/<name>` | 单个功能/里程碑任务 | 从 `main` 切出，完成后合回 `main` |
| `fix/<name>` | 缺陷修复 | 同上 |
| `docs/<name>` | 纯文档改动 | 可轻量，直接合入 |

单人开发时允许直接在 `main` 上提交，但**必须保证每次提交都能编译通过**（否则回滚与 bisect 失效）。

## 2. 提交规范（Conventional Commits）

```
<type>(<scope>): <描述>

<可选正文：为什么这么改>

<可选脚注>
```

| type | 用途 |
|---|---|
| `feat` | 新增功能 |
| `fix` | 修复缺陷 |
| `docs` | 文档 |
| `test` | 测试 |
| `build` | 构建系统 / 依赖 |
| `refactor` | 重构（不改行为） |
| `perf` | 性能优化 |
| `chore` | 杂项 |

**scope** 建议用模块名：`core` `network` `http` `ffmpeg` `media` `output` `stats` `docs`。

**示例**

```
feat(network): EventPoller 支持跨线程 async 投递

通过 PipeWrap 唤醒 epoll_wait，任务在事件线程执行，
保证连接对象仅在所属线程被访问，消除锁竞争。
```

```
fix(media): 修正 23.976fps 源的时间戳累计漂移

改用整数有理换算 ts = idx * 90000 * 1001 / 24000，
避免每帧浮点取整导致的单调性被破坏。
```

## 3. 提交粒度

- 一个提交只做一件事；**能编译通过**是硬要求。
- 同一里程碑内按"可独立验证的最小步"提交，便于回滚与 `git bisect`。
- 不要提交构建产物、日志、媒体文件（`.gitignore` 已覆盖）。

## 4. 发布流程

```
1. 确认 main 上全部验收通过（见 TESTING.md 第 8 节）
2. 更新 CHANGELOG.md
3. git add / commit（docs 或 chore 类型）
4. git push origin main
5. git tag -a vX.Y.Z -m "release vX.Y.Z"
6. git push origin vX.Y.Z
7. 校验：git ls-remote --tags origin  → 能看到该 tag
```

**五条强制要求**（摘自 VERSIONING.md，不得省略）：

1. 每一版都必须**上传 Git**（推送到远程才算发布）
2. 每个版本必须**打 tag**，格式 `vX.Y.Z`
3. tag 必须**推送到远程**
4. 版本号遵循**语义化版本**（X 不兼容 / Y 新增功能 / Z 修复）
5. 每个版本必须**可回滚**（该 tag 代码能独立构建并通过测试）

## 5. 回滚

```bash
# 查看某个版本（只读验证）
git fetch --tags && git checkout v0.1.0

# 推荐：以新提交回滚某次改动，保留历史
git revert <commit-id> && git push origin main

# 强制回到某 tag（改写历史，需确认）
git reset --hard v0.1.0 && git push --force-with-lease origin main
```

## 6. 远程仓库

| 项 | 值 |
|---|---|
| 仓库 | `git@github.com:bai1232/mzmedia.git`（SSH） |
| 默认分支 | `main` |
| 认证方式 | SSH key（不依赖 token，CI 之外无需 PAT） |

> 若仓库曾用旧名 `mzrtsp`，GitHub 会做自动重定向；本地请执行：
> `git remote set-url origin git@github.com:bai1232/mzmedia.git`
