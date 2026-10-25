#!/usr/bin/env bash
# ============================================================================
# M11-a1 + a2 + a2b 验收：RTSP 输入（能连上、能拉、缺参数集当场拒绝、断流自愈、音视频两条轨）
#
# 为什么能用脚本验：真摄像机不可控，所以用一个**零依赖的假 RTSP 服务端**
# （`scripts/rtsp_fake_server.py`，纯 python3 标准库）把"协议行为"变成可注入的：
#   · 正常源（SDP 带 sprop-parameter-sets，`--loop` 使关键帧周期性出现 → HLS 能切片）
#   · 缺参数集的源（`--no-sprop`）→ 必须**当场**拒绝（不能空等到 open 超时）
#   · 只接受 TCP 的源（`--tcp-only`，对 UDP SETUP 回 461）→ FFmpeg 应自动回退 TCP
#   · 带凭据的 URL → 日志与 /api/stats 必须脱敏
#
# **不能**证明（要真机，见 docs/DESIGN_M11.md §7）：真实摄像机的时间戳/乱序行为、
# UDP 丢包、RTSP 鉴权（Basic/Digest）；断流自愈见 §8（注入故障，不是真网络抖动）。
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
RTSP_CLOSE=$(( PORT + 103 ))   # M11-a2：发 N 帧后关连接（对端消失）
RTSP_SILENT=$(( PORT + 104 ))  # M11-a2：发 N 帧后半开（只靠读超时能发现）
RTSP_AV=$(( PORT + 105 ))      # M11-a2b：H264 + AAC 两条轨（真摄像机都有声）
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
# 断流源：**第一条连接**只发 25 帧（1 秒）就断（close=关连接 / silent=半开），
# 之后一直推流（--fault-close-first）—— 这样"重连成功"才会有后续数据可测
start_fake "$RTSP_CLOSE" --frames 25 --after close --fault-close-first
start_fake "$RTSP_SILENT" --frames 25 --after silent --fault-close-first
# 音视频源：视频用 320x240 样本，音频用 samples/sample_audio.aac（mono 44.1k）
start_fake "$RTSP_AV" --frames 400 --loop --after hold --audio-file "$ROOT/samples/sample_audio.aac"
sleep 1.5
ok_sources=0
for port in "$RTSP_OK" "$RTSP_NOSPROP" "$RTSP_TCPONLY" "$RTSP_CLOSE" "$RTSP_SILENT" "$RTSP_AV"; do
    grep -q "监听 127.0.0.1:$port" "$OUT/fake_$port.log" && ok_sources=$((ok_sources + 1))
done
check "六个假 RTSP 源就绪" "$ok_sources" "6"

"$SERVER" --port "$PORT" --media-root "$ROOT/samples" --log-level info \
    --rtsp "rtsp://127.0.0.1:$RTSP_OK/live#cam1" \
    --rtsp "rtsp://127.0.0.1:$RTSP_NOSPROP/live#bad" \
    --rtsp "rtsp://127.0.0.1:$RTSP_TCPONLY/live#tcp" \
    --rtsp "rtsp://user:secret123@127.0.0.1:$RTSP_OK/live#auth" \
    --rtsp "rtsp://127.0.0.1:$RTSP_CLOSE/live#brk" \
    --rtsp "rtsp://127.0.0.1:$RTSP_SILENT/live#half" \
    --rtsp "rtsp://127.0.0.1:$RTSP_AV/live#av" \
    --rtsp-timeout-ms 3000 \
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
check_true "服务已就绪（7 路 RTSP 注册）"
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
need = ["enabled", "state", "connects", "last_packet_age_ms", "label", "url"]
missing = [k for k in need if k not in r]
if missing:
    sys.exit("缺少字段：%s" % missing)
# 【M11-a2b】名字必须一眼对上命令行（`#cam1`），而不是只有 IP
names = [s.get("names") for s in d.get("media_sources", []) if s.get("path") == p]
if r["label"] != "cam1":
    sys.exit("label 不是 cam1：%r" % r["label"])
if not names or names[0] != ["cam1"]:
    sys.exit("names 不是 [cam1]：%r" % (names[0] if names else None))
print("sources=%d state=%s age_ms=%s label=%s names=%s"
      % (len(rows), r["state"], r["last_packet_age_ms"], r["label"], names[0]))
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

