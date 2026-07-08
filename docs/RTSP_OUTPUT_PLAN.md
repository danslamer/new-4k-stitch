# 拼接输出 RTSP 服务方案 (v3.1)

> **日期**: 2026-07-08 | **状态**: 📋 设计阶段 (待评审)
> **配套**: 输入侧见 [NETWORK_CAMERA_PLAN.md](NETWORK_CAMERA_PLAN.md) (v3.0, 已 Sprint 0/1/2)
> **目标**: 将 6 路 IP camera → 2×3 拼接后的全景画面，通过 RTSP 服务推到开发板 8554 端口，让后端服务器拉流

---

## 0. 背景 & 目标

### 0.1 现状 (v3.0 收尾)

stitch loop 每 33ms 产出一帧 NV12 (DMA-BUF backed, `output_drm_buf_`)，目前消费方只有：

- **SDL2 可视化窗口** (`g_enable_visual_tuning=1` 时打开)
- **CameraPage**（http_server 取 status JSON，不消费帧）
- **`SaveImage` 落盘 PNG**（`g_config.save_enabled`）
- **DRM 全屏输出**（若 enable）

**没有任何一路把这帧 NV12 推上网络**。需求方提的服务器端拉流，目前需要额外工具（如 ffmpeg 命令行录屏 + RTSP 重推），链路脆弱，不可控。

### 0.2 改动驱动

1. **后端 NVR/客户端拉流**: 服务器从开发板直接 `ffplay rtsp://<board>/...` 看全景，**不要**在板子上跑 ffmpeg 命令行、不导出 HLS/文件
2. **零拷贝延续**: 板内 stitch loop 已经把帧落到 DMA-BUF，输出 RTSP 的理想路径是**appsrc + DMA-BUF → mpp H.264 encoder → rtph264pay**，中间不让帧回 CPU
3. **架构硬约束不变**: 仍是 gstreamer1.0-rockchip1 唯一 RK HW 编解码路径，禁止引入非 vendor 解码器；但**编码器侧 mpph264enc 仍走 vendor 路径**，符合约束
4. **依赖输入 v3.0**: 假设 v3.0 Sprint 0/1 已 PASS（6 路 RTSP → 6 路 DMA-BUF → 2×3 拼接 ≥ 25 fps 跑通）

### 0.3 目标

| # | 目标 | 度量 |
|---|---|---|
| **O1** | 开发板跑 1 个 RTSP server，拉流即可看到全景 | `ffplay rtsp://<board>:8554/stitch` 出图 |
| **O2** | 主码流 1920×1080 @ 30fps H.264，码率 4 Mbps | `mediainfo` 看 `Duration`/`Frame rate`/`Bit rate` |
| **O3** | 编码走零拷贝：stitch → appsrc → mpph264enc 全部 DMA-BUF | `gst-launch-1.0 ... ! fakesink` dump pipeline trace，看 `dmabuf` |
| **O4** | 多个客户端同时拉，互不影响 | 2 个 `ffplay` 并行，画面不卡顿，stitch FPS 不掉 |
| **O5** | 性能：完整拼接 + 输出 30fps 端到端 | `STITCH_PERF_FPS` 日志 ≥ 30 |
| **O6** | 服务器端 vlc/ffmpeg 主流播放器都拉得到 | 默认 URL 走 RFC 2326 RTSP，无私有扩展 |
| **O7** | 可选子码流 1280×720 @ 15fps 给预览 | 同一 server mount 两个 factory |

### 0.4 不做（v3.1 范围外）

- ❌ HLS / WebRTC / SRT：另立项
- ❌ RTSP over HTTPS / DTLS / SRTP：v3.2+ 安全加密阶段
- ❌ 多客户端鉴权（账号密码 / token）：v3.2+；v3.1 仅 IP 白名单 + 单 basic 账密
- ❌ 录像落盘（独立 mp4）：v3.3，再起一个 `save_*` factory 同时存 + 推
- ❌ 双向音频：项目无音频需求

---

## 1. 硬约束 (从 v3.0 继承, 不破坏)

| 层 | 必须用 | 不接受 |
|---|---|---|
| **输入解码** | gstreamer1.0-rockchip1 mppvideodec `dma-feature=true` | sw decoder、FFmpeg rkmpp |
| **输出编码** | **gstreamer1.0-rockchip1 mpph264enc (HW)** 主选，`x264enc` (sw) 兜底 | h264_rkmpp (vendor 维护不周, segfault)、自写 x264 命令行 |
| **零拷贝** | DMA-BUF 贯穿 stitch → appsrc → encoder | memcpy 到 system memory (NV12→BGR 再回 NV12) |
| **RTSP 协议** | RFC 2326 主流兼容（VLC/ffmpeg/ffplay/ONVIF 客户端） | gstreamer 私有 RTCP 扩展 |
| **板上资源** | 单进程 image-stitching 内部集成 RTSP server | 单独 ffmpeg/rtsp-server 进程 |

**禁止 SW fallback 到 stitch 主路径**：v3.0 已锁定，本方案不引入 SW 解码；H.264 输出侧只在 mpph264enc 不可用时才走 sw，且 yaml `output.encoder: h264_sw` 显式声明，**不影响** stitch 输入。

---

## 2. 库 & 元素选型

### 2.1 RTSP 服务器：`gstreamer1.0-rtsp-server-1.0`

| 项 | 选 | 备选 | 备注 |
|---|---|---|---|
| 协议库 | **`gstreamer1.0-rtsp-server-1.0`** | live555 / 自实现 | gst-rtsp-server 是 GStreamer 官方子项目；Rockchip BSP 镜像 (`gstreamer1.0-rtsp-server`) 默认装；API 简洁，2-3 小时接入 |
| 认证 | `GstRTSPAuth` + `GstRTSPAuthToken` (basic) | 自定义 | basic 够用（v3.1），token/Digest 后续 |
| 端口 | **8554** | 8555 / 自定 | 8554 是 RFC 默认；与 CameraPage 8080 不冲突 |
| mount point | **`/stitch`** 主码流 + **`/stitch_sub`** 子码流 | 单码流 | 双 mount 由 yaml 决定是否启用子码流 |
| 多客户端 | gst-rtsp-server 原生支持 (per-client media factory) | — | 每客户端 1 份 pipeline 实例，gst-rtsp-server 内部 lazy-spinup，0 客户端时不开 |

