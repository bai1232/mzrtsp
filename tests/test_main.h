/*
 * mzmedia 轻量单元测试框架（header-only，零第三方依赖）
 * ============================================================================
 * 设计取舍：
 *   1. 不引入 gtest / Catch2 —— 遵守"零第三方依赖"约束，离线可构建；
 *   2. header-only + C++17 inline 变量 —— 多个测试 TU 各自 include 即可，
 *      用例注册表与计数器跨 TU 唯一，不需要额外的初始化代码；
 *   3. 断言默认"非致命"（失败后继续跑完当前用例）—— 一次运行暴露更多问题，
 *      需要立即中断时用 MZ_FAIL；
 *   4. 用例名重复会被拒绝并导致整轮失败 —— 防止复制粘贴时悄悄覆盖。
 *   5. 【单线程】断言计数器与用例注册表都不是线程安全的，**禁止在工作线程里调用
 *      MZ_ASSERT_***：线程里只记录状态（std::atomic / 普通变量），断言回到测试
 *      主线程再做。否则既是数据竞争（TSAN 会报），计数也会丢。
 *
 * 用例命名约定（必须遵守）：
 *   MZ_TEST(<分组>_<用例名>)，例如 MZ_TEST(util_split)、MZ_TEST(logger_roll)
 *   tests/CMakeLists.txt 按分组前缀注册 ctest 用例，分组名必须与其
 *   MZMEDIA_TEST_GROUPS 中的名字一致。
 *
 * 用法：
 *   MZ_TEST(util_trim) {
 *       std::string s = "  a  ";
 *       MZ_ASSERT_STR_EQ(mzmedia::trim(s), "a");
 *   }
 *
 * 运行：
 *   ./mzmedia_unittest              # 跑全部
 *   ./mzmedia_unittest util         # 只跑名字包含 "util" 的用例
 *   ./mzmedia_unittest --list       # 列出全部用例
 * ============================================================================
 */

#pragma once

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <unistd.h>

