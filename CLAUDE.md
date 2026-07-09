# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Real-time 6-camera video stitcher running on a Rockchip ARM64 board. Pipeline:

```
rtspsrc → rtph264depay → mppvideodec (DMA-BUF) → RGA crop/rotate → OpenCL seam feathering → DRM output
                         + (v3.2) → /stitch RTSP + /stitch_diff RTSP
```

- **Target boards**: rocktech RK3588 (primary), RK3576 (verified, 6× MIPI).
- **Input**: 6× IP cameras @ 2K (2560×1440) 30 fps, RTSP/TCP, through a PoE switch. Layout: **2×3 grid (2 cols × 3 rows)** → 4560×4080 panorama target.
- **Current phase**: v3.2 (RTSP output / frame diff) on top of v3.0 (IP camera ingress), which superseded v2.x (dataset mp4 / MIPI).
- **No unit tests, no linter, no CI** — only manual on-board validation.

## Quick start (on the board)

```bash
cd ~/Projects/new-4k-stitch    # /userdata/ is NOT a separate partition (see HISTORY §2.4)
mkdir -p build && cd build
cmake .. && make -j$(nproc)
./image-stitching
```

- Cross-compile is NOT used — `image-stitching` builds natively on the board with on-board gcc/cmake.
- Inputs are configured via `params/camera_sources.yaml` (6× `type: rtsp`). The repo also has legacy `INPUT_SOURCE_MODE=dataset` fallback for offline mp4 work.
- To run a one-shot IP-only smoke test, `bash tools/run_rtsp_stitching.sh` (overrides IP/user/pass from env or `cams.txt`).
- v3.0 acceptance gate: `bash tools/sprint0_smoke.sh` must PASS before declaring v3.0 done.

## Hard architecture constraints (locked)

These are not suggestions — they're load-bearing. Violating them has caused green-stripe bugs, decoder failures, and IOMMU exhaustion in the past. Detail: [`docs/HISTORY.md`](docs/HISTORY.md) §3.

| Layer | Required | Forbidden |
|---|---|---|
| Decode | `gstreamer1.0-rockchip1 mppvideodec` (DMA-BUF feature on) | FFmpeg rkmpp wrapper (vendor-unmaintained, 0-frame output); any sw decoder fallback |
| Frame buffer | DMA-BUF-backed GstBuffer; appsink caps = `video/x-raw(memory:DMABuf),format=NV12` | memcpy to system RAM (YUV420P / RGBA) |
| RGA path | librga for crop/rotate (affine only) | CPU OpenCV warpAffine |
| OpenCL | Mali GPU seam feathering with `clImportMemoryARM` (DMA-BUF → cl_mem cache keyed by fd) | CPU alpha blending |
| RTSP transport | `protocols=0x4` (TCP), `latency_ms=80–150` | UDP-only in production |
| Language | **C++11** (CMake-locked) | C++17 features — `std::filesystem` is unavailable, use POSIX `realpath()` |

Non-DMA-BUF frames from `appsink` are **rejected** in `gst_mpp_decoder.cc`; don't try to "just" make it work in system memory.

## Main loop data flow

`src/app.cc` → `App::run_stitching()` is the noreturn hot loop. Per frame:

1. `SensorDataInterface::get_image_vector()` — pulls 6× `NV12Frame` (each = DMA-BUF fd + width/height/stride + shared_ptr to GstSample keeping GstBuffer alive).
2. `ImageStitcher::WarpImages(img_idx, ...)` × 6 — RGA crop/rotate per cam into the panorama DRM buffer.
3. `ImageStitcher::BlendSeams(...)` — OpenCL feathered alpha-blend over the seam strips.
4. (v3.2, optional) `ExportNv12DrmBufferToBgr` → `GstRtspServer::PushBgrFrame("/stitch", ...)` and `FrameDiff::ComputeMask` → `PushBgrFrame("/stitch_diff", ...)`.
5. (Stage 2) downscaled MJPEG pushed to `mjpeg_streamer` for the CameraPage `/api/stream` live preview.
6. (Optional SDL2 debug) `RoiVisualizer` shows the panorama with ROI markers + keyboard controls.

