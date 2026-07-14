# new-4k-stitch

实时多摄像头视频拼接器, Rockchip ARM64 开发板运行。 6 路 GC4683 / RTSP H.264 -> gstreamer1.0-rockchip1 mppvideodec -> DMA-BUF 零拷贝 -> RGA 裁剪/旋转 -> OpenCL 接缝羽化 -> DRM 输出。

- **Primary target**: RK3588
- **Verified on**: RK3576 (rocktech 主板, Ubuntu 22.04, kernel 6.1.75)
- **Target input**: **2K (2560x1440) @ 30 fps**, 6 路(IP camera RTSP 或 dataset fallback)
- **Target layout**: **2x3 (6 cameras, 2 cols x 3 rows)**
- **当前阶段 (2026-07-14)**: 见 [`docs/USER_GOAL_ROADMAP.md`](docs/USER_GOAL_ROADMAP.md) - 面向 GC4683 MIPI 直连 / FOV 101x68° / 横纵 ~180° 俯视全景的 Sprint 4 计划。

## Reference

> Du, Chengyao, et al. (2020). *GPU based parallel optimization for real time panoramic video stitching.* Pattern Recognition Letters, 133, 62-69.

## Quick start

```bash
mkdir build && cd build
cmake ..            # CMake 3.10+, C++11
make -j$(nproc)
./image-stitching
```

- CMake options: `ENABLE_RK_HARDWARE_DECODING=ON`, `ENABLE_RGA_DMA_STITCHING=ON`(默认 ON, 零拷贝 pipeline 必需)
- FFmpeg 链接:CMakeLists.txt 默认 `/usr/lib/aarch64-linux-gnu`, 可 `-DFFMPEG_LIB_DIR=...` 覆盖(多架构编译坑见 [`docs/HISTORY.md`](docs/HISTORY.md) §FFmpeg)
- 依赖:OpenCV >= 4.5, gstreamer-1.0 + gstreamer1.0-rockchip1(mppvideodec), OpenCL, EGL, GLESv2, GBM, librga, libdrm, SDL2
- **无单元测试、无 linter、无 CI** - 仅 Rockchip 板端手动验证

### 输入源切换

```bash
unset INPUT_SOURCE_MODE                       # 默认: dataset fallback(fallback 用)
export INPUT_SOURCE_MODE=camera               # 走 params/camera_sources.yaml(v3.0 = 6 路 RTSP)
```

### 性能调试

```bash
SAVE_STITCH_FRAMES=0 SAVE_DIAGNOSTIC_FRAMES=0 ./image-stitching     # 纯 FPS
```

完整 Sprint 0/1/2 历史验收脚本已删除, 详见 `docs/HISTORY.md`。

## Architecture

单可执行 `image-stitching`。入口 `src/app.cc` `main()` -> `App::run_stitching()`(noreturn loop)。

| 模块 | 职责 |
|---|---|
| `src/app.cc` | 主循环、ROI bootstrap、布局、visualizer action 分发 |
| `src/sensor_data_interface.cc` | 每路相机一个解码/采集线程, 队列帧供应(dataset / RTSP 双模式) |
| `src/gst_mpp_decoder.cc` | gstreamer pipeline(`BuildFilePipeline` + `BuildRtspPipeline`), v3.0 加 RTSP/watchdog, 输出 NV12 DMA-BUF fd + DIAG 诊断段 |
| `src/image_stitcher.cc` | RGA crop/rotate + 0/90/180/270 rotate 拷贝; 可选 GLES warp; OpenCL 羽化 |
| `src/rk_gles_warper.cc` | EGL+GLES warp via DMA-BUF import(可选, 初始化失败静默回退) |
| `src/drm_allocator.cc` | DRM dumb buffer 分配 |
| `src/roi_config.cc` + `src/roi_visualizer.cc` | ROI YAML 读写 + SDL2 可视化调参(**当前硬编码 4 路, 仅 offset**) |
| `src/http_server.cc` + `src/status_writer.cc` | CameraPage HTTP server(cpp-httplib, 端口 8080) + 状态 JSON 写入 |
| `src/mjpeg_streamer.cc` | MJPEG 推流缓冲(`/api/stream` 多播 part) |
| `src/stitching_param_generater.cc` | OpenCV `cv::detail` 拼接 pipeline wrapper(SIFT + Bundle Adjustment + Spherical Warper + MultiBand Blend) - **已编译但未调用, 不要"激活"它** |

### Pipeline modes(`src/app.cc` 定义)

1. **ROI + RGA + OpenCL**(默认): 多帧 ROI -> 2x2/2x3 布局 -> RGA 裁剪/拷贝 -> OpenCL 羽化
2. **GLES Warp + RGA + OpenCL**: GLES 非仿射 warp -> RGA 拷贝 -> OpenCL 羽化
3. 模式 2 在 GLES 初始化失败时静默回退到模式 1

## Current status(2026-07-14,以代码为真)

