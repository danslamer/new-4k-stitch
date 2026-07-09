#ifndef ROI_YAML_WATCHER_H
#define ROI_YAML_WATCHER_H

#include <atomic>
#include <string>
#include <thread>

// 监听 roi_tuning.yaml 文件修改时间 (mtime), 给主线程一个原子标志触发重建.
// 启动: App 构造里 new + Start() (后台 std::thread 跑 WatchLoop).
// 停止: App 析构里 Stop() (join thread).
// 主线程每帧: ShouldReload() → true 时 Consume() 复位, 然后 reload yaml + RebuildLayout().
//
// 设计要点:
//   * 只检测 mtime, 不主动解析 yaml — 重解析由主线程在原 load 路径上做, 避免
//     跨线程持有 cv::FileStorage / g_config 锁. 二次写入 (visualizer E 键) 与
//     mtime 跳变会被 50ms 抖动规避掉, 不会出现误触发.
//   * polling 间隔 500ms — 调参手感足够, 主线程完全不感知 watcher 存在.
class RoiYamlWatcher {
 public:
    explicit RoiYamlWatcher(const std::string& yaml_path);
    ~RoiYamlWatcher();

    // 启动后台线程, 记录当前 mtime 作为基线. 多次 Start() 安全.
    void Start();

    // 停后台线程 (join). 多次 Stop() 安全.
    void Stop();

    // 主线程读 — true 表示文件被外部修改过, 等着 reload.
    bool ShouldReload() const;

    // 主线程拿到 true 后, 调用一次复位原子标志.
    void Consume();

 private:
    void WatchLoop();

    std::string yaml_path_;
    std::atomic<bool> running_;
    std::atomic<bool> pending_reload_;
    std::thread thread_;
    long last_mtime_;  // seconds since epoch
};

#endif