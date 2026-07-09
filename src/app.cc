#include "app.h"
#include "status_writer.h"
#include "http_server.h"
#include "mjpeg_streamer.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <sys/mman.h>

#include <opencv2/opencv.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/highgui.hpp>

#include "stitching_param_generater.h"
#include "camera_intrinsics.h"  // v3.x.2: LoadCamchain + BuildUndistortMap

extern "C" {
#include <rga/RgaApi.h>
}

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

StitchGlobalConfig g_config;

// v2 改动: g_is_using_camera 死代码删除 (从未被任何代码置 true).
// 输入源路由已下沉到 SensorDataInterface::InitVideoCapture
// (按 CameraSource.type 分发, 见 include/sensor_data_interface.h).
bool g_enable_visual_tuning = true;
bool g_show_roi_markers = true;
bool g_use_roi_config = true;
bool g_skip_bootstrap = false;   // SKIP_BOOTSTRAP=1: 固定支架场景, YAML 缺失时仍跑一次 bootstrap 作为兜底

static constexpr size_t NUM_BOOTSTRAP_FRAMES = 3;
static constexpr double CONFIDENCE_THRESHOLD = 0.25;

namespace {

struct CameraTuning {
  int rotation_deg = 0;
  int crop_left = 0;
  int crop_right = 0;
  int crop_top = 0;
  int crop_bottom = 0;
  bool enabled = true;
};

struct OverlapEstimate {
  int overlap = 0;
  int shift_y = 0;
  double score = 0.0;
  // v3.x.1 (2026-07-09): cv::estimateAffinePartial2D 返回的 2x3 矩阵 [a b; c d; tx ty]
  //   (cv::Mat 表示法, 2 行 3 列, 行主序). empty 表示没拿到 (匹配失败或 score 太低).
  //   App 用它做 bootstrap 阶段 pre-warp 和 stitch 阶段 per-cam affine.
  cv::Mat affine;
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
  if (frame.empty()) {
    return cv::Mat();
  }

  // v2.4 (2026-07-06): gstreamer-rockchip 路径 (vendor 推荐), 走 RGA 把
  // DMA-BUF NV12 直接转 BGR 落到 cv::Mat. 老的 FFmpeg rkmpp AVFrame 路径
  // 保留作 fallback (理论上 2026-07 后不会再走到).
  if (frame.owner_gst_sample != nullptr && frame.fd >= 0) {
    const int w = frame.width;
    const int h = frame.height;
    if (w <= 0 || h <= 0) {
      return cv::Mat();
    }
    cv::Mat bgr(h, w, CV_8UC3);

    rga_info_t src_info;
    memset(&src_info, 0, sizeof(src_info));
    src_info.fd = frame.fd;
    src_info.mmuFlag = 1;
    const int src_stride = frame.stride_w > 0 ? frame.stride_w : w;
    rga_set_rect(&src_info.rect, 0, 0, w, h, src_stride, h, RK_FORMAT_YCbCr_420_SP);

    rga_info_t dst_info;
    memset(&dst_info, 0, sizeof(dst_info));
    dst_info.virAddr = bgr.data;
    dst_info.mmuFlag = 1;
    rga_set_rect(&dst_info.rect, 0, 0, w, h, w, h, RK_FORMAT_BGR_888);

    if (c_RkRgaBlit(&src_info, &dst_info, nullptr) != 0) {
      Logger::GetInstance().LogError(
          "[ExportHardwareFrameToBgr] RGA NV12->BGR blit failed (gst path)");
      return cv::Mat();
    }
    return bgr;
  }

