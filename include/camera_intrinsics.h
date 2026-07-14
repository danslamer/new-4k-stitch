#ifndef IMAGE_STITCHING_CAMERA_INTRINSICS_H
#define IMAGE_STITCHING_CAMERA_INTRINSICS_H

// =====================================================================
// camera_intrinsics.h - camera intrinsics + per-camera pose
// =====================================================================
// Sprint 4-A (2026-07-14): GC4683 @ RK3576 MIPI 直连 / FOV 101x68 deg.
//
// 默认值推导 (FOV 反推):
//   fx = (W/2) / tan(H-FOV/2), fy = (H/2) / tan(V-FOV/2).
//   默认 W=2560, H=1440, H-FOV=101 deg, V-FOV=68 deg:
//     fx = 1280 / tan(50.5 deg)  ~= 1052.5541
//     fy =  720 / tan(34 deg)    ~= 1068.8894
//     cx = 1280, cy =  720 (图像主点, 默认 W/2 / H/2)
//     k1 = k2 = p1 = p2 = 0 (Sprint 4-A 占位; 后续 Sprint 5-D 自标定覆盖).
//
// 加载机制:
//   src/app.cc::App::App() 启动期会调 camera_intrinsics::CalibrationConfig::
//   LoadOrDefault("../params/calibration.yaml"). 缺 file / 无 / 解析失败 ->
//   保留默认; 部分字段缺失 -> 仅覆盖存在字段.
//
// 与各模块的关系:
//   * ImageStitcher::SetLayout 读 tuning.yaw_h_deg / yaw_v_deg (BUILD LAYOUT 4-B)
//   * App::InitFromConfig / BootStrapOptimalLayout 启动日志打印每 cam fx/fy/yaw
//   * 后续 Sprint 5-D IPM plane warp 会接 K + R 直出
// =====================================================================

#include <array>
#include <string>

namespace camera_intrinsics {

constexpr int kNumCams = 6;

struct Intrinsics {
    // 图像分辨率 (像素)
    int    image_w = 2560;
    int    image_h = 1440;

    // K 矩阵 (3x3 row-major):
    //   [ fx   0  cx ]
    //   [  0  fy  cy ]
    //   [  0   0   1 ]
    double fx        = 1052.5541;   // = 1280 / tan(50.5 deg), FOV 101 deg horizontal
    double fy        = 1068.8894;   // =  720 / tan(34 deg),   FOV  68 deg vertical
    double cx        = 1280.0;      // image_w * 0.5
    double cy        =  720.0;      // image_h * 0.5

    // 物理姿态 (相机相对支架坐标的偏角, degree)
    //   yaw_h_deg: 水平方向偏角 (panorama +x 顺时针为正)
    //   yaw_v_deg: 垂直方向偏角 (panorama +y 顺时针为正)
    // 默认值在 params/calibration.yaml; 这里默认 0 让 App 启动期 log 可见.
    double yaw_h_deg = 0.0;
    double yaw_v_deg = 0.0;

    // 径向/切向畸变 (Sprint 4-A 默认 0; 后续 Sprint 5-D 自标定覆盖)
    double k1 = 0.0;
    double k2 = 0.0;
    double p1 = 0.0;
    double p2 = 0.0;

    void fill_K(double K[9]) const {
        K[0]=fx;  K[1]=0.0; K[2]=cx;
        K[3]=0.0; K[4]=fy;  K[5]=cy;
        K[6]=0.0; K[7]=0.0; K[8]=1.0;
    }

    // 由 image_w 与 h_fov_deg 反推 fx (Sprint 4-A 用于默认或 yaml 覆盖).
    //   fx = (W/2) / tan(H-FOV/2)
    static double FovToFx(int image_w, double h_fov_deg) {
        const double half = h_fov_deg * 0.5 * 3.14159265358979323846 / 180.0;
        return (image_w * 0.5) / std::tan(half);
    }
    static double FovToFy(int image_h, double v_fov_deg) {
        const double half = v_fov_deg * 0.5 * 3.14159265358979323846 / 180.0;
        return (image_h * 0.5) / std::tan(half);
    }
};

// 全局 Intrinsics 阵列 (6 路).
//   App::App() 启动期用 CalibrationConfig::LoadOrDefault() 覆盖.
//   ImageStitcher::SetLayout (Sprint 4-B 之后) 会读 ForCam(i).
// 取可写引用: MutableIntrinsics(); 取只读引用: ForCam(i).
std::array<Intrinsics, kNumCams>& MutableIntrinsics();
const Intrinsics& ForCam(int i);  // i 越界返回 cam0 默认

// yaml 加载 / 保存 (cv::FileStorage 风格, 与 roi_tuning.yaml / camera_sources.yaml 同家族)
//   缺 file / cv::Exception -> 保留默认.
struct CalibrationConfig {
    static bool LoadOrDefault(const std::string& path);
    static bool SaveToFile(const std::string& path);
};

}  // namespace camera_intrinsics

#endif  // IMAGE_STITCHING_CAMERA_INTRINSICS_H
