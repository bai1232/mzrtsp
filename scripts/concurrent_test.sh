#!/usr/bin/env bash
# ============================================================================
# M6-d 验收：并发 + 长跑（NFR-2 / NFR-3 / NFR-4 / NFR-6、SC-3 / SC-4）
#
# 它回答两类问题，**判据分开**（这是刻意的，见 docs/DESIGN_M6.md §5.5）：
#   ① 功能性（硬门禁）：每路都拿到**完整且可解**的码流；中途 kill 一半不影响其余；
#      断开后订阅者与 fd **100% 回收**；超额连接被 `max_subscribers` 挡住；服务端错误日志 0 条。
#   ② 性能（只记基线，不设死阈值）：RSS 斜率、服务端 CPU、丢帧计数、一次唤醒搬多少包、
#      每秒包数。5 分钟的斜率折算成"每小时"噪声很大 —— 真门禁留 M7 的 1 小时跑。
#
# 用法：
#   ./scripts/concurrent_test.sh                 # 10 路 × 300 秒（默认，5 分钟）
#   ./scripts/concurrent_test.sh --quick         # 10 路 × 20 秒（冒烟/回归门禁）
#   ./scripts/concurrent_test.sh --kill-half     # 额外做"中途 kill 一半"专项（TESTING §6）
#   ./scripts/concurrent_test.sh --overload 8    # 额外压 max_subscribers（第 17 路起应被拒）
#   ./scripts/concurrent_test.sh --duration 3600 # 1 小时跑（NFR-3 原文；建议 M7 发版前执行）
#
# 依赖：build/bin/mzmedia、samples/（make_samples.sh）、curl、ffmpeg、ffprobe、python3
# 产物：$OUT_DIR/{server.log, clients/cN.*, samples.csv, summary.txt}
# ============================================================================
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# 允许换二进制（例如用 ASAN 版本跑长跑：`MZMEDIA_SERVER=./build-asan/bin/mzmedia ...`）
SERVER="${MZMEDIA_SERVER:-$ROOT/build/bin/mzmedia}"
MEDIA_ROOT="$ROOT/samples"
OUT_DIR="/tmp/mzmedia_concurrent"
PORT=$(( (RANDOM % 2000) + 23000 ))

CLIENTS=10
DURATION=300
SPEED=1
LOOP=yes
KILL_HALF=no
OVERLOAD=0
SAMPLE_INTERVAL=5

usage() {
    sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        --clients) CLIENTS="${2:?}"; shift 2 ;;
        --duration) DURATION="${2:?}"; shift 2 ;;
        --speed) SPEED="${2:?}"; shift 2 ;;
        --quick) CLIENTS=10; DURATION=20; shift ;;
        --no-loop) LOOP=no; shift ;;
        --kill-half) KILL_HALF=yes; shift ;;
        --overload) OVERLOAD="${2:?}"; shift 2 ;;
        --port) PORT="${2:?}"; shift 2 ;;
        --media-root) MEDIA_ROOT="${2:?}"; shift 2 ;;
        --help|-h) usage ;;
        *) echo "未知参数：$1（--help 看用法）" >&2; exit 2 ;;
    esac
done

[ -x "$SERVER" ] || { echo "错误：找不到 $SERVER，先 cmake --build build" >&2; exit 1; }
[ -d "$MEDIA_ROOT" ] || { echo "错误：媒体目录不存在 $MEDIA_ROOT（先 ./scripts/make_samples.sh）" >&2; exit 1; }
for tool in curl ffmpeg ffprobe python3; do
    command -v "$tool" > /dev/null 2>&1 || { echo "错误：需要 $tool" >&2; exit 1; }
done

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/clients"
SRV_LOG="$OUT_DIR/server.log"
CSV="$OUT_DIR/samples.csv"
SUMMARY="$OUT_DIR/summary.txt"

pass=0
fail=0
check() { # check <说明> <实际> <期望>
    if [ "$2" = "$3" ]; then
        printf '  ✔ %-40s %s\n' "$1" "$2"
        pass=$((pass + 1))
    else
        printf '  ✘ %-40s 实际=%s 期望=%s\n' "$1" "$2" "$3"
        fail=$((fail + 1))
    fi
}
check_true() {
    printf '  ✔ %-40s\n' "$1"
    pass=$((pass + 1))
}
check_false_msg() {
    printf '  ✘ %-40s %s\n' "$1" "$2"
    fail=$((fail + 1))
}

