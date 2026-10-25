# mzmedia

> 一个支持多格式的流媒体文件服务器：**输入多种媒体文件，输出多种流媒体格式，中间按需转封装或转码。**

自研 epoll 网络底座 + FFmpeg 编解码内核 + 一源多消费者的实时分发。目标是把「**解封装 → 解码 → 编码 → 封装**」这条全链路跑通、跑稳，并且**可持续扩展新格式而不动核心**。

## 项目状态

| 里程碑 | 内容 | 状态 |
|---|---|---|
| 设计 | 规格、架构、能力矩阵、验收指标 | ✅ 已定稿 |
| **v0.1.0** | MP4 / H264 裸流 → **HTTP-FLV**，remux 零拷贝，多客户端共享 | ✅ **已发布**（tag `v0.1.0`；验收证据见 [docs/ACCEPTANCE_v0.1.0.md](docs/ACCEPTANCE_v0.1.0.md)） |
| **v0.1.1** | 补丁：`Connection: close` 与畸形请求之后**真的关闭连接**（RFC 7230 §6.6） | ✅ **已发布**（tag `v0.1.1`；证据见 [docs/ACCEPTANCE_v0.1.1.md](docs/ACCEPTANCE_v0.1.1.md)） |
| **v0.2.0** | MKV / TS 输入、**HLS** 输出、**转码**（含 HEVC → H264） | ✅ **已发布**（tag `v0.2.0`；验收证据见 [docs/ACCEPTANCE_v0.2.0.md](docs/ACCEPTANCE_v0.2.0.md)） |
| **v0.2.1** | 补丁：**B 帧源**的时间戳（pts 不再被钳 → 显示顺序正确；TS 负时间戳整体平移；循环接缝不倒退） | ✅ **已发布**（tag `v0.2.1`；证据见 [docs/ACCEPTANCE_v0.2.1.md](docs/ACCEPTANCE_v0.2.1.md)） |
| **v0.2.2** | v0.2 收尾：**转码 + HLS**（`/hls/x.m3u8?transcode=1&h=720`） | ✅ **已发布**（tag `v0.2.2`；证据见 [docs/ACCEPTANCE_v0.2.2.md](docs/ACCEPTANCE_v0.2.2.md)） |
| v0.3.0 | RTSP 输入、解码处理（滤镜/缩放/水印） | ⏳ 计划 |

### v0.2.0 验收摘要（实测，完整矩阵见验收报告）

| 指标 | 实测 | 要求 |
|---|---|---|
| 输入 | MP4 / H264 裸流 / **MKV** / **TS**（含纯视频）→ 都能拉出合法 FLV；TS 的 ADTS **`ASC=12 08`** 且 raw AAC 已剥头 | FR-2.2 |
| HLS | 2 s 切片（只切视频关键帧）/ 窗口 **6** / `MEDIA-SEQUENCE` 前进 / 老切片 404 / **`ffmpeg` 读 m3u8 零 error** / 切片 Range 206 / **转码 + HLS**（切片 URI 自带规格，直通不受影响） | FR-3.2 / FR-4.1 / FR-4.2 |
| **转码（NFR-5）** | 1080p→720p **3.03x**（媒体 4880 ms vs 墙钟 1688 ms，74 帧/s）；服务内 `/api/stats` 的 `speed` **3.26x**；HEVC→H264 实测可播 | **≥1.0x ✅ 首次达成** |
| 并发/稳定性 | **10 路 × 1 小时**：每路完整可解、丢帧 0、fd 恒定、错误日志 0；ASAN 构建 10/10 | NFR-2/3/4 |
| 质量（NFR-7） | `--clean-first` 重建 **0 warning**；**281 用例 / 24 分组**；ASAN 24/24；TSAN **370 用例 / 真问题 0** | 零警告 |

### v0.2.1 补丁摘要（实测，详见 [验收报告](docs/ACCEPTANCE_v0.2.1.md)）

