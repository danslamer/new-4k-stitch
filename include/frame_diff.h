// frame_diff.h
//
// 帧间差值 (Frame-Difference) 模块: 维护前一帧, 每帧计算 |cur - prev| 的可视化掩码.
//
// 设计 (v3.2 2026-07-09):
//   - 输入: NV12 拼接后帧 (来自 output_drm_buf_)
//   - 输出: BGR 掩码图, 标注帧间变动
//     背景 = 当前帧 * dim_factor (灰暗)
//     前景 = 阈值化 |Δ| > threshold 的区域, 用红色 + 矩形框标出
//   - 应用: 作为独立 RTSP 流 (/stitch_diff) 输出, 让操作员/算法看到动态区域
//   - 性能: NV12→BGR 转换 ~5-15ms (aarch64, 2K), absdiff+threshold+findContours ~3-8ms,
//     在 30 FPS 预算 (33ms) 内可行; 用户可在 yaml 调低 fps 节流

#ifndef FRAME_DIFF_H
#define FRAME_DIFF_H

#include <opencv2/core.hpp>

#include "nv12_frame.h"

namespace frame_diff {

struct DiffConfig {
    int threshold = 30;            // per-channel absdiff 阈值 (0-255)
    int bbox_min_area = 100;        // 过滤掉比这小的连通块 (减少噪声)
    float dim_factor = 0.4f;        // 背景 = 当前帧 × dim_factor (越低越暗)
    bool draw_bboxes = true;        // 画红色矩形框
    bool fill_mask = true;          // 红色填充变化区域
};

// 单实例. 持有前一帧的 BGR 缓存. ComputeMask 是线程不安全的 (假定 stitch 线程单线程调用).
class FrameDiff {
 public:
    explicit FrameDiff(DiffConfig cfg = DiffConfig{});
    ~FrameDiff();

    FrameDiff(const FrameDiff&) = delete;
    FrameDiff& operator=(const FrameDiff&) = delete;

    // 喂入当前 NV12 帧, 返回 BGR 掩码图.
    //   - 第一次调用 (没有 prev): 输出 dimmed 当前帧, 同时缓存 prev.
    //   - 后续调用: 输出带红色变动高亮 + 矩形框的 BGR 图.
    //   - 输入为空 (empty()) 时返回空 cv::Mat.
    cv::Mat ComputeMask(const NV12Frame& current);

    void Reset();
    bool HasPrevious() const { return has_prev_; }
    const DiffConfig& config() const { return cfg_; }

 private:
    DiffConfig cfg_;
    cv::Mat prev_bgr_;         // 缓存的前一帧 BGR
    bool has_prev_ = false;
};

}  // namespace frame_diff

#endif  // FRAME_DIFF_H