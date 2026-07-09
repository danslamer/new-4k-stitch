// gst_rtsp_server.cc
//
// v3.2 (2026-07-09): 实现 gst-rtsp-server 包装. 详见 gst_rtsp_server.h.
//
// 当板端缺 gstreamer-rtsp-server-1.0 时 (CMakeLists.txt 自动设 HAVE_GST_RTSP_SERVER=0),
// 所有成员函数都退化为 no-op, stitch 主循环正常跑, 只是不出 RTSP 流.

#include "gst_rtsp_server.h"

#include "config.h"  // v3.2: HAVE_GST_RTSP_SERVER

#include "logger.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#if HAVE_GST_RTSP_SERVER
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtsp-server/rtsp-server.h>
#endif

namespace gst_rtsp_server {

#if HAVE_GST_RTSP_SERVER

// 每路流的 runtime state. 一个 StreamPipeline 实例对应 yaml 里一个 stream 块.
class StreamPipeline {
 public:
    explicit StreamPipeline(output_streams::StreamConfig cfg) : cfg_(cfg) {}
    ~StreamPipeline() { Stop(); }

    void Start() {
        if (running_.exchange(true)) return;
        pump_thread_ = std::thread([this] { PumpLoop(); });
    }
    void Stop() {
        if (!running_.exchange(false)) return;
        queue_cv_.notify_all();
        if (pump_thread_.joinable()) pump_thread_.join();
        appsrc_.store(nullptr);
    }

    // Producer 调. copy bgr 后入队. 队列上限 4 (max-buffers=1 + drop=true 在 appsrc 端).
    void Push(const cv::Mat& bgr) {
        if (bgr.empty()) return;
        cv::Mat copy = bgr.clone();
        std::lock_guard<std::mutex> lk(queue_mutex_);
        if (queue_.size() >= 4) {
            // 队列满, 丢最早 (encoder 跟不上, 旧帧无意义)
            queue_.pop();
            drops_++;
        }
        queue_.push(std::move(copy));
        queue_cv_.notify_one();
    }

    void SetAppsrc(GstElement* appsrc) { appsrc_.store(appsrc); }
    GstElement* appsrc() const { return appsrc_.load(); }
    const output_streams::StreamConfig& cfg() const { return cfg_; }
    int drops() const { return drops_.load(); }

    // 构造 gstreamer pipeline 字符串 (作为 factory launch 用).
    std::string BuildPipelineString() const {
        const int w = cfg_.width  > 0 ? cfg_.width  : 1920;
        const int h = cfg_.height > 0 ? cfg_.height : 1080;
        const int fps = cfg_.fps > 0 ? cfg_.fps : 30;
        const int bps = cfg_.bitrate_kbps > 0 ? cfg_.bitrate_kbps : 4000;
        // HW h264 (mpph264enc). 兜底 sw (x264enc) 仅在 yaml encoder="h264_sw".
        const std::string enc = (cfg_.encoder == "h264_sw")
                                    ? std::string("x264enc tune=zerolatency speed-preset=ultrafast bitrate=") + std::to_string(bps)
                                    : std::string("mpph264enc bps=") + std::to_string(bps * 1000) +
                                          " rc-mode=cbr gop=" + std::to_string(fps * 2);
        std::ostringstream ss;
        std::string safe_name = "appsrc_";
        for (char c : cfg_.path) safe_name += (c == '/' ? '_' : c);
        ss << "appsrc name=" << safe_name
           << " format=time is-live=true do-timestamp=true block=false max-bytes=0"
           << " caps=\"video/x-raw,format=BGR,width=" << w
           << ",height=" << h
           << ",framerate=" << fps << "/1,pixel-aspect-ratio=1/1,interlace-mode=progressive\""
           << " ! videoconvert ! video/x-raw,format=NV12"
           << " ! " << enc
           << " ! h264parse ! rtph264pay name=pay0 pt=96 config-interval=1";
        return ss.str();
    }

