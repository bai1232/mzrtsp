# M4 设计：FFmpeg 封装（v0.1.0 的解封装层）

> 权威契约以 `src/ffmpeg/*.h` 为准；本文件说明"为什么"。行为/形状变更同批同步（§3.8）。

## 1. 范围与验收

| 范围内（v0.1） | 明确不做 |
|---|---|
| `AvPtr` 系列 RAII（context/packet/frame/dict） | 解码（M5+）、编码 |
| `Demuxer`：打开 → 报流信息 → 读包（**不解码**） | 网络输入（RTSP 拉流是 v0.3） |
| `time_base`：整数换算 + 单调守卫 | 浮点时间戳（漂移不可控） |
| `CodecMatrix`：Remux / Transcode / Unsupported 判定（M4-b） | 转码本身（v0.2） |

**验收**（`ROADMAP.md` M4 行）：从样本 MP4 解出正确的流参数；裸流能提取 SPS/PPS 并报出分辨率。

## 2. 分层

```
core → network → http → ffmpeg → media → output
```

`ffmpeg` 只依赖 `core`（日志/时钟）。**它不认识 Session/HTTP** —— 这样 M5 的 `MediaSource`
可以只依赖它，而不会把网络层拖进来。

## 3. 契约

### 3.1 `AvPtr`（`av_ptr.h`）

`AvPtr<T, void(*)(T**)>` + 四个别名（`AvFormatCtx` / `AvCodecCtx` / `AvPacket` / `AvFrame` / `AvDictionary`）。
工厂 `makePacket()` / `makeFrame()` 失败**返回 nullptr + ErrorP**（不抛不静默）。

两个必须记住的 FFmpeg 细节（都写进实现注释）：
1. `avformat_open_input` **失败**时 ctx 仍归调用方 → 必须自己 `avformat_free_context`；
2. FFmpeg 的释放接口统一是 `void f(T**)`（会把指针置空）→ 正好适合 deleter。

### 3.2 `time_base`（`time_base.h`，**不依赖 FFmpeg**）

| 接口 | 契约 |
|---|---|
| `rescaleTimestamp(ts, src, dst, *out)` | 返回 `bool`：时间基非法或**结果溢出 int64** → `false`（用 `__int128` 精确判，不做就近截断） |
| `MonotonicGuard::apply(ts)` | 保证不减；回退时钳到上一次的值并 `++monotonicClampCount()` |

为什么单独拆出来：换算是纯整数逻辑 —— 单测不需要样本、不需要 libav*，能进最快的分组。

### 3.3 `Demuxer`（`demuxer.h/.cpp`）

```cpp
struct Limits { int max_streams = 16; int64_t max_packet_size = 8MB;
                int open_timeout_ms = 5000; int read_timeout_ms = 5000; };
enum class ReadResult { Packet, Eof, Error };     // ★ Eof 与 Error 必须分开
bool open(const std::string &path);               // 失败：false + lastError() + 日志
ReadResult readPacket();                          // Eof 是稳定终态（重复读仍 Eof）
bool packetTimestampsMs(int64_t *pts, int64_t *dts);   // 已换算 + 已钳制
const std::atomic<bool> *setAbortFlag(const std::atomic<bool> *);  // ★M5-c 外部中止（返回上一个）
```

| 硬约束 | 做法 | 为什么 |
|---|---|---|
| 超时 | FFmpeg `interrupt_callback`（虚拟机上也要真能中断） | "读完再比时间"在卡死时根本回不来 |
| **外部中止（M5-c 新增）** | `setAbortFlag()`：`interrupt_callback` 除超时外**再检查该标志** | `SourcePump::stop()` 要 join 源线程；只靠"读超时"得等满 5s 才回来。这仍然**没有**引入"另起线程强制关"（同一个回调里多一个判断而已） |
| 流数上限 | `nb_streams > max_streams` → **拒绝打开** | "只取前 N 路"是静默截断，后面所有统计都对不上 |
| 单包上限 | `pkt->size > max_packet_size` → 拒绝 + 计数 + 释放 | 不缓存、不截断；异常文件不能把内存拉爆 |
| 时间戳 | 换算 + 按流单调钳制；失败/缺失都计数 | 播放器把 dts 回退当跳帧；静默改数据不可接受 |
| 观测 | `totalPackets/totalBytes/rejectedPackets/rescaleFailures/monotonicClamped` + `lastError()` | 每个上限与降级都要能被证明"发生过" |

## 4. 分批

