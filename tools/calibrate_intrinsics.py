#!/usr/bin/env python3
"""Step A 单目内参标定 — RK3588 + GC4683 4 路 2K.

标定板: A3 棋盘 11 行 x 14 列, 方格边长 22 mm, 左上角黑格, 哑光激光打印.
采集: 4 路独立录 15-20 个位姿, 涵盖中心/4 角/4 边/倾斜/近远, 距离 20-40 cm.
环境: 室内漫射光, 关闭 DOL HDR 跑标定, 等 ISP 3A 收敛 3 秒再开始.

用法:
    python3 calibrate_intrinsics.py --cam 0 --images datasets/calib_cam0/
    python3 calibrate_intrinsics.py --all  # 4 路全跑, 读 params/camera_sources.yaml

输出:
    params/intrinsics/cam{N}.npz   K, D, rms 单路原始
    params/intrinsics/summary.txt  4 路一致性报告
    params/camchain_{N}.yaml       工程用 camchain (K, D, R=I, T=0)
"""

import argparse
import glob
import os
import sys
from pathlib import Path

import cv2
import numpy as np

# ===== 必须与打印的标定板一致 =====
ROWS = 10  # 内部角点行数 (11 行方格 - 1)
COLS = 13  # 内部角点列数 (14 列方格 - 1)
SQUARE_M = 0.022  # 22 mm
# ====================================

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


def calibrate_one_cam(cam_id: int, image_paths: list, out_dir: Path) -> dict:
    objp_template = build_object_points()
    objpoints, imgpoints = [], []
    img_size = None
    skipped = 0

    for p in image_paths:
        img = cv2.imread(p)
        if img is None:
            print(f"  skip {p}: imread failed")
            skipped += 1
            continue
        if img_size is None:
            img_size = (img.shape[1], img.shape[0])
        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        ok, corners = detect_corners(gray)
        if not ok:
            print(f"  skip {Path(p).name}: no corners")
            skipped += 1
            continue
        objpoints.append(objp_template)
        imgpoints.append(corners)

    n_valid = len(objpoints)
    n_total = len(image_paths)
    print(f"[cam {cam_id}] valid: {n_valid}/{n_total}  (skipped={skipped})")
    if n_valid < 10:
        sys.exit(f"[cam {cam_id}] only {n_valid} valid frames, need >=10")

    ret, K, D, rvecs, tvecs = cv2.calibrateCamera(
        objpoints,
        imgpoints,
        img_size,
        None,
        None,
    )
    print(f"[cam {cam_id}] RMS = {ret:.4f} px")
    print(
        f"[cam {cam_id}] K = fx={K[0, 0]:.2f} fy={K[1, 1]:.2f} "
        f"cx={K[0, 2]:.2f} cy={K[1, 2]:.2f}"
    )
    print(f"[cam {cam_id}] D = {D.ravel()}")

    out_dir.mkdir(parents=True, exist_ok=True)
    npz_path = out_dir / f"cam{cam_id}.npz"
    np.savez(npz_path, K=K, D=D, rms=ret, img_size=img_size)
    print(f"[cam {cam_id}] saved {npz_path}")

    return {
        "cam_id": cam_id,
        "K": K,
        "D": D,
        "rms": ret,
        "n_valid": n_valid,
        "n_total": n_total,
    }


def write_camchain_yaml(
    cam_id: int, K: np.ndarray, D: np.ndarray, out_path: Path
) -> None:
    fs = cv2.FileStorage(str(out_path), cv2.FILE_STORAGE_WRITE)
    fs.write("image_width", int(K[0, 2] * 2))  # 估算, 实际用 2560
    fs.write("image_height", int(K[1, 2] * 2))  # 估算, 实际用 1440
    fs.write("K", K)
    fs.write("D", D)
    fs.write("R", np.eye(3, dtype=np.float64))
    fs.write("T", np.zeros((3, 1), dtype=np.float64))
    fs.release()
    print(f"[cam {cam_id}] wrote {out_path}")


