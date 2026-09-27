# 输入 × 输出可行性矩阵

> 本文定义"**按需转封装或转码**"的**判定规则**。实现必须严格遵循本表；遇到表中未覆盖的组合，**拒绝并返回明确错误**，不得静默降级。

## 1. 判定流程

```
输入流已 demux 出视频/音频编码
        │
        ├─(1) 目标封装是否支持该编码？ ── 否 ──► 需要转码 / 拒绝
        │
        ├─(2) 目标播放端是否支持该编码？ ─ 否 ──► 需要转码 / 拒绝
        │
        └─(3) 编码参数是否可直接沿用（SPS/PPS、AudioSpecificConfig、
               时间基、比特流格式 Annex-B vs AVCC）？
                   │
                   ├─ 是 ──► REMUX（零拷贝，仅重写容器与时间戳）
                   └─ 否 ──► 需要**轻量转换**（如 ADTS→raw AAC、Annex-B→AVCC），
                             仍属 remux 范畴，不解码
```

**三档处理**，实现时必须能区分并记录日志：

| 档位 | 动作 | 代价 |
|---|---|---|
| **REMUX** | 直接改写容器，`AVPacket` 原样搬运 | 极低（NFR-4：单路 <5% 单核） |
| **REPACK** | 比特流格式/头部转换，仍不解码 | 低 |
| **TRANSCODE** | 解码 → 编码（可改分辨率/码率） | 高（v0.2 才有） |

## 2. 输出容器支持的编码

| 输出 | 视频 | 音频 | 播放端 |
|---|---|---|---|
| **HTTP-FLV** | H264（必须 AVCC 格式 + `AVCDecoderConfigurationRecord`） | AAC（raw + `AudioSpecificConfig`）、MP3 | 浏览器（flv.js / MSE）**仅 H264 + AAC** |
| **HLS (mpegts)**（v0.2） | H264 | AAC（ADTS）、AC3 | 浏览器原生 / hls.js |

> **结论**：HTTP-FLV 与 HLS 的可播编码集**都是 H264 + AAC**。因此 H265/HEVC 一律落在"需要转码或拒绝"分支——这也是 v0.1–v0.3 明确不做 H265 的直接原因。

## 3. 输入 → HTTP-FLV（v0.1 目标）

| # | 输入 | 视频 | 音频 | 判定 | 动作说明 |
|---|---|---|---|---|---|
| 1 | MP4 | H264 (AVCC) | AAC | ✅ **REMUX** | 提取 `extradata`（SPS/PPS）写 FLV `AVCDecoderConfigurationRecord`；AAC 写 `AAC sequence header` |
| 2 | MP4 | H264 | 无 | ✅ **REMUX** | 纯视频 FLV，仅需 AVC sequence header |
| 3 | MP4 | H264 | MP3 | ✅ **REMUX** | FLV 支持 MP3 |
| 4 | H264 裸流 | H264 (Annex-B) | 无 | ✅ **REPACK** | 需从流中提取 SPS/PPS，并**把 Annex-B 起始码转成 AVCC 长度前缀** |
| 5 | MP4 | **HEVC** | AAC | ❌ 不支持 | 浏览器播不了 → 转码为 H264（v0.2）或**返回 415** |
| 6 | MP4 | MPEG-4 Part 2 | AAC | ❌ 不支持 | FLV 不支持 → 转码或拒绝 |
| 7 | 任意 | — | 仅音频 | ✅ 可播 | FLV 纯音频流；须在 SDP/响应中正确标识 |

## 4. 输入 → HLS（v0.2）

| # | 输入 | 判定 | 说明 |
|---|---|---|---|
| 1 | MP4 (H264+AAC) | ✅ **REMUX** | 音视频分别转 ADTS 与 Annex-B 后写入 TS |
| 2 | MP4 (H264+AAC) 已有 ADTS | ✅ **REMUX** | 直接进 TS |
| 3 | H264 裸流 | ✅ **REPACK** | 补 SPS/PPS、每帧打 90kHz PTS/DTS |
| 4 | 任意 HEVC | ❌ 不支持 | 与 FLV 同理（且本项目不做 H265） |

## 5. 需要特别注意的转换点（易踩坑）

| 坑 | 说明 | 归属 |
|---|---|---|
| **Annex-B ↔ AVCC** | FLV/MP4 用 4 字节长度前缀（AVCC），裸流/TS 用 `00 00 01` 起始码。转换必须遍历每个 NAL | 输入 H264 裸流 / 输出 FLV |
| **SPS/PPS 提取** | 裸流里 SPS/PPS 可能只在开头出现一次，也可能周期性重复；FLV 要求它们在自定义头里 | REPACK |
| **AAC 封装** | MP4/MKV 里是 raw AAC（配 `AudioSpecificConfig`）；TS 里是 ADTS（带 7 字节头）。**必须按目标容器转换** | 双向 |
| **时间基换算** | MP4 `time_base=1/90000` 或 `1/1000`，FLV 是**毫秒**，TS 是 **90kHz**。换算必须用整数有理运算避免漂移 | 所有输出 |
| **DTS vs PTS** | 有 B 帧时 `DTS ≤ PTS`；FLV 只存 DTS + Composition Time 偏移，算错会导致画面顺序错乱 | 输出 FLV |
| **时间戳回绕** | 32 位时间戳约 26.5 小时回绕（FLV 与 RTP 均如此），长时间运行须处理 | NFR-3 |
| **`extradata` 缺失** | MP4 的 `extradata` 一定存在；裸流没有，必须自行拼装 | REPACK |
| **非标准采样率/声道** | AAC 的采样率索引表外的情况（如 7350Hz）需要特判 | 输出 FLV |

## 6. 拒绝策略（不支持时怎么表现）

| 场景 | 行为 |
|---|---|
| 编码不受支持且转码关闭 | HTTP **415 Unsupported Media Type**，响应体给出明确原因（含编码名），日志 `Warn` |
| 容器无法识别 | 启动/请求时 **400**，日志说明探测到的容器 |
| 文件中无音视频轨 | 启动失败并输出明确错误（仅音频轨允许，纯无轨拒绝） |
| 转码过程中失败 | 关闭该源的所有连接，返回已发送字节与错误原因，日志 `Error` |

## 7. 待补全

- v0.2 补齐 MKV（`matroska`）的编码组合实测结果
- v0.3 补齐 RTSP 输入的编码探测（远端 SDP 与实际码流可能不一致）
- 每个组合都需要一条 `ffprobe` 校验用例，写入 `TESTING.md`
