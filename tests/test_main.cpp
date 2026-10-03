/*
 * 单元测试入口（唯一的 main 编译单元）+ 测试框架自检用例
 * ============================================================================
 * 为什么需要这个文件：
 *   1. test_main.h 是 header-only 框架，但可执行文件必须由"恰好一个"翻译单元
 *      提供 main()，否则链接不出程序；
 *   2. 骨架阶段若一个用例都没有，mzmedia_unittest 会以"零用例"退出，
 *      而 ctest 会把它当成通过 —— 属于假绿。这里是第 1 批唯一的真实用例，
 *      用于验证"注册 → 断言 → 计数 → 退出码"这条链路本身可用。
 * ============================================================================
 */

#include "test_main.h"

#include "core/logger.h"

#include <cstdlib>
#include <cstring>

MZ_TEST(selftest_assertions) {
    // 基础断言：全部应当通过
    MZ_ASSERT_TRUE(1 + 1 == 2);
    MZ_ASSERT_FALSE(1 + 1 == 3);

    // 相等 / 不等（左值避免临时对象歧义）
    const int six = 2 * 3;
    MZ_ASSERT_EQ(six, 6);
    MZ_ASSERT_NE(six, 7);

    // 大小比较
    const int three = 3;
    MZ_ASSERT_GE(three, 3);
    MZ_ASSERT_GT(three, 2);
    MZ_ASSERT_LE(three, 3);
    MZ_ASSERT_LT(three, 4);

    // 字符串：char* 与 std::string 混用，验证 toStr 重载
    const std::string joined = std::string("mz") + "media";
    MZ_ASSERT_STR_EQ(joined, "mzmedia");
    MZ_ASSERT_STR_EQ("mzmedia", joined);

    // 指针
    const char *nonNull = "value";
    const char *nullValue = nullptr;
    MZ_ASSERT_NOT_NULL(nonNull);
    MZ_ASSERT_NULL(nullValue);

    // 浮点近似
    const double sum = 0.1 + 0.2;
    MZ_ASSERT_NEAR(sum, 0.3, 1e-9);
}

MZ_TEST(selftest_describe) {
    // 可打印类型走 operator<<
    MZ_ASSERT_STR_EQ(::mztest::describe(42), "42");
    MZ_ASSERT_STR_EQ(::mztest::describe("abc"), "abc");

    // 不可打印类型降级为占位串，而不是编译失败
    struct Opaque {
        int x;
    };
    MZ_ASSERT_STR_EQ(::mztest::describe(Opaque{1}), "<类型不可打印>");
}

MZ_TEST(selftest_near_tolerance) {
    // 容差边界：差异等于容差算通过，超过则失败（这里只验证通过的一侧）
    MZ_ASSERT_NEAR(1.0, 1.5, 0.5);
}

MZ_TEST(selftest_scoped_cwd_restores) {
    // 锁定"工作目录守卫"本身：进入前记录、离开作用域必须回到原处。
    // 为什么值得单测：这个守卫是"用例之间不隔离"的堵漏机制，它自己坏了必须立刻红。
    const std::string before = ::mztest::currentCwd();
    MZ_ASSERT_FALSE(before.empty());

    const std::string target = "/tmp/mzmedia_scoped_cwd";
    (void) ::system(("rm -rf " + target).c_str());
    MZ_ASSERT_TRUE(mzmedia::createDirectory(target));

    {
        const ::mztest::ScopedCwd cwd(target.c_str());
        MZ_ASSERT_TRUE(cwd.ok);
        if (!cwd.ok) {
            return;   // 断言不中断执行，必须显式早退
        }
        // 作用域内确实换了目录，否则这个守卫什么都没测到（假绿）
        MZ_ASSERT_STR_EQ(::mztest::currentCwd(), target);
    }

    MZ_ASSERT_STR_EQ(::mztest::currentCwd(), before);
    (void) ::system(("rm -rf " + target).c_str());
}

int main(int argc, char **argv) {
    // 诊断开关：MZ_TEST_LOG=1 时把库日志（InfoP/WarnP/ErrorP）打到控制台。
    // 为什么需要：Logger 默认**没有任何 writer**，调试失败用例时"库明明报了错却看不见"，
    // 只能靠猜。默认关闭是为了不被"故意触发的错误路径用例"刷屏。
    if (const char *log = ::getenv("MZ_TEST_LOG"); log != nullptr && std::strcmp(log, "0") != 0) {
        mzmedia::Logger::Instance().add(std::make_shared<mzmedia::ConsoleWriter>());
        mzmedia::Logger::Instance().setLevel(mzmedia::LogLevel::Debug);
    }
    return mztest::runMain(argc, argv);
}
