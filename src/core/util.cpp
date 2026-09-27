#include "core/util.h"

#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

namespace mzmedia {

// ---------------------------------------------------------------------------
// 字符串
// ---------------------------------------------------------------------------

std::string strFormatV(const char *fmt, va_list ap) {
    if (fmt == nullptr) {
        return {};
    }
    // 第一次调用只为探测长度（不产生输出），因此需要一份 va_list 副本
    va_list ap_probe;
    va_copy(ap_probe, ap);
    const int len = vsnprintf(nullptr, 0, fmt, ap_probe);
    va_end(ap_probe);
    if (len <= 0) {
        return {};
    }

    std::string str(static_cast<size_t>(len), '\0');
    va_list ap_write;
    va_copy(ap_write, ap);
    // C++17 起 std::string::data() 返回可写指针，缓冲区为 len + 1（含结尾 '\0'）
    vsnprintf(str.data(), static_cast<size_t>(len) + 1, fmt, ap_write);
    va_end(ap_write);
    return str;
}

std::string strFormat(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string str = strFormatV(fmt, ap);
    va_end(ap);
    return str;
}

std::vector<std::string> split(const std::string &str, const char *delim) {
    std::vector<std::string> result;
    if (delim == nullptr || *delim == '\0') {
        result.emplace_back(str);
        return result;
    }

    const size_t delim_len = std::strlen(delim);
    size_t pos = 0;
    while (true) {
        const size_t next = str.find(delim, pos);
        if (next == std::string::npos) {
            result.emplace_back(str.substr(pos));
            break;
        }
        result.emplace_back(str.substr(pos, next - pos));
        pos = next + delim_len;
    }
    return result;
}

std::string &trim(std::string &str, const char *chars) {
    if (str.empty() || chars == nullptr) {
        return str;
    }
    const size_t begin = str.find_first_not_of(chars);
    if (begin == std::string::npos) {
        // 全部是要裁掉的字符
        str.clear();
        return str;
    }
    str.erase(0, begin);
    str.erase(str.find_last_not_of(chars) + 1);
    return str;
}

std::string &toLower(std::string &str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return str;
}

std::string &toUpper(std::string &str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return str;
}

bool startWith(const std::string &str, const std::string &prefix) {
    return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
}

bool endWith(const std::string &str, const std::string &suffix) {
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string replace(const std::string &str, const std::string &from, const std::string &to) {
    if (from.empty()) {
        return str;
    }
    std::string result = str;
    size_t pos = 0;
    while ((pos = result.find(from, pos)) != std::string::npos) {
        result.replace(pos, from.size(), to);
        pos += to.size();
    }
    return result;
}

std::string join(const std::vector<std::string> &parts, const std::string &sep) {
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            result += sep;
        }
        result += parts[i];
    }
    return result;
}

std::string basename(const std::string &path) {
    const size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string dirname(const std::string &path) {
    const size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return ".";
    }
    if (pos == 0) {
        return "/";
    }
    return path.substr(0, pos);
}

// ---------------------------------------------------------------------------
// 时间
// ---------------------------------------------------------------------------

