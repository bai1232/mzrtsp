/*
 * HttpResponse 实现（M3-b）
 * ============================================================================
 * 只做"把响应拼成字节"。两个刻意的行为：
 *   1) 没有显式设置 Content-Length 时**自动补**（值 = body 长度），避免"头说有 100 字节、
 *      实际发 50"这种最难查的不一致；
 *   2) 只在**要关闭**时才发 `Connection: close`；HTTP/1.1 默认长连接，不啰嗦。
 * ============================================================================
 */

#include "http/http_response.h"

#include <cstdio>

namespace mzmedia {

namespace {

bool sameHeaderName(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

} // namespace

HttpResponse::HttpResponse(int status) : _status(status) {}

int HttpResponse::status() const {
    return _status;
}

HttpResponse &HttpResponse::setStatus(int code) {
    _status = code;
    return *this;
}

HttpResponse &HttpResponse::setHeader(const std::string &name, const std::string &value) {
    for (auto &kv : _headers) {
        if (sameHeaderName(kv.first, name)) {
            kv.second = value;   // 覆盖，不重复追加
            return *this;
        }
    }
    _headers.emplace_back(name, value);
    return *this;
}

HttpResponse &HttpResponse::setContentType(const std::string &mime) {
    return setHeader("Content-Type", mime);
}

HttpResponse &HttpResponse::setBody(std::string body) {
    _body = std::move(body);
    return *this;
}

HttpResponse &HttpResponse::setKeepAlive(bool keep) {
    _keep_alive = keep;
    return *this;
}

bool HttpResponse::keepAlive() const {
    return _keep_alive;
}

size_t HttpResponse::bodySize() const {
    return _body.size();
}

bool HttpResponse::hasHeader(const std::string &name) const {
    return header(name) != nullptr;
}

const std::string *HttpResponse::header(const std::string &name) const {
    for (const auto &kv : _headers) {
        if (sameHeaderName(kv.first, name)) {
            return &kv.second;
        }
    }
    return nullptr;
}

std::string HttpResponse::serialize() const {
    std::string out;
    out.reserve(256 + _body.size());

    char line[64] = {0};
    std::snprintf(line, sizeof(line), "HTTP/1.1 %d %s\r\n", _status, reasonPhrase(_status));
    out += line;

    for (const auto &kv : _headers) {
        out += kv.first;
        out += ": ";
        out += kv.second;
        out += "\r\n";
    }
    if (!hasHeader("Content-Length")) {
        std::snprintf(line, sizeof(line), "Content-Length: %zu\r\n", _body.size());
        out += line;
    }
    if (!_keep_alive) {
        out += "Connection: close\r\n";
    }
    out += "\r\n";
    out += _body;
    return out;
}

const char *HttpResponse::reasonPhrase(int code) {
    switch (code) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 416: return "Range Not Satisfiable";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "Unknown";
    }
}

} // namespace mzmedia
