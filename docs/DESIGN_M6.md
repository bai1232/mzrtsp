# M6 设计：输出层（Output / HTTP-FLV）

> 上游需求：`docs/SPEC.md` `FR-3.1`（HTTP-FLV）/ `FR-3.3`（AAC 转发、无音频可播）/ `FR-3.4`（时间戳基准）
>           `FR-4.1`（`/live/<name>.flv`）/ `FR-1.2`（节流、可选循环）/ `SC-1`（浏览器能播）
> 上游实现：`docs/DESIGN_M5.md`（媒体层）、`docs/DESIGN_M3.md`（chunked 响应）、`docs/DESIGN_M2.md`（Session 发送队列）
> 本文件对应 ROADMAP 的 **M6 Output** 一行，拆成 M6-a / M6-b / M6-c / M6-d 四批。

## 1. 范围与验收

| 批次 | 内容 | 状态 |
|---|---|---|
| **M6-a**（本批） | `FlvMuxer`：**纯函数式** FLV 封装（FLV Header、AVC/AAC sequence header、视频/音频 tag、`CompositionTime`、per-client 时间戳基准） | ✅ |
| **M6-b** | `FlvSender`（每连接一个：把订阅者队列封成 FLV 并写出）、`/live/<name>.flv` 接线（应用侧注册路由）、`HttpResponse` 的**异步流式 + 附件**、`Session::shutdownAfterFlush()` | ✅ |
| M6-c | 最小 app（`src/main.cpp`，零参数启动 FR-7.2）+ 测试页播放器（**flv.js 入库**）+ 循环播放（`--loop`，默认关）+ `broken()` 真实断连 + `dumpStatsJson()` 接进 `/api/stats` | 后续 |
| M6-d | 10 路并发 / 5 分钟播放验收脚本；**拿真实数据回填 M5 未决**（锁粒度 / 包数硬顶 / 唤醒合并 / `max_subscribers`） | 后续 |

**验收（对应 `docs/ROADMAP.md:16`）**：

| 验收点 | 落在哪批 |
|---|---|
| 编出来的 FLV **能被 `ffprobe` 认出**：h264 320x240 + aac、帧数与时长与源一致、`ffmpeg` 完整解码无错 | **M6-a ✅** |
| 字节级布局正确（header/tag/sequence header/CompositionTime） | **M6-a ✅** |
| 每个客户端**独立时间戳基准**（后接入者时间戳从 0 开始） | **M6-a ✅**（机制）+ **M6-b ✅**（每连接一个 `FlvSender`） |
| `curl -N http://…/live/x.flv` 拿到合法 FLV —— **经过 HTTP + chunked + 连接关闭之后仍然合法** | **M6-b ✅**：`scripts/flv_http_test.sh` **15/15**（h264 320x240 + aac、50 帧、`ffmpeg` 完整解码无错、55,623 字节与 M6-a 产物一致） |
| 源 EOS → 发结束块 + **等排空再关**（一个字节不丢）；客户端一直不读 → 到点强制关并计数 | **M6-b ✅**：`ntimed_session_shutdown_after_flush`（256KB 全收到后才 EOF；只连不读 → 100ms 后以 `SendFailed` 关） |
| 客户端断开 → 订阅者回收（NFR-6） | **M6-b ✅**：`/api/stats` 的 `subscribers` 回到 0（验收脚本第 5 项） |
| `ffplay` 出画面 + 有声音；浏览器测试页能播 | M6-c |
| 10 路并发各自播放 5 分钟无异常 | M6-d |

## 2. 分层位置与依赖方向

```
core → network → http → ffmpeg → media → output
```

- `output/` 依赖 `ffmpeg/` 的 `StreamInfo`（要 avcC / AudioSpecificConfig）与 `media/` 的 `MediaPacket`（要包与时间戳）。
- **不依赖 `network/`、不依赖 `http/`**：`FlvMuxer` 只产出字节，“谁来发”是 M6-b 的事。
  这样它能用 `ffprobe` 独立验证（本批的验收就是靠这个）。
- 反向依赖检查（`output/` 不得出现 network / http）：
  `grep -rn "network/\|http/" src/output/` → 应为空