**依赖**：`apt install -y gstreamer1.0-rtsp-server-1.0 libgstrtspserver-1.0-dev` (开发包可选，纯动态链接不要 dev 也行)

### 2.2 编码器：`mpph264enc` 主选，`x264enc` 兜底

| 候选 | 路径 | 板端验证状态 (2026-07-08) | 风险 |
|---|---|---|---|
| **mpph264enc (gstreamer1.0-rockchip1)** | `mpph264enc` element, 用 librockchip_mpp 旧 mpp_service 接口 | 解码侧 `mppvideodec` 已跑通 (Sprint 0 PASS, 6 路 100+ fps), 同一组驱动/固件可推断编码也 OK | 编码器参数比 sw 少, `tune`/`preset` 等不可设；对参数敏感（如 `bframes=0` 是 H.264 baseline 必备） |
| `x264enc` (sw) | libx264 主线, 板上 `apt install x264` + `apt install gstreamer1.0-plugins-ugly` 拿到 | HISTORY §踩过的坑: "板上 RK 自定义 ffmpeg 只 --enable-rkmpp 不带 sw encoder. 数据集转码要么 sudo apt install x264 装 CLI". 类似故事：gst 系统包一般带 `x264enc`（libx264 是 gstreamer1.0-plugins-ugly 的依赖） | 2K@30 sw x264 大概率吃满 4×A76 (RGA + sw x264 同时跑, 帧时间可能被合并超 33ms) |
| `vp8enc` / `vp9enc` (sw) | 系统 gst-plugins-good | — | 兼容性更好但非 H.264，主流 RTSP 客户端拉流更广 |

**决策矩阵**：

| 场景 | 默认 | 切 sw 触发条件 |
|---|---|---|
| 输出 1920×1080 @ 30fps (主码流) | **mpph264enc** | first init 失败 / 5s 出 0 帧 → 自动切 x264enc |
| 输出 1280×720 @ 15fps (子码流) | **mpph264enc** (降码率) | 同上 |
| 输出 4560×2540 @ 30fps (全景原始尺寸) | **mpph264enc** 试；失败回落到 1920×1080 | RK3588 VPU 可扛，RK3576 视板端 VPU 固件 |

**为什么 SW 可作兜底而不是首选**：libx264 sw 单帧 ~20-30ms@1080p on aarch64，与 stitch ~30ms 串行 → 帧时间 ≥ 60ms（≤ 16 fps）。HW 编码器 ~5ms 8K@30 没问题。两路都跑要保证总时间 ≤ 33ms，只能二选一（HW）或分两条 stream 限速（HW）。**SW 只能兜底不能用默认**。

**yaml 配置**：
```yaml
output:
  encoder: h264_hw          # h264_hw | h264_sw
  fallback_to_sw: true      # HW init 失败自动切
```

### 2.3 码率控制

| 项 | 主码流默认 | 子码流默认 | yaml 字段 |
|---|---|---|---|
| 分辨率 | 1920×1080 | 1280×720 | `output.main.width/height`, `output.sub.width/height` |
| 帧率 | 30 fps | 15 fps | `output.main.fps`, `output.sub.fps` |
| H.264 profile | Baseline (兼容广) | Baseline | `output.profile` (默认 `baseline`) |
| 码率 | 4 Mbps (1080p30) | 1 Mbps (720p15) | `output.main.bitrate_kbps`, `output.sub.bitrate_kbps` |
| GOP | 60 (=2s @ 30fps) | 30 (=2s @ 15fps) | `output.gop` |
| B-frame | 0 (baseline 限制) | 0 | `output.bframes` |
| rate-control | CBR | CBR | `output.rc_mode` |
| 切片 | 1（编码 1 slice, 板子 4×A76 足够）| 1 | `output.slices` |

**mpph264enc 对应 gst property**（待 v3.1 Phase B 实机验证）：
```
mpph264enc:
  g_object_set(enc, "bps", bitrate_kbps * 1000, NULL);     // bps in bps
  g_object_set(enc, "gop", gop, NULL);
  g_object_set(enc, "rc-mode", "cbr", NULL);
```

---

## 3. 架构

### 3.1 数据流 (mermaid)

```mermaid
flowchart LR
    subgraph 板端 single_process["开发板 (单进程 image-stitching)"]
        direction LR
        INPUT["6× rtspsrc → mppvideodec (DMA-BUF)"] --> STITCH["RGA 裁剪 → OpenCL 接缝羽化 → output_drm_buf_ (NV12 DMA-BUF)"]
        STITCH --> SCALER_MAIN["RGA 缩放 主码流 1920×1080"]
        STITCH --> SCALER_SUB["RGA 缩放 子码流 1280×720"]
        SCALER_MAIN --> APPSRC_MAIN["appsrc_main (per-client)"]
        SCALER_SUB --> APPSRC_SUB["appsrc_sub (per-client)"]
        APPSRC_MAIN --> ENC_MAIN["mpph264enc / x264enc"]
        APPSRC_SUB --> ENC_SUB["mpph264enc / x264enc"]
        ENC_MAIN --> PAY_MAIN["rtph264pay"]
        ENC_SUB --> PAY_SUB["rtph264pay"]
        PAY_MAIN --> RTSP_SVR["gst-rtsp-server"]
        PAY_SUB --> RTSP_SVR
    end
    RTSP_SVR -- "rtsp://board:8554/stitch" --> CLIENT1["服务器端 VLC / ffplay / NVR"]
    RTSP_SVR -- "rtsp://board:8554/stitch_sub" --> CLIENT2["同上层预览"]
```

### 3.2 单 stitch loop + 双 stream 路线

**为什么不用单独的输出线程**：

- stitch loop 已经 30fps 节奏严格
- 编码放在 stitch loop 同步路径上，背压立即可见（appsrc `max-buffers=1` + `drop=true`）
- 避免再开线程 + ring buffer + 同步状态机

