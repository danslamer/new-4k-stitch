# new-4k-stitch

实时多摄像头视频拼接器，Rockchip ARM64 开发板运行：6 路 IP camera → gstreamer1.0-rockchip1 mppvideodec → DMA-BUF 零拷贝 → RGA 裁剪/旋转 → OpenCL 接缝羽化 → DRM 输出。

- **Primary target**: RK3588
- **Verified on**: RK3576 (rocktech 主板, Ubuntu 22.04, kernel 6.1.75)
- **Target input**: **2K (2560×1440) @ 30 fps**, 6 路 RTSP
- **Target layout**: **2×3 (6 cameras, 2 cols × 3 rows)**
- **当前阶段**: v3.0 Sprint 0（IP camera 骨架贯通），详见 [`docs/NETWORK_CAMERA_PLAN.md`](docs/NETWORK_CAMERA_PLAN.md)

## Reference

> Du, Chengyao, et al. (2020). *GPU based parallel optimization for real time panoramic video stitching.* Pattern Recognition Letters, 133, 62-69.

## Quick start

```bash
mkdir build && cd build
cmake ..            # CMake 3.10+, C++11
make -j$(nproc)
./image-stitching
```

- CMake options: `ENABLE_RK_HARDWARE_DECODING=ON`, `ENABLE_RGA_DMA_STITCHING=ON`（默认 ON, 零拷贝 pipeline 必需）
- FFmpeg 链接：CMakeLists.txt 默认 `/usr/lib/aarch64-linux-gnu`, 可 `-DFFMPEG_LIB_DIR=...` 覆盖（多架构编译坑见 [`docs/HISTORY.md`](docs/HISTORY.md) §FFmpeg）
- 依赖：OpenCV ≥ 4.5, gstreamer-1.0 + gstreamer1.0-rockchip1（mppvideodec）, OpenCL, EGL, GLESv2, GBM, librga, libdrm, SDL2
- **无单元测试、无 linter、无 CI** — 仅 Rockchip 板端手动验证

### 输入源切换

```bash
unset INPUT_SOURCE_MODE                       # 默认: dataset fallback（v3.0 占位用）
export INPUT_SOURCE_MODE=camera               # 走 params/camera_sources.yaml（v3.0 = 6 路 RTSP）
```

### 性能 / Sprint 0 验收

```bash
SAVE_STITCH_FRAMES=0 SAVE_DIAGNOSTIC_FRAMES=0 ./image-stitching     # 纯 FPS
bash tools/sprint0_smoke.sh                                          # v3.0 骨架贯通一键验收
```

## Architecture

单可执行 `image-stitching`。入口 `src/app.cc` `main()` → `App::run_stitching()`（noreturn loop）。

| 模块 | 职责 |
|---|---|
| `src/app.cc` | 主循环、ROI bootstrap、布局 |
| `src/sensor_data_interface.cc` | 每路相机一个解码/采集线程，队列帧供应（v3.0: rtspsrc + watchdog）|
| `src/gst_mpp_decoder.cc` | gstreamer pipeline（v2.4 起接管 mppvideodec；v3.0 加 RTSP/watchdog），输出 NV12 DMA-BUF fd + DIAG 诊断段 |
| `src/image_stitcher.cc` | RGA/GLES warp, OpenCL 接缝, `dma_buf_cache_` |
| `src/rk_gles_warper.cc` | EGL+GLES warp via DMA-BUF import（可选，初始化失败静默回退）|
| `src/drm_allocator.cc` | DRM dumb buffer 分配 |
| `src/roi_config.cc` + `src/roi_visualizer.cc` | ROI YAML 读写 + SDL2 可视化调参 |
| `src/http_server.cc` + `src/status_writer.cc` | CameraPage HTTP server（cpp-httplib, 端口 8080）+ 状态 JSON 写入 |
| `src/stitching_param_generater.cc` | 相机标定 + warp map（已初始化但**不在主 pipeline**, 不要"激活"它）|

### Pipeline modes（`src/app.cc` 定义）

1. **ROI + RGA + OpenCL**（默认）：多帧 ROI → 2×2/2×3 布局 → RGA 裁剪/拷贝 → OpenCL 羽化
2. **GLES Warp + RGA + OpenCL**：GLES 非仿射 warp → RGA 拷贝 → OpenCL 羽化
3. 模式 2 在 GLES 初始化失败时静默回退到模式 1