## 3. 契约（M6-a）

### 3.1 `FlvMuxer`（`output/flv_muxer.h`）

```cpp
class FlvMuxer {
public:
    struct Streams { const StreamInfo *video = nullptr; const StreamInfo *audio = nullptr; };
    enum class Result : uint8_t {
        Ok, NoStreams, UnsupportedVideoCodec, UnsupportedAudioCodec,
        MissingExtradata, AnnexBNotSupported, NotPrepared, InvalidPacket, TooLarge,
    };

    explicit FlvMuxer(const Streams &streams);
    Result prepare();                       // 校验编码 + 初始化数据，失败必须给原因
    static const char *resultName(Result);

    void setTimestampBaseMs(int64_t);       // 不设置 → 首个包的 dts 自动定基准
    int64_t timestampBaseMs() const;  bool baseFixed() const;

    bool   writeHeader(std::string *out);   // FLV Header(9B) + PreviousTagSize0
    Result writeSequenceHeaders(std::string *out);   // AVC / AAC sequence header（必须紧跟 header）
    Result writePacket(const MediaPacket &packet, std::string *out);

    uint64_t tagCount() const;  uint64_t bytesWritten() const;
    uint32_t lastTimestampMs() const;  uint64_t clampedBeforeBase() const;
    std::string lastError() const;
};
```

**每个订阅者一个实例**（M6-b 里 `FlvSender` 持有它）：因为时间戳基准是**按客户端**的。

### 3.2 FLV 字节布局（写错就是"能连上但播不出"，所以逐字段列清楚）

| 区域 | 布局 |
|---|---|
| FLV Header（9B） | `'F''L''V'` \| version=1 \| flags（0x01 视频 / 0x04 音频）\| DataOffset=9（u32） |
| | PreviousTagSize0 = 0（u32） |
| Tag（重复） | type(1) \| dataSize(u24) \| timestamp 低 24 位 \| timestamp 高 8 位 \| StreamID=0(u24) \| data \| PreviousTagSize=11+dataSize(u32) |
| 视频 tag data | `(frameType<<4 \| 7=AVC)` \| `AVCPacketType`(0=sequence header, 1=NALU) \| `CompositionTime`(int24) \| payload |
| 音频 tag data | `(10=AAC<<4 \| rate<<2 \| size<<1 \| type)` \| `AACPacketType`(0=sequence header, 1=raw) \| payload |

- **sequence header 的 payload 就是 extradata**：H264 用 `AVCDecoderConfigurationRecord`（avcC），
  AAC 用 `AudioSpecificConfig` —— 都是 `Demuxer` 从 `AVCodecParameters::extradata` 带出来的（M6-a 补齐）。
- **视频 NALU 直接搬**：MP4 里的包本来就是 length-prefixed（AVCC），FLV 要的也是它 → 不需要转换。
- FLV 里 AAC 的 `SoundRate` **恒为 3**（44kHz）、`SoundSize=1`、`SoundType=1`（规范如此，不按源的采样率填）。

### 3.3 `prepare()` 的把关（宁可不开工，也不出半成品）

| 情况 | 返回 |
|---|---|
| 既无视频也无音频 | `NoStreams` |
| 视频不是 H264（如 HEVC） | `UnsupportedVideoCodec`（SPEC 有"不做 H265"的决策记录） |
| H264 缺 avcC | `MissingExtradata` |
| 初始化数据**看着像 Annex-B**（首字节 0x00） | `AnnexBNotSupported`（明确说明"请用 MP4 输入，转换见 M6-c"） |
| 音频不是 AAC | `UnsupportedAudioCodec` |
| AAC 缺 AudioSpecificConfig | `MissingExtradata` |
| 没 `prepare()` 就写 | `NotPrepared` |
| 包不属于这两路流 / CompositionTime 超 24 位 | `InvalidPacket` |
| tag 数据超 24 位长度上限 | `TooLarge` |

### 3.4 `FlvSender`（`output/flv_sender.h`，M6-b）

**每个连接一个**：把一个订阅者的队列封成 FLV 写出去。

