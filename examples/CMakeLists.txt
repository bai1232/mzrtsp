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

# M2 起：网络层可运行示例（同时是 FR-4.4 / NFR-2 的验收工具，配 scripts/echo_test.sh）
add_executable(echo_server echo_server.cpp)
target_link_libraries(echo_server PRIVATE mzmedia)

# M3 起：HTTP 服务示例（ROADMAP 的 "curl 取到测试页" 验收靠它，配 scripts/http_test.sh）
add_executable(http_server http_server.cpp)
target_link_libraries(http_server PRIVATE mzmedia)

# M6-a 起：FLV 封装示例（"demux → 封成 FLV → 落盘"，配 scripts/flv_mux_test.sh 用 ffprobe 校验）
add_executable(flv_mux_demo flv_mux_demo.cpp)
target_link_libraries(flv_mux_demo PRIVATE mzmedia)

# M6-b 起：HTTP-FLV 服务器示例（配 scripts/flv_http_test.sh：curl 拉流 + ffprobe 校验）
add_executable(flv_http_server flv_http_server.cpp)
target_link_libraries(flv_http_server PRIVATE mzmedia)
