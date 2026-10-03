/*
 * util 单元测试
 */

#include "test_main.h"

#include "core/util.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <unistd.h>

using namespace mzmedia;

// ---------------------------------------------------------------------------
// 字符串
// ---------------------------------------------------------------------------

MZ_TEST(util_strformat) {
    MZ_ASSERT_STR_EQ(strFormat("%d-%s", 42, "abc"), "42-abc");
    MZ_ASSERT_STR_EQ(strFormat("%s", "中文测试"), "中文测试");
    MZ_ASSERT_STR_EQ(strFormat("%s|%s", "", "x"), "|x");

    // 长串不能被截断（strFormatV 先用 vsnprintf 探测长度）
    const std::string long_text(5000, 'x');
    MZ_ASSERT_EQ(strFormat("%s", long_text.c_str()).size(), 5000u);

    // 不含转换说明的格式串原样返回
    MZ_ASSERT_STR_EQ(strFormat("plain"), "plain");

    // 空格式串（用变量承载，避开 -Wformat-zero-length；带一个参数避开 -Wformat-security）：
    // 输出长度为 0，应返回空串
    const char *empty_fmt = "";
    MZ_ASSERT_STR_EQ(strFormat(empty_fmt, 0), "");
}

MZ_TEST(util_split) {
    const auto parts = split("a,b,c", ",");
    MZ_ASSERT_EQ(parts.size(), 3u);
    MZ_ASSERT_STR_EQ(parts[0], "a");
    MZ_ASSERT_STR_EQ(parts[1], "b");
    MZ_ASSERT_STR_EQ(parts[2], "c");

    // 多字符分隔串
    const auto multi = split("1::2::3", "::");
    MZ_ASSERT_EQ(multi.size(), 3u);
    MZ_ASSERT_STR_EQ(multi[1], "2");

    // 空片段保留
    const auto empties = split("a,,b", ",");
    MZ_ASSERT_EQ(empties.size(), 3u);
    MZ_ASSERT_STR_EQ(empties[1], "");

    // 尾随分隔符产生一个空片段（文档化行为）
    MZ_ASSERT_EQ(split("a,", ",").size(), 2u);

    // 无分隔符：单元素
    MZ_ASSERT_EQ(split("abc", ",").size(), 1u);
    MZ_ASSERT_STR_EQ(split("abc", ",")[0], "abc");

    // 空分隔串：不按字符拆，整体返回
    MZ_ASSERT_EQ(split("abc", "").size(), 1u);
    MZ_ASSERT_STR_EQ(split("abc", "")[0], "abc");
}

MZ_TEST(util_trim) {
    std::string padded = "  \t abc \r\n ";
    MZ_ASSERT_STR_EQ(trim(padded), "abc");

    // 全是待裁字符
    std::string blanks = "   ";
    MZ_ASSERT_STR_EQ(trim(blanks), "");

    // 空串
    std::string empty;
    MZ_ASSERT_STR_EQ(trim(empty), "");

    // 自定义字符集
    std::string custom = "xxabcxx";
    MZ_ASSERT_STR_EQ(trim(custom, "x"), "abc");

    // 中间字符不动
    std::string inner = " a b ";
    MZ_ASSERT_STR_EQ(trim(inner), "a b");
}

MZ_TEST(util_case) {
    std::string text = "MzMeDia-123";
    MZ_ASSERT_STR_EQ(toLower(text), "mzmedia-123");
    MZ_ASSERT_STR_EQ(toUpper(text), "MZMEDIA-123");
}

MZ_TEST(util_prefix_suffix) {
    MZ_ASSERT_TRUE(startWith("mzmedia", "mz"));
    MZ_ASSERT_FALSE(startWith("mz", "mzmedia"));
    MZ_ASSERT_TRUE(startWith("mzmedia", ""));
    MZ_ASSERT_TRUE(startWith("", ""));
    MZ_ASSERT_FALSE(startWith("", "a"));

    MZ_ASSERT_TRUE(endWith("mzmedia", "media"));
    MZ_ASSERT_FALSE(endWith("mz", "media"));
    MZ_ASSERT_TRUE(endWith("mzmedia", ""));
    MZ_ASSERT_FALSE(endWith("", "a"));
}

