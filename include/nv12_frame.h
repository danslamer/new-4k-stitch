// nv12_frame.h
#ifndef NV12_FRAME_H
#define NV12_FRAME_H

#include <memory>

struct AVFrame;
struct _GstSample;

struct NV12Frame {
    int fd = -1;         // DMA-BUF fd
    int width = 0;
    int height = 0;
    int stride_w = 0;
    int stride_h = 0;
    // owner 优先取 owner_gst_sample (gstreamer-rockchip 路径, vendor 推荐),
    // 若为空再回退 owner (老 ffmpeg rkmpp 路径, 2026-07 后基本不再用).
    // NV12Frame 析构时 owner refcount 归零 → GstBuffer/AVFrame 释放 → DMA-BUF fd 失效.
    std::shared_ptr<_GstSample> owner_gst_sample;
    std::shared_ptr<AVFrame> owner;

    bool empty() const {
        return fd < 0 || width <= 0 || height <= 0;
    }
};

#endif