| 场景（`bf.mp4`，`-bf 2`） | 修前 | 修后 |
|---|---|---|
| FLV 的 CompositionTime | `80,160,120,80,…`（B 帧被抬高 → **画面顺序错乱**） | `80,160,40,40,…`（**与源逐包一致**） |
| `ffmpeg -f null -` | 3 条 `non monotonically increasing dts` | **零输出** |
| TS 切片的 PES dts | `1.000, 1.000, 1.000`（负 dts 被钳 0） | `1.000, 1.040, 1.080`（整体平移，差值不变） |
| `--loop` 接缝 dts | 961 → **921**（倒退 39 ms，每次循环抖一下） | 严格递增 |

### v0.1.0 验收摘要（历史，完整矩阵见 [验收报告](docs/ACCEPTANCE_v0.1.0.md)）

| 指标 | 实测 | 要求 |
|---|---|---|
| 端到端 | `curl -N …/live/sample.flv` → **55,623 字节**合法 FLV（h264 320x240 + aac / 50 帧 / 解码无 error） | SC-1 |
| 首帧（NFR-1） | **20 ms 级**（TTFB 3.3 ms） | < 1s |
| 并发（NFR-2/SC-3） | **10 路 × 5 分钟**：每路完整可解、丢帧 0、断开后 fd 回基准 | ≥10 路不崩 |
| 服务端开销（NFR-4） | **单核 4.11%（十路合计）** ≈ 每路 0.4% | 单路 <5% |
| 质量（NFR-7） | 清洁构建 **0 warning**；**247 用例 / 21 分组** | 零警告 |

## 能力矩阵

| 能力 | v0.1 | v0.2 | v0.3 |
|---|---|---|---|
| 输入 MP4 / H264 裸流 | ✅ | | |
| 输入 MKV / TS | | ✅ | |
| 输入 RTSP | | | ✅ |
| 视频 H264 / 音频 AAC | ✅ | | |
| 输出 HTTP-FLV | ✅ | | |
| 输出 HLS | | ✅ | |
| 转封装（零拷贝） | ✅ | | |
| 转码 | | ✅ | |
| 解码处理（滤镜/缩放/水印） | | | ✅ |
| H265 / HEVC 输入（**直通**） | — | ❌ | ❌ 不做 |
| H265 / HEVC → H264（**转码**） | | ✅ | |

> 为什么 HEVC 只能转码？HTTP-FLV / HLS 的浏览器播放端都不支持 HEVC，而"H264 + AAC 直通"是零拷贝路径
> 的前提。所以 v0.2.0 起：**不转码**时 HEVC 明确 **415**；带 `?transcode=1` 时解码 HEVC 再编 H264
> （实测 `sample_hevc.mp4` → h264 160x120、`ffmpeg -f null -` 零 error）。判定由 `CodecMatrix` 统一做
> （能力感知：本机缺 HEVC 解码器或 H264 编码器时仍是 415）。

## 架构一览

```
输入 demux ──► 可行性判定 ──► remux（零拷贝） ──► mux(FLV/TS) ──┐
（MP4/H264/MKV/TS/RTSP）  └─► transcode(v0.2+) ──────────────┘  │
                                                                ▼
                                            MediaSource（一源一份输出）
                                                                │
                              ┌─────────────────┬───────────────┴────────┐
                          Client A          Client B               Client C
                       （各自独立连接与发送队列，慢客户端丢帧不影响他人）
```

- **网络与并发全部自研**：epoll 事件循环、TCP 封装、线程池、定时器、日志
- **FFmpeg 只做编解码/封装**：所有 FFmpeg 对象都用 C++ RAII 封装，杜绝泄漏
- **一源多消费者**：同一文件只转/封一份，N 个客户端共享，各自独立队列

## 构建

### 依赖

| 依赖 | 版本 | 用途 |
|---|---|---|
| Linux | — | epoll，不支持 Windows |
| CMake | ≥ 3.15 | 构建 |
| GCC | ≥ 9（需 C++17） | 编译 |
| FFmpeg 开发库 | **4.4.x** | `libavformat` / `libavcodec` / `libavutil` / `libswresample` |
| FFmpeg 开发库（**v0.2.0 起必需**） | 4.4.x | **`libswscale-dev`**（转码的缩放/像素格式转换；缺库 CMake 直接 `FATAL_ERROR`） |
| FFmpeg 开发库（v0.3 计划） | 4.4.x | `libavfilter-dev`（滤镜/水印） |

