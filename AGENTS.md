# AGENTS.md

Target: keep agents out of the 2×2 → 2×3 (6-channel) migration rabbit holes. Read once, work faster.

## Build

```bash
mkdir build && cd build
cmake ..                        # CMake 3.10+, C++11
make
./image-stitching
```

- CMake options: `ENABLE_RK_HARDWARE_DECODING=ON`, `ENABLE_RGA_DMA_STITCHING=ON` (both default ON; both must stay ON for the zero-copy pipeline)
- FFmpeg root is hardcoded to `$ENV{HOME}/dev/ffmpeg60` in `CMakeLists.txt:10` — must exist on the target device with `PKG_CONFIG_PATH` and `LD_LIBRARY_PATH` set
- Requires: OpenCV >= 4.5, FFmpeg (rkmpp), OpenCL, EGL, GLESv2, GBM, librga, libdrm, SDL2
- **No tests, no linter, no CI.** Only manual verification on the Rockchip board (see "Target platform").

## Current status: v2 阶段 (4 路 GC4683 MIPI, 2026-06)

跟踪 9 阶段时间线 (见 `docs/HISTORY.md` 0x04 章节), 当前进度:

| 阶段 | 状态 | 备注 |
|---|---|---|
| 1. ISP 通路 + GC4683 sensor driver | **🚧 阻塞中** | 板子有 4 颗 GC4683 物理接好, **但当前 BSP 不带 GC4683 sensor driver**, rkisp0 输出 800x600 fallback. 需要找模组厂 / 移植 GC4653 / GitHub 搜社区 patch. **整个 v2 阶段的硬阻塞点** |
| 2. CameraSource 抽象 (YAML 驱动) | ✅ **已完成** | `include/sensor_data_interface.h` 加 `CameraSource`, `params/camera_sources.yaml` 4 路 mipi/file 配置, `InitVideoCapture` 按 `INPUT_SOURCE_MODE` 二选一 (`dataset` 走 t50..t53.mp4 默认, `camera` 走 yaml). 删 `g_is_using_camera` 死代码. |
| 3. V4L2 采集线程 (MIPI) | ⏳ 等阶段 1 | GC4683 驱动 + rkisp0 链路通后做 |
| 4. 软件时间戳同步 | ⏳ 等阶段 3 | 已在 `camera_sources.yaml` 预留 `sync_window_ms` |
| 5. 帧率配置 | ✅ **已完成** (YAML 写死 30fps) | sensor 物理 60fps 不用满 |
| 6. 标定 | ✅ **工具就绪** | `tools/calibrate_intrinsics.py` (Step A) + `calibrate_extrinsics.py` (Step B) + `undistort_preview.py`. 标定板: A3 棋盘 11×14, 方格 22mm, 哑光激光打印. 标定流程独立, 不阻塞其他阶段 |
| 7. 2×2 真机跑通 | ⏳ 等阶段 1 | ROI bootstrap 自动检测布局, 不依赖标定 |
| 8. 2×3 扩展 (6 路板) | ⏳ 远期 | 换支持 6 路 MIPI 的开发板后启动 |
| 9. 收尾归档 | ⏳ 等阶段 7 | 标定报告 + IQ 评估 |

**4 路 2×2 拼接真实跑通的硬条件**: 阶段 1 完成 (GC4683 驱动装上 + rkisp0 输出 4 路 2560x1440 NV12). 在此之前, 工程代码改动**不影响真机运行** (数据集路径仍走 t50..t53.mp4).

## Target platform

Linux ARM64. Code uses `/dev/dri/card0`, DRM ioctl, GBM, `mmap` — not portable to Windows/macOS for execution.

- **Primary target**: **RK3588** (declared by project owner).
- **Verified on**: RK3576 — current code paths (librga, EGL/GBM, rkmpp decoders) are exercised on a RK3576 board; RK3588 shares the same rknpu/mpp2 stack and is expected to run unchanged, but the codebase is **not yet validated on RK3588** hardware. Treat RK3576 as the de-facto reference until a fresh test cycle on RK3588 is recorded.

## v2 阶段 2 改动 (2026-06-25): 4K → 2K 适配

