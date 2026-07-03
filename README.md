# new-4k-stitch

Real-time multi-camera video stitcher for Rockchip ARM64 boards. Pipeline: FFmpeg rkmpp HW decode → DMA-BUF zero-copy → RGA crop/rotate (+ optional GLES warp) → OpenCL seam feathering → DRM output.

- **Primary target**: RK3588
- **Verified on**: RK3576 (rocktech 主板, Ubuntu 22.04, kernel 6.1.75; RK3588 is expected to run unchanged, not yet validated)
- **Target input**: **2K (2560×1440)**, 30 fps. v1 era 4K 输入已废弃 (搜索带宽 magic number 改为分辨率自适应, 见 `src/app.cc` 的注释)
- **Target layout**: **2×3 (6 cameras, 2 cols × 3 rows)**. 当前代码仍跑 2×2 (4 cams), 按 [DEVELOPMENT_PLAN v2.3 §2](docs/DEVELOPMENT_PLAN.md) 直接改造为 6 路.

## Reference

> Du, Chengyao, et al. (2020). *GPU based parallel optimization for real time panoramic video stitching.* Pattern Recognition Letters, 133, 62-69.

## Quick start

```bash
mkdir build && cd build
cmake ..            # CMake 3.10+, C++11
make
./image-stitching
```

- CMake options: `ENABLE_RK_HARDWARE_DECODING=ON`, `ENABLE_RGA_DMA_STITCHING=ON` (default ON; both required for the zero-copy pipeline)
- FFmpeg root hardcoded to `$ENV{HOME}/dev/ffmpeg60` — must exist on target with `PKG_CONFIG_PATH` and `LD_LIBRARY_PATH` set
- Requires: OpenCV ≥ 4.5, FFmpeg (rkmpp), OpenCL, EGL, GLESv2, GBM, librga, libdrm, SDL2
- **No tests, no linter, no CI.** Manual verification on Rockchip board only.

## Architecture

Single executable `image-stitching`. Entrypoint: `src/app.cc:1049` `main()` → `App::run_stitching()` (noreturn loop).

| Module | Role |
|--------|------|
| `src/app.cc` | Main loop, ROI bootstrap, layout |
| `src/sensor_data_interface.cc` | One decode thread per camera, queued frame supply |
| `src/image_stitcher.cc` | RGA/GLES warp, OpenCL seam blending, `dma_buf_cache_` |
| `src/rk_gles_warper.cc` | EGL+GLES warp via DMA-BUF import (optional) |
| `src/drm_allocator.cc` | DRM dumb buffer alloc/map/free |
| `src/logger.cc` | Singleton logger, results dir |
| `src/roi_config.cc` | ROI offset YAML read/write (`params/roi_tuning.yaml`) |
| `src/roi_visualizer.cc` | SDL2-based interactive tuning |
| `src/stitching_param_generater.cc` | Camera calibration + warp map (initialized but **not** in active path) |

### Pipeline modes (defined in `src/app.cc`)

1. **ROI + RGA + OpenCL** (default): multi-frame ROI → 2×2 layout → RGA crop/copy → OpenCL feather
2. **GLES Warp + RGA + OpenCL**: if warp data valid, GLES warp → RGA copy → OpenCL feather
3. Mode 2 falls back to mode 1 automatically on GLES init failure

## Current status: 4-cam → 6-cam (2×3) direct migration

**Project target**: 6 路 GC4683 MIPI → 2×3 (6-cam, 2 cols × 3 rows) panoramic stitch. Current code is **2×2 (4-cam)**, and we are migrating **directly to 6-cam 2×3 without intermediate 4-cam validation** per [DEVELOPMENT_PLAN.md v2.3](docs/DEVELOPMENT_PLAN.md).

**Phase status (2026-06-30)**:

| # | Phase | Status |
|---|---|---|
| 1 | 板子与驱动可用性测试 | ⏳ SSH ✅, 驱动 ✅, 6 路拓扑待确认 ([HISTORY §0x06](docs/HISTORY.md)) |
| **2** | **6 路数据集输入适配** | **⏳ 下一步** (改 yaml + 默认文件 + roi_offsets → 6 路) |
| 3 | 6 路 2×3 代码迁移 | ⏳ 等阶段 2 |
| 4 | 6 路 V4L2 摄像头采集 | ⏳ 等镜头到位 |
| 5 | 6 路相机标定 | ⏳ 与 3-4 并行 |
| 6 | 6 路 2×3 真机跑通 | ⏳ 等 3-5 |

**Hardcoded 4-cam assumptions** (to be removed in phase 3): `BuildDefaultTuning`'s `i < 4`, `EstimateOverlaps2x2` / `BuildCameraRois2x2` / `BuildStitchLayout2x2`, `roi_config.h`'s `roi_offsets[4]`, `BlendSeams`' `cl_in(4)` + 4 dispatch_seam calls, `params/camera_sources.yaml`'s 4 cam entries, `params/roi_tuning.yaml`'s cam0..cam3 keys, the visualizer's 4-cam cycling. Full list at [CLAUDE.md §"2×3 迁移硬编码点速查"](CLAUDE.md). **Read `CLAUDE.md` and `DEVELOPMENT_PLAN.md` before touching this code.**

## Environment variables

| Variable | Purpose |
|----------|---------|
| `SAVE_STITCH_FRAMES`, `SAVE_DIAGNOSTIC_FRAMES`, `SAVE_FRAME_INTERVAL`, `DIAGNOSTIC_FRAME_LIMIT` | Disk output control |
| `INPUT_SOURCE_MODE` | `dataset` (default) or `camera` |
| `STITCH_K_FOCAL_SCALE`, `STITCH_K_FX/FY_SCALE`, `STITCH_K_CX/CY_OFFSET` | Global K-matrix tuning |
| `STITCH_K_FOCAL_SCALE_CAM_0..3` | Per-camera K tuning (extend to `_CAM_5` for 2×3) |
| `STITCH_DEBUG_LEVEL`, `RK_GLES_WARPER_DEBUG_LEVEL` | Debug verbosity |
| `ENABLE_VISUAL_TUNING` (default 1) | Show SDL2 window |
| `SHOW_ROI_MARKERS` (default 1) | Draw ROI borders |
| `USE_ROI_CONFIG` (default 1) | Load `params/roi_tuning.yaml` on startup; `0` = force re-detect and overwrite |
| `SKIP_BOOTSTRAP` (default 0) | Fixed-rig mode (车载 GC4683 场景): YAML 存在 → 用 YAML; YAML 缺失 → 仍跑一次 bootstrap 兜底 (详 [HISTORY §6.5](docs/HISTORY.md)) |

## Further reading

- `CLAUDE.md` — code conventions, 2×3 migration checklist, hardcoded 4-cam locations, visualizer keyboard map, code-style standards, **6 路 board 选型**
- `docs/DEVELOPMENT_PLAN.md` — **完整开发方案 v2.3** (2026-06-30 重构, 6 阶段, 跳过 4 路过渡态)
- `docs/HISTORY.md` — full design-iteration timeline, problems encountered (4K search-band bug, RGA bandwidth, MMU/IOMMU cost, AFBC incompatibility), 6 路 MIPI 验证清单 (§0x06), operational playbook (FFmpeg/EGL/SSH/性能调优)

## Acknowledgments

Original paper: Du et al. 2020 (cited above). Hardware adaptation for Rockchip SoCs based on community references at <https://github.com/nyanmisaka/ffmpeg-rockchip/wiki/Compilation>.
