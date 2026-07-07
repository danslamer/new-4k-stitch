#!/bin/bash
# 进一步诊断: 跳过 mppvideodec, 直接 qtdemux + h264parse, 看流是否对
cd /home/rocktech/Projects/new-4k-stitch

echo "=== A: qtdemux + appsink 直接接 H264 (no decoder) ==="
cat > /tmp/probeA.c <<'EOF'
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <stdio.h>
static GMainLoop *loop = NULL;
static int count = 0;
static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user) {
  count++;
  GstSample *s = gst_app_sink_pull_sample(sink);
  if (s) {
    if (count % 30 == 1) {
      GstBuffer *buf = gst_sample_get_buffer(s);
      GstCaps *caps = gst_sample_get_caps(s);
      fprintf(stderr, "[A] sample #%d size=%lu caps=%s\n", count,
              gst_buffer_get_size(buf), gst_caps_to_string(caps));
    }
    gst_sample_unref(s);
  }
  return GST_FLOW_OK;
}
static gboolean on_bus_msg(GstBus *bus, GstMessage *msg, gpointer data) {
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError *err = NULL; gchar *dbg = NULL;
      gst_message_parse_error(msg, &err, &dbg);
      fprintf(stderr, "[A bus] ERROR: %s (%s)\n", err->message, dbg ? dbg : "");
      g_clear_error(&err); g_free(dbg);
      g_main_loop_quit(loop); break;
    }
    case GST_MESSAGE_EOS: fprintf(stderr, "[A bus] EOS (count=%d)\n", count);
      g_main_loop_quit(loop); break;
    default: break;
  }
  return TRUE;
}
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GError* err = NULL;
  GstElement* p = gst_parse_launch(
    "filesrc location=./datasets/2k-test-h264/t50.mp4 "
    "! qtdemux ! h264parse "
    "! appsink name=sink drop=true max-buffers=2 sync=false", &err);
  if (!p) { fprintf(stderr, "parse failed: %s\n", err->message); return 1; }
  GstElement* appsink = gst_bin_get_by_name(GST_BIN(p), "sink");
  GstAppSinkCallbacks cbs = { NULL, NULL, on_new_sample, NULL };
  gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &cbs, NULL, NULL);
  GstBus *bus = gst_element_get_bus(p);
  gst_bus_add_watch(bus, on_bus_msg, NULL);
  gst_object_unref(bus);
  gst_element_set_state(p, GST_STATE_PLAYING);
  loop = g_main_loop_new(NULL, FALSE);
  g_main_loop_run(loop);
  fprintf(stderr, "[A] FINAL count: %d\n", count);
  gst_element_set_state(p, GST_STATE_NULL);
  gst_object_unref(p);
  return 0;
}
EOF
gcc /tmp/probeA.c -o /tmp/probeA $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0)
LD_LIBRARY_PATH=/usr/lib/aarch64-linux-gnu timeout 8 /tmp/probeA 2>&1 | head -30

echo
echo "=== B: mppvideodec + appsink (no qtdemux, 测试 gst_app_src 喂帧太复杂,跳过) ==="
echo
echo "=== C: 全 pipeline 但加 ts_offset / reset qtdemux ==="
echo "  idea: 末尾 mdat 不全 (faststart 把 moov 移到前面但 mdat 没变, 应仍 valid)"

echo
echo "=== D: 用 avdec_h264 (软件) 代替 mppvideodec, 看 H.264 流本身是否健康 ==="
cat > /tmp/probeD.c <<'EOF'
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <stdio.h>
static GMainLoop *loop = NULL;
static int count = 0;
static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user) {
  count++;
  GstSample *s = gst_app_sink_pull_sample(sink);
  if (s) {
    if (count % 30 == 1) {
      GstBuffer *buf = gst_sample_get_buffer(s);
      GstCaps *caps = gst_sample_get_caps(s);
      fprintf(stderr, "[D] sample #%d size=%lu caps=%s\n", count,
              gst_buffer_get_size(buf), gst_caps_to_string(caps));
    }
    gst_sample_unref(s);
  }
  return GST_FLOW_OK;
}
static gboolean on_bus_msg(GstBus *bus, GstMessage *msg, gpointer data) {
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError *err = NULL; gchar *dbg = NULL;
      gst_message_parse_error(msg, &err, &dbg);
      fprintf(stderr, "[D bus] ERROR: %s (%s)\n", err->message, dbg ? dbg : "");
      g_clear_error(&err); g_free(dbg);
      g_main_loop_quit(loop); break;
    }
    case GST_MESSAGE_EOS: fprintf(stderr, "[D bus] EOS (count=%d)\n", count);
      g_main_loop_quit(loop); break;
    default: break;
  }
  return TRUE;
}
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GError* err = NULL;
  GstElement* p = gst_parse_launch(
    "filesrc location=./datasets/2k-test-h264/t50.mp4 "
    "! qtdemux ! h264parse "
    "! avdec_h264 "
    "! appsink name=sink drop=true max-buffers=2 sync=false", &err);
  if (!p) { fprintf(stderr, "parse failed: %s\n", err->message); return 1; }
  GstElement* appsink = gst_bin_get_by_name(GST_BIN(p), "sink");
  GstAppSinkCallbacks cbs = { NULL, NULL, on_new_sample, NULL };
  gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &cbs, NULL, NULL);
  GstBus *bus = gst_element_get_bus(p);
  gst_bus_add_watch(bus, on_bus_msg, NULL);
  gst_object_unref(bus);
  gst_element_set_state(p, GST_STATE_PLAYING);
  loop = g_main_loop_new(NULL, FALSE);
  g_main_loop_run(loop);
  fprintf(stderr, "[D] FINAL count: %d\n", count);
  gst_element_set_state(p, GST_STATE_NULL);
  gst_object_unref(p);
  return 0;
}
EOF
gcc /tmp/probeD.c -o /tmp/probeD $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0)
LD_LIBRARY_PATH=/usr/lib/aarch64-linux-gnu timeout 30 /tmp/probeD 2>&1 | head -10