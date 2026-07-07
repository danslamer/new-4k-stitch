#!/bin/bash
# 试 decodebin (auto demux+decode) - 看是否能跑通
cd /home/rocktech/Projects/new-4k-stitch

echo "=== F: decodebin 自动选 decoder + appsink ==="
cat > /tmp/probeF.c <<'EOF'
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <stdio.h>
static GMainLoop *loop = NULL;
static int count = 0;
static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user) {
  count++;
  GstSample *s = gst_app_sink_pull_sample(sink);
  if (s) {
    if (count % 20 == 1) {
      GstBuffer *buf = gst_sample_get_buffer(s);
      GstCaps *caps = gst_sample_get_caps(s);
      fprintf(stderr, "[F] sample #%d size=%lu caps=%s\n", count,
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
      fprintf(stderr, "[F bus] ERROR: %s (%s)\n", err->message, dbg ? dbg : "");
      g_clear_error(&err); g_free(dbg);
      g_main_loop_quit(loop); break;
    }
    case GST_MESSAGE_EOS: fprintf(stderr, "[F bus] EOS (count=%d)\n", count);
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
    "! decodebin "
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
  fprintf(stderr, "[F] FINAL count: %d\n", count);
  gst_element_set_state(p, GST_STATE_NULL);
  gst_object_unref(p);
  return 0;
}
EOF
gcc /tmp/probeF.c -o /tmp/probeF $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0)
LD_LIBRARY_PATH=/usr/lib/aarch64-linux-gnu timeout 30 /tmp/probeF 2>&1 | head -30