# ---- 从 /api/stats 里按**路径**取一个数（缺失返回 0）----
#   为什么不用 grep 取键名：`"rejected"` 这个名字在 JSON 里出现**三次**
#   （http 层的连接被拒 / source_manager 的 acquire 被拒 / media 的订阅被拒），
#   grep 只会抓到第一个 → 曾经把 http 的 rejected 当成 media 的（M6-d 实测踩到，见 DESIGN_M6 §5.5）。
stat_get() { # stat_get <点分路径> [json]  例：media_sources.0.media_source.counters.rejected
    local json="${2:-}"
    if [ -z "$json" ]; then
        json="$(curl -s --max-time 3 "http://127.0.0.1:$PORT/api/stats" || true)"
    fi
    printf '%s' "$json" | python3 -c '
import json, sys
path = sys.argv[1].split(".")
try:
    d = json.load(sys.stdin)
except Exception:
    print(0); sys.exit(0)
for key in path:
    if d is None:
        break
    if isinstance(d, list):
        i = int(key)
        d = d[i] if 0 <= i < len(d) else None
    elif isinstance(d, dict):
        d = d.get(key)
    else:
        d = None
print(d if isinstance(d, int) else 0)
' "$1" 2> /dev/null || echo 0
}
# ---- 数组/对象里的同一个键可能出现多次（每源一份）：这里只取第一个 ----
proc_rss_kb() { awk '/VmRSS/{print $2}' "/proc/$1/status" 2> /dev/null || echo 0; }
proc_fd() { ls "/proc/$1/fd" 2> /dev/null | wc -l; }
proc_cpu_ticks() { # utime + stime（单位 CLK_TCK，本机 100）
    awk '{print $14 + $15}' "/proc/$1/stat" 2> /dev/null || echo 0
}

# ---- 把 FLV 截到"最后一个完整 tag"（客户端被 --max-time/kill 掐断是**预期**的：
#      尾部可能留下半个 tag。ffprobe 会因此把它当畸形文件 → 先截干净再验，并报出丢了多少字节）----
trim_flv() { # trim_flv <in> ; 结果写到 <in>.trim（若无残缺则直接复制）
    python3 - "$1" << 'PY'
import sys, shutil
path = sys.argv[1]
data = open(path, 'rb').read()
if len(data) < 13:
    shutil.copyfile(path, path + '.trim')
    print(0)
    sys.exit(0)
pos = 13
while pos + 11 <= len(data):
    size = int.from_bytes(data[pos + 1:pos + 4], 'big')
    end = pos + 11 + size + 4
    if end > len(data):
        break
    pos = end
open(path + '.trim', 'wb').write(data[:pos])
print(len(data) - pos)
PY
}

SRV_PID=""
CLIENT_PIDS=()
cleanup() {
    for pid in "${CLIENT_PIDS[@]:-}"; do
        kill -TERM "$pid" 2> /dev/null || true
    done
    if [ -n "$SRV_PID" ]; then
        kill -INT "$SRV_PID" 2> /dev/null || true
    fi
    wait 2> /dev/null || true
}
trap cleanup EXIT

echo "== M6-d 并发/长跑：$CLIENTS 路 × ${DURATION}s（speed=${SPEED}，loop=${LOOP}，"
echo "   kill-half=${KILL_HALF}，overload=${OVERLOAD}，端口 $PORT）=="

# ---------------------------------------------------------------------------
# 0) 起服务 + 基线
# ---------------------------------------------------------------------------
SERVER_ARGS=(--port "$PORT" --media-root "$MEDIA_ROOT" --speed "$SPEED")
if [ "$LOOP" = yes ]; then
    SERVER_ARGS+=(--loop)
fi
"$SERVER" "${SERVER_ARGS[@]}" > "$SRV_LOG" 2>&1 &
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
    echo "错误：服务未就绪："; cat "$SRV_LOG"; exit 1
fi
FD_BASE="$(proc_fd "$SRV_PID")"
RSS_BASE="$(proc_rss_kb "$SRV_PID")"
CPU_BASE="$(proc_cpu_ticks "$SRV_PID")"
echo "  服务已就绪（PID $SRV_PID）；基线：fd=$FD_BASE rss=${RSS_BASE}kB"
echo "  注：第 16 路起是否被拒由 /api/stats 的 max_subscribers 决定（本脚本不硬编码 16）"