Hot paths touch only DMA-BUF fds; BGR conversions go through RGA (`ExportHardwareFrameToBgr`, `ExportNv12DrmBufferToBgr`) — not OpenCV `cvtColor` on system memory.

## Module map (where to look first)

| Concern | File(s) |
|---|---|
| Main loop, ROI bootstrap, layout selection, RTSP output wiring | `src/app.cc`, `include/app.h` |
| RTSP/file pipeline, `mppvideodec` → appsink DMA-BUF, reconnect watchdog | `src/gst_mpp_decoder.cc`, `include/gst_mpp_decoder.h` |
| 6-cam decode threads, queue, fps reporting | `src/sensor_data_interface.cc` |
| RGA warp + OpenCL blend + DMA-BUF→`cl_mem` cache | `src/image_stitcher.cc` |
| EGL+GLES non-affine warp (optional, falls back silently) | `src/rk_gles_warper.cc` |
| ROI YAML, visualizer keyboard mapping, debug tuning | `src/roi_config.cc`, `src/roi_visualizer.cc` |
| CameraPage HTTP server (cpp-httplib, port 8080) + JSON status | `src/http_server.cc`, `src/status_writer.cc` |
| MJPEG panorama producer for browser preview | `src/mjpeg_streamer.cc` |
| Frame diff (NV12→BGR→absdiff→threshold→contours) | `src/frame_diff.cc` |
| RTSP server (gst-rtsp-server) for 2 output streams | `src/gst_rtsp_server.cc`, `include/gst_rtsp_server.h` |
| Output yaml parser (`output:` block) | `src/output_streams.cc` |

`src/stitching_param_generater.cc` is **initialized but not in the main pipeline** — ROI-driven layout is the active path. Don't "activate" it.

## 6-camera hardcoded locations

When refactoring anything that loops over cameras, these are the places that were extended from 4 → 6 in v2.3 and must stay consistent:

- `include/roi_config.h` — `RoiOffset roi_offsets[6]` and `StitchGlobalConfig`
- `src/app.cc` — `BuildDefaultTuning` (no longer capped at 4), `EstimateOverlaps2x3` / `BuildCameraRois2x3` / `BuildStitchLayout2x3`, `BlendSeams` (6 dispatch_seam)
- `src/roi_visualizer.cc` — Tab cycles over 6 cams
- `params/camera_sources.yaml` — 6 camera blocks (yaml is the canonical list; adding cams needs no C++ changes)
- `include/camera_intrinsics.h` — `Intrinsics::kNumCams = 6` (Sprint 0 placeholder; Sprint 2 calibration overrides)

YAML is the source of truth for camera count — new cams just need a new block in `params/camera_sources.yaml`.

## Conventions

- Classes PascalCase (`ImageStitcher`), functions snake_case (`load_parameters`), constants UPPER_SNAKE_CASE.
- Globals use `g_` prefix (`g_debug_level`, `g_feather_width`, `g_use_roi_config`).
- `g_feather_width` **must be even** (kernel divides by 2). Same constraint on per-camera crop widths via `NormalizeEvenFloor`/`NormalizeEvenCeil` in `src/app.cc:63-69`.
- Camera tuning offset arrays are indexed by **physical camera id**, not grid position.
- Camera intrinsics are placeholders: `fx=1027, fy=1378, cx=1280, cy=720` (2.8mm 102.5°/55.2° FOV — see `docs/NETWORK_CAMERA_PLAN.md` §0.5). Sprint 2 calibration will replace these.

## Environment variables that matter

