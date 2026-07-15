//
// gst_mpp_decoder.cc - gstreamer + mppvideodec (Rockchip) H.264 硬解实现
// 关键: dma-feature=true 让 mppvideodec 输出 DMA-BUF backed GstBuffer,
// stitcher 拿到 fd 直接 RGA, 零拷贝.
//
// v3.0 (2026-07-08): 新增 RTSP 拉流路径
//   rtspsrc(sometimes-pads) → rtph264depay(动态接) → h264parse → mppvideodec → appsink
//   内置 watchdog 线程负责断流重连.
//

#include "gst_mpp_decoder.h"

#include "logger.h"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

extern "C" {
// gst_dmabuf 是个 1.20+ 的内建 API, 不需要单独装 gst-plugins-bad
// (Ubuntu 22.04 gstreamer 1.20.3 已带)
#include <gst/allocators/gstdmabuf.h>
}

// v3.0: pthread mutex / cond. gstreamer 回调里有可能用到.
#include <chrono>
#include <mutex>
#include <cerrno>      // errno
#include <sys/stat.h>  // stat
#include <unistd.h>    // (used by gstreamer; future hooks)
#include <fcntl.h>     // (used by gstreamer; future hooks)
#include <sstream>
#include <thread>

namespace image_stitching {

namespace {

bool g_gst_inited = false;

void EnsureGstInit() {
  if (!g_gst_inited) {
    gst_init(nullptr, nullptr);
    g_gst_inited = true;
  }
}

// 找到 pipeline 里的 appsink 元素. 失败返回 nullptr.
GstElement* FindAppsink(GstElement* pipeline, const char* sink_name) {
  GstElement* appsink = gst_bin_get_by_name(GST_BIN(pipeline), sink_name);
  return appsink;
}

// 从 GstBuffer 提取 DMA-BUF fd. 失败返回 -1.
int ExtractDmaBufFd(GstBuffer* buffer) {
  if (buffer == nullptr) return -1;

  guint mem_count = gst_buffer_n_memory(buffer);
  for (guint i = 0; i < mem_count; ++i) {
    GstMemory* mem = gst_buffer_peek_memory(buffer, i);
    if (mem == nullptr) continue;
    if (gst_is_dmabuf_memory(mem)) {
      return gst_dmabuf_memory_get_fd(mem);
    }
  }
  return -1;
}

// 从 caps 提取视频尺寸 + stride.
// 注意: gstreamer 1.20 没有 gst_buffer_get_pad(), 强制调用方传 caps (来自 gst_sample_get_caps).
bool ExtractVideoInfo(GstCaps* caps,
                      int* out_w, int* out_h,
                      int* out_stride_y, int* out_stride_uv) {
  if (caps == nullptr) return false;
  GstVideoInfo vinfo;
  if (!gst_video_info_from_caps(&vinfo, caps)) {
    return false;
  }
  *out_w = GST_VIDEO_INFO_WIDTH(&vinfo);
  *out_h = GST_VIDEO_INFO_HEIGHT(&vinfo);
  *out_stride_y = GST_VIDEO_INFO_PLANE_STRIDE(&vinfo, 0);
  *out_stride_uv = GST_VIDEO_INFO_PLANE_STRIDE(&vinfo, 1);
  return (*out_w > 0 && *out_h > 0);
}

}  // namespace

void GstMppDeleter::operator()(_GstSample* sample) const {
  if (sample != nullptr) {
    gst_sample_unref(sample);
  }
}

GstMppDecoder::GstMppDecoder() {
  EnsureGstInit();
}

GstMppDecoder::~GstMppDecoder() {
  Stop();
}

// ============================================================================
// ★ 老路径 (file) — 维持 v2.5 行为完全不变, 拆成 BuildFilePipeline()
// ============================================================================

bool GstMppDecoder::BuildFilePipeline() {
  // pipeline 描述:
  // filesrc → qtdemux → h264parse → mppvideodec(dma-feature=true, format=NV12)
  //   → caps filter (NV12) → appsink
  //
  // 关键点:
  // 1. mppvideodec dma-feature=true 让 mpp 输出 DMA-BUF backed GstBuffer
  // 2. caps filter 强制 (memory:DMABuf) + NV12, 否则 appsink 拿到 sw frame
  // 3. appsink drop=true + max-buffers=2
  std::ostringstream pipeline_desc;
  pipeline_desc << "filesrc location=\"" << uri_ << "\" "
                << "! qtdemux "
                << "! h264parse "
                << "! mppvideodec dma-feature=true format=NV12 "
                // 不强制 width/height, 让 mppvideodec 输出自动协商
                << "! video/x-raw(memory:DMABuf),format=NV12 "
                << "! appsink name=sink "
                << "drop=true max-buffers=2 sync=false";

  Logger::GetInstance().Log(
      "[gst_mpp_decoder] file pipeline_desc: " + pipeline_desc.str());

  GError* err = nullptr;
  pipeline_ = gst_parse_launch(pipeline_desc.str().c_str(), &err);
  if (pipeline_ == nullptr) {
    Logger::GetInstance().LogError(
        "[gst_mpp_decoder] gst_parse_launch failed: " +
        std::string(err != nullptr ? err->message : "unknown"));
    if (err != nullptr) g_error_free(err);
    return false;
  }

  appsink_ = FindAppsink(pipeline_, "sink");
  if (appsink_ == nullptr) {
    Logger::GetInstance().LogError(
        "[gst_mpp_decoder] appsink 'sink' not found in pipeline");
    Stop();
    return false;
  }

  GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    Logger::GetInstance().LogError(
        "[gst_mpp_decoder] file pipeline set_state PLAYING failed");
    Stop();
    return false;
  }
  return true;
}

