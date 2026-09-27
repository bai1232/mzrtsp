/*
 * mzmedia 日志模块：分级、异步、滚动文件、流式与 printf 两种写法
 * ============================================================================
 * 用法：
 *   Logger::Instance().add(std::make_shared<ConsoleWriter>());
 *   Logger::Instance().add(std::make_shared<FileWriter>("logs/mzmedia.log"));
 *   Logger::Instance().setLevel(LogLevel::Debug);
 *
 *   InfoL << "hello " << 42;                      // 流式
 *   ErrorP("connect to %s:%d failed", "1.2.3.4", 9000);  // printf 风格
 *
 * 日志行格式：
 *   [2026-09-27 16:58:01.123][I][tid 12345][logger.cpp:42 run] 消息
 * ============================================================================
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/util.h"

namespace mzmedia {

enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Fatal = 5,
};

/**
 * 级别的单字符表示：T / D / I / W / E / F
 */
const char *levelName(LogLevel level);

/**
 * 级别的单词表示：Trace / Debug / Info / Warn / Error / Fatal
 */
const char *levelNameLong(LogLevel level);

class LogContext;

// ---------------------------------------------------------------------------
// 输出目标（可自由扩展：写文件、写控制台、写网络……）
// ---------------------------------------------------------------------------
class LogWriter {
public:
    using Ptr = std::shared_ptr<LogWriter>;
    virtual ~LogWriter() = default;

    /**
     * 写出一条日志。实现必须是线程安全的：
     * 异步模式下由日志线程调用，同步模式下由业务线程直接调用。
     */
    virtual void write(const LogContext &ctx) = 0;

    /**
     * 刷新缓冲。Logger::flush() 与进程退出时会被调用。
     */
    virtual void flush() {}
};

/**
 * 控制台输出
 * - 着色：仅在 TTY 下有意义，颜色写在日志行首尾
 * - 分流：Warn 及以上写 stderr，其余写 stdout（便于 `2>` 单独收集告警）
 */
class ConsoleWriter : public LogWriter {
public:
    using Ptr = std::shared_ptr<ConsoleWriter>;

    explicit ConsoleWriter(bool color = true);

    void write(const LogContext &ctx) override;
    void flush() override;

private:
    const bool _color;
};

/**
 * 文件输出（按大小滚动）
 *
 * 滚动策略（决策）：
 *   1. 当累计写入字节数 >= max_size 时触发一次滚动；
 *   2. 历史文件命名为 "<日志文件>.<YYYYmmdd-HHMMSS>-<毫秒>"，
 *      毫秒后缀保证「文件名字典序 == 时间序」，同时避免同一秒内多次滚动互相覆盖；
 *      若目标名已存在（同毫秒连续滚动），追加 -1 / -2 ... 序号，绝不覆盖已有历史；
 *   3. 滚动后清理旧历史，**只保留最近的 max_files 个**；max_files <= 0 表示不清理；
 *   4. 改名失败不致命：打 stderr 提示并截断重写，保证日志不丢。
 */
class FileWriter : public LogWriter {
public:
    using Ptr = std::shared_ptr<FileWriter>;

    /// 默认单文件上限 64MB（决策：够跑很久，又不至于单文件过大难以翻阅）
    static constexpr uint64_t kDefaultMaxSize = 64ull * 1024 * 1024;
    /// 默认保留最近 5 个历史文件
    static constexpr int kDefaultMaxFiles = 5;

    /**
     * @param file      日志文件路径，如 logs/mzmedia.log（父目录不存在会自动创建）
     * @param max_size  单文件字节上限，0 表示不滚动
     * @param max_files 保留的历史文件个数，<= 0 表示不清理
     */
    explicit FileWriter(const std::string &file,
                        uint64_t max_size = kDefaultMaxSize,
                        int max_files = kDefaultMaxFiles);
    ~FileWriter() override = default;

    void write(const LogContext &ctx) override;
    void flush() override;