def main():
    parser = argparse.ArgumentParser(description="GC4683 单目标定")
    parser.add_argument("--cam", type=int, help="单路 cam id (0..3)")
    parser.add_argument("--all", action="store_true", help="4 路全跑")
    parser.add_argument("--images", type=str, help="单路图片目录")
    parser.add_argument(
        "--cams-dir",
        type=str,
        default="datasets/calib",
        help="4 路目录, 命名 calib_cam{N}/",
    )
    parser.add_argument(
        "--out", type=str, default="params/intrinsics", help="npz 输出目录"
    )
    parser.add_argument(
        "--camchain-out", type=str, default="params", help="camchain yaml 输出目录"
    )
    args = parser.parse_args()

    out_dir = Path(args.out)
    camchain_dir = Path(args.camchain_out)

    results = []
    if args.all:
        cams_dir = Path(args.cams_dir)
        for cam_id in [0, 1, 2, 3]:
            cam_dir = cams_dir / f"calib_cam{cam_id}"
            paths = sorted(glob.glob(str(cam_dir / "*.png")))
            if not paths:
                print(f"[cam {cam_id}] no images in {cam_dir}, skip")
                continue
            r = calibrate_one_cam(cam_id, paths, out_dir)
            write_camchain_yaml(
                cam_id, r["K"], r["D"], camchain_dir / f"camchain_{cam_id}.yaml"
            )
            results.append(r)
    elif args.cam is not None and args.images:
        paths = sorted(glob.glob(str(Path(args.images) / "*.png")))
        if not paths:
            sys.exit(f"no images in {args.images}")
        r = calibrate_one_cam(args.cam, paths, out_dir)
        write_camchain_yaml(
            args.cam, r["K"], r["D"], camchain_dir / f"camchain_{args.cam}.yaml"
        )
        results.append(r)
    else:
        parser.print_help()
        sys.exit(1)

    # 一致性报告
    if len(results) >= 2:
        fxs = [r["K"][0, 0] for r in results]
        fys = [r["K"][1, 1] for r in results]
        cxs = [r["K"][0, 2] for r in results]
        cys = [r["K"][1, 2] for r in results]
        rms = [r["rms"] for r in results]

        out_dir.mkdir(parents=True, exist_ok=True)
        report = out_dir / "summary.txt"
        with open(report, "w") as f:
            f.write("=== 4-cam K consistency (Step A) ===\n")
            f.write(
                f"fx: mean={np.mean(fxs):.2f} range={max(fxs) - min(fxs):.2f} "
                f"({(max(fxs) - min(fxs)) / np.mean(fxs) * 100:.2f}%)\n"
            )
            f.write(
                f"fy: mean={np.mean(fys):.2f} range={max(fys) - min(fys):.2f} "
                f"({(max(fys) - min(fys)) / np.mean(fys) * 100:.2f}%)\n"
            )
            f.write(f"cx: mean={np.mean(cxs):.2f} range={max(cxs) - min(cxs):.2f}\n")
            f.write(f"cy: mean={np.mean(cys):.2f} range={max(cys) - min(cys):.2f}\n")
            f.write(f"RMS: per-cam={rms}\n")
            f.write(f"RMS: max={max(rms):.4f} (target < 0.5 px)\n")
            f.write(f"\nExpected from 101 deg FOV / 2560 px:\n")
            f.write(f"  fx ~ 1280 / tan(50.5 deg) = 1055 px\n")
            f.write(f"  fy ~ 720  / tan(34 deg)   = 1067 px\n")
        print(f"\n=== 4-cam K consistency ===")
        print(
            f"fx: mean={np.mean(fxs):.2f} range={max(fxs) - min(fxs):.2f} "
            f"({(max(fxs) - min(fxs)) / np.mean(fxs) * 100:.2f}%)"
        )
        print(
            f"fy: mean={np.mean(fys):.2f} range={max(fys) - min(fys):.2f} "
            f"({(max(fys) - min(fys)) / np.mean(fys) * 100:.2f}%)"
        )
        print(
            f"fx range / mean = "
            f"{(max(fxs) - min(fxs)) / np.mean(fxs) * 100:.2f}% (target < 5%)"
        )
        print(f"max RMS = {max(rms):.4f} px (target < 0.5 px)")
        print(f"summary saved to {report}")

        if max(rms) > 0.5:
            print("WARN: RMS > 0.5 px, re-shoot more poses or check print")
        if (max(fxs) - min(fxs)) / np.mean(fxs) > 0.05:
            print("WARN: fx spread > 5%, sensors differ or calibration issue")


if __name__ == "__main__":
    main()
