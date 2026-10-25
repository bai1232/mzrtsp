# M10 设计：转码（v0.2.0 的第三块）

> 权威契约以 `src/ffmpeg/transcoder.h` 为准；本文件说明"为什么"。行为/形状变更同批同步（`AI_COLLAB.md` §3.8）。

## 1. 范围与验收

| 范围内（M10-a **已完成**） | 范围内（M10-b **已完成**） | 明确不做 |
|---|---|---|
| `Transcoder`：H264/HEVC 解码 → `libswscale` 缩放/像素格式转换 → x264 编码（`veryfast` + `zerolatency` + 无 B 帧）→ 现场构造 `avcC` + 逐包转 AVCC | 接进 `DemuxerProducer`（视频转、音频透传）与 `SourceManager`（转码源 = 独立源键、并发硬上限、播完即让出名额）、`CodecMatrix` 的能力感知分支、`--transcode`/`--transcode-height`/`--transcode-max`、`?transcode=1&h=`、`/api/stats` 的 `transcode.speed` | 音频转码、多音轨、多码率 ABR、GPU 转码、**转码 + HLS**（M10-c） |

**M10-a 验收**（`scripts/transcode_test.sh`，**12 项**全过）：

| 判据 | 实测（4 核 VM / g++ 11.4 / FFmpeg 4.4.2） |
|---|---|
| 分辨率 | 1920x1080 → **1280x720** |
| 编码 | `ffprobe` 认 **h264 / 25 fps / 4.96 s** |
| 帧数 | 源 125 帧 → **编码 125 包 / 解码 125 帧**（一帧不少） |
| 时间戳 | 输出 dts **严格单调**（脚本逐包验） |
| 内容 | `ffmpeg -f null -` **零 error** |
| **NFR-5 速度** | `speed = 媒体 4880 ms / 墙钟 1688 ms` = **2.89x**（**74 帧/s**），要求 ≥ **1.0x** |

**M10-b 验收**（`scripts/flv_http_test.sh` §10/§11 **28 项** + `producer`/`srcmgr`/`ffmpeg` 三组单测）：

| 判据 | 实测（4 核 VM） |
|---|---|
| 开关语义 | 未开 `--transcode` 时 `?transcode=1` → **501**（**不降级成直通**）；开了才转 |
| 参数校验 | `h` 非数字/奇数/越界、`transcode` 非法值、未知参数、重复参数 → **全部 400** |
| 缩放 | 320x240 → `?transcode=1`（默认上限 120）= **160x120**；`?transcode=1&h=120` 同 |
| 音频 | 原样透传：音频包数 **88 → 88**，`ffprobe` 认 **aac** |
| 帧数 | 50 帧 → **50 帧**（一帧不少；EOF flush 生效） |
| HEVC → H264 | 不转码 → **415**；`?transcode=1` → **h264 160x120**，`ffmpeg -f null -` 零 error |
| 并发上限 | `--transcode-max 1`：第一路在转时第二路 → **503**，`transcode_rejected=1` |
| **speed** | 1080p→720p 服务内实测 **3.26x**（媒体 5000ms / 转码耗时 1533ms）；320x240→160x120 **26–31x** |
| 循环 + 转码（§8.5） | `--loop --speed 4` 拉 4 秒：视频末包 **15.923s** vs 音频末包 **15.950s**（差 ≈ 一帧）；回归用例锁 300ms 界（变异后 ~2000ms 变红） |

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
| **M10-b** ✅ | 接进 `DemuxerProducer`（视频走解码→编码，音频透传）→ `SourceManager`（转码源 = 独立源键 / 并发硬上限 / 播完让出名额）；`CodecMatrix` 的能力感知分支（HEVC → H264）；`--transcode`/`--transcode-height`/`--transcode-max`；`?transcode=1&h=`；`/api/stats` 的 `transcode.speed` | `flv_http_test.sh` **96/96**（+28 项）；`producer` 14 例 / `srcmgr` 15 例 / `ffmpeg` 36 例 |
| **M10-c** ⏳ | 转码 + HLS（转码输出也进 `HlsService`，`/hls/x-720p.m3u8`） | 待定 |

