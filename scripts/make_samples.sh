#!/usr/bin/env bash
# ============================================================================
# 生成 M4 用的样本（**不依赖外网**，用本机 ffmpeg 现场造）
#   samples/sample.mp4   ：320x240@25fps H264 + 440Hz AAC，2 秒
#   samples/sample.h264  ：上面的视频轨转成 H264 裸流（M4-b 提 SPS/PPS 用）
#   samples/bf.mp4       ：带 B 帧的样本（学习用：观察 dts 单调 / pts 乱序）
#   samples/garbage.bin  ：随机字节（"畸形输入"用例用）
# 用法：./scripts/make_samples.sh
# ============================================================================
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/samples"
mkdir -p "$OUT"

if ! command -v ffmpeg > /dev/null 2>&1; then
    echo "错误：需要 ffmpeg（本脚本用它现场生成样本）" >&2
    exit 1
fi

ffmpeg -y -f lavfi -i "testsrc=size=320x240:rate=25" -f lavfi -i "sine=frequency=440" \
    -t 2 -pix_fmt yuv420p -c:v libx264 -preset ultrafast -c:a aac -shortest \
    "$OUT/sample.mp4" > /dev/null 2>&1

ffmpeg -y -i "$OUT/sample.mp4" -c:v copy -bsf:v h264_mp4toannexb -f h264 \
    "$OUT/sample.h264" > /dev/null 2>&1

# 带 B 帧的样本：用来观察"dts 单调、pts 乱序"（docs/AV_BASICS.md §2 ④）
ffmpeg -y -f lavfi -i "testsrc=size=320x240:rate=25" -t 1 -bf 2 -pix_fmt yuv420p \
    -c:v libx264 -preset ultrafast "$OUT/bf.mp4" > /dev/null 2>&1

head -c 65536 /dev/urandom > "$OUT/garbage.bin"

ls -l "$OUT"
echo "样本已生成：$OUT"
