# ============================================================================
# 示例程序
#
# 约定：examples/ 属于"使用方"，只包含伞头 <mzmedia.h>，
#       不直接 include core/ 下的模块头（见 docs/ARCHITECTURE.md 的包含约定）。
# ============================================================================

if(NOT TARGET mzmedia)
    message(STATUS "examples: 静态库目标不存在（骨架阶段），跳过示例构建")
    return()
endif()

add_executable(core_demo core_demo.cpp)
target_link_libraries(core_demo PRIVATE mzmedia)