// ============================================================================
// ★ 新 v3.0 路径 (rtsp) — BuildRtspPipeline() + pad-added 回调 + watchdog
// ============================================================================

bool GstMppDecoder::BuildRtspPipeline() {
  Logger::GetInstance().Log(
      "[gst_mpp_decoder] build RTSP pipeline: " + rtsp_url_ +
      " (latency=" + std::to_string(rtsp_opts_.latency_ms) +
      "ms use_tcp=" + (rtsp_opts_.use_tcp ? "tcp" : "udp") + ")");

  pipeline_ = gst_pipeline_new("stitch-rtsp");
  if (pipeline_ == nullptr) {
    Logger::GetInstance().LogError("[gst_mpp_decoder] gst_pipeline_new failed");
    return false;
  }

  _GstElement* src   = gst_element_factory_make("rtspsrc",    "src");
  _GstElement* depay = gst_element_factory_make("rtph264depay", "depay");
  _GstElement* parse = gst_element_factory_make("h264parse",  "parse");
  _GstElement* dec   = gst_element_factory_make("mppvideodec", "dec");
  _GstElement* sink  = gst_element_factory_make("appsink",    "sink");

  if (!src || !depay || !parse || !dec || !sink) {
    Logger::GetInstance().LogError(
        "[gst_mpp_decoder][rtsp] factory_make failed (one of rtspsrc/rtph264depay/"
        "h264parse/mppvideodec/appsink is missing — install gstreamer1.0-plugins-good + "
        "gstreamer1.0-rockchip1)");
    if (pipeline_) {
      gst_object_unref(pipeline_);
      pipeline_ = nullptr;
    }
    return false;
  }

  // rtspsrc 属性 (location / protocols / latency / retry / timeout / ntp-sync / drop-on-latency / creds)
  g_object_set(src, "location",    rtsp_url_.c_str(), nullptr);
  g_object_set(src, "protocols",   rtsp_opts_.use_tcp ? 0x4 : 0x7, nullptr);
  g_object_set(src, "latency",     (guint)rtsp_opts_.latency_ms, nullptr);
  g_object_set(src, "retry",       rtsp_opts_.retry_attempts, nullptr);
  g_object_set(src, "timeout",     (guint64)(rtsp_opts_.connect_timeout_s * G_USEC_PER_SEC), nullptr);
  g_object_set(src, "tcp-timeout", (guint64)(rtsp_opts_.connect_timeout_s * G_USEC_PER_SEC), nullptr);
  g_object_set(src, "ntp-sync",    TRUE, nullptr);
  g_object_set(src, "drop-on-latency", TRUE, nullptr);
  if (!rtsp_user_id_.empty()) {
    g_object_set(src, "user-id", rtsp_user_id_.c_str(), nullptr);
    g_object_set(src, "user-pw", rtsp_user_pw_.c_str(), nullptr);
  }

  // mppvideodec (硬约束 — 不可改)
  g_object_set(dec, "dma-feature", TRUE, nullptr);
  // format: 与 v2.5 file 路径写法一致 ("NV12" 字符串字面量), gst 1.20 mppvideodec 内部四字符码 = 23 = AV_PIX_FMT_NV12
  g_object_set(dec, "format", "NV12", nullptr);

  // appsink (实时, 旧帧丢, 等 rtsp GOP 关键帧)
  g_object_set(sink, "drop",         TRUE,  nullptr);
  g_object_set(sink, "max-buffers",  2,      nullptr);
  g_object_set(sink, "sync",         FALSE,  nullptr);
  // 不重复设 caps (v2.5 line 132 注释: 让 pipeline 协商)

  gst_bin_add_many(GST_BIN(pipeline_), src, depay, parse, dec, sink, nullptr);
  if (!gst_element_link_many(depay, parse, dec, sink, nullptr)) {
    Logger::GetInstance().LogError(
        "[gst_mpp_decoder][rtsp] link_many(depay→parse→dec→sink) failed");
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    return false;
  }

  // rtspsrc pad-added 回调动态接 depay (sometimes-pads 必须)
  g_signal_connect(src, "pad-added", G_CALLBACK(OnRtspsrcPadAdded), depay);

  GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    Logger::GetInstance().LogError(
        "[gst_mpp_decoder][rtsp] set_state PLAYING failed (url=" + rtsp_url_ + ")");
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    return false;
  }
  return true;
}

