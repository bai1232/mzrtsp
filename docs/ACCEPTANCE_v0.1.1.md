# v0.1.1 验收报告（补丁版）

> 本文件是 **v0.1.1 的发布证据**。v0.1.0 的完整矩阵见 [ACCEPTANCE_v0.1.0.md](ACCEPTANCE_v0.1.0.md)；
> 本版**只修缺陷、不新增能力**，因此只逐条列出**受影响的**需求，其余沿用 v0.1.0 的结论。
> 原则：**只写跑过的**。没跑过的写"未验证"。

| 项 | 值 |
|---|---|
| 版本 | **v0.1.1**（补丁：`Z` 位递增，向后兼容的缺陷修复） |
| 日期 | 2026-10-18 |
| 结论 | 修复 **2 个真缺陷**（`Connection: close` 不生效；畸形请求回完不关且可被反复触发）。门禁全绿，变异验证 3 组按预期变红；唯一的行为收紧是 `malformed` 计数语义 |

## 0. 一页结论（修前 vs 修后，均为实测）

| 场景 | 修前（v0.1.0） | 修后（v0.1.1） |
|---|---|---|
| `GET / HTTP/1.1` + `Connection: close` | 响应头如实写 close，**连接不关**（2s 内读不到 EOF）；之后同一连接再发请求 → **仍回 200** | 响应头 close + **读到 EOF**；之后同一连接再发请求 → **0 个响应**（连接已关） |
| 畸形请求（缺版本段 / 坏头） | 头写 close，**连接不关** | 头写 close + **读到 EOF** |
| 414（5KB URI）/ 431（40KB 头） | 头写 close，**连接不关** | 头写 close + **读到 EOF** |
| 同一条连接连发 5 条畸形 | **收到 5 个 400**，`malformed`=5（日志仅 1 条） | **收到 1 个 400**，`malformed`=1，随后 EOF |
| 405（合法但非 GET） | 不关（keep-alive） | 不关（keep-alive）—— **未回归** |
| 正常 `GET /` | 不关 | 不关 —— **未回归** |

> 「修前」数据来源：裸 socket 探测（`python3` + `recv` 读到 EOF/超时）；
> 「修后」数据来源：同一脚本复跑 + `ntimed_http` 单测。

## 1. 验收环境

| 项 | 值 |
|---|---|
| OS | Ubuntu 22.04.5 LTS（VM，`bpy-virtual-machine`），4 核 / 3.8 GB |
| 编译器 | g++ **11.4**（默认）；g++ **12.3.0**（TSAN 运行时，规避 glibc 2.35 的 sanitizer 误报） |
| CMake | 3.22.1 |
| FFmpeg | **4.4.2**（本版未改 FFmpeg 相关代码） |
| 构建 | `build/`（Debug）、`build-asan/`、`build-tsan/` |

## 2. 受影响的需求（逐条）

| # | 状态 | 本版做了什么 / 证据 |
|---|---|---|
| **FR-4.5** 404/405/500 规范响应 + 日志 | ✅ **本版补上"响应之后连接怎么办"的验证** | v0.1.0 只验了状态码/body；`docs/TESTING.md` 里**没有 FR-4.5 的验证行**（三方对齐机检发现的缺口，本版补上）。现在 `ntimed_http` 每条用例都断言 status + body + 关键头 + **连接开/关**；`totalMalformed()` 每连接 +1、`totalCloseFallback()` 恒为 0 |
| **FR-4.4** 连接上限 / 读空闲 / 写阻塞 | ✅ 语义收紧 | `Connection: close` 现在**真的关**，不再依赖 60s 读空闲兜底；读空闲/写阻塞的超时逻辑未改 |
| **NFR-6** 资源回收 | ✅ 间接改善 | 声明关闭的连接立即回收（不再占着一个会话槽等 60s）。未做"64 个畸形连接压满 `max_session_count`"的压测 → **该量化结论属【推论】** |
| **NFR-7** 零警告 + 覆盖 | ✅ | 清洁构建 **0 warning / 0 error**；用例 **248**（+1）；`ntimed_http` **24**（+1）/ 断言 **263** |
| SC-1~SC-4、其余 NFR / FR | 不变 | 本版不触碰 media / network 数据面；回归面由 `ctest` 全量 + `flv_http_test.sh` 51/51 覆盖 |

## 3. 变异验证（证明新断言真的会红）

三次注入都是**临时改源码 → 重建 → 跑用例 → 恢复**（不用 `git checkout`，避免丢未提交改动）。

| 变异 | 注入内容 | 预期 | 实测 |
|---|---|---|---|
| **M1** 复现修前行为 | `closeAfterFlush()` 变成空操作 | "必须 EOF"的用例全红 | **6/6 红**：`connection_close_honored` / `400` / `414` / `431` / `server_survives_malformed` / `malformed_single_response` |
| **M2** 修过头 | 普通响应出口改成**无条件关** | "必须保活"的对照组全红 | **9/9 红**：测试页 / `404` / `501` / `keepalive_two_requests` / `405` / 管线化 / `chunked_keeps_connection_usable` / `500` / CORS |
| **M3** 去掉去重守卫 | `_malformed_replied` 恒为 false | —— | **仍绿** → 见 §5 第 1 条（诚实记录） |

