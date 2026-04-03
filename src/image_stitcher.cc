#include "image_stitcher.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#include <rga/im2d.h>
#include <rga/RgaApi.h>
#include <rga/rga.h>

#include "logger.h"

namespace {
constexpr int kNv12Format = RK_FORMAT_YCbCr_420_SP;

int NormalizeEvenFloor(int value) {
    return std::max(0, value & ~1);
}

int NormalizeEvenCeil(int value) {
    return std::max(2, (value + 1) & ~1);
}

bool IsRightAngleRotation(int rotation_deg) {
    return rotation_deg == 0 || rotation_deg == 90 ||
           rotation_deg == 180 || rotation_deg == 270;
}

int ToRgaRotation(int rotation_deg) {
    switch (rotation_deg) {
        case 90:
            return IM_HAL_TRANSFORM_ROT_90;
        case 180:
            return IM_HAL_TRANSFORM_ROT_180;
        case 270:
            return IM_HAL_TRANSFORM_ROT_270;
        case 0:
        default:
            return 0;
    }
}

bool BlitByRect(int src_fd,
                int src_stride_w,
                int src_stride_h,
                int src_x,
                int src_y,
                int copy_w,
                int copy_h,
                int dst_fd,
                int dst_stride_w,
                int dst_stride_h,
                int dst_x,
                int dst_y) {
    rga_info_t src_info;
    std::memset(&src_info, 0, sizeof(src_info));
    src_info.fd = src_fd;
    src_info.mmuFlag = 1;
    rga_set_rect(&src_info.rect,
                 src_x, src_y,
                 copy_w, copy_h,
                 src_stride_w, src_stride_h,
                 kNv12Format);

    rga_info_t dst_info;
    std::memset(&dst_info, 0, sizeof(dst_info));
    dst_info.fd = dst_fd;
    dst_info.mmuFlag = 1;
    rga_set_rect(&dst_info.rect,
                 dst_x, dst_y,
                 copy_w, copy_h,
                 dst_stride_w, dst_stride_h,
                 kNv12Format);

    return c_RkRgaBlit(&src_info, &dst_info, nullptr) == 0;
}
}

ImageStitcher::ImageStitcher()
    : num_img_(0), blend_width_(0), out_w_(0), out_h_(0) {}

ImageStitcher::~ImageStitcher() {
    ReleaseScratchBuffers();
}

void ImageStitcher::SetParams(int blend_width,
                              int num_img,
                              int out_w,
                              int out_h) {
    num_img_ = num_img;
    blend_width_ = blend_width;
    out_w_ = out_w;
    out_h_ = out_h;

    warp_mutex_ = std::vector<std::mutex>(num_img_);
    crop_buffers_.resize(num_img_);
    rotate_buffers_.resize(num_img_);
}

void ImageStitcher::SetLayout(const std::vector<StitchTask>& tasks) {
    tasks_ = tasks;
}

void ImageStitcher::ClearOutput(const NV12Frame& output) const {
    if (output.empty()) {
        return;
    }

    rga_buffer_t dst = wrapbuffer_fd(output.fd, output.width, output.height, kNv12Format);
    im_rect full_rect = {0, 0, output.width, output.height};
    imfill(dst, full_rect, 0x00000000);
}

bool ImageStitcher::EnsureScratchBuffer(std::vector<DrmBuffer>* buffers,
                                        int img_idx,
                                        int width,
                                        int height) {
    if (buffers == nullptr || img_idx < 0 || img_idx >= static_cast<int>(buffers->size())) {
        return false;
    }

    DrmBuffer& buffer = (*buffers)[img_idx];
    if (buffer.fd >= 0 && buffer.width == width && buffer.height == height) {
        return true;
    }

    drm_free(buffer);
    return drm_alloc_nv12(width, height, buffer) == 0;
}

void ImageStitcher::ReleaseScratchBuffers() {
    for (DrmBuffer& buffer : crop_buffers_) {
        drm_free(buffer);
    }
    for (DrmBuffer& buffer : rotate_buffers_) {
        drm_free(buffer);
    }
}

int ImageStitcher::RotatedWidth(const StitchTask& task) const {
    return (task.rotation_deg == 90 || task.rotation_deg == 270) ? task.src_h : task.src_w;
}

int ImageStitcher::RotatedHeight(const StitchTask& task) const {
    return (task.rotation_deg == 90 || task.rotation_deg == 270) ? task.src_w : task.src_h;
}

