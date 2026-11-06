#include "output/rtsp_server.h"

#include "core/logger.h"
#include "core/util.h"
#include "ffmpeg/h264_util.h"
#include "media/media_source.h"
#include "media/subscriber_quota.h"
#include "network/socket.h"
#include "network/tcp_server.h"
#include "output/rtp_packetizer.h"
#include "output/rtsp_request.h"
#include "output/sdp_builder.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>

#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace mzmedia {

namespace {

std::string randomSessionId() {
    static std::atomic<uint32_t> counter{1};
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%08X%04X", static_cast<unsigned>(getCurrentMillisecond() & 0xFFFFFFFFu),
                  static_cast<unsigned>(counter.fetch_add(1) & 0xFFFF));
    return std::string(buf);
}

/// 本机出口 IPv4：优先非 loopback 的可用于 `c=` 的地址（探测失败退回 127.0.0.1）
std::string detectLocalIpv4() {
    struct ifaddrs *list = nullptr;
    if (::getifaddrs(&list) != 0 || list == nullptr) {
        return "127.0.0.1";
    }
    std::string fallback;
    for (struct ifaddrs *it = list; it != nullptr; it = it->ifa_next) {
        if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        char text[INET_ADDRSTRLEN] = {0};
        const auto *addr = reinterpret_cast<const struct sockaddr_in *>(it->ifa_addr);
        if (::inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text)) == nullptr) {
            continue;
        }
        const std::string ip(text);
        if (ip == "127.0.0.1") {
            fallback = ip;
            continue;
        }
        if (ip.rfind("169.254.", 0) == 0) {
            continue; // 链路本地地址不能给客户端用
        }
        ::freeifaddrs(list);
        return ip;
    }
    ::freeifaddrs(list);
    return fallback.empty() ? "127.0.0.1" : fallback;
}

std::string escapeJson(const std::string &in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c & 0xFF));
                out += buf;
            } else {
                out += c;
            }
            break;
        }
    }
    return out;
}

std::string interleavedFrame(int channel, const std::string &rtp) {
    std::string out;
    out.reserve(4 + rtp.size());
    out.push_back('$');
    out.push_back(static_cast<char>(channel & 0xFF));
    out.push_back(static_cast<char>((rtp.size() >> 8) & 0xFF));
    out.push_back(static_cast<char>(rtp.size() & 0xFF));
    out.append(rtp);
    return out;
}

} // namespace

// ============================================================================
// RtspSession：每连接一份（协议分帧 + 状态机 + 发送）
// ============================================================================
class RtspSession : public Session {
public:
    RtspSession(const Socket::Ptr &sock, const EventPoller::Ptr &poller, RtspServer *owner)
        : Session(sock, poller)
        , _owner(owner) {
        // RTSP 客户端会周期性发 OPTIONS/GET_PARAMETER 保活；空闲阈值给宽一点（60s 默认已够）
        (void) setRecvIdleTimeout(60000);
        _owner->onSessionOpen();
    }

    ~RtspSession() override {
        // 订阅者在成员析构时释放 → 名额/资源自动归还（Subscriber 析构里 release）
        _sub.reset();
        _source.reset();
        _owner->onSessionClose();
    }

protected:
    void onRecv(const Buffer::Ptr &buf) override {
        if (isShutdown()) {
            return;
        }
        if (buf == nullptr || buf->size() == 0) {
            return;
        }
        _buf.append(buf->data(), buf->size());
        if (_buf.size() > kMaxBuffer) {
            _owner->onRejected("请求头过大");
            (void) shutdown(SockException(SockException::ErrType::RecvOverflow, 0, "RTSP 请求过大"));
            return;
        }
        for (;;) {
            RtspRequest::Status status =
                _req.parse(_buf.data(), _buf.size());
            if (status == RtspRequest::Status::NeedMoreData) {
                return;
            }
            if (status == RtspRequest::Status::Error) {
                _owner->onRejected(_req.errorName());
                reply(400, _req.cseq(), "", "Bad request", "text/plain");
                (void) shutdownAfterFlush(500);
                return;
            }
            const bool keep = handleOne();
            _buf.erase(0, _req.consumed());
            if (!keep || isShutdown()) {
                return;
            }
            if (_buf.empty()) {
                return;
            }
        }
    }

