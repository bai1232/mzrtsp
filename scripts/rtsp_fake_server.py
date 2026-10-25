#!/usr/bin/env python3
"""最小 RTSP 服务端（v0.3-a 的**验收工具**：零第三方依赖，只用 python3 标准库）

它能证明什么：
  · 我们的 Demuxer/服务端能按 RTSP 协议连上、DESCRIBE/SETUP/PLAY 走完、拿到可解码的 H264；
  · 断流注入：`--after close`（对端消失）/ `--after silent`（半开，只有读超时能发现）；
  · 传输方式：`--tcp-only`（对 UDP SETUP 回 461，验证 ffmpeg 的自动回退）；
  · 缺参数集：`--no-sprop`（SDP 不带 sprop-parameter-sets，验证"必须明确拒绝"）。
**不能**证明什么：真实摄像机的时间戳/乱序/B 帧行为、UDP 丢包、鉴权（RTSP Basic/Digest）——
  这些要真机验收，见 docs/DESIGN_M11.md §7。

做什么：
  · 说够用的 RTSP：OPTIONS / DESCRIBE / SETUP / PLAY / TEARDOWN
  · SDP 里带 H264 + sprop-parameter-sets（故意做成可关，用来验"没有 SPS/PPS 会怎样"）
  · RTP over TCP（interleaved，`$` 帧），H264 单 NAL / FU-A 分片，25fps → 90kHz 时间戳
  · `--loop`：发完一轮从头再来（时间戳继续往前，关键帧因此周期性出现 —— HLS 切片需要它）
  · 播完 N 帧后按 --after 选择行为：
      close  = 直接关连接（对端收到 RST / EOF）
      silent = **保持连接但不发数据**（模拟"半开"：只靠读超时才能发现）
      hold   = 继续空转（保持连接，不发也不关）
  · `--audio-file <file.aac>`：再开一条 **AAC 音频轨**（RFC 3640 / AAC-hbr，SDP 带 `config=`），
    用来验 RTSP 的**音视频**路径（真摄像机基本都有声）；`--fps` 按素材真实帧率给
  · `--fault-close-first`：故障只发生在**第一条**连接，之后一直推流（模拟相机重启后恢复）——
    没有它，"每轮 25 帧就断"永远测不出自愈，只能测出"反复断"

用法：
  python3 rtsp_fake_server.py --port 8554 --file samples/sample.h264 --frames 100 --after silent
  # 自愈用例（M11-a2）：第一条连接 25 帧后断，之后一直推流
  python3 rtsp_fake_server.py --port 8554 --file samples/sample.h264 --frames 25 --after close --fault-close-first
"""
import argparse
import base64
import re
import socket
import struct
import sys
import threading
import time

SSRC = 0x12345678


def split_annexb(data):
    """按 00 00 01 / 00 00 00 01 切 Annex-B → [nal_bytes]

    **为什么用 re 而不是逐字节扫**：45MB 的真实素材逐字节扫要 20–40 s（纯 Python 循环），
    一小时的验收工具不能有这种启动开销；`re.finditer` 在 C 层扫，毫秒级。
    语义与逐字节版本等价：`00 00 00 01` 里也含 `00 00 01`，从偏移 +1 处命中，起点一致。
    """
    starts = [m.start() + 3 for m in re.finditer(b"\x00\x00\x01", data)]
    nals = []
    n = len(data)
    for idx, s in enumerate(starts):
        e = starts[idx + 1] - 3 if idx + 1 < len(starts) else n
        # 回退掉下一个起始码里的 0
        while e > s and data[e - 1] == 0:
            e -= 1
        if e > s:
            nal = data[s:e]
            if nal:
                nals.append(nal)
    return nals


class Rtp:
    def __init__(self, mt=96, max_payload=1400):
        self.seq = 0
        self.mt = mt
        self.max_payload = max_payload

    def packet(self, nal, ts, marker):
        """把一个 NAL 打成 1..N 个 RTP 包（返回 bytes 列表）"""
        out = []
        # TCP 交错传输没有 MTU 限制：`--no-fu-a` 时整个 NAL 一个 RTP 包
        # （便于排查"首帧时间戳异常是不是分片路径引起的"）
        max_payload = self.max_payload
        if len(nal) <= max_payload:
            payload = nal
            out.append(self._mk(ts, marker, payload))
            return out
        # FU-A 分片
        nal_type = nal[0] & 0x1F
        indicator = (nal[0] & 0xE0) | 28  # FU-A
        body = nal[1:]
        off = 0
        while off < len(body):
            chunk = body[off:off + max_payload - 2]
            off += len(chunk)
            start = (off == len(chunk))
            end = (off >= len(body))
            fu_header = (0x80 if start else 0) | (0x40 if end else 0) | nal_type
            out.append(self._mk(ts, marker and end, bytes([indicator, fu_header]) + chunk))
        return out

    def _mk(self, ts, marker, payload):
        self.seq = (self.seq + 1) & 0xFFFF
        b0 = 0x80
        b1 = (0x80 if marker else 0) | self.mt
        header = struct.pack("!BBHII", b0, b1, self.seq, ts & 0xFFFFFFFF, SSRC)
        return header + payload


