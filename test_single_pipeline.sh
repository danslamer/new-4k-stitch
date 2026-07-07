#!/bin/bash
# 测同样的 pipeline + appsink 是否能拿到 sample
cd /home/rocktech/Projects/new-4k-stitch
timeout 8 gst-launch-1.0 -v \
  filesrc location=./datasets/2k-test-h264/t50.mp4 \
  ! qtdemux \
  ! h264parse \
  ! mppvideodec dma-feature=true format=NV12 \
  ! 'video/x-raw(memory:DMABuf),format=NV12,width=2560,height=1440' \
  ! appsink name=sink drop=true max-buffers=2 sync=false 2>&1 | grep -E "ERROR|WARN|state|push|sample|frame|dma|drm" | head -30
echo "---"
# 加 GST_DEBUG=mppvideodec:5 看 mppvideodec 内部
timeout 5 GST_DEBUG="mppvideodec:5,mpp:5" gst-launch-1.0 \
  filesrc location=./datasets/2k-test-h264/t50.mp4 \
  ! qtdemux ! h264parse \
  ! mppvideodec dma-feature=true format=NV12 \
  ! 'video/x-raw(memory:DMABuf),format=NV12,width=2560,height=1440' \
  ! fakesink 2>&1 | tail -30