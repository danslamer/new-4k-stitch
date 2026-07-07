#!/bin/bash
echo "=== kill leaked image-stitching (PID 3170) ==="
kill -9 3170 2>&1
sleep 2

echo
echo "=== verify killed ==="
ps aux | grep -iE 'image-stitch|gst-launch' | grep -v grep | head

echo
echo "=== meminfo head ==="
head -3 /proc/meminfo

echo
echo "=== drop_caches ==="
sudo -n sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>&1
head -3 /proc/meminfo