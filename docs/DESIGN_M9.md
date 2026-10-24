# M9 设计：MPEG-TS 封装 + HLS 输出（v0.2.0 第二块）

> 权威契约以 `src/output/mpegts_muxer.h`、`src/output/hls_writer.h`（M9-b 落地）为准；
> 本文件说明"为什么"。行为/形状变更同批同步（`AI_COLLAB.md` §3.8）。

## 1. 范围与验收

| 范围内（M9-a **已完成**） | 范围内（M9-b 待做） | 明确不做 |
|---|---|---|
| `MpegTsMuxer`：PAT/PMT、PES、188 字节包、PCR、90kHz 时间戳、关键帧补 SPS/PPS、raw AAC → ADTS | `HlsWriter`：按关键帧切 2s、滑动窗口 6 片、`.m3u8`、内存 ring、租约式空闲释放 | 点播 seek / 录像（SPEC §9）；多码率 ABR；fMP4；DRM |

**M9-a 验收**（`ROADMAP.md` 的 v0.2 行）：`scripts/ts_mux_test.sh` —— 三种输入形状（MP4 / TS / 纯视频 TS）
各出一份 TS，**23 项**全过：188 对齐 + 逐包同步字、`ffprobe` 认 h264 320x240 + aac 44100、
视频帧数 50、`ffmpeg -f null -` **零 error**、含 PAT(0x0000) 与 PMT(0x1000)。

## 2. 分层

```
core → network → http → ffmpeg → media → output
```

`output/mpegts_muxer` 只依赖 `ffmpeg`（StreamInfo / aac_util / h264_util）与 `media`（MediaPacket），
**不认识 Session / HTTP** —— 与 `FlvMuxer` 同一条纪律（`ARCHITECTURE.md` §1）。
验证方式一样：`grep -rn "network/\|http/" src/output/` 必须为空。

## 3. 契约

### 3.1 `MpegTsMuxer`（`output/mpegts_muxer.h`，M9-a ✅）

与 `FlvMuxer` **同一个形状**：给它包，它还你字节。

```cpp
struct Streams { const StreamInfo *video; const StreamInfo *audio; };   // 来自 DemuxerProducer 的快照
Result prepare();                        // 校验编码 + 解析 avcC/SPS/PPS + ASC
Result writePatPmt(std::string *out);    // **每个切片开头都要调一次**
Result writePacket(const MediaPacket &, std::string *out);
// 观测：tsPackets/pesPackets/bytesWritten/spsPpsInjected/pcrCount/lastDts90k
```

| 硬约束 | 做法 | 为什么 |
|---|---|---|
| 切片必须能独立解码 | `writePatPmt()` 把 PSI 写进每个切片；**关键帧前补 SPS/PPS**（从 avcC 取） | MP4 的 H264 把 SPS/PPS 放在 avcC 里，**码流包里没有**；不补的话"单拉一片 = 一片黑" |
| 音频要 ADTS | `rawAacToAdts()`：用 ASC 的 AOT/sfi/cc 造 7 字节头 + payload | TS 的 AAC 是 ADTS（自描述）；`MediaPacket` 里是 raw AAC（FLV 要） |
| 时间基 90kHz | `ms × 90`，33 bit 自然回绕；`pts != dts` 时两个都写 | FR-3.4 写明"TS → 90kHz"；有 B 帧时不写 DTS 会解码乱序 |
| PES 长度 | 视频写 0（规范只允许视频这样）；音频写精确值且 ≤ 65535 | 视频关键帧可能远大于 65535；音频超限就**明确失败**（不截断） |
| 包长恒为 188 | 不满的 payload 用 **adaptation field** 填充（不能补 0xFF 到负载里） | TS 的负载必须被 PES 数据填满；填充只能在 AF 里 |
| PCR | 关键帧的首个 TS 包携带（`PCR_PID = 视频 PID`） | 播放器靠 PCR 建时钟；间隔由 `pcrCount()`/`lastDts90k()` 可观测 |
| 拒绝而不是猜 | 非 H264 / 缺 avcC / 缺 ASC / 外来包 / 空包 → 具体 `Result` + 日志 | §4 的"不静默降级"；调用方据此回 415/500 |

