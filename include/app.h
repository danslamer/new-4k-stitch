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
    int v02_overlap = 0;
    int v02_shift_y = 0;
    double v02_score = 0.0;
    int v13_overlap = 0;
    int v13_shift_y = 0;
    double v13_score = 0.0;
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
};

#endif