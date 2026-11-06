# RTSP 输出设计（v0.4-a，R1 已落地）

> 目标：**我们当 RTSP 服务端**，标准客户端（ffprobe / ffmpeg / ffplay / VLC / NVR）可以直接
> `rtsp://<ip>:<port>/live/<name>` 拉流。与 v0.3 的 RTSP **输入**互补：
> 输入用 FFmpeg 当客户端（不自己写协议），输出**自己写服务端 + RTP 打包**。

## 1. 落点（复用优先，`media/` 一行不改）

| 文件 | 职责 |
|---|---|
| `src/output/rtsp_server.{h,cpp}` | `RtspServer` **组合** `TcpServer`（监听/accept/会话表/关停排水全部复用）；`RtspSession : public Session` 只重写 `onRecv`（分帧）与复用 `onIdle`（超时） |
| `src/output/rtsp_request.{h,cpp}` | 小型 RTSP 请求解析（**不复用 HttpParser**：它的 Method 枚举里没有 DESCRIBE/SETUP/PLAY，硬塞会让 HTTP 侧跟着变形） |
| `src/output/sdp_builder.{h,cpp}` | `StreamInfo` → SDP（含 **`c=` 连接行**、`sprop-parameter-sets`、AAC `config=`） |
| `src/output/rtp_packetizer.{h,cpp}` | RTP 头 / seq / SSRC / FU-A 分片 / AAC AU 头 |
| `src/ffmpeg/h264_util.*` | 新增 `avccToNalus()`：AVCC 长度前缀直接切 NAL（**零拷贝**，不转 Annex-B 再扫起始码） |
| `src/network/socket.*` | 未改（R1 只走 TCP 交错，复用 `Session::send`） |

数据流：`MediaSource(GOP/队列/配额) → Subscriber → RtspSession → RtpPacketizer → Session::send`。

## 2. R1 已实现（视频 + TCP 交错）

| 能力 | 状态 |
|---|---|
| `OPTIONS` / `DESCRIBE` / `SETUP` / `PLAY` / `PAUSE` / `TEARDOWN` / `GET_PARAMETER` | ✅ |
| SDP：`c=` 行、`m=video 0 RTP/AVP 96`、`a=rtpmap`、`a=fmtp`（`packetization-mode=1` + `sprop-parameter-sets` + `profile-level-id`）、`a=control` | ✅ |
| `PLAY` 响应带 **`RTP-Info`**（`seq` + `rtptime` 取自首包真值） | ✅ |
| RTP 打包：单 NAL / **FU-A**（`ceil((nal−1)/1386)`，marker 只在帧末）+ **PTS×90** 时间戳（起始偏移 90000，避开 ts=0 回绕分支） | ✅ |
| RTCP **Sender Report**（每 ~30 包一次，走 SETUP 的 RTCP 通道） | ✅（A/V 同步与统计用；**不是**修首包时间戳） |
| UDP 传输 | ❌ **回 461**（语义：传输方式不支持；FFmpeg 会自动回退 TCP） |
| 音频轨 | ❌ R2（SDP 不宣告 `m=audio`，所以客户端不会等） |
| 名称校验 | ✅ 与 `/live/` 同一套规则 + 拒 `..`（防路径注入）；未注册 → 404 |

## 3. 实测（R1 验收）

```bash
./build/bin/mzmedia --port 33270 --media-root samples --loop --rtsp-out 33271
ffprobe -v error -rtsp_transport tcp rtsp://127.0.0.1:33271/live/sample
# → 0,h264,video,320,240
ffmpeg -v error -y -rtsp_transport tcp -i rtsp://127.0.0.1:33271/live/sample -t 5 -c copy -f flv /tmp/r1.flv
# → rc=0, 129 帧（--loop 下 5 秒 ≈ 125 帧）
```

单测（`rtspout` 组 **8 例**）：SDP 的 `c=`/sprop/profile-level-id、AVCC 零拷贝切 NAL、
RTP 单片头字段、**FU-A 分片数 + 反向重组逐字节相等**、AAC AU 头、请求解析（半包/版本错/路径穿越）、
`parseInterleaved` 的 TCP/UDP 分支。

