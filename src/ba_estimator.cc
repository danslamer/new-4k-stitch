// ba_estimator.cc
//
// v3.x.3 (2026-07-09): 把 OpenCV stitcher pipeline 移植到 new-4k-stitch.
//   路径: gpu-based-image-stitching-dataset/src/stitching_param_generater.cc
//   改写点:
//   - cv::UMat → cv::Mat (CPU, 一次性, 不再要 OpenCL UMat 后端)
//   - 默认走 AffineWarper (spherical 同名冲突); 不 waveCorrect (2x3 grid 已固定布局)
//   - 默认 BA = AffinePartial (不依赖 LAPACK/LM); env STITCH_BA_COST 可切
//   - 不做 Blender/Timelapser/ExposureCompensator (OpenCL seam 已用)
//   - cell 大小 + dst_x/y 由 BuildStitchLayout2x3 决定, 不被 warper 的 ROI 限制
//
// 调用方约定:
//   frames_bgr[i] = 单路 capture 后的 BGR (从 ExportHardwareFrameToBgr 出来, 大小 = live_w x live_h)
//   camchain_dir = "../params" (cwd = build/ 时)
//   失败返回 success=false, K/R/K_live 字段空, 调用方降级到 BuildAffineWarpData.

#include "ba_estimator.h"
#include "camera_intrinsics.h"

#include <opencv2/features2d.hpp>
#include <opencv2/stitching/detail/matchers.hpp>
// OpenCV 4.x mainline 把 HomographyBasedEstimator + BundleAdjusterBase 系都合并到
// motion_estimators.hpp 里 (estimators.hpp / reprojection.hpp 是 contrib 时代的旧路径, 已删除).
#include <opencv2/stitching/detail/motion_estimators.hpp>
#include <opencv2/stitching/detail/warpers.hpp>
#include <opencv2/stitching/detail/camera.hpp>

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "logger.h"

