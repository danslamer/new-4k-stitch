/**
 * @file app.cc
 * @brief 视频拼接应用主文件
 *
 * 此文件实现了视频拼接应用的核心逻辑，包括初始化、ROI检测、拼接布局构建和运行循环。
 * 使用ffmpeg库里的rkmpp解码器硬件解码和OpenCV进行实时视频拼接，RGA用于剪裁拼接处理。
 */

#include "app.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
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


bool g_save_stitched_frames = true;
size_t g_save_frame_interval = 30;

// 新增的离线文件调试支持 (全局变量打点)
bool g_debug_opencl_feathering = true;

// ============================================================================
// 羽化(Feathering) 融合参数配置
// ============================================================================
// 羽化宽度 (像素)：决定相邻两路相机在拼接缝相交重叠的像素带宽度。
// 注意：该值必须为偶数，且不能大于相机的物理重合盲区。
int g_feather_width = 120; 

// 羽化强度 (控制 Alpha 渐变曲线的形状)：
// 1.0 : 纯线性渐变 (Linear)
// > 1.0 : S型平滑曲线 (如 2.0 或 3.0，中心过渡快，边缘更融合)
double g_feather_strength = 2.0;

// ============================================================================
// 手动ROI微调参数 (全局配置)
// ============================================================================
// 用于在自动ROI识别基础上进行手动干预，调整各路相机的裁剪和偏移。
// 按相机索引组织：0=左上，1=右上，2=左下，3=右下
struct ManualRoiTuning {
  int offset_x;     ///< X轴全局偏移（正值向右偏移，负值向左偏移）
  int offset_y;     ///< Y轴全局偏移（正值向下偏移，负值向上偏移）
  int crop_left;    ///< 左侧额外裁剪像素
  int crop_right;   ///< 右侧额外裁剪像素
  int crop_top;     ///< 顶部额外裁剪像素
  int crop_bottom;  ///< 底部额外裁剪像素
};

// 切换标识，用于区分当前是通过数据集视频调试还是使用真实相机输入
// 后续如果改成相机输入，将此全局变量置为 true 即可
bool g_is_using_camera = false;

// 1. 用于“数据集视频输入”的手动拼接微调参数
//offset_x（第1个元素）：X 轴全图偏移量，offset_y（第2个元素）：Y 轴全图偏移量，crop_left（第3个元素）：左侧裁剪，crop_right（第4个元素）：右侧裁剪，crop_top（第5个元素）：顶部裁剪，crop_bottom（第6个元素）：底部裁剪
ManualRoiTuning g_dataset_roi_tuning[4] = {
    {0, 0, 0, 0, 0, 0}, // Cam 0: 左上
    {0, -10, 0, 0, 0, 0}, // Cam 1: 右上
    {0, 0, 0, 0, 0, 0}, // Cam 2: 左下
    {0, -10, 0, 0, 0, 0}  // Cam 3: 右下
};

// 2. 用于“真实相机输入”的手动拼接微调参数
ManualRoiTuning g_camera_roi_tuning[4] = {
    {0, 0, 0, 0, 0, 0}, // Cam 0: 左上
    {0, 0, 0, 0, 0, 0}, // Cam 1: 右上
    {0, 0, 0, 0, 0, 0}, // Cam 2: 左下
    {0, 0, 0, 0, 0, 0}  // Cam 3: 右下
};

// ============================================================================
// 多帧ROI检测参数
// ============================================================================
/// 多帧ROI检测过程中要获取的帧数上限
static constexpr size_t NUM_BOOTSTRAP_FRAMES = 10;

/// 置信度阈值 - ROI检测结果的最小置信度要求
static constexpr double CONFIDENCE_THRESHOLD = 0.25;


