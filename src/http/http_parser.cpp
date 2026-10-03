/*
 * HttpParser 实现（M3-a）
 * ============================================================================
 * 解析顺序刻意做成"能早判就早判"：
 *   - 请求行还没结束就超过 max_request_line → 立刻 LineTooLong（不用等 32KB 攒满）
 *   - 累计缓冲超过硬上限 → 立刻按原因拒绝（内存有界，不允许"先攒着再检查"）
 *   - 头部行缺 ':' / 名字含非法字符 / 用了裸 LF → BadHeaderLine
 * 所有拒绝路径都走 reject()：设 error + 计数 + ErrorP（**不静默**）。
 * ============================================================================
 */

#include "http/http_parser.h"

#include "core/logger.h"

#include <cctype>

namespace mzmedia {

namespace {

/// 方法名上限：既是合理性检查，也避免"超长方法名"白白吃掉请求行预算
constexpr size_t kMaxMethodLen = 16;

bool isTokenChar(unsigned char c) {
    // RFC 7230 tchar
    if (std::isalnum(c) != 0) {
        return true;
    }
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
    case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
        return true;
    default:
        return false;
    }
}

std::string trimOws(const std::string &s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t')) {
        --end;
    }
    return s.substr(begin, end - begin);
}

std::string toLowerAscii(const std::string &s) {
    std::string out = s;
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool equalsIgnoreCase(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        const char cb = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

HttpParser::Method methodFromStr(const std::string &m) {
    if (m == "GET") return HttpParser::Method::Get;
    if (m == "POST") return HttpParser::Method::Post;
    if (m == "PUT") return HttpParser::Method::Put;
    if (m == "DELETE") return HttpParser::Method::Delete;
    if (m == "HEAD") return HttpParser::Method::Head;
    if (m == "OPTIONS") return HttpParser::Method::Options;
    return HttpParser::Method::Unsupported;   // 不报错：由服务器回 405/501
}

} // namespace

HttpParser::HttpParser() : HttpParser(Limits{}) {}

HttpParser::HttpParser(const Limits &limits) : _limits(limits) {}

HttpParser::~HttpParser() = default;

// ---------------------------------------------------------------------------
// 喂数据
// ---------------------------------------------------------------------------

HttpParser::Status HttpParser::parse(const char *data, size_t len) {
    if (_error != Error::None) {
        return Status::Error;   // 已经判错了：不再接受数据（调用方应断开）
    }
    if (data == nullptr || len == 0) {
        return Status::NeedMoreData;
    }
    _total_bytes += len;

    // 上限是硬约束：累计缓冲不能无限涨（"先攒着等头结束再检查"= 把内存交给对端）
    const size_t hard_cap = _limits.max_header_bytes + _limits.max_request_line;
    if (_buf.size() + len > hard_cap) {
        const size_t room = (_buf.size() < hard_cap) ? (hard_cap - _buf.size()) : 0;
        if (room > 0) {
            _buf.append(data, room);
        }
        // 到这一步说明"在硬上限之内都没出现头结束符"：按哪一段超了给具体原因
        const size_t line_end = _buf.find("\r\n");
        if (line_end == std::string::npos) {
            return reject(Error::LineTooLong);
        }
        if (line_end + 2 > _limits.max_request_line) {
            return reject(Error::LineTooLong);
        }
        return reject(Error::HeadersTooLarge);
    }

    _buf.append(data, len);
    return tryParseHead();
}

HttpParser::Status HttpParser::parse(const std::string &data) {
    return parse(data.data(), data.size());
}

HttpParser::Status HttpParser::tryParseHead() {
    // 裸 LF（前面不是 CR）要**立刻**拒绝：否则一直等到上限才报"超长"，把排查带偏
    for (size_t i = 0; i < _buf.size(); ++i) {
        if (_buf[i] == '\n' && (i == 0 || _buf[i - 1] != '\r')) {
            return reject(Error::BadHeaderLine);
        }
    }

    const size_t head_end = _buf.find("\r\n\r\n");
    if (head_end == std::string::npos) {
        // 还没结束。但请求行本身超长要**现在**就能判，不能等攒满 40KB
        const size_t line_end = _buf.find("\r\n");
        if (line_end == std::string::npos) {
            if (_buf.size() > _limits.max_request_line) {
                return reject(Error::LineTooLong);
            }
            return Status::NeedMoreData;
        }
        if (line_end + 2 > _limits.max_request_line) {
            return reject(Error::LineTooLong);
        }
        return Status::NeedMoreData;
    }

    const size_t head_len = head_end + 4;   // 含结尾 CRLFCRLF
    if (head_len > _limits.max_header_bytes + _limits.max_request_line) {
        return reject(Error::HeadersTooLarge);
    }
    // 头部里出现 NUL：二进制垃圾 / 攻击载荷，明确拒绝（不做"当普通字符处理"）
    if (_buf.find('\0', 0) < head_len) {
        return reject(Error::NulByte);
    }

    const std::string head = _buf.substr(0, head_end);   // 请求行 + 头部，不含结尾 CRLFCRLF
    size_t line_start = 0;
    bool first = true;
    for (;;) {
        const size_t nl = head.find("\r\n", line_start);
        const bool last_line = (nl == std::string::npos);
        const std::string line =
            head.substr(line_start, last_line ? std::string::npos : (nl - line_start));
        if (first) {
            if (!parseRequestLine(line)) {
                return Status::Error;   // _error 已在内部设好并计数
            }
            first = false;
        } else if (!line.empty() && !parseHeaderLine(line)) {
            return Status::Error;
        }
        if (last_line) {
            break;
        }
        line_start = nl + 2;
    }

    // keep-alive：HTTP/1.1 默认长连接，HTTP/1.0 默认短连接，再被 Connection 头覆盖
    _keep_alive = (_version == Version::Http11);
    if (const std::string *conn = header("Connection")) {
        const std::string v = toLowerAscii(*conn);
        if (v.find("close") != std::string::npos) {
            _keep_alive = false;
        } else if (v.find("keep-alive") != std::string::npos) {
            _keep_alive = true;
        }
    }

    _parsed_bytes = head_len;   // 调用方用 reset() 取走它之后的字节（body / 下一个请求）
    ++_total_parsed;
    return Status::Complete;
}

bool HttpParser::parseRequestLine(const std::string &line) {
    const size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos || sp1 == 0) {
        reject(Error::BadRequestLine);
        return false;
    }
    const size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos || sp2 == sp1 + 1) {
        reject(Error::BadRequestLine);
        return false;
    }
    const std::string method = line.substr(0, sp1);
    const std::string uri = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string ver = line.substr(sp2 + 1);
    if (ver.find(' ') != std::string::npos || ver.empty()) {
        reject(Error::BadRequestLine);
        return false;
    }
    if (method.size() > kMaxMethodLen) {
        reject(Error::BadRequestLine);
        return false;
    }
    for (char c : method) {
        if (!isTokenChar(static_cast<unsigned char>(c))) {
            reject(Error::BadRequestLine);
            return false;
        }
    }
    if (uri.size() > _limits.max_uri) {
        reject(Error::UriTooLong);
        return false;
    }
    for (char c : uri) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7f) {   // 不允许空格与控制字符（空格已被切分排除）
            reject(Error::BadRequestLine);
            return false;
        }
    }

    _method_str = method;
    _method = methodFromStr(method);
    _uri = uri;
    const size_t q = uri.find('?');
    if (q == std::string::npos) {
        _path = uri;
        _query.clear();
    } else {
        _path = uri.substr(0, q);
        _query = uri.substr(q + 1);
    }

    if (ver == "HTTP/1.1") {
        _version = Version::Http11;
    } else if (ver == "HTTP/1.0") {
        _version = Version::Http10;
    } else {
        reject(Error::BadVersion);
        return false;
    }
    return true;
}

