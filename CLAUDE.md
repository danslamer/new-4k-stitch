# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Real-time 6-camera video stitcher running on Rockchip ARM64 boards. Pipeline:

```
v4l2src (rkisp_mainpath NV12) → mppvideodec (DMA-BUF) → RGA crop/rotate
  → OpenCL seam feathering → DRM output
```

- **Primary target**: RK3588 (Ubuntu 22.04, kernel 5.10.226)
- **Verified on**: RK3576 (kernel 6.1.75, 6 physical MIPI connected)
- **Single binary**: `image-stitching` (CMake target)
- **Camera count**: 6 cameras, 2×3 layout (2 cols × 3 rows)
- **Resolution**: 2K (2560×1440) @ 30 fps per camera; panorama 4800×4080
- **No unit tests, no linter, no CI** — only manual verification on the board

## Build

Native compile on the board (no cross-compile). PC ≠ build host for this binary.

```bash
mkdir -p build && cd build
cmake ..                # ENABLE_RK_HARDWARE_DECODING=ON, ENABLE_RGA_DMA_STITCHING=ON (default)
make -j$(nproc)
./image-stitching
```

Required deps: OpenCV ≥ 4.5, gstreamer-1.0 + gstreamer1.0-rockchip1 (mppvideodec), OpenCL, EGL, GLESv2, GBM, librga, libdrm, SDL2, FFmpeg (transcoding only). cpp-httplib is vendored at `third_party/cpp-httplib/httplib.h` (header-only).

FFmpeg `--prefix=/usr` multiarch pitfall (libdir must be `/usr/lib/aarch64-linux-gnu`): see `docs/HISTORY.md` §2.2.

## Input source switching

`INPUT_SOURCE_MODE` env var + `params/camera_sources.yaml` drive runtime source routing.

```bash
unset INPUT_SOURCE_MODE                       # default: dataset fallback
export INPUT_SOURCE_MODE=camera               # load params/camera_sources.yaml (currently 6× MIPI)
```

Each camera in `camera_sources.yaml` has `type: mipi | rtsp | file`. Per-type fields (see `include/sensor_data_interface.h`):
- `mipi`: `device_path` (e.g. `/dev/video66`), `io_mode=dmabuf|mmap`, `v4l2_buffer_count`
- `rtsp`: `uri`, `user_id`/`user_pw`, `latency_ms`, `use_tcp`, watchdog/reconnect params
- `file`: `uri` (mp4 path), `width`, `height`, `fps`

If a MIPI device open fails, `SensorDataInterface` automatically falls back to a 30 fps `BlackFrameProvider` placeholder (NV12 all-zeros) — see `camera_actually_started_()` and `BlackFramePushLoop` in `src/sensor_data_interface.cc`.

## Board-end smoke test (Sprint 4-MIPI, current)

```bash
# ssh rockemb (192.168.137.100), then:
bash tools/sprint4_mipi_bootstrap.sh             # probe → cmake+make → launch 5s → log
bash tools/sprint4_mipi_bootstrap.sh --probe-only # probe + auto-patch camera_sources.yaml
bash tools/sprint4_mipi_bootstrap.sh --bg         # background, log → /tmp/stitch.log
```

`tools/probe_v4l2.sh` enumerates `/dev/videoN` rkisp_mainpath nodes (NV12 + 2560×1440), and with `--edit-yaml` patches `device_path` entries in `camera_sources.yaml`. **All `v4l2-ctl --stream-mmap` invocations MUST include `--stream-count=N`**, otherwise ISP queue deadlocks on `kill -9` (see `docs/HISTORY.md` §3).

## Architecture (the big picture)

Entry point is `src/app.cc::App::run_stitching()` (noreturn loop). Per-frame pipeline:

1. **`SensorDataInterface`** (`src/sensor_data_interface.cc`): one decode/capture thread per camera; pushes `QueuedFrame` (DMA-BUF backed GstSample) into a per-camera queue.
2. **`App`**: collects one frame per camera → calls `ImageStitcher::WarpImages` per camera into the panorama DMA-BUF → `ImageStitcher::BlendSeams` for 7 pair overlaps → optional MJPEG snapshot to CameraPage → status writer.
3. **`ImageStitcher`** (`src/image_stitcher.cc`): RGA crop/rotate into scratch DMA-BUFs, OpenCL `clImportMemoryARM` (DMA-BUF zero-copy) for seam blending, with `dma_buf_cache_` (unordered_map by fd) to keep IOMMU page-table setup O(1) after warmup.
4. **`RkGlesWarper`** (`src/rk_gles_warper.cc`): EGL+GLES fallback path for non-affine warp via DMA-BUF import; init failure falls back silently to RGA path.
5. **`Logger` / `status_writer` / `http_server` / `mjpeg_streamer`**: observability + CameraPage control plane (cpp-httplib, port 8080).

### Layout / K matrix / ROI bootstrap

- `camera_intrinsics.h`: per-cam `Intrinsics` (K + distortion) and pose (yaw_h/v) loaded from `params/calibration.yaml` at startup; defaults to GC4683 FOV 101×68° (fx≈1052.55, fy≈1068.89). `MutableIntrinsics()` is the live writer, `ForCam(i)` the read accessor.
- `src/app.cc::BuildStitchLayout2x3` computes panorama 4670×2826 from per-cam yaw + K (replaces the historical hardcoded `W/8`, `H/12`).
- `src/app.cc::BootStrapOptimalLayout` does multi-frame ROI bootstrap: ORB + BFMatcher (NORM_HAMMING, Lowe 0.65) + RANSAC + `estimateAffinePartial2D`. EstimateOverlapByTemplate is a low-texture fallback.
- ROI state lives in `params/roi_tuning.yaml`; runtime override via SDL2 keys (see AGENTS.md) or `POST /api/roi` (`src/http_server.cc`).

### Stitching param generator (legacy, inactive)

`src/stitching_param_generater.cc` is the OpenCV `cv::detail` wrapper (SIFT→ORB as of v3.3, `OrbFeaturesFinder` + `OrbPairwiseMatcher`; default `matcher_type=affine`, `estimator_type=affine`, `ba_cost_func=no`, `warp_type=plane`). **It compiles but is not called by the App loop — do not "activate" it as a drive-by refactor.** ROI-driven layout is the active path.

### Architectural hard constraints (locked 2026-07-06)

| Layer | Required | Forbidden |
|---|---|---|
| Decode | `gstreamer1.0-rockchip1` mppvideodec `dma-feature=true` | FFmpeg rkmpp wrapper, sw decoders (vendor RKMPP wrapper is unmaintained, returns 0 frames) |
| Frame buffer | DMA-BUF GstBuffer (`memory:DMABuf` → `gst_dmabuf_memory_get_fd`) | memcpy to system memory (YUV420P / RGBA) |
| RGA | librga crop/rotate/copy | OpenCV `warpAffine` (CPU) |
| OpenCL | Mali GPU, `clImportMemoryARM` zero-copy | CPU alpha blend |

`gst_mpp_decoder.cc` rejects non-DMA-BUF frames at appsink caps filter (`video/x-raw(memory:DMABuf),format=NV12`). No "temporary sw decode" PR will be accepted.

## Coding conventions

- Class names: `PascalCase` (`ImageStitcher`)
- Functions: `snake_case` (`load_parameters`)
- Constants: `UPPER_SNAKE_CASE`
- Globals: `g_` prefix + snake_case (`g_debug_level`, `g_feather_width`)
- C++11 (CMake sets `CMAKE_CXX_STANDARD 11`); **no `std::filesystem`** (C++17). Use POSIX `realpath()` (see `CanonicalizeUri` in `src/sensor_data_interface.cc`).
- NV12 throughout. `stride_w`/`stride_h` may ≠ `width`/`height`. `gstreamer` caps can lie about stride — always assert `buf_size >= stride * 1.5 * height` and prefer kernel-stride computed from `actual_stride = buf_size / (height * 1.5)` over gstreamer-reported (see `stride_override` in `src/gst_mpp_decoder.cc` and `docs/HISTORY.md` §3).
- `feather_width` must be even.
- Camera tuning arrays indexed by **physical camera number**, not grid position.
- The vendored `assets/` directory is historical source — does NOT participate in build. Ignore any suggestion to put new headers there.
- `StitchingParamGenerator` is compiled but unused; do not introduce it into the main loop.

