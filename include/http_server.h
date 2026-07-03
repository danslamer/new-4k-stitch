// http_server.h - image-stitching 嵌入式 HTTP server (v2.3 阶段 2)
// 用 cpp-httplib (vendor 在 third_party/cpp-httplib/)
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

namespace http_server {

struct Config {
    int         port = 8080;           // 监听端口
    const char* camera_page_dir = "../CameraPage";  // CameraPage 静态文件目录
    int         num_threads = 4;       // cpp-httplib 工作线程数
};

// 启动 HTTP server 线程. 返回 0 成功, -1 失败.
// 注意: 启动后会持续运行, 直到 stop() 被调用或进程退出.
int start(const Config& cfg);

// 停止 HTTP server.
void stop();

// 检查是否正在运行.
bool is_running();

}  // namespace http_server

#endif