// rtspsrc pad-added 静态回调 (C 风格).
// 收到的新 pad 是 application/x-rtp, 需要检查 stream codec (video/x-h264) 后链到 depay.
// 注意 depay 通常只有 1 个 sink pad, 多次 pad-added 只接一次.
void GstMppDecoder::OnRtspsrcPadAdded(_GstElement* src, _GstPad* new_pad,
                                      void* user_data) {
  (void)src;
  _GstElement* depay = GST_ELEMENT(user_data);
  if (depay == nullptr) return;

  _GstPad* sinkpad = gst_element_get_static_pad(depay, "sink");
  if (sinkpad == nullptr) return;
  if (gst_pad_is_linked(sinkpad)) {
    gst_object_unref(sinkpad);
    return;
  }

  // 只接 video RTP 流 (RTSP DESCRIBE 可能返回多个 media — 视频/音频/元数据)
  GstCaps* new_pad_caps = gst_pad_get_current_caps(new_pad);
  if (new_pad_caps == nullptr) {
    new_pad_caps = gst_pad_query_caps(new_pad, nullptr);
  }
  bool should_link = false;
  if (new_pad_caps != nullptr) {
    guint caps_size = gst_caps_get_size(new_pad_caps);
    if (caps_size > 0) {
      GstStructure* s = gst_caps_get_structure(new_pad_caps, 0);
      if (s != nullptr) {
        const gchar* media = gst_structure_get_name(s);
        if (media != nullptr
            && g_str_has_prefix(media, "application/x-rtp")) {
          // 进一步看 payload 编码 — h264 vs h265 vs mpeg4 — 现阶段我们只接 h264
          const gchar* enc = gst_structure_get_string(s, "encoding-name");
          if (enc == nullptr || g_ascii_strcasecmp(enc, "H264") == 0
                              || g_ascii_strcasecmp(enc, "H265") == 0) {
            should_link = true;
          } else {
            Logger::GetInstance().Log(
                "[gst_mpp_decoder][rtsp] pad-added non-h264/h265 media (" +
                std::string(enc ? enc : "?") + "), ignored");
          }
        }
      } else {
        // s == nullptr: caps 是 empty structures; 静默忽略
      }
    }
    gst_caps_unref(new_pad_caps);
  }

  if (should_link) {
    GstPadLinkReturn lr = gst_pad_link(new_pad, sinkpad);
    if (lr != GST_PAD_LINK_OK) {
      Logger::GetInstance().LogError(
          "[gst_mpp_decoder][rtsp] pad link failed: " + std::to_string(lr));
    } else {
      Logger::GetInstance().Log("[gst_mpp_decoder][rtsp] pad linked rtspsrc→depay");
    }
  }
  gst_object_unref(sinkpad);
}

