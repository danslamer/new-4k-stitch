# AGENTS.md

Codex / Claude Code / 其他 AI agent 入口。详细设计、踩坑记录、操作手册见 `docs/`：

- **`docs/HISTORY.md`** — 设计演进时间线 + **操作手册**（SSH / EGL / GPU / 构建）+ **踩过的坑**（绿条纹、stride、IOMMU、GLES warp 局限性 等）
- **`docs/NETWORK_CAMERA_PLAN.md`** — **当前阶段计划**（v3.0, 2026-07-08）：6 路 IP camera (PoE) → 2×3 拼接，Sprint 0/1/2 三段，被该文件引用的 `tools/sprint0_*.sh` 必须保留
- **`docs/CAMERA_PAGE_INTEGRATION.md`** — 浏览器管理平台（C++ 内嵌 cpp-httplib, 端口 8080）接入方案 + 阶段 1 部署运维

- **docs/RTSP_OUTPUT_PLAN.md** — **v3.1 输出侧计划**（拼接全景 RTSP 服务化, 端口 8554, `rtsp://<board>:8554/stitch` + `/stitch_sub`）


- **`docs/QUICK_START_IP_ONLY.md`** — `改 IP 就能用` 5 步入门 (commit set_cam_ips.sh 后, 改完 IP + 一行启动, 自动走 RTSP camera 模式)
- **`docs/RTSP_OUTPUT_PLAN.md`** — v3.1 输出侧计划（**v3.2 已实现**: 帧差掩码 + 2 路 RTSP 推流 `/stitch` + `/stitch_diff`）
---

## 仓库定位

实时 6 路摄像头视频拼接器，运行在 Rockchip ARM64 开发板上：

```
rtspsrc → rtph264depay → mppvideodec (DMA-BUF) → RGA crop/rotate → OpenCL seam feathering → DRM output
```

- **主开发板**：rocktech RK3588（Ubuntu 22.04.5 LTS, kernel 5.10.226）
- **已验证板**：rocktech RK3576（kernel 6.1.75，6 路物理 MIPI 直连接口）
- **当前输入**：**6 路 IP camera RTSP**（PoE 8+2 交换机, rtspsrc → mppvideodec DMA-BUF；v3.0 2026-07-08 取代了 v2.x 的本地 mp4 + MIPI 计划）
- **当前布局**：**2×3 (6 路)** — 2 列 × 3 行, 4800×4080 panorama
- **历史中间态**：4 路 dataset（`datasets/2k-test-h264/t50-t53+t40+t41.mp4`）fallback 跑通过；2×3 layout 已初始化（2026-07-08 实测 6 路全通 100+ fps）
- **目标 FPS**：30 fps @ 6×2K 端到端

## 架构硬约束（锁定, 2026-07-06）

> 6 路 2K 拼接要求 30 FPS 端到端, 整个 pipeline 必须满足：

| 层 | 必须用 | 不接受 |
|---|---|---|
| **解码** | `gstreamer1.0-rockchip1 mppvideodec` (`dma-feature=true` 走 mpp_service) | 软件解码器；FFmpeg rkmpp wrapper（vendor 不维护，2026-07-06 实测 0 帧） |
| **帧缓冲** | DMA-BUF backed GstBuffer（appsink 拿 `memory:DMABuf` → `gst_dmabuf_memory_get_fd`） | memcpy 到系统内存（YUV420P / RGBA） |
| **RGA 路径** | RGA2 HW 裁剪/旋转/羽化 | CPU 端的 OpenCV 仿射（warpAffine） |
| **OpenCL** | Mali GPU 接缝羽化（`clImportMemoryARM`） | CPU 端 alpha blend |

**禁止任何 sw fallback**：
- appsink 收到非 DMA-BUF 帧在 `gst_mpp_decoder.cc` 直接 reject + 报错, 不会进入 stitch 路径
- appsink caps 限定 `video/x-raw(memory:DMABuf),format=NV12`
- 任何"暂时软解码跑一下看效果"的提案不应合入

## 当前状态（一句话）

