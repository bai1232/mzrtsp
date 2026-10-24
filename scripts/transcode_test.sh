#!/usr/bin/env bash
# ============================================================================
# M10-a 验收：转码（解码 → libswscale → x264）+ **NFR-5 的 speed 实测**
#
# 为什么必须有它：转码写错的典型表现是"能出文件、播放器也能播，但分辨率/帧率/时间戳不对"，
# 或者"比实时还慢"。所以判据必须是外部的：
#   ① ffprobe 认输出（编码 / 分辨率 / 帧率 / 时长）
#   ② `ffmpeg -f null -` 完整解码零 error（真解码，不是只看头）
#   ③ `speed = 媒体时长 / 墙钟 ≥ 1.0`（NFR-5 的原文口径；数字来自程序自报，机器可读行）
#
# 用法：./scripts/transcode_test.sh
# 依赖：build/bin/transcode_demo、samples/sample1080.mp4、ffprobe、ffmpeg
# ============================================================================
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEMO="$ROOT/build/bin/transcode_demo"
SRC1080="$ROOT/samples/sample1080.mp4"
SRCSMALL="$ROOT/samples/sample.mp4"
OUT="/tmp/mzmedia_transcode"
OUT720="$OUT/out720.flv"
OUTFULL="$OUT/out_full.flv"

[ -x "$DEMO" ] || { echo "错误：找不到 $DEMO，先执行 cmake --build build" >&2; exit 1; }
[ -f "$SRC1080" ] || { echo "错误：找不到 samples/sample1080.mp4，先执行 ./scripts/make_samples.sh" >&2; exit 1; }
for tool in ffprobe ffmpeg; do
    command -v "$tool" > /dev/null 2>&1 || { echo "错误：需要 $tool" >&2; exit 1; }
done

pass=0
fail=0
check() { # check <说明> <实际> <期望>
    if [ "$2" = "$3" ]; then
        printf '  ✔ %-46s %s\n' "$1" "$2"; pass=$((pass + 1))
    else
        printf '  ✘ %-46s 实际=%s 期望=%s\n' "$1" "$2" "$3"; fail=$((fail + 1))
    fi
}
check_true() { printf '  ✔ %-46s\n' "$1"; pass=$((pass + 1)); }
check_false() { printf '  ✘ %-46s %s\n' "$1" "$2"; fail=$((fail + 1)); }
check_range() { # check_range <说明> <实际> <下界> <上界>
    if [ "$2" -ge "$3" ] && [ "$2" -le "$4" ]; then
        printf '  ✔ %-46s %s（期望 %s..%s）\n' "$1" "$2" "$3" "$4"; pass=$((pass + 1))
    else
        printf '  ✘ %-46s 实际=%s 期望 %s..%s\n' "$1" "$2" "$3" "$4"; fail=$((fail + 1))
    fi
}

rm -rf "$OUT"; mkdir -p "$OUT"

echo "== 1) 1080p → 720p 转码（NFR-5 的口径）=="
if ! "$DEMO" "$SRC1080" "$OUT720" 720 > "$OUT/run720.log" 2>&1; then
    check_false "transcode_demo 1080p→720p" "退出码非 0（见 $OUT/run720.log）"
    sed -n '1,20p' "$OUT/run720.log"
    exit 1
fi
line="$(grep -E '^  机器可读：' "$OUT/run720.log" || true)"
out_w="$(echo "$line" | sed -nE 's/.*OUT_W=([0-9]+).*/\1/p')"
out_h="$(echo "$line" | sed -nE 's/.*OUT_H=([0-9]+).*/\1/p')"
speed="$(echo "$line" | sed -nE 's/.*SPEED=([0-9.]+).*/\1/p')"
frames="$(echo "$line" | sed -nE 's/.*FRAMES=([0-9]+).*/\1/p')"
echo "  自报：OUT_W=$out_w OUT_H=$out_h SPEED=$speed FRAMES=$frames"

check "输出宽" "$out_w" "1280"
check "输出高" "$out_h" "720"
# NFR-5：≥ 1.0x 实时（不达标就如实红，不改口径）
if awk -v s="${speed:-0}" 'BEGIN { exit (s >= 1.0) ? 0 : 1 }'; then
    check_true "NFR-5 转码速度 ≥ 1.0x（实测 ${speed}x）"
else
    check_false "NFR-5 转码速度 ≥ 1.0x" "实测 ${speed}x"
fi
# 帧数完整性：源 5s@25fps = 125 帧；一帧都不能少
check "编码帧数（源 125）" "$frames" "125"

echo "== 2) 产物结构（ffprobe 独立判定）=="
check "视频编码" "$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$OUT720")" "h264"
check "分辨率" "$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$OUT720")" "1280,720"
check "帧率" "$(ffprobe -v error -select_streams v:0 -show_entries stream=avg_frame_rate -of csv=p=0 "$OUT720")" "25/1"
check_range "解码帧数（ffprobe -count_frames）" "$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$OUT720")" 120 130
# 时间戳必须单调（曾经因为"输入 pts 被单调守卫钳过"导致输出 dts 乱序）
if ffprobe -v error -select_streams v:0 -show_entries packet=dts_time -of csv=p=0 "$OUT720" \
    | awk 'NF && $1 != "N/A" { if (prev != "" && $1 + 0 <= prev + 0) bad = 1; prev = $1 } END { exit bad ? 1 : 0 }'
then
    check_true "输出 dts 严格单调递增"
else
    check_false "输出 dts 严格单调递增" "存在回退/重复"
fi
err="$(ffmpeg -v error -i "$OUT720" -f null - 2>&1 > /dev/null || true)"
if [ -z "$err" ]; then
    check_true "ffmpeg 完整解码无 error"
else
    check_false "ffmpeg 完整解码无 error" "$(echo "$err" | head -2)"
fi

echo "== 3) 只重编码不缩放（out_height=0）=="
# 这条能证明"缩放不是必需的"：同一个 Transcoder 在尺寸/像素格式都匹配时不建 sws
if "$DEMO" "$SRCSMALL" "$OUTFULL" 0 > "$OUT/runfull.log" 2>&1; then
    check "不缩放时分辨率保持" \
        "$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$OUTFULL")" \
        "320,240"
    err="$(ffmpeg -v error -i "$OUTFULL" -f null - 2>&1 > /dev/null || true)"
    if [ -z "$err" ]; then
        check_true "不缩放产物：ffmpeg 完整解码无 error"
    else
        check_false "不缩放产物：ffmpeg 完整解码无 error" "$(echo "$err" | head -2)"
    fi
else
    check_false "transcode_demo 不缩放" "退出码非 0（见 $OUT/runfull.log）"
fi

echo
echo "结果：通过 $pass 项 / 失败 $fail 项（产物：$OUT）"
[ "$fail" -eq 0 ] || exit 1
echo "结论：M10-a 通过（1080p→720p 可解码、帧数完整、时间戳单调、speed ≥ 1.0x）"
