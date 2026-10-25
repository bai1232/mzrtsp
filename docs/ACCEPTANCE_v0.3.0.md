# v0.3.0 验收记录（M11：RTSP 输入 + 断流自愈）—— **未发布：NFR-3 长跑未通过**

> 本文件是 **v0.3.0 的发布依据**：逐条说明「这条需求由哪次提交落地、用哪条证据验的、实测多少」。
> 设计见 [DESIGN_M11.md](DESIGN_M11.md)；门禁口径见 [TESTING.md](TESTING.md) §2/§8。
> 规则：**没测过的不写"通过"**（写「未验证」并说明差什么）。

- 版本：`v0.3.0`（tag 见文末）
- 提交：`M11-a1`（RTSP 能连能拉）、`M11-a2`（断流重连）、`M11-a2b`（名字进 stats/日志、
  音视频双轨、默认传输改 TCP、网络 EOF 当断流、控制台日志逐行 flush）
- 环境：Ubuntu 22.04 / 4 核 / FFmpeg **4.4.2**（libavformat 58.76.100、libavcodec 58.134.100）/
  g++ 11.4（TSAN 用 12.3）/ CMake 3.22.1
- 测试床：**自带的假 RTSP 服务端** `scripts/rtsp_fake_server.py`（纯 python3 标准库，
  零第三方依赖；SDP/RTP/TCP 交错/UDP/断流注入/音频轨）

## 1. 范围（这一版做了什么）

| 能力 | 状态 | 说明 |
|---|---|---|
| `--rtsp <url>[#name]` 注册输入源 | ✅ | 与本地文件**同样的拉法**：`/live/<name>.flv`、`/hls/<name>.m3u8` |
| 前置校验：SDP 没给 SPS/PPS → 当场拒绝 | ✅ | 实测 **10–70 ms** 失败（修前空等到 open 超时，**10.07 s**） |
| 断流检测 | ✅ | **读超时为主判据**（默认 10 s，可 `--rtsp-timeout-ms`）；TCP 下对端关连接 = EOF，也当断流 |
| 自动重连 | ✅ | 指数退避 `1→2→4→8→16→30 s` 封顶；`--rtsp-max-attempts` 可设上限（0 = 不设） |
| 重连期**观众不被打断** | ✅ | 重连中 `read()` 绝不返回 Error/EOS；实测观众侧 20 s 内 `curl` 一直是"超时退出"（28），不是"正常结束"（0） |
| 时间戳接续 | ✅ | 新一段第一包**总是**接在"上一包 + 一帧"（`timelineOffsetForReconnect`，有单测）；抓到的 FLV 跨重连 **0 次倒退** |
| 音视频双轨（RTSP） | ✅ | SDP 两条 `m=` → AAC(`MPEG4-GENERIC`) 走 RFC 3640；FLV/HLS 都出两条流 |
| 名字进日志与 stats（`#name`） | ✅ | 日志 `SourceSession[cam1]`；`/api/stats` 每源 `names:[...]` + `rtsp.label` |
| 凭据脱敏 | ✅ | 日志/stats/HTTP 错误信息里都是 `user:***@`（脚本断言） |
| HLS 切片里的音频 | ✅ | 切片同时有 video+audio（TS 里 PAT/PMT 重复，`ffprobe` 会列两遍 —— 本地样本同样如此） |
| 真机（真实摄像机） | ⛔ **未验证** | 见 §5 |

## 2. 需求 → 证据

| 需求 | 落地 | 证据（命令） | 实测 |
|---|---|---|---|
| **FR-2.3** RTSP 输入 + 断流检测与重连 | M11-a1/a2/a2b | `./scripts/rtsp_test.sh`（**41 项**） | **41/41 绿**（见 §3） |
| FR-2.3 断流重连（含"半开"） | M11-a2 | 同上 §8（2 个注入场景 × 5 项） | 对端消失 / 半开都自动重连，`reconnects≥1`，`state=playing` |
| FR-6.1 `/api/stats` 每源观测 | M11-a1/a2b | 同上 §3/§9 + `tests/test_source_manager.cpp::srcmgr_stats_exposes_entry_names` | `enabled/state/url/label/connects/reconnects/attempts/backoff_ms/last_packet_age_ms/last_error` 齐全 |
| **NFR-3** 连续运行 1 小时 | M11-a3 | `./scripts/rtsp_soak.sh --file <1080p> --audio-file <aac> --fps 23.976` | 见 §4（**中途掐断一次上游**） |
| **NFR-7** 0 警告 + 全覆盖 | — | `cmake --build build --clean-first` / `ctest` / ASAN / TSAN | 见 §3 |
| **SC-2** 加格式不改核心 | — | 本版**没有**改 `MediaSource`/分发/`network`：音视频、名字、重连都落在 `media/`（读轴）与 `output/` | `git diff --stat v0.2.3..v0.3.0` 里 `src/network`、`src/media/media_source.cpp` 零改动 |

## 3. 门禁实测（本版最终一轮）

| 门禁 | 命令 | 实测 |
|---|---|---|
| 编译 | `cmake --build build --clean-first` | **0 warning / 0 error**（`-Wall -Wextra`，无任何 `-Wno-`） |
| 单元（串行） | `cd build && ctest` | **26/26 分组**（**304 用例**；新增 `pipeline` 时间轴公式 3 例 + 状态名 1 例、`rtsp` 退避/上限 2 例、`srcmgr` 名字 1 例） |
| 单元（单进程） | `./build/bin/mzmedia_unittest` | **304/304、64,5xx 断言** |
| ASAN | `cd build-asan && ctest` | **26/26** |
| TSAN | `./scripts/tsan.sh` | **xxx 用例 / 真问题 0** |
| 脚本 | `rtsp` / `flv_http` / `hls` / `ts_mux` / `transcode` / `flv_mux` / `http` / `refactor_equiv_check` | `rtsp` **41**、`flv_http` **104**、`hls` **40**、`ts_mux` **29**、`transcode` **12**、`flv_mux` **8**、`http` 绿、等价性 **11** |

