/*
 * onceToken：作用域退出时自动执行收尾动作的 RAII 小工具
 * ============================================================================
 * 典型用途：
 *   1. 在函数里成对地做"进入/离开"处理（打点、计数、恢复现场）；
 *   2. 测试里验证某个路径是否被走到；
 *   3. 临时改了一个全局开关，离开作用域时自动恢复 —— 这类"恢复现场"最容易漏，
 *      交给 RAII 就不会漏。
 *
 * 注意：析构函数里**不要抛异常**，否则在栈展开过程中会 std::terminate。
 * ============================================================================
 */

#pragma once

#include <functional>
#include <utility>

namespace mzmedia {

class onceToken {
public:
    using function = std::function<void()>;

    /**
     * @param on_construct 构造时立即调用（可为 nullptr）
     * @param on_destruct  析构时调用（可为 nullptr）
     */
    explicit onceToken(function on_construct = nullptr, function on_destruct = nullptr)
            : _on_destruct(std::move(on_destruct)) {
        if (on_construct) {
            on_construct();
        }
    }

    ~onceToken() {
        if (_on_destruct) {
            _on_destruct();
        }
    }

    onceToken(const onceToken &) = delete;
    onceToken &operator=(const onceToken &) = delete;
    onceToken(onceToken &&) = delete;
    onceToken &operator=(onceToken &&) = delete;

    /// 取消析构回调（例如某个错误分支已经把现场恢复过了）
    void dismiss() { _on_destruct = nullptr; }

private:
    function _on_destruct;
};

} // namespace mzmedia
