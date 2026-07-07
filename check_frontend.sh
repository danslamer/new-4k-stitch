#!/bin/bash
echo "=== process check ==="
ps aux | grep image-stitching | grep -v grep | head
echo
echo "=== /api/health ==="
curl -s http://localhost:8080/api/health
echo
echo
echo "=== /api/status ==="
curl -s http://localhost:8080/api/status 2>&1 | head -20
echo
echo "=== check missing stream endpoints ==="
for ep in /api/stream /api/video /api/snapshot /mjpeg /api/preview /api/feed /stream.mjpg; do
  code=$(curl -s -o /dev/null -w '%{http_code}' http://localhost:8080$ep)
  echo "  $ep -> HTTP $code"
done