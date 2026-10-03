/*
 * AvPtr：FFmpeg 对象的 RAII 包装（M4-a）
 * ============================================================================
 * 形状来源：docs/DESIGN_M4.md §3.1
 *
 * 为什么用模板 + 释放函数指针：FFmpeg 的释放接口都是 `void f(T**)`（会把指针置空），
 * 直接包成 unique_ptr 的自定义 deleter 最省心 —— 不需要每种类型写一个类，
 * 也不会漏掉释放（FFmpeg 的泄漏是最难查的一类：跑一小时才显形）。
 *
 * 约定（AI_COLLAB §4）：工厂失败**返回 nullptr 并记 ErrorP**，不抛异常、不静默。
 * ============================================================================
 */

#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <memory>

#include "core/logger.h"

namespace mzmedia {

/// 通用删除器：Free 的签名固定是 `void f(T**)`（FFmpeg 会把指针置空）
template<typename T, void (*Free)(T **)>
struct AvDeleter {
    void operator()(T *ptr) const {
        if (ptr != nullptr) {
            Free(&ptr);
        }
    }
};

template<typename T, void (*Free)(T **)>
using AvPtr = std::unique_ptr<T, AvDeleter<T, Free>>;

using AvFormatCtx = AvPtr<AVFormatContext, avformat_close_input>;
using AvCodecCtx = AvPtr<AVCodecContext, avcodec_free_context>;
using AvPacket = AvPtr<AVPacket, av_packet_free>;
using AvFrame = AvPtr<AVFrame, av_frame_free>;
using AvDictionary = AvPtr<AVDictionary, av_dict_free>;

/// @return nullptr = 分配失败（已记 ErrorP，调用方必须检查）
inline AvPacket makePacket() {
    AVPacket *pkt = av_packet_alloc();
    if (pkt == nullptr) {
        ErrorP("AvPtr: av_packet_alloc 失败（内存不足？）");
        return AvPacket(nullptr);
    }
    return AvPacket(pkt);
}

inline AvFrame makeFrame() {
    AVFrame *frame = av_frame_alloc();
    if (frame == nullptr) {
        ErrorP("AvPtr: av_frame_alloc 失败（内存不足？）");
        return AvFrame(nullptr);
    }
    return AvFrame(frame);
}

} // namespace mzmedia
