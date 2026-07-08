// status_writer.cc - 独立线程 + 写 /tmp/stitch_status.json 给 CameraPage 读.
// v3.0 (2026-07-08): 不再用硬编码 mp4 路径. 改读 g_camera_source_list (yaml) 把真实
//   URI + is_rtsp / online 写出去. 没有 yaml 时 fallback 到原模拟数据 (dev/board-on-shelve).
// 真 stitch 数据仍由 update() 接口覆盖 (被 g_data_mutex 保护).
#include "status_writer.h"

#include "sensor_data_interface.h"

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

// 模拟数据 (没有 yaml / 还 fallback 时期). v3.0 这些只是 dev 占位.
static constexpr int kNumCams = 6;
static constexpr int kPanoramaW = 4613;  // 2x3 拼接 6 路 2K (近似)
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

// ★ v3.0: 6 路 online 状态, 由 SensorDataInterface::get_frame_vector 反馈 (Sprint 0 简化,
//   直接读 GetDecodeFpsSnapshot() 是否 > 0 推 online; Sprint 1 可换成显式 online_ 数组).
std::atomic<int>  g_online[6] = {{}, {}, {}, {}, {}, {}};

void write_atomic(const char* content, size_t len) {
    int fd = open(kTempPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    ssize_t w = write(fd, content, len);
    close(fd);
    if (w == (ssize_t)len) rename(kTempPath, kStatusPath);
}

// v3.0: 读 yaml 实际 URI + is_rtsp 填每一路 status, 真实数据流入.
//   缺 yaml (camera_sources list 还没初始化) 时, 退回旧 sim 行为.
GlobalStatus make_from_sources_or_sim() {
    GlobalStatus s;
    memset(&s, 0, sizeof(s));
    s.panorama_w = kPanoramaW;
    s.panorama_h = kPanoramaH;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    s.timestamp_us = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;

    const CameraSourceList& sl = GetCameraSourceList();
    const bool have_yaml = !sl.cameras.empty();

    if (have_yaml) {
        // 读 decode_fps 推 online
        // Sprint 0 简化: online 直接看 set_online() 维护的状态.
        // 注: set_online() 由 SensorDataInterface::get_frame_vector 同步调用 (在 stitch loop 主线程)
        // 或 rtsp decoder 异常时清零. Sprint 1 可改成显式 thread-safe 状态机.
        int n = static_cast<int>(std::min<size_t>(sl.cameras.size(), 6));
        s.num_cameras = n;
        s.mode = 1;          // v3.0: mode=1 = camera (yaml 走的非 dataset)
        s.current_fps = 30.0;
        s.frame_idx = g_sim_frame.fetch_add(1);
        for (int i = 0; i < n; ++i) {
            const CameraSource& c = sl.cameras[i];
            s.cams[i].online = g_online[i].load();
            s.cams[i].fps    = s.cams[i].online ? 30 : 0;
            s.cams[i].width  = c.width  > 0 ? c.width  : kCamW;
            s.cams[i].height = c.height > 0 ? c.height : kCamH;
            snprintf(s.cams[i].name, sizeof(s.cams[i].name), "cam%d", i);
            // uri: 优先放 rtsp URL / 否则 file 路径. 截断到 sizeof-1 安全.
            std::string uri = c.uri;
            if (uri.size() >= sizeof(s.cams[i].uri)) uri.resize(sizeof(s.cams[i].uri) - 1);
            snprintf(s.cams[i].uri, sizeof(s.cams[i].uri), "%s", uri.c_str());
        }
    } else {
        // fallback: 老 sim 行为
        static thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<double> fps_dist(24.0, 30.0);
        std::uniform_int_distribution<int> fps_jitter(28, 30);
        s.num_cameras = kNumCams;
        s.mode = 0;
        s.current_fps = fps_dist(rng);
        s.frame_idx = g_sim_frame.fetch_add(1);
        s.blend_ms = 8 + (s.frame_idx % 5);
        s.warp_ms  = 3 + (s.frame_idx % 3);
        for (int i = 0; i < kNumCams; ++i) {
            s.cams[i].online = 1;
            s.cams[i].fps    = fps_jitter(rng);
            s.cams[i].width  = kCamW;
            s.cams[i].height = kCamH;
            snprintf(s.cams[i].name, sizeof(s.cams[i].name), "cam%d", i);
            snprintf(s.cams[i].uri,  sizeof(s.cams[i].uri),  "../datasets/2k-test/cam%d.mp4", i);
        }
    }
    return s;
}

void worker_thread() {
    fprintf(stderr, "[status_writer] worker thread started\n");
    auto next_write = std::chrono::steady_clock::now();

    while (g_running.load()) {
        GlobalStatus s;
        {
            std::lock_guard<std::mutex> lock(g_data_mutex);
            if (g_have_real) s = g_latest;
            else             s = make_from_sources_or_sim();
        }

        // 序列化 JSON (v3.0 加 is_rtsp / online_n 字段给 CameraPage 调试看)
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

        const CameraSourceList& sl = GetCameraSourceList();
        for (int i = 0; i < s.num_cameras && i < 6; ++i) {
            const CameraStatus& c = s.cams[i];
            int is_rtsp = (i < static_cast<int>(sl.cameras.size())
                           && sl.cameras[i].is_rtsp()) ? 1 : 0;
            n += snprintf(buf + n, sizeof(buf) - n,
                "    {\"online\": %d, \"fps\": %d, \"is_rtsp\": %d, "
                "\"width\": %d, \"height\": %d, "
                "\"name\": \"%s\", \"uri\": \"%s\"}%s\n",
                c.online, c.fps, is_rtsp, c.width, c.height,
                c.name, c.uri,
                (i < 5 && i < s.num_cameras - 1) ? "," : "");
        }
        n += snprintf(buf + n, sizeof(buf) - n,
            "  ],\n  \"simulated\": %s\n}\n",
            g_have_real ? "false" : (sl.cameras.empty() ? "true" : "false"));

        if (n > 0 && n < (int)sizeof(buf)) write_atomic(buf, (size_t)n);

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

void set_online(int cam_index, int online) {
    if (cam_index >= 0 && cam_index < 6) g_online[cam_index].store(online);
}

void shutdown() {
    if (!g_running.load()) return;
    g_running.store(false);
    if (g_thread.joinable()) g_thread.join();
    unlink(kStatusPath);
    unlink(kTempPath);
}

}  // namespace stitch_status