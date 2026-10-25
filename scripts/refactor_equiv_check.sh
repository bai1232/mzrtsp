#!/usr/bin/env bash
# ============================================================================
# 重构等价性检查（**字节级**）：同一份输入 → 同一份输出字节
#
# 为什么需要它：重构（把 DemuxerProducer 拆成 读/形状转换/转码 三段）**不允许改变行为**。
# 单元测试只能证明"我以为的语义没变"；这个脚本证明"**实际产出的字节没变**"。
#
# 做法：跑一遍所有 demo（它们分别覆盖 remux / Annex-B / ADTS / 转码 / TS 封装五条路径），
# 对产物算 md5，与 `scripts/refactor_baseline.txt` 里记录的基线逐条比对。
#
# 用法：
#   ./scripts/refactor_equiv_check.sh          # 与基线比对（重构后必须全绿）
#   ./scripts/refactor_equiv_check.sh --save   # 重新生成基线（只在**有意改变行为**时用）
#
# 注意：样本不入库，先跑 ./scripts/make_samples.sh。
# 已验证：同一构建重复运行两次 → md5 完全相同（否则这个脚本毫无意义）。
# ============================================================================
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="/tmp/mzmedia_equiv"
BASE_FILE="$ROOT/scripts/refactor_baseline.txt"
SAVE=0
[ "${1:-}" = "--save" ] && SAVE=1

DEMO_FLV="$ROOT/build/bin/flv_mux_demo"
DEMO_TS="$ROOT/build/bin/ts_mux_demo"
DEMO_TX="$ROOT/build/bin/transcode_demo"
for exe in "$DEMO_FLV" "$DEMO_TS" "$DEMO_TX"; do
    [ -x "$exe" ] || { echo "错误：找不到 $exe，先 cmake --build build" >&2; exit 1; }
done

rm -rf "$OUT"; mkdir -p "$OUT"
rows=""
add_row() { # add_row <名字> <文件>
    local md5
    md5="$(md5sum "$2" | cut -d' ' -f1)"
    rows="${rows}${1} ${md5}\n"
}

# ---- 路径 1/2/3：remux（MP4/MKV/TS/纯视频 TS/裸流/B 帧源）----
for pair in "mp4:sample.mp4" "mkv:sample.mkv" "ts:sample.ts" "vonly_ts:vonly.ts" \
            "h264_raw:sample.h264" "bframes:bf.mp4"; do
    name="${pair%%:*}"; file="${pair##*:}"
    "$DEMO_FLV" "$ROOT/samples/$file" "$OUT/$name.flv" > "$OUT/$name.log" 2>&1
    add_row "flv_$name" "$OUT/$name.flv"
done

# ---- 路径 4：转码（同一输入、两种目标高度：缩放 / 只重编码）----
"$DEMO_TX" "$ROOT/samples/sample.mp4" "$OUT/tx_120.flv" 120 > "$OUT/tx_120.log" 2>&1
add_row "tx_sample_120p" "$OUT/tx_120.flv"
"$DEMO_TX" "$ROOT/samples/sample.mp4" "$OUT/tx_src.flv" 0 > "$OUT/tx_src.log" 2>&1
add_row "tx_sample_src" "$OUT/tx_src.flv"
if [ -f "$ROOT/samples/sample1080.mp4" ]; then
    "$DEMO_TX" "$ROOT/samples/sample1080.mp4" "$OUT/tx_720.flv" 720 > "$OUT/tx_720.log" 2>&1
    add_row "tx_1080_to_720p" "$OUT/tx_720.flv"
fi

# ---- 路径 5：TS 封装（含负首 dts 的整体平移）----
for pair in "mp4:sample.mp4" "bframes:bf.mp4"; do
    name="${pair%%:*}"; file="${pair##*:}"
    "$DEMO_TS" "$ROOT/samples/$file" "$OUT/ts_$name.ts" > "$OUT/ts_$name.log" 2>&1
    add_row "ts_$name" "$OUT/ts_$name.ts"
done

printf "%b" "$rows" > "$OUT/manifest.txt"

if [ "$SAVE" = "1" ]; then
    {
        echo "# 重构等价性基线（scripts/refactor_equiv_check.sh --save 生成）"
        echo "# 每行：<用例名> <产物 md5>。**有意改变行为**时才重新生成，并在 CHANGELOG 里写明。"
        printf "%b" "$rows"
    } > "$BASE_FILE"
    echo "已写入基线：$BASE_FILE（$(grep -c '^[a-z]' "$BASE_FILE") 条）"
    cat "$BASE_FILE"
    exit 0
fi

[ -f "$BASE_FILE" ] || { echo "错误：没有基线 $BASE_FILE（先跑 --save）" >&2; exit 1; }

pass=0
fail=0
while read -r name want; do
    case "$name" in \#*|"") continue;; esac
    got="$(grep -E "^$name " "$OUT/manifest.txt" | awk '{print $2}')"
    if [ "$got" = "$want" ]; then
        printf '  ✔ %-24s %s\n' "$name" "$got"
        pass=$((pass + 1))
    else
        printf '  ✘ %-24s 实际=%s 期望=%s\n' "$name" "${got:-（缺产物）}" "$want"
        fail=$((fail + 1))
    fi
done < "$BASE_FILE"

echo "------------------------------------------------------------------------"
echo "结果：通过 $pass 项 / 失败 $fail 项（产物：$OUT）"
[ "$fail" -eq 0 ] || { echo "结论：**产出字节变了** —— 重构不允许改变行为（除非本次就是有意改）"; exit 1; }
echo "结论：字节级等价（同一输入 → 同一输出），重构未改变行为"
