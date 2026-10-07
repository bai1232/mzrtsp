#!/usr/bin/env bash
# ============================================================================
# M6-c 验收：完整的 HTTP-FLV 服务（build/bin/mzmedia）端到端
#
# 这一步是 M6-c 唯一算数的证据 —— 单元测试证明"字节拼得对"，而这里证明：
#   ① 浏览器要的东西（播放页 + 入库的 flv.js）真的能被取到；
#   ② **经过真实 HTTP + chunked + 连接关闭之后**，拉到的东西仍是一份合法 FLV（ffprobe/ffmpeg 独立判定）；
#   ③ **H264 裸流（Annex-B）** 也能拉：服务端现场构造 avcC + 转 AVCC（ffprobe 是独立裁判）；
#   ④ NFR-1 首帧 < 1s（用"文件长到 2KB"近似首帧到达，见 §4 的说明）；
#   ⑤ 边界：路径穿越 / 不存在的源 / 非法扩展名 / 断开后回收（NFR-6）；
#   ⑥ `--loop` 真的循环（拉 3 秒 @4x 应得到约 12 秒媒体时长，且时间戳不回退）；
#   ⑦ **源结束后再拉同一路会立刻结束**（M6-c 修的 bug：曾经会一直挂着不动）。
#
# 每个阶段用**各自的媒体文件**（复制到临时目录）：源的 EOS 与 GOP 缓存有状态，
# 共用一份会被前一步的"读完了"影响，测出来的东西就不是这一阶段想测的了。
#
# 用法：./scripts/flv_http_test.sh
# 依赖：build/bin/mzmedia、samples/（scripts/make_samples.sh）、third_party/flv.js、curl、ffprobe、ffmpeg、python3
# ============================================================================
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERVER="$ROOT/build/bin/mzmedia"
SAMPLE="$ROOT/samples/sample.mp4"
RAW_SAMPLE="$ROOT/samples/sample.h264"
FLVJS="$ROOT/third_party/flv.js/flv.min.js"

MEDIA_DIR="/tmp/mzmedia_acc_media"
OUT="/tmp/mzmedia_flv_http.flv"
OUT_RAW="/tmp/mzmedia_flv_http_raw.flv"
OUT_LOOP="/tmp/mzmedia_flv_http_loop.flv"
OUT_LATE="/tmp/mzmedia_flv_http_late.flv"
PAGE="/tmp/mzmedia_page.html"
FLVJS_OUT="/tmp/mzmedia_flvjs.js"
TTFB_OUT="/tmp/mzmedia_ttfb.flv"
TTFB_INFO="/tmp/mzmedia_ttfb.txt"
SRV_LOG="/tmp/mzmedia_flv_http_server.log"
LOOP_LOG="/tmp/mzmedia_flv_http_loop_server.log"
PORT=$(( (RANDOM % 3000) + 21000 ))
LOOP_PORT=$(( PORT + 1 ))

[ -x "$SERVER" ] || { echo "错误：找不到 $SERVER，先执行 cmake --build build" >&2; exit 1; }
[ -f "$SAMPLE" ] || { echo "错误：找不到样本，先执行 ./scripts/make_samples.sh" >&2; exit 1; }
[ -f "$RAW_SAMPLE" ] || { echo "错误：找不到 samples/sample.h264，先执行 ./scripts/make_samples.sh" >&2; exit 1; }
[ -f "$FLVJS" ] || { echo "错误：找不到 $FLVJS（播放器要入库，见 third_party/flv.js/README.md）" >&2; exit 1; }
for tool in curl ffprobe ffmpeg python3; do
    command -v "$tool" > /dev/null 2>&1 || { echo "错误：需要 $tool" >&2; exit 1; }
done

pass=0
fail=0
check() { # check <说明> <实际> <期望>
    if [ "$2" = "$3" ]; then
        printf '  ✔ %-34s %s\n' "$1" "$2"
        pass=$((pass + 1))
    else
        printf '  ✘ %-34s 实际=%s 期望=%s\n' "$1" "$2" "$3"
        fail=$((fail + 1))
    fi
}
check_true() { # check_true <说明>
    printf '  ✔ %-34s\n' "$1"
    pass=$((pass + 1))
}
check_false_msg() { # check_false_msg <说明> <细节>
    printf '  ✘ %-34s %s\n' "$1" "$2"
    fail=$((fail + 1))
}
check_range() { # check_range <说明> <实际> <下界> <上界>
    if [ "$2" -ge "$3" ] && [ "$2" -le "$4" ]; then
        printf '  ✔ %-34s %s（期望 %s..%s）\n' "$1" "$2" "$3" "$4"
        pass=$((pass + 1))
    else
        printf '  ✘ %-34s 实际=%s 期望 %s..%s\n' "$1" "$2" "$3" "$4"
        fail=$((fail + 1))
    fi
}