**RGA 二次缩放**：

- stitch 输出 NV12 = `4560×2540` (估算, 见 v3.0 §0.6)
- 主码流 1920×1080：通过 RGA 一次性 resize（lwir2 或 RGA2 硬件）
- 子码流 1280×720：再起一次 RGA 或共用主码流再 downsample
- **两路编码不互阻塞**：每个 mount point 一个独立的 `appsrc + encoder + pay` 子图，由 gst-rtsp-server 在新客户端到来时 spinup

**stitch loop 与 appsrc 之间的桥梁**：

- 在 stitch 完成后，对每路 output stream 调一次 `appsrc_main.push_buffer(NV12 frame DMA-BUF)`
- appsrc 内部 lazy-alloc 一份 GstBuffer 引用 DMA-BUF fd（`gst_buffer_new_wrapped_full` + `gst_dmabuf_memory_alloc` 或者 gstreamer1.20 新 API `gst_buffer_append_memory` + 自定义 dmabuf allocator）
- 实际选型见 §5.3 DMA-BUF 集成细节

### 3.3 gst-rtsp-server media factory 模式

```cpp
// pseudo
GstRTSPServer* server = gst_rtsp_server_new();
gst_rtsp_server_set_service(server, 8554);

GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(server);

// 主码流 mount point
GstRTSPMediaFactory* main_factory = gst_rtsp_media_factory_new();
gst_rtsp_media_factory_set_launch(main_factory, build_main_pipeline_string());
gst_rtsp_mount_points_add_factory(mounts, "/stitch", main_factory);

// 子码流 mount point
GstRTSP… // 同上, "/stitch_sub"

// 服务器跑自己的 main loop, 或 attach 到现有 glib main loop
gst_rtsp_server_attach(server, NULL);   // NULL = 默认 glib main loop
```

每客户端连接时 gst-rtsp-server 自动调用 factory 的 `media-constructed` 回调，构造一个独立的 GstRTSPMedia（即独立 pipeline 实例）。各客户端独立，互不影响。appsrc 由 factory 创建后 attach 到 `media`，frame 入口是 callback 形式（回调里从 stitch 主循环的 ring buffer 拉新帧）。

---

## 4. 配置 / yaml schema

扩展 `params/camera_sources.yaml` 顶层加一个 `output` 块（与现有 `sync_window_ms` 平级），或新建独立 `params/output_streams.yaml`。本方案选**扩展原 yaml**（输入输出同一文件，开销小）。

```yaml
# ===== v3.1 (2026-07-08) 新增 ===== 保留 v3.0 输入块不动 =====

sync_window_ms: 16
auto_calibrate: 0

cameras:  # v3.0 已实现, 不动
  - type: rtsp
    uri: rtsp://admin:CHANGE_ME@192.168.10.21:554/Streaming/Channels/101
    ...

# ===== v3.1 新增: 输出 =====
output:
  enabled: true                    # false = 完全不起 RTSP server (省 CPU)
  bind_address: 0.0.0.0            # listen ip
  port: 8554                       # RTSP port
  auth:                            # v3.1 单 basic; v3.2+ JWT/Digest
    enabled: false
    user: admin
    pass: CHANGE_ME
  ip_allowlist: []                 # 空 = 全部允许; ['192.168.10.0/24'] 等限制
  on_demand: true                  # true = 仅在客户端连接时启动 encoder pipeline (默认)
                                   # false = 不论有无 client 都常开 (always-on, 适合 NVR)
  encoder: h264_hw                 # h264_hw | h264_sw
  fallback_to_sw: true             # hw 不可用自动回落到 sw
  profile: baseline                # baseline | main | high
  gop: 60                          # 帧
  rc_mode: cbr                     # cbr | vbr | cqp
  slices: 1                        # 切片数 (低端板子增加)
  bframes: 0                       # 0 = baseline 兼容
  streams:
    - path: /stitch                # 主码流 mount point
      enabled: true
      width: 1920
      height: 1080
      fps: 30
      bitrate_kbps: 4000           # 4 Mbps
      rate_cap:
        enabled: true
        max_fps: 30                # 客户端频次上限 (drop 超过部分)

    - path: /stitch_sub            # 子码流 (可关)
      enabled: true
      width: 1280
      height: 720
      fps: 15
      bitrate_kbps: 1000           # 1 Mbps
```

**C++ struct 镜像**（`include/output_config.h`）：

```cpp
struct OutputStreamConfig {
    bool   enabled   = true;
    std::string path  = "/stitch";      // URL path
    int    width     = 1920;
    int    height    = 1080;
    int    fps       = 30;
    int    bitrate_kbps = 4000;
};

struct OutputConfig {
    bool   enabled        = true;
    std::string bind_address = "0.0.0.0";
    int    port           = 8554;
    bool   auth_enabled   = false;
    std::string auth_user = "admin";
    std::string auth_pass = "";
    std::vector<std::string> ip_allowlist;
    bool   on_demand      = true;
    std::string encoder   = "h264_hw";    // h264_hw | h264_sw
    bool   fallback_to_sw = true;
    std::string profile   = "baseline";
    int    gop            = 60;
    std::string rc_mode   = "cbr";
    int    slices         = 1;
    int    bframes        = 0;
    std::vector<OutputStreamConfig> streams;
};
```

`include/output_config.h` 还提供 `OutputConfig LoadFromYaml(const std::string& path)` 与 `SaveToFile` (与现有 `roi_config.h` 同模式)。

---

## 5. appsrc 集成与零拷贝路径

### 5.1 stitch loop 端 (生产者侧) 调用约定

```cpp
// 在 stitch loop 的 stitch 完成后追加一段:
if (output_server_.enabled()) {
    for (auto& stream : output_server_.streams()) {
        // 1. 从 output_drm_buf_ (4560×2540) 缩放到 stream.width/height
        DrmBuffer scaled;
        rga_resize_nv12(output_drm_buf_,                // src
                        scaled,                          // dst (RGA 分配)
                        stream.width, stream.height);
        // 2. push 给对应 stream 的 gst_rtsp_server
        output_server_.push_frame(stream.path, &scaled);
    }
}
```

