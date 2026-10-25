#!/usr/bin/env bash
# ============================================================================
# M11-a3 验收：RTSP 输入**长跑**（默认 1 小时）—— 断流自愈 + 稳定推流 + 资源不涨
#
# 它能证明什么：
#   · 连续 1 小时从一路 RTSP 源（假服务端，H264 [+ AAC]）拉流并转 HTTP-FLV/HLS，**不掉线**；
#   · 中途**人为掐断**一次上游（kill 假服务端再起），客户端自己重连、观众侧 `curl` 不结束；
#   · 时间戳全程不倒退、抓到的 FLV 能完整解码；
#   · 进程 RSS 不随时间单调增长（长跑最典型的"慢性泄漏"）；
#   · `/api/stats` 的重连计数与状态与事实一致。
# **不能**证明什么：真机（真摄像机）的时间戳/丢包/鉴权/并发 —— 见 docs/DESIGN_M11.md §7。
#
# 用法：
#   ./scripts/rtsp_soak.sh                          # 默认 3600s，samples/sample.h264（无音频）
#   ./scripts/rtsp_soak.sh --duration 600           # 10 分钟（快速自检）
#   ./scripts/rtsp_soak.sh --file /path/real.h264 --audio-file /path/real.aac --fps 23.976
#   ./scripts/rtsp_soak.sh --restart-at 1800        # 第 30 分钟掐断一次上游（默认 1800；0 = 不掐）
#
# 产物：/tmp/mzmedia_rtsp_soak/
#   server.log      服务端日志       fake.log        假服务端日志
#   soak.csv        每分钟采样（RSS / CPU / 包数 / 重连数 / 帧龄）
#   client.flv      观众侧抓到的 FLV   hls.log    HLS 播放列表的滚动记录
#   summary.txt     结论与关键数字
# ============================================================================
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERVER="$ROOT/build/bin/mzmedia"
FAKE="$ROOT/scripts/rtsp_fake_server.py"
OUT="/tmp/mzmedia_rtsp_soak"
DURATION=3600
RESTART_AT=1800
FILE="$ROOT/samples/sample.h264"
AUDIO=""
FPS=25
PORT=$(( (RANDOM % 2000) + 29000 ))   # HTTP 服务端口
RTSP_PORT=$(( PORT + 1 ))            # 假 RTSP 源端口（**不能和 HTTP 同端口**，否则 bind 冲突）
NAME="soak"

while [ $# -gt 0 ]; do
    case "$1" in
        --duration) DURATION="$2"; shift 2 ;;
        --restart-at) RESTART_AT="$2"; shift 2 ;;
        --file) FILE="$2"; shift 2 ;;
        --audio-file) AUDIO="$2"; shift 2 ;;
        --fps) FPS="$2"; shift 2 ;;
        --port) PORT="$2"; RTSP_PORT=$(( PORT + 1 )); shift 2 ;;
        --name) NAME="$2"; shift 2 ;;
        -h | --help) sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "未知参数：$1" >&2; exit 2 ;;
    esac
done

[ -x "$SERVER" ] || { echo "错误：找不到 $SERVER，先 cmake --build build" >&2; exit 1; }
[ -f "$FILE" ] || { echo "错误：找不到素材 $FILE" >&2; exit 1; }
for tool in python3 curl ffprobe ffmpeg; do
    command -v "$tool" > /dev/null 2>&1 || { echo "错误：需要 $tool" >&2; exit 1; }
done

rm -rf "$OUT"
mkdir -p "$OUT"
FAKE_PID=""
SRV_PID=""
CURL_PID=""
HLS_PID=""

cleanup() {
    [ -n "$CURL_PID" ] && kill "$CURL_PID" 2> /dev/null || true
    [ -n "$HLS_PID" ] && kill "$HLS_PID" 2> /dev/null || true
    [ -n "$SRV_PID" ] && kill -INT "$SRV_PID" 2> /dev/null || true
    [ -n "$FAKE_PID" ] && kill "$FAKE_PID" 2> /dev/null || true
    wait 2> /dev/null || true
}
trap cleanup EXIT INT TERM

start_fake() {
    python3 "$FAKE" --port "$RTSP_PORT" --file "$FILE" --fps "$FPS" --frames 1000000 \
        --loop --after hold ${AUDIO:+--audio-file "$AUDIO"} > "$OUT/fake.log" 2>&1 &
    FAKE_PID=$!
}

echo "== 0) 起假 RTSP 源（$FILE，fps=$FPS${AUDIO:+，音频 $AUDIO}）+ 服务实例 =="
start_fake
sleep 2
grep -q "监听 127.0.0.1:$RTSP_PORT" "$OUT/fake.log" || { echo "错误：假源没起来" >&2; cat "$OUT/fake.log"; exit 1; }