## Current status（v3.0）

| 阶段 | 状态 | 备注 |
|---|---|---|
| v2.x 6 路 2×3 layout（dataset fallback）| ✅ 板上实测 6 路全 100+ fps @ 100% DMA-BUF | 绿条纹修复后 (2026-07-08), 见 [`docs/HISTORY.md`](docs/HISTORY.md) § 绿条纹 |
| v3.0 Sprint 0 骨架贯通 | ⏳ 立即 | yaml + TCP/554 + gst-launch 烟测 + 编译 + 30s 端到端（`tools/sprint0_smoke.sh` 必须 PASS）|
| v3.0 Sprint 1 同步 + 稳定 | ⏳ 计划 | watchdog + NTP/PTP、L2 |
| v3.0 Sprint 2 标定 + 美化 | ⏳ 计划 | 实际 K 矩阵覆盖 FOV 反推占位、标定板视频录入 |

**6 路硬编码改造列表**（迁移进度, 集中在 [`AGENTS.md`](AGENTS.md) "6 路硬编码位置速查"）：`camera_rois[6]` (v3.x 改成存绝对 ROI)、`i < 6`、`EstimateOverlaps2x3` / `BuildCameraRois2x3` / `BuildStitchLayout2x3` / `BlendSeams` 6 路、`roi_visualizer` Tab 6 路循环 — 全部已完成。

**新代码动 `AGENTS.md` 前必读**：6 路硬编码位置速查（[`AGENTS.md`](AGENTS.md)）+ 当前活跃计划（[`docs/NETWORK_CAMERA_PLAN.md`](docs/NETWORK_CAMERA_PLAN.md)）+ 踩过的坑（[`docs/HISTORY.md`](docs/HISTORY.md) § 3）。

## Environment variables

| 变量 | 用途 |
|---|---|
| `SAVE_STITCH_FRAMES`, `SAVE_DIAGNOSTIC_FRAMES`, `SAVE_FRAME_INTERVAL`, `DIAGNOSTIC_FRAME_LIMIT` | 落盘控制 |
| `INPUT_SOURCE_MODE` | `dataset`（默认, fallback）或 `camera`（走 yaml）|
| `STITCH_K_FOCAL_SCALE`, `STITCH_K_FX/FY_SCALE`, `STITCH_K_CX/CY_OFFSET` | 全局 K 矩阵调参 |
| `STITCH_K_FOCAL_SCALE_CAM_0..5` | 单相机焦距缩放（v3.0: 6 路已扩）|
| `STITCH_DEBUG_LEVEL`, `RK_GLES_WARPER_DEBUG_LEVEL` | 调试 verbosity |
| `ENABLE_VISUAL_TUNING`（默认 1）| 显示 SDL2 窗口 |
| `SHOW_ROI_MARKERS`（默认 1）| 画 ROI 边框 |
| `USE_ROI_CONFIG`（默认 1）| 启动时读 `params/roi_tuning.yaml`；`0` = 强制重检并覆盖 |
| `SKIP_BOOTSTRAP`（默认 0）| 固定支架场景（详 [`docs/HISTORY.md`](docs/HISTORY.md) § 2.7） |

## Further reading

- **[`AGENTS.md`](AGENTS.md)** — Agent 入口 / 编码规范 / 6 路硬编码位置速查 / 命名规范 / 调试开关 / 环境变量 / 可视化键盘映射
- **[`docs/NETWORK_CAMERA_PLAN.md`](docs/NETWORK_CAMERA_PLAN.md)** — **当前活跃计划** (v3.0, 2026-07-08, 6 路 IP camera RTSP, Sprint 0/1/2)
- **[`docs/HISTORY.md`](docs/HISTORY.md)** — 设计演进时间线 + **操作手册**（构建 / SSH / EGL / GPU 监控）+ **踩过的坑**（绿条纹 / stride / IOMMU / GLES warp 限制 / FFmpeg 多架构坑）
- **[`docs/CAMERA_PAGE_INTEGRATION.md`](docs/CAMERA_PAGE_INTEGRATION.md)** — 浏览器管理平台（C++ 内嵌 cpp-httplib, 端口 8080）方案 + 阶段 1 部署运维

## Acknowledgments

Original paper: Du et al. 2020（cited above）. Hardware adaptation for Rockchip SoCs based on community references at <https://github.com/nyanmisaka/ffmpeg-rockchip/wiki/Compilation>.