  // 老 FFmpeg rkmpp AVFrame 路径, 保留作 fallback.
  if (!frame.owner) {
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
  // v3.x (2026-07-09): offset_x/y 已删. ROI 直接来自 g_config.camera_rois[i] (yaml),
  //   BuildCameraRois2x3/2x2 内自己读, 不再通过 CameraTuning 间接传.
  return tuning;
}

OverlapEstimate EstimateOverlapByTemplate(const cv::Mat& left_gray, const cv::Mat& right_gray) {
  OverlapEstimate estimate;
  if (left_gray.empty() || right_gray.empty()) {
    return estimate;
  }

  const int common_w = std::min(left_gray.cols, right_gray.cols);
  const int common_h = std::min(left_gray.rows, right_gray.rows);
  // v2: 2K 视频 (2560x1440) 下 common_w=2560, common_w/2=1280 自动覆盖水平重叠
  // 候选区, 不再需要 4K 时代的 1920 magic number. 这里改用 common_w 自身做软上限,
  // 对 2K/4K 都自适应. (4K 时代 bug: 上限 1200 不足以覆盖 1400-1600 真实重叠)
  const int search_w = NormalizeEvenFloor(common_w / 2);
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
  // v2: 同 EstimateOverlapByTemplate 的注释, 改用 common_w 自适应.
  const int band_w = NormalizeEvenFloor(common_w / 2);
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
  // v3.x.1 (2026-07-09): 把 2x3 affine 保存下来, 上层 App 用它做 per-cam warp.
  //   注意: estimateAffinePartial2D 返回的 affine 把 right_gray 上的点映射到 left_gray
  //   上的对应点. 我们要让 cam_right (邻接的 cam) 映射到 cam_left (左/上 cam), 因此
  //   这里直接保存 cv::Mat 即可. 但语义上对垂直对 (EstimateVerticalOverlap) 需要
  //   旋转 90° 再用 (EstimateVerticalOverlap 自己处理).
  estimate.affine = affine;

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
  OverlapEstimate h01;  // cam0-cam1 (水平)
  OverlapEstimate h23;  // cam2-cam3 (水平)
  OverlapEstimate h45;  // cam4-cam5 (水平)
  OverlapEstimate v02;  // cam0-cam2 (垂直)
  OverlapEstimate v13;  // cam1-cam3 (垂直)
  OverlapEstimate v24;  // cam2-cam4 (垂直)
  OverlapEstimate v35;  // cam3-cam5 (垂直)
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
  co.h45_overlap = mo.h45.overlap;
  co.h45_shift_y = mo.h45.shift_y;
  co.h45_score = mo.h45.score;
  co.v02_overlap = mo.v02.overlap;
  co.v02_shift_y = mo.v02.shift_y;
  co.v02_score = mo.v02.score;
  co.v13_overlap = mo.v13.overlap;
  co.v13_shift_y = mo.v13.shift_y;
  co.v13_score = mo.v13.score;
  co.v24_overlap = mo.v24.overlap;
  co.v24_shift_y = mo.v24.shift_y;
  co.v24_score = mo.v24.score;
  co.v35_overlap = mo.v35.overlap;
  co.v35_shift_y = mo.v35.shift_y;
  co.v35_score = mo.v35.score;
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
  mo.h45.overlap = co.h45_overlap;
  mo.h45.shift_y = co.h45_shift_y;
  mo.h45.score = co.h45_score;
  mo.v02.overlap = co.v02_overlap;
  mo.v02.shift_y = co.v02_shift_y;
  mo.v02.score = co.v02_score;
  mo.v13.overlap = co.v13_overlap;
  mo.v13.shift_y = co.v13_shift_y;
  mo.v13.score = co.v13_score;
  mo.v24.overlap = co.v24_overlap;
  mo.v24.shift_y = co.v24_shift_y;
  mo.v24.score = co.v24_score;
  mo.v35.overlap = co.v35_overlap;
  mo.v35.shift_y = co.v35_shift_y;
  mo.v35.score = co.v35_score;
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
  // v3.x.1 (2026-07-09): 内部 PairOverlap 是把 bottom_rot 的点映射到 top_rot 上 (旋转 90° 后
  //   横向变纵向). 要拿回原图坐标系下的 cam_bottom -> cam_top 仿射, 需要把 affine 也旋转回去:
  //   设 rot 是 90° CCW 旋转矩阵 (2x2), 则 rot^-1 = rot^T (90° CW).
  //     rot * [a b tx]^T -> 在原图坐标系 = rot * est.affine * rot^-1 * 原图点
  //   等价于把 affine 整体旋转回去:
  //     A_orig = rot * A_rot * rot^-1
  if (!est.affine.empty() && est.affine.rows == 2 && est.affine.cols == 3) {
    const double a = est.affine.at<double>(0, 0);
    const double b = est.affine.at<double>(0, 1);
    const double c = est.affine.at<double>(1, 0);
    const double d = est.affine.at<double>(1, 1);
    const double tx = est.affine.at<double>(0, 2);
    const double ty = est.affine.at<double>(1, 2);
    // rot = [cos90 -sin90; sin90 cos90] = [0 -1; 1 0]
    // rot^-1 = [0 1; -1 0]
    // A_orig = rot * A_rot * rot^-1, 把旋转矩阵显式乘开:
    //   A_orig[0,0] = -c, A_orig[0,1] = a
    //   A_orig[1,0] = -d, A_orig[1,1] = b
    // 平移分量 tx, ty 不变 (因为旋转中心在原点, 但我们的旋转是绕帧中心旋转的, 中心不在原点;
    // 严格来说需要补偿帧尺寸. 2K 帧小角度旋转下误差很小, 这里暂时简化处理).
    cv::Mat affine_orig = cv::Mat::zeros(2, 3, CV_64F);
    affine_orig.at<double>(0, 0) = -c;
    affine_orig.at<double>(0, 1) = a;
    affine_orig.at<double>(1, 0) = -d;
    affine_orig.at<double>(1, 1) = b;
    affine_orig.at<double>(0, 2) = tx;
    affine_orig.at<double>(1, 2) = ty;
    result.affine = affine_orig;
  }
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

// v2.3: 6 路 2x3 (2 列 x 3 行) overlap 估计. 7 对邻接:
//   水平: h01 (cam0-cam1), h23 (cam2-cam3), h45 (cam4-cam5)
//   垂直: v02 (cam0-cam2), v13 (cam1-cam3), v24 (cam2-cam4), v35 (cam3-cam5)
MatrixOverlap EstimateOverlaps2x3(const std::vector<cv::Mat>& frames) {
  MatrixOverlap result;
  if (frames.size() < 6) {
    return result;
  }

  result.h01 = EstimatePairOverlap(frames[0], frames[1]);
  result.h23 = EstimatePairOverlap(frames[2], frames[3]);
  result.h45 = EstimatePairOverlap(frames[4], frames[5]);
  result.v02 = EstimateVerticalOverlap(frames[0], frames[2]);
  result.v13 = EstimateVerticalOverlap(frames[1], frames[3]);
  result.v24 = EstimateVerticalOverlap(frames[2], frames[4]);
  result.v35 = EstimateVerticalOverlap(frames[3], frames[5]);

  const double sum = result.h01.score + result.h23.score + result.h45.score
                   + result.v02.score + result.v13.score + result.v24.score + result.v35.score;
  result.confidence = sum / 7.0;
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

  // v3.x (2026-07-09): tuning offset 不再加到 panorama 位置 X[i]/Y[i],
  //   改成加到 source 帧的 ROI 剪裁起点上 (见下面 rois[i].x/y += offset).
  //   与 2x3 路径语义对齐: 剪裁窗口在源图平移, 不动目标网格.

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

  // v3.x (2026-07-09): tuning offset 已删. yaml 里存的绝对 ROI 优先, valid=false 走
  //   上面的 crop_* 默认值. 视觉器调参直接改 g_config.camera_rois, 本函数下次调用生效.
  for (int i = 0; i < 4; ++i) {
    const CameraRoiRect& cfg = g_config.camera_rois[i];
    if (cfg.valid) {
      rois[i].x      = NormalizeEvenFloor(cfg.x);
      rois[i].y      = NormalizeEvenFloor(cfg.y);
      rois[i].width  = NormalizeEvenFloor(cfg.width);
      rois[i].height = NormalizeEvenFloor(cfg.height);
    }
  }

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

// v2.3: 6 路 2x3 (2 列 x 3 行) ROI 计算.
//   布局: cam0 cam1 / cam2 cam3 / cam4 cam5
//   简化: 不做复杂邻接 alignment, 使用 grid 平均分割 + tuning offset 微调.
//   overlap 区域是相邻 cam 的交叠区, BlendSeams 在此做羽化.
std::vector<CameraRoi> BuildCameraRois2x3(const std::vector<NV12Frame>& frames,
                                          const MatrixOverlap& overlaps,
                                          const std::vector<CameraTuning>& tuning) {
  if (frames.size() < 6) {
    // Fallback: 用 2x2 (前 4 路) 走老路径
    return {};
  }
  const int n = 6;
  int W[n], H[n];
  for (int i = 0; i < n; ++i) {
    W[i] = frames[i].width;
    H[i] = frames[i].height;
  }

  // 应用 tuning offset
  // 注意: tuning[] 只有 6 个才有效, BuildDefaultTuning 数组已扩到 [6]
  std::vector<CameraRoi> rois(n);

  // 简化策略: 每个 cam 满分辨率取 ROI, 不裁剪
  // (真正的 ROI 由 g_config.camera_rois[i] 在最后覆盖 — 下面有 v3.x 注释)
  for (int i = 0; i < n; ++i) {
    rois[i].x = 0;
    rois[i].y = 0;
    rois[i].width = W[i];
    rois[i].height = H[i];
  }

  // 用 overlap 区域的 shift_x / shift_y 做微调 (alignment hint)
  // 水平邻接 shift_y (cam0/2/4 是 cam 左, cam1/3/5 是右)
  rois[1].y = NormalizeEvenFloor(overlaps.h01.shift_y);
  rois[3].y = NormalizeEvenFloor(overlaps.h23.shift_y);
  rois[5].y = NormalizeEvenFloor(overlaps.h45.shift_y);
  // 垂直邻接 shift_y (cam0/1 是上, cam2/3 是中, cam4/5 是下)
  // v02/v13 shift_x -> cam2/cam3 的 x 偏移
  rois[2].x = NormalizeEvenFloor(overlaps.v02.shift_y);
  rois[3].x = NormalizeEvenFloor(overlaps.v13.shift_y);
  // v24/v35 shift_x -> cam4/cam5 的 x 偏移
  rois[4].x = NormalizeEvenFloor(overlaps.v24.shift_y);
  rois[5].x = NormalizeEvenFloor(overlaps.v35.shift_y);

  // v3.x (2026-07-09): 优先用 yaml 存的绝对 ROI (g_config.camera_rois[i]).
  //   valid=true 才覆盖, 否则保留上面的 overlap 兜底值. 后续 visualizer 调参
  //   也是直接改 g_config.camera_rois, RebuildLayout 重新调本函数即可生效.
  for (int i = 0; i < n; ++i) {
    const CameraRoiRect& cfg = g_config.camera_rois[i];
    if (cfg.valid) {
      rois[i].x      = NormalizeEvenFloor(cfg.x);
      rois[i].y      = NormalizeEvenFloor(cfg.y);
      rois[i].width  = NormalizeEvenFloor(cfg.width);
      rois[i].height = NormalizeEvenFloor(cfg.height);
    }
  }

  // 边界 clip (不超 cam 实际尺寸)
  for (int i = 0; i < n; ++i) {
    if (rois[i].x < 0) rois[i].x = 0;
    if (rois[i].y < 0) rois[i].y = 0;
    if (rois[i].x >= W[i]) rois[i].x = W[i] - 1;
    if (rois[i].y >= H[i]) rois[i].y = H[i] - 1;
    if (rois[i].x + rois[i].width > W[i]) rois[i].width = NormalizeEvenFloor(W[i] - rois[i].x);
    if (rois[i].y + rois[i].height > H[i]) rois[i].height = NormalizeEvenFloor(H[i] - rois[i].y);
    if (rois[i].width < 2 || rois[i].height < 2) {
      throw std::runtime_error("invalid 2x3 crop mapping for camera " + std::to_string(i));
    }
  }

  return rois;
}

// v2.3: 6 路 2x3 (2 列 x 3 行) 输出布局.
//   panorama 尺寸:
//     width  = 2*W - overlap_h (一列竖接缝)
//     height = 3*H - 2*overlap_v (两行横接缝)
//   邻接接缝:
//     水平: (0,1), (2,3), (4,5)
//     垂直: (0,2), (1,3), (2,4), (3,5)
std::vector<StitchTask> BuildStitchLayout2x3(const std::vector<CameraRoi>& rois,
                                             const std::vector<CameraTuning>& tuning,
                                             int* panorama_width,
                                             int* panorama_height) {
  if (rois.size() < 6) return {};
  const int n = 6;

  std::vector<StitchTask> tasks(n);
  for (size_t i = 0; i < n; ++i) {
    tasks[i].enabled = tuning[i].enabled;
    tasks[i].rotation_deg = tuning[i].rotation_deg;
    tasks[i].src_x = rois[i].x;
    tasks[i].src_y = rois[i].y;
    tasks[i].src_w = rois[i].width;
    tasks[i].src_h = rois[i].height;
  }

  // 简化的 overlap 值 (跟 ROI 简化策略一致, 不依赖 EstimatePairOverlap 结果)
  // 默认水平 overlap = W/8, 垂直 overlap = H/12 (粗略估计)
  const int W = rois[0].width;
  const int H = rois[0].height;
  const int overlap_h = W / 8;
  const int overlap_v = H / 12;

  // 2x3 grid 布局 (cam0/2/4 在左列, cam1/3/5 在右列)
  // row 0: cam0 (左上), cam1 (右上)
  // row 1: cam2 (左中), cam3 (右中)
  // row 2: cam4 (左下), cam5 (右下)
  const int blend_w = NormalizeEvenFloor(std::max(20, g_config.feather_width));

  // 左列 (cam0/2/4) x = 0
  // 右列 (cam1/3/5) x = W - overlap_h
  // 行 0: y = 0
  // 行 1: y = H - overlap_v
  // 行 2: y = 2*(H - overlap_v)

  // 应用 tuning 微调
  int col_x[2] = { 0, NormalizeEvenFloor(W - overlap_h) };
  int row_y[3] = { 0, NormalizeEvenFloor(H - overlap_v), NormalizeEvenFloor(2 * (H - overlap_v)) };

  // v3.x (2026-07-09): offset 已经在 BuildCameraRois2x3 加到 source ROI (rois[i].x/y) 上了,
  //   dst 只用 grid 位置, 不再加 tuning offset. 行为: 剪裁窗口在源图平移 → 剪下来的画面
  //   仍落在标准网格位置, 不会因 offset 把画面拖出 panorama.
  for (int i = 0; i < n; ++i) {
    int col = i % 2;   // 0 or 1
    int row = i / 2;   // 0, 1, or 2
    tasks[i].dst_x = col_x[col];
    tasks[i].dst_y = row_y[row];
  }

  // panorama 尺寸
  *panorama_width = NormalizeEvenCeil(col_x[1] + W);   // 右列起点 + cam 宽度
  *panorama_height = NormalizeEvenCeil(row_y[2] + H);  // 下行起点 + cam 高度

  Logger::GetInstance().Log("[App] [2x3] overlap_h=" + std::to_string(overlap_h)
                           + " overlap_v=" + std::to_string(overlap_v)
                           + " blend_w=" + std::to_string(blend_w));
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

// v3.x.1 (2026-07-09): 仿射合成. A * B 表示"先 B 后 A" (OpenCV 标准).
//   A = [a1 b1 tx1; c1 d1 ty1] (2x3 cv::Mat), B 同.
//   结果 = [a1*a2+b1*c2,  a1*b2+b1*d2,  a1*tx2+b1*ty2+tx1;
//           c1*a2+d1*c2,  c1*b2+d1*d2,  c1*tx2+d1*ty2+ty1]
// 任一为空 → 返回另一个的副本; 都为空 → 返回空 Mat.
cv::Mat ComposeAffines(const cv::Mat& a, const cv::Mat& b) {
  if (a.empty() && b.empty()) return cv::Mat();
  if (a.empty()) return b.clone();
  if (b.empty()) return a.clone();
  if (a.rows != 2 || a.cols != 3 || b.rows != 2 || b.cols != 3) {
    return cv::Mat();
  }
  cv::Mat out = cv::Mat::zeros(2, 3, CV_64F);
  const double a1 = a.at<double>(0,0), b1 = a.at<double>(0,1);
  const double c1 = a.at<double>(1,0), d1 = a.at<double>(1,1);
  const double tx1 = a.at<double>(0,2), ty1 = a.at<double>(1,2);
  const double a2 = b.at<double>(0,0), b2 = b.at<double>(0,1);
  const double c2 = b.at<double>(1,0), d2 = b.at<double>(1,1);
  const double tx2 = b.at<double>(0,2), ty2 = b.at<double>(1,2);
  out.at<double>(0,0) = a1*a2 + b1*c2;
  out.at<double>(0,1) = a1*b2 + b1*d2;
  out.at<double>(0,2) = a1*tx2 + b1*ty2 + tx1;
  out.at<double>(1,0) = c1*a2 + d1*c2;
  out.at<double>(1,1) = c1*b2 + d1*d2;
  out.at<double>(1,2) = c1*tx2 + d1*ty2 + ty1;
  return out;
}

// v3.x.1 (2026-07-09): 把 cv::Mat 2x3 affine 写到 CameraRoiRect::affine[6].
//   empty → 写单位阵 (向后兼容). 行主序 [a b c d tx ty].
void WriteAffineToRoi(CameraRoiRect& r, const cv::Mat& affine) {
  if (affine.empty() || affine.rows != 2 || affine.cols != 3) {
    r.affine[0] = 1.0; r.affine[1] = 0.0;
    r.affine[2] = 0.0; r.affine[3] = 1.0;
    r.affine[4] = 0.0; r.affine[5] = 0.0;
    return;
  }
  r.affine[0] = affine.at<double>(0, 0);
  r.affine[1] = affine.at<double>(0, 1);
  r.affine[2] = affine.at<double>(1, 0);
  r.affine[3] = affine.at<double>(1, 1);
  r.affine[4] = affine.at<double>(0, 2);
  r.affine[5] = affine.at<double>(1, 2);
}

// v3.x.1 (2026-07-09): 从 6 路 MatrixOverlap 推导每个 cam (i=1..5) 相对于 cam 0 的仿射.
//   用最直接的邻接路径 (不走合成), 因为垂直对已经被 EstimateVerticalOverlap 旋转回原图坐标:
//     cam1: h01 (直接 = cam1 -> cam0)
//     cam2: v02 (直接 = cam2 -> cam0)
//     cam3: h23 * v02 (cam3 -> cam2 -> cam0) 或 h01 * v13 (cam3 -> cam1 -> cam0), 二选一非空
//     cam4: v02 * v24 (cam4 -> cam2 -> cam0)
//     cam5: v02 * v24 * v35 (cam5 -> cam4 -> cam2 -> cam0) 或等价的路径
//   返回: per-cam cv::Mat (2x3). cam 0 永远是单位阵.
std::vector<cv::Mat> ComputePerCamAffine(const MatrixOverlap& m, int num_cams) {
  std::vector<cv::Mat> result(num_cams);
  if (num_cams <= 0) return result;
  // cam0 = identity
  result[0] = cv::Mat::eye(2, 3, CV_64F);
  if (num_cams == 1) return result;

  auto any_nonempty = [](std::initializer_list<cv::Mat> mats) -> cv::Mat {
    for (const auto& m : mats) {
      if (!m.empty() && m.rows == 2 && m.cols == 3) return m;
    }
    return cv::Mat();
  };

  if (num_cams >= 2) result[1] = m.h01.affine;  // cam1 -> cam0
  if (num_cams >= 3) result[2] = m.v02.affine;  // cam2 -> cam0 (EstimateVerticalOverlap 已旋转回原图)
  if (num_cams >= 4) {
    result[3] = any_nonempty({ ComposeAffines(m.v02.affine, m.h23.affine),
                               ComposeAffines(m.h01.affine, m.v13.affine) });
  }
  if (num_cams >= 5) {
    result[4] = any_nonempty({ ComposeAffines(m.v02.affine, m.v24.affine) });
  }
  if (num_cams >= 6) {
    result[5] = any_nonempty({
        ComposeAffines(ComposeAffines(m.v02.affine, m.v24.affine), m.h45.affine),
        ComposeAffines(ComposeAffines(m.v02.affine, m.v24.affine), m.v35.affine),
        ComposeAffines(ComposeAffines(m.h01.affine, m.v13.affine), m.h45.affine) });
  }
  return result;
}

// v3.x.1 (2026-07-09): 用 cv::warpAffine 把 bootstrap_bgr[i] 全部 warp 到 cam 0 参照帧.
//   warped[0] = bootstrap_bgr[0].clone() (cam 0 自己).
//   warped[i] = cv::warpAffine(bootstrap_bgr[i], affine_i, bootstrap_bgr[0].size()).
//   empty/失败 → fallback 到原图 (后续 EstimateOverlap 走原图路径).
std::vector<cv::Mat> PreWarpBootstrapFrames(const std::vector<cv::Mat>& bootstrap_bgr,
                                             const std::vector<cv::Mat>& per_cam_affine) {
  std::vector<cv::Mat> warped(bootstrap_bgr.size());
  if (bootstrap_bgr.empty()) return warped;
  const cv::Size ref_size = bootstrap_bgr[0].size();
  for (size_t i = 0; i < bootstrap_bgr.size(); ++i) {
    if (i >= per_cam_affine.size() || per_cam_affine[i].empty() ||
        per_cam_affine[i].rows != 2 || per_cam_affine[i].cols != 3) {
      warped[i] = bootstrap_bgr[i];
      continue;
    }
    // 2x3 cv::Mat 是 warpAffine 期望的格式, 但 cv::warpAffine 要的是 float. 转换一下.
    // 注意 WARP_INVERSE_MAP 标志: cv::warpAffine 默认把 M 当作 src->dst 映射
    //   (即 dst = M * src), 加了 WARP_INVERSE_MAP 才把 M 当作 inverse (dst->src).
    // 我们的 per_cam_affine 语义是 cam_i->cam_0, 直接把 cam_i 像素映射到 cam_0 像素,
    //   所以不要 WARP_INVERSE_MAP. (之前误加这个 flag 把图像往反方向拉, 难怪 bootstrap
    //   没起效.)
    cv::Mat M32f;
    per_cam_affine[i].convertTo(M32f, CV_32F);
    cv::warpAffine(bootstrap_bgr[i], warped[i], M32f, ref_size,
                   cv::INTER_LINEAR, cv::BORDER_CONSTANT,
                   cv::Scalar(0, 0, 0));
    if (warped[i].empty()) {
      warped[i] = bootstrap_bgr[i];
    }
  }
  return warped;
}

// v3.x.1 (2026-07-09): 把每路 cam 的 2x3 affine 转成 xmap/ymap (CV_32FC1) 给 GLES warper.
//   输出尺寸 = cam 的 panorama 足迹 (tasks[i].src_w x src[i].src_h).
//   xmap[py_local][px_local] = inv_affine 把它映射回源帧像素 (含 src_x/y offset 补偿).
//   ymap 同理.
//   identity/empty → 返回无效 entry (entry.valid()=false), SetWarpData 会跳过.
// v3.x.2 (2026-07-09): 如果该 cam 有 undist_xmap/undist_ymap (从 camchain_*.yaml 算出),
//   把畸变校正再合成一次进 xmap/ymap. final_src = undist(affine_inv(pano)).
//   GLES sampler 一次 lookup 同时完成畸变校正 + 仿射对齐, 完全 GPU, 不破 DMA-BUF.
StitchingWarpData BuildAffineWarpData(const std::vector<StitchTask>& tasks,
                                      const std::vector<CameraRoiRect>& rois,
                                      const std::vector<cv::Mat>& undist_xmap_vector,
                                      const std::vector<cv::Mat>& undist_ymap_vector,
                                      int panorama_w, int panorama_h) {
  StitchingWarpData wd;
  wd.panorama_size = cv::Size(panorama_w, panorama_h);
  wd.entries.resize(tasks.size());

  for (size_t i = 0; i < tasks.size(); ++i) {
    const CameraRoiRect& r = rois[i];
    if (!r.has_affine()) {
      wd.entries[i] = WarpMapEntry{};
      continue;
    }
    const StitchTask& t = tasks[i];
    const int dst_w = NormalizeEvenFloor(t.src_w);
    const int dst_h = NormalizeEvenFloor(t.src_h);
    if (dst_w < 2 || dst_h < 2 || !t.enabled) {
      wd.entries[i] = WarpMapEntry{};
      continue;
    }
    const double a = r.affine[0], b = r.affine[1];
    const double c = r.affine[2], d = r.affine[3];
    const double tx = r.affine[4], ty = r.affine[5];
    const double det = a*d - b*c;
    if (std::fabs(det) < 1e-6) {
      wd.entries[i] = WarpMapEntry{};
      continue;
    }
    const double inv_a = d / det;
    const double inv_b = -b / det;
    const double inv_c = -c / det;
    const double inv_d = a / det;

    // v3.x.2: 准备该 cam 的畸变校正 map (可能为空 = no-undistort)
    const bool have_undist = (i < undist_xmap_vector.size() &&
                              i < undist_ymap_vector.size() &&
                              !undist_xmap_vector[i].empty() &&
                              !undist_ymap_vector[i].empty() &&
                              undist_xmap_vector[i].type() == CV_32FC1 &&
                              undist_ymap_vector[i].type() == CV_32FC1);
    const cv::Mat& ux = have_undist ? undist_xmap_vector[i] : cv::Mat();
    const cv::Mat& uy = have_undist ? undist_ymap_vector[i] : cv::Mat();
    const int umap_w = have_undist ? ux.cols : 0;
    const int umap_h = have_undist ? ux.rows : 0;

    cv::Mat xmap(dst_h, dst_w, CV_32FC1);
    cv::Mat ymap(dst_h, dst_w, CV_32FC1);
    for (int py = 0; py < dst_h; ++py) {
      const float pano_y = static_cast<float>(t.dst_y + py);
      float* xrow = xmap.ptr<float>(py);
      float* yrow = ymap.ptr<float>(py);
      for (int px = 0; px < dst_w; ++px) {
        const float pano_x = static_cast<float>(t.dst_x + px);
        // step 1: 逆仿射 → 校正后源帧坐标 (corrected_src)
        const float csx_f = static_cast<float>(inv_a * (pano_x - tx) + inv_b * (pano_y - ty));
        const float csy_f = static_cast<float>(inv_c * (pano_x - tx) + inv_d * (pano_y - ty));
        if (!have_undist) {
          xrow[px] = csx_f;
          yrow[px] = csy_f;
          continue;
        }
        // step 2: 查 undist 表 → 原始 (畸变) 源帧坐标
        // cv::remap 风格: 浮点坐标在边界外用 BORDER_CONSTANT(0). 我们的 cv::initUndistortRectifyMap
        //   生成的是 CV_32FC1, 越界值可能为 -1 (默认) 或 0; 反正 GLES 采样越界会 clamp 到边.
        const int csx_i = static_cast<int>(csx_f);
        const int csy_i = static_cast<int>(csy_f);
        if (csx_i < 0 || csx_i >= umap_w || csy_i < 0 || csy_i >= umap_h) {
          xrow[px] = -1.0f;
          yrow[px] = -1.0f;
          continue;
        }
        xrow[px] = ux.at<float>(csy_i, csx_i);
        yrow[px] = uy.at<float>(csy_i, csx_i);
      }
    }
    WarpMapEntry entry;
    entry.xmap = xmap;
    entry.ymap = ymap;
    entry.roi = cv::Rect(t.dst_x, t.dst_y, dst_w, dst_h);
    wd.entries[i] = entry;
  }
  return wd;
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
    
    const MatrixOverlap overlaps = (num_img_ == 6)
        ? EstimateOverlaps2x3(bootstrap_bgr)
        : EstimateOverlaps2x2(bootstrap_bgr);
    // v3.x.1 (2026-07-09): 用 overlaps 里每对的 affine 推出每路 cam (相对于 cam0) 的仿射,
    //   然后 pre-warp 所有 cam 到 cam0 参照帧. warped_bgr 在 EstimateOverlaps2x3 已经把每对
    //   affine 算好, 这里把 warped 帧主要用于 ROI 计算 (参考帧坐标). BuildCameraRois2x3 函数
    //   只用 frames 取 width/height, image_vector_ 的尺寸与 warped_bgr 一致, 这里沿用
    //   image_vector_; 真正的 ROI 内容会被 g_config.camera_rois[i] (yaml) 覆盖.
    std::vector<cv::Mat> per_cam_affine =
        ComputePerCamAffine(overlaps, static_cast<int>(num_img_));
    std::vector<cv::Mat> warped_bgr =
        PreWarpBootstrapFrames(bootstrap_bgr, per_cam_affine);
    (void)warped_bgr;  // 留作日志 / 后续扩展; 当前 BuildCameraRois2x3 用 image_vector_ 取尺寸
    const vector<CameraRoi> rois = (num_img_ == 6)
        ? BuildCameraRois2x3(image_vector_, overlaps, tuning)
        : BuildCameraRois2x2(image_vector_, overlaps, tuning);
    
    double avg_confidence = overlaps.confidence;
    
    bool all_valid;
    if (num_img_ == 6) {
      all_valid = (overlaps.h01.score >= CONFIDENCE_THRESHOLD &&
                   overlaps.h23.score >= CONFIDENCE_THRESHOLD &&
                   overlaps.h45.score >= CONFIDENCE_THRESHOLD &&
                   overlaps.v02.score >= CONFIDENCE_THRESHOLD &&
                   overlaps.v13.score >= CONFIDENCE_THRESHOLD &&
                   overlaps.v24.score >= CONFIDENCE_THRESHOLD &&
                   overlaps.v35.score >= CONFIDENCE_THRESHOLD);
    } else {
      all_valid = (overlaps.h01.score >= CONFIDENCE_THRESHOLD &&
                   overlaps.h23.score >= CONFIDENCE_THRESHOLD &&
                   overlaps.v02.score >= CONFIDENCE_THRESHOLD &&
                   overlaps.v13.score >= CONFIDENCE_THRESHOLD);
    }

    int dummy_panorama_width = 0;
    int dummy_panorama_height = 0;
    const vector<StitchTask> layout =
        (num_img_ == 6)
        ? BuildStitchLayout2x3(rois, tuning, &dummy_panorama_width, &dummy_panorama_height)
        : BuildStitchLayout2x2(rois, tuning, &dummy_panorama_width, &dummy_panorama_height);

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

      debug_msg << " h01=" << std::fixed << std::setprecision(3) << overlaps.h01.score;
      debug_msg << " h23=" << std::fixed << std::setprecision(3) << overlaps.h23.score;
      if (num_img_ == 6) {
        debug_msg << " h45=" << std::fixed << std::setprecision(3) << overlaps.h45.score;
      }
      debug_msg << " v02=" << std::fixed << std::setprecision(3) << overlaps.v02.score;
      debug_msg << " v13=" << std::fixed << std::setprecision(3) << overlaps.v13.score;
      if (num_img_ == 6) {
        debug_msg << " v24=" << std::fixed << std::setprecision(3) << overlaps.v24.score;
        debug_msg << " v35=" << std::fixed << std::setprecision(3) << overlaps.v35.score;
      }
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

  // v3.x (2026-07-09): bootstrap 成功识别后, 把 detected rois 写到 g_config.camera_rois
  //   (valid=true), 下次 SaveToFile 就会把绝对 ROI 落到 yaml, 后续运行直接读取不重跑 bootstrap.
  // v3.x.1 (2026-07-09): 同步保存 affine (cam 0 = 单位阵, 其余从 overlaps 推出, 见 ComputePerCamAffine).
  //   注意: 这里用的是最后一次循环 (best_result 那一帧) 的 overlaps, 不是 detection_results[best_result_idx].
  //   检测循环已经算好了每对的 affine, 我们这里用同样的 overlaps 推出 per-cam.
  const std::vector<cv::Mat> best_per_cam_affine =
      ComputePerCamAffine(overlaps, static_cast<int>(num_img_));
  for (size_t i = 0; i < num_img_ && i < rois.size(); ++i) {
    g_config.camera_rois[i].x      = rois[i].x;
    g_config.camera_rois[i].y      = rois[i].y;
    g_config.camera_rois[i].width  = rois[i].width;
    g_config.camera_rois[i].height = rois[i].height;
    g_config.camera_rois[i].valid  = true;
    WriteAffineToRoi(g_config.camera_rois[i],
                     i < best_per_cam_affine.size() ? best_per_cam_affine[i] : cv::Mat());
  }
  
  if (g_multi_frame_roi_debug_level >= 1) {
    ostringstream summary_msg;
    summary_msg << "[App] [MULTI-FRAME ROI " << (num_img_ == 6 ? "2x3" : "2x2") << "] Detection completed: "
                << "total_frames=" << detection_results.size()
                << " best_frame=" << best_result.frame_index
                << " best_confidence=" << std::fixed << std::setprecision(4) << max_confidence;
    Logger::GetInstance().Log(summary_msg.str());
  }

  int dummy_panorama_width = 0;
  int dummy_panorama_height = 0;
  if (num_img_ == 6) {
    BuildStitchLayout2x3(rois, tuning, &dummy_panorama_width, &dummy_panorama_height);
  } else {
    BuildStitchLayout2x2(rois, tuning, &dummy_panorama_width, &dummy_panorama_height);
  }
  total_cols_ = dummy_panorama_width;
  height_ = dummy_panorama_height;

  int blend_width = NormalizeEvenFloor(std::max(20, g_config.feather_width));
  image_stitcher_.SetParams(blend_width, static_cast<int>(num_img_), total_cols_, height_);
  image_stitcher_.SetLayout(layout);

  // v3.x.1 (2026-07-09): 把每路 cam 的 affine 喂给 GLES warper (走 SetWarpData).
  //   cam 0 是单位阵 → entry 不写; 其余 cam 用 xmap/ymap 把源帧 warp 到 warped 坐标系.
  //   InitFromConfig / RebuildLayout 也调相同的 BuildAffineWarpData + SetWarpData 路径,
  //   确保 yaml reload 后新 affine 生效.
  if (!image_vector_.empty() && image_vector_[0].width > 0) {
    // v3.x.2 (2026-07-09): 喂入畸变校正 map (水平 cam pair), 让 GLES warper 一次 GPU
    //   pass 同时完成畸变校正 + 仿射对齐. cam0 是单位阵, entry 为空, 自动 skip.
    StitchingWarpData wd = BuildAffineWarpData(layout, g_config.camera_rois,
                                               undist_xmap_vector_, undist_ymap_vector_,
                                               total_cols_, height_);
    image_stitcher_.SetWarpData(wd, image_vector_[0].width, image_vector_[0].height);
  }

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


bool App::InitRtspOutput() {
#if HAVE_GST_RTSP_SERVER
    // 读 yaml 顶层 `output:` 块.
    rtsp_cfg_ = output_streams::LoadFromYaml("../params/camera_sources.yaml");
    if (!rtsp_cfg_.enabled) {
        Logger::GetInstance().Log(
            "[App] output.enabled=false in yaml, RTSP output disabled");
        return false;
    }

    // 构造 FrameDiff 配置.
    frame_diff::DiffConfig fd_cfg;
    fd_cfg.threshold = rtsp_cfg_.diff_threshold;
    fd_cfg.bbox_min_area = rtsp_cfg_.diff_bbox_min_area;
    frame_diff_.reset(new frame_diff::FrameDiff(fd_cfg));
    Logger::GetInstance().Log(
        "[App] FrameDiff ready: threshold=" + std::to_string(fd_cfg.threshold) +
        " bbox_min=" + std::to_string(fd_cfg.bbox_min_area));

    // 启 GST RTSP server.
    auto& srv = gst_rtsp_server::GstRtspServer::GetInstance();
    if (!srv.Start(rtsp_cfg_, rtsp_cfg_.streams)) {
        Logger::GetInstance().LogError(
            "[App] gst_rtsp_server Start failed, RTSP output disabled");
        frame_diff_.reset();
        return false;
    }
    Logger::GetInstance().Log(
        "[App] RTSP output enabled on port " + std::to_string(rtsp_cfg_.port) +
        " streams=" + std::to_string(rtsp_cfg_.streams.size()));
    return true;
#endif  // HAVE_GST_RTSP_SERVER

  // HAVE_GST_RTSP_SERVER=0: RTSP 没编译进来, 默认关.
  return false;
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
  cached_overlaps_ = MatrixOverlapToCached(
      (num_img_ == 6) ? EstimateOverlaps2x3(bootstrap_bgr) : EstimateOverlaps2x2(bootstrap_bgr));

  const MatrixOverlap overlaps = CachedToMatrixOverlap(cached_overlaps_);
  const vector<CameraRoi> rois = (num_img_ == 6)
      ? BuildCameraRois2x3(image_vector_, overlaps, tuning)
      : BuildCameraRois2x2(image_vector_, overlaps, tuning);
  int w = 0, h = 0;
  const vector<StitchTask> layout = (num_img_ == 6)
      ? BuildStitchLayout2x3(rois, tuning, &w, &h)
      : BuildStitchLayout2x2(rois, tuning, &w, &h);

  total_cols_ = w;
  height_ = h;

  int blend_width = NormalizeEvenFloor(std::max(20, g_config.feather_width));
  image_stitcher_.SetParams(blend_width, static_cast<int>(num_img_), total_cols_, height_);
  image_stitcher_.SetLayout(layout);

  // v3.x.1 (2026-07-09): 启动期 yaml 加载路径也要喂 affine 给 GLES warper.
  //   没有这一段 yaml 里的 affine 不会生效 (因为 reload 走 RebuildLayout, 首次加载走 InitFromConfig).
  if (!image_vector_.empty() && image_vector_[0].width > 0) {
    // v3.x.2 (2026-07-09): 喂入畸变校正 map (水平 cam pair), 让 GLES warper 一次 GPU
    //   pass 同时完成畸变校正 + 仿射对齐. cam0 是单位阵, entry 为空, 自动 skip.
    StitchingWarpData wd = BuildAffineWarpData(layout, g_config.camera_rois,
                                               undist_xmap_vector_, undist_ymap_vector_,
                                               total_cols_, height_);
    image_stitcher_.SetWarpData(wd, image_vector_[0].width, image_vector_[0].height);
  }

  if (g_multi_frame_roi_debug_level >= 1) {
    ostringstream msg;
    msg << "[App] [CONFIG INIT " << (num_img_ == 6 ? "2x3" : "2x2") << "] Panorama size: "
        << total_cols_ << "x" << height_;
    Logger::GetInstance().Log(msg.str());
  }
}

void App::RebuildLayout() {
  const vector<CameraTuning> tuning = BuildDefaultTuning(num_img_);

  const MatrixOverlap overlaps = CachedToMatrixOverlap(cached_overlaps_);
  const vector<CameraRoi> rois = (num_img_ == 6)
      ? BuildCameraRois2x3(frames_locked_ ? saved_frames_ : image_vector_, overlaps, tuning)
      : BuildCameraRois2x2(frames_locked_ ? saved_frames_ : image_vector_, overlaps, tuning);
  int new_w = 0, new_h = 0;
  const vector<StitchTask> layout = (num_img_ == 6)
      ? BuildStitchLayout2x3(rois, tuning, &new_w, &new_h)
      : BuildStitchLayout2x2(rois, tuning, &new_w, &new_h);

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

  // v3.x.1 (2026-07-09): yaml reload 路径 (RoiYamlWatcher 触发) 也要重喂 affine.
  //   用最新的 image_vector_ 尺寸作为 GLES warper input 维度.
  if (!image_vector_.empty() && image_vector_[0].width > 0) {
    // v3.x.2 (2026-07-09): 喂入畸变校正 map (水平 cam pair), 让 GLES warper 一次 GPU
    //   pass 同时完成畸变校正 + 仿射对齐. cam0 是单位阵, entry 为空, 自动 skip.
    StitchingWarpData wd = BuildAffineWarpData(layout, g_config.camera_rois,
                                               undist_xmap_vector_, undist_ymap_vector_,
                                               total_cols_, height_);
    image_stitcher_.SetWarpData(wd, image_vector_[0].width, image_vector_[0].height);
  }
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

  const MatrixOverlap overlaps = CachedToMatrixOverlap(cached_overlaps_);
  const vector<CameraRoi> rois = (num_img_ == 6)
      ? BuildCameraRois2x3(saved_frames_, overlaps, tuning)
      : BuildCameraRois2x2(saved_frames_, overlaps, tuning);
  int new_w = 0, new_h = 0;
  const vector<StitchTask> layout = (num_img_ == 6)
      ? BuildStitchLayout2x3(rois, tuning, &new_w, &new_h)
      : BuildStitchLayout2x2(rois, tuning, &new_w, &new_h);
  
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

  // 阶段 2: MJPEG 推流参数 (CameraPage "实时预览" 用). MJPEG_INTERVAL=1 全速 30 FPS,
  // MJPEG_INTERVAL=2 默认 15 FPS, MJPEG_INTERVAL=3 10 FPS (低功耗).
  {
    const char* env_int = getenv("MJPEG_INTERVAL");
    if (env_int && atoi(env_int) > 0) mjpeg_interval_ = std::max(1, atoi(env_int));
    const char* env_w = getenv("MJPEG_DOWNSCALE_W");
    if (env_w && atoi(env_w) >= 64) mjpeg_width_ = NormalizeEvenFloor(atoi(env_w));
    const char* env_h = getenv("MJPEG_DOWNSCALE_H");
    if (env_h && atoi(env_h) >= 64) mjpeg_height_ = NormalizeEvenFloor(atoi(env_h));
    const char* env_q = getenv("MJPEG_QUALITY");
    if (env_q) {
      int q = atoi(env_q);
      mjpeg_quality_ = q < 10 ? 10 : (q > 95 ? 95 : q);
    }
  }

  // v2.3 阶段 1: 初始化 status writer (CameraPage 后端用, 独立线程 + 模拟数据)
  stitch_status::init();
  // 阶段 2: 初始化 MJPEG producer/consumer 全局缓冲 (condvar + 1 slot 覆盖)
  mjpeg_streamer::init();

  sensorDataInterface_.InitVideoCapture(num_img_);
  // v3.2 (2026-07-09): 启动 RTSP 推流 (yaml output.enabled=true). 在 stitch 之前 init, 让 pipeline ready.
  rtsp_output_enabled_ = InitRtspOutput();

  // v3.x.2 (2026-07-09): 6 路 cam 全做畸变校正 (2026-07-09 改: 之前限定水平 cam pair,
  //   用户确认 2x3 6 路 cam 都做, 包括 cam0/cam2/cam4 参照帧). 启动期读 params/camchain_<i>.yaml,
  //   算 initUndistortRectifyMap 出 CV_32FC1 的 xmap/ymap. 任何 cam 缺 yaml / 字段不齐 →
  //   该 cam 留空, GLES warper 自动 skip, 不影响其他 cam.
  //   注: 这意味着 cam0 (单位阵 + 有 undist map) 现在会先做畸变校正, 再被 GLES warper 视为
  //   "xmap 是 inverse affine" 处理. 因为 cam0 的 affine 是单位阵, inv 是单位阵, 等价于
  //   "只做畸变校正"; 这是期望行为.
  {
    undist_xmap_vector_.assign(num_img_, cv::Mat());
    undist_ymap_vector_.assign(num_img_, cv::Mat());
    int loaded = 0;
    for (size_t i = 0; i < num_img_; ++i) {
      const std::string path = std::string("../params/camchain_") + std::to_string(i) + ".yaml";
      camera_intrinsics::CamchainIntrinsics ci;
      if (!camera_intrinsics::LoadCamchain(path, &ci)) {
        Logger::GetInstance().Log(
            "[App] [UNDISTORT] cam" + std::to_string(i) + ": " + path +
            " missing or invalid, skip (will not undistort this cam)");
        continue;
      }
      // live 帧分辨率在 image_vector_ 还没填时拿不到. 用 yaml 里的 calib 尺寸 + 设备常见
      //   2560x1440 兜底 (camera_sources.yaml 默认). 实际 stitch 时 BuildAffineWarpData
      //   检查 map 尺寸, 不一致时会被 cv::remap 的边界裁剪兜住.
      const int live_w = 2560;
      const int live_h = 1440;
      cv::Mat xmap, ymap;
      if (!camera_intrinsics::BuildUndistortMap(ci, live_w, live_h, &xmap, &ymap)) {
        Logger::GetInstance().LogError(
            "[App] [UNDISTORT] cam" + std::to_string(i) +
            ": initUndistortRectifyMap failed, skip");
        continue;
      }
      undist_xmap_vector_[i] = xmap;
      undist_ymap_vector_[i] = ymap;
      ++loaded;
      Logger::GetInstance().Log(
          "[App] [UNDISTORT] cam" + std::to_string(i) + ": " + path +
          " loaded (calib " + std::to_string(ci.calib_w) + "x" +
          std::to_string(ci.calib_h) + " -> live " + std::to_string(live_w) + "x" +
          std::to_string(live_h) + ")");
    }
    Logger::GetInstance().Log(
        "[App] [UNDISTORT] total loaded: " + std::to_string(loaded) + "/" +
        std::to_string(num_img_) + " cams (2x3 grid, all undistorted if yaml present).");
  }

  image_vector_.resize(num_img_);

  bool config_loaded = false;
  if (g_use_roi_config) {
    config_loaded = RoiConfig::LoadFromFile("../params/roi_tuning.yaml", g_config);
  }

  if (g_skip_bootstrap) {
    // 固定支架场景 (车载 GC4683 等): 期望用 YAML 启动, 但 YAML 缺失时仍跑一次
    // bootstrap 作为兜底, 而不是直接报错退出. 这是 2026-06 与用户确认的语义.
    if (config_loaded) {
      Logger::GetInstance().Log(
          "[App] SKIP_BOOTSTRAP=1 + YAML OK, using params/roi_tuning.yaml (no bootstrap)");
    } else {
      Logger::GetInstance().Log(
          "[App] SKIP_BOOTSTRAP=1 but YAML missing/invalid, falling back to bootstrap "
          "(下次启动前请确认 params/roi_tuning.yaml)");
    }
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
    // v3.0 (2026-07-08) BUG FIX: was hardcoded 2x2 → 6-cam 触发越界; 用 num_img_ 路由
    cached_overlaps_ = MatrixOverlapToCached(
        (num_img_ == 6) ? EstimateOverlaps2x3(bootstrap_bgr) : EstimateOverlaps2x2(bootstrap_bgr));

    try {
      RoiConfig::SaveToFile("../params/roi_tuning.yaml", g_config);
    } catch (const cv::Exception& e) {
      Logger::GetInstance().LogError(std::string("[App] SaveToFile cv::Exception (bootstrap) tolerated: ") + e.what());
    } catch (const std::exception& e) {
      Logger::GetInstance().LogError(std::string("[App] SaveToFile std::exception (bootstrap) tolerated: ") + e.what());
    }
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

  // 阶段 2: 分配 MJPEG 降采样目标 DMA-BUF (小, 一次性, 全程复用).
  if (drm_alloc_nv12(mjpeg_width_, mjpeg_height_, mjpeg_drm_buf_) != 0) {
    Logger::GetInstance().LogError("[App] mjpeg_drm_buf_ alloc failed, MJPEG stream disabled");
  } else {
    Logger::GetInstance().Log(string("[App] MJPEG stream enabled: ") +
        std::to_string(mjpeg_width_) + "x" + std::to_string(mjpeg_height_) +
        " interval=" + std::to_string(mjpeg_interval_) +
        " quality=" + std::to_string(mjpeg_quality_));
  }

  if (visual_mode_) {
    Logger::GetInstance().Log("[App] Initializing visualizer with panorama size: " +
                              std::to_string(total_cols_) + "x" + std::to_string(height_) +
                              " num_cams=" + std::to_string(num_img_));
    // v3.x (2026-07-09): 把 num_img_ (4 或 6) 传给 visualizer, 决定画几个 cam 框 +
    //   Tab 切换的范围. 默认 6 兼容老调用方.
    if (!RoiVisualizer::Init(total_cols_, height_, static_cast<int>(num_img_))) {
      Logger::GetInstance().Log("[App] OpenCV highgui init failed, disabling visual mode");
      Logger::GetInstance().Log("[App] Check if DISPLAY environment variable is set correctly");
      visual_mode_ = false;
    } else {
      Logger::GetInstance().Log("[App] Visualizer initialized successfully");
    }
  }

  // v3.x (2026-07-09): 启动 yaml watcher. 必须在 SaveToFile / InitFromConfig 之后启动,
  //   这样 watcher 记下的基线 mtime 就是当前最新的 yaml 状态, 不会把启动后立刻写入的
  //   yaml 当成"外部修改"误触发 reload. yaml 不存在时 Start() 静默失败, 主循环也不 reload.
  yaml_path_ = "../params/roi_tuning.yaml";
  if (g_use_roi_config) {
    roi_yaml_watcher_.reset(new RoiYamlWatcher(yaml_path_));
    roi_yaml_watcher_->Start();
  }
}

App::~App() {
  // v3.x (2026-07-09): 先停 yaml watcher, 避免它和 main thread 抢着 reload.
  if (roi_yaml_watcher_) {
    roi_yaml_watcher_->Stop();
    roi_yaml_watcher_.reset();
  }
  ReleaseSavedFrames();
  if (visual_mode_) RoiVisualizer::Shutdown();
  mjpeg_streamer::shutdown();   // 阶段 2: 唤醒 wait 的 consumer, 清缓冲
  drm_free(mjpeg_drm_buf_);     // 阶段 2
  drm_free(output_drm_buf_);
  stitch_status::shutdown();  // v2.3 阶段 1: 清理 status 文件

  // v3.2: 关 RTSP server (detach glib main loop, 停 pump 线程).
  gst_rtsp_server::GstRtspServer::GetInstance().Stop();
  frame_diff_.reset();
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

    // v3.2 (2026-07-09): 帧差掩码 + 2 路 RTSP 推流. (yaml output.enabled=true)
    if (rtsp_output_enabled_ && !debug_mode_) {
      // ExportNv12DrmBufferToBgr returns cv::UMat (OpenCL); convert to Mat for RTSP push.
      cv::UMat stitched_umat_for_rtsp = ExportNv12DrmBufferToBgr(output_drm_buf_);
      if (!stitched_umat_for_rtsp.empty()) {
        cv::Mat stitched_bgr_for_rtsp = stitched_umat_for_rtsp.getMat(cv::ACCESS_READ);
        gst_rtsp_server::GstRtspServer::GetInstance().PushBgrFrame(
            "/stitch", stitched_bgr_for_rtsp);
        if (frame_diff_) {
          // frame_diff needs NV12Frame (fd + width + height + stride).
          // Build a NV12Frame view from output_drm_buf_ without copying.
          NV12Frame nv12_view;
          nv12_view.fd = output_drm_buf_.fd;
          nv12_view.width = output_drm_buf_.width;
          nv12_view.height = output_drm_buf_.height;
          nv12_view.stride_w = static_cast<int>(output_drm_buf_.pitch);
          nv12_view.stride_h = output_drm_buf_.height;
          cv::Mat mask_bgr = frame_diff_->ComputeMask(nv12_view);
          if (!mask_bgr.empty()) {
            gst_rtsp_server::GstRtspServer::GetInstance().PushBgrFrame(
                "/stitch_diff", mask_bgr);
          }
        }
      }
    }

    }

    // 阶段 2: CameraPage "实时预览" MJPEG 推流.
    // gate: 每 mjpeg_interval_ 帧做一次降采样 + JPEG 编码, 推到 mjpeg_streamer 全局缓冲.
    // 主线程做 imencode (~5-15ms @ 960x816 on RK3576 ARM). interval_=2 (15 FPS 默认)
    // 留 ~16ms 给其他帧做 stitch, 不挤占 30 FPS 主预算.
    // 注: 走 RGA NV12→NV12 降采样 (mmuFlag=1, 与现有 stitch RGA 同一 channel),
    // 然后 drm_map + cv::cvtColor(NV12→BGR) + cv::imencode(".jpg").
    if (!debug_mode_ &&
        mjpeg_drm_buf_.fd >= 0 &&
        (frame_idx % static_cast<size_t>(mjpeg_interval_)) == 0) {
      rga_info_t src_info;
      memset(&src_info, 0, sizeof(src_info));
      src_info.fd = output_drm_buf_.fd;
      src_info.mmuFlag = 1;
      // 源 rect 用 panorama 实际尺寸 (运行时动态拿, 不硬编码 4800x4080)
      const int src_w = output_drm_buf_.width;
      const int src_h = output_drm_buf_.height;
      const int src_stride_w = static_cast<int>(output_drm_buf_.pitch);
      const int src_stride_h = src_h;
      rga_set_rect(&src_info.rect, 0, 0, src_w, src_h, src_stride_w, src_stride_h,
                   RK_FORMAT_YCbCr_420_SP);

      rga_info_t dst_info;
      memset(&dst_info, 0, sizeof(dst_info));
      dst_info.fd = mjpeg_drm_buf_.fd;
      dst_info.mmuFlag = 1;
      // 目标 rect 用 mjpeg_width_/height_ (运行时动态, 默认 960x816)
      const int dst_stride_w = static_cast<int>(mjpeg_drm_buf_.pitch);
      const int dst_stride_h = mjpeg_height_;
      rga_set_rect(&dst_info.rect, 0, 0, mjpeg_width_, mjpeg_height_,
                   dst_stride_w, dst_stride_h, RK_FORMAT_YCbCr_420_SP);

      // RGA 自动 stretch (源 4800x4080 -> 目标 960x816), 在 mmuFlag=1 下 DMA-BUF 间硬跳.
      if (c_RkRgaBlit(&src_info, &dst_info, nullptr) == 0) {
        void* mapped = drm_map(mjpeg_drm_buf_);
        if (mapped != MAP_FAILED) {
          const int y_rows = mjpeg_height_ * 3 / 2;
          cv::Mat nv12_host(y_rows, mjpeg_width_, CV_8UC1);
          const uint8_t* src_y = static_cast<const uint8_t*>(mapped);
          const uint8_t* src_uv = src_y + static_cast<size_t>(dst_stride_w) * mjpeg_height_;
          CopyNv12DataToMat(
              src_y, src_uv,
              mjpeg_width_, mjpeg_height_,
              dst_stride_w, dst_stride_w,
              nv12_host);

          cv::Mat bgr_host;
          cv::cvtColor(nv12_host, bgr_host, cv::COLOR_YUV2BGR_NV12);

          std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, mjpeg_quality_};
          std::vector<unsigned char> jpg_buf;
          // drop frame 后 pjpeg bytestream 才 push, 避免 imencode 走完整 timeline
          if (cv::imencode(".jpg", bgr_host, jpg_buf, params) && !jpg_buf.empty()) {
            mjpeg_streamer::update(jpg_buf);
          }
          drm_unmap(mjpeg_drm_buf_, mapped);
        }
      }
    }

    // v3.x (2026-07-09): 实时 yaml reload. watcher 后台线程检测到 mtime 跳变后会
    //   set pending flag, 这里 Consume 后 reload g_config + RebuildLayout.
    //   跑在主线程: 单写者, 不需要锁 g_config. debug_mode_ 不处理 (用 saved frame 调参,
    //   不应被外部 yaml 修改抢断).
    if (roi_yaml_watcher_ && roi_yaml_watcher_->ShouldReload() && !debug_mode_) {
      roi_yaml_watcher_->Consume();
      StitchGlobalConfig new_cfg;
      if (RoiConfig::LoadFromFile(yaml_path_, new_cfg)) {
        g_config = new_cfg;
        try {
          RebuildLayout();
          Logger::GetInstance().Log(
              "[App] YAML reloaded + layout rebuilt (cam0 roi=" +
              std::to_string(g_config.camera_rois[0].x) + "," +
              std::to_string(g_config.camera_rois[0].y) + " " +
              std::to_string(g_config.camera_rois[0].width) + "x" +
              std::to_string(g_config.camera_rois[0].height) + ")");
        } catch (const std::exception& e) {
          Logger::GetInstance().LogError(
              std::string("[App] YAML reload RebuildLayout failed: ") + e.what());
        }
      } else {
        Logger::GetInstance().LogError(
            "[App] YAML reload failed (parse error?), keeping previous config");
      }
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
        
        if (action == kVisRefreshFrames) {
          // v3.x (2026-07-09): 临时解锁, 拉一帧最新的 (六个 decoder 都到齐为止),
          //   再重新拷到 saved_drm_bufs_. 之后 frames_locked_ 由 SaveCurrentFrames 内部
          //   设回 true, 行为跟初次进 debug 一致.
          frames_locked_ = false;
          sensorDataInterface_.get_image_vector(image_vector_);
          SaveCurrentFrames();
          RestitchSavedFrames();
          Logger::GetInstance().Log("[App] [DEBUG] refreshed saved frames from live");
        }
        if (action == kVisSaveConfig) {
          try {
      RoiConfig::SaveToFile("../params/roi_tuning.yaml", g_config);
    } catch (const cv::Exception& e) {
      Logger::GetInstance().LogError(std::string("[App] SaveToFile cv::Exception (bootstrap) tolerated: ") + e.what());
    } catch (const std::exception& e) {
      Logger::GetInstance().LogError(std::string("[App] SaveToFile std::exception (bootstrap) tolerated: ") + e.what());
    }
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

    // v2.3 阶段 1: 写 status 给 CameraPage 后端读
    // 每 kWriteEveryNFrames 帧写一次, 内部已 debounce
    {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      stitch_status::GlobalStatus stitch_gstatus;
      memset(&stitch_gstatus, 0, sizeof(stitch_gstatus));
      stitch_gstatus.num_cameras = static_cast<int>(num_img_);

      // mode 计算: 与 sensor_data_interface::InitVideoCapture 同步 — 显式 env 优先,
      // 否则 yaml 里有 rtsp 就当 camera 模式, 否则 dataset. 不要单独看 env, 否则 yaml
      // 自动切到 camera 时 mode 还是 0, CameraPage 会以为是 dataset.
      const char* env_mode = getenv("INPUT_SOURCE_MODE");
      const CameraSourceList& src_list = GetCameraSourceList();
      int computed_mode = 0;  // 0 = dataset, 1 = camera
      if (env_mode != nullptr) {
        if (strcmp(env_mode, "camera") == 0) computed_mode = 1;
      } else {
        for (const auto& src : src_list.cameras) {
          if (src.is_rtsp()) { computed_mode = 1; break; }
        }
      }
      stitch_gstatus.mode = computed_mode;

      stitch_gstatus.panorama_w = total_cols_;
      stitch_gstatus.panorama_h = height_;
      stitch_gstatus.current_fps = fps;
      stitch_gstatus.frame_idx = static_cast<int64_t>(frame_idx);
      stitch_gstatus.timestamp_us = static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
      stitch_gstatus.blend_ms = 0;  // TODO: 实测
      stitch_gstatus.warp_ms = 0;   // TODO: 实测
      const vector<double> decode_fps_vec = sensorDataInterface_.GetDecodeFpsSnapshot();
      for (size_t i = 0; i < num_img_ && i < 6; ++i) {
        stitch_status::CameraStatus& c = stitch_gstatus.cams[i];
        c.online = (image_vector_[i].width > 0) ? 1 : 0;
        c.fps = (i < decode_fps_vec.size()) ? static_cast<int>(decode_fps_vec[i]) : 0;
        c.width = image_vector_[i].width;
        c.height = image_vector_[i].height;
        // v1 测试: 用 cam0/cam1 占位, 后续通过 public getter 拿 CameraSourceList 的 uri
        snprintf(c.name, sizeof(c.name), "cam%zu", i);
        // uri: 优先从 yaml 拿真值, 别再硬编码 mp4 路径. yaml 缺/越界才退回占位.
        if (i < src_list.cameras.size() && !src_list.cameras[i].uri.empty()) {
          std::string uri = src_list.cameras[i].uri;
          if (uri.size() >= sizeof(c.uri)) uri.resize(sizeof(c.uri) - 1);
          snprintf(c.uri, sizeof(c.uri), "%s", uri.c_str());
        } else {
          snprintf(c.uri, sizeof(c.uri), "../datasets/2k-test/cam%zu.mp4", i);
        }
      }
      stitch_status::update(stitch_gstatus);
    }
  }
}

int main() {
  // v2.3 阶段 1.5: status writer 线程 (独立, 模拟数据)
  // 必须在最开头: 即使 App 构造卡死 (vpu/kmpp 问题), worker 仍能写
  stitch_status::init();

  const char* env_visual = getenv("ENABLE_VISUAL_TUNING");
  g_enable_visual_tuning = (env_visual == nullptr || atoi(env_visual) != 0);

  const char* env_markers = getenv("SHOW_ROI_MARKERS");
  g_show_roi_markers = (env_markers == nullptr || atoi(env_markers) != 0);

  const char* env_config = getenv("USE_ROI_CONFIG");
  g_use_roi_config = (env_config == nullptr || atoi(env_config) != 0);

  const char* env_skip = getenv("SKIP_BOOTSTRAP");
  g_skip_bootstrap = (env_skip != nullptr && atoi(env_skip) != 0);

  // v2.3: CameraPage HTTP 后端 (嵌入式, 单进程)
  // 默认开, 设 STITCH_HTTP=0 关闭 (调试时方便)
  const char* env_http = getenv("STITCH_HTTP");
  bool enable_http = (env_http == nullptr) || (atoi(env_http) != 0);
  if (enable_http) {
    http_server::Config hcfg;
    hcfg.port = 8080;
    const char* env_port = getenv("STITCH_HTTP_PORT");
    if (env_port) hcfg.port = atoi(env_port);
    hcfg.camera_page_dir = "../CameraPage";  // 相对 CWD
    http_server::start(hcfg);
  } else {
    Logger::GetInstance().Log("[main] HTTP server disabled (STITCH_HTTP=0)");
  }

  App app;
  app.run_stitching();
}