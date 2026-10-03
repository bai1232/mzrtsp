/*
 * SourcePump：源线程 —— 反复向"包来源"要包，推给 MediaSource（M5-b）
 * ============================================================================
 * 形状来源：docs/DESIGN_M5.md §3.5；上位依据 ARCHITECTURE.md §3
 *   「源线程 / 转码线程：每源 1 个（v0.1）；demux 读包、remux/transcode、投递到订阅者队列；
 *     允许随输入 IO 阻塞」以及「绝不阻塞事件循环」。
 *
 * 为什么要单独一个类、而不是把线程塞进 MediaSource：
 *   MediaSource 是**纯策略**（上限、丢帧、GOP），它必须能在没有线程的情况下被确定性测试；
 *   线程、IO、中断是另一件事。分开之后，本类的测试可以**完全控制"什么时候出一个包"**
 *   （ReadFn 由用例提供），不需要样本文件、不需要 sleep 去凑时序。
 *
 * 中断契约（重要）：`stop()` 会 join 线程，所以 ReadFn **必须能被打断**。
 *   做法就是定期检查 `stopRequested()`。真实实现（M5-c 接 Demuxer）把它接到 FFmpeg 的
 *   `interrupt_callback` 上 —— M4 已经用同一机制做超时中断。
 *
 * 结束语义：无论正常读完、读失败还是被停，退出前都会给 MediaSource 广播一次 EOS。
 *   消费者必须能区分"暂时没有"与"不会再有"，否则会永远等下去。
 * ============================================================================
 */

#pragma once

#include "media/media_source.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace mzmedia {

class SourcePump {
public:
    enum class ReadResult : uint8_t {
        Packet = 0,      // 拿到一个包
        EndOfStream = 1, // 正常结束（文件读完）
        Error = 2,       // 失败（打不开 / 读错）
    };

    /**
     * 读一个包
     * @param packet 出参；返回 Packet 时必须写入（写入空包会被判为调用方 bug 并停下来）
     * @param error Error 时应当写入原因（不许只给错误码、不给原因）
     */
    using ReadFn = std::function<ReadResult(MediaPacket::Ptr *packet, std::string *error)>;

    SourcePump() = default;
    ~SourcePump(); // 析构 = 停止 + join：绝不允许留下裸线程

    SourcePump(const SourcePump &) = delete;
    SourcePump &operator=(const SourcePump &) = delete;

    /// @return false = 已在运行 / read 为空 / source 为空
    bool start(ReadFn read, MediaSource::Ptr source);

    /// 请求停止并 join。@return true = 确实停掉了一个**正在运行**的线程；
    ///         false = 本来就没在运行（已经自然结束/从未启动，但线程已回收）
    bool stop();

    bool running() const {
        return _running.load();
    }
    /// 供 ReadFn 查询：真实实现把它接到 Demuxer 的 interrupt_callback
    bool stopRequested() const {
        return _stop_requested.load();
    }
    /// 是否因为**正常读完**而结束（错误结束时为 false —— 与 DESIGN_M4 的 EOF/Error 分离同一原则）
    bool eof() const {
        return _eof.load();
    }
    /// 最近一次失败原因（空串 = 没有失败）
    std::string lastError() const;

    uint64_t readCount() const {
        return _read_count.load();
    }
    uint64_t pushedCount() const {
        return _pushed_count.load();
    }

private:
    void run(ReadFn read, MediaSource::Ptr source);

    std::thread _thread;
    std::atomic<bool> _running{false};
    std::atomic<bool> _stop_requested{false};
    std::atomic<bool> _eof{false};
    std::atomic<uint64_t> _read_count{0};
    std::atomic<uint64_t> _pushed_count{0};

    mutable std::mutex _mutex; // 保护 _last_error
    std::string _last_error;
};

} // namespace mzmedia
