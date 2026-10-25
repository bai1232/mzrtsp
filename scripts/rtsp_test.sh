#!/usr/bin/env bash
# ============================================================================
# M11-a1 验收：RTSP 输入（能连上、能拉、缺参数集当场拒绝）
#
# 为什么能用脚本验：真摄像机不可控，所以用一个**零依赖的假 RTSP 服务端**
# （`scripts/rtsp_fake_server.py`，纯 python3 标准库）把"协议行为"变成可注入的：
#   · 正常源（SDP 带 sprop-parameter-sets，`--loop` 使关键帧周期性出现 → HLS 能切片）
#   · 缺参数集的源（`--no-sprop`）→ 必须**当场**拒绝（不能空等到 open 超时）
#   · 只接受 TCP 的源（`--tcp-only`，对 UDP SETUP 回 461）→ FFmpeg 应自动回退 TCP
#   · 带凭据的 URL → 日志与 /api/stats 必须脱敏
#
# **不能**证明（要真机，见 docs/DESIGN_M11.md §7）：真实摄像机的时间戳/乱序行为、
# UDP 丢包、RTSP 鉴权（Basic/Digest）、断流重连（属 M11-a2）。
#
# 用法：./scripts/rtsp_test.sh
# 依赖：build/bin/mzmedia、samples/sample.h264、python3、curl、ffprobe、ffmpeg
# ============================================================================
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERVER="$ROOT/build/bin/mzmedia"
FAKE="$ROOT/scripts/rtsp_fake_server.py"
SAMPLE="$ROOT/samples/sample.h264"
PORT=$(( (RANDOM % 2000) + 26000 ))
RTSP_OK=$(( PORT + 100 ))
RTSP_NOSPROP=$(( PORT + 101 ))
RTSP_TCPONLY=$(( PORT + 102 ))
OUT="/tmp/mzmedia_rtsp"
SRV_PID=""
FAKE_PIDS=""

[ -x "$SERVER" ] || { echo "错误：找不到 $SERVER，先 cmake --build build" >&2; exit 1; }
[ -f "$FAKE" ] || { echo "错误：找不到 $FAKE" >&2; exit 1; }
[ -f "$SAMPLE" ] || { echo "错误：找不到 samples/sample.h264，先 ./scripts/make_samples.sh" >&2; exit 1; }
for tool in python3 curl ffprobe ffmpeg; do
    command -v "$tool" > /dev/null 2>&1 || { echo "错误：需要 $tool" >&2; exit 1; }
done

cleanup() {
    [ -n "$SRV_PID" ] && kill -INT "$SRV_PID" 2> /dev/null || true
    for pid in $FAKE_PIDS; do kill "$pid" 2> /dev/null || true; done
    wait 2> /dev/null || true
}
trap cleanup EXIT

rm -rf "$OUT"; mkdir -p "$OUT"

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

echo "== 0) 起三个假 RTSP 源 + 一个服务实例 =="
start_fake() { # start_fake <port> <额外参数...>
    local port="$1"; shift
    python3 "$FAKE" --port "$port" --file "$SAMPLE" "$@" > "$OUT/fake_$port.log" 2>&1 &
    FAKE_PIDS="$FAKE_PIDS $!"
}
start_fake "$RTSP_OK" --frames 400 --loop --after hold
start_fake "$RTSP_NOSPROP" --frames 100 --after hold --no-sprop
start_fake "$RTSP_TCPONLY" --frames 400 --loop --after hold --tcp-only
sleep 1.5
ok_sources=0
for port in "$RTSP_OK" "$RTSP_NOSPROP" "$RTSP_TCPONLY"; do
    grep -q "监听 127.0.0.1:$port" "$OUT/fake_$port.log" && ok_sources=$((ok_sources + 1))
done
check "三个假 RTSP 源就绪" "$ok_sources" "3"

