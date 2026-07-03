//
// Created by s1nh.org on 11/11/20.
// Updated to use FFmpeg decoding for multi-channel video input.
//

/**
 * @file sensor_data_interface.cc
 * @brief 传感器数据接口实现
 * 负责多路视频流的获取、FFmpeg硬件解码和帧队列管理
 * 支持RK硬件解码(rkmpp)和DRM_PRIME缓冲区输出
 *
 * ⚠️ 架构硬约束 (2026-07-02 锁定):
 *   - 6 路 4K 拼接要求 30 FPS 端到端, 软件解码 (mpeg4 sw) 性能远不够
 *   - 整条管线必须是 硬件解码 (rkmpp) + DMA-BUF 零拷贝
 *   - 收到 sw 帧 (AV_PIX_FMT_YUV420P) 必须直接 reject + 报错, 不能 fallback
 *   - 当前 sw fallback 已被业务侧明确拒绝 (见 CLAUDE.md 架构硬约束节)
 *   - 任何"暂时 sw 解码跑一下看效果"的提案都不应合入
 */

AvErrorToString() - FFmpeg错误代码转换为可读字符串
IsHardwarePixelFormat() - 判断像素格式是否为硬件加速格式
FindPreferredDecoder() - 查找优先解码器（rkmpp或软件）
DecoderPerfStats 结构体 - 解码器性能统计
AVFrameDeleter 结构体 - 智能指针删除器用于自动释放AVFrame
CloneFrameReference() - 克隆AVFrame引用
FillDrmPrimeMetadata() - 填充DRM_PRIME帧元数据
SelectDecoderPixelFormat() - 选择解码器输出像素格式
FormatDecoderPerfLog() - 格式化解码器性能日志

InitExampleImages() - 初始化示例图像（未使用）
InitVideoCapture() - 初始化视频捕获和解码线程
StartDecodeThreads() - 启动解码线程
StopDecodeThreads() - 停止解码线程
RecordVideos() - 记录视频
get_frame_vector() - 从缓冲队列提取帧
ConvertQueuedFrameToDmabuf() - 将QueuedFrame转换为DMA-BUF格式
get_image_vector() - 获取所有通道的NV12帧
GetDecodeFpsSnapshot() - 获取解码FPS快照
 */

#include "sensor_data_interface.h"

#include "logger.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace {

/**
 * @brief 将FFmpeg错误代码转换为可读的错误字符串
 * @param errnum FFmpeg错误代码
 * @return 错误说明字符串
 */
std::string AvErrorToString(int errnum) {
  char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
  av_strerror(errnum, errbuf, sizeof(errbuf));
  return std::string(errbuf);
}

/**
 * @brief 判断像素格式是否为硬件加速格式
 * @param pixel_format AVPixelFormat格式
 * @return 如果是硬件格式返回true，否则返回false
 */
