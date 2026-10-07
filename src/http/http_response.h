/*
 * HttpResponse：攒一个完整的 HTTP 响应（M3-b；M3-c 加 chunked；M6-b 加"异步流式 + 附件"）
 * ============================================================================
 * 为什么单独一个类：**序列化与发送分开** —— 序列化是纯函数，可以单测（不需要 socket），
 * 而 M6 的 FLV 也会复用响应头构造。M3-c 的 chunked 会在这上面加"增量发送"能力。
 *
 * 约定（尽量不用 void）：setXxx 返回自身便于链式；查询接口返回 bool / 指针 / 值。
 *
 * 【M6-b 的两项新增】
 *   1) **异步流式**：M3-c 的 chunked 演示是"handler 里同步发完"，而 FLV 是"handler 返回后
 *      还会持续推流很久"。所以需要一个东西告诉框架"结束块由我自己发，你别兜底"，
 *      以及一个**在 handler 返回后仍然可用**的收尾出口（`endStreamFn()`）——
 *      它由 `HttpSession` 注入，内部做"发结束块 + 等发送队列排空再关连接"。
 *   2) **附件（holdResource）**：流式响应的发送器/订阅者必须活得和**连接**一样久，
 *      但又不能由 handler 自己抱着（handler 一返回就析构，而订阅者的 drain 回调里
 *      捕获裸 session 指针 → UAF）。放进附件后由会话托管，**连接关闭时统一释放**。
 * ============================================================================
 */

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
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
    /// 取回发送出口（M6-b：`FlvSender` 的 sink 直接复用它，不必再包一层）
    const Sender &sender() const;

    /// 进入 chunked 模式并立刻发状态行 + 头（`Transfer-Encoding: chunked`，**不发** Content-Length）
    bool beginChunked();
    /// 发一块（len == 0 视为空操作返回 true；未进入 chunked 模式返回 false）
    bool sendChunk(const char *data, size_t len);
    /// 发结束块 `0\r\n\r\n`（幂等）
    bool endChunked();
    bool chunked() const;
    bool chunkedEnded() const;

    /// **分块编码的写出函数**：内部加 `[size]\r\n … \r\n` 帧头
    /// @note 它捕获的是 `Sender` 的**拷贝**，因此**不依赖本对象** —— 可以在 handler 返回之后
    ///       （例如流式推送期间）继续使用。`FlvSender` 的 sink 就该接这个，而不是裸 `sender()`
    ///       （接裸 sender 会把 FLV 字节直接灌进 chunked 流 → 客户端报 "Malformed encoding"）
    using ChunkWriter = std::function<bool(const char *data, size_t len)>;
    ChunkWriter chunkWriter() const;

    // ------------------------------------------------------------------
    // 异步流式（M6-b）
    // ------------------------------------------------------------------

    /// 「结束并关闭」出口：**由 `HttpSession` 注入**，处理器可在 handler 返回后任意时刻调用
    /// @note 内部动作 = 发结束块 `0\r\n\r\n` + 等发送队列排空后关闭连接（超时有上限）
    using EndStreamFn = std::function<bool()>;
    bool setEndStream(EndStreamFn fn);
    const EndStreamFn &endStreamFn() const;

    /// 声明这是**异步流式**响应：框架不再兜底 `endChunked()`
    /// （兜底只对"同步发完"的处理器有意义；对异步流会误把流结束掉）
    bool setChunkedAsync();
    bool chunkedAsync() const;

    // ------------------------------------------------------------------
    // 附件（M6-b）：交给会话托管，连接关闭时自动释放
    // ------------------------------------------------------------------

    void holdResource(std::shared_ptr<void> resource);
    /// 取出全部附件（由 `HttpSession` 在 handler 返回后接管）
    std::vector<std::shared_ptr<void>> takeResources();

private:
    /// 只序列化状态行 + 头（chunked 与非 chunked 共用）
    std::string serializeHead() const;
    int _status;
    bool _keep_alive = true;
    std::vector<std::pair<std::string, std::string>> _headers;
    std::string _body;
    Sender _sender;
    EndStreamFn _end_stream;
    bool _chunked = false;
    bool _chunked_ended = false;
    bool _chunked_async = false;
    std::vector<std::shared_ptr<void>> _resources;
};

} // namespace mzmedia
