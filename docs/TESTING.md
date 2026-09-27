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

## 3. 集成测试（ffprobe 校验，可自动化）

```bash
# 1) 服务启动
./build/bin/mzmedia -f media/sample.mp4 -p 8080 &

# 2) 拉流存盘（限时长，便于校验）
timeout 10 ffmpeg -hide_banner -loglevel error \
  -i http://127.0.0.1:8080/live/sample.flv -c copy -t 8 /tmp/out.flv

# 3) 校验输出流：编码、分辨率、是否有音频
ffprobe -v error -show_entries stream=codec_name,width,height,channels \
        -of default=noprint_wrappers=1 /tmp/out.flv

# 4) 期望结果（源为 1920x1080 H264 时）
#   codec_name=h264
#   width=1920
#   height=1080
```

**判定标准**：输出流参数与源一致；`ffmpeg` 转存过程**零错误输出**；帧数与时长匹配（允许 1 帧误差）。

## 4. 端到端（播放器）

| 播放端 | 命令 / 方式 | 关注点 |
|---|---|---|
| ffplay | `ffplay -f flv http://127.0.0.1:8080/live/sample.flv` | 出画面、有声音、无卡顿 |
| 内置测试页 | 浏览器打开 `http://127.0.0.1:8080/` | flv.js 能播、CORS 正常、首帧 <1s |
| curl（协议层） | `curl -N -v http://127.0.0.1:8080/live/sample.flv \| head -c 1k \| xxd` | 前 9 字节是 `FLV\x01`；chunked 头正确 |
| hls（v0.2） | `ffplay http://127.0.0.1:8080/hls/sample.m3u8` | 切片连续、无 404 |

## 5. 稳定性（1 小时长跑）

```bash
# 用 ASAN 构建，暴露内存与 fd 问题
cmake -B build-asan -DMZMEDIA_ENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j

# 长跑 + 每 60s 采样 RSS 与 fd 数
./build-asan/bin/mzmedia -f media/sample.mp4 -p 8080 &
for i in $(seq 1 60); do
  pid=$(pgrep -f "bin/mzmedia" | head -1)
  echo "$(date +%T) rss=$(awk '/VmRSS/{print $2}' /proc/$pid/status)kB fd=$(ls /proc/$pid/fd | wc -l)"
  curl -s http://127.0.0.1:8080/api/stats >> /tmp/stats.log
  sleep 60
done
```

**通过标准（NFR-3）**：RSS 增长 < 5MB/h、fd 数稳定不增长、ASAN 无报错、日志无 `Error`。

## 6. 压测（并发，NFR-2）

```bash
# 10 路同源并发，各自拉 60s
for i in $(seq 1 10); do
  timeout 60 ffmpeg -hide_banner -loglevel error \
    -i http://127.0.0.1:8080/live/sample.flv -c copy -f null - &
done
wait
curl -s http://127.0.0.1:8080/api/stats | python3 -m json.tool
```

**通过标准**：10 路全部正常结束、无崩溃、11 路接入时按配置拒绝或排队、`/api/stats` 客户端数与预期一致。

**专项用例**：接入 10 路后**故意 kill 其中 5 路**，确认源任务继续、剩余 5 路不受影响、资源回收（NFR-6）。

## 7. 需求 → 验证手段对照

| 需求 | 验证手段 |
|---|---|
| SC-1 浏览器能播 | 内置测试页 + flv.js（第 4 节） |
| SC-2 加格式不改核心 | 代码走查：新增 sink 是否只实现 `IMediaSink` 且未改动 `media/`、`network/` |
| SC-3 多客户端不崩 | 压测（第 6 节） |
| SC-4 1 小时稳定 | 长跑（第 5 节） |
| NFR-1 首帧 <1s | 浏览器 Performance 面板 / 日志打点（请求到首字节） |
| NFR-4 remux CPU <5% | `top -H -p $(pgrep mzmedia)` 观察单路负载 |
| NFR-5 转码 ≥1.0x | `/api/stats` 的 `speed` 字段 |
| NFR-6 资源回收 | 断开前后对比 `/api/stats` 与 `/proc/<pid>/fd` |
| FR-5.2 慢客户端丢帧 | `tc` 限速或 `kill -STOP` 阻塞客户端，观察 `dropped` 计数与其他客户端 |
| 各 codec 组合 | `CODEC_MATRIX.md` 每个组合一条 ffprobe 用例 |

## 8. 发版前验收清单（配合 VERSIONING.md）

- [ ] 单元测试全部通过（`ctest --output-on-failure`）
- [ ] 集成测试：目标码流 `ffprobe` 参数与源一致，零错误输出
- [ ] 端到端：`ffplay` + 浏览器均能播
- [ ] 压测：达标（NFR-2）
- [ ] 长跑：达标（NFR-3，含 ASAN 无报错）
- [ ] `-Wall -Wextra` 零警告
- [ ] `CHANGELOG.md` 已更新
- [ ] 该 tag 代码可独立构建通过（保证可回滚）
