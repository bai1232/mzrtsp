/*
 * RTSP 输入（M11-a1，分组 `rtsp`）—— 只测**不依赖网络**的部分
 * ============================================================================
 * 真正的"连上/拉流/断流"在 `scripts/rtsp_test.sh`（用假 RTSP 服务端注入场景）。
 * 这里只锁三件纯逻辑：
 *   ① 网络源判定（`rtsp://` / `rtsps://`）—— 决定"要不要走 RTSP 分支"；
 *   ② 凭据脱敏 —— 它决定**密码会不会进日志/统计**（泄露一次就够呛）；
 *   ③ 没开 `--rtsp` 时，拿到 `rtsp://` 源必须**明确拒绝**（不静默联网）。
 * 三者都是"错了不会崩、但会悄悄泄露或悄悄联网"的类型，所以单独成组。
 * ============================================================================
 */

#include "test_main.h"

#include "core/util.h"
#include "media/demuxer_producer.h"

#include <string>

using namespace mzmedia;

// ---------------------------------------------------------------------------
// ① URL 判定
// ---------------------------------------------------------------------------

MZ_TEST(rtsp_network_url_detection) {
    // 只认这两种 scheme（rtsps 是 RTSP over TLS）
    MZ_ASSERT_TRUE(isNetworkMediaUrl("rtsp://127.0.0.1:8554/live"));
    MZ_ASSERT_TRUE(isNetworkMediaUrl("rtsps://cam.example/stream"));
    // 本地路径 / 其它 scheme 都不算网络源（否则会把文件路径送进 RTSP 分支）
    MZ_ASSERT_FALSE(isNetworkMediaUrl("samples/sample.mp4"));
    MZ_ASSERT_FALSE(isNetworkMediaUrl("/abs/path/sample.h264"));
    MZ_ASSERT_FALSE(isNetworkMediaUrl("http://example/x.mp4"));
    MZ_ASSERT_FALSE(isNetworkMediaUrl("rtmp://example/live")); // 未支持：不冒充
    MZ_ASSERT_FALSE(isNetworkMediaUrl(""));
    // 前缀判定必须是"开头就是"（中间出现 rtsp:// 不算）
    MZ_ASSERT_FALSE(isNetworkMediaUrl("samples/rtsp://x"));
}

// ---------------------------------------------------------------------------
// ② 凭据脱敏
// ---------------------------------------------------------------------------

MZ_TEST(rtsp_credentials_are_redacted) {
    // 有密码：只留用户名，密码替换为 ***
    MZ_ASSERT_STR_EQ(redactMediaUrl("rtsp://user:secret123@127.0.0.1:8554/live"),
                     "rtsp://user:***@127.0.0.1:8554/live");
    // 只有用户名没有密码：连用户名一起隐掉（账号也是敏感信息）
    MZ_ASSERT_STR_EQ(redactMediaUrl("rtsp://user@127.0.0.1:8554/live"),
                     "rtsp://***@127.0.0.1:8554/live");
    // 没有凭据：一个字符都不改
    MZ_ASSERT_STR_EQ(redactMediaUrl("rtsp://127.0.0.1:8554/live"),
                     "rtsp://127.0.0.1:8554/live");
    // 本地路径：原样返回（脱敏函数对文件路径必须是 no-op，否则会改坏日志）
    MZ_ASSERT_STR_EQ(redactMediaUrl("samples/sample.mp4"), "samples/sample.mp4");
    MZ_ASSERT_STR_EQ(redactMediaUrl(""), "");
    // 密码里带 @ 的 URL（不太合法但真实存在）：以**第一个 @** 为界，密码整体隐掉
    MZ_ASSERT_STR_EQ(redactMediaUrl("rtsp://u:p@ss@host/x"), "rtsp://u:***@ss@host/x");
    // rtsps 同样处理
    MZ_ASSERT_STR_EQ(redactMediaUrl("rtsps://user:pw@cam/x"), "rtsps://user:***@cam/x");
}

// ---------------------------------------------------------------------------
// ③ 没开 --rtsp → 明确拒绝（不静默联网）
// ---------------------------------------------------------------------------

MZ_TEST(rtsp_source_rejected_when_disabled) {
    // 默认配置：rtsp.enabled = false
    DemuxerProducer::Config config;
    MZ_ASSERT_FALSE(config.rtsp.enabled);
    DemuxerProducer producer(config);
    // 注意：这里**故意**用一个不可达的 URL —— 如果实现"先尝试连接再判断开关"，
    // 这个用例会挂住直到超时（那就说明开关判晚了）。
    const bool opened = producer.open("rtsp://user:secret123@127.0.0.1:1/never");
    MZ_ASSERT_FALSE(opened);
    const std::string err = producer.lastError();
    MZ_ASSERT_TRUE(err.find("--rtsp") != std::string::npos);   // 说清"怎么开"
    MZ_ASSERT_TRUE(err.find("RTSP") != std::string::npos);
    // ★ 错误信息里**不许**出现密码（它会被 HTTP 响应带出去给客户端）
    MZ_ASSERT_TRUE(err.find("secret123") == std::string::npos);
    MZ_ASSERT_TRUE(err.find("***") != std::string::npos);
    MZ_ASSERT_FALSE(producer.opened());
    MZ_ASSERT_FALSE(producer.networkSource()); // 拒绝之后不留"这是网络源"的半状态
}
