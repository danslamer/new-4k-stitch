// output_streams.h
//
// v3.2 (2026-07-09): 输出流配置. 两路 RTSP 推流:
//   - /stitch       拼接完成后的全景画面
//   - /stitch_diff  帧差掩码 (FrameDiff 输出)
//
// 加载自 params/camera_sources.yaml 顶层 `output:` 块 (与 v3.1 计划对齐).
// 与 v3.0 输入 `cameras:` 块并列. 默认输出关闭, 配 `output.enabled: true` 才起 server.

#ifndef OUTPUT_STREAMS_H
#define OUTPUT_STREAMS_H

#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace output_streams {

struct StreamConfig {
    std::string path = "/stitch";        // URL mount point
    int width = 0;                        // 0 = 用 stitcher 实际尺寸
    int height = 0;
    int fps = 30;
    int bitrate_kbps = 4000;
    std::string encoder = "h264_hw";     // h264_hw (mpph264enc) | h264_sw (x264enc)
};

struct ServerConfig {
    bool enabled = false;
    int port = 8554;
    std::string bind_address = "0.0.0.0";
    bool auth_enabled = false;
    std::string auth_user;
    std::string auth_pass;
    bool on_demand = false;              // true = 0 client 时不编码 (省 CPU)
    int diff_threshold = 30;              // FrameDiff 阈值
    int diff_bbox_min_area = 100;
    std::vector<StreamConfig> streams;
};

// 加载自 yaml 顶层 `output:` 块. 解析失败返回默认 (enabled=false).
ServerConfig LoadFromYaml(const std::string& yaml_path);

// 全局配置, gst_rtsp_server 启动时读. 由 LoadFromYaml 填充.
const ServerConfig& GetConfig();

}  // namespace output_streams

#endif  // OUTPUT_STREAMS_H