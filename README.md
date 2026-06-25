# new-4k-stitch

Real-time multi-camera video stitcher for Rockchip ARM64 boards. Pipeline: FFmpeg rkmpp HW decode → DMA-BUF zero-copy → RGA crop/rotate (+ optional GLES warp) → OpenCL seam feathering → DRM output.

- **Primary target**: RK3588
- **Verified on**: RK3576 (current code paths, librga/EGL/GBM/rkmpp; RK3588 is expected to run unchanged, not yet validated)
- **Current input**: **2K (2560×1440)**, 30 fps. v1 era 4K 输入已废弃 (搜索带宽 magic number 改为分辨率自适应, 见 `src/app.cc` 的注释)
- **Current scope**: 2×2 (4 cameras). **Migration in progress → 2×3 (6 cameras, 2 cols × 3 rows).**

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

## Current status: 2×2 (4 cameras) → 2×3 (6 cameras) migration

**This project is mid-migration from a 2×2 (4-cam) layout to a 2×3 (6-cam, 2 cols × 3 rows) layout.** The codebase still ships 4-cam defaults in many places. Every hardcoded 4-cam assumption is enumerated in `AGENTS.md` (BuildDefaultTuning, EstimateOverlaps2x2, BuildCameraRois2x2, BuildStitchLayout2x2, roi_config.h's 4-entry array, the 4 camchain YAMLs, the 4 ROI tuning YAML keys, the visualizer's 4-cam cycling, etc.) — **read `AGENTS.md` first before touching this code.**

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

## Further reading

- `AGENTS.md` — code conventions, 2×3 migration checklist, hardcoded 4-cam locations, visualizer keyboard map, code-style standards
- `docs/HISTORY.md` — full design-iteration timeline, problems encountered (4K search-band bug, RGA bandwidth, MMU/IOMMU cost, AFBC incompatibility), and operational playbook (FFmpeg/EGL/SSH/性能调优)

## Acknowledgments

Original paper: Du et al. 2020 (cited above). Hardware adaptation for Rockchip SoCs based on community references at <https://github.com/nyanmisaka/ffmpeg-rockchip/wiki/Compilation>.
