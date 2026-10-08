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
| **M6-c**（本批） | 应用入口 `src/main.cpp`（零参数启动 `FR-7.2`、`--port/--media-root/--web-root/--loop/--speed/--log-level`）、测试页播放器（**flv.js 1.6.2 入库**，离线可用）、循环播放（`--loop`，默认关）、**Annex-B → AVCC + avcC 构造**（裸流终于能拉）、`Sink::abort`（`broken()` 真实断连）、`dumpStatsJson()` 接进 `/api/stats`、**修两个真 bug**（见 §4.7/§4.8） | ✅ |
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
| `ffplay` 出画面 + 有声音；浏览器测试页能播 | **M6-c ✅**：`GET /` 就是播放页（flv.js **入库**，离线可用）；`curl -N …/live/x.flv > x.flv && ffplay x.flv` 实测有画面有声音 |
| `NFR-1`：首帧 **< 1s** | **M6-c ✅**：验收脚本 §4 用"文件长到 2KB"近似首帧到达，实测 **20ms 级**（TTFB 3.3ms） |
| `FR-2.1`：H264 **裸流**（Annex-B）也能输出 | **M6-c ✅**：`/live/sample.h264.flv` → 现场构造 avcC + 逐包转 AVCC；`ffprobe` 认 h264 320x240、`ffmpeg` 完整解码无错、python 逐 tag 复查 AVCC 结构 |
| 客户端断开/关键帧进不去 → **真的断连**（不是只记一个结果） | **M6-c ✅**：`Sink::abort` → `Session::shutdown`（不发结束块），客户端会明确看到传输被截断 |
| 源结束后**再拉同一路**：立刻结束，不挂连接 | **M6-c ✅**：验收脚本 §8（修前会一直挂着，见 §4.7） |
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

### 3.4 `FlvSender`（`output/flv_sender.h`，M6-b；M6-c 加 `abort`）

**每个连接一个**：把一个订阅者的队列封成 FLV 写出去。

```cpp
class FlvSender {
public:
    using Ptr = std::shared_ptr<FlvSender>;
    struct Sink {
        std::function<bool(const char *data, size_t len)> write; // false = 连接已坏
        std::function<void()> end;                               // EOS：发结束块 + 等排空再关
        std::function<void()> abort;                             // 【M6-c】异常：**硬断连**（不发结束块）
    };
    enum class Result : uint8_t { Ok, BadStreams, SinkFailed, NotStarted, Aborted };

    static Ptr create(const Sink &sink, FlvMuxer::Streams streams, Subscriber::Ptr subscriber);
    Result start();    // FLV Header + AVC/AAC sequence header（FR-3.1：**立即**发）+ **立刻搬一次队列**
    Result onDrain();  // 队列取空 → 封 tag → 缓冲到 32KB 写一块；EOS 收尾
    bool finished() const;  bool aborted() const;
    uint64_t packetsMuxed() const; uint64_t bytesWritten() const; uint32_t lastTimestampMs() const;
};
```

- **不依赖 `http/`**：写出目标是 `Sink`（`write` + `end` + `abort`）→ 可以用**假 sink** 做确定性单测
  （不需要网络、不需要线程、不依赖时序）。
- `create()` 内部完成 `FlvMuxer::prepare()`，并**自己注册 drain 回调**（回调只持弱引用：
  既不成环，也不需要调用方再拿订阅者句柄 —— 订阅者已经移交给它了）。
- `onDrain()` 的两个收尾判断：**`broken()` → `Aborted`**（关键帧都进不去，继续发只会让对端花屏）、
  **EOS → `finished()` + 调 `end()`**（幂等，重复 drain 不会重复收尾）。
- 【M6-c】**`end` 与 `abort` 是两个语义，不能混**：`end` = "流正常结束"（发 `0\r\n\r\n`，客户端认为数据完整）；
  `abort` = "流是残缺的"（订阅者 broken / 写失败）→ 由 HTTP 侧**硬关连接**，客户端明确报"传输被截断"。
  用 `end` 收尾等于替客户端把丢帧这件事掩盖掉。`fail()` 在 `SinkFailed`/`Aborted` 时调用它，**最多一次**。
- 【M6-c】`start()` 在发完头之后**立刻搬一次队列**。两个理由：
  1. 订阅时源**可能已经结束**（`MediaSource::subscribe()` 会给新队列 `markEndOfStream()`），
     但那一刻 drain 回调还没注册 → **不会有任何唤醒** → 这一路连接会挂着不动（§4.7 的 bug）；
  2. 中途接入拿到的 GOP 缓存应当立刻发出去（首帧延迟 `NFR-1`）。
- 生命周期由**连接**托管：调用方 `resp.holdResource(sender)` —— 连接一断就释放 → 退订（NFR-6），
  drain 回调里的弱引用随之失效，**不会在连接断掉之后还去写 socket**。

### 3.5 HTTP 接线（M6-b；M6-c 起在 `src/main.cpp`）：`/live/<name>.flv` 由**应用侧**注册

- 路由：`server.setPrefixRoute("/live/", …)`。内置的 `501` 占位是在 `HttpServer::start()` 里带
  `findHandler` 守卫注册的，所以应用侧在 `start()` 之前注册就能覆盖它 —— **`http` 层不必依赖 `media`**。
- 名字映射（**外部输入，必须校验**，M6-c 起支持扩展名）：

  | 请求 | 媒体文件 | 说明 |
  |---|---|---|
  | `/live/sample.flv` | `<root>/sample.mp4` | 不带扩展名 → 默认 `.mp4`（M6-b 行为，保持兼容） |
  | `/live/sample.h264.flv` | `<root>/sample.h264` | 带**白名单**扩展名（`.mp4/.h264/.mkv/.ts/.aac/.flv`）→ 原样用 |
  | `/live/../etc/passwd.flv` · `/live/.hidden.flv` · `/live/a b.flv` | —— | 404：名字只允许 `[A-Za-z0-9_.-]`，且**显式拒绝** `..` 与以 `.` 开头 |
  | `/live/notes.sh.flv` | —— | 404，**且提示不同**（"路径应为…"而不是"文件不存在"）：扩展名不在白名单 |

  白名单 + 显式拒绝 > "把 `/` 过滤掉"：前者可审计，后者总要漏。