## Key environment variables

| Var | Default | Effect |
|---|---|---|
| `INPUT_SOURCE_MODE` | (unset) | `dataset` fallback / `camera` (yaml) |
| `SAVE_STITCH_FRAMES`, `SAVE_DIAGNOSTIC_FRAMES` | 1 | set 0 for pure FPS measurement |
| `SAVE_FRAME_INTERVAL`, `DIAGNOSTIC_FRAME_LIMIT` | 30 / 3 | disk output controls |
| `USE_ROI_CONFIG` | 1 | read `params/roi_tuning.yaml` at start; 0 = force re-bootstrap |
| `SKIP_BOOTSTRAP` | 0 | fixed-mount scenario: YAML present → use it; YAML missing → bootstrap once (no error). Standard recipe: `USE_ROI_CONFIG=1 SKIP_BOOTSTRAP=1` |
| `ENABLE_VISUAL_TUNING` | 1 | SDL2 window (set 0 on headless board) |
| `SHOW_ROI_MARKERS` | 1 | draw ROI borders (M toggles in debug mode) |
| `STITCH_DEBUG_LEVEL`, `RK_GLES_WARPER_DEBUG_LEVEL` | 0 | verbosity |
| `MJPEG_INTERVAL` | 2 | N frames between MJPEG pushes to `/api/stream` |
| `MJPEG_DOWNSCALE_W/H` | 960 / 816 | MJPEG preview downscale |
| `MJPEG_QUALITY` | 75 | cv::imencode JPEG quality |

## Currently active plan (Sprint 4-MIPI / 5 / 6)

The current goal: 6× GC4683 MIPI @ FOV 101×68° / near-180° overhead / matrix persistence / explicit per-cam ROI UI. Active doc is `docs/USER_GOAL_ROADMAP.md`. v3.4 architecture: foreground/background separation — AANAP (static multi-plane) + Seam (dynamic) + BgSubtractor switch on `fg_mask`. Sub-sprints:

- **Sprint 4-A/B/C**: K matrix from FOV, layout by yaw (✓ code committed aa3d3b7)
- **Sprint 4-MIPI** (in progress, board verification pending): v4l2src pipeline + `BlackFrameProvider` fallback; see `docs/BOARD_VERIFICATION_STATUS.md` for the SCP+SSH verification checklist
- **Sprint 5-PRE** (✓ done in aa3d3b7): SIFT → ORB swap in `stitching_param_generater.cc`
- **Sprint 5-AANAP-A/B**: offline AANAP H + T_sim estimation → CV_32FC2 LUTs → RGA `imremap` runtime
- **Sprint 5-SEAM-A/B**: GraphCut default seam + hard-cut + 3 px narrow band OpenCL kernel
- **Sprint 5-FG-A/B**: `BgSubtractor` + `PushSeam` + `SeamTracker` (Kalman)
- **Sprint 5-SAL**: 1-week saliency heatmap accumulation
- **Sprint 5-IPM** (optional): physical-scale ground IPM LUTs
- **Sprint 6-A/B/C**: UI 4→6 cam loop + `x/y/w/h` adjustment + `/api/roi` schema

**Reference paper**: Du, Chengyao, et al. (2020). *GPU based parallel optimization for real time panoramic video stitching.* Pattern Recognition Letters, 133, 62-69.

## Files to read first when touching the main loop

