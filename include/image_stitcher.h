#ifndef IMAGE_STITCHER_H
#define IMAGE_STITCHER_H

#include <vector>
#include <mutex>
#include "drm_allocator.h"
#include "nv12_frame.h"

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

    void SetLayout(const std::vector<StitchTask>& tasks);
    void ClearOutput(const NV12Frame& output) const;

    void WarpImages(int img_idx,
                    size_t frame_idx,
                    const std::vector<NV12Frame>& input,
                    NV12Frame& output);

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
    std::vector<DrmBuffer> crop_buffers_;
    std::vector<DrmBuffer> rotate_buffers_;
    std::vector<std::mutex> warp_mutex_;
};

#endif
