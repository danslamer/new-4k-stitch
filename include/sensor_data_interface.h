//
// Created by s1nh.org on 2020/11/11.
//

#ifndef IMAGE_STITCHING_SENSOR_DATA_INTERFACE_H
#define IMAGE_STITCHING_SENSOR_DATA_INTERFACE_H

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/opencv.hpp>

#include "nv12_frame.h"

extern "C" {
struct AVFrame;
}

enum class QueuedFrameStorage {
    kEmpty = 0,
    kSoftwareNV12 = 1,
    kDrmPrime = 2,
};

// v2: 输入源描述 (替代 4-cam 硬编码 t50..t53.mp4 列表).
// 加载自 params/camera_sources.yaml, 暂未启用 mipi 采集线程 (阶段 3 待写),
// 当前 InitVideoCapture 按 type 路由: file 走 FFmpeg 线程, mipi 暂 fallback 到 file.
struct CameraSource {
    enum class Type {
        kFile,    // 本地视频文件 (走 FFmpeg avformat_open_input)
        kMipi,    // V4L2 节点 (阶段 3 实现, 当前 fallback 到 file)
    };

    Type type = Type::kFile;
    std::string uri;       // 文件路径 或 /dev/videoN
    int width = 0;
    int height = 0;
    int fps = 30;
    std::string pixel_format;  // "NV12" (期望, 仅日志用)

    bool is_mipi() const { return type == Type::kMipi; }
    bool is_file() const { return type == Type::kFile; }
};

// 全局输入源列表 (启动时由 SensorDataInterface 填充, 供 app.cc 路由).
// 默认空 (fallback 到原 t50..t53.mp4 数据集路径).
struct CameraSourceList {
    std::vector<CameraSource> cameras;
    int sync_window_ms = 16;  // 阶段 4 时间戳同步窗口, 默认 1 帧 60fps 预算
    bool auto_calibrate = false;  // 阶段 3 启动期自标定, 默认 off
};

struct QueuedFrame {
    QueuedFrameStorage storage = QueuedFrameStorage::kEmpty;
    std::shared_ptr<AVFrame> hardware_frame;
    int width = 0;
    int height = 0;
    int stride_w = 0;
    int stride_h = 0;
    int pixel_format = -1;
    int dma_buf_fd = -1;
    int drm_layer_count = 0;

    bool empty() const {
        if (storage == QueuedFrameStorage::kDrmPrime) {
            return hardware_frame == nullptr;
        }
        return true;
    }
};

class SensorDataInterface {
 public:
    SensorDataInterface();
    ~SensorDataInterface();

    void InitExampleImages();
    void InitVideoCapture(size_t& num_img);

    void get_frame_vector(std::vector<QueuedFrame>& frame_vector);
    void get_image_vector(std::vector<NV12Frame>& image_vector);
    bool ConvertQueuedFrameToDmabuf(const QueuedFrame& queued_frame,
                                    NV12Frame& frame,
                                    size_t channel_index);

    void RecordVideos();
    std::vector<double> GetDecodeFpsSnapshot();

 private:
    void StartDecodeThreads();
    void StopDecodeThreads();

    const size_t max_queue_length_;
    size_t num_img_;
    size_t frame_idx;
    std::vector<std::queue<QueuedFrame>> image_queue_vector_;
    std::vector<std::mutex> image_queue_mutex_vector_;
    std::vector<cv::VideoCapture> video_capture_vector_;
    std::vector<std::string> video_file_paths_;
    std::vector<std::thread> decode_threads_;
    std::vector<double> decode_fps_vector_;
    std::vector<size_t> decoded_frames_since_report_;
    std::vector<std::chrono::steady_clock::time_point> decode_report_time_vector_;
    std::vector<bool> decoder_ready_vector_;
    std::vector<bool> decoder_finished_vector_;
    std::vector<bool> drm_prime_fallback_logged_vector_;
    std::mutex decode_stats_mutex_;
    std::atomic<bool> stop_requested_;
    std::atomic<bool> decode_threads_started_;
};

#endif  // IMAGE_STITCHING_SENSOR_DATA_INTERFACE_H
