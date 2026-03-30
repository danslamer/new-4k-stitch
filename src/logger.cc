//
// Created by video-stitch project.
//

#include "logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
    #include <direct.h>
#else
    #include <sys/stat.h>
    #include <sys/types.h>
#endif
#include <sys/types.h>

Logger& Logger::GetInstance() {
    static Logger instance;
    return instance;
}

void Logger::Initialize() {
    std::call_once(init_flag_, [this]() {
        CreateResultsDirectory();

        std::string timestamp = GetCurrentTimestamp();
        log_file_path_ = results_dir_ + "/log_" + timestamp + ".log";

        log_file_.open(log_file_path_, std::ios::out);
        if (!log_file_.is_open()) {
            std::cerr << "[Logger] Failed to open log file: " << log_file_path_ << std::endl;
        } else {
            std::cout << "[Logger] Log file created: " << log_file_path_ << std::endl;
            std::cout << "[Logger] Results directory: " << results_dir_ << std::endl;
        }
    });
}

void Logger::Log(const std::string& message) {
    Initialize();
    WriteLine(message, std::cout);
}

void Logger::LogFrame(size_t frame_idx, const std::string& message) {
    Initialize();
    WriteLine(AddFramePrefix(frame_idx, message), std::cout);
}

void Logger::LogError(const std::string& message) {
    Initialize();
    WriteLine(message, std::cerr);
}

void Logger::SaveImage(const cv::UMat& image, const std::string& filename) {
    Initialize();

    std::string full_path = results_dir_ + "/" + filename;
    cv::imwrite(full_path, image);
    Log("[Logger] Image saved: " + full_path);
}

void Logger::SaveImage(const cv::UMat& image,
                       const std::string& filename,
                       size_t frame_idx) {
    Initialize();

    std::string full_path = results_dir_ + "/" + filename;
    cv::imwrite(full_path, image);
    LogFrame(frame_idx, "[Logger] Image saved: " + full_path);
}

std::string Logger::GetResultsDir() const {
    return results_dir_;
}

std::string Logger::GetCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y%m%d_%H%M%S");
    return ss.str();
}

std::string Logger::AddFramePrefix(size_t frame_idx, const std::string& message) const {
    std::ostringstream stream;
    stream << "[frame " << frame_idx << "] " << message;
    return stream.str();
}

void Logger::WriteLine(const std::string& message, std::ostream& terminal_stream) {
    std::lock_guard<std::mutex> lock(write_mutex_);

    std::string timestamp = GetCurrentTimestamp();
    std::string log_message = "[" + timestamp + "] " + message;

    terminal_stream << log_message << std::endl;

    if (log_file_.is_open()) {
        log_file_ << log_message << std::endl;
        log_file_.flush();
    }
}

void Logger::CreateResultsDirectory() {
    std::string base_dir = "../results";
    
    #ifdef _WIN32
        _mkdir(base_dir.c_str());
    #else
        mkdir(base_dir.c_str(), 0777);
    #endif
    
    std::string timestamp = GetCurrentTimestamp();
    results_dir_ = base_dir + "/" + timestamp;
    
    #ifdef _WIN32
        _mkdir(results_dir_.c_str());
    #else
        mkdir(results_dir_.c_str(), 0777);
    #endif
}
