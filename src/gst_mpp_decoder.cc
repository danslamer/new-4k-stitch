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
  // v2.5 (2026-07-08) stride override happens in PullFrame (buffer visible). here we keep gstreamer-reported stride.
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
  first_frame_dumped_ = false;

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

  // v2.5 (2026-07-08) 一次性诊断 (每个 URI 第一帧). 打印 caps / n_memory / 每块 GstMemory / 每平面 offset stride,
  // 旨在一秒看出三个绿条纹根因: (a) qtdemux 输出 mp4v 但 pipeline 强制 h264parse;
  // (b) mppvideodec 输出多块 DMA-BUF (Y fd + UV fd) 而当前只取第一个 fd, UV 平面丢失;
  // (c) NV12 stride 与 mpp 硬件实际分配 stride 不一致. 日志前缀 [gst_mpp_decoder][DIAG].
  if (!first_frame_dumped_) {
    first_frame_dumped_ = true;
    if (caps != nullptr) {
      gchar* caps_str = gst_caps_to_string(caps);
      if (caps_str != nullptr) {
        Logger::GetInstance().Log(std::string("[gst_mpp_decoder][DIAG] caps: ") + caps_str);
        g_free(caps_str);
      }
    } else {
      Logger::GetInstance().LogError("[gst_mpp_decoder][DIAG] caps == nullptr");
    }
    if (buffer != nullptr) {
      guint mc = gst_buffer_n_memory(buffer);
      std::ostringstream ss_mem;
      ss_mem << "[gst_mpp_decoder][DIAG] mem_count=" << mc
             << " buf_size=" << (long long)gst_buffer_get_size(buffer)
             << " RGA_total=" << (long long)gst_buffer_get_size(buffer);
      Logger::GetInstance().Log(ss_mem.str());
      for (guint i = 0; i < mc; ++i) {
        GstMemory* mem = gst_buffer_peek_memory(buffer, i);
        if (mem == nullptr) continue;
        gint fd = -1;
        gboolean is_dmabuf = gst_is_dmabuf_memory(mem);
        if (is_dmabuf) fd = gst_dmabuf_memory_get_fd(mem);
        gsize off = 0, maxsz = 0;
        gst_memory_get_sizes(mem, &off, &maxsz);
        std::ostringstream ss;
        ss << "[gst_mpp_decoder][DIAG] mem[" << i << "] dmabuf=" << (is_dmabuf ? "yes" : "no")
           << " fd=" << fd << " off=" << (unsigned long long)off
           << " max=" << (unsigned long long)maxsz;
        Logger::GetInstance().Log(ss.str());
      }
      if (caps != nullptr) {
        GstVideoInfo vinfo;
        if (gst_video_info_from_caps(&vinfo, caps)) {
          std::ostringstream ss;
          ss << "[gst_mpp_decoder][DIAG] planes w=" << GST_VIDEO_INFO_WIDTH(&vinfo)
             << " h=" << GST_VIDEO_INFO_HEIGHT(&vinfo)
             << " size=" << GST_VIDEO_INFO_SIZE(&vinfo)
             << " stride[0,1,2]=" << GST_VIDEO_INFO_PLANE_STRIDE(&vinfo, 0)
             << "," << GST_VIDEO_INFO_PLANE_STRIDE(&vinfo, 1)
             << "," << GST_VIDEO_INFO_PLANE_STRIDE(&vinfo, 2)
             << " offset[0,1,2]=" << GST_VIDEO_INFO_PLANE_OFFSET(&vinfo, 0)
             << "," << GST_VIDEO_INFO_PLANE_OFFSET(&vinfo, 1)
             << "," << GST_VIDEO_INFO_PLANE_OFFSET(&vinfo, 2)
             << " fmt=" << (int)GST_VIDEO_INFO_FORMAT(&vinfo);
          Logger::GetInstance().Log(ss.str());
        }
      }
      // 给出最可能的根因结论 (便于一眼定位绿条纹原因).
      if (mc == 1) {
        Logger::GetInstance().Log("[gst_mpp_decoder][DIAG] layout=1 (single DMA-BUF, Y+UV contiguous) -> ok, no extra fix for NV12.");
      } else if (mc >= 2) {
        std::ostringstream ss;
        ss << "[gst_mpp_decoder][DIAG] layout=" << mc << " (multi DMA-BUF) fds=[";
        for (guint i = 0; i < mc && i < 8; ++i) {
          GstMemory* mem = gst_buffer_peek_memory(buffer, i);
          int d = -1;
          if (mem != nullptr && gst_is_dmabuf_memory(mem)) d = gst_dmabuf_memory_get_fd(mem);
          if (i > 0) ss << ",";
          ss << d;
        }
        ss << "] -> likely Y-fd + UV-fd separate, our PullFrame returns only the first fd;";
        Logger::GetInstance().Log(ss.str());
        Logger::GetInstance().LogError("[gst_mpp_decoder][DIAG] POTENTIAL ROOT CAUSE: UV plane missing -> green stripes. Fix: extend GstMppFrame to carry per-plane fds+offsets or fetch UV fd separately via GstVideoMeta.");
      }
    }
  }

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
  // v2.5 (2026-07-08) stride true-derive: gstreamer 上报 stride 经常小于实际 (mppvideodec dma-feature 64-byte 对齐, 例 width=2560 上 2560 实 2816). 后端 RGA/cvtColor 拿上 报 stride 算 UV 偏移 -> 绿条纹. 这里用 buf_size / (1.5 * height) 反推.
  if (out_frame.height > 0 && stride_y > 0) {
    gsize bsize = gst_buffer_get_size(buffer);
    if (bsize > 0) {
      int actual_stride = static_cast<int>(bsize / (static_cast<gsize>(out_frame.height) * 3 / 2));
      if (actual_stride > stride_y && actual_stride > out_frame.width) {
        Logger::GetInstance().Log(std::string("[gst_mpp_decoder][DIAG] stride_override: gstr=") +
                                 std::to_string(stride_y) + ", buf_size=" + std::to_string(static_cast<long long>(bsize)) +
                                 ", h=" + std::to_string(out_frame.height) +
                                 ", actual=" + std::to_string(actual_stride));
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
  if (pipeline_ != nullptr) {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
  }
  appsink_ = nullptr;  // appsink 是 pipeline 的子元素, pipeline unref 时一起释放
  is_eos_ = false;
}

}  // namespace image_stitching
