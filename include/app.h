#ifndef IMAGE_STITCHING_APP_H
#define IMAGE_STITCHING_APP_H

#include "sensor_data_interface.h"
#include "image_stitcher.h"
#include "logger.h"
#include "nv12_frame.h"
#include "drm_allocator.h"
#include "roi_config.h"
#include "roi_visualizer.h"

#include <vector>

using namespace std;

static int g_multi_frame_roi_debug_level = 1;
static bool g_save_roi_confidence_debug = true;

extern StitchGlobalConfig g_config;

struct CachedOverlap {
    int h01_overlap = 0;
    int h01_shift_y = 0;
    double h01_score = 0.0;
    int h23_overlap = 0;
    int h23_shift_y = 0;
    double h23_score = 0.0;
    int h45_overlap = 0;
    int h45_shift_y = 0;
    double h45_score = 0.0;
    int v02_overlap = 0;
    int v02_shift_y = 0;
    double v02_score = 0.0;
    int v13_overlap = 0;
    int v13_shift_y = 0;
    double v13_score = 0.0;
    int v24_overlap = 0;
    int v24_shift_y = 0;
    double v24_score = 0.0;
    int v35_overlap = 0;
    int v35_shift_y = 0;
    double v35_score = 0.0;
    double confidence = 0.0;
};

class App {
 public:
    App();
    ~App();

    [[noreturn]] void run_stitching();

 private:
    void BootStrapOptimalLayout();
    void InitFromConfig();
    void RebuildLayout();
    void SyncConfigToGlobals();
    void SaveCurrentFrames();
    void ReleaseSavedFrames();
    void RestitchSavedFrames();

    size_t num_img_;

    SensorDataInterface sensorDataInterface_;
    ImageStitcher image_stitcher_;

    vector<NV12Frame> image_vector_;

    NV12Frame image_concat_;
    DrmBuffer output_drm_buf_;

    int total_cols_;
    int height_;

    CachedOverlap cached_overlaps_;
    bool visual_mode_;
    bool debug_mode_;

    vector<NV12Frame> saved_frames_;
    vector<DrmBuffer> saved_drm_bufs_;
    bool frames_locked_;
    size_t locked_frame_idx_;

    // 阶段 2: MJPEG panorama 推流 (CameraPage "实时预览" 用).
    // panorama NV12 -> RGA downscale -> small NV12 -> cvtColor -> cv::imencode -> mjpeg_streamer::update().
    // gate: 每 mjpeg_interval_ 帧做一次 (默认 2 = 15 FPS, 避免吃掉 stitch loop 30 FPS 预算).
    DrmBuffer     mjpeg_drm_buf_;            // RGA 降采样目标: 960x816 NV12 DMA-BUF
    int           mjpeg_width_   = 960;      // 降采样后宽
    int           mjpeg_height_  = 816;      // 降采样后高
    int           mjpeg_interval_ = 2;       // 每 N 帧编码一次
    int           mjpeg_quality_ = 75;       // cv::imencode jpeg quality
};

#endif