### 3.2 `HlsWriter` + `HlsService`（M9-b ✅）

**形状先定死**（`AI_COLLAB.md` §3.3 的纪律），实现随后：

```cpp
class HlsWriter {                       // 一源一份，所有客户端共享切片
public:
    struct Config {
        uint32_t target_duration_ms = 2000;   // 目标 2s（FR-3.2）
        size_t   window = 6;                  // 滑动窗口 6 片（FR-3.2）
        uint32_t max_slice_ms = 10000;        // 单片硬顶（超出 → 计数 + Warn，不静默产出畸形 playlist）
        size_t   max_slice_bytes = ...;       // 单片上限（有界性不可协商）
        size_t   max_total_bytes = ...;       // 总量上限 = window × max_slice_bytes
        uint32_t lease_ms = 60000;            // 租约：任意 /hls/ 请求续租（FR-1.3 的 60s）
    };
    // 由源线程喂包；切片落内存 ring；m3u8 文本按需生成
    // HTTP 侧只读：`/hls/<name>.m3u8`、`/hls/<name>-<seq>.ts`
};
```

**落地形态**：`HlsWriter`（纯切片逻辑，`onPacket()` 同步、可手造包单测）+ `HlsService`
（名字→(源, 切片器) 注册表 + 租约定时器 + 两个只读入口 `playlist()/slice()`）。
HTTP 侧只读切片，复用 `serveMemoryAsStatic()`（= HttpServer 里那套已测的单区间 Range 逻辑）。

关键决策（已定并已实现）：

| 决策 | 选择 | 排除的选项与原因 |
|---|---|---|
| 切片边界 | **切在关键帧**（目标 2s，允许随 GOP 浮动） | 排除"到点就切"：从非关键帧开始的切片播放端解不了（静默花屏） |
| `#EXT-X-TARGETDURATION` | `ceil(实测最大切片时长)` | 排除"固定 2"：GOP > 2s 的源会违约（播放器行为未定义） |
| 生命周期 | **租约**（任意 `/hls/` 请求续租 60s；到期且无订阅者 → 停源） | 排除"常驻到进程退出"（违背 FR-1.3）；排除"每个 m3u8 请求方各自切片"（N 倍 CPU） |
| 切片存放 | **内存 ring（window 片）+ 单片/总量双上限** | 排除落盘（多一套清理与磁盘上限，NFR-6 的口径变脏） |
| 探活 | m3u8 里 `#EXT-X-ENDLIST` 只在源 EOS 时出现 | 文件输入 + `--loop` 下窗口永不满，播放器当直播一直播 |
| 切点 | **只认视频关键帧**（`is_video && packet.isKeyFrame()`） | 排除「用 `packet.isKeyFrame()` 直接判」：AAC 音频包的 key 标志**恒为 true** → 会切在音频包上（实测：切片时长变成音频的 2199ms、多出 40ms 尾巴片、跨片时间戳回退） |
| 时间轴起点 | 每个 TS 切片的时间戳 = **90000**（1s）+ ms×90 | 排除「从 0 开始」：ffmpeg 的 mpegts demuxer 会把首包 PTS=0 当 33 位回绕，吐出 **2^33**（实测 8589934592） |

## 4. 分批

| 批次 | 内容 | 验收 |
|---|---|---|
| **M9-a** ✅ | `MpegTsMuxer` + `h264_util` 的反向转换（`avccToAnnexB`/`parseAvcC`）+ `aac_util` 的反向转换（`buildAdtsHeader`/`rawAacToAdts`）+ `examples/ts_mux_demo` + `scripts/ts_mux_test.sh` | `ts_mux_test.sh` **23/23**；`mpegts` 组 **5 例 / 78 断言**；`ffmpeg` 组 +3 例（反向转换） |
| **M9-b** ⏳ | `HlsWriter` + `/hls/` 路由（替换 501 占位）+ 切片静态服务（含 Range）+ `third_party/hls.js` + 播放页接入 | 待定：`scripts/hls_test.sh` + `ffplay`/`hls.js` |

## 5. 测试计划

