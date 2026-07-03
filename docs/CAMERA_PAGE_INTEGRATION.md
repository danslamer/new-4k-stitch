# CameraPage × image-stitching 接入方案 (v2.4, 2026-07-03)

> **状态**: 阶段 1 完成 (HTTP + 模拟 status), 阶段 2-5 待做
> **更新**: 2026-07-03
> **总工作量**: 13-15 天 (阶段 2 软编路径 +3 天, MPP 路径不增加)

## 0. 设计决策（锁定 v2.4）

| # | 决策项 | **最终选择** | 理由 |
|---|---|---|---|
| 1 | 接入架构 | 方案 A：image-stitching 内嵌 HTTP server | 单进程，符合"板上自主"原则 |
| 2 | 视频流方案 | **方案 X**：全分辨率 4613×3888 H.264 给浏览器 | 质量优先，局域网带宽够 |
| 3 | 视频编码 | H.264 (软编 **libx264** 现可用 / **MPP h264_rkmpp** vpu 修好后切) | 阶段 3 用 libx264 绕过 vpu 限制 |
| 4 | RGA 缩放 | **完全删除** | NV12→NV12 不需要转换；方案 X 不缩放 |
| 5 | HTTP 库 | cpp-httplib (header-only) | 零依赖，~1MB |
| 6 | WebSocket | WebSocket + MSE + fMP4 | 浏览器原生支持，延迟 100-300ms |
| 7 | 状态同步 | **/tmp/stitch_status.json (POSIX 原子 rename) + 独立 worker 线程** | 板端 vpu 卡死时仍能写 |
| 8 | 端口 | 8080 | 复用 CameraPage 默认 |
| 9 | 算法管理模块 | v1 隐藏整个模块 | 当前无算法实现 |
| 10 | 网络配置模块 | 改板端真实网络 | 读 `/sys/class/net/...` 和 `ip 命令` |

## 0.5 当前进度 (2026-07-03)

| 阶段 | 内容 | 状态 | 备注 |
|---|---|---|---|
| **1. 阶段 1: 后端基础设施** | HTTP server + 状态桥接 (模拟数据) | ✅ **完成** | 板端跑通 (PID 84321, 端口 8080) |
| 1a | `status_writer.h/cc` (独立线程 + 模拟) | ✅ | vpu/kmpp 卡死时, 500ms 写模拟 status |
| 1b | `http_server.h/cc` (cpp-httplib) | ✅ | CameraPage 静态 + 5 个 API 端点 |
| 1c | `third_party/cpp-httplib/httplib.h` (v0.49.0) | ✅ | header-only, gh-proxy.com 拉取 |
| 1d | CMakeLists.txt 集成 | ✅ | vendor 检测, 缺则降级 stub |
| 1e | 板端 `image-stitching` 构建通过 | ✅ | PID 84321 在跑 |
| 1f | CameraPage 静态文件 serve | ✅ | `curl /` 返回 DOCTYPE 正确 |
| 1g | `/api/health` | ✅ | `{"ok": true}` |
| 1h | `/api/status` (JSON 模拟数据) | ✅ | 6 路 cam online, frame_idx 递增, `simulated: true` |
| 1i | `/api/devices` (从 yaml 解析) | ✅ | 6 路 |
| 1j | `/api/config` (从 yaml 读) | ✅ | 完整 yaml |
| 1k | `/api/network` (从 /sys/class/net) | ✅ | primary_ip = 192.168.137.100 (eth0) |
| 1l | `POST /api/roi` (原子 yaml 写) | ✅ | tmp + rename 原子替换 |
| **2. 阶段 2: H.264 编码 + fMP4 + WebSocket** | libx264 软编 → fMP4 → WS → 浏览器 MSE | ⏳ **暂停** | 等 vpu 修好后改用 MPP; 当前可继续 libx264 路径 |
| 2a | `h264_encoder_x264.{h,cc}` (libx264 wrapper) | ❌ | 0.5 天 |
| 2b | `fmp4_muxer.{h,cc}` (NAL → fragmented MP4) | ❌ | 1-1.5 天 |
| 2c | `http_server` 加 WS handler (binary push) | ❌ | 0.5 天 |
| 2d | `app.cc` 接 OpenCL blend → encoder | ❌ | 0.5 天 |
| **3. 阶段 3: CameraPage 前端改造** | 4 模块接真 API + `<video>` 替换 mock | ⏳ | 等阶段 2 视频流 |
| 3a | `js/app.js` 删 setInterval 假数据, 接 `/api/*` | ❌ | 1 天 |
| 3b | `index.html` `<video>` + MSE 替换占位图 | ❌ | 0.5 天 |
| 3c | 算法管理模块隐藏 (display:none 或注释) | ❌ | 0.5 天 |
| **4. 阶段 4: 真数据接入** | vpu/kmpp/ffmpeg 修好后 | ⏳ **等厂商** | vpu 缺固件 + kmpp 缺内核模块 + ffmpeg 编译不兼容 |
| 4a | `status_writer` 自动从模拟切真 (`g_have_real` 已就位) | 🟡 等 | 改完一帧 `update()` 自动覆盖 |
| 4b | `h264_rkmpp` 替换 libx264 (几行) | 🟡 等 | 切 mpp_init + mpp_put_packet |
| **5. 阶段 5: 生产化** | 多客户端 / RTSP / 鉴权 / 热更新 | ⏳ | 远期 |
| 5a | 多客户端 (WS broadcast) | ❌ |  |
| 5b | RTSP 输出 (ffmpeg 拉 H.264 → RTSP) | ❌ |  |
| 5c | HTTP 鉴权 (login + JWT) | ❌ |  |
| 5d | yaml 热更新 (inotify 触发 reload) | ❌ |  |
| 5e | 算法扩展 (ONNX/RKNN 6 路推理) | ❌ |  |

