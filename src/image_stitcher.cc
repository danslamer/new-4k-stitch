#include "image_stitcher.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#include <rga/im2d.h>
#include <rga/RgaApi.h>
#include <rga/rga.h>

#include "logger.h"

#include <cmath>
#ifndef CL_IMPORT_TYPE_ARM
#define CL_IMPORT_TYPE_ARM 0x40B2
#endif
#ifndef CL_IMPORT_TYPE_DMA_BUF_ARM
#define CL_IMPORT_TYPE_DMA_BUF_ARM 0x40B4
#endif
typedef cl_mem(CL_API_CALL *PFN_clImportMemoryARM)(cl_context, cl_mem_flags, const cl_import_properties_arm*, void*, size_t, cl_int*);
static PFN_clImportMemoryARM pfn_clImportMemoryARM = nullptr;

extern double g_feather_strength;

const char* ocl_kernel_src = R"CLC(
int get_src_x(int X, int Y, int t_dst_x, int t_dst_y, int t_src_x, int t_src_y, int t_rot, int t_sw, int t_sh) {
    int lx = X - t_dst_x;
    int ly = Y - t_dst_y;
    if(t_rot == 0) return t_src_x + lx;
    if(t_rot == 90) return t_src_x + ly;
    if(t_rot == 180) return t_src_x + t_sw - 1 - lx;
    if(t_rot == 270) return t_src_x + t_sh - 1 - ly;
    return t_src_x + lx;
}
int get_src_y(int X, int Y, int t_dst_x, int t_dst_y, int t_src_x, int t_src_y, int t_rot, int t_sw, int t_sh) {
    int lx = X - t_dst_x;
    int ly = Y - t_dst_y;
    if(t_rot == 0) return t_src_y + ly;
    if(t_rot == 90) return t_src_y + t_sw - 1 - lx;
    if(t_rot == 180) return t_src_y + t_sh - 1 - ly;
    if(t_rot == 270) return t_src_y + lx;
    return t_src_y + ly;
}

__kernel void blend_seam(
    __global const uchar* img1, int s1_w, int s1_h, int str1, int h_str1, int d1_x, int d1_y, int sx1, int sy1, int rot1,
    __global const uchar* img2, int s2_w, int s2_h, int str2, int h_str2, int d2_x, int d2_y, int sx2, int sy2, int rot2,
    __global const uchar* mask, int is_vertical,
    __global uchar* out_img, int out_w, int out_h, int out_str, int out_h_str,
    int seam_x, int seam_y, int blend_w, int blend_h)
{
    int gx = get_global_id(0) * 2;
    int gy = get_global_id(1) * 2;
    if (gx >= blend_w || gy >= blend_h) return;
    
    __global const uchar* uv1 = img1 + str1 * h_str1;
    __global const uchar* uv2 = img2 + str2 * h_str2;
    __global       uchar* uv_out = out_img + out_str * out_h_str;
    
    int alpha = mask[is_vertical ? gx : gy];
    int inv_a = 255 - alpha;

    for(int dy = 0; dy < 2; ++dy) {
        for(int dx = 0; dx < 2; ++dx) {
            int out_px = seam_x + gx + dx;
            int out_py = seam_y + gy + dy;
            
            int px1 = get_src_x(out_px, out_py, d1_x, d1_y, sx1, sy1, rot1, s1_w, s1_h);
            int py1 = get_src_y(out_px, out_py, d1_x, d1_y, sx1, sy1, rot1, s1_w, s1_h);
            int y1 = img1[py1 * str1 + px1];
            
            int px2 = get_src_x(out_px, out_py, d2_x, d2_y, sx2, sy2, rot2, s2_w, s2_h);
            int py2 = get_src_y(out_px, out_py, d2_x, d2_y, sx2, sy2, rot2, s2_w, s2_h);
            int y2 = img2[py2 * str2 + px2];
            
            out_img[out_py * out_str + out_px] = (uchar)((y1 * inv_a + y2 * alpha) / 255);
        }
    }
    
    int out_px = seam_x + gx;
    int out_py = seam_y + gy;
    int px1 = get_src_x(out_px, out_py, d1_x, d1_y, sx1, sy1, rot1, s1_w, s1_h) / 2 * 2;
    int py1 = get_src_y(out_px, out_py, d1_x, d1_y, sx1, sy1, rot1, s1_w, s1_h) / 2;
    int px2 = get_src_x(out_px, out_py, d2_x, d2_y, sx2, sy2, rot2, s2_w, s2_h) / 2 * 2;
    int py2 = get_src_y(out_px, out_py, d2_x, d2_y, sx2, sy2, rot2, s2_w, s2_h) / 2;
    
    int u1 = uv1[py1 * str1 + px1];
    int v1 = uv1[py1 * str1 + px1 + 1];
    int u2 = uv2[py2 * str2 + px2];
    int v2 = uv2[py2 * str2 + px2 + 1];
    
    int out_uv_y = out_py / 2;
    int out_uv_x = out_px / 2 * 2;
    uv_out[out_uv_y * out_str + out_uv_x]     = (uchar)((u1 * inv_a + u2 * alpha) / 255);
    uv_out[out_uv_y * out_str + out_uv_x + 1] = (uchar)((v1 * inv_a + v2 * alpha) / 255);
}
)CLC";

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
    gles_warper_.Shutdown();
    CleanupOpenCL();
}