- 响应：`Content-Type: video/x-flv` + `beginChunked()` + `setChunkedAsync()` + `setKeepAlive(false)`。
  **顺序讲究**：先建 `FlvSender`（它内部校验编码/初始化数据），**再**发响应头 ——
  否则会出现"已经回了 200、才发现这路流封不了"的半成品响应。
- 三个出口都由 `HttpSession` 注入，**不依赖 `HttpResponse` 对象**（handler 返回后仍能用）：

  | 出口 | 用途 | 踩过的坑 |
  |---|---|---|
  | `chunkWriter()` | 带**分块帧头**的写出函数，接 `FlvSender::Sink::write` | 用裸 `sender()` 会把 FLV 字节直接灌进 chunked 流 → 客户端报 `curl: (56) Malformed encoding`（验收脚本第一次跑就抓到） |
  | `endStreamFn()` | 发结束块 `0\r\n\r\n` + `Session::shutdownAfterFlush(5000)` | —— |
  | `abortFn()`【M6-c】 | **不发**结束块，直接 `Session::shutdown()`（硬断连） | 与 `end` 混用会把"流残缺"伪装成"正常结束" |

- `holdResource()`：把 `FlvSender` 挂到连接上，会话析构时统一释放（NFR-6）。

### 3.6 `DemuxerProducer` 的两项新增（M6-c）：输入形状与循环

`DemuxerProducer` 是**解封装层的最后一次加工**：往下的 `MediaPacket` 对输出层必须是"同一种形状"
（H264 = AVCC、AAC = raw），往上的时间戳必须是"一条连续的时间轴"。

```cpp
struct Config {
    Demuxer::Limits demux;
    bool loop = false;                 // 【M6-c】读到文件尾重开（--loop）
};
const StreamInfo *muxerVideo() const;  // 输出层**唯一**该用的入口：Annex-B 时这里已是构造好的 avcC
const StreamInfo *muxerAudio() const;
bool annexBVideo() const;              // 输入是不是裸流（观测/日志）
uint64_t loopCount() const;  int64_t loopOffsetMs() const;  uint64_t loopFailures() const;
```

**① Annex-B → AVCC（`FR-2.1`）**

- 为什么放这里：`h264_util` 的 `annexBToAvcc()` / `buildAvcC()` 是纯函数（能单测），
  但"什么时候转"是**解封装层**的知识（输入什么形状），输出层只管"我要 avcC"。
  转完之后 `MediaPacket` 与 MP4 输入**完全同形** → `FlvMuxer` 一行都不用改。
- 触发条件严格：`codec_id == H264` 且 `extradata` **看起来是 Annex-B**（`00 00 01` 开头）。
  avcC 的首字节固定是 `1`，两者不会混淆。
- 失败必须**明确**：extradata 里没同时出现 SPS/PPS → `open()` 直接失败（**不用裸 SPS/PPS 冒充 avcC**）；
  包内没有合法起始码 → `read()` 返回 `Error`（**不原样拷一份冒充成功** —— 那正是"花屏"的成因）。
- `avcC` 的基础字段：`version=1`、`profile/compat/level`（取自 SPS 第 1..3 字节）、
  `0xFF`（lengthSizeMinusOne=3 → **4 字节长度**，与逐包转换一致）、`0xE1`（1 个 SPS）、SPS、`0x01`、PPS。
  high profile 的扩展字段**不写**（播放端从 SPS 自己读）—— 这条不靠"字段看起来齐了"自我确认，
  而是靠 `ffprobe`/`ffmpeg` 真解一遍 + 脚本里 python 逐 tag 复查 AVCC 结构。

**② 循环重开（`--loop`）**

- 读到 EOF → `Demuxer::close()` + 重开同一路径，`offset += 上一趟的"最后 dts + 一帧时长 + 1ms"`，
  之后所有包的 dts/pts 都加上这个累计偏移 → 时间轴**连续且严格递增**。
- 三处刻意的设计（都有对应的失败模式）：

  | 设计 | 排除的失败 |
  |---|---|
  | 偏移由**生产者**加，输出层无感 | 时间轴回退 → 播放器花屏/卡死（我们控制不了播放端怎么反应） |
  | 每趟都校验**流参数没变**（index/codec/宽高/extradata） | 运行期文件被换掉 → 用旧快照封装新字节 = 静默出坏流；不一致就**明确失败** |
  | 一趟**一个包都没有**时不再重开 | 空文件/截断 → 死循环 + 100% CPU |
  | 帧长**向上取整** + **1ms 间隙** | 第二趟首包与第一趟末包撞在同一毫秒（毫秒是整数，AAC 一帧真实 23.22ms）→ ffmpeg 报 `non monotonically increasing dts`（§4.7） |

- 快照**只写一次**（`open()` 时），重开时只**校验**不刷新：连接可能正拿着 `StreamInfo*` 写 FLV，
  就地刷新就是跨线程写 → 数据竞争 + 悬垂。

### 3.7 应用入口 `src/main.cpp`（M6-c）

app 只做**接线**，不做媒体逻辑。三件事：

1. **零参数启动**（`FR-7.2`）：`./bin/mzmedia` → `0.0.0.0:8080` + 媒体目录 `./samples`，
   启动横幅直接打印播放页 / 拉流 / 统计 URL（验收脚本会检查"横幅里有 URL"）。
2. **命令行**（`FR-7.1`/`FR-7.3`）：`--port` / `--media-root` / `--web-root` / `--loop` / `--speed` /
   `--log-level` / `--help`。未知参数**报错退出**（打错一个字母却"看起来启动成功了"最难查）。
3. **两条路由**：`/live/`（媒体层）与 `/flv.min.js`（静态网页资源，`loadFile` 直读）；
   `/`（播放页）与 `/api/stats`（统计）走 `HttpServer` 的内建实现。

- 播放页在 `http_server.cpp` 的 `kTestPage` 里（**内建、离线可用**），引用 `/flv.min.js`；
  `flv.js` **入库**在 `third_party/flv.js/`（Apache-2.0，来源/版本/MD5 见该目录 `README.md`）。
  页面里刻意做了一件事：**flv.js 没加载出来时把原因写在页面上** ——
  否则用户看到的是"点了没反应"的播放器，无从判断是页面坏了还是流坏了。