bool IsHardwarePixelFormat(AVPixelFormat pixel_format) {
  const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(pixel_format);
  return descriptor != nullptr &&
         (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0;
}

/**
 * @brief 查找优先使用的解码器
 * 若启用硬件解码，优先选择rkmpp解码器；否则返回软件解码器
 * @param codec_id 编码器ID
 * @return 指向AVCodec结构的指针，如果未找到则返回nullptr
 */
const AVCodec* FindPreferredDecoder(AVCodecID codec_id) {
#ifdef ENABLE_RK_HARDWARE_DECODING
  const char* decoder_name = nullptr;
  switch (codec_id) {
    case AV_CODEC_ID_H264:
      decoder_name = "h264_rkmpp";
      break;
    case AV_CODEC_ID_HEVC:
      decoder_name = "hevc_rkmpp";
      break;
    case AV_CODEC_ID_MPEG2VIDEO:
      decoder_name = "mpeg2_rkmpp";
      break;
    case AV_CODEC_ID_MPEG4:
      decoder_name = "mpeg4_rkmpp";
      break;
    case AV_CODEC_ID_VP8:
      decoder_name = "vp8_rkmpp";
      break;
    case AV_CODEC_ID_VP9:
      decoder_name = "vp9_rkmpp";
      break;
    default:
      break;
  }

  if (decoder_name != nullptr) {
    const AVCodec* hw_codec = avcodec_find_decoder_by_name(decoder_name);
    if (hw_codec != nullptr) {
      return hw_codec;
    }
  }
#endif

  return avcodec_find_decoder(codec_id);
}

/**
 * @struct DecoderPerfStats
 * @brief 解码器性能统计结构体
 * 记录解码过程中各阶段的帧数、字节数和耗时
 */
struct DecoderPerfStats {
  size_t packets_read = 0;
  size_t packets_sent = 0;
  size_t frames_decoded = 0;
  size_t hardware_frames = 0;
  size_t software_frames = 0;
  size_t drm_prime_frames = 0;
  size_t hw_transfers = 0;
  size_t queue_pushes = 0;
  size_t queue_drops = 0;
  double read_seconds = 0.0;
  double send_seconds = 0.0;
  double receive_seconds = 0.0;
  double transfer_seconds = 0.0;
  double nv12_extract_seconds = 0.0;
  std::chrono::steady_clock::time_point report_time =
      std::chrono::steady_clock::now();
};

/**
 * @struct AVFrameDeleter
 * @brief AVFrame智能指针删除器
 * 用于shared_ptr自动释放AVFrame内存
 */
struct AVFrameDeleter {
  /**
   * @brief 删除AVFrame对象
   * @param frame 要删除的AVFrame指针
   */
  void operator()(AVFrame* frame) const {
    if (frame != nullptr) {
      av_frame_free(&frame);
    }
  }
};

using AVFramePtr = std::shared_ptr<AVFrame>;

/**
 * @brief 克隆AVFrame引用
 * 复制源帧的元数据和缓冲区引用（非深拷贝）
 * @param source_frame 源AVFrame指针
 * @return 包含帧引用的shared_ptr，如果失败则返回空指针
 */
AVFramePtr CloneFrameReference(const AVFrame* source_frame) {
  if (source_frame == nullptr) {
    return AVFramePtr();
  }

  AVFrame* frame_ref = av_frame_alloc();
  if (frame_ref == nullptr) {
    return AVFramePtr();
  }

  if (av_frame_ref(frame_ref, source_frame) < 0) {
    av_frame_free(&frame_ref);
    return AVFramePtr();
  }

  return AVFramePtr(frame_ref, AVFrameDeleter{});
}

/**
 * @brief 填充DRM_PRIME帧元数据
 * 从DRM_PRIME格式的AVFrame中提取DMA-BUF文件描述符和步长信息
 * @param frame AVFrame指针，应为DRM_PRIME格式
 * @param queued_frame 待填充的QueuedFrame结构
 */
void FillDrmPrimeMetadata(const AVFrame* frame, QueuedFrame& queued_frame) {
  queued_frame.width = frame != nullptr ? frame->width : 0;
  queued_frame.height = frame != nullptr ? frame->height : 0;
  queued_frame.stride_w = 0;
  queued_frame.stride_h = 0;
  queued_frame.pixel_format =
      frame != nullptr ? frame->format : static_cast<int>(AV_PIX_FMT_NONE);
  queued_frame.dma_buf_fd = -1;
  queued_frame.drm_layer_count = 0;

  if (frame == nullptr || frame->format != AV_PIX_FMT_DRM_PRIME ||
      frame->data[0] == nullptr) {
    return;
  }

  const AVDRMFrameDescriptor* descriptor =
      reinterpret_cast<const AVDRMFrameDescriptor*>(frame->data[0]);
  if (descriptor == nullptr) {
    return;
  }

  queued_frame.drm_layer_count = descriptor->nb_layers;
  if (descriptor->nb_objects > 0) {
    queued_frame.dma_buf_fd = descriptor->objects[0].fd;
  }
  if (descriptor->nb_layers > 0 && descriptor->layers[0].nb_planes > 0) {
    queued_frame.stride_w = descriptor->layers[0].planes[0].pitch;
    queued_frame.stride_h = queued_frame.height;
  }
}

/**
 * @brief 选择解码器输出像素格式
 * 在支持的格式列表中优先选择DRM_PRIME硬件格式
 * @param codec_context 编码器上下文
 * @param pixel_formats 支持的像素格式数组（以AV_PIX_FMT_NONE结尾）
 * @return 选定的像素格式
 */
AVPixelFormat SelectDecoderPixelFormat(AVCodecContext* codec_context,
                                       const AVPixelFormat* pixel_formats) {
  if (pixel_formats == nullptr) {
    return AV_PIX_FMT_NONE;
  }

  std::ostringstream candidate_stream;
  candidate_stream << "[decoder_get_format] codec="
                   << (codec_context != nullptr && codec_context->codec != nullptr
                           ? codec_context->codec->name
                           : "unknown")
                   << " candidates=";
  bool first_candidate = true;
  for (const AVPixelFormat* format = pixel_formats;
       *format != AV_PIX_FMT_NONE;
       ++format) {
    const char* format_name = av_get_pix_fmt_name(*format);
    if (!first_candidate) {
      candidate_stream << ",";
    }
    candidate_stream << (format_name != nullptr ? format_name : "unknown");
    first_candidate = false;
  }
  Logger::GetInstance().Log(candidate_stream.str());

#ifdef ENABLE_RK_HARDWARE_DECODING
  for (const AVPixelFormat* format = pixel_formats;
       *format != AV_PIX_FMT_NONE;
       ++format) {
    if (*format == AV_PIX_FMT_DRM_PRIME) {
      return *format;
    }
  }
#endif

  return pixel_formats[0];
}

/**
 * @brief 格式化解码器性能日志
 * @param decoder_index 解码器索引（通道号）
 * @param decoder_name 解码器名称
 * @param frame_format_name 输出帧格式名称
 * @param stats 性能统计数据
 * @param elapsed_seconds 统计时间间隔（秒）
 * @return 格式化的日志字符串
 */
std::string FormatDecoderPerfLog(size_t decoder_index,
                                 const std::string& decoder_name,
                                 const std::string& frame_format_name,
                                 const DecoderPerfStats& stats,
                                 double elapsed_seconds) {
  std::ostringstream stream;
  stream << "[decoder_perf " << decoder_index << "]"
         << " decoder=" << decoder_name
         << " frame_fmt=" << frame_format_name
         << " packets=" << stats.packets_read
         << " frames=" << stats.frames_decoded
         << " fps=" << (elapsed_seconds > 0.0
                            ? static_cast<double>(stats.frames_decoded) / elapsed_seconds
                            : 0.0)
         << " hw_frames=" << stats.hardware_frames
         << " drm_prime_frames=" << stats.drm_prime_frames
         << " sw_frames=" << stats.software_frames
         << " hw_transfers=" << stats.hw_transfers
         << " read_ms=" << stats.read_seconds * 1000.0
         << " send_ms=" << stats.send_seconds * 1000.0
         << " receive_ms=" << stats.receive_seconds * 1000.0
         << " transfer_ms=" << stats.transfer_seconds * 1000.0
         << " nv12_extract_ms=" << stats.nv12_extract_seconds * 1000.0
         << " queue_pushes=" << stats.queue_pushes
         << " queue_drops=" << stats.queue_drops;
  return stream.str();
}

}  // namespace