**当前真实状态**:
- ✅ 后端基础架构完整 (HTTP server 跑通, 5 个 API 端点都通, 板端进程在 8080 监听)
- ✅ CameraPage 静态文件 serve 正常 (浏览器能打开, 但看到的是 mock 数据, 4 模块显示 fake setInterval 数据)
- ⏳ 视频流: 未做 (浏览器看到的是 CameraPage mock 占位图, 真实的 stitched video 还没推到浏览器)
- ⏳ 真实数据: 等 vpu 修好才能看到真 stitch 结果 (当前 `simulated: true`)

## 0.6 板上硬件约束 (2026-07-03 实测)

| 项 | 状态 | 影响 |
|---|---|---|
| kernel 6.1.75 + Ubuntu 22.04 | ✅ OK |  |
| GC4683 驱动 v00.01.01 (built-in) | ✅ OK | DTS 6 路 sensor 节点齐 (5 gc4683 + 1 gc5035), 厂商待修 |
| ffmpeg 6.1 (含 h264_rkmpp, --enable-rkmpp) | ⚠️ 编码器可识别但**VPU 缺固件** | 不能用 h264_rkmpp 编码 |
| libx264 v0.163 (apt 装) | ✅ OK | **当前用软件编码兜底** |
| 板载 DSI 屏 (`card0-DSI-1`) | disconnected | 无物理显示, 走 HTTP CameraPage |
| 6 路 mpp_dec 线程在跑 | ✅ OK | 等不到帧是因为 vpu 缺, 不是驱动问题 |
| 11 | 客户端数量 | v1 单客户端 | 简化设计 |
| 12 | 鉴权 | v1 无（仅内网） | LAN 部署 |
| 13 | 后端编码格式 | NV12 全程，无转换 | GC4683+rkisp 输出 NV12，MPP 原生 NV12 |
| 14 | 拼接输出尺寸 | 4613×3888（全分辨率） | 方案 X |
| 15 | H.264 码率 | 15-20 Mbps @ 30fps | 方案 X |

## 1. 最终架构

```
┌────────────────────────────────────────────────────────────────────┐
│  Rockchip 板 (rockemb, 192.168.137.200:8080)                       │
│                                                                     │
│  ┌──── image-stitching 单进程 (Linux ARM64) ──────────┐            │
│  │  拼接主线程                                       │            │
│  │  6× V4L2 → rkisp ─→ NV12 (2560×1440)              │            │
│  │       ↓                                          │            │
│  │  RGA crop/rotate ─→ NV12 (ROI)                   │            │
│  │       ↓                                          │            │
│  │  OpenCL blend ─→ NV12 panorama (4613×3888)        │            │
│  │       ↓                                          │            │
│  │  路径 A: DRM 输出 (物理显示, 零拷贝)             │            │
│  │  路径 B: MPP H.264 编码 (NV12 → H.264)           │            │
│  │       ↓                                          │            │
│  │  fMP4 muxer → ring buffer → WebSocket             │            │
│  │  每帧结尾: 更新 /dev/shm/stitch_status           │            │
│  │                                                  │            │
│  │  HTTP/WS 服务线程 (cpp-httplib, 端口 8080)       │            │
│  │  GET /api/{status,devices,config,network}        │            │
│  │  POST /api/roi                                   │            │
│  │  WS /ws/preview → fMP4 stream                    │            │
│  └──────────────────────────────────────────────────┘            │
└────────────────────────────────────────────────────────────────────┘
                          ↑ HTTP / WebSocket
                          │
                  ┌───────┴────────┐
                  │  PC 浏览器       │
                  │  CameraPage UI  │
                  │  <video> + MSE  │
                  └─────────────────┘
```

