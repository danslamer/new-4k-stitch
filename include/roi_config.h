#ifndef ROI_CONFIG_H
#define ROI_CONFIG_H

#include <string>

// v3.x (2026-07-09): 单路相机的 ROI 矩形, 直接保存"剪裁哪一块"而不是相对偏移.
//   yaml 里 cam{i}: { x, y, width, height }, x/y 是源帧坐标, width/height 是剪裁尺寸.
//   valid = false 表示该 cam 还没被 bootstrap 写入, BuildCameraRois 走 overlap 兜底.
struct CameraRoiRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool valid = false;

    CameraRoiRect() = default;
    CameraRoiRect(int x_, int y_, int w_, int h_)
        : x(x_), y(y_), width(w_), height(h_), valid(true) {}
};

struct StitchGlobalConfig {
    CameraRoiRect camera_rois[6];

    int feather_width = 120;
    double feather_strength = 2.0;
    bool feather_enabled = true;

    bool save_enabled = false;
    int save_interval = 30;

    int step_size = 1;
    std::string mode = "dataset";
    int selected_cam = 1;
};

class RoiConfig {
public:
    static bool LoadFromFile(const std::string& path, StitchGlobalConfig& config);
    static bool SaveToFile(const std::string& path, const StitchGlobalConfig& config);
};

#endif