**RGA 缩放复用现有 `drm_allocator` + RGA2 API**（已有 `EnsureScratchBuffer` 模式）。对每个 stream 各自 alloc 一块 scratch drm buffer，缓存复用（同 dma_buf_cache 思路）。详见 §5.3 DMA-BUF details。

### 5.2 gst-rtsp-server 端 (消费者侧) per-client pipeline

```cpp
// build_main_pipeline_string() 返回给 factory.launch:
//   appsrc name=src is-live=true format=NV12 width=1920 height=1080 framerate=30/1 \
//     ! videoconvert ! video/x-raw,format=I420 \
//     ! mpph264enc bps=4000000 gop=60 rc-mode=cbr \
//     ! h264parse \
//     ! rtph264pay name=pay0 pt=96 config-interval=1
```

**关键 property**：

| appsrc 参数 | 值 | 理由 |
|---|---|---|
| `is-live` | TRUE | RTSP 是 live stream，禁用 buffer 自动 resync |
| `do-timestamp` | TRUE | 不用 sink 的 clock，自己用 frame 出图时间打 PTS |
| `format` | `time` | time-based，而非 byte-based |
| `max-bytes` | 1 | 1 帧 buffer（不要等几帧再送） |
| `min-percent` | 100 | 一直等到 appsrc 真的送出再读下一帧，避免 build-up |
| `block` | FALSE | 异步 push，不阻塞 stitch 线程 |
| `drop` | TRUE | 上游 push 超 max-bytes 时自动 drop（旧帧不再用） |

| videoconvert 参数 | 值 | 理由 |
|---|---|---|
| 输入 caps | `video/x-raw(memory:DMABuf),format=NV12,w={W},h={H},stride={stride_w}` | 必须是 DMABuf 零拷贝 |
| 输出 caps | `video/x-raw,format=I420` | mpp H.264 编码器接受 I420（也支持 NV12，但 I420 更通用） |

> 注：板上实测 mpph264enc 是否直接吃 NV12 DMA-BUF，v3.1 Phase B 第一步就要验证。如果可以直接吃 → 砍掉 `videoconvert`，整条 0 CPU copy。

| mpph264enc 参数 | 值 | 备注 |
|---|---|---|
| `bps` | `bitrate_kbps * 1000` | gstreamer 是 bps，配置是 kbps |
| `gop` | yaml `gop` | — |
| `rc-mode` | `cbr` / `vbr` / `cqp` | 需验证 gst-rockchip1 接受 `cbr` 字串 |
| `qp-init` | -1 (auto) | 仅 cqp 用 |

| rtph264pay 参数 | 值 | 理由 |
|---|---|---|
| `pt` | 96 | 动态 payload type |
| `config-interval` | 1 | 每个 IDR 都送 SPS/PTS（重连容错好） |

### 5.3 DMA-BUF 跨 process/GstBuffer 引用

关键问题：stitch 输出 `output_drm_buf_` (DRM allocated). 如何把它的 fd 喂给 gst-rtsp-server 的 appsrc？

**方案 A：gst-allocators approach**（推荐）

```cpp
// 1. 用 gst_dmabuf_allocator_new() 注册一个 GstAllocator, 名字 "dmabuf-rockemb"
//    或复用现有的 "dmabuf" allocator (gstreamer1.0-dma-buf-support 插件提供)
// 2. appsrc caps 限定: video/x-raw(memory:DMABuf)
// 3. stitch 主线程 alloc GstBuffer, 把 drm_buf fd wrap 到 GstBuffer 的 GstMemory 中
//    gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY,
//                                drm_buf.mapped_ptr, drm_buf.size,
//                                0, drm_buf.size,
//                                NULL, drm_buf_close_notify)
//    gst_buffer_add_memory(buf, gst_dmabuf_memory_new(allocator, drm_buf.fd));
// 4. appsrc_push_buffer(buf)  （appsrc 引用计数, 引用消失时调 close_notify）
```

**方案 B：gst_buffer_new_allocate + memcpy**（应急）

如果 DMA-BUF 入 GstBuffer 卡住，先 memcpy → CPU mem，验证整链通路，再切到 A。**Phase B 第一天应急方案**。

**`drm_buf_close_notify`** 何时触发：

- gst-rtsp-server 在 client `TEARDOWN` 时释放 pipeline，appsrc 释放，buffer 引用计数 → 0 → 触发 notify
- notify 内部调用 **`drm_buf.ref_count--`**，不 unmap（同一块 drm buffer 由 stitch 持续复用，只是 refcount 控制生命周期）
- 关键不变量：**drm_buf 在 stitch 主循环内 alloc，在 gst-rtsp-server 释放后 refcount 回到 0，不重复 alloc**（省 DRM ioctl）

### 5.4 用户可见的 server 操作

```bash
# 1. 启动 image-stitching
INPUT_SOURCE_MODE=camera ./image-stitching

# 2. 服务器端 (或同板子上 vlc) 拉流
ffplay rtsp://board-ip:8554/stitch                # 主码流
ffplay rtsp://board-ip:8554/stitch_sub            # 子码流

# 3. 板端观察
journalctl -u image-stitching | grep rtsp_server
# 看到 [rtsp_server] client c1 connected, stream=main, pipeline=appsrc...
#      [rtsp_server] client c1 disconnected

# 4. 完全关闭 RTSP server
#    yaml: output.enabled: false (不需要重编)
```

### 5.5 watchdog & 资源回收

| 故障 | 行为 |
|---|---|
| 客户端断网 | gst-rtsp-server 标准 timeout，pipeline 自动销毁，appsrc 释放 |
| 板子断网（拉流方）| 服务端继续监听，重连后自动续上（appsrc 一直保持 preroll + EOS-then-restart 模式）|
| 单客户端长时间拉流 + stitch 卡死 | appsrc push 阻塞 / timeout 30s，server 关 client；其他客户端不受影响 |
| encoder init 失败 | 切 sw 回退；都失败 → 该 stream 不挂载，**log `[rtsp_server] stream /stitch init failed: encoder unavailable`**，其他 stream 不影响 |
| stitch loop 死锁 | `STITCH_PERF_FPS=0` 持续 5s → application watchdog kill server pipeline（与 v3.0 §5 watchdog 一致）|

