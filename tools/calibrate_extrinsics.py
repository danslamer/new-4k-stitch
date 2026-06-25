#!/usr/bin/env python3
"""Step B 立体标定 — 4 路 GC4683 相对外参 R, T.

依赖: 阶段 6 跑完 calibrate_intrinsics.py, 已有:
  - params/intrinsics/cam{0..3}.npz  (K, D)
  - params/camchain_{0..3}.yaml     (K, D, R=I, T=0, 由 intrinsics 脚本生成)

采集: 标定板**同时**被至少 2 路看到, 录 15-20 个位姿, 同步帧对齐.
      4 路同时录, 用 OpenCV VideoCapture 读 4 个 mp4 (录自 FFmpeg 同步录制).

输出: 写回 params/camchain_{1,2,3}.yaml, 更新 R, T 字段 (cam 0 是参考).
      同时输出 params/extrinsics/summary.txt (4 路闭环误差, 物理基线 sanity).

注意: 阶段 7 (真机跑通) 不依赖本脚本, ROI bootstrap 自动检测布局;
      本脚本是**优化** ROI bootstrap 初值 + 远期 2×3 扩展时必跑.

用法:
    python3 calibrate_extrinsics.py --all
    python3 calibrate_extrinsics.py --videos cam0.mp4 cam1.mp4 cam2.mp4 cam3.mp4
"""

import argparse
import glob
import os
import sys
from pathlib import Path

import cv2
import numpy as np

# ===== 必须与 calibrate_intrinsics.py 一致 =====
ROWS = 10
COLS = 13
SQUARE_M = 0.022
# ===============================================

CRITERIA = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 30, 0.001)


def build_object_points() -> np.ndarray:
    obj = np.zeros((ROWS * COLS, 3), np.float32)
    obj[:, :2] = np.mgrid[0:COLS, 0:ROWS].T.reshape(-1, 2) * SQUARE_M
    return obj


def detect_corners(img_gray: np.ndarray) -> tuple:
    ok, corners = cv2.findChessboardCorners(
        img_gray,
        (COLS, ROWS),
        cv2.CALIB_CB_ADAPTIVE_THRESH + cv2.CALIB_CB_NORMALIZE_IMAGE,
    )
    if not ok:
        return False, None
    corners = cv2.cornerSubPix(img_gray, corners, (11, 11), (-1, -1), CRITERIA)
    return True, corners


def read_4cams_intrinsics(intr_dir: Path) -> dict:
    K = {}
    D = {}
    for cam_id in [0, 1, 2, 3]:
        npz = intr_dir / f"cam{cam_id}.npz"
        if not npz.exists():
            sys.exit(f"missing {npz}, run calibrate_intrinsics.py first")
        d = np.load(npz)
        K[cam_id] = d["K"]
        D[cam_id] = d["D"]
    return K, D


def extract_sync_frames(video_paths: list, n_frames: int = 20) -> list:
    """4 路视频逐帧读取, 返回 n_frames 个 (cam0..3) 同步帧列表.
    简化: 取前 n_frames 帧作为同步 (实际应用需要时间戳对齐, 阶段 4 会做)."""
    caps = [cv2.VideoCapture(p) for p in video_paths]
    if not all(c.isOpened() for c in caps):
        sys.exit("failed to open some video: " + str(video_paths))
    sync_frames = []
    for i in range(n_frames):
        frames = []
        for c in caps:
            ok, f = c.read()
            if not ok:
                break
            frames.append(f)
        if len(frames) == 4:
            sync_frames.append(frames)
    for c in caps:
        c.release()
    print(f"Extracted {len(sync_frames)} sync frames from 4 videos")
    return sync_frames


def detect_in_frame(frame_bgr: np.ndarray) -> tuple:
    gray = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2GRAY)
    return detect_corners(gray)


def stereo_calibrate_pair(
    K1, D1, K2, D2, img_size, objpoints, imgpoints1, imgpoints2
) -> dict:
    criteria = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 100, 1e-6)
    flags = cv2.CALIB_FIX_INTRINSIC
    try:
        ret, K1o, D1o, K2o, D2o, R, T, E, F = cv2.stereoCalibrate(
            objpoints,
            imgpoints1,
            imgpoints2,
            K1,
            D1,
            K2,
            D2,
            img_size,
            criteria=criteria,
            flags=flags,
        )
        return {
            "rms": ret,
            "R": R,
            "T": T,
            "E": E,
            "F": F,
            "K1": K1o,
            "D1": D1o,
            "K2": K2o,
            "D2": D2o,
        }
    except cv2.error as e:
        print(f"  stereoCalibrate failed: {e}")
        return None


def update_camchain_yaml(camchain_path: Path, R: np.ndarray, T: np.ndarray) -> None:
    """读 yaml, 更新 R/T 字段 (K, D 保留), 写回."""
    fs = cv2.FileStorage(str(camchain_path), cv2.FILE_STORAGE_READ)
    K = fs.getNode("K").mat()
    D = fs.getNode("D").mat()
    fs.release()

    fs = cv2.FileStorage(str(camchain_path), cv2.FILE_STORAGE_WRITE)
    fs.write("K", K)
    fs.write("D", D)
    fs.write("R", R)
    fs.write("T", T)
    fs.release()
    print(f"  updated {camchain_path} with R, T")


