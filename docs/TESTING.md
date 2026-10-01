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
| FR-4.4 连接上限 / 读空闲 / 写阻塞 | 单测：连接上限设为 1 时第 2 个连接被拒且 `totalRejected()` 增长；`recv_idle=50ms` + 连上不发数据的客户端 → 阈内断开且 `onError` 为超时；`send_blocked=50ms` + 只连不读的客户端 → 断开且 `bytesOut` 停止增长 |
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

### 8.2 已知误报：带超时的等待（重要）

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

| 分组 | 用例数（去重） | TSAN 报告 |
|---|---|---|
| 严格组：`selftest` / `util` / `logger` / `queue` / `pool` / `semaphore` / `core` | 58 | **0** |
| 已知误报组：`qtimed` / `ptimed` | 3 | 5（全部为第 8.2 节的误报） |
| 合计 | 61 | 真问题 **0** |

> 分组按**用例名子串**匹配，因此个别用例会同时属于两个组
> （如 `logger_queue_overflow_drop` 同时属于 `logger` 与 `queue`）。上表为去重后的数字。

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
