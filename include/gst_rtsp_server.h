// gst_rtsp_server.h
//
// v3.2 (2026-07-09): gst-rtsp-server 包装, 提供两路 RTSP 推流 (stitch + stitch_diff).
//
// 架构:
//   - 全局 GstRtspServer 单例.
//   - 每路输出 = 一个 GstRTSPMediaFactory (mount point) + 一个 stream pipeline 模板
//     appsrc (BGR) -> videoconvert -> mpph264enc (HW) -> h264parse -> rtph264pay
//   - 生产者 (stitch 线程) PushBgrFrame -> 锁 + condvar 队列 -> pump 线程 -> appsrc
//   - pump 线程每路一个, 调 gst_app_src_push_buffer 喂 gstreamer
//   - 共享媒体 (gst_rtsp_media_factory_set_shared): 同一路多个客户端共享一个 encoder.
//
// 板端依赖: gstreamer1.0-rtsp-server-1.0 (= libgstrtspserver-1.0.so.0),
//           gstreamer1.0-rockchip1 (= libgstrockchipmpp.so 含 mpph264enc).

#ifndef GST_RTSP_SERVER_H
#define GST_RTSP_SERVER_H

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "output_streams.h"

namespace gst_rtsp_server {

// 单例.
class GstRtspServer {
 public:
    static GstRtspServer& GetInstance();

    // 启动 server. 调用一次.
    //   server: 服务端配置 (port, auth, ...)
    //   streams: 每路流配置 (path, fps, bitrate, encoder).
    //   diff_threshold: FrameDiff 阈值 (从 server cfg 传过来, 跟 stitch loop 共用)
    //   diff_bbox_min_area: 同上.
    // 返回: true=ok, false=fail (gstreamer 初始化失败 / port 占用等).
    bool Start(const output_streams::ServerConfig& server_cfg,
               const std::vector<output_streams::StreamConfig>& streams);

    void Stop();
    bool IsRunning() const { return running_.load(); }

    // Producer (stitch 线程). 线程安全. bgr 被内部 clone 一份, 调用方释放.
    //   path: 目标流 (匹配 yaml streams 配置的 path)
    //   bgr: BGR 图像 (CV_8UC3)
    //   返回: false = path 未配置 / server 未启动 (producer 应忽略)
    bool PushBgrFrame(const std::string& path, const cv::Mat& bgr);

    int ClientCount(const std::string& path) const;

 private:
    GstRtspServer();
    ~GstRtspServer();
    GstRtspServer(const GstRtspServer&) = delete;
    GstRtspServer& operator=(const GstRtspServer&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
};

}  // namespace gst_rtsp_server

#endif  // GST_RTSP_SERVER_H