#include "image_stitcher_nv12.h"
#include <iostream>

using namespace std;
using namespace cv;

void ImageStitcherNV12::SetParams(
    const int& blend_width,
    vector<UMat>& undist_xmap_vector,
    vector<UMat>& undist_ymap_vector,
    vector<UMat>& reproj_xmap_vector,
    vector<UMat>& reproj_ymap_vector,
    vector<Rect>& projected_image_roi_vect_refined) {

    num_img_ = undist_xmap_vector.size();
    warp_mutex_vector_ = vector<mutex>(num_img_);

    roi_vect_ = projected_image_roi_vect_refined;

    final_xmap_vector_ = vector<UMat>(num_img_);
    final_ymap_vector_ = vector<UMat>(num_img_);
    final_xmap_uv_vector_ = vector<UMat>(num_img_);
    final_ymap_uv_vector_ = vector<UMat>(num_img_);

    tmp_y_ = vector<UMat>(num_img_);
    tmp_uv_ = vector<UMat>(num_img_);

    for (size_t i = 0; i < num_img_; ++i) {
        remap(undist_xmap_vector[i], final_xmap_vector_[i],
              reproj_xmap_vector[i], reproj_ymap_vector[i], INTER_LINEAR);

        remap(undist_ymap_vector[i], final_ymap_vector_[i],
              reproj_xmap_vector[i], reproj_ymap_vector[i], INTER_LINEAR);

        // ✅ UV map = 1/2 scale
        resize(final_xmap_vector_[i], final_xmap_uv_vector_[i],
               Size(), 0.5, 0.5, INTER_LINEAR);

        resize(final_ymap_vector_[i], final_ymap_uv_vector_[i],
               Size(), 0.5, 0.5, INTER_LINEAR);
    }

    int min_h = roi_vect_[0].height;
    for (int i = 1; i < num_img_; i++)
        min_h = min(min_h, roi_vect_[i].height);

    CreateWeightMap(min_h, blend_width);
}

void ImageStitcherNV12::CreateWeightMap(const int& height, const int& width) {

    Mat l(height, width, CV_8UC1);
    Mat r(height, width, CV_8UC1);

    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            l.at<uchar>(i, j) = saturate_cast<uchar>((float)j / width * 255);
            r.at<uchar>(i, j) = saturate_cast<uchar>((float)(width - j) / width * 255);
        }
    }

    weightMap_.push_back(l.getUMat(ACCESS_READ));
    weightMap_.push_back(r.getUMat(ACCESS_READ));

    // UV weight
    UMat l_uv, r_uv;
    resize(weightMap_[0], l_uv, Size(), 0.5, 0.5);
    resize(weightMap_[1], r_uv, Size(), 0.5, 0.5);

    weightMap_uv_.push_back(l_uv);
    weightMap_uv_.push_back(r_uv);
}

void ImageStitcherNV12::WarpImages(
    const int& img_idx,
    const NV12Frame& input,
    NV12Frame& output,
    vector<mutex>& image_mutex_vector) {

    image_mutex_vector[img_idx].lock();

    // ===== remap =====
    remap(input.y, tmp_y_[img_idx],
          final_xmap_vector_[img_idx],
          final_ymap_vector_[img_idx], INTER_LINEAR);

    remap(input.uv, tmp_uv_[img_idx],
          final_xmap_uv_vector_[img_idx],
          final_ymap_uv_vector_[img_idx], INTER_LINEAR);

    image_mutex_vector[img_idx].unlock();

    // ===== blend =====
    if (img_idx > 0) {
        Rect roi = roi_vect_[img_idx];
        Rect roi_prev = roi_vect_[img_idx - 1];

        // Y
        UMat r_y = tmp_y_[img_idx](Rect(
            roi.x, roi.y,
            weightMap_[0].cols, weightMap_[0].rows));

        warp_mutex_vector_[img_idx - 1].lock();
        UMat l_y = tmp_y_[img_idx - 1](Rect(
            roi_prev.x + roi_prev.width,
            roi_prev.y,
            weightMap_[0].cols,
            weightMap_[0].rows));
        warp_mutex_vector_[img_idx - 1].unlock();

        multiply(r_y, weightMap_[0], r_y, 1.0 / 255);
        multiply(l_y, weightMap_[1], l_y, 1.0 / 255);
        add(r_y, l_y, r_y);

        // UV（注意 /2）
        Rect roi_uv(
            roi.x / 2, roi.y / 2,
            weightMap_uv_[0].cols,
            weightMap_uv_[0].rows);

        Rect roi_prev_uv(
            (roi_prev.x + roi_prev.width) / 2,
            roi_prev.y / 2,
            weightMap_uv_[0].cols,
            weightMap_uv_[0].rows);

        UMat r_uv = tmp_uv_[img_idx](roi_uv);

        warp_mutex_vector_[img_idx - 1].lock();
        UMat l_uv = tmp_uv_[img_idx - 1](roi_prev_uv);
        warp_mutex_vector_[img_idx - 1].unlock();

        multiply(r_uv, weightMap_uv_[0], r_uv, 1.0 / 255);
        multiply(l_uv, weightMap_uv_[1], l_uv, 1.0 / 255);
        add(r_uv, l_uv, r_uv);
    }

    // ===== concat =====
    int cols = 0;
    for (int i = 0; i < img_idx; i++)
        cols += roi_vect_[i].width;

    Rect roi = roi_vect_[img_idx];

    // Y
    tmp_y_[img_idx](roi).copyTo(
        output.y(Rect(cols, 0, roi.width, roi.height)));

    // UV
    Rect roi_uv(roi.x / 2, roi.y / 2, roi.width / 2, roi.height / 2);

    tmp_uv_[img_idx](roi_uv).copyTo(
        output.uv(Rect(cols / 2, 0, roi.width / 2, roi.height / 2)));
}