## 3.1 R2 增量（音频 + 慢客户端 + 第二实现验收）

| 能力 | 状态 |
|---|---|
| **音频轨**：SDP `m=audio`（`MPEG4-GENERIC` + `aac-hbr` + `config=`） | ✅ `ffprobe` 认 `aac 44100/1` |
| 每轨独立 SETUP 通道（`trackID=0/1` → `0-1` / `2-3`） | ✅ |
| AAC 打包（RFC 3640 AU 头）+ **音频 ts = 采样数**（由 pts 推导，**不累加 1024**） | ✅ |
| `RTP-Info` 同时列出两条轨（`seq`/`rtptime` 各取首包真值） | ✅ 单测逐字节 |
| **慢客户端**：待发超 256 KB → **跳到下一个 IDR** 并计数（不无限缓冲） | ✅ 实现 + 计数；**未做压力注入验证**（R2 待补） |
| 第二独立实现验收：`gst-launch-1.0 rtspsrc ! rtph264depay ! fakesink` | ✅ 10 s 收到 274 个 buffer、无 ERROR |
| 音视频解码：`ffmpeg -rtsp_transport tcp -i …/sample -t 5 -f null -` | ✅ **零 error** |

### 1080p（新测试文件 `samples/sample1080.mp4`，1920×1080 / 5 s / **High profile + B 帧**）

`ffprobe` 认 `h264 1920x1080`；`ffmpeg -t 4 -f null -` **rc=0**（能解）。
但客户端会打 `non monotonically increasing dts`：**RTP 没有 dts 字段**，ffmpeg 把 RTP ts 同时当
pts/dts 用，而 B 帧的呈现序天然乱序（实测 pts = 0,160,80,40,120…）。
我们**不做单调化**（那会把呈现顺序压平，`ts_reordered` 曾涨到 124），只计数。

由此：`ffmpeg -c copy` 把一路 B 帧 RTSP **重新封装成 FLV 会失败**（`Invalid argument`），
这是客户端侧的约束，不是服务端时间戳错。**基线样本（无 B 帧）不受影响**。

## 3.2 R3：UDP 传输 + 端口池（已落地）

| 能力 | 实现 |
|---|---|
| `--rtsp-udp-pool-pairs N`（默认 0 = 只支持 TCP） | `output/udp_port_pool.{h,cpp}`：启动时**预绑定** N 对（RTP 偶数 / RTCP 紧随），运行期只租/还 |
| UDP SETUP | 解析 `client_port=A-B`，每**轨**租一对，回 `server_port=<rtp>-<rtcp>` + `ssrc` |
| UDP 发送 | `Socket::sendTo()`（新增，SOCK_DGRAM）直接 fire-and-forget；RTCP SR 发到 `client_port+1` |
| **状态码语义** | 未开 UDP → 池为空 → **461**（传输方式不支持）；池已满 → **503 + Retry-After**（资源临时不足）；缺 `client_port` → 400 |
| 端口归还 | 会话析构里归还（幂等）；池里的 socket **不关**，端口继续被池占着 |
| 观测 | `/api/stats` 的 `rtsp_out.udp_pool{size,used,exhausted}` + `counters.udp_sent` |
| fd 账 | `fd 预算 = 20 + max_connections + 2 × 池对数`（预绑定口径，不是"每会话 2 个"） |

**实测**：
- `ffmpeg -rtsp_transport udp -i rtsp://…/live/sample -t 4 -f null -` → **rc=0**；`udp_sent=304`、`audio_pkts=184`；
- 池 4 对（= 2 个会话）、**3 个并发 UDP 客户端**：2 个被拒（`exhausted=2`、HTTP 层 503 语义），1 个正常拉完（rc=0）——池上限生效，且不是 461 冒充。

## 3.3 R4：慢客户端实证 + 1 小时“自己拉自己”（已通过）

### 慢客户端（A 的 RTSP 输出，1080p/8 Mbps 源）