- **媒体层的空闲释放必须避开"有人在看"**（§4.8）：`SourceManager::Config` 新增
  `deferred_recheck_ms`（默认 1000ms），空闲到期但仍有订阅者时**推迟**并复查。

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

### 4.4 Annex-B → AVCC：M6-a 明确拒绝，M6-c 在这里补上

`FR-2.1` 要求支持 H264 裸流，但那是**输入侧**的能力；裸流进来的包是 Annex-B（起始码），
要转成 length-prefixed 才能进 FLV。M6-a 的选择是**只吃 MP4/avcC**，遇到 Annex-B 直接
`AnnexBNotSupported`（明确报错，不是静默出坏流）。

M6-c 的补法（见 §3.6）：转换放在 `DemuxerProducer`（解封装层），
`FlvMuxer` 保持"M6-a 只吃 AVCC"的契约**完全不变** —— 也就是说，
`output/` 至今不知道"输入可能是裸流"这件事，转换的正确性完全由
`h264_util` 的纯函数单测 + 端到端 `ffprobe/ffmpeg` 负责。

> 边界提醒：`splitAnnexBNals` 是**朴素切分**（找 `00 00 01`），不做 emulation prevention 反转义。
> 这对**合规码流**是正确的 —— H.264 规定 NAL 负载里不允许出现 `00 00 00/01/02/03`，
> 编码器会插入 `0x03` 防竞争字节。`DESIGN_M4.md` §7 的"不做反转义"因此依然是安全的。

### 4.5 sequence header 必须**每个连接重发**

`GopCache` 保证"新订阅者的首包是关键帧"，但**初始化头不在 GOP 缓存里** ——
每个新连接都必须先发 header + 两个 sequence header（`FR-3.1` 的"连接建立后立即发送"），
这是 M6-b 的接线纪律。

### 4.6 循环播放：把"第二趟"接成同一根时间轴

```
第一趟  dts: 0 ─────────────────────────────► 2019
                                              +offset(2020)          ← 重开：close + open 同一路径
第二趟  dts: 2020 ───────────────────────────► 4039
```

- 偏移 = Σ(每趟的 `末包 dts + 帧长 + 1ms`)，由 `DemuxerProducer` 在**包装成 `MediaPacket` 时**加上。
- 于是下游（节流、GOP 缓存、`FlvMuxer` 的 per-client 基准、播放端）**完全看不出这是第二趟**
  —— 这就是为什么不需要任何"新一段"标记（见 §7 里被删掉的 `PacketNewSegment`）。
- 偏移量可观测：`/api/stats` 的 `producer.loops` / `producer.loop_offset_ms`
  （验收脚本 §9 就是拿它 + `ffprobe` 的时长/时间戳单调性做判定）。

### 4.7 本批抓到的两个真 bug（都是"接起来才暴露"的那类）

| # | 现象 | 根因 | 修法 | 用例 |
|---|---|---|---|---|
| 1 | **源读完之后再拉同一路，连接一直挂着不动**（客户端：连上了、头也发了、然后永远黑屏） | 晚到的订阅者加入时 `MediaSource::subscribe()` 会给它的队列 `markEndOfStream()`，但那一刻 drain 回调**还没注册**（由 `FlvSender::create()` 稍后注册）→ 不会有任何唤醒 → 这一路永远不会被搬、也就永远不会收尾 | `FlvSender::start()` 在发完头之后**立刻搬一次队列** | 单测 `flv_sender_start_drains_already_ended_source` + 单测 `flv_sender_start_drains_seeded_gop` + 验收脚本 §8（修前 curl 会 rc=28 超时挂着，修后 12ms 正常结束） |
| 2 | **循环播放每 2 秒被 ffmpeg 报一次 `non monotonically increasing dts`** | 偏移量算的是"末包 dts + 帧长"，而时间戳是**整数毫秒**：AAC 一帧真实是 23.22ms，只能写 23 → 第二趟的首包正好落在第一趟末包**后 23ms**，而解码器的采样级时间轴已经走了 23.22ms → 差 0.22ms 的回退 | 帧长**向上取整** + 额外留 **1ms 间隙**（`kLoopGapMs`）；两条流各自都要求跨趟**严格递增** | 单测 `producer_loop_offsets_timestamps_monotonically`（视频**与音频**分别断言严格递增）+ 验收脚本 §9（`ffmpeg` 完整解码无 error） |

两个都是**端到端才会暴露**的问题：单元测试各自都绿，串起来才看得见 ——
这也是"必须有真脚本验收"的直接证据。

### 4.8 跨连接与跨线程的生命周期（本批补齐的一条规则）

**"句柄数为 0"≠"没人看"**。应用层 handler 一返回就把 `MediaSource::Ptr` 句柄放掉了，
而连接还连着、订阅者还在推流。M5-c/M5-d 的空闲释放只看句柄数 → 60s 一到就把源线程停掉，
**观看者会突然"播完"**（M6-d 的 5 分钟验收必然踩到，本批在写 app 时读代码发现）。

修法（`SourceManager::tryIdleRelease()`）：空闲到期时若 `subscriberCount() > 0`
→ **推迟**（计数 `idle_deferred`）+ 按 `deferred_recheck_ms` 复查，直到订阅者也走掉才释放。
三个入口（句柄释放、挂表、计时到点）走**同一个守卫**，避免漏一条路径。
`deferred_recheck_ms` 必须 > 0（0 会退化成"async 里再 async"的忙等，`setConfig` 直接拒绝）。

顺带明确一条**使用契约**：`muxerStreamsFor()` 返回的 `StreamInfo*` 指向源自己持有的快照，
可以跨连接长期保存（重开也不换），但**源必须先活着** —— 上面这条守卫正是它的前提。