class Server:
    def __init__(self, args):
        self.args = args
        data = open(args.file, "rb").read()
        nals = split_annexb(data)
        sps = next((n for n in nals if (n[0] & 0x1F) == 7), b"")
        pps = next((n for n in nals if (n[0] & 0x1F) == 8), b"")
        self.sdp_sps = base64.b64encode(sps).decode()
        self.sdp_pps = base64.b64encode(pps).decode()
        # 只保留视频 VCL 帧（1/5）：这里按 NAL 逐帧发
        self.frames = [n for n in nals if (n[0] & 0x1F) in (1, 5)]
        self.fps = float(args.fps)
        # 音频轨（可选）：`--audio-file` 给一个 ADTS(.aac) → 裸 AAC 帧 + ASC（SDP 的 config=）
        self.audio_frames = []
        self.audio_asc = b""
        self.audio_rate = 44100
        self.audio_channels = 2
        if args.audio_file:
            self.load_adts(args.audio_file)
        self.conn_seq = 0  # 第几条连接（1 起）：`--fault-close-first` 靠它只坑第一条
        self.log(f"就绪：SPS {len(sps)}B / PPS {len(pps)}B，可发 {len(self.frames)} 帧 @ {self.fps:g}fps")
        if self.audio_frames:
            self.log(f"音频轨：{len(self.audio_frames)} 帧 AAC，{self.audio_rate}Hz/{self.audio_channels}ch，"
                     f"ASC={self.audio_asc.hex()}")

    def log(self, msg):
        print(f"[srv] {msg}", flush=True)

    def load_adts(self, path):
        """把 ADTS 拆成裸 AAC 帧（RFC 3640 的 payload 里不要 ADTS 头），并算出 ASC"""
        data = open(path, "rb").read()
        pos = 0
        first = True
        sfi = 4
        chan = 2
        profile = 1
        while pos + 7 <= len(data):
            h = data[pos:pos + 7]
            if h[0] != 0xFF or (h[1] & 0xF0) != 0xF0:
                break  # 不是 ADTS 头：停止（不猜）
            protection_absent = h[1] & 0x01
            profile = (h[2] >> 6) & 0x03
            sfi = (h[2] >> 2) & 0x0F
            chan = ((h[2] & 0x01) << 2) | ((h[3] >> 6) & 0x03)
            frame_len = ((h[3] & 0x03) << 11) | (h[4] << 3) | ((h[5] >> 5) & 0x07)
            header_len = 7 if protection_absent else 9
            if frame_len <= header_len:
                break
            self.audio_frames.append(data[pos + header_len:pos + frame_len])
            pos += frame_len
            if first:
                first = False
        if not self.audio_frames:
            raise SystemExit(f"读不出 ADTS 帧：{path}")
        rates = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000]
        self.audio_rate = rates[sfi] if sfi < len(rates) else 44100
        self.audio_channels = chan if chan else 2
        # AudioSpecificConfig：5bit object type（profile+1）| 4bit 采样率索引 | 4bit 声道
        obj = profile + 1
        asc = (obj << 11) | (sfi << 7) | (self.audio_channels << 3)
        self.audio_asc = bytes([(asc >> 8) & 0xFF, asc & 0xFF])

    def sdp(self, with_params):
        fmtp = "packetization-mode=1"
        if with_params:
            fmtp += f";sprop-parameter-sets={self.sdp_sps},{self.sdp_pps}"
        sdp = (
            "v=0\r\n"
            "o=- 0 0 IN IP4 127.0.0.1\r\n"
            "s=mzmedia-test\r\n"
            "t=0 0\r\n"
            "a=control:*\r\n"
            "m=video 0 RTP/AVP 96\r\n"
            "a=rtpmap:96 H264/90000\r\n"
            f"a=fmtp:96 {fmtp}\r\n"
            "a=control:trackID=0\r\n"
        )
        if self.audio_frames:
            sdp += (
                "m=audio 0 RTP/AVP 97\r\n"
                f"a=rtpmap:97 MPEG4-GENERIC/{self.audio_rate}/{self.audio_channels}\r\n"
                "a=fmtp:97 profile-level-id=1;mode=AAC-hbr;sizelength=13;"
                f"indexlength=3;indexdeltalength=3;config={self.audio_asc.hex()}\r\n"
                "a=control:trackID=1\r\n"
            )
        return sdp

    def send_packet(self, conn, state, track, pkt):
        """把一帧 RTP 发出去：UDP 走该轨自己的端口，TCP 交错走该轨的通道"""
        tr = state["transports"].get(track)
        if tr is None:
            return True
        if tr["kind"] == "udp":
            try:
                tr["sock"].sendto(pkt, (tr["peer"][0], tr["cport"]))
            except OSError as e:
                self.log(f"UDP 写失败（对端走了？）：{e}")
                return False
        else:
            frame = b"$" + bytes([tr["rtp_ch"]]) + struct.pack("!H", len(pkt)) + pkt
            try:
                with state["write_lock"]:
                    conn.sendall(frame)
            except OSError as e:
                self.log(f"TCP 写失败（对端走了？）：{e}")
                return False
        return True

    def handle(self, conn):
        # 【每连接一份】轨传输表 + 停止标志：共享会串台（旧连接的线程把新连接搅乱）
        state = {"transports": {}, "stop": threading.Event(),
                 "write_lock": threading.Lock()}
        buf = b""
        session = "mz-test-session;timeout=60"
        while True:
            try:
                chunk = conn.recv(4096)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            while b"\r\n\r\n" not in buf:
                chunk = conn.recv(4096)
                if not chunk:
                    return
                buf += chunk
            head, buf = buf.split(b"\r\n\r\n", 1)
            lines = head.decode("utf-8", "replace").split("\r\n")
            method, uri, _ = (lines[0].split(" ") + ["", "", ""])[:3]
            cseq = "1"
            transport = ""
            for ln in lines[1:]:
                low = ln.lower()
                if low.startswith("cseq:"):
                    cseq = ln.split(":", 1)[1].strip()
                elif low.startswith("transport:"):
                    transport = ln.split(":", 1)[1].strip()
            self.log(f"<- {method} {uri} (CSeq {cseq})")
            peer = conn.getpeername()
            if method == "OPTIONS":
                resp = "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n\r\n"
            elif method == "DESCRIBE":
                body = self.sdp(not self.args.no_sprop).encode()
                resp = (f"Content-Type: application/sdp\r\n"
                        f"Content-Length: {len(body)}\r\n\r\n").encode() + body
            elif method == "SETUP":
                import re as _re
                m = _re.search(r"trackID=(\d+)", uri)
                track = int(m.group(1)) if m else 0
                self.log(f"   客户端 Transport：{transport}（trackID={track}）")
                if "TCP" in transport.upper():
                    ch = 0
                    mc = _re.search(r"interleaved=(\d+)-(\d+)", transport)
                    if mc:
                        ch = int(mc.group(1))
                    state["transports"][track] = {"kind": "tcp", "rtp_ch": ch, "rtcp_ch": ch + 1}
                    resp = (f"Transport: RTP/AVP/TCP;unicast;interleaved={ch}-{ch + 1};"
                            f"ssrc={SSRC:08X}\r\n"
                            f"Session: {session}\r\n\r\n")
                elif self.args.tcp_only:
                    self.log("   拒绝 UDP SETUP（tcp_only）→ 461 Unsupported Transport")
                    conn.sendall(b"RTSP/1.0 461 Unsupported Transport\r\nCSeq: " +
                                 cseq.encode() + b"\r\n\r\n")
                    continue
                else:
                    mc = _re.search(r"client_port=(\d+)-(\d+)", transport)
                    cport = int(mc.group(1)) if mc else 5000
                    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                    sock.bind(("127.0.0.1", 0))
                    sport = sock.getsockname()[1]
                    state["transports"][track] = {"kind": "udp", "sock": sock, "cport": cport,
                                              "peer": peer}
                    resp = (f"Transport: RTP/AVP;unicast;client_port={cport}-{cport + 1};"
                            f"server_port={sport}-{sport + 1};ssrc={SSRC:08X}\r\n"
                            f"Session: {session}\r\n\r\n")
            elif method == "PLAY":
                resp = (f"Session: {session}\r\n"
                        "Range: npt=0.000-\r\n\r\n")
                state["stop"].clear()
                threading.Thread(target=self.stream, args=(conn, state), daemon=True).start()
                if self.audio_frames and 1 in state["transports"]:
                    threading.Thread(target=self.stream_audio, args=(conn, state),
                                     daemon=True).start()
            else:
                resp = "\r\n"
            out = f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\n".encode() + \
                  (resp if isinstance(resp, bytes) else resp.encode())
            self.log(f"-> {out[:120]!r}")
            conn.sendall(out)

    def stream(self, conn, state):
        rtp = Rtp(96, self.args.max_payload)
        ts_step = int(90000 / self.fps)
        self.conn_seq += 1
        first = self.conn_seq == 1
        # `--fault-close-first`：只让**第一条**连接出故障（模拟相机重启/网络抖动），
        # 后续连接一直推流。否则每轮 25 帧就被断掉 —— 那样任何客户端都不可能持续出数据，
        # 测的就不是"自愈"，而是"反复断"。
        fault = self.args.after if (first or not self.args.fault_close_first) else "hold"
        loop = self.args.loop or not first
        limit = self.args.frames if first else 0  # 0 = 不限轮数，一直发
        n = min(self.args.frames, len(self.frames)) if first else len(self.frames)
        if getattr(self.args, "delay_ms", 0):
            time.sleep(self.args.delay_ms / 1000.0)
        t0 = time.time()
        sent = 0
        round_index = 0
        frame_index = 0
        # --loop：一轮发完从头再来（时间戳继续走；首个 NAL 是 IDR → 周期性关键帧）
        while True:
            if frame_index >= n:
                if not loop:
                    break
                frame_index = 0
                round_index += 1
            nal = self.frames[frame_index]
            i = round_index * n + frame_index
            frame_index += 1
            ts = (i * ts_step) & 0xFFFFFFFF
            if getattr(self.args, "trace_ts", False) and sent < 8:
                self.log(f"   发#{sent} i={i} ts={ts} bytes={len(nal)}")
            for pkt in rtp.packet(nal, ts, True):
                if not self.send_packet(conn, state, 0, pkt):
                    return
            target = t0 + (i + 1) / self.fps
            time.sleep(max(0.0, target - time.time()))
            sent += 1
            if loop and limit and sent >= limit:
                break
        state["stop"].set()  # 视频完了（或断了）：音频轨也停，别继续往死连接写
        self.log(f"已发 {sent} 帧（{round_index + 1} 轮）；after={fault}")
        if fault == "close":
            try:
                conn.close()
            except OSError:
                pass
        elif fault == "silent":
            # 半开：连接还在，但一个字节都不再发（只有读超时/保活能发现）
            time.sleep(600)
        else:
            time.sleep(600)

    def stream_audio(self, conn, state):
        """音频轨：AAC-hbr（RFC 3640），1024 样本一帧，按采样率实时发"""
        rtp = Rtp(97, self.args.max_payload)
        step = 1024
        t0 = time.time()
        i = 0
        n = len(self.audio_frames)
        while not state["stop"].is_set():
            raw = self.audio_frames[i % n]
            # AU-headers-length=16bit，AU-size=13bit，AU-index=3bit
            au = struct.pack("!HH", 0x0010, ((len(raw) << 3) & 0xFFF8))
            ts = (i * step) & 0xFFFFFFFF
            for pkt in rtp.packet(au + raw, ts, True):
                if not self.send_packet(conn, state, 1, pkt):
                    return
            i += 1
            if getattr(self.args, "trace_ts", False) and i % 100 == 0:
                self.log(f"   音频已发 {i} 帧（ts={ts}）")
            target = t0 + (i * step) / self.audio_rate
            time.sleep(max(0.0, target - time.time()))
        self.log(f"音频轨已停（共发 {i} 帧）")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8554)
    ap.add_argument("--file", default="samples/sample.h264")
    ap.add_argument("--frames", type=int, default=100)
    ap.add_argument("--after", choices=["close", "silent", "hold"], default="hold")
    ap.add_argument("--no-sprop", action="store_true", help="SDP 里**不带** sprop-parameter-sets")
    ap.add_argument("--tcp-only", action="store_true", help="只接受 TCP 交错传输（UDP SETUP → 461）")
    ap.add_argument("--max-payload", type=int, default=1400,
                    help="单个 RTP 包的负载上限（>= 帧大小即不分片；TCP 交错下可调大）")
    ap.add_argument("--fps", type=float, default=25.0,
                    help="视频轨帧率（时间戳步长 = 90000/fps；真实素材按 ffprobe 的 r_frame_rate 给）")
    ap.add_argument("--audio-file", default="",
                    help="可选：再发一条 AAC 音频轨（给一个 ADTS .aac 文件）→ 验 RTSP 音视频")
    ap.add_argument("--loop", action="store_true",
                    help="发完一轮从头再来（时间戳继续；关键帧周期性出现，供 HLS 切片）")
    ap.add_argument("--delay-ms", type=int, default=0, help="PLAY 后延迟多少毫秒才开始发帧（排查用）")
    ap.add_argument("--trace-ts", action="store_true", help="打印前 8 帧的 RTP 时间戳（排查用）")
    ap.add_argument("--fault-close-first", action="store_true",
                    help="故障只发生在第一条连接，之后一直推流（测自愈需要它）")
    args = ap.parse_args()

    srv = Server(args)
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", args.port))
    s.listen(1)
    srv.log(f"监听 127.0.0.1:{args.port}")
    while True:
        conn, _ = s.accept()
        srv.log("客户端接入")
        threading.Thread(target=srv.handle, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    main()