v2 阶段输入从 v1 时代的 4K (3840×2160) 改为 **2K (2560×1440)** @ 30fps. 已完成的具体改动:

| 位置 | v1 (4K) | **v2 (2K)** |
|---|---|---|
| `src/app.cc:192` `EstimateOverlapByTemplate` 搜索带宽 | `min(common_w/2, 1920)` (4K 中心线) | `common_w/2` (自适应, 2K 时 = 1280) |
| `src/app.cc:267` `EstimatePairOverlap` ORB 带宽 | 同上 magic number | 同上 |
| `src/roi_visualizer.cc:171` SDL 窗口标题 | `"4K Stitch ROI Tuner"` | `"2K Stitch ROI Tuner"` |
| `src/sensor_data_interface.cc` 3 处 `"../datasets/4k-test/"` | 4K 目录名 | **保留目录名 (字符串不变), 仅加注释说明 v1 时代命名遗留** |
| `params/camera_sources.yaml` 4 路 width/height | (无 yaml) | 2560×1440 (已写) |
| `tools/calibrate_intrinsics.py` 注释 | (无) | 2560×1440, fx ~ 1055 px (已写) |

**为什么 1920 magic number 改成 `common_w/2`**:
- 4K 时代 bug 修复 (v1): 上限 1200 不足, 改到 1920 (4K 中心线)
- v2 改用 `common_w/2`: 2K 输入时自然 = 1280, 4K 输入时自然 = 1920, 不再硬编码, 对任何分辨率自适应
- 2×3 迁移时**不需要重新推导上限**

## Architecture

Single executable `image-stitching`. Entrypoint: `src/app.cc:1049` `main()` → `App::run_stitching()` (`src/app.cc:950`, noreturn loop).

Pipeline: FFmpeg rkmpp HW decode → DRM_PRIME zero-copy → RGA crop/rotate (and optionally GLES warp) → OpenCL seam feathering → DRM output buffer.

### Key modules

| File | Role |
|------|------|
| `src/app.cc` | Main loop, ROI bootstrap, layout, debug globals |
| `src/sensor_data_interface.cc` | One decode thread per camera, queued frame supply |
| `src/image_stitcher.cc` | RGA/GLES warp, OpenCL seam blending, `dma_buf_cache_` |
| `src/rk_gles_warper.cc` | EGL+GLES warp via DMA-BUF import |
| `src/drm_allocator.cc` | DRM dumb buffer alloc/map/free |
| `src/stitching_param_generater.cc` | Camera calibration + warp map (initialized but **NOT** used in main pipeline) |
| `src/logger.cc` | Singleton logger, results dir creation |
| `src/roi_config.cc` | ROI offset YAML read/write (`params/roi_tuning.yaml`) |
| `src/roi_visualizer.cc` | SDL2-based interactive tuning: FPS overlay, ROI markers, keyboard controls |
| `include/nv12_frame.h` | DMA-BUF NV12 frame struct passed across modules |

### Pipeline modes (defined in `src/app.cc`)

1. **ROI + RGA + OpenCL** (default): multi-frame ROI → 2×2 layout → RGA crop/copy → OpenCL feather
2. **GLES Warp + RGA + OpenCL**: if warp data valid, GLES warp → RGA copy → OpenCL feather
3. Mode 2 falls back to mode 1 automatically on GLES init failure

## Current scope: 2×2 (4 cameras) — migration target 2×3 (6 cameras)

Hardcoded 4-cam assumptions. **Every one of these must change for the 2×3 (2H × 3V) migration.** Don't miss any:

