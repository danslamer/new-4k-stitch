#!/bin/bash
cd /home/rocktech/Projects/new-4k-stitch

cat > /tmp/probe5.c <<'EOF'
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/allocators/gstdmabuf.h>
#include <stdio.h>

static GMainLoop *loop = NULL;
static int count = 0;

static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user) {
  count++;
  GstSample *s = gst_app_sink_pull_sample(sink);
  if (s) {
    GstBuffer *buf = gst_sample_get_buffer(s);
    GstCaps *caps = gst_sample_get_caps(s);
    guint n_mem = gst_buffer_n_memory(buf);
    int dmabuf_fd = -1;
    for (guint i = 0; i < n_mem; ++i) {
      GstMemory *mem = gst_buffer_peek_memory(buf, i);
      if (gst_is_dmabuf_memory(mem)) {
        dmabuf_fd = gst_dmabuf_memory_get_fd(mem);
      }
    }
    GstVideoInfo vinfo;
    if (gst_video_info_from_caps(&vinfo, caps)) {
      if (count % 30 == 1) {
        fprintf(stderr, "[cb] #%d: %dx%d %s dma_fd=%d (n_mem=%u)\n",
                count,
                GST_VIDEO_INFO_WIDTH(&vinfo),
                GST_VIDEO_INFO_HEIGHT(&vinfo),
                gst_video_format_to_string(GST_VIDEO_INFO_FORMAT(&vinfo)),
                dmabuf_fd, n_mem);
      }
    }
    gst_sample_unref(s);
  }
  return GST_FLOW_OK;
}

static gboolean on_bus_msg(GstBus *bus, GstMessage *msg, gpointer data) {
  GstElement *p = GST_ELEMENT(data);
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError *err = NULL;
      gchar *dbg = NULL;
      gst_message_parse_error(msg, &err, &dbg);
      fprintf(stderr, "[bus] ERROR: %s (%s)\n", err->message, dbg ? dbg : "");
      g_clear_error(&err); g_free(dbg);
      g_main_loop_quit(loop);
      break;
    }
    case GST_MESSAGE_EOS:
      fprintf(stderr, "[bus] EOS (count=%d)\n", count);
      g_main_loop_quit(loop);
      break;
    case GST_MESSAGE_STATE_CHANGED:
      if (GST_MESSAGE_SRC(msg) == GST_OBJECT(p)) {
        GstState old_s, new_s, pending;
        gst_message_parse_state_changed(msg, &old_s, &new_s, &pending);
        fprintf(stderr, "[bus] state %s -> %s\n",
                gst_element_state_get_name(old_s),
                gst_element_state_get_name(new_s));
      }
      break;
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
    "! mppvideodec dma-feature=true format=NV12 "
    "! video/x-raw(memory:DMABuf),format=NV12 "
    "! appsink name=sink drop=true max-buffers=2 sync=false", &err);
  if (!p) { fprintf(stderr, "parse failed: %s\n", err->message); return 1; }
  GstElement* appsink = gst_bin_get_by_name(GST_BIN(p), "sink");

  GstAppSinkCallbacks cbs = { NULL, NULL, on_new_sample, NULL };
  gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &cbs, NULL, NULL);

  GstBus *bus = gst_element_get_bus(p);
  gst_bus_add_watch(bus, on_bus_msg, p);
  gst_object_unref(bus);

  GstStateChangeReturn r = gst_element_set_state(p, GST_STATE_PLAYING);
  fprintf(stderr, "set_state PLAYING returned %d\n", r);

  loop = g_main_loop_new(NULL, FALSE);
  g_main_loop_run(loop);

  fprintf(stderr, "FINAL count: %d\n", count);
  gst_element_set_state(p, GST_STATE_NULL);
  gst_object_unref(p);
  return 0;
}
EOF
gcc /tmp/probe5.c -o /tmp/probe5 $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0) 2>&1 | head -3
LD_LIBRARY_PATH=/usr/lib/aarch64-linux-gnu timeout 10 /tmp/probe5 2>&1 | head -30