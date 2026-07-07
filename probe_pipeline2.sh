#!/bin/bash
# 排查 qtdemux streaming stopped -5 错误
cd /home/rocktech/Projects/new-4k-stitch/datasets/2k-test-h264

echo "=== test 1: qtdemux + h264parse + fakesink (no decoder) ==="
timeout 5 gst-launch-1.0 -v filesrc location=./t50.mp4 ! qtdemux ! h264parse ! fakesink 2>&1 | tail -3

echo
echo "=== test 2: qtdemux + appsink only ==="
cat > /tmp/probe2.c <<'EOF'
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <stdio.h>
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GError* err = NULL;
  GstElement* p = gst_parse_launch(
    "filesrc location=./datasets/2k-test-h264/t50.mp4 "
    "! qtdemux ! h264parse "
    "! appsink name=sink drop=true max-buffers=2 sync=false", &err);
  if (!p) { fprintf(stderr, "parse failed: %s\n", err->message); return 1; }
  GstElement* appsink = gst_bin_get_by_name(GST_BIN(p), "sink");
  GstStateChangeReturn r = gst_element_set_state(p, GST_STATE_PLAYING);
  fprintf(stderr, "set_state PLAYING returned %d\n", r);
  int count = 0;
  for (int i = 0; i < 300; ++i) {
    GstSample* s = gst_app_sink_pull_sample(GST_APP_SINK(appsink));
    if (s) {
      count++;
      gst_sample_unref(s);
      if (count % 20 == 0) fprintf(stderr, "got %d samples (iter %d)\n", count, i);
    } else {
      fprintf(stderr, "got null at iter %d (count=%d)\n", i, count);
      break;
    }
  }
  fprintf(stderr, "TOTAL: %d samples\n", count);
  gst_element_set_state(p, GST_STATE_NULL);
  gst_object_unref(p);
  return 0;
}
EOF
gcc /tmp/probe2.c -o /tmp/probe2 $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0)
LD_LIBRARY_PATH=/usr/lib/aarch64-linux-gnu timeout 8 /tmp/probe2 2>&1 | tail -15

echo
echo "=== test 3: decodebin (auto demux+parse) + appsink ==="
cat > /tmp/probe3.c <<'EOF'
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <stdio.h>
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GError* err = NULL;
  GstElement* p = gst_parse_launch(
    "filesrc location=./datasets/2k-test-h264/t50.mp4 "
    "! decodebin "
    "! appsink name=sink drop=true max-buffers=2 sync=false", &err);
  if (!p) { fprintf(stderr, "parse failed: %s\n", err->message); return 1; }
  GstElement* appsink = gst_bin_get_by_name(GST_BIN(p), "sink");
  GstStateChangeReturn r = gst_element_set_state(p, GST_STATE_PLAYING);
  fprintf(stderr, "decodebin: set_state PLAYING returned %d\n", r);
  int count = 0;
  for (int i = 0; i < 300; ++i) {
    GstSample* s = gst_app_sink_pull_sample(GST_APP_SINK(appsink));
    if (s) {
      count++;
      gst_sample_unref(s);
      if (count % 20 == 0) fprintf(stderr, "got %d samples (iter %d)\n", count, i);
    } else {
      fprintf(stderr, "got null at iter %d (count=%d)\n", i, count);
      break;
    }
  }
  fprintf(stderr, "decodebin TOTAL: %d samples\n", count);
  gst_element_set_state(p, GST_STATE_NULL);
  gst_object_unref(p);
  return 0;
}
EOF
gcc /tmp/probe3.c -o /tmp/probe3 $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0)
LD_LIBRARY_PATH=/usr/lib/aarch64-linux-gnu timeout 8 /tmp/probe3 2>&1 | tail -15