```cpp
class FlvSender {
public:
    using Ptr = std::shared_ptr<FlvSender>;
    struct Sink {
        std::function<bool(const char *data, size_t len)> write; // false = 连接已坏
        std::function<void()> end;                               // EOS：发结束块 + 等排空再关
    };
    enum class Result : uint8_t { Ok, BadStreams, SinkFailed, NotStarted, Aborted };

    static Ptr create(const Sink &sink, FlvMuxer::Streams streams, Subscriber::Ptr subscriber);
    Result start();    // FLV Header + AVC/AAC sequence header（FR-3.1：**立即**发，不等第一帧）
    Result onDrain();  // 队列取空 → 封 tag → 缓冲到 32KB 写一块；EOS 收尾
    bool finished() const;  bool aborted() const;
    uint64_t packetsMuxed() const; uint64_t bytesWritten() const; uint32_t lastTimestampMs() const;
};
```

- **不依赖 `http/`**：写出目标是 `Sink`（`write` + `end`）→ 可以用**假 sink** 做确定性单测
  （不需要网络、不需要线程、不依赖时序）。
- `create()` 内部完成 `FlvMuxer::prepare()`，并**自己注册 drain 回调**（回调只持弱引用：
  既不成环，也不需要调用方再拿订阅者句柄 —— 订阅者已经移交给它了）。
- `onDrain()` 的两个收尾判断：**`broken()` → `Aborted`**（关键帧都进不去，继续发只会让对端花屏）、
  **EOS → `finished()` + 调 `end()`**（幂等，重复 drain 不会重复收尾）。
- 生命周期由**连接**托管：调用方 `resp.holdResource(sender)` —— 连接一断就释放 → 退订（NFR-6），
  drain 回调里的弱引用随之失效，**不会在连接断掉之后还去写 socket**。

### 3.5 HTTP 接线（M6-b）：`/live/<name>.flv` 由**应用侧**注册

- 路由：`server.setPrefixRoute("/live/", …)`。内置的 `501` 占位是在 `HttpServer::start()` 里带
  `findHandler` 守卫注册的，所以应用侧在 `start()` 之前注册就能覆盖它 —— **`http` 层不必依赖 `media`**。
- 名字映射（**外部输入，必须校验**）：只接受 `[A-Za-z0-9_-]+`（**不放行 `/` 与 `.`** → 杜绝目录穿越），
  拼成 `<media-root>/<name>.mp4`；名字非法 / 文件不存在 → 404。
- 响应：`Content-Type: video/x-flv` + `beginChunked()` + `setChunkedAsync()` + `setKeepAlive(false)`。
  **顺序讲究**：先建 `FlvSender`（它内部校验编码/初始化数据），**再**发响应头 ——
  否则会出现"已经回了 200、才发现这路流封不了"的半成品响应。
- 两个出口都由 `HttpSession` 注入，**不依赖 `HttpResponse` 对象**（handler 返回后仍能用）：

  | 出口 | 用途 | 踩过的坑 |
  |---|---|---|
  | `chunkWriter()` | 带**分块帧头**的写出函数，接 `FlvSender::Sink::write` | 用裸 `sender()` 会把 FLV 字节直接灌进 chunked 流 → 客户端报 `curl: (56) Malformed encoding`（验收脚本第一次跑就抓到） |
  | `endStreamFn()` | 发结束块 `0\r\n\r\n` + `Session::shutdownAfterFlush(5000)` | —— |

- `holdResource()`：把 `FlvSender` 挂到连接上，会话析构时统一释放（NFR-6）。

## 4. 关键机制

### 4.1 每个客户端一份的时间戳基准（`ARCHITECTURE.md` §4）

FLV 的 timestamp 要求从 0 附近开始递增，而各客户端接入时刻不同：
后接入者若直接用源的时间戳，播放器会看到很大的初值。所以基准是**每订阅者一份**：

- 未显式设置时 → **首个包的 dts** 自动定基准；
- 实测（`sample.mp4`）：首个包的 dts = **-23 ms** → 基准就是 -23，第一个 tag 的时间戳正好 0。

### 4.2 32 位回绕 + 早于基准的钳制

