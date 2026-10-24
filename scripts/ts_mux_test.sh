#!/usr/bin/env bash
# ============================================================================
# M9-a 验收：demux → PES → TS 落盘 → **ffprobe 校验**
#
# 为什么这一步不能省：TS/PSI 写错的表现是"能打开、但没有节目 / 一片花"。
# 单元测试（`mpegts` 组）只能证明"我按自己理解的 ISO 13818-1 拼了字节"；
# 只有 ffprobe / ffmpeg 认了，才能说"这是真的 MPEG-TS"（对应 docs/TESTING.md §3）。
#
# 判据分三层（互为独立）：
#   ① 结构：188 字节对齐 + 每个包同步字 0x47（错了后面全废，先卡这一层）
#   ② 元数据：ffprobe 认出 h264 + aac 44100 / 帧数
#   ③ 内容：`ffmpeg -f null -` **零 error 输出**（真解码，不是只看头）
#
# 用法：./scripts/ts_mux_test.sh
# 依赖：build/bin/ts_mux_demo、samples/{sample.mp4,sample.ts,vonly.ts}、ffprobe、ffmpeg
# ============================================================================
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEMO="$ROOT/build/bin/ts_mux_demo"
SAMPLES="$ROOT/samples"
OUTDIR="/tmp/mzmedia_ts_mux"

[ -x "$DEMO" ] || { echo "错误：找不到 $DEMO，先执行 cmake --build build" >&2; exit 1; }
for f in sample.mp4 sample.ts vonly.ts; do
    [ -f "$SAMPLES/$f" ] || { echo "错误：找不到样本 $f，先执行 ./scripts/make_samples.sh" >&2; exit 1; }
done
for tool in ffprobe ffmpeg python3; do
    command -v "$tool" > /dev/null 2>&1 || { echo "错误：需要 $tool" >&2; exit 1; }
done

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
check_true() { printf '  ✔ %-40s\n' "$1"; pass=$((pass + 1)); }
check_false() { printf '  ✘ %-40s %s\n' "$1" "$2"; fail=$((fail + 1)); }
# ★ ffprobe 对 mpegts 会把同一个流**打两遍**（PMT 预扫描 + 真实解析），
#   直接取值会得到 "h264\n\nh264" 这种，`check` 必然红。统一用这个助手去重。
probe() { ffprobe -v error "$@" 2>/dev/null | awk 'NF' | sort -u; }
probe_count() { ffprobe -v error "$@" 2>/dev/null | awk 'NF' | sort -u | wc -l | tr -d ' '; }

check_range() { # check_range <说明> <实际> <下界> <上界>
    if [ "$2" -ge "$3" ] && [ "$2" -le "$4" ]; then
        printf '  ✔ %-40s %s（期望 %s..%s）\n' "$1" "$2" "$3" "$4"
        pass=$((pass + 1))
    else
        printf '  ✘ %-40s 实际=%s 期望 %s..%s\n' "$1" "$2" "$3" "$4"
        fail=$((fail + 1))
    fi
}

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

# 三份输入覆盖三种形状：MP4（avcC/ASC）、TS（Annex-B/ADTS）、纯视频 TS（无音频）
for entry in "mp4:sample.mp4" "ts:sample.ts" "vonly:vonly.ts"; do
    label="${entry%%:*}"
    sample="${entry#*:}"
    out="$OUTDIR/$label.ts"

    echo "== 输入 $sample（标签 $label）=="
    if ! "$DEMO" "$SAMPLES/$sample" "$out"; then
        check_false "ts_mux_demo 处理 $sample" "退出码非 0"
        continue
    fi
    [ -f "$out" ] || { check_false "产物存在" "$out"; continue; }

    # ① 结构：188 对齐 + 同步字（用 python 逐包验，比 wc -c 更硬）
    if python3 - "$out" > "$OUTDIR/$label.struct.txt" 2>&1 << 'PY'
import sys
data = open(sys.argv[1], 'rb').read()
if len(data) == 0:
    sys.exit("空文件")
if len(data) % 188 != 0:
    sys.exit("长度 %d 不是 188 的整数倍（余 %d）" % (len(data), len(data) % 188))