## 5. 测试计划

| 分组 | 位置 | 说明 |
|---|---|---|
| `transcode` ✅ | `tests/test_transcoder.cpp`（3 例） | 拒绝路径（**不认识的编码 VP9** / HEVC 缺初始化数据 / 缺 avcC / 只给宽 / 0 上限 / 0 GOP）、真样本 320x240→160x120 全量转码（AVCC 结构、帧数不丢、dts 单调、`scaling()` 懒创建、flush 幂等）、空包拒绝 |
| `producer` ✅（M10-b +4 例） | `tests/test_demuxer_producer.cpp` | `producer_transcode_downscales_and_rewrites_avcc`（avcC 与直通源**不同**、50→50 帧、音频 88 包透传、dts 严格递增、`cost_ms>0`）、`producer_transcode_height_is_only_a_cap_by_default`（上限比源高 → **不缩放**，不报错）、`producer_transcode_rejects_bad_requests`（奇数/放大/只有音频/关掉转码照常）、`producer_transcode_hevc_becomes_h264`（不转码时 codec 是 HEVC、转码后 h264 160x120、进出包数相等；缺样本时**明确打印跳过**）、`producer_transcode_loop_keeps_audio_and_video_aligned`（`--loop` 跨接缝后两条时间轴仍贴在一起，界 300ms） |
| `srcmgr` ✅（M10-b +3 例） | `tests/test_source_manager.cpp` | 独立源键 / 同规格复用 / 转码与直通的 `MuxerStreams` 不同、并发上限（第二路被拒且**不降级**、`totalTranscodeRejected`↑、放掉后名额回来）、`max_transcoding` 的 0 与超硬上限被 `setConfig` 拒且原配置不变 |
| `ffmpeg` ✅（M10-b +1 例） | `tests/test_ffmpeg.cpp` | `decideOutput(..., TranscodeCaps)`：缺能力 → Unsupported、两者齐 → Transcode、H264 源仍是 Remux、`detectTranscodeCaps()` 与真实 avcodec 一致 |
| 脚本 ✅ | `scripts/transcode_test.sh`（12 项） | 1080p→720p 的 NFR-5 speed + ffprobe + `ffmpeg -f null -` + 逐包 dts 单调 + "不缩放"路径 |
| 脚本 ✅（M10-b） | `scripts/flv_http_test.sh` §10/§11（**28 项**） | 开关语义（501）、参数校验（7 种 400）、缩放、帧数、音频透传、dts 单调、HEVC（415 → h264）、并发上限（503）、`speed ≥ 1.0`、stats 字段 |
| 样本 ✅ | `scripts/make_samples.sh` | `sample_hevc.mp4`（libx265 现场生成，缺库时明确跳过）、`sample_audio.aac`（"只有音频不许转码"） |

> 注意：分组是**用例名子串**匹配，所以 `transcode` 组现在是"**一切与转码有关的用例**"：
> `transcode_*`（3）+ `producer_transcode_*`（4）+ `srcmgr_transcode_*`（3）+
> `ffmpeg_codec_matrix_*transcode*`（2）= **12 例**（`--list` 可见）。这是刻意保留的既有性质：
> 报"转码组红/绿"就能一次覆盖契约、接线、并发与判定四处。

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

## 8. M10-b：把转码接进服务（已完成）

### 8.1 一句话形状

> **转码是一路"独立源"**：同一个文件 × 同一个转码规格 = 一个 `MediaSource`；
> 视频在 `DemuxerProducer::read()` 里过 `Transcoder`，音频原样透传；
> 输出层（`FlvSender`/`FlvMuxer`）看到的永远是"一路 H264 + 一路 AAC"。

```
URL: /live/x.mp4.flv?transcode=1&h=720
      └─ SourceManager: key = "samples/x.mp4|h=720"
            └─ DemuxerProducer{transcode{enabled, out_height}}
                  ├─ 视频包 → Transcoder（解码→sws→x264）→ _pending → read() 一次交一个
                  └─ 音频包 → 原样（SPEC §9：不做音频转码）
```

### 8.2 为什么是"独立源"，不是"同一源的第二条输出"