MZ_TEST(util_replace_join) {
    MZ_ASSERT_STR_EQ(replace("a.b.c", ".", "-"), "a-b-c");
    MZ_ASSERT_STR_EQ(replace("abc", "x", "-"), "abc");
    // from 为空串时直接返回原串，不能死循环
    MZ_ASSERT_STR_EQ(replace("abc", "", "-"), "abc");
    // 替换结果又被扫描到的情况：a -> aa 只应替换原始匹配
    MZ_ASSERT_STR_EQ(replace("a", "a", "aa"), "aa");

    const std::vector<std::string> parts{"a", "b", "c"};
    MZ_ASSERT_STR_EQ(join(parts, ","), "a,b,c");
    MZ_ASSERT_STR_EQ(join(parts, ""), "abc");
    MZ_ASSERT_STR_EQ(join(std::vector<std::string>{}, ","), "");
}

MZ_TEST(util_path) {
    MZ_ASSERT_STR_EQ(basename("/a/b/c.log"), "c.log");
    MZ_ASSERT_STR_EQ(basename("c.log"), "c.log");
    MZ_ASSERT_STR_EQ(basename("/a/b/"), "");

    MZ_ASSERT_STR_EQ(dirname("/a/b/c.log"), "/a/b");
    MZ_ASSERT_STR_EQ(dirname("c.log"), ".");
    MZ_ASSERT_STR_EQ(dirname("/c.log"), "/");
}

// ---------------------------------------------------------------------------
// 时间：系统时钟 vs 单调时钟
// ---------------------------------------------------------------------------

MZ_TEST(util_time) {
    // 单调时钟只用于求差，必须随时间递增
    const uint64_t mono_begin = getCurrentMillisecond(false);
    const uint64_t mono_us_begin = getCurrentMicrosecond(false);
    sleepMs(20);
    const uint64_t mono_end = getCurrentMillisecond(false);

    MZ_ASSERT_GE(mono_end - mono_begin, 15u);   // 允许少量调度误差
    MZ_ASSERT_LT(mono_end - mono_begin, 500u);
    MZ_ASSERT_GE(getCurrentMicrosecond(false) - mono_us_begin, 15000u);

    // 系统时钟应与 time(nullptr) 同量级（允许 2 秒误差）
    const uint64_t wall_ms = getCurrentMillisecond(true);
    const time_t wall_sec = getCurrentSecond();
    MZ_ASSERT_NEAR(static_cast<double>(wall_ms / 1000), static_cast<double>(wall_sec), 2.0);

    // 时间字符串："YYYY-MM-DD HH:MM:SS.mmm" 共 23 字符
    const std::string with_ms = getTimeStrWithMs();
    MZ_ASSERT_EQ(with_ms.size(), 23u);
    MZ_ASSERT_STR_EQ(with_ms.substr(4, 1), "-");
    MZ_ASSERT_STR_EQ(with_ms.substr(10, 1), " ");
    MZ_ASSERT_STR_EQ(with_ms.substr(19, 1), ".");

    // 指定时间戳：必须用非 0 的真实时间戳验证（t == 0 被约定为"当前时间"，
    // 原来的 size() == 4 是弱断言 —— 任何年份都是 4 个字符，测不出任何东西）
    MZ_ASSERT_STR_EQ(getTimeStr("%Y", 1000000000), "2001");
    MZ_ASSERT_STR_EQ(getTimeStr("%Y", 0), getTimeStr("%Y"));   // 0 等价于"当前时间"
}

// ---------------------------------------------------------------------------
// 线程 / 进程
// ---------------------------------------------------------------------------

