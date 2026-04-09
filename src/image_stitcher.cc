#include "image_stitcher.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#include <rga/im2d.h>
#include <rga/RgaApi.h>
#include <rga/rga.h>

#include "logger.h"

namespace {
/**
 * @brief NV12像素格式常量定义
 * 用于RGA库的图像格式设置，表示YUV 4:2:0平面格式
 */
constexpr int kNv12Format = RK_FORMAT_YCbCr_420_SP;

/**
 * @brief 将像素值向下归一化为偶数
 * 用于确保DMA-BUF操作的宽高为偶数（NV12格式要求）
 * @param value 输入值
 * @return 不小于0且为偶数的值
 */
int NormalizeEvenFloor(int value) {
    return std::max(0, value & ~1);
}

/**
 * @brief 将像素值向上归一化为偶数
 * 用于确保DMA-BUF操作的宽高为偶数（NV12格式要求）
 * @param value 输入值
 * @return 不小于2且为偶数的值
 */
int NormalizeEvenCeil(int value) {
    return std::max(2, (value + 1) & ~1);
}

/**
 * @brief 判断旋转角度是否为直角旋转
 * RGA硬件仅支持0/90/180/270度的四个直角旋转
 * @param rotation_deg 旋转角度（度数）
 * @return 如果是支持的直角旋转返回true，否则返回false
 */
bool IsRightAngleRotation(int rotation_deg) {
    return rotation_deg == 0 || rotation_deg == 90 ||
           rotation_deg == 180 || rotation_deg == 270;
}

/**
 * @brief 将旋转角度转换为RGA库格式
 * 将度数转换为RGA硬件支持的旋转操作标志
 * @param rotation_deg 旋转角度（0/90/180/270）
 * @return RGA旋转操作标志（IM_HAL_TRANSFORM_ROT_*）
 */
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

/**
 * @brief RGA位操作函数
 * 使用RGA硬件加速在目标DMA-BUF上进行指定矩形区域的复制
 * @param src_fd 源DMA-BUF文件描述符
 * @param src_stride_w 源图像行宽度（像素）
 * @param src_stride_h 源图像列高度（像素）
 * @param src_x 源矩形左上角X坐标
 * @param src_y 源矩形左上角Y坐标
 * @param copy_w 复制矩形宽度
 * @param copy_h 复制矩形高度
 * @param dst_fd 目标DMA-BUF文件描述符
 * @param dst_stride_w 目标图像行宽度（像素）
 * @param dst_stride_h 目标图像列高度（像素）
 * @param dst_x 目标矩形左上角X坐标
 * @param dst_y 目标矩形左上角Y坐标
 * @return 如果操作成功返回true，否则返回false
 */
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

/**
 * @brief ImageStitcher构造函数
 * 初始化类成员变量
 */
ImageStitcher::ImageStitcher()
    : num_img_(0), blend_width_(0), out_w_(0), out_h_(0) {}

/**
 * @brief ImageStitcher析构函数
 * 释放所有临时缓冲区
 */
ImageStitcher::~ImageStitcher() {
    ReleaseScratchBuffers();
}

/**
 * @brief 设置拼接参数
 * 配置拼接器的基本参数：相机数量、输出全景图尺寸等
 * @param blend_width 混合宽度（用于阴影处理，当前版本未使用）
 * @param num_img 输入相机数量
 * @param out_w 输出全景图宽度（像素）
 * @param out_h 输出全景图高度（像素）
 */
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

/**
 * @brief 设置拼接布局
 * 配置每个输入相机的拼接任务参数（源ROI、目标位置、旋转角度等）
 * @param tasks 拼接任务向量，每个元素对应一个相机的处理参数
 */
void ImageStitcher::SetLayout(const std::vector<StitchTask>& tasks) {
    tasks_ = tasks;
}

/**
 * @brief 清空输出缓冲区
 * 使用RGA硬件加速将输出全景图用黑色（0x00000000）填充
 * @param output 输出NV12帧（包含DMA-BUF信息）
 */
void ImageStitcher::ClearOutput(const NV12Frame& output) const {
    if (output.empty()) {
        return;
    }

    rga_buffer_t dst = wrapbuffer_fd(output.fd, output.width, output.height, kNv12Format);
    im_rect full_rect = {0, 0, output.width, output.height};
    imfill(dst, full_rect, 0x00000000);
}

/**
 * @brief 确保临时缓冲区存在且尺寸正确
 * 如果缓冲区不存在或尺寸不匹配，则分配新的DRM缓冲区；否则复用现有缓冲区
 * @param buffers 缓冲区向量指针
 * @param img_idx 图像索引（通道号）
 * @param width 需要的缓冲区宽度
 * @param height 需要的缓冲区高度
 * @return 如果缓冲区就绪返回true，否则返回false
 */
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

/**
 * @brief 释放所有临时缓冲区
 * 释放用于裁剪和旋转的临时DRM DMA-BUF
 */
void ImageStitcher::ReleaseScratchBuffers() {
    for (DrmBuffer& buffer : crop_buffers_) {
        drm_free(buffer);
    }
    for (DrmBuffer& buffer : rotate_buffers_) {
        drm_free(buffer);
    }
}

/**
 * @brief 计算旋转后的图像宽度
 * 对于90/270度旋转，宽高互换；对于0/180度旋转，宽度不变
 * @param task 拼接任务配置
 * @return 旋转后的图像宽度
 */
int ImageStitcher::RotatedWidth(const StitchTask& task) const {
    return (task.rotation_deg == 90 || task.rotation_deg == 270) ? task.src_h : task.src_w;
}

/**
 * @brief 计算旋转后的图像高度
 * 对于90/270度旋转，宽高互换；对于0/180度旋转，高度不变
 * @param task 拼接任务配置
 * @return 旋转后的图像高度
 */
int ImageStitcher::RotatedHeight(const StitchTask& task) const {
    return (task.rotation_deg == 90 || task.rotation_deg == 270) ? task.src_w : task.src_h;
}

/**
 * @brief 拼接处理主函数
 * 执行单个相机的完整拼接流程（可选旋转后复制到输出全景图）
 * 
 * 处理步骤：
 * 1. 验证输入参数有效性
 * 2. 从输入帧中按ROI参数进行裁剪
 * 3. 如果需要旋转，先旋转后再复制；否则直接复制
 * 4. 使用RGA硬件加速实现零拷贝操作
 * 
 * 处理3种情况：
 * - 无旋转(0度): 直接使用RGA位操作从源复制到目标
 * - 有旋转(90/180/270度): 先裁剪到临时缓冲区，再旋转到临时缓冲区，最后复制到输出
 * - 无效旋转角度: 记录错误并跳过该相机
 * 
 * @param img_idx 输入相机索引（通道号）
 * @param 未使用的帧索引参数
 * @param inputs 输入的多相机NV12硬件帧向量
 * @param output 输出的拼接全景图（NV12硬件帧）
 */
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
