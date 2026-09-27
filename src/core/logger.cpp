#include "core/logger.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mzmedia {

// ---------------------------------------------------------------------------
// 级别名称与颜色
// ---------------------------------------------------------------------------

const char *levelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "T";
        case LogLevel::Debug: return "D";
        case LogLevel::Info:  return "I";
        case LogLevel::Warn:  return "W";
        case LogLevel::Error: return "E";
        case LogLevel::Fatal: return "F";
    }
    return "?";
}

const char *levelNameLong(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "Trace";
        case LogLevel::Debug: return "Debug";
        case LogLevel::Info:  return "Info";
        case LogLevel::Warn:  return "Warn";
        case LogLevel::Error: return "Error";
        case LogLevel::Fatal: return "Fatal";
    }
    return "Unknown";
}

namespace {

constexpr const char *kColorReset = "\033[0m";

const char *levelColor(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "\033[37m";   // 白
        case LogLevel::Debug: return "\033[36m";   // 青
        case LogLevel::Info:  return "\033[32m";   // 绿
        case LogLevel::Warn:  return "\033[33m";   // 黄
        case LogLevel::Error: return "\033[31m";   // 红
        case LogLevel::Fatal: return "\033[35;1m"; // 紫加粗
    }
    return "";
}

} // namespace

// ---------------------------------------------------------------------------
// LogContext
// ---------------------------------------------------------------------------

LogContext::LogContext(LogLevel level, const char *file, int line, const char *func)
        : _level(level),
          _time(getCurrentMillisecond(true)),
          _time_str(getTimeStrWithMs()),
          _file(file == nullptr ? "" : file),
          _line(line),
          _func(func == nullptr ? "" : func) {}

const char *LogContext::levelName() const {
    return mzmedia::levelName(_level);
}

const std::string &LogContext::message() const {
    if (!_message_built) {
        _message = _ss.str();
        _message_built = true;
    }
    return _message;
}

std::string LogContext::str() const {
    return strFormat("[%s][%s][tid %llu][%s:%d %s] %s",
                     _time_str.c_str(),
                     levelName(),
                     static_cast<unsigned long long>(getThreadId()),
                     basename(_file).c_str(),
                     _line,
                     _func,
                     message().c_str());
}

// ---------------------------------------------------------------------------
// ConsoleWriter
// ---------------------------------------------------------------------------

ConsoleWriter::ConsoleWriter(bool color) : _color(color) {}

void ConsoleWriter::write(const LogContext &ctx) {
    FILE *fp = (static_cast<int>(ctx.level()) >= static_cast<int>(LogLevel::Warn)) ? stderr : stdout;
    const std::string line = ctx.str();
    if (_color) {
        std::fprintf(fp, "%s%s%s\n", levelColor(ctx.level()), line.c_str(), kColorReset);
    } else {
        std::fprintf(fp, "%s\n", line.c_str());
    }
}

void ConsoleWriter::flush() {
    std::fflush(stdout);
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// FileWriter
// ---------------------------------------------------------------------------

FileWriter::FileWriter(const std::string &file, uint64_t max_size, int max_files)
        : _file(file),
          _dir(dirname(file)),
          _name(basename(file)),
          _max_size(max_size),
          _max_files(max_files) {
    createDirectory(_dir);
    _ofs.open(_file, std::ios::binary | std::ios::app);
    if (_ofs.good()) {
        _ofs.seekp(0, std::ios::end);
        _written = static_cast<uint64_t>(_ofs.tellp());
    }
}

uint64_t FileWriter::writtenBytes() const {
    std::lock_guard<std::mutex> lck(_mtx);
    return _written;
}

uint64_t FileWriter::rollCount() const {
    std::lock_guard<std::mutex> lck(_mtx);
    return _roll_count;
}

void FileWriter::write(const LogContext &ctx) {
    std::lock_guard<std::mutex> lck(_mtx);
    if (!_ofs.good()) {
        return;
    }
    const std::string line = ctx.str() + "\n";
    _ofs.write(line.data(), static_cast<std::streamsize>(line.size()));
    _written += line.size();
    if (_max_size > 0 && _written >= _max_size) {
        roll();
    }
}

void FileWriter::flush() {
    std::lock_guard<std::mutex> lck(_mtx);
    if (_ofs.good()) {
        _ofs.flush();
    }
}

std::string FileWriter::makeRollPath() {
    const uint64_t ms = getCurrentMillisecond(true);
    const std::string stamp = getTimeStr("%Y%m%d-%H%M%S", static_cast<time_t>(ms / 1000));
    const std::string base = strFormat("%s.%s-%03u", _file.c_str(), stamp.c_str(),
                                       static_cast<unsigned>(ms % 1000));
    std::string target = base;
    // 同毫秒连续滚动时追加序号，保证不覆盖已有历史文件
    for (int i = 1; fileExists(target); ++i) {
        target = strFormat("%s-%d", base.c_str(), i);
    }
    return target;
}

bool FileWriter::roll() {
    if (_ofs.is_open()) {
        _ofs.flush();
        _ofs.close();
    }

    const std::string target = makeRollPath();
    if (::rename(_file.c_str(), target.c_str()) != 0) {
        // 改名失败不致命：直接截断重写，保证日志不丢。
        // 注意：这里**不能用 ErrorP()** —— roll() 是在持有 _mtx 的情况下被调用的，
        // 同步模式下再打日志会重入 FileWriter::write 并再次锁 _mtx，造成自死锁。
        std::fprintf(stderr, "[mzmedia][FileWriter] 日志滚动改名失败: %s -> %s (errno=%d)\n",
                     _file.c_str(), target.c_str(), errno);
    }

    _ofs.open(_file, std::ios::binary | std::ios::trunc);
    _written = 0;
    ++_roll_count;
    if (!_ofs.good()) {
        return false;
    }
    if (_max_files > 0) {
        removeOldFiles();
    }
    return true;
}

void FileWriter::removeOldFiles() {
    DIR *dir = ::opendir(_dir.c_str());
    if (dir == nullptr) {
        return;
    }

    // 历史文件的判定：以 "<当前文件名>." 开头（当前文件本身不含该后缀，天然被排除）
    const std::string prefix = _name + ".";
    std::vector<std::string> history;
    struct dirent *entry = nullptr;
    while ((entry = ::readdir(dir)) != nullptr) {
        const std::string name = entry->d_name;
        if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0) {
            history.emplace_back(name);
        }
    }
    ::closedir(dir);

    if (static_cast<int>(history.size()) <= _max_files) {
        return;
    }
    // 文件名含时间戳，字典序即时间序
    std::sort(history.begin(), history.end());
    const size_t remove_count = history.size() - static_cast<size_t>(_max_files);
    for (size_t i = 0; i < remove_count; ++i) {
        const std::string path = _dir + "/" + history[i];
        (void) ::remove(path.c_str());
    }
}

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------

