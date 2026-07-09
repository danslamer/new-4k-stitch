#ifndef IMAGE_STITCHING_APP_H
#define IMAGE_STITCHING_APP_H

#include "sensor_data_interface.h"
#include "image_stitcher.h"
#include "logger.h"
#include "nv12_frame.h"
#include "drm_allocator.h"
#include "roi_config.h"
#include "roi_visualizer.h"
#include "frame_diff.h"
#include "output_streams.h"
#include "gst_rtsp_server.h"

#include <memory>
#include <vector>

using namespace std;

static int g_multi_frame_roi_debug_level = 1;
static bool g_save_roi_confidence_debug = true;

extern StitchGlobalConfig g_config;

struct CachedOverlap {
    int h01_overlap = 0;
    int h01_shift_y = 0;
    double h01_score = 0.0;
    int h23_overlap = 0;
    int h23_shift_y = 0;
    double h23_score = 0.0;
    int h45_overlap = 0;
    int h45_shift_y = 0;
    double h45_score = 0.0;
    int v02_overlap = 0;
    int v02_shift_y = 0;
    double v02_score = 0.0;
    int v13_overlap = 0;
    int v13_shift_y = 0;
    double v13_score = 0.0;
    int v24_overlap = 0;
    int v24_shift_y = 0;
    double v24_score = 0.0;
    int v35_overlap = 0;
    int v35_shift_y = 0;
    double v35_score = 0.0;
    double confidence = 0.0;
};

class App {
 public:
    App();
    ~App();

    [[noreturn]] void run_stitching();

 private:
    void BootStrapOptimalLayout();
    void InitFromConfig();
    void RebuildLayout();
    void SyncConfigToGlobals();
    void SaveCurrentFrames();
    void ReleaseSavedFrames();
    void RestitchSavedFrames();

    // v3.2: 输出流 RTSP 推流初始化. 读 yaml 顶层 `output:` 块, 决定是否起 server.
    //   创建 FrameDiff 实例 (按配置 threshold).
    //   启 GstRtspServer (port=server_cfg.port, streams=server_cfg.streams).
    // 返回: true=rtsp output 启用, false=关闭 (默认).
    bool InitRtspOutput();

    size_t num_img_;

    SensorDataInterface sensorDataInterface_;
    ImageStitcher image_stitcher_;

    vector<NV12Frame> image_vector_;

    NV12Frame image_concat_;
    DrmBuffer output_drm_buf_;

    int total_cols_;
    int height_;

    CachedOverlap cached_overlaps_;
    bool visual_mode_;
    bool debug_mode_;

    vector<NV12Frame> saved_frames_;
    vector<DrmBuffer> saved_drm_bufs_;
    bool frames_locked_;
    size_t locked_frame_idx_;

    // 阶段 2: MJPEG panorama 推流 (CameraPage "实时预览" 用).
    // panorama NV12 -> RGA downscale -> small NV12 -> cvtColor -> cv::imencode -> mjpeg_streamer::update().
    // gate: 每 mjpeg_interval_ 帧做一次 (默认 2 = 15 FPS, 避免吃掉 stitch loop 30 FPS 预算).
    DrmBuffer     mjpeg_drm_buf_;
    int           mjpeg_width_   = 960;
    int           mjpeg_height_  = 816;
    int           mjpeg_interval_ = 2;
    int           mjpeg_quality_ = 75;

    // v3.2 (2026-07-09): 帧差掩码 + 2 路 RTSP 推流.
    //   frame_diff_: 持前一帧, 每帧出 BGR 掩码 (dimmed + red highlights + bbox).
    //   rtsp_output_enabled_: yaml output.enabled=true 时为 true.
    //   启: InitRtspOutput() 在 run_stitching() 开头调一次.
    //   跑: 拼接完一帧后, 转换 BGR -> push /stitch, 算 mask -> push /stitch_diff.
    std::unique_ptr<frame_diff::FrameDiff> frame_diff_;
    bool rtsp_output_enabled_ = false;
    output_streams::ServerConfig rtsp_cfg_;  // 缓存, 给 gst_rtsp_server 用
};

#endif