- FLV 的时间戳是 **32 位毫秒**：`dts - base` 按 `uint32` **自然回绕**（规范如此，长片/长时间运行会绕）。
- 但**早于基准**的包不能让它回绕成"40 亿"这种值（播放端会直接崩/黑屏）→ **钳到 0 并计数 + 首次告警**，
  绝不静默回绕。实测正常文件 `clampedBeforeBase() == 0`（第一个包就是最早的 dts）。

### 4.3 `CompositionTime = pts - dts`

视频 tag 里有 24 位**有符号**的 `CompositionTime`，B 帧必需（`AV_BASICS.md` 里那组
`packet 0 / 1536 / 512` 就是 pts 乱序的现场）。超出 ±2^23 直接报 `InvalidPacket`，**不截断**。

### 4.4 为什么 M6-a 不做 Annex-B → AVCC 转换

`FR-2.1` 要求支持 H264 裸流，但那是**输入侧**的能力；裸流进来的包是 Annex-B（起始码），
要转成 length-prefixed 才能进 FLV —— 而 `DESIGN_M4.md` §7 明确"不做 emulation prevention 反转义"，
这块的边界得重新交代。所以本批**只吃 MP4/avcC**，遇到 Annex-B 直接 `AnnexBNotSupported`
（明确报错，不是静默出坏流）。转换放 M6-c。

### 4.5 sequence header 必须**每个连接重发**

`GopCache` 保证"新订阅者的首包是关键帧"，但**初始化头不在 GOP 缓存里** ——
每个新连接都必须先发 header + 两个 sequence header（`FR-3.1` 的"连接建立后立即发送"），
这是 M6-b 的接线纪律。

## 5. 测试计划（M6-a）

### 5.1 单元测试（分组 `flv`，10 个用例 / 515 断言，进 TSAN 严格组）

| # | 用例 | 覆盖维度 | 断言要点 |
|---|---|---|---|
| 1 | `flv_header_bytes_are_exact` | 正常 | `'FLV'` + version=1 + flags（只有视频 0x01 / 有音频 0x05）+ DataOffset=9 + PreviousTagSize0=0，共 13 字节 |
| 2 | `flv_sequence_headers_layout` | 正常 | AVC/AAC sequence header 的 tag 类型、dataSize、时间戳 0、负载**逐字节等于 extradata**、PreviousTagSize 配平 |
| 3 | `flv_prepare_rejects_bad_inputs` | 空 / 非法 | 无流 / HEVC / 缺 avcC / **Annex-B 被明确拒绝** / 非 AAC / 缺 ASC / 未 prepare 就写 —— 逐条给对码 |
| 4 | `flv_video_tag_layout_and_composition_time` | 正常 | 关键帧 `0x17`、NALU `0x01`、`CompositionTime=40`；B 帧语义（pts<dts）写到 24 位补码 `0xFFFFD8` |
| 5 | `flv_audio_tag_layout` | 正常 | `0xAF`（AAC+44kHz+16bit+stereo）、raw 包 `0x01`、时间戳正确 |
| 6 | `flv_rejects_foreign_stream_and_oversized_composition` | 满 / 非法 | 外来流**不静默丢**；CompositionTime 超 24 位；tag 超 24 位长度上限 |
| 7 | `flv_timestamp_base_is_per_client` | 正常 | 从头接入 → 0/40/80；**中途接入 → 自己的时间戳也从 0 开始** |
| 8 | `flv_timestamp_wraps_at_32bit_and_clamps_before_base` | 超大 | `2^32+5` → 回绕成 5；早于基准 → 钳 0 且计数 1 |
| 9 | `flv_muxes_real_mp4_to_file` | 正常（真文件） | 真 demux `sample.mp4` → 封装落盘：tag 数 = 2 + 视频包 + 音频包、首视频包是关键帧、`clampedBeforeBase==0` |
| 10 | （由 9 顺带覆盖） | — | 落盘文件交给下面脚本做 ffprobe 校验 |

### 5.2 集成验收（`scripts/flv_mux_test.sh`，8 项，**已实跑通过**）