| 批次 | 内容 | 验收 |
|---|---|---|
| **M4-a** ✅ | `AvPtr` + `time_base` + `Demuxer` + `scripts/make_samples.sh` + `tests/test_ffmpeg.cpp`（11 用例） | 样本 MP4 解出 h264 320x240@25fps + aac；读完 138 包到 Eof；时间戳单调；上限用例红/绿可控 |
| **M4-b** ✅ | `CodecMatrix`（`src/ffmpeg/codec_matrix.h/.cpp`）+ `h264_util`（Annex-B 切分 / SPS+PPS 提取 / SPS 分辨率解析） | **已完成**：裸流样本提取出 SPS(22B)+PPS(4B) 并解析出 **320x240**，与 `Demuxer` 对同一份裸流的结果**逐项一致**（两套独立路径互相印证）；HEVC→FLV、未知输出格式 → 明确 `Unsupported`；MPEG2/MP3 → `Transcode` |

## 5. 测试计划

| 分组 | 位置 | 说明 |
|---|---|---|
| `ffmpeg` ✅ | `tests/test_ffmpeg.cpp`（**19 用例**：M4-a 11 + M4-b 8） | 纯函数 + 真样本；无并发无等待 → **TSAN 严格组** |
| 样本 | `scripts/make_samples.sh` ✅ | `sample.mp4` / `sample.h264` / `garbage.bin`，**现场生成不入库**（`.gitignore` 已加 `/samples/`） |

用例覆盖：正常（打开报流参数、读完到 Eof、时间戳单调）、空（未打开就读必须是 **Error 而不是 Eof**）、
满（流数上限、单包上限，都要拒绝 + 计数）、断开（文件不存在、随机字节）、超大（单包超限、换算溢出）。

## 6. 风险清单

| # | 风险 | 触发条件 | 应对 |
|---|---|---|---|
| 1 | 打开网络/坏文件卡死 | 异常输入 | `interrupt_callback` 超时（5s）+ 用例覆盖"打不开" |
| 2 | 上下文泄漏（每失败一次漏一个 ctx） | open 失败路径 | 失败时显式 `avformat_free_context`；ASAN 跑同一批用例 |
| 3 | 单包超大把内存拉爆 | 畸形文件 | `max_packet_size` + 拒绝 + 计数 |
| 4 | 时间戳回退导致播放跳帧 | B 帧/坏流 | 按流单调守卫 + 计数（可见） |
| 5 | 时基为 0（部分容器不填） | 畸形文件 | 换算前检查 `num/den > 0`；失败计数 |
| 6 | FFmpeg 4.x/5.x API 差异 | 换机器 | SPEC 锁定 4.x；`channels` vs `ch_layout` 这类差异写在代码注释里（本机 4.4.2 实测） |

## 7. 决策记录

| 决策 | 选择 | 排除的选项与原因 |
|---|---|---|
| 读包返回值 | 枚举 `{Packet, Eof, Error}` | 排除 `bool`（EOF 与错误混在一起 = 静默提前结束播放） |
| 超时实现 | `interrupt_callback` | 排除"读完再看表"（卡死时回不来）；排除"另起线程强制关"（会引入并发复杂度） |
| 流数超限 | 拒绝打开 | 排除"只取前 N 路"（静默截断） |
| 时间戳换算 | 整数 + `__int128` 溢出判定 | 排除浮点（长片会漂）；排除"截断到 int64"（错值比失败更糟） |
| 单调性 | 守卫钳制 + 计数 | 排除"直接透传"（播放器会跳帧）；排除"静默丢弃回退包"（丢数据不可见） |
| 样本 | 本机 ffmpeg 现场生成 | 排除"下载样本入库"（体积 + 网络依赖 + 版权） |
| CodecMatrix 的"不知道" | 一律 `Unsupported`（+ErrorP），绝不默认 Remux | 假成功最贵：对 HEVC 照 remux 的结果是浏览器一片黑，排查成本极高 |
| HEVC → FLV | `Unsupported`（不是 Transcode） | 播放端（flv.js/MSE）不支持 HEVC，"转码"也解决不了 → 这是契约层面的不做 |
| SPS 解析的契约 | 只承诺**语法解析**；`h264_util.h` 明确写"SPS 无校验和，随机字节可能凑出语法合法结果，**调用方必须交叉校验**" | 让解析器去"猜合法性"是不可靠的（实测 0xff 填充会解析成 16x16）；交叉校验（vs Demuxer）才是真防线 |
| Annex-B 切分 | 只按起始码切分，**不做** emulation prevention 反转义 | 反转义是解码器的职责；FLV 只需要"把 SPS/PPS 原样去掉起始码" |

## 8. 未决事项

| # | 事项 | 何时定 |
|---|---|---|
| 1 | 超时初值（open/read 各 5s）是否合适 | M5 接真实输入后按 `lastError()` 观察 |
| 2 | `max_packet_size` 8MB 是否够（4K 关键帧可能更大） | M5/M6 实测 |
| 3 | ~~SPS/PPS 提取放哪~~ | **已定（M4-b）**：独立的 `h264_util`（便于单测：不需要解码器、手造字节即可测） |
| 4 | 是否支持"只读指定流"（`av_read_frame` 目前返回所有流） | M5 按需要 |
