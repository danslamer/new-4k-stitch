#!/bin/bash
# 用 h264_v4l2m2m (vendor mpp 硬编) 转码 + faststart
# v4l2m2m 走 mpp_service 老接口 (与板子 SDK mppvideodec 一致),
# 与 h264_rkmpp 走新版 mpp 编码器不同 — 后者 segfault
cd /home/rocktech/Projects/new-4k-stitch/datasets/2k-test-h264

for f in t50 t51 t52 t53 t40 t41; do
  echo "=== re-encoding $f.mp4 ==="
  # 用 v4l2m2m (vendor-supported), 失败回落到 libx264 sw
  if ffmpeg -i $f.mp4 -an -c:v h264_v4l2m2m -b:v 8M -pix_fmt nv12 -movflags +faststart ${f}_faststart.mp4 -y 2>&1 | tail -3; then
    sz=$(stat -c%s ${f}_faststart.mp4 2>/dev/null || echo 0)
    if [ "$sz" -gt 100000 ]; then
      echo "  -> $f via v4l2m2m OK ($sz bytes)"
      continue
    fi
  fi
  # fallback: libx264 (板上应该装了)
  echo "  -> v4l2m2m failed, trying libx264 sw"
  ffmpeg -i $f.mp4 -an -c:v libx264 -preset ultrafast -b:v 8M -pix_fmt yuv420p -movflags +faststart ${f}_faststart.mp4 -y 2>&1 | tail -3
  sz=$(stat -c%s ${f}_faststart.mp4 2>/dev/null || echo 0)
  echo "  -> $f final size: $sz bytes"
done

echo "=== final listing ==="
ls -la *_faststart.mp4