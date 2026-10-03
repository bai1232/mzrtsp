#!/usr/bin/env bash
# ============================================================================
# M3-b HTTP 层验收（对应 docs/ROADMAP.md 的 M3 验收行：curl 取到测试页）
#
# 用法：./scripts/http_test.sh
#
# 六项检查：
#   1) GET /            → 200 + 测试页正文 + Content-Length 与正文一致
#   2) CORS             → Access-Control-Allow-Origin: *（FR-4.3）
#   3) GET /hello       → 自定义路由 200
#   4) GET /nope        → 404（FR-4.5）
#   5) POST /           → 405 + Allow: GET（v0.1 只支持 GET，不假装支持）
#   6) GET /live/x.flv  → 501（路由存在但 M6 才实现）
#   7) 畸形请求         → 400 + Connection: close
#   8) keep-alive       → 一条连接上两次请求都成功（curl --next 复用连接）
# ============================================================================
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/bin/http_server"
PORT="${PORT:-$((18000 + RANDOM % 2000))}"   # 随机端口：连跑多轮不撞端口
LOGFILE="/tmp/mz_http_server_$$.log"

if [ ! -x "$BIN" ]; then
    echo "错误：找不到 $BIN，请先构建（cmake --build build）" >&2
    exit 1
fi
if ! command -v curl > /dev/null 2>&1; then
    echo "错误：需要 curl" >&2
    exit 1
fi

cleanup() {
    if [ -n "${SRV_PID:-}" ]; then
        kill -INT "$SRV_PID" 2> /dev/null   # 优雅退出 → atexit flush 日志
        wait "$SRV_PID" 2> /dev/null
    fi
}
trap cleanup EXIT

fails=0
check() {   # $1=描述 $2=条件(0=通过)
    if [ "$2" -eq 0 ]; then
        printf '  ✔ %s\n' "$1"
    else
        printf '  ✗ %s\n' "$1"
        fails=$((fails + 1))
    fi
}

echo "== 启动 http_server (port=$PORT) =="
"$BIN" --port "$PORT" --recv-idle-ms 3000 > "$LOGFILE" 2>&1 &
SRV_PID=$!
for _ in $(seq 1 50); do
    grep -q 'http_server 监听' "$LOGFILE" && break
    sleep 0.1
done
if ! kill -0 "$SRV_PID" 2> /dev/null; then
    echo "错误：http_server 启动失败，日志：" >&2
    cat "$LOGFILE" >&2
    exit 1
fi
BASE="http://127.0.0.1:$PORT"

echo "== 1) 测试页 =="
BODY="/tmp/mz_http_body_$$.txt"
HDRS="/tmp/mz_http_hdrs_$$.txt"
code=$(curl -s -o "$BODY" -D "$HDRS" -w '%{http_code}' "$BASE/")
echo "  GET / → $code"
[ "$code" = "200" ]; check "GET / 返回 200" $?
grep -qi '<title>mzmedia</title>' "$BODY"; check "正文是内置测试页" $?
# Content-Length 必须与正文实际大小一致（头与实际不一致是最难查的一类问题）
cl=$(grep -i '^content-length:' "$HDRS" | tr -d '\r' | awk '{print $2}')
actual=$(stat -c%s "$BODY")
echo "  Content-Length=$cl 实际=$actual"
[ "$cl" = "$actual" ]; check "Content-Length 与正文大小一致" $?

echo "== 2) CORS（FR-4.3） =="
cors=$(grep -i '^access-control-allow-origin:' "$HDRS" | tr -d '\r' | awk '{print $2}')
echo "  Access-Control-Allow-Origin: ${cors:-（缺失）}"
[ "$cors" = "*" ]; check "响应带 Access-Control-Allow-Origin: *" $?

echo "== 3) 自定义路由 =="
code=$(curl -s -o "$BODY" -w '%{http_code}' "$BASE/hello")
echo "  GET /hello → $code"
[ "$code" = "200" ]; check "自定义路由可用" $?
grep -q 'hello from mzmedia' "$BODY"; check "自定义路由正文正确" $?