---

## 6. URL & API 设计

### 6.1 服务器端拉流 URL

| 用途 | URL | 默认码流 |
|---|---|---|
| 主码流（全景） | `rtsp://<board-ip>:8554/stitch` | 1920×1080@30 H.264 4Mbps |
| 子码流（预览） | `rtsp://<board-ip>:8554/stitch_sub` | 1280×720@15 H.264 1Mbps |
| 鉴权示例 | `rtsp://admin:CHANGE_ME@<board-ip>:8554/stitch` | yaml `auth.enabled=true` 时强制 |

**`/stitch` 路径对齐** IPC 主流命名 (`/video1` `/live/0`) 给后端一个清晰锚点。

### 6.2 板上 CameraPage 接入（额外增强，v3.1 顺手做）

`status_writer.cc` 扩展：

```json
{
  "output_rtsp": {
    "enabled": true,
    "bind": "0.0.0.0:8554",
    "active_streams": [
      {"path": "/stitch", "width": 1920, "height": 1080, "fps": 30,
       "bitrate_kbps": 4000, "encoder": "h264_hw", "clients": 1},
      {"path": "/stitch_sub", "width": 1280, "height": 720, "fps": 15,
       "bitrate_kbps": 1000, "encoder": "h264_hw", "clients": 0}
    ]
  }
}
```

CameraPage 后期可拉一个 live `<video>` 标签直接显示拉到的 RTSP（实际上还得靠 CameraPage 前端接 WebSocket/MSE，跟 §9 § CAM 方案对接，v3.1 后再迭代）。

### 6.3 服务端拉到的内容回显客户端

服务端 (NVR/分析) 拉 `rtsp://<ip>:8554/stitch` 看到的就是开发板上**已实时拼接好的全景**。这就满足需求："服务器端通过 rtsp 协议从开发板拉取拼接后的完整视频流"。

---

## 7. 性能预算

### 7.1 时间预算 (单帧)

| 阶段 | 时间 (目标, ms) | 备注 |
|---|---|---|
| 6 路 RTSP rtspsrc + jitter | (后台, 不占 stitch loop budget) | latency=120ms 是 rtsp 内部，与 stitch 不阻塞 |
| 6× mppvideodec 解码 | (后台, ring buffer) | 已 6 路 100+ fps 实测 |
| 6× RGA 裁剪 (stitch 输入) | ~10 ms | v2.5 已验 |
| OpenCL 羽化 (Mali GPU) | ~3 ms | v2.5 已验 |
| 1× RGA 缩放 4560→1920 | **~3 ms**（RGA2 估计） | RGA2 缩 1080p 4ms 以内，2K→1080p 估算 |
| 1× RGA 缩放 4560→1280 | ~3 ms | 同上 |
| mpph264enc 1920×1080@30 | **~5 ms**（HW）| RK3588 VPU 实测 8K@30 没问题 |
| mpph264enc 1280×720@15 | ~2 ms | 几乎免费 |
| **总和（主+子双码流）** | ~10 ms（仅新增）| 旧 stitch 已 ~13ms，加起来 ~23ms，**留 10ms 余量给采集端抖动** |
| **目标 stitch FPS** | ≥ 30（实测应该 ≥ 35） | `STITCH_PERF_FPS` 日志 |

### 7.2 CPU/GPU 负载

| 资源 | 占比（6 路解码 + 双码流 H.264 HW 编码） |
|---|---|
| 4× A76 | 30-40%（decode + 服务端 RTSP glue）|
| 2× A55 | 5%（rtspsrc 解包 + yaml） |
| GPU (Mali) | 5%（OpenCL blend, 几乎与编码无关） |
| VPU 编码 | 20-30%（mpph264enc busy）|
| 1GbE 网卡 | < 100 Mbps（output 码流 + 6 路 RTSP input）|

板端 (RK3588/RK3576) 1GbE 网卡理论上 1000Mbps 双向，单路 4Mbps 客户端 × 5 + input 6× 6Mbps = ~80 Mbps，远低于 1Gbps，余量充足。

### 7.3 内存

| 资源 | 估算 |
|---|---|
| output_drm_buf_ (4560×2540 NV12) | 4560×2540×1.5 ≈ 17 MB (独占 DMA-BUF) |
| 主码流 scratch (1920×1080 NV12) | ~3 MB |
| 子码流 scratch (1280×720 NV12) | ~1 MB |
| per-client pipeline (appsrc+enc+pay state) | ~2 MB × N 客户端 |
| 编码器内部缓冲 (B-frame/H.264 reference) | ~10 MB |
| **总计** | ~30 MB + 客户端数 × 2 MB |

板子普遍 4GB RAM，余量 99.9%。**没什么压力**。

---

## 8. 代码模块

### 8.1 新增模块

```
include/
  output_config.h                    # OutputConfig / OutputStreamConfig + Load/SaveFromYaml
src/
  gst_rtsp_server.cc                 # gst-rtsp-server 启动/管理/client 跟踪
  appsrc_bridge.cc                   # appsrc feed 逻辑 (DMA-BUF wrap, timestamp, drop)
  rga_scaler.cc                      # ★ 把 output_drm_buf_ 缩到目标分辨率 (复用 RGA2)

tools/
  output_smoke.sh                    # v3.1 Sprint A 一键验收 (类比 sprint0_smoke.sh)
  rtsp_url_probe.sh                  # 用 ffprobe + gst-launch 烟测单个 stream 输出 (类比现有 rtsp_url_probe.sh)
```

### 8.2 修改模块

