/*
 * HttpParser 单元测试（M3-a，分组 `http`）
 * ============================================================================
 * 覆盖维度（AI_COLLAB §3.4：正常 / 空 / 满 / 断开 / 超大）：
 *   正常  标准 GET、大小写不敏感查找、值两端 OWS 裁剪、path/query 切分、keep-alive 矩阵
 *   空    空喂入、无 query、无头部
 *   满    请求行超长、URI 超长、头部总量超限、头部条数超限（且缓冲有界）
 *   断开  ——（解析器无 I/O，无连接语义；连接级在 ntimed_http_* 里测）
 *   超大  32KB 级头部、8KB 级请求行
 *
 * 为什么这一组能进 TSAN 严格组：纯内存解析、单线程、无等待。
 * ============================================================================
 */

#include "test_main.h"

#include "http/http_parser.h"

#include <cstdio>
#include <string>

using namespace mzmedia;

namespace {

const std::string kSimpleGet = "GET /index.html HTTP/1.1\r\nHost: example.com\r\n\r\n";

/// 造一个恰好 n 字节长的长行（用于超限用例）
std::string longString(size_t n, char fill) {
    return std::string(n, fill);
}

} // namespace

// ---------------------------------------------------------------------------
// 正常
// ---------------------------------------------------------------------------

MZ_TEST(http_parse_simple_get) {
    HttpParser parser;
    const HttpParser::Status st = parser.parse(kSimpleGet);
    MZ_ASSERT_EQ(static_cast<int>(st), static_cast<int>(HttpParser::Status::Complete));
    MZ_ASSERT_EQ(static_cast<int>(parser.method()), static_cast<int>(HttpParser::Method::Get));
    MZ_ASSERT_STR_EQ(parser.methodStr(), std::string("GET"));
    MZ_ASSERT_STR_EQ(parser.uri(), std::string("/index.html"));
    MZ_ASSERT_STR_EQ(parser.path(), std::string("/index.html"));
    MZ_ASSERT_TRUE(parser.query().empty());
    MZ_ASSERT_EQ(static_cast<int>(parser.version()), static_cast<int>(HttpParser::Version::Http11));
    MZ_ASSERT_TRUE(parser.keepAlive());
    MZ_ASSERT_EQ(parser.headerCount(), 1u);
    MZ_ASSERT_EQ(parser.parsedBytes(), kSimpleGet.size());
    MZ_ASSERT_EQ(parser.totalParsed(), 1u);
    MZ_ASSERT_EQ(parser.totalRejected(), 0u);
    MZ_ASSERT_EQ(parser.totalBytes(), static_cast<uint64_t>(kSimpleGet.size()));
    MZ_ASSERT_TRUE(parser.reset().empty());   // 没有多余字节
}

MZ_TEST(http_parse_partial_byte_by_byte) {
    // 半包：一个字节一个字节喂，只有最后一个字节才 Complete
    HttpParser parser;
    for (size_t i = 0; i + 1 < kSimpleGet.size(); ++i) {
        const HttpParser::Status st = parser.parse(kSimpleGet.data() + i, 1);
        MZ_ASSERT_EQ(static_cast<int>(st), static_cast<int>(HttpParser::Status::NeedMoreData));
    }
    const HttpParser::Status st = parser.parse(kSimpleGet.data() + kSimpleGet.size() - 1, 1);
    MZ_ASSERT_EQ(static_cast<int>(st), static_cast<int>(HttpParser::Status::Complete));
    MZ_ASSERT_STR_EQ(parser.path(), std::string("/index.html"));
    MZ_ASSERT_EQ(parser.totalParsed(), 1u);
}

MZ_TEST(http_parse_empty_feed) {
    HttpParser parser;
    MZ_ASSERT_EQ(static_cast<int>(parser.parse("", 0)),
                 static_cast<int>(HttpParser::Status::NeedMoreData));
    MZ_ASSERT_EQ(static_cast<int>(parser.parse(nullptr, 0)),
                 static_cast<int>(HttpParser::Status::NeedMoreData));
    MZ_ASSERT_EQ(parser.bufferedBytes(), 0u);
    MZ_ASSERT_EQ(parser.totalRejected(), 0u);
}

