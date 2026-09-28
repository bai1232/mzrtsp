/*
 * mzmedia 伞头文件（umbrella header）
 * ============================================================================
 * 用途：给"使用方"提供单行包含入口。
 *
 *     #include "mzmedia.h"      // 等价于包含全部公开模块头
 *
 * 包含约定（重要，写进 CONTRIBUTING.md 与 docs/ARCHITECTURE.md）：
 *   1. 【使用方】examples/、tests/、外部集成 —— 优先使用本伞头，少写 include。
 *   2. 【库内部】src/ 下的 .cpp 实现 —— **禁止**包含本文件，只包含自己需要的
 *      模块头。原因：伞头会引入无关依赖，拖慢编译，并掩盖模块间真实耦合。
 *   3. 本文件只做 include，不放任何声明、定义或宏。
 *
 * 依赖分层（只允许上层依赖下层，反向依赖视为设计缺陷）：
 *
 *     core → network → http → ffmpeg → media → output
 *
 * 接入计划（M1 各批次逐步把实际 include 加进来）：
 *   第 2 批  core/util.h  core/logger.h
 *   第 3 批  core/task_queue.h  core/thread_pool.h  core/semaphore.h
 *   第 4 批  core/ticker.h  core/once_token.h  core/task_cancelable.h
 *            core/thread_group.h  core/notice_center.h
 *   M2 起    network/ 下的各头文件
 *   M3 起    http/ 下的各头文件
 *   M4 起    ffmpeg/ 下的各头文件
 *   M5 起    media/、output/、stats/ 下的各头文件
 * ============================================================================
 */

#pragma once

// ---------------------------------------------------------------------------
// 实际 include 区（随批次逐步填充）
// ---------------------------------------------------------------------------
// 第 2 批（已接入）
#include "core/util.h"
#include "core/logger.h"

// 第 3 批（已接入）
#include "core/task_queue.h"
#include "core/thread_pool.h"
#include "core/semaphore.h"

// 第 4 批（已接入）
#include "core/ticker.h"
#include "core/once_token.h"
#include "core/task_cancelable.h"
#include "core/thread_group.h"
#include "core/notice_center.h"