    void onError(const SockException &err) override {
        Session::onError(err);
        _playing = false;
        _owner->onPlayingChanged(false);
    }

private:
    static constexpr size_t kMaxBuffer = 64u << 10;

    bool reply(int status, const std::string &cseq, const std::string &extra_headers = std::string(),
               const std::string &body = std::string(),
               const std::string &content_type = std::string()) {
        const char *reason = "OK";
        switch (status) {
        case 200: reason = "OK"; break;
        case 400: reason = "Bad Request"; break;
        case 404: reason = "Not Found"; break;
        case 405: reason = "Method Not Allowed"; break;
        case 453: reason = "Not Enough Bandwidth"; break;
        case 454: reason = "Session Not Found"; break;
        case 455: reason = "Method Not Valid in This State"; break;
        case 461: reason = "Unsupported Transport"; break;
        case 500: reason = "Internal Server Error"; break;
        case 501: reason = "Not Implemented"; break;
        case 503: reason = "Service Unavailable"; break;
        default: reason = "OK"; break;
        }
        std::string out;
        out += "RTSP/1.0 " + std::to_string(status) + " " + reason + "\r\n";
        if (!cseq.empty()) {
            out += "CSeq: " + cseq + "\r\n";
        }
        if (!_session_id.empty()) {
            out += "Session: " + _session_id + "\r\n";
        }
        out += "Server: mzmedia\r\n";
        if (!content_type.empty()) {
            out += "Content-Type: " + content_type + "\r\n";
        }
        out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        if (!extra_headers.empty()) {
            out += extra_headers;
            if (out.size() < 2 || out.compare(out.size() - 2, 2, "\r\n") != 0) {
                out += "\r\n";
            }
        }
        out += "\r\n";
        out += body;
        return send(out) >= 0;
    }

    /// @return false = 本次处理要做收尾（已关连接），调用方停止继续解析
    bool handleOne() {
        _owner->onRequest(_req.methodStr().c_str());
        if (_session_id.empty()) {
            _session_id = randomSessionId();
        }
        const std::string cseq = _req.cseq();
        switch (_req.method()) {
        case RtspRequest::Method::Options: {
            std::string public_hdr =
                "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER\r\n";
            return reply(200, cseq, public_hdr);
        }
        case RtspRequest::Method::Describe:
            return handleDescribe(cseq);
        case RtspRequest::Method::Setup:
            return handleSetup(cseq);
        case RtspRequest::Method::Play:
            return handlePlay(cseq);
        case RtspRequest::Method::Pause:
            _playing = false;
            _owner->onPlayingChanged(false);
            return reply(200, cseq, "Range: npt=0.000-\r\n");
        case RtspRequest::Method::Teardown:
            _playing = false;
            _owner->onPlayingChanged(false);
            _sub.reset();
            _source.reset();
            reply(200, cseq);
            (void) shutdownAfterFlush(500);
            return false;
        case RtspRequest::Method::GetParameter:
            _owner->onRequest("GET_PARAMETER");
            return reply(200, cseq);
        case RtspRequest::Method::Unsupported:
        default:
            return reply(501, cseq);
        }
    }

    bool handleDescribe(const std::string &cseq) {
        const std::string name = RtspRequest::pathName(_req.uri());
        std::string path;
        if (name.empty() || !_owner->_resolve(name, &path)) {
            return reply(404, cseq); // 名字未注册：与 /live/ 的 404 语义一致，不暴露路径
        }
        std::string error;
        const std::string sdp = _owner->sdpFor(path, &error);
        if (sdp.empty()) {
            WarnL << "RtspSession: DESCRIBE 失败（" << error << "）";
            return reply(404, cseq, "", error, "text/plain");
        }
        char base[256];
        std::snprintf(base, sizeof(base), "Content-Base: rtsp://%s:%u/live/%s/\r\n",
                      _owner->sdpIp().c_str(), static_cast<unsigned>(_owner->port()), name.c_str());
        return reply(200, cseq, base, sdp, "application/sdp");
    }