namespace mztest {

// ---------------------------------------------------------------------------
// 用例描述与注册表
// ---------------------------------------------------------------------------
struct TestCase {
    const char *name;
    const char *file;
    int line;
    void (*fn)();
};

inline std::vector<TestCase> &registry() {
    static std::vector<TestCase> cases;
    return cases;
}

// 用例名重复 / 空名 等"名单错误"计数：非零则整轮测试失败
inline int &rosterErrors() {
    static int errors = 0;
    return errors;
}

// 已执行的断言总数
inline int &assertCount() {
    static int count = 0;
    return count;
}

// 失败断言总数（同时作为进程退出码依据）
inline int &failCount() {
    static int failures = 0;
    return failures;
}

// 当前正在执行的用例名，用于失败信息定位
inline const char *&currentTest() {
    static const char *name = "";
    return name;
}

// ---------------------------------------------------------------------------
// 输出着色（仅在 TTY 下着色，避免污染 ctest 日志）
// ---------------------------------------------------------------------------
inline bool useColor() {
    static const bool enabled = (::isatty(STDOUT_FILENO) != 0);
    return enabled;
}

inline const char *color(const char *code) {
    return useColor() ? code : "";
}

inline const char *kRed = "\033[31m";
inline const char *kGreen = "\033[32m";
inline const char *kYellow = "\033[33m";
inline const char *kCyan = "\033[36m";
inline const char *kReset = "\033[0m";

inline void registerTest(const char *name, const char *file, int line, void (*fn)()) {
    if (name == nullptr || *name == '\0') {
        ++rosterErrors();
        std::fprintf(stderr, "%s测试框架错误：存在空用例名 (%s:%d)%s\n", color(kRed), file, line,
                     color(kReset));
        return;
    }
    for (const auto &existing : registry()) {
        if (std::strcmp(existing.name, name) == 0) {
            ++rosterErrors();
            std::fprintf(stderr, "%s测试框架错误：用例名重复 \"%s\" (%s:%d 与 %s:%d)%s\n",
                         color(kRed), name, existing.file, existing.line, file, line, color(kReset));
            return;
        }
    }
    registry().push_back(TestCase{name, file, line, fn});
}

struct Registrar {
    Registrar(const char *name, const char *file, int line, void (*fn)()) {
        registerTest(name, file, line, fn);
    }
};

// ---------------------------------------------------------------------------
// 值的可读化输出（类型不可打印时降级为占位串，避免编译期报错）
// ---------------------------------------------------------------------------
template<typename T, typename = void>
struct IsStreamable : std::false_type {};

template<typename T>
struct IsStreamable<T, std::void_t<decltype(std::declval<std::ostream &>()
                                            << std::declval<const T &>())>> : std::true_type {};

template<typename T>
std::string describe(const T &value) {
    if constexpr (IsStreamable<T>::value) {
        std::ostringstream oss;
        oss << value;
        return oss.str();
    } else {
        return "<类型不可打印>";
    }
}

inline std::string toStr(const std::string &value) {
    return value;
}

inline std::string toStr(const char *value) {
    return value == nullptr ? std::string("(null)") : std::string(value);
}

// ---------------------------------------------------------------------------
// 工作目录守卫（RAII）
//   为什么必须有：::chdir() 改的是**进程级全局状态**，用例忘记还原会污染同一进程里
//   后面所有用例。真踩过：`util_file_relative` / `logger_file_relative_path` 收尾写
//   chdir("/") 而不是切回进入前的目录 → 8 个依赖相对路径的 ffmpeg 用例在"单进程全量跑"
//   时假红；而 ctest 每个分组是独立进程，把这个问题掩盖了 4 个里程碑。
//   用法：需要改目录的用例一律
//       { ::mztest::ScopedCwd cwd("/tmp/x"); if (!cwd.ok) return; ... }
//   离开作用域自动还原；框架还会在用例结束后做一次不变量检查（见 runAll）。
// ---------------------------------------------------------------------------
inline std::string currentCwd() {
    char buf[4096];
    if (::getcwd(buf, sizeof(buf)) == nullptr) {
        return {};   // 取不到不算错，但不变量检查会退化为"跳过检查"
    }
    return std::string(buf);
}

struct ScopedCwd {
    std::string original;
    bool ok = false;   // 进入是否成功：失败必须能被调用方察觉，不静默继续