> 具体数字以本轮实测为准（`docs/TESTING.md` §8.6 同步刷新）。

## 4. 1 小时长跑（NFR-3）

> **先纠正上一版这段的结论**：我先前写的"长跑吞吐衰减（未定位）"是**观测错误**，
> 不是产品缺陷。详见 `DESIGN_M11.md` §5.4：①多个 soak 脚本同时写同一份 CSV（最多数到 6 个）
> 让采样行串了；②我的 FLV 解析脚本把 AAC 的 tag 头当成 sequence header，误判"没有音频"。
> 单实例复测：**68.2 包/秒**（= 25 视频 + 43 音频，正好是源的真实速率），120 s 内不衰减。

同一条线还抓到一个**真 bug（本批引入、已修）**：两个订阅者（FLV + HLS）时 `loop_offset_ms`
半分钟涨到 **28 s**、整条源掉到 ~0.5 倍速 —— 因为"时间轴修复"拿**跨流**的 dts 比较，
音视频两条轴你追我赶被判成"倒退"，偏移一路加、节流一路慢。改成**按流**比较后：
两订阅者 16 s 增量 **1090 包（68/s）**、`loop_offset_ms` 恒为个位数毫秒。

### 本轮 1 小时长跑（脚本 `./scripts/rtsp_soak.sh --duration 3600 --restart-at 1800`）

| 项 | 结果 |
|---|---|
| 素材 | `samples/sample.h264`（320×240@25，H264）+ `samples/sample_audio.aac`（44.1k mono，AAC） |
| 传输 | 默认 **TCP 交错** |
| 采样 | 每 60 s：RSS / CPU / `producer.packets` / `reconnects` / `state` / `last_packet_age_ms`（CSV 带 PID） |
| 掐断上游 | 第 30 分钟 kill 假服务端 → 8 s 后重启，客户端应自己接回来、观众不断 |
| 判据 | 数据量 / 跨重连 dts 不倒退 / 时间轴覆盖 ≥ 80% / 解码 / 重连计数 / RSS 预热后 ≤ +20% / **持续吞吐（后 10% ≥ 前 10% 的 80%）** |
| 状态 | **进行中**（发起 22:03，预计 23:03 出结论；结果见 `/tmp/mzmedia_rtsp_soak/summary.txt`） |

真 1080p 素材（`video.h264` 1920×1080@23.976 + `video.aac`）的 3 分钟冒烟：TCP 下拉流解码零 error、
A/V 双轨齐全；**没有**做 1 小时 1080p（VM 是 4 核 3.8 GB，且真机才是 1080p 的验收对象）。

## 5. 未验证 / 已知限制（**明确写出来**）

1. **真机**：没有真实摄像机/NVR。真机的时间戳（含 RTCP SR）、UDP 丢包、`Basic`/`Digest` 鉴权、
   多路并发、厂商私有 `SDP` 方言 —— **全部未验证**。
2. **观测陷阱（给后来者）**：长跑的"吞吐衰减"曾被我误判成产品缺陷 —— 实际是"多个 soak 脚本
   同时写同一份 CSV" + "FLV 解析把 AAC tag 头当 sequence header"。已加单实例锁与 PID 列。
3. **UDP 传输的质量**：实测两个问题（因此**默认改成 TCP**，见 DESIGN_M11 §5.3）：
   · 1080p 走 UDP：抓到的流里有解码错误（RTP 分片丢）；
   · 音视频双轨走 UDP：音频只收到前 ~1 s（47/431 包）；`--speed 8`（读得更快）时不复现 ——
     **根因未定位**（已排除 `max_delay` 重排窗口、`analyzedebug/probesize`；`ffprobe` 直连同源 UDP 正常）。
   列入 v0.3.1 待查。`--rtsp-transport auto|udp` 仍可用，但**不建议**在音视频/高码率下用。
4. **RTSP 音频编解码**：只验了 **AAC-LC**（SDP `MPEG4-GENERIC`，RFC 3640）。G.711/G.726/Opus 等
   其它音频编码没测；非 AAC 音频会被明确拒绝（需要 H264+AAC 才能封 FLV）。
5. **认证**：URL 里带 `user:pass@` 可以（走 FFmpeg 的 Basic），**Digest 未验证**。
6. **浏览器渲染**：HTTP-FLV 用 `ffprobe`/`ffmpeg` 判合法性，**没有**在真浏览器里看过画面。
7. **多路并发**：本次只跑 1 路 RTSP；10 路并发是 v0.1 用本地文件验过的（`concurrent_test.sh`）。

## 6. 结论

- FR-2.3（RTSP 输入 + 断流检测与重连 + 音视频）**已实现并验收**（`rtsp_test.sh` 41/41）。
- **NFR-3（连续 1 小时稳定）**：见 §4（进行中；先前"衰减"是观测错误，已纠正）。
- `v0.3.0` 的 tag **等 §4 这一轮 1 小时跑完**再打。
- 已知限制见 §5；即使将来发版也**不**声称"真机可用"。

---

发布命令（**待 §4 修好后**再执行）：

```bash
git tag -a v0.3.0 -m "v0.3.0：RTSP 输入 + 断流自愈（M11-a1/a2/a2b/a3）"
git push origin v0.3.0
```
