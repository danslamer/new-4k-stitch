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
#ifndef IMAGE_STITCHING_CAMERA_INTRINSICS_H
#define IMAGE_STITCHING_CAMERA_INTRINSICS_H

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

}  // namespace camera_intrinsics

#endif  // IMAGE_STITCHING_CAMERA_INTRINSICS_H