| Location | What's hardcoded | What it should become (2×3 = 6 cams, 2 cols × 3 rows) |
|----------|------------------|-------------------------------------------------------|
| `src/sensor_data_interface.cc` `InitVideoCapture` | 4-file hardcoded `t50..t53.mp4` 列表, **`v2 阶段 2 已改为 CameraSource 列表驱动`** (fallback 仍是 t50..t53) | 6 entries (fallback t50..t55 即可); 阶段 2 完成后用 `params/camera_sources.yaml` 直接加 cam4/cam5 块 |
| `src/sensor_data_interface.cc:360` | `num_img_ = video_file_name.size()` | Already data-driven — verify vector sizes propagate |
| `include/roi_config.h:14` | `RoiOffset roi_offsets[4]` | Expand to 6 entries |
| `src/app.cc:172-180` `BuildDefaultTuning()` | Loops `for (size_t i = 0; i < num_cameras && i < 4; ++i)` reading `g_config.roi_offsets[i]` | Lift the `i < 4` cap; works for 6 once array is resized |
| `src/app.cc:369-440` `EstimateOverlaps2x2()`, `EstimateVerticalOverlap()` | Defines `h01 / h23 / v02 / v13` overlaps only — exactly 4 cameras in 2×2 | New function `EstimateOverlaps2x3()` covering 5 pairs: `h01, h12, h23, h34, v03, v14, v25, v36` ... verify with user (the 6 cams form a 2-col × 3-row grid, so there are 1 horizontal pair per row + 2 vertical pairs per column). Also the `CachedOverlap` struct in `include/app.h:21-35` must grow |
| `src/app.cc:442-534` `BuildCameraRois2x2()` | Computes per-camera `(x,y,w,h)` for a 2×2 layout (cut_x_left/right, cut_y_top/bottom) | New `BuildCameraRois2x3()` for 2 cols × 3 rows (cut_x_left/right + 2 horizontal seams = `cut_y_top/mid/bottom`); current function throws on `width < 2` — preserve that safety check |
| `src/app.cc:536-570` `BuildStitchLayout2x2()` | Output is `rois[0].width × rois[0].height` quadrant | New `BuildStitchLayout2x3()`; panorama height = sum of three row heights (with blend overlap), width = max of two col widths |
| `src/app.cc:592-749` `BootStrapOptimalLayout()` | Hardcodes `NUM_BOOTSTRAP_FRAMES = 3` and a 4-cam threshold check | Still works dimensionally, but the `[VALID] / [WEAK]` log lines print `h01_score, h23_score, v02_score, v13_score` — rename to the new overlap keys |
| `src/app.cc:711` `image_stitcher_.SetParams(blend_width, num_img_, w, h)` | `num_img_` passed as int | Already data-driven; just ensure 6 propagates |
| `src/image_stitcher.cc` (loop body in `WarpImages` & `BlendSeams`) | Iterates `for (i = 0; i < num_img_; ++i)` | Confirm per-camera buffers in `crop_buffers_`, `warped_buffers_`, `rotate_buffers_` size with `num_img_` |
| `src/roi_visualizer.cc` keyboard handlers | Tab = next cam, total of 4 cam IDs in HUD | Cycle through 6 cams; verify color/label rendering still works |
| `params/camchain_0.yaml` … `camchain_3.yaml` | 4 calibration files | Add `camchain_4.yaml`, `camchain_5.yaml` (or rename by grid position) |
| `params/roi_tuning.yaml` | `cam0..cam3` keys | Extend to `cam0..cam5`; `RoiConfig::LoadFromFile` parses a fixed-shape 4-cam block — must be made dynamic before old configs won't load |

The numbered `t30.mp4`–`t33.mp4` mention in the previous AGENTS.md is **outdated** — current default (when `INPUT_SOURCE_MODE` unset or `dataset`) is `t50.mp4`–`t53.mp4`. When `INPUT_SOURCE_MODE=camera`, `params/camera_sources.yaml` is required.

**v2 阶段 2 已完成**: `InitVideoCapture` 用 `INPUT_SOURCE_MODE` 二选一 (`dataset` 默认, `camera` 走 yaml). 阶段 3 之前, `camera` 模式下 yaml 的 `type: mipi` 会被跳过 (日志告警). 6 路扩展时只需在 yaml 加 cam4/cam5 块, 不动 C++ 代码.

## Naming conventions

- Classes: PascalCase (`ImageStitcher`)
- Functions: snake_case (`load_parameters`)
- Constants: UPPER_SNAKE_CASE
- Global vars: `g_` prefix + snake_case (`g_debug_level`, `g_feather_width`)
- Camera tuning offset arrays indexed by **physical camera number**, not grid position

## Debug switches (all in `src/app.cc` or `include/app.h`)

