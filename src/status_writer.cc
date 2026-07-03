// status_writer.cc - 独立线程 + 模拟数据写 /tmp/stitch_status.json
// v2.3 阶段 1.5: 因为板上 vpu/kmpp/ffmpeg 三方不兼容, stitch loop 卡住取不到帧,
// 所以 CameraPage 需要的状态不能依赖 stitch loop. 改为独立线程模拟数据.
// 真 stitch 数据通过 update() 接口覆盖 (被 update_lock_ 保护, 线程安全).
#include "status_writer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <unistd.h>
#include <fcntl.h>

namespace stitch_status {

static const char* kStatusPath = "/tmp/stitch_status.json";
static const char* kTempPath   = "/tmp/stitch_status.json.tmp";

// 模拟数据: 6 路 cam, 2613x1440 NV12, 24-30 fps 波动
static constexpr int kNumCams = 6;
static constexpr int kPanoramaW = 4613;  // 2x3 拼接 6 路 2K
static constexpr int kPanoramaH = 3888;
static constexpr int kCamW = 2560;
static constexpr int kCamH = 1440;

namespace {

std::thread       g_thread;
std::atomic<bool> g_running{false};
std::mutex        g_data_mutex;
GlobalStatus      g_latest;       // 最新一次 update() 写入的数据 (真 stitch 数据)
bool              g_have_real = false;  // 是否收到过真数据
std::atomic<int64_t> g_sim_frame{0};

void write_atomic(const char* content, size_t len) {
    int fd = open(kTempPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    ssize_t w = write(fd, content, len);
    close(fd);
    if (w == (ssize_t)len) rename(kTempPath, kStatusPath);
}

// 生成模拟数据 (只在没有真数据时用)
GlobalStatus make_simulated() {
    GlobalStatus s;
    memset(&s, 0, sizeof(s));
    s.num_cameras = kNumCams;
    s.mode = 0;
    s.panorama_w = kPanoramaW;
    s.panorama_h = kPanoramaH;

    // FPS 在 24-30 波动 (伪随机)
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<double> fps_dist(24.0, 30.0);
    std::uniform_int_distribution<int> fps_jitter(28, 30);
    s.current_fps = fps_dist(rng);
    s.frame_idx = g_sim_frame.fetch_add(1);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    s.timestamp_us = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;

    // blend/warp 模拟耗时 (毫秒)
    s.blend_ms = 8 + (s.frame_idx % 5);  // 8-12 ms
    s.warp_ms  = 3 + (s.frame_idx % 3);  // 3-5 ms

    for (int i = 0; i < kNumCams; ++i) {
        s.cams[i].online = 1;
        s.cams[i].fps = fps_jitter(rng);
        s.cams[i].width = kCamW;
        s.cams[i].height = kCamH;
        snprintf(s.cams[i].name, sizeof(s.cams[i].name), "cam%d", i);
        snprintf(s.cams[i].uri,  sizeof(s.cams[i].uri),  "../datasets/2k-test/cam%d.mp4", i);
    }
    return s;
}

void worker_thread() {
    fprintf(stderr, "[status_writer] worker thread started\n");
    auto next_write = std::chrono::steady_clock::now();

    while (g_running.load()) {
        // 拼装要写的数据: 如果有真 stitch 数据, 用真数据; 否则用模拟
        GlobalStatus s;
        {
            std::lock_guard<std::mutex> lock(g_data_mutex);
            if (g_have_real) s = g_latest;
            else             s = make_simulated();
        }

        // 序列化 JSON
        char buf[8192];
        int n = 0;
        n += snprintf(buf + n, sizeof(buf) - n,
            "{\n"
            "  \"num_cameras\": %d,\n"
            "  \"mode\": %d,\n"
            "  \"panorama_w\": %d,\n"
            "  \"panorama_h\": %d,\n"
            "  \"current_fps\": %.2f,\n"
            "  \"frame_idx\": %ld,\n"
            "  \"timestamp_us\": %ld,\n"
            "  \"blend_ms\": %d,\n"
            "  \"warp_ms\": %d,\n"
            "  \"cams\": [\n",
            s.num_cameras, s.mode, s.panorama_w, s.panorama_h,
            s.current_fps, (long)s.frame_idx, (long)s.timestamp_us,
            s.blend_ms, s.warp_ms);

        for (int i = 0; i < s.num_cameras && i < 6; ++i) {
            const CameraStatus& c = s.cams[i];
            n += snprintf(buf + n, sizeof(buf) - n,
                "    {\"online\": %d, \"fps\": %d, \"width\": %d, \"height\": %d, "
                "\"name\": \"%s\", \"uri\": \"%s\"}%s\n",
                c.online, c.fps, c.width, c.height,
                c.name, c.uri,
                (i < 5 && i < s.num_cameras - 1) ? "," : "");
        }
        n += snprintf(buf + n, sizeof(buf) - n,
            "  ],\n  \"simulated\": %s\n}\n",
            g_have_real ? "false" : "true");

        if (n > 0 && n < (int)sizeof(buf)) write_atomic(buf, (size_t)n);

        // 500ms 写一次
        next_write += std::chrono::milliseconds(500);
        std::this_thread::sleep_until(next_write);
    }
    fprintf(stderr, "[status_writer] worker thread exiting\n");
}

}  // namespace

void init() {
    if (g_running.load()) return;
    g_running.store(true);
    g_thread = std::thread(worker_thread);
}

void update(const GlobalStatus& s) {
    std::lock_guard<std::mutex> lock(g_data_mutex);
    g_latest = s;
    g_have_real = true;
}

void shutdown() {
    if (!g_running.load()) return;
    g_running.store(false);
    if (g_thread.joinable()) g_thread.join();
    unlink(kStatusPath);
    unlink(kTempPath);
}

}  // namespace stitch_status