| 文件 | 改动 |
|---|---|
| `CMakeLists.txt` | 链接 `gstreamer-1.0` (已有), `gstrtspserver-1.0` (新) |
| `src/app.cc` | (1) stitch 循环结尾 push 到 appsrc_bridge; (2) `main()` 启动 `gst_rtsp_server`; (3) `stitch_status::GlobalStatus` 加 `output_rtsp` 字段 |
| `src/status_writer.cc` | schema 加 `output_rtsp` 段 |
| `include/sensor_data_interface.h` | `GetCameraSourceList()` 旁增 `GetOutputConfig()` |
| `src/sensor_data_interface.cc` | `LoadOutputConfig()` 解析 yaml 输出块 |
| `include/logger.h` | 不变 |
| `params/camera_sources.yaml` | 顶层加 `output:` 块（默认 `/stitch` 主码流 disabled 等待 v3.1 启用）|

### 8.3 改动量估算

| 模块 | 行数（含注释） | 难度 |
|---|---|---|
| `gst_rtsp_server.cc` | ~250 | 中（gst-rtsp-server API 适配） |
| `appsrc_bridge.cc` | ~150 | 低（包装 GstBuffer 喂给 appsrc） |
| `rga_scaler.cc` | ~100 | 低（复用现有 RGA dma_buf_cache_） |
| `output_config.h` + yaml 解析 | ~150 | 低（类比 roi_config.h） |
| `tools/output_smoke.sh` | ~120 | 中（端到端验证） |
| `src/app.cc` 修改 | ~80 增量 | 中（接缝在 stitch loop 末尾） |
| **合计** | **~850 行** | |

工期估计：**2-3 工作日**（Phase A/B/C 各 1 天）。

---

## 9. Sprint 分期

> 与 v3.0 的 Sprint 0/1/2 并行或顺序？**建议顺序**：v3.0 Sprint 0 跑通后接 v3.1 Sprint A，避免同时调试两个端复杂度。

### Sprint A: 骨架贯通 (1 天)

**目标**: 输出端能起 server，能拉一帧过。

- [ ] `apt install gstreamer1.0-rtsp-server-1.0`
- [ ] `output_config.h` + yaml 解析
- [ ] `gst_rtsp_server.cc` 最小骨架：起 port 8554，挂 `/stitch`，pipeline=`appsrc ! videoconvert ! x264enc ! rtph264pay`（**先 sw**，HW 后续）
- [ ] `src/app.cc` stitch loop 末尾 push 一个 dummy frame
- [ ] `ffplay rtsp://<board>:8554/stitch` 拉得到流（即使 frame 质量差或常黑）

**验收**:
- [ ] 服务端 `ffprobe rtsp://...` 返回 SDT + media info
- [ ] 1 个客户端拉 5s 不掉线

### Sprint B: HW 编码器 + 双码流 (1.5 天)

**目标**: 走 mpph264enc 30fps，HW 双码流同开。

- [ ] sw → hw 切换：try mpph264enc，失败回 x264enc（自动 fallback）
- [ ] RGA 缩放 (4560 → 1920 + 4560 → 1280) 接进 stitch loop
- [ ] 双 mount points（`/stitch` + `/stitch_sub`）并发
- [ ] on_demand 模式：仅有 client 时启动 encoder pipeline

**验收**:
- [ ] `mediainfo` 显示 H.264 Baseline, 1920×1080 @ 30fps, 4Mbps ± 10%
- [ ] 2 个 ffplay 并行拉双码流，stitch FPS 不掉

### Sprint C: 实战验收 + 生产化 (0.5 天)

**目标**: 真实场景稳定跑 1 小时。

- [ ] `tools/output_smoke.sh` 一键验收（PASS/WARN/FAIL）
- [ ] 板上跑 1 小时 `output_smoke.sh` 监控 FPS、CPU、丢帧率
- [ ] CameraPage `/api/status` 加 `output_rtsp` 段 (CameraPage 字段规约已就位)
- [ ] 文档更新（AGENTS.md 引用本文件，HISTORY.md 加 v3.1 章节）

**验收**:
- [ ] `bash tools/output_smoke.sh` 全 PASS
- [ ] 拉流 1 小时，无掉线，stitch FPS ≥ 30 稳定
- [ ] AGENTS.md / HISTORY.md / CAMERA_PAGE_INTEGRATION.md 同步更新

### 总周期

| Sprint | 天 | 备注 |
|---|---|---|
| A | 1 | 骨架贯通 |
| B | 1.5 | HW 编码 + 双码流 |
| C | 0.5 | 验收 + 生产化 |
| **合计** | **3 工作日** | v3.0 Sprint 0 PASS 后接 |

---

## 10. 验收清单 (Definition of Done)

### 10.1 端到端 (E2E)

| 级 | 验证 | 方法 |
|---|---|---|
| **E2E-A** | 板起服务，1 个 VLC/ffplay 拉 `/stitch` 出图且持续 5 min | `ffplay rtsp://board-ip:8554/stitch` 出图, 不掉 |
| **E2E-B** | 两个客户端同时拉 `/stitch` + `/stitch_sub`，互不影响 | 2 个 ffplay 进程 |
| **E2E-C** | 客户端随机断网/重连，30s 内能续上 | 关 / 开 ffplay |
| **E2E-D** | 服务端 kill ffplay 后 stitch loop 继续 30fps | `STITCH_PERF_FPS` 日志 |
| **E2E-E** | 远程服务器 (`ffmpeg -i rtsp://... -t 30 -f null -`) 30s 录 0 错 | 端到端稳定跑 |

### 10.2 性能

| 指标 | 目标 | 实测方法 |
|---|---|---|
| 主码流帧率 | 30 fps ± 1 | `mediainfo --inform="Video;%FrameRate%"` 或 vlc 里看 |
| 单帧端到端 (stitch+resize+encode+pay) | ≤ 33ms | 板上 perf log |
| CPU 4×A76 总占用 (含输入解码) | ≤ 70% | `top -p $(pidof image-stitching)` |
| 网卡 outbound (单 client 拉主码流) | ≤ 5 Mbps | `sar -n DEV 1` 或 `iftop` |
| 多客户端 5 个并发 | 都流畅 | 5 个 ffplay 同时拉 |

### 10.3 资源

