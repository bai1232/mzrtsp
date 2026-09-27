# ============================================================================
# 单元测试：自研轻量测试框架（零第三方依赖）
#
# 用例命名约定：MZ_TEST(<分组>_<用例名>)
# 本文件按"分组前缀"为每个分组注册一个独立的 ctest 用例，便于定位失败。
#
# 新增测试文件时做两件事：
#   1) 把 .cpp 追加进 MZMEDIA_TEST_SOURCES
#   2) 若引入了新的分组前缀，把分组名追加进 MZMEDIA_TEST_GROUPS
# 注意：分组名与用例名对不上时，该分组会因"零用例"被判为失败（防止假绿）。
# ============================================================================

set(MZMEDIA_TEST_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_main.cpp
        # ---- 第 2 批 ----
        # ${CMAKE_CURRENT_SOURCE_DIR}/test_util.cpp
        # ${CMAKE_CURRENT_SOURCE_DIR}/test_logger.cpp
        # ---- 第 3 批 ----
        # ${CMAKE_CURRENT_SOURCE_DIR}/test_task_queue.cpp
        # ${CMAKE_CURRENT_SOURCE_DIR}/test_thread_pool.cpp
        # ${CMAKE_CURRENT_SOURCE_DIR}/test_semaphore.cpp
        # ---- 第 4 批 ----
        # ${CMAKE_CURRENT_SOURCE_DIR}/test_core_misc.cpp
        )

set(MZMEDIA_TEST_GROUPS
        selftest
        # ---- 第 2 批 ----  util logger
        # ---- 第 3 批 ----  queue pool semaphore
        # ---- 第 4 批 ----  core
        )

add_executable(mzmedia_unittest ${MZMEDIA_TEST_SOURCES})

target_include_directories(mzmedia_unittest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}   # test_main.h
        ${CMAKE_SOURCE_DIR}/src)      # 被测代码（第 2 批起使用）

find_package(Threads REQUIRED)
target_link_libraries(mzmedia_unittest PRIVATE Threads::Threads)

# 骨架阶段（第 1 批）静态库目标尚不存在，存在时才链接
if(TARGET mzmedia)
    target_link_libraries(mzmedia_unittest PRIVATE mzmedia)
endif()

foreach(group IN LISTS MZMEDIA_TEST_GROUPS)
    add_test(NAME ${group} COMMAND mzmedia_unittest ${group})
    set_tests_properties(${group} PROPERTIES TIMEOUT 120)
endforeach()