    bool handleSetup(const std::string &cseq) {
        const std::string transport = _req.header("transport");
        int rtp_ch = 0;
        int rtcp_ch = 1;
        if (!RtspRequest::parseInterleaved(transport, &rtp_ch, &rtcp_ch)) {
            // R1 只支持 TCP 交错：UDP 的 SETUP 明确回 461（语义正确，FFmpeg 会自动回退 TCP）
            _owner->onRejected("UDP SETUP（R1 只支持 TCP 交错）");
            return reply(461, cseq);
        }
        _rtp_ch = rtp_ch;
        _rtcp_ch = rtcp_ch;
        char hdr[160];
        std::snprintf(hdr, sizeof(hdr), "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n",
                      _rtp_ch, _rtcp_ch);
        return reply(200, cseq, hdr);
    }

    bool handlePlay(const std::string &cseq) {
        if (_sub != nullptr && _playing) {
            return reply(200, cseq, "Range: npt=0.000-\r\n"); // 重复 PLAY：幂等
        }
        if (_source == nullptr) {
            const std::string name = RtspRequest::pathName(_req.uri());
            std::string path;
            if (name.empty() || !_owner->_resolve(name, &path)) {
                return reply(404, cseq);
            }
            auto source = _owner->_manager->acquire(path);
            if (!source) {
                WarnL << "RtspSession: PLAY 取源失败：" << _owner->_manager->lastError();
                return reply(503, cseq, "Retry-After: 1\r\n");
            }
            _source = source;
            _path = path;
            const SourceManager::MuxerStreams streams = _owner->_manager->muxerStreamsFor(path);
            if (streams.video == nullptr) {
                return reply(404, cseq, "", "这个源没有视频轨", "text/plain");
            }
            _video_index = streams.video->index;
            std::vector<uint8_t> sps;
            std::vector<uint8_t> pps;
            size_t length_size = 4;
            if (!streams.video->extradata ||
                !SdpBuilder::parseAvcC(*streams.video->extradata, &sps, &pps, nullptr, nullptr, nullptr,
                                       &length_size)) {
                return reply(404, cseq, "", "视频没有 avcC", "text/plain");
            }
            _length_size = length_size;
            RtpPacketizer::Config config;
            config.payload_type = 96;
            config.mtu = 1400;
            _video_packetizer = std::make_unique<RtpPacketizer>(config);
        }

        // 订阅：GOP 缓存保证第一条是视频关键帧
        Subscriber::Ptr sub = _source->subscribe();
        if (!sub) {
            return reply(503, cseq, "Retry-After: 1\r\n");
        }
        (void) sub->bindPoller(poller());
        // 注：`Session` 是 `enable_shared_from_this<Session>`，所以拿到的是 weak_ptr<Session>；
        // 回调里再 dynamic_pointer_cast 回 RtspSession（弱引用：不与订阅者成环）
        std::weak_ptr<Session> weak = weak_from_this();
        sub->setDrainCallback([weak](const Subscriber::Ptr &) {
            if (auto base = weak.lock()) {
                if (auto self = std::dynamic_pointer_cast<RtspSession>(base)) {
                    self->onDrain();
                }
            }
        });
        _sub = std::move(sub);

        // 先把队列头部（GOP 缓存灌进来的）打出来，拿到首包的 seq/rtptime —— RTP-Info 要用真值
        std::string first_frames;
        const uint16_t first_seq = _video_packetizer->nextSeq();
        uint32_t first_ts = 0;
        bool got_first = false;
        for (;;) {
            MediaPacket::Ptr packet;
            if (_sub->queue().pop(&packet) != FrameQueue::PopResult::Packet) {
                break;
            }
            if (!got_first && packet && packet->kind() == MediaKind::Video) {
                first_ts = rtpTimestampOf(*packet);
                got_first = true;
            }
            packetToInterleaved(packet, &first_frames);
            if (_pending_bytes > kMaxPendingBytes) {
                break; // 首屏最多推这么多，剩下的交给 drain（别在这里写爆发送队列）
            }
        }

        char extra[420];
        std::snprintf(extra, sizeof(extra),
                      "Range: npt=0.000-\r\n"
                      "RTP-Info: url=rtsp://%s:%u/live/%s/trackID=0;seq=%u;rtptime=%u\r\n",
                      _owner->sdpIp().c_str(), static_cast<unsigned>(_owner->port()), nameOf(_path).c_str(),
                      static_cast<unsigned>(first_seq), static_cast<unsigned>(first_ts));
        if (!reply(200, cseq, extra)) {
            return false;
        }
        if (!first_frames.empty()) {
            if (send(first_frames) < 0) {
                return false;
            }
        }
        _playing = true;
        _owner->onPlayingChanged(true);
        return true;
    }

