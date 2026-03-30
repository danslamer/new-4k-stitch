//
// Created by video-stitch project.
//

#ifndef IMAGE_STITCHING_LOGGER_H
#define IMAGE_STITCHING_LOGGER_H

#include <string>
#include <fstream>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <opencv2/opencv.hpp>

class Logger {
 public:
    static Logger& GetInstance();
    
    void Initialize();
    void Log(const std::string& message);
    void LogFrame(size_t frame_idx, const std::string& message);
    void LogError(const std::string& message);
    void SaveImage(const cv::UMat& image, const std::string& filename);
    void SaveImage(const cv::UMat& image,
                   const std::string& filename,
                   size_t frame_idx);
    std::string GetResultsDir() const;
    
 private:
    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    
    std::string AddFramePrefix(size_t frame_idx, const std::string& message) const;
    void WriteLine(const std::string& message, std::ostream& terminal_stream);
    std::string GetCurrentTimestamp();
    void CreateResultsDirectory();
    
    std::string results_dir_;
    std::string log_file_path_;
    std::ofstream log_file_;
    std::once_flag init_flag_;
    std::mutex write_mutex_;
};

#endif //IMAGE_STITCHING_LOGGER_H
