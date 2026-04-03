// nv12_frame.h
#ifndef NV12_FRAME_H
#define NV12_FRAME_H

#include <memory>

struct AVFrame;

struct NV12Frame {
    int fd = -1;         // DMA-BUF fd
    int width = 0;
    int height = 0;
    int stride_w = 0;
    int stride_h = 0;
    std::shared_ptr<AVFrame> owner;

    bool empty() const {
        return fd < 0 || width <= 0 || height <= 0;
    }
};

#endif
