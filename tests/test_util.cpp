/*
 * util 单元测试
 */

#include "test_main.h"

#include "core/util.h"

#include <cstdlib>
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

    // 指定时间戳
    MZ_ASSERT_EQ(getTimeStr("%Y", 0).size(), 4u);
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
    MZ_ASSERT_EQ(::chdir(root.c_str()), 0);

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

    // 还原工作目录，避免影响其它用例
    MZ_ASSERT_EQ(::chdir("/"), 0);
    (void) ::system(("rm -rf " + root).c_str());
}
