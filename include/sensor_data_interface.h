//
// Created by s1nh.org on 2020/11/11.
// v3.0 (2026-07-08): 加 Type::kRtsp + 7 个 rtsp 字段. dataset/mipi 退役.
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

#include "gst_mpp_decoder.h"

extern "C" {
struct AVFrame;
struct _GstSample;
}

enum class QueuedFrameStorage {
    kEmpty = 0,
    kSoftwareNV12 = 1,
    kDrmPrime = 2,
    kBlackFrame = 3,   // Sprint 4-MIPI: 缺设备/打开失败时占位, NV12 全 0, RGA 零拷贝用.
};

// Sprint 4-MIPI (2026-07-15): 黑图占位帧的 holder. 持有一个一次性分配的
//   DMA-BUF NV12 buffer (DrmBuffer), lifetime 贯穿队列. 比 gst_sample 简单:
//   不走 gstreamer, 直接 drm_alloc_nv12 + memset 0 + 后台线程 30 fps 推同
//   一块; 多个 consumer (stitcher) 各持 shared_ptr<BlackFrameHolder>,
//   全部释放后才 drm_free.
#include "drm_allocator.h"
struct BlackFrameHolder {
    DrmBuffer drm;
    int width  = 0;
    int height = 0;
    int dma_buf_fd()  const { return drm.fd; }
    int pitch_px()    const { return drm.pitch; }
    ~BlackFrameHolder();  // drm_free 析构 (defined in sensor_data_interface.cc)
};

// v3.0 (2026-07-08) 输入源描述. 加载自 params/camera_sources.yaml.
// kFile: 本地 H.264 mp4 (dev/test/devops 用)
// kMipi: 占位 (阶段 3 MIPI 计划已退役, 不再开新功能; 保留 enum 防 yaml 兼容)
// kRtsp: ★ 网络摄像头 RTSP 拉流 (生产)
struct CameraSource {
    enum class Type {
        kFile,    // 本地视频文件 (走 gstreamer filesrc → qtdemux → h264parse → mppvideodec)
        kMipi,    // MIPI camera V4L2 节点 (走 gstreamer v4l2src + ISP → NV12 DMA-BUF → appsink)
        kRtsp,    // ★ 网络摄像头 RTSP 拉流 (走 rtspsrc → rtph264depay → h264parse → mppvideodec)
    };

    Type type = Type::kFile;
    std::string uri;              // file: 路径 / rtsp: rtsp://... / mipi: /dev/videoN
    int width = 0;
    int height = 0;
    int fps = 30;
    std::string pixel_format;     // "NV12" (期望, 仅日志用). MIPI 通常已经是 NV12 (ISP 输出).

    // Sprint 4-MIPI (2026-07-15) MIPI 字段. 仅 is_mipi() 时生效.
    //   device_path: V4L2 节点绝对路径, e.g. "/dev/video11".
    //   driver 输出格式默认 NV12 (rockchip vendor SDK ISP pipeline 通常已经 NV12).
    //   io_mode: dmabuf (零拷贝, 仅要求 sensor driver 支持) 或 mmap (兜底).
    std::string device_path;       // mipi: /dev/videoN  (仅 is_mipi() 生效)
    std::string io_mode = "dmabuf"; // mipi: "dmabuf" / "mmap"
    int v4l2_buffer_count = 4;     // mipi: v4l2src num-buffers (1..8)
    bool use_isp_pipeline = false; // mipi: true 时把 camera 输出先经 rockchip ISP (rkvideoconvert)
                                   //      false (默认) 则假定 device 节点已经是 NV12 零拷贝直通.

    // ★ v3.0 RTSP 字段. 仅 is_rtsp() 时生效.
    std::string user_id;          // 空 = 无认证
    std::string user_pw;
    int latency_ms = 100;         // gstreamer rtspsrc latency (jitter buffer 预算)
    bool use_tcp = true;          // rtspsrc protocols: 0x4=tcp+tcp+http (true) 或 0x7=udp (false)
    int connect_timeout_s = 10;   // rtspsrc 信令 + TCP connect 超时 (秒)
    int retry_attempts = 5;       // rtspsrc RTC 重试, 0 = 无限
    int reconnect_backoff_ms = 2000;  // 断线重连退避
    double frame_drop_threshold = 0.8;  // fps / 期望 fps 低于此值告警