/**
 * @class SensorDataInterface
 * @brief 传感器数据接口类
 * 负责管理多路视频解码且框管理
 */
SensorDataInterface::SensorDataInterface()
    : max_queue_length_(2),
      num_img_(0),
      frame_idx(0),
      stop_requested_(false),
      decode_threads_started_(false) {}

/**
 * @brief SensorDataInterface析构函数
 * 程序终止时止解码线程
 */
SensorDataInterface::~SensorDataInterface() {
  StopDecodeThreads();
}

/**
 * @brief 初始化示例图像列表
 * 目前不使用，仅特残帧查详素
 */
void SensorDataInterface::InitExampleImages() {}

// v2: 输入源描述 (阶段 2 引入, 由 InitVideoCapture 填充).
// 加载自 params/camera_sources.yaml, 不存在则 fallback 到原 t50..t53.mp4 列表 (4 路).
// 阶段 4 改 6 路时, yaml 加 cam4/cam5 块 + LoadDefaultDatasetSources 加 t54/t55.mp4.
// 阶段 3 才会用 kMipi 实际跑 V4L2 采集线程; 当前 kMipi 也 fallback 到 file
// (仅日志告警, 不破坏数据集调试).
static CameraSourceList g_camera_source_list;
const CameraSourceList& GetCameraSourceList() { return g_camera_source_list; }