```
== 1) 生成 FLV ==   flv_mux_demo samples/sample.mp4 /tmp/mzmedia_flv_mux_check.flv
      → 55,623 字节 / 140 个 tag（视频 50 包、音频 88 包，时间戳基准 -23 ms）
== 2) 文件头 ==     前 3 字节 = FLV
== 3) 流参数 ==     h264 / 320 / 240 / aac
== 4) 帧数与时长 == 解码出 50 帧（源 50）、时长 2 秒
== 5) 完整解码 ==   ffmpeg -f null - 无 error 输出
结果：通过 8 项 / 失败 0 项
```

**为什么这一步不能省**：FLV 写错的表现是"能连上、播放器一片黑"，没有异常也没有报错。
单元测试只能证明"我按自己理解的规范拼了字节"；只有 `ffprobe`/`ffmpeg` 认了，才能说"这是真的 FLV"。

### 5.3 M6-b：每连接的发送器与 HTTP 接线

| # | 用例 / 脚本 | 位置 | 覆盖维度 | 断言要点 |
|---|---|---|---|---|
| 1 | `flv_sender_start_writes_header_and_sequence_headers` | 单测 `flv` | 正常 | `start()` **一次**写出去 header + 两个 sequence header（首帧延迟最小）；第一个 tag 是视频 sequence header |
| 2 | `flv_sender_create_rejects_bad_streams` | 单测 `flv` | 非法 | HEVC / sink 缺 `write` / subscriber 为空 → `create` 返回 nullptr |
| 3 | `flv_sender_rejects_drain_before_start` | 单测 `flv` | 空 | 没 `start()` 就 drain → `NotStarted`，且一个字节都不写 |
| 4 | `flv_sender_drain_muxes_all_packets_in_order` | 单测 `flv` | 正常 | 3 个包（视频/音频/视频）按序成 tag、时间戳 0/10/40、队列取空、字节数与 tag 长度自洽 |
| 5 | `flv_sender_eos_finishes_once` | 单测 `flv` | 断开 | 没有 EOS 就不结束；EOS → `finished()` + `end()` 一次；再 drain 幂等 |
| 6 | `flv_sender_broken_subscriber_aborts` | 单测 `flv` | 满 | 关键帧进不去队列 → `Aborted`（与"正常结束"分开），**不调** `end()` |
| 7 | `flv_sender_stops_when_sink_fails` | 单测 `flv` | 断开 | 写失败 → `SinkFailed` + `aborted()`，之后不再写 |
| 8 | `flv_sender_foreign_packet_is_reported` | 单测 `flv` | 非法 | 外来流 → `BadStreams`（**不静默丢**） |
| 9 | `ntimed_session_shutdown_after_flush` | 单测 `ntimed` | 断开 | 客户端一直读 → **256KB 一个字节不少**、然后 EOF、关闭原因 None；只连不读 → 100ms 后以 `SendFailed` 强制关、会话计数归零 |
| 10 | `scripts/flv_http_test.sh` | 脚本（**M6-b 验收**） | 端到端 | **15 项全过**：curl 拉到 **55,623 字节**合法 FLV（`ffprobe` + `ffmpeg` 双重校验）、`Content-Type: video/x-flv`、chunked、CORS、404（不存在 / 穿越 / 非法名）、断开后 `subscribers=0` |

## 6. 风险清单

| # | 风险 | 触发条件 | 应对 |
|---|---|---|---|
| 1 | sequence header 写错 → 播放器一片黑/无声 | extradata 缺失或格式不对 | `prepare()` 拒绝 + 集成脚本用 ffprobe 校验编码；`ROADMAP` 风险清单点名过 AAC 头 |
| 2 | 时间戳初值巨大 → 播放器异常 | 后接入者共用源时间戳 | per-client 基准（§4.1），用例 7 锁死 |
| 3 | 早于基准的包回绕成 40 亿 | 基准取到了非最小值 | 钳 0 + 计数 + 告警（§4.2），用例 8 |
| 4 | B 帧的 CompositionTime 写错 → 画面顺序乱 | pts/dts 理解错 | 24 位有符号 + 负值用例（§4.3），用例 4 |
| 5 | 外来流被静默丢 | 直接把不认识的包扔掉 | 显式 `InvalidPacket`（§3.3），用例 6 |
| 6 | Annex-B 裸流混进来出坏流 | 未做转换却照搬 | `AnnexBNotSupported` 明确拒绝（§4.4），用例 3 |
| 7 | 长片 32 位回绕 | 连续播放 > 49 天（毫秒溢出） | 按规范自然回绕；用例 8 覆盖机制 |
| 8 | tag 超过 16MB | 异常大包 | `TooLarge` 拒绝（不截断） |

