#!/bin/bash
# run_mjpeg_stage2.sh - on board run + verify
echo "=== kill any leak ==="
pkill -9 -f image-stitching
sleep 1
echo "=== pre-state ==="
ps aux | grep image-stitching | grep -v grep | head
free -m | awk '/^Mem:/{print "memfree:", $7, "MB"}'

echo "=== start image-stitching ==="
cd /home/rocktech/Projects/new-4k-stitch/build
nohup ./image-stitching > /tmp/image-stitching-mjpeg.log 2>&1 &
PID=$!
echo "started_pid=$PID"
sleep 5

echo "=== alive check ==="
ps -p $PID -o pid,etime,vsz,rss,comm 2>&1 | head

echo "=== mjpeg/stream related log ==="
grep -E "MJPEG|mjpeg|stream" /tmp/image-stitching-mjpeg.log 2>&1 | head -20

echo "=== curl /api/health ==="
curl -s --max-time 2 http://localhost:8080/api/health
echo

echo "=== curl /api/snapshot ==="
curl -s -o /tmp/snap.jpg -w "http=%{http_code} ct=%{content_type} size=%{size_download} bytes\n" --max-time 4 http://localhost:8080/api/snapshot
file /tmp/snap.jpg

echo "=== curl /api/stream headers 4s window ==="
curl -v --max-time 4 http://localhost:8080/api/stream -o /tmp/stream.mjpg 2>&1 | grep -iE "HTTP|Content-Type|Content-Length|Transfer-Encoding" | head -10

echo "=== stream bytes captured in 4s ==="
ls -la /tmp/stream.mjpg
file /tmp/stream.mjpg