| 方案 | 代价 | 结论 |
|---|---|---|
| A. 独立源（各自解封装，本批选的） | 同一个文件被 demux 两次；转码那份多占 CPU | ✅ 守 `SC-2`：**`media/` 的分发层（`MediaSource`/`Subscriber`/`FrameQueue`）一行没改** |
| B. 同一 `MediaSource` 挂两条输出 | 省一次 demux | ❌ 要改分发层的扇出模型 —— 那是"加能力改核心"，SC-2 明令禁止 |

代价是真实存在的，所以 `MAX_TRANSCODING` 是**硬上限**（默认 `nproc-1`，`0` 非法）。

### 8.3 URL 与开关语义（**每一条都拒绝静默**）

| 请求 | 响应 | 为什么这样 |
|---|---|---|
| `/live/x.flv` | 200，原始码流 | 默认行为，与 v0.1 完全一致 |
| `/live/x.flv?transcode=1` | 200，`--transcode-height` 当**上限**（源更高才缩） | 意图是"给我一路能播的流"；240p 的源不该被"默认 720p"判成放大 |
| `/live/x.flv?transcode=1&h=480` | 200，缩到 480p | 显式高度：必须偶数、≤2160、不高过源 |
| `?h=480`（没有 transcode=1） | **400** | 多半是漏了 `transcode=1`；不猜 |
| `?transcode=abc` / 未知参数 / 重复参数 | **400** | 拼错的参数静默忽略是最难查的一类问题 |
| `?transcode=1` 但服务端没开 `--transcode` | **501** | **绝不降级成直通**：那会让用户以为"转过了" |
| 转码并发已满 | **503** | 同样不降级；`lastError()` 说明"正在转 N/M 路" |
| `/hls/x.m3u8?transcode=1` | **400** | 转码 + HLS 属 M10-c；不"看起来生效了" |

### 8.4 一次输入 → 0..N 个输出（`read()` 的形状）

`SourcePump` 的契约是"一次一个包"，而解码器会攒帧、编码器有延迟：

- 一次 `transcode()` 可能吐 **0** 个包 → `read()` **继续读下一个输入包**（不是返回空包，也不是 EOF）；
- 可能吐 **N** 个包 → 先进 `_pending`（**有界**：一次最多 `max_out_packets`），再一个一个交出去；
- **EOF 必须 `flush()`**：不冲残余就是"少最后几帧且不报错"（M10-a 已验证：50 → 50）；
- **循环模式下不 flush**：那只是"这一趟结束"，解码器还要接着解下一趟。

### 8.5 时间轴：转码器自己连续，接缝只补差额

`Transcoder` 自己生成 CFR 时间轴（§3.3 说明为什么不能用源 pts），这条轴**跨趟自动连续**
（"锚点 + 帧序号 × 步长"，帧序号一直在涨）。所以：

- 转码视频**不能**再叠 `_loop_offset_ms`（那是给透传路径用的）：叠了就双计。
  **实测踩到**：`--loop` 下视频跑到 **14.0s**、音频才 **8.1s**，音画彻底错位；
- 但两条轴也不能完全各走各的：音频的趟偏移是 `pass_end`（真实媒体时长），
  视频的是 `帧数 × 步长`，两者可能差几毫秒~几十毫秒（音频 1024 样本/帧 ≈ 23.22ms 这种小数帧长会累积）。
  所以接缝处补一个差额：`_tx_ts_shift += pass_end − 这一趟转出的媒体时长`；
- 实测（`--loop --speed 4` 拉 4 秒）：视频末包 **15.923s** vs 音频末包 **15.950s**（差 ≈ 一帧）；
- 回归用例：`producer_transcode_loop_keeps_audio_and_video_aligned`（界 300ms —— 正常 ≈80ms，
  双计的 bug 会让 gap 直接跳到 ~2000ms）。

媒体时长按 `帧数 × 标称步长` 算 —— 用"输出 dts 的跨度"会被接缝的跳变虚高（实测 4.96s 被算成 9.76s，
speed 跟着虚高）。

### 8.6 `speed` 的定义（NFR-5 / FR-6.1）

