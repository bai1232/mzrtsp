#!/usr/bin/env bash
# ============================================================================
# 生成 M4 用的样本（**不依赖外网**，用本机 ffmpeg 现场造）
#   samples/sample.mp4   ：320x240@25fps H264 + 440Hz AAC，2 秒
#   samples/sample.h264  ：上面的视频轨转成 H264 裸流（M4-b 提 SPS/PPS 用）
#   samples/bf.mp4       ：带 B 帧的样本（学习用：观察 dts 单调 / pts 乱序）
#   samples/garbage.bin  ：随机字节（"畸形输入"用例用）
#   samples/sample.mkv   ：sample.mp4 转封装成 Matroska（M8 / FR-2.2）
#   samples/sample.ts    ：sample.mp4 转封装成 MPEG-TS（M8：AAC 在 TS 里是 ADTS，要现场造 ASC）
#   samples/vonly.ts     ：纯视频 TS（"没有音频也要能出 FLV"，FR-3.3）
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

# M8（FR-2.2）：同一份 MP4 **转封装**成 MKV / TS。
# 为什么用 -c copy：这批要测的是**解封装**（容器 → 码流），码流必须与 sample.mp4 完全同一份，
# 否则"TS 出来的 FLV 与 MP4 出来的 FLV 是否等价"就没有可比性。
ffmpeg -y -i "$OUT/sample.mp4" -c copy "$OUT/sample.mkv" > /dev/null 2>&1
# TS 必须带 -f mpegts：AAC 会被写成 ADTS（每帧自带头、没有 extradata）—— 这正是 M8 要处理的形状
ffmpeg -y -i "$OUT/sample.mp4" -c copy -f mpegts "$OUT/sample.ts" > /dev/null 2>&1
# 纯视频 TS：验证"没有音频也不报错"（FR-3.3）
ffmpeg -y -i "$OUT/sample.mp4" -an -c:v copy -f mpegts "$OUT/vonly.ts" > /dev/null 2>&1

# M10（NFR-5）：1080p 样本 —— 转码验收口径是"1080p → 720p，x264 veryfast ≥ 1.0x 实时"。
# 用 lavfi testsrc 现场造（不入库：5 秒 1080p 约几 MB）；GOP=50 ≈ 2s@25fps，便于切片/关键帧观察。
ffmpeg -y -f lavfi -i "testsrc=size=1920x1080:rate=25" -t 5 -pix_fmt yuv420p \
    -c:v libx264 -preset veryfast -g 50 "$OUT/sample1080.mp4" > /dev/null 2>&1

# M10-b（HEVC → H264 转码）：HEVC 源**只有转码才能进 FLV**（不转码 = 415）。
# 用 libx265 现场造（不入库）；`-tag:v hvc1` 让 MP4 的 extradata 是标准 hvcC。
# 造不出来（本机 FFmpeg 没有 libx265）时**不阻塞**其它样本，但会明确打一行原因 ——
# 相关的验收项会自动跳过并打印"跳过"，不会假装通过。
if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q "libx265"; then
    ffmpeg -y -f lavfi -i "testsrc=size=320x240:rate=25" -t 2 -pix_fmt yuv420p \
        -c:v libx265 -preset ultrafast -x265-params log-level=none:keyint=50 \
        -tag:v hvc1 "$OUT/sample_hevc.mp4" > /dev/null 2>&1
    if [ ! -s "$OUT/sample_hevc.mp4" ]; then
        echo "注意：sample_hevc.mp4 生成失败（HEVC 相关验收会跳过）" >&2
        rm -f "$OUT/sample_hevc.mp4"
    fi
else
    rm -f "$OUT/sample_hevc.mp4"
    echo "注意：本机 FFmpeg 没有 libx265 → 跳过 sample_hevc.mp4（HEVC 相关验收会跳过）" >&2
fi

# M10-b（"只有音频的源不许转码"）：纯音频样本 —— FLV 的音频必须原样透传，
# 而"转码"这个词在 v0.2 里只指视频，所以这类源要**明确拒绝**而不是转出个没画面的流。
ffmpeg -y -i "$OUT/sample.mp4" -vn -c:a copy "$OUT/sample_audio.aac" > /dev/null 2>&1

head -c 65536 /dev/urandom > "$OUT/garbage.bin"

ls -l "$OUT"
echo "样本已生成：$OUT"