MZ_TEST(util_thread) {
    MZ_ASSERT_GT(getThreadId(), 0u);

    setThreadName("mz-util-t");
    MZ_ASSERT_STR_EQ(getThreadName(), "mz-util-t");

    // 超长线程名会被显式截断到 15 个字符（不依赖 glibc 的 ERANGE 行为）
    setThreadName("mzmedia-thread-name-too-long");
    MZ_ASSERT_STR_EQ(getThreadName(), "mzmedia-thread-");
    MZ_ASSERT_LE(getThreadName().size(), 15u);

    // 可执行文件信息
    MZ_ASSERT_TRUE(endWith(exePath(), exeName()));
    MZ_ASSERT_TRUE(isDir(exeDir()));
    MZ_ASSERT_FALSE(exeName().empty());
    // 防御式约定：无法定位时也必须返回当前目录，绝不能是空串
    // （否则调用方拼 exeDir() + "/logs" 会得到根路径 "/logs"）
    MZ_ASSERT_FALSE(exeDir().empty());

    // 还原，避免影响其他用例的日志输出
    setThreadName("mz-util-t");
}

// ---------------------------------------------------------------------------
// 文件系统
// ---------------------------------------------------------------------------

MZ_TEST(util_file) {
    const std::string root = "/tmp/mzmedia_util_test";
    const std::string nested = root + "/a/b";
    (void) ::system(("rm -rf " + root).c_str());

    // 递归创建
    MZ_ASSERT_FALSE(isDir(nested));
    MZ_ASSERT_TRUE(createDirectory(nested));
    MZ_ASSERT_TRUE(isDir(nested));
    MZ_ASSERT_TRUE(createDirectory(nested));   // 幂等

    const std::string path = nested + "/x.txt";
    MZ_ASSERT_FALSE(fileExists(path));

    MZ_ASSERT_TRUE(saveFile(path, "hello 中文"));
    MZ_ASSERT_TRUE(fileExists(path));

    std::string error;
    MZ_ASSERT_STR_EQ(loadFile(path, &error), "hello 中文");
    MZ_ASSERT_TRUE(error.empty());

    // 读不存在的文件：返回空串并给出错误原因
    std::string error2;
    MZ_ASSERT_STR_EQ(loadFile(nested + "/none.txt", &error2), "");
    MZ_ASSERT_FALSE(error2.empty());

    // saveFile 自动创建父目录
    MZ_ASSERT_TRUE(saveFile(root + "/x/y/z.txt", "z"));
    MZ_ASSERT_TRUE(fileExists(root + "/x/y/z.txt"));

    // 目录不算是文件
    MZ_ASSERT_FALSE(fileExists(nested));

    (void) ::system(("rm -rf " + root).c_str());
}

/*
 * 相对路径回归测试
 *
 * 曾经的 bug：createDirectory("logs") 内部把相对路径误拼成绝对路径 "/logs"，
 * 于是去根目录建目录、非 root 下必然 EACCES 失败，连带 FileWriter("logs/x.log")
 * 静默写不出日志（打开失败被忽略）。
 * 之前的用例全部使用绝对路径，所以完全没有覆盖到这条路径。
 */
MZ_TEST(util_file_relative) {
    const std::string root = "/tmp/mzmedia_util_rel";
    (void) ::system(("rm -rf " + root).c_str());
    MZ_ASSERT_TRUE(createDirectory(root));

    // 本用例必须真的改工作目录，才测得到"相对路径"语义 —— 用 RAII 守卫保证退出时还原。
    // 曾经收尾写 chdir("/")，把进程工作目录永久留在根目录：同一进程里后面所有依赖相对
    // 路径的用例全部失效（8 个 ffmpeg 用例假红），而 ctest 每个分组是独立进程把它掩盖了。
    {
        const ::mztest::ScopedCwd cwd(root.c_str());
        MZ_ASSERT_TRUE(cwd.ok);
        if (!cwd.ok) {
            return;   // 断言不中断执行，必须显式早退
        }

        // 单级 / 多级相对路径
        MZ_ASSERT_TRUE(createDirectory("logs"));
        MZ_ASSERT_TRUE(isDir("logs"));
        MZ_ASSERT_TRUE(createDirectory("logs/deep"));
        MZ_ASSERT_TRUE(isDir("logs/deep"));
        MZ_ASSERT_TRUE(createDirectory("logs/deep"));   // 幂等

        // 关键断言：绝不能跑到根目录下创建
        MZ_ASSERT_FALSE(isDir("/logs"));

        // saveFile 的相对路径也要走通
        MZ_ASSERT_TRUE(saveFile("data/x.txt", "rel"));
        MZ_ASSERT_TRUE(fileExists("data/x.txt"));
        MZ_ASSERT_TRUE(isDir("data"));
    }   // ← 离开作用域自动还原工作目录（不需要手写 chdir）

    (void) ::system(("rm -rf " + root).c_str());
}

