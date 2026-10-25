#!/usr/bin/env bash
# ============================================================================
# M9-b 验收：HLS 输出（切片 2s / 窗口 6 / m3u8 / Range / 租约释放）
#
# 为什么必须有它：m3u8 写错、切片没切在关键帧、时间戳不单调 —— 这些都**不会崩**，
# 表现是"播放器转圈"或"播两秒停"。所以判据必须是外部的：
#   ① `ffmpeg` 直接读 m3u8 并解码（它同时验证 播放列表语法 + 切片内容 + 时间戳）；
#   ② 切片字节的结构判据（188 对齐、同步字、PAT/PMT 在片头）；
#   ③ HTTP 面的语义（Content-Type、Range 206、滑出窗口的老切片 404）。
#
# 用法：./scripts/hls_test.sh
# 依赖：build/bin/mzmedia、samples/sample.mp4、curl、ffprobe、ffmpeg、python3
# ============================================================================
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERVER="$ROOT/build/bin/mzmedia"
SAMPLE="$ROOT/samples/sample.mp4"
TX_SAMPLE="$ROOT/samples/sample1080.mp4"   # M10-c：转码 + HLS（1080p → 120p）
PORT=$(( (RANDOM % 3000) + 24000 ))
LEASE_PORT=$(( PORT + 1 ))
TX_PORT=$(( PORT + 2 ))
OUT="/tmp/mzmedia_hls"
SRV_PID=""
LEASE_PID=""
TX_PID=""

[ -x "$SERVER" ] || { echo "错误：找不到 $SERVER，先执行 cmake --build build" >&2; exit 1; }
[ -f "$SAMPLE" ] || { echo "错误：找不到样本，先执行 ./scripts/make_samples.sh" >&2; exit 1; }
for tool in curl ffprobe ffmpeg python3; do
    command -v "$tool" > /dev/null 2>&1 || { echo "错误：需要 $tool" >&2; exit 1; }
done

pass=0
fail=0
check() { # check <说明> <实际> <期望>
    if [ "$2" = "$3" ]; then
        printf '  ✔ %-44s %s\n' "$1" "$2"; pass=$((pass + 1))
    else
        printf '  ✘ %-44s 实际=%s 期望=%s\n' "$1" "$2" "$3"; fail=$((fail + 1))
    fi
}
check_true() { printf '  ✔ %-44s\n' "$1"; pass=$((pass + 1)); }
check_false() { printf '  ✘ %-44s %s\n' "$1" "$2"; fail=$((fail + 1)); }

