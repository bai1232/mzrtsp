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
#include "output/rtsp_timing.h"
#include "output/sdp_builder.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>

#include <cstdio>
#include <chrono>
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
        (void) setRecvIdleTimeout(60000); // RTSP 客户端会周期发 OPTIONS/GET_PARAMETER 保活
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
        if (isShutdown() || buf == nullptr || buf->size() == 0) {
            return;
        }
        _buf.append(buf->data(), buf->size());
        if (_buf.size() > kMaxBuffer) {
            _owner->onRejected("请求头过大");
            (void) shutdown(SockException(SockException::ErrType::RecvOverflow, 0, "RTSP 请求过大"));
            return;
        }
        for (;;) {
            const RtspRequest::Status status = _req.parse(_buf.data(), _buf.size());
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
            if (!keep || isShutdown() || _buf.empty()) {
                return;
            }
        }
    }

    void onError(const SockException &err) override {
        Session::onError(err);
        _owner->onPlayingChanged(false);
    }

private:
    static constexpr size_t kMaxBuffer = 64u << 10;
    /// 「待发字节」上限：超过就**跳到下一个 IDR**（TCP 交错上丢单帧只会花屏，跳整段才干净）
    static constexpr size_t kMaxPendingBytes = 256u << 10;

    static constexpr const char *statusReason(int status) {
        switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 453: return "Not Enough Bandwidth";
        case 454: return "Session Not Found";
        case 455: return "Method Not Valid in This State";
        case 461: return "Unsupported Transport";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "OK";
        }
    }

    bool reply(int status, const std::string &cseq, const std::string &extra_headers = std::string(),
               const std::string &body = std::string(),
               const std::string &content_type = std::string()) {
        std::string out;
        out += "RTSP/1.0 " + std::to_string(status) + " " + statusReason(status) + "\r\n";
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

    /// @return false = 已收尾（调用方停止继续解析）
    bool handleOne() {
        _owner->onRequest(_req.methodStr().c_str());
        if (_session_id.empty()) {
            _session_id = randomSessionId();
        }
        const std::string cseq = _req.cseq();
        switch (_req.method()) {
        case RtspRequest::Method::Options:
            return reply(200, cseq,
                         "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER\r\n");
        case RtspRequest::Method::Describe:
            return handleDescribe(cseq);
        case RtspRequest::Method::Setup:
            return handleSetup(cseq);
        case RtspRequest::Method::Play:
            return handlePlay(cseq);
        case RtspRequest::Method::Pause:
            _owner->onPlayingChanged(false);
            return reply(200, cseq, "Range: npt=0.000-\r\n");
        case RtspRequest::Method::Teardown:
            _owner->onPlayingChanged(false);
            _sub.reset();
            _source.reset();
            reply(200, cseq);
            (void) shutdownAfterFlush(500);
            return false;
        case RtspRequest::Method::GetParameter:
            return reply(200, cseq); // 保活
        case RtspRequest::Method::Unsupported:
        default:
            return reply(501, cseq);
        }
    }

    bool handleDescribe(const std::string &cseq) {
        const std::string name = RtspRequest::pathName(_req.uri());
        std::string path;
        if (name.empty() || !_owner->_resolve(name, &path)) {
            return reply(404, cseq);
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
        const int track = trackIndexOf(_req.uri());
        int rtp_ch = 2 * track;
        int rtcp_ch = 2 * track + 1;
        if (!RtspRequest::parseInterleaved(transport, &rtp_ch, &rtcp_ch)) {
            // R1/R2 只支持 TCP 交错：UDP 的 SETUP 明确回 461（FFmpeg 会自动回退 TCP）
            _owner->onRejected("UDP SETUP（只支持 TCP 交错）");
            return reply(461, cseq);
        }
        if (track == 0) {
            _v_rtp_ch = rtp_ch;
            _v_rtcp_ch = rtcp_ch;
        } else {
            _a_rtp_ch = rtp_ch;
            _a_rtcp_ch = rtcp_ch;
        }
        char hdr[160];
        std::snprintf(hdr, sizeof(hdr), "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n",
                      rtp_ch, rtcp_ch);
        return reply(200, cseq, hdr);
    }

    bool handlePlay(const std::string &cseq) {
        if (_sub != nullptr) {
            return reply(200, cseq, "Range: npt=0.000-\r\n"); // 重复 PLAY：幂等
        }
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
        RtpPacketizer::Config video_config;
        video_config.payload_type = 96;
        video_config.mtu = 1400;
        _video_pkt = std::make_unique<RtpPacketizer>(video_config);

        if (streams.audio != nullptr && streams.audio->extradata != nullptr &&
            streams.audio->extradata->size() == 2) {
            _audio_index = streams.audio->index;
            _audio_rate = streams.audio->sample_rate > 0 ? streams.audio->sample_rate : 44100;
            RtpPacketizer::Config audio_config;
            audio_config.payload_type = 97;
            audio_config.mtu = 1400;
            _audio_pkt = std::make_unique<RtpPacketizer>(audio_config);
        }

        Subscriber::Ptr sub = _source->subscribe();
        if (!sub) {
            return reply(503, cseq, "Retry-After: 1\r\n");
        }
        (void) sub->bindPoller(poller());
        std::weak_ptr<Session> weak = weak_from_this();
        sub->setDrainCallback([weak](const Subscriber::Ptr &) {
            if (auto base = weak.lock()) {
                if (auto self = std::dynamic_pointer_cast<RtspSession>(base)) {
                    self->onDrain();
                }
            }
        });
        _sub = std::move(sub);

        // 先把队列头部（GOP 缓存灌进来的）打出来，顺便拿到**两轨首包**的 seq/rtptime —— RTP-Info 用真值
        std::string first_frames;
        std::vector<RtspTiming::TrackInfo> tracks;
        RtspTiming::TrackInfo video_info;
        video_info.track_id = 0;
        video_info.seq = _video_pkt->nextSeq();
        video_info.rtptime = RtspTiming::videoRtpTs(0);
        RtspTiming::TrackInfo audio_info;
        audio_info.track_id = 1;
        if (_audio_pkt) {
            audio_info.seq = _audio_pkt->nextSeq();
            audio_info.rtptime = RtspTiming::audioRtpTs(0, _audio_rate);
        }
        bool got_video = false;
        bool got_audio = (_audio_pkt == nullptr);
        for (;;) {
            MediaPacket::Ptr packet;
            if (_sub->queue().pop(&packet) != FrameQueue::PopResult::Packet) {
                break;
            }
            if (!got_video && packet && packet->kind() == MediaKind::Video) {
                video_info.seq = _video_pkt->nextSeq();
                video_info.rtptime = videoTimestampOf(packet->ptsMs());
                got_video = true;
            }
            if (!got_audio && packet && packet->kind() == MediaKind::Audio) {
                audio_info.seq = _audio_pkt->nextSeq();
                audio_info.rtptime = audioTimestampOf(packet->ptsMs());
                got_audio = true;
            }
            packetToInterleaved(packet, &first_frames);
            if (_pending_bytes > kMaxPendingBytes) {
                break; // 首屏别写爆发送队列，剩下的交给 drain
            }
        }
        tracks.push_back(video_info);
        if (_audio_pkt) {
            tracks.push_back(audio_info);
        }
        char base_url[300];
        std::snprintf(base_url, sizeof(base_url), "rtsp://%s:%u/live/%s", _owner->sdpIp().c_str(),
                      static_cast<unsigned>(_owner->port()), name.c_str());
        const std::string rtp_info = RtspTiming::buildRtpInfo(base_url, tracks);
        if (!reply(200, cseq, "Range: npt=0.000-\r\n" + rtp_info)) {
            return false;
        }
        if (!first_frames.empty() && send(first_frames) < 0) {
            return false;
        }
        _pending_bytes = 0;
        _owner->onPlayingChanged(true);
        return true;
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
                _owner->onPlayingChanged(false);
                (void) shutdownAfterFlush(500); // 源结束：把已排队的发完再关
                break;
            }
            packetToInterleaved(packet, &out);
            if (out.size() >= kMaxPendingBytes) {
                // 慢客户端：**跳到下一个 IDR**（丢掉这半个 GOP，计数），不要无限缓冲
                if (!_skip_to_idr) {
                    _skip_to_idr = true;
                    _owner->_c_skipped_to_idr.fetch_add(1);
                }
                break;
            }
        }
        if (!out.empty()) {
            if (send(out) < 0) {
                (void) shutdown();
            }
            _pending_bytes = 0;
        }
    }

    void packetToInterleaved(const MediaPacket::Ptr &packet, std::string *out) {
        if (!packet) {
            return;
        }
        const bool is_video = packet->kind() == MediaKind::Video &&
                              packet->streamIndex() == _video_index;
        const bool is_audio = _audio_pkt != nullptr && packet->kind() == MediaKind::Audio &&
                              packet->streamIndex() == _audio_index;
        if (!is_video && !is_audio) {
            return;
        }
        // 跳 IDR 状态下只放行视频关键帧（音频照发：它不影响解码参考链）
        if (_skip_to_idr) {
            if (!(is_video && packet->isKeyFrame())) {
                return;
            }
            _skip_to_idr = false;
        }

        std::vector<std::string> rtp_packets;
        if (is_video) {
            std::vector<std::pair<const uint8_t *, size_t>> nals;
            const auto &payload = *packet->payload();
            const bool ok = avccToNalus(payload.data(), payload.size(), _length_size,
                                        [&nals](const uint8_t *nal, size_t len) {
                                            nals.emplace_back(nal, len);
                                            return true;
                                        });
            if (!ok || nals.empty()) {
                return; // 结构非法：跳过该帧（计数见 R3）
            }
            const uint32_t rtp_ts = videoTimestampOf(packet->ptsMs());
            for (size_t i = 0; i < nals.size(); ++i) {
                const bool marker = (i + 1 == nals.size());
                _video_pkt->packetizeNal(nals[i].first, nals[i].second, rtp_ts, marker, &rtp_packets);
            }
            for (const auto &rtp : rtp_packets) {
                out->append(interleavedFrame(_v_rtp_ch, rtp));
                _pending_bytes += rtp.size() + 4;
                _owner->_c_rtp_pkts.fetch_add(1);
                _owner->_c_rtp_bytes.fetch_add(rtp.size());
            }
        } else {
            const uint32_t rtp_ts = audioTimestampOf(packet->ptsMs());
            _audio_pkt->packetizeAac(packet->payload()->data(), packet->payload()->size(), rtp_ts,
                                     &rtp_packets);
            for (const auto &rtp : rtp_packets) {
                out->append(interleavedFrame(_a_rtp_ch, rtp));
                _owner->_c_rtp_pkts.fetch_add(1);
                _owner->_c_rtp_bytes.fetch_add(rtp.size());
            }
            _owner->_c_audio_pkts.fetch_add(1);
        }

        // 每 ~30 个视频包跟一个 RTCP SR（≈1.2 s）：A/V 同步与客户端统计
        if (is_video) {
            _pkts_since_sr += rtp_packets.size();
            if (_pkts_since_sr >= 30) {
                _pkts_since_sr = 0;
                appendSenderReport(out);
            }
        }
    }

    /// 视频 ts = **PTS** 相对本会话首包折算到 90 kHz + 1 s 偏移。
    /// @note **绝不单调化**：B 帧源的 pts 天然乱序（`sample1080.mp4` 实测 pts=0,160,80,40,120…），
    ///       强行改成单调会把呈现顺序压平（实测 `ts_reordered` 涨到 124 次、客户端报
    ///       `non monotonically increasing dts`）。RTP 的语义就是"时间戳可以回退、重排靠 seq"。
    uint32_t videoTimestampOf(int64_t pts_ms) {
        if (!_video_base_set) {
            _video_base_ms = pts_ms; // 以本会话首包为基准（首包 ts 恰好 = 90000）
            _video_base_set = true;
        }
        const int64_t rel = pts_ms - _video_base_ms;
        if (rel < 0) {
            _owner->_c_neg_ts.fetch_add(1);
        }
        const uint32_t ts = RtspTiming::videoRtpTs(rel);
        if (_has_last_video_ts && static_cast<int32_t>(ts - _last_video_ts) < 0) {
            _owner->_c_ts_reordered.fetch_add(1); // 只**计数**（B 帧的正常现象），不改数据
        }
        if (!_has_last_video_ts || static_cast<int32_t>(ts - _last_video_ts) > 0) {
            _last_video_ts = ts;
        }
        _has_last_video_ts = true;
        return ts;
    }

    /// 音频 ts：采样数（**由 pts 推导，不累加 1024**）；同样做同轨单调保护
    uint32_t audioTimestampOf(int64_t pts_ms) {
        uint32_t ts = RtspTiming::audioRtpTs(pts_ms, _audio_rate);
        if (pts_ms < 0) {
            _owner->_c_neg_ts.fetch_add(1);
        }
        if (_has_last_audio_ts && static_cast<int32_t>(ts - _last_audio_ts) <= 0) {
            ts = _last_audio_ts + 1;
        }
        _last_audio_ts = ts;
        _has_last_audio_ts = true;
        return ts;
    }

    /// RTCP Sender Report（RFC 3550 §6.4.1）：把 RTP 时钟映射到墙上时钟
    void appendSenderReport(std::string *out) {
        if (_video_pkt == nullptr) {
            return;
        }
        const auto now = std::chrono::system_clock::now();
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
        const auto micros =
            std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) -
            std::chrono::duration_cast<std::chrono::microseconds>(secs);
        const uint64_t ntp_sec = static_cast<uint64_t>(secs.count()) + 2208988800ull; // 1900 起算
        const uint64_t ntp_frac = (static_cast<uint64_t>(micros.count()) << 32) / 1000000ull;
        std::string sr;
        auto put32 = [&sr](uint32_t v) {
            sr.push_back(static_cast<char>((v >> 24) & 0xFF));
            sr.push_back(static_cast<char>((v >> 16) & 0xFF));
            sr.push_back(static_cast<char>((v >> 8) & 0xFF));
            sr.push_back(static_cast<char>(v & 0xFF));
        };
        sr.push_back(static_cast<char>(0x80)); // V=2, P=0, RC=0
        sr.push_back(static_cast<char>(200));  // PT=SR
        sr.push_back(0x00);
        sr.push_back(0x06); // length = 6
        put32(_video_pkt->ssrc());
        put32(static_cast<uint32_t>(ntp_sec & 0xFFFFFFFFu));
        put32(static_cast<uint32_t>(ntp_frac & 0xFFFFFFFFu));
        put32(_last_video_ts);
        put32(static_cast<uint32_t>(_video_pkt->packets() & 0xFFFFFFFFu));
        put32(static_cast<uint32_t>(_video_pkt->bytes() & 0xFFFFFFFFu));
        out->append(interleavedFrame(_v_rtcp_ch, sr));
        _owner->_c_rtcp_sr.fetch_add(1);
    }

    /// SETUP 的 URI 里 `trackID=N` → N（0=视频，1=音频）；没有就按 0
    static int trackIndexOf(const std::string &uri) {
        const std::string key = "trackID=";
        const size_t at = uri.find(key);
        if (at == std::string::npos || at + key.size() >= uri.size()) {
            return 0;
        }
        const char c = uri[at + key.size()];
        if (c < '0' || c > '9') {
            return 0;
        }
        return c - '0';
    }

    RtspServer *_owner;
    std::string _buf;
    RtspRequest _req;
    std::string _session_id;
    std::string _path;
    MediaSource::Ptr _source;
    Subscriber::Ptr _sub;
    std::unique_ptr<RtpPacketizer> _video_pkt;
    std::unique_ptr<RtpPacketizer> _audio_pkt;
    int _video_index = -1;
    int _audio_index = -1;
    int _audio_rate = 44100;
    size_t _length_size = 4;
    int _v_rtp_ch = 0;
    int _v_rtcp_ch = 1;
    int _a_rtp_ch = 2;
    int _a_rtcp_ch = 3;
    size_t _pending_bytes = 0;
    size_t _pkts_since_sr = 0;
    uint32_t _last_video_ts = 0;
    int64_t _video_base_ms = 0;
    bool _video_base_set = false;
    uint32_t _last_audio_ts = 0;
    bool _has_last_video_ts = false;
    bool _has_last_audio_ts = false;
    bool _skip_to_idr = false;
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
    out += ",\"audio_pkts\":" + std::to_string(_c_audio_pkts.load());
    out += ",\"skipped_to_idr\":" + std::to_string(_c_skipped_to_idr.load());
    out += "}}";
    return out;
}

} // namespace mzmedia
