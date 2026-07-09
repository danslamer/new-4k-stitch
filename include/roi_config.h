#ifndef ROI_CONFIG_H
#define ROI_CONFIG_H

#include <string>

// v3.x (2026-07-09): 单路相机的 ROI 矩形, 直接保存"剪裁哪一块"而不是相对偏移.
//   yaml 里 cam{i}: { x, y, width, height, affine }, x/y 是源帧坐标 (无 warp 时) 或
//   参照帧坐标 (有 warp 时, 见 affine 字段), width/height 是剪裁尺寸.
//   valid = false 表示该 cam 还没被 bootstrap 写入, BuildCameraRois 走 overlap 兜底.
//
//   affine: 2x3 行主序 [a b c d tx ty] cv::Mat 表示法 (与 cv::Mat::at<double>(2,3) 一致).
//     含义: 把 cam i 的源帧像素 (sx, sy) 映射到参照帧像素 (px, py):
//       px = a*sx + b*sy + tx
//       py = c*sx + d*sy + ty
//     默认 = 单位矩阵 (a=1, b=0, c=0, d=1, tx=0, ty=0), 表示 no-warp. 由 App::BootStrapOptimalLayout
//     通过 cv::estimateAffinePartial2D (特征匹配 vs cam0) 计算出来, 写到 yaml 后每次启动复用.
struct CameraRoiRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    double affine[6] = {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
    // v3.x.3 (2026-07-09): OpenCV stitcher pipeline 移植 — 每路 cam 持久化 SIFT+BA 算出的
    //   3x3 K (内参, 占位/标定, 行主序) 和 3x3 R (相机旋转, BA 输出). have_ba_R = true 时
    //   表示该 cam 的 R 是 BA 跑的, 可以走 AffineWarper::buildMaps 路径.
    //   缺省值占位 = Intrinsics::fill_K (fx=1027 fy=1378 cx=1280 cy=720) + 单位阵 R.
    double K[9] = {1027.0, 0.0, 1280.0, 0.0, 1378.0, 720.0, 0.0, 0.0, 1.0};
    double R[9] = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    bool have_ba_R = false;
    bool valid = false;

    CameraRoiRect() = default;
    CameraRoiRect(int x_, int y_, int w_, int h_)
        : x(x_), y(y_), width(w_), height(h_), valid(true) {}

    bool has_affine() const {
        return !(affine[0] == 1.0 && affine[1] == 0.0
              && affine[2] == 0.0 && affine[3] == 1.0
              && affine[4] == 0.0 && affine[5] == 0.0);
    }
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