/*
 * HttpParser：HTTP/1.x 请求头解析器（M3-a）
 * ============================================================================
 * 形状来源：docs/DESIGN_M3.md §3（本文件的接口就是那一节）
 *
 * 设计取向（三条，都对应被踩过的坑）：
 *   1) **I/O 无关**：只吃 (data,len)，不认识 socket/Buffer/poller → 纯函数式，单测极快，
 *      也不给 http 层引入 network 依赖（分层：core → network → http）。
 *   2) **上限是硬约束**（§4.3）：请求行 / URI / 头部总量 / 头部条数都有上限，而且
 *      **累计缓冲本身也受限** —— 不允许"先攒着，等头结束再检查"。超限一律返回
 *      Error（带具体原因）+ 计数，交给调用方回 4xx。
 *   3) **半包/粘包是常态**：没解析完就返回 NeedMoreData（不是错误）；一个包里多于一个
 *      请求时，`reset()` 会把剩余字节**交还**给调用方，由调用方再次 `parse()`。
 *
 * 返回值策略（尽量不用 void）：解析结果用 Status 枚举；`reset()` 返回剩余字节；
 *   观测计数有独立 getter。没有任何"静默截断"或"猜一个默认值"的路径。
 *
 * 解析严格性：头结束符只认 `\r\n\r\n`，逐行只认 `\r\n`（curl / 浏览器 / ffmpeg 都发 CRLF）。
 *   裸 LF 会被判 BadHeaderLine —— 这是**明确的取舍**，写在这里避免以后被当成 bug。
 * ============================================================================
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mzmedia {

class HttpParser {
public:
    /// 方法：认识的列出来；不认识的**不报错**（解析成功，`Method::Unsupported`），
    /// 由服务器回 405/501 —— 解析层不该替业务层决定"这个请求合法吗"
    enum class Method { Get, Post, Put, Delete, Head, Options, Unsupported };
    enum class Version { Http10, Http11 };

    enum class Status {
        NeedMoreData,   // 半包：数据不够（**不是错误**，等更多数据）
        Complete,       // 一个完整请求头已就绪
        Error,          // 畸形或超限：见 error()
    };

    enum class Error {
        None = 0,
        LineTooLong,        // 请求行超过 max_request_line
        UriTooLong,         // URI 超过 max_uri
        HeadersTooLarge,    // 头部总字节超过 max_header_bytes
        TooManyHeaders,     // 头部条数超过 max_headers
        BadRequestLine,     // 请求行结构不对（不是"方法 SP URI SP 版本"）
        BadHeaderLine,      // 头部行没有 ':' / 名字不合法 / 用了裸 LF
        BadVersion,         // 版本不是 HTTP/1.0 或 HTTP/1.1
        NulByte,            // 头部里出现 NUL（二进制垃圾或攻击载荷）
    };

    /// 上限（初值，全部可配；改大改小都要有依据 —— §4.6）
    struct Limits {
        size_t max_request_line = 8 * 1024;    // 请求行（含 CRLF）
        size_t max_uri = 4 * 1024;             // URI（含 query）
        size_t max_header_bytes = 32 * 1024;   // 请求行 + 所有头部的总字节
        size_t max_headers = 100;              // 头部条数
    };

    HttpParser();                                  // 用内置默认上限
    explicit HttpParser(const Limits &limits);
    ~HttpParser();
    HttpParser(const HttpParser &) = delete;
    HttpParser &operator=(const HttpParser &) = delete;

    // ------------------------------------------------------------------
    // 喂数据 / 复位
    // ------------------------------------------------------------------

    /**
     * 喂入一段数据并尝试解析
     * @return Complete = 头已解析完（结果见下面各 getter）；NeedMoreData = 继续喂；
     *         Error = 畸形/超限（见 error()），此时**必须停止解析并断开**（不要重试）
     * @note 可反复调用（半包）；一次喂入多于此请求的字节不会丢，`reset()` 会交还
     */
    Status parse(const char *data, size_t len);
    Status parse(const std::string &data);

    /**
     * 复位以解析下一个请求
     * @return **本包里多出来的字节**（粘包/管线化的第二个请求、或 body）
     *         调用方应当把它重新喂给 `parse()`，直到返回 NeedMoreData
     * @note 计数（totalParsed 等）不清零；**上一次的解析结果（path/headers…）会被清掉** ——
     *       所以要在 `reset()` **之前**读完结果。典型调用顺序：
     *       `parse → Complete → 用结果 → reset() → 把返回的剩余字节再 parse`
     */
    std::string reset();

    /// 当前累计还未组成完整头的字节数（观测"半包堆积"）
    size_t bufferedBytes() const;
    /// 已解析完的头部字节数（含结尾 CRLFCRLF；Complete 时有意义）
    size_t parsedBytes() const;

    // ------------------------------------------------------------------
    // 解析结果（Complete 时有效）
    // ------------------------------------------------------------------

    Method method() const;
    const std::string &methodStr() const;   // 原样保留（含 Unsupported 的情况）
    const std::string &uri() const;         // 原样（含 query）
    const std::string &path() const;        // URI 去掉 '?' 之后
    const std::string &query() const;       // '?' 之后；没有则为空串
    Version version() const;
    /// HTTP/1.1 默认长连接、HTTP/1.0 默认短连接，再被 Connection 头覆盖
    bool keepAlive() const;

    size_t headerCount() const;
    const std::vector<std::pair<std::string, std::string>> &headers() const;
    /// 大小写不敏感（HTTP 头名不区分大小写）；同名多条时返回**第一条**；没有返回 nullptr
    const std::string *header(const std::string &name) const;

    Error error() const;
    const char *errorName() const;   // 日志用（"headers-too-large" / ...）

    // ------------------------------------------------------------------
    // 观测（线程安全：只在单线程里用，但计数用原子便于以后跨线程看）
    // ------------------------------------------------------------------

    uint64_t totalParsed() const;     // 成功解析的请求数
    uint64_t totalRejected() const;   // 因畸形/超限被拒的次数
    uint64_t totalBytes() const;      // 累计喂入字节数

private:
    /// 找到头结束符（"\r\n\r\n"）则解析并返回 Complete；否则 NeedMoreData/Error
    Status tryParseHead();
    /// 只解析请求行；失败返回 false 并设置 _error
    bool parseRequestLine(const std::string &line);
    /// 解析一行头部；失败返回 false 并设置 _error
    bool parseHeaderLine(const std::string &line);
    Status reject(Error err);   // 记日志 + 计数 + 清状态，返回 Error

    Limits _limits;
    std::string _buf;          // 累计缓冲（未组成完整头之前）

    // 解析结果
    Method _method = Method::Get;
    std::string _method_str;
    std::string _uri;
    std::string _path;
    std::string _query;
    Version _version = Version::Http11;
    bool _keep_alive = true;
    std::vector<std::pair<std::string, std::string>> _headers;
    size_t _parsed_bytes = 0;

    Error _error = Error::None;
    uint64_t _total_parsed = 0;
    uint64_t _total_rejected = 0;
    uint64_t _total_bytes = 0;
};

} // namespace mzmedia