- `src/app.cc` — main loop, `BootStrapOptimalLayout`, `BuildStitchLayout2x3`, `BuildCameraRois2x3`, ROI bootstrap, K matrix loading
- `src/image_stitcher.cc` — `WarpImages` (RGA crop/rotate), `BlendSeams` (OpenCL kernel), `dma_buf_cache_`
- `include/sensor_data_interface.h` + `src/sensor_data_interface.cc` — `CameraSource` enum, per-cam thread, `BlackFrameProvider`, RTSP/MIPI/file source routing
- `include/gst_mpp_decoder.h` + `src/gst_mpp_decoder.cc` — `BuildFilePipeline` / `BuildRtspPipeline` / `BuildMipiPipeline`, `DIAG` segment, watchdog, stride handling
- `include/camera_intrinsics.h` + `src/camera_intrinsics.cc` — K matrix storage and `params/calibration.yaml` I/O
- `include/roi_config.h` + `src/roi_config.cc` — `StitchGlobalConfig` (`RoiOffset[6]`), YAML I/O
- `include/roi_visualizer.h` + `src/roi_visualizer.cc` — SDL2 visual tuning + keyboard map
- `include/http_server.h` + `src/http_server.cc` — CameraPage HTTP API (port 8080)
- `include/mjpeg_streamer.h` + `src/mjpeg_streamer.cc` — `/api/stream` MJPEG producer/consumer bridge

## Observability hooks

- `stitch_status::update` → `/tmp/stitch_status.json` (500 ms cadence) for CameraPage
- `stitch_status::set_online(cam_idx, 0|1)` — per-cam online flag (RTSP watchdog)
- `Logger::SaveImage` — stitched frames and diagnostic frames under `results/`
- `[DIAG]` lines in `gst_mpp_decoder.cc` for first-frame caps/stride/plane-layout debugging
- GPU load: `cat /sys/class/devfreq/27800000.gpu/load`

## Common gotchas (do not relearn)

- **IOMMU exhaustion**: kill -9 on a running stitcher leaks DMA-BUF fds (`56644+` single-process); `out of I/O virtual memory -28` → all subsequent mppvideodec produces 0 frames. Always start with a clean process; clean up old PIDs before relaunching.
- **v4l2-ctl without `--stream-count`**: infinite streaming + `kill -9` → ISP queue deadlock. Reset is `media-ctl -d /dev/mediaN --reset` and relink, or reboot.
- **GC4683 has no built-in ISP**: must capture from `rkisp_mainpath` (`/dev/video66/75/84/93/102/111`), not raw `/dev/video0..`. The SoC-side `rkisp` does demosaic/3A/CCM/gamma.
- **NetworkManager is mandatory**: rocktech image has no `networking.service`. Use `nmcli`, not `ifupdown` / `/etc/network/interfaces`. NM `autoconnect-priority=100` is required for static IP to persist across reboot (see `docs/HISTORY.md` §2.3).
- **RK gles warper input cache fd poisoning**: `RkGlesWarper::WarpFrame` must NOT cache input by DMA-BUF fd; mppvideodec's pool recycles fds across cameras and stale cache hits assign cam N's EGLImage texture to cam M. Import per frame instead.
- **3D points above ground cannot be 2D-warped** — a moving person viewed by two cameras cannot be aligned with any 2D transformation. Background/foreground separation (seam + AANAP mix) is required, not just H + alpha blend.

## No-go zones / history not to revive

- `docs/NETWORK_CAMERA_PLAN.md` (v3.0 IP camera plan, 2026-07-08) — deleted
- `docs/RTSP_OUTPUT_PLAN.md` (v3.1 RTSP output) — deleted
- `docs/QUICK_START_IP_ONLY.md` (5-step "just change the IP") — deleted
- `tools/sprint0_*.sh` (Sprint 0 acceptance scripts) — deleted
- FFmpeg rkmpp decoder (vendor confirmed 0 frames) — never re-introduce
- `stitching_param_generater.cc` into the main loop — compile-time only
- DH / APAP / `cv::warpAffine` for moving subjects — geometrically impossible
- α-blend / multiband across full overlap — does not solve parallax