    /// 发一个 RTCP Sender Report（RFC 3550 §6.4.1）：把 RTP 时钟映射到墙上时钟。
    /// 作用：**A/V 同步与客户端统计**（不是修时间戳；首包时间戳靠 SDP + RTP-Info）。
    void appendSenderReport(std::string *out) {
        if (_video_packetizer == nullptr) {
            return;
        }
        const auto now = std::chrono::system_clock::now();
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
        const auto frac = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) -
                          std::chrono::duration_cast<std::chrono::microseconds>(secs);
        const uint64_t ntp_sec = static_cast<uint64_t>(secs.count()) + 2208988800ull; // 1900 起算
        const uint64_t ntp_frac =
            (static_cast<uint64_t>(frac.count()) << 32) / 1000000ull;
        std::string sr;
        sr.push_back(static_cast<char>(0x80)); // V=2, P=0, RC=0
        sr.push_back(static_cast<char>(200));  // PT=SR
        sr.push_back(0x00);
        sr.push_back(0x06); // length = 6（(28/4)-1）
        const uint32_t ssrc = _video_packetizer->ssrc();
        sr.append(reinterpret_cast<const char *>(&ssrc), 0); // 占位，下面按大端写
        sr.clear();
        sr.push_back(static_cast<char>(0x80));
        sr.push_back(static_cast<char>(200));
        sr.push_back(0x00);
        sr.push_back(0x06);
        auto put32 = [&sr](uint32_t v) {
            sr.push_back(static_cast<char>((v >> 24) & 0xFF));
            sr.push_back(static_cast<char>((v >> 16) & 0xFF));
            sr.push_back(static_cast<char>((v >> 8) & 0xFF));
            sr.push_back(static_cast<char>(v & 0xFF));
        };
        put32(ssrc);
        put32(static_cast<uint32_t>(ntp_sec & 0xFFFFFFFFu));
        put32(static_cast<uint32_t>(ntp_frac & 0xFFFFFFFFu));
        put32(_last_rtp_ts);
        put32(static_cast<uint32_t>(_video_packetizer->packets() & 0xFFFFFFFFu));
        put32(static_cast<uint32_t>(_video_packetizer->bytes() & 0xFFFFFFFFu));
        out->append(interleavedFrame(_rtcp_ch, sr));
        _owner->_c_rtcp_sr.fetch_add(1);
    }

    void onDrain() {
        if (!_sub) {
            return;
        }
        std::string out;
        for (;;) {
            MediaPacket::Ptr packet;
            const FrameQueue::PopResult result = _sub->queue().pop(&packet);
            if (result == FrameQueue::PopResult::Empty) {
                break;
            }
            if (result == FrameQueue::PopResult::EndOfStream) {
                _playing = false;
                _owner->onPlayingChanged(false);
                (void) shutdownAfterFlush(500); // 源结束：把已排队的数据发完再关
                break;
            }
            if (!_playing) {
                continue; // 暂停中：丢弃（计数在 R2 加）
            }
            packetToInterleaved(packet, &out);
            if (out.size() >= kMaxPendingBytes) {
                break; // 剩下的下次唤醒继续（队列有界，不会无限涨）
            }
        }
        if (!out.empty() && send(out) < 0) {
            (void) shutdown();
        }
    }

    void packetToInterleaved(const MediaPacket::Ptr &packet, std::string *out) {
        if (!packet || _video_packetizer == nullptr) {
            return;
        }
        if (packet->kind() != MediaKind::Video || packet->streamIndex() != _video_index) {
            return; // R1：音频先不发（SDP 里也没有 audio 轨 → 客户端不会等）
        }
        const uint32_t rtp_ts = rtpTimestampOf(*packet);
        std::vector<std::pair<const uint8_t *, size_t>> nals;
        const auto &payload = *packet->payload();
        const bool ok = avccToNalus(payload.data(), payload.size(), _length_size,
                                    [&nals](const uint8_t *nal, size_t len) {
                                        nals.emplace_back(nal, len);
                                        return true;
                                    });
        if (!ok || nals.empty()) {
            return; // 结构非法：跳过该帧（计数在 R2）
        }
        std::vector<std::string> rtp_packets;
        for (size_t i = 0; i < nals.size(); ++i) {
            const bool marker = (i + 1 == nals.size()); // 一帧的最后一个 NAL 才置 marker
            _video_packetizer->packetizeNal(nals[i].first, nals[i].second, rtp_ts, marker, &rtp_packets);
        }
        for (const auto &rtp : rtp_packets) {
            out->append(interleavedFrame(_rtp_ch, rtp));
            _pending_bytes += rtp.size() + 4;
            _owner->_c_rtp_pkts.fetch_add(1);
            _owner->_c_rtp_bytes.fetch_add(rtp.size());
        }
        // 每 ~30 包（≈1.2 s）跟一个 SR：客户端据此把 RTP 时钟映射到墙上时钟（A/V 同步）
        _pkts_since_sr += rtp_packets.size();
        if (_pkts_since_sr >= 30) {
            _pkts_since_sr = 0;
            appendSenderReport(out);
        }
    }

    /**
     * RTP 时间戳 = **呈现时间**（PTS）折算到 90 kHz，并以"本会话首个视频包"为基准。
     * 为什么要减基准：RTP 时间戳本来就是相对量（客户端用 RTP-Info 的 rtptime 对齐），
     * 而源的首包 pts 可能是"容器起始偏移"（`sample.mp4` 实测首包 200 ms、后续 0/40/80…）。
     * 不减基准就会让 ts 先给一个大值再跳回去，客户端算出负 dts → 直接不再出流
     * （实测：`ffmpeg -c copy` 报 `Non-monotonous DTS … current: -200`，落地 0 字节）。
     *
     * 单调化（`_last_rtp_ts + 1`）：R1 先保证"绝不倒退"，代价是 B 帧源的显示顺序会退化。
     * B 帧源（samples/bf.mp4）在 R1 **没有样本验收**，R2 改成"按 pts 原样发 + 依赖客户端重排"，
     * 并用 bf.mp4 专项验一次 —— 这条是已知限制，不假装已经支持。
     */
    uint32_t rtpTimestampOf(MediaPacket &packet) {
        if (!_ts_base_set) {
            _ts_base_ms = packet.ptsMs();
            _ts_base_set = true;
        }
        int64_t pts_ms = packet.ptsMs();
        if (pts_ms < 0) {
            _owner->_c_neg_ts.fetch_add(1);
            pts_ms = 0;
        }
        int64_t rel_ms = pts_ms - _ts_base_ms;
        if (rel_ms < 0) {
            rel_ms = 0;
            _owner->_c_neg_ts.fetch_add(1);
        }
        // 起始偏移 1 秒（90000 = RFC 3550 认可的"非 0 初始值"）：ffmpeg 的 RTSP/TS 解复用器
        // 遇到 ts=0 会走"33 位回绕"分支（我们在 MPEG-TS 侧也踩过：PTS=0 被吐成 2^33），
        // 表现就是客户端首个包时间戳凭空多出 0.2s、随后又回到 0 → dts 为负。
        uint32_t ts = static_cast<uint32_t>((kRtpStartOffset90k + static_cast<uint64_t>(rel_ms) * 90u) &
                                          0xFFFFFFFFu);
        if (_has_last_rtp_ts && static_cast<int32_t>(ts - _last_rtp_ts) <= 0) {
            ts = _last_rtp_ts + 1; // 同值/倒退都往前提 1 tick（11 µs，肉眼不可见）
            _owner->_c_ts_reordered.fetch_add(1);
        }
        _last_rtp_ts = ts;
        _has_last_rtp_ts = true;
        return ts;
    }

    static std::string nameOf(const std::string &path) {
        const size_t slash = path.find_last_of('/');
        std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos) {
            name = name.substr(0, dot);
        }
        return name;
    }

    static constexpr size_t kMaxPendingBytes = 256u << 10;

    RtspServer *_owner;
    std::string _buf;
    RtspRequest _req;
    std::string _session_id;
    std::string _path;
    MediaSource::Ptr _source;
    Subscriber::Ptr _sub;
    std::unique_ptr<RtpPacketizer> _video_packetizer;
    int _video_index = -1;
    size_t _length_size = 4;
    int _rtp_ch = 0;
    int _rtcp_ch = 1;
    bool _playing = false;
    size_t _pending_bytes = 0;
    size_t _pkts_since_sr = 0;
    static constexpr uint32_t kRtpStartOffset90k = 90000; // 1 秒（避开 ts=0 的回绕分支）
    int64_t _ts_base_ms = 0;
    bool _ts_base_set = false;
    uint32_t _last_rtp_ts = 0;
    bool _has_last_rtp_ts = false;
};

