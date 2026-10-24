# third_party/hls.js —— 浏览器端 HLS 播放器（第二个入库的第三方文件）

## 这是什么、为什么在这里

mzmedia 的 HLS 输出是 **标准 MPEG-TS 切片 + `.m3u8`**。Chrome/Firefox 的 `<video>` **不能**直接吃 m3u8
（只有 Safari/iOS 原生支持），需要 `hls.js` 把切片拉下来、转成 fMP4 喂给 MSE
（Media Source Extensions）。`hls.js` 是这件事的事实标准实现（Apache-2.0）。

入库（而不是让页面从 CDN 拉）的理由与 `flv.js` 完全相同：

1. **离线可用**：验收脚本、演示环境都不依赖外网 —— CDN 一断，"播放页打不开"会被误判成
   "服务坏了"，那是最浪费时间的一类故障；
2. **版本确定**：`1.5.17` 写死（下面有散列），不会某天悄无声息地换一个行为不同的版本；
3. **可审计**：调试时能直接看到播放器源码，也能确认"页面加载的确实是这一版"。

C++ 侧仍然**零第三方依赖** —— 这里只有浏览器用的 JS。

## 来源与版本（可复查）

| 项 | 值 |
|---|---|
| 文件 | `hls.min.js` |
| 版本 | `1.5.17`（`Hls.version` 会显示它，播放页把它们打印在页面上） |
| 下载地址 | `https://cdn.jsdelivr.net/npm/hls.js@1.5.17/dist/hls.min.js` |
| 许可 | Apache-2.0（Copyright (c) 2017 Dailymotion），全文见 `LICENSE`（同样入库） |
| 大小 | 413952 字节 |
| MD5 | `b0e74c4afba433e2362a2e0931c58657` |
| SHA256 | `484054e8cd03d3f6d1781fb7f402bdc318d8a4c527f933a95c624e27cc9a9470` |
| LICENSE MD5 | `8c22b1b3074f826155dbc85e6a0d4d1f` |

复核命令（下载地址可能失效，散列不会）：

```bash
md5sum third_party/hls.js/hls.min.js       # → b0e74c4afba433e2362a2e0931c58657
md5sum third_party/hls.js/LICENSE          # → 8c22b1b3074f826155dbc85e6a0d4d1f
grep -o '1\.5\.17' third_party/hls.js/hls.min.js | head -1   # 版本串在文件里
# 或者重新下载后对比：
curl -sSL -o /tmp/hls.min.js https://cdn.jsdelivr.net/npm/hls.js@1.5.17/dist/hls.min.js
cmp /tmp/hls.min.js third_party/hls.js/hls.min.js
```

## 它怎么被服务出去

- `src/main.cpp` 启动时按"当前目录 → 可执行文件旁边"的顺序找 `third_party/hls.js/`；
- 命中则注册 `GET /hls.min.js`（`application/javascript`，`Cache-Control: public, max-age=600`）；
- 没找到**不阻塞启动**：播放页会显示「hls.js 未加载」并给出原因，HLS 拉流本身仍然可用
  （`ffplay`/`ffmpeg` 不需要它）。
- `tests/` 与验收脚本用 **逐字节比对**（`cmp`）确认页面拿到的就是入库文件 ——
  这能证明"浏览器要的东西真的能取到"，但**不能**证明真实渲染（那需要人看一眼）。

## 许可边界

项目本体 MIT；本目录的 `hls.min.js` 是 Apache-2.0（宽松、可与 MIT 共存）。
再分发时请保留 `LICENSE` 与本文的来源说明。
