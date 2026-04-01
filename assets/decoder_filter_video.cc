/*
 * C++ 多路视频解码模板（4K/MP4）
 * - 每路视频一个线程
 * - 解码帧直接回调给拼接算法
 * - 内存管理使用 RAII
 */

#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

// 回调类型：当解码得到一帧时调用
using FrameCallback = std::function<void(AVFrame* frame, int video_index)>;

// 视频解码器类
class VideoDecoder {
public:
    VideoDecoder(const std::string& filename, int index, FrameCallback cb)
        : filename_(filename), video_index_(index), callback_(cb),
          fmt_ctx_(nullptr), dec_ctx_(nullptr), packet_(nullptr), frame_(nullptr)
    {}

    ~VideoDecoder() {
        if (frame_) av_frame_free(&frame_);
        if (packet_) av_packet_free(&packet_);
        if (dec_ctx_) avcodec_free_context(&dec_ctx_);
        if (fmt_ctx_) avformat_close_input(&fmt_ctx_);
    }

    // 启动解码线程
    void start() {
        thread_ = std::thread(&VideoDecoder::decodeLoop, this);
    }

    // 等待线程结束
    void join() {
        if (thread_.joinable())
            thread_.join();
    }

private:
    void decodeLoop() {
        int ret;

        // 打开输入文件
        if ((ret = avformat_open_input(&fmt_ctx_, filename_.c_str(), nullptr, nullptr)) < 0) {
            std::cerr << "Failed to open file: " << filename_ << "\n";
            return;
        }

        if ((ret = avformat_find_stream_info(fmt_ctx_, nullptr)) < 0) {
            std::cerr << "Failed to get stream info: " << filename_ << "\n";
            return;
        }

        // 找视频流
        AVCodec* dec = nullptr;
        ret = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
        if (ret < 0) {
            std::cerr << "Cannot find video stream: " << filename_ << "\n";
            return;
        }
        int stream_index = ret;

        // 创建解码器上下文
        dec_ctx_ = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(dec_ctx_, fmt_ctx_->streams[stream_index]->codecpar);
        if ((ret = avcodec_open2(dec_ctx_, dec, nullptr)) < 0) {
            std::cerr << "Failed to open decoder: " << filename_ << "\n";
            return;
        }

        // 分配 AVPacket 和 AVFrame
        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();

        // 循环读取 packet 并解码
        while (av_read_frame(fmt_ctx_, packet_) >= 0) {
            if (packet_->stream_index == stream_index) {
                ret = avcodec_send_packet(dec_ctx_, packet_);
                if (ret < 0) {
                    std::cerr << "Error sending packet for decoding\n";
                    break;
                }

                while (ret >= 0) {
                    ret = avcodec_receive_frame(dec_ctx_, frame_);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    else if (ret < 0) {
                        std::cerr << "Error during decoding\n";
                        break;
                    }

                    // 调用回调将帧传递给拼接算法
                    if (callback_) callback_(frame_, video_index_);

                    av_frame_unref(frame_);
                }
            }
            av_packet_unref(packet_);
        }
    }

private:
    std::string filename_;
    int video_index_;
    FrameCallback callback_;

    AVFormatContext* fmt_ctx_;
    AVCodecContext* dec_ctx_;
    AVPacket* packet_;
    AVFrame* frame_;

    std::thread thread_;
};

// 示例拼接算法回调
void stitch_callback(AVFrame* frame, int video_index) {
    // 这里直接可以将 frame->data[0] / frame->linesize[0] 传给你的拼接算法
    std::cout << "Video " << video_index
              << ": got frame " << frame->width << "x" << frame->height << std::endl;
    // TODO: stitch_frame(frame, video_index);
}

int main() {
    av_log_set_level(AV_LOG_QUIET);
    av_register_all();

    std::vector<std::string> videos = {
        "datasets/4k-test/40.mp4",
        "datasets/4k-test/41.mp4",
        "datasets/4k-test/42.mp4",
        "datasets/4k-test/43.mp4"
    };

    std::vector<std::unique_ptr<VideoDecoder>> decoders;

    // 创建多路解码器
    for (int i = 0; i < videos.size(); ++i) {
        decoders.emplace_back(std::make_unique<VideoDecoder>(videos[i], i, stitch_callback));
    }

    // 启动线程
    for (auto& dec : decoders) dec->start();

    // 等待所有线程结束
    for (auto& dec : decoders) dec->join();

    std::cout << "All videos decoded.\n";
    return 0;
}