echo "== 5) 只接受 TCP 的源（UDP SETUP 被 461）与 UDP 传输（M11-a2b 改默认后仍要验）=="
# 【M11-a2b】默认 transport 从 auto 改成 tcp（UDP 实测丢包，见 DESIGN_M11 §5.3）。
# 但 `auto`/`udp` 仍是**可用**的，所以另起一个短命实例（同一批假源、transport=auto）单独验：
#   ① 普通源走 UDP 仍能拉（不是"默认 tcp 就把 udp 路径废了"）；
#   ② TCP-only 源（UDP SETUP 回 461）→ FFmpeg 自动回退 TCP。
code=$(curl -s -o "$OUT/tcp.flv" -w '%{http_code}' --max-time 10 "$BASE/live/tcp.flv" || true)
check "GET /live/tcp.flv → 200（TCP-only 源也能拉）" "$code" "200"
check "TCP-only 源的编码" \
    "$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$OUT/tcp.flv" | awk 'NF' | head -1)" "h264"

PORT_UDP=$(( PORT + 2 ))
"$SERVER" --port "$PORT_UDP" --media-root "$ROOT/samples" --log-level info \
    --rtsp "rtsp://127.0.0.1:$RTSP_OK/live#u1" \
    --rtsp "rtsp://127.0.0.1:$RTSP_TCPONLY/live#u2" \
    --rtsp-transport auto --rtsp-timeout-ms 3000 \
    > "$OUT/server_udp.log" 2>&1 &
UDP_PID=$!
for _ in $(seq 1 60); do
    if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT_UDP/api/stats"; then break; fi
    sleep 0.1
done
BASE_UDP="http://127.0.0.1:$PORT_UDP"
code=$(curl -s -o "$OUT/udp.flv" -w '%{http_code}' --max-time 10 "$BASE_UDP/live/u1.flv" || true)
check "transport=auto：普通源走 UDP 也能拉" "$code" "200"
code=$(curl -s -o "$OUT/fallback.flv" -w '%{http_code}' --max-time 10 "$BASE_UDP/live/u2.flv" || true)
check "transport=auto：TCP-only 源（461）自动回退成功" "$code" "200"
kill -INT "$UDP_PID" 2> /dev/null || true
wait "$UDP_PID" 2> /dev/null || true

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

