# 测试与验收

> 原则：**每条需求都要有可执行的验证手段**。规格里的 SC（成功标准）与 NFR（非功能需求）不允许只靠"看起来没问题"来确认。

## 1. 测试分层

| 层级 | 范围 | 手段 | 运行时机 |
|---|---|---|---|
| 单元测试 | 解析、缓冲、时间戳换算、codec 判定、队列 | 自研轻量断言宏（零依赖） | 每次提交 |
| 集成测试 | demux → mux 全链路（不经过网络） | `ffprobe` 校验输出文件的流参数与帧数 | 每次提交 |
| 端到端 | HTTP 服务 → 播放器 | `ffplay` / `curl` / 内置测试页 | 每个里程碑 |
| 稳定性 | 长时间运行 | ASAN 构建 + RSS/fd 采样，1 小时 | 发版前 |
| 压测 | 多客户端并发 | `scripts/bench.sh` | 发版前 |

## 2. 单元测试

自研断言宏（不引入 gtest，保持零依赖 + 离线可构建）：

```cpp
MZ_TEST(logger_level_filter) {
    Logger::Instance().setLevel(LogLevel::Warn);
    MZ_ASSERT_FALSE(Logger::Instance().enabled(LogLevel::Info));
    MZ_ASSERT_TRUE(Logger::Instance().enabled(LogLevel::Error));
}

MZ_TEST(timestamp_rational_conversion) {
    // 1080p 23.976fps 源：第 1000 帧在 90kHz 下的时间戳
    int64_t ts = mzmedia::scaleTo90k(1000, 24000, 1001);
    MZ_ASSERT_EQ(ts, 3753750);   // 1000 * 90000 * 1001 / 24000
}
```

**必须覆盖的点**：HTTP 解析（含分块与畸形请求）、`Buffer` 边界、时间基换算（含 23.976/25/29.97 等有理帧率）、32 位时间戳回绕、`CodecMatrix` 判定表每一行、`FrameQueue` 溢出丢帧策略、日志滚动。

**还要覆盖"记账"与"真的发生了"**（v0.1.2 起，`ntimed` 组为范本）：
- **记账**：`bytesIn` / `bytesOut` / `pendingSendBytes` / `flushCloseTimeoutCount` / `totalAccepted` ——
  "回显内容对得上"不等于"记账对得上"，而 `/api/stats`、容量规划、NFR-3 的 RSS 斜率全依赖记账。
- **"计数涨了" ≠ "副作用发生了"**：例如写阻塞超时要断言**客户端真的收到了 EOF**，而不只是
  `totalIdleTimeout()==1`（只改计数不 `close(fd)` 会 fd 泄漏，NFR-6）。
- **断言要能区分"谁干的"**：超时用例要顺带断言 `totalSendOverflow()==0`（方向断言），
  否则"时间防线"与"字节防线"串了也看不出来。
- **最终一致 vs 立即相等**：`bytesOut` 是「先 `writeToFd`、**之后**才 `fetch_add`」，
  所以客户端刚收到数据时计数可能还是旧值 —— 这类断言必须写成 `waitFor(...)`（实测：
  写成立即相等，单进程全量跑时翻红过一次，组跑时侥幸绿）。

### 2.1 用例之间必须隔离（硬约束）

判据只有一句：**同一个用例，单独跑 与 放在全量里跑，结果必须一致。**

用例之间会共享的东西：工作目录、临时文件、环境变量、全局单例、端口、fd、信号处理。
其中**工作目录**已经踩过：

> **真事故**：`util_file_relative` / `logger_file_relative_path` 必须 `chdir` 到临时目录才能验证
> "相对路径"语义，但收尾写的是 `chdir("/")`（本意是"还原"）——进程工作目录被永久留在根目录。
> 同一进程里后面 **8 个**依赖相对路径找样本的 ffmpeg 用例全部假红，而报出的原因是
> "找不到样本，请先跑 `make_samples.sh`"（样本一直都在）。真凶只有 2 个用例，锅却扣在
> 8 个无辜用例头上。`ctest` 每个分组是独立进程，把这个问题掩盖了 4 个里程碑。
> 详见 `docs/RETROSPECTIVE.md` §2.4。

规矩：

1. 凡要 `chdir` 的用例，一律用 `::mztest::ScopedCwd`（`tests/test_main.h`）包住，**禁止手写还原**；
2. 框架会在每个用例结束后检查工作目录：被改动就**当场点名该用例**并立刻还原
   （替代"让后面某个无辜用例背锅"，也避免一个污染源把后面几十个用例连带染红）；
3. 新增用例时自问：**它给后面的用例留下了什么状态？**

### 2.2 单进程全量跑不是"计时门禁"

| 跑法 | 用途 |
|---|---|
| 分组跑（`ctest`）/ `scripts/tsan.sh` | **验收门禁**（每个分组独立进程，天然隔离） |
| 单进程跑全部（`./build/bin/mzmedia_unittest`） | **诊断手段**：查"用例之间是否串味" |

计时类用例在单进程上下文里会受宿主调度影响。实测 `timer_precision`（判据是
`min(库) ≤ min(裸) + 5ms`，只差 1ms 就会翻面）：

| 跑法 | 实测结果 |
|---|---|
| 单独跑该分组（`ctest -R '^timer$'`） | 连续 3 次**全绿**（3.5s）；ASAN 构建下同样 3/3 绿 |
| **并行**跑全部分组（`ctest -j4`） | **红**（4 个测试进程抢 CPU） |
| 单进程跑全部用例 | 见过红（2 次红、多次绿） |
| **ASAN 构建**下串行 `ctest` | 红过 1 次（随后该组单独跑 3/3 绿 → 同一间歇性） |