### 4.9 M6-d 抓到的三个真问题

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | **5 分钟跑在 t=61s 时 10 路连接全部断开**（3~5 秒的验收永远看不到） | HTTP-FLV 是服务端**单向推流**，客户端不发数据 → FR-4.4 的"读空闲 60s"把正在观看的连接掐掉 | 异步流式响应（`setChunkedAsync()`）**关掉该连接的读空闲检测**；写阻塞超时 + TCP keepalive 仍兜底。用例 `ntimed_http_stream_survives_recv_idle`（**修前必红**：把读空闲压到 300ms，1.5s 后 recv 拿到 EOF） |
| 2 | 一台 5 路被 kill 的客户端在日志里留下 **10 条 `[E]`** → NFR-3 的"错误日志 0 条"没法测 | "对端消失"被当成服务器故障，而且在**四处**各错一次（`Session::emitError` 的级别、`Socket::send/recv` 的日志、`Buffer::readFromFd` 的日志、`onEvent` 的分支顺序） | `isPeerGoneErrno()`（`core/util`，纯函数）+ `Session::closeLogLevel()`（**public static**，可测）+ 三处日志分级；**类型与计数一律不变**（既有 onError 断言与计数不受影响） |
| 3 | `EPOLLHUP/EPOLLERR` 时**先判错就 return** → 缓冲区里对端剩下的数据被丢掉，且原因被硬编码成 `recv-failed (errno=0)` | `EventPoller::toEvent()` 明明同时给了 `EventRead`（注释写着"让回调先把缓冲区读干净再关闭"），`Session::onEvent()` 却先判错误事件 | 顺序改为 **先写 → 再读 → 最后判错误事件**；真"挂断且无数据可读"才报 `PeerClosed`（不再假报 `RecvFailed(errno=0)`）。**这条没有确定性单测**（依赖内核事件组合，且 `onEvent()` 是 private）——证据是 poller 注释与代码的矛盾 + 并发脚本 Error 计数 10 → 0，补测留 M7 |

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

### 5.4 M6-c：输入形状、循环与 app（单测 + 端到端）

**（a）纯函数单测（分组 `ffmpeg`，7 个新用例）**

| # | 用例 | 覆盖维度 | 断言要点 |
|---|---|---|---|
| 1 | `ffmpeg_avcc_looks_like_annexb` | 正常/空 | 3 字节与 4 字节起始码都认；avcC（首字节 1）不认；太短/全 0 不认 |
| 2 | `ffmpeg_avcc_convert_known_bytes` | 正常 | 手写 2 个 NAL 的 Annex-B → **逐字节**对期望（`00 00 00 03 65 AA BB` + `00 00 00 02 41 CC`）；4 字节起始码残留的 0 字节被吃掉 |
| 3 | `ffmpeg_avcc_convert_rejects_bad_input` | 空/非法 | 无起始码 / 空指针 / 只有起始码 → **false 且输出为空**（不许原样拷） |
| 4 | `ffmpeg_avcc_build_layout` | 正常 | avcC 20 字节**逐字段**：`01 42 C0 1E FF E1 00 05 <SPS> 01 00 04 <PPS>` |
| 5 | `ffmpeg_avcc_build_rejects_bad_params` | 非法/超大 | SPS 太短 / 类型不是 7 / 不是 8 / 空 PPS / 长度超 16 位（0x10001）→ 全部拒绝 |
| 6 | `ffmpeg_avcc_roundtrip_on_raw_sample` | 正常（真文件） | 真裸流整体转换：输出 = Σ(4+N)，再按长度前缀反解**恰好用完**、NAL 类型合法、个数一致 |
| 7 | `ffmpeg_avcc_build_from_raw_sample_sps_pps` | 正常（真文件） | 抽真 SPS/PPS 构造 avcC 并逐字段核对；**交叉验证**解封装给出的 extradata 确实是 Annex-B |

**（b）输入形状与循环（分组 `producer`，6 个新用例；单线程、直接用真样本、不起线程不等时序）**

| # | 用例 | 覆盖维度 | 断言要点 |
|---|---|---|---|
| 1 | `producer_mp4_snapshot_is_usable_without_conversion` | 正常 | 快照与解封装一致、extradata 首字节 1、`annexBPackets()==0`；**按流**校验 dts 单调 |
| 2 | `producer_without_loop_stops_at_eof` | 正常 | `loop=false` → `EndOfStream`（不是 Error）、`loopCount()==0`、总包数 > 100 |
| 3 | `producer_annexb_sample_gets_avcc_snapshot` | 正常（裸流） | `annexBVideo()==true`、快照 extradata 是 avcC（`01 … FF E1 … 67`）、**`FlvMuxer::prepare()==Ok`**（M6-a 时代这里会返回 `AnnexBNotSupported`） |
| 4 | `producer_annexb_packets_are_converted_to_avcc` | 正常（裸流） | 前 30 个包逐个按 4 字节长度切完**不剩字节**、NAL 类型合法、有 IDR、`annexBPackets()==包数` |
| 5 | `producer_annexb_without_extradata_is_rejected` | 空 | 剥掉 SPS/PPS 的裸流：要么打不开、要么下游 `MissingExtradata`/`NoStreams` —— **两条路都必须明确失败** |
| 6 | `producer_loop_offsets_timestamps_monotonically` | 断开（循环） | 跨趟**视频与音频各自严格递增**；`loopOffsetMs()∈[1800,2600]`（2s 样本）；`loopFailures()==0` |

**（c）发送器与源管理（`flv` +2 / `srcmgr` +2）**

| # | 用例 | 分组 | 断言要点 |
|---|---|---|---|
| 1 | `flv_sender_start_drains_already_ended_source` | flv | 源已 EOS 时晚到订阅：`start()` 就应当 `finished()` + `end()` 一次（§4.7 bug 1） |
| 2 | `flv_sender_start_drains_seeded_gop` | flv | 中途接入拿到 GOP 缓存 → `start()` 立刻搬走（`packetsMuxed()==1`），且不误判结束 |
| 3 | `flv_sender_broken_subscriber_aborts`（扩充） | flv | `Aborted` 时 `sink.abort` **恰好一次**、`end` 零次；再 drain 仍是 `Aborted`（不会像"又能发了"） |
| 4 | `flv_sender_stops_when_sink_fails`（扩充） | flv | `SinkFailed` 也调 `abort`（写不出去时必须真断连），且只断一次 |
| 5 | `srcmgr_idle_release_waits_for_subscribers` | srcmgr | `idle_release_ms=0` + 有订阅者 → **不释放**且 `idle_deferred` 增长；订阅者一走 → 下一次复查释放（§4.8 bug 2） |
| 6 | `srcmgr_muxer_streams_for_mp4_and_annexb` | srcmgr | MP4 快照是 avcC；裸流快照**现场构造成 avcC**（`E1`）+ `annex_b_video=true`；不存在的源两路都是 `nullptr` |