"$SERVER" --port "$PORT" --media-root "$ROOT/samples" --log-level info \
    --rtsp "rtsp://127.0.0.1:$RTSP_OK/live#cam1" \
    --rtsp "rtsp://127.0.0.1:$RTSP_NOSPROP/live#bad" \
    --rtsp "rtsp://127.0.0.1:$RTSP_TCPONLY/live#tcp" \
    --rtsp "rtsp://user:secret123@127.0.0.1:$RTSP_OK/live#auth" \
    > "$OUT/server.log" 2>&1 &
SRV_PID=$!
ready=no
for _ in $(seq 1 80); do
    if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/api/stats"; then ready=yes; break; fi
    sleep 0.1
done
if [ "$ready" != yes ]; then
    echo "错误：服务未就绪，日志：" >&2; cat "$OUT/server.log" >&2; exit 1
fi
BASE="http://127.0.0.1:$PORT"
check_true "服务已就绪（带 4 个 RTSP 注册）"
if grep -q "RTSP 输入" "$OUT/server.log"; then
    check_true "启动横幅列出 RTSP 源（脱敏）"
else
    check_false "启动横幅列出 RTSP 源" "日志里没有"
fi

echo "== 1) 正常源：/live/<name>.flv 能拉到合法 FLV =="
# 直播流不会自己结束：curl 到 --max-time 必然返回 28，所以 `|| true` 后再读 http_code
code=$(curl -s -o "$OUT/cam1.flv" -w '%{http_code}' --max-time 10 "$BASE/live/cam1.flv" || true)
check "GET /live/cam1.flv → 200" "$code" "200"
check "前 3 字节是 FLV" "$(head -c 3 "$OUT/cam1.flv" | tr -d '\0')" "FLV"
check "视频编码" "$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$OUT/cam1.flv" | awk 'NF' | head -1)" "h264"
check "分辨率（假源是 320x240）" \
    "$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$OUT/cam1.flv" | awk 'NF' | head -1)" "320,240"
if [ -z "$(ffmpeg -v error -i "$OUT/cam1.flv" -f null - 2>&1 > /dev/null || true)" ]; then
    check_true "RTSP → FLV 完整解码无 error"
else
    check_false "RTSP → FLV 完整解码无 error" "$(ffmpeg -v error -i "$OUT/cam1.flv" -f null - 2>&1 > /dev/null | head -2)"
fi

echo "== 2) 同一条 RTSP 源也能出 HLS（切片可独立解码）=="
curl -s -o /dev/null --max-time 5 "$BASE/hls/cam1.m3u8"
sleep 6
curl -s -o "$OUT/cam1.m3u8" --max-time 5 "$BASE/hls/cam1.m3u8"
n_slices=$(grep -c '^/hls/cam1' "$OUT/cam1.m3u8" || true)
if [ "${n_slices:-0}" -ge 1 ]; then
    check_true "HLS 播放列表有切片（$n_slices 片）"
else
    check_false "HLS 播放列表有切片" "$(head -6 "$OUT/cam1.m3u8" | tr '\n' '|')"
fi
slice_uri=$(grep -o '/hls/cam1-[0-9]*\.ts' "$OUT/cam1.m3u8" | tail -1)
if [ -n "$slice_uri" ]; then
    code=$(curl -s -o "$OUT/cam1.ts" -w '%{http_code}' --max-time 5 "$BASE$slice_uri")
    check "RTSP 源的 HLS 切片 → 200" "$code" "200"
    check "切片的视频编码" \
        "$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$OUT/cam1.ts" | awk 'NF' | head -1)" "h264"
    if [ -z "$(ffmpeg -v error -i "$OUT/cam1.ts" -f null - 2>&1 > /dev/null || true)" ]; then
        check_true "RTSP 源的 HLS 切片解码无 error"
    else
        check_false "RTSP 源的 HLS 切片解码无 error" "$(ffmpeg -v error -i "$OUT/cam1.ts" -f null - 2>&1 > /dev/null | head -2)"
    fi
fi