**规矩**：计时结论只在**串行、无干扰**下取 —— 用 `ctest --output-on-failure`（不带 `-j`，与
`VERSIONING.md` 发版清单一致），或单跑该分组。**不要**用 `ctest -j4` / 单进程全量跑的结果当精度指标。

【推论】红的根因判为宿主调度/CPU 争用与 sanitizer 插桩开销，**未做对照实验**（依据：① 同一二进制
单跑绿、并行红；② 本节 §8 R11 已实测本宿主唤醒延迟 0~14ms 且与延时长短无关；③ 该门禁比较的是
**两个独立噪声分布的最小值**，+5ms 余量本身就偏紧）。
**另案（待批）**：`DESIGN_M2.md` §9 已有先例把它在 **TSAN** 下放宽到 25ms；ASAN 也需要同样的处理
（或改成与"同相位裸基线"对拍），否则发版清单里的 ASAN 一栏会随机翻红。见 `docs/DESIGN_M2.md` §8 R11。

## 3. 集成测试（ffprobe 校验，可自动化）

**M6-c 起：全部走脚本**（不要手敲命令 —— 脚本会同时验证"浏览器要的东西"和"字节对不对"，且会自己起停服务）：

```bash
./scripts/flv_http_test.sh     # M6-c/M8/M10-b 端到端 96 项（HTTP + chunked + ffprobe + 播放器资源 + 循环 + 边界 + MKV/TS 与 ADTS 结构 + 转码开关/缩放/HEVC/并发上限/speed）
./scripts/flv_mux_test.sh      # M6-a 封装链 8 项（demux → FLV → 落盘 → ffprobe）
./scripts/ts_mux_test.sh       # M9-a TS 封装 29 项（demux → TS → 落盘 → ffprobe/ffmpeg 四层判据）
./scripts/hls_test.sh          # M9-b/c HLS 32 项（m3u8/切片/Range/窗口滚动/租约释放/播放页与 hls.js + ffmpeg 读 m3u8）
./scripts/http_test.sh         # M3 HTTP 层（含 Range / chunked / 统计）
./scripts/echo_test.sh         # M2 网络层（100MB 回显）
```

手工复现（脚本里的关键几步）：

```bash
# 1) 起服务（零参数即可：0.0.0.0:8080 + ./samples）
./build/bin/mzmedia &

# 2) 拉流存盘（HTTP-FLV 就是 FLV，可以直接落盘）
curl -sN --max-time 30 http://127.0.0.1:8080/live/sample.flv -o /tmp/out.flv

# 3) 校验输出流：编码、分辨率、音频、帧数
ffprobe -v error -show_entries stream=codec_name,width,height,sample_rate,channels \
        -of default=noprint_wrappers=1 /tmp/out.flv
ffprobe -v error -count_frames -select_streams v:0 \
        -show_entries stream=nb_read_frames -of csv=p=0 /tmp/out.flv

# 4) 完整解码（**不许有任何 error 输出**）
ffmpeg -v error -i /tmp/out.flv -f null -

# 5) H264 裸流走同一套（`sample.h264.flv` → `<media-root>/sample.h264`）
curl -sN --max-time 30 http://127.0.0.1:8080/live/sample.h264.flv -o /tmp/raw.flv
ffprobe -v error -show_entries stream=codec_name,width,height -of default=noprint_wrappers=1 /tmp/raw.flv
ffmpeg -v error -i /tmp/raw.flv -f null -
```

**判定标准**：输出流参数与源一致（`sample.mp4` → h264 320x240 + aac 44100）；帧数与源一致（±2）；
`ffmpeg` 转存过程**零错误输出**；裸流那条同样要求（avcC 构造错了这里必红）。

### 3.1 `NFR-1`（首帧 < 1s）怎么量

- **协议层（自动，脚本 §4）**：后台 `curl -N` 存盘，前台每 20ms 看一次文件大小，长到 **2KB**
  （≈ FLV 头 + sequence header + 第一帧的一部分）即认为"首帧已到"。粒度 20ms，用来抓
  "要等一秒才出画面"这类真问题；**它不追求毫秒级精度**（那需要播放器埋点）。
  实测：**20ms 级**（`curl -w '%{time_starttransfer}'` 的 TTFB 是 3.3ms）。
- **播放器层（人工）**：浏览器 DevTools → Network → 该 FLV 请求的 **Time to First Byte**，
  以及页面 `#err` 区域是否为空（页面会把 flv.js 的加载/播放错误直接写出来）。
- 用**冷启动**的媒体文件量（脚本里每个阶段各用一份复制文件）：源读过一遍就 EOS 了，
  再量到的是"缓存回放"，不是首帧延迟。

## 4. 端到端（播放器）

| 播放端 | 命令 / 方式 | 关注点 |
|---|---|---|
| ffplay | `ffplay -f flv http://127.0.0.1:8080/live/sample.flv` | 出画面、有声音、无卡顿 |
| 浏览器（内置页） | `http://127.0.0.1:8080/` → 填媒体名 → 点「播放」 | flv.js **入库**（`/flv.min.js`）能播、CORS 正常、首帧 <1s；页面上会显示 `flvjs.version`（应为 **1.6.2**）。若 flv.js 没取到，页面会**明确写出原因**（不是"点了没反应"） |
| 浏览器（循环） | `./build/bin/mzmedia --loop` 后打开同一页 | 播完自动从头接上；时间戳连续（不会回退花屏） |
| curl（协议层） | `curl -N -v http://127.0.0.1:8080/live/sample.flv \| head -c 1k \| xxd` | 前 9 字节是 `FLV\x01`；chunked 头正确 |
| hls（v0.2 ✅） | `ffplay http://127.0.0.1:8080/hls/<name>.m3u8`；浏览器打开 `/` → 点「播放（HLS）」 | 切片连续、无 404；页面显示 `hls.js 1.5.17 已就绪`（取不到会**明确写原因**，不是点了没反应） |