// ============================================================================
// RtspServer
// ============================================================================

RtspServer::Ptr RtspServer::create(const EventPoller::Ptr &poller, const SourceManager::Ptr &manager,
                                   PathResolver resolver, const Config &config) {
    if (!poller || !manager || !resolver || config.max_clients == 0) {
        ErrorL << "RtspServer::create 失败：poller/manager/resolver 为空，或 max_clients=0";
        return nullptr;
    }
    auto server = Ptr(new RtspServer(poller, manager, std::move(resolver), config));
    if (server->_config.port > 0 && server->_sdp_ip.empty()) {
        server->_sdp_ip = detectLocalIpv4();
    }
    return server;
}

RtspServer::RtspServer(const EventPoller::Ptr &poller, SourceManager::Ptr manager,
                       PathResolver resolver, const Config &config)
    : _poller(poller)
    , _manager(std::move(manager))
    , _resolve(std::move(resolver))
    , _config(config)
    , _sdp_ip(config.sdp_ip) {
    _tcp = std::make_shared<TcpServer>(poller);
}

bool RtspServer::start() {
    if (_config.port == 0) {
        return false;
    }
    if (_sdp_ip.empty()) {
        _sdp_ip = detectLocalIpv4();
    }
    (void) _tcp->setMaxSessionCount(_config.max_clients);
    RtspServer *raw = this;
    (void) _tcp->setSessionCreator([raw](const Socket::Ptr &sock) -> Session::Ptr {
        auto poller = EventPoller::getCurrentPoller();
        if (!poller) {
            return nullptr;
        }
        return std::make_shared<RtspSession>(sock, poller, raw);
    });
    if (!_tcp->start(_config.port)) {
        ErrorL << "RtspServer：监听端口 " << _config.port << " 失败";
        return false;
    }
    InfoP("RtspServer: 就绪（rtsp://%s:%u/live/<name>，最多 %zu 个客户端）", _sdp_ip.c_str(),
          static_cast<unsigned>(_tcp->port()), _config.max_clients);
    return true;
}

