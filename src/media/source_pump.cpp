/*
 * SourcePump 实现（M5-b）
 * ============================================================================
 * 线程规则（照 ARCHITECTURE.md §3）：
 *   - 本线程**允许**随输入 IO 阻塞，但**不允许**把阻塞传给别人：事件循环一行都不碰；
 *   - 推包只通过 `MediaSource::pushPacket`（它会顺手做合并唤醒，跨线程投递交给 EventPoller）；
 *   - 退出前一律广播 EOS，保证消费者不会永久等待。
 * ============================================================================
 */

#include "media/source_pump.h"

#include "core/logger.h"

#include <utility>

namespace mzmedia {

SourcePump::~SourcePump() {
    (void) stop();
}

bool SourcePump::start(ReadFn read, MediaSource::Ptr source) {
    if (_running.load()) {
        WarnL << "SourcePump::start 被拒：已经在运行";
        return false;
    }
    if (!read) {
        WarnL << "SourcePump::start 被拒：read 回调为空";
        return false;
    }
    if (!source) {
        WarnL << "SourcePump::start 被拒：MediaSource 为空";
        return false;
    }
    // 回收上一次已经结束、但还没 join 的线程（否则 std::thread 析构会 terminate）
    if (_thread.joinable()) {
        _thread.join();
    }

    _stop_requested.store(false);
    _eof.store(false);
    _read_count.store(0);
    _pushed_count.store(0);
    _throttle.reset(); // 节流基准：每次 start 重新对表（FR-3.5）
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _last_error.clear();
    }

    _running.store(true);
    _thread = std::thread(&SourcePump::run, this, std::move(read), std::move(source));
    return true;
}

bool SourcePump::stop() {
    const bool was_running = _running.load();
    _stop_requested.store(true);
    if (_thread.joinable()) {
        // ReadFn 必须能被打断（见头文件的中断契约）；否则这里会一直等
        _thread.join();
    }
    if (!was_running) {
        return false;
    }
    _running.store(false);
    return true;
}

std::string SourcePump::lastError() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _last_error;
}

void SourcePump::run(ReadFn read, MediaSource::Ptr source) {
    while (!_stop_requested.load()) {
        MediaPacket::Ptr packet;
        std::string error;
        const ReadResult result = read(&packet, &error);

        if (result == ReadResult::EndOfStream) {
            // "被停"不算"读完"：只有**没有收到停止请求**时才认定是自然结束，
            // 否则调用方无法区分"文件读完了"与"我把它掐了"
            if (!_stop_requested.load()) {
                _eof.store(true);
            }
            break;
        }
        if (result == ReadResult::Error) {
            {
                std::lock_guard<std::mutex> lock(_mutex);
                _last_error = error.empty() ? "读取失败（调用方未提供原因）" : error;
            }
            WarnL << "SourcePump 读取失败：" << lastError();
            break;
        }
        if (!packet) {
            // 说好返回 Packet 却给空包 —— 继续循环会变成"死循环 + 什么都不做"，
            // 那种静默比直接失败难查得多
            {
                std::lock_guard<std::mutex> lock(_mutex);
                _last_error = "ReadFn 返回 Packet 但包为空（调用方 bug）";
            }
            ErrorL << "SourcePump 停止：" << lastError();
            break;
        }

        // 节流（FR-3.5）：按 dts 与墙钟对齐；被中止就立刻退出（不再读下一包）
        if (!_throttle.pace(packet->dtsMs(), _stop_requested)) {
            break;
        }

        ++_read_count;
        (void) source->pushPacket(std::move(packet));
        ++_pushed_count;
    }

    // 不管怎么退出，都要让消费者知道"不会再有数据了"
    (void) source->endOfStream();
    _running.store(false);
}

} // namespace mzmedia
