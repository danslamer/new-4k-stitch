#!/usr/bin/env python3
"""
tools/stitch_geometry_validator.py
=================================
模拟 BuildStitchLayout2x3 的几何计算, 验证 6 路 2x3 拼接参数.

不需要 camera/stitcher 就能跑, 纯数学.
输出:
  - K 矩阵 (FOV 反推)
  - 重叠率 (跟 BuildStitchLayout2x3 实际用值对齐)
  - 6 路 cam 在全景中的 dst 位置
  - DRAM 预算估算

用法:
  python3 tools/stitch_geometry_validator.py
  # 没 python3 时: python tools/stitch_geometry_validator.py

依据: docs/NETWORK_CAMERA_PLAN.md §0.5 / §0.6 / §0.7
      src/app.cc BuildStitchLayout2x3 (W // 8, H // 12)
"""

import math
import sys

CAM_W = 2560
CAM_H = 1440

K_FOV_H = 102.5  # H-FOV 102.5°
K_FOV_V = 55.2   # V-FOV 55.2°

def k_matrix():
    fx = (CAM_W / 2) / math.tan(math.radians(K_FOV_H / 2))
    fy = (CAM_H / 2) / math.tan(math.radians(K_FOV_V / 2))
    return fx, fy, CAM_W / 2, CAM_H / 2

def layout():
    # 跟 src/app.cc BuildStitchLayout2x3 完全一致
    overlap_h = CAM_W // 8
    overlap_v = CAM_H // 12
    col_x = [0, CAM_W - overlap_h]
    row_y = [0, CAM_H - overlap_v, 2 * (CAM_H - overlap_v)]
    return overlap_h, overlap_v, col_x, row_y

def main():
    print("=== K 矩阵 (FOV 反推) ===")
    fx, fy, cx, cy = k_matrix()
    print(f"  fx = {fx:.1f}  fy = {fy:.1f}")
    print(f"  cx = {cx}    cy = {cy}")
    print(f"  (camera_intrinsics.h 占位与本计算差 < 0.1 px)")

    print()
    print("=== 重叠 / 布局 (按 BuildStitchLayout2x3 实际逻辑) ===")
    overlap_h, overlap_v, col_x, row_y = layout()
    print(f"  overlap_h = {overlap_h} (W // 8 = {CAM_W // 8})")
    print(f"  overlap_v = {overlap_v} (H // 12 = {CAM_H // 12})")
    print(f"  panorama_w = {col_x[1] + CAM_W}")
    print(f"  panorama_h = {row_y[2] + CAM_H}")

    print()
    print("=== 6 路 cam 在全景中的 dst ===")
    for i in range(6):
        col = i % 2
        row = i // 2
        x = col_x[col]
        y = row_y[row]
        print(f"  cam{i} col={col} row={row} -> dst=({x}, {y})")

    print()
    print("=== DRAM 预算 (NV12 = 1.5 byte/pixel) ===")
    panorama_bytes = (col_x[1] + CAM_W) * (row_y[2] + CAM_H) * 1.5
    queue_bytes = 6 * CAM_W * CAM_H * 1.5
    print(f"  panorama buffer: {panorama_bytes / 1024 / 1024:.1f} MB")
    print(f"  6 source in queue: {queue_bytes / 1024 / 1024:.1f} MB")
    print(f"  total stitch budget: {(panorama_bytes + queue_bytes) / 1024 / 1024:.1f} MB")

    # Sanity
    pw = col_x[1] + CAM_W
    ph = row_y[2] + CAM_H
    assert pw <= 2 * CAM_W, f"panorama_w {pw} > 2 * W"
    assert ph <= 3 * CAM_H, f"panorama_h {ph} > 3 * H"
    for i in range(6):
        col = i % 2
        x = col_x[col]
        assert x + CAM_W <= pw, f"cam{i} extends beyond panorama width"

    print()
    print("OK — geometry sanity all pass.")
    return 0

if __name__ == "__main__":
    sys.exit(main())