**没有浏览器时的替代验证**（脚本 §1 就是这么做的）：`GET /` 的正文里必须有 `<video>` /
`flv.min.js` / `flvjs.createPlayer` / `/live/`，且 `GET /flv.min.js` 与入库文件**逐字节一致** ——
这能证明"浏览器要的东西确实能取到"，但**不能**证明真实渲染（那需要人看一眼）。

## 5. 稳定性与并发（`scripts/concurrent_test.sh`）

```bash
./scripts/concurrent_test.sh --quick         # 10 路 × 20s：回归门禁（功能性硬判据）
./scripts/concurrent_test.sh                 # 10 路 × 5 分钟：NFR-2/3/4/6 + 性能基线（M6-d 验收）
./scripts/concurrent_test.sh --kill-half     # 专项：中途 kill 一半，其余路必须不受影响
./scripts/concurrent_test.sh --overload 8    # 专项：压 max_subscribers（第 17 路起必须被拒）
./scripts/concurrent_test.sh --duration 3600 # 1 小时（NFR-3 原文；M7 已实跑，见验收报告 §3.3）
```

**判据分两层**（性能不设死阈值 —— 5 分钟的 RSS 斜率噪声大，拿它当门禁会随机翻红）：

| 类别 | 判据 |
|---|---|
| **功能性（硬门禁）** | 每路都拿到**完整可解码**的码流（h264 + 帧数在预期区间 + `ffmpeg -f null -` 无 error）；运行期间**同时在线 = N 路**（NFR-2）；断开后订阅者归零/源被释放、fd 回落（NFR-6）；服务端**错误日志 0 条**（NFR-3）；`--kill-half` 后其余路不受影响且被 kill 的自动退订；`--overload` 的接受/拒绝数与 `max_subscribers` 一致 |
| **性能（只记基线）** | 服务端 CPU（单核%）、RSS 斜率（含折算 MB/h）、fd 曲线、`wakeup`（`notify`/`coalesced`/`popped`）、源端包/s、丢帧计数、音画偏移峰值 |

**M6-d 实测基线**（10 路 × 300s，`--loop` + 2 秒样本 = **150 次循环重开**，4 核机器）：

| 指标 | 实测 | 对照要求 |
|---|---|---|
| 每路码流 | 10/10 完整可解（视频 7426 帧 / 预期 7500，容差内） | NFR-2 ✅ |
| 服务端 CPU | **单核 4.11%**（整机 1.03%） | NFR-4「单路 < 5% 单核」——这是**十路合计** ✅ |
| RSS | 基线 25.1MB → 结束 35.1MB（订阅者队列/GOP 预分配）；**稳态段 232s 仅 +24 kB → 折算 0.36 MB/h** | NFR-3「< 5MB/h」✅（折算口径；1 小时真跑留 M7） |
| fd | 运行期恒为 30；断开后回落到基线 19 | NFR-6 ✅ |
| 丢帧 | `dropped=0`、`dropped_incoming=0`（10 路 1x 下队列余量充足） | FR-5.1/5.2 未触发（慢客户端场景由单测覆盖） |
| 唤醒合并 | `notify` 30687 / `coalesced` 10249（33%）/ `popped` 41130 → **1.4 包/唤醒** | 回填 `DESIGN_M5` §8 #9：够用 |
| 吞吐 | 源端 ≈68 包/s（每路 6.8 包/s） | 回填 #7（不需要包数硬顶）、#8（不需要升级锁粒度） |
| 音画 | 视频跨度 299.960s、音频 299.981s、**偏移峰值 2.0 ms** | 回填 #13（混合 dts 对齐不漂；一帧 = 40ms） |
| 错误日志 | `[E]`/`[F]` = 0 条（Warn 12 条，都是"对端消失"） | NFR-3 ✅ |

> **怎么读这张表**：`--loop` 让源在 5 分钟里重开 150 次，所以 RSS 平坦同时证明了"直播推流不涨内存"
> 和"循环重开不漏内存"；而 CPU 是在**客户端解码另占 CPU** 的情况下测的服务端进程时间 → 偏保守。
> 完整原始采样在 `/tmp/mzmedia_concurrent/{samples.csv,perf.txt,server.log}`。

### 5.1 长跑必须用 ASAN 再跑一遍（NFR-3 的"ASAN 无报错"）

```bash
cmake -B build-asan -DMZMEDIA_ENABLE_ASAN=ON && cmake --build build-asan -j4
MZMEDIA_SERVER=./build-asan/bin/mzmedia ./scripts/concurrent_test.sh --quick   # 先冒烟
# ASAN 版本的长跑：M7 用 ASAN 跑了冒烟（--quick）；1 小时跑的是 **Debug 构建**（见验收报告 §3.3 与 §6 第 12 项）
```

## 7. 需求 → 验证手段对照