**（d）端到端：`scripts/flv_http_test.sh`（49 项，**已实跑通过**）**

```
== 0) 起服务（--port/--media-root/--web-root）+ 启动横幅打印播放 URL（FR-7.2）
== 1) 网页侧：GET / 200、页面含 <video>/flv.min.js/createPlayer；GET /flv.min.js 200
       + 与入库文件**逐字节一致**（144165 字节）+ 版本 1.6.2
== 2) MP4 源：curl 拉流 → FLV / h264 320x240 / aac / 50 帧 / ffmpeg 完整解码无 error
== 3) H264 裸流源：FLV / h264 320x240 / 50 帧 / 解码无 error / **python 逐 tag 复查 AVCC 结构**
== 4) NFR-1：冷启动首帧（≥2KB）实测 20ms 级（ttfb 3.3ms）
== 5) 响应头：video/x-flv + chunked + CORS
== 6) 边界：不存在的源 / 路径穿越 / 非法名 / 非白名单扩展名 / 缺 .flv 后缀 —— 全部 404（并区分提示语）
== 7) 断开后 subscribers=0（NFR-6）+ /api/stats 含 source_manager/media_sources/producer/annex_b_packets/idle_deferred
== 8) **源结束后再拉同一路：12ms 正常结束**（§4.7 bug 1 的回归测试）
== 9) --loop @4x 拉 3 秒 → 11 秒媒体时长 / 297 帧 / loops=6 / 时间戳跨趟单调 / ffmpeg 解码无 error
结果：通过 49 项 / 失败 0 项
```

**为什么 §8/§9 这两条最值钱**：它们是本批唯一由"跑起来"发现的缺陷来源
（单测全绿、脚本一跑就红）。第 8 条修前是 `curl rc=28`（挂满 8 秒超时），
第 9 条修前 ffmpeg 每秒报一次 `non monotonically increasing dts` ——
两者都不是"崩了"，而是"看起来在跑，其实不对"，正是最难靠 review 发现的那类。

### 5.5 M6-d：并发 / 长跑（`scripts/concurrent_test.sh`）

**一个脚本、两层判据**（刻意分开：功能性设硬门禁，性能只记基线 —— 5 分钟的 RSS 斜率噪声大，
拿它当门禁会变成随机翻红）：

| 类别 | 判据 | 说明 |
|---|---|---|
| **功能性（硬门禁）** | 每一路都拿到**完整且可解码**的码流（h264 + 帧数在预期区间 + `ffmpeg -f null -` 无 error） | 5×curl（纯拉流）+ 5×ffmpeg（边拉边真解码），**错开 200ms 接入** |
| | 断开后 `subscribers == 0`、fd 回落到基线（±2） | NFR-6 |
| | 服务端**错误日志 0 条**（`[E]`/`[F]`）；Warn 只报数 | NFR-3（对端消失记 Warn，见 §4.9） |
| | `--kill-half`：被 kill 的路 `auto_unsub` 增长，其余路**不受影响** | TESTING §6 的专项 |
| | `--overload N`：接受的正好是 `max_subscribers - 已占用`，其余 `rejected` 增长 | FR-5.1 的上限真的在生效（脚本不硬编码 16） |
| **性能（只记基线）** | 服务端 CPU（单核%）、RSS 斜率（含折算 MB/h）、fd 曲线 | NFR-3 / NFR-4 |
| | `wakeup`：`notify`/`coalesced`/`popped` 与"一次唤醒搬多少包" | 回填 `DESIGN_M5` §8 未决 #9 |
| | 源端包/s、丢帧计数、订阅者曲线 | 回填 #7（包数洪泛）与 #8（锁粒度） |
| | 音/视频时间轴跨度 + **音画偏移峰值** | 回填 #13（按混合 dts 对齐会不会漂） |

跑法：`--quick`（10 路 × 20s，回归门禁）／默认 `--duration 300`（10 路 × 5 分钟）／
`--duration 3600`（1 小时，NFR-3 原文，留 M7 发版前执行）。
客户端被 `--max-time`/`-t` 掐断时尾部可能留半个 tag → 脚本先"切到最后一个完整 tag"再校验，
并报出丢弃字节数（不静默）。

**M6-d 实测结果**（10 路 × 300s，`--loop` + 2 秒样本 = 150 次循环重开；详细表格见 `TESTING.md` §5）：

- 功能性 **8/8（含 `--overload`）→ 快速版 10/10**：每路码流完整可解（视频 7426 / 预期 7500）、
  断开后订阅者归零且 fd 回落到基线、**错误日志 0 条**、超额连接"接受 6 / 拒绝 2"；
- 性能：服务端 **CPU 单核 4.11%**（十路合计）、RSS 稳态段 232s **+24 kB（折算 0.36 MB/h）**、
  丢帧 0、唤醒 **1.4 包/次**、源端 ≈68 包/s、音画偏移峰值 **2.0 ms**；
- 回填结论（`DESIGN_M5` §8）：#1 保持 16、#7 不加包数硬顶、#8 不升级锁粒度、#9 唤醒合并够、#13 节流基准不改。

## 6. 风险清单

