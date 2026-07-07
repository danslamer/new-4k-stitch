//
// gst_mpp_decoder.cc - gstreamer + mppvideodec (Rockchip) H.264 硬解实现
// 关键: dma-feature=true 让 mppvideodec 输出 DMA-BUF backed GstBuffer,
// stitcher 拿到 fd 直接 RGA, 零拷贝.
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
  // NV12: plane 0 = Y (stride = width), plane 1 = UV (stride = width).
  // gst_video_info 里 stride 通过 stride[i] 拿.
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

bool GstMppDecoder::Start(const std::string& uri, int expected_w, int expected_h) {
  Stop();

  uri_ = uri;
  expected_w_ = expected_w;
  expected_h_ = expected_h;
  is_eos_ = false;

  // pipeline 描述:
  // filesrc → qtdemux → h264parse → mppvideodec(dma-feature=true, format=NV12)
  //   → caps filter (NV12 + 期望分辨率) → appsink
  //
  // 关键点:
  // 1. mppvideodec dma-feature=true 让 mpp 输出 DMA-BUF backed GstBuffer
  // 2. caps filter 强制 (memory:DMABuf) + NV12, 否则 appsink 拿到 sw frame
  // 3. appsink drop=true (旧的丢) + max-buffers=2 (限 queue 上限, 跟原代码 max_queue_length_=2 一致)
  std::ostringstream pipeline_desc;
  pipeline_desc << "filesrc location=\"" << uri_ << "\" "
                << "! qtdemux "
                << "! h264parse "
                << "! mppvideodec dma-feature=true format=NV12 "
                // 不强制 width/height, 让 mppvideodec 输出自动协商 (它的 src pad caps
                // 是 DMA-BUF + NV12, 实际尺寸由 decoder 报告). 加死 width/height 在
                // 6 路并行时会偶尔导致 capsfilter 不通过 (协商不匹配).
                << "! video/x-raw(memory:DMABuf),format=NV12 "
                << "! appsink name=sink "
                << "drop=true max-buffers=2 sync=false";

  Logger::GetInstance().Log(
      "[gst_mpp_decoder] pipeline_desc: " + pipeline_desc.str());

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

  // caps filter 在 pipeline desc 里已经限制了 (memory:DMABuf) + format=NV12 + 期望分辨率,
  // appsink 通过 pipeline caps 自动继承. 不要再额外调 gst_app_sink_set_caps, 否则可能
  // 跟 mppvideodec 的实际输出 caps 不一致 (capsfilter 已基于协商 caps 设了). 见
  // 板端测试: 重复设 caps 会导致 set_state 返回 SUCCESS 但流不通.

  // 触发 PAUSED → PLAYING
  GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    Logger::GetInstance().LogError(
        "[gst_mpp_decoder] pipeline set_state PLAYING failed: " + uri_ +
        " (ret=" + std::to_string(ret) + ")");
    Stop();
    return false;
  }

  Logger::GetInstance().Log(
      "[gst_mpp_decoder] started: " + uri_ +
      " expected=" + std::to_string(expected_w_) + "x" +
      std::to_string(expected_h_) +
      " ret=" + std::to_string(ret));
  return true;
}

bool GstMppDecoder::PullFrame(GstMppFrame& out_frame) {
  out_frame = GstMppFrame{};

  if (pipeline_ == nullptr || appsink_ == nullptr) {
    out_frame.is_eos = true;
    is_eos_ = true;
    return false;
  }

  static int pull_calls = 0;
  if (++pull_calls % 60 == 1) {
    Logger::GetInstance().Log(
        "[gst_mpp_decoder] PullFrame called #" + std::to_string(pull_calls) +
        " uri=" + uri_);
  }

  GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(appsink_));
  if (sample == nullptr) {
    // EOS or error
    out_frame.is_eos = true;
    is_eos_ = true;
    Logger::GetInstance().Log(
        "[gst_mpp_decoder] pull_sample returned null (EOS): " + uri_);
    return false;
  }

  static int frame_count = 0;
  if (++frame_count % 30 == 1) {
    Logger::GetInstance().Log(
        "[gst_mpp_decoder] pulled sample #" + std::to_string(frame_count) +
        " uri=" + uri_);
  }

  GstBuffer* buffer = gst_sample_get_buffer(sample);
  GstCaps* caps = gst_sample_get_caps(sample);

  out_frame.dma_buf_fd = ExtractDmaBufFd(buffer);
  if (out_frame.dma_buf_fd < 0) {
    Logger::GetInstance().LogError(
        "[gst_mpp_decoder] no DMA-BUF fd in buffer (violates zero-copy path), uri=" + uri_);
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
  out_frame.stride_y = stride_y;
  out_frame.stride_uv = stride_uv;

  out_frame.sample.reset(sample, GstMppDeleter{});
  return true;
}

void GstMppDecoder::Stop() {
  if (pipeline_ != nullptr) {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
  }
  appsink_ = nullptr;  // appsink 是 pipeline 的子元素, pipeline unref 时一起释放
  is_eos_ = false;
}

}  // namespace image_stitching
