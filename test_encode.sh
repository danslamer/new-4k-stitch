#!/bin/bash
cd /home/rocktech/Projects/new-4k-stitch/datasets/2k-test-h264
rm -f t50_test*.mp4

echo "=== attempt 1: no faststart ==="
ffmpeg -i t50.mp4 -an -c:v h264_rkmpp -b:v 8M t50_test1.mp4 -y 2>&1 | tail -3
ls -la t50_test1.mp4 2>/dev/null

echo
echo "=== attempt 2: copy + faststart no re-encode ==="
ffmpeg -i t50.mp4 -c copy -movflags +faststart t50_test2.mp4 -y 2>&1 | tail -3
ls -la t50_test2.mp4 2>/dev/null

echo
echo "=== attempt 3: re-encode + faststart ==="
rm -f t50_test3.mp4
ffmpeg -i t50.mp4 -an -c:v h264_rkmpp -b:v 8M -movflags +faststart t50_test3.mp4 -y 2>&1 | tail -5
ls -la t50_test3.mp4 2>/dev/null

echo
echo "=== check 1 faststart flag if size > 0 ==="
ffprobe -v error -show_streams t50_test2.mp4 2>&1 | head -8