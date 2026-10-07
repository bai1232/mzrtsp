/*
 * FlvSender：把一个订阅者的队列封成 FLV 并写出去（M6-b）
 * ============================================================================
 * 形状来源：docs/DESIGN_M6.md §3.4；上位需求 `docs/SPEC.md`
 *   FR-3.1「连接建立后**立即**发送 FLV Header + AVC sequence header，随后持续推流」
 *   FR-3.4「时间戳转换到输出基准（FLV → 毫秒）」；`NFR-1` 首帧 < 1s
 *
 * 三条设计约束：
 *   1) **不依赖 `http/`**：写出目标抽象成 `Sink`（`write` + `end`）。好处是它可以用**假 sink**
 *      做确定性单测；HTTP 那侧只负责把 `HttpResponse` 的 chunked 出口适配过来。
 *   2) **每个连接一个实例**：`FlvMuxer` 持有该客户端的时间戳基准，且 sequence header 必须
 *      每个连接重发（DESIGN_M6.md §4.5）。
 *   3) **生命周期挂在连接上**：调用方（路由处理器）要把它交给 `HttpResponse::holdResource()`，
 *      由会话在连接关闭时释放 —— 这样 drain 回调里的 `weak_ptr` 会自然失效，
 *      **不会在连接已断之后还去写 socket（UAF）**，也保证断开即退订（NFR-6）。
 *
 * 谁在什么时候调它：
 *   · `start()`   —— 路由处理器内（poller 线程），**先发 header + sequence header 再返回**
 *     （FR-3.1 的"立即"就靠这个；也顺带把首帧时间压到 1s 内）
 *   · `onDrain()` —— 订阅者的 drain 回调里（同一个 poller 线程），把队列取空并写出去
 * ============================================================================
 */

#pragma once

#include "media/media_source.h"
#include "output/flv_muxer.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace mzmedia {

class FlvSender : public std::enable_shared_from_this<FlvSender> {
public:
    using Ptr = std::shared_ptr<FlvSender>;

    /// 写出目标（HTTP 侧由 `HttpResponse::sender()` / `endStreamFn()` / `abortFn()` 适配）
    struct Sink {
        /// @return false = 连接已坏（不要再写）
        std::function<bool(const char *data, size_t len)> write;
        /// 流正常结束时的收尾：发 chunked 结束块 + 等排空再关连接
        std::function<void()> end;
        /**
         * 【M6-c】异常终止：**主动断开**（`Aborted` / `SinkFailed` 时调用，最多一次）
         * @note 为什么不复用 `end`：`end` 的语义是"流正常结束"（发结束块，客户端会认为
         *       收到了完整数据）。而 `Aborted`/`SinkFailed` 意味着**流是残缺的** ——
         *       用 `end` 收尾等于替客户端把"丢帧/被掐断"这件事掩盖掉。
         *       所以这里要求一个**硬断连**出口（HTTP 侧 = Session::shutdown，不发结束块）：
         *       客户端会明确报"传输被截断"，这才是事实。
         */
        std::function<void()> abort;
    };

    enum class Result : uint8_t {
        Ok = 0,
        BadStreams = 1,  // 编码/初始化数据不满足（`FlvMuxer::prepare()` 失败）
        SinkFailed = 2,  // 写出去失败（连接已坏）
        NotStarted = 3,  // 没 start() 就 onDrain()
        Aborted = 4,     // 订阅者已 broken：主动断开
    };

    /// @return nullptr = 流参数不满足 FLV 封装（原因已记日志，见 `FlvMuxer::resultName`）
    static Ptr create(const Sink &sink, FlvMuxer::Streams streams, Subscriber::Ptr subscriber);

    /// 发 FLV Header + AVC/AAC sequence header（FR-3.1：**立即**发，不等第一帧），
    /// 然后**立刻搬一次队列**（中途接入的 GOP 缓存 / "源已结束"都要马上处理，见 .cpp 说明）
    Result start();
    /// 把订阅者队列里的包全部封成 tag 写出去；源 EOS 时顺带收尾。**由 drain 回调调用**
    Result onDrain();

    bool finished() const {
        return _finished;
    }
    bool aborted() const {
        return _aborted;
    }
    std::string lastError() const {
        return _last_error;
    }
    static const char *resultName(Result result);

    // 观测
    uint64_t packetsMuxed() const {
        return _packets;
    }
    uint64_t bytesWritten() const {
        return _bytes;
    }
    uint32_t lastTimestampMs() const {
        return _muxer.lastTimestampMs();
    }

private:
    FlvSender(const Sink &sink, FlvMuxer::Streams streams, Subscriber::Ptr subscriber);
    Result fail(Result result, const std::string &message);
    /// 通知"连接不该继续了"（幂等：最多调用一次 sink.abort）
    void notifyAbort();
    /// @return false = 写失败（已置 `_aborted`）
    bool flushBuffer(std::string *buffer);

    /// 攒到这个大小就写一块：太小会让 chunk 数暴涨，太大会拉高首帧延迟
    static constexpr size_t kFlushThreshold = 32u * 1024u;

    Sink _sink;
    FlvMuxer _muxer;
    Subscriber::Ptr _subscriber;
    bool _started = false;
    bool _finished = false;
    bool _aborted = false;
    bool _abort_notified = false;
    std::string _last_error;
    uint64_t _packets = 0;
    uint64_t _bytes = 0;
};

} // namespace mzmedia