MZ_TEST(http_parse_pipelined_two_requests) {
    // 粘包：一个包里两个请求 —— reset() 必须把第二个请求交还出来
    const std::string two = kSimpleGet + "POST /upload HTTP/1.0\r\n\r\n";
    HttpParser parser;
    MZ_ASSERT_EQ(static_cast<int>(parser.parse(two)),
                 static_cast<int>(HttpParser::Status::Complete));
    MZ_ASSERT_STR_EQ(parser.path(), std::string("/index.html"));

    const std::string rest = parser.reset();
    MZ_ASSERT_STR_EQ(rest, std::string("POST /upload HTTP/1.0\r\n\r\n"));
    MZ_ASSERT_EQ(static_cast<int>(parser.parse(rest)),
                 static_cast<int>(HttpParser::Status::Complete));
    MZ_ASSERT_EQ(static_cast<int>(parser.method()), static_cast<int>(HttpParser::Method::Post));
    MZ_ASSERT_STR_EQ(parser.path(), std::string("/upload"));
    MZ_ASSERT_FALSE(parser.keepAlive());   // HTTP/1.0 默认短连接
    MZ_ASSERT_TRUE(parser.reset().empty());
    MZ_ASSERT_EQ(parser.totalParsed(), 2u);
}

MZ_TEST(http_parse_leftover_body) {
    // 头部之后还有 body：头部 Complete，body 作为剩余字节交还（M3 的 POST 不处理，但字节不能丢）
    const std::string req = "POST /x HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
    HttpParser parser;
    MZ_ASSERT_EQ(static_cast<int>(parser.parse(req)),
                 static_cast<int>(HttpParser::Status::Complete));
    const std::string *len = parser.header("content-length");   // 大小写不敏感
    MZ_ASSERT_NOT_NULL(len);
    MZ_ASSERT_STR_EQ(*len, std::string("5"));
    // ★ 结果要在 reset() **之前**读：reset() 会清掉这一次的 path/headers
    const std::string body = parser.reset();
    MZ_ASSERT_STR_EQ(body, std::string("hello"));
    MZ_ASSERT_NULL(parser.header("content-length"));   // reset 之后结果已清空（契约）
}

MZ_TEST(http_parse_path_query_split) {
    HttpParser parser;
    MZ_ASSERT_EQ(static_cast<int>(parser.parse("GET /live/a.flv?x=1&y=2 HTTP/1.1\r\n\r\n")),
                 static_cast<int>(HttpParser::Status::Complete));
    MZ_ASSERT_STR_EQ(parser.path(), std::string("/live/a.flv"));
    MZ_ASSERT_STR_EQ(parser.query(), std::string("x=1&y=2"));
    parser.reset();

    // 只有 '/'、无 query
    MZ_ASSERT_EQ(static_cast<int>(parser.parse("GET / HTTP/1.1\r\n\r\n")),
                 static_cast<int>(HttpParser::Status::Complete));
    MZ_ASSERT_STR_EQ(parser.path(), std::string("/"));
    MZ_ASSERT_TRUE(parser.query().empty());
    parser.reset();

    // query 里有 '?'：只有第一个 '?' 是分隔符
    MZ_ASSERT_EQ(static_cast<int>(parser.parse("GET /a?b=1?2 HTTP/1.1\r\n\r\n")),
                 static_cast<int>(HttpParser::Status::Complete));
    MZ_ASSERT_STR_EQ(parser.path(), std::string("/a"));
    MZ_ASSERT_STR_EQ(parser.query(), std::string("b=1?2"));
}

