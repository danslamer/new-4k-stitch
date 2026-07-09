// camera_intrinsics.h - 相机内参占位 (v3.0 / 2026-07-08)
//
// Sprint 0 占位值 (基于 2.8mm 镜头 FOV 102.5°/55.2° 反推, 不依赖 sensor 物理尺寸):
//   fx = (W/2) / tan(H-FOV/2) = 1280 / tan(51.25°) = 1027
//   fy = (H/2) / tan(V-FOV/2) =  720 / tan(27.6°)  = 1378
//   cx = W/2 = 1280, cy = H/2 = 720
//   畸变系数 k1=k2=p1=p2=0 (Sprint 2 标定覆盖)
//
// 推导 / 估算依据见 docs/NETWORK_CAMERA_PLAN.md §0.5.
//
// Sprint 2 落地:
//   - tools/calibrate_intrinsics.py 跑出实测量 → 写到 params/camchain_*.yaml
//   - App 在启动期读 camchain_*.yaml 把 IntrinsicsPlaceholder 覆盖
//
// v3.x.2 (2026-07-09): 加 LoadCamchain() + initUndistortRectifyMap 路径.
//   只对水平相邻 cam pair (h01/h23/h45) 做畸变校正, 垂直对 (v02/v13/v24/v35)
//   不做 (overlap 小, 校正后 ROI 还可能错位). 把 undist_xmap/undist_ymap 合成
//   进 GLES warper 的 xmap/ymap, GPU 一遍搞定畸变+仿射, 不破 DMA-BUF 路径.
//
#ifndef IMAGE_STITCHING_CAMERA_INTRINSICS_H
#define IMAGE_STITCHING_CAMERA_INTRINSICS_H

#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

namespace camera_intrinsics {

struct Intrinsics {
    int    image_w   = 2560;
    int    image_h   = 1440;
    double fx        = 1027.0;
    double fy        = 1378.0;
    double cx        = 1280.0;
    double cy        =  720.0;
    double k1        = 0.0;
    double k2        = 0.0;
    double p1        = 0.0;
    double p2        = 0.0;

    // 单 camera 的 K 矩阵 (3x3 row-major)
    void fill_K(double K[9]) const {
        K[0]=fx;  K[1]=0.0; K[2]=cx;
        K[3]=0.0; K[4]=fy;  K[5]=cy;
        K[6]=0.0; K[7]=0.0; K[8]=1.0;
    }

    // 6 路 camera 各占一份 (Sprint 0 默认全相同; Sprint 2 覆盖)
    static constexpr int kNumCams = 6;
    static const Intrinsics& ForCam(int /*i*/) {
        // 全部 cam 一组默认占位
        static const Intrinsics kDefault;
        return kDefault;
    }
};

// v3.x.2 (2026-07-09): 从 params/camchain_<cam_id>.yaml 读 KMat/D/RMat/width/height.
//   yaml 用 OpenCV FileStorage (!!opencv-matrix). camchain_<i>.yaml 是相对工程根目录.
//   返回 false 时 (文件不存在 / 字段缺失) 让上层决定是否走 fallback 单位阵.
//   calib_width/calib_height 输出 yaml 里 width/height 字段 (用于 K 缩放, 我们的
//   标定用 1920x1080 但 live stream 是 2560x1440, 必须 scale K).
struct CamchainIntrinsics {
    cv::Mat K;          // 3x3 CV_64F
    cv::Mat D;          // Nx1 CV_64F (畸变系数)
    cv::Mat R;          // 3x3 CV_64F (rectify 旋转, cam0 = eye)
    int     calib_w = 1920;
    int     calib_h = 1080;
    bool    valid = false;
};

bool LoadCamchain(const std::string& yaml_path, CamchainIntrinsics* out);

// v3.x.2: 给定 camchain K/D/R + 标定分辨率 + live 流分辨率, 输出 live 流坐标系下的
//   undist_xmap / undist_ymap (CV_32FC1). 走 cv::initUndistortRectifyMap, K 自动
//   按比例缩放 (fx/fy/cx/cy * scale). D 不变. R 不变.
//   返回 false 时 (D 空 / R 空 / 尺寸非法) 不写 map.
bool BuildUndistortMap(const CamchainIntrinsics& ci,
                       int live_w, int live_h,
                       cv::Mat* undist_xmap, cv::Mat* undist_ymap);

}  // namespace camera_intrinsics

#endif  // IMAGE_STITCHING_CAMERA_INTRINSICS_H