    explicit ScopedCwd(const char *dir) {
        original = currentCwd();
        ok = (dir != nullptr) && (::chdir(dir) == 0);
    }
    ScopedCwd(const ScopedCwd &) = delete;
    ScopedCwd &operator=(const ScopedCwd &) = delete;
    ~ScopedCwd() {
        if (!original.empty()) {
            (void) ::chdir(original.c_str());
        }
    }
};

// ---------------------------------------------------------------------------
// 失败上报
// ---------------------------------------------------------------------------
inline void reportFailure(const char *file, int line, const std::string &message) {
    ++failCount();
    std::fprintf(stderr, "%s    ✗ [%s] %s:%d%s\n      %s\n", color(kRed), currentTest(), file, line,
                 color(kReset), message.c_str());
}

// ---------------------------------------------------------------------------
// 运行
// ---------------------------------------------------------------------------
inline void listTests() {
    std::printf("共 %zu 个用例：\n", registry().size());
    for (const auto &c : registry()) {
        std::printf("  %-40s (%s:%d)\n", c.name, c.file, c.line);
    }
}

inline void printUsage(const char *argv0) {
    std::printf("用法：%s [过滤关键字|--list|-h]\n", argv0);
    std::printf("  <无参数>  运行全部用例\n");
    std::printf("  <关键字>  只运行名字中包含该关键字的用例\n");
    std::printf("  --list    列出全部用例\n");
}

inline int runAll(const std::string &filter) {
    int executed = 0;
    int passed = 0;
    int failed_cases = 0;   // 失败的是**用例**数；断言失败数另算（见下方汇总行）
    const auto beginAll = std::chrono::steady_clock::now();

    std::printf("%s=== mzmedia 单元测试 | 注册 %zu 个用例%s ===%s\n", color(kCyan),
                registry().size(), filter.empty() ? "" : (", 过滤: " + filter).c_str(),
                color(kReset));

    for (const auto &c : registry()) {
        if (!filter.empty() && std::string(c.name).find(filter) == std::string::npos) {
            continue;
        }
        ++executed;
        currentTest() = c.name;
        const int failuresBefore = failCount();
        const auto begin = std::chrono::steady_clock::now();

        std::printf("%s[ RUN  ]%s %s\n", color(kYellow), color(kReset), c.name);

        const std::string cwdBefore = currentCwd();
        try {
            c.fn();
        } catch (const std::exception &e) {
            reportFailure(c.file, c.line, std::string("用例抛出未捕获异常: ") + e.what());
        } catch (...) {
            reportFailure(c.file, c.line, "用例抛出未知类型异常");
        }

        // 不变量：用例不得把进程工作目录留在别处。
        // 为什么放在框架里、而不是靠各用例自觉：工作目录是**跨用例的隐式全局状态**，
        // 忘记还原时，报错会落在后面某个无辜的用例上（真踩过：8 个 ffmpeg 用例假红，
        // 真正的凶手是 util/logger 的两个"相对路径"用例）。在框架里检出 → 责任者当场点名。
        // 检出后**立刻还原**：一个污染源不该把后面几十个用例连带染红（雪崩会让定位变难）。
        if (!cwdBefore.empty()) {
            const std::string cwdAfter = currentCwd();
            if (cwdAfter != cwdBefore) {
                reportFailure(c.file, c.line,
                              "用例结束时进程工作目录被改动了（用例之间因此不隔离）"
                              "\n        用例开始: " + cwdBefore +
                              "\n        用例结束: " + cwdAfter +
                              "\n        修法: 用 ::mztest::ScopedCwd 包住 ::chdir（框架已还原并继续）");
                if (::chdir(cwdBefore.c_str()) != 0) {
                    std::fprintf(stderr, "%s    ! 框架还原工作目录失败，后续用例可能连带失败%s\n",
                                 color(kRed), color(kReset));
                }
            }
        }

        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - begin)
                                .count();
        if (failCount() == failuresBefore) {
            ++passed;
            std::printf("%s[ PASS ]%s %s (%lld ms)\n", color(kGreen), color(kReset), c.name,
                        static_cast<long long>(ms));
        } else {
            ++failed_cases;
            std::printf("%s[ FAIL ]%s %s (%lld ms)\n", color(kRed), color(kReset), c.name,
                        static_cast<long long>(ms));
        }
    }

    const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - beginAll)
                                 .count();

    std::printf("------------------------------------------------------------------------\n");
    // 单位必须写清楚：通过/失败是**用例**数，括号里的才是**断言**失败数。
    // 修前把 failCount()（断言数）与 passed（用例数）并列打印，会出现
    // "用例 18 个（通过 16 / 失败 3）" 这种自相矛盾的输出（3 是断言数）
    std::printf("用例 %d 个（通过 %s%d%s / 失败 %s%d%s）| 断言 %d 条（失败 %s%d%s）| 用时 %lld ms\n",
                executed, color(kGreen), passed, color(kReset),
                failed_cases > 0 ? color(kRed) : color(kGreen), failed_cases, color(kReset),
                assertCount(), color(kRed), failCount(), color(kReset),
                static_cast<long long>(totalMs));

    if (rosterErrors() > 0) {
        std::fprintf(stderr, "%s测试名单有 %d 处错误（用例名重复或为空），整轮判定为失败%s\n",
                     color(kRed), rosterErrors(), color(kReset));
        return 1;
    }

    if (executed == 0) {
        // 关键保护：一个用例都没跑（例如分组名写错）必须判为失败，
        // 否则 ctest 会把"什么都没测"当成绿色。
        std::fprintf(stderr, "%s没有匹配 \"%s\" 的用例，判定为失败%s\n", color(kRed),
                     filter.c_str(), color(kReset));
        return 1;
    }

    return failCount() == 0 ? 0 : 1;
}

inline int runMain(int argc, char **argv) {
    // 行缓冲，保证 ctest 能实时看到输出
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    std::string filter;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list") {
            listTests();
            return 0;
        }
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
        filter = arg;
    }
    return runAll(filter);
}

} // namespace mztest

