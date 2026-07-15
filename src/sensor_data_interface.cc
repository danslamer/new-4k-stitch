//
// Created by s1nh.org on 11/11/20.
// v2.4 (2026-07-06): FFmpeg rkmpp 路径已废弃 (vendor 不维护, 0 帧).
// 改走 gstreamer1.0-rockchip1 mppvideodec (vendor SDK 标准组件).
//

/**
 * @file sensor_data_interface.cc
 * @brief 传感器数据接口实现
 * 负责多路视频流的获取、gstreamer-rockchip 硬件解码和帧队列管理
 * 输出 DMA-BUF backed NV12 帧, 直接给 stitcher 走 RGA 零拷贝
 *
 * ⚠️ 架构硬约束 (2026-07-06 锁定):
 *   - 6 路 2K 拼接要求 30 FPS 端到端, 软件解码 (mpeg4 sw) 性能远不够
 *   - 整条管线必须是 硬件解码 (gstreamer mppvideodec) + DMA-BUF 零拷贝
 *   - appsink 收到非 DMA-BUF 帧 (sw path) 必须直接 reject + 报错, 不能 fallback
 *   - 任何"暂时 sw 解码跑一下看效果"的提案都不应合入
 *   - 历史 FFmpeg rkmpp 路径: vendor 不维护 + 0 帧 (jellyfin-mpp 1.3.9 与板子 SDK 1.5.0 mpp_service ABI 不兼容), 已废弃
 *   - 详见 AGENTS.md "架构硬约束"
 */

/**
 * 模块 API:
 * InitExampleImages() - 初始化示例图像（未使用）
 * InitVideoCapture() - 初始化视频捕获和解码线程
 * StartDecodeThreads() - 启动解码线程 (gstreamer mppvideodec)
 * StopDecodeThreads() - 停止解码线程
 * RecordVideos() - 记录视频
 * get_frame_vector() - 从缓冲队列提取帧
 * ConvertQueuedFrameToDmabuf() - 将 QueuedFrame 转换为 NV12Frame (DMA-BUF)
 * get_image_vector() - 获取所有通道的 NV12 帧
 * GetDecodeFpsSnapshot() - 获取解码 FPS 快照
 */

#include "sensor_data_interface.h"

#include "gst_mpp_decoder.h"
#include "logger.h"
#include "status_writer.h"
#include "drm_allocator.h"
#include <sys/mman.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>
#include <utility>

namespace {

/**
 * @brief v2.4 (2026-07-06) FFmpeg 路径已废弃. AvErrorToString / IsHardwarePixelFormat /
 *        FindPreferredDecoder / FillDrmPrimeMetadata / SelectDecoderPixelFormat /
 *        FormatDecoderPerfLog 全部删除, gstreamer path 不再用.
 *   之前用 FFmpeg rkmpp 走 mpeg4/h264 解码, 但 vendor 不维护 rkmpp wrapper,
 *   且 jellyfin-mpp 1.3.9 跟板子 vendor SDK 1.5.0 mpp_service ABI 不兼容, 0 帧.
 *   新路径: GstMppDecoder (gst_parse_launch + mppvideodec dma-feature=true) →
 *   appsink 拿 NV12/DMABuf → 直接 dma_buf_fd 给 stitcher.
 */

/**
 * @struct DecoderPerfStats
 * @brief 解码器性能统计 (gstreamer path 精简版).
 *        1s 间隔刷 fps / queue 计数日志.
 */
struct DecoderPerfStats {
  size_t frames_decoded = 0;
  size_t hardware_frames = 0;
  size_t drm_prime_frames = 0;
  size_t queue_pushes = 0;
  size_t queue_drops = 0;
  std::chrono::steady_clock::time_point report_time =
      std::chrono::steady_clock::now();
};

}  // namespace

/**
 * @class SensorDataInterface
 * @brief 传感器数据接口类
 * 负责管理多路视频解码且框管理
 */