// 解析 params/camera_sources.yaml, 用 OpenCV FileStorage (项目已依赖).
// 失败时 (文件不存在 / 解析错) 返回 false, 保持 g_camera_source_list 为空,
// 触发 InitVideoCapture 的 fallback 逻辑 (与 v1 行为一致).
// OpenCV 4.5.4 的 YAML 解析器在格式上比 libyaml 严, 任何 cv::Exception 都
// 一律吞掉返回 false — 主流程 (InitVideoCapture) 只看返回值, 不应该被
// yaml 格式问题 crash. 这与本函数注释"加载失败时 fallback"的契约一致.
static bool LoadCameraSourceList(const std::string& path) {
  try {
    cv::FileStorage fs(path, cv::FileStorage::READ);
    if (!fs.isOpened()) {
      Logger::GetInstance().LogError(
          "[sensor_data_interface] camera_sources.yaml: cannot open '" + path +
          "', fallback to default dataset");
      return false;
    }
    cv::FileNode root = fs.root();
    if (root.empty() || !root.isMap()) {
      Logger::GetInstance().LogError(
          "[sensor_data_interface] camera_sources.yaml: empty root or not a map, fallback");
      return false;
    }
    cv::FileNode cameras = root["cameras"];
    if (cameras.empty() || !cameras.isSeq()) {
      Logger::GetInstance().LogError(
          "[sensor_data_interface] camera_sources.yaml: 'cameras' missing or not a seq, fallback");
      return false;
    }
    g_camera_source_list.cameras.clear();
    for (cv::FileNodeIterator it = cameras.begin(); it != cameras.end(); ++it) {
      cv::FileNode cam = *it;
      CameraSource src;
      std::string type_str;
      cam["type"] >> type_str;
      if (type_str == "mipi") {
        src.type = CameraSource::Type::kMipi;
      } else {
        src.type = CameraSource::Type::kFile;
      }
      cam["uri"] >> src.uri;
      cam["width"] >> src.width;
      cam["height"] >> src.height;
      cam["fps"] >> src.fps;
      cam["pixel_format"] >> src.pixel_format;
      if (src.uri.empty()) {
        Logger::GetInstance().LogError(
            "[sensor_data_interface] camera_sources.yaml: empty uri, skipped");
        continue;
      }
      g_camera_source_list.cameras.push_back(src);
    }
    if (root["sync_window_ms"].isInt()) {
      root["sync_window_ms"] >> g_camera_source_list.sync_window_ms;
    }
    if (root["auto_calibrate"].isInt()) {
      int v = 0;
      root["auto_calibrate"] >> v;
      g_camera_source_list.auto_calibrate = (v != 0);
    }
    return !g_camera_source_list.cameras.empty();
  } catch (const cv::Exception& e) {
    Logger::GetInstance().LogError(
        "[sensor_data_interface] camera_sources.yaml: cv::Exception '" +
        std::string(e.what()) + "', fallback to default dataset");
    return false;
  } catch (const std::exception& e) {
    Logger::GetInstance().LogError(
        "[sensor_data_interface] camera_sources.yaml: std::exception '" +
        std::string(e.what()) + "', fallback to default dataset");
    return false;
  }
}

// 默认 6 路数据集 (v2.3 阶段: 2K 2560x1440, 2x3 布局).
// 目录: datasets/2k-test/. 4K 源在 datasets/4k-test/, 用 tools/downscale_4k_to_2k.py 降下来.
// 注: t40/t41 是临时占位 (我们没有真正的 6-camera-rig 视频), 后续用真实 6 路替换.
// ⚠️ 真实项目应该用 6 个同一相机组的视频 (例如全 t50 编号); 阶段 6 真机跑通前需替换.
static std::vector<CameraSource> LoadDefaultDatasetSources() {
  const std::string video_dir = "../datasets/2k-test/";
  const std::vector<std::string> default_files = {
      "t50.mp4", "t51.mp4", "t52.mp4", "t53.mp4",
      "t40.mp4", "t41.mp4"};
  std::vector<CameraSource> sources;
  sources.reserve(default_files.size());
  for (const auto& f : default_files) {
    CameraSource src;
    src.type = CameraSource::Type::kFile;
    src.uri = video_dir + f;
    src.fps = 30;
    sources.push_back(src);
  }
  Logger::GetInstance().Log(
      "[sensor_data_interface] INPUT_SOURCE_MODE=dataset, use " +
      std::to_string(sources.size()) + " file source(s) from " + video_dir);
  return sources;
}

/**
 * @brief 初始化视频捕取
 * 根据输入源描述创建缓冲队列和解码线程
 * @param num_img 数组大小（口数），传回实际路数
 *
 * v2 行为: 优先读 params/camera_sources.yaml (4 路 mipi / file),
 * 加载失败时 fallback 到原 t50..t53.mp4 数据集路径 (保持向后兼容).
 */
