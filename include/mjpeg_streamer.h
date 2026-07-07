// mjpeg_streamer.h - 跨线程共享最新 JPEG 帧的 producer/consumer 桥
//
// 设计: 全局唯一一张最新 JPEG buffer + atomic seq + condvar.
// producer (stitch loop) -> update(jpg_buf) 覆盖式写入.
// consumer (http handler) -> wait_for_new_frame(seq_out, out, timeout_ms) 阻塞等新帧.
//
// 镜像 src/status_writer.cc 的跨线程模式 (g_data_mutex + g_latest + 模拟写).
// 这里不是写文件, 而是把最新 JPEG 推到内嵌 httplib chunked provider.
#ifndef MJPEG_STREAMER_H
#define MJPEG_STREAMER_H

#include <atomic>
#include <cstdint>
#include <vector>

namespace mjpeg_streamer {

// 初始化 / 清理全局状态 (隐式也通过静态初始化可工作, 但显式调一次更稳).
void init();
void shutdown();

// Producer: stitch loop 调, 把刚 imencode 出来的 jpg_buf 覆盖到全局.
// 内容拷贝一份进 mutex 区, 不会阻塞超过锁内 std::vector 复制时间.
void update(const std::vector<unsigned char>& jpg_buf);

// Consumer: http handler 调, 阻塞等待一个新帧.
//   out_buf: 拷出当前最新 JPEG
//   timeout_ms: 最长等多久 (ms). 0 = 不等, 立即返回当前缓存;
//               客户断开时让 httplib cleanup 也能跑.
// 返回: true = 拿到新内容 (seq > 上次 seq_or_null, 或缓存非空), false = 超时/未就绪.
//
// 用法:
//   uint64_t last_seq = 0;
//   std::vector<uchar> buf;
//   if (mjpeg_streamer::wait_for_new_frame(&last_seq, buf, 1000)) {
//     // 写 buf 到 socket
//   }
bool wait_for_new_frame(uint64_t* seq_io,
                        std::vector<unsigned char>& out_buf,
                        int timeout_ms);

// Consumer 单次快照: 返回最新 JPEG (不阻塞). 没就绪则返回 false.
bool get_latest_snapshot(std::vector<unsigned char>& out_buf);

// 当前最新帧序号 (用于调试/监控).
uint64_t latest_seq();

}  // namespace mjpeg_streamer

#endif