 private:
    void PumpLoop() {
        Logger::GetInstance().Log(
            "[gst_rtsp_server] pump start path=" + cfg_.path +
            " " + std::to_string(cfg_.width) + "x" + std::to_string(cfg_.height) +
            "@" + std::to_string(cfg_.fps));
        while (running_.load()) {
            cv::Mat bgr;
            {
                std::unique_lock<std::mutex> lk(queue_mutex_);
                queue_cv_.wait_for(lk, std::chrono::milliseconds(100),
                                   [this] {
                                       return !queue_.empty() || !running_.load();
                                   });
                if (!running_.load()) break;
                if (queue_.empty()) continue;
                bgr = std::move(queue_.front());
                queue_.pop();
            }

            GstElement* appsrc = appsrc_.load();
            if (!appsrc) {
                // 还没客户端连; on_demand=true 时没客户端不编码, 直接丢.
                // on_demand=false 时, 等下次 client 来再编码也不迟.
                continue;
            }

            const int w = bgr.cols;
            const int h = bgr.rows;
            const gsize size = static_cast<gsize>(bgr.total() * bgr.elemSize());

            GstBuffer* buf = gst_buffer_new_allocate(nullptr, size, nullptr);
            if (!buf) {
                Logger::GetInstance().LogError(
                    "[gst_rtsp_server] gst_buffer_new_allocate failed");
                continue;
            }
            GstMapInfo info;
            if (!gst_buffer_map(buf, &info, GST_MAP_WRITE)) {
                gst_buffer_unref(buf);
                continue;
            }
            std::memcpy(info.data, bgr.data, size);
            gst_buffer_unmap(buf, &info);

            // 每次 push 都重设 caps (支持可变 width/height). 简单, 可优化.
            GstCaps* caps = gst_caps_new_simple(
                "video/x-raw", "format", G_TYPE_STRING, "BGR",
                "width", G_TYPE_INT, w,
                "height", G_TYPE_INT, h,
                "framerate", GST_TYPE_FRACTION, cfg_.fps, 1,
                "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
                "interlace-mode", G_TYPE_STRING, "progressive",
                nullptr);
            gst_app_src_set_caps(GST_APP_SRC(appsrc), caps);
            gst_caps_unref(caps);

            GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buf);
            if (ret != GST_FLOW_OK) {
                Logger::GetInstance().LogError(
                    "[gst_rtsp_server] push_buffer ret=" + std::to_string(ret) +
                    " path=" + cfg_.path);
            }
        }
        Logger::GetInstance().Log(
            "[gst_rtsp_server] pump stop path=" + cfg_.path);
    }

    output_streams::StreamConfig cfg_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<cv::Mat> queue_;
    std::atomic<bool> running_{false};
    std::thread pump_thread_;
    std::atomic<GstElement*> appsrc_{nullptr};
    std::atomic<int> drops_{0};
};

struct GstRtspServer::Impl {
    GstRTSPServer* server = nullptr;
    GstRTSPMountPoints* mounts = nullptr;
    std::map<std::string, std::unique_ptr<StreamPipeline>> pipelines;
    std::thread glib_thread;
    std::atomic<bool> glib_loop_running{false};
    GMainLoop* main_loop = nullptr;

    ~Impl() {
        for (std::map<std::string, std::unique_ptr<StreamPipeline> >::iterator
                 it = pipelines.begin(); it != pipelines.end(); ++it) {
            it->second->Stop();
        }
        if (glib_loop_running.load()) {
            if (main_loop) g_main_loop_quit(main_loop);
        }
        if (glib_thread.joinable()) glib_thread.join();
    }
};

// 工厂回调: media 创建时, 把 appsrc 抓给对应 stream pipeline.
static void OnMediaConstructed(GstRTSPMedia* media, gpointer user_data) {
    StreamPipeline* sp = reinterpret_cast<StreamPipeline*>(user_data);
    GstElement* appsrc = gst_bin_get_by_name_recurse_up(
        GST_BIN(media), "appsrc");
    if (!appsrc) {
        // Try without leading path separator
        std::string name = "appsrc_";
        for (std::string::const_iterator c = sp->cfg().path.begin();
             c != sp->cfg().path.end(); ++c) {
            if (*c != '/') name += *c;
        }
        appsrc = gst_bin_get_by_name_recurse_up(GST_BIN(media), name.c_str());
    }
    if (appsrc) {
        sp->SetAppsrc(appsrc);
        Logger::GetInstance().Log(
            "[gst_rtsp_server] media constructed path=" + sp->cfg().path +
            " appsrc linked");
    } else {
        Logger::GetInstance().LogError(
            "[gst_rtsp_server] media constructed path=" + sp->cfg().path +
            " but no appsrc found!");
    }
}