| 指标 | 目标 |
|---|---|
| 内存增长 (1h 跑) | < 50 MB（无泄漏）|
| DMA-BUF fd 累计 (单进程) | < 32 个 (gst shared pool)|
| stitch loop 在多 client 期间不掉帧 | `STITCH_PERF_FPS` 平稳 |

### 10.4 健壮性

| 场景 | 期望 |
|---|---|
| `output.enabled: false` (yaml) | service 不启动; stitch 流程不受影响 |
| `encoder: h264_sw` | 强制 sw；HW 报失败也不切换 |
| `on_demand: true` 且无 client | 不开 encoder pipeline, 仅保留 server listen |
| 客户端断开后 30s 重连 | 自动续上, 无缝 |
| 板子 reboot | `systemd` 拉起 image-stitching (yaml 持久化在 /etc) → service listen |
| RH 路径断电重启 | 30s 内恢复 RTSP 服务 |

---

## 11. 风险 & 缓解

| 风险 | 概率 | 缓解 |
|---|---|---|
| **R1**: 板上 `gstreamer1.0-rtsp-server-1.0` 包没装 | 高（rocktech 镜像可能未默认装）| Phase A 第 1 步先 `apt install`，文档固化进 `tools/post_flash_test.sh` 新增 "rtsp-server" 检查 |
| **R2**: `mpph264enc` 不可用 (VPU 固件缺失) | 中 | yaml `encoder: h264_sw` 切 sw；Sprint B 第一天调通 sw fallback 后再实验 hw |
| **R3**: `mpph264enc` 接受 NV12 DMA-BUF 而不需 `videoconvert` | 中 | Phase B 第一天先去掉 videoconvert 测 0 CPU copy；若 pipeline 协商失败保留 videoconvert |
| **R4**: RGA 缩放 4560→1920 抢带宽与 stitch warp 同 RGA | 中 | 复用 RGA2 不同子通道 (rk3588 RGA2 有 2-3 个独立引擎) 或 串行（两个阶段不冲突） |
| **R5**: appsrc 多客户端共享同一份 NV12 DMA-BUF，fd 引用计数对不上 | 中 | 引用计数法：buffer push 时 `gst_buffer_ref`，pipeline 用完 `gst_buffer_unref`；stitch 端保留 1 个 ref，client 那边 release 后才还 scratch；不走这条则 rga 缩放独立的 scratch buffer，stitch 完成后立刻释放 |
| **R6**: stitch 30fps + encoder 30fps，二者速率不一致导致 buffer build-up | 中 | appsrc `max-bytes=1 drop=true` 保证上限，编码器跟不上自动 drop stitch 输出 |
| **R7**: 多个客户端，每个人一份 encoder pipeline，CPU 撑不住 | 低（mpph264enc HW 几乎零 CPU）| 若真出现，gst-rtsp-server 有 shared pipeline 模式 (单 encoder + tee 分发给 N 个 pay0) |
| **R8**: 板子 (rk3588) 与客户期望的 RTSP 扩展不一致（如 ONVIF back-channel） | 低 | 默认 RFC 2326，足够；客户需要 ONVIF 单独 v3.3 立 |
| **R9**: 客户端用 ffprobe 拉不到 SDT / capabilities | 中 | 板子像 gst-rtsp-server 默认 options 一致就 OK；若不行手动 rtsp-server.conf 调 (`set_service` API) |
| **R10**: 拼接输出原始 4560×2540 时，HW encoder 吞吐够，但 L4 cache miss / DMA 带宽顶不住 | 低 | 默认 1920×1080 输出，已规避；如用户要 4560×2540，Sprint B 后期再做 |
| **R11**: vendor ffmpeg 包没装 libx264，apt 又因为无网装不上 | 中 | 历史 §踩坑: "vendor SDK ffmpeg 不带 libx264, 项目已用 ffmpeg + libx264 sw encoder". 同样故事：板子 gstreamer1.0-plugins-ugly 包依赖 libx264，跟 ffmpeg 独立，可装；Phase A 第一步验证 `gst-inspect-1.0 x264enc` |

---

## 12. 关键代码骨架

### 12.1 `gst_rtsp_server.cc` 接口 (草案)

```cpp
class GstRtspServer {
public:
    static GstRtspServer& GetInstance();

    bool Start(const OutputConfig& cfg,
               std::function<void(const std::string& path,
                                  const DrmBuffer& scaled_nv12)> frame_source);
    // frame_source 由 gst_rtsp_server 在 stitch loop 调用时回调, 传入 (path, scaled_drm_buf)
    //   - 例如 path="/stitch" → 1920×1080 scratch
    //   - 例如 path="/stitch_sub" → 1280×720 scratch

    void Stop();
    bool IsRunning() const;
    int ClientCount(const std::string& path) const;

private:
    GstRtspServer(); ~GstRtspServer();
    // 内部: glib main loop attach, factory 注册, per-client pipeline 维护
};
```

### 12.2 `appsrc_bridge.cc` 接口 (草案)

```cpp
class AppSrcBridge {
public:
    AppSrcBridge(const std::string& path, int width, int height, int fps,
                 GstElement* pipeline_root);  // pipeline_root = gst_rtsp_media 实例
    ~AppSrcBridge();

    void PushFrame(const DrmBuffer& nv12_drm, int64_t pts_us);
    // 内部:
    //   1. gst_buffer_new_wrapped_full + gst_dmabuf_memory_new
    //   2. gst_buffer_with_video_meta 标 NV12 + (W,H) + (stride_w,stride_h)
    //   3. appsrc emit "need-data" callback 中 push_buffer
    //   4. 引用计数确保 gst pipeline 释放时 unmap DMA-BUF (但不 unmap, 只 ref--)

    void NotifyNoClient();              // on_demand=true 时, 没 client 不 push
    bool HasClient();

private:
    GstElement* appsrc_ = nullptr;
    DrmBuffer scratch_;                 // RGA 缩放后的目标尺寸
    int64_t   frame_count_ = 0;
    std::atomic<bool> has_client_{false};
};
```

### 12.3 yaml 自循环 (与现有 `RoiConfig::LoadFromFile` 同模式)