namespace {

/**
 * @struct CameraTuning
 * @brief 相机调参结构体
 *
 * 包含相机旋转、裁剪和偏移参数，用于调整拼接效果。
 */
struct CameraTuning {
  int rotation_deg = 0;  ///< 旋转角度（度）
  int crop_left = 0;     ///< 左侧裁剪像素
  int crop_right = 0;    ///< 右侧裁剪像素
  int crop_top = 0;      ///< 顶部裁剪像素
  int crop_bottom = 0;   ///< 底部裁剪像素
  int offset_x = 0;      ///< X轴偏移
  int offset_y = 0;      ///< Y轴偏移
  bool enabled = true;   ///< 是否启用该相机
};

/**
 * @struct OverlapEstimate
 * @brief 重叠区域估计结构体
 *
 * 存储相邻相机间的重叠宽度、Y轴偏移和匹配分数。
 */
struct OverlapEstimate {
  int overlap = 0;     ///< 重叠宽度（像素）
  int shift_y = 0;     ///< Y轴偏移
  double score = 0.0;  ///< 匹配分数
};

/**
 * @struct CameraRoi
 * @brief 相机感兴趣区域结构体
 *
 * 定义每个相机的ROI坐标和尺寸。
 */
struct CameraRoi {
  int x = 0;      ///< ROI起始X坐标
  int y = 0;      ///< ROI起始Y坐标
  int width = 0;  ///< ROI宽度
  int height = 0; ///< ROI高度
};

/**
 * @brief 向下取整到偶数
 * @param value 输入值
 * @return 偶数结果
 * 因为硬件加速库要求偶数尺寸，所以这里提供了一个工具函数来确保尺寸是偶数。
 */
int NormalizeEvenFloor(int value) {
  return std::max(0, value & ~1);
}

/**
 * @brief 向上取整到偶数
 * @param value 输入值
 * @return 偶数结果
 * 因为NV12帧结构要求偶数尺寸，所以这里提供了一个工具函数来确保尺寸是偶数。
 */
int NormalizeEvenCeil(int value) {
  return std::max(2, (value + 1) & ~1);
}

/**
 * @brief 计算旋转后的宽度
 * @param width 原始宽度
 * @param height 原始高度
 * @param rotation_deg 旋转角度
 * @return 旋转后宽度
 */
int RotatedWidth(int width, int height, int rotation_deg) {
  return (rotation_deg == 90 || rotation_deg == 270) ? height : width;
}

/**
 * @brief 计算旋转后的高度
 * @param width 原始宽度
 * @param height 原始高度
 * @param rotation_deg 旋转角度
 * @return 旋转后高度
 */
int RotatedHeight(int width, int height, int rotation_deg) {
  return (rotation_deg == 90 || rotation_deg == 270) ? width : height;
}

/**
 * @brief 计算整数中位数
 * @param values 值列表
 * @return 中位数
 */
int MedianInt(std::vector<int> values) {
  if (values.empty()) {
    return 0;
  }
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

/**
 * @brief 计算浮点数中位数
 * @param values 值列表
 * @return 中位数
 */
double MedianDouble(std::vector<double> values) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

/**
 * @brief 将硬件NV12帧导出为BGR格式
 * @param frame NV12帧
 * @return BGR图像
 */
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

/**
 * @brief 将NV12 DRM缓冲区导出为BGR UMat
 * @param buffer DRM缓冲区
 * @return BGR UMat
 * 用于初始化时使用opencv读取DRM_PRIME帧进行ROI检测和拼接布局构建。
 */
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

/**
 * @brief 构建默认相机调参
 * @param num_cameras 相机数量
 * @return 调参列表
 * 结合全局的手动微调参数
 */
std::vector<CameraTuning> BuildDefaultTuning(size_t num_cameras) {
  std::vector<CameraTuning> tuning(num_cameras);
  
  // 选择根据当前运行模式对应的参数源
  ManualRoiTuning* current_tuning = g_is_using_camera ? g_camera_roi_tuning : g_dataset_roi_tuning;
  
  // 应用全局的手动微调参数
  for (size_t i = 0; i < num_cameras && i < 4; ++i) {
    tuning[i].offset_x = current_tuning[i].offset_x;
    tuning[i].offset_y = current_tuning[i].offset_y;
    tuning[i].crop_left = current_tuning[i].crop_left;
    tuning[i].crop_right = current_tuning[i].crop_right;
    tuning[i].crop_top = current_tuning[i].crop_top;
    tuning[i].crop_bottom = current_tuning[i].crop_bottom;
  }
  return tuning;
}

/**
 * @brief 估计两帧之间的重叠区域
 * 使用ORB特征匹配和模板匹配相结合的方法
 * @param left_bgr 左侧帧BGR图像
 * @param right_bgr 右侧帧BGR图像
 * @return 重叠估计结果
 */
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
  const int band_w = NormalizeEvenFloor(std::min(common_w / 2, 1920));
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

/**
 * @brief 保存检测到的ROI调试图像
 * @param bootstrap_bgr 引导帧BGR图像
 * @param rois ROI列表
 */
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

/**
 * @brief 结构体：2x2矩阵模式下上下相邻帧的重叠估计
 */
struct MatrixOverlap {
  OverlapEstimate h01; ///< 左右重叠 (0和1)
  OverlapEstimate h23; ///< 左右重叠 (2和3)
  OverlapEstimate v02; ///< 上下重叠 (0和2)
  OverlapEstimate v13; ///< 上下重叠 (1和3)
  double confidence = 0.0; ///< 矩阵综合置信度
};

/**
 * @brief 估计上下两帧之间的垂直重叠区域
 * 利用图像旋转技巧复用水平重叠匹配算法
 * @param top_bgr 上侧帧BGR图像
 * @param bottom_bgr 下侧帧BGR图像
 * @return 重叠估计结果
 */
OverlapEstimate EstimateVerticalOverlap(const cv::Mat& top_bgr, const cv::Mat& bottom_bgr) {
  cv::Mat top_rot, bottom_rot;
  // 逆时针旋转90度，使底边变右边，顶边变左边
  cv::rotate(top_bgr, top_rot, cv::ROTATE_90_COUNTERCLOCKWISE);
  cv::rotate(bottom_bgr, bottom_rot, cv::ROTATE_90_COUNTERCLOCKWISE);
  
  OverlapEstimate est = EstimatePairOverlap(top_rot, bottom_rot);
  
  OverlapEstimate result;
  result.overlap = est.overlap;  // 原图像的Y轴重叠高度
  result.shift_y = -est.shift_y; // 原图像的X轴偏移（取反是因为旋转后新Y坐标方向相反）
  result.score = est.score;
  return result;
}

/**
 * @brief 估计2x2输入矩阵所有相邻帧之间的重叠
 * @param frames 引导帧列表
 * @return 矩阵重叠估计结构体
 */
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

/**
 * @brief 基于重叠估计构建2x2输入矩阵的相机ROI
 * 通过对齐4个角点的全局坐标计算精确裁剪参数
 * @param frames 硬件帧列表
 * @param overlaps 2x2矩阵重叠估计
 * @param tuning 相机调参列表
 * @return 相机ROI列表
 */
std::vector<CameraRoi> BuildCameraRois2x2(const std::vector<NV12Frame>& frames,
                                          const MatrixOverlap& overlaps,
                                          const std::vector<CameraTuning>& tuning) {
  if (frames.size() < 4) return {};
  int W[4] = { frames[0].width, frames[1].width, frames[2].width, frames[3].width };
  int H[4] = { frames[0].height, frames[1].height, frames[2].height, frames[3].height };
  
  int X[4] = {0, 0, 0, 0};
  int Y[4] = {0, 0, 0, 0};

  // 左上角(0)作为原点
  X[0] = 0;
  Y[0] = 0;
  // 右上角(1)
  X[1] = W[0] - overlaps.h01.overlap;
  Y[1] = overlaps.h01.shift_y;
  // 左下角(2)
  X[2] = overlaps.v02.shift_y;
  Y[2] = H[0] - overlaps.v02.overlap;
  
  // 右下角(3)由两条路径推导并取平均
  int X3_1 = X[1] + overlaps.v13.shift_y;
  int Y3_1 = Y[1] + H[1] - overlaps.v13.overlap;
  int X3_2 = X[2] + W[2] - overlaps.h23.overlap;
  int Y3_2 = Y[2] + overlaps.h23.shift_y;
  X[3] = (X3_1 + X3_2) / 2;
  Y[3] = (Y3_1 + Y3_2) / 2;
  
  // 加入固定校准偏移
  for (int i = 0; i < 4; ++i) {
    X[i] += tuning[i].offset_x;
    Y[i] += tuning[i].offset_y;
  }
  
  // 计算内侧切割线
  int X_mid_01 = (X[1] + X[0] + W[0]) / 2;
  int X_mid_23 = (X[3] + X[2] + W[2]) / 2;
  int cut_x = NormalizeEvenFloor((X_mid_01 + X_mid_23) / 2);
  
  int Y_mid_02 = (Y[2] + Y[0] + H[0]) / 2;
  int Y_mid_13 = (Y[3] + Y[1] + H[1]) / 2;
  int cut_y = NormalizeEvenFloor((Y_mid_02 + Y_mid_13) / 2);
  
  // ==========================================
  // 更新逻辑：修改布局生成机制创造重叠区
  // ==========================================
  // 使用全局配置的羽化宽度
  int blend_w = NormalizeEvenFloor(g_feather_width);
  
  int cut_x_left   = cut_x;
  int cut_x_right  = cut_x;
  int cut_y_top    = cut_y;
  int cut_y_bottom = cut_y;
  
  cut_x_left  += blend_w / 2;
  cut_x_right -= blend_w / 2;
  cut_y_top += blend_w / 2;
  cut_y_bottom -= blend_w / 2;

  // 全局包围盒边界
  int min_x = NormalizeEvenCeil(std::max(X[0], X[2]));
  int max_x = NormalizeEvenFloor(std::min(X[1] + W[1], X[3] + W[3]));
  int min_y = NormalizeEvenCeil(std::max(Y[0], Y[1]));
  int max_y = NormalizeEvenFloor(std::min(Y[2] + H[2], Y[3] + H[3]));
  
  // 确保切割线在边界内
  cut_x_left = std::max(min_x + 2, std::min(max_x - 2, cut_x_left));
  cut_x_right = std::max(min_x + 2, std::min(max_x - 2, cut_x_right));
  cut_y_top = std::max(min_y + 2, std::min(max_y - 2, cut_y_top));
  cut_y_bottom = std::max(min_y + 2, std::min(max_y - 2, cut_y_bottom));
  
  std::vector<CameraRoi> rois(4);
  
  // Cam 0 (左上)
  rois[0].x = NormalizeEvenFloor(min_x - X[0] + tuning[0].crop_left);
  rois[0].y = NormalizeEvenFloor(min_y - Y[0] + tuning[0].crop_top);
  rois[0].width = NormalizeEvenFloor(cut_x_left - min_x - tuning[0].crop_left - tuning[0].crop_right);
  rois[0].height = NormalizeEvenFloor(cut_y_top - min_y - tuning[0].crop_top - tuning[0].crop_bottom);
  
  // Cam 1 (右上)
  rois[1].x = NormalizeEvenFloor(cut_x_right - X[1] + tuning[1].crop_left);
  rois[1].y = NormalizeEvenFloor(min_y - Y[1] + tuning[1].crop_top);
  rois[1].width = NormalizeEvenFloor(max_x - cut_x_right - tuning[1].crop_left - tuning[1].crop_right);
  rois[1].height = NormalizeEvenFloor(cut_y_top - min_y - tuning[1].crop_top - tuning[1].crop_bottom);
  
  // Cam 2 (左下)
  rois[2].x = NormalizeEvenFloor(min_x - X[2] + tuning[2].crop_left);
  rois[2].y = NormalizeEvenFloor(cut_y_bottom - Y[2] + tuning[2].crop_top);
  rois[2].width = NormalizeEvenFloor(cut_x_left - min_x - tuning[2].crop_left - tuning[2].crop_right);
  rois[2].height = NormalizeEvenFloor(max_y - cut_y_bottom - tuning[2].crop_top - tuning[2].crop_bottom);
  
  // Cam 3 (右下)
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

/**
 * @brief 构建2x2输入矩阵的拼接布局任务
 * @param rois 相机ROI列表
 * @param tuning 相机调参列表
 * @param panorama_width 输出全景图宽度
 * @param panorama_height 输出全景图高度
 * @return 拼接任务列表
 */
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

  tasks[0].dst_x = 0;
  tasks[0].dst_y = 0;

  tasks[1].dst_x = NormalizeEvenFloor(rois[0].width) - NormalizeEvenFloor(g_feather_width);
  tasks[1].dst_y = 0;

  tasks[2].dst_x = 0;
  tasks[2].dst_y = NormalizeEvenFloor(rois[0].height) - NormalizeEvenFloor(g_feather_width);

  tasks[3].dst_x = NormalizeEvenFloor(rois[0].width) - NormalizeEvenFloor(g_feather_width);
  tasks[3].dst_y = NormalizeEvenFloor(rois[0].height) - NormalizeEvenFloor(g_feather_width);

  *panorama_width = NormalizeEvenCeil(tasks[1].dst_x + rois[1].width);
  *panorama_height = NormalizeEvenCeil(tasks[2].dst_y + rois[2].height);

  return tasks;
}

}  

using namespace std;

/**
 * @brief App构造函数，初始化拼接应用
 * 执行引导阶段：捕获前10帧，对每帧进行ROI检测，选择置信度最高的结果
 */
App::App() : num_img_(0), total_cols_(0), height_(0) {
  Logger::GetInstance().Initialize();
  Logger::GetInstance().Log("[App] Application starting...");

  sensorDataInterface_.InitVideoCapture(num_img_);
  image_vector_.resize(num_img_);

  // ============================================================================
  // 多帧ROI检测阶段：获取前NUM_BOOTSTRAP_FRAMES帧，进行多次ROI检测
  // ============================================================================
  Logger::GetInstance().Log("[App] [MULTI-FRAME ROI] Starting multi-frame ROI detection...");
  if (g_multi_frame_roi_debug_level >= 1) {
    ostringstream debug_msg;
    debug_msg << "[App] [MULTI-FRAME ROI] Will capture up to " << NUM_BOOTSTRAP_FRAMES 
              << " frames for ROI detection";
    Logger::GetInstance().Log(debug_msg.str());
  }

  const vector<CameraTuning> tuning = BuildDefaultTuning(num_img_);
  
  // 用于存储每次检测的结果和置信度
  struct RoiDetectionResult {
    MatrixOverlap overlaps;
    vector<CameraRoi> rois;
    vector<StitchTask> layout;
    double confidence = 0.0;  // 综合置信度（所有重叠score的平均值）
    size_t frame_index = 0;
  };
  
  std::vector<RoiDetectionResult> detection_results;
  
  // 循环获取最多NUM_BOOTSTRAP_FRAMES帧进行检测
  size_t frame_count = 0;
  while (frame_count < NUM_BOOTSTRAP_FRAMES) {
    sensorDataInterface_.get_image_vector(image_vector_);
    
    vector<cv::Mat> bootstrap_bgr(num_img_);
    for (size_t i = 0; i < num_img_; ++i) {
      bootstrap_bgr[i] = ExportHardwareFrameToBgr(image_vector_[i]);
    }
    
    // 执行第frame_count+1次检测
    const MatrixOverlap overlaps = EstimateOverlaps2x2(bootstrap_bgr);
    const vector<CameraRoi> rois = BuildCameraRois2x2(image_vector_, overlaps, tuning);
    
    double avg_confidence = overlaps.confidence;
    
    // 检查是否所有置信度都达到阈值
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
      
      // 输出各相邻摄像头对的score
      debug_msg << " h01_score=" << std::fixed << std::setprecision(3) << overlaps.h01.score;
      debug_msg << " h23_score=" << std::fixed << std::setprecision(3) << overlaps.h23.score;
      debug_msg << " v02_score=" << std::fixed << std::setprecision(3) << overlaps.v02.score;
      debug_msg << " v13_score=" << std::fixed << std::setprecision(3) << overlaps.v13.score;
      Logger::GetInstance().Log(debug_msg.str());
    }
    
    frame_count++;
    
    // 如果已经找到高质量结果则可以提前退出
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
  
  // 选择置信度最高的检测结果
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
  
  // 计算布局参数（最终使用最优结果的尺寸）
  int dummy_panorama_width = 0;
  int dummy_panorama_height = 0;
  BuildStitchLayout2x2(rois, tuning, &dummy_panorama_width, &dummy_panorama_height);
  total_cols_ = dummy_panorama_width;
  height_ = dummy_panorama_height;

  image_stitcher_.SetParams(g_feather_width, static_cast<int>(num_img_), total_cols_, height_);
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
  
  // 保存ROI检测调试信息（使用最优结果的第一帧）
  std::vector<cv::Mat> best_bootstrap_bgr(num_img_);
  for (size_t i = 0; i < num_img_; ++i) {
    best_bootstrap_bgr[i] = ExportHardwareFrameToBgr(image_vector_[i]);
  }
  SaveDetectedRoiDebug(best_bootstrap_bgr, rois);

  if (drm_alloc_nv12(total_cols_, height_, output_drm_buf_) != 0) {
    throw std::runtime_error("failed to allocate output DRM DMA-BUF");
  }

  image_concat_.fd = output_drm_buf_.fd;
  image_concat_.width = total_cols_;
  image_concat_.height = height_;
  image_concat_.stride_w = static_cast<int>(output_drm_buf_.pitch);
  image_concat_.stride_h = height_;
}

/**
 * @brief App析构函数，释放DRM缓冲区
 */
App::~App() {
  drm_free(output_drm_buf_);
}

/**
 * @brief 运行拼接主循环
 * 无限循环：获取帧 -> 拼接 -> 保存结果 -> 记录性能
 */
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

    if (g_debug_opencl_feathering && frame_idx == 10) {
      cv::UMat debug_rga = ExportNv12DrmBufferToBgr(output_drm_buf_);
      Logger::GetInstance().SaveImage(debug_rga, "debug_rga_hollow_body.png");
    }

    // 执行 OpenCL 重叠带融合
    image_stitcher_.BlendSeams(image_vector_, image_concat_);

    if (g_debug_opencl_feathering && frame_idx == 10) {
      cv::UMat debug_cl = ExportNv12DrmBufferToBgr(output_drm_buf_);
      Logger::GetInstance().SaveImage(debug_cl, "debug_opencl_seam_blended.png");
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

/**
 * @brief 主函数，创建App实例并运行拼接
 * @return 程序退出码（实际不会返回，因为run_stitching是noreturn）
 */
int main() {
  App app;
  app.run_stitching();
}