> M2 的第一次尝试里 **`get_test_page` 侥幸绿了**：因为 `stillOpen()` 当时只做一次 `recv(MSG_DONTWAIT)`，
> 而误关的 FIN 晚到了一点。改成 `poll()` + **50ms 观察窗**后 M2 才 9/9 全红。
> 这条本身就是"变异验证能抓出**用例自己的**漏洞"的实例。

## 4. 门禁（本版实测，命令可复跑）

| 门禁 | 命令 | 实测 |
|---|---|---|
| 编译 | `cmake --build build -j4` | **0 warning / 0 error**（`-Wall -Wextra`，无任何 `-Wno-`） |
| 单元（串行） | `cd build && ctest --output-on-failure` | **21/21 分组、248 用例**（**不加 `-j`**，见 `TESTING.md` §2.2） |
| 单元（单进程全量，诊断） | `./build/bin/mzmedia_unittest` | **248/248 用例、61,386 条断言、0 失败**（14.3s） |
| 单元（ASAN） | `cmake --build build-asan -j4 && cd build-asan && ctest` | **21/21 通过**（ASAN 构建同样 0 warning） |
| 并发（TSAN） | `./scripts/tsan.sh` | **全绿**：严格组 18 组 **282** 用例、含超时组 **44**，**真问题 0**；合计 **326**（含跨组重复计数） |
| 端到端（HTTP） | `./scripts/http_test.sh` | **EXIT=0**（测试页 / CORS / 路由 / 404 / 405 / 501 / 400 / keep-alive / chunked / Range / stats） |
| 端到端（FLV） | `./scripts/flv_http_test.sh` | **51/51 通过**（含 §8「源结束后正常收尾」—— 它跑的就是本版改动所在的关闭路径） |

## 5. 已知边界与"未验证"（诚实版）

| # | 项 | 说明 |
|---|---|---|
| 1 | **「每连接只回一次 4xx」的守卫在"关闭正常"时不可观测** | 变异 M3（去掉守卫）`ntimed_http_malformed_single_response` **仍绿**：因为关闭是立即的，客户端根本来不及让服务端读到第二个报文。它只兜底"关闭失效"的降级路径（此时才可观测）。按 §4.5「可观测是辅助」保留为防御，但**不声称它有用例覆盖** |
| 2 | **chunked 响应的 `Connection: close` 头写不出来** | 头在 handler 的 `beginChunked()` 里就序列化了（`http_response.cpp:144-155`），而 `resp.setKeepAlive(_parser.keepAlive())` 在 `http_server.cpp:314` 才调用（晚于 handler）。行为仍然正确（会关；且客户端自己请求了 close 不会复用），但**声明缺失**。要修得把 `setKeepAlive` 提到 handler 之前 —— 属独立行为变更，本版**未做** |
| 3 | 未做「64 个畸形连接压满 `max_session_count`」压测 | 本版让这类连接更快回收，方向只会更好，但**没有实测数字** |
| 4 | 未重跑 1 小时长跑 / 10 路并发 | 本版只改 HTTP 错误路径（不碰 media/network 数据面）。替代证据：`ctest` 全量 + ASAN + TSAN + `flv_http_test.sh` 51/51 |
| 5 | HTTP/1.0 行为变更 | `_parser.keepAlive()` 对 HTTP/1.0 默认 **false**，所以修后 HTTP/1.0 请求会**真的关连接**（修前只写头）。这是**修复**（原来头与行为矛盾），但确实是可观测变化，已记入 `DESIGN_M3.md` §决策记录 |

## 6. 复现步骤

```bash
cd /home/bpy/MyZltoolkit
cmake -B build && cmake --build build -j"$(nproc)"
cd build && ctest --output-on-failure          # 21/21、248 用例

# 关闭语义的裸 socket 复现（关键：客户端什么都不做，只等 EOF）
python3 - <<'PY'
import socket
s = socket.create_connection(("127.0.0.1", 8080), timeout=3)
s.sendall(b"GET / HTTP/1.1\r\nConnection: close\r\n\r\n")
data = b""
while True:                                     # 修前：这里会超时（连接没关）
    c = s.recv(4096)
    if not c:
        print("收到 EOF：服务端真的关了 ✔"); break
    data += c
PY

# 畸形请求：一条连接只回一次
python3 - <<'PY'
import socket
s = socket.create_connection(("127.0.0.1", 8080), timeout=3)
n = 0
for _ in range(5):
    try: s.sendall(b"GET /\r\n\r\n")
    except OSError: break
    try:
        if not s.recv(4096): break
        n += 1
    except socket.timeout: break
print("收到的 400 个数 =", n)                    # 修前 5、修后 1
PY
```

环境：Ubuntu 22.04.5 / g++ 11.4 / Python 3.10.12 / 4 核。

## 7. 发布信息与回滚

- tag：`v0.1.1`（annotated，指向本文件的发布提交）
- 回滚：`git revert <commit>`（保留历史）；或 `git checkout v0.1.0`（回到修复前）
- 本版变更：见 [`CHANGELOG.md`](../CHANGELOG.md) 的 `[0.1.1]`
