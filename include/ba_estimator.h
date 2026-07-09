// ba_estimator.h
//
// v3.x.3 (2026-07-09): OpenCV stitcher pipeline 移植 — SIFT 特征 + BestOf2NearestMatcher
//   + HomographyBasedEstimator + BundleAdjuster (默认 AffinePartial, 不依赖 LAPACK/LM)
//   + AffineWarper::buildMaps. 移植自 gpu-based-image-stitching-dataset 参考实现.
//
//   设计要点:
//   - 跟 StitchingParamGenerator 不同 (它 cv::UMat + 全 spherical warper, 不是当前 2x3 grid 兼容的),
//     这里专为我们的 2x3 cell 做设计:
//       * cells[i] = tasks[i].{src_w, src_h, dst_x, dst_y}
//       * final_xmap/ymap 直接 bake 到 cell 大小, GLES warper 不需要改
//       * camchain yaml 没数据时走 Intrinsics::ForCam 占位 K (与 v3.x.2 一致)
//   - WAVE_CORRECT 关闭: 我们的 2x3 grid 已经定好布局, waveCorrect 会改全局 R 让 buildMaps
//     输出 ROI 跟 cell 对不齐.
//   - BA 默认 AffinePartial (no LAPACK), env STITCH_BA_COST 可切 reproj/no.
//   - cam 个数 = 6 (kNumCams), 用 vector 保接口灵活, 内部按 6 路处理.
//   - 失败返回 false → 调用方 (app.cc) 走老的 BuildAffineWarpData 兜底.

#ifndef IMAGE_STITCHING_BA_ESTIMATOR_H
#define IMAGE_STITCHING_BA_ESTIMATOR_H

#include <opencv2/opencv.hpp>

#include <string>
#include <vector>

namespace ba_estimator {

struct BaResult {
    bool success = false;
    cv::Mat K[6];        // 3x3 CV_64F, BA 之前的 K (通常等于 K_live 输入)
    cv::Mat R[6];        // 3x3 CV_32F, BA + 不 waveCorrect
    cv::Mat K_live[6];   // 3x3 CV_32F, K 缩放到 live 分辨率 (2560x1440)
};

// 单帧 6 路 BGR (从已 capture 的 NV12 → BGR 转换, 与现有 ExportHardwareFrameToBgr 一致).
// camchain_dir = "../params" (相对 cwd, 板端 build/ 下启动); 找不到的 cam 用 Intrinsics 占位 K.
//
// env:
//   STITCH_BA_MATCH_CONF = 0.65 (默认)
//   STITCH_BA_COST       = "affine" | "reproj" | "no" (默认 "affine")
//   STITCH_BA_DOWNSAMPLE = 1 (默认; >1 时把 SIFT 输入 downsample, 实验用)
//
// 失败时 out->success = false, K/R/K_live 全部空. 不要假设 R==eye 表示失败 (cam0 可能本来就是 eye).
bool EstimateCameraParamsBA(const std::vector<cv::Mat>& frames_bgr,
                            const std::string& camchain_dir,
                            BaResult* out);

struct StitcherWarpOutput {
    cv::Mat final_xmap[6];    // CV_32FC1, size = cell (tasks[i].src_w x tasks[i].src_h)
    cv::Mat final_ymap[6];    // CV_32FC1, 同上
    bool any_valid = false;
};

// 把 BA 算出来的 K/R + camchain undist 合成到 2x3 cell 大小的 xmap/ymap.
//
// 算法 (per cam):
//   1. cv::AffineWarper warper; warper->buildMaps(Size(live_w, live_h), K_live, R, reproj_x, reproj_y)
//      reproj_x/y 大小 = warped ROI, inverse map: 每个 pano 像素 → source 像素坐标.
//   2. cv::remap(undist_xmap[i], combined_x, reproj_x, reproj_y, INTER_LINEAR, BORDER_CONSTANT, -1)
//      等价于 "reproj_inv ∘ undist_inv" 一次性 bake 出 warped ROI 尺寸的反向 remap 表.
//   3. crop combined_x/y 到 (dst_x[i], dst_y[i], src_w[i], src_h[i]) = 2x3 cell 大小.
//      越界 (cell rect 在 warped ROI 之外) 的 cam 标记 invalid, 调用方会跳过走兜底.
//
// 失败返回 false → 调用方走 BuildAffineWarpData 兜底 (保持 v3.x.2 路径不变).
//
// cell_*_w/h (cell 尺寸, 单 cam 输出尺寸) 和 cell_*_x/y (cell 在 panorama 的位置, 全局坐标)
// 都按 BuildStitchLayout2x3 的输出. live_w/live_h = 单 cam live 分辨率 (默认 2560x1440).
bool BuildStitcherWarpMaps(const BaResult& ba,
                           const std::vector<cv::Mat>& undist_xmap_vector,
                           const std::vector<cv::Mat>& undist_ymap_vector,
                           const int cell_src_w[6],
                           const int cell_src_h[6],
                           const int cell_dst_x[6],
                           const int cell_dst_y[6],
                           int live_w, int live_h,
                           StitcherWarpOutput* out);

}  // namespace ba_estimator

#endif  // IMAGE_STITCHING_BA_ESTIMATOR_H
