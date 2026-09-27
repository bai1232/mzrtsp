/*
 * logger 单元测试
 *
 * 注意：Logger 是全局单例，用例之间会互相影响，因此每个用例都用 LoggerGuard
 * 在进入和退出时把状态恢复成默认值（清空 writer、异步、队列 4096、级别 Info）。
 */

#include "test_main.h"

#include "core/logger.h"
#include "core/util.h"

#include <dirent.h>
#include <unistd.h>

#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

using namespace mzmedia;

namespace {

/// 把日志收进内存，便于断言；可注入写入延时以制造队列堆积
class MemoryWriter : public LogWriter {
public:
    explicit MemoryWriter(uint32_t delay_ms = 0) : _delay_ms(delay_ms) {}

    void write(const LogContext &ctx) override {
        if (_delay_ms > 0) {
            sleepMs(_delay_ms);
        }
        std::lock_guard<std::mutex> lck(_mtx);
        _lines.emplace_back(ctx.str());
        _messages.emplace_back(ctx.message());
    }

    void flush() override {}

    size_t size() const {
        std::lock_guard<std::mutex> lck(_mtx);
        return _lines.size();
    }

    std::vector<std::string> lines() const {
        std::lock_guard<std::mutex> lck(_mtx);
        return _lines;
    }

    std::vector<std::string> messages() const {
        std::lock_guard<std::mutex> lck(_mtx);
        return _messages;
    }

    bool containsMessage(const std::string &message) const {
        std::lock_guard<std::mutex> lck(_mtx);
        for (const auto &item : _messages) {
            if (item == message) {
                return true;
            }
        }
        return false;
    }

private:
    const uint32_t _delay_ms;
    mutable std::mutex _mtx;
    std::vector<std::string> _lines;
    std::vector<std::string> _messages;
};

/// 用例前后把全局 Logger 恢复成默认状态
class LoggerGuard {
public:
    LoggerGuard() { reset(); }
    ~LoggerGuard() { reset(); }

