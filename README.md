# mzmedia

> 一个支持多格式的流媒体文件服务器：**输入多种媒体文件，输出多种流媒体格式，中间按需转封装或转码。**

自研 epoll 网络底座 + FFmpeg 编解码内核 + 一源多消费者的实时分发。目标是把「**解封装 → 解码 → 编码 → 封装**」这条全链路跑通、跑稳，并且**可持续扩展新格式而不动核心**。

## 项目状态

| 里程碑 | 内容 | 状态 |
|---|---|---|
| 设计 | 规格、架构、能力矩阵、验收指标 | ✅ 已定稿 |
| **v0.1.0** | MP4 / H264 裸流 → **HTTP-FLV**，remux 零拷贝，多客户端共享 | 🚧 开发中（**M1–M6-c 已完成**：端到端可播；M6-d 并发/长跑 + M7 收尾） |
| v0.2.0 | MKV / TS 输入、**HLS** 输出、转码 | ⏳ 计划 |
| v0.3.0 | RTSP 输入、解码处理（滤镜/缩放/水印） | ⏳ 计划 |

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
| H265 / HEVC | — | — | ❌ 不做 |

> 为什么不做 H265？HTTP-FLV 的浏览器播放端不支持 HEVC，与"浏览器能播"的目标冲突。详见 `docs/SPEC.md` 的决策记录。

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
| FFmpeg 开发库 | **4.4.x** | `libavformat` / `libavcodec` / `libavutil` |
| FFmpeg 开发库（v0.2 起） | 4.4.x | 追加 `libswscale-dev`、可选 `libavfilter-dev` |

**C++ 代码零第三方依赖**（网络、线程、定时器、日志、测试框架全部自研）。
唯一的例外是**浏览器播放器** `flv.js` 1.6.2（Apache-2.0）：浏览器原生不能播 FLV，
需要它做 FLV → MSE 的转换。它**已入库**在 `third_party/flv.js/`（离线可用，
来源/版本/MD5/许可声明见该目录 `README.md`），由服务端通过 `GET /flv.min.js` 提供给网页。

```bash
# Ubuntu 22.04：v0.1 所需（通常已随系统安装）
sudo apt-get install -y libavformat-dev libavcodec-dev libavutil-dev libswresample-dev

# v0.2 起转码/滤镜还需（当前环境缺失，需要时再装）
sudo apt-get install -y libswscale-dev libavfilter-dev
```

> ⚠️ **FFmpeg 版本锁定 4.x**。4.x 与 5.x/6.x 的 API 不兼容（`avcodec_close` 弃用、声道布局字段变更等），CMake 会做版本探测并在不满足时报错。

### 编译

```bash
git clone git@github.com:bai1232/mzmedia.git
cd mzmedia
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
#    常用选项：--port 9000 --media-root /data --loop --speed 4 --log-level debug --help

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
```

### 验证（每个脚本都会打印"通过 N 项 / 失败 N 项"）

```bash
cd build && ctest                        # 21 个分组（245 个用例），串行跑
./bin/mzmedia_unittest producer           # 只跑某组：分组名是**用例名的子串**
../scripts/flv_http_test.sh               # M6-c 端到端验收：49 项（HTTP + ffprobe + 浏览器资源 + 循环）
../scripts/concurrent_test.sh --quick     # M6-d 并发/回收：10 路 × 20s（硬门禁）
../scripts/concurrent_test.sh             # M6-d 长跑：10 路 × 5 分钟（NFR-2/3/4/6 + 性能基线）
../scripts/flv_mux_test.sh                # M6-a：8 项（FLV 封装 → ffprobe）
../scripts/http_test.sh                   # M3：HTTP 层
../scripts/tsan.sh                        # TSAN：严格组 0 报告
```

## 文档

| 文档 | 说明 |
|---|---|
| [docs/SPEC.md](docs/SPEC.md) | **需求规格（唯一规格来源）**：能力矩阵、流水线、功能/非功能需求、成功标准 |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 架构设计：分层、线程模型、一源多消费者、资源管理 |
| [docs/CODEC_MATRIX.md](docs/CODEC_MATRIX.md) | 输入 × 输出的可行性判定（何时 remux、何时必须转码） |
| [docs/ROADMAP.md](docs/ROADMAP.md) | 里程碑任务分解与风险 |
| [docs/TESTING.md](docs/TESTING.md) | 验收与测试方法（ffprobe 校验、并发、1 小时长跑） |
| [docs/DESIGN_M2.md](docs/DESIGN_M2.md) | **网络层设计（M2）**：组件接口形状、机制、分批计划、风险清单、决策记录 |
| [docs/DESIGN_M3.md](docs/DESIGN_M3.md) | **HTTP 层设计（M3）**：解析器契约与上限、分帧/keep-alive/chunked/Range、决策记录 |
| [docs/DESIGN_M4.md](docs/DESIGN_M4.md) | **FFmpeg 封装设计（M4）**：AvPtr RAII、时间基换算与单调守卫、Demuxer 上限与观测、决策记录 |
| [docs/DESIGN_M5.md](docs/DESIGN_M5.md) | **媒体分发设计（M5）**：MediaPacket 零拷贝、FrameQueue 上限与丢帧策略、GopCache 接入保证、MediaSource 订阅者生命周期 |
| [docs/DESIGN_M6.md](docs/DESIGN_M6.md) | **输出层设计（M6）**：FLV 字节布局逐字段、per-client 时间戳基准、sequence header 重发纪律、ffprobe 验收 |
| [docs/AV_BASICS.md](docs/AV_BASICS.md) | **音视频基础（本项目需要的 20%）**：容器/编码、I-P-B、pts/dts、time_base、SPS/PPS；每条概念配可自己跑的命令 |
| [docs/RETROSPECTIVE.md](docs/RETROSPECTIVE.md) | **复盘总结**：技术坑库（现象→根因→修复→防回归用例）、测量工具误判、决策与代价、AI 协作复盘、简历素材 |
| [docs/VERSIONING.md](docs/VERSIONING.md) | **版本与发布强制要求**（每版必推 Git、必打 tag、可回滚） |
| [docs/GIT_WORKFLOW.md](docs/GIT_WORKFLOW.md) | 分支模型与提交规范 |
| [CHANGELOG.md](CHANGELOG.md) | 版本变更记录 |

## 许可证

项目本体采用 [MIT](LICENSE)。

**入库的第三方文件**：`third_party/flv.js/`（浏览器播放器，Apache-2.0，含 `LICENSE` 与来源说明）——
它是 v0.1.0 "浏览器能播"的唯一第三方组成，再分发时请保留其版权与许可声明。

但请注意**运行时依赖的许可证**：本项目动态链接 Ubuntu 打包的 FFmpeg 库，该构建**启用了 GPL 组件**（`libx264`、`libx265` 等）。因此**以二进制形式对外分发**时，整体须遵循 GPL 的约束；仅源码分发或自用不受影响。若需宽松分发，可改用不含 GPL 组件的 FFmpeg 构建并避免使用 `libx264`。