# ---------------------------------------------------------------------------
# 1) 起客户端：**错开接入**（0/200/400…ms）—— 能顺带暴露订阅时序/GOP 缓存问题
#    奇数路 curl（纯拉流）、偶数路 ffmpeg（边拉边真解码）
# ---------------------------------------------------------------------------
URL="http://127.0.0.1:$PORT/live/sample.flv"
echo "== 1) 起 $CLIENTS 路客户端（错开 200ms；奇数 curl / 偶数 ffmpeg）=="
for i in $(seq 1 "$CLIENTS"); do
    if [ $((i % 2)) -eq 1 ]; then
        curl -sS -N --max-time "$DURATION" -o "$OUT_DIR/clients/c$i.flv" \
            -w 'http=%{http_code} size=%{size_download}' "$URL" \
            > "$OUT_DIR/clients/c$i.log" 2>&1 &
    else
        ffmpeg -v error -y -i "$URL" -t "$DURATION" -c copy -f flv "$OUT_DIR/clients/c$i.flv" \
            > "$OUT_DIR/clients/c$i.log" 2>&1 &
    fi
    CLIENT_PIDS+=("$!")
    sleep 0.2
done
echo "  已起 $((${#CLIENT_PIDS[@]})) 路"

# ---------------------------------------------------------------------------
# 2) 采样循环（RSS / fd / CPU / stats），中途可选 kill 一半 + 超额连接
# ---------------------------------------------------------------------------
echo "== 2) 采样中（每 ${SAMPLE_INTERVAL}s 一行，写入 $CSV）=="
echo "t,rss_kb,fd,cpu_ticks,subscribers,delivered,dropped,dropped_incoming,rejected,notify,coalesced,popped,loops,packets" > "$CSV"
T0="$(date +%s%3N)"
killed=no
overloaded=no
OVERLOAD_RESULT=""
FINAL_STATS=""
while :; do
    now="$(date +%s%3N)"
    elapsed=$(( (now - T0) / 1000 ))
    if [ "$elapsed" -ge "$DURATION" ]; then
        break
    fi
    FINAL_STATS="$(curl -s --max-time 3 "http://127.0.0.1:$PORT/api/stats" || true)"
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$elapsed" "$(proc_rss_kb "$SRV_PID")" "$(proc_fd "$SRV_PID")" "$(proc_cpu_ticks "$SRV_PID")" \
        "$(stat_get media_sources.0.media_source.subscribers "$FINAL_STATS")" "$(stat_get media_sources.0.media_source.counters.delivered "$FINAL_STATS")" \
        "$(stat_get media_sources.0.media_source.counters.dropped "$FINAL_STATS")" "$(stat_get media_sources.0.media_source.counters.dropped_incoming "$FINAL_STATS")" \
        "$(stat_get media_sources.0.media_source.counters.rejected "$FINAL_STATS")" "$(stat_get media_sources.0.media_source.wakeup.notify "$FINAL_STATS")" \
        "$(stat_get media_sources.0.media_source.wakeup.coalesced "$FINAL_STATS")" "$(stat_get media_sources.0.media_source.wakeup.popped "$FINAL_STATS")" \
        "$(stat_get media_sources.0.producer.loops "$FINAL_STATS")" "$(stat_get media_sources.0.producer.packets "$FINAL_STATS")" >> "$CSV"
    tail -1 "$CSV" | sed 's/^/    /'

    # 中途 kill 一半（TESTING §6 的专项）：只 kill 前半数
    if [ "$KILL_HALF" = yes ] && [ "$killed" = no ] && [ "$elapsed" -ge $((DURATION / 2)) ]; then
        killed=yes
        half=$(( (${#CLIENT_PIDS[@]} + 1) / 2 ))
        for idx in $(seq 0 $((half - 1))); do
            kill -TERM "${CLIENT_PIDS[$idx]}" 2> /dev/null || true
        done
        echo "    [中途] 已 kill 前 $half 路客户端（t=${elapsed}s）"
    fi

    # 超额连接（验证 max_subscribers）：并行开 OVERLOAD 路，看能进几个、拒几个
    if [ "$OVERLOAD" -gt 0 ] && [ "$overloaded" = no ] && [ "$elapsed" -ge 3 ]; then
        overloaded=yes
        subs_before="$(stat_get media_sources.0.media_source.subscribers "$FINAL_STATS")"
        rej_before="$(stat_get media_sources.0.media_source.counters.rejected "$FINAL_STATS")"
        OVERLOAD_PIDS=()
        for k in $(seq 1 "$OVERLOAD"); do
            curl -s -o /dev/null --max-time 8 "$URL" &
            OVERLOAD_PIDS+=("$!")
        done
        sleep 2
        s="$(curl -s --max-time 3 "http://127.0.0.1:$PORT/api/stats" || true)"
        subs_peak="$(stat_get media_sources.0.media_source.subscribers "$s")"
        rej_after="$(stat_get media_sources.0.media_source.counters.rejected "$s")"
        max_subs="$(stat_get media_sources.0.media_source.max_subscribers "$s")"
        # 等它们退出，否则后面的"断开后订阅者归零"会被它们吊住（假红）
        for pid in "${OVERLOAD_PIDS[@]}"; do
            wait "$pid" 2> /dev/null || true
        done
        OVERLOAD_RESULT="$subs_before $subs_peak $max_subs $rej_before $rej_after $OVERLOAD"
        echo "    [超额] 开 $OVERLOAD 路：subscribers $subs_before → $subs_peak（上限 $max_subs），rejected $rej_before → $rej_after"
    fi

    sleep "$SAMPLE_INTERVAL"
done

# ---------------------------------------------------------------------------
# 3) 收尾：等客户端退出、读最终统计、看回收
# ---------------------------------------------------------------------------
echo "== 3) 收尾（等客户端结束 + 回收检查）=="
for pid in "${CLIENT_PIDS[@]:-}"; do
    wait "$pid" 2> /dev/null || true
done
sleep 2
STATS_END="$(curl -s --max-time 3 "http://127.0.0.1:$PORT/api/stats" || true)"
CPU_END="$(proc_cpu_ticks "$SRV_PID")"
RSS_END="$(proc_rss_kb "$SRV_PID")"
FD_END="$(proc_fd "$SRV_PID")"
WALL_MS=$(( $(date +%s%3N) - T0 ))
SRV_ALIVE=no
if kill -0 "$SRV_PID" 2> /dev/null; then SRV_ALIVE=yes; fi
echo "  墙钟 $((WALL_MS / 1000))s；服务进程存活=$SRV_ALIVE；fd=$FD_END（基线 $FD_BASE）rss=${RSS_END}kB"

# ---------------------------------------------------------------------------
# 4) 功能性判定（硬门禁）
# ---------------------------------------------------------------------------
echo "== 4) 功能性判定 =="
expected_frames="$(python3 -c "print(round($DURATION * $SPEED * 25))")"
lo="$(python3 -c "print(max(1, round($expected_frames * 0.95)))")"
hi="$(python3 -c "print(round($expected_frames * 1.05) + 25)")"
killed_index=0
if [ "$KILL_HALF" = yes ]; then
    killed_index=$(( (${#CLIENT_PIDS[@]} + 1) / 2 ))
fi

bad_clients=0
short_clients=0
trimmed_total=0
for i in $(seq 1 "$CLIENTS"); do
    f="$OUT_DIR/clients/c$i.flv"
    log="$OUT_DIR/clients/c$i.log"
    if [ ! -s "$f" ]; then
        printf '  ✘ 第 %s 路没有拿到数据（日志：%s）\n' "$i" "$(tr '\n' ' ' < "$log" | head -c 120)"
        bad_clients=$((bad_clients + 1))
        continue
    fi
    # 先截到最后一个完整 tag（客户端被 --max-time / kill 掐断是预期的），再校验
    dropped_bytes="$(trim_flv "$f")"
    trimmed_total=$((trimmed_total + dropped_bytes))
    v="$f.trim"
    # 码流必须可解、参数正确、解码无 error
    vcodec="$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$v" 2>/dev/null || echo '?')"
    frames="$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$v" 2>/dev/null || echo 0)"
    decode_err="$(ffmpeg -v error -i "$v" -f null - 2>&1 > /dev/null || true)"
    if [ "$vcodec" != "h264" ] || [ -n "$decode_err" ]; then
        printf '  ✘ 第 %s 路码流有问题（codec=%s，解码错误：%s）\n' "$i" "$vcodec" "$(printf '%s' "$decode_err" | head -c 80)"
        bad_clients=$((bad_clients + 1))
        continue
    fi
    if [ "$KILL_HALF" = yes ] && [ "$i" -le "$killed_index" ]; then
        # 被 kill 的路只要求"拿到了可解的一段"
        if [ "${frames:-0}" -lt 10 ]; then
            printf '  ✘ 第 %s 路（被 kill）只解出 %s 帧\n' "$i" "$frames"
            short_clients=$((short_clients + 1))
        fi
        continue
    fi
    if [ "${frames:-0}" -lt "$lo" ] || [ "${frames:-0}" -gt "$hi" ]; then
        printf '  ✘ 第 %s 路帧数 %s 不在预期区间 [%s, %s]\n' "$i" "$frames" "$lo" "$hi"
        short_clients=$((short_clients + 1))
    fi
done
printf '    [数据] 客户端被掐断丢弃的尾字节合计：%s（切齐到完整 tag 之后才校验）\n' "$trimmed_total"
if [ "$bad_clients" -eq 0 ]; then
    check_true "$CLIENTS 路全部拿到可解码流（h264 + 解码无 error）"
else
    check_false_msg "$CLIENTS 路全部拿到可解码流" "$bad_clients 路不合格"
fi
if [ "$short_clients" -eq 0 ]; then
    check_true "存活路帧数完整（预期 $expected_frames，容差 [${lo},${hi}]）"
else
    check_false_msg "存活路帧数完整" "$short_clients 路帧数不足"
fi

# 源端丢帧：功能上允许（FR-5.2 是有意丢），但**要报出来**。
# 注意从**采样最后一刻**取值：订阅者全部注销后，`media_source` 的聚合计数会归零
# （计数器挂在订阅者上，源只持 weak_ptr）—— 读"结束之后"的 stats 只会得到 0。
last_row="$(tail -1 "$CSV")"
row_delivered="$(printf '%s' "$last_row" | cut -d, -f6)"
row_dropped="$(printf '%s' "$last_row" | cut -d, -f7)"
row_dropped_inc="$(printf '%s' "$last_row" | cut -d, -f8)"
row_rejected="$(printf '%s' "$last_row" | cut -d, -f9)"
printf '    [数据] 源端 counters（采样最后一刻）：delivered=%s dropped=%s dropped_incoming=%s rejected=%s\n' \
    "${row_delivered:-0}" "${row_dropped:-0}" "${row_dropped_inc:-0}" "${row_rejected:-0}"

# 断开后回收（NFR-6）。两种都算合格：
#   · 源还在 → `subscribers` 必须为 0；
#   · 源已被空闲释放（连源都不在列表里）→ 回收得更彻底（空闲到期时恰好没有订阅者）。
subs_end="$(stat_get media_sources.0.media_source.subscribers "$STATS_END")"
src_count="$(printf '%s' "$STATS_END" | python3 -c 'import json,sys; print(len(json.load(sys.stdin).get("media_sources",[])))' 2>/dev/null || echo 0)"
if [ "${src_count:-0}" -eq 0 ]; then
    check_true "断开后订阅者已回收（源已被空闲释放）"
else
    check "断开后订阅者归零（NFR-6）" "${subs_end:-x}" "0"
fi
# NFR-2：运行期间**同时**在线 N 路（从采样里看 —— 断开之后聚合会随源释放一起消失）
max_subs_seen="$(cut -d, -f5 "$CSV" | tail -n +2 | sort -n | tail -1)"
check "运行期间同时在线的订阅者峰值" "${max_subs_seen:-0}" "$CLIENTS"
fd_delta=$((FD_END - FD_BASE))
if [ "$fd_delta" -le 2 ] && [ "$fd_delta" -ge -2 ]; then
    check_true "断开后 fd 回落到基线（$FD_BASE → $FD_END）"
else
    check_false_msg "断开后 fd 回落到基线" "$FD_BASE → $FD_END（差 $fd_delta）"
fi
check "服务进程存活（无崩溃）" "$SRV_ALIVE" "yes"

# 错误日志 0 条（NFR-3）；Warn 只报数
# 日志级别字段是**短名**（`[E]`/`[W]`/`[I]`，见 core/logger.cpp 的 levelName()），
# 而且行首可能带 ANSI 颜色码 → 用 "[E][" 这种"级别 + 紧随其后的时间戳括号"来定位
err_lines="$(grep -cE '\[E\]\[|\[F\]\[' "$SRV_LOG" || true)"
warn_lines="$(grep -cE '\[W\]\[' "$SRV_LOG" || true)"
check "服务端错误日志 0 条（NFR-3）" "${err_lines:-0}" "0"
printf '    [数据] 服务端日志：Warn %s 条\n' "${warn_lines:-0}"

# kill 一半：被 kill 的路必须真的退订（auto_unsub 增长），其余不受影响（上面已逐路验过）
if [ "$KILL_HALF" = yes ]; then
    auto_unsub="$(stat_get media_sources.0.media_source.counters.auto_unsub "$STATS_END")"
    if [ "${auto_unsub:-0}" -ge "$killed_index" ]; then
        printf '  ✔ %-40s auto_unsub=%s（kill 了 %s 路）\n' "被 kill 的路已自动退订" "$auto_unsub" "$killed_index"
        pass=$((pass + 1))
    else
        check_false_msg "被 kill 的路已自动退订" "auto_unsub=${auto_unsub:-0} < $killed_index"
    fi
fi

# 超额连接：能进的正好是 max_subscribers 减去已有订阅者，其余必须被拒
if [ -n "$OVERLOAD_RESULT" ]; then
    read -r subs_before subs_peak max_subs rej_before rej_after overload <<< "$OVERLOAD_RESULT"
    expected_accepted=$((max_subs - subs_before))
    if [ "$expected_accepted" -lt 0 ]; then expected_accepted=0; fi
    expected_rejected=$((overload - expected_accepted))
    accepted=$((subs_peak - subs_before))
    rejected_delta=$((rej_after - rej_before))
    check "超额：接受的连接数（上限 $max_subs - 已占 $subs_before）" "$accepted" "$expected_accepted"
    check "超额：被拒的连接数（rejected 增长）" "$rejected_delta" "$expected_rejected"
fi

# ---------------------------------------------------------------------------
# 5) 性能基线（只报数，不设阈值）
# ---------------------------------------------------------------------------
echo "== 5) 性能基线（不设阈值；5 分钟折算噪声大，真门禁留 M7 的 1 小时跑）=="
python3 - "$CSV" "$CPU_BASE" "$CPU_END" "$RSS_BASE" "$RSS_END" "$WALL_MS" "$CLIENTS" "$DURATION" "$SUMMARY" > "$OUT_DIR/perf.txt" << 'PY'
import sys, csv, os
csv_path, cpu_base, cpu_end, rss_base, rss_end, wall_ms, clients, duration, summary = sys.argv[1:10]
cpu_base, cpu_end = int(cpu_base), int(cpu_end)
rss_base, rss_end = int(rss_base), int(rss_end)
wall_ms, clients, duration = int(wall_ms), int(clients), int(duration)
ncpu = os.cpu_count() or 1
hz = 100  # CLK_TCK
rows = []
with open(csv_path) as f:
    for row in csv.DictReader(f):
        rows.append({k: int(v or 0) for k, v in row.items()})
lines = []
def out(s):
    lines.append(s)
    print(s)
if len(rows) >= 2:
    first, mid, last = rows[0], rows[len(rows)//2], rows[-1]
    wall_s = max(1.0, wall_ms/1000.0)
    # 服务端 CPU：占**单核**的百分比（NFR-4 的口径）；同时给整机占用
    cpu_single = (cpu_end - cpu_base) / hz / wall_s * 100.0
    out("  服务端 CPU（全程）：单核 %.2f%%（整机 %.2f%%，%d 核）" % (cpu_single, cpu_single/ncpu, ncpu))
    # RSS 斜率：起 → 终，以及"去掉前 20% 的稳态段斜率"（启动期分配不算泄漏）
    start_idx = max(1, len(rows)//5)
    stable = rows[start_idx:]
    d_rss = last["rss_kb"] - rows[start_idx]["rss_kb"]
    d_t = max(1, last["t"] - rows[start_idx]["t"])
    out("  RSS：基线 %d kB → 结束 %d kB（Δ %+d kB）；稳态段 %d s 内 Δ %+d kB → 折算 %+.2f MB/h"
        % (rss_base, rss_end, rss_end - rss_base, d_t, d_rss, d_rss/1024.0*3600.0/d_t))
    out("  fd：%s" % ", ".join("%d@%ds" % (r["fd"], r["t"]) for r in rows[::max(1, len(rows)//6)]))
    # 唤醒合并（M5 §8 未决 #9 的关键数据）：稳态段的 popped/notify
    d_pop = last["popped"] - stable[0]["popped"]
    d_notify = last["notify"] - stable[0]["notify"]
    ratio = (d_pop / d_notify) if d_notify else float("nan")
    out("  唤醒：notify %d → %d，coalesced %d，popped %d → %d；一次唤醒平均搬 %.1f 个包"
        % (stable[0]["notify"], last["notify"], last["coalesced"], stable[0]["popped"], last["popped"], ratio))
    # 吞吐：源线程每秒推包数 / 每路每秒包数
    d_packets = last["packets"] - stable[0]["packets"]
    out("  吞吐：源端 %.1f 包/s（%d 路共享，即每路 %.1f 包/s）"
        % (d_packets / d_t, clients, d_packets / d_t / max(1, clients)))
    out("  丢帧：dropped %d、dropped_incoming %d、rejected %d（结束时累计）"
        % (last["dropped"], last["dropped_incoming"], last["rejected"]))
    out("  订阅者数：%s" % ", ".join(str(r["subscribers"]) for r in rows[::max(1, len(rows)//6)]))
else:
    out("  [数据不足] 采样点只有 %d 个（时长太短）" % len(rows))
with open(summary, "w") as f:
    f.write("\n".join(lines) + "\n")
PY
cat "$OUT_DIR/perf.txt"

# ---------------------------------------------------------------------------
# 5.1) 音视频时间轴（回填 DESIGN_M5 §8 未决 #13：节流按"混合 dts"对齐，音画会不会越走越偏）
#      做法：取一路存盘文件，算音频/视频的 pts 跨度，以及"每个视频帧到最近音频帧"的偏移峰值。
#      两者跨度都应当≈媒体时长；偏移峰值应当远小于一帧（40ms）——否则就该改成以音频时钟为准。
# ---------------------------------------------------------------------------
ref="$OUT_DIR/clients/c2.flv.trim"
[ -f "$ref" ] || ref="$OUT_DIR/clients/c1.flv.trim"
if [ -f "$ref" ]; then
    echo "  ---- 音视频时间轴（$(basename "$ref")）----"
    python3 - "$ref" << 'PY' 2> /dev/null || true
import bisect, subprocess, sys

path = sys.argv[1]


def pts(sel):
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", sel,
                          "-show_entries", "packet=pts_time", "-of", "csv=p=0", path],
                         capture_output=True, text=True).stdout
    return [float(x) for x in out.split() if x not in ("N/A", "")]


v, a = pts("v:0"), pts("a:0")
if v:
    print("    视频 pts：%.3fs → %.3fs（跨度 %.3fs，%d 包）" % (v[0], v[-1], v[-1] - v[0], len(v)))
if a:
    print("    音频 pts：%.3fs → %.3fs（跨度 %.3fs，%d 包）" % (a[0], a[-1], a[-1] - a[0], len(a)))
if v and a:
    worst = 0.0
    for x in v[::25]:  # 抽样：每 25 帧取一个，够看趋势又不慢
        i = bisect.bisect_left(a, x)
        cand = [a[j] for j in (i - 1, i, i + 1) if 0 <= j < len(a)]
        if cand:
            worst = max(worst, min(abs(x - y) for y in cand))
    print("    音画偏移峰值（视频帧 → 最近音频帧）：%.1f ms（一帧 = 40ms）" % (worst * 1000.0))
PY
fi

echo "------------------------------------------------------------------------"
echo "结果：通过 $pass 项 / 失败 $fail 项"
echo "  产物：$OUT_DIR（server.log / clients/*.flv / samples.csv / perf.txt）"
if [ "$fail" -gt 0 ]; then
    echo "结论：M6-d 未通过"
    exit 1
fi
echo "结论：M6-d 通过（$CLIENTS 路 × ${DURATION}s：码流完整可解、回收 100%、错误日志 0）"
