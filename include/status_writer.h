// status_writer.h - 写 /tmp/stitch_status.json 给 CameraPage / HTTP 后端读
// v3.0 (2026-07-08): 加 set_online() 让 SensorDataInterface 上报单路 online 状态
//   (rtsp 重连 / 拔网线会反映成 online=0). YAML 是真实 URI 来源.
#ifndef STATUS_WRITER_H
#define STATUS_WRITER_H

#include <atomic>
#include <cstdint>

namespace stitch_status {

struct CameraStatus {
    int     online;
    int     fps;
    int     width;
    int     height;
    char    name[64];
    char    uri[128];
    // v3.0 新增: 让 CameraPage 区分 file / rtsp 输入源 (保留旧字段兼容)
    // 注: is_rtsp 不进 CameraStatus 结构, 见 .cc worker_thread 序列化时从 yaml 同步取
};

struct GlobalStatus {
    int         num_cameras;
    int         mode;           // 0=dataset (legacy), 1=camera (yaml, 含 rtsp/file), 2=mipi(deprecated)
    int         panorama_w;
    int         panorama_h;
    double      current_fps;
    int64_t     frame_idx;
    int64_t     timestamp_us;
    CameraStatus cams[6];
    int         blend_ms;
    int         warp_ms;
};

// 启动独立线程, 每 500ms 写一次 /tmp/stitch_status.json
void init();
void update(const GlobalStatus& s);            // 真 stitch 数据覆盖 (被 mutex 保护)
void set_online(int cam_index, int online);    // v3.0: 单路 online 状态上报 (0/1)
void shutdown();

}  // namespace stitch_status

#endif