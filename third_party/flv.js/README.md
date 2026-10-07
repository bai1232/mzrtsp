# third_party/flv.js —— 浏览器端 FLV 播放器（唯一入库的第三方文件）

## 这是什么、为什么在这里

mzmedia 的输出是 **HTTP-FLV**（`Content-Type: video/x-flv`）。现代浏览器**不能**直接播 FLV：
需要把 FLV 拆开、把 H264/AAC 喂给 MSE（Media Source Extensions）。`flv.js` 就是做这件事的库
（Bilibili 开源，Apache-2.0）。

入库（而不是让页面从 CDN 拉）的理由：
1. **离线可用**：本项目的验收脚本、演示环境都不依赖外网 —— CDN 一断，"播放页打不开"会被
   误判成"服务坏了"，那是最浪费时间的一类故障；
2. **版本确定**：`1.6.2` 是写死的（下面有散列），不会某天悄无声息地换一个行为不同的版本；
3. **可审计**：构建/调试时能直接看到播放器源码。

## 来源与版本（可复查）

| 项 | 值 |
|---|---|
| 文件 | `flv.min.js` |
| 版本 | `1.6.2`（页面里 `flvjs.version` 会显示它，用来确认加载的是这一版） |
| 下载地址 | `https://cdn.jsdelivr.net/npm/flv.js@1.6.2/dist/flv.min.js` |
| 许可 | Apache-2.0，全文见 `LICENSE`（同样入库） |
| 大小 | 144165 字节 |
| MD5 | `5d215f2c188f0bb8d82908bbfda85aec` |
| LICENSE MD5 | `175792518e4ac015ab6696d16c4f607e` |

复核命令（下载地址可能失效，散列不会）：

```bash
md5sum third_party/flv.js/flv.min.js third_party/flv.js/LICENSE
# 期望：5d215f2c188f0bb8d82908bbfda85aec 与 175792518e4ac015ab6696d16c4f607e
```

## 怎么被用起来

- `src/main.cpp` 用 `--web-root`（默认自动找仓库里的本目录）把 `flv.min.js` 通过
  **`GET /flv.min.js`** 提供给浏览器；
- 内置测试页 `HttpServer` 的 `kTestPage`（见 `src/http/http_server.cpp`）加载它并创建播放器；
- `scripts/flv_http_test.sh` 会 `curl` 这个路径，确认它真的能取到（否则"页面能开、播放器没有"
  这种半成品会被漏掉）。

## 升级步骤（要动就一起动）

1. 换 `flv.min.js` 与 `LICENSE`；
2. 更新上表的版本/大小/MD5；
3. 跑 `./scripts/flv_http_test.sh`（其中有 `curl /flv.min.js` 与 `flvjs.version` 的检查）。

## 许可提示

`flv.js` 是 Apache-2.0：**再分发时要保留版权与许可声明**，所以 `LICENSE` 必须和
`flv.min.js` 放在一起（本目录就是这样）。本项目其余代码为自研、零第三方依赖，
唯一例外就是这里（`README.md` 与 `docs/ARCHITECTURE.md` 都写明了）。