| 阶段 | 状态 | 备注 |
|---|---|---|
| v2.x 6 路 2x3 layout(dataset fallback)| OK 板上实测 6 路全 100+ fps @ 100% DMA-BUF | 绿条纹修复后(2026-07-08), 见 [`docs/HISTORY.md`](docs/HISTORY.md) § 绿条纹 |
| v3.0 6 路 RTSP IP camera pipeline | 部分(gstreamer-mpp pipeline 已编码, yaml 驱动就位, PoE 上电后端到端待跑)| `src/gst_mpp_decoder.cc::BuildRtspPipeline` |
| 用户最新目标: GC4683 MIPI / FOV 101x68° / ~180° 俯视 / 矩阵持久化 / UI 调 ROI 长宽 | 进行中, 详见 [`docs/USER_GOAL_ROADMAP.md`](docs/USER_GOAL_ROADMAP.md) | Sprint 4 起 4 段 |

**6 路硬编码改造进度**(集中在 [`AGENTS.md`](AGENTS.md) "6 路硬编码位置速查"):

- 已完成: `camera_rois[6]`、 `i < 6` cap 解除、 `EstimateOverlaps2x3` / `BuildCameraRois2x3` / `BuildStitchLayout2x3` / `BlendSeams` 6 路、 yaml 任意路数框架
- 未完成(Sprint 6 计划): `roi_visualizer.cc` 4->6 路循环、 `CameraRoiRect{x,y,w,h}` 取代 `RoiOffset`、 `/api/roi` 接 `x/y/w/h`、 `stitching_param_generater.cc` 主 pipeline 集成(Sprint 5)

**新代码动 `AGENTS.md` 前必读**:6 路硬编码位置速查([`AGENTS.md`](AGENTS.md)) + 当前活跃计划([`docs/USER_GOAL_ROADMAP.md`](docs/USER_GOAL_ROADMAP.md)) + 踩过的坑([`docs/HISTORY.md`](docs/HISTORY.md) § 3)。

## Environment variables

| 变量 | 用途 |
|---|---|
| `SAVE_STITCH_FRAMES`, `SAVE_DIAGNOSTIC_FRAMES`, `SAVE_FRAME_INTERVAL`, `DIAGNOSTIC_FRAME_LIMIT` | 落盘控制 |
| `INPUT_SOURCE_MODE` | `dataset`(默认, fallback)或 `camera`(走 yaml) |
| `STITCH_DEBUG_LEVEL`, `RK_GLES_WARPER_DEBUG_LEVEL` | 调试 verbosity |
| `ENABLE_VISUAL_TUNING`(默认 1)| 显示 SDL2 窗口 |
| `SHOW_ROI_MARKERS`(默认 1)| 画 ROI 边框 |
| `USE_ROI_CONFIG`(默认 1)| 启动时读 `params/roi_tuning.yaml`;`0` = 强制重检并覆盖 |
| `SKIP_BOOTSTRAP`(默认 0)| 固定支架场景(详 [`docs/HISTORY.md`](docs/HISTORY.md) § 2.7) |
| `MJPEG_INTERVAL`(默认 2)| 每 N 帧推一帧 MJPEG 给 `/api/stream` |
| `MJPEG_DOWNSCALE_W`/`MJPEG_DOWNSCALE_H`(默认 960/816)| MJPEG 预览降采样尺寸 |
| `MJPEG_QUALITY`(默认 75)| cv::imencode JPEG quality |

> README.md 旧版本曾列的 `STITCH_K_FOCAL_SCALE` / `STITCH_K_FX/FY_SCALE` / `STITCH_K_CX/CY_OFFSET` / `STITCH_K_FOCAL_SCALE_CAM_0..5` **代码没有读取**, 见 [`AGENTS.md`](AGENTS.md) § 环境变量 / Sprint 4-A 计划。

## Further reading

- **[`AGENTS.md`](AGENTS.md)** - Agent 入口 / 编码规范 / 6 路硬编码位置速查 / 命名规范 / 调试开关 / 环境变量 / 可视化键盘映射
- **[`docs/USER_GOAL_ROADMAP.md`](docs/USER_GOAL_ROADMAP.md)** - **当前活跃计划**(2026-07-14, 6 路 GC4683 MIPI, FOV 101x68°, 横纵 ~180° 俯视, Sprint 4-7)
- **[`docs/HISTORY.md`](docs/HISTORY.md)** - 设计演进时间线 + **操作手册**(构建 / SSH / EGL / GPU 监控) + **踩过的坑**(绿条纹 / stride / IOMMU / GLES warp 限制 / FFmpeg 多架构坑)+ Sprint 0/1/2 历史回顾
- **[`docs/CAMERA_PAGE_INTEGRATION.md`](docs/CAMERA_PAGE_INTEGRATION.md)** - 浏览器管理平台(C++ 内嵌 cpp-httplib, 端口 8080)方案 + 阶段 1 部署运维

## Acknowledgments

Original paper: Du et al. 2020(cited above). Hardware adaptation for Rockchip SoCs based on community references at <https://github.com/nyanmisaka/ffmpeg-rockchip/wiki/Compilation>.
