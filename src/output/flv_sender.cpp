/*
 * FlvSender 实现（M6-b）
 * ============================================================================
 * 一次 `onDrain()` 做四件事，顺序不能换：
 *   1) 把订阅者队列**取空**（一次 drain 尽量多搬，减少唤醒次数）；
 *   2) 每帧封成 FLV tag 追加到本地缓冲；
 *   3) 缓冲攒到 kFlushThreshold 就写一块（别一帧一个 chunk）；
 *   4) 收尾判断：**broken → 断开**、**EOS → 发结束块并置 finished**。
 *
 * 为什么 broken 要主动断：`Subscriber::broken()` 的含义是"不可丢的包（视频关键帧）
 * 已经进不去队列了" —— 继续发出去的流会缺关键帧，播放端只会花屏。断掉更诚实。
 *
 * 【M6-c】"断掉"从"只记一个 Result"变成**真的断**：`fail()` 在 `Aborted`/`SinkFailed`
 *   时会调用 `sink.abort()`（最多一次），由 HTTP 侧执行硬关连接 —— 在此之前只发了日志，
 *   客户端会一直挂在半截流上（M6-b 遗留，M6-c 补齐）。
 * ============================================================================
 */

#include "output/flv_sender.h"

#include "core/logger.h"

#include <utility>

namespace mzmedia {

FlvSender::Ptr FlvSender::create(const Sink &sink, FlvMuxer::Streams streams,
                                 Subscriber::Ptr subscriber) {
    if (!sink.write) {
        ErrorP("FlvSender::create 失败：sink.write 为空");
        return nullptr;
    }
    if (!subscriber) {
        ErrorP("FlvSender::create 失败：subscriber 为空");
        return nullptr;
    }
    Ptr sender(new FlvSender(sink, streams, std::move(subscriber)));
    const FlvMuxer::Result prepared = sender->_muxer.prepare();
    if (prepared != FlvMuxer::Result::Ok) {
        ErrorP("FlvSender::create 失败：%s —— %s", FlvMuxer::resultName(prepared),
               sender->_muxer.lastError().c_str());
        return nullptr;
    }
    // **自己注册 drain 回调**：sender 持有订阅者，而回调只持**弱引用** ——
    // 既不与订阅者成环（成环就永远释放不掉），也不需要调用方拿到订阅者句柄
    // （调用方把订阅者移交给 sender 之后就取不到了，让调用方注册反而别扭）
    std::weak_ptr<FlvSender> weak = sender;
    sender->_subscriber->setDrainCallback([weak](const Subscriber::Ptr &) {
        if (auto self = weak.lock()) {
            (void) self->onDrain();
        }
    });
    return sender;
}

FlvSender::FlvSender(const Sink &sink, FlvMuxer::Streams streams, Subscriber::Ptr subscriber)
    : _sink(sink)
    , _muxer(streams)
    , _subscriber(std::move(subscriber)) {}

const char *FlvSender::resultName(Result result) {
    switch (result) {
    case Result::Ok:
        return "Ok";
    case Result::BadStreams:
        return "BadStreams";
    case Result::SinkFailed:
        return "SinkFailed";
    case Result::NotStarted:
        return "NotStarted";
    case Result::Aborted:
        return "Aborted";
    }
    return "Unknown";
}

FlvSender::Result FlvSender::fail(Result result, const std::string &message) {
    _last_error = message;
    WarnP("FlvSender: %s（%s）", resultName(result), message.c_str());
    // M6-c：连接已经不能用（写失败）或不该继续（订阅者 broken）→ 让 HTTP 侧**真的断开**。
    // 不在这里吞掉：只发一个 Warn 而不关连接，客户端会一直挂在半截流上（最糟的那种"假活着"）
    if (result == Result::SinkFailed || result == Result::Aborted) {
        notifyAbort();
    }
    return result;
}

void FlvSender::notifyAbort() {
    if (_abort_notified) {
        return; // 幂等：drain 可能被多调一次，断连只能发一次
    }
    _abort_notified = true;
    if (_sink.abort) {
        _sink.abort();
    }
}

bool FlvSender::flushBuffer(std::string *buffer) {
    if (buffer == nullptr || buffer->empty()) {
        return true;
    }
    if (!_sink.write(buffer->data(), buffer->size())) {
        _aborted = true;
        _last_error = "写出失败：连接已坏";
        buffer->clear();
        return false;
    }
    _bytes += buffer->size();
    buffer->clear();
    return true;
}

FlvSender::Result FlvSender::start() {
    std::string out;
    (void) _muxer.writeHeader(&out);
    const FlvMuxer::Result sequence = _muxer.writeSequenceHeaders(&out);
    if (sequence != FlvMuxer::Result::Ok) {
        return fail(Result::BadStreams, _muxer.lastError());
    }
    if (!flushBuffer(&out)) {
        return fail(Result::SinkFailed, _last_error);
    }
    _started = true;

    // 【M6-c 修的 bug】立刻搬一次队列。两个理由：
    //   1) 订阅时源**可能已经结束**：`MediaSource::subscribe()` 会给新订阅者的队列
    //      `markEndOfStream()`，但那一刻 drain 回调还没注册（它由 create() 注册），
    //      所以**不会有任何唤醒** —— 不主动搬一次，这一路连接就会挂着不动，
    //      客户端看到的是"连上了、头也发了、然后永远黑屏"（最糟的假活着）。
    //      这正是验收脚本 `scripts/flv_http_test.sh` §9 抓到的（源结束后再拉同一路）；
    //   2) 中途接入拿到的 GOP 缓存应当立刻发出去（首帧延迟，NFR-1）。
    return onDrain();
}

FlvSender::Result FlvSender::onDrain() {
    if (!_started) {
        return fail(Result::NotStarted, "start() 之前不能发数据");
    }
    if (_finished || _aborted) {
        return Result::Ok; // 已经收尾：不再动（幂等，drain 可能被多调一次）
    }

    std::string buffer;
    MediaPacket::Ptr packet;
    while (_subscriber->queue().pop(&packet) == FrameQueue::PopResult::Packet) {
        const FlvMuxer::Result muxed = _muxer.writePacket(*packet, &buffer);
        if (muxed != FlvMuxer::Result::Ok) {
            return fail(Result::BadStreams, _muxer.lastError());
        }
        ++_packets;
        if (buffer.size() >= kFlushThreshold && !flushBuffer(&buffer)) {
            return fail(Result::SinkFailed, _last_error);
        }
    }
    if (!flushBuffer(&buffer)) {
        return fail(Result::SinkFailed, _last_error);
    }

    if (_subscriber->broken()) {
        // 关键帧都进不去了：继续发只会让对端花屏，明确断开（不计入 finished）
        return fail(Result::Aborted, "订阅者已 broken（不可丢的包进不去）→ 断开该客户端");
    }

    if (_subscriber->queue().endOfStream()) {
        _finished = true;
        if (_sink.end) {
            _sink.end(); // 发结束块 + 等排空再关连接（由 HTTP 侧注入）
        }
    }
    return Result::Ok;
}

} // namespace mzmedia
