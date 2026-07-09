// frame_diff.cc
#include "frame_diff.h"

#include "logger.h"

#include <opencv2/imgproc.hpp>

#include <cstring>

extern "C" {
#include <sys/mman.h>
#include <unistd.h>
}

namespace frame_diff {

namespace {

// NV12 DMA-BUF → BGR cv::Mat (CPU mmap + cv::cvtColor).
// 与 app.cc::ExportNv12DrmBufferToBgr 同样的路径, 这里 inline 一份, 避免新增
// 头文件依赖; 后续如需复用可抽出来.
cv::Mat Nv12DmaBufToBgr(const NV12Frame& nv12) {
    if (nv12.empty() || nv12.fd < 0) return cv::Mat();

    void* mapped = ::mmap(nullptr,
                          static_cast<size_t>(nv12.stride_w) * nv12.stride_h * 3 / 2,
                          PROT_READ, MAP_SHARED, nv12.fd, 0);
    if (mapped == MAP_FAILED) {
        Logger::GetInstance().LogError(
            "[frame_diff] mmap fd=" + std::to_string(nv12.fd) + " failed");
        return cv::Mat();
    }

    const int w = nv12.width;
    const int h = nv12.height;
    const int sw = nv12.stride_w;
    const int sh = nv12.stride_h;

    // NV12 单平面: Y 平面 + (UV 交织). 总字节 = sw * sh * 3/2.
    cv::Mat nv12_host(sh * 3 / 2, sw, CV_8UC1, mapped);

    cv::Mat bgr;
    try {
        cv::cvtColor(nv12_host(cv::Rect(0, 0, w, h * 3 / 2)), bgr,
                     cv::COLOR_YUV2BGR_NV12);
    } catch (const cv::Exception& e) {
        Logger::GetInstance().LogError(
            std::string("[frame_diff] cv::cvtColor failed: ") + e.what());
        bgr = cv::Mat();
    }

    ::munmap(mapped, static_cast<size_t>(sw) * sh * 3 / 2);
    return bgr;
}

}  // namespace

FrameDiff::FrameDiff(DiffConfig cfg) : cfg_(cfg) {}

FrameDiff::~FrameDiff() = default;

void FrameDiff::Reset() {
    prev_bgr_ = cv::Mat();
    has_prev_ = false;
}

cv::Mat FrameDiff::ComputeMask(const NV12Frame& current,
                              std::vector<cv::Point2f>* centroids_out) {
    if (current.empty()) return cv::Mat();

    // v3.2.1: 每次调用前清空输出, 保证第一次调用 (没 prev_) 时 centroid 列表也为空,
    //   调用方拿到的 centroids 严格对应"本帧检测到的运动块".
    if (centroids_out != nullptr) {
        centroids_out->clear();
    }

    cv::Mat curr_bgr = Nv12DmaBufToBgr(current);
    if (curr_bgr.empty()) return cv::Mat();

    cv::Mat mask_bgr;
    if (!has_prev_) {
        // 第一次: 没有 prev, 输出 dimmed 当前帧 (全黑) + 缓存 prev.
        curr_bgr.convertTo(mask_bgr, -1, cfg_.dim_factor, 0);
        has_prev_ = true;
    } else {
        cv::Mat diff, gray, bin_mask;
        cv::absdiff(curr_bgr, prev_bgr_, diff);
        cv::cvtColor(diff, gray, cv::COLOR_BGR2GRAY);
        cv::threshold(gray, bin_mask, cfg_.threshold, 255, cv::THRESH_BINARY);

        // 背景 = dimmed 当前帧
        curr_bgr.convertTo(mask_bgr, -1, cfg_.dim_factor, 0);

        if (cfg_.fill_mask) {
            cv::Mat red_layer = cv::Mat::zeros(curr_bgr.size(), curr_bgr.type());
            red_layer.setTo(cv::Scalar(0, 0, 255), bin_mask);
            cv::addWeighted(mask_bgr, 1.0, red_layer, 0.7, 0, mask_bgr);
        }
        // v3.2.1 (2026-07-09): 一次 findContours 同时服务 bbox 绘制和 centroid 提取.
        //   之前 bbox 路径独立 findContours 一次, 现在统一一次, 不增加 CPU 开销.
        //   没有 draw_bboxes 也没 centroids_out 时, 不调 findContours, 省 1-3ms.
        const bool need_contours = cfg_.draw_bboxes || (centroids_out != nullptr);
        if (need_contours) {
            std::vector<std::vector<cv::Point>> contours;
            cv::findContours(bin_mask, contours, cv::RETR_EXTERNAL,
                              cv::CHAIN_APPROX_SIMPLE);
            for (const auto& contour : contours) {
                const double area = cv::contourArea(contour);
                if (area < static_cast<double>(cfg_.bbox_min_area)) {
                    continue;
                }
                if (cfg_.draw_bboxes) {
                    cv::Rect bbox = cv::boundingRect(contour);
                    cv::rectangle(mask_bgr, bbox, cv::Scalar(0, 0, 255), 3);
                }
                if (centroids_out != nullptr) {
                    cv::Moments m = cv::moments(contour);
                    if (m.m00 > 0.0) {
                        centroids_out->emplace_back(
                            static_cast<float>(m.m10 / m.m00),
                            static_cast<float>(m.m01 / m.m00));
                    }
                }
            }
        }
    }

    prev_bgr_ = curr_bgr.clone();
    return mask_bgr;
}

}  // namespace frame_diff