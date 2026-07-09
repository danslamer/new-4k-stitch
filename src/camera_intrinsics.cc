// camera_intrinsics.cc - 实现 camchain yaml 加载 + 畸变 map 生成 (v3.x.2, 2026-07-09)
//
// 路径:
//   LoadCamchain(path) → 读 OpenCV FileStorage KMat / D / RMat / width / height
//   BuildUndistortMap(ci, live_w, live_h) → initUndistortRectifyMap 输出 CV_32FC1
//
// K 缩放逻辑 (与 src/stitching_param_generater.cc::InitUndistortMap 一致):
//   live 与 calib 同分辨率 → 直接用
//   live > calib → 按比例放大 fx/fy/cx/cy
//   live < calib → 缩小 (本项目不会出现)
//
// 异常保护: cv::FileStorage 抛 cv::Exception / std::exception 都 catch, 返回 false,
// 上层走 no-undistort 路径. 不让 boot 阶段因为 yaml 缺失/损坏崩进程.

#include "camera_intrinsics.h"

#include <opencv2/opencv.hpp>
#include <opencv2/calib3d.hpp>

#include <string>
#include <vector>

namespace camera_intrinsics {

bool LoadCamchain(const std::string& yaml_path, CamchainIntrinsics* out) {
    if (out == nullptr) return false;
    out->valid = false;

    cv::FileStorage fs;
    try {
        fs.open(yaml_path, cv::FileStorage::READ);
    } catch (const cv::Exception&) {
        return false;
    } catch (const std::exception&) {
        return false;
    }
    if (!fs.isOpened()) return false;

    try {
        cv::FileNode kmat_node = fs["KMat"];
        cv::FileNode d_node    = fs["D"];
        cv::FileNode rmat_node = fs["RMat"];
        if (kmat_node.empty() || d_node.empty() || rmat_node.empty()) {
            fs.release();
            return false;
        }
        kmat_node >> out->K;
        d_node    >> out->D;
        rmat_node >> out->R;
        if (out->K.empty() || out->K.rows != 3 || out->K.cols != 3) {
            fs.release();
            return false;
        }
        if (out->D.empty()) {
            fs.release();
            return false;
        }
        if (out->R.empty()) {
            out->R = cv::Mat::eye(3, 3, CV_64F);
        }
        // 标定分辨率 (calib): 优先 width/height, 缺则用 resolution 数组
        int w = 0, h = 0;
        cv::FileNode wh_node = fs["width"];
        if (!wh_node.empty()) w = static_cast<int>(wh_node);
        cv::FileNode hh_node = fs["height"];
        if (!hh_node.empty()) h = static_cast<int>(hh_node);
        if (w <= 0 || h <= 0) {
            cv::FileNode res_node = fs["resolution"];
            if (!res_node.empty() && res_node.isSeq() && res_node.size() >= 2) {
                w = static_cast<int>(res_node[0]);
                h = static_cast<int>(res_node[1]);
            }
        }
        if (w > 0) out->calib_w = w;
        if (h > 0) out->calib_h = h;
    } catch (const cv::Exception&) {
        fs.release();
        return false;
    } catch (const std::exception&) {
        fs.release();
        return false;
    }

    fs.release();
    out->valid = true;
    return true;
}

bool BuildUndistortMap(const CamchainIntrinsics& ci,
                       int live_w, int live_h,
                       cv::Mat* undist_xmap, cv::Mat* undist_ymap) {
    if (!ci.valid || undist_xmap == nullptr || undist_ymap == nullptr) return false;
    if (live_w <= 0 || live_h <= 0) return false;
    if (ci.K.empty() || ci.D.empty() || ci.R.empty()) return false;

    // K 缩放到 live 分辨率
    cv::Mat K_live;
    ci.K.convertTo(K_live, CV_64F);
    if (ci.calib_w > 0 && ci.calib_h > 0 &&
        (ci.calib_w != live_w || ci.calib_h != live_h)) {
        const double sx = static_cast<double>(live_w) / ci.calib_w;
        const double sy = static_cast<double>(live_h) / ci.calib_h;
        K_live.at<double>(0, 0) *= sx;  // fx
        K_live.at<double>(1, 1) *= sy;  // fy
        K_live.at<double>(0, 2) *= sx;  // cx
        K_live.at<double>(1, 2) *= sy;  // cy
    }

    cv::Mat R = ci.R.empty() ? cv::Mat::eye(3, 3, CV_64F) : ci.R;
    cv::Mat new_K = K_live;  // 不再 getOptimalNewCameraMatrix (全画幅保留, ROI 留给上层)
    try {
        cv::initUndistortRectifyMap(K_live, ci.D, R, new_K,
                                    cv::Size(live_w, live_h),
                                    CV_32FC1, *undist_xmap, *undist_ymap);
    } catch (const cv::Exception&) {
        return false;
    } catch (const std::exception&) {
        return false;
    }
    if (undist_xmap->empty() || undist_ymap->empty()) return false;
    if (undist_xmap->size() != cv::Size(live_w, live_h)) return false;
    return true;
}

}  // namespace camera_intrinsics