cleanup() {
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null || true
    [ -n "$TX_PID" ] && kill "$TX_PID" 2>/dev/null || true
    [ -n "$LEASE_PID" ] && kill "$LEASE_PID" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

rm -rf "$OUT"; mkdir -p "$OUT"
cp "$SAMPLE" "$OUT/movie.mp4"

echo "== 0) 起服务（端口 $PORT；--loop --speed 2 让切片快点出来）=="
"$SERVER" --port "$PORT" --media-root "$OUT" --loop --speed 2 \
    --log-file "$OUT/server.log" > /dev/null 2>&1 &
SRV_PID=$!
sleep 1.5

BASE="http://127.0.0.1:$PORT"
M3U8="$BASE/hls/movie.mp4.m3u8"

echo "== 1) 懒启动 + 播放列表头部 =="
head_code="$(curl -s -o "$OUT/first.m3u8" -w '%{http_code}' --max-time 5 "$M3U8")"
check "首次请求 /hls/movie.mp4.m3u8 → 200" "$head_code" "200"
ctype="$(curl -s -o /dev/null -D - --max-time 5 "$M3U8" | tr -d '\r' | awk 'tolower($1)=="content-type:" {print $2}')"
check "Content-Type" "$ctype" "application/vnd.apple.mpegurl"
grep -q '^#EXTM3U' "$OUT/first.m3u8" && check_true "首行是 #EXTM3U" || check_false "首行是 #EXTM3U" "$(head -1 "$OUT/first.m3u8")"

echo "== 1b) 播放页与 hls.js（M9-c：浏览器要的东西真的能取到）=="
# 说明：这里只证明"页面/资源能取到、JS 语法没坏"，**不证明真实渲染**（那需要人看一眼）。
# 但对播放页来说，这两件事恰好是最容易坏的：文件没入库、模板字符串被改坏。
page_code="$(curl -s -o "$OUT/page.html" -w '%{http_code}' --max-time 5 "$BASE/")"
check "GET / → 200" "$page_code" "200"
for marker in 'hls.min.js' 'play-hls' 'Hls.isSupported' '/hls/' 'flv.min.js'; do
    if grep -q -- "$marker" "$OUT/page.html"; then
        check_true "页面含 $marker"
    else
        check_false "页面含 $marker" "缺"
    fi
done
js_code="$(curl -s -o "$OUT/hls.min.js" -w '%{http_code}' --max-time 10 "$BASE/hls.min.js")"
check "GET /hls.min.js → 200" "$js_code" "200"
if cmp -s "$ROOT/third_party/hls.js/hls.min.js" "$OUT/hls.min.js"; then
    check_true "hls.min.js 与入库文件**逐字节一致**（$(stat -c%s "$OUT/hls.min.js") 字节）"
else
    check_false "hls.min.js 与入库文件逐字节一致" "内容不同"
fi
# 内联 JS 的语法检查：页面坏了的话，浏览器只会白屏，任何 HTTP 断言都看不出来
if command -v node > /dev/null 2>&1; then
    python3 - "$OUT/page.html" > "$OUT/inline.js" << 'PY'
import re, sys
html = open(sys.argv[1], encoding="utf-8").read()
# 取**最后一个**没有 src 的 <script>（页面里的内联脚本）
blocks = re.findall(r"<script(?![^>]*\bsrc=)[^>]*>(.*?)</script>", html, re.S)
if not blocks:
    sys.exit("页面里没有内联 <script>")
sys.stdout.write(blocks[-1])
PY
    if node --check "$OUT/inline.js" > "$OUT/node_check.txt" 2>&1; then
        check_true "播放页内联 JS 语法正确（node --check）"
    else
        check_false "播放页内联 JS 语法正确" "$(head -2 "$OUT/node_check.txt")"
    fi
else
    printf '  ⚠ %-44s 环境没有 node，**未检查**（不计入通过项）\n' "播放页内联 JS 语法"
fi

echo "== 2) 等 6 秒让切片出来（2 倍速 ≈ 12 秒媒体；窗口 6 片覆盖 6 秒墙钟）=="
sleep 6
curl -s --max-time 5 -o "$OUT/list.m3u8" "$M3U8"
grep -q '^#EXT-X-TARGETDURATION:' "$OUT/list.m3u8" && check_true "有 #EXT-X-TARGETDURATION" || check_false "有 #EXT-X-TARGETDURATION" "缺"
grep -q '^#EXT-X-MEDIA-SEQUENCE:' "$OUT/list.m3u8" && check_true "有 #EXT-X-MEDIA-SEQUENCE" || check_false "有 #EXT-X-MEDIA-SEQUENCE" "缺"
n_slices="$(grep -c '\.ts$' "$OUT/list.m3u8" || true)"
if [ "$n_slices" -ge 1 ] && [ "$n_slices" -le 6 ]; then
    check_true "切片数在 1..6（窗口上限 6）：$n_slices"
else
    check_false "切片数在 1..6（窗口上限 6）" "$n_slices"
fi
# 播放列表里的 URI 必须能被我们的路由解析（形态 /hls/<name>-<seq>.ts）
grep -qE '^/hls/movie\.mp4-[0-9]+\.ts$' "$OUT/list.m3u8" && check_true "切片 URI 形态正确" || check_false "切片 URI 形态正确" "$(grep '\.ts$' "$OUT/list.m3u8" | head -1)"

echo "== 3) 取当前窗口里的第一片 → 结构判据 =="
# 取**最新**的一片：窗口在滑动，脚本每做一次请求都要花时间，
# 拿最老的片会被滑出窗口 → 404（这是**测试自身**的竞态，不是服务端 bug）。
curl -s --max-time 5 -o "$OUT/list.ts" "$M3U8"
first_uri="$(grep -E '^/hls/movie\.mp4-[0-9]+\.ts$' "$OUT/list.ts" | tail -1)"
first_seq="$(echo "$first_uri" | sed -E 's#^/hls/movie\.mp4-([0-9]+)\.ts$#\1#')"
code="$(curl -s -o "$OUT/slice.ts" -w '%{http_code}' --max-time 5 "$BASE$first_uri")"
check "GET 切片（seq=$first_seq，取最新一片）→ 200" "$code" "200"
sctype="$(curl -s -o /dev/null -D - --max-time 5 "$BASE$first_uri" | tr -d '\r' | awk 'tolower($1)=="content-type:" {print $2}')"
check "切片 Content-Type" "$sctype" "video/mp2t"
if python3 - "$OUT/slice.ts" > "$OUT/struct.txt" 2>&1 << 'PY'
import sys
data = open(sys.argv[1], 'rb').read()
if len(data) == 0:
    sys.exit("空切片")
if len(data) % 188 != 0:
    sys.exit("长度不是 188 的整数倍")
if any(data[i] != 0x47 for i in range(0, len(data), 188)):
    sys.exit("存在缺同步字的包")
pids = {((data[i + 1] & 0x1F) << 8) | data[i + 2] for i in range(0, len(data), 188)}
if not {0x0000, 0x1000}.issubset(pids):
    sys.exit("切片缺 PAT/PMT（PID 集合 %s）" % sorted(pids))
print("%d 字节 / %d 包 / PID %s" % (len(data), len(data) // 188, sorted(pids)))
PY
then
    check_true "切片结构（$(cat "$OUT/struct.txt")）"
else
    check_false "切片结构" "$(cat "$OUT/struct.txt")"
fi

echo "== 4) Range（FR-4.2）=="
range_headers="$(curl -s -o /dev/null -D - -H 'Range: bytes=0-187' --max-time 5 "$BASE$first_uri" | tr -d '\r')"
echo "$range_headers" | grep -q "206" && check_true "Range 请求 → 206" || check_false "Range 请求 → 206" "$(echo "$range_headers" | head -1)"
echo "$range_headers" | grep -qi "content-range: bytes 0-187/" && check_true "Content-Range 正确" || check_false "Content-Range 正确" "$(echo "$range_headers" | grep -i content-range | head -1)"
echo "$range_headers" | grep -qi "accept-ranges: bytes" && check_true "带 Accept-Ranges: bytes" || check_false "带 Accept-Ranges" "缺"
range_len="$(echo "$range_headers" | awk 'tolower($1)=="content-length:" {print $2}')"
check "Range 响应体长度" "$range_len" "188"

echo "== 5) 关键帧判据：切片首个视频包必须是关键帧 =="
flags_first="$(ffprobe -v error -select_streams v:0 -show_entries packet=flags -of csv=p=0 "$OUT/slice.ts" | awk 'NF' | head -1)"
case "$flags_first" in
    *K*) check_true "切片首帧是关键帧（flags=$flags_first）" ;;
    *)   check_false "切片首帧是关键帧" "flags=$flags_first" ;;
esac

echo "== 6) 独立裁判：ffmpeg 直接读 m3u8 并解码 4 秒 =="
err="$(ffmpeg -v error -t 4 -i "$M3U8" -f null - 2>&1 > /dev/null || true)"
if [ -z "$err" ]; then
    check_true "ffmpeg 读 m3u8 完整解码无 error"
else
    check_false "ffmpeg 读 m3u8 完整解码无 error" "$(echo "$err" | head -2)"
fi

echo "== 7) 窗口滚动：MEDIA-SEQUENCE 必须前进 =="
seq_before="$(grep -oE '^#EXT-X-MEDIA-SEQUENCE:[0-9]+' "$OUT/list.m3u8" | cut -d: -f2)"
sleep 5
curl -s --max-time 5 -o "$OUT/list2.m3u8" "$M3U8"
seq_after="$(grep -oE '^#EXT-X-MEDIA-SEQUENCE:[0-9]+' "$OUT/list2.m3u8" | cut -d: -f2)"
if [ "${seq_after:-0}" -gt "${seq_before:-0}" ]; then
    check_true "MEDIA-SEQUENCE 前进（$seq_before → $seq_after）"
else
    check_false "MEDIA-SEQUENCE 前进" "$seq_before → $seq_after"
fi
old_code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$BASE/hls/movie.mp4-0.ts")"
check "滑出窗口的老切片 → 404" "$old_code" "404"
# 播放列表里最多 6 片（滚动窗口）
n2="$(grep -c '\.ts$' "$OUT/list2.m3u8" || true)"
if [ "$n2" -ge 1 ] && [ "$n2" -le 6 ]; then
    check_true "窗口仍不超过 6 片：$n2"
else
    check_false "窗口仍不超过 6 片" "$n2"
fi

echo "== 8) /api/stats 暴露 HLS 段（FR-6.1）=="
if curl -s --max-time 5 "$BASE/api/stats" | python3 -c '
import sys, json
d = json.load(sys.stdin)
h = d.get("hls")
assert isinstance(h, dict), "没有 hls 段"
assert h["streams"] >= 1, "streams 应 >= 1"
assert h["window"] == 6, "window 应为 6"
assert h["target_duration_ms"] == 2000, "target 应为 2000"
assert len(h.get("playlists", [])) >= 1
print("ok")
' > /dev/null 2>&1; then
    check_true "/api/stats 的 hls 段字段正确"
else
    check_false "/api/stats 的 hls 段字段正确" "字段缺失或值不对"
fi

echo "== 9) 租约到期释放（FR-1.3 的 HLS 版本；用 --hls-lease-ms 2s 加速验证）=="
"$SERVER" --port "$LEASE_PORT" --media-root "$OUT" --hls-lease-ms 2000 \
    --log-file "$OUT/lease.log" > /dev/null 2>&1 &
LEASE_PID=$!
sleep 1.2
LBASE="http://127.0.0.1:$LEASE_PORT"
curl -s -o /dev/null --max-time 5 "$LBASE/hls/movie.mp4.m3u8"   # 建流
sleep 1
alive="$(curl -s --max-time 5 "$LBASE/api/stats" | python3 -c 'import sys,json;print(json.load(sys.stdin).get("hls",{}).get("streams",-1))' 2>/dev/null || echo "?")"
check "拉流后 hls.streams" "$alive" "1"
sleep 5   # 超过租约(2s) + 扫描间隔
released="$(curl -s --max-time 5 "$LBASE/api/stats" | python3 -c 'import sys,json;print(json.load(sys.stdin).get("hls",{}).get("streams",-1))' 2>/dev/null || echo "?")"
expired="$(curl -s --max-time 5 "$LBASE/api/stats" | python3 -c 'import sys,json;print(json.load(sys.stdin).get("hls",{}).get("lease_expired",-1))' 2>/dev/null || echo "?")"
check "租约到期后 hls.streams" "$released" "0"
if [ "${expired:-0}" -ge 1 ]; then
    check_true "lease_expired 计数 >= 1（$expired）"
else
    check_false "lease_expired 计数 >= 1" "$expired"
fi

echo "== 10) 转码 + HLS（M10-c）：播放列表里的每个切片都要带上同一份规格 =="
# 为什么必须验：播放器**只认播放列表里给出的 URI**。切片 URI 若没带 ?transcode=1&h=120，
# 播放器会去请求直通切片 → 画面突然变回 1080p（甚至来回跳），而服务端不会报任何错。
TXDIR="/tmp/mzmedia_hls_tx"
rm -rf "$TXDIR"; mkdir -p "$TXDIR"
cp "$TX_SAMPLE" "$TXDIR/tx.mp4"
"$SERVER" --port "$TX_PORT" --media-root "$TXDIR" --loop --speed 2 --transcode \
    --transcode-height 120 --log-file "$TXDIR/server.log" > /dev/null 2>&1 &
TX_PID=$!
sleep 1.2
TBASE="http://127.0.0.1:$TX_PORT"
# ① 没开转码的实例 → 501（不降级成直通）
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$BASE/hls/movie.mp4.m3u8?transcode=1")
check "未开转码时 HLS + ?transcode=1 → 501" "$code" "501"
# ② 开了转码：m3u8 里每个切片 URI 都必须带同一份查询串
curl -s -o /dev/null --max-time 5 "$TBASE/hls/tx.mp4.m3u8?transcode=1&h=120"   # 先建流
sleep 6                                                                         # 2 倍速 ≈ 12 秒媒体
curl -s -o "$TXDIR/tx.m3u8" --max-time 5 "$TBASE/hls/tx.mp4.m3u8?transcode=1&h=120"
slices=$(grep -c 'transcode=1&h=120' "$TXDIR/tx.m3u8" || true)
if [ "${slices:-0}" -ge 1 ]; then
    check_true "转码 m3u8 的切片 URI 带规格（$slices 处）"
else
    check_false "转码 m3u8 的切片 URI 带规格" "$(head -6 "$TXDIR/tx.m3u8" | tr '\n' '|')"
fi
# ③ 按播放列表里的 URI 取切片 → 必须是 120p（等比：1920x1080 → 212x120）
txslice=$(grep -o '/hls/tx\.mp4-[0-9]*\.ts?transcode=1&h=120' "$TXDIR/tx.m3u8" | tail -1)
code=$(curl -s -o "$TXDIR/slice.ts" -w '%{http_code}' --max-time 5 "$TBASE$txslice")
check "转码切片 → 200" "$code" "200"
check "转码切片的编码" \
    "$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$TXDIR/slice.ts" | awk 'NF' | head -1)" "h264"