- `g_debug_level` (0=OFF, 1=INFO, 2=DEBUG, 3=VERBOSE)
- `g_debug_opencl_feathering`, `g_save_roi_confidence_debug`, `g_save_stitched_frames`
- `g_feather_width` (pixels, **must be even** — kernel divides by 2), `g_feather_strength` (S-curve, >1.0 = smoother)
- `g_multi_frame_roi_debug_level` (in `include/app.h:16`, default 1)
- `g_enable_visual_tuning` (env `ENABLE_VISUAL_TUNING`, default ON via `app.cc:30`)
- `g_show_roi_markers` (env `SHOW_ROI_MARKERS`, default 1; also toggled by `M` key in debug mode)
- `g_use_roi_config` (env `USE_ROI_CONFIG`, default 1 = read `params/roi_tuning.yaml`; 0 = force re-detect and overwrite)

## Environment variables

`SAVE_STITCH_FRAMES`, `SAVE_DIAGNOSTIC_FRAMES`, `SAVE_FRAME_INTERVAL`, `DIAGNOSTIC_FRAME_LIMIT`, `INPUT_SOURCE_MODE`, `STITCH_K_FOCAL_SCALE`, `STITCH_K_FX/FY_SCALE`, `STITCH_K_CX/CY_OFFSET`, `STITCH_K_FOCAL_SCALE_CAM_0..3`, `STITCH_DEBUG_LEVEL`, `RK_GLES_WARPER_DEBUG_LEVEL`

Visual tuning: `ENABLE_VISUAL_TUNING` (default 1=show SDL window), `SHOW_ROI_MARKERS` (1=draw ROI borders, default 1), `USE_ROI_CONFIG` (1=load `params/roi_tuning.yaml` on startup; 0=force auto-detect and overwrite config).

## Visual tuning (SDL2) keyboard map

| Key | Function |
|-----|----------|
| `↑`/`↓`/`←`/`→` or `W`/`A`/`S`/`D` | ROI step |
| `Tab` | Next/prev camera |
| `1` / `5` / `0` / `P` | Step size 1 / 5 / 10 / 50-`+` |
| `F` | Feather toggle |
| `+` / `-` | Feather width ±10 |
| `B` | Save toggle |
| `L` / `K` | Save interval ±10 |
| `E` | Save config to YAML |
| `M` | ROI marker toggle |
| `Q` / `Esc` | Quit debug mode |
| `Enter` / `D` | Enter debug mode |

Behavior: Entering debug mode (`Enter`) saves the current frame, locks frame index, pauses new frame fetch. `↑↓←→` re-stitches the saved frame live. `F/+/-` triggers full layout rebuild. `E` writes YAML. `Q/Esc` clears saved frames and resumes live stitching.

## Project upgrade roadmap (extracted from README history)

The README contains a chronological log of design iterations. Condensed for agent context:

