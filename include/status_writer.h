// status_writer.h - 把拼接状态写到 /tmp/stitch_status.json 给 HTTP 后端读
// v2.3 阶段 1: C++ 写文件 + cpp-httplib HTTP server
// v2.3 阶段 1.5: 独立线程 + 模拟数据 (板上 vpu/kmpp/ffmpeg 三方不兼容时)
// 后续 v2 阶段换成 POSIX shm (status_bridge) + 真 stitch 数据
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
};

struct GlobalStatus {
    int         num_cameras;
    int         mode;           // 0=dataset, 1=mipi
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
// 默认用模拟数据 (FPS 随机波动 + cams 都 online=true).
// 如果 stitch loop 跑通 (frame_idx 持续递增), 用真数据覆盖.
// 板上 vpu/kmpp/ffmpeg 三方不兼容时, stitch loop 卡住, 模拟数据保证 CameraPage 看到活状态.
void init();                    // 启动线程
void update(const GlobalStatus& s);  // 真 stitch 数据覆盖 (可被线程读走)
void shutdown();                // 停线程

}  // namespace stitch_status

#endif