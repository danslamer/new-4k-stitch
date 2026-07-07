#!/bin/bash
# 测同样的 pipeline 配 fakesink, 看是否能拿到帧
cd /home/rocktech/Projects/new-4k-stitch
echo "=== Test 1: 同样 pipeline 但用 fakesink ==="
timeout 5 gst-launch-1.0 -v \
  filesrc location=./datasets/2k-test-h264/t50.mp4 \
  ! qtdemux ! h264parse \
  ! mppvideodec dma-feature=true format=NV12 \
  ! 'video/x-raw(memory:DMABuf),format=NV12' \
  ! fakesink 2>&1 | tail -30
echo ""
echo "=== Test 2: 同样 pipeline 配 appsink, 用信号回调确认收到帧 ==="
cat > /tmp/probe.c <<'EOF'
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <stdio.h>
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GError* err = NULL;
  GstElement* p = gst_parse_launch(
    "filesrc location=./datasets/2k-test-h264/t50.mp4 "
    "! qtdemux ! h264parse "
    "! mppvideodec dma-feature=true format=NV12 "
    "! video/x-raw(memory:DMABuf),format=NV12 "
    "! appsink name=sink drop=true max-buffers=2 sync=false", &err);
  if (!p) { fprintf(stderr, "parse failed: %s\n", err->message); return 1; }
  GstElement* appsink = gst_bin_get_by_name(GST_BIN(p), "sink");
  GstStateChangeReturn r = gst_element_set_state(p, GST_STATE_PLAYING);
  fprintf(stderr, "set_state PLAYING returned %d\n", r);
  int count = 0;
  for (int i = 0; i < 100; ++i) {
    GstSample* s = gst_app_sink_pull_sample(GST_APP_SINK(appsink));
    if (s) {
      count++;
      gst_sample_unref(s);
      if (count % 30 == 0) fprintf(stderr, "got %d samples\n", count);
    } else {
      fprintf(stderr, "got null sample (EOS or err) at iter %d\n", i);
      break;
    }
  }
  fprintf(stderr, "TOTAL samples: %d\n", count);
  gst_element_set_state(p, GST_STATE_NULL);
  gst_object_unref(p);
  return 0;
}
EOF
gcc /tmp/probe.c -o /tmp/probe $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0) 2>&1 | head -3
LD_LIBRARY_PATH=/usr/lib/aarch64-linux-gnu timeout 5 /tmp/probe 2>&1 | tail -10