void ImageStitcher::InitOpenCL() {
    cl_uint num_platforms;
    clGetPlatformIDs(0, nullptr, &num_platforms);
    if (num_platforms == 0) return;
    std::vector<cl_platform_id> platforms(num_platforms);
    clGetPlatformIDs(num_platforms, platforms.data(), nullptr);
    cl_platform_id platform = platforms[0];
    
    cl_uint num_devices;
    clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 0, nullptr, &num_devices);
    if (num_devices == 0) return;
    std::vector<cl_device_id> devices(num_devices);
    clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, num_devices, devices.data(), nullptr);
    cl_device_id device = devices[0];
    
    cl_context_ = clCreateContext(nullptr, 1, &device, nullptr, nullptr, nullptr);
    cl_queue_ = clCreateCommandQueueWithProperties(cl_context_, device, nullptr, nullptr);
    
    pfn_clImportMemoryARM = (PFN_clImportMemoryARM)clGetExtensionFunctionAddressForPlatform(platform, "clImportMemoryARM");
    
    const char* src = ocl_kernel_src;
    cl_prog_ = clCreateProgramWithSource(cl_context_, 1, &src, nullptr, nullptr);
    clBuildProgram(cl_prog_, 1, &device, nullptr, nullptr, nullptr);
    cl_kern_blend_v_ = clCreateKernel(cl_prog_, "blend_seam", nullptr); // 均复用这一个
    
    int bw = blend_width_ > 0 ? blend_width_ : 120;
    std::vector<uint8_t> mask(bw);
    for(int i = 0; i < bw; ++i) {
        double x = (double)i / (bw - 1);
        double val = x;
        if (g_feather_strength > 1.0) { // S型曲线过缓
            val = (x < 0.5) ? 0.5 * std::pow(2.0 * x, g_feather_strength) 
                            : 1.0 - 0.5 * std::pow(2.0 * (1.0 - x), g_feather_strength);
        }
        mask[i] = (uint8_t)std::max(0.0, std::min(255.0, val * 255.0));
    }
    cl_alpha_mask_v_ = clCreateBuffer(cl_context_, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, bw, mask.data(), nullptr);
}

void ImageStitcher::CleanupOpenCL() {
    if (cl_alpha_mask_v_) clReleaseMemObject(cl_alpha_mask_v_);
    if (cl_kern_blend_v_) clReleaseKernel(cl_kern_blend_v_);
    if (cl_prog_) clReleaseProgram(cl_prog_);

    // Release all cached cl_mems
    for (auto& pair : dma_buf_cache_) {
        clReleaseMemObject(pair.second);
    }
    dma_buf_cache_.clear();

    if (cl_queue_) clReleaseCommandQueue(cl_queue_);
    if (cl_context_) clReleaseContext(cl_context_);
}