echo "== 4) 404（FR-4.5） =="
code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/no/such/path")
echo "  GET /no/such/path → $code"
[ "$code" = "404" ]; check "未知路径返回 404" $?

echo "== 5) 405（非 GET） =="
code=$(curl -s -o /dev/null -D "$HDRS" -w '%{http_code}' -X POST "$BASE/")
echo "  POST / → $code"
[ "$code" = "405" ]; check "POST 返回 405" $?
grep -qi '^allow: *GET' "$HDRS"; check "405 带 Allow: GET" $?

echo "== 6) 501（未实现的媒体路由） =="
code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/live/a.flv")
echo "  GET /live/a.flv → $code"
[ "$code" = "501" ]; check "媒体路由返回 501（M6 才实现）" $?

echo "== 7) 畸形请求 → 400 =="
# 用 printf 造一个缺版本段的请求行（curl 不会发这种），直接走裸 socket
code=$(printf 'GET /\r\n\r\n' | timeout 5 nc 127.0.0.1 "$PORT" | head -1 | awk '{print $2}')
echo "  畸形请求 → status=${code:-（无响应）}"
[ "$code" = "400" ]; check "畸形请求返回 400" $?

echo "== 8) keep-alive：一条连接上两次请求 =="
# curl --next 在同一个进程里复用连接；两次请求都成功即说明连接没被提前关掉
out=$(curl -s -o /dev/null -w '%{http_code} ' "$BASE/" --next -s -o /dev/null -w '%{http_code}' "$BASE/hello" 2> /dev/null)
echo "  两次请求的状态码：$out"
[ "$out" = "200 200" ]; check "一条连接上两次请求都成功" $?

echo "== 9) chunked 流式（curl -N） =="
code=$(curl -s -N -o "$BODY" -D "$HDRS" -w '%{http_code}' --max-time 5 "$BASE/stream")
echo "  GET /stream → $code"
grep -qi '^transfer-encoding: *chunked' "$HDRS"; check "声明 Transfer-Encoding: chunked" $?
if grep -qi '^content-length:' "$HDRS"; then cl=1; else cl=0; fi
[ "$cl" -eq 0 ]; check "chunked 响应没有 Content-Length" $?
grep -q 'chunk-3' "$BODY"; check "curl -N 收到全部块数据" $?

echo "== 10) Range（单区间 206 / 越界 416） =="
code=$(curl -s -o "$BODY" -D "$HDRS" -w '%{http_code}' -r 0-9 "$BASE/")
size=$(stat -c%s "$BODY")
echo "  Range: bytes=0-9 → $code, 长度 $size"
[ "$code" = "206" ]; check "Range 返回 206" $?
[ "$size" = "10" ]; check "只返回请求的 10 字节" $?
grep -qi '^content-range: *bytes 0-9/' "$HDRS"; check "带 Content-Range: bytes 0-9/总长" $?
code=$(curl -s -o /dev/null -w '%{http_code}' -r 999999- "$BASE/")
echo "  Range: bytes=999999- → $code"
[ "$code" = "416" ]; check "越界 Range 返回 416" $?

echo "== 11) /api/stats =="
code=$(curl -s -o "$BODY" -w '%{http_code}' "$BASE/api/stats")
echo "  GET /api/stats → $code"
[ "$code" = "200" ]; check "/api/stats 返回 200" $?
if grep -q '"requests":' "$BODY" && grep -q '"sessions":' "$BODY" && grep -q '"sendOverflow":' "$BODY"; then ok=0; else ok=1; fi
[ "$ok" -eq 0 ]; check "stats 是 JSON 且含 requests/sessions/sendOverflow" $?

echo "== 服务器统计（来自日志） =="
grep '统计：' "$LOGFILE" | tail -1 || true
rm -f "$BODY" "$HDRS"

echo "------------------------------------------------------------------------"
if [ "$fails" -eq 0 ]; then
    echo "结论：M3 验收全部通过（M3-b + M3-c）"
else
    echo "结论：$fails 项失败（日志：$LOGFILE）"
fi
exit "$fails"
