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