## 7. 决策记录（为什么这么选 / 排除了什么）

| 决策 | 选择 | 排除的选项与原因 |
|---|---|---|
| 封装器形态 | **纯函数式** `FlvMuxer`（不碰网络/线程/文件） | 排除"直接写进 socket 的 FlvSender 一体版"（那样只能用整条链路验证，错了只能猜）；本批的验收工具是 `ffprobe`，这正是纯函数换来的 |
| 时间戳基准的归属 | `FlvMuxer` 内部维护，**每订阅者一个实例** | 排除"全局基准"（后接入者时间戳初值巨大）；排除"基准放到 Sender 里算"（那 muxer 就不是自洽的了） |
| 基准默认值 | 未设置时用**首个包的 dts** | 排除"强制调用方传"（多一个必填参数，且首个包的 dts 就是最自然的答案） |
| 早于基准的包 | 钳到 0 + 计数 + 首次告警 | 排除"自然回绕"（会出现 40 亿这种时间戳，播放端直接崩/黑屏）；排除"丢弃该包"（丢数据不可见） |
| Annex-B 输入 | **明确拒绝**（`AnnexBNotSupported`） | 排除"顺手做转换"（M4 明确不做反转义，边界要重新交代；本批只吃 MP4 一次到位）；排除"静默产出坏流"（最坏选项） |
| AVC sequence header 的负载 | **直接用 MP4 的 avcC** | 排除"自己从 SPS/PPS 重建 avcC"（多一处可能出错的地方；MP4 的 extradata 本来就是 avcC） |
| AAC 的 SoundRate | **恒填 3（44kHz）** | 按 FLV 规范：AAC 恒为 44kHz。排除"按源采样率映射"（那套映射只对 MP3 之类有意义） |
| `StreamInfo` 增加 `extradata` | 由 `Demuxer` 拷贝带出 | 排除"到用时再去 Demuxer 拿"（`AVFormatContext` 一关就没了；且 FLV 头是每个连接都要发的） |
| 视频 NALU 是否转换 | **直接搬**（MP4 已是 length-prefixed） | 排除"统一转 Annex-B"（FLV 恰恰要 length-prefixed） |
| 循环播放默认值 | **关**，`--loop` 打开（M6-c） | 排除"默认开"（验收时分不清是循环还是卡住；且"源会 EOS"这条路径要能被走到） |
| 测试页播放端（M6-c） | **flv.js 入库**（用户已定） | 排除 CDN（离线不可用）；排除自研 MSE 播放器（要自己写 FLV demux + fMP4 封装，工作量与收益不匹配） |
| M6-a 的验收工具 | `scripts/flv_mux_test.sh` + example | 排除"只在单元测试里断言字节"（自说自话）；`ffprobe` 是**外部**标准 |
| 【M6-b】写出目标抽象成 `Sink` | `FlvSender` 不依赖 `http/` | 排除"直接吃 `HttpResponse`/`Session`"（只能靠整条链路测，且 output→http 耦合没必要）；好处是可以用假 sink 做确定性单测 |
| 【M6-b】路由由**应用侧**注册 | `setPrefixRoute("/live/", …)` | 排除"把 media 逻辑塞进 `HttpServer`"（`http` 会依赖 `media`，破坏分层）；已确认内置 501 在 `start()` 里注册、可被覆盖 |
| 【M6-b】两个出口都**不依赖 `HttpResponse` 对象** | `chunkWriter()` / `endStreamFn()` 由会话注入（只捕获 Sender 的拷贝） | 排除"在 handler 里捕获 `resp`"（handler 一返回 `resp` 就没了 → 悬垂）；排除"用裸 `sender()` 当 chunked 出口"（**实测**：客户端报 `Malformed encoding`） |
| 【M6-b】异步流式要**显式声明** | `resp.setChunkedAsync()` → 框架不再兜底 `endChunked()` | 排除"按 keepAlive 猜"（启发式迟早猜错）；兜底只对"同步发完"的处理器有意义，对异步流会把流提前掐断 |
| 【M6-b】流式资源的生命周期 | `resp.holdResource(sender)`，由会话在连接关闭时释放 | 排除"handler 里捕获 `shared_ptr`"（与订阅者成环 → 永不释放，NFR-6 不达标）；排除"应用层自建连接表"（handler 拿不到 session 标识） |
| 【M6-b】"发完再关"挂在哪 | 写路径的**"队列空"分支** + 截止定时器兜底 | 排除"直接 `shutdown`"（丢尾部数据）；排除"挂在写循环之后"（那个循环**只在队列空/出错时退出** → 钩子变成**死代码**，本次实测踩到、被新用例抓出） |
| 【M6-b】`dumpStatsJson()` 要**惰性清理** | 统计前先 `pruneExpiredLocked()` | 排除"只靠流量触发清理"（源 EOS 之后没有流量 → 统计永远停在旧值；NFR-6 的验收正是拿 `subscribers` 当判据） |