**C++ 代码零第三方依赖**（网络、线程、定时器、日志、测试框架全部自研）。
唯一的例外是**浏览器播放器** `flv.js` 1.6.2（Apache-2.0）：浏览器原生不能播 FLV，
需要它做 FLV → MSE 的转换。它**已入库**在 `third_party/flv.js/`（离线可用，
来源/版本/MD5/许可声明见该目录 `README.md`），由服务端通过 `GET /flv.min.js` 提供给网页。

```bash
# Ubuntu 22.04：v0.1 所需（通常已随系统安装）
sudo apt-get install -y libavformat-dev libavcodec-dev libavutil-dev libswresample-dev

# v0.2.0 起：转码必须先装 swscale（缺了 build 会直接失败，不会静默退化成"不缩放"）
sudo apt-get install -y libswscale-dev
# v0.3 的滤镜/水印再装：libavfilter-dev
```

> ⚠️ **FFmpeg 版本锁定 4.x**。4.x 与 5.x/6.x 的 API 不兼容（`avcodec_close` 弃用、声道布局字段变更等），CMake 会做版本探测并在不满足时报错。

### 编译

```bash
git clone git@github.com:bai1232/mzrtsp.git
cd mzrtsp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## 快速开始

> v0.1.0 的 **M6-c 已完成**：HTTP-FLV 端到端可播（浏览器 / `ffplay` 都行）。
> 零参数就能跑起来（`FR-7.2`）；下面每一步都是本机实测过的。

```bash
# 1) 构建（Debug 是默认值；发布用 -DCMAKE_BUILD_TYPE=Release）
cmake -B build && cmake --build build -j"$(nproc)"

# 2) 生成测试样本（320x240 H264 + 44.1k AAC 2 秒，以及一份 H264 裸流）
./scripts/make_samples.sh

# 3) 启动服务：零参数 = 0.0.0.0:8080 + 媒体目录 ./samples
./build/bin/mzmedia
#    常用选项：--port 9000 --media-root /data --loop --speed 4 --log-level debug --log-file logs/x.log --help

# 4) 看画面（二选一）
#    浏览器：打开 http://127.0.0.1:8080/ ，点「播放」（页面用的是**入库**的 flv.js，离线可用）
ffplay -f flv http://127.0.0.1:8080/live/sample.flv

# 5) 命令行拉流 / 存盘（HTTP-FLV 就是 FLV，可以直接落盘给 ffprobe）
curl -N http://127.0.0.1:8080/live/sample.flv -o /tmp/x.flv
ffprobe /tmp/x.flv

# 6) H264 裸流也能拉（服务端现场构造 avcC 并把 Annex-B 转成 AVCC）
curl -N http://127.0.0.1:8080/live/sample.h264.flv -o /tmp/y.flv
ffprobe /tmp/y.flv

# 7) 循环推流 + 运行统计
./build/bin/mzmedia --loop &          # 播完自动从头接上（时间戳连续，不会回退）
curl -s http://127.0.0.1:8080/api/stats | head -c 400

# 8) 【v0.2】MKV / TS 输入（样本由 make_samples.sh 生成）
curl -N http://127.0.0.1:8080/live/sample.mkv.flv -o /tmp/m.flv && ffprobe /tmp/m.flv
curl -N http://127.0.0.1:8080/live/sample.ts.flv  -o /tmp/t.flv && ffprobe /tmp/t.flv

# 9) 【v0.2】HLS：ffplay / ffmpeg / 浏览器都能直接吃 m3u8
ffplay http://127.0.0.1:8080/hls/sample.mp4.m3u8
#    要转码的 HLS：同一个服务上还能同时提供直通（上面那条）与转码（这条）
ffplay 'http://127.0.0.1:8080/hls/sample1080.mp4.m3u8?transcode=1&h=120'
ffmpeg -v error -i http://127.0.0.1:8080/hls/sample.mp4.m3u8 -t 4 -f null -   # 零 error 即通过

