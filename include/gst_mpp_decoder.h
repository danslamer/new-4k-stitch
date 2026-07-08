//
// gst_mpp_decoder.h - 用 gstreamer-rockchip 走 mppvideodec 硬件解 H.264
// 替代之前的 FFmpeg rkmpp wrapper (vendor 不维护, 解码器实际 0 帧).
// 输出: NV12 DMA-BUF (零拷贝) → stitcher 直接用 fd
//
// 2026-07-06 新增: vendor 确认 FFmpeg rkmpp 不支持, kmpp 解码器未完成,
// SDK 自带的 librockchip-mpp 1.5.0 (走 mpp_service 老接口) 是 vendor 官方推荐路径.
// gstreamer1.0-rockchip1 1.14-4 (含 mppvideodec) 是 vendor SDK 标准组件.
//

#ifndef IMAGE_STITCHING_GST_MPP_DECODER_H
#define IMAGE_STITCHING_GST_MPP_DECODER_H

#include <memory>
#include <string>

extern "C" {
struct _GstElement;
struct _GstSample;
struct _GstBuffer;
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

// 单个 pipeline 实例 (一个 sensor / 一个 .mp4 对应一个实例).
// 用法:
//   GstMppDecoder dec;
//   dec.Start("datasets/2k-test-h264/t50.mp4");
//   while (dec.PullFrame(f)) { /* use f.dma_buf_fd */ }
//
class GstMppDecoder {
 public:
  GstMppDecoder();
  ~GstMppDecoder();

  GstMppDecoder(const GstMppDecoder&) = delete;
  GstMppDecoder& operator=(const GstMppDecoder&) = delete;

  // 启动 pipeline. 失败返回 false. 同一实例可重复 Start (内部会 Stop 旧的).
  // 期望 file 是 H.264 mp4 (阶段 2' 一次性 transcode 产物: datasets/2k-test-h264/*.mp4).
  // width/height 期望 (2560x1440), 留 0 走 caps 自动检测.
  bool Start(const std::string& uri, int expected_w = 0, int expected_h = 0);

  // 拉一帧, 阻塞. 出错或 EOS 时 is_eos=true.
  // 调用方拿到 GstMppFrame 后, sample 持有 GstBuffer, 直接用 dma_buf_fd 即可
  // (gstreamer 已 DMA-BUF feature on, buffer 是 DMA-BUF 内存).
  // sample 析构时 gstreamer 自动还 buffer.
  bool PullFrame(GstMppFrame& out_frame);

  // 主动停止. 析构时也会自动停.
  void Stop();

  // 当前是否已经 EOS
  bool IsEos() const { return is_eos_; }

  // 当前是否在跑
  bool IsRunning() const { return pipeline_ != nullptr; }

 private:
  _GstElement* pipeline_ = nullptr;   // gst_parse_launch 出来的 GstPipeline
  _GstElement* appsink_ = nullptr;    // pipeline 里的 appsink 元素
  std::string uri_;
  int expected_w_ = 0;
  int expected_h_ = 0;
  bool is_eos_ = false;
  // v2.5 (2026-07-08) 一次性诊断位: 每个 pipeline (URI) 第一帧输出详细 caps/内存/平面布局, 帮助定位 NV12 绿条纹.
  bool first_frame_dumped_ = false;
};

}  // namespace image_stitching

#endif  // IMAGE_STITCHING_GST_MPP_DECODER_H
