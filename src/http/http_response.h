/*
 * HttpResponse：攒一个完整的 HTTP 响应（M3-b）
 * ============================================================================
 * 为什么单独一个类：**序列化与发送分开** —— 序列化是纯函数，可以单测（不需要 socket），
 * 而 M6 的 FLV 也会复用响应头构造。M3-c 的 chunked 会在这上面加"增量发送"能力。
 *
 * 约定（尽量不用 void）：setXxx 返回自身便于链式；查询接口返回 bool / 指针 / 值。
 * ============================================================================
 */

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace mzmedia {

class HttpResponse {
public:
    explicit HttpResponse(int status = 200);

    int status() const;
    HttpResponse &setStatus(int code);

    /// 覆盖同名头（大小写不敏感）；保序（先设置的在前）
    HttpResponse &setHeader(const std::string &name, const std::string &value);
    HttpResponse &setContentType(const std::string &mime);
    /// 设置 body；序列化时若没有显式设置 Content-Length 会自动补上
    HttpResponse &setBody(std::string body);
    HttpResponse &setKeepAlive(bool keep);
    bool keepAlive() const;

    size_t bodySize() const;
    bool hasHeader(const std::string &name) const;
    const std::string *header(const std::string &name) const;

    /// 状态行 + 头 + CRLF + body（**不发送**，纯序列化）
    std::string serialize() const;
    /// 状态码对应的 reason phrase（未知码返回 "Unknown"）
    static const char *reasonPhrase(int code);

    // ------------------------------------------------------------------
    // chunked 流式（M3-c）：给 M6 的 FLV 用
    // ------------------------------------------------------------------

    /// 发送出口（由 HttpSession 注入：把字节写进 Session）。返回 false = sender 为空
    using Sender = std::function<bool(const char *data, size_t len)>;
    bool setSender(Sender sender);

    /// 进入 chunked 模式并立刻发状态行 + 头（`Transfer-Encoding: chunked`，**不发** Content-Length）
    bool beginChunked();
    /// 发一块（len == 0 视为空操作返回 true；未进入 chunked 模式返回 false）
    bool sendChunk(const char *data, size_t len);
    /// 发结束块 `0\r\n\r\n`（幂等）
    bool endChunked();
    bool chunked() const;
    bool chunkedEnded() const;

private:
    /// 只序列化状态行 + 头（chunked 与非 chunked 共用）
    std::string serializeHead() const;
    int _status;
    bool _keep_alive = true;
    std::vector<std::pair<std::string, std::string>> _headers;
    std::string _body;
    Sender _sender;
    bool _chunked = false;
    bool _chunked_ended = false;
};

} // namespace mzmedia
