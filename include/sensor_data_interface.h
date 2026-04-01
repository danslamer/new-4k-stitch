//
// Created by s1nh.org on 2020/11/11.
//

#ifndef IMAGE_STITCHING_SENSOR_DATA_INTERFACE_H
#define IMAGE_STITCHING_SENSOR_DATA_INTERFACE_H

#include <atomic>
#include <chrono>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/opencv.hpp>

class SensorDataInterface {
 public:
    SensorDataInterface();
    ~SensorDataInterface();

    void InitExampleImages();
    void InitVideoCapture(size_t& num_img);

    void get_image_vector(std::vector<cv::UMat>& image_vector,
                          std::vector<std::mutex>& image_mutex_vector);

    void RecordVideos();
    std::vector<double> GetDecodeFpsSnapshot();

 private:
    void StartDecodeThreads();
    void StopDecodeThreads();

    const size_t max_queue_length_;
    size_t num_img_;
    size_t frame_idx;
    std::vector<std::queue<cv::UMat>> image_queue_vector_;
    std::vector<std::mutex> image_queue_mutex_vector_;
    std::vector<cv::VideoCapture> video_capture_vector_;
    std::vector<std::string> video_file_paths_;
    std::vector<std::thread> decode_threads_;
    std::vector<double> decode_fps_vector_;
    std::vector<size_t> decoded_frames_since_report_;
    std::vector<std::chrono::steady_clock::time_point> decode_report_time_vector_;
    std::vector<bool> decoder_ready_vector_;
    std::vector<bool> decoder_finished_vector_;
    std::mutex decode_stats_mutex_;
    std::atomic<bool> stop_requested_;
    std::atomic<bool> decode_threads_started_;
};

#endif  // IMAGE_STITCHING_SENSOR_DATA_INTERFACE_H
