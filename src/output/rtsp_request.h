/*
 * RtspRequest：RTSP 请求解析（**只做服务端收到的那些方法**）
 * ============================================================================
 * 为什么不复用 `http::HttpParser`：
 *   · 它的 `Method` 枚举里没有 DESCRIBE/SETUP/PLAY/PAUSE/TEARDOWN/GET_PARAMETER，
 *     塞进去会让 HTTP 侧跟着多出六个"HTTP 永远不会出现的值"；
 *   · RTSP 还有自己的头语义（`Transport`、`Session`、`CSeq`），校验点也不同。
 *   两者共享的只有"请求行 + 头 + CRLF"这套骨架 —— 重复的部分是几十行，换来的两边都干净。
 *
 * 边界（都有上限，不靠"客户端不会那么发"）：
 *   · 请求行 ≤ 8 KB、头 ≤ 64 个、body ≤ 64 KB（RTSP 的 SET_PARAMETER 才可能有 body）
 *   · 只认 CRLF；不是 CRLF 分隔 → Error（不猜、不容错）
 * ============================================================================
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace mzmedia {

class RtspRequest {
public:
    enum class Method {
        Options,
        Describe,
        Setup,
        Play,
        Pause,
        Teardown,
        GetParameter,
        Unsupported,
    };
    enum class Status {
        NeedMoreData, // 半包：不是错误，等更多字节
        Complete,     // 一个完整请求（含 body）已就绪
        Error,        // 畸形 / 超限：调用方应回 400 并断开
    };

    static constexpr size_t kMaxRequestLine = 8u << 10;
    static constexpr size_t kMaxHeaders = 64;
    static constexpr size_t kMaxBody = 64u << 10;

    RtspRequest() = default;

    /// 从**累积缓冲**的开头解析；成功时 `consumed()` 给出应丢弃的字节数
    Status parse(const char *data, size_t len);

    Method method() const {
        return _method;
    }
    const std::string &methodStr() const {
        return _method_str;
    }
    const std::string &uri() const {
        return _uri;
    }
    /// 头名大小写不敏感（内部按小写存）；没有则返回空串
    std::string header(const std::string &name) const;
    /// `CSeq`（没给就返回空串：调用方决定要不要回）
    std::string cseq() const {
        return header("cseq");
    }
    size_t consumed() const {
        return _consumed;
    }
    const char *errorName() const;

    /// `Transport: RTP/AVP/TCP;unicast;interleaved=0-1` → (0, 1)
    /// @return false = 没有 interleaved 参数（调用方据此判断走 UDP 还是回 461）
    static bool parseInterleaved(const std::string &transport, int *rtp_ch, int *rtcp_ch);

    /// `Transport: RTP/AVP;unicast;client_port=5000-5001` → (5000, 5001)
    /// @return false = 没有 client_port（UDP 的 SETUP 必须带，没有就是客户端 bug → 400）
    static bool parseClientPort(const std::string &transport, int *rtp_port, int *rtcp_port);

    /// 从 URI 里取媒体名：`rtsp://host:554/live/sample` → `sample`
    /// @return 空串 = URI 不合法（不是 /live/<name> 形式）
    static std::string pathName(const std::string &uri);

private:
    Method _method = Method::Unsupported;
    std::string _method_str;
    std::string _uri;
    std::map<std::string, std::string> _headers; // key 已转小写
    size_t _consumed = 0;
    const char *_error = "None";
};

} // namespace mzmedia