Logger &Logger::Instance() {
    static Logger *instance = new Logger();
    return *instance;
}

Logger::Logger() {
    _thread = std::thread([this]() { run(); });
    _thread_id = _thread.get_id();
    // 退出阶段收尾：flush 队列、停日志线程
    std::atexit(&Logger::atexitHandler);
}

Logger::~Logger() {
    shutdown();
}

void Logger::atexitHandler() {
    Logger::Instance().shutdown();
}

void Logger::add(const LogWriter::Ptr &writer) {
    if (!writer) {
        return;
    }
    std::lock_guard<std::mutex> lck(_mtx);
    _writers.emplace_back(writer);
}

void Logger::clear() {
    std::lock_guard<std::mutex> lck(_mtx);
    _writers.clear();
}

std::vector<LogWriter::Ptr> Logger::writers() const {
    std::lock_guard<std::mutex> lck(_mtx);
    return _writers;
}

void Logger::setAsync(bool async) {
    if (_async.load() == async) {
        return;
    }
    // 切换前先把已有队列写干净，避免出现日志乱序
    flush();
    _async.store(async);
}

void Logger::write(const std::shared_ptr<LogContext> &ctx) {
    if (!ctx) {
        return;
    }
    if (!_async.load()) {
        writeNow(*ctx);
        return;
    }
    {
        std::lock_guard<std::mutex> lck(_mtx);
        if (_queue.size() >= _max_queue.load()) {
            _dropped.fetch_add(1);
            return;
        }
        _queue.emplace_back(ctx);
    }
    _cv.notify_one();
}

void Logger::writeNow(const LogContext &ctx) {
    const auto targets = writers();
    for (const auto &writer : targets) {
        writer->write(ctx);
    }
    if (ctx.level() == LogLevel::Fatal) {
        // Fatal 立即落盘，保证崩溃前日志不丢
        for (const auto &writer : targets) {
            writer->flush();
        }
    }
}

void Logger::run() {
    setThreadName("mzmedia-log");
    while (true) {
        std::list<std::shared_ptr<LogContext>> todo;
        {
            std::unique_lock<std::mutex> lck(_mtx);
            _cv.wait(lck, [this]() { return _exit || !_queue.empty(); });
            if (_queue.empty() && _exit) {
                break;
            }
            // 整批取出后在锁外写，避免写文件/写终端时占着锁
            todo.swap(_queue);
            _writing = true;
        }

        const auto targets = writers();
        for (const auto &ctx : todo) {
            for (const auto &writer : targets) {
                writer->write(*ctx);
            }
            if (ctx->level() == LogLevel::Fatal) {
                for (const auto &writer : targets) {
                    writer->flush();
                }
            }
        }

        {
            std::lock_guard<std::mutex> lck(_mtx);
            _writing = false;
        }
        _cv_flush.notify_all();
    }
}

void Logger::flush() {
    const bool in_log_thread = _thread.joinable() && (std::this_thread::get_id() == _thread_id);
    // 同步模式、没有日志线程、或在日志线程内部 —— 直接刷新 writer，不能等待（会自等）
    if (!_async.load() || !_thread.joinable() || in_log_thread) {
        for (const auto &writer : writers()) {
            writer->flush();
        }
        return;
    }

    {
        std::unique_lock<std::mutex> lck(_mtx);
        _cv_flush.wait(lck, [this]() { return _queue.empty() && !_writing; });
    }
    for (const auto &writer : writers()) {
        writer->flush();
    }
}

void Logger::shutdown() {
    if (!_thread.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lck(_mtx);
        _exit = true;
    }
    _cv.notify_all();
    _thread.join();
    // 线程退出前已把队列写空，这里只需刷新 writer 缓冲
    for (const auto &writer : writers()) {
        writer->flush();
    }
}

void Logger::print(LogLevel level, const char *file, int line, const char *func, const char *fmt, ...) {
    // print 是静态成员，这里显式取单例
    Logger &logger = Instance();
    if (!logger.enabled(level)) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    std::string message = strFormatV(fmt, ap);
    va_end(ap);

    auto ctx = std::make_shared<LogContext>(level, file, line, func);
    ctx->stream() << message;
    logger.write(ctx);
}

} // namespace mzmedia
