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
