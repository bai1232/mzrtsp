# M10 设计：转码（v0.2.0 的第三块）

> 权威契约以 `src/ffmpeg/transcoder.h` 为准；本文件说明"为什么"。行为/形状变更同批同步（`AI_COLLAB.md` §3.8）。

## 1. 范围与验收

| 范围内（M10-a **已完成**） | 范围内（M10-b 待做） | 明确不做 |
|---|---|---|
| `Transcoder`：H264 解码 → `libswscale` 缩放/像素格式转换 → x264 编码（`veryfast` + `zerolatency` + 无 B 帧）→ 现场构造 `avcC` + 逐包转 AVCC | 接进 `DemuxerProducer`/`SourceManager`（一源一份转码输出、ThreadPool 承载、lazy/空闲释放）、`CodecMatrix` 的转码分支、`--transcode`/`--transcode-height`、`/api/stats` 的 `speed` | 音频转码、多音轨、多码率 ABR、GPU 转码（SPEC §9） |

**M10-a 验收**（`scripts/transcode_test.sh`，**12 项**全过）：

| 判据 | 实测（4 核 VM / g++ 11.4 / FFmpeg 4.4.2） |
|---|---|
| 分辨率 | 1920x1080 → **1280x720** |
| 编码 | `ffprobe` 认 **h264 / 25 fps / 4.96 s** |
| 帧数 | 源 125 帧 → **编码 125 包 / 解码 125 帧**（一帧不少） |
| 时间戳 | 输出 dts **严格单调**（脚本逐包验） |
| 内容 | `ffmpeg -f null -` **零 error** |
| **NFR-5 速度** | `speed = 媒体 4880 ms / 墙钟 1688 ms` = **2.89x**（**74 帧/s**），要求 ≥ **1.0x** |

## 2. 分层与位置

```
core → network → http → ffmpeg → media → output
                        ↑
                    Transcoder（M10-a 只依赖 ffmpeg/core）
```

`Transcoder` 只吃/吐 `AVCC 包 + 毫秒时间戳`，**不碰网络/线程/文件/音频** —— 于是"转得对不对"可以交给
`ffprobe` 独立裁判（`examples/transcode_demo` → `scripts/transcode_test.sh`），和 `FlvMuxer`/`MpegTsMuxer`
是同一条纪律。**音频在调用方原样透传**（SPEC §9 不做音频转码），所以"转码后音画同步"只取决于
两条时间轴是否共用同一个起点（见 §3.3）。

## 3. 契约

### 3.1 接口（`ffmpeg/transcoder.h`，M10-a ✅）

```cpp
struct Config { int out_width, out_height;      // 0/0 = 不缩放；只给一个 → 拒绝
                int out_bitrate_kbps;           // 0 = 按像素数×帧率推导（夹在 [500, 8000]）
                int gop_frames;                 // 关键帧间隔
                int max_out_packets; };         // 单次产出上限（0 非法）
enum class Result { Ok, BadConfig, UnsupportedCodec, NoDecoder, NoEncoder,
                    DecodeFailed, EncodeFailed, TooManyOutputs, BadStream };
Result transcode(const uint8_t *avcc, size_t size, int64_t dts_ms, int64_t pts_ms,
                 std::vector<OutPacket> *out);   // 0 个输出是正常的（编解码器有延迟）
Result flush(std::vector<OutPacket> *out);       // EOF 冲残余（**漏了会少最后几帧且不报错**）
const StreamInfo &videoInfo() const;             // H264 + 现场构造的 avcC + 目标宽高 + 同一个 index
```

### 3.2 三条硬约束

| 约束 | 做法 | 为什么 |
|---|---|---|
| 输出形状必须能被现有输出层吃下 | 编码器 `AV_CODEC_FLAG_GLOBAL_HEADER` → 取 extradata 的 SPS/PPS → `buildAvcC()` 构造 avcC；每包 `annexBToAvcc()` | `FlvMuxer` 只认 H264+avcC；这样"转码"对输出层完全透明（SC-2 的第二次验证） |
| 帧数一帧不能少 | 每个输入包后都 drain 解码器+编码器到 `EAGAIN`；EOF 时 `flush()` 再冲一遍 | 漏掉任一段 = 少最后几帧，**不报错**（表现是"时长偏短"） |
| 有界 | `max_out_packets`（超出 → `TooManyOutputs`）；0 不是无界 | 解码器可能一进多出；异常输入不能把内存拉爆 |

### 3.3 时间轴：**自己生成**，不用输入的 pts（这条是实测踩出来的）

`Demuxer` 的单调守卫会**钳制乱序的 pts**。对有 B 帧的源，pts 本来就是乱序的（实测 `sample1080.mp4`
的前几包 pts = 0, 160, 80, 40, 120 ms），于是被钳成 **0, 160, 160, 160, 160**。把这样的 pts 喂给
编码器 → 输入的 pts 不递增 → **编码器输出的 dts 乱序**（`ffmpeg -f null -` 报
`non monotonically increasing dts to muxer`，FLV 里看到 tag 时间戳 0, 160, 80, 120 跳来跳去）。

所以 `Transcoder`：

- 送给解码器时**只设 dts**（`pkt->pts = AV_NOPTS_VALUE`），显示顺序交给解码器从码流里还原；
- 输出时间轴 = `第一个输入包的 dts（锚点）+ 帧序号 × 标称帧步长`（步长 = `1000 × den / num`）；
- 锚点用源起点 ⇒ **透传的音频与转码视频共用同一条时间轴**（A/V 同步不漂）；
- 若相邻输入 dts 间距偏离标称步长 >10% → **Warn 一次**（VFR 源会被按 CFR 规整，属**可见的降级**，
  不静默；`vfrDetected()` 可查）。

