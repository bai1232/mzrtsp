cmake_minimum_required(VERSION 3.15)

project(mzmedia
        VERSION 0.1.0
        DESCRIPTION "多格式流媒体文件服务器：输入多种媒体文件，输出多种流媒体格式，按需转封装或转码"
        LANGUAGES CXX)

# ============================================================================
# 构建选项
# ============================================================================
option(MZMEDIA_BUILD_TESTS    "构建单元测试"                     ON)
option(MZMEDIA_BUILD_EXAMPLES "构建示例程序"                     ON)
option(MZMEDIA_ENABLE_ASAN    "开启 AddressSanitizer（排查内存/泄漏问题）" OFF)
option(MZMEDIA_ENABLE_TSAN    "开启 ThreadSanitizer（排查数据竞争）" OFF)

# 决策：先不开 -Werror，等所有批次零警告后再打开（可随时 -DMZMEDIA_WARN_AS_ERROR=ON 验证）
option(MZMEDIA_WARN_AS_ERROR  "把编译警告视为错误"               OFF)

# ============================================================================
# 全局编译参数
# ============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 便于 clangd / clang-tidy 使用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 决策：开发期默认 Debug（带 -g，便于 gdb / ASAN / 断言定位问题）。
# 发布构建必须显式指定，避免"以为在优化、实际在 Debug"：
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "构建类型 (Debug/Release/RelWithDebInfo/MinSizeRel)" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 编译器探测：本项目仅支持 GCC / Clang
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "mzmedia 仅支持 GCC 或 Clang（当前: ${CMAKE_CXX_COMPILER_ID}）")
endif()

# 警告：-Wall -Wextra，且不做任何 -Wno-xxx 抑制，保证"零警告"是真的零
add_compile_options(-Wall -Wextra)

if(MZMEDIA_WARN_AS_ERROR)
    add_compile_options(-Werror)
endif()

if(MZMEDIA_ENABLE_ASAN AND MZMEDIA_ENABLE_TSAN)
    message(FATAL_ERROR "ASAN 与 TSAN 不能同时开启（两者都依赖编译器插桩，互斥）")
endif()

if(MZMEDIA_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()

if(MZMEDIA_ENABLE_TSAN)
    # 并发组件（TaskQueue / ThreadPool / Semaphore）必须过 TSAN：
    # 数据竞争不会在单线程测试里暴露，只会以"偶发崩溃"的形式出现在生产环境。
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
endif()

# ============================================================================
# 源文件清单（显式列举，不使用 file(GLOB)）
#
# 按 M1 的批次逐步填充，固定后不再变化：
#   第 2 批 util / logger
#   第 3 批 task_queue / thread_pool / semaphore
#   第 4 批 notice_center
# （M2 起才追加 network 等模块；M4 才引入 FFmpeg）
# ============================================================================
set(MZMEDIA_SOURCES
        # 第 2 批
        src/core/util.cpp
        src/core/logger.cpp
        # 第 3 批
        src/core/task_queue.cpp
        src/core/thread_pool.cpp
        src/core/semaphore.cpp
        # 第 4 批
        # src/core/notice_center.cpp
        )

# 骨架阶段（第 1 批）没有任何源文件，此时不能创建 STATIC 库目标
# （CMake 不允许无源文件的 add_library），因此做存在性判断。
if(MZMEDIA_SOURCES)
    add_library(mzmedia STATIC ${MZMEDIA_SOURCES})
    add_library(mzmedia::mzmedia ALIAS mzmedia)

    target_include_directories(mzmedia
            PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
                   $<INSTALL_INTERFACE:include>)

    find_package(Threads REQUIRED)
    target_link_libraries(mzmedia PUBLIC Threads::Threads)

    target_compile_definitions(mzmedia PRIVATE MZMEDIA_VERSION="${PROJECT_VERSION}")
else()
    message(STATUS "mzmedia: 当前批次尚无源文件（骨架阶段），暂不创建静态库目标")
endif()

# ============================================================================
# 子目录
# ============================================================================
# examples/ 在第 4 批才创建，此处做存在性判断，加入后自动生效
if(MZMEDIA_BUILD_EXAMPLES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt)
    add_subdirectory(examples)
endif()

if(MZMEDIA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ============================================================================
# 安装规则
# ============================================================================
if(TARGET mzmedia)
    install(TARGETS mzmedia ARCHIVE DESTINATION lib)
    install(DIRECTORY src/ DESTINATION include/mzmedia FILES_MATCHING PATTERN "*.h")
endif()

message(STATUS "mzmedia ${PROJECT_VERSION} | 构建类型=${CMAKE_BUILD_TYPE} 编译器=${CMAKE_CXX_COMPILER_ID} "
               "| tests=${MZMEDIA_BUILD_TESTS} examples=${MZMEDIA_BUILD_EXAMPLES} "
               "asan=${MZMEDIA_ENABLE_ASAN} tsan=${MZMEDIA_ENABLE_TSAN} werror=${MZMEDIA_WARN_AS_ERROR}")