| 分组 | 位置 | 说明 |
|---|---|---|
| `mpegts` ✅ | `tests/test_mpegts_muxer.cpp`（5 例） | 纯字节布局（PAT/PMT/PES/AF/PCR）+ 拒绝路径；手造 StreamInfo/MediaPacket，无样本无线程 → 严格组 |
| `ffmpeg` ✅（+3） | `tests/test_ffmpeg.cpp` | `avccToAnnexB` 互逆、`parseAvcC` 取 SPS/PPS、`rawAacToAdts` 往返与拒绝（HE-AAC/超长） |
| 脚本 ✅ | `scripts/ts_mux_test.sh`（23 项） | `ts_mux_demo` 的产物交给 `ffprobe`/`ffmpeg` 独立判定（结构/元数据/内容/PSI 四层） |

## 6. 风险清单

| # | 风险 | 触发条件 | 应对 |
|---|---|---|---|
| 1 | `adaptation field` 的 `af_len` 少算 flags 字节 | 任何"靠 AF 填充"的包 | 每个包都断言 188 对齐（`mpegts_small_frames_are_still_188_aligned`；这个 bug 真发生过：138 包 × 1 字节 → 全流花屏） |
| 2 | ASC 带扩展（MP4 实测 5 字节 `12 08 56 e5 00`） | MP4 输入的 AAC | 允许 ≥2 字节，只取核心 2 字节并 **Warn**（ADTS 表达不了扩展） |
| 3 | HE-AAC（AOT=5）无法用 ADTS 的 2 bit profile 表示 | 高压缩音频 | `rawAacToAdts` 明确失败（不硬塞成 LC） |
| 4 | PCR 间隔过大 | 关键帧间隔很长（如 10s GOP） | 当前只在关键帧插；HLS 要求 ≤100ms —— **M9-b 必须改成按时间插**（若 GOP 大），或明确记录"仅支持 GOP ≤100ms 的源" |
| 5 | `PAT/PMT` 的 CRC 写错 | 手写位域 | 用例验 CRC 由 `crc32Mpeg` 生成；`ffprobe` 认得出就是对的 |
| 6 | **PES 头两个 flags 字节写反**（把 `PTS_DTS_flags` 放进 byte6） | 任何切片 | ffmpeg 会认为「没有时间戳」，把那 5/10 字节当 header_data 跳过 → `pts=N/A`，**解码看起来完全正常**。单测当时把错的布局断言成了对的 → 已加**外部判据**：`ts_mux_test.sh` 断言 `pts != N/A` 且 dts 单调 |
| 7 | 音频包 key 标志恒真导致切点错位 | AAC 音频 | 见 §3.2 决策表「切点」；用例 `hls_cuts_only_on_video_keyframes` 锁住 |
| 8 | 首包 PTS=0 被 ffmpeg 当回绕 | 源时间轴从 0 开始 | 加 1s 起点偏移（`kStartOffset90k`）；`ts_mux_test.sh` 断言首包 pts=90000 |

## 7. 决策记录

| 决策 | 选择 | 排除的选项与原因 |
|---|---|---|
| 与 FLV 同形状（纯函数） | `writePacket(packet, &out)`，不碰网络/线程/文件 | 排除"直接在 HTTP 处理器里拼字节"（无法单测、无法用 ffprobe 独立判定） |
| 反向转换放哪 | `avccToAnnexB`/`parseAvcC` 放 `h264_util`，`buildAdtsHeader`/`rawAacToAdts` 放 `aac_util` | 排除"塞进 muxer"：这两个工具是纯字节函数，独立后可以脱库单测（与 M4-b/M8 一致） |
| 视频 PES 长度 | 写 0 | 排除"精确长度"（关键帧常 > 65535；规范允许视频写 0） |
| `StreamInfo` 来源 | 用 `DemuxerProducer` 的快照（`muxerVideo()/muxerAudio()`） | 排除"直接用 Demuxer 的引用"（重开/析构后会悬垂） |
| 验证手段 | `ts_mux_demo` + ffprobe 四层判据 | 排除"只靠单测"：单测只能证明"我以为的布局对"（与 M6-a 同一条教训） |
