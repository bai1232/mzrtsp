# mzmedia

> 一个支持多格式的流媒体文件服务器：**输入多种媒体文件，输出多种流媒体格式，中间按需转封装或转码。**

自研 epoll 网络底座 + FFmpeg 编解码内核 + 一源多消费者的实时分发。目标是把「**解封装 → 解码 → 编码 → 封装**」这条全链路跑通、跑稳，并且**可持续扩展新格式而不动核心**。

## 项目状态

| 里程碑 | 内容 | 状态 |
|---|---|---|
| 设计 | 规格、架构、能力矩阵、验收指标 | ✅ 已定稿 |
| **v0.1.0** | MP4 / H264 裸流 → **HTTP-FLV**，remux 零拷贝，多客户端共享 | 🚧 开发中 |
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

> v0.1.0 尚未完成，以下为设计目标用法。

```bash
# 启动服务（零参数也会启动内置测试页）
./build/bin/mzmedia -f media/sample.mp4 -p 8080

# 浏览器打开测试页
xdg-open http://127.0.0.1:8080/
# 或用 ffplay 直接拉流
ffplay -f flv http://127.0.0.1:8080/live/sample.flv
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
| [docs/AV_BASICS.md](docs/AV_BASICS.md) | **音视频基础（本项目需要的 20%）**：容器/编码、I-P-B、pts/dts、time_base、SPS/PPS；每条概念配可自己跑的命令 |
| [docs/RETROSPECTIVE.md](docs/RETROSPECTIVE.md) | **复盘总结**：技术坑库（现象→根因→修复→防回归用例）、测量工具误判、决策与代价、AI 协作复盘、简历素材 |
| [docs/VERSIONING.md](docs/VERSIONING.md) | **版本与发布强制要求**（每版必推 Git、必打 tag、可回滚） |
| [docs/GIT_WORKFLOW.md](docs/GIT_WORKFLOW.md) | 分支模型与提交规范 |
| [CHANGELOG.md](CHANGELOG.md) | 版本变更记录 |

## 许可证

项目本体采用 [MIT](LICENSE)。

但请注意**运行时依赖的许可证**：本项目动态链接 Ubuntu 打包的 FFmpeg 库，该构建**启用了 GPL 组件**（`libx264`、`libx265` 等）。因此**以二进制形式对外分发**时，整体须遵循 GPL 的约束；仅源码分发或自用不受影响。若需宽松分发，可改用不含 GPL 组件的 FFmpeg 构建并避免使用 `libx264`。
