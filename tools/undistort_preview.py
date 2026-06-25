#!/usr/bin/env python3
"""畸变校正验证 — 用 params/camchain_N.yaml 的 K, D 对单路图片做 undistort.

目的: 跑完标定后, 肉眼验证 D 是否正确, 边缘直线是否变直.
对比 undistort 前后的原图, 存到 params/intrinsics/preview_cam{N}.png.

用法:
    python3 undistort_preview.py --cam 0 --image datasets/calib_cam0/01.png
    python3 undistort_preview.py --all --image-dir datasets/calib_cam0/
"""

import argparse
import glob
from pathlib import Path

import cv2
import numpy as np


def load_camchain(path: Path) -> tuple:
    fs = cv2.FileStorage(str(path), cv2.FILE_STORAGE_READ)
    K = fs.getNode("K").mat()
    D = fs.getNode("D").mat()
    fs.release()
    return K, D


def undistort_one(cam_id: int, image_path: Path, out_dir: Path) -> None:
    chain_path = Path("params") / f"camchain_{cam_id}.yaml"
    if not chain_path.exists():
        print(f"  cam {cam_id}: missing {chain_path}, skip")
        return
    K, D = load_camchain(chain_path)
    print(f"  cam {cam_id}: K fx={K[0, 0]:.2f}, D = {D.ravel()}")

    img = cv2.imread(str(image_path))
    if img is None:
        print(f"  cam {cam_id}: failed to load {image_path}")
        return
    h, w = img.shape[:2]

    new_K, roi = cv2.getOptimalNewCameraMatrix(K, D, (w, h), 1, (w, h))
    undist = cv2.undistort(img, K, D, None, new_K)

    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / f"preview_cam{cam_id}.png"
    # 拼接原图 / 校正后 左右对比
    side = np.hstack([img, undist])
    cv2.putText(
        side, "original", (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 255, 0), 2
    )
    cv2.putText(
        side, "undistorted", (w + 10, 30), cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 255, 0), 2
    )
    cv2.imwrite(str(out_path), side)
    print(f"  cam {cam_id}: wrote {out_path}")


def main():
    parser = argparse.ArgumentParser(description="畸变校正预览")
    parser.add_argument("--cam", type=int, help="单路 cam id")
    parser.add_argument("--all", action="store_true", help="4 路全跑")
    parser.add_argument("--image", type=str, help="单路图片")
    parser.add_argument("--image-dir", type=str, help="4 路目录")
    parser.add_argument("--out", type=str, default="params/intrinsics", help="输出目录")
    args = parser.parse_args()

    out_dir = Path(args.out)

    if args.all:
        for cam_id in [0, 1, 2, 3]:
            cam_dir = Path(args.image_dir) / f"calib_cam{cam_id}"
            pngs = sorted(glob.glob(str(cam_dir / "*.png")))
            if not pngs:
                print(f"  cam {cam_id}: no images, skip")
                continue
            undistort_one(cam_id, Path(pngs[0]), out_dir)
    elif args.cam is not None and args.image:
        undistort_one(args.cam, Path(args.image), out_dir)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
