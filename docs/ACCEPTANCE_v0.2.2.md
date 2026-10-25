# v0.2.2 验收报告

> 本文件是 **v0.2.2 的发布证据**，也是 **v0.2 里程碑的收尾记录**。
> 前三块的完整矩阵见 [ACCEPTANCE_v0.2.0.md](ACCEPTANCE_v0.2.0.md)（M8/M9/M10-a/b）与
> [ACCEPTANCE_v0.2.1.md](ACCEPTANCE_v0.2.1.md)（B 帧时间戳修复）；
> 本版只做一件事：**补齐 M10-c（转码 + HLS）**，因此只逐条列出**受影响的**需求。
> 原则：**只写跑过的**。

| 项 | 值 |
|---|---|
| 版本 | **v0.2.2**（`Z` 位；补完当前里程碑的功能，见 §6 的版本号说明） |
| 日期 | 2026-10-25 |
| 结论 | `/hls/` 现在也支持转码：`/hls/<name>.m3u8?transcode=1[&h=]`。与 `/live/` 共用同一套参数解析、拒绝语义与并发上限；**切片 URI 自带规格**，所以同一个服务上"直通 + 转码"两路 HLS 可以同时被看 |

## 0. 一页结论（实测）

| 判据 | 实测 |
|---|---|
| 转码 HLS 能播 | `GET /hls/tx.mp4.m3u8?transcode=1&h=120` → 200；切片按等比缩到 **212x120**（1920x1080 @120p），`ffprobe` 认 **h264** |
| **独立裁判** | `ffmpeg -v error -i 'http://…/hls/tx.mp4.m3u8?transcode=1&h=120' -t 4 -f null -` → **零输出** |
| 切片 URI 自解释 | 播放列表里每条切片都是 `/hls/tx.mp4-<seq>.ts?transcode=1&h=120`（**5/5 条**都带规格） |
| 直通不受影响 | 同一服务、不带查询串 → 切片仍是 **1920x1080** |
| 开关语义 | 未开 `--transcode` 时 `?transcode=1` → **501**（**不降级成直通**） |
| 参数校验 | `?h=120`（缺 transcode=1）/ `?transcode=abc` / `?foo=1` → **全部 400** |
| 流键隔离 | `/api/stats` 里同名两条流：`tx.mp4`（直通）与 `tx.mp4|h=120`（转码），各自 `slices` 独立 |

## 1. 受影响的需求（逐条）

| # | 状态 | 本版做了什么 / 证据 |
|---|---|---|
| **FR-3.2** HLS 输出 | ✅ **扩展** | 同一个 `HlsWriter`/`HlsService` 现在能服务转码流：源由 `SourceManager::acquire(path, spec)` 提供（转码后的宽高与 avcC 已就位，切片器**不知道**这是转码流）。证据：`hls_test.sh` §10（8 项） |
| **FR-4.1** HLS 路由 | ✅ **扩展** | `/hls/<name>.m3u8?transcode=1[&h=]`；切片 `/hls/<name>-<seq>.ts?transcode=1&h=120`。参数解析与 `/live/` **同一套** `parseTranscodeQuery()` |
| **FR-1.3** 懒启动 + 空闲释放 + 转码并发上限 | ✅ 覆盖到 HLS | HLS 的转码流走**同一个** `acquire(path, spec)` 入口 → 吃同一份并发名额、同样在租约到期后释放（`hls.streams` 回落）。⚠ **未做**：专门验证"HLS 的转码流把名额占满后 `/live/` 的转码请求被拒"的跨入口用例（见 §5） |
| **FR-6.1** `/api/stats` | ✅ 扩展 | `hls.playlists[]` 增 `key` 与 `transcode{enabled,out_height}`（一个 `name` 可能对应两条流，`name` 不再唯一） |
| **FR-7.1** 命令行转码开关 | ✅ 覆盖到 HLS | 与 `/live/` 同一套：`--transcode` 关着 → 501；`--transcode-height` 是缺省上限 |
| 其余 SC/NFR/FR | 不变 | 本版只碰 `output/hls_*` 与 `main.cpp` 的 HLS 路由（外加 `HlsWriter::Config::uri_query`）；`media/`、`network/`、`http/`、`ffmpeg/` 均未改 |

