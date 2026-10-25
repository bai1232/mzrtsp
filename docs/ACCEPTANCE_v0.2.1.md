# v0.2.1 验收报告（补丁版）

> 本文件是 **v0.2.1 的发布证据**。v0.2.0 的完整矩阵见 [ACCEPTANCE_v0.2.0.md](ACCEPTANCE_v0.2.0.md)；
> 本版**只修缺陷、不新增能力**，因此只逐条列出**受影响的**需求，其余沿用 v0.2.0 的结论。
> 原则：**只写跑过的**。没跑过的写"未验证"。

| 项 | 值 |
|---|---|
| 版本 | **v0.2.1**（补丁：`Z` 位递增，向后兼容的缺陷修复） |
| 日期 | 2026-10-25 |
| 结论 | 修了**一族**（三处）时间戳缺陷，共同点是「**B 帧源**（x264 的默认产物）」：① `Demuxer` 把 pts 也钳了 → 显示顺序被篡改；② `MpegTsMuxer` 逐包钳负时间戳 → 前几帧的 dts 差异被抹掉；③ 循环重开的趟偏移没算首包 dts → 每个接缝倒退。**三条都不报错**，只表现为"画面顺序错乱 / 播放抖一下" |

## 0. 一页结论（修前 vs 修后，均为实测）

样本：`samples/bf.mp4`（`scripts/make_samples.sh` 用 `-bf 2` 生成，25 帧 / 1 s / 25 fps）。

### 0.1 FLV 的 CompositionTime（`curl -N /live/bf.mp4.flv`，逐 tag 读）

| 包（解码序） | 源 pts−dts (ms) | **修前 cts** | **修后 cts** |
|---|---|---|---|
| 1 | 80 | 80 ✅ | 80 ✅ |
| 2 | 160 | 160 ✅ | 160 ✅ |
| 3 | **40** | **120 ❌** | **40 ✅** |
| 4 | **40** | **80 ❌** | **40 ✅** |
| 5 | 160 | 160 ✅ | 160 ✅ |
| 6 | **40** | **120 ❌** | **40 ✅** |
| 7 | **40** | **80 ❌** | **40 ✅** |
| 8 | 160 | 160 ✅ | 160 ✅ |

- `ffmpeg -v error -i x.flv -f null -`：修前 **3 条** `Application provided invalid,
  non monotonically increasing dts to muxer`；修后 **零输出**。
- pts 序列：修前 `80,200,200,200,320,320,320,440`（B 帧全被抬到上一个 pts）；修后
  `80,200,120,160,320,240,280,440`（与源的显示顺序一致）。

### 0.2 TS / HLS 切片的 PES 时间戳

| 指标 | 修前 | 修后 |
|---|---|---|
| 前 3 个 PES 的 dts | **1.000, 1.000, 1.000**（负 dts 被逐包钳 0） | **1.000, 1.040, 1.080**（整体平移 80 ms，差值不变） |
| 前 3 个 PES 的 pts | 1.000, 1.120, 1.040 | 1.080, 1.200, 1.120 |
| `ffmpeg -f null -` 读切片 | 4 条 dts 警告 | **零输出** |

### 0.3 循环 + 负首包 dts（`--loop`）

| 指标 | 修前 | 修后 |
|---|---|---|
| 接缝处 dts | 上一趟末包 **961 ms** → 下一趟首包 **921 ms**（**倒退 39 ms**） | 严格递增，无倒退/重复 |
| 2 秒 4 倍速拉 4 秒 | 每个接缝抖一次（200 tag 里 3 处倒退） | **200 tag / 跨接缝 dts 严格递增** |

## 1. 验收环境

与 v0.2.0 相同（Ubuntu 22.04.5 / 4 核 / g++ 11.4 + 12.3(TSAN) / CMake 3.22.1 /
FFmpeg 4.4.2 + libswscale 5.9.100 / Node 12.22.9 / Python 3.10.12）。本版**没有**新增依赖。

## 2. 受影响的需求（逐条）

