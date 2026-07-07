#!/bin/bash
cd /home/rocktech/Projects/new-4k-stitch
echo "--- Test 1: NO single quotes around caps filter ---"
timeout 3 gst-launch-1.0 filesrc location=./datasets/2k-test-h264/t50.mp4 ! qtdemux ! h264parse ! mppvideodec dma-feature=true format=NV12 ! video/x-raw\(memory:DMABuf\),format=NV12,width=2560,height=1440 ! appsink name=sink drop=true max-buffers=2 sync=false 2>&1 | tail -10
echo ""
echo "--- Test 2: WITH single quotes ---"
timeout 3 gst-launch-1.0 filesrc location=./datasets/2k-test-h264/t50.mp4 ! qtdemux ! h264parse ! mppvideodec dma-feature=true format=NV12 ! 'video/x-raw(memory:DMABuf),format=NV12,width=2560,height=1440' ! appsink name=sink drop=true max-buffers=2 sync=false 2>&1 | tail -10