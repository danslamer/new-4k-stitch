// mjpeg_streamer.cc - 实现 MJPEG producer/consumer 共享缓冲.
// 模式抄 src/status_writer.cc (g_data_mutex + g_latest + atomic seq).
#include "mjpeg_streamer.h"

#include <condition_variable>
#include <cstdio>
#include <mutex>

namespace mjpeg_streamer {

namespace {

std::mutex                 g_mutex;
std::condition_variable    g_cv;
std::vector<unsigned char> g_latest_jpeg;
uint64_t                   g_latest_seq = 0;     // 0 = 没就绪
bool                       g_running = false;

}  // namespace

void init() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_running = true;
  g_latest_jpeg.clear();
  g_latest_seq = 0;
}

void shutdown() {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_running = false;
    g_latest_jpeg.clear();
  }
  g_cv.notify_all();
}

void update(const std::vector<unsigned char>& jpg_buf) {
  if (jpg_buf.empty()) return;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_latest_jpeg = jpg_buf;       // copy
    ++g_latest_seq;
  }
  g_cv.notify_all();
}

bool wait_for_new_frame(uint64_t* seq_io,
                        std::vector<unsigned char>& out_buf,
                        int timeout_ms) {
  if (!seq_io) return false;
  std::unique_lock<std::mutex> lock(g_mutex);
  const uint64_t want_seq = *seq_io + 1;   // 至少下一帧

  if (g_latest_seq >= want_seq) {
    // 已经更新过, 立即返回当前帧.
    out_buf = g_latest_jpeg;
    *seq_io = g_latest_seq;
    return !out_buf.empty();
  }

  if (timeout_ms <= 0) {
    if (g_latest_seq == 0 || g_latest_jpeg.empty()) return false;
    out_buf = g_latest_jpeg;
    *seq_io = g_latest_seq;
    return true;
  }

  // 等新帧, 或者超时 / shutdown.
  auto deadline = std::chrono::steady_clock::now() +
                 std::chrono::milliseconds(timeout_ms);
  while (g_running && g_latest_seq < want_seq) {
    if (g_cv.wait_until(lock, deadline) == std::cv_status::timeout) {
      break;
    }
  }
  if (!g_running || g_latest_seq == 0 || g_latest_jpeg.empty()) {
    return false;
  }
  out_buf = g_latest_jpeg;
  *seq_io = g_latest_seq;
  return true;
}

bool get_latest_snapshot(std::vector<unsigned char>& out_buf) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_latest_seq == 0 || g_latest_jpeg.empty()) return false;
  out_buf = g_latest_jpeg;
  return true;
}

uint64_t latest_seq() {
  return g_latest_seq;
}

}  // namespace mjpeg_streamer
