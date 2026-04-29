#include "app.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <sys/mman.h>

#include <opencv2/opencv.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/highgui.hpp>

#include "stitching_param_generater.h"

extern "C" {
#include <rga/RgaApi.h>
}

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

StitchGlobalConfig g_config;

bool g_is_using_camera = false;
bool g_enable_visual_tuning = true;
bool g_show_roi_markers = true;
bool g_use_roi_config = true;

static constexpr size_t NUM_BOOTSTRAP_FRAMES = 3;
static constexpr double CONFIDENCE_THRESHOLD = 0.25;

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

inline void CopyNv12DataToMat(const uint8_t* y_src, const uint8_t* uv_src, int width, int height, int pitch_y, int pitch_uv, cv::Mat& dst) {
  for (int row = 0; row < height; ++row) {
    std::memcpy(dst.ptr(row), y_src + static_cast<size_t>(row) * pitch_y, static_cast<size_t>(width));
  }
  for (int row = 0; row < height / 2; ++row) {
    std::memcpy(dst.ptr(height + row), uv_src + static_cast<size_t>(row) * pitch_uv, static_cast<size_t>(width));
  }
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
  CopyNv12DataToMat(
      sw_frame->data[0], sw_frame->data[1],
      sw_frame->width, sw_frame->height,
      sw_frame->linesize[0], sw_frame->linesize[1],
      nv12
  );

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
  const uint8_t* uv_src = src + static_cast<size_t>(buffer.pitch) * buffer.height;

  CopyNv12DataToMat(
      src, uv_src,
      buffer.width, buffer.height,
      buffer.pitch, buffer.pitch,
      nv12_host
  );

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
  
  for (size_t i = 0; i < num_cameras && i < 4; ++i) {
    tuning[i].offset_x = g_config.roi_offsets[i].offset_x;
    tuning[i].offset_y = g_config.roi_offsets[i].offset_y;
  }
  return tuning;
}

OverlapEstimate EstimateOverlapByTemplate(const cv::Mat& left_gray, const cv::Mat& right_gray) {
  OverlapEstimate estimate;
  if (left_gray.empty() || right_gray.empty()) {
    return estimate;
  }

  const int common_w = std::min(left_gray.cols, right_gray.cols);
  const int common_h = std::min(left_gray.rows, right_gray.rows);
  const int search_w = NormalizeEvenFloor(std::min(common_w / 2, 1920));
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
}

