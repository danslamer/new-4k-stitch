#include "sensor_data_interface.h"
#include "logger.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <string>

// make video_decode_test
// ./video_decode_test

// 测试视频解码性能的主函数
int main() {
    // 初始化Logger
    Logger::GetInstance().Initialize();
    
    // 创建SensorDataInterface实例
    SensorDataInterface sensor_interface;
    
    // 测试参数
    size_t num_cameras = 0;
    const int test_frames = 100; // 测试的帧数
    
    // 1. 测试视频初始化时间
    auto start_init = std::chrono::high_resolution_clock::now();
    sensor_interface.InitVideoCapture(num_cameras);
    auto end_init = std::chrono::high_resolution_clock::now();
    auto init_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_init - start_init).count();
    std::cout << "视频初始化时间: " << init_duration << " ms" << std::endl;
    
    // 2. 测试视频读取和解码性能
    std::vector<cv::UMat> image_vector(num_cameras);
    std::vector<std::mutex> image_mutex_vector(num_cameras);
    
    // 启动视频录制线程（填充图像队列）
    std::thread record_thread(&SensorDataInterface::RecordVideos, &sensor_interface);
    record_thread.detach();
    
    // 等待线程启动并填充队列
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    
    // 预热（读取几帧）
    std::cout << "正在预热..." << std::endl;
    for (int i = 0; i < 10; ++i) {
        sensor_interface.get_image_vector(image_vector, image_mutex_vector);
    }
    
    // 开始正式测试
    std::cout << "开始测试视频读取性能..." << std::endl;
    auto start_read = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < test_frames; ++i) {
        // 记录单帧读取时间
        auto frame_start = std::chrono::high_resolution_clock::now();
        sensor_interface.get_image_vector(image_vector, image_mutex_vector);
        auto frame_end = std::chrono::high_resolution_clock::now();
        auto frame_duration = std::chrono::duration_cast<std::chrono::milliseconds>(frame_end - frame_start).count();
        
        if (i % 10 == 0) {
            std::cout << "第 " << i << " 帧读取时间: " << frame_duration << " ms" << std::endl;
        }
    }
    
    auto end_read = std::chrono::high_resolution_clock::now();
    auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_read - start_read).count();
    double avg_frame_time = total_duration / (double)test_frames;
    double fps = 1000.0 / avg_frame_time;
    
    std::cout << "\n测试结果：" << std::endl;
    std::cout << "总测试帧数: " << test_frames << std::endl;
    std::cout << "总耗时: " << total_duration << " ms" << std::endl;
    std::cout << "平均每帧耗时: " << avg_frame_time << " ms" << std::endl;
    std::cout << "帧率: " << fps << " FPS" << std::endl;
    
    // 3. 测试单独的视频捕获性能
    std::cout << "\n测试单独的视频捕获性能..." << std::endl;
    std::string video_dir = "../datasets/4k-test/";
    std::vector<std::string> video_file_names = {"40.mp4", "41.mp4", "42.mp4", "43.mp4"};
    
    for (size_t i = 0; i < video_file_names.size(); ++i) {
        std::string file_name = video_dir + video_file_names[i];
        cv::VideoCapture capture;
        
        auto cap_start = std::chrono::high_resolution_clock::now();
        capture.open(file_name);
        auto cap_end = std::chrono::high_resolution_clock::now();
        auto cap_duration = std::chrono::duration_cast<std::chrono::milliseconds>(cap_end - cap_start).count();
        
        if (capture.isOpened()) {
            std::cout << "打开视频 " << video_file_names[i] << " 耗时: " << cap_duration << " ms" << std::endl;
            
            // 测试单视频解码性能
            cv::UMat frame;
            auto decode_start = std::chrono::high_resolution_clock::now();
            int decode_count = 0;
            while (capture.read(frame) && decode_count < 50) {
                decode_count++;
            }
            auto decode_end = std::chrono::high_resolution_clock::now();
            auto decode_duration = std::chrono::duration_cast<std::chrono::milliseconds>(decode_end - decode_start).count();
            double avg_decode_time = decode_duration / (double)decode_count;
            
            std::cout << "视频 " << video_file_names[i] << " 解码50帧耗时: " << decode_duration << " ms" << std::endl;
            std::cout << "平均每帧解码时间: " << avg_decode_time << " ms" << std::endl;
            
            capture.release();
        } else {
            std::cout << "无法打开视频 " << video_file_names[i] << std::endl;
        }
    }
    
    return 0;
}