```cpp
// src/output_config.cc (新增)
OutputConfig OutputConfig::LoadFromYaml(const std::string& path) {
    cv::FileStorage fs(path, cv::FileStorage::READ);
    if (!fs.isOpened()) throw std::runtime_error("cannot open: " + path);
    OutputConfig cfg;
    cfg.enabled         = (int)fs["output"]["enabled"] != 0;
    cfg.port            = (int)fs["output"]["port"];
    cfg.encoder         = (std::string)fs["output"]["encoder"];
    cfg.on_demand       = (int)fs["output"]["on_demand"] != 0;
    // ... streams[]
    cv::FileNode streams = fs["output"]["streams"];
    for (auto it = streams.begin(); it != streams.end(); ++it) {
        OutputStreamConfig s;
        s.path          = (std::string)(*it)["path"];
        s.width         = (int)(*it)["width"];
        // ...
        cfg.streams.push_back(s);
    }
    return cfg;
}
```

### 12.4 tools/output_smoke.sh 骨架

```bash
#!/usr/bin/env bash
# Sprint A 验收: RTSP 输出端到端
set -uo pipefail
PASS=0; FAIL=0; WARN=0
ok()    { echo "[PASS] $*"; PASS=$((PASS+1)); }
warn()  { echo "[WARN] $*"; WARN=$((WARN+1)); }
fail()  { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }

bar() { echo ""; echo "===== $* ====="; }

# 1. gst-rtsp-server 包已装
bar "1. system packages"
dpkg -l gstreamer1.0-rtsp-server-1.0 | grep -q ii && ok "gstreamer1.0-rtsp-server-1.0 installed" || fail "gstreamer1.0-rtsp-server-1.0 missing (apt install)"

# 2. image-stitching 已起来且 RTSP 端口 listen
bar "2. service running"
ss -tln | grep -q ':8554 ' && ok "port 8554 listening" || fail "port 8554 not listening"

# 3. ffprobe 能拿到 SDT
bar "3. ffprobe RTSP describe"
ffprobe -v quiet -rtsp_transport tcp -i rtsp://localhost:8554/stitch 2>&1 | head -20 | grep -q "Video:" \
    && ok "ffprobe got SDP" || fail "ffprobe failed"

# 4. gst-launch 拉一帧
bar "4. single frame pull (5s)"
timeout 10 gst-launch-1.0 -v rtspsrc location=rtsp://localhost:8554/stitch latency=50 protocols=4 \
    ! rtph264depay ! h264parse ! fakesink num-buffers=30 2>&1 | grep -q "Got EOS" \
    && ok "30 frames decoded OK" || fail "decode error"

# 5. performance
bar "5. perf fps + bitrate"
tail -100 logs/image-stitching.log | grep "STITCH_PERF_FPS" | tail -3 | sed 's/^/    /'
fps=$(tail -100 logs/image-stitching.log | grep "STITCH_PERF_FPS" | tail -1 | grep -oE "fps=[0-9.]+" | sed 's/fps=//')
[ "${fps%.*}" -ge 25 ] && ok "stitch fps=${fps} (>= 25)" || fail "stitch fps=${fps} (< 25)"

# 6. summary
bar "SUMMARY"
echo "  PASS=${PASS}  WARN=${WARN}  FAIL=${FAIL}"
[ "${FAIL}" -eq 0 ] && exit 0 || exit 1
```

---

## 13. 与已有方案的呼应

| 现有方案 | 关系 |
|---|---|
| [`NETWORK_CAMERA_PLAN.md`](NETWORK_CAMERA_PLAN.md) v3.0 | **前置依赖**: 输入侧 Sprint 0/1/2 PASS 后接 v3.1 Sprint A |
| [`HISTORY.md`](HISTORY.md) § 0x02 踩坑 | **直接复用**: 不要相信 gstreamer 上报 stride、mmap 限制、IOMMU 耗尽 |
| [`HISTORY.md`](HISTORY.md) § 2.5 SKIP_BOOTSTRAP | 受影响: SKIP_BOOTSTRAP + RTSP 输出互不冲突, 但要确保 stitch 启动顺序 (output 在 `STITCH_HTTP` 之后 init) |
| [`CAMERA_PAGE_INTEGRATION.md`](CAMERA_PAGE_INTEGRATION.md) | **同期**: CameraPage `/api/status` 加 `output_rtsp` 字段; 端口 8080 (服务) 与 8554 (RTSP) 不同 |
| `[gst_mpp_decoder][..]` 模块 | 对称: `[gst_rtsp_server][..]` 与 `[gst_mpp_decoder][..]` 解码器侧同命名空间, 避免日志混乱 |
| `[stitch_status::GlobalStatus]` | schema 加 `output_rtsp` 块 (atomic, 单一 writer = GstRtspServer) |
| `tools/post_flash_test.sh` | 加 1 项: `gstreamer1.0-rtsp-server-1.0` 包检测 |
| `tools/sprint0_smoke.sh` (输入侧验收) | 不动; v3.1 新增 `tools/output_smoke.sh`（输出侧验收）|

---

## 14. 引用

- **前置**: `docs/NETWORK_CAMERA_PLAN.md` — v3.0 输入侧, Sprint 0/1/2
- **基础架构**: `AGENTS.md` — 硬约束 / 6 路硬编码 / 可视化键位
- **操作手册**: `docs/HISTORY.md` § 操作手册 — 构建 / EGL / GPU / SSH / SKIP_BOOTSTRAP
- **踩过的坑**: `docs/HISTORY.md` § 踩过的坑 — mppvideodec 行为、stride、IOMMU、gstreamer filesrc 拒 `..`
- **CameraPage**: `docs/CAMERA_PAGE_INTEGRATION.md` — status 字段规约 / 端口约定
- **gstreamer1.0-rtsp-server API**: <https://gstreamer.freedesktop.org/documentation/rtsp-server/index.html>
- **gstreamer1.0-rockchip1 元素表**: <https://github.com/JeffyCN/rockchip_mirrors> （含 `mpph264enc` props）
- **RFC 2326**: <https://datatracker.ietf.org/doc/html/rfc2326>