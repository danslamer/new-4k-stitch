#ifndef IMAGE_STITCHING_APP_H
#define IMAGE_STITCHING_APP_H

#include "sensor_data_interface.h"
#include "image_stitcher.h"
#include "logger.h"
#include "nv12_frame.h"
#include "drm_allocator.h"

#include <vector>

using namespace std;

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