/*
 * loadFile 的失败语义：必须能区分"空文件"与"失败"
 *
 * 曾经的 bug：ifstream 在 Linux 上能成功"打开"目录，读取失败后代码又把 error 清空，
 * 于是返回空串且 error 为空 —— 调用方完全无法察觉失败，属于最隐蔽的静默失败。
 */
MZ_TEST(util_load_file_semantics) {
    const std::string root = "/tmp/mzmedia_load_test";
    (void) ::system(("rm -rf " + root).c_str());
    MZ_ASSERT_TRUE(createDirectory(root));

    // 1) 空文件：成功，error 必须为空
    MZ_ASSERT_TRUE(saveFile(root + "/empty.txt", ""));
    std::string error = "sentinel";
    MZ_ASSERT_STR_EQ(loadFile(root + "/empty.txt", &error), "");
    MZ_ASSERT_TRUE(error.empty());

    // 2) 目录：必须失败且 error 非空（曾经的静默失败点）
    error = "sentinel";
    MZ_ASSERT_STR_EQ(loadFile(root, &error), "");
    MZ_ASSERT_FALSE(error.empty());

    // 3) 不存在的文件：必须失败
    error = "sentinel";
    MZ_ASSERT_STR_EQ(loadFile(root + "/none.txt", &error), "");
    MZ_ASSERT_FALSE(error.empty());

    // 4) 正常读取
    MZ_ASSERT_TRUE(saveFile(root + "/a.txt", "hello 中文"));
    error = "sentinel";
    MZ_ASSERT_STR_EQ(loadFile(root + "/a.txt", &error), "hello 中文");
    MZ_ASSERT_TRUE(error.empty());

    (void) ::system(("rm -rf " + root).c_str());
}

/*
 * saveFile 必须是原子写
 *
 * 旧实现直接 open(trunc)：写入期间并发读者会读到"被清空后的残缺状态"
 * （实测 16MB 写入期间 45377 次轮询中有 25716 次读到空或残缺），
 * 且写入中途失败会让原数据永久丢失。
 * 本用例在写入期间持续轮询，要求只能看到"旧的 3 字节"或"完整新大小"。
 */
MZ_TEST(util_save_file_atomic) {
    const std::string root = "/tmp/mzmedia_save_atomic";
    (void) ::system(("rm -rf " + root).c_str());
    const std::string path = root + "/data.bin";
    MZ_ASSERT_TRUE(saveFile(path, "OLD"));

    constexpr size_t kSize = 16 * 1024 * 1024;
    std::atomic<size_t> polls{0};
    std::atomic<size_t> torn{0};
    std::atomic<bool> stop{false};
    std::thread poller([&]() {
        while (!stop.load()) {
            std::ifstream ifs(path, std::ios::binary | std::ios::ate);
            if (!ifs.good()) {
                continue;
            }
            const long long size = static_cast<long long>(ifs.tellg());
            ++polls;
            // 合法状态只有两种：旧的 3 字节，或完整的新大小
            if (size != 3 && size != static_cast<long long>(kSize)) {
                ++torn;
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    MZ_ASSERT_TRUE(saveFile(path, std::string(kSize, 'x')));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stop = true;
    poller.join();

    MZ_ASSERT_GT(polls.load(), 0u);
    MZ_ASSERT_EQ(torn.load(), 0u);   // 关键：一次残缺都不能出现

    // 临时文件必须已被 rename 掉，不留残渣
    MZ_ASSERT_FALSE(fileExists(strFormat("%s.tmp.%llu", path.c_str(),
                                        static_cast<unsigned long long>(getThreadId()))));
    std::string error;
    MZ_ASSERT_EQ(loadFile(path, &error).size(), kSize);

    (void) ::system(("rm -rf " + root).c_str());
}