check "转码切片的高度（--transcode-height 120）" \
    "$(ffprobe -v error -select_streams v:0 -show_entries stream=height -of csv=p=0 "$TXDIR/slice.ts" | awk 'NF' | head -1)" "120"
# ④ 直通 HLS 不受影响（同一个服务、不带查询串 → 仍是 1920x1080）
curl -s -o /dev/null --max-time 5 "$TBASE/hls/tx.mp4.m3u8"
sleep 6
curl -s -o "$TXDIR/plain.m3u8" --max-time 5 "$TBASE/hls/tx.mp4.m3u8"
pslice=$(grep -o '/hls/tx\.mp4-[0-9]*\.ts' "$TXDIR/plain.m3u8" | tail -1)
curl -s -o "$TXDIR/plain.ts" --max-time 5 "$TBASE$pslice"
check "直通切片的宽度（不受转码影响）" \
    "$(ffprobe -v error -select_streams v:0 -show_entries stream=width -of csv=p=0 "$TXDIR/plain.ts" | awk 'NF' | head -1)" "1920"
# ⑤ 独立裁判：ffmpeg 直接读**转码的** m3u8 并解码 4 秒
tx_err="$(ffmpeg -v error -i "$TBASE/hls/tx.mp4.m3u8?transcode=1&h=120" -t 4 -f null - 2>&1 > /dev/null || true)"
if [ -z "$tx_err" ]; then
    check_true "ffmpeg 直接读转码 m3u8 解码 4 秒无 error"