| 需求 | 验证手段 |
|---|---|
| SC-1 浏览器能播 | 内置测试页 + flv.js（第 4 节） |
| SC-2 加格式不改核心 | 代码走查：新增 sink 是否只实现 `IMediaSink` 且未改动 `media/`、`network/` |
| SC-3 多客户端不崩 | 并发/长跑脚本（第 5 节）：10 路并发，每路码流完整可解、无崩溃 |
| SC-4 1 小时稳定 | 第 5 节（`--duration 3600`，**M7 已实跑并写进 `ACCEPTANCE_v0.1.0.md` §3.3**） |
| NFR-1 首帧 <1s | 浏览器 Performance 面板 / 日志打点（请求到首字节） |
| NFR-4 remux CPU <5% | `top -H -p $(pgrep mzmedia)` 观察单路负载 |
| NFR-5 转码 ≥1.0x（**M10-a ✅ 组件级 / M10-b ✅ 服务内**） | 口径 = **1080p→720p、x264 veryfast**、本机 **4 核**；`speed = 已输出媒体时长 / **转码本身**耗时`（**不是**整段墙钟：源按墙钟推流，那样算永远 ≈1.0，量到的是节流速度 —— 实测 0.97x）。**实测 2.61x / 2.89x**（两次：`/usr/bin/time` 墙钟 1868 ms / 1688 ms 对媒体 4880 ms；74 帧/s），判据 ≥1.0 ✅。独立性：`ffprobe` 认 1280x720 / h264 / 25 fps，`ffmpeg -f null -` 零 error，125 帧一帧不少。**M10-b ✅** 已把 `speed` 暴露到 `/api/stats` 并在**服务进程内**实测（1080p→720p **3.26x**）——服务端不达标就如实写「未达成」，不放宽口径 |
| NFR-6 资源回收 | 断开前后对比 `/api/stats` 与 `/proc/<pid>/fd`；单测 `media`：订阅者 `shared_ptr` 释放后由 `weak_ptr` **自动注销**、其队列被回收 |
| ROADMAP M2 验收：定时器精度 | 单测 `timer` 分组（`tests/test_network_timer.cpp`，用例清单见 `DESIGN_M2.md` §7.1）：**0 早触发** + 相对宿主裸 `nanosleep` 基线增量 ≤5ms。**绝对 ±10ms 在本宿主实测不可达**（裸 `nanosleep` 自身的超出量就有 0~14ms，且与延时长短无关），原始数据见 `DESIGN_M2.md` §7.1 / §8 R11 |
| FR-5.1 每客户端队列上限（**只按字节**：码率上限 × 延迟额度） | 单测 `media` 分组（`tests/test_media.cpp`，清单见 `docs/DESIGN_M5.md` §5）：`setLimits(0)` / 超硬上限被拒；推导值 = `码率 × 时长 / 8000`（用例断言 8 Mbps × 2 s = 2,000,000）；上限不可绕过（`bytes() ≤ max_bytes`） |
| FR-5.2 慢客户端丢帧 | 单测 `media`：溢出丢**最旧的一段**（音视频成对 —— 用例断言一次丢掉音频与视频各一包），**唯一不可丢的是视频关键帧**；腾不出空间 → `RejectedNoSpace`（调用方断开该订阅者）。集成：`tc` 限速或 `kill -STOP` 阻塞客户端，观察 `dropped` 计数与其他客户端 |
| FR-5.3 隔离性 | 单测 `media`：一个订阅者（一直不取帧）持续溢出，其他订阅者的帧序列仍**逐帧一致**（每帧指针相同） |
| FR-6.1 `/api/stats` | 单元：`MediaSource::dumpStatsJson()` / `SourceManager::dumpStatsJson()`（每源订阅数、丢帧、broken 列表、推导上限）；HTTP 层用 `setExtraStatsProvider()` 拼接（用例 `ntimed_http_api_stats_extra_provider` 验证片段真的进了 JSON 且括号配平）；M7 的 main 把两者接起来后，用 `curl -s /api/stats` 与压测前后对账 |
| FR-1.2 直播式重放（按需启动、不支持 seek） | 单测 `srcmgr`：**懒启动**（未 acquire 时源数为 0，acquire 才开文件 + 起源线程）；从文件头开始推（新订阅者首包是视频关键帧）。按墙钟节流的能力见下一行 FR-3.5 |
| FR-3.5 节流（按源时间轴与墙钟对齐，不得以磁盘速度灌入） | 单测 `media`：`Throttle` 按 dts 等待（`speed=1` 时 40ms 处的包真的等 ~40ms；`speed=100` 时 500ms 处的包几乎不等）；`enabled=false` 立即放行；**中止标志置位立刻返回 false**（不睡满，否则 `stop()` 的 join 要等满一拍）。集成：单测 `srcmgr` 用 **8 倍速**读 2 秒样本，断言用时 **≥100ms**（证明没有全速灌入）且 **<1500ms**（没有慢到实时） |
| FR-2.1 v0.1 输入 MP4 / H264 裸流 | 单测 `srcmgr`：`DemuxerProducer` 真读 `samples/sample.mp4`（320x240 H264 + AAC）到 EOF，视频 dts 单调、关键帧 1 个、音视频包都 > 0；H264 裸流由 M4 的 `ffmpeg` 分组覆盖 |
| FR-6.3 关键事件落日志 | 源创建 / 空闲释放走 `InfoL`（`SourceManager`）；打不开走 `WarnL` 且**不注册源**（`srcmgr_open_failure_not_silent` 断言 `lastError()` 非空） |
| FR-4.4 连接上限 / 读空闲 / 写阻塞（**v0.1.1 起含「声明 close 必须真关」；v0.1.2 起含记账与「真关」断言**） | 单测：连接上限设为 1 时第 2 个连接被拒且 `totalRejected()` 增长、**`totalAccepted()` 仍为 1**（拒绝不计入接入）；`recv_idle=50ms` + 连上不发数据的客户端 → 阈内断开且 `onError` 为超时；`send_blocked=50ms` + 只连不读的客户端 → 断开且 `bytesOut` 停止增长 |
| FR-3.1 HTTP-FLV（chunked、首包即 sequence header） | **M6-a ✅**：单元 `flv` 组逐字节断言 header/tag/sequence header；集成 `scripts/flv_mux_test.sh` —— 产物经 `ffprobe` 认成 **h264 320x240 + aac**、解码 **50 帧 / 2 秒**、`ffmpeg -f null -` 完整解码无 error（**8/8**）。M6-b：`curl -N http://…/live/x.flv` 拿到合法 FLV |
| FR-3.3 AAC 转发 / 无音频也能播 | 单元 `flv`：AAC sequence header（`0xAF` + AudioSpecificConfig）与 raw 包布局；另有**纯视频**用例（`Streams{video, nullptr}`）证明"没有音频不报错" |
| FR-3.4 时间戳基准（FLV → 毫秒） | 单元 `flv`：`CompositionTime = pts - dts`（含负值的 24 位补码）；**per-client 基准**（中途接入的时间戳也从 0 开始）；32 位自然回绕；早于基准的包钳 0 并计数 |
| **FR-4.5** 404/405/500 规范响应 + 日志（v0.1.1 起含「错误响应之后连接怎么办」） | 单测 `ntimed_http`（`tests/test_http_server.cpp`）：每条用例都断言 **status + body + 关键头**，并**断言这一步之后连接是开还是关** —— `400/414/431`（解析错误）必须读到 **EOF**（`waitEof`）；`404/405/500/501`（合法请求）必须 `stillOpen`（`poll` 给 50ms 观察窗，避免"误关的 FIN 晚到"被放过）。计数不变式：`totalMalformed()` **每连接 +1**、`total4xx()/total5xx()` 与响应数一致、`totalCloseFallback()` 恒为 0。端到端：`scripts/http_test.sh` 的 404/405/501/400 四项 |
| FR-4.1 `/live/<name>.flv` 路由 | **M6-c ✅**：`scripts/flv_http_test.sh` **49/49** —— `curl -N` 拉到 **55,623 字节**合法 FLV（`ffprobe` 认 h264 320x240 + aac、解码 50 帧；`ffmpeg -f null -` 无 error）；`Content-Type: video/x-flv` + chunked + CORS；名字映射支持白名单扩展名（`.mp4/.h264/…`）；不存在 / 目录穿越 / 非法名 / 非白名单扩展名一律 404（并用**不同提示语**区分"路径非法"与"文件不存在"） |
| NFR-1 首帧 < 1s | **M6-c ✅**：协议层用脚本 §4（"文件长到 2KB"近似首帧到达，实测 **20ms 级**，TTFB 3.3ms）；`FlvSender::start()` 在路由处理器内**同步**发出 FLV Header + sequence header 并**立刻搬一次队列**（不等第一帧，FR-3.1）；浏览器侧看 DevTools 的 TTFB（第 3.1 节） |
| FR-2.1 裸流 → FLV（Annex-B → AVCC） | **M6-c ✅**：纯函数单测 `ffmpeg` 组 7 例（逐字节转换、avcC 逐字段、拒绝畸形）；`producer` 组断言快照是 avcC 且 `FlvMuxer::prepare()==Ok`、每个包都是合法 AVCC；端到端 `scripts/flv_http_test.sh` §3 用 `ffprobe`+`ffmpeg`+**python 逐 tag 复查 AVCC 结构**三重判定 |
| FR-7.1 / FR-7.2 命令行与零参数启动（"日志级别"是 FR-7.1 里的参数之一） | **M6-c ✅**：`./build/bin/mzmedia` 零参数启动并在横幅打印播放页/拉流/统计 URL（脚本 §0 断言横幅里有 URL）；`--port/--media-root/--web-root/--loop/--speed/--log-level/--help`；**未知参数直接报错退出**（不静默忽略） |
| `--loop`（循环播放，默认关） | **M6-c ✅**：单测 `producer_loop_offsets_timestamps_monotonically`（跨趟**视频与音频各自严格递增**、偏移≈样本时长、`loopFailures()==0`）；端到端脚本 §9（4 倍速拉 3 秒 → **11 秒**媒体时长 / 297 帧 / `loops=6` / `ffmpeg` 解码无 error） |
| 源结束后的行为（不挂连接） | **M6-c ✅**：单测 `flv_sender_start_drains_already_ended_source`；端到端脚本 §8（修前 `curl rc=28` 挂满超时，修后 **12ms** 正常结束） |
| 空闲释放不打断观看 | **M6-c ✅**：单测 `srcmgr_idle_release_waits_for_subscribers`（有订阅者 → 推迟并计数；订阅者一走 → 下一次复查释放） |
| **FR-2.2** MKV / MPEG-TS 输入（**M8 ✅**） | 样本：`make_samples.sh` 从 `sample.mp4` `-c copy` 转出 `sample.mkv` / `sample.ts` / `vonly.ts`。单测：`ffmpeg` 组 5 例纯函数（ADTS 头位域、ASC=12 08/12 10/11 90、9 字节 CRC 头、畸形拒绝、多帧剥头）+ 1 例**与真实 TS 交叉校验**；`producer` 组 2 例（`producer_ts_aac_gets_asc_and_strips_adts`：`open()` 内 ASC 已就绪 + 与裸 `Demuxer` 的原始包长**逐字节对账**；`producer_ts_video_only_still_has_no_audio`：纯视频 TS 不因"造不出 ASC"而失败）。端到端：`flv_http_test.sh` **§3b** 17 项 —— MKV/TS 各出合法 FLV（`ffprobe` 认 h264 320x240 + aac 44100、50 帧、`ffmpeg` 零 error），外加 python 逐 tag 复查 `ASC=12 08` 且 **raw AAC tag 不带 ADTS 同步字**（`audio_tags=89 seq=1 raw=88`）。**变异**：不剥头 / 不造 ASC → `producer` 用例双红 |
| **FR-3.2** HLS 切片与播放列表（**M9-a ✅ TS 封装 / M9-b ✅ 切片器 / M9-c ✅ 浏览器播放页**） | **已验**：`scripts/hls_test.sh` **32/32** —— ① 切片语义（M9-b）：m3u8 头部三要素、切片 URI 形态、窗口 ≤6 且 MEDIA-SEQUENCE 前进、老切片 404、**`ffmpeg` 直接读 m3u8 解码 4 秒零 error**；② 浏览器播放页（M9-c）：`GET /` 含 `hls.min.js`/`play-hls`/`Hls.isSupported`/`/hls/`、`GET /hls.min.js` 与入库文件**逐字节一致**（413,952 字节）、页面内联 JS 过 **`node --check`**（页面语法坏了不会红在 HTTP 断言上）。单测 `hls` 组 6 例（切点只认视频关键帧、窗口/序号、TARGETDURATION 取实测最大、ENDLIST 幂等、空列表、非法配置）；TS 封装：`scripts/ts_mux_test.sh` **29/29** + `mpegts` 组 5 例。⚠ **仍未验**：真实浏览器渲染（需要人看一眼） |
| **FR-4.1** `/hls/<name>.m3u8` 与 `/hls/<name>-<seq>.ts` 路由（**M9-b ✅**） | `scripts/hls_test.sh`：m3u8 = 200 + `application/vnd.apple.mpegurl`；切片 = 200 + `video/mp2t`；名字规则与 `/live/` 同一套（`mediaNameToPath` 白名单）；非法路径 404。原计划： 计划：新增 `scripts/hls_test.sh` —— m3u8 返回 200 + `Content-Type: application/vnd.apple.mpegurl`；切片 200 + `video/mp2t`；不存在 / 目录穿越 / 非法名 / 非白名单扩展名 → 404（与 `/live/` 同一套名字映射） |
| **FR-4.2** Range（**HLS 切片**，**M9-b ✅**） | `scripts/hls_test.sh` 对切片发 `Range: bytes=0-187` → **206** + `Content-Range: bytes 0-187/<total>` + `Accept-Ranges: bytes` + 响应体 **188 字节**。实现复用 `HttpServer` 的 `serveMemoryAsStatic()`（与静态文件同一套 Range 语义）。原计划： 计划：对 `-<seq>.ts` 发 `Range: bytes=a-b` → 206 + `Content-Range`；越界 → 416；响应带 `Accept-Ranges: bytes`。现有 `http_test.sh` 已覆盖通用 Range，但**必须对切片再验一次**（切片来自内存/磁盘的实现不同） |
| **FR-3.4** 时间戳基准（**TS → 90kHz**，⏳ **v0.2 计划**） | 计划：单测断言 `ms → 90kHz` 用整数有理换算（`time_base.h`）且丢帧 0；集成 `ffprobe -show_packets` 断言切片内 dts 单调、**跨切片接续**（不因切片边界回退） |
| **FR-6.1** 实时倍数 / **NFR-5** 转码 ≥1.0x（**M10-a ✅ 组件级 / M10-b ✅ 服务内**） | 两层判定：①**组件自证**（`transcode` 组 3 例，`tests/test_transcoder.cpp`）——拒绝路径 5 类（不认识的编码 VP9 / HEVC 缺初始化数据 / 缺 avcC / 只给宽或高 / `max_out_packets=0`、`gop_frames=0`）、真样本 320x240→160x120 全量转码（输出是合法 AVCC、avcC 现场构造、**解码 25 帧 → 编码 25 包**、dts 严格单调、`scaling()` 懒创建、`flush()` 幂等、空包拒绝）；②**外部裁判**（`scripts/transcode_test.sh` **12 项**）——`ffprobe` 认 1280x720 / h264 / 25 fps / ≈5 s、`ffmpeg -f null -` 零 error、**逐包 dts 单调**、"不缩放"路径保持源分辨率、**NFR-5 speed ≥ 1.0x（实测 2.61x / 2.89x）**。✅ **M10-b 已补服务内实测**：`/api/stats` 的 `producer.transcode.speed`（`speed = 已输出媒体时长 / 转码耗时`；`elapsed` 含节流等待，**不能**当分母 —— 那样永远是 ≈1.0）。实测 1080p→720p **3.26x**（媒体 5000 ms / 转码耗时 1533 ms）、320x240→160x120 **26–31x**；`flv_http_test.sh` §11.5 断言 `speed ≥ 1.0` |
| **FR-1.3** lazy 启动 + 空闲释放 + **转码并发上限**（**HLS ✅ / 转码 ✅ M10-b**） | ①HLS 的「客户端」是拉流请求，所以用**租约**：`scripts/hls_test.sh` 用 `--hls-lease-ms 2000` 起实例 → 拉一次 m3u8 后 `hls.streams=1`，超过租约再查 → **`streams=0` 且 `lease_expired≥1`**。②转码源同样是**懒启动**（`acquire()` 才 open + 建转码器），且有**硬上限**：`srcmgr` 组 3 例（`srcmgr_transcode_source_is_separate_from_passthrough` / `srcmgr_transcode_concurrency_is_capped` / `srcmgr_transcode_config_rejects_zero_limit`）断言"转码源与直通源是两个源、同规格复用、第二路撞上限被拒（`totalTranscodeRejected`↑、**不降级成直通**）、放掉后名额回来、`max_transcoding=0` 与超硬上限都被 `setConfig` 拒"；②端到端 `flv_http_test.sh` §11.4（`--transcode-max 1` 时第二路 **503**）+ stats 的 `transcoding`/`transcode_rejected`。**EOS 的源不占名额**（实测踩到：2 秒样本播完后仍被判成"正在转"，后续请求全 503） |
| **FR-7.1** 命令行**转码开关**（✅ **M10-b**） | `--transcode`（默认**关**）、`--transcode-height`（0 = 只重编码；否则 [120,2160] 偶数）、`--transcode-max`（1..64，0 非法）。端到端（`flv_http_test.sh` §10/§11 **28 项**）：未开转码时 `?transcode=1` → **501**（**不静默降级成直通**）；开了才转；`h` 非数字/奇数/64/9999、`transcode=abc`、未知参数 `foo=1`、参数重复 → **全部 400**；`/hls/*?transcode=1` → **400**（转码+HLS 属 M10-c）。**不转码时 HEVC 仍 415**（§11.3 实测） |
| 各 codec 组合 | `CODEC_MATRIX.md` 每个组合一条 ffprobe 用例 |