void SensorDataInterface::InitVideoCapture(size_t& num_img) {
  num_img_ = 0;
  num_img = 0;
  image_queue_vector_.clear();
  image_queue_mutex_vector_.clear();
  video_capture_vector_.clear();
  video_file_paths_.clear();
  decode_threads_.clear();
  decode_fps_vector_.clear();
  decoded_frames_since_report_.clear();
  decode_report_time_vector_.clear();
  decoder_ready_vector_.clear();
  decoder_finished_vector_.clear();
  drm_prime_fallback_logged_vector_.clear();

  const std::string sources_path = "../params/camera_sources.yaml";
  const bool loaded = LoadCameraSourceList(sources_path);
  std::vector<CameraSource> effective_sources;

  // INPUT_SOURCE_MODE: dataset | camera, 默认 dataset (无 env 走历史数据集路径).
  //   dataset - 走默认 t50..t53.mp4 数据集, 忽略 yaml
  //   camera  - 走 params/camera_sources.yaml, 阶段 3 启用 V4L2 线程
  const char* env_mode = std::getenv("INPUT_SOURCE_MODE");
  std::string mode = env_mode ? env_mode : "dataset";
  if (mode != "dataset" && mode != "camera") {
    Logger::GetInstance().LogError(
        "[sensor_data_interface] INPUT_SOURCE_MODE='" + mode +
        "' invalid, expected dataset/camera, fallback to dataset");
    mode = "dataset";
  }

  if (mode == "camera") {
    if (!loaded) {
      Logger::GetInstance().LogError(
          "[sensor_data_interface] INPUT_SOURCE_MODE=camera but " +
          sources_path + " not found or invalid");
      return;
    }
    effective_sources = g_camera_source_list.cameras;
    Logger::GetInstance().Log(
        "[sensor_data_interface] INPUT_SOURCE_MODE=camera, loaded " +
        std::to_string(effective_sources.size()) +
        " camera source(s) from " + sources_path);
  } else {
    effective_sources = LoadDefaultDatasetSources();
  }

  // 阶段 3 之前: kMipi 暂跳过, 仅日志告警.
  int n_mipi = 0;
  for (const auto& src : effective_sources) {
    if (src.is_mipi()) {
      ++n_mipi;
      Logger::GetInstance().LogError(
          "[sensor_data_interface] MIPI type not yet implemented (phase 3), "
          "uri=" + src.uri + " -> skipped. Wait for V4L2 capture thread.");
    }
  }
  const bool all_mipi = (n_mipi == static_cast<int>(effective_sources.size())
                         && !effective_sources.empty());
  if (all_mipi && mode == "camera") {
    Logger::GetInstance().LogError(
        "[sensor_data_interface] INPUT_SOURCE_MODE=camera but all sources are "
        "MIPI and V4L2 capture thread not implemented yet (phase 3). "
        "Either install GC4683 driver + finish phase 3, or set "
        "INPUT_SOURCE_MODE=dataset to use t50..t53.mp4");
    return;
  }

  std::vector<CameraSource> file_sources;
  for (const auto& src : effective_sources) {
    if (src.is_mipi()) continue;  // phase 3 后才会用
    file_sources.push_back(src);
  }
  if (file_sources.empty()) {
    Logger::GetInstance().LogError(
        "[sensor_data_interface] no usable file sources, num_img=0");
    return;
  }

  num_img_ = file_sources.size();
  num_img = static_cast<int>(num_img_);

  image_queue_vector_ = std::vector<std::queue<QueuedFrame>>(num_img_);
  image_queue_mutex_vector_ = std::vector<std::mutex>(num_img_);

  decode_threads_.reserve(num_img_);
  decode_fps_vector_ = std::vector<double>(num_img_, 0.0);
  decoded_frames_since_report_ = std::vector<size_t>(num_img_, 0);
  decode_report_time_vector_ =
      std::vector<std::chrono::steady_clock::time_point>(
          num_img_, std::chrono::steady_clock::now());
  decoder_ready_vector_ = std::vector<bool>(num_img_, false);
  decoder_finished_vector_ = std::vector<bool>(num_img_, false);
  drm_prime_fallback_logged_vector_ = std::vector<bool>(num_img_, false);

  for (size_t i = 0; i < num_img_; ++i) {
    video_file_paths_.push_back(file_sources[i].uri);
  }

  StartDecodeThreads();
}

/**
 * @brief 启动解码线程
 */