MZ_TEST(http_parse_headers_lookup_and_duplicates) {
    const std::string req =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "X-Two: first\r\n"
        "x-two: second\r\n"
        "\r\n";
    HttpParser parser;
    MZ_ASSERT_EQ(static_cast<int>(parser.parse(req)),
                 static_cast<int>(HttpParser::Status::Complete));
    MZ_ASSERT_EQ(parser.headerCount(), 3u);
    MZ_ASSERT_EQ(parser.headers().size(), 3u);

    const std::string *host = parser.header("HOST");     // 全大写也能找到
    MZ_ASSERT_NOT_NULL(host);
    MZ_ASSERT_STR_EQ(*host, std::string("example.com"));

    const std::string *dup = parser.header("X-TWO");     // 同名多条 → 返回第一条
    MZ_ASSERT_NOT_NULL(dup);
    MZ_ASSERT_STR_EQ(*dup, std::string("first"));

    MZ_ASSERT_NULL(parser.header("No-Such-Header"));
    MZ_ASSERT_NULL(parser.header(""));
}

MZ_TEST(http_parse_header_value_trim) {
    const std::string req =
        "GET / HTTP/1.1\r\n"
        "Host:   example.com   \r\n"
        "Empty:\r\n"
        "Tabs:\t a \t b \r\n"
        "\r\n";
    HttpParser parser;
    MZ_ASSERT_EQ(static_cast<int>(parser.parse(req)),
                 static_cast<int>(HttpParser::Status::Complete));
    MZ_ASSERT_STR_EQ(*parser.header("Host"), std::string("example.com"));   // 两端 OWS 去掉
    MZ_ASSERT_STR_EQ(*parser.header("Empty"), std::string(""));             // 空值合法
    MZ_ASSERT_STR_EQ(*parser.header("Tabs"), std::string("a \t b"));        // 中间原样保留
    MZ_ASSERT_EQ(parser.headerCount(), 3u);
}

MZ_TEST(http_parse_keepalive_matrix) {
    HttpParser parser;
    // 1.1 默认长连接
    MZ_ASSERT_EQ(static_cast<int>(parser.parse("GET / HTTP/1.1\r\n\r\n")),
                 static_cast<int>(HttpParser::Status::Complete));
    MZ_ASSERT_TRUE(parser.keepAlive());
    parser.reset();
    // 1.1 + close
    MZ_ASSERT_EQ(static_cast<int>(parser.parse("GET / HTTP/1.1\r\nConnection: close\r\n\r\n")),
                 static_cast<int>(HttpParser::Status::Complete));
    MZ_ASSERT_FALSE(parser.keepAlive());
    parser.reset();
    // 1.0 默认短连接
    MZ_ASSERT_EQ(static_cast<int>(parser.parse("GET / HTTP/1.0\r\n\r\n")),
                 static_cast<int>(HttpParser::Status::Complete));
    MZ_ASSERT_FALSE(parser.keepAlive());
    parser.reset();
    // 1.0 + keep-alive（且大小写混合、值带其它 token）
    MZ_ASSERT_EQ(static_cast<int>(parser.parse(
                     "GET / HTTP/1.0\r\nConnection: Keep-Alive, Foo\r\n\r\n")),
                 static_cast<int>(HttpParser::Status::Complete));
    MZ_ASSERT_TRUE(parser.keepAlive());
}

MZ_TEST(http_parse_unknown_method_is_ok) {
    // 不认识的方法**不是解析错误**：解析成功，由服务器回 405/501
    HttpParser parser;
    MZ_ASSERT_EQ(static_cast<int>(parser.parse("FROBNICATE / HTTP/1.1\r\n\r\n")),
                 static_cast<int>(HttpParser::Status::Complete));
    MZ_ASSERT_EQ(static_cast<int>(parser.method()),
                 static_cast<int>(HttpParser::Method::Unsupported));
    MZ_ASSERT_STR_EQ(parser.methodStr(), std::string("FROBNICATE"));
    MZ_ASSERT_EQ(parser.totalRejected(), 0u);
}

// ---------------------------------------------------------------------------
// 畸形（每一条都要能明确指出原因，不能"猜"）
// ---------------------------------------------------------------------------