```
speed = out_media_ms / cost_ms
  out_media_ms = 已转出的帧数 × 标称帧步长
  cost_ms      = 累计花在 transcode()/flush() **里面**的墙钟（不含解封装/节流/网络）
```

**为什么不用"从开始到结束的墙钟"**：源按墙钟推流（`Throttle` 默认 1 倍速），那样算出来永远是
≈1.0 —— 量到的是**节流**的速度，不是转码的速度（实测踩到：0.97x）。

`elapsed_ms` 也一并暴露（含等待），但 NFR-5 只看 `speed`。源播完/转完后 `elapsed_ms` **冻结**
（否则会一路涨，看起来像"越转越慢"）。

### 8.7 并发上限与"播完让出名额"

- 上限在 `SourceManager::acquire()` 的**锁内**判定（否则两个请求同时通过检查就超限了）；
- `0` 是**非法**（不是"不限"）：转码吃满 CPU 会让整台机器不可用；
- **EOS 的源不再占名额**：播完的源不吃 CPU。这条是实测踩出来的 —— 一开始只数"配置了转码的条目"，
  于是 2 秒样本播完后仍被判成"正在转"，后面的转码请求全部 503。

### 8.8 HEVC → H264（`CodecMatrix` 的能力感知分支）

`decideOutput(video, audio, output)` 的**行为完全不变**（等价于"没有任何转码能力"），
新增 `decideOutput(..., const TranscodeCaps&)`：

| 能力 | HEVC → FLV |
|---|---|
| 缺 HEVC 解码器 或 缺 H264 编码器 | `Unsupported` → 415（与 v0.1 一致） |
| 两者都有 | `Transcode` → 200（本机实测：h264 160x120，`ffmpeg` 零 error） |

"**不转码**时 HEVC 仍 415" 是刻意保留的：`SPEC` 冻结的是"HEVC 直通"，不是"HEVC 永远进不了 FLV"。
判定的唯一权威仍是 `CodecMatrix`（`DemuxerProducer::openTranscoder()` 调它，不自己另写一套）。

### 8.9 `/api/stats` 的形状

```json
"source_manager": { "max_transcoding": 3, "transcoding": 1, "transcode_rejected": 0, ... },
"media_sources": [ { "path": "samples/x.mp4", "key": "samples/x.mp4|h=720", ...,
    "producer": { ..., "transcode": { "enabled": true, "in_hevc": false,
      "out_width": 1280, "out_height": 720, "scaling": true, "vfr": false,
      "inputs": 125, "outputs": 125, "out_media_ms": 5000,
      "cost_ms": 1533, "elapsed_ms": 4978, "speed": 3.26 } } } ]
```

### 8.10 本批的已知限制

| # | 限制 | 现状 |
|---|---|---|
| 1 | 转码 + HLS | **未做**（M10-c）；`/hls/` 带转码参数明确 400 |
| 2 | 音频转码 | **不做**（SPEC §9）；音频不是 AAC 的源 → 转码请求**明确拒绝** |
| 3 | HEVC 的 Annex-B 裸流（TS） | 需要现场构造 hvcC（与 avcC 对称的另一套格式）→ **明确拒绝**，无实测样本前不做 |
| 4 | 转码跑在**源线程**上（不是 ThreadPool） | 单路 3.26x 余量够用；"一源一份 + 并发硬上限"已保证不会把机器打满。改成线程池属性能优化，不是行为缺口 |
| 5 | libx264 的 banner 打到 stderr | 无 per-context 日志选项（`-h encoder=libx264` 里没有 log_level）；纯噪声，不影响功能 |
| 6 | `Demuxer` 对 pts 的钳制 | 仍会弄错 B 帧源的 CompositionTime（remux 路径）；本批只在转码器侧绕开 |
| 7 | 循环 + 转码的长期漂移 | 每趟按 `pass_end − 帧数×步长` 补差额，单趟误差 < 1 帧；**没有**小时级长跑实测（`--loop` 的真实场景） |
| 8 | `?transcode=1` 的源键带 `auto=<高度>` | 同一文件用不同 `--transcode-height` 起停会得到**不同**源键（旧源的键会留在 stats 里直到空闲释放）。这是刻意的：不同目标高度确实是不同的输出 |
