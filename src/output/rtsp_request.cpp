#include "output/rtsp_request.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace mzmedia {

namespace {

std::string toLower(const std::string &text) {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim(const std::string &text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool parseUint(const std::string &text, size_t *out) {
    if (text.empty()) {
        return false;
    }
    size_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<size_t>(c - '0');
        if (value > (1u << 20)) { // 头部长度/序号都不可能这么大：当成畸形
            return false;
        }
    }
    *out = value;
    return true;
}

} // namespace

const char *RtspRequest::errorName() const {
    return _error;
}

std::string RtspRequest::header(const std::string &name) const {
    const auto it = _headers.find(toLower(name));
    return it == _headers.end() ? std::string() : it->second;
}

RtspRequest::Status RtspRequest::parse(const char *data, size_t len) {
    _method = Method::Unsupported;
    _method_str.clear();
    _uri.clear();
    _headers.clear();
    _consumed = 0;
    _error = "None";

    if (data == nullptr || len == 0) {
        return Status::NeedMoreData;
    }
    const std::string_view view(data, len);
    const size_t head_end = view.find("\r\n\r\n");
    if (head_end == std::string_view::npos) {
        // 还没收到完整的头：只有在"已经超过上限"时才判错（否则正常等下一批）
        if (len > kMaxRequestLine + kMaxHeaders * 1024) {
            _error = "HeadTooLarge";
            return Status::Error;
        }
        return Status::NeedMoreData;
    }

    // ---- 请求行：METHOD SP URI SP VERSION ----
    const size_t line_end = view.find("\r\n");
    if (line_end == std::string_view::npos || line_end > kMaxRequestLine) {
        _error = "BadRequestLine";
        return Status::Error;
    }
    const std::string line(data, line_end);
    const size_t sp1 = line.find(' ');
    const size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        _error = "BadRequestLine";
        return Status::Error;
    }
    _method_str = line.substr(0, sp1);
    _uri = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string version = line.substr(sp2 + 1);
    if (version != "RTSP/1.0") {
        _error = "BadVersion";
        return Status::Error;
    }
    if (_method_str == "OPTIONS") {
        _method = Method::Options;
    } else if (_method_str == "DESCRIBE") {
        _method = Method::Describe;
    } else if (_method_str == "SETUP") {
        _method = Method::Setup;
    } else if (_method_str == "PLAY") {
        _method = Method::Play;
    } else if (_method_str == "PAUSE") {
        _method = Method::Pause;
    } else if (_method_str == "TEARDOWN") {
        _method = Method::Teardown;
    } else if (_method_str == "GET_PARAMETER" || _method_str == "SET_PARAMETER") {
        _method = Method::GetParameter; // 都当保活处理（SET_PARAMETER 我们不支持设参数，但语义上是"活着"）
    } else {
        _method = Method::Unsupported;
    }

    // ---- 头部 ----
    size_t pos = line_end + 2;
    size_t header_count = 0;
    while (pos < head_end) {
        const size_t next = view.find("\r\n", pos);
        if (next == std::string_view::npos || next > head_end) {
            _error = "BadHeader";
            return Status::Error;
        }
        const std::string text(data + pos, next - pos);
        const size_t colon = text.find(':');
        if (colon == std::string::npos) {
            _error = "BadHeader";
            return Status::Error;
        }
        if (++header_count > kMaxHeaders) {
            _error = "TooManyHeaders";
            return Status::Error;
        }
        _headers[toLower(trim(text.substr(0, colon)))] = trim(text.substr(colon + 1));
        pos = next + 2;
    }

    // ---- body（按 Content-Length；RTSP 只有少数方法带）----
    size_t body_len = 0;
    const std::string cl = header("content-length");
    if (!cl.empty() && !parseUint(cl, &body_len)) {
        _error = "BadContentLength";
        return Status::Error;
    }
    if (body_len > kMaxBody) {
        _error = "BodyTooLarge";
        return Status::Error;
    }
    const size_t total = head_end + 4 + body_len;
    if (len < total) {
        return Status::NeedMoreData;
    }
    _consumed = total;
    return Status::Complete;
}

bool RtspRequest::parseInterleaved(const std::string &transport, int *rtp_ch, int *rtcp_ch) {
    if (rtp_ch == nullptr || rtcp_ch == nullptr || transport.empty()) {
        return false;
    }
    const std::string key = "interleaved=";
    const size_t at = toLower(transport).find(key);
    if (at == std::string::npos) {
        return false;
    }
    size_t pos = at + key.size();
    size_t first = 0;
    if (!parseUint(transport.substr(pos, 1), &first)) {
        return false;
    }
    pos += 1;
    size_t second = first + 1; // 只给一个通道时，RTCP 默认紧随其后
    if (pos < transport.size() && transport[pos] == '-') {
        ++pos;
        size_t parsed = 0;
        if (!parseUint(transport.substr(pos, 1), &parsed)) {
            return false;
        }
        second = parsed;
    }
    *rtp_ch = static_cast<int>(first);
    *rtcp_ch = static_cast<int>(second);
    return true;
}

bool RtspRequest::parseClientPort(const std::string &transport, int *rtp_port, int *rtcp_port) {
    if (rtp_port == nullptr || rtcp_port == nullptr || transport.empty()) {
        return false;
    }
    const std::string key = "client_port=";
    const size_t at = toLower(transport).find(key);
    if (at == std::string::npos) {
        return false;
    }
    size_t pos = at + key.size();
    size_t first = 0;
    size_t second = 0;
    // 只取数字（端口最多 5 位）
    std::string a;
    while (pos < transport.size() && transport[pos] >= '0' && transport[pos] <= '9') {
        a.push_back(transport[pos++]);
    }
    if (!parseUint(a, &first)) {
        return false;
    }
    second = first + 1;
    if (pos < transport.size() && transport[pos] == '-') {
        ++pos;
        std::string b;
        while (pos < transport.size() && transport[pos] >= '0' && transport[pos] <= '9') {
            b.push_back(transport[pos++]);
        }
        if (!parseUint(b, &second)) {
            return false;
        }
    }
    *rtp_port = static_cast<int>(first);
    *rtcp_port = static_cast<int>(second);
    return true;
}

std::string RtspRequest::pathName(const std::string &uri) {
    // 只接受 .../live/<name>（与 HTTP 侧 /live/ 同一套名字规则，防路径注入）
    const std::string marker = "/live/";
    const size_t at = uri.find(marker);
    if (at == std::string::npos) {
        return std::string();
    }
    std::string name = uri.substr(at + marker.size());
    // 名字是**单段**路径：SETUP 的 URI 会带 `/trackID=0`，查询串/片段也都得切掉
    const size_t cut = name.find_first_of("/?#");
    if (cut != std::string::npos) {
        name = name.substr(0, cut);
    }
    // 名字非法（空 / 带路径穿越）一律不给名字：与 /live/ 同一套防线
    if (name.empty() || name.find("..") != std::string::npos) {
        return std::string();
    }
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '.' || c == '-';
        if (!ok) {
            return std::string();
        }
    }
    return name;
}

} // namespace mzmedia
