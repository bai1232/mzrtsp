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
#include "media/reconnect_policy.h"

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

// ---------------------------------------------------------------------------
// ④ 【M11-a2】重连退避曲线（纯逻辑）
// ---------------------------------------------------------------------------

MZ_TEST(rtsp_reconnect_policy_backoff_sequence) {
    // 契约：1s → 2s → 4s → 8s → 16s → 封顶 30s（之后一直 30s），读到包才复位。
    // 什么会让它红：曲线写错（比如线性、或没封顶）、复位没做、上限判定错。
    ReconnectPolicy::Config config;
    config.initial_ms = 1000;
    config.max_ms = 30000;
    config.max_attempts = 0; // 不设上限
    ReconnectPolicy policy;
    MZ_ASSERT_TRUE(policy.setConfig(config));

    const int64_t expect[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000};
    for (int i = 0; i < 8; ++i) {
        MZ_ASSERT_EQ(policy.nextDelayMs(), expect[i]);
        MZ_ASSERT_EQ(policy.attempts(), static_cast<uint64_t>(i + 1));
    }
    // 成功一次 → 退避复位（下一次又从 initial 开始）
    policy.onSuccess();
    MZ_ASSERT_EQ(policy.attempts(), 0u);
    MZ_ASSERT_EQ(policy.nextDelayMs(), 1000);
    // nextDelayIfFailedMs 与实际下一次一致（stats 展示用，不能与实际行为不符）
    const int64_t peek = policy.nextDelayIfFailedMs();
    MZ_ASSERT_EQ(peek, 2000);
    MZ_ASSERT_EQ(policy.nextDelayMs(), peek);
}

MZ_TEST(rtsp_reconnect_policy_attempt_cap_and_bad_config) {
    ReconnectPolicy::Config config;
    config.initial_ms = 500;
    config.max_ms = 2000;
    config.max_attempts = 3;
    ReconnectPolicy policy;
    MZ_ASSERT_TRUE(policy.setConfig(config));
    MZ_ASSERT_EQ(policy.nextDelayMs(), 500);   // 第 1 次
    MZ_ASSERT_EQ(policy.nextDelayMs(), 1000);  // 第 2 次
    MZ_ASSERT_EQ(policy.nextDelayMs(), 2000);  // 第 3 次（封顶）
    MZ_ASSERT_EQ(policy.nextDelayMs(), -1);    // 次数用完 → 调用方必须放弃
    MZ_ASSERT_EQ(policy.nextDelayIfFailedMs(), -1);

    // 非法配置一律**拒绝并保持原值**（0 不是"立即"）
    const ReconnectPolicy::Config good = config;
    ReconnectPolicy::Config bad = good;
    bad.initial_ms = 0;
    MZ_ASSERT_FALSE(policy.setConfig(bad));
    MZ_ASSERT_EQ(policy.config().initial_ms, good.initial_ms);
    bad = good;
    bad.max_ms = 100; // < initial_ms
    MZ_ASSERT_FALSE(policy.setConfig(bad));
    bad = good;
    bad.max_attempts = -1;
    MZ_ASSERT_FALSE(policy.setConfig(bad));
    bad = good;
    bad.max_ms = 400000; // 超硬上限 300000
    MZ_ASSERT_FALSE(policy.setConfig(bad));
    MZ_ASSERT_TRUE(policy.setConfig(good)); // 合法值仍可设
}