void ImageStitcher::WarpImages(int img_idx,
                               size_t,
                               const std::vector<NV12Frame>& inputs,
                               NV12Frame& output) {
    if (img_idx < 0 || img_idx >= static_cast<int>(inputs.size()) || output.empty()) {
        return;
    }
    if (img_idx >= static_cast<int>(tasks_.size())) {
        return;
    }

    const NV12Frame& in = inputs[img_idx];
    const StitchTask& task = tasks_[img_idx];
    if (in.empty() || !task.enabled) {
        return;
    }
    if (!IsRightAngleRotation(task.rotation_deg)) {
        std::ostringstream stream;
        stream << "[ImageStitcher] cam" << img_idx
               << " invalid rotation_deg=" << task.rotation_deg
               << ", only 0/90/180/270 are supported.";
        Logger::GetInstance().LogError(stream.str());
        return;
    }

    const int src_x = NormalizeEvenFloor(std::min(task.src_x, in.width));
    const int src_y = NormalizeEvenFloor(std::min(task.src_y, in.height));
    const int src_w = NormalizeEvenFloor(
        std::min(task.src_w > 0 ? task.src_w : in.width - src_x, in.width - src_x));
    const int src_h = NormalizeEvenFloor(
        std::min(task.src_h > 0 ? task.src_h : in.height - src_y, in.height - src_y));
    if (src_w < 2 || src_h < 2) {
        return;
    }

    rga_buffer_t src = wrapbuffer_fd(
        in.fd,
        in.width,
        in.height,
        in.stride_w > 0 ? in.stride_w : in.width,
        in.stride_h > 0 ? in.stride_h : in.height,
        kNv12Format);

    rga_buffer_t dst = wrapbuffer_fd(
        output.fd,
        output.width,
        output.height,
        output.stride_w > 0 ? output.stride_w : output.width,
        output.stride_h > 0 ? output.stride_h : output.height,
        kNv12Format);

    im_rect src_rect = {src_x, src_y, src_w, src_h};
    StitchTask normalized_task = task;
    normalized_task.src_x = src_x;
    normalized_task.src_y = src_y;
    normalized_task.src_w = src_w;
    normalized_task.src_h = src_h;
    const int rotated_w = NormalizeEvenCeil(RotatedWidth(normalized_task));
    const int rotated_h = NormalizeEvenCeil(RotatedHeight(normalized_task));
    const int dst_x = NormalizeEvenFloor(task.dst_x);
    const int dst_y = NormalizeEvenFloor(task.dst_y);
    if (dst_x + rotated_w > output.width || dst_y + rotated_h > output.height) {
        std::ostringstream stream;
        stream << "[ImageStitcher] cam" << img_idx
               << " dst rect out of panorama bounds: "
               << "dst=(" << dst_x << "," << dst_y << "," << rotated_w << "," << rotated_h << ")"
               << " output=(" << output.width << "x" << output.height << ")";
        Logger::GetInstance().LogError(stream.str());
        return;
    }

    if (task.rotation_deg == 0) {
        if (!BlitByRect(in.fd,
                        in.stride_w > 0 ? in.stride_w : in.width,
                        in.stride_h > 0 ? in.stride_h : in.height,
                        src_x,
                        src_y,
                        rotated_w,
                        rotated_h,
                        output.fd,
                        output.stride_w > 0 ? output.stride_w : output.width,
                        output.stride_h > 0 ? output.stride_h : output.height,
                        dst_x,
                        dst_y)) {
            Logger::GetInstance().LogError("[ImageStitcher] c_RkRgaBlit failed on direct copy path.");
        }
        return;
    }

    if (!EnsureScratchBuffer(&crop_buffers_, img_idx, src_w, src_h) ||
        !EnsureScratchBuffer(&rotate_buffers_, img_idx, rotated_w, rotated_h)) {
        Logger::GetInstance().LogError("[ImageStitcher] failed to allocate scratch DMA-BUF for rotation.");
        return;
    }

    DrmBuffer& crop = crop_buffers_[img_idx];
    DrmBuffer& rotated = rotate_buffers_[img_idx];
    rga_buffer_t crop_buf = wrapbuffer_fd(
        crop.fd,
        crop.width,
        crop.height,
        static_cast<int>(crop.pitch),
        crop.height,
        kNv12Format);
    rga_buffer_t rotated_buf = wrapbuffer_fd(
        rotated.fd,
        rotated.width,
        rotated.height,
        static_cast<int>(rotated.pitch),
        rotated.height,
        kNv12Format);

    imcrop(src, crop_buf, src_rect);
    imrotate(crop_buf, rotated_buf, ToRgaRotation(task.rotation_deg));

    if (!BlitByRect(rotated.fd,
                    static_cast<int>(rotated.pitch),
                    rotated.height,
                    0,
                    0,
                    rotated_w,
                    rotated_h,
                    output.fd,
                    output.stride_w > 0 ? output.stride_w : output.width,
                    output.stride_h > 0 ? output.stride_h : output.height,
                    dst_x,
                    dst_y)) {
        Logger::GetInstance().LogError("[ImageStitcher] c_RkRgaBlit failed on rotated copy path.");
    }
}