## 8. 并发检查（TSAN）

并发组件（`TaskQueue` / `ThreadPool` / `Semaphore`，以及后续的 `EventPoller`）必须过
ThreadSanitizer —— 数据竞争不会在单线程测试里暴露，只会以"偶发崩溃"的形式出现在生产环境。

```bash
./scripts/tsan.sh              # 配置 + 编译 + 检查
./scripts/tsan.sh --no-build   # 复用已有 build-tsan
```

### 8.1 必须用 `setarch -R`（环境限制）

本机 TSAN 与高熵 ASLR 冲突，直接运行会立刻 FATAL：

```
FATAL: ThreadSanitizer: unexpected memory mapping 0x...
```

这不是代码问题，而是 TSAN 与内核 ASLR 熵位的已知冲突。`scripts/tsan.sh` 已统一用
`setarch -R` 关闭该进程的地址随机化（不需要 root）；也可临时降熵
`sudo sysctl vm.mmap_rnd_bits=28`。

### 8.2 已知误报：带超时的等待（**已在 GCC 11 环境下根治**）

> **【2026-10-03 更新】已根治**：装上 `g++-12`（12.3.0）后本机实测**全部组 0 报告**
> （含此前稳定误报的 `qtimed`/`ptimed`，以及间歇误报的 `semaphore`）。`scripts/tsan.sh`
> 会自动优先使用 `g++-12`。下面记录的是 **GCC 11 环境下的现象与判据**，保留用于兼容
> 没装 `g++-12` 的机器（那种机器上本组会间歇性报 `double lock` / `data race`）。

