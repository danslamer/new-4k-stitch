#include "app.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <sys/mman.h>

#include <opencv2/opencv.hpp>
#include <opencv2/features2d.hpp>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

namespace {

struct CameraTuning {
  int rotation_deg = 0;
  int crop_left = 0;
  int crop_right = 0;
  int crop_top = 0;
  int crop_bottom = 0;
  int offset_x = 0;
  int offset_y = 0;
  bool enabled = true;
};

struct OverlapEstimate {
  int overlap = 0;
  int shift_y = 0;
  double score = 0.0;
};

struct CameraRoi {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

int NormalizeEvenFloor(int value) {
  return std::max(0, value & ~1);
}

int NormalizeEvenCeil(int value) {
  return std::max(2, (value + 1) & ~1);
}

int RotatedWidth(int width, int height, int rotation_deg) {
  return (rotation_deg == 90 || rotation_deg == 270) ? height : width;
}

int RotatedHeight(int width, int height, int rotation_deg) {
  return (rotation_deg == 90 || rotation_deg == 270) ? width : height;
}

int MedianInt(std::vector<int> values) {
  if (values.empty()) {
    return 0;
  }
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

double MedianDouble(std::vector<double> values) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

cv::Mat ExportHardwareFrameToBgr(const NV12Frame& frame) {
  if (frame.empty() || !frame.owner) {
    return cv::Mat();
  }

  AVFrame* sw_frame = av_frame_alloc();
  if (sw_frame == nullptr) {
    return cv::Mat();
  }

  const int ret = av_hwframe_transfer_data(sw_frame, frame.owner.get(), 0);
  if (ret < 0) {
    av_frame_free(&sw_frame);
    return cv::Mat();
  }

  if (sw_frame->width <= 0 || sw_frame->height <= 0 || sw_frame->data[0] == nullptr) {
    av_frame_free(&sw_frame);
    return cv::Mat();
  }

  cv::Mat nv12(sw_frame->height * 3 / 2, sw_frame->width, CV_8UC1);
  for (int row = 0; row < sw_frame->height; ++row) {
    std::memcpy(nv12.ptr(row),
                sw_frame->data[0] + static_cast<size_t>(row) * sw_frame->linesize[0],
                static_cast<size_t>(sw_frame->width));
  }
  for (int row = 0; row < sw_frame->height / 2; ++row) {
    std::memcpy(nv12.ptr(sw_frame->height + row),
                sw_frame->data[1] + static_cast<size_t>(row) * sw_frame->linesize[1],
                static_cast<size_t>(sw_frame->width));
  }

  cv::Mat bgr;
  cv::cvtColor(nv12, bgr, cv::COLOR_YUV2BGR_NV12);
  av_frame_free(&sw_frame);
  return bgr;
}

cv::UMat ExportNv12DrmBufferToBgr(const DrmBuffer& buffer) {
  if (buffer.fd < 0 || buffer.width <= 0 || buffer.height <= 0 || buffer.pitch == 0) {
    return cv::UMat();
  }

  void* mapped = drm_map(buffer);
  if (mapped == MAP_FAILED) {
    return cv::UMat();
  }

  const int y_rows = buffer.height * 3 / 2;
  cv::Mat nv12_host(y_rows, buffer.width, CV_8UC1);
  const uint8_t* src = static_cast<const uint8_t*>(mapped);

  for (int row = 0; row < buffer.height; ++row) {
    std::memcpy(nv12_host.ptr(row),
                src + static_cast<size_t>(row) * buffer.pitch,
                static_cast<size_t>(buffer.width));
  }

  const uint8_t* uv_src = src + static_cast<size_t>(buffer.pitch) * buffer.height;
  for (int row = 0; row < buffer.height / 2; ++row) {
    std::memcpy(nv12_host.ptr(buffer.height + row),
                uv_src + static_cast<size_t>(row) * buffer.pitch,
                static_cast<size_t>(buffer.width));
  }

  drm_unmap(buffer, mapped);

  if (nv12_host.empty()) {
    return cv::UMat();
  }

  cv::Mat bgr_host;
  cv::cvtColor(nv12_host, bgr_host, cv::COLOR_YUV2BGR_NV12);
  return bgr_host.getUMat(cv::ACCESS_READ);
}

std::vector<CameraTuning> BuildDefaultTuning(size_t num_cameras) {
  std::vector<CameraTuning> tuning(num_cameras);
  // Industrial deployment should replace these defaults with fixed installation
  // parameters measured on the target rig.
  return tuning;
}

OverlapEstimate EstimatePairOverlap(const cv::Mat& left_bgr, const cv::Mat& right_bgr) {
  auto estimate_by_template = [&]() {
  OverlapEstimate estimate;
  if (left_bgr.empty() || right_bgr.empty()) {
    return estimate;
  }

  cv::Mat left_gray;
  cv::Mat right_gray;
  cv::cvtColor(left_bgr, left_gray, cv::COLOR_BGR2GRAY);
  cv::cvtColor(right_bgr, right_gray, cv::COLOR_BGR2GRAY);

  const int common_w = std::min(left_gray.cols, right_gray.cols);
  const int common_h = std::min(left_gray.rows, right_gray.rows);
  const int search_w = NormalizeEvenFloor(std::min(common_w / 2, 1200));
  const int template_w = NormalizeEvenFloor(std::max(192, search_w * 2 / 3));
  const int band_h = NormalizeEvenFloor(std::min(common_h / 5, 480));
  const int max_shift_y = std::min(240, std::max(16, common_h / 12));
  if (search_w <= template_w || band_h <= 0) {
    estimate.overlap = NormalizeEvenFloor(common_w / 8);
    return estimate;
  }

  std::vector<int> overlap_candidates;
  std::vector<int> shift_candidates;
  std::vector<double> score_candidates;
  const int left_x = left_gray.cols - search_w;

  for (int band = 0; band < 5; ++band) {
    const int center_y = NormalizeEvenFloor((band + 1) * common_h / 6);
    int left_y = NormalizeEvenFloor(center_y - band_h / 2);
    left_y = std::max(0, std::min(left_y, left_gray.rows - band_h));
    const int right_y = std::max(0, left_y - max_shift_y);
    const int right_h = std::min(right_gray.rows - right_y, band_h + max_shift_y * 2);
    if (right_h < band_h) {
      continue;
    }

    const cv::Rect left_strip_roi(left_x, left_y, search_w, band_h);
    const cv::Rect templ_roi(search_w - template_w, 0, template_w, band_h);
    const cv::Rect right_search_roi(0, right_y, search_w, right_h);

    cv::Mat left_strip = left_gray(left_strip_roi);
    cv::Mat templ = left_strip(templ_roi);
    cv::Mat right_search = right_gray(right_search_roi);

    cv::Mat result;
    cv::matchTemplate(right_search, templ, result, cv::TM_CCOEFF_NORMED);
    double max_val = 0.0;
    cv::Point max_loc;
    cv::minMaxLoc(result, nullptr, &max_val, nullptr, &max_loc);
    if (max_val < 0.05) {
      continue;
    }

    overlap_candidates.push_back(NormalizeEvenFloor(search_w - max_loc.x));
    shift_candidates.push_back((right_y + max_loc.y) - left_y);
    score_candidates.push_back(max_val);
  }

  if (overlap_candidates.empty()) {
    estimate.overlap = NormalizeEvenFloor(common_w / 8);
    estimate.shift_y = 0;
    estimate.score = 0.0;
    return estimate;
  }

  estimate.overlap = std::max(32, MedianInt(overlap_candidates));
  estimate.shift_y = MedianInt(shift_candidates);
  estimate.score = MedianDouble(score_candidates);
  return estimate;
  };

  if (left_bgr.empty() || right_bgr.empty()) {
    return OverlapEstimate{};
  }

  cv::Mat left_gray;
  cv::Mat right_gray;
  cv::cvtColor(left_bgr, left_gray, cv::COLOR_BGR2GRAY);
  cv::cvtColor(right_bgr, right_gray, cv::COLOR_BGR2GRAY);

  const int common_w = std::min(left_gray.cols, right_gray.cols);
  const int common_h = std::min(left_gray.rows, right_gray.rows);
  const int band_w = NormalizeEvenFloor(std::min(common_w / 2, 1200));
  if (band_w < 256 || common_h < 128) {
    return estimate_by_template();
  }

  const cv::Rect left_roi(left_gray.cols - band_w, 0, band_w, left_gray.rows);
  const cv::Rect right_roi(0, 0, band_w, right_gray.rows);
  cv::Mat left_band = left_gray(left_roi);
  cv::Mat right_band = right_gray(right_roi);

  cv::Ptr<cv::ORB> orb = cv::ORB::create(4000);
  std::vector<cv::KeyPoint> kp_left;
  std::vector<cv::KeyPoint> kp_right;
  cv::Mat desc_left;
  cv::Mat desc_right;
  orb->detectAndCompute(left_band, cv::noArray(), kp_left, desc_left);
  orb->detectAndCompute(right_band, cv::noArray(), kp_right, desc_right);

  if (desc_left.empty() || desc_right.empty()) {
    return estimate_by_template();
  }

  cv::BFMatcher matcher(cv::NORM_HAMMING);
  std::vector<std::vector<cv::DMatch>> knn_matches;
  matcher.knnMatch(desc_right, desc_left, knn_matches, 2);

  std::vector<cv::Point2f> pts_right;
  std::vector<cv::Point2f> pts_left;
  for (size_t i = 0; i < knn_matches.size(); ++i) {
    if (knn_matches[i].size() < 2) {
      continue;
    }
    const cv::DMatch& best = knn_matches[i][0];
    const cv::DMatch& second = knn_matches[i][1];
    if (best.distance >= second.distance * 0.75f) {
      continue;
    }

    cv::Point2f p_right = kp_right[best.queryIdx].pt;
    cv::Point2f p_left = kp_left[best.trainIdx].pt;
    p_right.x += static_cast<float>(right_roi.x);
    p_left.x += static_cast<float>(left_roi.x);
    pts_right.push_back(p_right);
    pts_left.push_back(p_left);
  }

  if (pts_left.size() < 12) {
    return estimate_by_template();
  }

  cv::Mat inliers;
  cv::Mat affine = cv::estimateAffinePartial2D(
      pts_right, pts_left, inliers, cv::RANSAC, 3.0, 2000, 0.99, 15);
  if (affine.empty()) {
    return estimate_by_template();
  }

  const double dx = affine.at<double>(0, 2);
  const double dy = affine.at<double>(1, 2);
  const int overlap = NormalizeEvenFloor(left_gray.cols - static_cast<int>(std::round(dx)));
  int inlier_count = 0;
  for (int i = 0; i < inliers.rows; ++i) {
    if (inliers.at<uchar>(i, 0) != 0) {
      ++inlier_count;
    }
  }

  OverlapEstimate estimate;
  estimate.overlap = std::max(32, std::min(left_gray.cols, overlap));
  estimate.shift_y = static_cast<int>(std::round(-dy));
  estimate.score = pts_left.empty() ? 0.0 : static_cast<double>(inlier_count) / pts_left.size();

  if (estimate.score < 0.25 || estimate.overlap >= left_gray.cols || estimate.overlap <= 32) {
    return estimate_by_template();
  }
  return estimate;
}

void SaveDetectedRoiDebug(const std::vector<cv::Mat>& bootstrap_bgr,
                          const std::vector<CameraRoi>& rois) {
  for (size_t i = 0; i < bootstrap_bgr.size() && i < rois.size(); ++i) {
    if (bootstrap_bgr[i].empty()) {
      continue;
    }

    cv::Mat debug = bootstrap_bgr[i].clone();
    const cv::Rect roi(rois[i].x, rois[i].y, rois[i].width, rois[i].height);
    cv::rectangle(debug, roi, cv::Scalar(0, 255, 0), 6);
    cv::putText(debug,
                "ROI",
                cv::Point(roi.x + 20, roi.y + 60),
                cv::FONT_HERSHEY_SIMPLEX,
                2.0,
                cv::Scalar(0, 255, 0),
                3);

    std::ostringstream debug_name;
    debug_name << "roi_debug_cam" << i << ".png";
    Logger::GetInstance().SaveImage(debug.getUMat(cv::ACCESS_READ), debug_name.str());

    cv::Mat cropped = bootstrap_bgr[i](roi).clone();
    std::ostringstream crop_name;
    crop_name << "roi_crop_cam" << i << ".png";
    Logger::GetInstance().SaveImage(cropped.getUMat(cv::ACCESS_READ), crop_name.str());
  }
}

std::vector<OverlapEstimate> EstimateOverlaps(const std::vector<cv::Mat>& frames) {
  std::vector<OverlapEstimate> overlaps;
  if (frames.size() < 2) {
    return overlaps;
  }

  overlaps.reserve(frames.size() - 1);
  for (size_t i = 0; i + 1 < frames.size(); ++i) {
    overlaps.push_back(EstimatePairOverlap(frames[i], frames[i + 1]));
  }
  return overlaps;
}

std::vector<CameraRoi> BuildCameraRois(const std::vector<NV12Frame>& frames,
                                      const std::vector<OverlapEstimate>& overlaps,
                                      const std::vector<CameraTuning>& tuning) {
  const size_t num_img = frames.size();
  std::vector<CameraRoi> rois(num_img);
  std::vector<int> left_crop(num_img, 0);
  std::vector<int> right_crop(num_img, 0);
  std::vector<int> global_y(num_img, 0);

  for (size_t i = 0; i < overlaps.size(); ++i) {
    const int overlap = overlaps[i].overlap;
    const int left_cut = NormalizeEvenFloor(overlap / 2);
    const int right_cut = NormalizeEvenFloor(overlap - left_cut);
    right_crop[i] += left_cut;
    left_crop[i + 1] += right_cut;
    global_y[i + 1] = global_y[i] - overlaps[i].shift_y;
  }

  int global_top = std::numeric_limits<int>::min();
  int global_bottom = std::numeric_limits<int>::max();
  for (size_t i = 0; i < num_img; ++i) {
    const NV12Frame& frame = frames[i];
    const CameraTuning& cfg = tuning[i];
    const int top = global_y[i] + cfg.crop_top + cfg.offset_y;
    const int bottom = global_y[i] + frame.height - cfg.crop_bottom + cfg.offset_y;
    global_top = std::max(global_top, top);
    global_bottom = std::min(global_bottom, bottom);
  }

  const int common_height = NormalizeEvenFloor(global_bottom - global_top);
  if (common_height < 2) {
    throw std::runtime_error("failed to build common ROI height from overlap/shift estimation");
  }

  for (size_t i = 0; i < num_img; ++i) {
    const NV12Frame& frame = frames[i];
    const CameraTuning& cfg = tuning[i];
    if (frame.empty()) {
      throw std::runtime_error("bootstrap hardware frame is empty for camera " + std::to_string(i));
    }

    CameraRoi roi;
    roi.x = NormalizeEvenFloor(left_crop[i] + cfg.crop_left);
    roi.y = NormalizeEvenFloor(global_top - global_y[i] - cfg.offset_y);
    roi.width = NormalizeEvenFloor(
        frame.width - left_crop[i] - right_crop[i] - cfg.crop_left - cfg.crop_right);
    roi.height = common_height;

    if (roi.x < 0) {
      roi.x = 0;
    }
    if (roi.y < 0) {
      roi.y = 0;
    }
    if (roi.x + roi.width > frame.width) {
      roi.width = NormalizeEvenFloor(frame.width - roi.x);
    }
    if (roi.y + roi.height > frame.height) {
      roi.height = NormalizeEvenFloor(frame.height - roi.y);
    }
    if (roi.width < 2 || roi.height < 2) {
      throw std::runtime_error("invalid crop configuration for camera " + std::to_string(i));
    }

    rois[i] = roi;
  }

  return rois;
}

std::vector<StitchTask> BuildStitchLayout(const std::vector<CameraRoi>& rois,
                                          const std::vector<CameraTuning>& tuning,
                                          int* panorama_width,
                                          int* panorama_height) {
  const size_t num_img = rois.size();
  std::vector<StitchTask> tasks(num_img);
  int cursor_x = 0;
  int max_y = 0;

  for (size_t i = 0; i < num_img; ++i) {
    StitchTask task;
    task.enabled = tuning[i].enabled;
    task.rotation_deg = tuning[i].rotation_deg;
    task.src_x = rois[i].x;
    task.src_y = rois[i].y;
    task.src_w = rois[i].width;
    task.src_h = rois[i].height;
    task.dst_x = NormalizeEvenFloor(cursor_x + tuning[i].offset_x);
    task.dst_y = 0;

    const int rotated_w = NormalizeEvenCeil(RotatedWidth(task.src_w, task.src_h, task.rotation_deg));
    const int rotated_h = NormalizeEvenCeil(RotatedHeight(task.src_w, task.src_h, task.rotation_deg));
    cursor_x = task.dst_x + rotated_w;
    max_y = std::max(max_y, task.dst_y + rotated_h);
    tasks[i] = task;
  }

  *panorama_width = NormalizeEvenCeil(cursor_x);
  *panorama_height = NormalizeEvenCeil(max_y);
  return tasks;
}

}  

using namespace std;

bool g_save_stitched_frames = false;
size_t g_save_frame_interval = 30;

App::App() : num_img_(0), total_cols_(0), height_(0) {
  Logger::GetInstance().Initialize();
  Logger::GetInstance().Log("[App] Application starting...");

  sensorDataInterface_.InitVideoCapture(num_img_);
  image_vector_.resize(num_img_);

  Logger::GetInstance().Log("[App] Waiting for first DRM_PRIME frames to build stitch layout...");
  sensorDataInterface_.get_image_vector(image_vector_);

  vector<cv::Mat> bootstrap_bgr(num_img_);
  for (size_t i = 0; i < num_img_; ++i) {
    bootstrap_bgr[i] = ExportHardwareFrameToBgr(image_vector_[i]);
  }

  const vector<CameraTuning> tuning = BuildDefaultTuning(num_img_);
  const vector<OverlapEstimate> overlaps = EstimateOverlaps(bootstrap_bgr);
  const vector<CameraRoi> rois =
      BuildCameraRois(image_vector_, overlaps, tuning);
  const vector<StitchTask> layout =
      BuildStitchLayout(rois, tuning, &total_cols_, &height_);

  image_stitcher_.SetParams(0, static_cast<int>(num_img_), total_cols_, height_);
  image_stitcher_.SetLayout(layout);

  ostringstream overlap_stream;
  overlap_stream << "[App] overlap estimation:";
  for (size_t i = 0; i < overlaps.size(); ++i) {
    overlap_stream << " cam" << i << "-" << (i + 1)
                   << "{overlap=" << overlaps[i].overlap
                   << " shift_y=" << overlaps[i].shift_y
                   << " score=" << overlaps[i].score << "}";
  }
  Logger::GetInstance().Log(overlap_stream.str());

  ostringstream layout_stream;
  layout_stream << "[App] zero-copy stitch layout:";
  for (size_t i = 0; i < layout.size(); ++i) {
    layout_stream << " cam" << i
                  << "{src=" << layout[i].src_x << "," << layout[i].src_y
                  << "," << layout[i].src_w << "x" << layout[i].src_h
                  << " dst=" << layout[i].dst_x << "," << layout[i].dst_y
                  << " rot=" << layout[i].rotation_deg << "}";
  }
  Logger::GetInstance().Log(layout_stream.str());

  ostringstream roi_stream;
  roi_stream << "[App] detected rois:";
  for (size_t i = 0; i < rois.size(); ++i) {
    roi_stream << " cam" << i
               << "{x=" << rois[i].x
               << " y=" << rois[i].y
               << " w=" << rois[i].width
               << " h=" << rois[i].height << "}";
  }
  Logger::GetInstance().Log(roi_stream.str());
  SaveDetectedRoiDebug(bootstrap_bgr, rois);

  if (drm_alloc_nv12(total_cols_, height_, output_drm_buf_) != 0) {
    throw std::runtime_error("failed to allocate output DRM DMA-BUF");
  }

  image_concat_.fd = output_drm_buf_.fd;
  image_concat_.width = total_cols_;
  image_concat_.height = height_;
  image_concat_.stride_w = static_cast<int>(output_drm_buf_.pitch);
  image_concat_.stride_h = height_;
}

App::~App() {
  drm_free(output_drm_buf_);
}

[[noreturn]] void App::run_stitching() {
  size_t frame_idx = 0;

  while (true) {
    const double t0 = cv::getTickCount();
    sensorDataInterface_.get_image_vector(image_vector_);
    const double t1 = cv::getTickCount();

    image_stitcher_.ClearOutput(image_concat_);

    for (size_t img_idx = 0; img_idx < num_img_; ++img_idx) {
      image_stitcher_.WarpImages(static_cast<int>(img_idx),
                                 frame_idx,
                                 image_vector_,
                                 image_concat_);
    }
    const double t2 = cv::getTickCount();

    const double tn = cv::getTickCount();
    string timing_msg =
        "[app] " +
        to_string((t1 - t0) / cv::getTickFrequency()) + ";" +
        to_string((t2 - t1) / cv::getTickFrequency());
    string fps_msg =
        to_string(1.0 / ((t2 - t0) / cv::getTickFrequency())) + " FPS; " +
        to_string(1.0 / ((tn - t0) / cv::getTickFrequency())) + " Real FPS.";

    vector<double> decode_fps_vector = sensorDataInterface_.GetDecodeFpsSnapshot();
    ostringstream decode_fps_stream;
    decode_fps_stream << "[decode_fps]";
    for (size_t i = 0; i < decode_fps_vector.size(); ++i) {
      decode_fps_stream << " ch" << i << "=" << decode_fps_vector[i];
    }

    Logger::GetInstance().LogFrame(frame_idx, timing_msg);
    Logger::GetInstance().LogFrame(frame_idx, fps_msg);
    Logger::GetInstance().LogFrame(frame_idx, decode_fps_stream.str());

    if (g_save_stitched_frames &&
        g_save_frame_interval > 0 &&
        (frame_idx % g_save_frame_interval == 0)) {
      cv::UMat stitched_bgr = ExportNv12DrmBufferToBgr(output_drm_buf_);
      if (!stitched_bgr.empty()) {
        ostringstream filename;
        filename << "stitched_" << frame_idx << ".png";
        Logger::GetInstance().SaveImage(stitched_bgr, filename.str(), frame_idx);
      } else {
        Logger::GetInstance().LogFrame(
            frame_idx,
            "[App] failed to mmap/export stitched DRM buffer for saving.");
      }
    }

    ++frame_idx;
  }
}

int main() {
  App app;
  app.run_stitching();
}