# ---- 给每个阶段各准备一份媒体文件（避免共用源的 EOS/GOP 状态互相影响）----
rm -rf "$MEDIA_DIR"
mkdir -p "$MEDIA_DIR"
cp "$SAMPLE" "$MEDIA_DIR/main.mp4"     # §2 整段拉完（会 EOS）
cp "$SAMPLE" "$MEDIA_DIR/movie.mp4"    # §5/§6/§7 用（保持"还没读完"的状态）
cp "$SAMPLE" "$MEDIA_DIR/cold.mp4"     # §4 冷启动首帧延迟
cp "$SAMPLE" "$MEDIA_DIR/notes.sh"     # §6 用来验证"扩展名白名单"
cp "$RAW_SAMPLE" "$MEDIA_DIR/clip.h264"

SRV_PID=""
LOOP_PID=""
cleanup() {
    if [ -n "$SRV_PID" ]; then
        kill -INT "$SRV_PID" 2> /dev/null || true
    fi
    if [ -n "$LOOP_PID" ]; then
        kill -INT "$LOOP_PID" 2> /dev/null || true
    fi
    wait 2> /dev/null || true
}
trap cleanup EXIT

echo "== 0) 起服务（端口 $PORT；--web-root 显式指到入库的 flv.js）=="
"$SERVER" --port "$PORT" --media-root "$MEDIA_DIR" --web-root "$ROOT/third_party/flv.js" \
    > "$SRV_LOG" 2>&1 &
SRV_PID=$!
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
# FR-7.2：启动横幅必须直接给出播放入口
if grep -q "http://127.0.0.1:$PORT/" "$SRV_LOG"; then
    check_true "启动横幅打印播放 URL（FR-7.2）"
else
    check_false_msg "启动横幅打印播放 URL（FR-7.2）" "日志里没有 URL"
fi

echo "== 1) 网页侧：播放页 + 入库的 flv.js（浏览器要的东西真的能取到）=="
code="$(curl -s -o "$PAGE" -w '%{http_code}' --max-time 5 "http://127.0.0.1:$PORT/")"
check "GET / → 200" "$code" "200"
for marker in '<video' 'flv.min.js' 'flvjs.createPlayer' '/live/'; do
    if grep -qF "$marker" "$PAGE"; then
        check_true "页面含播放器要素：$marker"
    else
        check_false_msg "页面含播放器要素：$marker" "没找到"
    fi
done
code="$(curl -s -o "$FLVJS_OUT" -w '%{http_code}' --max-time 5 "http://127.0.0.1:$PORT/flv.min.js")"
check "GET /flv.min.js → 200" "$code" "200"
check "播放器字节数" "$(wc -c < "$FLVJS_OUT")" "$(wc -c < "$FLVJS")"
if cmp -s "$FLVJS_OUT" "$FLVJS"; then
    check_true "提供的 flv.js 与入库文件逐字节一致"
else
    check_false_msg "提供的 flv.js 与入库文件逐字节一致" "内容不同"
fi
check "flv.js 版本（1.6.2）" "$(grep -c '"1.6.2"' "$FLVJS_OUT")" "1"

echo "== 2) MP4 源：curl -N 拉流 → ffprobe / ffmpeg 独立校验 =="
curl -sS -N --max-time 30 "http://127.0.0.1:$PORT/live/main.flv" -o "$OUT"
check "前 3 字节是 FLV" "$(head -c 3 "$OUT" | tr -d '\0')" "FLV"
check "视频编码" "$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$OUT")" "h264"
check "宽" "$(ffprobe -v error -select_streams v:0 -show_entries stream=width -of csv=p=0 "$OUT")" "320"
check "高" "$(ffprobe -v error -select_streams v:0 -show_entries stream=height -of csv=p=0 "$OUT")" "240"
check "音频编码" "$(ffprobe -v error -select_streams a:0 -show_entries stream=codec_name -of csv=p=0 "$OUT")" "aac"
frames="$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$OUT")"
check_range "解码出的视频帧数（源 50）" "$frames" 48 52
err="$(ffmpeg -v error -i "$OUT" -f null - 2>&1 > /dev/null || true)"
if [ -z "$err" ]; then
    check_true "ffmpeg 完整解码无 error"