glibc 2.35 把 `std::condition_variable` 的超时接口（`wait_for` / `wait_until`）
实现为 `pthread_cond_clockwait`，而 **GCC 11 的 libtsan 没有该拦截器**：

| 符号 | libtsan 中的拦截器数量 |
|---|---|
| `pthread_cond_timedwait` | 2 |
| `pthread_cond_clockwait` | **0** |

于是 TSAN 看不到超时等待内部的"解锁 → 睡眠 → 重锁"，误以为线程仍持锁，
从而误报 `double lock of a mutex` 与随之而来的 `data race`。

**最小复现**（30 行、完全正确的双线程程序）：

```cpp
std::thread t([] { sleep 50ms; { lock; ready = true; } cv.notify_one(); });
std::unique_lock<std::mutex> lck(mtx);
cv.wait_for(lck, 1s, [&] { return ready; });   // → TSAN 误报 2 处
cv.wait(lck, [&] { return ready; });           // → TSAN 0 报告
```

### 8.3 应对方式：分组 + 签名判定（不做抑制）

抑制会把真竞争一起藏掉，因此 `scripts/tsan.sh` **不做任何 suppression**，而是：

1. **分组**：使用超时等待的用例单独命名并单列分组（`qtimed_*` → 组 `qtimed`，
   `ptimed_*` → 组 `ptimed`）；其余分组（`selftest`/`util`/`logger`/`queue`/`pool`/`semaphore`）
   是**严格组，必须 0 报告**，否则脚本失败。