echo "== 3) /api/stats 的 rtsp 段（FR-6.1）=="
curl -s --max-time 5 "$BASE/api/stats" > "$OUT/stats.json"
rtsp_json="$(python3 - "$OUT/stats.json" << 'PY'
import json, sys
d = json.load(open(sys.argv[1]))
rows = [(s.get("path"), (s.get("producer") or {}).get("rtsp")) for s in d.get("media_sources", [])]
rows = [(p, r) for p, r in rows if r]
if not rows:
    sys.exit("stats 里没有 rtsp 段")
p, r = rows[0]
need = ["enabled", "state", "connects", "last_packet_age_ms"]
missing = [k for k in need if k not in r]
if missing:
    sys.exit("缺少字段：%s" % missing)
print("sources=%d state=%s age_ms=%s" % (len(rows), r["state"], r["last_packet_age_ms"]))
PY
)" 
if [ -n "$rtsp_json" ]; then
    printf '  ✔ %-44s %s\n' "stats：rtsp 段字段齐全" "$rtsp_json"
    pass=$((pass + 1))
else
    check_false "stats：rtsp 段字段齐全" "见 $OUT/stats.json"
fi

echo "== 4) 缺 SPS/PPS 的源：必须**当场**拒绝（不能空等到 open 超时）=="
t0=$(date +%s%3N)
code=$(curl -s -o "$OUT/bad.txt" -w '%{http_code}' --max-time 15 "$BASE/live/bad.flv")
t1=$(date +%s%3N)
cost_ms=$(( t1 - t0 ))
check "缺参数集 → 500（明确失败）" "$code" "500"
if grep -q "sprop-parameter-sets" "$OUT/bad.txt"; then
    check_true "错误信息点名 SDP 缺 sprop-parameter-sets"
else
    check_false "错误信息点名 sprop-parameter-sets" "$(head -2 "$OUT/bad.txt")"
fi
if [ "$cost_ms" -lt 3000 ]; then
    printf '  ✔ %-44s %s ms（要求 < 3000）\n' "缺参数集的失败耗时" "$cost_ms"
    pass=$((pass + 1))
else
    check_false "缺参数集的失败耗时" "${cost_ms} ms（要求 < 3000：不许空等 open 超时）"
fi

echo "== 5) 只接受 TCP 的源（UDP SETUP 被 461）：FFmpeg 自动回退 =="
code=$(curl -s -o "$OUT/tcp.flv" -w '%{http_code}' --max-time 10 "$BASE/live/tcp.flv" || true)
check "GET /live/tcp.flv → 200（TCP-only 源也能拉）" "$code" "200"
check "TCP-only 源的编码" \
    "$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$OUT/tcp.flv" | awk 'NF' | head -1)" "h264"

echo "== 6) 未注册的名字 → 404（URL 不能从请求里注入）=="
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$BASE/live/notregistered.flv")
check "未注册名字 → 404" "$code" "404"

echo "== 7) 凭据脱敏：日志与 stats 里不许出现明文密码 =="
curl -s -o /dev/null --max-time 10 "$BASE/live/auth.flv" || true
curl -s --max-time 5 "$BASE/api/stats" > "$OUT/stats2.json"
leak_log=$(grep -c "secret123" "$OUT/server.log" || true)
leak_stats=$(grep -c "secret123" "$OUT/stats2.json" || true)
check "日志里没有明文密码" "${leak_log:-0}" "0"
check "stats 里没有明文密码" "${leak_stats:-0}" "0"
if grep -q '\*\*\*' "$OUT/server.log"; then
    check_true "日志里能看到脱敏后的 URL（user:***@）"
else
    check_false "日志里能看到脱敏后的 URL" "没找到 ***"
fi

echo
echo "结果：通过 $pass 项 / 失败 $fail 项（产物：$OUT）"
[ "$fail" -eq 0 ] || { echo "结论：M11-a1 未通过"; exit 1; }
echo "结论：M11-a1 通过（RTSP 能连、能拉 FLV 与 HLS、缺参数集当场拒绝、TCP 回退、凭据脱敏）"