else
    check_false_msg "ffmpeg 完整解码无 error" "$err"
fi

echo "== 3) H264 裸流源（FR-2.1）：Annex-B → avcC + AVCC 转换 =="
# 请求 clip.h264.flv：服务端现场构造 avcC（AVCDecoderConfigurationRecord）并把每个包的
# 起始码换成 4 字节长度前缀。**ffprobe/ffmpeg 是独立裁判** —— 转错了这里必红。
code="$(curl -sS -N --max-time 30 -o "$OUT_RAW" -w '%{http_code}' "http://127.0.0.1:$PORT/live/clip.h264.flv")"
check "GET /live/clip.h264.flv → 200" "$code" "200"
check "前 3 字节是 FLV" "$(head -c 3 "$OUT_RAW" | tr -d '\0')" "FLV"
check "视频编码" "$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$OUT_RAW")" "h264"
check "宽" "$(ffprobe -v error -select_streams v:0 -show_entries stream=width -of csv=p=0 "$OUT_RAW")" "320"
check "高" "$(ffprobe -v error -select_streams v:0 -show_entries stream=height -of csv=p=0 "$OUT_RAW")" "240"
raw_frames="$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$OUT_RAW")"
check_range "裸流解出的视频帧数" "${raw_frames:-0}" 1 60
err="$(ffmpeg -v error -i "$OUT_RAW" -f null - 2>&1 > /dev/null || true)"
if [ -z "$err" ]; then
    check_true "裸流转出的 FLV 解码无 error"
else
    check_false_msg "裸流转出的 FLV 解码无 error" "$err"
fi
# 每个视频 tag 的负载都必须是合法 AVCC：4 字节长度前缀 + NAL，逐个走完不能有剩余
# （这条是"转换对不对"的**结构**证据，与 ffprobe 的"能解"互为独立判据）
if python3 - "$OUT_RAW" > /tmp/mzmedia_avcc_check.txt 2>&1 << 'PY'
import sys
data = open(sys.argv[1], 'rb').read()
pos, tags, nals = 13, 0, 0
while pos + 11 <= len(data):
    ttype = data[pos]
    size = int.from_bytes(data[pos + 1:pos + 4], 'big')
    body = data[pos + 11: pos + 11 + size]
    if len(body) != size:
        sys.exit(1)
    if ttype == 9 and len(body) >= 5 and body[1] == 1:   # AVCPacketType = 1（NALU）
        off = 5
        while off + 4 <= len(body):
            nlen = int.from_bytes(body[off:off + 4], 'big')
            if nlen == 0 or off + 4 + nlen > len(body):
                sys.exit(2)
            ntype = body[off + 4] & 0x1F
            if ntype < 1 or ntype > 12:
                sys.exit(3)
            off += 4 + nlen
            nals += 1
        if off != len(body):
            sys.exit(4)
        tags += 1
    pos += 11 + size + 4
if tags == 0 or nals == 0:
    sys.exit(5)
print("    [ INFO ] 独立校验：%d 个视频 tag / %d 个 AVCC NAL" % (tags, nals))
PY
then
    check_true "每个视频 tag 都是合法 AVCC（python 独立解析）"
    cat /tmp/mzmedia_avcc_check.txt
else
    check_false_msg "每个视频 tag 都是合法 AVCC（python 独立解析）" "$(cat /tmp/mzmedia_avcc_check.txt)"
fi

echo "== 4) NFR-1：冷启动首帧延迟 < 1s =="
# 度量：后台 curl 存盘，前台每 20ms 看一次文件大小，长到 2KB 即认为"首帧已到"。
# 说明：这是**近似**（20ms 粒度；2KB ≈ FLV 头 + sequence header + 第一帧的一部分），
#       精确到毫秒需要播放器埋点，v0.1 不做 —— 但它足以抓住"要等一秒才出画面"这类真问题。
# 用一份**没人读过的**媒体文件（cold.mp4）：否则源可能已经 EOS，测到的不是冷启动。
rm -f "$TTFB_OUT" "$TTFB_INFO"
: > "$TTFB_OUT"
curl -sS -N --max-time 20 "http://127.0.0.1:$PORT/live/cold.flv" -o "$TTFB_OUT" \
    -w 'ttfb=%{time_starttransfer}s\n' > "$TTFB_INFO" 2>&1 &