// media-configure 回调 (GstRTSPMediaFactory 创建后立即调用, 设置 hooks).
static GstElement* OnMediaConfigure(GstRTSPMediaFactory* factory,
                                  GstRTSPMedia* media, gpointer user_data) {
    StreamPipeline* sp = reinterpret_cast<StreamPipeline*>(user_data);
    g_signal_connect(media, "media-constructed",
                     G_CALLBACK(OnMediaConstructed), sp);
    return nullptr;  // 不需要返回 element
}

#else  // !HAVE_GST_RTSP_SERVER

// 板端缺 gstreamer-rtsp-server-1.0: header 里的 std::unique_ptr<Impl> 仍要分配.
// 给个空 stub 满足 ABI. 真实现见上面 #if 分支.
struct GstRtspServer::Impl {
    ~Impl() {}
};

#endif  // HAVE_GST_RTSP_SERVER

GstRtspServer& GstRtspServer::GetInstance() {
    static GstRtspServer inst;
    return inst;
}

GstRtspServer::GstRtspServer() : impl_(new Impl()) {}
GstRtspServer::~GstRtspServer() { Stop(); }

bool GstRtspServer::Start(const output_streams::ServerConfig& server_cfg,
                          const std::vector<output_streams::StreamConfig>& streams) {
#if HAVE_GST_RTSP_SERVER
    if (running_.exchange(true)) {
        Logger::GetInstance().Log("[gst_rtsp_server] already running, skip Start");
        return true;
    }

    if (!gst_is_initialized()) {
        gst_init(nullptr, nullptr);
    }

    // Impl 已经在 ctor 里 new 出来, 直接用.
    impl_->server = gst_rtsp_server_new();
    if (!impl_->server) {
        Logger::GetInstance().LogError("[gst_rtsp_server] gst_rtsp_server_new failed");
        running_ = false;
        return false;
    }

    // 绑定地址 + 端口
    if (!server_cfg.bind_address.empty()) {
        g_object_set(impl_->server, "address", server_cfg.bind_address.c_str(), nullptr);
    }
    g_object_set(impl_->server, "port", server_cfg.port, nullptr);
    g_object_set(impl_->server, "service", std::to_string(server_cfg.port).c_str(), nullptr);

    // mounts
    impl_->mounts = gst_rtsp_server_get_mount_points(impl_->server);

    // 每路流: 创建 factory
    for (std::vector<output_streams::StreamConfig>::const_iterator
             it = streams.begin(); it != streams.end(); ++it) {
        const output_streams::StreamConfig& sc = *it;
        // C++11: std::unique_ptr<T>(new T(args)) 替代 std::make_unique
        std::unique_ptr<StreamPipeline> sp(new StreamPipeline(sc));

        GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();
        std::string launch_str = sp->BuildPipelineString();
        gst_rtsp_media_factory_set_launch(factory, launch_str.c_str());

        // 共享媒体: 同一路多 client 共享 encoder pipeline.
        gst_rtsp_media_factory_set_shared(factory, TRUE);
        gst_rtsp_media_factory_set_latency(factory, 0);
        gst_rtsp_media_factory_set_transport_mode(factory, GST_RTSP_TRANSPORT_MODE_PLAY);

        // media-configure 回调: 把 appsrc 抓给 StreamPipeline
        g_signal_connect(factory, "media-configure",
                         G_CALLBACK(OnMediaConfigure), sp.get());

        // 挂到 mount point
        const std::string& path = sp->cfg().path;
        gst_rtsp_mount_points_add_factory(impl_->mounts, path.c_str(), factory);
        Logger::GetInstance().Log(
            "[gst_rtsp_server] factory created path=" + path +
            " launch=" + launch_str);

        sp->Start();
        impl_->pipelines[path] = std::move(sp);
    }

    // 鉴权: gstreamer 1.20+ 的 GstRTSPAuthToken/GstRTSPAuthorization API 与本版本头文件
    // 不一致 (本板 gst-rtsp-server 1.14-4 用 GstRTSPToken + gst_rtsp_auth_set_*);
    // 先把 warning log 出来, yaml auth_enabled=true 用户自行承担不生效.
    if (server_cfg.auth_enabled && !server_cfg.auth_user.empty()) {
        Logger::GetInstance().Log(
            "[gst_rtsp_server] WARN: auth_enabled=true in yaml, but auth wiring not yet "
            "ported to installed gstreamer-rtsp-server headers. Server runs unauthenticated.");
    }

    // 启动 glib main loop 在独立线程 (rtsp server attach 需要 loop/context)
    impl_->main_loop = g_main_loop_new(nullptr, FALSE);
    impl_->glib_loop_running = true;
    // C++11 lambda 不能用 init capture; 把 main_loop 指针按值捕获即可
    // (loop 生命周期 > 线程, 由 Impl 析构负责 quit + join).
    GMainLoop* loop_ptr = impl_->main_loop;
    impl_->glib_thread = std::thread([loop_ptr] {
        g_main_loop_run(loop_ptr);
    });

    // gst_rtsp_server_attach 收 GMainContext* (不是 GMainLoop*). 用 default context 即可.
    if (gst_rtsp_server_attach(impl_->server, g_main_context_default()) == 0) {
        Logger::GetInstance().LogError("[gst_rtsp_server] gst_rtsp_server_attach failed");
        g_main_loop_quit(impl_->main_loop);
        if (impl_->glib_thread.joinable()) impl_->glib_thread.join();
        running_ = false;
        return false;
    }

    Logger::GetInstance().Log(
        "[gst_rtsp_server] started, port=" + std::to_string(server_cfg.port) +
        " streams=" + std::to_string(streams.size()) +
        " on_demand=" + std::to_string(server_cfg.on_demand));
    return true;
#else
    // 无 gst-rtsp-server: yaml 即便 enabled=true 也不启, stitch 照常工作.
    (void)server_cfg;
    (void)streams;
    Logger::GetInstance().Log(
        "[gst_rtsp_server] HAVE_GST_RTSP_SERVER=0, RTSP output disabled (build without gstreamer-rtsp-server-1.0).");
    return false;
#endif
}

