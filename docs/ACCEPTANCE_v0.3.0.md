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

## 4. 1 小时长跑（NFR-3）—— **未通过（2026-10-25）**

脚本：`./scripts/rtsp_soak.sh --duration 3600 --restart-at 1800`
（假服务端 = H264 320×240 + AAC；每 60 s 采一次 RSS/CPU/包数/重连数；第 30 分钟掐断上游）
另外用真 1080p 素材（`video.h264` 1920×1080@23.976 + `video.aac`）跑过 3 分钟冒烟。

**结论：这一版不能声称 1 小时稳定；`v0.3.0` 未打 tag。**

| 观察 | 数据 |
|---|---|
| 前 10 秒一切正常 | `rtsp_test.sh` §9：10 s 内 384 个音频包（≈38/s，满速）+ 视频满速，解码零 error |
| 长时间运行后**吞吐衰减** | 同一条源，跑到 ~60 s 后 `producer.packets` 的增长降到 **~19/s**（音频 25fps+43fps 应 ≈68/s）；`bitrate_bps` 从名义 138 kbps 掉到 **~99 kbps** |
| 但**没有掉包计数** | `/api/stats`：`dropped=0`、`dropped_incoming=0`、`rejected=0`，`state=playing`、`pts_out_of_order=0`、`dts_clamped=0` |
| 音频最先消失 | 长跑抓到的 FLV **一个音频 tag 都没有**（`sample_audio.aac` 的 88 帧只在前 2 秒），同一源 10 s 短跑时音频是满的 |
| 采样循环也会卡住 | `soak.csv` 只写到 t=61s，`hls.log` 写到 102s —— 说明**连 HTTP 侧也被冻住**，不只是某个消费者慢 |
| 冒烟（1080p，3 分钟） | 第 1 次：64 MB / 170 s、`reconnects=1`（掐断后自愈）、dts 0 次倒退、RSS 32→72 MB；第 2 次：只有 3.3 MB / 145 s（同样衰减） |

**已经排除的**：
- 不是"重连在打转"：整轮 `断流` 日志 **0 条**（`grep -c 断流`）；
- 不是队列塞满：`dropped/dropped_incoming/rejected` 全是 0，`popped == delivered`；
- 不是 HLS 消费者拖累：**只开 FLV**（不请求 m3u8）时同样衰减（20 s → 537 包，60 s → 1297 包 ≈ 19/s）；
- 不是假服务端崩溃：`fake.log` 无 traceback，只有 1 条客户端连接，OPTIONS 保活正常回 200。

**下一步（v0.3.1 第一件事）**：按"时间 × 吞吐"曲线定位是
①`SourcePump` 的 `Throttle`（按 dts 节流）在双轨/长时间下的累积误差；
②FFmpeg RTSP **interleaved TCP** 的读节奏（`av_read_frame` 每次只取一个交错帧）；
③还是假服务端（python 双线程 + `sendall`）自身跑不满。
最快的判别：让 `--speed 8` 跑长跑（不节流）看是否还衰减；再把假服务端换成
`ffmpeg` 自己的 RTP 打包（或真机）交叉验证。

> 这一条**没有**被"看起来还行"糊过去：NFR-3 明确是"连续 1 小时稳定"，实测连 2 分钟就衰减，
> 所以 tag 不发、ACCEPTANCE 不写"通过"。

## 5. 未验证 / 已知限制（**明确写出来**）

1. **真机**：没有真实摄像机/NVR。真机的时间戳（含 RTCP SR）、UDP 丢包、`Basic`/`Digest` 鉴权、
   多路并发、厂商私有 `SDP` 方言 —— **全部未验证**。
2. **长时间运行的吞吐衰减（见 §4）**：长跑 2 分钟起就衰减，根因未定位 —— **这是 v0.3.0 不发布的原因**。
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
- **NFR-3（连续 1 小时稳定）未通过**（§4：长时间吞吐衰减，根因未定位）。
- 因此 **`v0.3.0` 不打 tag**；等 §4 的衰减定位并修掉后再发。
- 已知限制见 §5；即使将来发版也**不**声称"真机可用"。

---

发布命令（**待 §4 修好后**再执行）：

```bash
git tag -a v0.3.0 -m "v0.3.0：RTSP 输入 + 断流自愈（M11-a1/a2/a2b/a3）"
git push origin v0.3.0
```