MZ_TEST(http_parse_bad_request_line) {
    HttpParser parser;
    // 缺版本段
    MZ_ASSERT_EQ(static_cast<int>(parser.parse("GET /\r\n\r\n")),
                 static_cast<int>(HttpParser::Status::Error));
    MZ_ASSERT_EQ(static_cast<int>(parser.error()),
                 static_cast<int>(HttpParser::Error::BadRequestLine));
    MZ_ASSERT_EQ(parser.totalRejected(), 1u);

    parser.reset();
    // 多了一个空格
    MZ_ASSERT_EQ(static_cast<int>(parser.parse("GET  / HTTP/1.1\r\n\r\n")),
                 static_cast<int>(HttpParser::Status::Error));
    MZ_ASSERT_EQ(static_cast<int>(parser.error()),
                 static_cast<int>(HttpParser::Error::BadRequestLine));

    parser.reset();
    // 空 URI
    MZ_ASSERT_EQ(static_cast<int>(parser.parse("GET  HTTP/1.1\r\n\r\n")),
                 static_cast<int>(HttpParser::Status::Error));

    parser.reset();
    // 方法里含非法字符
    MZ_ASSERT_EQ(static_cast<int>(parser.parse("GE(T / HTTP/1.1\r\n\r\n")),
                 static_cast<int>(HttpParser::Status::Error));

    parser.reset();
    // URI 里有空格（会被切成 4 段）
    MZ_ASSERT_EQ(static_cast<int>(parser.parse("GET /a b HTTP/1.1\r\n\r\n")),
                 static_cast<int>(HttpParser::Status::Error));
}

MZ_TEST(http_parse_bad_version) {
    HttpParser parser;
    MZ_ASSERT_EQ(static_cast<int>(parser.parse("GET / HTTP/9.9\r\n\r\n")),
                 static_cast<int>(HttpParser::Status::Error));
    MZ_ASSERT_EQ(static_cast<int>(parser.error()),
                 static_cast<int>(HttpParser::Error::BadVersion));
    parser.reset();
    MZ_ASSERT_EQ(static_cast<int>(parser.parse("GET / HTTP/2.0\r\n\r\n")),
                 static_cast<int>(HttpParser::Status::Error));
    MZ_ASSERT_EQ(static_cast<int>(parser.error()),
                 static_cast<int>(HttpParser::Error::BadVersion));
}

MZ_TEST(http_parse_bad_header_line) {
    HttpParser parser;
    // 缺冒号
    MZ_ASSERT_EQ(static_cast<int>(parser.parse("GET / HTTP/1.1\r\nHost example.com\r\n\r\n")),
                 static_cast<int>(HttpParser::Status::Error));
    MZ_ASSERT_EQ(static_cast<int>(parser.error()),
                 static_cast<int>(HttpParser::Error::BadHeaderLine));
    parser.reset();

    // 名字后带空格（畸形）
    MZ_ASSERT_EQ(static_cast<int>(parser.parse("GET / HTTP/1.1\r\nHost : x\r\n\r\n")),
                 static_cast<int>(HttpParser::Status::Error));
    MZ_ASSERT_EQ(static_cast<int>(parser.error()),
                 static_cast<int>(HttpParser::Error::BadHeaderLine));
    parser.reset();

    // 裸 LF：必须立刻拒（不能等攒到上限才报"超长"）
    MZ_ASSERT_EQ(static_cast<int>(parser.parse("GET / HTTP/1.1\nHost: x\n\n")),
                 static_cast<int>(HttpParser::Status::Error));
    MZ_ASSERT_EQ(static_cast<int>(parser.error()),
                 static_cast<int>(HttpParser::Error::BadHeaderLine));
}

MZ_TEST(http_parse_nul_byte) {
    HttpParser parser;
    std::string req = "GET / HTTP/1.1\r\nHost: ex";
    req.push_back('\0');
    req += "ample\r\n\r\n";
    MZ_ASSERT_EQ(static_cast<int>(parser.parse(req)),
                 static_cast<int>(HttpParser::Status::Error));
    MZ_ASSERT_EQ(static_cast<int>(parser.error()), static_cast<int>(HttpParser::Error::NulByte));
}

// ---------------------------------------------------------------------------
// 超限（"有界"是硬要求：既要拒绝，也要保证缓冲不涨）
// ---------------------------------------------------------------------------

