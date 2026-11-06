/*
 * RtspServer：RTSP **输出**（我们当服务端，客户端来拉）
 * ============================================================================
 * 和 `HttpServer` 完全同构：**组合一个 `TcpServer`**（监听 + accept + 会话表 + 关停排水都复用），
 * 每个连接包成 `RtspSession : public Session`，只重写 `onRecv` 做分帧。
 * 这样监听/会话表/fd 延迟删除/关停顺序全部免费 —— 不再自己写一遍 accept 循环。
 *
 * 数据来源：复用 `MediaSource`（GOP 缓存 → 中途接入从关键帧起播；有界队列 → 慢客户端
 * 不会拖垮别人；`SubscriberQuota` → 名额准入）。`media/` 一行不改。
 *
 * 本批（R1）范围：视频 H264 + **TCP 交错** + OPTIONS/DESCRIBE/SETUP/PLAY/PAUSE/TEARDOWN/
 * GET_PARAMETER + 会话超时。UDP/音频/RTCP 在 R2/R3，**不在这里假装支持**：
 * 客户端只要 UDP SETUP 就回 `461 Unsupported Transport`（语义正确，FFmpeg 会自动回退 TCP）。
 * ============================================================================
 */

#pragma once

#include "media/source_manager.h"
#include "network/event_poller.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace mzmedia {

class RtspSession;

class RtspServer {
public:
    using Ptr = std::shared_ptr<RtspServer>;
    /// 名字 → 媒体路径（由 app 注入：与 /live/<name> 同一套映射，防路径注入）
    using PathResolver = std::function<bool(const std::string &name, std::string *path)>;

    struct Config {
        uint16_t port = 0;          ///< 0 = 不启动 RTSP 输出
        size_t max_clients = 64;    ///< 同时最多几个 RTSP 客户端（0 非法）
        std::string sdp_ip;         ///< SDP 的 `c=` 地址；空 = 自动探测本机出口 IP
    };

    static Ptr create(const EventPoller::Ptr &poller, const SourceManager::Ptr &manager,
                      PathResolver resolver, const Config &config);

    /// 启动监听；@return false = 失败（已记日志）
    bool start();
    void shutdown();
    bool listening() const;
    uint16_t port() const;

    /// `/api/stats` 的 `rtsp_out` 段（合法 JSON 片段，不含最外层花括号）
    std::string statsJson() const;

    /// 观测（原子）
    uint64_t totalSessions() const {
        return _total_sessions.load();
    }
    uint64_t totalRejected() const {
        return _total_rejected.load();
    }
    uint64_t totalRequests() const {
        return _total_requests.load();
    }
    uint64_t playingSessions() const {
        return _playing.load();
    }

private:
    friend class RtspSession;

    RtspServer(const EventPoller::Ptr &poller, SourceManager::Ptr manager, PathResolver resolver,
               const Config &config);

    /// 会话开始/结束时调（只改计数，不做生命周期管理 —— 那由 Session/TcpServer 负责）
    void onSessionOpen();
    void onSessionClose();
    void onPlayingChanged(bool playing);
    void onRequest(const char *method);
    void onRejected(const char *why);

    /// SDP 缓存：同一路源的 SDP 只生成一次（`StreamInfo` 在源存活期不变）
    std::string sdpFor(const std::string &path, std::string *error);
    /// 本机出口 IP（`c=` 行用）
    std::string sdpIp() const;

    EventPoller::Ptr _poller;
    SourceManager::Ptr _manager;
    PathResolver _resolve;
    Config _config;
    std::shared_ptr<class TcpServer> _tcp;
    std::string _sdp_ip;

    std::atomic<uint64_t> _total_sessions{0};
    std::atomic<uint64_t> _total_rejected{0};
    std::atomic<uint64_t> _total_requests{0};
    std::atomic<uint64_t> _playing{0};
    std::atomic<uint64_t> _c_describe{0};
    std::atomic<uint64_t> _c_setup{0};
    std::atomic<uint64_t> _c_play{0};
    std::atomic<uint64_t> _c_teardown{0};
    std::atomic<uint64_t> _c_keepalive{0};
    std::atomic<uint64_t> _c_badrequest{0};
    std::atomic<uint64_t> _c_unsupported{0};
    std::atomic<uint64_t> _c_rtp_pkts{0};
    std::atomic<uint64_t> _c_rtp_bytes{0};
    std::atomic<uint64_t> _c_neg_ts{0};
    std::atomic<uint64_t> _c_ts_reordered{0};
    std::atomic<uint64_t> _c_rtcp_sr{0};
};

} // namespace mzmedia