void SensorDataInterface::StartDecodeThreads() {
  if (decode_threads_started_.exchange(true)) {
    return;
  }

  stop_requested_ = false;

  for (size_t i = 0; i < num_img_; ++i) {
    decode_threads_.emplace_back([this, i]() {
      const std::string& file_name = video_file_paths_[i];
      DecoderPerfStats perf_stats;
      std::string last_frame_format_name = "unknown";

      AVFormatContext* format_context = nullptr;
      AVCodecContext* codec_context = nullptr;
      AVPacket* packet = nullptr;
      AVFrame* decoded_frame = nullptr;
      int video_stream_index = -1;

      auto cleanup = [&]() {
        if (decoded_frame != nullptr) {
          av_frame_free(&decoded_frame);
        }
        if (packet != nullptr) {
          av_packet_free(&packet);
        }
        if (codec_context != nullptr) {
          avcodec_free_context(&codec_context);
        }
        if (format_context != nullptr) {
          avformat_close_input(&format_context);
        }
      };

      auto receive_and_queue_frames = [&]() -> bool {
        while (!stop_requested_) {
          const double receive_t0 = cv::getTickCount();
          int receive_ret = avcodec_receive_frame(codec_context, decoded_frame);
          perf_stats.receive_seconds +=
              (cv::getTickCount() - receive_t0) / cv::getTickFrequency();
          if (receive_ret == AVERROR(EAGAIN)) {
            return true;
          }
          if (receive_ret == AVERROR_EOF) {
            return true;
          }
          if (receive_ret < 0) {
            Logger::GetInstance().LogError(
                "[decoder " + std::to_string(i) + "] receive frame failed: " +
                AvErrorToString(receive_ret));
            return false;
          }

          const AVPixelFormat decoded_pixel_format =
              static_cast<AVPixelFormat>(decoded_frame->format);
          const char* decoded_format_name = av_get_pix_fmt_name(decoded_pixel_format);
          last_frame_format_name =
              decoded_format_name != nullptr ? decoded_format_name : "unknown";
          perf_stats.frames_decoded++;
          QueuedFrame queued_frame;
          queued_frame.width = decoded_frame->width;
          queued_frame.height = decoded_frame->height;
          queued_frame.pixel_format = decoded_frame->format;

          if (decoded_pixel_format == AV_PIX_FMT_DRM_PRIME) {
            perf_stats.hardware_frames++;
            perf_stats.drm_prime_frames++;
            queued_frame.storage = QueuedFrameStorage::kDrmPrime;
            queued_frame.hardware_frame = CloneFrameReference(decoded_frame);
            FillDrmPrimeMetadata(decoded_frame, queued_frame);
            if (queued_frame.hardware_frame == nullptr) {
              Logger::GetInstance().LogError(
                  "[decoder " + std::to_string(i) +
                  "] failed to retain DRM_PRIME frame reference.");
              av_frame_unref(decoded_frame);
              continue;
            }
          } else if (IsHardwarePixelFormat(decoded_pixel_format)) {
            perf_stats.hardware_frames++;
            Logger::GetInstance().LogError(
                "[decoder " + std::to_string(i) +
                "] received unsupported hardware pixel format: " +
                last_frame_format_name);
            av_frame_unref(decoded_frame);
            continue;
          } else {
            perf_stats.software_frames++;
            Logger::GetInstance().LogError(
                "[decoder " + std::to_string(i) +
                "] received software frame " + last_frame_format_name +
                ", but the current stitch path requires DRM_PRIME DMA-BUF input.");
            av_frame_unref(decoded_frame);
            continue;
          }

          {
            std::lock_guard<std::mutex> queue_lock(image_queue_mutex_vector_[i]);
            if (image_queue_vector_[i].size() >= max_queue_length_) {
              image_queue_vector_[i].pop();
              perf_stats.queue_drops++;
            }
            image_queue_vector_[i].push(std::move(queued_frame));
            perf_stats.queue_pushes++;
          }

          {
            std::lock_guard<std::mutex> stats_lock(decode_stats_mutex_);
            decoder_ready_vector_[i] = true;
            decoder_finished_vector_[i] = false;
            decoded_frames_since_report_[i]++;

            const auto now = std::chrono::steady_clock::now();
            const std::chrono::duration<double> elapsed =
                now - decode_report_time_vector_[i];
            if (elapsed.count() >= 1.0) {
              decode_fps_vector_[i] =
                  static_cast<double>(decoded_frames_since_report_[i]) /
                  elapsed.count();
              decoded_frames_since_report_[i] = 0;
              decode_report_time_vector_[i] = now;
            }
          }

          const auto now = std::chrono::steady_clock::now();
          const std::chrono::duration<double> perf_elapsed =
              now - perf_stats.report_time;
          if (perf_elapsed.count() >= 1.0) {
            Logger::GetInstance().Log(
                FormatDecoderPerfLog(i,
                                     codec_context->codec != nullptr
                                         ? codec_context->codec->name
                                         : "unknown",
                                     last_frame_format_name,
                                     perf_stats,
                                     perf_elapsed.count()));
            perf_stats = DecoderPerfStats{};
          }

          av_frame_unref(decoded_frame);
        }

        return true;
      };

      if (avformat_open_input(&format_context, file_name.c_str(), nullptr, nullptr) <
          0) {
        Logger::GetInstance().LogError("[decoder " + std::to_string(i) +
                                       "] failed to open input: " + file_name);
        std::lock_guard<std::mutex> stats_lock(decode_stats_mutex_);
        decoder_finished_vector_[i] = true;
        cleanup();
        return;
      }

      if (avformat_find_stream_info(format_context, nullptr) < 0) {
        Logger::GetInstance().LogError("[decoder " + std::to_string(i) +
                                       "] failed to read stream info: " +
                                       file_name);
        std::lock_guard<std::mutex> stats_lock(decode_stats_mutex_);
        decoder_finished_vector_[i] = true;
        cleanup();
        return;
      }

      video_stream_index =
          av_find_best_stream(format_context,
                              AVMEDIA_TYPE_VIDEO,
                              -1,
                              -1,
                              nullptr,
                              0);
      if (video_stream_index < 0) {
        Logger::GetInstance().LogError("[decoder " + std::to_string(i) +
                                       "] failed to find video stream: " +
                                       file_name);
        std::lock_guard<std::mutex> stats_lock(decode_stats_mutex_);
        decoder_finished_vector_[i] = true;
        cleanup();
        return;
      }

      AVStream* video_stream = format_context->streams[video_stream_index];
      const AVCodec* codec = FindPreferredDecoder(video_stream->codecpar->codec_id);
      if (codec == nullptr) {
        Logger::GetInstance().LogError(
            "[decoder " + std::to_string(i) + "] failed to find decoder.");
        std::lock_guard<std::mutex> stats_lock(decode_stats_mutex_);
        decoder_finished_vector_[i] = true;
        cleanup();
        return;
      }

      codec_context = avcodec_alloc_context3(codec);
      if (codec_context == nullptr) {
        Logger::GetInstance().LogError(
            "[decoder " + std::to_string(i) + "] failed to allocate decoder context.");
        std::lock_guard<std::mutex> stats_lock(decode_stats_mutex_);
        decoder_finished_vector_[i] = true;
        cleanup();
        return;
      }

      if (avcodec_parameters_to_context(codec_context, video_stream->codecpar) < 0) {
        Logger::GetInstance().LogError(
            "[decoder " + std::to_string(i) +
            "] failed to copy codec parameters to decoder context.");
        std::lock_guard<std::mutex> stats_lock(decode_stats_mutex_);
        decoder_finished_vector_[i] = true;
        cleanup();
        return;
      }

#ifdef ENABLE_RK_HARDWARE_DECODING
      codec_context->get_format = SelectDecoderPixelFormat;

      // Keep DRM_PRIME output linear for the current fallback path.
      // AFBC/non-linear surfaces cannot be downloaded by av_hwframe_transfer_data.
      av_opt_set(codec_context->priv_data, "afbc", "0", 0);
#endif

      if (avcodec_open2(codec_context, codec, nullptr) < 0) {
        Logger::GetInstance().LogError("[decoder " + std::to_string(i) +
                                       "] failed to open decoder: " +
                                       std::string(codec->name));
        std::lock_guard<std::mutex> stats_lock(decode_stats_mutex_);
        decoder_finished_vector_[i] = true;
        cleanup();
        return;
      }

      packet = av_packet_alloc();
      decoded_frame = av_frame_alloc();

      if (packet == nullptr || decoded_frame == nullptr) {
        Logger::GetInstance().LogError(
            "[decoder " + std::to_string(i) +
            "] failed to allocate packet or frame buffers.");
        std::lock_guard<std::mutex> stats_lock(decode_stats_mutex_);
        decoder_finished_vector_[i] = true;
        cleanup();
        return;
      }

#ifdef ENABLE_RK_HARDWARE_DECODING
      Logger::GetInstance().Log("[decoder " + std::to_string(i) +
                                "] hardware decode requested, actual decoder=" +
                                std::string(codec->name) +
                                " size=" + std::to_string(codec_context->width) +
                                "x" + std::to_string(codec_context->height) +
                                " requested_output=DRM_PRIME");
      if (std::string(codec->name).find("rkmpp") == std::string::npos) {
        Logger::GetInstance().LogError(
            "[decoder " + std::to_string(i) +
            "] hardware decode was requested but FFmpeg selected a non-rkmpp decoder.");
      }
#else
      Logger::GetInstance().Log(
          "[decoder " + std::to_string(i) + "] FFmpeg software decoding enabled with " +
          std::string(codec->name) +
          " size=" + std::to_string(codec_context->width) +
          "x" + std::to_string(codec_context->height));
#endif

      while (!stop_requested_) {
        const double read_t0 = cv::getTickCount();
        const int read_ret = av_read_frame(format_context, packet);
        perf_stats.read_seconds +=
            (cv::getTickCount() - read_t0) / cv::getTickFrequency();
        if (read_ret == AVERROR_EOF) {
          avcodec_send_packet(codec_context, nullptr);
          if (!receive_and_queue_frames()) {
            break;
          }
          avcodec_flush_buffers(codec_context);
          if (av_seek_frame(format_context,
                            video_stream_index,
                            0,
                            AVSEEK_FLAG_BACKWARD) < 0) {
            Logger::GetInstance().LogError(
                "[decoder " + std::to_string(i) + "] failed to loop video: " +
                file_name);
            break;
          }
          continue;
        }

        if (read_ret < 0) {
          Logger::GetInstance().LogError(
              "[decoder " + std::to_string(i) + "] read frame failed: " +
              AvErrorToString(read_ret));
          break;
        }

        if (packet->stream_index == video_stream_index) {
          perf_stats.packets_read++;
          const double send_t0 = cv::getTickCount();
          const int send_ret = avcodec_send_packet(codec_context, packet);
          perf_stats.send_seconds +=
              (cv::getTickCount() - send_t0) / cv::getTickFrequency();
          if (send_ret < 0) {
            Logger::GetInstance().LogError(
                "[decoder " + std::to_string(i) + "] send packet failed: " +
                AvErrorToString(send_ret));
            av_packet_unref(packet);
            break;
          }
          perf_stats.packets_sent++;

          if (!receive_and_queue_frames()) {
            av_packet_unref(packet);
            break;
          }
        }

        av_packet_unref(packet);
      }

      {
        std::lock_guard<std::mutex> stats_lock(decode_stats_mutex_);
        decoder_finished_vector_[i] = true;
      }

      cleanup();
    });
  }
}