    /// 供测试与运维观测
    const std::string &path() const { return _file; }
    uint64_t writtenBytes() const;
    uint64_t rollCount() const;

private:
    bool roll();
    std::string makeRollPath();
    void removeOldFiles();

    const std::string _file;
    const std::string _dir;
    const std::string _name;
    const uint64_t _max_size;
    const int _max_files;

    mutable std::mutex _mtx;
    std::ofstream _ofs;
    uint64_t _written = 0;
    uint64_t _roll_count = 0;
};

// ---------------------------------------------------------------------------
// 一条日志
// ---------------------------------------------------------------------------
class LogContext {
public:
    LogContext(LogLevel level, const char *file, int line, const char *func);

    LogLevel level() const { return _level; }
    /// 创建时刻的**系统时钟**毫秒（日志时间戳必须可读，不能用单调时钟）
    uint64_t createdTime() const { return _time; }
    const char *levelName() const;
    const char *file() const { return _file; }
    int line() const { return _line; }
    const char *func() const { return _func; }

    /// 供流式写法追加内容
    std::ostringstream &stream() { return _ss; }

    /// 用户消息部分（首次访问时从流中固化）
    const std::string &message() const;

    /// 渲染成完整一行（不含换行）
    std::string str() const;

private:
    const LogLevel _level;
    const uint64_t _time;
    const std::string _time_str;
    const char *_file;
    const int _line;
    const char *_func;
    std::ostringstream _ss;
    mutable std::string _message;
    mutable bool _message_built = false;
};

// ---------------------------------------------------------------------------
// 日志器（单例）
// ---------------------------------------------------------------------------
class Logger {
public:
    /**
     * 全局单例
     *
     * 单例策略（决策）：**故意泄漏**（内部 new 出来不 delete）。
     *   原因：日志可能被其他静态对象、后台线程的析构路径调用；若用栈上静态对象，
     *   析构顺序不可控，会出现"日志器已销毁但还在打日志"的 use-after-free。
     *   泄漏一个 Logger 的代价可忽略，换退出阶段的安全。
     *   退出收尾由构造函数注册的 atexit 钩子完成（flush 队列 + 停线程）。
     */
    static Logger &Instance();

    /// 添加输出目标（Logger 持有其 shared_ptr，直到 clear()）
    void add(const LogWriter::Ptr &writer);

    /// 移除全部输出目标
    void clear();

    /// 全局级别。**默认 Info**（决策：Trace/Debug 噪声大，默认只保留可用信息）
    void setLevel(LogLevel level) { _level.store(level); }
    LogLevel level() const { return _level.load(); }

    /// 该级别是否会被输出（低于阈值的内容连格式化都不会做）
    bool enabled(LogLevel level) const {
        return static_cast<int>(level) >= static_cast<int>(_level.load());
    }

    /// 提交一条日志（异步模式下入队后立即返回）
    void write(const std::shared_ptr<LogContext> &ctx);

    /**
     * printf 风格写日志（带编译期格式检查）
     *
     * 声明为 static 的原因：GCC 对**非静态成员函数**的 format 属性会把隐式 this
     * 计入参数序号（此时 fmt 会变成第 6 个参数，必须写 format(printf, 6, 7)），
     * 既容易写错，又与 Clang 的判定不一致。静态成员函数没有 this，序号从 1 开始，
     * 即 fmt 是第 5 个参数、可变参从第 6 个开始，跨编译器一致。
     */
    static void print(LogLevel level, const char *file, int line, const char *func, const char *fmt, ...)
            __attribute__((format(printf, 5, 6)));

    /// 异步开关，**默认开启**
    void setAsync(bool async);
    bool async() const { return _async.load(); }

    /**
     * 异步队列上限（默认 4096）
     *
     * 队列满策略（决策）：**丢弃新日志并累加计数，绝不阻塞业务线程**。
     *   理由：日志是辅助设施，不能因为磁盘慢/写日志慢而拖住 demux、转码等关键路径；
     *   丢弃量可通过 droppedCount() 观测，出现丢弃说明写入能力不足，应调整级别或 writer。
     */
    void setMaxQueueSize(size_t size) { _max_queue.store(size); }
    size_t maxQueueSize() const { return _max_queue.load(); }