6 路 2×3 layout 已端到端跑通（v2.5 2026-07-07, 绿条纹修复后 100+ fps）→ v3.0（2026-07-08）切换为 6 路 IP camera RTSP，Sprint 0 骨架贯通（yaml + TCP/554 + gst-launch 烟测 + 编译 + 30s 端到端验证脚本就绪）。下一步：Sprint 1（同步 + 稳定 + watchdog）。

详细状态 & 测试命令：见 `docs/NETWORK_CAMERA_PLAN.md` §9 实施分期 & `tools/sprint0_smoke.sh`。

## 常用命令

### 构建

```bash
mkdir -p build && cd build
cmake ..                # CMake 3.10+, C++11; ENABLE_RK_HARDWARE_DECODING=ON, ENABLE_RGA_DMA_STITCHING=ON 默认
make -j$(nproc)
./image-stitching
```

依赖：OpenCV ≥ 4.5、gstreamer-1.0 + gstreamer1.0-rockchip1（mppvideodec）、OpenCL、EGL、GLESv2、GBM、librga、libdrm、SDL2。FFmpeg 保留链接（作为转码工具）。

**构建路径**: CMakeLists.txt 默认 `/usr/lib/aarch64-linux-gnu`，可 `-DFFMPEG_LIB_DIR=...` 覆盖。**FFmpeg 多架构编译坑** 详见 `docs/HISTORY.md` §FFmpeg。

### 输入源切换

```bash
unset INPUT_SOURCE_MODE                       # 默认: dataset 模式（fallback 用）
export INPUT_SOURCE_MODE=camera               # 走 params/camera_sources.yaml（v3.0 = 6 路 rtsp）
```

### 性能调试

```bash
SAVE_STITCH_FRAMES=0 SAVE_DIAGNOSTIC_FRAMES=0 ./image-stitching  # 纯 FPS 测
eglinfo --display surfaceless                                     # EGL 链路检查
sudo cat /sys/class/devfreq/27800000.gpu/load                    # GPU 负载
```

**Sprint 0 一键验收**（v3.0）：`bash tools/sprint0_smoke.sh` → 跑完 PASS 才进入 Sprint 1。

## 核心模块

| 文件 | 职责 |
|---|---|
| `src/app.cc` | 主循环、ROI bootstrap、布局 |
| `src/sensor_data_interface.cc` | 每路相机一个解码/采集线程（v3.0：rtspsrc + watchdog） |
| `src/gst_mpp_decoder.cc` | gstreamer pipeline（v2.4 起接管 mppvideodec，输出 NV12 DMA-BUF fd + DIAG 诊断段） |
| `src/image_stitcher.cc` | RGA/GLES warp, OpenCL 接缝, `dma_buf_cache_` |
| `src/rk_gles_warper.cc` | EGL+GLES warp via DMA-BUF import（可选，初始化失败静默回退） |
| `src/drm_allocator.cc` | DRM dumb buffer 分配 |
| `src/roi_config.cc` + `src/roi_visualizer.cc` | ROI YAML 读写 + SDL2 可视化调参 |
| `src/http_server.cc` + `src/status_writer.cc` | CameraPage HTTP server (cpp-httplib, 端口 8080) + 状态 JSON 写入 |

## 6 路硬编码位置速查（2×3 迁移时改动）

| 位置 | 当前 | 目标 |
|---|---|---|
| `include/roi_config.h` | `CameraRoiRect camera_rois[6]` (v3.x: 改成绝对 ROI, 不再是 offset) | （已完成 4→6 扩展, 数组越界护栏待改 6→N 通用） |
| `src/app.cc:BuildDefaultTuning` | `i < 6` 已解除 cap | — |
| `src/app.cc:EstimateOverlaps2x3` / `BuildCameraRois2x3` / `BuildStitchLayout2x3` | 6 路版本 | （与 2×2 版本并存，按 `num_img_` 分支） |
| `src/image_stitcher.cc:BlendSeams` | 6 路 + 6 dispatch_seam | — |
| `src/roi_visualizer.cc` Tab | 4→6 cam 循环已完成 | — |
| `params/camera_sources.yaml` | 6 路 cam 块（v3.0: `type: rtsp`） | — |