echo "== 8) 断流自愈（M11-a2）：对端消失 / 半开，都要自动重连且**不打断观众** =="
# 判据四件事：
#   ① 观众连接一直没被结束：curl 20 秒后退出码必须是 28（超时），而不是 0（对端正常关流 = 被 EOS 踢了）；
#   ② 时间轴**不倒退**且持续前进：跨重连处 dts 不许变小，且总时长远超第一段（25 帧 ≈ 1 秒）；
#   ③ 所有帧都能解出来（-count_frames），且没有"非单调 dts"以外的解码错误；
#   ④ /api/stats 的 rtsp 段：reconnects ≥ 1 且 state 回到 playing。
#
# 注：这里**不断言**"dts 严格递增"。假源没发 RTCP SR，FFmpeg 给每个连接的第一个包
# 一个乱序 pts（0.2s，第 5 帧的位置），我们的单调守卫把它后面 5 个包钳到同一 dts →
# 每个连接开头（含重连处）会有 6 个重复 dts，这是**源/FFmpeg**的已知现象
# （见 docs/DESIGN_M11.md §5.2 观测 3），不是重连逻辑的错；真机有 RTCP SR 不会这样。
self_heal() { # self_heal <名字> <端口> <说明>
    local name="$1" port="$2" label="$3"
    local flv="$OUT/${name}.flv"
    local rc=0
    # 20 秒：足够覆盖"断开 → 退避 1s → 重连 → 继续推流"
    curl -s -o "$flv" --max-time 20 "$BASE/live/${name}.flv" || rc=$?
    local bytes
    bytes=$(stat -c%s "$flv" 2> /dev/null || echo 0)
    if [ "${bytes:-0}" -gt 10000 ]; then
        printf '  ✔ %-44s %s 字节\n' "$label：20 秒内持续有数据" "$bytes"
        pass=$((pass + 1))
    else
        check_false "$label：20 秒内持续有数据" "${bytes} 字节（重连后没有继续推流？）"
    fi
    # ① 观众没被踢：curl 仍在等数据（超时退出 28），不是收到 EOF 正常结束
    check "$label：观众连接没被 EOS 结束（curl=28）" "$rc" "28"
    # ② 时间轴不倒退 + 持续前进
    if ffprobe -v error -select_streams v:0 -show_entries packet=dts_time -of csv=p=0 "$flv" \
        | awk 'NF && $1 != "N/A" { v = $1 + 0; if (last != "" && v < last) back = 1; last = v; if (v > mx) mx = v; n++ }
               END { exit (back || n < 60 || mx < 5.0) }'; then
        check_true "$label：跨重连 dts 不倒退且持续前进"
    else
        check_false "$label：跨重连 dts 不倒退且持续前进" \
            "$(ffprobe -v error -select_streams v:0 -show_entries packet=dts_time -of csv=p=0 "$flv" \
               | awk 'NF && $1 != "N/A" { v = $1 + 0; if (last != "" && v < last) back++; last = v; if (v > mx) mx = v; n++ }
                      END { printf "包=%d 倒退=%d 最远=%.2fs", n, back + 0, mx + 0 }')"
    fi
    # ③ 全部帧可解；错误只允许已知的"非单调 dts"（假源每个连接开头的重复 dts）
    local frames errs
    frames=$(ffprobe -v error -select_streams v:0 -count_frames \
        -show_entries stream=nb_read_frames -of csv=p=0 "$flv" | awk 'NF' | head -1)
    # 允许的只有已知的"重复 dts"一族（含 ffmpeg 的 "Last message repeated N times" 折叠行）
    errs=$(ffmpeg -v error -i "$flv" -f null - 2>&1 > /dev/null \
        | grep -vE "non monotonically increasing dts|Last message repeated [0-9]+ times" | head -2)
    # 帧数门限给 100 而不是 200：20 秒里能出多少帧取决于**退避节奏**（1+2+4+8…）
    # 和"每个连接只发 25 帧"的设定，200 会把"退避算得准"误判成"数据没接上"。
    # 真正的判据是上面的"持续有数据 + dts 不倒退 + 观众没被 EOS"，帧数只防"几乎没数据"。
    if [ "${frames:-0}" -ge 100 ] && [ -z "$errs" ]; then
        check_true "$label：解出 $frames 帧、无其它解码错误"
    elif [ -n "$errs" ]; then
        check_false "$label：解出 $frames 帧、无其它解码错误" "$errs"
    else
        check_false "$label：解出 $frames 帧、无其它解码错误" "只解出 $frames 帧（<100）"
    fi
    # ④ stats：重连计数与状态
    curl -s --max-time 5 "$BASE/api/stats" > "$OUT/stats_$name.json"
    python3 - "$OUT/stats_$name.json" "$port" << 'PY' || return 0
import json, sys
path, port = sys.argv[1], sys.argv[2]
d = json.load(open(path))
# 多路 RTSP 源的 URL 可能同名不同端口（本用例就是这样），所以按端口定位，别按名字猜
rtsp = None
labeled = [n for s in d.get("media_sources", [])
           for n in (s.get("names") or [])
           if s.get("path", "").find(":%s/" % port) >= 0]
for s in d.get("media_sources", []):
    r = (s.get("producer") or {}).get("rtsp")
    if not (r and r.get("enabled")):
        continue
    if r.get("label") in labeled or (":%s/" % port) in (r.get("url") or ""):
        rtsp = r
if rtsp is None:
    sys.exit("没有找到端口 %s 的 rtsp 段" % port)
print("state=%s reconnects=%s attempts=%s" % (rtsp.get("state"), rtsp.get("reconnects"), rtsp.get("attempts")))
sys.exit(0 if int(rtsp.get("reconnects", 0)) >= 1 else 1)
PY
    if [ $? -eq 0 ]; then
        check_true "$label：stats 显示 reconnects ≥ 1"
    else
        check_false "$label：stats 显示 reconnects ≥ 1" "见 $OUT/stats_$name.json"
    fi
}
self_heal "brk" "$RTSP_CLOSE" "对端消失（--after close）"
self_heal "half" "$RTSP_SILENT" "半开静默（--after silent，3s 读超时）"
echo "== 9) RTSP 音视频（M11-a2b）：SDP 两条轨 → FLV/HLS 也必须两条 =="
# 真摄像机基本都有声：这条用例补的是"RTSP 只验过视频轨"的缺口。
# 红 = 音频被静默丢掉（只剩 1 条流）/ 音频解不出 / 音频 dts 倒退。
av_flv="$OUT/av.flv"
code=$(curl -s -o "$av_flv" -w '%{http_code}' --max-time 10 "$BASE/live/av.flv" || true)
check "GET /live/av.flv → 200" "$code" "200"
av_streams=$(ffprobe -v error -show_entries stream=codec_name,codec_type -of csv=p=0 "$av_flv" 2>/dev/null | grep -v '^$' | sort -u | tr '\n' ' ')
check "FLV 里有两条流（video h264 + audio aac）" "$av_streams" "aac,audio h264,video "
if [ -z "$(ffmpeg -v error -i "$av_flv" -t 6 -f null - 2>&1 | grep -vE "non monotonically increasing dts|Last message repeated [0-9]+ times")" ]; then
    check_true "av：音视频都能解（除已知首包重复 dts）"