## 2. 设计要点（为什么这么切）

| 决定 | 选择 | 排除项与原因 |
|---|---|---|
| 规格怎么进 HLS | **查询串**：`?transcode=1&h=120`，并**写进每条切片 URI** | 排除"路径编码变体名"（`tx-120p.m3u8`）：会与真实文件名冲突（`movie-720p.mp4` 到底指哪个？）。排除"服务端记住播放器要哪一路"：HLS 是无状态拉模型，播放器可能换连接/换 IP/隔几分钟回来 |
| 流键 | `HlsService::streamKey(name, spec)`，与 `SourceManager::sourceKey` **同构** | 排除"一个名字只能一条流"：那会让直通与转码互斥，而它们的用途恰恰是同时提供 |
| 调度 | **复用** `/live/` 的 `acquire(path, spec)` | 排除"给 HLS 单写一套转码调度"：并发上限/让出名额/懒启动都会有两份实现，必然漂移 |
| 切片 URI 的自解释 | `HlsWriter::Config::uri_query` | 排除"在服务端按 m3u8 请求顺序推断"：播放器行为不受我们控制 |

## 3. 变异验证

| 变异 | 注入内容 | 预期 | 实测 |
|---|---|---|---|
| **M10-c-1** 切片 URI 不带规格 | 去掉 `m3u8 += _config.uri_query;` | `hls_slice_uri_carries_transcode_query` 红 | **1/1 红**（2 条断言失败）；恢复后绿 |

## 4. 门禁（本版实测，命令可复跑）

| 门禁 | 命令 | 实测 |
|---|---|---|
| 编译 | `cmake --build build -j4 --clean-first` | **0 warning / 0 error**（`-Wall -Wextra`，无任何 `-Wno-`） |
| 单元（串行） | `cd build && ctest --output-on-failure` | **24/24 分组、288 用例** |
| 单元（单进程全量，诊断） | `./build/bin/mzmedia_unittest` | **288/288、63,275 条断言** |
| 单元（ASAN） | `cmake --build build-asan -j4 && cd build-asan && ctest` | **24/24 通过** |
| 并发（TSAN） | `./scripts/tsan.sh` | **379 用例 | 已知误报 0 | 真问题 0** |
| 端到端 | `scripts/{hls,flv_http,ts_mux,transcode,flv_mux,http}_test.sh` | **40/40**、**104/104**、**29/29**、**12/12**、8/8、全绿 |
| **tag 独立构建** | `git clone --branch v0.2.2 …` → `make_samples` → `cmake` → `build` → `ctest` | 见 §7.1 |

### 4.1 TSAN 与"脚本项数的调整"

- TSAN：`./scripts/tsan.sh` → **379 用例 / 已知误报 0 / 真问题 0**（`transcode` 组 13 例、
  `hls` 组 8 例，都含本版新增用例；本版没有新增线程或定时器）。
- 项数变化：`hls_test.sh` **32 → 40**（+8：§10 转码+HLS）；`flv_http_test.sh` **101 → 104**
  （+3：HLS 的 `?transcode=1` → 501 与两条非法参数 → 400。原来那条"必须 400（属 M10-c）"的断言
  已过期 —— HLS 现在**真的接受**这些参数了，所以改成验"同一套语义"）。

## 5. 已知边界与"未验证"（诚实版）

