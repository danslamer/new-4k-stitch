#ifndef IMAGE_STITCHER_H
#define IMAGE_STITCHER_H

#include <vector>
#include <mutex>
#include <unordered_map>
#include "drm_allocator.h"
#include "nv12_frame.h"
#include "rk_gles_warper.h"
#include "roi_config.h"

#define CL_TARGET_OPENCL_VERSION 200
#include <CL/cl.h>
#include <CL/cl_ext.h>

struct StitchTask {
    bool enabled = true;
    int rotation_deg = 0;
    int src_x = 0;
    int src_y = 0;
    int src_w = 0;
    int src_h = 0;
    int dst_x = 0;
    int dst_y = 0;
};

class ImageStitcher {
public:
    ImageStitcher();
    ~ImageStitcher();

    void SetParams(int blend_width,
                   int num_img,
                   int out_w,
                   int out_h);
    bool SetWarpData(const StitchingWarpData& warp_data,
                     int input_width,
                     int input_height);

void SetLayout(const std::vector<StitchTask>& tasks);
    const std::vector<StitchTask>& GetLayout() const { return tasks_; }
    void ClearOutput(const NV12Frame& output) const;

    void WarpImages(int img_idx,
                    size_t frame_idx,
                    const std::vector<NV12Frame>& input,
                    NV12Frame& output);

    void BlendSeams(const std::vector<NV12Frame>& input, NV12Frame& output);

private:
    bool EnsureScratchBuffer(std::vector<DrmBuffer>* buffers,
                             int img_idx,
                             int width,
                             int height);
    void ReleaseScratchBuffers();
    int RotatedWidth(const StitchTask& task) const;
    int RotatedHeight(const StitchTask& task) const;

    int num_img_;
    int blend_width_;
    int out_w_, out_h_;

    std::vector<StitchTask> tasks_;
    StitchingWarpData warp_data_;
    std::vector<DrmBuffer> crop_buffers_;
    std::vector<DrmBuffer> warped_buffers_;
    std::vector<NV12Frame> warped_frames_;
    RkGlesWarper gles_warper_;
    bool use_gles_warp_ = false;

    // OpenCL members
    cl_context cl_context_ = nullptr;
    cl_command_queue cl_queue_ = nullptr;
    cl_program cl_prog_ = nullptr;
    cl_kernel cl_kern_blend_v_ = nullptr;
    cl_kernel cl_kern_blend_h_ = nullptr;
    cl_mem cl_alpha_mask_v_ = nullptr;
    cl_mem cl_alpha_mask_h_ = nullptr;

    // DMA-BUF to cl_mem mapping cache to avoid MMU mapping overhead every frame
    std::unordered_map<int, cl_mem> dma_buf_cache_;
    
    void InitOpenCL();
    void CleanupOpenCL();
    std::vector<DrmBuffer> rotate_buffers_;
    std::vector<std::mutex> warp_mutex_;
};

#endif
