#!/bin/bash
# 用 -c copy + -movflags +faststart 给所有 6 个 mp4 加 faststart
# (re-encode via h264_rkmpp 会 segfault, 但 mp4 本来就是 H.264, 不需要重编码)
cd /home/rocktech/Projects/new-4k-stitch/datasets/2k-test-h264

for f in t50 t51 t52 t53 t40 t41; do
  echo "=== faststart: $f.mp4 ==="
  ffmpeg -i $f.mp4 -c copy -movflags +faststart ${f}_faststart.mp4 -y 2>&1 | tail -2
  sz=$(stat -c%s ${f}_faststart.mp4 2>/dev/null || echo 0)
  echo "  -> ${f}_faststart.mp4: $sz bytes"
done

echo
echo "=== final ==="
ls -la *_faststart.mp4
echo
echo "=== verify faststart: probe each ==="
for f in t50 t51 t52 t53 t40 t41; do
  moov_pos=$(ffprobe -v error -show_entries format=size -of default=nw=1 ${f}_faststart.mp4 2>&1 | head -1)
  echo "$f: $moov_pos"
done