namespace ba_estimator {

namespace {

const char* GetEnv(const char* key, const char* fallback) {
    const char* v = std::getenv(key);
    return (v != nullptr && *v != '\0') ? v : fallback;
}

std::string FormatMatrixRowMajor(const cv::Mat& m) {
    if (m.empty()) return "<empty>";
    std::ostringstream ss;
    ss << "[";
    for (int r = 0; r < m.rows; ++r) {
        if (r > 0) ss << "; ";
        for (int c = 0; c < m.cols; ++c) {
            if (c > 0) ss << ",";
            ss << m.at<double>(r, c);
        }
    }
    ss << "]";
    return ss.str();
}

}  // namespace

bool EstimateCameraParamsBA(const std::vector<cv::Mat>& frames_bgr,
                            const std::string& camchain_dir,
                            BaResult* out) {
    using namespace cv::detail;

    if (out == nullptr) return false;
    for (int i = 0; i < 6; ++i) {
        out->K[i] = cv::Mat();
        out->R[i] = cv::Mat();
        out->K_live[i] = cv::Mat();
    }
    out->success = false;

    const int N = static_cast<int>(frames_bgr.size());
    if (N < 2) return false;
    if (N > 6) {
        Logger::GetInstance().Log(
            "[ba_estimator] more than 6 cams, BA runs on first 6 only");
    }
    const int n = std::min(N, 6);

    // env 配置
    const float match_conf = static_cast<float>(
        std::atof(GetEnv("STITCH_BA_MATCH_CONF", "0.65")));
    const std::string ba_cost = GetEnv("STITCH_BA_COST", "affine");
    const int downsample = std::max(1, std::atoi(GetEnv("STITCH_BA_DOWNSAMPLE", "1")));

    std::vector<cv::Mat> frames_for_sift;
    frames_for_sift.reserve(n);
    for (int i = 0; i < n; ++i) {
        cv::Mat f = frames_bgr[i];
        if (f.empty()) {
            Logger::GetInstance().LogError(
                "[ba_estimator] empty frame at index " + std::to_string(i));
            return false;
        }
        if (downsample > 1) {
            cv::Mat small;
            cv::resize(f, small, cv::Size(f.cols / downsample, f.rows / downsample),
                       0, 0, cv::INTER_AREA);
            frames_for_sift.push_back(small);
        } else {
            frames_for_sift.push_back(f);
        }
    }

    try {
        // 1. SIFT 特征
        cv::Ptr<cv::Feature2D> finder = cv::SIFT::create();
        std::vector<ImageFeatures> features(n);
        for (int i = 0; i < n; ++i) {
            computeImageFeatures(finder, frames_for_sift[i], features[i]);
            features[i].img_idx = i;
        }

        // 2. Best-of-2 pairing (CPU; CUDA 不支持, RK3588 Mali 走 OpenCL 后端未启)
        std::vector<MatchesInfo> pairwise_matches;
        cv::Ptr<FeaturesMatcher> matcher =
            cv::makePtr<BestOf2NearestMatcher>(false, match_conf);
        (*matcher)(features, pairwise_matches);
        matcher->collectGarbage();

        // 3. 每路 cam 的 K init: camchain_<i>.yaml → placeholder Intrinsics.
        //    注意 placeholder K 在我们的项目里是给 1920x1080 标定回推的, 但 live 是
        //    2560x1440, 这里 live 坐标系下直接用 fx/fy/cx/cy (已经按 live 标).
        std::vector<CameraParams> cam_params(n);
        for (int i = 0; i < n; ++i) {
            cv::Mat K;
            camera_intrinsics::CamchainIntrinsics ci;
            const std::string yaml_path = camchain_dir + "/camchain_" +
                                          std::to_string(i) + ".yaml";
            const bool loaded = camera_intrinsics::LoadCamchain(yaml_path, &ci);
            if (loaded) {
                ci.K.convertTo(K, CV_64F);
                const int live_w = frames_bgr[i].cols;
                const int live_h = frames_bgr[i].rows;
                if (ci.calib_w > 0 && ci.calib_h > 0 &&
                    (ci.calib_w != live_w || ci.calib_h != live_h)) {
                    const double sx = static_cast<double>(live_w) / ci.calib_w;
                    const double sy = static_cast<double>(live_h) / ci.calib_h;
                    K.at<double>(0, 0) *= sx;
                    K.at<double>(1, 1) *= sy;
                    K.at<double>(0, 2) *= sx;
                    K.at<double>(1, 2) *= sy;
                }
            } else {
                const auto& ph = camera_intrinsics::Intrinsics::ForCam(i);
                K = cv::Mat::eye(3, 3, CV_64F);
                K.at<double>(0, 0) = ph.fx;
                K.at<double>(1, 1) = ph.fy;
                K.at<double>(0, 2) = ph.cx;
                K.at<double>(1, 2) = ph.cy;
            }
            cam_params[i].K() = K.clone();
            cam_params[i].R = cv::Mat::eye(3, 3, CV_32F);
        }

        // 4. Homography 估计
        cv::Ptr<Estimator> estimator = cv::makePtr<HomographyBasedEstimator>();
        if (!(*estimator)(features, pairwise_matches, cam_params)) {
            Logger::GetInstance().LogError(
                "[ba_estimator] HomographyBasedEstimator failed");
            return false;
        }

        // HomographyBasedEstimator 返回 CV_32F R, 留给 BundleAdjuster 内部用.
        for (auto& cp : cam_params) {
            if (cp.R.type() != CV_32F) {
                cv::Mat r32;
                cp.R.convertTo(r32, CV_32F);
                cp.R = r32;
            }
        }

        // 5. Bundle adjustment
        cv::Ptr<BundleAdjusterBase> adjuster;
        if (ba_cost == "reproj") {
            adjuster = cv::makePtr<BundleAdjusterReproj>();
        } else if (ba_cost == "no") {
            adjuster = cv::makePtr<NoBundleAdjuster>();
        } else {
            adjuster = cv::makePtr<BundleAdjusterAffinePartial>();
        }
        adjuster->setConfThresh(1.0f);
        cv::Mat_<uchar> refine_mask = cv::Mat::zeros(3, 3, CV_8U);
        if (ba_cost != "no") {
            // 解 fx, fy, cx, cy (典型 6-cam 配置)
            refine_mask(0, 0) = 1;  // fx
            refine_mask(1, 1) = 1;  // fy
            refine_mask(0, 2) = 1;  // cx
            refine_mask(1, 2) = 1;  // cy
        }
        adjuster->setRefinementMask(refine_mask);
        if (!(*adjuster)(features, pairwise_matches, cam_params)) {
            Logger::GetInstance().LogError(
                "[ba_estimator] BundleAdjuster failed (cost=" + ba_cost + ")");
            return false;
        }

        // 6. SKIP waveCorrect. 2x3 grid 已经固定布局, waveCorrect 会改全局 R 让 buildMaps
        //    输出 ROI 跟 cell 对不齐.

        // 7. 输出
        for (int i = 0; i < n; ++i) {
            out->K[i] = cam_params[i].K().clone();
            if (cam_params[i].R.empty() || cam_params[i].R.rows != 3 ||
                cam_params[i].R.cols != 3) {
                Logger::GetInstance().LogError(
                    "[ba_estimator] cam" + std::to_string(i) +
                    " R invalid after BA");
                return false;
            }
            out->R[i] = cam_params[i].R.clone();
            out->K_live[i] = cam_params[i].K().clone();
            out->K_live[i].convertTo(out->K_live[i], CV_32F);
        }

        // 8. 日志: R[0..2] 三个 cam 各打印 3x3
        std::ostringstream rlog;
        rlog << "[ba_estimator] BA succeeded n=" << n
             << " cost=" << ba_cost
             << " match_conf=" << match_conf
             << " downsample=" << downsample;
        Logger::GetInstance().Log(rlog.str());
        for (int i = 0; i < n; ++i) {
            std::ostringstream row;
            row << "[ba_estimator] cam" << i
                << " R=" << FormatMatrixRowMajor(out->R[i])
                << " K_live=" << FormatMatrixRowMajor(out->K_live[i]);
            Logger::GetInstance().Log(row.str());
        }

        // 9. 退化检测: 如果所有 cam 的 R 都是单位阵 / 接近单位阵 (= BA 没找到匹配,
        //   或 HomographyBasedEstimator 退化 / SIFT features 太差), 那 BA 输出不能用 —
        //   cell 内 pixel 会全打到 OOB, 大部分输出黑屏. 标记 success=false 走兜底.
        bool all_identity = true;
        for (int i = 0; i < n; ++i) {
            if (out->R[i].empty()) continue;
            cv::Mat R = out->R[i].clone();
            if (R.type() != CV_64F) R.convertTo(R, CV_64F);
            const double tx = std::fabs(R.at<double>(0,2));
            const double ty = std::fabs(R.at<double>(1,2));
            const double off01 = std::fabs(R.at<double>(0,1));
            const double off10 = std::fabs(R.at<double>(1,0));
            if (tx > 0.05 || ty > 0.05 || off01 > 0.05 || off10 > 0.05) {
                all_identity = false;
                break;
            }
        }
        if (all_identity) {
            Logger::GetInstance().LogError(
                "[ba_estimator] BA R matrices all near-identity, matches too weak or "
                "homography decomposition failed; treating BA as failure, "
                "falling back to BuildAffineWarpData");
            out->success = false;
            return false;
        }
        out->success = true;
        return true;
    } catch (const cv::Exception& e) {
        Logger::GetInstance().LogError(
            std::string("[ba_estimator] cv::Exception: ") + e.what());
        return false;
    } catch (const std::exception& e) {
        Logger::GetInstance().LogError(
            std::string("[ba_estimator] std::exception: ") + e.what());
        return false;
    }
}

bool BuildStitcherWarpMaps(const BaResult& ba,
                           const std::vector<cv::Mat>& undist_xmap_vector,
                           const std::vector<cv::Mat>& undist_ymap_vector,
                           const int cell_src_w[6],
                           const int cell_src_h[6],
                           const int cell_dst_x[6],
                           const int cell_dst_y[6],
                           int live_w, int live_h,
                           StitcherWarpOutput* out) {
    using namespace cv::detail;

    if (out == nullptr) return false;
    if (!ba.success || live_w <= 0 || live_h <= 0) return false;
    if (static_cast<int>(undist_xmap_vector.size()) < 6) return false;
    out->any_valid = false;

    try {
        const bool have_undist[6] = {
            !undist_xmap_vector[0].empty() && !undist_ymap_vector[0].empty(),
            !undist_xmap_vector[1].empty() && !undist_ymap_vector[1].empty(),
            !undist_xmap_vector[2].empty() && !undist_ymap_vector[2].empty(),
            !undist_xmap_vector[3].empty() && !undist_ymap_vector[3].empty(),
            !undist_xmap_vector[4].empty() && !undist_ymap_vector[4].empty(),
            !undist_xmap_vector[5].empty() && !undist_ymap_vector[5].empty(),
        };

        for (int i = 0; i < 6; ++i) {
            if (ba.R[i].empty() || ba.K_live[i].empty()) continue;
            const int cell_w = cell_src_w[i];
            const int cell_h = cell_src_h[i];
            if (cell_w < 2 || cell_h < 2) continue;
            const int cdx = cell_dst_x[i];
            const int cdy = cell_dst_y[i];

            // Per-pixel inverse warp:
            //   For each cell pixel (px_local, py_local):
            //     pano (px, py) = (cdx + px_local, cdy + py_local)
            //     Apply inverse K * R^-1 * K^-1 to (px, py, 1)^T -> (rectified sx, rectified sy, 1)
            //     (rectified sx, rectified sy) is a coordinate in the rectified image plane
            //     After undistort map: distorted (sx, sy) = undist_xmap[sy][sx], undist_ymap[sy][sx]
            //
            // 我们不依赖 cv::AffineWarper::buildMaps 的 ROI — 因为 BA 输出没有 translation,
            // warper 的 ROI 不带 cell offset; 在没有 waveCorrect 的情况下, warper 输出只是
            // source 投影, 不能直接 crop 到 (cdx, cdy). 自己算更稳: 把 cell 局部坐标
            // (px_local, py_local) 视作 warper 的局部输出, 直接送 K * R^-1 * K^-1.

            cv::Mat K_live = ba.K_live[i].clone();
            cv::Mat R = ba.R[i].clone();
            if (K_live.type() != CV_64F) K_live.convertTo(K_live, CV_64F);
            if (R.type() != CV_64F)     R.convertTo(R, CV_64F);

            // 关键: BundleAdjusterAffinePartial 的 R 是 3x3 homogeneous affine (含平移
            // 在第三行, 形式 [A | t; 0 0 1]), 不是纯旋转. 对 affine 来说 R^T != R^-1, 必须
            // 用 cv::Mat::inv() 拿逆矩阵. 用 R.t() 会让 inverse warp 错位 (translation 段
            // 反向但 rot+shear 段没真做逆), panorama 上 cam 1+ 内容水平错位 + ROI 不对齐.
            cv::Mat R_inv = R.inv();
            cv::Mat K_inv = K_live.inv();
            // H_inv = K * R^-1 * K^-1 — 把 panorama 像素映射回 rectified source 像素.
            // 注意 K_inv 是从 K 反推的浮点逆, 数值稳定 (K 几乎是 diag + 几个小量).
            cv::Mat H_inv = K_live * R_inv * K_inv;

            // 诊断: 检测 R 是不是退化到单位 / 全零. 是的话 BuildOrFallbackWarpData 会兜底.
            const double r_trace = R.at<double>(0,0) + R.at<double>(1,1) + R.at<double>(2,2);
            const double r_det = cv::determinant(R);
            std::ostringstream detlog;
            detlog << "[ba_estimator] cam" << i
                   << " R_trace=" << r_trace
                   << " det=" << r_det
                   << " tx=" << R.at<double>(0,2)
                   << " ty=" << R.at<double>(1,2);
            Logger::GetInstance().Log(detlog.str());

            const int umap_w = have_undist[i] ? undist_xmap_vector[i].cols : 0;
            const int umap_h = have_undist[i] ? undist_xmap_vector[i].rows : 0;

            cv::Mat xmap = cv::Mat(cell_h, cell_w, CV_32FC1, cv::Scalar(-1.0f));
            cv::Mat ymap = cv::Mat(cell_h, cell_w, CV_32FC1, cv::Scalar(-1.0f));
            const double* h0 = H_inv.ptr<double>(0);
            const double* h1 = H_inv.ptr<double>(1);
            const double* h2 = H_inv.ptr<double>(2);

            for (int py = 0; py < cell_h; ++py) {
                float* xrow = xmap.ptr<float>(py);
                float* yrow = ymap.ptr<float>(py);
                const int pano_y = cdy + py;
                for (int px = 0; px < cell_w; ++px) {
                    const int pano_x = cdx + px;
                    // Apply H_inv to (pano_x, pano_y, 1) in homogeneous coords:
                    const double denom = h2[0] * pano_x + h2[1] * pano_y + h2[2];
                    if (std::fabs(denom) < 1e-12) {
                        xrow[px] = -1.0f;
                        yrow[px] = -1.0f;
                        continue;
                    }
                    const double rect_sx = (h0[0] * pano_x + h0[1] * pano_y + h0[2]) / denom;
                    const double rect_sy = (h1[0] * pano_x + h1[1] * pano_y + h1[2]) / denom;

                    // (rect_sx, rect_sy) = rectified source coord. 然后查 undist map 拿
                    // distorted source coord; 没 undist map 时直接用 rectified coord.
                    if (have_undist[i]) {
                        const int rsx_i = static_cast<int>(rect_sx);
                        const int rsy_i = static_cast<int>(rect_sy);
                        if (rsx_i < 0 || rsy_i < 0 ||
                            rsx_i >= umap_w || rsy_i >= umap_h) {
                            xrow[px] = -1.0f;
                            yrow[px] = -1.0f;
                            continue;
                        }
                        xrow[px] = undist_xmap_vector[i].at<float>(rsy_i, rsx_i);
                        yrow[px] = undist_ymap_vector[i].at<float>(rsy_i, rsx_i);
                    } else {
                        if (rect_sx < 0 || rect_sy < 0 ||
                            rect_sx >= live_w || rect_sy >= live_h) {
                            xrow[px] = -1.0f;
                            yrow[px] = -1.0f;
                            continue;
                        }
                        xrow[px] = static_cast<float>(rect_sx);
                        yrow[px] = static_cast<float>(rect_sy);
                    }
                }
            }

            out->final_xmap[i] = xmap;
            out->final_ymap[i] = ymap;
            out->any_valid = true;
        }

        if (out->any_valid) {
            Logger::GetInstance().Log(
                std::string("[ba_estimator] BuildStitcherWarpMaps succeeded, live=") +
                std::to_string(live_w) + "x" + std::to_string(live_h));
        }
        return out->any_valid;
    } catch (const cv::Exception& e) {
        Logger::GetInstance().LogError(
            std::string("[ba_estimator] BuildStitcherWarpMaps cv::Exception: ") +
            e.what());
        return false;
    } catch (const std::exception& e) {
        Logger::GetInstance().LogError(
            std::string("[ba_estimator] BuildStitcherWarpMaps std::exception: ") +
            e.what());
        return false;
    }
}

}  // namespace ba_estimator
