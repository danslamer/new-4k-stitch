//
// Created by s1nh.org on 2020/12/2.
//

#ifndef IMAGE_STITCHING_APP_H
#define IMAGE_STITCHING_APP_H

#include "opencv2/opencv.hpp"

#include "sensor_data_interface.h"
#include "image_stitcher_nv12.h"
#include "logger.h"

using namespace std;

class App {
 public:
    App();

    [[noreturn]] void run_stitching();

 private:
    std::size_t num_img_;
    SensorDataInterface sensorDataInterface_;
    ImageStitcherNV12 image_stitcher_;
    NV12Frame image_concat_nv12_;
    int total_cols_;
};

#endif //IMAGE_STITCHING_APP_H