## 2. 文件改动清单

### 新增

| 文件 | 行数估 | 职责 |
|---|---|---|
| [include/http_server.h](../include/http_server.h) | 50 | HTTP server 接口 |
| [src/http_server.cc](../src/http_server.cc) | 250 | cpp-httplib 集成，路由，handler |
| [src/status_bridge.cc](../src/status_bridge.cc) | 80 | POSIX shm 写入/读取 |
| [include/mpp_encoder.h](../include/mpp_encoder.h) | 40 | MPP H.264 编码器封装 |
| [src/mpp_encoder.cc](../src/mpp_encoder.cc) | 200 | MPP API 调用，NV12 → H.264 |
| [src/fmp4_muxer.cc](../src/fmp4_muxer.cc) | 150 | H.264 NAL → fragmented MP4 |
| [include/ws_server.h](../include/ws_server.h) | 30 | WebSocket 接口 |
| [src/ws_server.cc](../src/ws_server.cc) | 120 | WS handler，binary frame push |
| [src/network_info.cc](../src/network_info.cc) | 60 | 读 `/sys/class/net/...` |
| **小计** | **~980** | |

### 修改

- [CMakeLists.txt](../CMakeLists.txt) — FetchContent 拉 cpp-httplib；加新 .cc 到 target_sources
- [src/app.cc](../src/app.cc) — 主循环结尾写 status；启 HTTP 线程；OpenCL 输出喂 MPP encoder
- [src/image_stitcher.cc](../src/image_stitcher.cc) — 导出 NV12 给 MPP（不直接给 DRM）
- [src/roi_visualizer.cc](../src/roi_visualizer.cc) — 4 → 6 (CAM_COLORS_BGR / CAM_LABELS / CAM_POSITIONS)

## 3. 部署阶段（9-11 天）

### 阶段 1: HTTP server + 状态桥接（2-3 天）
- CMakeLists.txt: FetchContent cpp-httplib
- status_bridge.{h,cc}: POSIX shm `/dev/shm/stitch_status`
- http_server.{h,cc}: cpp-httplib 集成
- app.cc: 主循环结尾写 status
- 板端测试: curl `/api/status` `/api/devices`

### 阶段 2: MPP H.264 编码 + fMP4 muxer（2-3 天）
- mpp_encoder.{h,cc}: NV12 DMA-BUF → H.264 NAL
- fmp4_muxer.{h,cc}: NAL → fragmented MP4
- 测试: 编码 1 帧 → ffmpeg 验证