    /// 等待队列清空并刷新所有 writer
    void flush();

    /// 停止日志线程并刷新（幂等，可重复调用）
    void shutdown();

    /// 因队列满被丢弃的日志条数
    uint64_t droppedCount() const { return _dropped.load(); }

private:
    Logger();
    ~Logger();

    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    void run();
    void writeNow(const LogContext &ctx);
    std::vector<LogWriter::Ptr> writers() const;
    static void atexitHandler();

    std::atomic<LogLevel> _level{LogLevel::Info};
    std::atomic<bool> _async{true};
    std::atomic<uint64_t> _dropped{0};
    std::atomic<size_t> _max_queue{4096};

    mutable std::mutex _mtx;
    std::condition_variable _cv;
    std::condition_variable _cv_flush;
    std::list<std::shared_ptr<LogContext>> _queue;
    std::vector<LogWriter::Ptr> _writers;
    bool _exit = false;
    bool _writing = false;

    std::thread _thread;
    std::thread::id _thread_id;
};

// ---------------------------------------------------------------------------
// 流式日志辅助：临时对象析构时才真正提交日志
// ---------------------------------------------------------------------------
class LogStream {
public:
    LogStream(LogLevel level, const char *file, int line, const char *func) {
        // 级别不足时连 LogContext 都不构造，彻底避免格式化开销
        if (Logger::Instance().enabled(level)) {
            _ctx = std::make_shared<LogContext>(level, file, line, func);
        }
    }

    ~LogStream() {
        if (_ctx) {
            Logger::Instance().write(_ctx);
        }
    }

    LogStream(const LogStream &) = delete;
    LogStream &operator=(const LogStream &) = delete;

    template<typename T>
    LogStream &operator<<(const T &value) {
        if (_ctx) {
            _ctx->stream() << value;
        }
        return *this;
    }

    /// 支持 std::endl 等操纵符（模板无法推导函数模板，需单独重载）
    using StreamManip = std::ostream &(*)(std::ostream &);
    LogStream &operator<<(StreamManip manip) {
        if (_ctx) {
            _ctx->stream() << manip;
        }
        return *this;
    }

private:
    std::shared_ptr<LogContext> _ctx;
};

} // namespace mzmedia

// ---------------------------------------------------------------------------
// 流式日志宏（对齐 ZLToolKit 的命名习惯）
// ---------------------------------------------------------------------------
#define TraceL mzmedia::LogStream(mzmedia::LogLevel::Trace, __FILE__, __LINE__, __func__)
#define DebugL mzmedia::LogStream(mzmedia::LogLevel::Debug, __FILE__, __LINE__, __func__)
#define InfoL  mzmedia::LogStream(mzmedia::LogLevel::Info,  __FILE__, __LINE__, __func__)
#define WarnL  mzmedia::LogStream(mzmedia::LogLevel::Warn,  __FILE__, __LINE__, __func__)
#define ErrorL mzmedia::LogStream(mzmedia::LogLevel::Error, __FILE__, __LINE__, __func__)
#define FatalL mzmedia::LogStream(mzmedia::LogLevel::Fatal, __FILE__, __LINE__, __func__)

// ---------------------------------------------------------------------------
// printf 风格日志宏
// ---------------------------------------------------------------------------
#define TraceP(...) \
    mzmedia::Logger::print(mzmedia::LogLevel::Trace, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define DebugP(...) \
    mzmedia::Logger::print(mzmedia::LogLevel::Debug, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define InfoP(...) \
    mzmedia::Logger::print(mzmedia::LogLevel::Info, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define WarnP(...) \
    mzmedia::Logger::print(mzmedia::LogLevel::Warn, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define ErrorP(...) \
    mzmedia::Logger::print(mzmedia::LogLevel::Error, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define FatalP(...) \
    mzmedia::Logger::print(mzmedia::LogLevel::Fatal, __FILE__, __LINE__, __func__, __VA_ARGS__)