CURL_PID=$!
first_ms=""
for i in $(seq 1 150); do
    size_now="$(wc -c < "$TTFB_OUT" 2> /dev/null || echo 0)"
    if [ "${size_now:-0}" -ge 2048 ]; then
        first_ms=$(( i * 20 ))
        break
    fi
    sleep 0.02
done
wait "$CURL_PID" 2> /dev/null || true
if [ -n "$first_ms" ] && [ "$first_ms" -lt 1000 ]; then
    printf '  ✔ %-34s %s ms（%s）\n' "首帧到达（≥2KB）" "$first_ms" "$(tr '\n' ' ' < "$TTFB_INFO")"
    pass=$((pass + 1))
else
    check_false_msg "首帧到达（≥2KB）" "实际=${first_ms:-超时(>3s)} ms"
fi

echo "== 5) 响应头（Content-Type / 断开语义 / CORS）=="
head_dump="$(curl -sS -D - -o /dev/null --max-time 2 "http://127.0.0.1:$PORT/live/movie.flv" | tr -d '\r' || true)"
for want in 'content-type: video/x-flv' 'transfer-encoding: chunked' 'access-control-allow-origin: *'; do
    if printf '%s' "$head_dump" | grep -qi "^$want"; then
        check_true "响应头：$want"
    else
        check_false_msg "响应头：$want" "实际头部：$(printf '%s' "$head_dump" | tr '\n' '|')"
    fi
done

echo "== 6) 边界：不存在的源 / 路径穿越 / 非法名字与扩展名 =="
code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:$PORT/live/nope.flv")"
check "不存在的源 → 404" "$code" "404"
body="$(curl -s --max-time 5 "http://127.0.0.1:$PORT/live/nope.flv" || true)"
if printf '%s' "$body" | grep -q '没有这个媒体文件'; then
    check_true "不存在的源：提示是「没有这个媒体文件」"
else
    check_false_msg "不存在的源：提示是「没有这个媒体文件」" "$body"
fi
code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:$PORT/live/..%2F..%2Fetc%2Fpasswd.flv")"
check "路径穿越被拒 → 404" "$code" "404"
code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:$PORT/live/bad%20name.flv")"
check "非法名字被拒 → 404" "$code" "404"
code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:$PORT/live/notes.sh.flv")"
check "非白名单扩展名被拒 → 404" "$code" "404"
body="$(curl -s --max-time 5 "http://127.0.0.1:$PORT/live/notes.sh.flv" || true)"
if printf '%s' "$body" | grep -q '路径应为'; then
    check_true "非白名单扩展名：提示是「路径应为 …」（不是「文件不存在」）"
else
    check_false_msg "非白名单扩展名：提示是「路径应为 …」" "$body"
fi
code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:$PORT/live/movie")"
check "缺少 .flv 后缀 → 404" "$code" "404"

echo "== 7) 客户端断开后源被回收（NFR-6）=="
curl -sS -N --max-time 1 "http://127.0.0.1:$PORT/live/movie.flv" -o /dev/null || true
sleep 1
subs="$(curl -s --max-time 3 "http://127.0.0.1:$PORT/api/stats" | tr -d ' ' | grep -o '"subscribers":[0-9]*' | head -1 | cut -d: -f2 || true)"
check "断开后订阅者已回收（subscribers）" "${subs:-x}" "0"
# FR-6.1：媒体层统计确实拼进了 /api/stats（含 M6-c 的 Annex-B 与循环字段）
stats="$(curl -s --max-time 3 "http://127.0.0.1:$PORT/api/stats" || true)"
for key in '"source_manager"' '"media_sources"' '"producer"' '"annex_b_packets"' '"idle_deferred"'; do
    if printf '%s' "$stats" | grep -q -- "$key"; then
        check_true "/api/stats 含 $key"
    else
        check_false_msg "/api/stats 含 $key" "缺少"
    fi
done