void GstRtspServer::Stop() {
#if HAVE_GST_RTSP_SERVER
    if (!running_.exchange(false)) {
        // header stub Impl 不需要清理
        return;
    }
    Logger::GetInstance().Log("[gst_rtsp_server] stopping");
    if (impl_) {
        impl_->pipelines.clear();  // calls StreamPipeline::Stop via ~unique_ptr
        if (impl_->main_loop) {
            g_main_loop_quit(impl_->main_loop);
        }
        if (impl_->glib_thread.joinable()) {
            impl_->glib_thread.join();
        }
        if (impl_->server) {
            g_object_unref(impl_->server);
            impl_->server = nullptr;
        }
        // 退回到 header stub, 下次 Start 重新填充.
        impl_.reset(new Impl());
    }
#else
    (void)impl_;
#endif
}

bool GstRtspServer::PushBgrFrame(const std::string& path, const cv::Mat& bgr) {
#if HAVE_GST_RTSP_SERVER
    if (!running_.load() || !impl_) return false;
    if (impl_->pipelines.empty()) return false;
    std::map<std::string, std::unique_ptr<StreamPipeline> >::iterator
        it = impl_->pipelines.find(path);
    if (it == impl_->pipelines.end()) return false;
    it->second->Push(bgr);
    return true;
#else
    (void)path;
    (void)bgr;
    return false;
#endif
}

int GstRtspServer::ClientCount(const std::string& path) const {
#if HAVE_GST_RTSP_SERVER
    if (!impl_ || impl_->pipelines.empty()) return 0;
    std::map<std::string, std::unique_ptr<StreamPipeline> >::const_iterator
        it = impl_->pipelines.find(path);
    if (it == impl_->pipelines.end()) return 0;
    return 0;  // TODO: 通过 GstRTSPMedia properties 查. 当前 on_demand 用 drops 估算.
#else
    (void)path;
    return 0;
#endif
}

}  // namespace gst_rtsp_server