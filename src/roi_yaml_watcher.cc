#include "roi_yaml_watcher.h"
#include "logger.h"

#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace {
// polling 间隔 (毫秒). 调参手感足够, 主线程感知不到 watcher.
// 二次写入防抖窗口 (毫秒). visualizer 的 "E" 保存会在几毫秒内写两次 (cv::FileStorage
// open → close), mtime 跳变会被这次防抖过滤掉, 避免误触发 reload.
constexpr int kPollIntervalMs = 500;
constexpr int kDebounceMs = 50;

long StatMtime(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return -1;
    }
    return static_cast<long>(st.st_mtime);
}
}  // namespace

RoiYamlWatcher::RoiYamlWatcher(const std::string& yaml_path)
    : yaml_path_(yaml_path),
      running_(false),
      pending_reload_(false),
      last_mtime_(0) {
}

RoiYamlWatcher::~RoiYamlWatcher() {
    Stop();
}

void RoiYamlWatcher::Start() {
    // atomic compare-and-swap: 已经跑就直接返回.
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }

    last_mtime_ = StatMtime(yaml_path_);

    try {
        thread_ = std::thread(&RoiYamlWatcher::WatchLoop, this);
        Logger::GetInstance().Log(
            "[RoiYamlWatcher] watching " + yaml_path_ +
            " (poll=" + std::to_string(kPollIntervalMs) + "ms)");
    } catch (const std::exception& e) {
        running_.store(false);
        Logger::GetInstance().LogError(
            std::string("[RoiYamlWatcher] thread start failed: ") + e.what());
    }
}

void RoiYamlWatcher::Stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    Logger::GetInstance().Log("[RoiYamlWatcher] stopped");
}

bool RoiYamlWatcher::ShouldReload() const {
    return pending_reload_.load();
}

void RoiYamlWatcher::Consume() {
    pending_reload_.store(false);
}

void RoiYamlWatcher::WatchLoop() {
    while (running_.load()) {
        usleep(kPollIntervalMs * 1000);
        if (!running_.load()) break;

        long mtime = StatMtime(yaml_path_);
        if (mtime < 0) {
            // 文件不存在/不可读 — 跳过本轮, 下轮再试.
            continue;
        }
        if (mtime == last_mtime_) {
            continue;
        }

        // 防抖: 等待 kDebounceMs 后再 stat 一次, mtime 稳定才算真变更.
        // 避免 visualizer 的 SaveConfig 触发的瞬时回声.
        usleep(kDebounceMs * 1000);
        long mtime2 = StatMtime(yaml_path_);
        if (mtime2 != mtime) {
            // 文件还在被写, mtime 又跳了, 放弃本轮, 下轮重新基线化.
            last_mtime_ = mtime2;
            continue;
        }

        last_mtime_ = mtime;
        pending_reload_.store(true);
        Logger::GetInstance().Log(
            "[RoiYamlWatcher] mtime changed, reload flag set");
    }
}