void ImageStitcher::BlendSeams(const std::vector<NV12Frame>& input, NV12Frame& output) {
    if (!cl_context_) InitOpenCL();
    if (!cl_context_ || !pfn_clImportMemoryARM) return;
    const std::vector<NV12Frame>& blend_input =
        (use_gles_warp_ && warped_frames_.size() == tasks_.size()) ? warped_frames_ : input;
    
    // lambda to fetch or create a mapped DMA_BUF object in the cache
    auto get_cl_mem = [&](const int* fd_ptr, int w, int h, cl_mem_flags flags) -> cl_mem {
        int fd = *fd_ptr;
        if (dma_buf_cache_.count(fd) > 0) return dma_buf_cache_[fd];
        
        cl_import_properties_arm props[] = { CL_IMPORT_TYPE_ARM, CL_IMPORT_TYPE_DMA_BUF_ARM, 0 };
        cl_int err;
        cl_mem mem = pfn_clImportMemoryARM(cl_context_, flags, props, (void*)fd_ptr, w * h * 3 / 2, &err);
        if (mem && err == CL_SUCCESS) {
            dma_buf_cache_[fd] = mem;
        }
        return mem;
    };
    
    std::vector<cl_mem> cl_in(4, nullptr);
    for(int i=0; i<4; ++i) {
        if (i >= static_cast<int>(blend_input.size())) continue;
        if(blend_input[i].empty() || !tasks_[i].enabled) continue;
        int w = blend_input[i].stride_w > 0 ? blend_input[i].stride_w : blend_input[i].width;
        int h = blend_input[i].stride_h > 0 ? blend_input[i].stride_h : blend_input[i].height;
        cl_in[i] = get_cl_mem(&blend_input[i].fd, w, h, CL_MEM_READ_ONLY);
    }
    int out_w = output.stride_w > 0 ? output.stride_w : output.width;
    int out_h = output.stride_h > 0 ? output.stride_h : output.height;
    cl_mem cl_out = get_cl_mem(&output.fd, out_w, out_h, CL_MEM_READ_WRITE);

    auto dispatch_seam = [&](int i1, int i2, int seam_x, int seam_y, int seam_w, int seam_h, int is_vert) {
        if(!cl_in[i1] || !cl_in[i2]) return;
        const auto& t1 = tasks_[i1]; const auto& t2 = tasks_[i2];
        const auto& f1 = blend_input[i1]; const auto& f2 = blend_input[i2];
        int s1w = t1.src_w, s1h = t1.src_h, str1 = f1.stride_w > 0 ? f1.stride_w : f1.width;
        int h_str1 = f1.stride_h > 0 ? f1.stride_h : f1.height;
        int s2w = t2.src_w, s2h = t2.src_h, str2 = f2.stride_w > 0 ? f2.stride_w : f2.width;
        int h_str2 = f2.stride_h > 0 ? f2.stride_h : f2.height;

        const int overlap_left = std::max(t1.dst_x, t2.dst_x);
        const int overlap_top = std::max(t1.dst_y, t2.dst_y);
        const int overlap_right =
            std::min(t1.dst_x + RotatedWidth(t1), t2.dst_x + RotatedWidth(t2));
        const int overlap_bottom =
            std::min(t1.dst_y + RotatedHeight(t1), t2.dst_y + RotatedHeight(t2));
        seam_x = std::max(seam_x, overlap_left);
        seam_y = std::max(seam_y, overlap_top);
        seam_w = std::min(seam_w, overlap_right - seam_x);
        seam_h = std::min(seam_h, overlap_bottom - seam_y);
        seam_w = NormalizeEvenFloor(seam_w);
        seam_h = NormalizeEvenFloor(seam_h);
        if (seam_w < 2 || seam_h < 2) {
            return;
        }
        
        int a=0;
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(cl_mem), &cl_in[i1]);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &s1w);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &s1h);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &str1);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &h_str1);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &t1.dst_x);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &t1.dst_y);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &t1.src_x);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &t1.src_y);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &t1.rotation_deg);
        
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(cl_mem), &cl_in[i2]);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &s2w);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &s2h);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &str2);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &h_str2);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &t2.dst_x);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &t2.dst_y);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &t2.src_x);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &t2.src_y);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &t2.rotation_deg);
        
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(cl_mem), &cl_alpha_mask_v_);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &is_vert);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(cl_mem), &cl_out);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &out_w);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &out_h);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &out_w);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &out_h);
        
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &seam_x);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &seam_y);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &seam_w);
        clSetKernelArg(cl_kern_blend_v_, a++, sizeof(int), &seam_h);
        
        size_t global_work_size[2] = { (size_t)seam_w / 2, (size_t)seam_h / 2 };
        clEnqueueNDRangeKernel(cl_queue_, cl_kern_blend_v_, 2, nullptr, global_work_size, nullptr, 0, nullptr, nullptr);
    };

    // 执行纵向 0+1 与 2+3
    int v_h1 = RotatedHeight(tasks_[0]);
    int v_h2 = RotatedHeight(tasks_[2]);
    dispatch_seam(0, 1, tasks_[1].dst_x, tasks_[0].dst_y, blend_width_, v_h1, 1);
    dispatch_seam(2, 3, tasks_[3].dst_x, tasks_[2].dst_y, blend_width_, v_h2, 1);
    
    // 执行横向 0+2 与 1+3 (会覆盖中心十字交叉区)
    int h_w1 = RotatedWidth(tasks_[0]);
    int h_w2 = RotatedWidth(tasks_[1]);
    dispatch_seam(0, 2, tasks_[0].dst_x, tasks_[2].dst_y, h_w1, blend_width_, 0);
    dispatch_seam(1, 3, tasks_[1].dst_x, tasks_[3].dst_y, h_w2, blend_width_, 0);

    clFinish(cl_queue_);
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
    warped_buffers_.resize(num_img_);
    warped_frames_.resize(num_img_);
}