| # | 状态 | 本版做了什么 / 证据 |
|---|---|---|
| **FR-3.4** 音视频同步 + 时间戳统一转换 | ✅ **修复（本版核心）** | 时间戳换算的"守卫"被修正：**pts 不再做单调钳制**（B 帧的 pts 乱序是正常现象，只计数 `ptsOutOfOrder()`），**dts 继续钳**。FLV 的 CompositionTime = `pts − dts` 因此与源**逐包一致**；TS 侧改成"首包整体平移"而不是逐包钳 0。证据：§0.1/§0.2 + 单测 `ffmpeg_bframes_pts_are_preserved_and_dts_stays_monotonic` / `flv_bframes_composition_time_comes_from_source` / `mpegts_negative_first_dts_is_shifted_not_clamped` |
| **FR-1.2** 直播式重放（可选循环、时间戳连续） | ✅ **修复** | 循环重开的趟偏移改成 `pass_end − pass_start`（首包 dts 为负的源不再倒退）；转码路径的接缝修正改成**增量**（`本趟真实长度 − 本趟转出媒体时长`）。证据：`producer_loop_with_negative_first_dts_does_not_regress` + `flv_http_test.sh` §12（循环 200 tag 严格递增）+ 原有的 `producer_loop_offsets_timestamps_monotonically` / `producer_transcode_loop_keeps_audio_and_video_aligned` 仍绿 |
| **FR-2.4** 边界（VFR / 时长未知）不崩 | ⚠ 仍是"部分验证" | 本版让 **pts 乱序（B 帧的正常现象）** 与 **dts 回退（真异常）** 在统计上分开；VFR 仍然**没有专门样本**（沿用 v0.2.0 的"未验证"结论） |
| **FR-6.1** `/api/stats` 可观测性 | ✅ 增强 | 每源新增 `producer.ts{pts_out_of_order,dts_clamped}`：`pts_out_of_order > 0` 才说明"B 帧信息没被搞丢"，`dts_clamped` 非 0 才是真异常。`flv_http_test.sh` §12 断言这两条 |
| **NFR-7** 零警告 + 覆盖 | ✅ | `--clean-first` 重建 0 warning；用例 **281 → 286**（+5）；`flv_http_test.sh` **96 → 104 项** |
| 其余 SC/NFR/FR | 不变 | 本版只碰"时间戳"这一条纵线（`ffmpeg/demuxer`、`output/mpegts_muxer`、`media/demuxer_producer` 的接缝计算），不涉及网络/HTTP/分发；回归面由全量 `ctest` + 6 个脚本覆盖 |

## 3. 变异验证（证明新断言真的会红）

三次注入都是**临时改源码 → 重建 → 跑用例 → 恢复**。

| 变异 | 注入内容 | 预期 | 实测 |
|---|---|---|---|
| **M1** 把 pts 钳回去 | 在 `readPacket()` 里加 `pts_ms = _last_pts[idx]` | B 帧的三个用例红 | **2/2 红**：`ffmpeg_bframes_pts_are_preserved_and_dts_stays_monotonic`、`producer_bframes_pts_pass_through_unclamped`（cts 用例因直接喂源数据而仍绿 —— 见 §5 第 1 条） |
| **M2** 趟偏移退回旧公式 | `pass_start = 0` | 循环接缝用例红 | **1/1 红**：`producer_loop_with_negative_first_dts_does_not_regress` |
| **M3** TS 退回逐包钳 0 | 去掉 `_base_ms` 平移 | TS 用例红 | **1/1 红**：`mpegts_negative_first_dts_is_shifted_not_clamped`（3 条断言失败） |

## 4. 门禁（本版实测，命令可复跑）

| 门禁 | 命令 | 实测 |
|---|---|---|
| 编译 | `cmake --build build -j4 --clean-first` | **0 warning / 0 error**（`-Wall -Wextra`，无任何 `-Wno-`） |
| 单元（串行） | `cd build && ctest --output-on-failure` | **24/24 分组、286 用例**（**不加 `-j`**） |
| 单元（单进程全量，诊断） | `./build/bin/mzmedia_unittest` | **286/286 用例、63,265 条断言、0 失败** |
| 单元（ASAN） | `cmake --build build-asan -j4 && cd build-asan && ctest` | **24/24 通过**（本轮无抖动） |
| 并发（TSAN） | `./scripts/tsan.sh` | **375 用例 | 已知误报 0 | 真问题 0**。⚠ 见下方"一次时序抖动" |
| 端到端（FLV / HLS / TS / 转码） | `scripts/{flv_http,hls,ts_mux,transcode}_test.sh` | **104/104**、**32/32**、**29/29**、**12/12** |
| 端到端（HTTP / FLV 封装） | `scripts/{http,flv_mux}_test.sh` | 全绿（8/8） |
| **tag 独立构建** | `git clone --branch v0.2.1 … && ./scripts/make_samples.sh && cmake -B build && cmake --build build -j4 && (cd build && ctest)` | 见 §7.1 |

> **一次时序抖动（诚实记录）**：门禁脚本里那一轮 TSAN 的 `srcmgr` 组红了一次
> （15 例里 1 例、3 条断言失败，**该组耗时 31 s** —— 正常是 1.2 s，说明当时机器被压住了）。
> 立刻单独重跑该组：**15/15 通过、1.2 s**；再整轮重跑 `./scripts/tsan.sh --no-build`：
> **375 用例、真问题 0、`tsan2=0`**。判定口径同 `TESTING.md` §2.2：计时敏感的用例只按
> "单组单跑 / 整轮重跑"判定。本版改动不碰线程/定时器（只改时间戳算术）。