    bool is_mipi() const { return type == Type::kMipi; }
    bool is_file() const { return type == Type::kFile; }
    bool is_rtsp() const { return type == Type::kRtsp; }
};

// 全局输入源列表 (启动时由 SensorDataInterface 填充, 供 app.cc / status_writer 路由).
// 默认空 (fallback 到原 t50..t53.mp4 数据集路径, 仅 INPUT_SOURCE_MODE=dataset).
struct CameraSourceList {
    std::vector<CameraSource> cameras;
    int sync_window_ms = 16;       // v3.0: 6 路 PTS 拉齐窗, 1 帧@60fps. 0 = 不要求同步.
    bool auto_calibrate = false;   // Sprint 2 自标定开关 (默认 off)
};

// ★ v3.0 (2026-07-08) 全局入口: status_writer.cc / http_server.cc 等需要看 yaml 解析结果时用
//   旧 API 直接读 file 但 yaml 解析在 sensor_data_interface.cc 一次性, 这里暴露只读引用.
const CameraSourceList& GetCameraSourceList();

struct QueuedFrame {
    QueuedFrameStorage storage = QueuedFrameStorage::kEmpty;
    // 旧路径: FFmpeg AVFrame (DRM_PRIME 包装). 当前 dataset 路径已不再用, 保留给
    // 极端 fallback. v2.4 (2026-07-06) 后默认走 gstreamer-rockchip path.
    std::shared_ptr<AVFrame> hardware_frame;
    // 新路径: gstreamer GstSample (DMA-BUF backed buffer, 零拷贝). 一帧解码完
    // 由 gst_mpp_decoder 持有, sample 析构时 gstreamer 自动还 buffer. dma_buf_fd
    // 是 sample 中 GstBuffer 第一个 DMA-BUF 内存的 fd, 给 stitcher 直接 RGA 用.
    std::shared_ptr<_GstSample> gst_sample;
    // Sprint 4-MIPI: 占位黑帧 holder. storage == kBlackFrame 时由 black_holder
    //   持有一块一次性分配的 NV12 DMA-BUF (全 0), 析构时自动 drm_free.
    std::shared_ptr<BlackFrameHolder> black_holder;
    int width = 0;
    int height = 0;
    int stride_w = 0;
    int stride_h = 0;
    int pixel_format = -1;
    int dma_buf_fd = -1;
    int drm_layer_count = 0;

    bool empty() const {
        if (storage == QueuedFrameStorage::kDrmPrime) {
            return hardware_frame == nullptr && gst_sample == nullptr;
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

    // Sprint 4-MIPI (2026-07-15): 启动期探测的结果.
    //   camera_actually_started_[i] == true  -> v4l2src 正常解码
    //   camera_actually_started_[i] == false -> 该路是黑图占位 (缺设备 / 打开失败)
    const std::vector<bool>& camera_actually_started() const {
        return camera_actually_started_;
    }
    size_t num_started_real() const;

 private:
    void StartDecodeThreads();
    void StopDecodeThreads();

    const size_t max_queue_length_;
    size_t num_img_;
    size_t frame_idx;
    std::vector<std::queue<QueuedFrame>> image_queue_vector_;
    std::vector<std::mutex> image_queue_mutex_vector_;
    std::vector<cv::VideoCapture> video_capture_vector_;
    // v3.0: 用 vector<CameraSource> 替代 vector<string>. 保留 video_file_paths_ 作为
    // 旧 thread log 兼容 (新 code 走 video_sources_).
    std::vector<std::string> video_file_paths_;
    std::vector<CameraSource> video_sources_;   // ★ v3.0: 与 decode_threads_ 一一对应
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

    // Sprint 4-MIPI (2026-07-15): 启动结果表 + 失败原因, 给 App::App() 启动期 log / status_writer 用.
    std::vector<bool> camera_actually_started_;
    std::vector<std::string> camera_startup_reason_;  // "v4l2src-ok" / "no-device" / "open-fail:..." / "v4l2-timeout"

    // 静态黑帧占位线程. 进入点是 StartDecodeThreads per-thread lambda 在
    //   mipi 路径 StartMipi 失败时调用. Mark finished on exit.
    static void BlackFramePushLoop(size_t i,
                                    std::shared_ptr<BlackFrameHolder> holder,
                                    SensorDataInterface* self);
};

#endif  // IMAGE_STITCHING_SENSOR_DATA_INTERFACE_H