#include "output/udp_port_pool.h"

#include "core/logger.h"

namespace mzmedia {

UdpPortPool::Ptr UdpPortPool::create(const Config &config, std::string *error) {
    auto fail = [error](const char *why) {
        if (error != nullptr) {
            *error = why;
        }
        return Ptr();
    };
    if (config.pairs == 0) {
        return fail("udp pairs=0（0 不是「无限」，而是不启用 UDP）");
    }
    if (config.pairs > 2048) {
        return fail("udp pairs 超过硬上限 2048（一个池占用 2×pairs 个 fd）");
    }
    if (config.port_base < 1024) {
        return fail("udp 端口基址 < 1024（需要 root）");
    }
    const uint32_t span = static_cast<uint32_t>(config.pairs) * 2u;
    if (static_cast<uint32_t>(config.port_base) + span > 65535u) {
        return fail("udp 端口范围越过 65535");
    }

    auto pool = Ptr(new UdpPortPool());
    pool->_pairs.reserve(config.pairs);
    uint16_t port = config.port_base;
    // 逐对尝试：某个端口被系统里别的进程占了就往后挪，绝不"拿到半个"
    while (pool->_pairs.size() < config.pairs && static_cast<uint32_t>(port) + 1u <= 65535u) {
        Socket::Ptr rtp = Socket::create(Socket::Domain::IPv4, SOCK_DGRAM);
        if (!rtp || !rtp->setNonBlock(true) || !rtp->bind(config.bind_ip, port)) {
            port = static_cast<uint16_t>(port + 2);
            continue;
        }
        Socket::Ptr rtcp = Socket::create(Socket::Domain::IPv4, SOCK_DGRAM);
        if (!rtcp || !rtcp->setNonBlock(true) || !rtcp->bind(config.bind_ip, static_cast<uint16_t>(port + 1))) {
            port = static_cast<uint16_t>(port + 2); // 半对不要：直接放弃这一对
            continue;
        }
        Pair pair;
        pair.rtp = rtp;
        pair.rtcp = rtcp;
        pair.rtp_port = port;
        pair.rtcp_port = static_cast<uint16_t>(port + 1);
        pool->_pairs.push_back(pair);
        port = static_cast<uint16_t>(port + 2);
    }
    if (pool->_pairs.empty()) {
        return fail("udp 端口一对都没绑上（端口范围被占用？）");
    }
    if (pool->_pairs.size() < config.pairs) {
        WarnL << "UdpPortPool：只绑到 " << pool->_pairs.size() << "/" << config.pairs
              << " 对（端口范围被占用），按实际数量服务";
    }
    InfoP("UdpPortPool: 就绪（%zu 对，端口 %u-%u，绑定 %s）", pool->_pairs.size(),
          static_cast<unsigned>(pool->_pairs.front().rtp_port),
          static_cast<unsigned>(pool->_pairs.back().rtcp_port), config.bind_ip.c_str());
    return pool;
}

UdpPortPool::Pair *UdpPortPool::acquire() {
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto &pair : _pairs) {
        if (!pair.in_use) {
            pair.in_use = true;
            return &pair;
        }
    }
    ++_exhausted;
    return nullptr;
}

void UdpPortPool::release(Pair *pair) {
    if (pair == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    pair->in_use = false; // 幂等：重复归还无害（socket 一直开着，端口不还系统）
}

size_t UdpPortPool::used() const {
    std::lock_guard<std::mutex> lock(_mutex);
    size_t count = 0;
    for (const auto &pair : _pairs) {
        if (pair.in_use) {
            ++count;
        }
    }
    return count;
}

} // namespace mzmedia