| # | 风险 | 触发条件 | 应对 |
|---|---|---|---|
| 1 | sequence header 写错 → 播放器一片黑/无声 | extradata 缺失或格式不对 | `prepare()` 拒绝 + 集成脚本用 ffprobe 校验编码；`ROADMAP` 风险清单点名过 AAC 头 |
| 2 | 时间戳初值巨大 → 播放器异常 | 后接入者共用源时间戳 | per-client 基准（§4.1），用例 7 锁死 |
| 3 | 早于基准的包回绕成 40 亿 | 基准取到了非最小值 | 钳 0 + 计数 + 告警（§4.2），用例 8 |
| 4 | B 帧的 CompositionTime 写错 → 画面顺序乱 | pts/dts 理解错 | 24 位有符号 + 负值用例（§4.3），用例 4 |
| 5 | 外来流被静默丢 | 直接把不认识的包扔掉 | 显式 `InvalidPacket`（§3.3），用例 6 |
| 6 | Annex-B 裸流混进来出坏流 | 未做转换却照搬 | M6-a：`AnnexBNotSupported` 明确拒绝；M6-c：在解封装层转换 + 纯函数单测 + ffprobe 验收（§4.4/§3.6） |
| 7 | 长片 32 位回绕 | 连续播放 > 49 天（毫秒溢出） | 按规范自然回绕；用例 8 覆盖机制 |
| 8 | tag 超过 16MB | 异常大包 | `TooLarge` 拒绝（不截断） |
| 9 | 【M6-c】avcC 构造错 → 裸流"能连上但一片黑" | 从 Annex-B 现场构造 avcC 时字段写错 | 纯函数逐字段单测 + `ffprobe`/`ffmpeg` 真解 + 脚本里 python 逐 tag 复查 AVCC 结构（§5.4a/c） |
| 10 | 【M6-c】循环播放时间轴回退 | 偏移量算小了（整数毫秒 vs 小数帧长） | 帧长向上取整 + 1ms 间隙；两条流各自严格递增的用例 + ffmpeg 解码无 error（§4.7 bug 2） |
| 11 | 【M6-c】源结束后晚到连接挂死 | EOS 早于 drain 回调注册 | `start()` 立刻搬一次队列；用例 + 验收脚本 §8（§4.7 bug 1） |
| 12 | 【M6-c】有人在看却把源释放了 | 只按"句柄数"判空闲 | `subscriberCount()>0` 就推迟 + 复查；用例 `srcmgr_idle_release_waits_for_subscribers`（§4.8） |
| 13 | 【M6-c】`--loop` 遇到被替换/截断的文件 | 运行期文件变了 | 每趟校验流参数；空趟不再重开（§3.6） |
| 14 | 【M6-d】**单向响应被"读空闲 60s"掐断**（长直播/大文件下载） | 客户端不发数据 → 读空闲到点 | 已修：异步流式响应（`setChunkedAsync()`）豁免读空闲；写阻塞 + keepalive 兜底。**通用化留 M7**（§8 未决 11） |
| 15 | 【M6-d】"对端消失"被记成 Error → NFR-3 的"错误日志 0 条"失去意义 | 客户端被杀/断网/RST | 已修：`isPeerGoneErrno()` + `closeLogLevel()`（四处收口）→ 记 Warn；用例锁住级别映射（§4.9 问题 2） |
| 16 | 【M6-d】`EPOLLHUP` 时丢掉缓冲区里的数据 | 对端挂断时还有未读数据 | 已修：`onEvent` 改为先读后判（§4.9 问题 3）。**风险**：这条**没有确定性用例**（M7 补，见 §8 未决 12）—— 若被改回去，只能靠并发脚本的日志计数发现 |

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
| 【M6-c】Annex-B → AVCC 放**哪一层** | 解封装层（`DemuxerProducer`）+ 纯函数工具（`h264_util`） | 排除"放 `output/`"（那 `FlvMuxer` 要知道输入可能是裸流，M6-a 的契约与已验证的字节布局都得改）；排除"放 app"（每接一路都要转一次、且不可单测）；放这里的好处是**转完与 MP4 输入同形**，`output/` 一行不改 |
| 【M6-c】裸流的 extradata 从哪来 | 解封装给的 Annex-B SPS/PPS → **现场构造 avcC** | 排除"直接把裸 SPS/PPS 当 avcC 塞进 FLV"（必然黑屏）；排除"缺 SPS/PPS 也硬上"（`open()` 明确失败） |
| 【M6-c】循环的时间轴 | **偏移累加**（一趟接一趟，绝对连续） | 排除"每趟时间戳归零"（下游要看"回退"：`FlvMuxer` 只会钳早于基准的包，回退会让播放端花屏；节流基准也得跟着重设）；副作用是"趟"这个概念对下游完全不存在 |
| 【M6-c】**删掉** `ReadResult::PacketNewSegment`（循环重开时重置节流基准） | 删除 | 曾打算让 `SourcePump` 在重开时重设节流基准，并为此加了枚举值 + 3 行代码。**实测反证**：`--loop --speed 4` 拉 3 秒，加/不加都是约 12.0 秒媒体时长（throttle 是"绝对时间轴 + 落后就放行"，偏移累加下 `due` 与 `elapsed` 同步增长）→ 无可观测差别 = 验证不了的复杂度，删。若将来改成"每趟归零"，才需要重新引入 |
| 【M6-c】`Sink::abort` 与 `Sink::end` **分开** | 两个出口 | 排除"复用 `end`"（`end` = 发结束块 = 告诉客户端"数据完整"，而 `Aborted`/`SinkFailed` 恰恰意味着流残缺 → 会把丢帧掩盖掉）；客户端报"传输被截断"才是事实 |
| 【M6-c】`start()` 里**立刻 drain 一次** | 是 | 排除"只发头"（源已 EOS 时不会有任何唤醒 → 连接挂死，§4.7）；排除"让 app 自己补一次 `onDrain()`"（那样每个调用方都要记得写，且单测覆盖不到） |
| 【M6-c】播放页放**内建** `kTestPage`（而不是 app 自己注册 `/`） | 内建 | 排除"app 注册 `/`"（`examples/http_server` 就没有播放器了，且内建页本就是 SC-1 的落地点）；标题保持 `<title>mzmedia</title>` —— `scripts/http_test.sh` 靠它判断"是我们的页"，是既有契约 |
| 【M6-c】`flv.js` 入库 | `third_party/flv.js/`（Apache-2.0，含 `LICENSE` 与来源/MD5 的 `README.md`） | 排除"页面引 CDN"（离线不可用 → 会被误判成"服务坏了"）；排除"自研 MSE 播放器"（要自己写 FLV demux + fMP4 封装，工作量与收益不匹配）。这也是全仓**唯一**的第三方文件，README/ARCHITECTURE 都写明了 |
| 【M6-c】app 的二进制名 | 目标 `mzmedia_app`，`OUTPUT_NAME mzmedia` | CMake 不允许 target 重名（静态库已叫 `mzmedia`），但用户/脚本看到的必须是 `bin/mzmedia` |
| 【M6-c】`--speed` 暴露给命令行 | 暴露（默认 1.0） | 它是 `FR-3.5` 的既有能力，且是**循环/长流验收的加速旋钮**（脚本 §9 用 4 倍速在 3 秒内验证 6 次循环）；排除"只留给单测"（那样端到端验收就得跑满时长） |
| 【M6-c】空闲释放的判据 | **句柄数 = 0 且订阅者数 = 0**（否则推迟 + 复查） | 排除"只看句柄数"（就是 §4.8 的 bug）；排除"订阅者一断就立刻释放"（句柄与订阅者是两条独立生命周期，硬绑会让"重连同一路"时源被反复关停） |
| 【M6-c】测试分组 | 新增 `producer` 组（依赖样本、单线程、无等待 → 进 TSAN 严格组） | 排除"塞进 `media` 组"（`media` 目前**不需要样本**，混进去会让"没跑 make_samples 就整组红"的范围无谓扩大）；排除"塞进 `srcmgr`"（那是"真起线程 + 定时器"的组，依赖面不同） |
| 【M6-d】验收脚本的数量 | **一个参数化脚本**（`concurrent_test.sh`，默认 5 分钟 + `--quick`） | 排除"并发脚本 + 长跑脚本"两份（采样/起停逻辑会重复 90%，改一处忘一处）；`--duration 3600` 就是同一份脚本跑 1 小时 |
| 【M6-d】判据分层 | **功能性设硬门禁、性能只记基线** | 排除"给 RSS/CPU 设死阈值"：5 分钟斜率噪声大（实测同机同负载 0.36 ~ 20 MB/h 都有），会造成随机翻红；性能基线写进 `TESTING` 供 M7 的 1 小时跑对比 |
| 【M6-d】客户端构成 | **5×curl（纯拉流）+ 5×ffmpeg（边拉边真解码）**，错开 200ms 接入 | 排除"全 curl"（拿不到"码流正确"的证据）；排除"全 ffmpeg"（10 路解码抢 CPU，污染 CPU/RSS 观测）；错开接入顺带压"订阅时序 + GOP 缓存" |
| 【M6-d】客户端被掐断的尾包 | 校验前**先切到最后一个完整 tag**（并报出丢弃字节数） | 排除"直接 ffprobe"（半个 tag 会被当畸形文件 → 假红）；排除"不掐断"（循环流不会自己结束，必须靠 `--max-time`/`-t`） |
| 【M6-d】`/api/stats` 的 `wakeup` 语义 | 聚合**当前在册**订阅者（不是累计） | 排除"累计计数"（要跨模块在 `Subscriber` 通知时回写源，为一个观测值增加写路径）；代价是"源释放后归零"→ 脚本改成**从采样最后一刻取值**并在注释里写明 |
| 【M6-d】读空闲的豁免方式 | 按**显式声明**（`setChunkedAsync()`）豁免该连接 | 排除"按 `keepAlive=false` 猜"（启发式）；排除"把 60s 调大"（治标，且会削弱 M2 的空闲回收）。通用化留 M7（§8 未决 11） |
| 【M6-d】"对端消失"的分级 | 纯函数 `isPeerGoneErrno()` + `Session::closeLogLevel()`，**只改级别不改类型/计数** | 排除"改 `ErrType`（新增 `PeerGone`）"（会动 M2 的计数契约与既有断言）；排除"直接降级所有 send/recv 失败"（`ENOBUFS` 这类是我们的问题，必须留 Error） |

