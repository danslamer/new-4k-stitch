//
// Created by s1nh.org.
//

#include "app.h"

#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "logger.h"
#include "stitching_param_generater.h"

#pragma clang diagnostic push
#pragma ide diagnostic ignored "UnreachableCode"

using namespace std;

bool g_save_each_frame = false;

namespace {

int AlignToEven(int value) {
  return (value % 2 == 0) ? value : (value + 1);
}

cv::Mat BuildPreviewBgrFromNV12Y(const NV12Frame& frame) {
  cv::Mat y_mat;
  frame.y.copyTo(y_mat);

  cv::Mat preview_bgr;
  cv::cvtColor(y_mat, preview_bgr, cv::COLOR_GRAY2BGR);
  return preview_bgr;
}

cv::UMat BuildPreviewBgrFromNV12(const NV12Frame& frame) {
  cv::UMat preview_bgr;
  cv::cvtColorTwoPlane(frame.y, frame.uv, preview_bgr, cv::COLOR_YUV2BGR_NV12);
  return preview_bgr;
}

}  // namespace

App::App() {
  Logger::GetInstance().Initialize();
  Logger::GetInstance().Log("[App] Application starting...");

  sensorDataInterface_.InitVideoCapture(num_img_);

  vector<NV12Frame> first_image_vector(num_img_);
  vector<cv::Mat> first_mat_vector(num_img_);
  vector<cv::UMat> reproj_xmap_vector;
  vector<cv::UMat> reproj_ymap_vector;
  vector<cv::UMat> undist_xmap_vector;
  vector<cv::UMat> undist_ymap_vector;
  vector<cv::Rect> image_roi_vect;

  vector<mutex> image_mutex_vector(num_img_);
  sensorDataInterface_.get_nv12_frame_vector(first_image_vector, image_mutex_vector);

  for (size_t i = 0; i < num_img_; ++i) {
    if (first_image_vector[i].empty()) {
      Logger::GetInstance().LogError(
          "[App] failed to fetch initial NV12 frame from channel " + to_string(i));
      continue;
    }
    first_mat_vector[i] = BuildPreviewBgrFromNV12Y(first_image_vector[i]);
  }

  StitchingParamGenerator stitching_param_generator(first_mat_vector);
  stitching_param_generator.GetReprojParams(undist_xmap_vector,
                                            undist_ymap_vector,
                                            reproj_xmap_vector,
                                            reproj_ymap_vector,
                                            image_roi_vect);

  image_stitcher_.SetParams(100,
                            undist_xmap_vector,
                            undist_ymap_vector,
                            reproj_xmap_vector,
                            reproj_ymap_vector,
                            image_roi_vect);
  total_cols_ = 0;
  for (size_t i = 0; i < num_img_; ++i) {
    total_cols_ += image_roi_vect[i].width;
  }

  const int stitched_width = total_cols_;
  const int stitched_height = image_roi_vect[0].height;
  const int output_height = AlignToEven(stitched_height);
  total_cols_ = AlignToEven(total_cols_);
  image_concat_nv12_.y = cv::UMat(output_height, total_cols_, CV_8UC1);
  image_concat_nv12_.uv = cv::UMat(output_height / 2, total_cols_ / 2, CV_8UC2);

  std::ostringstream init_stream;
  init_stream << "[App] pipeline=FFmpeg(rkmpp)->NV12->NV12 Stitch(GPU)->output"
              << " stitched_size=" << stitched_width
              << "x" << stitched_height
              << " aligned_output_size=" << total_cols_ << "x" << output_height;
  Logger::GetInstance().Log(init_stream.str());
}

[[noreturn]] void App::run_stitching() {
  vector<NV12Frame> image_vector(num_img_);
  vector<mutex> image_mutex_vector(num_img_);
  double t0;
  double t1;
  double t2;
  double t3;

  size_t frame_idx = 0;
  while (true) {
    t0 = cv::getTickCount();

    sensorDataInterface_.get_nv12_frame_vector(image_vector, image_mutex_vector);
    t1 = cv::getTickCount();

    image_concat_nv12_.y.setTo(cv::Scalar::all(0));
    image_concat_nv12_.uv.setTo(cv::Scalar::all(128));

    vector<thread> warp_thread_vect;
    for (size_t img_idx = 0; img_idx < num_img_; ++img_idx) {
      if (image_vector[img_idx].empty()) {
        Logger::GetInstance().LogFrame(
            frame_idx,
            "[App] input channel " + to_string(img_idx) + " returned empty NV12 frame.");
        continue;
      }
      warp_thread_vect.emplace_back(&ImageStitcherNV12::WarpImages,
                                    &image_stitcher_,
                                    static_cast<int>(img_idx),
                                    cref(image_vector[img_idx]),
                                    ref(image_concat_nv12_),
                                    ref(image_mutex_vector));
    }
    for (auto& warp_thread : warp_thread_vect) {
      warp_thread.join();
    }
    t2 = cv::getTickCount();

    if (g_save_each_frame) {
      cv::UMat preview_bgr = BuildPreviewBgrFromNV12(image_concat_nv12_);
      std::string filename = "image_concat_nv12_" + to_string(frame_idx) + ".png";
      Logger::GetInstance().SaveImage(preview_bgr, filename, frame_idx);
    }
    t3 = cv::getTickCount();

    std::ostringstream timing_stream;
    timing_stream << "[app_timing]"
                  << " fetch_ms=" << (t1 - t0) * 1000.0 / cv::getTickFrequency()
                  << " stitch_ms=" << (t2 - t1) * 1000.0 / cv::getTickFrequency()
                  << " output_ms=" << (t3 - t2) * 1000.0 / cv::getTickFrequency();

    std::ostringstream fps_stream;
    fps_stream << "[app_fps]"
               << " stitch_fps="
               << (1.0 / ((t2 - t0) / cv::getTickFrequency()))
               << " real_fps="
               << (1.0 / ((t3 - t0) / cv::getTickFrequency()));

    vector<double> decode_fps_vector = sensorDataInterface_.GetDecodeFpsSnapshot();
    std::ostringstream decode_fps_stream;
    decode_fps_stream << "[decode_fps]";
    for (size_t i = 0; i < decode_fps_vector.size(); ++i) {
      decode_fps_stream << " ch" << i << "=" << decode_fps_vector[i];
    }

    Logger::GetInstance().LogFrame(frame_idx, timing_stream.str());
    Logger::GetInstance().LogFrame(frame_idx, fps_stream.str());
    Logger::GetInstance().LogFrame(frame_idx, decode_fps_stream.str());
    frame_idx++;
  }
}

int main() {
  App app;
  app.run_stitching();
}

#pragma clang diagnostic pop
