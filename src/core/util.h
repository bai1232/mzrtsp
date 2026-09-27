/*
 * mzmedia 通用工具：字符串、时间、线程/进程、文件
 * ============================================================================
 * 设计约定：
 *   1. 零第三方依赖，只依赖 libstdc++ 与 POSIX（本项目仅支持 Linux）；
 *   2. 尽量 header 里只声明，实现放 .cpp，避免每处 include 都拖编译；
 *   3. 不做"万能工具类"，只放各模块复用的基础函数。
 * ============================================================================
 */

#pragma once

#include <cstdarg>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace mzmedia {

// ---------------------------------------------------------------------------
// 字符串
// ---------------------------------------------------------------------------

/**
 * printf 风格格式化，返回 std::string
 * @param fmt 格式串（带编译期格式检查）
 * @return 格式化结果；fmt 为空或格式化失败时返回空串
 */
std::string strFormat(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/**
 * strFormat 的 va_list 版本（供自身是变参函数的上层复用）
 */
std::string strFormatV(const char *fmt, va_list ap);

/**
 * 按分隔串切分字符串
 * @param delim 支持多字符分隔串；为空串时返回只含原串的单元素数组（不拆字符）
 * @note 空片段会被保留，例如 split("a::b", "::") -> {"a", "b"}，
 *       split("a,,b", ",") -> {"a", "", "b"}
 */
std::vector<std::string> split(const std::string &str, const char *delim);

/**
 * 去掉首尾指定字符（原地修改并返回引用，便于链式调用）
 */
std::string &trim(std::string &str, const char *chars = " \t\r\n");

/**
 * 转小写 / 转大写（原地修改并返回引用）
 */
std::string &toLower(std::string &str);
std::string &toUpper(std::string &str);

bool startWith(const std::string &str, const std::string &prefix);
bool endWith(const std::string &str, const std::string &suffix);

/**
 * 全局替换；from 为空串时直接返回原串（避免死循环）
 */
std::string replace(const std::string &str, const std::string &from, const std::string &to);

/**
 * 用分隔串拼接
 */
std::string join(const std::vector<std::string> &parts, const std::string &sep);

/**
 * 取路径最后一段："/a/b/c.log" -> "c.log"；无分隔符时返回原串
 */
std::string basename(const std::string &path);

/**
 * 取路径目录部分："/a/b/c.log" -> "/a/b"；无分隔符时返回 "."
 */
std::string dirname(const std::string &path);

// ---------------------------------------------------------------------------
// 时间
// ---------------------------------------------------------------------------

/**
 * 当前毫秒时间戳
 *
 * @param system false（默认）→ **单调时钟**（steady_clock）
 *                 绝对值没有意义，只能用于计算时间差（计时、超时、RTT），
 *                 不受 NTP 校时或手动改系统时间影响，**计时必须用这个**。
 *               true → **系统时钟**（system_clock）
 *                 可与 `date` 命令对比，适合做日志时间戳、导出给外部系统；
 *                 但会被校时影响，**绝不能用于计时**。
 */
uint64_t getCurrentMillisecond(bool system = false);

/**
 * 当前微秒时间戳，system 参数含义同 getCurrentMillisecond()
 */
uint64_t getCurrentMicrosecond(bool system = false);

/**
 * 当前墙钟秒（始终使用系统时钟）
 * @note 需要"经过了多少秒"请用 getCurrentMillisecond() 求差，不要用本函数相减
 */
time_t getCurrentSecond();

/**
 * 时间戳转字符串
 * @param fmt strftime 格式
 * @param t   时间戳；**t == 0 被约定为"当前时间"**，因此本函数无法表示
 *            1970-01-01 00:00:00 这一时刻（约定值占用了合法取值）
 */
std::string getTimeStr(const char *fmt = "%Y-%m-%d %H:%M:%S", time_t t = 0);

/**
 * 当前时间的 "年-月-日 时:分:秒.毫秒" 字符串，供日志行使用
 */
std::string getTimeStrWithMs();

// ---------------------------------------------------------------------------
// 线程 / 进程
// ---------------------------------------------------------------------------

/**
 * 设置当前线程名（Linux 上限 16 字节含 '\0'，超出会被内核截断）
 */
void setThreadName(const char *name);

/**
 * 获取当前线程名（未设置过时可能返回空串）
 */
std::string getThreadName();

/**
 * 当前线程的 Linux 内核线程号（gettid），用于日志定位
 */
uint64_t getThreadId();

/**
 * 当前线程休眠
 * @note 使用单调时钟实现，不受系统时间调整影响
 */
void sleepMs(uint32_t ms);

/**
 * 可执行文件绝对路径
 * @note 读取 /proc/self/exe；路径超长时会自动扩容重试（readlink 不保证 NUL 结尾，
 *       缓冲区装不下会静默截断）；读取失败返回空串
 */
std::string exePath();

/**
 * 可执行文件所在目录（无结尾 '/'）
 * @note 无法定位可执行文件时返回 "."（当前目录）而不是空串 ——
 *       否则调用方拼接 exeDir() + "/logs" 会得到 "/logs"（根目录），非常危险
 */
std::string exeDir();

/// 可执行文件名；无法定位时返回空串
std::string exeName();

// ---------------------------------------------------------------------------
// 文件系统
// ---------------------------------------------------------------------------

/**
 * 是否是**普通文件**（regular file）
 * @note 只认 S_ISREG：目录、字符/块设备（如 /dev/null）、FIFO、socket 一律返回 false。
 *       需要"该路径已存在"请用 fileExists(path) || isDir(path)
 */
bool fileExists(const std::string &path);

/// 是否是目录（判断时跟随符号链接）
bool isDir(const std::string &path);

/**
 * 递归创建目录（等价 mkdir -p）
 * @return 目录已存在或创建成功返回 true
 */
bool createDirectory(const std::string &path);

/**
 * 读取整个文件
 *
 * 失败语义（必须靠 error 区分，因为空文件与失败都返回空串）：
 *   - 成功（含空文件）：返回内容，*error 被清空
 *   - 失败（不存在 / 无权限 / 路径是目录 / 读到一半出错）：返回空串，*error 非空
 * @param error 非空时写入失败原因
 */
std::string loadFile(const std::string &path, std::string *error = nullptr);

/**
 * 覆盖写入整个文件，父目录不存在时自动创建
 *
 * **原子写**：先写同目录下的临时文件，成功后用 rename(2) 原子替换。
 * 因此并发读者要么看到旧内容、要么看到完整新内容，不会读到"被清空后的残缺状态"；
 * 写入失败时原文件保持不变（旧实现直接 trunc，会先清空原文件）。
 *
 * @return 全部成功才返回 true；失败时会清理临时文件
 */
bool saveFile(const std::string &path, const std::string &data);

} // namespace mzmedia
