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
    char buf[1024] = {0};
    const ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) {
        return {};
    }
    return std::string(buf, static_cast<size_t>(len));
}

std::string exeDir() {
    const std::string path = exePath();
    return path.empty() ? std::string() : dirname(path);
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
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.good()) {
        if (error != nullptr) {
            *error = strFormat("无法打开文件: %s", path.c_str());
        }
        return {};
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
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
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.good()) {
        return false;
    }
    ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    ofs.flush();
    return ofs.good();
}

} // namespace mzmedia