    static void reset() {
        Logger &logger = Logger::Instance();
        logger.flush();
        logger.clear();
        logger.setAsync(true);
        logger.setMaxQueueSize(4096);
        logger.setLevel(LogLevel::Info);
    }
};

size_t countOccurrences(const std::string &text, const std::string &needle) {
    size_t count = 0;
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

} // namespace

// ---------------------------------------------------------------------------
// 级别过滤
// ---------------------------------------------------------------------------

MZ_TEST(logger_default_level) {
    LoggerGuard guard;
    Logger &logger = Logger::Instance();

    // 默认级别 Info，默认异步，默认队列上限 4096
    MZ_ASSERT_EQ(logger.level(), LogLevel::Info);
    MZ_ASSERT_TRUE(logger.async());
    MZ_ASSERT_EQ(logger.maxQueueSize(), 4096u);

    MZ_ASSERT_TRUE(logger.enabled(LogLevel::Info));
    MZ_ASSERT_TRUE(logger.enabled(LogLevel::Warn));
    MZ_ASSERT_TRUE(logger.enabled(LogLevel::Fatal));
    MZ_ASSERT_FALSE(logger.enabled(LogLevel::Debug));
    MZ_ASSERT_FALSE(logger.enabled(LogLevel::Trace));
}

MZ_TEST(logger_level_filter) {
    LoggerGuard guard;
    Logger &logger = Logger::Instance();
    auto writer = std::make_shared<MemoryWriter>();
    logger.add(writer);
    logger.setLevel(LogLevel::Warn);

    DebugL << "不该出现-debug";
    InfoL << "不该出现-info";
    WarnL << "应该出现-warn";
    ErrorL << "应该出现-error";
    logger.flush();

    MZ_ASSERT_EQ(writer->size(), 2u);
    MZ_ASSERT_TRUE(writer->containsMessage("应该出现-warn"));
    MZ_ASSERT_TRUE(writer->containsMessage("应该出现-error"));
    MZ_ASSERT_FALSE(writer->containsMessage("不该出现-debug"));
    MZ_ASSERT_FALSE(writer->containsMessage("不该出现-info"));

    // 级别名称映射
    MZ_ASSERT_STR_EQ(levelName(LogLevel::Trace), "T");
    MZ_ASSERT_STR_EQ(levelName(LogLevel::Fatal), "F");
    MZ_ASSERT_STR_EQ(levelNameLong(LogLevel::Error), "Error");
}

// ---------------------------------------------------------------------------
// 格式化与行格式
// ---------------------------------------------------------------------------

MZ_TEST(logger_format) {
    LoggerGuard guard;
    Logger &logger = Logger::Instance();
    auto writer = std::make_shared<MemoryWriter>();
    logger.add(writer);
    logger.setLevel(LogLevel::Trace);

    InfoL << "value=" << 42 << " pi=" << 3.5;
    DebugP("code=%d name=%s", 7, "err");
    logger.flush();

    const auto messages = writer->messages();
    MZ_ASSERT_EQ(messages.size(), 2u);
    MZ_ASSERT_STR_EQ(messages[0], "value=42 pi=3.5");
    MZ_ASSERT_STR_EQ(messages[1], "code=7 name=err");

    // 整行格式：[时间][级别][tid][文件:行 函数] 消息
    const auto lines = writer->lines();
    MZ_ASSERT_EQ(lines.size(), 2u);
    MZ_ASSERT_TRUE(startWith(lines[0], "["));
    MZ_ASSERT_TRUE(lines[0].find("[I]") != std::string::npos);
    MZ_ASSERT_TRUE(lines[0].find("[tid ") != std::string::npos);
    MZ_ASSERT_TRUE(lines[0].find("test_logger.cpp:") != std::string::npos);
    MZ_ASSERT_TRUE(endWith(lines[0], "value=42 pi=3.5"));

    // printf 风格使用的是 Debug 级别
    MZ_ASSERT_TRUE(lines[1].find("[D]") != std::string::npos);
    MZ_ASSERT_TRUE(endWith(lines[1], "code=7 name=err"));
}

// ---------------------------------------------------------------------------
// 异步 / 同步
// ---------------------------------------------------------------------------

MZ_TEST(logger_async_flush) {
    LoggerGuard guard;
    Logger &logger = Logger::Instance();
    auto writer = std::make_shared<MemoryWriter>();
    logger.add(writer);
    logger.setLevel(LogLevel::Info);

    constexpr int kCount = 2000;
    // droppedCount 是进程内累计值，必须比较增量，否则会受前面用例影响
    const uint64_t dropped_before = logger.droppedCount();
    for (int i = 0; i < kCount; ++i) {
        InfoL << "async-line-" << i;
    }
    // 队列上限 4096 > 2000，不应有丢弃
    logger.flush();

    MZ_ASSERT_EQ(writer->size(), static_cast<size_t>(kCount));
    MZ_ASSERT_EQ(logger.droppedCount(), dropped_before);
    MZ_ASSERT_TRUE(writer->containsMessage("async-line-0"));
    MZ_ASSERT_TRUE(writer->containsMessage("async-line-1999"));
}

MZ_TEST(logger_sync_mode) {
    LoggerGuard guard;
    Logger &logger = Logger::Instance();
    auto writer = std::make_shared<MemoryWriter>();
    logger.add(writer);
    logger.setLevel(LogLevel::Info);
    logger.setAsync(false);

    InfoL << "sync-line";
    // 同步模式下不 flush 也应当已经可见
    MZ_ASSERT_EQ(writer->size(), 1u);
    MZ_ASSERT_TRUE(writer->containsMessage("sync-line"));
    MZ_ASSERT_FALSE(logger.async());
}

// ---------------------------------------------------------------------------
// 队列满：丢弃 + 计数，绝不阻塞
// ---------------------------------------------------------------------------

MZ_TEST(logger_queue_overflow_drop) {
    LoggerGuard guard;
    Logger &logger = Logger::Instance();
    // 每条日志写 5ms + 队列上限 4：生产者必然跑在消费者前面，出现丢弃
    auto writer = std::make_shared<MemoryWriter>(5);
    logger.add(writer);
    logger.setLevel(LogLevel::Info);
    logger.setMaxQueueSize(4);
    MZ_ASSERT_EQ(logger.maxQueueSize(), 4u);

    constexpr int kCount = 500;
    const uint64_t dropped_before = logger.droppedCount();
    for (int i = 0; i < kCount; ++i) {
        InfoL << "drop-line-" << i;
    }

    logger.flush();

    const uint64_t dropped = logger.droppedCount() - dropped_before;
    const uint64_t written = static_cast<uint64_t>(writer->size());
    MZ_ASSERT_GT(dropped, 0u);
    // 精确守恒：每条日志要么入队后被写出，要么被丢弃，不会有第三种去向
    MZ_ASSERT_EQ(written + dropped, static_cast<uint64_t>(kCount));
    // 队列上限 4，消费者每次最多再抱走一批，写入量必然很小
    MZ_ASSERT_LE(written, 16u);
}

// ---------------------------------------------------------------------------
// 多线程并发：每行必须是完整的一条日志
// ---------------------------------------------------------------------------

MZ_TEST(logger_concurrent_lines_atomic) {
    LoggerGuard guard;
    Logger &logger = Logger::Instance();
    auto writer = std::make_shared<MemoryWriter>();
    logger.add(writer);
    logger.setLevel(LogLevel::Info);

    constexpr int kThreads = 4;
    constexpr int kPerThread = 500;
    const uint64_t dropped_before = logger.droppedCount();
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(kThreads));
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t]() {
            for (int i = 0; i < kPerThread; ++i) {
                InfoL << "t" << t << "-i" << i;
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }
    logger.flush();

    MZ_ASSERT_EQ(writer->size(), static_cast<size_t>(kThreads * kPerThread));
    // 队列上限 4096 > 2000，本用例不应有新增丢弃
    MZ_ASSERT_EQ(logger.droppedCount(), dropped_before);

    size_t bad = 0;
    for (const auto &line : writer->lines()) {
        // 一条完整日志行只能有一个 tid 标记（交错会让它出现两次或缺失）
        if (countOccurrences(line, "[tid ") != 1) {
            ++bad;
            continue;
        }
        // 消息段必须完好（形如 t<数字>-i<数字>）
        const size_t msg_pos = line.rfind("] ");
        if (msg_pos == std::string::npos) {
            ++bad;
            continue;
        }
        const std::string message = line.substr(msg_pos + 2);
        if (!startWith(message, "t") || message.find("-i") == std::string::npos) {
            ++bad;
        }
    }
    MZ_ASSERT_EQ(bad, 0u);
}

