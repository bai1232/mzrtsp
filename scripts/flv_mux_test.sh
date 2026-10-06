#!/usr/bin/env bash
# ============================================================================
# M6-a 验收：demux → FLV 落盘 → **ffprobe 校验**
#
# 为什么这一步不能省：FLV 写错的表现是"能连上、播放器一片黑"，没有异常也没有报错。
# 单元测试只能证明"我按自己理解的规范拼了字节"；只有 ffprobe / ffplay 认了，
# 才能说"这是真的 FLV"（对应 docs/TESTING.md §3 的集成测试方案）。
#
# 用法：./scripts/flv_mux_test.sh
# 依赖：build/bin/flv_mux_demo（cmake --build build）、samples/sample.mp4、ffprobe
# ============================================================================
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEMO="$ROOT/build/bin/flv_mux_demo"
SAMPLE="$ROOT/samples/sample.mp4"
OUT="/tmp/mzmedia_flv_mux_check.flv"

[ -x "$DEMO" ] || { echo "错误：找不到 $DEMO，先执行 cmake --build build" >&2; exit 1; }
[ -f "$SAMPLE" ] || { echo "错误：找不到样本 $SAMPLE，先执行 ./scripts/make_samples.sh" >&2; exit 1; }
command -v ffprobe > /dev/null 2>&1 || { echo "错误：需要 ffprobe" >&2; exit 1; }

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

echo "== 1) 生成 FLV =="
"$DEMO" "$SAMPLE" "$OUT"

echo "== 2) 文件头 =="
magic="$(head -c 3 "$OUT" | tr -d '\0')"
check "前 3 字节是 FLV" "$magic" "FLV"

echo "== 3) ffprobe 流参数（与源一致）=="
vcodec="$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$OUT")"
width="$(ffprobe -v error -select_streams v:0 -show_entries stream=width -of csv=p=0 "$OUT")"
height="$(ffprobe -v error -select_streams v:0 -show_entries stream=height -of csv=p=0 "$OUT")"
acodec="$(ffprobe -v error -select_streams a:0 -show_entries stream=codec_name -of csv=p=0 "$OUT")"
check "视频编码" "$vcodec" "h264"
check "宽" "$width" "320"
check "高" "$height" "240"
check "音频编码" "$acodec" "aac"

echo "== 4) 帧数与时长 =="
frames="$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$OUT")"
duration="$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$OUT" | cut -d. -f1)"
# 源是 2 秒 @25fps = 50 帧；允许 ±2
if [ "$frames" -ge 48 ] && [ "$frames" -le 52 ]; then
    printf '  ✔ %-34s %s 帧（源 50）\n' "解码出的视频帧数" "$frames"
    pass=$((pass + 1))
else
    printf '  ✘ %-34s 实际=%s 期望≈50\n' "解码出的视频帧数" "$frames"
    fail=$((fail + 1))
fi
if [ "$duration" = "2" ]; then
    printf '  ✔ %-34s %s 秒\n' "时长" "$duration"
    pass=$((pass + 1))
else
    printf '  ✘ %-34s 实际=%s 期望=2\n' "时长" "$duration"
    fail=$((fail + 1))
fi

echo "== 5) 完整解码不应有错误（用 ffmpeg 解码一遍；ffprobe 只读头，力度不够）=="
err="$(ffmpeg -v error -i "$OUT" -f null - 2>&1 >/dev/null || true)"
if [ -z "$err" ]; then
    echo "  ✔ ffmpeg 完整解码无 error 输出"
    pass=$((pass + 1))
else
    printf '  ✘ ffmpeg 解码有错误输出：\n%s\n' "$err"
    fail=$((fail + 1))
fi

echo "------------------------------------------------------------------------"
echo "结果：通过 $pass 项 / 失败 $fail 项（产物：$OUT）"
if [ "$fail" -gt 0 ]; then
    echo "结论：M6-a 未通过"
    exit 1
fi
echo "结论：M6-a 通过（ffprobe 认了这份 FLV：h264 320x240 + aac，2 秒 50 帧）"