**yaml 框架已支持任意路数**：`params/camera_sources.yaml` 加新 cam 块即可，不动 C++ 代码。

## 命名规范

- 类：PascalCase（`ImageStitcher`）
- 函数：snake_case（`load_parameters`）
- 常量：`UPPER_SNAKE_CASE`
- 全局变量：`g_` 前缀 + snake_case（`g_debug_level`, `g_feather_width`）
- 相机调参 offset 数组按**物理相机编号**索引, 不按网格位置

## 调试开关

| 变量 | 默认 | 含义 |
|---|---|---|
| `g_debug_level` | 0 | 0=OFF, 1=INFO, 2=DEBUG, 3=VERBOSE |
| `g_save_stitched_frames` | 1 | 落盘拼接结果图 |
| `g_save_roi_confidence_debug` | 1 | 落盘 ROI bootstrap 置信度图 |
| `g_feather_width` | (像素) | **必须偶数**（kernel 除以 2） |
| `g_feather_strength` | (S-curve, >1.0 = 更平滑) | |
| `g_multi_frame_roi_debug_level` | 1 | bootstrap verbose |
| `g_enable_visual_tuning` | `ENABLE_VISUAL_TUNING=1` | SDL2 窗口 |
| `g_show_roi_markers` | `SHOW_ROI_MARKERS=1` | 画 ROI 边框（debug 模式 M 键切换） |
| `g_use_roi_config` | `USE_ROI_CONFIG=1` | 启动读 `params/roi_tuning.yaml`，`0` = 强制重检并覆盖 |
| `g_skip_bootstrap` | `SKIP_BOOTSTRAP=0` | **固定支架场景置 1**：YAML 存在→用 YAML；YAML 缺失→仍跑一次 bootstrap 兜底（不报错退出）。详 `docs/HISTORY.md` |

## 环境变量

| 变量 | 用途 |
|---|---|
| `SAVE_STITCH_FRAMES`, `SAVE_DIAGNOSTIC_FRAMES`, `SAVE_FRAME_INTERVAL`, `DIAGNOSTIC_FRAME_LIMIT` | 落盘控制 |
| `INPUT_SOURCE_MODE` | `dataset`（默认，fallback）或 `camera`（走 yaml） |
| `STITCH_K_FOCAL_SCALE`, `STITCH_K_FX/FY_SCALE`, `STITCH_K_CX/CY_OFFSET` | 全局 K 矩阵调参 |
| `STITCH_K_FOCAL_SCALE_CAM_0..5` | 单相机焦距缩放（6 路已扩） |
| `STITCH_DEBUG_LEVEL`, `RK_GLES_WARPER_DEBUG_LEVEL` | 调试 verbosity |
| `ENABLE_VISUAL_TUNING`（默认 1）| 显示 SDL2 窗口 |
| `SHOW_ROI_MARKERS`（默认 1）| 画 ROI 边框 |
| `USE_ROI_CONFIG`（默认 1）| 启动时读 `params/roi_tuning.yaml`；`0` = 强制重检并覆盖 |
| `SKIP_BOOTSTRAP`（默认 0）| 固定支架场景（详 `g_skip_bootstrap` 与 `docs/HISTORY.md`） |

## 可视化调参（SDL2）键盘映射

| 键 | 功能 |
|---|---|
| `↑`/`↓`/`←`/`→` 或 `W`/`A`/`S`/`D` | ROI 步进 |
| `Tab` | 上/下一相机（6 路循环） |
| `1` / `5` / `0` / `P` | 步长 1 / 5 / 10 / 50+ |
| `F` | 羽化 toggle |
| `+` / `-` | 羽化宽度 ±10 |
| `B` | 保存 toggle |
| `L` / `K` | 保存间隔 ±10 |
| `E` | 保存配置到 YAML |
| `M` | ROI marker toggle |
| `Q` / `Esc` | 退出 debug 模式 |
| `Enter` / `D` | 进入 debug 模式（锁帧, 暂停新帧拉取） |

## 引用论文

> Du, Chengyao, et al. (2020). *GPU based parallel optimization for real time panoramic video stitching.* Pattern Recognition Letters, 133, 62-69.