// Sprint 4-MIPI (2026-07-15): BlackFrameHolder + 黑帧占位线程
BlackFrameHolder::~BlackFrameHolder() {
    if (drm.fd >= 0) {
        drm_free(drm);  // drm_allocator.cc: 关 drm_fd, reset
        drm.fd = -1;
    }
}

#include <cstring>  // memset
// 一次性分配一块 NV12 DMA-BUF, mmap 后 memset 全 0. 失败返回 nullptr.
static std::shared_ptr<BlackFrameHolder> MakeZeroBlackFrame(int w, int h) {
    auto holder = std::make_shared<BlackFrameHolder>();
    holder->width  = w;
    holder->height = h;
    if (drm_alloc_nv12(w, h, holder->drm) != 0) {
        Logger::GetInstance().LogError(
            "[BlackFrame] drm_alloc_nv12 failed for black placeholder " +
            std::to_string(w) + "x" + std::to_string(h));
        return nullptr;
    }
    void* ptr = drm_map(holder->drm);
    if (ptr != MAP_FAILED) {
        const size_t y_size  = static_cast<size_t>(holder->drm.pitch) * static_cast<size_t>(h);
        const size_t uv_size = static_cast<size_t>(holder->drm.pitch) * static_cast<size_t>(h / 2);
        std::memset(ptr, 0, y_size + uv_size);
        drm_unmap(holder->drm, ptr);
    }
    return holder;
}
}

size_t SensorDataInterface::num_started_real() const {
    size_t n = 0;
    for (bool b : camera_actually_started_) if (b) ++n;
    return n;
}

