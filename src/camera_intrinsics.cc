// camera_intrinsics.cc - 实现 camchain yaml 加载 + 畸变 map 生成 (v3.x.6, 2026-07-09)
//
// 路径:
//   LoadCamchain(path) → 纯文本 regex 提取 KMat/D/RMat/width/height (不再用 cv::FileStorage)
//   BuildUndistortMap(ci, live_w, live_h) → initUndistortRectifyMap 输出 CV_32FC1
//
// v3.x.6 (2026-07-09): 板端 OpenCV 4.5.4 binary 上 cv::FileStorage 解析 camchain_*.yaml
//   抛 'isMap() in operator[]' cv::Exception (v3.x.4/v3.x.5 已经包 try/catch, 但 6 路
//   camchain 全部 loaded 0/6). Standalone C++ 测试用同一份 yaml + 同一份 OpenCV 链接能读
//   成功, 怀疑是 binary 启动早期 RTSP/GLES/some lib 干扰 OpenCV 内部 parser state.
//
//   修法: 完全绕开 cv::FileStorage. camchain yaml 格式非常固定 (OpenCV 标准
//   `!!opencv-matrix` 风格), 用 std::ifstream + std::regex 提取 K.data (9 个 double),
//   D (任意长度 double seq), RMat.data (9 个 double), width/height (int). 这样:
//     * 不依赖 OpenCV 的 yaml 解析器, 任何平台/任何 OpenCV 版本一致
//     * 错误直接走 std::regex_search 失败返回, 无 try/catch 噪音
//   行格式: KMat 块 = "data: [a, b, c, ...]" 单行, regex 抓数字.
#include "camera_intrinsics.h"

#include <opencv2/opencv.hpp>
#include <opencv2/calib3d.hpp>

#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace camera_intrinsics {

namespace {

// 找第一个匹配 key "K" / "D" / "R" / "width" / "height" 后面的 [..] 或 : <num>,
// 把里面的所有浮点数 / 整数返回. 如果 key 后面不是 seq, 返回空.
// pattern 例子:
//   "D: [-0.35, 0.22, ...]"     -> [-0.35, 0.22, ...]
//   "KMat: ... data: [a, b, ...]" -> 在 KMat 块内找 data: [..]
//   "width: 1920"               -> [1920]
// 简化: 用 std::regex 在全文搜 "key\\s*:\\s*\\[[^\\]]*\\]" 或 "key\\s*:\\s*-?[0-9.]+",
// 取出数字 token.

bool ExtractNumberList(const std::string& text, const std::string& key,
                       std::vector<double>* out) {
    // 顺序匹配:
    //   1) "key: [a, b, c, ...]"
    //   2) "key: <scalar>"        (单值)
    // 走两种 regex, 第一种命中就用, 否则第二种.
    static const std::regex seq_re(
        "(?:" + key + ")\\s*:\\s*\\[([^\\]]*)\\]",
        std::regex::ECMAScript);
    static const std::regex scalar_re(
        "(?:" + key + ")\\s*:\\s*(-?[0-9]+\\.?[0-9]*(?:[eE][-+]?[0-9]+)?)",
        std::regex::ECMAScript);

    std::smatch m;
    if (std::regex_search(text, m, seq_re)) {
        const std::string& body = m[1].str();
        std::regex num_re("-?[0-9]+\\.?[0-9]*(?:[eE][-+]?[0-9]+)?");
        for (auto it = std::sregex_iterator(body.begin(), body.end(), num_re);
             it != std::sregex_iterator(); ++it) {
            try {
                out->push_back(std::stod((*it).str()));
            } catch (...) {
                return false;
            }
        }
        return !out->empty();
    }
    if (std::regex_search(text, m, scalar_re)) {
        try {
            out->push_back(std::stod(m[1].str()));
        } catch (...) {
            return false;
        }
        return true;
    }
    return false;
}

// KMat / RMat 是 !!opencv-matrix 块, 格式:
//   KMat: !!opencv-matrix
//       rows: 3
//       cols: 3
//       dt: d
//       data: [a, b, c, ...]
// 找 "key\n... data: [...]" 的 data: [...] 子串.
bool ExtractMatrixData(const std::string& text, const std::string& key,
                       std::vector<double>* out) {
    // 找 "<key>" 后面紧跟 "data: [...]"
    std::regex re(
        "(?:" + key + ")[\\s\\S]*?data\\s*:\\s*\\[([^\\]]*)\\]",
        std::regex::ECMAScript);
    std::smatch m;
    if (!std::regex_search(text, m, re)) return false;
    const std::string& body = m[1].str();
    std::regex num_re("-?[0-9]+\\.?[0-9]*(?:[eE][-+]?[0-9]+)?");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), num_re);
         it != std::sregex_iterator(); ++it) {
        try {
            out->push_back(std::stod((*it).str()));
        } catch (...) {
            return false;
        }
    }
    return !out->empty();
}

}  // namespace

bool LoadCamchain(const std::string& yaml_path, CamchainIntrinsics* out) {
    if (out == nullptr) return false;
    out->valid = false;

    std::ifstream ifs(yaml_path);
    if (!ifs.is_open()) return false;
    std::stringstream ss;
    ss << ifs.rdbuf();
    const std::string text = ss.str();
    if (text.empty()) return false;

    // 1. KMat.data (9 double) → 3x3 CV_64F
    std::vector<double> kvec;
    if (!ExtractMatrixData(text, "KMat", &kvec) || kvec.size() != 9) return false;
    cv::Mat K(3, 3, CV_64F);
    for (int i = 0; i < 9; ++i) K.at<double>(i / 3, i % 3) = kvec[i];

    // 2. D (任意长度 double seq, 通常 4 或 5) → Nx1 CV_64F
    std::vector<double> dvec;
    if (!ExtractNumberList(text, "D", &dvec) || dvec.empty()) return false;
    cv::Mat D(static_cast<int>(dvec.size()), 1, CV_64F);
    for (size_t i = 0; i < dvec.size(); ++i) D.at<double>(static_cast<int>(i), 0) = dvec[i];

    // 3. RMat.data (9 double, dt: f 常见) → 3x3 CV_64F. 缺则单位阵.
    std::vector<double> rvec;
    cv::Mat R;
    if (ExtractMatrixData(text, "RMat", &rvec) && rvec.size() == 9) {
        R = cv::Mat(3, 3, CV_64F);
        for (int i = 0; i < 9; ++i) R.at<double>(i / 3, i % 3) = rvec[i];
    } else {
        R = cv::Mat::eye(3, 3, CV_64F);
    }

    // 4. width / height (int). 缺则从 resolution: [w, h] 兜底.
    std::vector<double> wvec, hvec, resvec;
    int w = 0, h = 0;
    if (ExtractNumberList(text, "width", &wvec) && !wvec.empty()) {
        w = static_cast<int>(wvec[0]);
    }
    if (ExtractNumberList(text, "height", &hvec) && !hvec.empty()) {
        h = static_cast<int>(hvec[0]);
    }
    if (w <= 0 || h <= 0) {
        if (ExtractNumberList(text, "resolution", &resvec) && resvec.size() >= 2) {
            w = static_cast<int>(resvec[0]);
            h = static_cast<int>(resvec[1]);
        }
    }

    out->K = K;
    out->D = D;
    out->R = R;
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