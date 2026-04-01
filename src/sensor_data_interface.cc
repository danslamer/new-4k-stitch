//
// Created by s1nh.org on 11/11/20.
// Updated to use FFmpeg decoding for multi-channel video input.
//

#include "sensor_data_interface.h"

#include "logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#ifdef ENABLE_RK_HARDWARE_DECODING
#if __has_include(<im2d.h>)
#include <im2d.h>
#elif __has_include(<rga/im2d.h>)
#include <rga/im2d.h>
#else
#error "librga header im2d.h not found"
#endif
#endif

namespace {

std::string AvErrorToString(int errnum) {
  char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
  av_strerror(errnum, errbuf, sizeof(errbuf));
  return std::string(errbuf);
}

#ifdef ENABLE_RK_HARDWARE_DECODING
std::string ImStatusToString(IM_STATUS status) {
  const char* status_name = imStrError(status);
  return status_name != nullptr ? std::string(status_name)
                                : std::string("unknown rga status");
}

bool ConvertFrameToBgrWithRga(const AVFrame* frame,
                              cv::Mat& bgr_frame,
                              std::string& error_message) {
  if (frame == nullptr) {
    error_message = "input frame is null";
    return false;
  }
  if (frame->format != AV_PIX_FMT_NV12) {
    error_message = "RGA color conversion only supports NV12 frames";
    return false;
  }
  if (frame->data[0] == nullptr || frame->data[1] == nullptr) {
    error_message = "NV12 frame planes are null";
    return false;
  }

  const int src_width_stride = frame->linesize[0];
  const int src_height_stride = frame->height;
  const size_t src_bytes =
      static_cast<size_t>(src_width_stride) * static_cast<size_t>(src_height_stride) * 3 / 2;
  std::vector<uint8_t> nv12_buffer(src_bytes);

  for (int row = 0; row < frame->height; ++row) {
    std::memcpy(nv12_buffer.data() + row * src_width_stride,
                frame->data[0] + row * frame->linesize[0],
                static_cast<size_t>(frame->width));
  }

  uint8_t* uv_destination =
      nv12_buffer.data() + static_cast<size_t>(src_width_stride) * frame->height;
  for (int row = 0; row < frame->height / 2; ++row) {
    std::memcpy(uv_destination + row * src_width_stride,
                frame->data[1] + row * frame->linesize[1],
                static_cast<size_t>(frame->width));
  }

  bgr_frame.create(frame->height, frame->width, CV_8UC3);

  rga_buffer_t src_buffer = wrapbuffer_virtualaddr(nv12_buffer.data(),
                                                   frame->width,
                                                   frame->height,
                                                   RK_FORMAT_YCbCr_420_SP,
                                                   src_width_stride,
                                                   src_height_stride);
  rga_buffer_t dst_buffer = wrapbuffer_virtualaddr(bgr_frame.data,
                                                   frame->width,
                                                   frame->height,
                                                   RK_FORMAT_BGR_888,
                                                   static_cast<int>(bgr_frame.step[0] / 3),
                                                   frame->height);

  const IM_STATUS status = imcvtcolor(src_buffer,
                                      dst_buffer,
                                      RK_FORMAT_YCbCr_420_SP,
                                      RK_FORMAT_BGR_888);
  if (status != IM_STATUS_SUCCESS) {
    error_message = "imcvtcolor failed: " + ImStatusToString(status);
    return false;
  }

  return true;
}
#endif

bool IsHardwarePixelFormat(AVPixelFormat pixel_format) {
  const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(pixel_format);
  return descriptor != nullptr &&
         (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0;
}

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

struct DecoderPerfStats {
  size_t packets_read = 0;
  size_t packets_sent = 0;
  size_t frames_decoded = 0;
  size_t hardware_frames = 0;
  size_t software_frames = 0;
  size_t queue_pushes = 0;
  size_t queue_drops = 0;
  double read_seconds = 0.0;
  double send_seconds = 0.0;
  double receive_seconds = 0.0;
  double convert_seconds = 0.0;
  double upload_seconds = 0.0;
  std::chrono::steady_clock::time_point report_time =
      std::chrono::steady_clock::now();
};

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
         << " sw_frames=" << stats.software_frames
         << " read_ms=" << stats.read_seconds * 1000.0
         << " send_ms=" << stats.send_seconds * 1000.0
         << " receive_ms=" << stats.receive_seconds * 1000.0
         << " convert_ms=" << stats.convert_seconds * 1000.0
         << " upload_ms=" << stats.upload_seconds * 1000.0
         << " queue_pushes=" << stats.queue_pushes
         << " queue_drops=" << stats.queue_drops;
  return stream.str();
}

}  // namespace

SensorDataInterface::SensorDataInterface()
    : max_queue_length_(2),
      num_img_(0),
      frame_idx(0),
      stop_requested_(false),
      decode_threads_started_(false) {}

SensorDataInterface::~SensorDataInterface() {
  StopDecodeThreads();
}

void SensorDataInterface::InitExampleImages() {}