// watchdog: 见 Set() → is_eos_/state==NULL → 自动重 BuildRtspPipeline().
// rtsp 断流时, rtspsrc 内部会触发 EOS / pipeline 自动归 NULL. 我们的 watchdog:
//   1. 检查 pipeline state == NULL (明示)
//   2. 检查 is_eos_ (appsink 收到 EOS 后, is_eos_ 会被 PullFrame 标 true)
// 触发任一条件时, sleep reconnect_backoff_ms, 然后 Stop+重 StartRtsp().
void GstMppDecoder::StartReconnectWatchdog() {
  if (!is_rtsp_) return;
  if (watchdog_running_.exchange(true)) return;
  reconnect_watchdog_ = std::thread([this]() { WatchdogLoop(); });
}

void GstMppDecoder::StopReconnectWatchdog() {
  if (!is_rtsp_) return;
  watchdog_running_.store(false);
  if (reconnect_watchdog_.joinable()) {
    reconnect_watchdog_.join();
  }
}

void GstMppDecoder::WatchdogLoop() {
  Logger::GetInstance().Log(
      "[gst_mpp_decoder][watchdog] started, check_interval=200ms, backoff=" +
      std::to_string(rtsp_opts_.reconnect_backoff_ms) + "ms");

  while (watchdog_running_.load()) {
    bool need_reconnect = false;

    // 1. pipeline state check
    if (pipeline_ != nullptr) {
      GstState state = GST_STATE_NULL;
      gst_element_get_state(pipeline_, &state, nullptr, 100 * GST_MSECOND);
      if (state == GST_STATE_NULL) {
        need_reconnect = true;
      }
    } else {
      need_reconnect = true;
    }

    // 2. is_eos_ flag
    if (is_eos_.load()) {
      need_reconnect = true;
    }

    if (need_reconnect) {
      reconnect_count_.fetch_add(1);
      Logger::GetInstance().Log(
          "[gst_mpp_decoder][watchdog] reconnect #" +
          std::to_string(reconnect_count_.load()) +
          " after backoff (" + std::to_string(rtsp_opts_.reconnect_backoff_ms) +
          "ms) uri=" + rtsp_url_);

      std::this_thread::sleep_for(
          std::chrono::milliseconds(rtsp_opts_.reconnect_backoff_ms));

      if (!watchdog_running_.load()) break;  // Stop() 已调用

      // Stop + 重建 (注意: pipeline_ 在 BuildRtspPipeline 失败时会被 unref)
      GstElement* old = pipeline_;
      pipeline_ = nullptr;
      // ★ v3.0.1 BUG FIX: 释放旧 appsink_ 引用 (gst_bin_get_by_name +1 ref, 否则会泄漏)
      if (appsink_ != nullptr) {
        gst_object_unref(appsink_);
        appsink_ = nullptr;
      }
      if (old != nullptr) {
        gst_element_set_state(old, GST_STATE_NULL);
        gst_object_unref(old);
      }
      is_eos_.store(false);

      if (!BuildRtspPipeline()) {
        Logger::GetInstance().LogError(
            "[gst_mpp_decoder][watchdog] reconnect failed: " + rtsp_url_ +
            ", retry after backoff");
        // 退避后下次循环再试
        continue;
      }
      appsink_ = FindAppsink(pipeline_, "sink");
      Logger::GetInstance().Log(
          "[gst_mpp_decoder][watchdog] reconnect OK");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  Logger::GetInstance().Log("[gst_mpp_decoder][watchdog] exiting");
}

// ============================================================================
// Start / Stop / PullFrame 公共 API
// ============================================================================

bool GstMppDecoder::Start(const std::string& uri, int expected_w, int expected_h) {
  Stop();
  uri_ = uri;
  expected_w_ = expected_w;
  expected_h_ = expected_h;
  is_rtsp_ = false;
  is_eos_ = false;
  first_frame_dumped_ = false;
  return BuildFilePipeline();
}

bool GstMppDecoder::StartRtsp(const std::string& url, const std::string& user_id,
                               const std::string& user_pw, const RtspOptions& opts,
                               int expected_w, int expected_h) {
  Stop();
  uri_ = url;
  rtsp_url_ = url;
  rtsp_user_id_ = user_id;
  rtsp_user_pw_ = user_pw;
  rtsp_opts_ = opts;
  expected_w_ = expected_w;
  expected_h_ = expected_h;
  is_rtsp_ = true;
  is_eos_ = false;
  first_frame_dumped_ = false;

  if (!BuildRtspPipeline()) {
    return false;
  }

  // 启动 watchdog (rtsp 专属). file 路径不启.
  StartReconnectWatchdog();
  return true;
}

// ============================================================================
// ★ Sprint 4-MIPI (2026-07-15): StartMipi()
//   启动 v4l2src 路径 (走 ISP 输出 NV12 DMA-BUF).
//   失败时返回 false, 调用方应回退到 BlackFrameProvider
//   (SensorDataInterface::StartDecodeThreads 已经做了这一步).
//
// io_mode 复用 uri_ 字段存 (dmabuf / mmap); mipi_device_path_ 是 /dev/videoN.
//   is_eos_ 由 IsEos() 暴露, watchdog 不启 (mipi 缺流时 v4l2src 会负数帧计数).
// ============================================================================
bool GstMppDecoder::StartMipi(const std::string& device_path,
                              const std::string& io_mode,
                              int num_buffers,
                              int expected_w, int expected_h) {
  Stop();  // 清干净再开
  is_rtsp_ = false;
  is_mipi_ = true;
  expected_w_ = expected_w;
  expected_h_ = expected_h;
  mipi_device_path_ = device_path;
  uri_ = io_mode;  // reuse: "dmabuf" / "mmap"
  is_eos_.store(false);
  first_frame_dumped_ = false;

  // num_buffers (v4l2src num-buffers) 当前 BuildMipiPipeline 里写死 4.
  // 后续如需用户可配, 加 private int mipi_num_buffers_{4};.
  (void)num_buffers;

  return BuildMipiPipeline();
}

bool GstMppDecoder::PullFrame(GstMppFrame& out_frame) {
  out_frame = GstMppFrame{};

  if (appsink_ == nullptr) {
    // pipeline 已 NULL (rtsp 断流 watchdog 重连中). 让上层 sleep+retry.
    out_frame.is_eos = true;
    is_eos_ = true;
    return false;
  }

  GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(appsink_));
  if (sample == nullptr) {
    // appsink 内 EOS / 阻塞超时. 标 is_eos 让上层 (对 rtsp) sleep+retry.
    out_frame.is_eos = true;
    is_eos_ = true;
    return false;
  }

  GstBuffer* buffer = gst_sample_get_buffer(sample);
  GstCaps*   caps   = gst_sample_get_caps(sample);

  if (is_rtsp_ && !first_frame_dumped_) {
    // v3.0: rtsp 第一帧加 [DIAG] 字段 (具体流参数 + mpp 输出 caps)
    guint mc = gst_buffer_n_memory(buffer);
    std::ostringstream ss;
    ss << "[gst_mpp_decoder][DIAG][rtsp] uri=" << rtsp_url_
       << " caps=" << (caps ? gst_caps_to_string(caps) : "(null)")
       << " mem_count=" << (int)mc
       << " buf_size=" << (long long)gst_buffer_get_size(buffer);
    Logger::GetInstance().Log(ss.str());
    for (guint i = 0; i < mc; ++i) {
      GstMemory* mem = gst_buffer_peek_memory(buffer, i);
      if (mem == nullptr) continue;
      gint fd = -1;
      gboolean is_dmabuf = gst_is_dmabuf_memory(mem);
      if (is_dmabuf) fd = gst_dmabuf_memory_get_fd(mem);
      gsize off = 0, maxsz = 0;
      gst_memory_get_sizes(mem, &off, &maxsz);
      std::ostringstream ss2;
      ss2 << "[gst_mpp_decoder][DIAG][rtsp] mem[" << i
          << "] dmabuf=" << (is_dmabuf ? "yes" : "no")
          << " fd=" << fd
          << " off=" << (unsigned long long)off
          << " max=" << (unsigned long long)maxsz;
      Logger::GetInstance().Log(ss2.str());
    }
    if (mc == 1) {
      Logger::GetInstance().Log(
          "[gst_mpp_decoder][DIAG][rtsp] layout=1 (single DMA-BUF, Y+UV contiguous) -> ok");
    } else if (mc >= 2) {
      Logger::GetInstance().LogError(
          "[gst_mpp_decoder][DIAG][rtsp] layout=" + std::to_string(mc) +
          " (multi DMA-BUF; same green-stripe risk as file path, see v2.5)");
    }
  }
  if (!first_frame_dumped_) {
    first_frame_dumped_ = true;
  }

  out_frame.dma_buf_fd = ExtractDmaBufFd(buffer);
  if (out_frame.dma_buf_fd < 0) {
    Logger::GetInstance().LogError(
        "[gst_mpp_decoder] no DMA-BUF fd in buffer (zero-copy violated), uri=" + uri_);
    gst_sample_unref(sample);
    out_frame.is_eos = true;
    is_eos_ = true;
    return false;
  }

  int stride_y = 0, stride_uv = 0;
  if (!ExtractVideoInfo(caps, &out_frame.width, &out_frame.height,
                        &stride_y, &stride_uv)) {
    Logger::GetInstance().LogError(
        "[gst_mpp_decoder] cannot extract video info from caps, uri=" + uri_);
    gst_sample_unref(sample);
    out_frame.is_eos = true;
    is_eos_ = true;
    return false;
  }

  // v2.5 (2026-07-08) stride true-derive: mppvideodec dma-feature 64-byte 对齐,
  // gstreamer 上报 stride 经常偏小, 这里用 buf_size / (1.5 * height) 反推.
  if (out_frame.height > 0 && stride_y > 0) {
    gsize bsize = gst_buffer_get_size(buffer);
    if (bsize > 0) {
      int actual_stride =
          static_cast<int>(bsize / (static_cast<gsize>(out_frame.height) * 3 / 2));
      if (actual_stride > stride_y && actual_stride > out_frame.width) {
        if (!is_rtsp_) {
          Logger::GetInstance().Log(
              std::string("[gst_mpp_decoder][DIAG] stride_override: gstr=") +
              std::to_string(stride_y) + ", buf_size=" +
              std::to_string(static_cast<long long>(bsize)) + ", h=" +
              std::to_string(out_frame.height) + ", actual=" +
              std::to_string(actual_stride));
        }
        stride_y = actual_stride;
        stride_uv = actual_stride;
      }
    }
  }
  out_frame.stride_y = stride_y;
  out_frame.stride_uv = stride_uv;

  out_frame.sample.reset(sample, GstMppDeleter{});
  return true;
}

void GstMppDecoder::Stop() {
  StopReconnectWatchdog();

  if (pipeline_ != nullptr) {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
  }
  // ★ v3.0.1 BUG FIX: 释放 appsink_ 引用 (FindAppsink 内部 gst_bin_get_by_name +1 ref)
  if (appsink_ != nullptr) {
    gst_object_unref(appsink_);
    appsink_ = nullptr;
  }
  is_eos_ = false;
}

}  // namespace image_stitching