# 10) 【v0.2】转码：服务端要显式打开（默认关），URL 里再选
./build/bin/mzmedia --transcode --transcode-height 480 &
curl -N 'http://127.0.0.1:8080/live/sample.mp4.flv?transcode=1&h=120' -o /tmp/tx.flv
ffprobe -v error -show_entries stream=codec_name,width,height /tmp/tx.flv    # h264 160x120（+ aac 透传）
#    没开 --transcode 时带 ?transcode=1 会明确回 501（绝不静默降级成直通）
```

### 验证（每个脚本都会打印"通过 N 项 / 失败 N 项"）

```bash
cd build && ctest                        # 24 个分组（288 个用例），**串行**跑（不要加 -j）
./bin/mzmedia_unittest producer           # 只跑某组：分组名是**用例名的子串**
../scripts/flv_http_test.sh               # 端到端：104 项（HTTP-FLV + MKV/TS + 播放页 + 循环 + 转码 + B 帧）
../scripts/hls_test.sh                    # HLS：40 项（m3u8 / 切片 / Range / 窗口 / 租约 / 播放页 / 转码+HLS）
../scripts/ts_mux_test.sh                 # TS 封装：29 项（ffprobe/ffmpeg 四层判据）
../scripts/transcode_test.sh              # 转码：12 项（1080p→720p + NFR-5 speed）
../scripts/flv_mux_test.sh                # FLV 封装：8 项（→ ffprobe）
../scripts/http_test.sh                   # HTTP 层（M3）
../scripts/concurrent_test.sh --quick     # 并发/回收：10 路 × 20s（硬门禁）
../scripts/concurrent_test.sh             # 长跑：10 路 × 5 分钟（NFR-2/3/4/6 + 性能基线）
../scripts/tsan.sh                        # TSAN：严格组 0 报告
```

## 部署提示（v0.1.0）

- **默认绑 `0.0.0.0` 且 `/api/stats` 无鉴权**（SPEC 未要求）：对外部署请放在反向代理/内网后面，或用防火墙限制来源。
- **无 TLS**：纯 HTTP，播放页与拉流都是明文；CORS 按 `FR-4.3` 放开 `Access-Control-Allow-Origin: *`（flv.js 需要）。
- **日志**默认写 `logs/mzmedia.log`（单文件 64MB、保留最近 5 个，**行缓冲**因此 `tail -f` 实时可见）；
  注意磁盘空间与轮转（`--log-file` 可改路径，`--log-level` 可调级别）。
- **端口**用 `--port`；媒体目录用 `--media-root`（`/live/<name>.flv` 在其中按名取文件）。
- 进程收到 `SIGINT`/`SIGTERM` 会优雅退出（停服务器 → 释放订阅者 → 按空闲规则回收源 → flush 日志）。

## 文档

| 文档 | 说明 |
|---|---|
| [docs/SPEC.md](docs/SPEC.md) | **需求规格（唯一规格来源）**：能力矩阵、流水线、功能/非功能需求、成功标准 |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 架构设计：分层、线程模型、一源多消费者、资源管理 |
| [docs/CODEC_MATRIX.md](docs/CODEC_MATRIX.md) | 输入 × 输出的可行性判定（何时 remux、何时必须转码） |
| [docs/ACCEPTANCE_v0.2.2.md](docs/ACCEPTANCE_v0.2.2.md) | **v0.2.2 验收报告（当前版本）**：转码 + HLS 补齐（v0.2 里程碑收尾） |
| [docs/ACCEPTANCE_v0.2.1.md](docs/ACCEPTANCE_v0.2.1.md) | **v0.2.1 补丁验收报告**：B 帧源时间戳三处缺陷的修前/修后实测 + 变异验证 |
| [docs/ACCEPTANCE_v0.2.0.md](docs/ACCEPTANCE_v0.2.0.md) | **v0.2.0 验收报告**：逐条 SC/NFR/FR → 状态 + 证据命令 + 实测数字 + 变异验证 + 未验证清单 |
| [docs/ACCEPTANCE_v0.1.0.md](docs/ACCEPTANCE_v0.1.0.md) | **v0.1.0 验收报告**：逐条 SC/NFR/FR → 状态 + 证据命令 + 实测数字 + 复现步骤 + 未达成清单 |
| [docs/ACCEPTANCE_v0.1.1.md](docs/ACCEPTANCE_v0.1.1.md) | **v0.1.1 补丁验收报告**：两个缺陷的修前/修后实测、变异验证、门禁数字、已知边界 |
| [docs/ROADMAP.md](docs/ROADMAP.md) | 里程碑任务分解与风险 |
| [docs/TESTING.md](docs/TESTING.md) | 验收与测试方法（ffprobe 校验、并发、1 小时长跑） |
| [docs/DESIGN_M2.md](docs/DESIGN_M2.md) | **网络层设计（M2）**：组件接口形状、机制、分批计划、风险清单、决策记录 |
| [docs/DESIGN_M3.md](docs/DESIGN_M3.md) | **HTTP 层设计（M3）**：解析器契约与上限、分帧/keep-alive/chunked/Range、决策记录 |
| [docs/DESIGN_M4.md](docs/DESIGN_M4.md) | **FFmpeg 封装设计（M4）**：AvPtr RAII、时间基换算与单调守卫、Demuxer 上限与观测、决策记录 |
| [docs/DESIGN_M5.md](docs/DESIGN_M5.md) | **媒体分发设计（M5）**：MediaPacket 零拷贝、FrameQueue 上限与丢帧策略、GopCache 接入保证、MediaSource 订阅者生命周期 |
| [docs/DESIGN_M6.md](docs/DESIGN_M6.md) | **输出层设计（M6）**：FLV 字节布局逐字段、per-client 时间戳基准、sequence header 重发纪律、ffprobe 验收 |
| [docs/DESIGN_M9.md](docs/DESIGN_M9.md) | **HLS 设计（M9）**：TS 封装（PAT/PMT/PCR/90kHz）、切片只认视频关键帧、窗口与租约、决策与风险 |
| [docs/DESIGN_M10.md](docs/DESIGN_M10.md) | **转码设计（M10）**：解码→sws→x264、自己生成 CFR 时间轴（为什么不能用源 pts）、服务接线与并发上限、`speed` 口径 |
| [docs/AV_BASICS.md](docs/AV_BASICS.md) | **音视频基础（本项目需要的 20%）**：容器/编码、I-P-B、pts/dts、time_base、SPS/PPS；每条概念配可自己跑的命令 |
| [docs/RETROSPECTIVE.md](docs/RETROSPECTIVE.md) | **复盘总结**：技术坑库（现象→根因→修复→防回归用例）、测量工具误判、决策与代价、AI 协作复盘、简历素材 |
| [docs/VERSIONING.md](docs/VERSIONING.md) | **版本与发布强制要求**（每版必推 Git、必打 tag、可回滚） |
| [docs/GIT_WORKFLOW.md](docs/GIT_WORKFLOW.md) | 分支模型与提交规范 |
| [CHANGELOG.md](CHANGELOG.md) | 版本变更记录 |

## 许可证

项目本体采用 [MIT](LICENSE)。

**入库的第三方文件**：`third_party/flv.js/`（浏览器播放器，Apache-2.0，含 `LICENSE` 与来源说明）——
它是 v0.1.0「浏览器能播」的第三方组成；v0.2 起 HLS 的播放器 `hls.js` 1.5.17 同样入库
（`third_party/hls.js/`）。再分发时请保留两者的版权与许可声明（均 Apache-2.0）。

但请注意**运行时依赖的许可证**：本项目动态链接 Ubuntu 打包的 FFmpeg 库，该构建**启用了 GPL 组件**（`libx264`、`libx265` 等）。因此**以二进制形式对外分发**时，整体须遵循 GPL 的约束；仅源码分发或自用不受影响。若需宽松分发，可改用不含 GPL 组件的 FFmpeg 构建并避免使用 `libx264`。