OverlapEstimate EstimatePairOverlap(const cv::Mat& left_bgr, const cv::Mat& right_bgr) {
  if (left_bgr.empty() || right_bgr.empty()) {
    return OverlapEstimate{};
  }

  cv::Mat left_gray;
  cv::Mat right_gray;
  cv::cvtColor(left_bgr, left_gray, cv::COLOR_BGR2GRAY);
  cv::cvtColor(right_bgr, right_gray, cv::COLOR_BGR2GRAY);

  const int common_w = std::min(left_gray.cols, right_gray.cols);
  const int common_h = std::min(left_gray.rows, right_gray.rows);
  const int band_w = NormalizeEvenFloor(std::min(common_w / 2, 1920));
  if (band_w < 256 || common_h < 128) {
    return EstimateOverlapByTemplate(left_gray, right_gray);
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
    return EstimateOverlapByTemplate(left_gray, right_gray);
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
    return EstimateOverlapByTemplate(left_gray, right_gray);
  }

  cv::Mat inliers;
  cv::Mat affine = cv::estimateAffinePartial2D(
      pts_right, pts_left, inliers, cv::RANSAC, 3.0, 2000, 0.99, 15);
  if (affine.empty()) {
    return EstimateOverlapByTemplate(left_gray, right_gray);
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
    return EstimateOverlapByTemplate(left_gray, right_gray);
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

struct MatrixOverlap {
  OverlapEstimate h01;
  OverlapEstimate h23;
  OverlapEstimate v02;
  OverlapEstimate v13;
  double confidence = 0.0;
};

CachedOverlap MatrixOverlapToCached(const MatrixOverlap& mo) {
  CachedOverlap co;
  co.h01_overlap = mo.h01.overlap;
  co.h01_shift_y = mo.h01.shift_y;
  co.h01_score = mo.h01.score;
  co.h23_overlap = mo.h23.overlap;
  co.h23_shift_y = mo.h23.shift_y;
  co.h23_score = mo.h23.score;
  co.v02_overlap = mo.v02.overlap;
  co.v02_shift_y = mo.v02.shift_y;
  co.v02_score = mo.v02.score;
  co.v13_overlap = mo.v13.overlap;
  co.v13_shift_y = mo.v13.shift_y;
  co.v13_score = mo.v13.score;
  co.confidence = mo.confidence;
  return co;
}

MatrixOverlap CachedToMatrixOverlap(const CachedOverlap& co) {
  MatrixOverlap mo;
  mo.h01.overlap = co.h01_overlap;
  mo.h01.shift_y = co.h01_shift_y;
  mo.h01.score = co.h01_score;
  mo.h23.overlap = co.h23_overlap;
  mo.h23.shift_y = co.h23_shift_y;
  mo.h23.score = co.h23_score;
  mo.v02.overlap = co.v02_overlap;
  mo.v02.shift_y = co.v02_shift_y;
  mo.v02.score = co.v02_score;
  mo.v13.overlap = co.v13_overlap;
  mo.v13.shift_y = co.v13_shift_y;
  mo.v13.score = co.v13_score;
  mo.confidence = co.confidence;
  return mo;
}

OverlapEstimate EstimateVerticalOverlap(const cv::Mat& top_bgr, const cv::Mat& bottom_bgr) {
  cv::Mat top_rot, bottom_rot;
  cv::rotate(top_bgr, top_rot, cv::ROTATE_90_COUNTERCLOCKWISE);
  cv::rotate(bottom_bgr, bottom_rot, cv::ROTATE_90_COUNTERCLOCKWISE);
  
  OverlapEstimate est = EstimatePairOverlap(top_rot, bottom_rot);
  
  OverlapEstimate result;
  result.overlap = est.overlap;
  result.shift_y = -est.shift_y;
  result.score = est.score;
  return result;
}

MatrixOverlap EstimateOverlaps2x2(const std::vector<cv::Mat>& frames) {
  MatrixOverlap result;
  if (frames.size() < 4) {
    return result;
  }

  result.h01 = EstimatePairOverlap(frames[0], frames[1]);
  result.h23 = EstimatePairOverlap(frames[2], frames[3]);
  result.v02 = EstimateVerticalOverlap(frames[0], frames[2]);
  result.v13 = EstimateVerticalOverlap(frames[1], frames[3]);
  
  result.confidence = (result.h01.score + result.h23.score + result.v02.score + result.v13.score) / 4.0;
  return result;
}

std::vector<CameraRoi> BuildCameraRois2x2(const std::vector<NV12Frame>& frames,
                                          const MatrixOverlap& overlaps,
                                          const std::vector<CameraTuning>& tuning) {
  if (frames.size() < 4) return {};
  int W[4] = { frames[0].width, frames[1].width, frames[2].width, frames[3].width };
  int H[4] = { frames[0].height, frames[1].height, frames[2].height, frames[3].height };
  
  int X[4] = {0, 0, 0, 0};
  int Y[4] = {0, 0, 0, 0};

  X[0] = 0;
  Y[0] = 0;
  X[1] = W[0] - overlaps.h01.overlap;
  Y[1] = overlaps.h01.shift_y;
  X[2] = overlaps.v02.shift_y;
  Y[2] = H[0] - overlaps.v02.overlap;
  
  int X3_1 = X[1] + overlaps.v13.shift_y;
  int Y3_1 = Y[1] + H[1] - overlaps.v13.overlap;
  int X3_2 = X[2] + W[2] - overlaps.h23.overlap;
  int Y3_2 = Y[2] + overlaps.h23.shift_y;
  X[3] = (X3_1 + X3_2) / 2;
  Y[3] = (Y3_1 + Y3_2) / 2;
  
  for (int i = 0; i < 4; ++i) {
    X[i] += tuning[i].offset_x;
    Y[i] += tuning[i].offset_y;
  }
  
  int X_mid_01 = (X[1] + X[0] + W[0]) / 2;
  int X_mid_23 = (X[3] + X[2] + W[2]) / 2;
  int cut_x = NormalizeEvenFloor((X_mid_01 + X_mid_23) / 2);
  
  int Y_mid_02 = (Y[2] + Y[0] + H[0]) / 2;
  int Y_mid_13 = (Y[3] + Y[1] + H[1]) / 2;
  int cut_y = NormalizeEvenFloor((Y_mid_02 + Y_mid_13) / 2);
  
  int blend_w = NormalizeEvenFloor(std::max(20, g_config.feather_width));
  
  int cut_x_left   = cut_x;
  int cut_x_right  = cut_x;
  int cut_y_top    = cut_y;
  int cut_y_bottom = cut_y;
  
  cut_x_left  += blend_w / 2;
  cut_x_right -= blend_w / 2;
  cut_y_top += blend_w / 2;
  cut_y_bottom -= blend_w / 2;

  int min_x = NormalizeEvenCeil(std::max(X[0], X[2]));
  int max_x = NormalizeEvenFloor(std::min(X[1] + W[1], X[3] + W[3]));
  int min_y = NormalizeEvenCeil(std::max(Y[0], Y[1]));
  int max_y = NormalizeEvenFloor(std::min(Y[2] + H[2], Y[3] + H[3]));
  
  cut_x_left = std::max(min_x + 2, std::min(max_x - 2, cut_x_left));
  cut_x_right = std::max(min_x + 2, std::min(max_x - 2, cut_x_right));
  cut_y_top = std::max(min_y + 2, std::min(max_y - 2, cut_y_top));
  cut_y_bottom = std::max(min_y + 2, std::min(max_y - 2, cut_y_bottom));
  
  std::vector<CameraRoi> rois(4);
  
  rois[0].x = NormalizeEvenFloor(min_x - X[0] + tuning[0].crop_left);
  rois[0].y = NormalizeEvenFloor(min_y - Y[0] + tuning[0].crop_top);
  rois[0].width = NormalizeEvenFloor(cut_x_left - min_x - tuning[0].crop_left - tuning[0].crop_right);
  rois[0].height = NormalizeEvenFloor(cut_y_top - min_y - tuning[0].crop_top - tuning[0].crop_bottom);
  
  rois[1].x = NormalizeEvenFloor(cut_x_right - X[1] + tuning[1].crop_left);
  rois[1].y = NormalizeEvenFloor(min_y - Y[1] + tuning[1].crop_top);
  rois[1].width = NormalizeEvenFloor(max_x - cut_x_right - tuning[1].crop_left - tuning[1].crop_right);
  rois[1].height = NormalizeEvenFloor(cut_y_top - min_y - tuning[1].crop_top - tuning[1].crop_bottom);
  
  rois[2].x = NormalizeEvenFloor(min_x - X[2] + tuning[2].crop_left);
  rois[2].y = NormalizeEvenFloor(cut_y_bottom - Y[2] + tuning[2].crop_top);
  rois[2].width = NormalizeEvenFloor(cut_x_left - min_x - tuning[2].crop_left - tuning[2].crop_right);
  rois[2].height = NormalizeEvenFloor(max_y - cut_y_bottom - tuning[2].crop_top - tuning[2].crop_bottom);
  
  rois[3].x = NormalizeEvenFloor(cut_x_right - X[3] + tuning[3].crop_left);
  rois[3].y = NormalizeEvenFloor(cut_y_bottom - Y[3] + tuning[3].crop_top);
  rois[3].width = NormalizeEvenFloor(max_x - cut_x_right - tuning[3].crop_left - tuning[3].crop_right);
  rois[3].height = NormalizeEvenFloor(max_y - cut_y_bottom - tuning[3].crop_top - tuning[3].crop_bottom);
  
  for (int i = 0; i < 4; ++i) {
    if (rois[i].x < 0) rois[i].x = 0;
    if (rois[i].y < 0) rois[i].y = 0;
    if (rois[i].x + rois[i].width > W[i]) rois[i].width = NormalizeEvenFloor(W[i] - rois[i].x);
    if (rois[i].y + rois[i].height > H[i]) rois[i].height = NormalizeEvenFloor(H[i] - rois[i].y);
    if (rois[i].width < 2 || rois[i].height < 2) {
      throw std::runtime_error("invalid 2x2 crop mapping for camera " + std::to_string(i));
    }
  }
  
  return rois;
}

std::vector<StitchTask> BuildStitchLayout2x2(const std::vector<CameraRoi>& rois,
                                             const std::vector<CameraTuning>& tuning,
                                             int* panorama_width,
                                             int* panorama_height) {
  if (rois.size() < 4) return {};
  std::vector<StitchTask> tasks(4);

  for (size_t i = 0; i < 4; ++i) {
    tasks[i].enabled = tuning[i].enabled;
    tasks[i].rotation_deg = tuning[i].rotation_deg;
    tasks[i].src_x = rois[i].x;
    tasks[i].src_y = rois[i].y;
    tasks[i].src_w = rois[i].width;
    tasks[i].src_h = rois[i].height;
  }

  int blend_w = NormalizeEvenFloor(std::max(20, g_config.feather_width));

  tasks[0].dst_x = 0;
  tasks[0].dst_y = 0;

  tasks[1].dst_x = NormalizeEvenFloor(rois[0].width) - blend_w;
  tasks[1].dst_y = 0;

  tasks[2].dst_x = 0;
  tasks[2].dst_y = NormalizeEvenFloor(rois[0].height) - blend_w;

  tasks[3].dst_x = NormalizeEvenFloor(rois[0].width) - blend_w;
  tasks[3].dst_y = NormalizeEvenFloor(rois[0].height) - blend_w;

  *panorama_width = NormalizeEvenCeil(tasks[1].dst_x + rois[1].width);
  *panorama_height = NormalizeEvenCeil(tasks[2].dst_y + rois[2].height);

  return tasks;
}

std::vector<StitchTask> BuildWarpLayout(const StitchingWarpData& warp_data) {
  std::vector<StitchTask> tasks(warp_data.entries.size());
  for (size_t i = 0; i < warp_data.entries.size(); ++i) {
    const WarpMapEntry& entry = warp_data.entries[i];
    tasks[i].enabled = entry.valid();
    tasks[i].rotation_deg = 0;
    tasks[i].src_x = 0;
    tasks[i].src_y = 0;
    tasks[i].src_w = entry.xmap.cols;
    tasks[i].src_h = entry.xmap.rows;
    tasks[i].dst_x = NormalizeEvenFloor(entry.roi.x);
    tasks[i].dst_y = NormalizeEvenFloor(entry.roi.y);
  }
  return tasks;
}

}

using namespace std;

void App::BootStrapOptimalLayout() {
  Logger::GetInstance().Log("[App] [MULTI-FRAME ROI] Starting multi-frame ROI detection...");
  if (g_multi_frame_roi_debug_level >= 1) {
    ostringstream debug_msg;
    debug_msg << "[App] [MULTI-FRAME ROI] Will capture up to " << NUM_BOOTSTRAP_FRAMES 
              << " frames for ROI detection";
    Logger::GetInstance().Log(debug_msg.str());
  }

  const vector<CameraTuning> tuning = BuildDefaultTuning(num_img_);
  sensorDataInterface_.get_image_vector(image_vector_);
  std::vector<cv::Mat> bootstrap_bgr(num_img_);
  for (size_t i = 0; i < num_img_; ++i) {
    bootstrap_bgr[i] = ExportHardwareFrameToBgr(image_vector_[i]);
  }

  struct RoiDetectionResult {
    MatrixOverlap overlaps;
    vector<CameraRoi> rois;
    vector<StitchTask> layout;
    double confidence = 0.0;
    size_t frame_index = 0;
  };
  
  std::vector<RoiDetectionResult> detection_results;
  
  size_t frame_count = 0;
  while (frame_count < NUM_BOOTSTRAP_FRAMES) {
    if (frame_count > 0) {
      sensorDataInterface_.get_image_vector(image_vector_);
      for (size_t i = 0; i < num_img_; ++i) {
        bootstrap_bgr[i] = ExportHardwareFrameToBgr(image_vector_[i]);
      }
    }
    
    const MatrixOverlap overlaps = EstimateOverlaps2x2(bootstrap_bgr);
    const vector<CameraRoi> rois = BuildCameraRois2x2(image_vector_, overlaps, tuning);
    
    double avg_confidence = overlaps.confidence;
    
    bool all_valid = (overlaps.h01.score >= CONFIDENCE_THRESHOLD &&
                      overlaps.h23.score >= CONFIDENCE_THRESHOLD &&
                      overlaps.v02.score >= CONFIDENCE_THRESHOLD &&
                      overlaps.v13.score >= CONFIDENCE_THRESHOLD);
    
    int dummy_panorama_width = 0;
    int dummy_panorama_height = 0;
    const vector<StitchTask> layout = 
        BuildStitchLayout2x2(rois, tuning, &dummy_panorama_width, &dummy_panorama_height);
    
    RoiDetectionResult result;
    result.overlaps = overlaps;
    result.rois = rois;
    result.layout = layout;
    result.confidence = avg_confidence;
    result.frame_index = frame_count;
    detection_results.push_back(result);
    
    if (g_multi_frame_roi_debug_level >= 1) {
      ostringstream debug_msg;
      debug_msg << "[App] [MULTI-FRAME ROI] Frame #" << frame_count << ": "
                << "avg_confidence=" << std::fixed << std::setprecision(4) << avg_confidence;
      if (all_valid) {
        debug_msg << " [VALID]";
      } else {
        debug_msg << " [WEAK]";
      }
      
      debug_msg << " h01_score=" << std::fixed << std::setprecision(3) << overlaps.h01.score;
      debug_msg << " h23_score=" << std::fixed << std::setprecision(3) << overlaps.h23.score;
      debug_msg << " v02_score=" << std::fixed << std::setprecision(3) << overlaps.v02.score;
      debug_msg << " v13_score=" << std::fixed << std::setprecision(3) << overlaps.v13.score;
      Logger::GetInstance().Log(debug_msg.str());
    }
    
    frame_count++;
    
    if (all_valid && avg_confidence > 0.7) {
      if (g_multi_frame_roi_debug_level >= 1) {
        ostringstream debug_msg;
        debug_msg << "[App] [MULTI-FRAME ROI] Early exit: Found high-quality result at frame #" 
                  << frame_count - 1 << " with confidence=" << std::fixed << std::setprecision(4) 
                  << avg_confidence;
        Logger::GetInstance().Log(debug_msg.str());
      }
      break;
    }
  }
  
  size_t best_result_idx = 0;
  double max_confidence = -1.0;
  for (size_t i = 0; i < detection_results.size(); ++i) {
    if (detection_results[i].confidence > max_confidence) {
      max_confidence = detection_results[i].confidence;
      best_result_idx = i;
    }
  }
  
  const RoiDetectionResult& best_result = detection_results[best_result_idx];
  const MatrixOverlap& overlaps = best_result.overlaps;
  const vector<CameraRoi>& rois = best_result.rois;
  const vector<StitchTask>& layout = best_result.layout;
  
  if (g_multi_frame_roi_debug_level >= 1) {
    ostringstream summary_msg;
    summary_msg << "[App] [MULTI-FRAME ROI 2x2] Detection completed: "
                << "total_frames=" << detection_results.size()
                << " best_frame=" << best_result.frame_index
                << " best_confidence=" << std::fixed << std::setprecision(4) << max_confidence;
    Logger::GetInstance().Log(summary_msg.str());
  }
  
  int dummy_panorama_width = 0;
  int dummy_panorama_height = 0;
  BuildStitchLayout2x2(rois, tuning, &dummy_panorama_width, &dummy_panorama_height);
  total_cols_ = dummy_panorama_width;
  height_ = dummy_panorama_height;

  int blend_width = NormalizeEvenFloor(std::max(20, g_config.feather_width));
  image_stitcher_.SetParams(blend_width, static_cast<int>(num_img_), total_cols_, height_);
  image_stitcher_.SetLayout(layout);

  ostringstream overlap_stream;
  overlap_stream << "[App] 2x2 matrix overlap estimation (from frame #" << best_result.frame_index << "):\n"
                 << " h01{overlap=" << overlaps.h01.overlap << " shift_y=" << overlaps.h01.shift_y << " score=" << overlaps.h01.score << "}\n"
                 << " h23{overlap=" << overlaps.h23.overlap << " shift_y=" << overlaps.h23.shift_y << " score=" << overlaps.h23.score << "}\n"
                 << " v02{overlap_y=" << overlaps.v02.overlap << " shift_x=" << overlaps.v02.shift_y << " score=" << overlaps.v02.score << "}\n"
                 << " v13{overlap_y=" << overlaps.v13.overlap << " shift_x=" << overlaps.v13.shift_y << " score=" << overlaps.v13.score << "}";
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
  roi_stream << "[App] detected rois (from frame #" << best_result.frame_index << "):";
  for (size_t i = 0; i < rois.size(); ++i) {
    roi_stream << " cam" << i
               << "{x=" << rois[i].x
               << " y=" << rois[i].y
               << " w=" << rois[i].width
               << " h=" << rois[i].height << "}";
  }
  Logger::GetInstance().Log(roi_stream.str());
  
  std::vector<cv::Mat> best_bootstrap_bgr(num_img_);
  for (size_t i = 0; i < num_img_; ++i) {
    best_bootstrap_bgr[i] = ExportHardwareFrameToBgr(image_vector_[i]);
  }
  SaveDetectedRoiDebug(best_bootstrap_bgr, rois);
}

void App::SyncConfigToGlobals() {
}

void App::InitFromConfig() {
  const vector<CameraTuning> tuning = BuildDefaultTuning(num_img_);

  sensorDataInterface_.get_image_vector(image_vector_);
  std::vector<cv::Mat> bootstrap_bgr(num_img_);
  for (size_t i = 0; i < num_img_; ++i) {
    bootstrap_bgr[i] = ExportHardwareFrameToBgr(image_vector_[i]);
  }
  cached_overlaps_ = MatrixOverlapToCached(EstimateOverlaps2x2(bootstrap_bgr));

  const vector<CameraRoi> rois = BuildCameraRois2x2(image_vector_, CachedToMatrixOverlap(cached_overlaps_), tuning);
  int w = 0, h = 0;
  const vector<StitchTask> layout = BuildStitchLayout2x2(rois, tuning, &w, &h);

  total_cols_ = w;
  height_ = h;
  
  int blend_width = NormalizeEvenFloor(std::max(20, g_config.feather_width));
  image_stitcher_.SetParams(blend_width, static_cast<int>(num_img_), total_cols_, height_);
  image_stitcher_.SetLayout(layout);

  if (g_multi_frame_roi_debug_level >= 1) {
    ostringstream msg;
    msg << "[App] [CONFIG INIT] Panorama size: " << total_cols_ << "x" << height_;
    Logger::GetInstance().Log(msg.str());
  }
}

void App::RebuildLayout() {
  const vector<CameraTuning> tuning = BuildDefaultTuning(num_img_);

  const vector<CameraRoi> rois = BuildCameraRois2x2(
      frames_locked_ ? saved_frames_ : image_vector_,
      CachedToMatrixOverlap(cached_overlaps_), tuning);
  int new_w = 0, new_h = 0;
  const vector<StitchTask> layout = BuildStitchLayout2x2(rois, tuning, &new_w, &new_h);

  if (new_w != total_cols_ || new_h != height_) {
    Logger::GetInstance().Log("[App] [REBUILD] Panorama size changed, reallocating DRM buffer");
    drm_free(output_drm_buf_);
    if (drm_alloc_nv12(new_w, new_h, output_drm_buf_) != 0) {
      throw std::runtime_error("failed to reallocate output DRM DMA-BUF");
    }
    total_cols_ = new_w;
    height_ = new_h;
    image_concat_.fd = output_drm_buf_.fd;
    image_concat_.width = total_cols_;
    image_concat_.height = height_;
    image_concat_.stride_w = static_cast<int>(output_drm_buf_.pitch);
    image_concat_.stride_h = height_;
  }

  int blend_width = NormalizeEvenFloor(std::max(20, g_config.feather_width));
  image_stitcher_.SetParams(blend_width, static_cast<int>(num_img_), total_cols_, height_);
  image_stitcher_.SetLayout(layout);
}

void App::SaveCurrentFrames() {
  ReleaseSavedFrames();
  
  saved_frames_.resize(num_img_);
  saved_drm_bufs_.resize(num_img_);
  
  for (size_t i = 0; i < num_img_; ++i) {
    if (image_vector_[i].empty()) continue;
    
    int w = image_vector_[i].width;
    int h = image_vector_[i].height;
    
    if (drm_alloc_nv12(w, h, saved_drm_bufs_[i]) != 0) {
      Logger::GetInstance().LogError("[App] Failed to allocate saved frame buffer for cam" + to_string(i));
      continue;
    }
    
    int src_stride_w = image_vector_[i].stride_w > 0 ? image_vector_[i].stride_w : w;
    int src_stride_h = image_vector_[i].stride_h > 0 ? image_vector_[i].stride_h : h;
    
    rga_info_t src_info;
    memset(&src_info, 0, sizeof(src_info));
    src_info.fd = image_vector_[i].fd;
    src_info.mmuFlag = 1;
    rga_set_rect(&src_info.rect, 0, 0, w, h, src_stride_w, src_stride_h, RK_FORMAT_YCbCr_420_SP);
    
    rga_info_t dst_info;
    memset(&dst_info, 0, sizeof(dst_info));
    dst_info.fd = saved_drm_bufs_[i].fd;
    dst_info.mmuFlag = 1;
    rga_set_rect(&dst_info.rect, 0, 0, w, h, saved_drm_bufs_[i].pitch, h, RK_FORMAT_YCbCr_420_SP);
    
    if (c_RkRgaBlit(&src_info, &dst_info, nullptr) != 0) {
      Logger::GetInstance().LogError("[App] RGA blit failed for saving frame cam" + to_string(i));
      drm_free(saved_drm_bufs_[i]);
      continue;
    }
    
    saved_frames_[i].fd = saved_drm_bufs_[i].fd;
    saved_frames_[i].width = w;
    saved_frames_[i].height = h;
    saved_frames_[i].stride_w = saved_drm_bufs_[i].pitch;
    saved_frames_[i].stride_h = h;
    saved_frames_[i].owner = nullptr;
  }
  
  frames_locked_ = true;
  locked_frame_idx_ = 0;
  Logger::GetInstance().Log("[App] [FRAME LOCK] Saved current frames for debug tuning");
}

void App::ReleaseSavedFrames() {
  for (size_t i = 0; i < saved_drm_bufs_.size(); ++i) {
    drm_free(saved_drm_bufs_[i]);
  }
  saved_drm_bufs_.clear();
  saved_frames_.clear();
  frames_locked_ = false;
}

void App::RestitchSavedFrames() {
  const vector<CameraTuning> tuning = BuildDefaultTuning(num_img_);
  const vector<CameraRoi> rois = BuildCameraRois2x2(saved_frames_, CachedToMatrixOverlap(cached_overlaps_), tuning);
  int new_w = 0, new_h = 0;
  const vector<StitchTask> layout = BuildStitchLayout2x2(rois, tuning, &new_w, &new_h);
  
  image_stitcher_.SetLayout(layout);
  
  image_stitcher_.ClearOutput(image_concat_);
  for (size_t img_idx = 0; img_idx < num_img_; ++img_idx) {
    image_stitcher_.WarpImages(static_cast<int>(img_idx), locked_frame_idx_, saved_frames_, image_concat_);
  }
  if (g_config.feather_enabled) {
    image_stitcher_.BlendSeams(saved_frames_, image_concat_);
  }
}

App::App() : num_img_(0), total_cols_(0), height_(0),
             visual_mode_(g_enable_visual_tuning), debug_mode_(false),
             frames_locked_(false), locked_frame_idx_(0) {
  Logger::GetInstance().Initialize();
  Logger::GetInstance().Log("[App] Application starting...");
  Logger::GetInstance().Log(string("[App] Visual tuning: ") + (visual_mode_ ? "ENABLED" : "DISABLED (set ENABLE_VISUAL_TUNING=1 to enable)"));

  sensorDataInterface_.InitVideoCapture(num_img_);
  image_vector_.resize(num_img_);

  bool config_loaded = false;
  if (g_use_roi_config) {
    config_loaded = RoiConfig::LoadFromFile("../params/roi_tuning.yaml", g_config);
  }

  if (config_loaded) {
    Logger::GetInstance().Log("[App] ROI config loaded from ../params/roi_tuning.yaml");
    InitFromConfig();
  } else {
    Logger::GetInstance().Log("[App] No ROI config or USE_ROI_CONFIG=0, running auto-detection...");
    BootStrapOptimalLayout();

    sensorDataInterface_.get_image_vector(image_vector_);
    std::vector<cv::Mat> bootstrap_bgr(num_img_);
    for (size_t i = 0; i < num_img_; ++i) {
      bootstrap_bgr[i] = ExportHardwareFrameToBgr(image_vector_[i]);
    }
    cached_overlaps_ = MatrixOverlapToCached(EstimateOverlaps2x2(bootstrap_bgr));

    RoiConfig::SaveToFile("../params/roi_tuning.yaml", g_config);
    Logger::GetInstance().Log("[App] ROI config saved to ../params/roi_tuning.yaml");
  }

  if (drm_alloc_nv12(total_cols_, height_, output_drm_buf_) != 0) {
    throw std::runtime_error("failed to allocate output DRM DMA-BUF");
  }

  image_concat_.fd = output_drm_buf_.fd;
  image_concat_.width = total_cols_;
  image_concat_.height = height_;
  image_concat_.stride_w = static_cast<int>(output_drm_buf_.pitch);
  image_concat_.stride_h = height_;

  if (visual_mode_) {
    Logger::GetInstance().Log("[App] Initializing visualizer with panorama size: " + 
                              std::to_string(total_cols_) + "x" + std::to_string(height_));
    if (!RoiVisualizer::Init(total_cols_, height_)) {
      Logger::GetInstance().Log("[App] OpenCV highgui init failed, disabling visual mode");
      Logger::GetInstance().Log("[App] Check if DISPLAY environment variable is set correctly");
      visual_mode_ = false;
    } else {
      Logger::GetInstance().Log("[App] Visualizer initialized successfully");
    }
  }
}

App::~App() {
  ReleaseSavedFrames();
  if (visual_mode_) RoiVisualizer::Shutdown();
  drm_free(output_drm_buf_);
}

[[noreturn]] void App::run_stitching() {
  size_t frame_idx = 0;

  while (true) {
    const double t0 = cv::getTickCount();
    
    if (!debug_mode_ || !frames_locked_) {
      sensorDataInterface_.get_image_vector(image_vector_);
    }
    
    const vector<NV12Frame>& stitch_input = debug_mode_ && frames_locked_ ? saved_frames_ : image_vector_;
    
    image_stitcher_.ClearOutput(image_concat_);
    
    for (size_t img_idx = 0; img_idx < num_img_; ++img_idx) {
      image_stitcher_.WarpImages(static_cast<int>(img_idx),
                                 debug_mode_ && frames_locked_ ? locked_frame_idx_ : frame_idx,
                                 stitch_input,
                                 image_concat_);
    }
    
    if (g_config.feather_enabled) {
      image_stitcher_.BlendSeams(stitch_input, image_concat_);
    }
    
    const double t2 = cv::getTickCount();
    double fps = 1.0 / ((t2 - t0) / cv::getTickFrequency());

    if (visual_mode_) {
      cv::UMat stitched_umat = ExportNv12DrmBufferToBgr(output_drm_buf_);
      cv::Mat stitched_bgr = stitched_umat.getMat(cv::ACCESS_READ);

      VisAction action;
      if (!debug_mode_) {
        action = RoiVisualizer::ShowStreamingFrame(stitched_bgr.data,
                                                     stitched_bgr.cols, stitched_bgr.rows,
                                                     static_cast<int>(stitched_bgr.step), fps);
        if (action == kVisEnterDebug) {
          debug_mode_ = true;
          locked_frame_idx_ = frame_idx;
          SaveCurrentFrames();
          Logger::GetInstance().Log("[App] [DEBUG] Entered ROI debug mode, frame #" + to_string(frame_idx) + " locked");
        }
      } else {
        action = RoiVisualizer::ShowDebugFrame(stitched_bgr.data,
                                                stitched_bgr.cols, stitched_bgr.rows,
                                                static_cast<int>(stitched_bgr.step), fps,
                                                image_stitcher_.GetLayout());

        if (action == kVisNeedRestitch) {
          RestitchSavedFrames();
          Logger::GetInstance().Log("[App] [DEBUG] ROI offset adjusted, restitched saved frame");
        }
        
        if (action == kVisNeedRebuild) {
          RebuildLayout();
          RestitchSavedFrames();
          Logger::GetInstance().Log("[App] [DEBUG] Feather params changed, layout rebuilt and restitched");
        }
        
        if (action == kVisSaveConfig) {
          RoiConfig::SaveToFile("../params/roi_tuning.yaml", g_config);
          Logger::GetInstance().Log("[App] [DEBUG] ROI config saved to ../params/roi_tuning.yaml");
        }
        
        if (action == kVisExitDebug) {
          debug_mode_ = false;
          ReleaseSavedFrames();
          Logger::GetInstance().Log("[App] [DEBUG] Exited ROI debug mode, resumed live stitching");
        }
      }
    } else {
      string fps_msg = to_string(fps) + " FPS;";
      vector<double> decode_fps_vector = sensorDataInterface_.GetDecodeFpsSnapshot();
      ostringstream decode_fps_stream;
      decode_fps_stream << "[decode_fps]";
      for (size_t i = 0; i < decode_fps_vector.size(); ++i) {
        decode_fps_stream << " ch" << i << "=" << decode_fps_vector[i];
      }

      Logger::GetInstance().LogFrame(frame_idx, fps_msg);
      Logger::GetInstance().LogFrame(frame_idx, decode_fps_stream.str());

      if (g_config.save_enabled &&
          g_config.save_interval > 0 &&
          (frame_idx % g_config.save_interval == 0)) {
        cv::UMat stitched_bgr = ExportNv12DrmBufferToBgr(output_drm_buf_);
        if (!stitched_bgr.empty()) {
          ostringstream filename;
          filename << "stitched_" << frame_idx << ".png";
          Logger::GetInstance().SaveImage(stitched_bgr, filename.str(), frame_idx);
        }
      }
    }

    if (!debug_mode_) ++frame_idx;
  }
}

int main() {
  const char* env_visual = getenv("ENABLE_VISUAL_TUNING");
  g_enable_visual_tuning = (env_visual == nullptr || atoi(env_visual) != 0);

  const char* env_markers = getenv("SHOW_ROI_MARKERS");
  g_show_roi_markers = (env_markers == nullptr || atoi(env_markers) != 0);

  const char* env_config = getenv("USE_ROI_CONFIG");
  g_use_roi_config = (env_config == nullptr || atoi(env_config) != 0);

  App app;
  app.run_stitching();
}