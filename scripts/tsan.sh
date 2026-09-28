#!/usr/bin/env bash
# ============================================================================
# TSAN（ThreadSanitizer）并发检查脚本
#
# 用法：
#   ./scripts/tsan.sh              # 配置 + 编译 + 检查
#   ./scripts/tsan.sh --no-build   # 只检查（复用已有 build-tsan）
#
# ---------------------------------------------------------------------------
# 背景（务必先读，否则会误判）
#
# 1. [必须] setarch -R
#    本机 TSAN 与高熵 ASLR 冲突，直接运行会 FATAL：
#      "ThreadSanitizer: unexpected memory mapping 0x..."
#    因此统一用 `setarch -R` 关闭该进程的地址随机化（不需要 root）。
#
# 2. [已知误报] 带超时的等待
#    glibc 2.35 把 std::condition_variable 的超时接口（wait_for / wait_until）
#    实现为 pthread_cond_clockwait，而 GCC 11 的 libtsan **没有该拦截器**
#    （实测：libtsan 对 pthread_cond_timedwait 有拦截器，对 clockwait 为 0 个）。
#    TSAN 因此看不到超时等待内部的"解锁 → 睡眠 → 重锁"，误以为线程仍持锁，
#    于是误报 "double lock of a mutex" 以及随之而来的 "data race"。
#    最小复现：一个 30 行、完全正确的双线程 wait_for 程序同样被误报；
#    把 wait_for 换成 wait 则 0 报告。
#
# 3. [本脚本的策略] 不做任何抑制（抑制会把真竞争一起藏掉），改为按能力分组：
#      - 严格组（不碰超时等待）：**必须 0 报告**，否则脚本失败
#      - 已知误报组（用例名带 qtimed / ptimed）：允许报告，但必须通过签名校验：
#          ① 报告类型只能是 "double lock of a mutex" 或 "data race"
#          ② 每条 data race 的**两个访问点都必须带 (mutexes: ...) 标注**
#             —— 双方都持锁却报竞争，才是"TSAN 丢失 happens-before"的误报特征；
#                只要有一方没持锁，就是真竞争 → 脚本失败
#
# 4. [根治方案] 换掉 sanitizer 运行时即可彻底消除误报：
#      sudo apt install g++-12        # 或 clang
#      cmake -B build-tsan -DMZMEDIA_ENABLE_TSAN=ON -DCMAKE_CXX_COMPILER=g++-12
#    换好后可把 qtimed/ptimed 也并入严格组，要求全部 0 报告。
# ============================================================================
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-tsan"

# 注意：变量名刻意不叫 GROUPS —— bash 把 $GROUPS 保留为"当前用户所属组 ID 数组"，
# 赋值会被忽略，$GROUPS 会展开成 gid（例如 1000），导致循环只跑一次且组名是数字。
STRICT_GROUPS="selftest util logger queue pool semaphore"
FP_GROUPS="qtimed ptimed"

if [ "${1:-}" != "--no-build" ]; then
    echo "== 配置并编译 TSAN 构建 =="
    cmake -B "$BUILD" -DMZMEDIA_ENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=Debug "$ROOT" > /dev/null || exit 1
    cmake --build "$BUILD" -j"$(nproc)" > /dev/null || exit 1
fi

if [ ! -x "$BUILD/bin/mzmedia_unittest" ]; then
    echo "错误：找不到 $BUILD/bin/mzmedia_unittest，请先不带 --no-build 运行一次" >&2
    exit 1
fi

export TSAN_OPTIONS="halt_on_error=0"

real_findings=0
known_fp=0
total_tests=0

# $1=分组名  $2=strict|fp
check_group() {
    local group="$1" mode="$2" out rc reports cases failed
    out="$(setarch -R "$BUILD/bin/mzmedia_unittest" "$group" 2>&1)"
    rc=$?
    reports=$(printf '%s\n' "$out" | grep -c 'WARNING: ThreadSanitizer')
    cases=$(printf '%s\n' "$out" | grep -o '用例 [0-9]* 个' | head -1 | grep -o '[0-9]*')
    total_tests=$((total_tests + ${cases:-0}))

    # 注意：TSAN 报错时进程退出码是 66，**不是**用例失败，所以不能用退出码判断，
    # 必须读用例自身的统计行；用例失败或"没匹配到用例"都要判为真问题。
    failed=$(printf '%s\n' "$out" | grep -o '失败 [0-9]*' | head -1 | grep -o '[0-9]*')
    if [ -z "$failed" ] || [ "$failed" -gt 0 ]; then
        printf '  %-9s 用例 %-3s 报告 %-2s          ✗ 用例失败或未匹配到用例（退出码 %s）\n' \
               "$group" "${cases:-?}" "$reports" "$rc"
        real_findings=$((real_findings + 1))
        printf '%s\n' "$out" | tail -15
        return
    fi

    if [ "$reports" -eq 0 ]; then
        printf '  %-9s 用例 %-3s 报告 0            ✔\n' "$group" "${cases:-?}"
        return
    fi

    if [ "$mode" = "strict" ]; then
        printf '  %-9s 用例 %-3s 报告 %-2s          ✗ 严格组不允许有报告\n' "$group" "${cases:-?}" "$reports"
        real_findings=$((real_findings + reports))
        printf '%s\n' "$out" | grep -A10 'WARNING: ThreadSanitizer' | head -50
        return
    fi

    # 已知误报组：签名校验
    local bad=0 other_types races annotated
    other_types=$(printf '%s\n' "$out" | grep 'WARNING: ThreadSanitizer' | grep -vcE 'double lock of a mutex|data race' || true)
    [ "$other_types" -gt 0 ] && bad=1

    races=$(printf '%s\n' "$out" | grep -c 'WARNING: ThreadSanitizer: data race' || true)
    if [ "$races" -gt 0 ]; then
        annotated=$(printf '%s\n' "$out" | grep -cE '^ +(Write|Read|Previous write|Previous read) of size .*\(mutexes:' || true)
        # 每条竞争的两个访问点都应带互斥量标注，否则说明有一方没持锁 = 真竞争
        [ "$annotated" -lt $((races * 2)) ] && bad=1
    fi

    if [ "$bad" -eq 1 ]; then
        printf '  %-9s 用例 %-3s 报告 %-2s          ✗ 报告签名不符合已知误报特征\n' "$group" "${cases:-?}" "$reports"
        real_findings=$((real_findings + 1))
        printf '%s\n' "$out" | grep -A10 'WARNING: ThreadSanitizer' | head -50
    else
        printf '  %-9s 用例 %-3s 报告 %-2s          △ 已知误报（签名校验通过）\n' "$group" "${cases:-?}" "$reports"
        known_fp=$((known_fp + reports))
    fi
}

echo "== 严格组（不含超时等待，必须 0 报告）=="
for group in $STRICT_GROUPS; do
    check_group "$group" strict
done

echo "== 已知误报组（含 std::condition_variable 超时接口，需签名校验）=="
for group in $FP_GROUPS; do
    check_group "$group" fp
done

echo "------------------------------------------------------------------------"
echo "用例合计 $total_tests | 已知误报 $known_fp | 真问题 $real_findings"
if [ "$real_findings" -gt 0 ]; then
    echo "结论：发现 $real_findings 处需要处理的问题（报告已打印在上面）"
    exit 1
fi
if [ "$known_fp" -gt 0 ]; then
    echo "结论：无真问题；$known_fp 处为已知误报（原因见脚本头部第 2 条）"
    exit 0
fi
echo "结论：TSAN 全绿，无任何报告"
