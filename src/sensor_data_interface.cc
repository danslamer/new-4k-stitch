//
// Created by s1nh.org on 11/11/20.
// https://zhuanlan.zhihu.com/p/38136322
//

#include "sensor_data_interface.h"
#include "logger.h"

#include <string>
#include <thread>

SensorDataInterface::SensorDataInterface()
    : max_queue_length_(2) {
  num_img_ = 0;
}

//读取图片数据
// void SensorDataInterface::InitExampleImages() {
//  std::string img_dir = "../datasets/cam01/pic_raw/";
//  std::vector<std::string> img_file_name = {"0.jpg",
//                                            "1.jpg",
//                                            "2.jpg",
//                                            "3.jpg",
//                                            "4.jpg"};
//  num_img_ = img_file_name.size();
//  image_queue_vector_ = std::vector<std::queue<cv::UMat>>(num_img_);
//  for (int i = 0; i < img_file_name.size(); ++i) {
//    std::string file_name = img_dir + img_file_name[i];
//    cv::UMat _;
//    cv::imread(file_name, 1).copyTo(_);
//    image_queue_vector_[i].push(_);
//  }
// }


void SensorDataInterface::InitVideoCapture(size_t& num_img) {
  std::string video_dir = "../datasets/4k-test/";
  std::vector<std::string> video_file_name = {
      "40.mp4", "41.mp4", "42.mp4", "43.mp4"};

  // 设置摄像头数量为视频文件的数量（4个）
  num_img_ = video_file_name.size();
  // 通过引用参数返回摄像头数量给调用者
  num_img = num_img_;
  // 为每个摄像头创建图像队列，用于存储帧数据（使用UMat支持OpenCL加速）
  image_queue_vector_ = std::vector<std::queue<cv::UMat>>(num_img_);
  // 为每个摄像头创建互斥锁，保证队列访问的线程安全
  image_queue_mutex_vector_ = std::vector<std::mutex>(num_img_);

  // 遍历所有视频文件，初始化视频捕获对象
  for (int i = 0; i < num_img_; ++i) {
    // 构建完整的视频文件路径
    std::string file_name = video_dir + video_file_name[i];

    // 创建视频捕获对象并打开视频文件
    cv::VideoCapture capture(file_name);
    // 检查视频文件是否成功打开
    if (!capture.isOpened()) {
      // 如果打开失败，记录错误日志
      Logger::GetInstance().LogError("fail to open! " + file_name);
    }
    // 将视频捕获对象存储到向量中
    video_capture_vector_.push_back(capture);

    // 读取视频的第一帧
    cv::UMat frame;
    capture.read(frame);
    // 将第一帧放入对应的队列中，供后续处理使用
    image_queue_vector_[i].push(frame);
  }
}


void SensorDataInterface::RecordVideos() {

   // 无限循环，持续从所有摄像头读取视频帧
  while (true) {
    // 遍历所有摄像头（num_img_ 个摄像头）
    for (int i = 0; i < num_img_; ++i) {
      // 创建 UMat 对象存储当前帧（使用 UMat 支持 OpenCL 加速）
      cv::UMat frame;
      
      // 从第 i 个视频捕获对象中读取一帧
      video_capture_vector_[i].read(frame);
      
      // 检查帧是否有效（frame.rows > 0 表示读取成功）
      if (frame.rows > 0) {
        // 加锁，保证队列操作的线程安全
        image_queue_mutex_vector_[i].lock();
        
        // 将读取的帧放入第 i 个摄像头的队列中
        image_queue_vector_[i].push(frame);
        
        // 检查队列长度是否超过最大限制（max_queue_length_ = 2）
        if (image_queue_vector_[i].size() > max_queue_length_) {
          // 如果队列过长，暂停10毫秒，让消费者有时间处理队列中的帧
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        // 解锁，释放互斥锁
        image_queue_mutex_vector_[i].unlock();
      } else {
        // 如果读取失败（视频结束或文件损坏），跳出循环
        break;
      }
    }
    
    // 帧索引递增
    frame_idx++;
    
    // 注释掉的代码：如果需要降低帧率，可以取消注释这行代码
    // std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  }
}


void
SensorDataInterface::get_image_vector(
    std::vector<cv::UMat>& image_vector,
    std::vector<std::mutex>& image_mutex_vector) {

  // 遍历所有摄像头（num_img_ 个摄像头）
  for (size_t i = 0; i < num_img_; ++i) {
    // 创建未畸变图像的临时变量（当前未使用，可能用于后续扩展）
    cv::Mat img_undistort;
    // 创建柱面投影图像的临时变量（当前未使用，可能用于后续扩展）
    cv::Mat img_cylindrical;

    // 等待第 i 个摄像头的队列中有数据
    // 如果队列为空，暂停10毫秒后继续检查
    // 这确保了生产者-消费者模式的同步，避免访问空队列
    while (image_queue_vector_[i].empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 加锁保护内部队列，防止多线程竞争
    // 确保在访问队列时不会被其他线程修改
    image_queue_mutex_vector_[i].lock();
    
    // 加锁保护外部图像向量，防止多线程竞争
    // 确保在写入图像向量时不会被其他线程读取或修改
    image_mutex_vector[i].lock();
    
    // 从第 i 个摄像头的队列中获取最新的帧（队列头部）
    // 将队列头部的帧数据复制到外部图像向量中
    image_vector[i] = image_queue_vector_[i].front();
    
    // 从队列中移除已获取的帧
    // 防止队列无限增长，保持队列长度在合理范围内
    image_queue_vector_[i].pop();
    
    // 解锁外部图像向量，释放互斥锁
    // 允许其他线程访问图像向量
    image_mutex_vector[i].unlock();
    
    // 解锁内部队列，释放互斥锁
    // 允许 RecordVideos() 线程继续向队列中添加帧
    image_queue_mutex_vector_[i].unlock();
  }
}
