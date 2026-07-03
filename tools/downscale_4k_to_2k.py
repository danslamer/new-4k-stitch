#!/usr/bin/env python3
"""
Downscale 4K (3840x2160) videos in datasets/4k-test to 2K (2560x1440),
saving to datasets/2k-test, preserving filenames.

Per CLAUDE.md v2 stage, the project runs at 2K (2560x1440) @ 30fps.
The source 4K videos are too large for end-to-end rkmpp pipeline tests
at the v2 scale; downscaled copies let us run the same code paths.

Usage (from project root):
    python tools/downscale_4k_to_2k.py
"""

from pathlib import Path
import cv2
import sys

ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = ROOT / "datasets" / "4k-test"
DST_DIR = ROOT / "datasets" / "2k-test"
DST_W, DST_H = 2560, 1440


def downscale_one(src: Path, dst: Path) -> tuple[bool, str]:
    cap = cv2.VideoCapture(str(src))
    if not cap.isOpened():
        return False, "open failed"

    src_w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    src_h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
    frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))

    # Use mp4v (broadly compatible). Yields larger files than h264
    # but no external codec dependency.
    fourcc = cv2.VideoWriter_fourcc(*"mp4v")
    writer = cv2.VideoWriter(str(dst), fourcc, fps, (DST_W, DST_H))
    if not writer.isOpened():
        cap.release()
        return False, "writer open failed"

    written = 0
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        # INTER_AREA is best for downscaling (avoids aliasing)
        resized = cv2.resize(frame, (DST_W, DST_H), interpolation=cv2.INTER_AREA)
        writer.write(resized)
        written += 1

    cap.release()
    writer.release()
    return True, f"{src_w}x{src_h}@{fps:.1f}fps {frames}f -> {DST_W}x{DST_H} {written}f"


def main() -> int:
    if not SRC_DIR.is_dir():
        print(f"[ERR] source dir not found: {SRC_DIR}", file=sys.stderr)
        return 1
    DST_DIR.mkdir(parents=True, exist_ok=True)

    sources = sorted(SRC_DIR.glob("*.mp4"))
    if not sources:
        print(f"[ERR] no .mp4 in {SRC_DIR}", file=sys.stderr)
        return 1

    print(f"[INFO] {len(sources)} source videos -> {DST_DIR}")
    ok_count, fail_count, skipped = 0, 0, 0
    for src in sources:
        dst = DST_DIR / src.name
        # Skip if already exists and matches source mtime (incremental re-runs)
        if dst.exists() and dst.stat().st_mtime >= src.stat().st_mtime:
            print(f"  [skip] {src.name} (already up-to-date)")
            skipped += 1
            continue
        ok, info = downscale_one(src, dst)
        if ok:
            print(f"  [ok]   {src.name}  {info}")
            ok_count += 1
        else:
            print(f"  [FAIL] {src.name}  {info}", file=sys.stderr)
            fail_count += 1

    print(f"[DONE] ok={ok_count} fail={fail_count} skipped={skipped}")
    return 0 if fail_count == 0 else 2


if __name__ == "__main__":
    sys.exit(main())