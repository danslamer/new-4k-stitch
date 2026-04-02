#ifndef IMAGE_STITCHER_NV12_H
#define IMAGE_STITCHER_NV12_H

#include <opencv2/opencv.hpp>
#include <vector>
#include <mutex>

struct NV12Frame {
    cv::UMat y;   // H x W, CV_8UC1
    cv::UMat uv;  // H/2 x W/2, CV_8UC2

    bool empty() const {
        return y.empty() || uv.empty();
    }
};

class ImageStitcherNV12 {
public:
    void SetParams(
        const int& blend_width,
        std::vector<cv::UMat>& undist_xmap_vector,
        std::vector<cv::UMat>& undist_ymap_vector,
        std::vector<cv::UMat>& reproj_xmap_vector,
        std::vector<cv::UMat>& reproj_ymap_vector,
        std::vector<cv::Rect>& projected_image_roi_vect_refined);

    void WarpImages(
        const int& img_idx,
        const NV12Frame& input,
        NV12Frame& output,
        std::vector<std::mutex>& image_mutex_vector);

private:
    void CreateWeightMap(const int& height, const int& width);

private:
    int num_img_;

    std::vector<cv::UMat> final_xmap_vector_;
    std::vector<cv::UMat> final_ymap_vector_;

    // UV map（关键）
    std::vector<cv::UMat> final_xmap_uv_vector_;
    std::vector<cv::UMat> final_ymap_uv_vector_;

    std::vector<cv::Rect> roi_vect_;

    std::vector<cv::UMat> tmp_y_;
    std::vector<cv::UMat> tmp_uv_;

    std::vector<cv::UMat> weightMap_;     // Y
    std::vector<cv::UMat> weightMap_uv_;  // UV

    std::vector<std::mutex> warp_mutex_vector_;
};

#endif
