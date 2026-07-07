#!/bin/bash
echo "=== leaked gst/image-stitch processes ==="
ps aux | grep -iE 'gst-launch|image-stitch|ffmpeg|mpi_dec' | grep -v grep | head

echo
echo "=== sudo -n uptime ==="
sudo -n uptime 2>&1 | head

echo
echo "=== try drop_caches ==="
sudo -n sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>&1 | head

echo
echo "=== /proc/meminfo head ==="
head -5 /proc/meminfo