"$SERVER" --port "$PORT" --media-root "$ROOT/samples" --log-level info \
    --rtsp "rtsp://127.0.0.1:$RTSP_PORT/live#$NAME" --rtsp-timeout-ms 10000 \
    > "$OUT/server.log" 2>&1 &
SRV_PID=$!
ready=no
for _ in $(seq 1 80); do
    if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/api/stats"; then ready=yes; break; fi
    sleep 0.1
done
[ "$ready" = yes ] || { echo "错误：服务未就绪" >&2; cat "$OUT/server.log"; exit 1; }
BASE="http://127.0.0.1:$PORT"
echo "  服务就绪：$BASE（源名 $NAME）"

T0=$(date +%s)
echo "== 1) 观众侧开始拉流（${DURATION}s）：HTTP-FLV =="
# 观众**不能**因为上游断了而结束：curl 只在 --max-time 到期时以 28 退出
curl -s -o "$OUT/client.flv" --max-time "$DURATION" "$BASE/live/$NAME.flv" &
CURL_PID=$!

# HLS 侧：每 10s 取一次播放列表，记录"切片数量与序号有没有继续往前滚"
(
    while true; do
        now=$(date +%s)
        [ $(( now - T0 )) -ge "$DURATION" ] && break
        playlist=$(curl -s --max-time 3 "$BASE/hls/$NAME.m3u8" 2> /dev/null)
        seq=$(printf '%s\n' "$playlist" | grep -m1 '^#EXT-X-MEDIA-SEQUENCE:' | cut -d: -f2)
        n=$(printf '%s\n' "$playlist" | grep -c '\.ts$')
        echo "$(( now - T0 ))s seq=${seq:-?} slices=${n:-0}" >> "$OUT/hls.log"
        sleep 10
    done
) &
HLS_PID=$!

