#include "app.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <sys/mman.h>

#include <opencv2/opencv.hpp>

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

std::vector<StitchTask> BuildStitchLayout(const std::vector<NV12Frame>& frames,
                                          const std::vector<CameraTuning>& tuning,
                                          int* panorama_width,
                                          int* panorama_height) {
  const size_t num_img = frames.size();
  std::vector<StitchTask> tasks(num_img);
  int cursor_x = 0;
  int min_y = 0;
  int max_y = 0;

  for (size_t i = 0; i < num_img; ++i) {
    const NV12Frame& frame = frames[i];
    const CameraTuning& cfg = tuning[i];
    if (frame.empty()) {
      throw std::runtime_error("bootstrap hardware frame is empty for camera " + std::to_string(i));
    }

    StitchTask task;
    task.enabled = cfg.enabled;
    task.rotation_deg = cfg.rotation_deg;
    task.src_x = NormalizeEvenFloor(cfg.crop_left);
    task.src_y = NormalizeEvenFloor(cfg.crop_top);
    task.src_w = NormalizeEvenFloor(
        frame.width - cfg.crop_left - cfg.crop_right);
    task.src_h = NormalizeEvenFloor(
        frame.height - cfg.crop_top - cfg.crop_bottom);
    if (task.src_w < 2 || task.src_h < 2) {
      throw std::runtime_error("invalid crop configuration for camera " + std::to_string(i));
    }

    const int rotated_w = NormalizeEvenCeil(RotatedWidth(task.src_w, task.src_h, task.rotation_deg));
    const int rotated_h = NormalizeEvenCeil(RotatedHeight(task.src_w, task.src_h, task.rotation_deg));
    task.dst_x = NormalizeEvenFloor(cursor_x + cfg.offset_x);
    task.dst_y = NormalizeEvenFloor(cfg.offset_y);

    cursor_x = task.dst_x + rotated_w;
    min_y = std::min(min_y, task.dst_y);
    max_y = std::max(max_y, task.dst_y + rotated_h);
    tasks[i] = task;
  }

  if (min_y < 0) {
    for (size_t i = 0; i < tasks.size(); ++i) {
      tasks[i].dst_y -= min_y;
    }
    max_y -= min_y;
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

  const vector<CameraTuning> tuning = BuildDefaultTuning(num_img_);
  const vector<StitchTask> layout =
      BuildStitchLayout(image_vector_, tuning, &total_cols_, &height_);

  image_stitcher_.SetParams(0, static_cast<int>(num_img_), total_cols_, height_);
  image_stitcher_.SetLayout(layout);

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