bool ImageStitcher::SetWarpData(const StitchingWarpData& warp_data,
                                int input_width,
                                int input_height) {
    warp_data_ = warp_data;
    use_gles_warp_ = warp_data_.valid() &&
                     gles_warper_.Initialize(warp_data_.entries, input_width, input_height);
    if (!use_gles_warp_) {
        warp_data_ = StitchingWarpData{};
        gles_warper_.Shutdown();
        Logger::GetInstance().Log(
            "[ImageStitcher] GLES warper unavailable, falling back to RGA path.");
    } else {
        Logger::GetInstance().Log("[ImageStitcher] GLES warper initialized.");
    }
    return use_gles_warp_;
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
    for (DrmBuffer& buffer : warped_buffers_) {
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

    if (use_gles_warp_ &&
        img_idx < static_cast<int>(warp_data_.entries.size()) &&
        warp_data_.entries[img_idx].valid()) {
        const int warped_w = warp_data_.entries[img_idx].xmap.cols;
        const int warped_h = warp_data_.entries[img_idx].xmap.rows;
        const int dst_x = NormalizeEvenFloor(task.dst_x);
        const int dst_y = NormalizeEvenFloor(task.dst_y);
        if (dst_x + warped_w > output.width || dst_y + warped_h > output.height) {
            Logger::GetInstance().LogError(
                "[ImageStitcher] warped dst rect is out of panorama bounds.");
            return;
        }
        if (!EnsureScratchBuffer(&warped_buffers_, img_idx, warped_w, warped_h)) {
            Logger::GetInstance().LogError("[ImageStitcher] failed to allocate warped DMA-BUF.");
            return;
        }

        DrmBuffer& warped = warped_buffers_[img_idx];
        if (!gles_warper_.WarpFrame(img_idx, in, warped)) {
            Logger::GetInstance().LogError("[ImageStitcher] GLES warp failed.");
            return;
        }

        warped_frames_[img_idx].fd = warped.fd;
        warped_frames_[img_idx].width = warped.width;
        warped_frames_[img_idx].height = warped.height;
        warped_frames_[img_idx].stride_w = static_cast<int>(warped.pitch);
        warped_frames_[img_idx].stride_h = warped.height;

        if (!BlitByRect(warped.fd,
                        static_cast<int>(warped.pitch),
                        warped.height,
                        0,
                        0,
                        warped_w,
                        warped_h,
                        output.fd,
                        output.stride_w > 0 ? output.stride_w : output.width,
                        output.stride_h > 0 ? output.stride_h : output.height,
                        dst_x,
                        dst_y)) {
            Logger::GetInstance().LogError("[ImageStitcher] c_RkRgaBlit failed on warped copy path.");
        }
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
