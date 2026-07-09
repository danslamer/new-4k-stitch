// camera_intrinsics.cc - 实现 camchain yaml 加载 + 畸变 map 生成 (v3.x.2, 2026-07-09;
//                                                                 v3.x.4 修复 2026-07-09)
//
// 路径:
//   LoadCamchain(path) → 读 OpenCV FileStorage KMat / D / RMat / width / height
//   BuildUndistortMap(ci, live_w, live_h) → initUndistortRectifyMap 输出 CV_32FC1
//
// K 缩放逻辑 (与 gpu-based-image-stitching-dataset/src/stitching_param_generater.cc
//   ::InitUndistortMap 一致):
//   live 与 calib 同分辨率 → 直接用
//   live > calib → 按比例放大 fx/fy/cx/cy
//   live < calib → 缩小 (本项目不会出现)
//
// v3.x.4 (2026-07-09) 修复: 之前 LoadCamchain 用 cv::FileStorage fs; + fs.open(...) 两步
//   + 对 KMat/D/RMat 做 cv::FileNode::empty() 过激检查 + 内层 try/catch 包裹. 在板端
//   OpenCV 4.5.4 上, 这个组合在 binary 里 6 路 cam 全部返回 false, 而 standalone C++ 测试
//   程序读同一个 yaml 文件 OK. 怀疑 empty() 检查在 !!opencv-matrix tag 后对 sequence 类型
//   的 FileNode 行为不一致 (KMat 后 parser 进入 matrix-tag state, D 是 sequence 落回 map 时
//   empty() 可能误报). 改成和原版 InitUndistortMap 一致的写法: cv::FileStorage 构造 +
//   直接 operator[] 读, 不加 empty() check. 只在最后做 K 形状 sanity check.

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
    } catch (const cv::Exception& e) {
        // 文件不存在 / 不可读 / 解析失败 — 当缺 yaml 处理, 不让 boot 阶段 terminate.
        (void)e;
        return false;
    } catch (const std::exception& e) {
        (void)e;
        return false;
    }
    if (!fs.isOpened()) return false;

    // v3.x.5 (2026-07-09): 板端 OpenCV 4.5.4 binary 里 `cv::FileStorage fs(path, READ)` +
    //   `fs["KMat"] >> K` 在某些 yaml 上会抛 'isMap() in operator[]' (fs 处于非 map state
    //   时 operator[] 触发 assertion). 单独的小测试程序读同一份 yaml 不报, 但 image-stitching
    //   binary 一启动就崩. 直接在 operator[] 周围包 try/catch, 跟空文件同等待遇 (当 missing
    //   处理) — 不再 align 原版 InitUndistortMap 的"裸 open"写法.
    cv::Mat K, D, R;
    try {
        fs["KMat"] >> K;
        fs["D"] >> D;
        fs["RMat"] >> R;
    } catch (const cv::Exception& e) {
        // 板端常踩: 'isMap() in operator[]' (fs 解析器进入 matrix-tag state 后没回到 map).
        // 让上层当 missing/invalid 处理, 不掩盖问题 (log 在调用方打).
        (void)e;
        return false;
    } catch (const std::exception& e) {
        (void)e;
        return false;
    }

    // Sanity check: K 必须是 3x3, D 必须非空. R 缺则用单位阵 (cam0 即此情况).
    if (K.empty() || K.rows != 3 || K.cols != 3) return false;
    if (D.empty()) return false;

    out->K = K.clone();
    out->D = D.clone();
    if (R.empty()) {
        out->R = cv::Mat::eye(3, 3, CV_64F);
    } else {
        out->R = R.clone();
    }

    // 标定分辨率 (calib): 优先 width/height 单独字段, 缺则从 resolution 数组取.
    int w = 0, h = 0;
    try {
        fs["width"]  >> w;
        fs["height"] >> h;
    } catch (const cv::Exception&) {
        return false;
    } catch (const std::exception&) {
        return false;
    }
    if (w <= 0 || h <= 0) {
        cv::FileNode res_node = fs["resolution"];
        if (!res_node.empty() && res_node.isSeq() && res_node.size() >= 2) {
            w = static_cast<int>(res_node[0]);
            h = static_cast<int>(res_node[1]);
        }
    }
    if (w > 0) out->calib_w = w;
    if (h > 0) out->calib_h = h;

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

    // R 一律转 CV_64F — K_live/D 都是 CV_64F, initUndistortRectifyMap 要求类型一致
    cv::Mat R_rect;
    if (ci.R.empty()) {
        R_rect = cv::Mat::eye(3, 3, CV_64F);
    } else {
        ci.R.convertTo(R_rect, CV_64F);
    }
    cv::Mat new_K = K_live;  // 不再 getOptimalNewCameraMatrix (全画幅保留, ROI 留给上层)
    try {
        cv::initUndistortRectifyMap(K_live, ci.D, R_rect, new_K,
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