// ---------------------------------------------------------------------------
// 用例定义宏
// ---------------------------------------------------------------------------
#define MZ_TEST(name)                                                     \
    static void mz_test_fn_##name();                                      \
    static const ::mztest::Registrar mz_test_reg_##name(#name, __FILE__,  \
                                                       __LINE__, &mz_test_fn_##name); \
    static void mz_test_fn_##name()

// ---------------------------------------------------------------------------
// 断言宏
//   说明：MZ_ASSERT_EQ 等宏内部使用 const auto&，两边类型不同时可能触发
//   -Wsign-compare（例如 size_t 与 int 比较），测试里请写 3u 而不是 3。
//   字符串比较请用 MZ_ASSERT_STR_EQ（避免退化为指针比较）。
// ---------------------------------------------------------------------------
#define MZ_ASSERT_TRUE(expr)                                                      \
    do {                                                                          \
        ++::mztest::assertCount();                                                \
        if (!(expr)) {                                                            \
            ::mztest::reportFailure(__FILE__, __LINE__, "期望为真: " #expr);      \
        }                                                                         \
    } while (0)

#define MZ_ASSERT_FALSE(expr)                                                     \
    do {                                                                          \
        ++::mztest::assertCount();                                                \
        if ((expr)) {                                                             \
            ::mztest::reportFailure(__FILE__, __LINE__, "期望为假: " #expr);      \
        }                                                                         \
    } while (0)

#define MZ_ASSERT_EQ(a, b)                                                              \
    do {                                                                               \
        ++::mztest::assertCount();                                                     \
        const auto &mz_a = (a);                                                        \
        const auto &mz_b = (b);                                                        \
        if (!(mz_a == mz_b)) {                                                         \
            ::mztest::reportFailure(__FILE__, __LINE__,                                \
                                    std::string("期望相等: " #a " == " #b)              \
                                            + "\n        实际: " + ::mztest::describe(mz_a) \
                                            + "\n        期望: " + ::mztest::describe(mz_b)); \
        }                                                                              \
    } while (0)

#define MZ_ASSERT_NE(a, b)                                                              \
    do {                                                                               \
        ++::mztest::assertCount();                                                     \
        const auto &mz_a = (a);                                                        \
        const auto &mz_b = (b);                                                        \
        if (!(mz_a != mz_b)) {                                                         \
            ::mztest::reportFailure(__FILE__, __LINE__,                                \
                                    std::string("期望不等: " #a " != " #b)              \
                                            + "\n        两者都是: " + ::mztest::describe(mz_a)); \
        }                                                                              \
    } while (0)

#define MZ_ASSERT_LT(a, b)                                                        \
    do {                                                                          \
        ++::mztest::assertCount();                                                \
        const auto &mz_a = (a);                                                   \
        const auto &mz_b = (b);                                                   \
        if (!(mz_a < mz_b)) {                                                     \
            ::mztest::reportFailure(__FILE__, __LINE__,                           \
                                    std::string("期望小于: " #a " < " #b)         \
                                            + "\n        左值: " + ::mztest::describe(mz_a) \
                                            + "\n        右值: " + ::mztest::describe(mz_b)); \
        }                                                                         \
    } while (0)

#define MZ_ASSERT_LE(a, b)                                                        \
    do {                                                                          \
        ++::mztest::assertCount();                                                \
        const auto &mz_a = (a);                                                   \
        const auto &mz_b = (b);                                                   \
        if (!(mz_a <= mz_b)) {                                                    \
            ::mztest::reportFailure(__FILE__, __LINE__,                           \
                                    std::string("期望小于等于: " #a " <= " #b)    \
                                            + "\n        左值: " + ::mztest::describe(mz_a) \
                                            + "\n        右值: " + ::mztest::describe(mz_b)); \
        }                                                                         \
    } while (0)

#define MZ_ASSERT_GT(a, b)                                                        \
    do {                                                                          \
        ++::mztest::assertCount();                                                \
        const auto &mz_a = (a);                                                   \
        const auto &mz_b = (b);                                                   \
        if (!(mz_a > mz_b)) {                                                     \
            ::mztest::reportFailure(__FILE__, __LINE__,                           \
                                    std::string("期望大于: " #a " > " #b)         \
                                            + "\n        左值: " + ::mztest::describe(mz_a) \
                                            + "\n        右值: " + ::mztest::describe(mz_b)); \
        }                                                                         \
    } while (0)

#define MZ_ASSERT_GE(a, b)                                                        \
    do {                                                                          \
        ++::mztest::assertCount();                                                \
        const auto &mz_a = (a);                                                   \
        const auto &mz_b = (b);                                                   \
        if (!(mz_a >= mz_b)) {                                                    \
            ::mztest::reportFailure(__FILE__, __LINE__,                           \
                                    std::string("期望大于等于: " #a " >= " #b)    \
                                            + "\n        左值: " + ::mztest::describe(mz_a) \
                                            + "\n        右值: " + ::mztest::describe(mz_b)); \
        }                                                                         \
    } while (0)

#define MZ_ASSERT_STR_EQ(a, b)                                                          \
    do {                                                                                \
        ++::mztest::assertCount();                                                      \
        const std::string mz_sa = ::mztest::toStr(a);                                   \
        const std::string mz_sb = ::mztest::toStr(b);                                   \
        if (mz_sa != mz_sb) {                                                           \
            ::mztest::reportFailure(__FILE__, __LINE__,                                 \
                                    std::string("期望字符串相等: " #a " == " #b)        \
                                            + "\n        实际: [" + mz_sa + "]"         \
                                            + "\n        期望: [" + mz_sb + "]");       \
        }                                                                               \
    } while (0)

#define MZ_ASSERT_NEAR(a, b, eps)                                                       \
    do {                                                                                \
        ++::mztest::assertCount();                                                      \
        const double mz_va = static_cast<double>(a);                                     \
        const double mz_vb = static_cast<double>(b);                                     \
        if (!(std::fabs(mz_va - mz_vb) <= static_cast<double>(eps))) {                   \
            ::mztest::reportFailure(__FILE__, __LINE__,                                 \
                                    std::string("期望近似相等: " #a " ≈ " #b)           \
                                            + "\n        实际差值: " + ::mztest::describe(mz_va - mz_vb) \
                                            + "\n        允许误差: " + ::mztest::describe(static_cast<double>(eps))); \
        }                                                                               \
    } while (0)

#define MZ_ASSERT_NULL(ptr)                                                        \
    do {                                                                          \
        ++::mztest::assertCount();                                                \
        if ((ptr) != nullptr) {                                                   \
            ::mztest::reportFailure(__FILE__, __LINE__, "期望为空指针: " #ptr);   \
        }                                                                         \
    } while (0)

#define MZ_ASSERT_NOT_NULL(ptr)                                                   \
    do {                                                                          \
        ++::mztest::assertCount();                                                \
        if ((ptr) == nullptr) {                                                   \
            ::mztest::reportFailure(__FILE__, __LINE__, "期望非空指针: " #ptr);   \
        }                                                                         \
    } while (0)

#define MZ_ASSERT_THROW(expr, ex_type)                                            \
    do {                                                                          \
        ++::mztest::assertCount();                                                \
        bool mz_caught = false;                                                   \
        try {                                                                     \
            expr;                                                                 \
        } catch (const ex_type &) {                                               \
            mz_caught = true;                                                     \
        } catch (...) {                                                           \
            mz_caught = true;                                                     \
            ::mztest::reportFailure(__FILE__, __LINE__,                           \
                                    "抛出异常类型不符: " #expr);                  \
        }                                                                         \
        if (!mz_caught) {                                                         \
            ::mztest::reportFailure(__FILE__, __LINE__,                           \
                                    "期望抛出 " #ex_type "，但没有异常: " #expr); \
        }                                                                         \
    } while (0)

#define MZ_FAIL(message)                                                          \
    do {                                                                          \
        ++::mztest::assertCount();                                                \
        ::mztest::reportFailure(__FILE__, __LINE__, (message));                   \
    } while (0)