// ---------------------------------------------------------------------------
// FileWriter：写入 / 滚动 / 历史清理
// ---------------------------------------------------------------------------

MZ_TEST(logger_file_writer) {
    LoggerGuard guard;
    const std::string dir = "/tmp/mzmedia_logger_test";
    (void) ::system(("rm -rf " + dir).c_str());

    Logger &logger = Logger::Instance();
    const std::string path = dir + "/mz.log";
    // max_size = 0 表示不滚动；max_files = 0 表示不清理
    auto writer = std::make_shared<FileWriter>(path, 0, 0);
    logger.add(writer);
    logger.setLevel(LogLevel::Info);

    for (int i = 0; i < 20; ++i) {
        InfoL << "file-line-" << i;
    }
    logger.flush();

    MZ_ASSERT_TRUE(fileExists(path));
    std::string error;
    const std::string content = loadFile(path, &error);
    MZ_ASSERT_TRUE(error.empty());
    MZ_ASSERT_TRUE(content.find("file-line-0") != std::string::npos);
    MZ_ASSERT_TRUE(content.find("file-line-19") != std::string::npos);
    MZ_ASSERT_GT(writer->writtenBytes(), 0u);
    MZ_ASSERT_EQ(writer->rollCount(), 0u);
    MZ_ASSERT_STR_EQ(basename(writer->path()), "mz.log");

    (void) ::system(("rm -rf " + dir).c_str());
}

MZ_TEST(logger_file_roll) {
    LoggerGuard guard;
    const std::string dir = "/tmp/mzmedia_logger_roll";
    (void) ::system(("rm -rf " + dir).c_str());

    Logger &logger = Logger::Instance();
    const std::string path = dir + "/roll.log";
    // 单文件 500 字节、只保留 3 个历史：写 200 行会连续滚动多次
    auto writer = std::make_shared<FileWriter>(path, 500, 3);
    logger.add(writer);
    logger.setLevel(LogLevel::Info);

    for (int i = 0; i < 200; ++i) {
        InfoL << "roll-line-" << i;
    }
    logger.flush();

    MZ_ASSERT_GT(writer->rollCount(), 0u);
    MZ_ASSERT_TRUE(fileExists(path));
    // 滚动后当前文件被重新打开，字节数应从 0 重新累计
    MZ_ASSERT_LT(writer->writtenBytes(), 500u);

    // 统计历史文件个数：以 "roll.log." 开头（当前文件不含该后缀，天然排除）
    DIR *dirp = ::opendir(dir.c_str());
    MZ_ASSERT_NOT_NULL(dirp);
    size_t history = 0;
    const std::string prefix = "roll.log.";
    struct dirent *entry = nullptr;
    while ((entry = ::readdir(dirp)) != nullptr) {
        const std::string name = entry->d_name;
        if (startWith(name, prefix)) {
            ++history;
        }
    }
    ::closedir(dirp);
    // max_files = 3：无论滚动多少次，历史文件都应稳定为 3 个
    MZ_ASSERT_EQ(history, 3u);

    (void) ::system(("rm -rf " + dir).c_str());
}

/*
 * 相对路径回归测试
 *
 * logger.h 的用法注释里推荐的是 FileWriter("logs/mzmedia.log")，即相对路径。
 * 若 createDirectory 处理相对路径有误，这里会静默不写日志，因此必须有用例守住。
 */
MZ_TEST(logger_file_relative_path) {
    LoggerGuard guard;
    const std::string root = "/tmp/mzmedia_logger_rel";
    (void) ::system(("rm -rf " + root).c_str());
    MZ_ASSERT_TRUE(createDirectory(root));
    MZ_ASSERT_EQ(::chdir(root.c_str()), 0);

    Logger &logger = Logger::Instance();
    auto writer = std::make_shared<FileWriter>("logs/rel.log", 0, 0);
    logger.add(writer);
    logger.setLevel(LogLevel::Info);

    InfoL << "relative-path-line";
    logger.flush();

    MZ_ASSERT_TRUE(isDir("logs"));
    MZ_ASSERT_TRUE(fileExists("logs/rel.log"));
    std::string error;
    const std::string content = loadFile("logs/rel.log", &error);
    MZ_ASSERT_TRUE(content.find("relative-path-line") != std::string::npos);

    MZ_ASSERT_EQ(::chdir("/"), 0);
    (void) ::system(("rm -rf " + root).c_str());
}
