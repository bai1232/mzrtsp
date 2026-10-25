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

用法：
  python3 rtsp_fake_server.py --port 8554 --file samples/sample.h264 --frames 100 --after silent
"""
import argparse
import base64
import socket
import struct
import sys
import threading
import time

SSRC = 0x12345678


def split_annexb(data):
    """按 00 00 01 / 00 00 00 01 切 Annex-B → [(nal_bytes, is_key)]"""
    nals = []
    i = 0
    n = len(data)
    starts = []
    while i < n - 3:
        if data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 1:
            starts.append(i + 3)
            i += 3
        elif i < n - 4 and data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 0 and data[i + 3] == 1:
            starts.append(i + 4)
            i += 4
        else:
            i += 1
    for idx, s in enumerate(starts):
        e = starts[idx + 1] - 3 if idx + 1 < len(starts) else n
        # 回退掉下一个起始码里的 0
        while e > s and data[e - 1] == 0:
            e -= 1
        if e > s:
            nal = data[s:e]
            # 去掉尾部可能残留的 0
            while nal and nal[-1] == 0:
                nal = nal[:-1]
            if nal:
                nals.append(nal)
    return nals


class Rtp:
    def __init__(self, mt=96):
        self.seq = 0
        self.mt = mt

    def packet(self, nal, ts, marker):
        """把一个 NAL 打成 1..N 个 RTP 包（返回 bytes 列表）"""
        out = []
        max_payload = 1400
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
        # 只保留视频 VCL 帧（1/5），并按"多少帧一包"分组：这里简单按 NAL 逐帧发
        self.frames = [n for n in nals if (n[0] & 0x1F) in (1, 5)]
        self.transport = "tcp"
        self.peer = None
        self.udp = None
        self.log(f"就绪：SPS {len(sps)}B / PPS {len(pps)}B，可发 {len(self.frames)} 帧")

    def log(self, msg):
        print(f"[srv] {msg}", flush=True)

    def sdp(self, with_params):
        fmtp = "packetization-mode=1"
        if with_params:
            fmtp += f";sprop-parameter-sets={self.sdp_sps},{self.sdp_pps}"
        return (
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

    def handle(self, conn):
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
            for ln in lines[1:]:
                if ln.lower().startswith("cseq:"):
                    cseq = ln.split(":", 1)[1].strip()
            self.log(f"<- {method} {uri} (CSeq {cseq})")
            self.peer = conn.getpeername()
            if method == "OPTIONS":
                resp = "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n\r\n"
            elif method == "DESCRIBE":
                sdp = self.sdp(not self.args.no_sprop)
                body = sdp.encode()
                resp = (f"Content-Type: application/sdp\r\n"
                        f"Content-Length: {len(body)}\r\n\r\n").encode() + body
            elif method == "SETUP":
                transport = ""
                for ln in lines[1:]:
                    if ln.lower().startswith("transport:"):
                        transport = ln.split(":", 1)[1].strip()
                self.log(f"   客户端 Transport: {transport}")
                if "TCP" in transport.upper():
                    self.transport = "tcp"
                    resp = (f"Transport: RTP/AVP/TCP;unicast;interleaved=0-1;ssrc={SSRC:08X}\r\n"
                            f"Session: {session}\r\n\r\n")
                elif self.args.tcp_only:
                    # 模拟"只接受 TCP 的摄像机 / UDP 被防火墙挡住"
                    self.log("   拒绝 UDP SETUP（tcp_only）→ 461 Unsupported Transport")
                    conn.sendall(b"RTSP/1.0 461 Unsupported Transport\r\nCSeq: " +
                                 cseq.encode() + b"\r\n\r\n")
                    continue
                else:
                    # UDP：从客户端的 client_port=PPPP-QQQQ 里取端口
                    import re as _re
                    m = _re.search(r"client_port=(\d+)-(\d+)", transport)
                    cport = int(m.group(1)) if m else 5000
                    self.transport = ("udp", cport, self.peer)
                    self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                    self.udp.bind(("127.0.0.1", 0))
                    sport = self.udp.getsockname()[1]
                    resp = (f"Transport: RTP/AVP;unicast;client_port={cport}-{cport + 1};"
                            f"server_port={sport}-{sport + 1};ssrc={SSRC:08X}\r\n"
                            f"Session: {session}\r\n\r\n")
            elif method == "PLAY":
                resp = (f"Session: {session}\r\n"
                        "Range: npt=0.000-\r\n\r\n")
            else:
                resp = ""
            status = "200 OK" if method != "TEARDOWN" else "200 OK"
            head_out = f"RTSP/1.0 {status}\r\nCSeq: {cseq}\r\n".encode()
            out = head_out + (resp if isinstance(resp, bytes) else resp.encode())
            self.log(f"-> {out[:120]!r}")
            conn.sendall(out)
            if method == "PLAY":
                threading.Thread(target=self.stream, args=(conn,), daemon=True).start()

    def stream(self, conn):
        rtp = Rtp()
        ts_step = 90000 // 25
        n = min(self.args.frames, len(self.frames))
        t0 = time.time()
        sent = 0
        round_index = 0
        frame_index = 0
        # --loop：一轮发完从头再来（时间戳继续走；首个 NAL 是 IDR → 周期性关键帧）
        while True:
            if frame_index >= n:
                if not self.args.loop:
                    break
                frame_index = 0
                round_index += 1
            nal = self.frames[frame_index]
            i = round_index * n + frame_index
            frame_index += 1
            ts = (i * ts_step) & 0xFFFFFFFF
            is_last = True  # 一个 NAL 视为一帧（样本是 x264 逐帧单 slice）
            try:
                for pkt in rtp.packet(nal, ts, is_last):
                    if isinstance(self.transport, tuple):
                        _, cport, peer = self.transport
                        self.udp.sendto(pkt, (peer[0], cport))
                    else:
                        conn.sendall(b"$" + bytes([0]) + struct.pack("!H", len(pkt)) + pkt)
            except OSError as e:
                self.log(f"写失败（对端走了？）：{e}")
                return
            target = t0 + (i + 1) / 25.0
            time.sleep(max(0.0, target - time.time()))
            sent += 1
            if self.args.loop and sent >= self.args.frames:
                break
        self.log(f"已发 {sent} 帧（{round_index + 1} 轮）；after={self.args.after}")
        if self.args.after == "close":
            try:
                conn.close()
            except OSError:
                pass
        elif self.args.after == "silent":
            # 半开：连接还在，但一个字节都不再发（只有读超时/保活能发现）
            time.sleep(600)
        else:
            time.sleep(600)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8554)
    ap.add_argument("--file", default="samples/sample.h264")
    ap.add_argument("--frames", type=int, default=100)
    ap.add_argument("--after", choices=["close", "silent", "hold"], default="hold")
    ap.add_argument("--no-sprop", action="store_true", help="SDP 里**不带** sprop-parameter-sets")
    ap.add_argument("--tcp-only", action="store_true", help="只接受 TCP 交错传输（UDP SETUP → 461）")
    ap.add_argument("--loop", action="store_true",
                    help="发完一轮从头再来（时间戳继续；关键帧周期性出现，供 HLS 切片）")
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