| # | 项 | 说明 |
|---|---|---|
| 1 | **跨入口的并发名额** | HLS 转码流与 `/live/` 转码流共用同一份名额（同一份 `SourceManager` 代码），但**没有**专门用例验证"一边占满、另一边被拒"。`srcmgr` 的单测覆盖的是同一入口，属【推论】 |
| 2 | **HLS 的转码变体清单** | 未做：一个 m3u8 列多档清晰度（`#EXT-X-STREAM-INF`）、按客户端自适应。现在是"每档一个 m3u8 URL" |
| 3 | **真实浏览器 / hls.js 的查询串** | 机器上验过 `ffmpeg`（作为 HLS 客户端会照抄切片 URI，含查询串）；**hls.js 与 Safari 未人眼验证**。若某个客户端丢掉查询串，表现是"画面分辨率跳回原始"而不是报错 —— 这一点已写进 `DESIGN_M10.md` §8.11 |
| 4 | 转码分辨率不是整数倍 | 1920x1080 + `h=120` → 宽 213 取偶成 **212**（yuv420p 要求偶数）。宽度因此不是严格的 16:9（212x120 ≈ 1.767 vs 1.778），属"少 1 像素"的已知取舍 |
| 5 | 1 小时长跑 | 本版没重跑（v0.2.0 跑过：稳态 RSS 0.00 MB/h、fd 恒定 31）。改动只影响 HLS 的"消费者入口"，不改变源线程/队列/生命周期 → 沿用，但**不声称本轮重跑** |

## 6. 版本号说明（诚实版）

`VERSIONING.md` 的语义化规则里，"新增功能"应递增 `Y`（→ `0.3.0`）。本版实际是**新增能力**
（转码 + HLS），但：

- `ROADMAP.md` 里 `v0.3.0` 已绑定 **RTSP 输入 + 解码处理**；
- 本版是 **v0.2 里程碑的收尾**（M10-c 本来就是 ROADMAP 里 v0.2 的待办项，`DESIGN_M10.md` §4 写着）；
- 因此按"**里程碑内补齐**"处理，递增 `Z` 位。**这是一个刻意的偏离**，记在这里而不是藏着。

若严格按语义化，本版编号应为 `v0.3.0`、RTSP 顺延为 `v0.4.0`；需要时可用 `git tag` 重新编排（代码不变）。

## 7. 发布信息与回滚

- **tag**：`v0.2.2`（annotated，指向本文件所在的发布提交）
- **回滚**：`git revert <commit>`；或 `git checkout v0.2.1`（回到"转码只做 /live/"的状态 ——
  `/hls/*?transcode=1` 会重新变成 400）
- 本版变更：见 [`CHANGELOG.md`](../CHANGELOG.md) 的 `[0.2.2]`
- **v0.2 里程碑到此收尾**：M8（MKV/TS 输入）、M9（HLS：TS 封装 + 切片器 + 播放页）、
  M10（转码：组件 + 服务接线 + HLS）全部落地并各自有验收文档

### 7.1 tag 独立构建与测试（`VERSIONING.md` §5）

tag 推送后**从本地仓库重新 clone**（不依赖工作区任何未提交改动）：

```bash
git clone --branch v0.2.2 /home/bpy/MyZltoolkit /tmp/v022_verify && cd /tmp/v022_verify
./scripts/make_samples.sh          # 样本不入库
cmake -B build && cmake --build build -j4
cd build && ctest --output-on-failure
```

| 步骤 | 实测 |
|---|---|
| clone | `clone=0`；`HEAD=96ed5aa`；`git describe --tags` = **`v0.2.2`** |
| `make_samples.sh` | `samples=0` |
| `cmake -B build` + `cmake --build build -j4` | `configure=0`、`build=0`，**`warnings=0`** |
| `ctest` | **24/24 通过**（288 用例） |
| 单进程全量 | **288/288、63,275 断言** |

> 回滚性：`git checkout v0.2.1` 能独立构建（转码只做 `/live/`、`/hls/*?transcode=1` 回到 400）；
> `v0.2.0`/`v0.1.1` 同理。
