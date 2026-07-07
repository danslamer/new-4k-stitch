#!/bin/bash
# 看 mppvideodec 内部 — 用 GST_DEBUG=mppvideodec:5
cd /home/rocktech/Projects/new-4k-stitch

echo "=== G: mppvideodec 详细日志 ==="
timeout 8 gst-launch-1.0 -v filesrc location=./datasets/2k-test-h264/t50.mp4 \
  ! qtdemux ! h264parse \
  ! mppvideodec format=NV12 \
  ! 'video/x-raw,format=NV12' \
  ! fakesink 2>&1 | grep -iE "mpp|error|warn|fail|flow|frame|push" | tail -40