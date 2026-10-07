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
        src/core/notice_center.cpp
        # M2-1 网络层
        src/network/pipe_wrap.cpp
        src/network/event_poller.cpp
        # M2-3a 缓冲与 socket
        src/network/buffer.cpp
        src/network/socket.cpp
        # M2-3b 会话与服务器
        src/network/session.cpp
        src/network/tcp_server.cpp
        # M3-a HTTP 解析
        src/http/http_parser.cpp
        # M3-b HTTP 服务端
        src/http/http_response.cpp
        src/http/http_server.cpp
        # M4-a FFmpeg 封装
        src/ffmpeg/demuxer.cpp
        # M4-b CodecMatrix + H264 工具
        src/ffmpeg/codec_matrix.cpp
        src/ffmpeg/h264_util.cpp
        # M5-a 媒体分发（MediaPacket / FrameQueue / GopCache / MediaSource）
        src/media/media_packet.cpp
        src/media/frame_queue.cpp
        src/media/gop_cache.cpp
        src/media/media_source.cpp
        # M5-b 源线程
        src/media/source_pump.cpp
        # M5-c 真接 Demuxer + 源管理（懒启动 / 空闲释放）
        src/media/demuxer_producer.cpp
        src/media/source_manager.cpp
        # M5-d 节流（FR-3.5）
        src/media/throttle.cpp
        # M6-a FLV 封装（FR-3.1 / 3.3 / 3.4）
        src/output/flv_muxer.cpp
        # M6-b 每连接的 FLV 发送器
        src/output/flv_sender.cpp
        )

# 骨架阶段（第 1 批）没有任何源文件，此时不能创建 STATIC 库目标
# （CMake 不允许无源文件的 add_library），因此做存在性判断。
# ---- FFmpeg（M4 起**必需**；SPEC §7 锁定 4.x）----
# 缺库必须**明确报错**，而不是"悄悄少编一个模块"（那种假成功最难查）
find_path(AVFORMAT_INCLUDE_DIR libavformat/avformat.h)
find_library(AVFORMAT_LIB avformat)
find_library(AVCODEC_LIB avcodec)
find_library(AVUTIL_LIB avutil)
if(NOT AVFORMAT_INCLUDE_DIR OR NOT AVFORMAT_LIB OR NOT AVCODEC_LIB OR NOT AVUTIL_LIB)
    message(FATAL_ERROR
            "M4 起需要 FFmpeg 开发库：libavformat / libavcodec / libavutil。\n"
            "Ubuntu: sudo apt install -y libavformat-dev libavcodec-dev libavutil-dev")
endif()

if(MZMEDIA_SOURCES)
    add_library(mzmedia STATIC ${MZMEDIA_SOURCES})
    add_library(mzmedia::mzmedia ALIAS mzmedia)
    # FFmpeg 是**公开**依赖：使用方（examples/tests）会看到 libav* 的类型
    target_include_directories(mzmedia PUBLIC ${AVFORMAT_INCLUDE_DIR})
    target_link_libraries(mzmedia PUBLIC ${AVFORMAT_LIB} ${AVCODEC_LIB} ${AVUTIL_LIB})

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
# 应用可执行文件（M6-c，FR-7.2 的落地）：零参数启动的完整 HTTP-FLV 服务器
#
# 目标名不能与静态库同名（CMake 不允许两个 target 重名），所以 target 叫 mzmedia_app，
# **输出文件名**才是 mzmedia —— 用户看到的、脚本里调用的都是 build/bin/mzmedia。
# （M6-b 的 examples/flv_http_server 已在本批并入这里，见 docs/DESIGN_M6.md §3.7）
# ============================================================================
if(TARGET mzmedia)
    add_executable(mzmedia_app src/main.cpp)
    set_target_properties(mzmedia_app PROPERTIES OUTPUT_NAME mzmedia)
    target_link_libraries(mzmedia_app PRIVATE mzmedia)
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
    install(TARGETS mzmedia_app RUNTIME DESTINATION bin)
    install(DIRECTORY src/ DESTINATION include/mzmedia FILES_MATCHING PATTERN "*.h")
endif()

message(STATUS "mzmedia ${PROJECT_VERSION} | 构建类型=${CMAKE_BUILD_TYPE} 编译器=${CMAKE_CXX_COMPILER_ID} "
               "| tests=${MZMEDIA_BUILD_TESTS} examples=${MZMEDIA_BUILD_EXAMPLES} "
               "asan=${MZMEDIA_ENABLE_ASAN} tsan=${MZMEDIA_ENABLE_TSAN} werror=${MZMEDIA_WARN_AS_ERROR}")