## 5. 已知边界与"未验证"（诚实版）

| # | 项 | 说明 |
|---|---|---|
| 1 | `flv_bframes_composition_time_comes_from_source` 这个用例**抓不到 M1**（pts 钳回去） | 它把**源包**直接喂给 `FlvMuxer`（绕过 `Demuxer`），锁的是"封装层不改 cts"；抓 `Demuxer` 钳制的是另外两条用例。两者是**分层**的，不是重复 |
| 2 | 负 cts 的策略 | FLV 允许 24 位补码负值（`flv_video_tag_layout_and_composition_time` 覆盖 -40 的编码）；本版**不改**这个行为，只保证"正常的 B 帧不会产出负 cts"（实测 cts ∈ {40,80,160}） |
| 3 | VFR 源 | 仍然没有专门样本（转码器会 Warn 并按 CFR 规整，remux 只是"pts 不再被钳"）。**未验证** |
| 4 | 其他容器的 B 帧 | 只实测了 MP4（`bf.mp4`）；MKV/TS 的 B 帧源**没有样本**（同一套代码路径，但不算已验证） |
| 5 | 真实播放器观感 | `ffmpeg -f null -` 零输出 + cts 与源一致，是"顺序正确"的机器证据；**没有人眼看过画面**（沿用 v0.2.0 §6 第 1 条） |
| 6 | 1 小时长跑 | 本版没重跑（v0.2.0 跑过：稳态 RSS 0.00 MB/h、fd 恒定 31）。本版改动只影响时间戳数值，不影响资源生命周期 → 沿用，但**不声称本轮重跑** |

## 6. 复现步骤

```bash
git checkout v0.2.1
cmake -B build && cmake --build build -j"$(nproc)"
./scripts/make_samples.sh          # ★ 必需：bf.mp4 就是 B 帧样本

# ① 机器判定（谁改坏了都会红）
cd build && ctest --output-on-failure
./bin/mzmedia_unittest --group bframes

# ② 端到端：修前会看到 cts 乱、ffmpeg 出 non-monotonic 警告
../scripts/flv_http_test.sh        # 看 §12「B 帧源」

# ③ 手动看一眼
./bin/mzmedia --loop &
curl -N http://127.0.0.1:8080/live/bf.flv -o /tmp/bf.flv
ffprobe -v error -select_streams v:0 -show_entries packet=pts_time,dts_time -of csv=p=0 /tmp/bf.flv | head
ffmpeg -v error -i /tmp/bf.flv -f null -     # 应无输出
```

## 7. 发布信息与回滚

- **tag**：`v0.2.1`（annotated，指向本文件所在的发布提交）
- **语义化版本**：`Z` 位递增（向后兼容的缺陷修复）。唯一的**可观测行为变化**是时间戳数值：
  TS 切片的 PES 时间戳整体平移了"首包负 dts"的量（差值不变），FLV 的 CompositionTime 恢复为源的真实值
- **回滚**：`git revert <commit>`（保留历史）；或 `git checkout v0.2.0`（回到修前 —— B 帧源会重新出现顺序错误）
- 本版变更：见 [`CHANGELOG.md`](../CHANGELOG.md) 的 `[0.2.1]`

### 7.1 tag 独立构建与测试（`VERSIONING.md` §5）

tag 推送后**从本地仓库重新 clone**（不依赖工作区任何未提交改动），走 `VERSIONING.md` §5 的最后一问：

```bash
git clone --branch v0.2.1 /home/bpy/MyZltoolkit /tmp/v021_verify && cd /tmp/v021_verify
./scripts/make_samples.sh          # 样本不入库：bf.mp4 正是本版的关键样本
cmake -B build && cmake --build build -j4
cd build && ctest --output-on-failure
```

| 步骤 | 实测 |
|---|---|
| clone | `clone=0`；`HEAD=4a70bcb`；`git describe --tags` = **`v0.2.1`** |
| `make_samples.sh` | `samples=0`（含 `bf.mp4` / `sample_hevc.mp4` / `sample1080.mp4` 等） |
| `cmake -B build` + `cmake --build build -j4` | `configure=0`、`build=0`，**`warnings=0`** |
| `ctest` | **24/24 通过**（286 用例） |
| 单进程全量 | **286/286、63,265 断言** |

> 这一步同时验证"可回滚"：切到 `v0.2.1` 能独立构建并跑完门禁；回退到 `v0.2.0` 也能构建
> （但会重新出现 B 帧源的顺序问题 —— 这正是本版要修的）。