/**
 * @brief 停止解码线程
 * 所有解码线程的停止信号，等待线程结束
 */
void SensorDataInterface::StopDecodeThreads() {
  stop_requested_ = true;
  for (std::thread& decode_thread : decode_threads_) {
    if (decode_thread.joinable()) {
      decode_thread.join();
    }
  }
  decode_threads_.clear();
  decode_threads_started_ = false;
}

/**
 * @brief 记录视频
 * 启动解码线程（与InitVideoCapture类似功能）
 */
void SensorDataInterface::RecordVideos() {
  StartDecodeThreads();
}

/**
 * @brief 从缓冲队列提取帧
 * 阻塞地等待每个通道的缓冲队列不空，然后获取帧并转换为QueuedFrame
 * @param frame_vector 输出的帧向量
 */
void SensorDataInterface::get_frame_vector(std::vector<QueuedFrame>& frame_vector) {
  frame_vector.resize(num_img_);
  for (size_t i = 0; i < num_img_; ++i) {
    while (true) {
      bool has_frame = false;
      bool decoder_finished = false;
      QueuedFrame queued_frame;

      {
        std::lock_guard<std::mutex> queue_lock(image_queue_mutex_vector_[i]);
        if (!image_queue_vector_[i].empty()) {
          queued_frame = std::move(image_queue_vector_[i].front());
          image_queue_vector_[i].pop();
          has_frame = true;
        }
      }

      if (has_frame) {
        frame_vector[i] = std::move(queued_frame);
        break;
      }

      {
        std::lock_guard<std::mutex> stats_lock(decode_stats_mutex_);
        decoder_finished = decoder_finished_vector_[i];
      }

      if (decoder_finished) {
        Logger::GetInstance().LogError(
            "[decoder " + std::to_string(i) +
            "] decoder stopped and queue is empty, cannot provide input frame.");
        frame_vector[i] = QueuedFrame{};
        break;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

/**
 * @brief 将QueuedFrame转换为DMA-BUF格式
 * 提取DRM_PRIME帧的DMA-buf文件描述符与NV12帧数据信息
 * @param queued_frame 源处理队列帧结构
 * @param frame 输出的NV12帧
 * @param channel_index 通道索引（用于错误日志）
 * @return 如果转换成功返回true，否则返回false
 */
bool SensorDataInterface::ConvertQueuedFrameToDmabuf(const QueuedFrame& queued_frame,
                                                     NV12Frame& frame,
                                                     size_t channel_index) {
  frame = NV12Frame{};
  if (queued_frame.storage == QueuedFrameStorage::kDrmPrime) {
    if (queued_frame.dma_buf_fd < 0 || queued_frame.hardware_frame == nullptr) {
      Logger::GetInstance().LogError(
          "[decoder " + std::to_string(channel_index) +
          "] DRM_PRIME frame missing dma_buf_fd or frame reference.");
      return false;
    }

    frame.fd = queued_frame.dma_buf_fd;
    frame.width = queued_frame.width;
    frame.height = queued_frame.height;
    frame.stride_w = queued_frame.stride_w > 0 ? queued_frame.stride_w : queued_frame.width;
    frame.stride_h = queued_frame.stride_h > 0 ? queued_frame.stride_h : queued_frame.height;
    frame.owner = queued_frame.hardware_frame;
    return true;
  }

  return false;
}

/**
 * @brief 获取所有通道的NV12帧
 * 内部调用get_frame_vector()获取QueuedFrame，然后转换为NV12Frame
 * @param image_vector 输出的NV12帧向量
 */
void SensorDataInterface::get_image_vector(std::vector<NV12Frame>& image_vector) {
  std::vector<QueuedFrame> frame_vector(num_img_);
  get_frame_vector(frame_vector);

  image_vector.resize(num_img_);
  for (size_t i = 0; i < num_img_; ++i) {
    NV12Frame frame;
    ConvertQueuedFrameToDmabuf(frame_vector[i], frame, i);
    image_vector[i] = std::move(frame);
  }
}

/**
 * @brief 获取解码FPS快照
 * 线程安全的获取最近一次解码帧速率统计
 * @return 包含每个通道FPS的double向量
 */
std::vector<double> SensorDataInterface::GetDecodeFpsSnapshot() {
  std::lock_guard<std::mutex> stats_lock(decode_stats_mutex_);
  return decode_fps_vector_;
}