MZ_TEST(http_parse_line_too_long) {
    HttpParser parser;
    // 请求行还没结束就超过 max_request_line（8KB）→ 立刻 LineTooLong，不用等 40KB
    const std::string req = "GET /" + longString(9 * 1024, 'a');
    MZ_ASSERT_EQ(static_cast<int>(parser.parse(req)),
                 static_cast<int>(HttpParser::Status::Error));
    MZ_ASSERT_EQ(static_cast<int>(parser.error()),
                 static_cast<int>(HttpParser::Error::LineTooLong));
    // 真性质：缓冲**有界**（不超过 硬上限 = max_header_bytes + max_request_line），
    // 且判错之后**不再增长**（再喂多少都不收）
    const size_t held = parser.bufferedBytes();
    MZ_ASSERT_LE(held, 32u * 1024 + 8u * 1024);
    parser.parse(longString(64 * 1024, 'c'));
    MZ_ASSERT_EQ(parser.bufferedBytes(), held);
    MZ_ASSERT_EQ(parser.totalRejected(), 1u);
}

MZ_TEST(http_parse_uri_too_long) {
    HttpParser parser;
    const std::string req = "GET /" + longString(5 * 1024, 'b') + " HTTP/1.1\r\n\r\n";
    MZ_ASSERT_EQ(static_cast<int>(parser.parse(req)),
                 static_cast<int>(HttpParser::Status::Error));
    MZ_ASSERT_EQ(static_cast<int>(parser.error()),
                 static_cast<int>(HttpParser::Error::UriTooLong));
}

MZ_TEST(http_parse_headers_too_large) {
    HttpParser parser;
    std::string req = "GET / HTTP/1.1\r\n";
    while (req.size() < 40 * 1024) {   // 远大于 max_header_bytes(32KB)
        req += "X-Filler: 0123456789012345678901234567890123456789\r\n";
    }
    req += "\r\n";
    MZ_ASSERT_EQ(static_cast<int>(parser.parse(req)),
                 static_cast<int>(HttpParser::Status::Error));
    MZ_ASSERT_EQ(static_cast<int>(parser.error()),
                 static_cast<int>(HttpParser::Error::HeadersTooLarge));
    // 硬上限是 max_header_bytes + max_request_line；缓冲绝不能到 40KB
    MZ_ASSERT_LE(parser.bufferedBytes(), 32u * 1024 + 8u * 1024);
}

MZ_TEST(http_parse_too_many_headers) {
    HttpParser parser;
    std::string req = "GET / HTTP/1.1\r\n";
    for (int i = 0; i < 101; ++i) {   // max_headers = 100
        req += "X-N: 1\r\n";
    }
    req += "\r\n";
    MZ_ASSERT_EQ(static_cast<int>(parser.parse(req)),
                 static_cast<int>(HttpParser::Status::Error));
    MZ_ASSERT_EQ(static_cast<int>(parser.error()),
                 static_cast<int>(HttpParser::Error::TooManyHeaders));
}

MZ_TEST(http_parse_garbage_never_crashes) {
    // 没有 CRLFCRLF 的随机字节：只允许 NeedMoreData，直到触到上限才 Error —— 不许崩、不许猜
    HttpParser parser;
    std::string junk;
    for (int i = 0; i < 1000; ++i) {
        junk.push_back(static_cast<char>(i % 251 + 1));   // 无 '\0'，且不含 CRLF 组合
    }
    const HttpParser::Status st = parser.parse(junk);
    MZ_ASSERT_TRUE(static_cast<int>(st) == static_cast<int>(HttpParser::Status::NeedMoreData) ||
                   static_cast<int>(st) == static_cast<int>(HttpParser::Status::Error));
    if (static_cast<int>(st) == static_cast<int>(HttpParser::Status::Error)) {
        // 若判了错，必须给出具体原因（不能是 None）
        MZ_ASSERT_NE(static_cast<int>(parser.error()), static_cast<int>(HttpParser::Error::None));
    }
}
