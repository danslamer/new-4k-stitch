//
// gst_mpp_decoder.h - 用 gstreamer-rockchip 走 mppvideodec 硬件解 H.264
// 替代之前的 FFmpeg rkmpp wrapper (vendor 不维护, 解码器实际 0 帧).
// 输出: NV12 DMA-BUF (零拷贝) → stitcher 直接用 fd
//
// 2026-07-06 新增: vendor 确认 FFmpeg rkmpp 不支持, kmpp 解码器未完成,
// SDK 自带的 librockchip-mpp 1.5.0 (走 mpp_service 老接口) 是 vendor 官方推荐路径.
// gstreamer1.0-rockchip1 1.14-4 (含 mppvideodec) 是 vendor SDK 标准组件.
//
// v3.0 (2026-07-08) 新增 RTSP 拉流路径. rtspsrc 是 sometimes-pads, 必须动态
// 链接 rtph264depay (g_signal_connect "pad-added"). watchdog 线程负责 rtsp 断流
// 自动重连. appsink 输出 NV12 DMA-BUF 完全复用文件路径.
//

#ifndef IMAGE_STITCHING_GST_MPP_DECODER_H
#define IMAGE_STITCHING_GST_MPP_DECODER_H

#include <atomic>
#include <memory>
#include <string>
#include <thread>

extern "C" {
struct _GstElement;
struct _GstSample;
struct _GstBuffer;
struct _GstPad;
}

namespace image_stitching {

struct GstMppDeleter {
  void operator()(_GstSample* sample) const;
};

// 一帧 NV12 + 对应 DMA-BUF fd (零拷贝 path).
// dma_buf_fd < 0 表示失败/未拿到.
struct GstMppFrame {
  std::shared_ptr<_GstSample> sample;  // keep buffer alive
  int dma_buf_fd = -1;
  int width = 0;
  int height = 0;
  int stride_y = 0;  // Y plane stride
  int stride_uv = 0;  // UV plane stride
  bool is_eos = false;

  bool valid() const { return dma_buf_fd >= 0 && width > 0 && height > 0; }
};

// v3.0 (2026-07-08) rtsp 拉流参数.
// 用 yaml 配置填, 不暴露任何不可默认值.
struct RtspOptions {
  int latency_ms = 100;             // rtspsrc jitter buffer
  bool use_tcp = true;              // true=0x4 (tcp+tcp+http), false=0x7 (all)
  int connect_timeout_s = 10;       // rtspsrc 信令 + TCP connect 超时 (秒)
  int retry_attempts = 5;           // rtspsrc RTC 重试, 0 = 无限
  int reconnect_backoff_ms = 2000;  // 断线重连退避 (watchdog 用)
};

// 单个 pipeline 实例 (一个 IP cam 或一个本地 .mp4 对应一个实例).
// 用法:
//   GstMppDecoder dec;
//   dec.Start("datasets/2k-test-h264/t50.mp4");                  // file 路径
//   dec.StartRtsp("rtsp://...", "admin", "pwd", opts, 2560, 1440); // 拉流路径
//   while (dec.PullFrame(f)) { /* use f.dma_buf_fd */ }
//
class GstMppDecoder {
 public:
  GstMppDecoder();
  ~GstMppDecoder();

  GstMppDecoder(const GstMppDecoder&) = delete;
  GstMppDecoder& operator=(const GstMppDecoder&) = delete;

  // ★ 老 API 保留: 启动本地文件 pipeline. uri 走 filesrc/qtdemux/h264parse/mppvideodec.
  bool Start(const std::string& uri, int expected_w = 0, int expected_h = 0);

  // ★ 新 v3.0 API: 启动 RTSP 拉流 pipeline. 内置 watchdog 线程监控断流,
  // 自动 Stop+Start 重连 (退避 reconnect_backoff_ms), 直到 Stop() / 析构.
  // user_id / user_pw 空 = rtsp URL 无凭据.
  bool StartRtsp(const std::string& url, const std::string& user_id,
                 const std::string& user_pw, const RtspOptions& opts,
                 int expected_w = 0, int expected_h = 0);

  // 拉一帧, 阻塞. 出错或 EOS 时 is_eos=true.
  // 调用方拿到 GstMppFrame 后, sample 持有 GstBuffer, 直接用 dma_buf_fd 即可
  // (gstreamer 已 DMA-BUF feature on, buffer 是 DMA-BUF 内存).
  // sample 析构时 gstreamer 自动还 buffer.
  bool PullFrame(GstMppFrame& out_frame);

  // 主动停止. 析构时也会自动停.
  void Stop();

  // 当前是否已经 EOS
  bool IsEos() const { return is_eos_.load(); }

  // 当前是否在跑
  bool IsRunning() const { return pipeline_ != nullptr; }

 private:
  // 拆 file / rtsp 两个构造路径, 共享 appsink/DMABuf 提取与 DIAG 日志.
  bool BuildFilePipeline();
  bool BuildRtspPipeline();

  // rtspsrc 是 sometimes-pads, pad-added 回调动态连 depay.
  // 通过 g_signal_connect (rtspsrc, "pad-added", OnRtspsrcPadAdded, depay) 注册.
  static void OnRtspsrcPadAdded(_GstElement* src, _GstPad* new_pad,
                                void* user_data);

  // watchdog: 监控 pipeline state == NULL 或 is_eos_, 自动 Stop+重 BuildRtspPipeline.
  void StartReconnectWatchdog();
  void StopReconnectWatchdog();
  void WatchdogLoop();

  _GstElement* pipeline_ = nullptr;   // gst_pipeline_new / gst_parse_launch 出来的
  _GstElement* appsink_ = nullptr;    // pipeline 里的 appsink 元素
  std::string uri_;                   // 当前生效的 URI (file: 路径 / rtsp: rtsp://...)
  int expected_w_ = 0;
  int expected_h_ = 0;
  std::atomic<bool> is_eos_{false};
  // v2.5 (2026-07-08) 一次性诊断位: 每个 pipeline (URI) 第一帧输出详细 caps/内存/平面布局, 帮助定位 NV12 绿条纹.
  bool first_frame_dumped_ = false;

  // ★ v3.0: rtsp 专属状态.
  bool is_rtsp_ = false;
  std::string rtsp_url_;
  std::string rtsp_user_id_;
  std::string rtsp_user_pw_;
  RtspOptions rtsp_opts_{};
  std::atomic<bool> watchdog_running_{false};
  std::thread reconnect_watchdog_;
  std::atomic<int>  reconnect_count_{0};  // ★ 仅诊断用: `[gst_mpp_decoder][DIAG] reconnects=N`
};

}  // namespace image_stitching

#endif  // IMAGE_STITCHING_GST_MPP_DECODER_H