测试要点（踩过两个坑，记录备查）：① 用 `samples/sample1080.mp4` 不行——它是 **B 帧源**，
`ffmpeg -c copy` 直接失败（RTP 无 dts 字段）；② `kill -STOP` 必须停**ffmpeg 进程**，
停 `timeout` 壳子等于没停。改用生成的**非 B 帧 1080p/8 Mbps**（`/tmp/test1080.mp4`）后：

| 时刻 | sessions | RTP 包数 |
|---|---|---|
| 冻结前 | 2 | 7,978 |
| +5 s / +10 s | 2 | 15,206 / 22,714 |
| **+15 s** | **1**（冻结的观众被服务端断开：发送队列到 8 MB 上限） | 30,600 |
| +20…+30 s | 1 | 34,225 → **41,561**（另一个观众持续收流） |
| SIGCONT 之后 | 0 | 44,254（被断的那个已不在；它后报 h264 解码错误，冻结期错过 GOP，符合预期） |

`skipped_to_idr=1`（“跳到下一个 IDR”这条路径确实走了一次）。
**结论：完全不读的客户端约 15 s 后被断开，同期另一个观众不受影响。**

### 1 小时“自己拉自己”（A 的 RTSP 输出 → B 的 RTSP 输入 → 观众拉 B 的 FLV）

| 判据 | 实测 |
|---|---|
| 观众侧数据 | **3,674,930,382 字节（3.42 GiB）** |
| 解码帧数 | **89,988 帧** |
| 媒体时间轴 | **3599.7 s**（覆盖 99.99%） |
| **跨接缝 dts 倒退** | **0 次** |
| 解码错误 | **无** |
| A / B 错误日志 | **0 / 0 条** |
| B 重连计数 | **0**（链路一次没断） |
| 资源 | A RSS 36–39 MB / fd 23；B RSS 35.8 MB / fd 22（1 小时稳，无增长） |

> “跨接缝 dts 0 次倒退”说明：源在 `--loop` 下反复重来、经 A 的 RTSP 输出、再由 B 的 RTSP
> 输入接续，**整条链路时间轴始终单调** —— 输入侧的“接续时间轴”与输出侧“按 pts 原样发 +
> 会话基准”是配套成立的。

## 4. 已知现象（**不是本仓的 bug**）

ffmpeg 客户端拉我们的流时，`ffprobe` 会把**首个 AU** 报成 `0.24s`、随后的包才是 `0.04, 0.08 …`，
于是 `ffmpeg -c copy` 会打 `Non-monotonous DTS` 并丢掉开头约 0.24 s。
服务端实测发出的 RTP 时间戳是 `0, 3600, 7200, …`（单调、正确），所以这是
**FFmpeg 的 RTP/H264 首包时间戳行为**（v0.3 在输入侧用假服务端也复现过同一现象：
首包 pts 凭空多 0.2 s）。加 RTCP SR 后仍然存在，进一步说明它与 SR 无关。
**未验证**：VLC / GStreamer / NVR 是否有同样表现（R2 用真播放器验）。

## 5. 后续

| 批次 | 内容 |
|---|---|
| R2 | 音频轨（AAC-hbr）、多客户端专项、慢客户端策略（有界队列 + 跳 IDR/断开）、**真播放器（VLC）验收** |
| R3 | UDP：端口池（预绑定、偶数对）、`--rtsp-udp-pool-pairs`、503（资源不足）/461（不支持）/453（带宽）语义分离、`rtcp-mux`（仅按请求） |
| R4 | 1 小时长跑 + "自己拉自己"（mzmedia RTSP out → RTSP in）+ 文档收尾 |

## 6. 边界（面试要说清）

- **RTSP 客户端**（输入方向）仍是 FFmpeg 的；我们自己写的是**服务端**这一侧（RTP/RTCP/SDP/会话）。
- 传输只支持 **TCP 交错**；UDP 明确 461（不是"不支持还假装能"）。
- 首包时间戳现象见 §4，真播放器行为未验证。