void RtspServer::shutdown() {
    if (_tcp) {
        (void) _tcp->shutdown();
    }
}

bool RtspServer::listening() const {
    return _tcp && _tcp->listening();
}

uint16_t RtspServer::port() const {
    return _tcp ? _tcp->port() : _config.port;
}

std::string RtspServer::sdpIp() const {
    return _sdp_ip.empty() ? _config.sdp_ip : _sdp_ip;
}

std::string RtspServer::sdpFor(const std::string &path, std::string *error) {
    auto source = _manager->acquire(path);
    if (!source) {
        if (error != nullptr) {
            *error = _manager->lastError();
        }
        return std::string();
    }
    const SourceManager::MuxerStreams streams = _manager->muxerStreamsFor(path);
    SdpBuilder::Streams sdp_streams;
    sdp_streams.video = streams.video;
    sdp_streams.audio = streams.audio;
    return SdpBuilder::build(sdp_streams, std::string(), sdpIp(), error);
}

void RtspServer::onSessionOpen() {
    _total_sessions.fetch_add(1);
}

void RtspServer::onSessionClose() {
    // 会话数由 TcpServer 的 sessionCount() 提供；这里只保证 playing 计数不悬挂
}

void RtspServer::onPlayingChanged(bool playing) {
    if (playing) {
        _playing.fetch_add(1);
    } else {
        const uint64_t current = _playing.load();
        if (current > 0) {
            _playing.fetch_sub(1);
        }
    }
}