else
    check_false "ffmpeg 直接读转码 m3u8 解码 4 秒无 error" "$(printf '%s' "$tx_err" | head -2)"
fi
# ⑥ stats：同名两条流（直通 + 转码），key 不同、transcode 标记正确
curl -s --max-time 5 "$TBASE/api/stats" > "$TXDIR/stats.json"
set +e
tx_stats="$(python3 - "$TXDIR/stats.json" << 'PYTX'
import json, sys
d = json.load(open(sys.argv[1]))
rows = [(p.get("name"), p.get("key"), p.get("transcode") or {}) for p in d.get("hls", {}).get("playlists", [])]
tx = [r for r in rows if r[0] == "tx.mp4" and r[2].get("enabled")]
plain = [r for r in rows if r[0] == "tx.mp4" and not r[2].get("enabled")]
print("streams=%d tx=%d plain=%d tx_key=%s" % (len(rows), len(tx), len(plain), tx[0][1] if tx else "-"))
sys.exit(0 if (len(tx) == 1 and len(plain) == 1) else 1)
PYTX
)"
tx_rc=$?
set -e
if [ "$tx_rc" -eq 0 ]; then
    printf '  ✔ %-44s %s\n' "stats：同名两条流（直通 + 转码）" "$tx_stats"
    pass=$((pass + 1))
else
    check_false "stats：同名两条流（直通 + 转码）" "$tx_stats"
fi

echo
echo "结果：通过 $pass 项 / 失败 $fail 项（产物：$OUT / $TXDIR）"
[ "$fail" -eq 0 ] || exit 1
echo "结论：M9-b + M10-c 通过（m3u8 结构/切片可独立解码/Range/窗口滚动/租约释放/转码+HLS，ffmpeg 独立判定无 error）"
