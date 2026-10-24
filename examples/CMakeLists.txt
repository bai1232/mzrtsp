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

# M9 起：MPEG-TS 封装示例（"demux → 封成 TS → 落盘"，配 scripts/ts_mux_test.sh 用 ffprobe 校验）
add_executable(ts_mux_demo ts_mux_demo.cpp)
target_link_libraries(ts_mux_demo PRIVATE mzmedia)

# M9 起：一路直播流的完整生命周期演示（DemuxerProducer → MediaSource → FlvSender → FLV 落盘）
# 只读现有 API、不改库代码；配置见 scripts/... 与文件头注释
add_executable(live_demo live_demo.cpp)
target_link_libraries(live_demo PRIVATE mzmedia)

# M10-a：转码示例（解封装 → 解码 → libswscale → x264 → FLV 落盘；NFR-5 的 speed 靠它量）
add_executable(transcode_demo transcode_demo.cpp)
target_link_libraries(transcode_demo PRIVATE mzmedia)

# M6-c：原 examples/flv_http_server.cpp 已**升级为应用**（src/main.cpp → build/bin/mzmedia）：
# 它是 v0.1 的交付物（零参数启动 FR-7.2），放在 examples/ 里会让人以为它只是"示例"。
# 验收脚本 scripts/flv_http_test.sh 已改指向 build/bin/mzmedia。
