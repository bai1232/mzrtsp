# 音视频基础（本项目需要的 20%）

> **这份文档只讲"够用"的部分**：能看懂本项目的设计、能判断 AI 有没有说错、面试能讲清楚。
> 每条概念都配一条**你能自己跑**的命令；下面贴的输出都是在**本机真跑出来的**（不是抄的）。
>
> 学习方式建议：不要通读，按顺序**跑一遍命令 + 回答每节末尾的问题**，约 40 分钟。
>
> 先准备样本：`./scripts/make_samples.sh`（用本机 ffmpeg 现场生成，不入库）

## 1. 五个概念（看懂这五个，M4/M5/M6 的设计就都通了）

| 概念 | 一句话 | 面试常问 |
|---|---|---|
| **容器 vs 编码** | MP4/TS/MKV 是**盒子**；H264/AAC 是**里面的东西** | "remux 和 transcode 的区别？" |
| **I/P/B 帧** | I 帧（关键帧）能独立解；P 帧参考前面；**B 帧参考前后两边** | "为什么需要关键帧？丢帧策略为什么保关键帧？" |
| **dts / pts** | dts=**送进解码器**的时间；pts=**显示**的时间 | "为什么有两个时间戳？" |
| **time_base** | 时间戳不是秒，是"多少个 tick"；tick 多大就是 time_base | "时间基换算为什么容易错？" |
| **SPS/PPS** | H264 的**参数集**（分辨率等）；解码器没有它一帧都解不了 | "FLV 为什么第一帧要带 sequence header？" |

## 2. 动手：七条命令

### ① 看流参数（时基 + 帧率）
```bash
ffprobe -v error -show_entries stream=index,codec_name,width,height,time_base,avg_frame_rate \
        -of csv samples/sample.mp4
```
本机输出：
```
stream,0,h264,320,240,25/1,1/12800     ← 视频：320x240，帧率 25/1，**时间基 1/12800**
stream,1,aac,0/0,1/44100               ← 音频：时间基 1/44100（= 采样率）
```

### ② 用自己的算盘验证"时间基"（这一步最能建立直觉）
```bash
ffprobe -v error -select_streams v -show_entries packet=pts,dts -of csv samples/sample.mp4 | head -5
```
本机输出：
```
packet,0,0
packet,512,512
packet,1024,1024
packet,1536,1536
```
相邻两个包的差是 **512 tick**；时间基是 **1/12800 秒/tick** → 512 × (1/12800) = **0.04 秒 = 1/25 秒** ✓ 正好是帧率 25fps 的倒数。
> **这就是"时间基"的全部含义**：同样写 `512`，在不同流里可能是 4ms 也可能是 40ms —— 所以跨容器必须换算（本项目 `src/ffmpeg/time_base.h`）。

### ③ 这个样本没有 B 帧，所以 pts == dts
上面 ② 里 `pts` 和 `dts` **每一行都相等** —— 因为 `testsrc` + `-preset ultrafast` 默认没有 B 帧。
**没有 B 帧时两个顺序重合**，你也就看不出区别；所以下一条专门造一个带 B 帧的：

### ④ 造一个带 B 帧的样本，看"两个顺序"
```bash
ffmpeg -y -f lavfi -i "testsrc=size=320x240:rate=25" -t 1 -bf 2 -pix_fmt yuv420p \
       -c:v libx264 -preset ultrafast samples/bf.mp4
ffprobe -v error -select_streams v -show_entries packet=pts,dts -of csv samples/bf.mp4 | head -6
ffprobe -v error -select_streams v -show_entries frame=pict_type -of csv samples/bf.mp4 | head -6
```
本机输出：
```
packet,0,-1024        ← pts=0    dts=-1024   （dts 是负数：B 帧的重排延迟）
packet,1536,-512
packet,512,0
packet,1024,512
packet,3072,1024
packet,2048,1536
帧类型：I B B P B B ...
```
看出来了吗：
- **dts 严格递增**（-1024 → -512 → 0 → 512 → 1024 → …），每步 512 tick；
- **pts 是乱的**（0, 1536, 512, 1024, 3072, 2048 …）——因为 B 帧要等后面的参考帧；
- 封装时**按 dts 排列**，所以"**dts 必须单调不减**"是硬要求（本项目 `MonotonicGuard` 就是守这个；回退会被播放器当成跳帧）。