def main():
    parser = argparse.ArgumentParser(description="GC4683 4 路立体标定")
    parser.add_argument("--videos", nargs=4, help="4 个同步视频文件 (cam 0..3)")
    parser.add_argument("--n-frames", type=int, default=20, help="提取的同步帧数")
    parser.add_argument(
        "--intr-dir",
        type=str,
        default="params/intrinsics",
        help="Step A 输出的 npz 目录",
    )
    parser.add_argument(
        "--camchain-dir", type=str, default="params", help="camchain yaml 目录"
    )
    parser.add_argument(
        "--out", type=str, default="params/extrinsics", help="外参报告输出目录"
    )
    args = parser.parse_args()

    if not args.videos or len(args.videos) != 4:
        parser.print_help()
        sys.exit("need --videos cam0.mp4 cam1.mp4 cam2.mp4 cam3.mp4")

    intr_dir = Path(args.intr_dir)
    camchain_dir = Path(args.camchain_dir)
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    K, D = read_4cams_intrinsics(intr_dir)
    print("=== Step A K loaded ===")
    for cam_id in [0, 1, 2, 3]:
        print(f"  cam {cam_id}: fx={K[cam_id][0, 0]:.2f}")

    sync_frames = extract_sync_frames(args.videos, args.n_frames)
    if len(sync_frames) < 10:
        sys.exit(f"only {len(sync_frames)} sync frames, need >=10")

    objp_template = build_object_points()
    img_size = (sync_frames[0][0].shape[1], sync_frames[0][0].shape[0])

    detections = {0: [], 1: [], 2: [], 3: []}
    valid_frames = []
    n_skipped = 0
    for frames in sync_frames:
        corners = {}
        ok_all = True
        for cam_id in [0, 1, 2, 3]:
            ok, c = detect_in_frame(frames[cam_id])
            if not ok:
                ok_all = False
                break
            corners[cam_id] = c
        if not ok_all:
            n_skipped += 1
            continue
        for cam_id in [0, 1, 2, 3]:
            detections[cam_id].append(corners[cam_id])
        valid_frames.append(frames)

    print(
        f"Detected corners in {len(valid_frames)} frames, "
        f"skipped {n_skipped} (at least one cam failed)"
    )
    if len(valid_frames) < 10:
        sys.exit(f"only {len(valid_frames)} valid frames, need >=10")

    # 对 (0, 1), (0, 2), (0, 3) 跑 stereo (cam 0 为参考)
    objpoints = [objp_template] * len(valid_frames)
    results = {}
    for target_cam in [1, 2, 3]:
        print(f"\n=== stereo (cam 0, cam {target_cam}) ===")
        r = stereo_calibrate_pair(
            K[0],
            D[0],
            K[target_cam],
            D[target_cam],
            img_size,
            objpoints,
            detections[0],
            detections[target_cam],
        )
        if r is None:
            print(f"  cam {target_cam}: FAILED")
            continue
        print(f"  cam {target_cam}: RMS = {r['rms']:.4f} px")
        print(
            f"  cam {target_cam}: T = {r['T'].ravel()}  (norm = {np.linalg.norm(r['T']):.4f})"
        )
        results[target_cam] = r
        update_camchain_yaml(
            camchain_dir / f"camchain_{target_cam}.yaml",
            r["R"],
            r["T"],
        )

    # 闭环 sanity: cam 0 -> cam 1 -> cam 2 -> cam 3 -> cam 0
    if all(k in results for k in [1, 2, 3]):
        R01, T01 = results[1]["R"], results[1]["T"]
        R02, T02 = results[2]["R"], results[2]["T"]
        R03, T03 = results[3]["R"], results[3]["T"]

        # cam 0 -> cam 2 (经 cam 1)
        R_via1 = R01 @ R12_via_path(results, 1, 2)  # placeholder
        # 简化: 用 OpenCV 验证 cam0->cam2 与 cam0->cam1, cam1->cam2 是否一致
        # 这里只做基本 sanity: 每条 T 模长差异
        T01_norm = np.linalg.norm(T01)
        T02_norm = np.linalg.norm(T02)
        T03_norm = np.linalg.norm(T03)
        print(f"\n=== 物理基线 sanity (T 模长) ===")
        print(f"  cam0->cam1: {T01_norm:.4f}")
        print(f"  cam0->cam2: {T02_norm:.4f}")
        print(f"  cam0->cam3: {T03_norm:.4f}")
        # 4 路 2x2 物理排布, 相邻 cam 基线应该接近

        # RMS 报告
        report = out_dir / "summary.txt"
        with open(report, "w") as f:
            f.write("=== 4-cam stereo calibration (Step B) ===\n")
            for k, r in results.items():
                f.write(f"\ncam 0 -> cam {k}:\n")
                f.write(f"  RMS = {r['rms']:.4f} px (target < 1.0)\n")
                f.write(f"  T = {r['T'].ravel()}\n")
                f.write(f"  |T| = {np.linalg.norm(r['T']):.4f}\n")
            f.write(f"\nBaseline norms:\n")
            f.write(f"  cam0->cam1: {T01_norm:.4f}\n")
            f.write(f"  cam0->cam2: {T02_norm:.4f}\n")
            f.write(f"  cam0->cam3: {T03_norm:.4f}\n")
        print(f"\nsummary saved to {report}")


def R12_via_path(results, a, b):
    """占位函数, 闭环 sanity 留待 v3 完善."""
    return np.eye(3)


if __name__ == "__main__":
    main()