void SensorDataInterface::InitVideoCapture(size_t& num_img) {
  const std::string video_dir = "../datasets/4k-test/";
  const std::vector<std::string> video_file_name = {
      "40.mp4", "41.mp4", "42.mp4", "43.mp4"};

  num_img_ = video_file_name.size();
  num_img = num_img_;

  image_queue_vector_ = std::vector<std::queue<cv::UMat>>(num_img_);
  image_queue_mutex_vector_ = std::vector<std::mutex>(num_img_);
  video_capture_vector_.clear();

  video_file_paths_.clear();
  decode_threads_.clear();
  decode_threads_.reserve(num_img_);
  decode_fps_vector_ = std::vector<double>(num_img_, 0.0);
  decoded_frames_since_report_ = std::vector<size_t>(num_img_, 0);
  decode_report_time_vector_ =
      std::vector<std::chrono::steady_clock::time_point>(
          num_img_, std::chrono::steady_clock::now());
  decoder_ready_vector_ = std::vector<bool>(num_img_, false);
  decoder_finished_vector_ = std::vector<bool>(num_img_, false);

  for (size_t i = 0; i < num_img_; ++i) {
    const std::string file_name = video_dir + video_file_name[i];
    video_file_paths_.push_back(file_name);
  }

  StartDecodeThreads();
}

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
      AVFrame* software_frame = nullptr;
      SwsContext* sws_context = nullptr;
      int video_stream_index = -1;

      auto cleanup = [&]() {
        if (sws_context != nullptr) {
          sws_freeContext(sws_context);
          sws_context = nullptr;
        }
        if (software_frame != nullptr) {
          av_frame_free(&software_frame);
        }
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

          AVFrame* frame_to_convert = decoded_frame;
          const AVPixelFormat decoded_pixel_format =
              static_cast<AVPixelFormat>(decoded_frame->format);
          const char* decoded_format_name = av_get_pix_fmt_name(decoded_pixel_format);
          last_frame_format_name =
              decoded_format_name != nullptr ? decoded_format_name : "unknown";
          perf_stats.frames_decoded++;
          if (IsHardwarePixelFormat(
                  decoded_pixel_format)) {
            perf_stats.hardware_frames++;
            if (av_hwframe_transfer_data(software_frame, decoded_frame, 0) < 0) {
              Logger::GetInstance().LogError(
                  "[decoder " + std::to_string(i) +
                  "] failed to transfer hardware frame to system memory.");
              av_frame_unref(decoded_frame);
              continue;
            }
            frame_to_convert = software_frame;
          } else {
            perf_stats.software_frames++;
          }

          const double convert_t0 = cv::getTickCount();
          cv::Mat bgr_frame;
#ifdef ENABLE_RK_HARDWARE_DECODING
          std::string rga_error;
          if (!ConvertFrameToBgrWithRga(frame_to_convert, bgr_frame, rga_error)) {
            Logger::GetInstance().LogError(
                "[decoder " + std::to_string(i) +
                "] RGA color conversion failed: " + rga_error);
            av_frame_unref(software_frame);
            av_frame_unref(decoded_frame);
            return false;
          }
#else
          sws_context = sws_getCachedContext(
              sws_context,
              frame_to_convert->width,
              frame_to_convert->height,
              static_cast<AVPixelFormat>(frame_to_convert->format),
              frame_to_convert->width,
              frame_to_convert->height,
              AV_PIX_FMT_BGR24,
              SWS_BILINEAR,
              nullptr,
              nullptr,
              nullptr);

          if (sws_context == nullptr) {
            Logger::GetInstance().LogError(
                "[decoder " + std::to_string(i) +
                "] failed to create pixel conversion context.");
            av_frame_unref(software_frame);
            av_frame_unref(decoded_frame);
            return false;
          }

          bgr_frame.create(frame_to_convert->height,
                           frame_to_convert->width,
                           CV_8UC3);
          uint8_t* destination_data[4] = {
              bgr_frame.data, nullptr, nullptr, nullptr};
          int destination_linesize[4] = {
              static_cast<int>(bgr_frame.step[0]), 0, 0, 0};

          sws_scale(sws_context,
                    frame_to_convert->data,
                    frame_to_convert->linesize,
                    0,
                    frame_to_convert->height,
                    destination_data,
                    destination_linesize);
#endif
          perf_stats.convert_seconds +=
              (cv::getTickCount() - convert_t0) / cv::getTickFrequency();

          const double upload_t0 = cv::getTickCount();
          cv::UMat umat_frame;
          bgr_frame.copyTo(umat_frame);
          perf_stats.upload_seconds +=
              (cv::getTickCount() - upload_t0) / cv::getTickFrequency();

          {
            std::lock_guard<std::mutex> queue_lock(image_queue_mutex_vector_[i]);
            if (image_queue_vector_[i].size() >= max_queue_length_) {
              image_queue_vector_[i].pop();
              perf_stats.queue_drops++;
            }
            image_queue_vector_[i].push(umat_frame);
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

          av_frame_unref(software_frame);
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
      software_frame = av_frame_alloc();

      if (packet == nullptr || decoded_frame == nullptr || software_frame == nullptr) {
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
                                " color_convert=RGA");
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

void SensorDataInterface::RecordVideos() {
  StartDecodeThreads();
}

void SensorDataInterface::get_image_vector(
    std::vector<cv::UMat>& image_vector,
    std::vector<std::mutex>& image_mutex_vector) {
  for (size_t i = 0; i < num_img_; ++i) {
    while (true) {
      bool has_frame = false;
      bool decoder_finished = false;
      cv::UMat frame;

      {
        std::lock_guard<std::mutex> queue_lock(image_queue_mutex_vector_[i]);
        if (!image_queue_vector_[i].empty()) {
          frame = image_queue_vector_[i].front();
          image_queue_vector_[i].pop();
          has_frame = true;
        }
      }

      if (has_frame) {
        std::lock_guard<std::mutex> image_lock(image_mutex_vector[i]);
        image_vector[i] = frame;
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
        image_vector[i] = cv::UMat();
        break;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

std::vector<double> SensorDataInterface::GetDecodeFpsSnapshot() {
  std::lock_guard<std::mutex> stats_lock(decode_stats_mutex_);
  return decode_fps_vector_;
}