### ⑤ 看 H264 裸流的"起始码"和 NAL 类型
```bash
xxd -l 96 samples/sample.h264
```
本机输出（开头）：
```
00000000: 0000 0001 0605 ffff 50dc 45e9 ...   x264 - core 163 ...
```
- `00 00 00 01` 是**起始码**（另一种是 `00 00 01`），后面跟一个 NAL；
- 紧跟的字节 `06` → NAL 类型 = `0x06 & 0x1F` = **6 = SEI**（编码器写进去的元数据，里面是 "x264 - core 163"）
- 所以**第一个 NAL 不是 SPS** —— SPS(type=7, 字节 `67`)/PPS(type=8, `68`) 在后面
> 本项目 `src/ffmpeg/h264_util.cpp` 干的就是：按起始码切分、把 type=7/8 挑出来（去掉起始码）。它只切分，**不做** emulation prevention 反转义（那是解码器的事）。

### ⑥ 确认 SPS/PPS 真的被提取出来了（对着代码的输出看）
跑测试即可（用例会打印提取到的字节数）：
```bash
./build/bin/mzmedia_unittest ffmpeg_h264_extract_from_raw_sample
```
本机输出里有：`裸流提取：SPS 22 字节 / PPS 4 字节`，并且用例断言 `sps[0] & 0x1F == 7`、`pps[0] & 0x1F == 8`。

### ⑦ 看 SPS 里面的实际字段（★ 最能建立"我真的懂"的一条）
```bash
ffmpeg -hide_banner -i samples/sample.mp4 -c:v copy -bsf:v trace_headers -f null - 2>&1 \
  | grep -E 'profile_idc|pic_width_in_mbs_minus1|pic_height_in_map_units_minus1|frame_mbs_only_flag'
```
本机输出：
```
profile_idc                         = 66        （Baseline profile）
pic_width_in_mbs_minus1             = 19
pic_height_in_map_units_minus1      = 14
frame_mbs_only_flag                 = 1
```
自己算一遍：
- 宽 = (19 + 1) × 16 = **320** ✓
- 高 = (14 + 1) × 16 × 1 = **240** ✓（`frame_mbs_only_flag=1` 表示不隔行，所以不乘 2）

**这就是 `h264_util.cpp` 里那段解析在做的事**（外加处理 `frame_cropping`：分辨率不是 16 的整数倍时要减掉裁剪量）。

> 注意：`trace_headers` 用 **info 级**日志，所以**不能**加 `-v error`（加了就什么都看不到）。

## 3. 这些概念在本项目里的落点

| 概念 | 代码位置 | 我们为此做的设计 |
|---|---|---|
| 容器 vs 编码 | `src/ffmpeg/codec_matrix.cpp` | H264+AAC→FLV = Remux；**HEVC → Unsupported**（浏览器播不了，假 remux 的结果是一片黑） |
| I/P/B 帧、dts 单调 | `src/ffmpeg/time_base.h`（`MonotonicGuard`） | 回退钳制 + **计数**（不静默改数据） |
| time_base | `src/ffmpeg/time_base.h`（`rescaleTimestamp`） | 整数换算 + `__int128` 溢出判定（失败返回 false，不截断）；FLV 的 tag 时间戳用 **dts**，另带 `CompositionTime = pts - dts` |
| SPS/PPS 三种形式 | `src/ffmpeg/h264_util.cpp` | 裸流是 Annex-B（起始码）；MP4 是 `avcC`（长度前缀）；**FLV 第一帧要带 sequence header** → M6 要组装 |
| 参数集/分辨率 | `Demuxer` 的 `StreamInfo` + `h264_util` | 两条独立路径**交叉校验**（自解 SPS vs FFmpeg），互相印证 |

## 4. 自检五问（能答出来就够开 M5）

1. 为什么一个流里会有 **两个** 时间戳？哪个必须单调？为什么？
2. `time_base = 1/90000`，某个包时间戳 `180000`，它是第几秒？
3. 为什么 `remux` 能在"不解码"的情况下工作？什么情况下必须转码？
4. SPS/PPS 在 MP4 里放哪？在裸流里长什么样？FLV 里为什么必须单独发一次？
5. 我们的 `MonotonicGuard` 在防什么？如果不做会怎样？

（想检验答案可以来问我，或者直接拿这些命令去验证 —— 这是最好的自检方式。）

## 5. 明确**不用**懂的部分（免得你陷进去）

- H264 的熵编码（CABAC/CAVLC）、变换/量化、运动补偿、参考帧管理 —— 那是**解码器**的事，我们只"搬运"（remux）不解码
- SPS 里每个字段的完整语义（scaling list、VUI 时序信息…）—— 我们只按规范顺序跳过/取需要的字段
- AAC 的帧结构、量化 —— M4/M6 只需要把 AAC 帧搬进 FLV 的 audio tag
- exp-Golomb 位编码的数学细节 —— 代码里有实现，你只要知道"它把变长整数按位存"

> 一句话：**你要能讲清"为什么"，不用能写出"每一位"**。后者我负责，并且在契约里写明我保证到哪一层
> （例如 `h264_util.h` 明确写：SPS 无校验和，解析器只承诺语法解析，**调用方必须交叉校验**）。