// Sprint 4-MIPI (2026-07-15): 30 fps 推一块全 0 NV12 frame. 直到 stop_requested_.
//   引用 self->image_queue_vector_ / mutex / decoder_ready/fps 向量.
void SensorDataInterface::BlackFramePushLoop(size_t i,
                                             std::shared_ptr<BlackFrameHolder> holder,
                                             SensorDataInterface* self) {
    Logger::GetInstance().Log(
        "[BlackFrame] cam" + std::to_string(i) +
        " placeholder running, w=" + std::to_string(holder->width) +
        " h=" + std::to_string(holder->height) +
        " fd=" + std::to_string(holder->dma_buf_fd()));
    auto frame_period = std::chrono::milliseconds(33);
    auto next_push    = std::chrono::steady_clock::now() + frame_period;
    size_t pushed = 0;
    auto last_log = std::chrono::steady_clock::now();
    while (!self->stop_requested_.load()) {
        if (std::chrono::steady_clock::now() >= next_push) {
            QueuedFrame qf;
            qf.storage      = QueuedFrameStorage::kBlackFrame;
            qf.width        = holder->width;
            qf.height       = holder->height;
            qf.stride_w     = holder->pitch_px();
            qf.stride_h     = holder->height;
            qf.dma_buf_fd   = holder->dma_buf_fd();
            qf.black_holder = holder;

            {
                std::lock_guard<std::mutex> queue_lock(
                    self->image_queue_mutex_vector_[i]);
                if (self->image_queue_vector_[i].size() >= self->max_queue_length_) {
                    self->image_queue_vector_[i].pop();
                }
                self->image_queue_vector_[i].push(std::move(qf));
            }
            ++pushed;
            next_push += frame_period;

            // 1s log + 30 fps 占位 + ready 标记
            auto now = std::chrono::steady_clock::now();
            if (now - last_log >= std::chrono::seconds(1)) {
                std::lock_guard<std::mutex> stats_lock(self->decode_stats_mutex_);
                self->decoder_ready_vector_[i] = true;
                self->decoded_frames_since_report_[i] = pushed;
                self->decode_report_time_vector_[i] = now;
                if (i < self->decode_fps_vector_.size())
                    self->decode_fps_vector_[i] = 30.0;
                std::ostringstream ss;
                ss << "[decoder_perf " << i << "]"
                   << " decoder=blackframe"
                   << " frame_fmt=NV12/DMABuf"
                   << " frames=" << pushed
                   << " fps=" << 30.0
                   << " uri=black-placeholder fd=" << holder->dma_buf_fd();
                Logger::GetInstance().Log(ss.str());
                last_log = now;
                stitch_status::set_online(static_cast<int>(i), 0);  // 0 = placeholder 走的
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    std::lock_guard<std::mutex> stats_lock(self->decode_stats_mutex_);
    self->decoder_finished_vector_[i] = true;
    Logger::GetInstance().Log(
        "[BlackFrame] cam" + std::to_string(i) +
        " placeholder stopped, total pushed=" + std::to_string(pushed));
}

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

//
// v2.5 (2026-07-08) URI canonicalizer: 板端 gstreamer filesrc 不接受 .. 形式相对路径,
// 启动阶段会报 "No such file" 直接 abort. 这里把 yaml uri 和 default
// `../datasets/2k-test-h264/` 都规范成 CWD+uri 的绝对路径再喂 mpp decoder.
// 注: 不用 std::filesystem 因为本项目 CMakeLists 锁在 C++11, std::filesystem 是 C++17.
// 用 POSIX realpath() 实现.
//
// v3.0 (2026-07-08): scheme-aware canonicalizer.
//   - `rtsp://...` / `http(s)://...` / `file://...`: 原样返回, 不走 realpath
//   - 本地路径: 走 POSIX realpath() 把 ../ 形式解析成 CWD+uri 绝对路径
//     (板上 gstreamer filesrc 不接受 .. 形式相对路径, 会报 No such file 直接 abort)
//   - 显式 type 参数让 caller 告诉本路是 file/rtsp; 没 type 时按 uri scheme 检测.
// v2.5 (2026-07-08) 已加 realpath() 基础版, 但 rtsp:// 误走 realpath 会 NULL 丢路径, 此处分流.
// 注: 项目锁 C++11, 不能用 std::filesystem (C++17).
static std::string CanonicalizeUri(const std::string& uri, CameraSource::Type type) {
  if (uri.empty()) return uri;
  // 先按 scheme 前缀快速判定 (覆盖把 rtsp 当 file 配的场景)
  auto starts_with = [&](const std::string& prefix) {
    return uri.rfind(prefix, 0) == 0;
  };
  if (starts_with("rtsp://") || starts_with("rtsp:")
      || starts_with("http://") || starts_with("https://")
      || starts_with("file://")) {
    return uri;
  }
  // type 显式是 rtsp 也走原样
  if (type == CameraSource::Type::kRtsp) {
    return uri;
  }
  // file: realpath
  bool trailing_slash = !uri.empty() && uri.back() == '/';
  char resolved[PATH_MAX];
  if (realpath(uri.c_str(), resolved) != nullptr) {
    std::string r(resolved);
    if (trailing_slash && !r.empty() && r.back() != '/') r.push_back('/');
    return r;
  }
  Logger::GetInstance().LogError(
      "[sensor_data_interface] CanonicalizeUri: realpath failed, keep as-is: " + uri);
  return uri;
}
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

      // v3.0 (2026-07-08): 解析 type: file | rtsp | mipi (deprecated).
      std::string type_str;
      cam["type"] >> type_str;
      if (type_str == "rtsp") {
        src.type = CameraSource::Type::kRtsp;
      } else if (type_str == "mipi") {
        src.type = CameraSource::Type::kMipi;
      } else {
        src.type = CameraSource::Type::kFile;  // 默认 + dev/test fallback
      }

      // v3.0: 拿 raw_uri, 先按 type 分流 canonicalize. rtsp URL 不走 realpath.
      { std::string raw_uri; cam["uri"] >> raw_uri; src.uri = CanonicalizeUri(raw_uri, src.type); }
      cam["width"]  >> src.width;
      cam["height"] >> src.height;
      cam["fps"]    >> src.fps;
      cam["pixel_format"] >> src.pixel_format;

      // v3.0: rtsp 字段 (空 type 时跳过, 让 default 生效)
      if (src.is_rtsp()) {
        cam["user_id"] >> src.user_id;
        cam["user_pw"] >> src.user_pw;
        if (cam["latency_ms"].isInt())             cam["latency_ms"]             >> src.latency_ms;
        if (cam["connect_timeout_s"].isInt())      cam["connect_timeout_s"]      >> src.connect_timeout_s;
        if (cam["retry_attempts"].isInt())         cam["retry_attempts"]         >> src.retry_attempts;
        if (cam["reconnect_backoff_ms"].isInt())   cam["reconnect_backoff_ms"]   >> src.reconnect_backoff_ms;
        if (cam["frame_drop_threshold"].isReal())  cam["frame_drop_threshold"]  >> src.frame_drop_threshold;
        else if (cam["frame_drop_threshold"].isInt())
                                                  cam["frame_drop_threshold"]  >> src.frame_drop_threshold;
        // bool 缺省: YAML 缺字段时 OpenCV 读 false, 与默认 true 矛盾, 手动处理.
        if (!cam["use_tcp"].empty()) {
          int v = 0; cam["use_tcp"] >> v; src.use_tcp = (v != 0);
        }
        // 显式日志: 让运维一眼看见 rtsp 拉的是谁
        Logger::GetInstance().Log(
            "[sensor_data_interface] cam " + std::to_string(g_camera_source_list.cameras.size()) +
            " rtsp: " + src.uri + " (uid='" + src.user_id + "' latency=" +
            std::to_string(src.latency_ms) + "ms use_tcp=" +
            (src.use_tcp ? "tcp" : "udp") + ")");
      }

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

// 默认 6 路数据集 (v2.4 阶段: 2K 2560x1440, 2x3 布局).
// v2.4 (2026-07-06): 走 gstreamer-rockchip mppvideodec 路径, 只支持 H.264.
// 原始 2k-test/ 是 MPEG-4 (mp4v), mppvideodec 不支持 MPEG-4 part 2, 一次
// 性转码到 2k-test-h264/ 走 h264_rkmpp 硬件编码器 (transcode 命令见 HISTORY.md).
// 真实 6-camera-rig 录的视频如果是 H.264, 直接放 2k-test-h264/ 即可.
// 4K 源在 datasets/4k-test/, 用 tools/downscale_4k_to_2k.py 降下来.
// 注: t40/t41 是临时占位, 后续用真实 6 路替换. 阶段 6 真机跑通前需替换.
static std::vector<CameraSource> LoadDefaultDatasetSources() {
  const std::string video_dir = CanonicalizeUri("../datasets/2k-test-h264/", CameraSource::Type::kFile);
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
      std::to_string(sources.size()) + " file source(s) from " + video_dir +
      " (H.264 pre-transcoded, decode via gstreamer mppvideodec)");
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

  // Sprint 4-MIPI (2026-07-15): mipi 不再被过滤, 直接进 file_sources
  //   (decode thread 自己按 is_rtsp() / is_mipi() 分发). 缺设备/打开失败的
  //   mipi 路径在 StartDecodeThreads 内自动降级到 BlackFrameProvider 线程.
  std::vector<CameraSource> file_sources;
  for (const auto& src : effective_sources) {
    if (src.is_rtsp()) continue;  // rtsp 走自己的 StartRtsp 路径
    file_sources.push_back(src);
  }
  if (file_sources.empty()) {
    Logger::GetInstance().LogError(
        "[sensor_data_interface] no usable file/mipi sources, num_img=0");
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

  // v3.0: 同时填 video_file_paths_ (旧 thread log 兼容) + video_sources_ (新 thread 路由).
  for (size_t i = 0; i < num_img_; ++i) {
    video_file_paths_.push_back(file_sources[i].uri);
    video_sources_.push_back(file_sources[i]);
  }

  // v3.0: 打印实际路由结果, 让运维一眼看出 6 路是 file 还是 rtsp.
  std::ostringstream route_ss;
  route_ss << "[sensor_data_interface] InitVideoCapture routes " << num_img_
           << " cam(s):";
  for (size_t i = 0; i < num_img_; ++i) {
    route_ss << " cam" << i << "="
             << (video_sources_[i].is_rtsp() ? "rtsp" :
                 video_sources_[i].is_mipi() ? "mipi" : "file");
  }
  Logger::GetInstance().Log(route_ss.str());

  // Sprint 4-MIPI: 启动结果表 + 失败原因, 由 StartDecodeThreads 写完后再 log.
  camera_actually_started_.assign(num_img_, false);
  camera_startup_reason_.assign(num_img_, std::string{});

  StartDecodeThreads();
}

// 见 SensorDataInterface::BlackFramePushLoop (static member; 访问 private fields).


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
      const CameraSource& src = video_sources_[i];
      DecoderPerfStats perf_stats;

      Logger::GetInstance().Log(
          "[decoder " + std::to_string(i) +
          "] gstreamer-rockchip mppvideodec path (" +
          (src.is_rtsp() ? "rtsp" : src.is_mipi() ? "mipi" : "file") +
          ", vendor SDK mpp 1.5.0): " + file_name);

      // v3.0 (2026-07-08) decoder dispatch:
      //   - file: GstMppDecoder::Start(uri) → filesrc + qtdemux + h264parse + mppvideodec
      //   - rtsp: GstMppDecoder::StartRtsp(uri, uid, pw, opts) → rtspsrc + rtph264depay +
      //     h264parse + mppvideodec + 内置 watchdog 重连
      //   - mipi: 已退役, 不应再到这里 (Sanity log + 标 finished)
      image_stitching::GstMppDecoder decoder;
      bool started = false;
      std::string started_kind;
      if (src.is_rtsp()) {
        image_stitching::RtspOptions opts;
        opts.latency_ms         = src.latency_ms;
        opts.use_tcp            = src.use_tcp;
        opts.connect_timeout_s  = src.connect_timeout_s;
        opts.retry_attempts     = src.retry_attempts;
        opts.reconnect_backoff_ms = src.reconnect_backoff_ms;
        started = decoder.StartRtsp(
            src.uri, src.user_id, src.user_pw, opts,
            /*expected_w*/ 2560, /*expected_h*/ 1440);
        started_kind = "rtsp";
      } else if (src.is_mipi()) {
        // Sprint 4-MIPI: v4l2src + capsfilter (NV12 DMA-BUF) + appsink.
        started = decoder.StartMipi(
            src.device_path.empty() ? src.uri : src.device_path,
            src.io_mode.empty() ? "dmabuf" : src.io_mode,
            src.v4l2_buffer_count,
            /*expected_w*/ 2560, /*expected_h*/ 1440);
        started_kind = "mipi";
      } else {
        started = decoder.Start(file_name, /*expected_w*/ 2560, /*expected_h*/ 1440);
        started_kind = "file";
      }

      if (!started) {
        // Sprint 4-MIPI: mipi 路径启动失败不退出线程, 改走 BlackFramePushLoop 占位.
        if (src.is_mipi()) {
          Logger::GetInstance().LogError(
              "[decoder " + std::to_string(i) +
              "] mipi start failed, falling back to BlackFrame placeholder (device=" +
              (src.device_path.empty() ? src.uri : src.device_path) + ")");
          int w = (src.width  > 0) ? src.width  : 2560;
          int h = (src.height > 0) ? src.height : 1440;
          auto black = MakeZeroBlackFrame(w, h);
          if (!black) {
            std::lock_guard<std::mutex> stats_lock(decode_stats_mutex_);
            decoder_finished_vector_[i] = true;
            return;
          }
          camera_actually_started_[i] = false;
          camera_startup_reason_[i] =
              std::string("mipi-placeholder:") +
              (src.device_path.empty() ? src.uri : src.device_path);
          stitch_status::set_online(static_cast<int>(i), 0);
          SensorDataInterface::BlackFramePushLoop(i, black, this);
          return;
        }
        Logger::GetInstance().LogError(
            "[decoder " + std::to_string(i) +
            "] failed to start gstreamer pipeline (" + started_kind +
            "): " + file_name);
        stitch_status::set_online(static_cast<int>(i), 0);
        std::lock_guard<std::mutex> stats_lock(decode_stats_mutex_);
        decoder_finished_vector_[i] = true;
        return;
      }
      // v3.0: pipeline 已起, 标记 online. watchdog 后续异常时会切回 0.
      camera_actually_started_[i] = true;
      camera_startup_reason_[i] = started_kind + "-ok";
      stitch_status::set_online(static_cast<int>(i), 1);

      // gstreamer pull 循环. file EOS 走 Stop + Start 重启循环;
      // rtsp EOS 由 decoder 内部 watchdog 重连, 本线程只 sleep+continue (避免与 watchdog 双重启竞态).
      while (!stop_requested_) {
        image_stitching::GstMppFrame frame;
        const bool got_frame = decoder.PullFrame(frame);

        if (frame.is_eos || !got_frame) {
          if (src.is_rtsp()) {
            // v3.0: rtsp 走 internal watchdog. 这里只需 sleep 让它先 Stop+Start,
            // 然后 pull 自动出帧 (新 pipeline 已 PLAYING).
            Logger::GetInstance().Log(
                "[decoder " + std::to_string(i) +
                "] rtsp EOS / pull fail — waiting for watchdog reconnect (uri=" +
                src.uri + ")");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
          }
          // file: 重启 pipeline 走循环 (跟原 FFmpeg av_seek_frame 等价).
          Logger::GetInstance().Log(
              "[decoder " + std::to_string(i) + "] reached EOS, looping: " +
              file_name);
          decoder.Stop();
          if (!decoder.Start(file_name, 2560, 1440)) {
            Logger::GetInstance().LogError(
                "[decoder " + std::to_string(i) +
                "] failed to restart gstreamer pipeline for loop: " + file_name);
            break;
          }
          continue;
        }

        if (!frame.valid()) {
          Logger::GetInstance().LogError(
              "[decoder " + std::to_string(i) +
              "] invalid frame (no DMA-BUF fd), skipping: " + file_name);
          continue;
        }

        // 构造 QueuedFrame, gstreamer sample 持有 GstBuffer (DMA-BUF backed).
        // dma_buf_fd 给 stitcher 直接 RGA, 零拷贝.
        QueuedFrame queued_frame;
        queued_frame.storage = QueuedFrameStorage::kDrmPrime;
        queued_frame.gst_sample = frame.sample;
        queued_frame.width = frame.width;
        queued_frame.height = frame.height;
        queued_frame.stride_w = frame.stride_y > 0 ? frame.stride_y : frame.width;
        queued_frame.stride_h = frame.height;
        queued_frame.dma_buf_fd = frame.dma_buf_fd;
        queued_frame.drm_layer_count = 1;  // NV12 单 layer 跨 Y+UV planes
        queued_frame.pixel_format = 23;    // AV_PIX_FMT_NV12 (仅日志用)

        perf_stats.frames_decoded++;
        perf_stats.hardware_frames++;
        perf_stats.drm_prime_frames++;

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
          std::ostringstream ss;
          ss << "[decoder_perf " << i << "]"
             << " decoder=gst_mppvideodec"
             << " frame_fmt=NV12/DMABuf"
             << " frames=" << perf_stats.frames_decoded
             << " fps=" << (perf_elapsed.count() > 0.0
                                ? static_cast<double>(perf_stats.frames_decoded) /
                                      perf_elapsed.count()
                                : 0.0)
             << " hw_frames=" << perf_stats.hardware_frames
             << " drm_prime_frames=" << perf_stats.drm_prime_frames
             << " queue_pushes=" << perf_stats.queue_pushes
             << " queue_drops=" << perf_stats.queue_drops
             << " uri=" << file_name;
          Logger::GetInstance().Log(ss.str());
          perf_stats = DecoderPerfStats{};
        }
      }  // while !stop_requested_

      decoder.Stop();

      {
        std::lock_guard<std::mutex> stats_lock(decode_stats_mutex_);
        decoder_finished_vector_[i] = true;
      }
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

  // v2.4: 优先 gst_sample 路径 (gstreamer-rockchip mppvideodec, vendor 推荐).
  // gst_sample 持有 GstBuffer (DMA-BUF backed), 析构时 gstreamer 自动还 buffer.
  // NV12Frame.fd 直接给 RGA 零拷贝, NV12Frame.owner 防止 sample 提前析构.
  // Sprint 4-MIPI (2026-07-15): 占位黑帧路径 — 不经 gst_sample,
  //   直接从 black_holder 拿 dma_buf_fd. BlackFrameProvider 线程持有
  //   shared_ptr<BlackFrameHolder>, 在所有消费者释放前不释放底层 DMA-BUF.
  if (queued_frame.storage == QueuedFrameStorage::kBlackFrame) {
    if (queued_frame.black_holder == nullptr ||
        queued_frame.black_holder->dma_buf_fd < 0) {
      Logger::GetInstance().LogError(
          "[decoder " + std::to_string(channel_index) +
          "] black frame missing holder/fd.");
      return false;
    }
    frame.fd = queued_frame.black_holder->dma_buf_fd;
    frame.width  = queued_frame.black_holder->width;
    frame.height = queued_frame.black_holder->height;
    frame.stride_w = queued_frame.black_holder->pitch > 0
                      ? queued_frame.black_holder->pitch
                      : queued_frame.black_holder->width;
    frame.stride_h = queued_frame.black_holder->height;
    // NV12Frame.owner 暂未用; 但保留指针让 BlackFrameHolder 续命到 stitcher
    //   消费完 (此处 queued_frame 已被 get_image_vector 一次性 move 进 NV12Frame
    //   之外的局部, 所以生命周期由 caller 保证).
    return true;
  }

  if (queued_frame.storage == QueuedFrameStorage::kDrmPrime) {
    if (queued_frame.dma_buf_fd < 0) {
      Logger::GetInstance().LogError(
          "[decoder " + std::to_string(channel_index) +
          "] DRM_PRIME frame missing dma_buf_fd.");
      return false;
    }

    // 必须有 gst_sample 或 hardware_frame 之一持有 buffer 生命周期.
    if (queued_frame.gst_sample == nullptr && queued_frame.hardware_frame == nullptr) {
      Logger::GetInstance().LogError(
          "[decoder " + std::to_string(channel_index) +
          "] DRM_PRIME frame missing buffer holder (gst_sample/hardware_frame).");
      return false;
    }

    frame.fd = queued_frame.dma_buf_fd;
    frame.width = queued_frame.width;
    frame.height = queued_frame.height;
    frame.stride_w = queued_frame.stride_w > 0 ? queued_frame.stride_w : queued_frame.width;
    frame.stride_h = queued_frame.stride_h > 0 ? queued_frame.stride_h : queued_frame.height;

    // 优先用 gst_sample 持有 buffer, gst 路径新数据都走它. 老 hardware_frame 路径
    // 保留作极端 fallback (理论上 2026-07 后不应再走到).
    if (queued_frame.gst_sample != nullptr) {
      frame.owner_gst_sample = queued_frame.gst_sample;
    } else {
      frame.owner = queued_frame.hardware_frame;
    }
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


