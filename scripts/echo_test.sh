#!/usr/bin/env bash
# ============================================================================
# M2-3 网络层验收脚本（对应 docs/ROADMAP.md 的 M2 验收行、FR-4.4、NFR-2、NFR-6）
#
# 用法：
#   ./scripts/echo_test.sh                 # 默认 100MB（ROADMAP 的验收口径）
#   SIZE_MB=8 ./scripts/echo_test.sh       # 快速跑一版
#
# 做四件事：
#   1) nc 灌 SIZE_MB 随机数据，比对 sha256（回显必须逐字节一致）
#   2) 8 路并发同时回显，校验和都要一致（NFR-2 不串流）
#   3) 连上不发数据 → 空闲超时必须断开（FR-4.4）
#   4) 断开之后服务器 fd 回落到基线（NFR-6 不泄漏）
#
# 为什么用 sha256 而不是"看有没有报错"：回显场景的失败模式是**悄悄错位/截断**，
# 只有校验和能证明"一个字节都没变"。
# ============================================================================
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/bin/echo_server"
SIZE_MB="${SIZE_MB:-100}"
PORT="${PORT:-19000}"
CONCURRENCY=8
LOGFILE="/tmp/mz_echo_server_$$.log"
SRC="/tmp/mz_echo_src_$$.bin"

if [ ! -x "$BIN" ]; then
    echo "错误：找不到 $BIN，请先构建（cmake --build build）" >&2
    exit 1
fi
if ! command -v nc > /dev/null 2>&1; then
    echo "错误：需要 nc（netcat）" >&2
    exit 1
fi

cleanup() {
    [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2> /dev/null
    rm -f "$SRC"
}
trap cleanup EXIT

fails=0
check() {   # $1=描述  $2=条件(0=通过)
    if [ "$2" -eq 0 ]; then
        printf '  ✔ %s\n' "$1"
    else
        printf '  ✗ %s\n' "$1"
        fails=$((fails + 1))
    fi
}

# 故意**不用** nc -N：-N 会在发完 stdin 后立刻半关闭，而"对端半关闭"时本实现
# 会立即断开（未发完的队列会丢，见 DESIGN_M2 §4.6 的已知限制）。
# 保持连接打开、让服务器的空闲超时来收尾，才能完整收到回显。
NC_OPTS=""


# fd 计数：服务器进程打开的 fd 数
count_srv_fds() { ls "/proc/$1/fd" 2> /dev/null | wc -l; }

echo "== 启动 echo_server (port=$PORT, 空闲超时 2s 便于脚本收尾) =="
"$BIN" --port "$PORT" --recv-idle-ms 2000 > "$LOGFILE" 2>&1 &
SRV_PID=$!
for _ in $(seq 1 50); do
    grep -q 'echo server 监听' "$LOGFILE" && break
    sleep 0.1
done
if ! kill -0 "$SRV_PID" 2> /dev/null; then
    echo "错误：echo_server 启动失败，日志：" >&2
    cat "$LOGFILE" >&2
    exit 1
fi

BASE_FDS=$(count_srv_fds "$SRV_PID")
echo "  服务器基线 fd 数：$BASE_FDS"

# ---------------------------------------------------------------------------
echo "== 1) 回显 ${SIZE_MB}MB 并比对 sha256 =="
dd if=/dev/urandom of="$SRC" bs=1M count="$SIZE_MB" status=none
GOT="/tmp/mz_echo_got_$$.bin"
# 先落盘再比：管道里一旦有环节提前退出，核验就变成假的（踩过）
# shellcheck disable=SC2086
timeout 300 nc $NC_OPTS 127.0.0.1 "$PORT" < "$SRC" > "$GOT"
src_bytes=$(stat -c%s "$SRC"); got_bytes=$(stat -c%s "$GOT")
echo "  源 ${src_bytes} 字节 sha256=$(sha256sum "$SRC" | cut -c1-16)..."
echo "  回显 ${got_bytes} 字节 sha256=$(sha256sum "$GOT" | cut -c1-16)..."
cmp -s "$SRC" "$GOT"; rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  首个差异：$(cmp -l "$SRC" "$GOT" 2>/dev/null | head -1)"
fi
rm -f "$GOT"
[ "$rc" -eq 0 ]; check "${SIZE_MB}MB 回显逐字节一致（cmp 核验）" $?

# ---------------------------------------------------------------------------
echo "== 2) ${CONCURRENCY} 路并发回显 =="
dd if=/dev/urandom of="$SRC" bs=1M count=1 status=none   # 每路 1MB
sha_one=$(sha256sum "$SRC" | cut -d' ' -f1)
pids=""
for i in $(seq 1 "$CONCURRENCY"); do
    # shellcheck disable=SC2086
    ( timeout 60 nc $NC_OPTS 127.0.0.1 "$PORT" < "$SRC" | sha256sum | cut -d' ' -f1 > "/tmp/mz_echo_out_$$_$i" ) &
    pids="$pids $!"
done
for pid in $pids; do wait "$pid"; done
bad=0
for i in $(seq 1 "$CONCURRENCY"); do
    got=$(cat "/tmp/mz_echo_out_$$_$i" 2> /dev/null)
    [ "$got" = "$sha_one" ] || bad=$((bad + 1))
    rm -f "/tmp/mz_echo_out_$$_$i"
done
[ "$bad" -eq 0 ]; check "${CONCURRENCY} 路并发校验和全部一致（不串流）" $?

# ---------------------------------------------------------------------------
echo "== 3) 空闲超时（连上不发数据） =="
# 服务器 recv_idle=2s：正常情况 2~4s 内被断开（读返回 EOF）
# 用 bash 的 /dev/tcp：建立连接后**什么都不发**，等服务器断开（读返回 EOF）
start_ms=$(date +%s%3N)
if exec 3<> "/dev/tcp/127.0.0.1/$PORT"; then
    timeout 15 head -c 1 <&3 > /dev/null 2>&1
    rc=$?
    exec 3<&-
else
    rc=99
fi
elapsed=$(( $(date +%s%3N) - start_ms ))
echo "  连接在 ${elapsed}ms 后被断开（rc=$rc）"
[ "$elapsed" -ge 2000 ] && [ "$elapsed" -lt 10000 ]; check "空闲连接在阈值后被断开" $?

# ---------------------------------------------------------------------------
echo "== 4) fd 回落（NFR-6） =="
sleep 1
NOW_FDS=$(count_srv_fds "$SRV_PID")
echo "  当前 fd 数：$NOW_FDS（基线 $BASE_FDS）"
[ "$NOW_FDS" -le $((BASE_FDS + 2)) ]; check "连接全部断开后 fd 回落到基线" $?

# ---------------------------------------------------------------------------
echo "== 服务器统计（来自日志） =="
grep '统计：' "$LOGFILE" | tail -2 || true

echo "------------------------------------------------------------------------"
if [ "$fails" -eq 0 ]; then
    echo "结论：M2-3 验收全部通过"
else
    echo "结论：$fails 项失败（日志：$LOGFILE）"
fi
exit "$fails"
