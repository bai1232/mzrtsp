#!/usr/bin/env bash
# ============================================================================
# M6-b 验收：起 HTTP-FLV 服务 → curl 拉流 → **ffprobe 校验** → 边界（404/穿越/头）
#
# 这一步是 M6-b 唯一算数的证据：单元测试证明"字节拼得对"，而这里证明
# "**经过真实 HTTP + chunked + 连接关闭之后**，拉到的东西仍是一份合法 FLV"。
#
# 用法：./scripts/flv_http_test.sh
# 依赖：build/bin/flv_http_server、samples/sample.mp4、curl、ffprobe、ffmpeg
# ============================================================================
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERVER="$ROOT/build/bin/flv_http_server"
SAMPLE="$ROOT/samples/sample.mp4"
OUT="/tmp/mzmedia_flv_http.flv"
SRV_LOG="/tmp/mzmedia_flv_http_server.log"
PORT=$(( (RANDOM % 3000) + 21000 ))

[ -x "$SERVER" ] || { echo "错误：找不到 $SERVER，先执行 cmake --build build" >&2; exit 1; }
[ -f "$SAMPLE" ] || { echo "错误：找不到样本，先执行 ./scripts/make_samples.sh" >&2; exit 1; }
for tool in curl ffprobe ffmpeg; do
    command -v "$tool" > /dev/null 2>&1 || { echo "错误：需要 $tool" >&2; exit 1; }
done

pass=0
fail=0
check() { # check <说明> <实际> <期望>
    if [ "$2" = "$3" ]; then
        printf '  ✔ %-32s %s\n' "$1" "$2"
        pass=$((pass + 1))
    else
        printf '  ✘ %-32s 实际=%s 期望=%s\n' "$1" "$2" "$3"
        fail=$((fail + 1))
    fi
}

cleanup() {
    kill -INT "$SRV" 2> /dev/null || true
    wait "$SRV" 2> /dev/null || true
}
trap cleanup EXIT

echo "== 0) 起服务（端口 $PORT）=="
"$SERVER" --port "$PORT" --media-root "$ROOT/samples" > "$SRV_LOG" 2>&1 &
SRV=$!
ready=no
for _ in $(seq 1 80); do
    if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/api/stats"; then
        ready=yes
        break
    fi
    sleep 0.1
done
if [ "$ready" != yes ]; then
    echo "错误：服务未就绪，日志如下：" >&2
    cat "$SRV_LOG" >&2
    exit 1
fi
echo "  服务已就绪"

echo "== 1) curl -N 拉流存盘（源 EOS 后服务端应主动关连接）=="
curl -sS -N --max-time 30 "http://127.0.0.1:$PORT/live/sample.flv" -o "$OUT"
if [ -s "$OUT" ]; then
    printf '  ✔ %-32s %s 字节\n' "拉到的文件非空" "$(wc -c < "$OUT")"
    pass=$((pass + 1))
else
    printf '  ✘ %-32s 文件为空\n' "拉到的文件非空"
    fail=$((fail + 1))
fi
magic="$(head -c 3 "$OUT" | tr -d '\0')"
check "前 3 字节是 FLV" "$magic" "FLV"

echo "== 2) ffprobe / ffmpeg 校验（与源一致）=="
vcodec="$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$OUT")"
width="$(ffprobe -v error -select_streams v:0 -show_entries stream=width -of csv=p=0 "$OUT")"
height="$(ffprobe -v error -select_streams v:0 -show_entries stream=height -of csv=p=0 "$OUT")"
acodec="$(ffprobe -v error -select_streams a:0 -show_entries stream=codec_name -of csv=p=0 "$OUT")"
check "视频编码" "$vcodec" "h264"
check "宽" "$width" "320"
check "高" "$height" "240"
check "音频编码" "$acodec" "aac"

frames="$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$OUT")"
if [ "$frames" -ge 48 ] && [ "$frames" -le 52 ]; then
    printf '  ✔ %-32s %s 帧（源 50）\n' "解码出的视频帧数" "$frames"
    pass=$((pass + 1))
else
    printf '  ✘ %-32s 实际=%s 期望≈50\n' "解码出的视频帧数" "$frames"
    fail=$((fail + 1))
fi
err="$(ffmpeg -v error -i "$OUT" -f null - 2>&1 > /dev/null || true)"
if [ -z "$err" ]; then
    printf '  ✔ %-32s\n' "ffmpeg 完整解码无 error"
    pass=$((pass + 1))
else
    printf '  ✘ ffmpeg 解码有错误：\n%s\n' "$err"
    fail=$((fail + 1))
fi

echo "== 3) 响应头（Content-Type / 断开语义 / CORS）=="
head_dump="$(curl -sS -D - -o /dev/null --max-time 5 "http://127.0.0.1:$PORT/live/sample.flv" | tr -d '\r' || true)"
if printf '%s' "$head_dump" | grep -qi '^content-type: video/x-flv'; then
    printf '  ✔ %-32s\n' "Content-Type: video/x-flv"
    pass=$((pass + 1))
else
    printf '  ✘ %-32s 实际头部：\n%s\n' "Content-Type" "$head_dump"
    fail=$((fail + 1))
fi
if printf '%s' "$head_dump" | grep -qi '^transfer-encoding: chunked'; then
    printf '  ✔ %-32s\n' "Transfer-Encoding: chunked"
    pass=$((pass + 1))
else
    printf '  ✘ %-32s\n' "Transfer-Encoding: chunked"
    fail=$((fail + 1))
fi
if printf '%s' "$head_dump" | grep -qi '^access-control-allow-origin: \*'; then
    printf '  ✔ %-32s\n' "CORS 头（FR-4.3，flv.js 需要）"
    pass=$((pass + 1))
else
    printf '  ✘ %-32s\n' "CORS 头"
    fail=$((fail + 1))
fi

echo "== 4) 边界：不存在的源 / 路径穿越 =="
code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:$PORT/live/nope.flv")"
check "不存在的源 → 404" "$code" "404"
code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:$PORT/live/..%2F..%2Fetc%2Fpasswd.flv")"
check "路径穿越被拒 → 404" "$code" "404"
code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:$PORT/live/bad%20name.flv")"
check "非法名字被拒 → 404" "$code" "404"

echo "== 5) 客户端断开后源被回收（NFR-6）=="
# 拉一小段就断开；随后 /api/stats 里的订阅者数应回到 0
curl -sS -N --max-time 1 "http://127.0.0.1:$PORT/live/sample.flv" -o /dev/null || true
sleep 1
subs="$(curl -s --max-time 3 "http://127.0.0.1:$PORT/api/stats" | tr -d ' ' | grep -o '"subscribers":[0-9]*' | head -1 | cut -d: -f2 || true)"
if [ "${subs:-x}" = "0" ]; then
    printf '  ✔ %-32s subscribers=0\n' "断开后订阅者已回收"
    pass=$((pass + 1))
else
    printf '  ✘ %-32s subscribers=%s 期望=0\n' "断开后订阅者已回收" "${subs:-未知}"
    fail=$((fail + 1))
fi

echo "------------------------------------------------------------------------"
echo "结果：通过 $pass 项 / 失败 $fail 项（产物：$OUT，服务日志：$SRV_LOG）"
if [ "$fail" -gt 0 ]; then
    echo "结论：M6-b 未通过"
    exit 1
fi
echo "结论：M6-b 通过（HTTP + chunked + 连接关闭之后，拉到的仍是一份合法 FLV）"