> 【待办/建议】`Demuxer` 对 **pts** 的单调钳制本身值得单独修（它会把 B 帧源的 CompositionTime
> 弄错；v0.1 的 FLV remux 对 B 帧源其实也是错的）。但那会改 `ffmpeg/` 层的既有行为与用例，
> 需要单独立项 + 补"B 帧源 FLV 的 composition time"用例。本批**只**在转码器侧绕开它。

### 3.4 编码参数（直播优先）

| 参数 | 值 | 理由 |
|---|---|---|
| `preset` | `veryfast` | NFR-5 的口径写的就是 veryfast；4 核 VM 实测 74 fps |
| `tune` | `zerolatency` | 关掉 lookahead / B 帧，延迟与内存都小 |
| `max_b_frames` | **0** | dts 恒等于 pts：下游的时间戳守卫、循环拼接、切片边界都简单 |
| `pix_fmt` | `yuv420p` | 浏览器/MSE 的通用格式 |
| `gop_size` | `Config::gop_frames`（默认 50 ≈ 2s@25fps） | 与 HLS 切片目标（2s）对齐 |
| 码率 | 0 → 按 `像素数 × 帧率 × 0.10 bit` 推导，夹在 [500k, 8000k] | 有界；不传就"等实测"是不允许的 |

## 4. 分批

| 批次 | 内容 | 验收 |
|---|---|---|
| **M10-a** ✅ | `Transcoder` + `examples/transcode_demo` + `scripts/transcode_test.sh` + 1080p 样本（`make_samples.sh`）+ `transcode` 组 3 例 | `transcode_test.sh` **12/12**；NFR-5 **2.89x** |
| **M10-b** ⏳ | 接进 `DemuxerProducer`（视频走解码→编码，音频透传）→ `SourceManager`（一源一份、lazy/空闲释放）→ `ThreadPool` 限流；`CodecMatrix` 的转码分支；`--transcode`/`--transcode-height`；`/api/stats` 的 `speed` | 待定：端到端 `ffplay` 能播转码后的 FLV；`speed` 出现在 stats；并发转码上限生效 |

## 5. 测试计划

| 分组 | 位置 | 说明 |
|---|---|---|
| `transcode` ✅ | `tests/test_transcoder.cpp`（3 例） | 拒绝路径（非 H264 / 缺 avcC / 只给宽 / 0 上限 / 0 GOP）、真样本 320x240→160x120 全量转码（AVCC 结构、帧数不丢、dts 单调、`scaling()` 懒创建、flush 幂等）、空包拒绝。**严格组**（无线程无等待） |
| 脚本 ✅ | `scripts/transcode_test.sh`（12 项） | 1080p→720p 的 NFR-5 speed + ffprobe + `ffmpeg -f null -` + 逐包 dts 单调 + "不缩放"路径 |

> 注意：分组是**用例名子串**匹配，所以 `transcode` 组还会捎上 `ffmpeg` 组里那条
> `ffmpeg_codec_matrix_unsupported_and_transcode`（`--list` 可见）。这是既有性质，不是 bug。

## 6. 风险清单

| # | 风险 | 触发条件 | 现状 |
|---|---|---|---|
| 1 | **VFR 源被按 CFR 规整** | 帧间距不均匀的源 | 会 Warn 一次（`vfrDetected()`），但**没有 VFR 样本实测** |
| 2 | 源的 pts 被单调守卫钳过 | 任何有 B 帧的源 | 转码器已绕开（自己生成时间轴）；但 **remux 路径的 B 帧 CompositionTime 仍然是错的**（另案） |
| 3 | 音频/视频时间轴起点不一致 | 源首个音频包 dts ≠ 首个视频包 dts | 视频锚定"首个视频包 dts"，音频透传原始 dts → 两者都相对源起点，实测 1080p 无音频样本；**带音频的转码时序未实测**（M10-b 要验） |
| 4 | 单帧超大（8MB 上限由 Demuxer 保证） | 畸形流 | 超限在 Demuxer 就已拒绝；转码器另设 `max_out_packets` |
| 5 | 编码器缺 libx264 | 换机器/裁剪版 FFmpeg | `create()` 返回 nullptr + 明确日志（不静默降级） |
| 6 | 4 核机上并发转码 | M10-b 的多源场景 | 单路 2.89x 余量 ≈ 2.9 路/核；M10-b 必须限并发（初值 `nproc-1`） |

## 7. 决策记录

| 决策 | 选择 | 排除的选项与原因 |
|---|---|---|
| 输入/输出形状 | 都是 **AVCC + 毫秒** | 排除"Annex-B 进、AVCC 出"（调用方还得转一次）；排除"用流的原始时间基"（全链路都是 ms） |
| 音频 | **不做**（调用方透传） | SPEC §9 明确不做音频转码；透传天然无损、零 CPU |
| 时间轴 | **自己生成**（锚点 + 帧序号 × 步长） | 排除"用输入 pts"（被单调守卫钳过，实测导致 dts 乱序）；排除"让编码器按 dts 推 pts"（B 帧源会错） |
| B 帧 | 不用 | 排除"开 B 帧省码率"：直播优先，dts==pts 让下游全部简单；代价是同码率画质略低 |
| avcC 来源 | 编码器的 extradata（Annex-B）→ `buildAvcC()` | 排除"用源的 avcC"（分辨率变了，SPS 里的宽高就错了） |
| 失败语义 | 每条路径都有具体 `Result` + 日志 | 排除"返回 false 无原因"（排查成本最高） |