## 8. 未决事项

| # | 事项 | 何时定 |
|---|---|---|
| 1 | `flv.js` 入库的体积/许可证备注（Apache-2.0，需保留声明） | ✅ **M6-c 已落库**：`third_party/flv.js/`（含 `LICENSE` + 来源/版本/MD5 的 `README.md`）；README/ARCHITECTURE 的"零第三方依赖"已改成"**C++ 侧**零第三方依赖，唯一例外是网页播放器" |
| 2 | 是否发 `onMetaData` script tag（flv.js/ffplay 都不强制） | ✅ M6-c 实测：**不发也能播**（flv.js 1.6.2 与 ffplay 都不依赖它）→ 保持**不发**（最小实现） |
| 3 | 是否发 AVC end-of-sequence tag（可选） | 仍未发：靠连接关闭/结束块表示结束（flv.js 正常收到 EOS） |
| 4 | Annex-B → AVCC 转换（裸流输入） | ✅ **M6-c 完成**（`FR-2.1`）：解封装层转换 + avcC 现场构造 + 单测 + `ffprobe`/`ffmpeg`/python 三重验收 |
| 5 | `FlvSender` 的发送节奏：一次 drain 多少 tag（与 M5 唤醒合并的关系） | ✅ M6-b/M6-c：一次 drain 取空队列、32KB 一块；M6-d 在"10 路 / 5 分钟"下拿真实数据回填 |
| 6 | `Session::shutdownAfterFlush()` 的实现方式（等队列排空再关） | ✅ **M6-b**（`DESIGN_M3` §9 未决 5），M6-c 增加 `abortFn()`（硬断连）作为"异常终止"出口 |
| 7 | 5 分钟 / 10 路并发下的真实参数（锁粒度、包数硬顶、唤醒合并、`max_subscribers`） | ✅ **M6-d 完成**：`DESIGN_M5` §8 已逐条回填（#7 不加包数硬顶、#8 不升级锁粒度、#9 唤醒合并够、#13 节流基准不改、#1 保持 16） |
| 8 | 【M6-c 新增】`avcC` 是否写 high profile 的扩展字段（chroma_format 等） | 不写（播放端从 SPS 自己读）。实测 `sample.h264`（baseline）可用；**high profile 的裸流没有样本可验** → 留 M7 补样本再定 |
| 9 | 【M6-c 新增】`--loop` 与"文件被替换"的交互 | 已按"参数变了就明确失败"处理；是否要"检测到变化自动重开"留 M7 定 |
| 10 | 【M6-c 新增】"源 EOS 后接入"这条路径值得再补一个"**GOP 缓存为空**"的用例（当前样本只有 1 个关键帧，缓存里总有东西） | M7（与 M6-d 的 10 路并发一起看：`seed_failed` 计数在 5 分钟跑里始终为 0） |
| 11 | 【M6-d 新增】单向响应的读空闲语义**通用化**（不只异步流式：大文件下载、将来的 HLS 长连接） | M7 单独决策：是"显式声明豁免"（现状）还是"最近有发送活动就不算读空闲" |
| 12 | 【M6-d 新增】`onEvent()` 的"先写 → 再读 → 判错误"顺序**补确定性用例**（可选做法：把它提到 `protected`，或加一个只有测试用的事件注入钩子） | M7（本批已修但只有间接证据，见 §4.9 问题 3） |
| 13 | 【M6-d 新增】NFR-3 原文是 **1 小时**（RSS < 5MB/h、fd 无泄漏、ASAN 无报错） | M7 发版前：`./scripts/concurrent_test.sh --duration 3600` + ASAN 构建各跑一遍 |

