/*
 * UdpPortPool：RTSP/UDP 的**预绑定端口池**（每会话一对：RTP + RTCP）
 * ============================================================================
 * 为什么预绑定（而不是 SETUP 时按需 bind）：
 *   · 端口连续、可写成防火墙白名单；
 *   · 运行期没有 bind 抖动；池大小 = fd 上限，**账是算得出来的**：
 *         fd 预算 = 基础(20) + max_connections + 2 × 池对数
 *     启动期校验一次，不够就拒绝启动（不在运行期才发现 fd 耗尽）。
 *
 * 语义约定（与项目其它上限一致）：
 *   · `pairs == 0` 不是"无限"，而是**不启用 UDP**（客户端 SETUP 会收到 461）；
 *   · 取不到 = 池满 → 调用方回 **503 Service Unavailable**（资源临时不足），
 *     **不要**用 461 冒充（461 的语义是"传输方式不支持"）。
 *
 * 线程安全：acquire/release 加锁（SETUP 在各自连接的 poller 线程上跑）。
 * ============================================================================
 */

#pragma once

#include "network/socket.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mzmedia {

class UdpPortPool {
public:
    using Ptr = std::shared_ptr<UdpPortPool>;

    struct Config {
        size_t pairs = 0;         ///< 池里几对（0 = 不启用）
        uint16_t port_base = 20000; ///< `[base, base + 2*pairs)` 逐对尝试（偶数起、落空就跳过）
        std::string bind_ip = "0.0.0.0";
    };

    /// 一对端口（RTP 在偶数位、RTCP 紧随其后，符合 RFC 3551 约定）
    struct Pair {
        Socket::Ptr rtp;
        Socket::Ptr rtcp;
        uint16_t rtp_port = 0;
        uint16_t rtcp_port = 0;
        bool in_use = false;
    };

    /// @param error 失败原因（端口全被占、参数非法）
    /// @return nullptr = 一对都绑不上（明确失败）
    static Ptr create(const Config &config, std::string *error);

    /// 租一对；@return nullptr = 池满（调用方回 503）或未启用（调用方回 461）
    Pair *acquire();

    /// 归还（幂等：重复归还只算一次）
    void release(Pair *pair);

    size_t size() const {
        return _pairs.size();
    }
    size_t used() const;
    uint64_t exhausted() const {
        return _exhausted.load();
    }

private:
    UdpPortPool() = default;

    std::vector<Pair> _pairs;
    mutable std::mutex _mutex;
    std::atomic<uint64_t> _exhausted{0};
};

} // namespace mzmedia