1. **Initial (OpenCV UMat)**: Fast panorama via UMat, 4K × 4-cam > 200fps on 1080Ti. Pure CPU/GPU stitching using `cv::Stitcher`.
2. **2026-03-24**: Switched to 4K input; fixed `KMat`/`RMat` to avoid distortion; added runtime tuning env vars (`STITCH_K_*`).
3. **2026-03-26** (perf regression analysis): FPS dropped from 50+ to single digits. Root cause: warp changed from parallel to serial. Fixed by sequential warp submission, removing `clone/copyTo`, reducing GPU jitter. Further analysis identified OpenCL queue sync (`remap_finish` 60-150ms) as the next bottleneck; comparison vs. original showed extra sync points and log output were inflating the apparent cost.
4. **2026-04-01**: Integrated RK hardware decoding (`rkmpp` + RGA). Initial perf gain was wiped out by `NV12 → BGR → UMat` conversions → set the goal as DMA-BUF zero-copy.
5. **2026-04-03**: Switched bootstrap to OpenCV feature-based ROI detection; saved ROI coords; reused them per frame for crop+stitch (no `remap_finish`, RGA doesn't support it).
6. **2026-04-17 onwards**: Added SDL2 visualizer (FPS overlay, ROI markers, ROI step controls, debug mode entry/exit), ROI config YAML persistence (`params/roi_tuning.yaml`), multi-frame ROI bootstrap with confidence voting (NUM_BOOTSTRAP_FRAMES = 3, threshold 0.25, early-exit at 0.7).
7. **GLES warp path**: Tried `RkGlesWarper` for non-affine warp (RGA only does affine) via EGLImageKHR + DMA-BUF import. Falls back silently to ROI+RGA+OpenCL on init failure. Currently initialized but **not the active path** — ROI-driven layout is.
8. **Current**: default path is ROI bootstrap (multi-frame, best-of-N by confidence) → RGA crop/copy → OpenCL feather. Optional GLES warp is a transparent zero-copy accelerator; doesn't drive layout.

### Known issues / lessons learned (preserve during 2×3 migration)

- **NV12 ↔ RGBA is bandwidth-heavy** — multiple RGA round-trips are bad. Do crop/rotate in RGA, feather only the seam strip in OpenCL. Don't use GLES FBO + RGBA → NV12 conversion as a substitute.
- **MMU / IOMMU mapping cost**: every `clImportMemoryARM` + `clReleaseMemObject` rebuilds IOMMU page tables. The fix already implemented: `dma_buf_cache_` (`std::unordered_map<int, cl_mem>`) in `src/image_stitcher.cc`. Keys are DMA-BUF FDs, which the kernel pools to a small set (typically 4-16), so steady-state is O(1) hash lookup. Cleanup happens in `ImageStitcher::CleanupOpenCL()`. **For 2×3 the FD pool will roughly double; verify the cache still covers the working set.**
- **RGA is affine-only** (translate/scale/ortho-rotate; verified on RK3576, same librga API on RK3588). Cylindrical/spherical/perspective warp requires GLES fragment shader via EGLImageKHR → DMA-BUF. RGA only for the affine parts (crop, rotate, copy); GLES only for non-affine seam warp.
- **4K hardcoded search band bug** (历史, v2 已改为自适应): 早期 `src/app.cc:192` `search_w = NormalizeEvenFloor(std::min(common_w / 2, 1920))` 硬编码 1920, 4K 时代修复. v2 阶段 2K 输入 (2560x1440) 下改为 `search_w = common_w / 2` (即 1280), 对 2K/4K 都自适应, 不再依赖 magic number. 2×3 迁移时**不需要再重新推导上限**, 因为已经是分辨率自适应.
- **Linear (non-AFBC) DRM_PRIME is required** — `sensor_data_interface.cc:609` forces `afbc=0` because `av_hwframe_transfer_data` cannot read AFBC surfaces. Keep this when changing decoders.
- **Best datasets**: `t40` / `h40`. Worst: `t00`, `t30`, `t50` (low overlap / lighting shift). For 2×3 testing pick matching pairs.

## Operational playbook (collected steps from README)

### Decode pipeline checks (FFmpeg rkmpp)
- Confirm decoder: `./ffmpeg -decoders | grep rkmpp` should list `h264_rkmpp`, `hevc_rkmpp`, etc.
- Test decoder no AFBC: `./ffmpeg -hwaccel rkmpp -hwaccel_output_format drm_prime -i <video> -f null -`
- Test decoder with AFBC: `./ffmpeg -hwaccel rkmpp -hwaccel_output_format drm_prime -afbc 1 -i <video> -f null -`
- Confirm FFmpeg version linkage: `ldd ./image-stitching | grep avcodec` should show `so.60`.

### EGL environment sanity (verified on RK3576, applies to RK3588 — no desktop)
1. Check session: `echo "$DISPLAY" "$WAYLAND_DISPLAY" "$XDG_RUNTIME_DIR"`. If pure terminal with stale vars: `unset DISPLAY WAYLAND_DISPLAY`.
2. Check DRM nodes: `ls -l /dev/dri` — need `card0` and `renderD128`.
3. Check EGL/GLES libs: `ldconfig -p | grep -E 'libEGL|libGLESv2'`.
4. Inspect EGL caps: `eglinfo --display surfaceless` (install via `apt install -y mesa-utils mesa-utils-extra libegl1-mesa libegl1-mesa-dev libgles2-mesa libgles2-mesa-dev libgbm1 libdrm2 libdrm-dev`).
5. Check `[RkGlesWarper]` startup logs for `EGL_VENDOR`/`EGL_VERSION`/`EGL_CLIENT_APIS`, `EGL_EXTENSIONS` containing `EGL_KHR_image_base`, `EGL_EXT_image_dma_buf_import`, `EGL_KHR_gl_texture_2D_image`; should reach `using EGL_PLATFORM_SURFACELESS_MESA display path` or at least `eglInitialize succeeded`; `GL_RENDERER` should name the RK3576 GPU.
6. Pass criteria: `eglInitialize`, `eglCreateContext`, `eglMakeCurrent`, EGLImage entry points all succeed AND `EGL_EXT_image_dma_buf_import` is present.

### Hardware monitoring
- GPU load: `sudo cat /sys/class/devfreq/27800000.gpu/load`
- CPU: `htop`
- Historical bottleneck: decode/fetch ~121ms, stitch ~16ms — target is rkmpp → DMA-BUF → RGA/GPU zero-copy end-to-end.

### SSH / network
- Set static IP on host NIC: `sudo ifconfig end1 192.168.1.123 netmask 255.255.255.0 up` (replace `end1` with actual iface: `eth0`, `enp0s...`)
- Test: `ping 192.168.1.10`
- Connect: `ssh root@192.168.1.10` or PuTTY

### Performance optimization notes (from log analysis)
- Disable save-on-disk for pure FPS measurement (`SAVE_STITCH_FRAMES=0`, `SAVE_DIAGNOSTIC_FRAMES=0`); reveal decode-wait otherwise hidden by disk I/O.
- `STITCH_K_FOCAL_SCALE` = global "bulge/flat" adjust; `STITCH_K_FY_SCALE` = vertical bulge; `STITCH_K_FX_SCALE` = horizontal bulge. `STITCH_K_CY_OFFSET` = horizontal seam alignment; `STITCH_K_CX_OFFSET` = vertical seam alignment. Per-camera variants: `STITCH_K_FOCAL_SCALE_CAM_0..3` (extend to 5 for 2×3).

## Gotchas

- `assets/` contains **old/backup source files** — NOT part of the build, do not edit them expecting changes (the `.github/industrial-coding.instructions.md` mistakenly still says headers can live there — ignore that for new code).
- `StitchingParamGenerator` is initialized but its warp output is **not used** in the current main pipeline; ROI-based layout is active. Don't assume otherwise from the file's existence.
- NV12 format throughout: Y plane + interleaved UV. `stride_w`/`stride_h` may differ from `width`/`height` (check `nv12_frame.h`).
- DMA-BUF file descriptors are reused across frames; `image_stitcher.cc` caches `dma_buf_cache_` (hash map) to avoid remapping. **For 2×3 the FD pool will roughly double — sanity check cache hit rate.**
- `g_feather_width` must be even — kernel divides by 2. Same constraint applies to per-camera crop widths (see `NormalizeEvenFloor` / `NormalizeEvenCeil` in `app.cc:63-69`).
- `BuildCameraRois2x2()` throws `std::runtime_error("invalid 2x2 crop mapping for camera N")` if any ROI ends up `< 2×2`. The 2×3 equivalent must keep this safety throw; bootstrap will fail loudly instead of producing garbage.
- `BootStrapOptimalLayout` (multi-frame ROI) keeps the first frame as `image_vector_` even when it picks a later frame as best — that's intentional, the post-loop `ExportHardwareFrameToBgr` reuses the current `image_vector_` to save debug crops. Don't "fix" this by refetching.

## Existing instruction files

- `.github/industrial-coding.instructions.md` — coding standards, debug switch conventions, RK3576 adaptation guide (applies to `**/*.{cc,c,h}`)
- `.github/agents/industrial-coder.agent.md` — modification workflow (5-step: analyze → design → implement → verify → document)
- `.github/agents/code-assistant.agent.md` — read-only assistant (comments, README, directory cleanup, no logic changes)

## OpenCode session bookkeeping

- List sessions: `opencode session list`
- Resume: `opencode --session <ID>`
- Delete: `opencode session delete <ID>`