bool HttpParser::parseHeaderLine(const std::string &line) {
    const size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) {
        reject(Error::BadHeaderLine);
        return false;
    }
    const std::string name = line.substr(0, colon);
    for (char c : name) {
        // 名字必须是 token：这样"名字后面带空格"（obs-fold/畸形）也会在这里被拒
        if (!isTokenChar(static_cast<unsigned char>(c))) {
            reject(Error::BadHeaderLine);
            return false;
        }
    }
    if (_headers.size() >= _limits.max_headers) {
        reject(Error::TooManyHeaders);
        return false;
    }
    _headers.emplace_back(name, trimOws(line.substr(colon + 1)));
    return true;
}

HttpParser::Status HttpParser::reject(Error err) {
    _error = err;
    ++_total_rejected;
    ErrorP("HttpParser: 拒绝请求头（%s）已缓冲 %zu 字节，累计喂入 %llu 字节", errorName(),
           _buf.size(), static_cast<unsigned long long>(_total_bytes));
    return Status::Error;
}

std::string HttpParser::reset() {
    std::string rest;
    if (_parsed_bytes > 0 && _buf.size() > _parsed_bytes) {
        rest = _buf.substr(_parsed_bytes);   // 粘包的下一个请求 / body：交还给调用方
    }
    _buf.clear();
    _parsed_bytes = 0;
    _method = Method::Get;
    _method_str.clear();
    _uri.clear();
    _path.clear();
    _query.clear();
    _version = Version::Http11;
    _keep_alive = true;
    _headers.clear();
    _error = Error::None;
    return rest;
}

// ---------------------------------------------------------------------------
// 结果 / 观测
// ---------------------------------------------------------------------------

size_t HttpParser::bufferedBytes() const {
    return _buf.size();
}

size_t HttpParser::parsedBytes() const {
    return _parsed_bytes;
}

HttpParser::Method HttpParser::method() const {
    return _method;
}

const std::string &HttpParser::methodStr() const {
    return _method_str;
}

const std::string &HttpParser::uri() const {
    return _uri;
}

const std::string &HttpParser::path() const {
    return _path;
}

const std::string &HttpParser::query() const {
    return _query;
}

HttpParser::Version HttpParser::version() const {
    return _version;
}

bool HttpParser::keepAlive() const {
    return _keep_alive;
}

size_t HttpParser::headerCount() const {
    return _headers.size();
}

const std::vector<std::pair<std::string, std::string>> &HttpParser::headers() const {
    return _headers;
}

const std::string *HttpParser::header(const std::string &name) const {
    for (const auto &kv : _headers) {
        if (equalsIgnoreCase(kv.first, name)) {
            return &kv.second;   // 同名多条：返回第一条
        }
    }
    return nullptr;
}

HttpParser::Error HttpParser::error() const {
    return _error;
}

const char *HttpParser::errorName() const {
    switch (_error) {
    case Error::None: return "none";
    case Error::LineTooLong: return "line-too-long";
    case Error::UriTooLong: return "uri-too-long";
    case Error::HeadersTooLarge: return "headers-too-large";
    case Error::TooManyHeaders: return "too-many-headers";
    case Error::BadRequestLine: return "bad-request-line";
    case Error::BadHeaderLine: return "bad-header-line";
    case Error::BadVersion: return "bad-version";
    case Error::NulByte: return "nul-byte";
    }
    return "unknown";
}

uint64_t HttpParser::totalParsed() const {
    return _total_parsed;
}

uint64_t HttpParser::totalRejected() const {
    return _total_rejected;
}

uint64_t HttpParser::totalBytes() const {
    return _total_bytes;
}

} // namespace mzmedia