### 阶段 3: WebSocket server + 浏览器 MSE（2 天）
- ws_server.{h,cc}: cpp-httplib WS handler
- CameraPage [index.html:76-84](../CameraPage/index.html#L76-L84): placeholder → `<video>` + MSE

### 阶段 4: CameraPage 4 模块改造（2 天）
- 删所有 setInterval 假数据生成
- 算法管理模块隐藏
- 网络配置改真实数据
- devices 数组改 fetch

### 阶段 5: 测试 + 文档（1 天）
- 端到端测试
- 写 `docs/CAMERA_PAGE_INTEGRATION.md`（本文档）

## 4. 验收标准

| 项 | 标准 |
|---|---|
| 板端进程 | `./image-stitching` 启动后 < 1s HTTP 8080 可访问 |
| 视频流延迟 | < 300ms |
| 视频流质量 | 4613×3888 H.264@30fps, 15-20 Mbps |
| HTTP API 响应 | `/api/status` < 5ms |
| 状态刷新 | ≥ 5Hz |
| 配置热更新 | POST 后 1s 内生效 |
| 内存 | < 200MB |
| CPU (不含 stitch) | < 1 核 |

## 5. 风险与缓解

| 风险 | 等级 | 缓解 |
|---|---|---|
| MPP API 学习曲线陡 | 中 | 参考 rockchip MPP 文档，先用 mpp_enc_test 命令行工具验证 |
| fMP4 muxer 实现复杂度 | 中 | 手工拼 mp4 box；或引 libmp4 |
| 浏览器 MSE 4K 解码 | 低 | Chrome/Edge 软解 1-2 核；Safari 需硬解 |
| 4K H.264 带宽 20 Mbps | 低 | 千兆 LAN 满载 1/50 |
| WS 单客户端瓶颈 | 低 | v1 单客户端设计 |
| **stitch loop 被 WS send 阻塞** | 🔴 严重 | ring buffer (`/dev/shm/stitch_stream`) + try_push 失败丢旧帧 |
| **OpenCL → MPP 异步未同步** | 🔴 严重 | `clEnqueueBarrierWithWaitList` 强制 GPU 完成 |
| **MPP stride 必须 16 字节对齐** | 🔴 严重 | OpenCL 输出时手动指定 `(W+15)&~15` 对齐 stride |
| **yaml 热更新机制缺失** | 🔴 严重 | stitch loop 1s 轮询 mtime，POST 后原子 rename 替换 |
| yaml 写入并发 | 🔴 严重 | 写临时文件 + `rename()` 原子替换 |
| cpp-httplib FetchContent 离线编译失败 | 🟡 重要 | **vendor 单 header 进项目**（`third_party/cpp-httplib/httplib.h`），不依赖网络 |
| WS 大消息 (>500KB) 拆分 | 🟡 重要 | cpp-httplib 测试；必要时拆 ~64KB 小包 |
| JSON 序列化 CPU 浪费 | 🟡 重要 | stitch 端预先 serialize，handler 直接返回缓存 |
| status struct torn read | 🟡 重要 | 序列号机制：stitch atomic_inc 前后，handler 等成对版本才读 |
| OpenCL 输出 buffer 读写冲突 | 🟡 重要 | 双 buffer (A/B 交替) |
| 退出时 shm 泄漏 | 🟡 重要 | SIGINT handler + `shm_unlink`；shm 名加 PID 防多实例冲突 |
| 多客户端并发 | 🟡 重要 | v1 单客户端，第 2 个连接直接 close |
| Logger 多线程安全 | 🟢 一般 | 验证 Logger 源码；必要时加 mutex |
| 浏览器 autoplay 限制 | 🟢 一般 | `<video autoplay muted playsinline>` 三件套 |
| ufw 防火墙 | 🟢 一般 | `sudo ufw allow 8080/tcp` |
| IE 兼容性 | 🟢 一般 | 不支持 IE（明确文档说明） |

## 6. 实施细节补充（关键补丁）

### 6.1 状态桥接：序列号 + 双 buffer 防 torn read

```cpp
// /dev/shm/stitch_status_v1 (POSIX shm, 含 PID 后缀避免多实例冲突)
struct ShmLayout {
    std::atomic<uint32_t> version_begin;  // 序列号起始
    StitchStatus          status;
    std::atomic<uint32_t> version_end;    // 序列号结束
};

// stitch 端写:
version_begin.fetch_add(1);  // 标记开始写
// ... 逐字段 atomic store ...
version_end.fetch_add(1);    // 标记结束

// HTTP handler 读:
do {
    v1 = version_begin.load();
    local = memcpy(status);
    v2 = version_end.load();
} while (v1 != v2);  // 不一致就 retry
```

### 6.2 视频流：双 buffer + ring buffer

```cpp
// OpenCL 输出双 buffer (A/B 交替)
// panorama_buf[0], panorama_buf[1]

// 每帧:
// 1. OpenCL blend → 当前 panorama_buf[i&1]
// 2. clEnqueueBarrierWithWaitList 同步
// 3. mpp_enc.put_packet(panorama_buf[i&1].fd)
// 4. mpp_enc.dequeue_packet() → H.264 NAL
// 5. fmp4_muxer.push(nal) → fragment

// /dev/shm/stitch_stream (mmap ring buffer, 10 帧深)
// stitch 端: try_push(fragment), 满则 pop + drop 旧帧
// WS handler: pop fragment → ws.send()

// mutex + condvar 保护 ring buffer
```

### 6.3 NV12 stride 对齐

```cpp
// panorama width = 4613, height = 3888
// MPP 要求 stride 16 字节对齐
const int aligned_stride = (panorama_w + 15) & ~15;  // 4624

// OpenCL 输出时:
// - Y plane:   stride_w = aligned_stride, stride_h = panorama_h
// - UV plane:  stride_w = aligned_stride, stride_h = panorama_h / 2
// OpenCL kernel 输出 buffer 用 aligned_stride 分配

// mpp_frame_set_stride(frame, aligned_stride, panorama_h);
```

### 6.4 yaml 热更新

```cpp
// /home/rocktech/image-stitching/params/roi_tuning.yaml
//   轮询 thread:
std::thread poll_yaml([]() {
    time_t last_mtime = 0;
    while (running) {
        struct stat st;
        stat("params/roi_tuning.yaml", &st);
        if (st.st_mtime != last_mtime) {
            last_mtime = st.st_mtime;
            g_stitch_loop.reload_roi_config();  // 重新读 yaml
        }
        sleep(1);
    }
});

// POST /api/roi 处理:
// 1. 写 /tmp/roi_tuning.yaml.tmp
// 2. rename() 原子替换原文件 (POSIX 保证)
// 3. stitch poll thread 1s 内检测到 mtime 变化
```

### 6.5 cpp-httplib vendor 方案

```cmake
# CMakeLists.txt
# 不走 FetchContent, 直接 vendor
set(CPPHTTPLIB_DIR ${CMAKE_SOURCE_DIR}/third_party/cpp-httplib)
add_library(httplib STATIC ${CPPHTTPLIB_DIR}/httplib.cpp)
target_include_directories(httplib PUBLIC ${CPPHTTPLIB_DIR})
target_compile_definitions(httplib PUBLIC CPPHTTPLIB_THREAD_POOL=1)
target_link_libraries(image-stitching PRIVATE httplib)
```

```bash
# vendor 步骤:
mkdir -p third_party/cpp-httplib
curl -L https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.15.3/httplib.h -o third_party/cpp-httplib/httplib.h
curl -L https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.15.3/httplib.cpp -o third_party/cpp-httplib/httplib.cpp
```

### 6.6 CameraPage 静态文件服务

```cpp
// http_server.cc
void setup_static_routes(httplib::Server& svr) {
    // 显式路由 + MIME type (避免 set_mount_point 的路径推断问题)
    svr.Get("/", [](auto& req, auto& res) {
        res.set_content(read_file("./CameraPage/index.html"), "text/html; charset=utf-8");
    });
    svr.Get("/index.html", [](auto& req, auto& res) {
        res.set_content(read_file("./CameraPage/index.html"), "text/html; charset=utf-8");
    });
    svr.Get("/css/style.css", [](auto& req, auto& res) {
        res.set_content(read_file("./CameraPage/css/style.css"), "text/css; charset=utf-8");
    });
    svr.Get("/js/app.js", [](auto& req, auto& res) {
        res.set_content(read_file("./CameraPage/js/app.js"), "application/javascript; charset=utf-8");
    });
}
```

### 6.7 WS 单客户端限制

```cpp
// ws_server.cc
std::atomic<bool> has_active_client{false};

svr.set_ws_handler("/ws/preview", [](auto& conn) {
    bool expected = false;
    if (!has_active_client.compare_exchange_strong(expected, true)) {
        // 已有客户端, 拒绝新连接
        Logger::GetInstance().Log("[WS] Reject new client, already has viewer");
        return;  // cpp-httplib 自动 close
    }
    
    // 发送 init segment
    send_init_segment(conn);
    
    // 注册到 ring buffer 的 sink 列表
    ...
    
    conn.on_close = [](auto&) {
        has_active_client.store(false);
        ...
    };
});
```

## 7. 修订后的工作量估算

| 阶段 | 工作量 | 关键补丁 |
|---|---|---|
| 阶段 1: HTTP + 状态桥接 | 3-4 天 | vendor cpp-httplib + 序列号机制 + json 预序列化 |
| 阶段 2: MPP + fMP4 | 3-4 天 | 双 buffer + clEnqueueBarrier + stride 对齐 + WS 大消息测试 |
| 阶段 3: WebSocket + MSE | 2-3 天 | 单客户端限制 + init segment 重发 + ring buffer |
| 阶段 4: CameraPage 改造 | 2 天 | 不变 |
| 阶段 5: 测试 + 文档 | 1-2 天 | 多坑位测试 (autoplay / 多标签 / 断网重连) |
| **合计** | **11-15 天** | (比上一版多 2-4 天, 因为更多细节) |

## 6. 待扩展（v2+）

- 多客户端 (WS broadcast + 每客户端 ring buffer)
- RTSP 输出 (ffmpeg 拉 H.264 → RTSP server)
- H.265 (带宽再降一半)
- HTTP 鉴权 (login + JWT)
- 配置文件 Web 编辑
- 录像回放
- 算法扩展 (ONNX/RKNN 6 路推理)