## 8. 未决事项

| # | 事项 | 何时定 |
|---|---|---|
| 1 | `flv.js` 入库的体积/许可证备注（Apache-2.0，需保留声明） | M6-c 落库时；同时更新 README 的"零第三方依赖"表述（只说 C++ 侧） |
| 2 | 是否发 `onMetaData` script tag（flv.js/ffplay 都不强制） | M6-c 实测浏览器行为后再定；当前**不发**（最小实现） |
| 3 | 是否发 AVC end-of-sequence tag（可选） | 同上；当前靠连接关闭表示结束 |
| 4 | Annex-B → AVCC 转换（裸流输入） | **M6-c**（`FR-2.1` 要求支持裸流） |
| 5 | `FlvSender` 的发送节奏：一次 drain 多少 tag（与 M5 唤醒合并的关系） | **M6-b** |
| 6 | `Session::shutdownAfterFlush()` 的实现方式（等队列排空再关） | **M6-b**（`DESIGN_M3` §9 未决 5） |
| 7 | 5 分钟 / 10 路并发下的真实参数（锁粒度、包数硬顶、唤醒合并、`max_subscribers`） | **M6-d**（回填 `DESIGN_M5` §8） |

## 9. 假设与影响面（`AI_COLLAB.md` §1 的②③）

**假设**：

- 输入是 **MP4/avcC**（v0.1 的主力输入）；H264 裸流在 M6-a 明确拒绝（M6-c 补）。
- 一路视频 + 一路音频（多路音频/多轨不在 v0.1 范围）。
- `MediaPacket::ptsMs()/dtsMs()` 已是**毫秒**（M4 换算 + 单调钳制）。
- 时间戳允许为负（首包 dts 实测 -23 ms）→ 基准可以取负值，`dts - base` 才落到 0 附近。
- 每个连接一个 `FlvMuxer` 实例（per-client 基准 + sequence header 重发）。

**影响面**：

- 新增：`src/output/flv_muxer.h/.cpp`、`tests/test_flv_muxer.cpp`、`examples/flv_mux_demo.cpp`、
  `scripts/flv_mux_test.sh`、本文件。
- 改动：`CMakeLists.txt`（新源文件）、`examples/CMakeLists.txt`（新示例）、`tests/CMakeLists.txt`
  （新文件 + 分组 `flv`）、`scripts/tsan.sh`（严格组）、`src/mzmedia.h`（output 段）、
  `src/ffmpeg/demuxer.h/.cpp`（`StreamInfo` 增加 `extradata` **并同步 `DESIGN_M4`**）、
  `docs/ROADMAP.md`、`docs/TESTING.md`、`CHANGELOG.md`。
- **不动**：`core/`、`network/`、`http/`、`media/` 的现有代码。