2. **签名校验**：误报组允许有报告，但必须满足两个特征，否则按真问题处理：
   - 报告类型只能是 `double lock of a mutex` 或 `data race`
   - 每条 `data race` 的**两个访问点都带 `(mutexes: ...)` 标注**
     —— 双方都持锁却报竞争，才是"TSAN 丢失 happens-before"的误报特征

### 8.4 签名判定为什么可信（已实测）

真竞争一定有一方**没持锁**，其访问点不会带 `(mutexes: ...)` 标注：

```
Write of size 4 ... by thread T1 (mutexes: write M9):   ← 持锁写
Previous read of size 4 ... by main thread:             ← ★无标注 = 真竞争
```

实测：该真竞争样例产生 2 条 `data race`，带标注的访问点只有 3 个 < 阈值 4，
脚本**正确判定为真问题**；而 `qtimed` / `ptimed` 的已知误报标注数均达到阈值，被正确归类为误报。
即：这套规则既能过滤噪音，也没有失去发现真竞争的能力。

### 8.5 根治方案

换掉 sanitizer 运行时即可彻底消除误报（之后可把 `qtimed`/`ptimed` 并入严格组）：

```bash
sudo apt install g++-12
cmake -B build-tsan -DMZMEDIA_ENABLE_TSAN=ON -DCMAKE_CXX_COMPILER=g++-12
./scripts/tsan.sh --no-build
```

### 8.6 当前基线

**v0.1.0 发布时实测**（`./scripts/tsan.sh`，g++-12 运行时；**全绿，0 报告**）：

| 分组 | 用例数 | TSAN 报告 |
|---|---|---|
| 严格组（20 组）：`selftest` 4 / `util` 13 / `logger` **14** / `queue` 15 / `pool` 6 / `semaphore` 5 / `core` 12 / `poller` 16 / `timer` 20 / `buffer` 13 / `http` **43** / `ntimed_http` **24** / `ffmpeg` **35** / `media` 24 / `ntimed_media` 6 / `srcmgr` 12 / `flv` 20 / `producer` **10** / `mpegts` **5** / `hls` **6** | **303** | **0** |
| 含超时等待的组：`qtimed` 2 / `ptimed` 1 / `ntimed` **41** | **44** | **0**（g++-12 运行时已无 §8.2 的误报） |
| 合计（脚本统计，含跨组重复计数） | **348** | 真问题 **0** |