bad = [i for i in range(0, len(data), 188) if data[i] != 0x47]
if bad:
    sys.exit("第 %d 个包缺同步字 0x47" % (bad[0] // 188))
print("%d 字节 / %d 包，同步字全对" % (len(data), len(data) // 188))
PY
    then
        check_true "[$label] 188 对齐 + 同步字（$(cat "$OUTDIR/$label.struct.txt")）"
    else
        check_false "[$label] 188 对齐 + 同步字" "$(cat "$OUTDIR/$label.struct.txt")"
    fi

    # ② 元数据
    check "[$label] 视频编码" \
        "$(probe -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$out")" "h264"
    check "[$label] 分辨率" \
        "$(probe -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$out")" "320,240"
    if [ "$label" = "vonly" ]; then
        check "[$label] 音频流数" \
            "$(probe_count -select_streams a -show_entries stream=index -of csv=p=0 "$out")" "0"
    else
        check "[$label] 音频编码" \
            "$(probe -select_streams a:0 -show_entries stream=codec_name -of csv=p=0 "$out")" "aac"
        check "[$label] 音频采样率" \
            "$(probe -select_streams a:0 -show_entries stream=sample_rate -of csv=p=0 "$out")" "44100"
    fi
    frames="$(probe -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$out")"
    frames="$(echo "$frames" | head -1)"
    check_range "[$label] 视频帧数" "${frames:-0}" 1 60

    # ③ 内容：真解码，必须零 error
    err="$(ffmpeg -v error -i "$out" -f null - 2>&1 > /dev/null || true)"
    if [ -z "$err" ]; then
        check_true "[$label] ffmpeg 完整解码无 error"
    else
        check_false "[$label] ffmpeg 完整解码无 error" "$err"
    fi

    # ④ 时间戳可读性（**最容易漏的一条**）：PES 里的 PTS 必须被 ffprobe 读出来。
    #    踩过：把 PTS_DTS_flags 写进 PES 头的 byte6 而不是 byte7，ffmpeg 会认为"没有时间戳"，
    #    把 5/10 字节当 header_data 跳过 → 解码看起来正常（切片都能播），但 `pts=N/A`、
    #    且再封一道就报 "non monotonically increasing dts"。单测当时把**错的布局**断言成了对的。
    pts_first="$(ffprobe -v error -select_streams v:0 -show_entries packet=pts -of csv=p=0 "$out" | awk 'NF' | head -1)"
    if [ -n "$pts_first" ] && [ "$pts_first" != "N/A" ]; then
        check_true "[$label] PES 时间戳可读（首包 pts=$pts_first）"
    else
        check_false "[$label] PES 时间戳可读" "首包 pts=$pts_first（N/A = 写错了 PES 头，见 DESIGN_M9 §6）"
    fi
    # 且必须**单调**（相邻包 dts 不递减）
    if ffprobe -v error -select_streams v:0 -show_entries packet=dts -of csv=p=0 "$out" \
        | awk 'NF && $1 != "N/A" { if (prev != "" && $1 + 0 <= prev + 0) bad=1; prev=$1 } END { exit bad ? 1 : 0 }'
    then
        check_true "[$label] 视频包 dts 单调不减"
    else
        check_false "[$label] 视频包 dts 单调不减" "存在回退/重复/缺失"
    fi

    # ⑤ PSI：每一份产物都必须带 PAT(0x0000) 与 PMT(0x1000)，否则"能播"只是运气
    if python3 - "$out" > "$OUTDIR/$label.psi.txt" 2>&1 << 'PY'
import sys
data = open(sys.argv[1], 'rb').read()
pids = set()
for off in range(0, len(data) - 188 + 1, 188):
    pids.add(((data[off + 1] & 0x1F) << 8) | data[off + 2])
need = {0x0000, 0x1000}
missing = need - pids
if missing:
    sys.exit("缺 PID %s（实际有 %s）" % (sorted(missing), sorted(pids)))
print("PID: %s" % sorted(pids))
PY
    then
        check_true "[$label] 含 PAT + PMT（$(cat "$OUTDIR/$label.psi.txt")）"
    else
        check_false "[$label] 含 PAT + PMT" "$(cat "$OUTDIR/$label.psi.txt")"
    fi
done

echo
echo "结果：通过 $pass 项 / 失败 $fail 项（产物：$OUTDIR）"
[ "$fail" -eq 0 ] || exit 1
echo "结论：M9-a 通过（TS 封装可被独立解码器完整识别与解码；三种输入形状都覆盖）"