## 9. 假设与影响面（`AI_COLLAB.md` §1 的②③）

**假设**：

- 输入是 **MP4/avcC** 或 **H264 裸流（Annex-B，带 SPS/PPS）**；其它容器（MKV/TS）走同一条"extradata 判断"路径，
  但**没有样本实测**（`FR-2.2` 的 MKV/TS 留 M7 补样本验收）。
- 裸流的 extradata 必须**同时**含 SPS 与 PPS（否则 `open()` 明确失败）—— 依赖 FFmpeg 的 h264 parser 先把它们抽出来。
- 一路视频 + 一路音频（多路音频/多轨不在 v0.1 范围）。
- `MediaPacket::ptsMs()/dtsMs()` 已是**毫秒**（M4 换算 + 单调钳制）。
- 时间戳允许为负（首包 dts 实测 -23 ms）→ 基准可以取负值，`dts - base` 才落到 0 附近。
- 每个连接一个 `FlvMuxer` 实例（per-client 基准 + sequence header 重发）。
- 浏览器支持 **MSE**（flv.js 的前提）；不支持时页面会明确写出来（不静默失败）。
- `--loop` 下"一趟"的边界**不需要**下游知道（时间轴连续，见 §4.6）。

**影响面**：

- 新增（M6-a/b）：`src/output/flv_muxer.h/.cpp`、`src/output/flv_sender.h/.cpp`、
  `tests/test_flv_muxer.cpp`、`tests/test_flv_sender.cpp`、`examples/flv_mux_demo.cpp`、
  `scripts/flv_mux_test.sh`、`scripts/flv_http_test.sh`、本文件。
- 新增（**M6-c**）：`src/main.cpp`（应用入口，`CMakeLists.txt` 里 target `mzmedia_app` → `bin/mzmedia`）、
  `third_party/flv.js/`（`flv.min.js` + `LICENSE` + `README.md`）、
  `tests/test_demuxer_producer.cpp`（分组 `producer`）。
- 改动（M6-c）：`src/ffmpeg/h264_util.h/.cpp`（`looksLikeAnnexB` / `annexBToAvcc` / `buildAvcC`）、
  `src/ffmpeg/demuxer.h/.cpp`（`close()`，供循环重开）、`src/media/demuxer_producer.h/.cpp`
  （`Config::loop`、Annex-B 转换、`muxerVideo()/muxerAudio()`、循环统计）、
  `src/media/source_manager.h/.cpp`（`muxerStreamsFor()` 取代 `demuxerFor()`、
  `deferred_recheck_ms` + `tryIdleRelease()` 守卫 + 统计）、`src/output/flv_sender.h/.cpp`（`Sink::abort`、`start()` 立刻 drain）、
  `src/http/http_response.h/.cpp`（`setAbortFn/abortFn`）、`src/http/http_server.cpp`（注入 `abortFn` + 播放页）、
  `CMakeLists.txt`（app 目标 + 安装）、`examples/CMakeLists.txt`（去掉 `flv_http_server`）、
  `tests/CMakeLists.txt`（新文件 + 分组 `producer`）、`scripts/tsan.sh`（严格组）、
  `scripts/flv_http_test.sh`（重写为 49 项）、`docs/ROADMAP.md`、`docs/TESTING.md`、
  `docs/ARCHITECTURE.md`、`README.md`、`CHANGELOG.md`。
- **删除**：`examples/flv_http_server.cpp`（升级成 `src/main.cpp`；留在 `examples/` 会让人以为它只是示例）。
- **不动**：`core/`、`network/` 的现有代码；`media/` 只动 `source_manager` 与 `demuxer_producer`
  （都是"新增能力"，`MediaSource`/`FrameQueue`/`GopCache`/`Throttle`/`SourcePump` 行为未变）。

### M6-d 的影响面（补充）

- 新增：`scripts/concurrent_test.sh`（10 路 × 5 分钟 / `--quick` 两层判据）。
- 改动（都是"修 M6-d 抓到的真问题"，见 §4.9）：`src/core/util.h/.cpp`（`isPeerGoneErrno()`）、
  `src/network/session.h/.cpp`（`closeLogLevel()` + `emitError` 记 `err.what()` + `onEvent` 先读后判 +
  运行期可改空闲阈值）、`src/network/socket.h/.cpp`（`send/recv` 失败分级）、
  `src/network/buffer.cpp`（`readFromFd` 失败分级）、`src/http/http_server.cpp`（异步流式豁免读空闲 +
  `abortFn` 记 Warn）、`src/media/media_source.h/.cpp`（`wakeup` 聚合 + 删掉过期的"独立时间戳基准"占位注释）、
  `tests/test_media_ntimed.cpp`、`tests/test_network_session.cpp`、`tests/test_http_server.cpp`。
- 文档：`docs/DESIGN_M6.md`（§4.9/§5.5/§6/§7/§8/§9）、`docs/DESIGN_M5.md`（§1 + §8 全量回填）、
  `docs/TESTING.md`（§5/§6 重写 + 基线表 + §7 引用）、`docs/ROADMAP.md`、`README.md`、
  `docs/RETROSPECTIVE.md`（§2.7 + 简历素材 + 快照）、`CHANGELOG.md`。
- **不动**：`FlvMuxer`/`FlvSender`/`SourcePump`/`FrameQueue`/`GopCache`/`Throttle`（10 路实测证明它们不需要改，
  M5 未决 #7/#8/#9/#13 的结论都是"不改"）。