uint64_t getCurrentMillisecond(bool system) {
    if (system) {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

uint64_t getCurrentMicrosecond(bool system) {
    if (system) {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(now).count());
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

time_t getCurrentSecond() {
    return ::time(nullptr);
}

std::string getTimeStr(const char *fmt, time_t t) {
    if (fmt == nullptr) {
        return {};
    }
    if (t == 0) {
        t = ::time(nullptr);
    }
    struct tm tm_buf;
    if (localtime_r(&t, &tm_buf) == nullptr) {
        return {};
    }
    char buf[128] = {0};
    if (strftime(buf, sizeof(buf), fmt, &tm_buf) == 0) {
        return {};
    }
    return buf;
}

std::string getTimeStrWithMs() {
    // 用系统时钟取墙钟，避免单调时钟的任意起点导致时间不可读
    const uint64_t ms = getCurrentMillisecond(true);
    const time_t sec = static_cast<time_t>(ms / 1000);
    const std::string base = getTimeStr("%Y-%m-%d %H:%M:%S", sec);
    if (base.empty()) {
        return {};
    }
    return strFormat("%s.%03u", base.c_str(), static_cast<unsigned>(ms % 1000));
}

// ---------------------------------------------------------------------------
// 线程 / 进程
// ---------------------------------------------------------------------------

void setThreadName(const char *name) {
    if (name == nullptr) {
        return;
    }
    // 显式截断到 15 个字符：glibc 的 pthread_setname_np 在名字过长时会直接返回
    // ERANGE 并且什么都不设置，依赖它截断并不可靠（Linux 上限 16 字节含 '\0'）。
    char buf[16] = {0};
    const size_t len = std::min(std::strlen(name), sizeof(buf) - 1);
    std::memcpy(buf, name, len);
    (void) ::pthread_setname_np(::pthread_self(), buf);
}

std::string getThreadName() {
    char buf[32] = {0};
    if (::pthread_getname_np(::pthread_self(), buf, sizeof(buf)) != 0) {
        return {};
    }
    return buf;
}

uint64_t getThreadId() {
    return static_cast<uint64_t>(::syscall(SYS_gettid));
}

void sleepMs(uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::string exePath() {
    // readlink 不保证 NUL 结尾，且缓冲区装不下时会**静默截断**：
    // 返回值 == 容量 - 1 时说明可能被截断，必须扩容重试。
    std::vector<char> buf(1024);
    while (true) {
        const ssize_t len = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
        if (len < 0) {
            return {};   // 读取失败（例如 /proc 未挂载）
        }
        if (static_cast<size_t>(len) < buf.size() - 1) {
            return std::string(buf.data(), static_cast<size_t>(len));
        }
        if (buf.size() >= 64 * 1024) {
            return {};   // 防御性上限，避免无限扩容
        }
        buf.resize(buf.size() * 2);
    }
}

std::string exeDir() {
    const std::string path = exePath();
    // 失败时返回 "." 而不是空串：否则调用方拼 exeDir() + "/logs" 会得到根路径 "/logs"
    return path.empty() ? std::string(".") : dirname(path);
}

std::string exeName() {
    return basename(exePath());
}

// ---------------------------------------------------------------------------
// 文件系统
// ---------------------------------------------------------------------------

bool fileExists(const std::string &path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool isDir(const std::string &path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool createDirectory(const std::string &path) {
    if (path.empty()) {
        return false;
    }
    if (isDir(path)) {
        return true;
    }

    // 逐级创建；cur 始终是"已存在的目录"或"待创建的目录"
    std::string cur;
    size_t pos = 0;
    if (path[0] == '/') {
        cur = "/";
        pos = 1;
    }
    while (pos <= path.size()) {
        size_t next = path.find('/', pos);
        if (next == std::string::npos) {
            next = path.size();
        }
        const std::string part = path.substr(pos, next - pos);
        if (!part.empty()) {
            // 只有"已有前缀"时才补分隔符。原来的 cur.empty() 判断是 bug：
            // 相对路径 "logs" 会被拼成 "/logs"，于是跑去根目录建目录而必然失败
            // （非 root 下 EACCES），导致 FileWriter("logs/x.log") 静默写不出日志。
            if (!cur.empty() && cur.back() != '/') {
                cur += '/';
            }
            cur += part;
            if (!isDir(cur) && ::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
                return false;
            }
        }
        if (next == path.size()) {
            break;
        }
        pos = next + 1;
    }
    return isDir(path);
}

std::string loadFile(const std::string &path, std::string *error) {
    // Linux 上 ifstream 能成功"打开"目录（open(2) 对目录不报错），但读取会失败，
    // 此时如果只看 good() 就会把失败当成"读到了空文件"。必须先拦掉目录。
    if (isDir(path)) {
        if (error != nullptr) {
            *error = strFormat("路径是目录，不是文件: %s", path.c_str());
        }
        return {};
    }

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.good()) {
        if (error != nullptr) {
            *error = strFormat("无法打开文件: %s (errno=%d)", path.c_str(), errno);
        }
        return {};
    }

    std::ostringstream oss;
    oss << ifs.rdbuf();
    // 读到一半出错（badbit）时不能把残缺内容当成功返回
    if (ifs.bad()) {
        if (error != nullptr) {
            *error = strFormat("读取文件失败: %s (errno=%d)", path.c_str(), errno);
        }
        return {};
    }

    // 成功：清空 error，从而与失败区分开（空文件走这里，error 为空）
    if (error != nullptr) {
        error->clear();
    }
    return oss.str();
}

bool saveFile(const std::string &path, const std::string &data) {
    const std::string dir = dirname(path);
    if (!dir.empty() && dir != "." && !createDirectory(dir)) {
        return false;
    }

    // 原子写：先写同目录下的临时文件，成功后 rename(2) 原子替换。
    // 直接 trunc 原文件的话，写入期间并发读者会读到空/残缺内容，且写入失败会丢原数据
    // （实测旧实现：16MB 写入期间 45377 次轮询里有 25716 次读到空或残缺）。
    const std::string tmp = strFormat("%s.tmp.%llu", path.c_str(),
                                      static_cast<unsigned long long>(getThreadId()));
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs.good()) {
            return false;
        }
        ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
        ofs.flush();
        if (!ofs.good()) {
            // 写临时文件失败：原文件毫发无损，清掉临时文件即可
            (void) ::remove(tmp.c_str());
            return false;
        }
    }

    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        (void) ::remove(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace mzmedia