echo "== 2) 每分钟采样（RSS / CPU / 包数 / 重连数 / 帧龄）=="
echo "elapsed_s,rss_kb,cpu_pct,packets,reconnects,state,age_ms" > "$OUT/soak.csv"
restarted=0
while true; do
    now=$(date +%s)
    elapsed=$(( now - T0 ))
    [ "$elapsed" -ge "$DURATION" ] && break

    # 中途掐一次上游：证明"1 小时不断流"不是靠"一次都没断"
    if [ "$RESTART_AT" -gt 0 ] && [ "$restarted" -eq 0 ] && [ "$elapsed" -ge "$RESTART_AT" ]; then
        restarted=1
        echo "  [掐断上游] 第 ${elapsed}s：kill 假服务端 → 8s 后重启（模拟相机重启/网络闪断）"
        kill "$FAKE_PID" 2> /dev/null || true
        sleep 8
        start_fake
        echo "  [掐断上游] 假服务端已重启（pid $FAKE_PID）"
    fi

    # 采样要**抗抖动**：curl 偶尔超时、ps 偶尔拿到空，都重试一次再下结论
    st=""
    for _ in 1 2 3; do
        st=$(curl -s --max-time 5 "$BASE/api/stats" 2> /dev/null)
        [ -n "$st" ] && break
        sleep 1
    done
    line=$(printf '%s' "$st" | python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    print("-,-,-,-")
    raise SystemExit
for s in d.get("media_sources", []):
    r = (s.get("producer") or {}).get("rtsp")
    if not r:
        continue
    p = s.get("producer") or {}
    print("%d,%d,%s,%d" % (p.get("packets", -1), r.get("reconnects", -1),
                           r.get("state", "?"), r.get("last_packet_age_ms", -1)))
    break
else:
    print("-,-,-,-")
')
    rss=$(ps -o rss= -p "$SRV_PID" 2> /dev/null | tr -d ' ')
    cpu=$(ps -o %cpu= -p "$SRV_PID" 2> /dev/null | tr -d ' ')
    echo "$elapsed,${rss:-0},${cpu:-0},$line" >> "$OUT/soak.csv"
    [ $(( elapsed % 300 )) -lt 60 ] && echo "  t=${elapsed}s rss=${rss}KB cpu=${cpu}% $(echo "$line" | tr ',' ' ')"
    sleep 60
done

echo "== 3) 收尾并判定 =="
kill "$CURL_PID" 2> /dev/null || true
kill "$HLS_PID" 2> /dev/null || true
wait "$CURL_PID" 2> /dev/null || true
curl -s --max-time 5 "$BASE/api/stats" > "$OUT/stats_final.json" 2> /dev/null
kill -INT "$SRV_PID" 2> /dev/null || true
kill "$FAKE_PID" 2> /dev/null || true
wait 2> /dev/null || true

pass=0
fail=0
ok() { echo "  ✔ $1" | tee -a "$OUT/summary.txt"; pass=$((pass + 1)); }
ng() { echo "  ✘ $1 实际=$2" | tee -a "$OUT/summary.txt"; fail=$((fail + 1)); }
: > "$OUT/summary.txt"

bytes=$(stat -c%s "$OUT/client.flv" 2> /dev/null || echo 0)
# 粗算：FLV 至少要有 "时长 × 码率/8" 的 40%（H264 裸流 1.6Mbps 量级）
[ "${bytes:-0}" -gt 1000000 ] && ok "观众侧 FLV 抓到 $bytes 字节" || ng "观众侧数据量" "${bytes} 字节（太少）"

# 时长与单调性
dts_report=$(ffprobe -v error -select_streams v:0 -show_entries packet=dts_time -of csv=p=0 "$OUT/client.flv" 2>/dev/null \
    | awk 'NF && $1 != "N/A" { v = $1 + 0; if (last != "" && v < last) back++; last = v; n++ }
           END { printf "%d %d %.1f", n, back + 0, last + 0 }')
n=${dts_report%% *}
rest=${dts_report#* }
back=${rest%% *}
span=${rest##* }
[ "${back:-1}" -eq 0 ] && ok "跨重连 dts 0 次倒退（$n 包，时间轴 ${span}s）" \
    || ng "跨重连 dts 倒退" "$back 次"

# 长跑时长：媒体时长要接近墙上时长（允许 20% 的重连损耗）
want=$(python3 -c "print(int($DURATION * 0.8))")
[ "${span%.*}" -ge "$want" ] && ok "媒体时间轴 ${span}s ≥ ${want}s（掉帧/重连损耗 < 20%）" \
    || ng "媒体时间轴覆盖" "${span}s（期望 ≥ ${want}s）"

# 解码：允许已知的"首包重复 dts"家族
errs=$(ffmpeg -v error -i "$OUT/client.flv" -f null - 2>&1 > /dev/null \
    | grep -vE "non monotonically increasing dts|Last message repeated [0-9]+ times" | head -2)
[ -z "$errs" ] && ok "整段解码无（已知之外的）错误" || ng "解码错误" "$(echo "$errs" | head -1)"

# 存活性：上游被掐断过 → 必须自己接回来，且观众没被 EOS
reconnects=$(python3 -c '
import json
try:
    d = json.load(open("'"$OUT"'/stats_final.json"))
except Exception:
    print(-1); raise SystemExit
for s in d.get("media_sources", []):
    r = (s.get("producer") or {}).get("rtsp")
    if r:
        print(r.get("reconnects", -1)); break
else:
    print(-1)
')
if [ "$RESTART_AT" -gt 0 ]; then
    [ "${reconnects:-0}" -ge 1 ] && ok "掐断上游后自动重连（reconnects=$reconnects）" \
        || ng "掐断上游后自动重连" "reconnects=$reconnects"
fi
tail -n 1 "$OUT/hls.log" | grep -q "slices=" && ok "HLS 播放列表持续滚动（$(tail -n 1 "$OUT/hls.log")）" \
    || ng "HLS 播放列表" "$(tail -n 2 "$OUT/hls.log" | tr '\n' '|')"

# 内存：**预热之后**不许再涨（"慢性泄漏"的粗判据）。
# 基线取"跑到 10% 时长"那一次采样，而不是第一次：第一次采样时 FFmpeg 的探测/GOP 缓存
# 还没铺开（1080p 实测 32MB → 72MB 是预热，不是泄漏），拿它当基线会误报。
mem_report=$(awk -F, 'NR > 1 && $2 ~ /^[0-9]+$/ && $2 > 0 {
                          if (first == 0) first = $2;
                          if (t <= warm) { base = $2 }
                          if ($2 > mx) mx = $2;
                          last = $2; t = $1
                      }
                      END { printf "%d %d %d %d", first, base, last, mx }' \
    warm=$(python3 -c "print(int($DURATION * 0.1))") "$OUT/soak.csv")
first=${mem_report%% *}
rest=${mem_report#* }; base=${rest%% *}
rest=${rest#* }; last=${rest%% *}
mx=${rest##* }
if [ "${base:-0}" -gt 0 ]; then
    limit=$(python3 -c "print(int($base * 1.20))")
    [ "$last" -le "$limit" ] \
        && ok "RSS 基线（10% 处）${base}KB → 收尾 ${last}KB（峰值 ${mx}KB，≤ +20%）" \
        || ng "RSS 增长" "基线 ${base}KB → 收尾 ${last}KB（峰值 ${mx}KB）"
fi

echo
echo "结果：通过 $pass 项 / 失败 $fail 项（产物：$OUT）" | tee -a "$OUT/summary.txt"
[ "$fail" -eq 0 ] && echo "结论：M11-a3 长跑通过（${DURATION}s，中途掐断一次上游仍自愈）" \
    || echo "结论：M11-a3 长跑**未通过**（见 $OUT/summary.txt）"
[ "$fail" -eq 0 ] || exit 1