| Var | Default | Effect |
|---|---|---|
| `INPUT_SOURCE_MODE` | unset (`dataset` fallback) | `camera` reads `params/camera_sources.yaml`; `dataset` uses legacy mp4 path |
| `SAVE_STITCH_FRAMES`, `SAVE_DIAGNOSTIC_FRAMES` | 1 | Set to `0` for FPS benchmarking |
| `USE_ROI_CONFIG` | 1 | Read `params/roi_tuning.yaml` at startup; 0 forces re-bootstrap |
| `SKIP_BOOTSTRAP` | 0 | Fixed-mount scenarios (e.g. 车载 GC4683): use YAML, but if missing still fall back to bootstrap + warn, do not exit. See HISTORY §2.7 |
| `STITCH_K_FOCAL_SCALE`, `STITCH_K_FX/FY_SCALE`, `STITCH_K_CX/CY_OFFSET` | — | Global K-matrix tuning; per-cam `STITCH_K_FOCAL_SCALE_CAM_0..5` |
| `STITCH_DEBUG_LEVEL`, `RK_GLES_WARPER_DEBUG_LEVEL` | 0 | Verbosity |
| `ENABLE_VISUAL_TUNING` | 1 | SDL2 panorama window (off the headless board) |
| `SHOW_ROI_MARKERS` | 1 | Overlay ROI rectangles in the visualizer |
| `MJPEG_INTERVAL`, `MJPEG_DOWNSCALE_W/H`, `MJPEG_QUALITY` | 2/960/816/75 | CameraPage live preview rate/size |

## Documentation (read these before changing code)

The codebase is sparse on inline docs but rich in `docs/`. Each one is the canonical source for its topic — don't duplicate it in this file:

- **[`AGENTS.md`](AGENTS.md)** — Agent entry point, naming conventions, env-var reference, SDL2 keymap.
- **[`docs/HISTORY.md`](docs/HISTORY.md)** — Design timeline, full operator manual (SSH/network, EGL checks, perf monitoring, ROI bootstrap semantics), and **the "踩过的坑" section §3 is mandatory reading before touching the decode or stitch path** (green-stripe diagnosis, IOMMU exhaustion, AFBC DRM_PRIME pitfalls, MMU/CL cache, multiple-RGA color-space waste, dataset quality).
- **[`docs/NETWORK_CAMERA_PLAN.md`](docs/NETWORK_CAMERA_PLAN.md)** — Active plan (v3.0): 6-cam IP RTSP改造, Sprint 0/1/2 acceptance, hardware specs, and `tools/sprint0_smoke.sh` script.
- **[`docs/RTSP_OUTPUT_PLAN.md`](docs/RTSP_OUTPUT_PLAN.md)** — v3.2 plan / current: 2× RTSP output streams + frame diff.
- **[`docs/CAMERA_PAGE_INTEGRATION.md`](docs/CAMERA_PAGE_INTEGRATION.md)** — In-process HTTP server (cpp-httplib, port 8080) + `/tmp/stitch_status.json` for the CameraPage browser UI.
- **[`docs/QUICK_START_IP_ONLY.md`](docs/QUICK_START_IP_ONLY.md)** — Five-step "change IPs and go" walkthrough.

## Common tasks

- **Verify a single camera path end-to-end** (without the full stitcher): `bash tools/rtsp_url_probe.sh`, then `gst-launch-1.0 rtspsrc location="..." latency=120 protocols=4 ! rtph264depay ! h264parse ! mppvideodec dma-feature=true format=NV12 ! "video/x-raw(memory:DMABuf),format=NV12" ! fakesink num-buffers=300`.
- **Diagnose a "green stripes" regression** — check `[DIAG]` lines in the log for `mem_count` and `layout`; see HISTORY §3 🟢 and the stride assertion in `src/gst_mpp_decoder.cc` (`actual_stride = buf_size / (height * 1.5)` overrides the gstreamer-reported stride).
- **Clean up a leaked/frozen stitcher** — `ps aux | grep image-stitching; kill -9 <pid>`. Stalled `image-stitching` accumulates DMA-BUF fds and exhausts the IOMMU (HISTORY §3 ⚫).
- **Watch stitch throughput** — `tail -f logs/image-stitching.log | grep -E "STITCH_PERF_FPS|decoder_perf"`. 6-cam target ≥ 25 fps steady-state.
- **Inspect live per-cam health** — `cat /tmp/stitch_status.json` or `curl http://localhost:8080/api/status`. `online` flips to 0 while a cam is reconnecting.
- **Tune ROI interactively** — visualizer keymap in `AGENTS.md`; press `M` to toggle ROI markers, `Tab` to cycle 6 cams, `E` to persist to `params/roi_tuning.yaml`.