void RtspServer::onRequest(const char *method) {
    _total_requests.fetch_add(1);
    const std::string name(method == nullptr ? "" : method);
    if (name == "DESCRIBE") {
        _c_describe.fetch_add(1);
    } else if (name == "SETUP") {
        _c_setup.fetch_add(1);
    } else if (name == "PLAY") {
        _c_play.fetch_add(1);
    } else if (name == "TEARDOWN") {
        _c_teardown.fetch_add(1);
    } else if (name == "GET_PARAMETER" || name == "SET_PARAMETER" || name == "OPTIONS") {
        _c_keepalive.fetch_add(1);
    } else if (name == "PAUSE") {
        _c_keepalive.fetch_add(1);
    } else {
        _c_unsupported.fetch_add(1);
    }
}

void RtspServer::onRejected(const char *why) {
    _total_rejected.fetch_add(1);
    if (why != nullptr) {
        WarnL << "RtspServer: 拒绝一个请求（" << why << "）";
    }
}

std::string RtspServer::statsJson() const {
    std::string out = "\"rtsp_out\":{\"enabled\":";
    out += (_config.port > 0 && _tcp && _tcp->listening()) ? "true" : "false";
    out += ",\"port\":" + std::to_string(port());
    out += ",\"sdp_ip\":\"" + escapeJson(_sdp_ip) + "\"";
    out += ",\"sessions\":" + std::to_string(_tcp ? _tcp->sessionCount() : 0);
    out += ",\"sessions_max\":" + std::to_string(_config.max_clients);
    out += ",\"playing\":" + std::to_string(_playing.load());
    out += ",\"rejected\":" + std::to_string(_total_rejected.load());
    out += ",\"requests\":" + std::to_string(_total_requests.load());
    out += ",\"counters\":{\"describe\":" + std::to_string(_c_describe.load());
    out += ",\"setup\":" + std::to_string(_c_setup.load());
    out += ",\"play\":" + std::to_string(_c_play.load());
    out += ",\"teardown\":" + std::to_string(_c_teardown.load());
    out += ",\"keepalive\":" + std::to_string(_c_keepalive.load());
    out += ",\"unsupported\":" + std::to_string(_c_unsupported.load());
    out += ",\"bad_request\":" + std::to_string(_c_badrequest.load());
    out += ",\"rtp_pkts\":" + std::to_string(_c_rtp_pkts.load());
    out += ",\"rtp_bytes\":" + std::to_string(_c_rtp_bytes.load());
    out += ",\"negative_ts_clamped\":" + std::to_string(_c_neg_ts.load());
    out += ",\"ts_reordered\":" + std::to_string(_c_ts_reordered.load());
    out += ",\"rtcp_sr\":" + std::to_string(_c_rtcp_sr.load());
    out += "}}";
    return out;
}

} // namespace mzmedia
