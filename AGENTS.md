# AGENTS.md

## Build

```bash
mkdir build && cd build
cmake ..                        # CMake 3.10+, C++11
make
./image-stitching
```

- CMake options: `ENABLE_RK_HARDWARE_DECODING=ON`, `ENABLE_RGA_DMA_STITCHING=ON` (both default ON)
- FFmpeg root is hardcoded: `$ENV{HOME}/dev/ffmpeg60` — must exist on target device with PKG_CONFIG_PATH and LD_LIBRARY_PATH set
- Requires: OpenCV >= 4.5, FFmpeg (rkmpp), OpenCL, EGL, GLESv2, GBM, librga, libdrm
- **No tests, no linter, no CI.** Only manual verification on RK3576 board.

## Target platform

Linux ARM64 (RK3576 Rockchip SoC). Code uses `/dev/dri/card0`, DRM ioctl, GBM, mmap — not portable to Windows/macOS for execution.

## Architecture

Single executable `image-stitching`. Entrypoint: `src/app.cc` `main()` → `App::run_stitching()` (noreturn loop).

Pipeline: FFmpeg rkmpp HW decode → DMA-BUF zero-copy → RGA crop/rotate + GLES warp → OpenCL seam feathering → DRM output buffer

### Key modules

| File | Role |
|------|------|
| `src/app.cc` | Main loop, ROI bootstrap, layout, global config vars |
| `src/sensor_data_interface.cc` | 4 decode threads, queued frame supply |
| `src/image_stitcher.cc` | RGA/GLES warp, OpenCL seam blending |
| `src/rk_gles_warper.cc` | EGL+GLES warp via DMA-BUF import |
| `src/drm_allocator.cc` | DRM dumb buffer alloc/map/free |
| `src/stitching_param_generater.cc` | Camera calibration + warp map (currently NOT used in main pipeline) |
| `src/logger.cc` | Singleton logger, results dir creation |
| `src/roi_config.cc` | ROI offset YAML config read/write (`params/roi_tuning.yaml`) |
| `src/roi_visualizer.cc` | OpenCV highgui interactive tuning: FPS overlay, ROI markers, keyboard controls |

### Pipeline modes

1. **ROI + RGA + OpenCL** (default): multi-frame ROI → 2×2 layout → RGA crop/copy → OpenCL feather
2. **GLES Warp + RGA + OpenCL**: if warp data valid, GLES warp → RGA copy → OpenCL feather
3. Mode 2 falls back to mode 1 automatically on GLES init failure

## Naming conventions

- Classes: PascalCase (`ImageStitcher`)
- Functions: snake_case (`load_parameters`)
- Constants: UPPER_SNAKE_CASE
- Global vars: `g_` prefix + snake_case (`g_debug_level`, `g_feather_width`)

## Debug switches

All debug output controlled by global variables in `src/app.cc`, not hardcoded prints:
- `g_debug_level` (0=OFF, 1=INFO, 2=DEBUG, 3=VERBOSE)
- `g_debug_opencl_feathering`, `g_save_roi_confidence_debug`, `g_save_stitched_frames`
- `g_feather_width` (pixels, must be even), `g_feather_strength` (S-curve, >1.0 = smoother)
- `g_enable_visual_tuning` (env `ENABLE_VISUAL_TUNING`, 1=enable OpenCV highgui window)
- `g_show_roi_markers` (env `SHOW_ROI_MARKERS`, default 1=show, also toggled by M key in debug mode)
- `g_use_roi_config` (env `USE_ROI_CONFIG`, default 1=read `params/roi_tuning.yaml`; 0=force re-detect and overwrite)

## Video input

Hardcoded in `sensor_data_interface.cc`: `../datasets/4k-test/` with files `t30.mp4`–`t33.mp4`.
Override via env var `INPUT_SOURCE_MODE` (dataset/camera). Camera calibration in `params/camchain_0.yaml`–`camchain_3.yaml`.

## Environment variables

`SAVE_STITCH_FRAMES`, `SAVE_DIAGNOSTIC_FRAMES`, `SAVE_FRAME_INTERVAL`, `DIAGNOSTIC_FRAME_LIMIT`, `INPUT_SOURCE_MODE`, `STITCH_K_FOCAL_SCALE`, `STITCH_K_FX/FY_SCALE`, `STITCH_K_CX/CY_OFFSET`, `STITCH_K_FOCAL_SCALE_CAM_0..3`, `STITCH_DEBUG_LEVEL`, `RK_GLES_WARPER_DEBUG_LEVEL`

### Visual tuning env vars

`ENABLE_VISUAL_TUNING` (1=show OpenCV window, default 0), `SHOW_ROI_MARKERS` (1=draw ROI borders in debug mode, default 1), `USE_ROI_CONFIG` (1=load `params/roi_tuning.yaml` on startup; 0=force auto-detect and overwrite config)

## Gotchas

- `assets/` contains **old/backup source files** — NOT part of the build, do not edit them expecting changes
- `StitchingParamGenerator` is initialized but its warp output is not used in the current main pipeline; ROI-based layout is active
- NV12 format throughout: Y plane + interleaved UV. stride may differ from width (check `nv12_frame.h`)
- DMA-BUF file descriptors are reused across frames; `image_stitcher.cc` caches `dma_buf_cache_` (hash map) to avoid remapping
- `g_feather_width` must be even — kernel divides by 2

## Existing instruction files

- `.github/industrial-coding.instructions.md` — coding standards, debug switch conventions, RK3576 adaptation guide (applies to `**/*.{cc,c,h}`)
- `.github/agents/industrial-coder.agent.md` — modification workflow (5-step: analyze → design → implement → verify → document)
- `.github/agents/code-assistant.agent.md` — read-only assistant (comments, README, directory cleanup, no logic changes)