echo "== 8) 源结束后再拉同一路：必须立刻结束（不是挂着不动）=="
# main.mp4 在 §2 已经读完（源 EOS）。再拉一次：订阅时队列已被 markEndOfStream，
# 但那一刻 drain 回调还没注册 → 修前**不会有任何唤醒**，连接会一直挂着（客户端永远黑屏）。
# 修法：FlvSender::start() 立刻搬一次队列。这里用"curl 是否按时正常结束"来判定。
start_ts="$(date +%s%3N)"
set +e
curl -sS -N --max-time 8 "http://127.0.0.1:$PORT/live/main.flv" -o "$OUT_LATE"
late_rc=$?
set -e
late_ms=$(( $(date +%s%3N) - start_ts ))
if [ "$late_rc" -eq 0 ]; then
    check_true "源已结束后再拉：连接正常结束（curl rc=0，${late_ms}ms）"
else
    check_false_msg "源已结束后再拉：连接正常结束" "curl rc=$late_rc（28=超时挂着不动），${late_ms}ms"
fi
check_range "晚到连接拿到的字节数（头 + GOP 缓存）" "$(wc -c < "$OUT_LATE")" 13 2000000

echo "== 9) --loop：循环推流（另起实例，4 倍速）=="
"$SERVER" --port "$LOOP_PORT" --media-root "$MEDIA_DIR" --loop --speed 4 > "$LOOP_LOG" 2>&1 &
LOOP_PID=$!
ready=no
for _ in $(seq 1 80); do
    if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$LOOP_PORT/api/stats"; then
        ready=yes
        break
    fi
    sleep 0.1
done
if [ "$ready" != yes ]; then
    check_false_msg "循环实例就绪" "$(cat "$LOOP_LOG")"
else
    # 4 倍速拉 3 秒 → 应得到约 12 秒媒体时长（2 秒样本循环 6 次）
    curl -sS -N --max-time 3 "http://127.0.0.1:$LOOP_PORT/live/main.flv" -o "$OUT_LOOP" || true
    duration="$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$OUT_LOOP" | cut -d. -f1)"
    check_range "3 秒 @4x 得到的媒体时长（秒）" "${duration:-0}" 9 15
    loop_frames="$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$OUT_LOOP")"
    if [ "${loop_frames:-0}" -ge 250 ]; then
        printf '  ✔ %-34s %s 帧（单趟 50 帧）\n' "循环后解出的视频帧数" "$loop_frames"
        pass=$((pass + 1))
    else
        check_false_msg "循环后解出的视频帧数" "实际=${loop_frames:-0} 期望≥250"
    fi
    loop_stats="$(curl -s --max-time 3 "http://127.0.0.1:$LOOP_PORT/api/stats" | tr ',' '\n')"
    loops="$(printf '%s' "$loop_stats" | grep -o '"loops":[0-9]*' | head -1 | cut -d: -f2)"
    offset="$(printf '%s' "$loop_stats" | grep -o '"loop_offset_ms":[0-9]*' | head -1 | cut -d: -f2)"
    if [ "${loops:-0}" -ge 3 ]; then
        printf '  ✔ %-34s loops=%s offset=%sms\n' "统计里的重开次数" "$loops" "${offset:-?}"
        pass=$((pass + 1))
    else
        check_false_msg "统计里的重开次数" "loops=${loops:-0} 期望≥3"
    fi
    # 时间戳必须**单调递增**：跨循环也不能回退（回退会让播放端花屏/卡死）
    if ffprobe -v error -show_entries packet=pts_time -of csv=p=0 -select_streams v:0 "$OUT_LOOP" \
        | awk 'NF && $1 != "N/A" { if (last != "" && $1 + 0 < last) { bad = 1 } last = $1 + 0 } END { exit bad }'; then
        check_true "跨循环后视频时间戳仍单调递增"
    else
        check_false_msg "跨循环后视频时间戳仍单调递增" "出现回退"
    fi
    if [ -z "$(ffmpeg -v error -i "$OUT_LOOP" -f null - 2>&1 > /dev/null || true)" ]; then
        check_true "循环流完整解码无 error"
    else
        check_false_msg "循环流完整解码无 error" "$(ffmpeg -v error -i "$OUT_LOOP" -f null - 2>&1 > /dev/null || true)"
    fi
fi

echo "------------------------------------------------------------------------"
echo "结果：通过 $pass 项 / 失败 $fail 项"
echo "  产物：$OUT / $OUT_RAW / $OUT_LOOP / $OUT_LATE"
echo "  日志：$SRV_LOG / $LOOP_LOG"
if [ "$fail" -gt 0 ]; then
    echo "结论：M6-c 未通过"
    exit 1
fi
echo "结论：M6-c 通过（播放页 + flv.js 可取；MP4 与 H264 裸流都能拉出合法 FLV；循环推流生效；源结束后不挂连接）"