**同批其它门禁**（`VERSIONING.md` 发版清单里的项目；下表为 v0.1.0 的最终数字）：

| 门禁 | 命令 | 实测 |
|---|---|---|
| 编译 | `cmake --build build` | **0 warning / 0 error**（`-Wall -Wextra`，无任何 `-Wno-`；ASAN 构建同样 0） |
| 单元（串行） | `cd build && ctest --output-on-failure` | **23/23 分组通过**（**269 用例**；**不要**加 `-j`，见 §2.2） |
| 单元（ASAN） | `cmake -B build-asan -DMZMEDIA_ENABLE_ASAN=ON && cd build-asan && ctest` | **21/21 通过** |
| 并发/长跑（**Debug** 构建） | `./scripts/concurrent_test.sh`（10 路 × 5 分钟）+ `--duration 3600`（1 小时） | 全绿；1 小时数据见 `ACCEPTANCE_v0.1.0.md` §3.3（Release 长跑未做，见该文件 §6 第 12 项） |
| 并发（**ASAN** 构建） | `MZMEDIA_SERVER=./build-asan/bin/mzmedia ./scripts/concurrent_test.sh --quick --kill-half --overload 8` | **10/10 通过**（对应 NFR-3 的"ASAN 无报错"） |
| 脚本验收 | `scripts/{http,flv_mux,ts_mux,hls,flv_http}_test.sh` | **全绿**（M3 / flv_mux 8 项 / **ts_mux 29 项** / **hls 32 项** / **flv_http 68 项**〔v0.1.0 当时的项数；M10-b 之后是 96〕） |
| 单进程全量 | `./build/bin/mzmedia_unittest` | **268/269 用例、61,728 条断言**（诊断用，不作为计时门禁；唯一的红是已知的 `timer_precision` 宿主噪声，按 §2.2 单跑该组 3/3 绿） |
| 发版验收 | `docs/ACCEPTANCE_v0.1.0.md` | 逐条 SC/NFR/FR 的"状态 + 证据 + 实测"（v0.1.0 的发布依据） |

> 分组按**用例名子串**匹配，因此个别用例会同时属于两个组
> （如 `logger_queue_overflow_drop` 同时属于 `logger` 与 `queue`）。上表为去重后的数字。

**v0.2 当前基线（M10-b 完成时全门禁重跑，2026-10-25；以后每批交付都刷新这张表）**：

| 门禁 | 实测 |
|---|---|
| 编译（`-Wall -Wextra`，无 `-Wno-`） | **0 warning / 0 error**（`--clean-first` 重建；Debug、ASAN、TSAN 三个构建都是 0） |
| 单元（串行 `ctest`，**不加 `-j`**） | **24/24 分组**（**281 用例**；`ffmpeg` 36 / `srcmgr` 15 / `producer` 15 / `transcode` **12**） |
| 单元（单进程全量，诊断用） | **281/281 用例、62,841 条断言**（本轮 `timer_precision` 也没抖） |
| ASAN | **24/24**（重跑确认） |
| TSAN | **370 用例 | 已知误报 0 | 真问题 0**（严格组 21 组 + `qtimed`/`ptimed`/`ntimed`；含 `transcode` 12 例） |
| 脚本 | `http` / `flv_mux` **8** / `ts_mux` **29** / `hls` **32** / **`flv_http` 96** / **`transcode` 12** —— 全绿 |

> **已知偶发（诚实记录）**：`ntimed_session_shutdown_after_flush`（计时敏感组，已被排除在 TSAN 严格组外）
> 在某一次 ASAN 全量 ctest 里红过一次（`sessionCount()` 期望 0 实得 1）。单跑该用例 **3/3 绿**、
> `ntimed` 整组 **2/2 绿**、ASAN 全量重跑 **24/24 绿** → 判为 ASAN 慢化下的时序抖动。
> 证据口径同 §2.2：计时敏感的用例只按"单组单跑"判定。M10-b **没有**改 `network/`、`session`。

### 8.7 已抓到的真实竞态（不是误报，要照规矩避开）

**测试里同步对象的生存期**：`Semaphore` 被销毁时，另一个线程不能还在 `post()` 里。
M2-2b 踩到过一次：`timer_precision` 把 `Semaphore` 声明在**循环里**，上一轮的
`~Semaphore()`（`pthread_cond_destroy`）与轮询线程仍在进行的 `post()`
（`pthread_cond_broadcast`）并发：

```
WARNING: ThreadSanitizer: data race
  Write of size 8 by main thread:  pthread_cond_destroy ← Semaphore::~Semaphore()
  Previous read of size 8 by thread T3:  pthread_cond_broadcast ← Semaphore::post()
```

规矩：**等待用的 `Semaphore` 必须活得比它等待的线程长** —— 只能在
`poller->shutdown()`（内部 join）之后销毁，不要声明在循环体 / 回调作用域里。
（这与 §8.2 的误报不同：误报的特征是两个访问点都带 `(mutexes: ...)` 标注，
这里是一个 `destroy` 与一个 `broadcast` 撞在一起，是真 UB。）

## 9. 发版前验收清单（配合 VERSIONING.md）

- [ ] 单元测试全部通过（`ctest --output-on-failure`）
- [ ] 集成测试：目标码流 `ffprobe` 参数与源一致，零错误输出
- [ ] 端到端：`ffplay` + 浏览器均能播
- [ ] 压测：达标（NFR-2）
- [ ] 长跑：达标（NFR-3，含 ASAN 无报错）
- [ ] 并发检查：`./scripts/tsan.sh` 无真问题（严格组 0 报告，误报组签名校验通过）
- [ ] `-Wall -Wextra` 零警告
- [ ] `CHANGELOG.md` 已更新
- [ ] 该 tag 代码可独立构建通过（保证可回滚）
