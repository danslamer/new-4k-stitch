#ifndef IMAGE_STITCHING_APP_H
#define IMAGE_STITCHING_APP_H

#include "sensor_data_interface.h"
#include "image_stitcher.h"
#include "logger.h"
#include "nv12_frame.h"
#include "drm_allocator.h"

#include <vector>

using namespace std;

// ============================================================================
// 全局调试开关 - 用于多帧ROI检测优化
// ============================================================================
/// 是否启用多帧ROI检测调试日志输出
/// 0: OFF (关闭), 1: INFO (基本信息), 2: DEBUG (详细调试)
static int g_multi_frame_roi_debug_level = 1;

/// 是否保存每帧的ROI检测结果和置信度分析
static bool g_save_roi_confidence_debug = true;

class App {
 public:
    App();
    ~App();

    [[noreturn]] void run_stitching();

 private:
    size_t num_img_;

    SensorDataInterface sensorDataInterface_;
    ImageStitcher image_stitcher_;

    // ❗ 改成 NV12Frame
    vector<NV12Frame> image_vector_;

    NV12Frame image_concat_;   // 输出DMA buffer
    DrmBuffer output_drm_buf_;

    int total_cols_;
    int height_;
};

#endif