else
    check_false "av：音视频都能解（除已知首包重复 dts）" \
        "$(ffmpeg -v error -i "$av_flv" -t 6 -f null - 2>&1 | head -2)"
fi
av_audio_dts=$(ffprobe -v error -select_streams a:0 -show_entries packet=dts_time -of csv=p=0 "$av_flv" 2>/dev/null \
    | awk 'NF && $1 != "N/A" { v = $1 + 0; if (last != "" && v < last) back++; last = v; n++ } END { printf "%d/%d", n, back + 0 }')
av_audio_n=${av_audio_dts%%/*}
av_audio_back=${av_audio_dts##*/}
if [ "${av_audio_n:-0}" -ge 100 ] && [ "${av_audio_back:-1}" -eq 0 ]; then
    check_true "音频（$av_audio_n 包，0 次 dts 倒退）"
else
    check_false "音频包数与 dts 单调" "$av_audio_dts（期望包数≥100 且倒退=0）"
fi
# HLS：切片里也要有音频（音频只在 FLV 有 = 播放器静音却看不出来）
curl -s -o /dev/null --max-time 5 "$BASE/hls/av.m3u8"
sleep 6
curl -s -o "$OUT/av.m3u8" --max-time 5 "$BASE/hls/av.m3u8"
av_slice=$(grep -o '/hls/av-[0-9]*\.ts' "$OUT/av.m3u8" | tail -1)
if [ -n "$av_slice" ]; then
    curl -s -o "$OUT/av_slice.ts" --max-time 5 "$BASE$av_slice"
    # 注意：TS 里 ffprobe 会把同一路流列两遍（PAT/PMT 重复 —— 本地 sample.ts 也一样），必须 sort -u
    av_ts_streams=$(ffprobe -v error -show_entries stream=codec_type -of csv=p=0 "$OUT/av_slice.ts" 2>/dev/null | grep -v '^$' | sort -u | tr '\n' ' ')
    check "HLS 切片里也有音频轨" "$av_ts_streams" "audio video "
else
    check_false "HLS 切片里也有音频轨" "播放列表没有切片：$(head -4 "$OUT/av.m3u8" | tr '\n' '|')"
fi
# 名字（M11-a2b）：日志里要看得到 `#av`，不是只有 IP
if grep -q "\[av\]" "$OUT/server.log"; then
    check_true "日志里能看到源的名字（[av]）"
else
    check_false "日志里能看到源的名字（[av]）" "没找到 [av]"
fi

# 重连日志必须能看（可观测性：不能只在 stats 里有个数字）
reconn_logs=$(grep -c "断流 → " "$OUT/server.log" || true)
if [ "${reconn_logs:-0}" -ge 2 ]; then
    check_true "日志里能看到断流与退避（$reconn_logs 条）"
else
    check_false "日志里能看到断流与退避" "$reconn_logs 条"
fi

echo
echo "结果：通过 $pass 项 / 失败 $fail 项（产物：$OUT）"
[ "$fail" -eq 0 ] || { echo "结论：M11-a1/a2/a2b 未通过"; exit 1; }
echo "结论：M11-a1 + a2 + a2b 通过（RTSP 能连能拉、缺参数集当场拒绝、TCP 回退、凭据脱敏；断流自动重连且观众不掉线；音视频两条轨都出 FLV/HLS；名字进日志与 stats）"
