# USER_GOAL_ROADMAP.md
| 6-A/B | 否 | UI 扩展 + HTTP 端点 |
| 5-PRE | `src/stitching_param_generater.cc` + `include/stitching_param_generater.h` | 编译无 cv::detail::FeaturesFinder/FeaturesMatcher 链接错; 启动日志 `Features in image #N: ~4000`; 端到端 FPS 维持 100+ |
| 5-CAL-A | `src/calibration.cc/.h` (新) + `tools/calibrate_6cam.cpp` (新) + `params/calibration.yaml` (新) | `tools/calibrate_6cam` 跑通, 6 路 K/D/R/t 非零; 6 路 R\|t 两两相对姿态与机械装夹一致 (误差 < 1°) |
| 5-AANAP-A | `src/aanap_warp.cc/.h` (新) + `params/aanap_warp_0..5.png` (新) | 6 个 CV_32FC2 xmap/ymap 落盘; 启动日志 `[AANAP] H + T_sim estimated, 6 LUTs written`; 静态场景墙-地转角对齐误差 < 2 px |
| 5-AANAP-B | `src/image_stitcher.cc::WarpImages` (改 AANAP 路径) + `src/app.cc::InitFromConfig` (读 AANAP LUT) | 启动日志 `AANAP remap enabled, 6 LUTs loaded`; 静态墙-地转角目视 < 2 px 错位; FPS ≥ 60 |
| 5-SEAM-A | `src/app.cc::BootStrapOptimalLayout` (末尾加 GraphCut) + `params/warp_data.yaml` (加 `seam_default_0..6`) | 启动日志 `[App] seam 0..6 computed, avg width = XX px`; 7 个 seam PNG 落盘可读 |
| 5-SEAM-B | `src/image_stitcher.cc::BlendSeams` (OpenCL 硬切+窄带) + `src/calibration.cc` (LoadSeamMasks) | log `[BlendSeams] hard-cut + 3px narrow band`; 静态人跨 seam 不重影; FPS 80+ |
| 5-FG-A | `src/bg_subtractor.cc/.h` (新) + `src/image_stitcher.cc::PushSeam` (新) + `BlendSeams` kernel 切换 (动区单边, 静区 AANAP 混合) | log `[BgSubtractor] sliding-window bg model enabled, τ=30, α=0.01`; 走动人跨 seam 单边完整 + 静态墙-地转角自然过渡; FPS ≥ 60 |
| 5-FG-B | `src/seam_tracker.cc/.h` (新) | log `[SeamTracker] Kalman enabled, process_noise=2.0, max_disp=20`; 30 秒录像 seam 抖 < 2 px/frame |
| 5-SAL | `src/image_stitcher.cc` (累积 saliency) + `params/saliency_0..5.png` (新) | 1 周后 6 张 saliency PNG 落盘; 重新跑默认 seam 落在人流量低区 |
| 5-IPM (可选) | `src/calibration.cc::ComputeIpmLut` (新) + `params/ipm_lut_0..5.png` (新) | `--ipm` 启动后地面上 1m 棋盘格测得 100 ± 2 px; log `IPM mode enabled, 1 cm/pixel` |

**面向用户的实施计划(2026-07-14 启动)**。本文件代替已删除的 `docs/NETWORK_CAMERA_PLAN.md`(v3.0 IP camera)、`docs/RTSP_OUTPUT_PLAN.md`(v3.1 RTSP output) 和 `docs/QUICK_START_IP_ONLY.md` —— 那些都是过时方案,不对齐用户最新目标。

## 0. 用户原始目标(一句话)

> 6 路 GC4683 通过 **MIPI 接口**连到开发板; 2560x1440 分辨率; FOV 101x68°/路; 顶视接近 **180°** 俯视全景; 程序初始化时通过 **仿射变换** 等算法把 6 路投到 **同一平面**; 用 **特征识别** 确定拼接位置, **保存映射矩阵和拼接位置**; 后续程序正常运行 **直接复用**; 通过 **显式控制界面** 手动调整每路 **用作拼接区域的位置与长宽大小**。

技术关键词: 鱼眼畸变校正 / IPM / SIFT / SURF / ORB / RANSAC / 仿射变换 / 透视变换 / 单应矩阵 / 掩码 / 融合。
## 0.5 v3.4 核心架构原则: 前景/后景分离 (2026-07-14 重新规划)

> **本节是 § 2 Sprint 5 的设计基础, 也是回答"为何不直接用 H 估计 + alpha 融合"的判据。**

### 0.5.1 问题再描述

- **场景**: 6 路顶视摄像头 (室内), 相机固定, 背景静态, **人走动**。
- **症状**: 重影。 一个静止的人跨在重叠区时, 两路相机把他投到不同位置, 叠加 = "半透明 + 模糊"。
- **根因 (数学)**: 同一个 3D 点 P=(X,Y,Z) 投影到 cam0 像素 (u0,v0) 和 cam1 像素 (u1,v1), 这两个像素**没有任何一个 2D warp 能把它们映到同一全景像素还保持人形状不变** —— 除非 (a) Z=0 (在地面, IPM 才能两路对齐) 或 (b) Z=∞ (无穷远, 单 H 才能对齐)。**人的头顶既不在地面也不在无穷远, 所以 2D warp 必坏**。

### 0.5.2 解法: 前景/后景分离处理

| 类别 | 几何性质 | 实时策略 | 算力 |
|---|---|---|---|
| **后景 (静态地面 + 静态家具)** | 在地面 (Y=0) 或固定 Z, 离线标定后**两路天然对齐** | IPM 投到共享地面坐标系, **离线算好 LUT 实时查表** | ~3ms / 6 路 |
| **前景 (走动的人)** | 立体物, 2D warp 必坏, **任何对齐都是错的** | **不试图对齐**, 改"同一个人只由一台相机画": 重叠区硬切 (single-side), 检出人跨 seam 时把 seam 推开 | ~7ms / 7 pair |

### 0.5.3 纯 IPM 单平面拼接是否够?

**短答**: 对**地面 + 静态低位物**完美, 对**移动人**不够, 但**seam + 局部形变补齐**。

- ✅ **地面零重影** (瓷砖、地板, 同一地面点 IPM 后两路像素重合)
- ✅ **静态低位物 (桌椅脚, 离地 Z≈0)** 几乎零重影
- ⚠️ **静态高位物 (桌面, 椅背)** 仍有拖影 → **5-IPM-C 多平面 IPM** 子项目补
- ❌ **移动的人** IPM 后两路位置不同 → 必须**seam 物理隔离**

### 0.5.4 整条 pipeline 一句话

> **离线算好 IPM LUT + 默认 seam + 背景模型, 实时 IPM 查表 + 背景减除 + 局部 seam 形变 + 硬切合成, 算力留给前景**。

### 0.5.5 接受 / 拒绝的方案 (2026-07-15 修订, 复活 AANAP)

> **修订历史**: 2026-07-14 初版把 AANAP 列入"拒绝"。 2026-07-15 经 Zhihu 文章 + 用户反馈 + Zhihu 截图 (`docs/PixPin_2026-07-15_13-00-22.png`) 对比, 认识到 AANAP 对**静态多平面** (墙-地转角) 显著优于单 H, **不能拒绝**。 AANAP 不解决动态 parallax (立体人走动), 但和 seam 互补不互斥 — AANAP 管静区自然过渡, seam 管动区物理隔离。 详见 `docs/HISTORY.md` v3.4 段 "2026-07-15 AANAP 复活" 小节。

#### 接受 (主路径 / 关键支撑)

- ✅ **AANAP (Lin et al. 2015)**: 全局 H + per-image 相似变换, 保持直线 (墙保持直, 墙-地转角处自然过渡)。 用于**静态多平面场景**。 是 5-AANAP-A/B 主路径。
- ✅ **Seam-based 合成 (GraphCut + Voronoi)**: 重叠区每个像素归属一台相机, 物理上消除动区重影。 是 5-SEAM-A/B + 5-FG-A/B 主路径。
- ✅ **背景减除 (MOG2 / 滑动平均)**: 分离动静, 动区走 seam 单边, 静区走 AANAP 混合。 是 5-FG-A 关键。

#### 拒绝 / 降级 (写在前面防回退)

- ❌ **APAP (Zaragoza 2013)**: 网格级 H 算力太重 (init 100-300ms, 远超 30 fps 预算, 不适合 6 路 × 7 pair)。 直线保持不如 AANAP。 仅在 AANAP 仍不够时考虑。
- ❌ **DH 双单应 (Eisenschtat 2016)**: 仅 2 平面, 室内多平面 (墙-地-天花-家具) 不够; 算力 ~25ms 略重。
- ❌ **α 羽化 / 多频段全幅混合**: 重影的物理根源没解决, 只能压低不能消除, 对动区无效。 改用**窄带 ±3px** 在 seam 紧邻处过渡 (本质是 multiband 的 1D 简化版, 范围受限)。
- ❌ **CPU 软解 / 软件 fallback**: 架构硬约束禁止 (2026-07-06 锁定)。
- ❌ **AANAP / IPM 在 CPU 算**: RGA `imremap` 或 GLES fragment shader 走 GPU 零拷贝; CPU `cv::remap` 仅作最后 fallback。

#### 范围与例外

- **AANAP 也不能解决动态 parallax** (立体人走动)。 这种情况**seam 必须接管**: 检测到 fg_mask 跨 seam 时, 该区域走单边, 不用 AANAP。
- **纯 IPM 单平面** (Y=0) 仅作为"是否需要物理尺度 (1 pixel = X cm)"的可选, 不再是主路径。 需要物理尺度的场景才上 5-IPM 子段。

## 1. 当前代码能力快照(以代码为真, 2026-07-14)

| 已具备? | 来源 |
|---|---|
| **6 路 GC4683 MIPI 直连** | RK3576 DTS 已验证(`docs/HISTORY.md` § 1) |
| **2560x1440 @ 30fps 输入** | mppvideodec dma-feature=true |
| **6 路 2x3 layout 端到端跑通(dataset fallback)** | `datasets/2k-test-h264/t{50..53,40,41}.mp4` + `src/app.cc` |
| **多帧 ROI bootstrap**: ORB + BFMatcher (KNN + Lowe ratio) + RANSAC + `estimateAffinePartial2D` | `src/app.cc::EstimatePairOverlap` |
| **OpenCL 接缝羽化 + RGA crop/rotate** | `src/image_stitcher.cc::BlendSeams` |
| **`/api/roi` HTTP 改 yaml** + SDL2 调参(但 UI 仅 4 cam + 仅 offset) | `src/http_server.cc`, `src/roi_visualizer.cc` |
| **`stitching_param_generater.cc`** (OpenCV `cv::detail` 完整 pipeline, v3.3 ORB 改造完成) | 已编译; 默认 Affine/no-BA/PlaneWarper |

| 未具备(用户最新目标的要求) | 影响 |
|---|---|
| **FOV 101x68° 正确 K 矩阵**; `App::InitFromConfig` 未读 `camera_intrinsics.h` | 重投影 + IPM 不准 |
| **layout 横向/纵向 overlap 按 FOV+姿态算**; 当前 `BuildStitchLayout2x3` 硬编码 `W/8`, `H/12` | 顶视接近 ~180° 落空 |
| **仿射/IPM 投影到同一平面**; `WarpImages` 走 RGA 0/90/180/270 直拷, 没 warp map | 与俯视平面对齐不对 |
| **bootstrap 估出的 Affine / overlap 持久化**; `cached_overlaps_` 仅内存? | 复跑结果不稳定? |
| **`warp_data.yaml` 启动期直读**, 跳过 bootstrap | 启动期兜底?3 帧? |
| **6 路循环 + width/height 调整 UI**; `roi_visualizer.cc` 硬编码 4 cam + 仅 offset | 用户"显式控制界面"未达 |
| **`/api/roi` POST 接 x/y/w/h**; 当前仅 offset | API 与 UI 不对称? |

## 2. Sprint 划分(2026-07-14 起)

每段都是独立可验收的; 通过则进入下一段; 失败则回到上段或不阻塞主路径(降级到现有 dataset fallback)。

### 已完成的部分 (2026-07-15 状态)

- **Sprint 4-A K 矩阵重算**: `include/camera_intrinsics.h` (mutable storage) + `src/camera_intrinsics.cc` + `params/calibration.yaml` 默认 FOV 101x68°; App 启动期 LoadOrDefault。**已 commit (aa3d3b7)**。
- **Sprint 4-B layout 按 yaw 算**: `src/app.cc::BuildStitchLayout2x3` 改用 `fx*(tan(yaw_h[1])-tan(yaw_h[0]))` 等公式, 替换硬编码 `W/8` + `H/12`; CameraTuning 加 yaw_h_deg/yaw_v_deg, BuildDefaultTuning 从 camera_intrinsics::ForCam 拿。**已 commit (aa3d3b7)**。
- **Sprint 4-C 边缘日志**: `BuildCameraRois2x3` 末尾加每路 edges=[L,R,T,B] sanity log。**已 commit (aa3d3b7)**。
- **Sprint 4-MIPI 真接入 (NEW)**: `gst_mpp_decoder.cc::BuildMipiPipeline` (v4l2src+capsfilter NV12+appsink) + `sensor_data_interface.cc::BlackFrameHolder` + `BlackFramePushLoop` (30 fps 占位) + `params/camera_sources.yaml` 6 路 device_path 改 rkisp_mainpath; `tools/probe_v4l2.sh` + `tools/sprint4_mipi_bootstrap.sh` 新建。**HEAD 之后未 commit, 待用户板端验收**. 详见 `docs/BOARD_VERIFICATION_STATUS.md` § 1-§ 5。
- **Sprint 5-PRE ORB 改造**: stitching_param_generater.cc 加 OrbFeaturesFinder + OrbPairwiseMatcher, matcher_type="affine" 默认。**已 commit (aa3d3b7)** (Sprint 5 的 P0 前置)。


### Sprint 4: 几何 / K 矩阵对齐 (预计 1 周, 不动 stitcher 主路径)

**目标**: 把硬编码 "W/8 overlap" 换成 "按 FOV+姿态算的 overlap", 并让 K 矩阵与 sensor FOV 一致。

- **4-A. K 矩阵重算**:
  - `include/camera_intrinsics.h`: 占位 fx=1280/tan(50.5°) ~= 1052.6, fy=720/tan(34°) ~= 1068.9 (FOV 101x68° / 2560x1440)。允许单路微调; `ForCam(i)` 接口已留好。
  - `src/app.cc::InitFromConfig` 启动期读 yaml `params/calibration.yaml` 覆盖(如 yaml 缺失则用 FOV 反推默认)。当前 `params/camchain_0.yaml..3.yaml` 是 1080p 老数据, **全部放弃**, 换成新 yaml 格式(6 路各一份)。
  - 验收: `./image-stitching` 启动日志打印 `cam_i fx=... fy=...`。
- **4-B. 按 FOV 计算 2x3 overlap**:
  - `src/app.cc::BuildStitchLayout2x3` 把 `overlap_h = W/8`, `overlap_v = H/12` 替换为 `overlap_h = 2 * fx * tan((h_fov - yaw_h)/2)` 形式(由 K 与 yaw 算出)。
  - 通用化: 把 `BuildDefaultTuning` 加 `yaw_h` / `yaw_v` 字段, 默认 ±50°(横), 0°(纵)。
  - 验收: `docs/HISTORY.md` § v3.2 段直插一行: 实测 6 路 layout 后横向覆盖 ~178° (≈ 2×h_fov - 22.5° gap), 纵向覆盖 ~166° (≈ 3×v_fov - 38° gap)。
- **4-C. 边缘 / 顶部 / 底部 overlap 裁剪**:
  - `BuildCameraRois2x3` 增加边界裁剪: 防止 cam0 左侧 / cam1 右侧 / cam4 底部的黑边与 overlap。
  - 验收: 落盘 diag_pngs/ 能看到无黑边的 ROI。

### Sprint 5: AANAP (静态多平面) + Seam (动态) + 背景减除 合成 (预计 7-9 周, 主路径重构)

**核心原则 (v3.4 架构, 2026-07-14 重新规划, 2026-07-15 修正 AANAP 复活)**:
- **后景 (静态多平面, 墙-地-家具-天花板)**: 相机固定、场景静态, 离线估 **AANAP 全局 H + per-image 相似变换 T_sim**, 落盘 `params/aanap_warp_0..5.png` (CV_32FC2 xmap/ymap), 实时只做查表和混合, 算力几乎为 0
- **前景 (走动的人)**: **不试图把两路对齐** (几何不可能 — 立体人两台相机视线夹角不同, 任何 2D 变换都对不齐), 改为**保证同一时刻一个人只由一台相机画**: 重叠区不做 α/多频段混合, 用 seam 切开, 检出人跨 seam 时把 seam 局部推开
- **合成 kernel 按 fg_mask 切换**: 动区 (fg_mask>0) → seam 单边 (无重影, 物理隔离); 静区 (fg_mask=0) → AANAP 混合 (墙-地转角自然过渡) + seam ±3px 窄带隐藏切痕
- **纯 IPM 单平面 (Y=0) 拼接** 仅作为"是否需要物理尺度 (1 pixel = X cm)"的可选, **不再主路径**。 需要物理尺度的场景才上 5-IPM 子段。 默认走 AANAP, 因为 AANAP 对**多平面** (墙-地) 显著优于 IPM (见 `docs/HISTORY.md` v3.4 段 "2026-07-15 AANAP 复活" 修正)。

**架构总览**:

```
离线 (启动 / 周期刷新, 5-10 分钟):
  1. 标定 6 路 K + D + R + t + 离地高 H + 俯角 θ      → params/calibration.yaml
  2. 算 6 路去畸变 LUT: undistort_LUT_i[u,v] = (u',v')  → params/undistort_0..5.png
  3. 估 6 路 AANAP warp: 
     - 全局 H (ORB+RANSAC, v3.3 改造已 OK)             (per pair)
     - per-image 相似变换 T_sim (s, θ, tx, ty)          (per image)
     - 组合: p' = T_sim · (u,v) 然后 H · p'             → params/aanap_warp_0..5.png (CV_32FC2, 8MB/路)
  4. 算 7 对相邻默认 seam (GraphCut on 静态多帧)        → params/seam_default_0..6.png
  5. 累积 saliency heatmap (1 周运行后)                 → params/saliency_0..5.png
  (可选, 仅当需要物理尺度时) 5-IPM: 
     - 算 IPM LUT (CV_32FC2, Y=0 投到地面)                → params/ipm_lut_0..5.png

实时 (每帧, 6×2K @ 30fps, ~12-15ms):
  for each camera (并行, ~3ms wall):
    1. 去畸变: out = undistort_LUT[input]                ← 0.3ms (RGA colorkey/remap)
    2. 背景减除: fg_mask = |out - bg_model| > τ         ← 0.3ms (CPU OpenCV)
    3. AANAP warp: img_pan = aanap_remap[out]            ← 0.5ms (RGA imremap 接 CV_32FC2)
  cross-camera (~10ms wall):
    4. 局部 seam 形变: fg_mask 跨 seam 处把 seam 推开     ← 0.2ms/pair × 7 = 1.4ms (CPU)
    5. Kalman 平滑 seam 位置                              ← < 0.1ms
    6. 合成 kernel (OpenCL, 1ms/pair × 7 = 7ms):
       for each pixel in overlap:
         if fg_mask > 0:                                  ← 动区
           out = cam[seam_owner(pixel)]                   ← 单边, 物理无重影
         else:                                            ← 静区
           out = w · cam_a_pan + (1-w) · cam_b_pan        ← AANAP 混合, w = smoothstep(0,3,dist)
  per-camera:
    7. 背景模型慢更新: bg = α·out + (1-α)·bg (α=0.01)    ← 0.05ms
```

**算力账 (RK3588, 6×2K @ 30 fps)**: 总 wall clock ~12-15ms, 30 fps 预算 33ms, 余 18-21ms ✓。 跟现 v2.5 100+ fps 比加 ~5-7ms, 仍在 65-100 fps, 远超 30 fps 要求。

**Sprint 子段**:

- **5-PRE. SIFT -> ORB 改造 (前置, 本周必做, 不动 RGA+OpenCL 栈)**:
  - **目标**: 把 `src/stitching_param_generater.cc` 内置的 SIFT 描述子整体切到 ORB, 保持现有 `RGA crop/rotate + OpenCL 接缝羽化` 零拷贝栈不变。
  - **改动**:
    1. `src/stitching_param_generater.cc` 新增内部类 `OrbFeaturesFinder` (继承 `cv::detail::FeaturesFinder`, 使用 `cv::ORB::create(4000)` 检测+计算 binary 描述子) + `OrbPairwiseMatcher` (继承 `cv::detail::FeaturesMatcher`, 用 `BFMatcher(NORM_HAMMING) + knnMatch(2) + Lowe ratio 0.65`)。
    2. `InitCameraParam()`: `SIFT::create()` 替换为 `makePtr<OrbFeaturesFinder>(4000)`; `AffineBestOf2NearestMatcher` / `BestOf2NearestMatcher` / `BestOf2NearestRangeMatcher` 全部替换为 `makePtr<OrbPairwiseMatcher>(match_conf)`。描述子字节布局对 `AffineBasedEstimator` / `HomographyBasedEstimator` / `BundleAdjuster*` 透明, 不动 estimator / BA 入口。
    3. `include/stitching_param_generater.h` 默认值改: `matcher_type="affine"` (was "homography"), `estimator_type="affine"` (was "homography"), `ba_cost_func="no"` (was "reproj"), `warp_type="plane"` (was "spherical")。贴 GC4683 顶视平面 + 6 路固定支架 + Affine 估计场景, 启动后 `cv::detail::PlaneWarper` 算出的 xmap/ymap 正好是 AANAP 友好形式。
  - **硬约束 (不破)**: RGA crop/rotate 不动; OpenCL 接缝羽化不动; 描述子类型硬编码 ORB binary; 匹配距离硬编码 NORM_HAMMING。
  - **不动**: `src/app.cc::EstimatePairOverlap` / `BootStrapOptimalLayout` / `src/rk_gles_warper.cc` / `src/gst_mpp_decoder.cc` / `src/drm_allocator.cc` / `src/roi_*.cc` / `src/http_server.cc`。
  - **风险 / 兜底**: ORB 在低纹理场景失配 → `src/app.cc::EstimatePairOverlap` 的 `EstimateOverlapByTemplate` 模板匹配兜底仍生效。 启动时间从 ~600ms 降到 ~200ms。
  - **验收**:
    1. `cmake --build build && ./build/image-stitching` 不报 `cv::detail::FeaturesFinder` / `FeaturesMatcher` 链接错。
    2. dataset 路径启动日志打印 `Features in image #N: ~4000` (代替 SIFT 的 ~1500)。
    3. 端到端 FPS 维持 100+。

- **5-CAL-A. 6 路标定 (1.5 周, 离线)**:
  - **目标**: 标定 6 路相机内参 (K, D) + 外参 (R, t) + 离地高 H + 俯角 θ; 供 AANAP 和 IPM 共用。
  - **新增文件**:
    - `src/calibration.cc/.h` (新): 6 路 K/D/R/t/H/θ 加载 + save; `CalibrationConfig::LoadOrDefault` (已有 camera_intrinsics.cc 同名函数, 合并 / 重命名)。
    - `tools/calibrate_6cam.cpp` (新): 棋盘格采集 + `cv::solvePnP` 标定工具。 输出 `params/calibration.yaml`。
  - **算法**:
    ```
    for each cam (0..5):
      采集 20+ 角度棋盘格 (multi-resolution)
      标定 K, D via cv::calibrateCamera
      solvePnP 标定 R, t (相对世界原点, 棋盘格放地面当世界系)
      H = -t.y / R.row(1)             // 离地高
      pitch = -atan2(R.row(1).z, R.row(1).y)  // 俯角
    ```
  - **不动**: 主循环 / 解码 / OpenCL blend / GLES warper。
  - **风险 / 兜底**:
    - 标定精度差 → 棋盘格 20+ 角度, 重投影误差 < 1 px 才接受, 6 路间两两验证 R|t 物理一致 (相机间相对姿态已知)。
    - 6 路标定不收敛 → 分组标定, 重叠多的先标。
  - **验收**:
    1. `tools/calibrate_6cam --chessboard 11x8 --square 30mm --cams 6` 跑通, 6 路 K/D/R/t 非零。
    2. 6 路 R|t 两两相对姿态与机械装夹一致 (误差 < 1°)。
    3. yaml 落盘 `params/calibration.yaml`。

- **5-AANAP-A. AANAP 全局 H + per-image T_sim 估计 (1.5 周, 离线)**:
  - **目标**: 估 6 路相机 AANAP warp = 全局 H (per pair) + per-image 相似变换 T_sim (per image), 输出 CV_32FC2 xmap/ymap LUT。
  - **新增文件**:
    - `src/aanap_warp.cc/.h` (新): `ComputeAanapLut(cam_i, cam_j, K, D, R, t) -> (lut_i, lut_j)`; 估 H + T_sim; 输出 CV_32FC2 xmap/ymap。
  - **算法 (Lin et al. 2015 简化版)**:
    ```
    1. 离线采集: 多帧静态 (无人在场) 6 路同步触发
    2. ORB 配准 (v3.3 改造已 OK) per pair: 用 BFMatcher(NORM_HAMMING) + Lowe 0.65
    3. 估全局 H: cv::findHomography(pts_j, pts_i, RANSAC, 3.0)
    4. 估 per-image T_sim: 
       T_sim = [s·cosθ  -s·sinθ  tx;  s·sinθ  s·cosθ  ty;  0  0  1]
       拟合 4 参数 (s, θ, tx, ty), 优化:
         min Σ ||T_sim · H · p_j_k - p_i_k||^2
         + λ · ||T_sim - I||^2     // content-preserving 正则, 保直线
    5. 算 LUT:
       for (u, v) in cam_j:
         p_local = T_sim · (u, v, 1)^T
         p_global = H · p_local
         lut_j.at<Vec2f>(v, u) = (p_global.x, p_global.y)
    ```
  - **不动**: 主循环 / 解码 / OpenCL blend。
  - **风险 / 兜底**:
    - T_sim 估偏 → 加大 λ (更保直线), 或 fallback 用单 H 估 (无 T_sim)。
    - 估 T_sim 时 outlier → RANSAC + 仅用 H 的 inliers 拟合。
  - **验收**:
    1. `params/aanap_warp_0..5.png` 落盘, 6 个 CV_32FC2, 每路 ~8MB。
    2. 启动日志 `[AANAP] H + T_sim estimated, 6 LUTs written`。
    3. 静态场景下 (无人在场), 拼接后墙-地转角处两路对齐误差 < 2 px。

- **5-AANAP-B. AANAP warp 实时应用 (1 周, 在线)**:
  - **目标**: 实时跑通 AANAP remap, 用 RGA `imremap` 接 CV_32FC2 aanap_lut, 替代原 RGA 0/90 旋转 + BlitByRect 路径。
  - **改动**:
    1. `src/image_stitcher.cc::WarpImages` 加 AANAP 路径: 当 `params/warp_data.yaml` 含 `aanap_warp_i` 时, 用 RGA `imremap` 模式接 CV_32FC2 xmap/ymap。
    2. `src/app.cc::InitFromConfig` 启动时读 `aanap_warp_0..5.png`, 喂给 `ImageStitcher::SetAanapLuts(...)`。
    3. 集成 kalman 之前的过渡: 5-AANAP-B 不带 fg_mask, 纯 AANAP 混合 (类似现 α blend), 跟 5-SEAM-B 衔接后才接 seam 单边。
  - **不动**: 解码 / RGA 主路径 / OpenCL blend (5-SEAM-B 之前不动) / 持久化。
  - **风险 / 兜底**:
    - RGA `imremap` 不支持 CV_32FC2 → 退到 GLES fragment shader 走同一 LUT (Mali GPU 走 EGL 路径也是 0 copy), 性能损耗 < 2ms。
    - 标定文件缺失 → fall back 到原 H 估计路径, log 警告。
  - **验收**:
    1. 启动后日志 `AANAP remap enabled, 6 LUTs loaded`。
    2. 静态场景下拼接图, 墙-地转角处无明显错位 (目视 < 2 px)。
    3. 端到端 FPS ≥ 60 (留余量给后续 seam 改造)。

- **5-SEAM-A. GraphCut 默认 seam (1 周, 离线 + 在线混合)**:
  - **目标**: 启动时跑一次 GraphCut 求 7 对相邻的默认 seam mask, 落盘 yaml。
  - **改动**:
    1. `src/app.cc::BootStrapOptimalLayout` 末尾加 `seam_finder->find(warped_aanap_images, warped_masks)` 用 `cv::detail::GraphCutSeamFinder(COST_COLOR_GRAD)`。
    2. 7 对相邻 (2x3 布局): 3 横 (cam0-1, cam2-3, cam4-5) + 2 左列纵 (cam0-2, cam2-4) + 2 右列纵 (cam1-3, cam3-5)。
    3. 落盘 `params/warp_data.yaml`: `seam_default_0..6` (8-bit 单通道 PNG, ~20MB/个, 压缩存)。
  - **风险 / 兜底**:
    - GraphCut 在 4800x4080 跑 50-100ms, 但只在 init / refresh 跑, 不影响实时。
    - 弱纹理区 seam 跳来跳去 → cost 加 Canny 边缘 + (若 5-SAL 已上) saliency。
  - **验收**:
    1. 启动日志 `[App] seam 0..6 computed, average width = XX px`。
    2. `params/warp_data.yaml` 7 个 PNG 可读。

- **5-SEAM-B. OpenCL 硬切 + 窄带合成 (1 周, 在线)**:
  - **目标**: 替代原 α 羽化为 seam 硬切 (主) + 窄带 3px 渐变 (缝隐藏), 立即消除移动物重影。
  - **改动**:
    1. `src/image_stitcher.cc::BlendSeams` 改 OpenCL kernel: 加 `seam_mask_buffer` 输入 (8-bit R8), 输出 = `smoothstep(0, 3, dist_to_seam) * cam_a + (1-...) * cam_b`。
    2. `ImageStitcher` 加 `seam_masks_[7]` 成员 (cv::Mat, 单通道), `SetLayout` 多接 `seam_masks` 参数。
    3. `src/calibration.cc` 加 `LoadSeamMasks(yaml_path) -> array<Mat, 7>`。
  - **不动**: 解码 / RGA crop / AANAP remap / 持久化。
  - **风险 / 兜底**:
    - 硬切在曝光/白平衡不同时可见 → 窄带扩到 5 px, 视觉几乎不可见。
    - seam mask 单帧硬切不跟随人 → 由 5-FG-A 局部形变补; 本段先验证静态 seam + AANAP 混合效果。
  - **验收**:
    1. log `[BlendSeams] hard-cut + 3px narrow band, 0 alpha`。
    2. 室内有 1 个静止的人跨在 seam 上 → 人在画面里完整, 没有"半透明 + 重影"。
    3. 端到端 FPS 维持 80+ (硬切省 α 算力, 实际比 α 略快)。

- **5-FG-A. 背景减除 + 局部 seam 形变 + 合成 kernel 切换 (1.5 周, 在线, 关键合成点)**:
  - **目标**: 检测人跨在 seam 上时, 把 seam 局部推开; 合成 kernel 按 fg_mask 切换 (动区 seam 单边, 静区 AANAP 混合)。
  - **改动**:
    1. `src/bg_subtractor.cc/.h` (新): 简化版 MOG2 / 滑动平均, 输出 0/255 fg_mask。 6 路线程安全 (各路独立 bg_model)。
    2. `src/image_stitcher.cc::PushSeam` (新成员): 接收 fg_mask (投到全景后) + 当前 seam, 用距离变换 + 形态学推开 seam, 输出新 seam_mask。
    3. `ImageStitcher::WarpImages` 之后: 把单路 fg_mask 通过 AANAP LUT 投到全景, 喂给 `PushSeam`。
    4. `ImageStitcher::BlendSeams` kernel 改为:
       ```
       if (fg_mask(uv) > 0.5)  // 动区
         out = cam[seam_owner(uv)]  // 单边
       else  // 静区
         out = w * cam_a + (1-w) * cam_b  // AANAP 混合, w = smoothstep(0,3,dist)
       ```
  - **算法**:
    ```
    for each pair:
      fg_pan = aanap_remap(fg_mask_a) | aanap_remap(fg_mask_b)  // OR
      dist_to_seam = distanceTransform(seam == 1 ? 0 : 255)
      push_zone = (dist_to_seam < PUSH_WIDTH) & (fg_pan > 0)
      seam_pushed = push_seam_around_fg(seam, push_zone, PUSH_WIDTH=20px)
    ```
  - **风险 / 兜底**:
    - 背景减除误报 (室内阴影/灯光) → 慢更新 α=0.01, 强光突变 frame 跳过背景更新。
    - 形变跟不上快速走动 → 单帧 max displacement clamp 20 px + 5-FG-B Kalman。
  - **验收**:
    1. 启动日志 `[BgSubtractor] sliding-window bg model enabled, τ=30, α=0.01`。
    2. 室内有人走动跨 seam, 全景图里这个人**不重影** (动区走 seam 单边)。
    3. 室内墙-地转角处静态物体**自然过渡** (静区走 AANAP 混合)。
    4. 端到端 FPS ≥ 60 (开背景减除后损耗 ~3ms)。

- **5-FG-B. Kalman 平滑 + 帧间差 cost (0.5 周, 在线)**:
  - **目标**: 抑制 seam 抖动, 让局部形变有方向性 (不每帧跳)。
  - **改动**:
    1. `src/seam_tracker.cc/.h` (新): 简单 Kalman 滤波器, 状态 = `[seam_x, velocity_x]` (按 y 索引的 1D seam)。
    2. `ImageStitcher::BlendSeams` 之前: 用 raw_seam + prev_seam 喂 Kalman, 输出 filtered_seam。
    3. (可选) 帧间差 cost: `cost = color_diff + edge + frame_diff * 0.5` 加到 GraphCut, 让动态 seam 跟运动物。
  - **风险 / 兜底**:
    - Kalman 滞后 → process noise 调大点 (σ=2 px), 折中响应 vs 平滑。
    - 单帧异常跳变 → clamp max displacement 20 px。
  - **验收**:
    1. log `[SeamTracker] Kalman enabled, process_noise=2.0, max_disp=20`。
    2. 录像 30 秒对比: 不开 Kalman 缝抖 5-10 px/frame, 开了 < 2 px/frame。

- **5-SAL. Saliency heatmap (0.5 周, 离线累积)**:
  - **目标**: 1 周运行后生成"人常出现位置"概率图, 给 GraphCut cost 用, 让默认 seam 避开人流量高区。
  - **改动**:
    1. 每帧 (5-FG-A 之后): `saliency[i] += 0.001 * fg_mask[i]_smoothed`, 慢累加, 自动归一化。
    2. 落盘 `params/saliency_0..5.png` (CV_32FC1)。
    3. 5-SEAM-A 启动期读 saliency 作为 GraphCut 附加 cost。
  - **验收**:
    1. 1 周后 `params/saliency_0..5.png` 落盘, 视觉上人常走的路径高亮。
    2. 用 saliency 加权后, 重跑的默认 seam 落在人流量低区 (imgviz 抽检)。

- **5-IPM. (可选) IPM 地面尺度 (1.5 周)**:
  - **触发条件**: 用户需要"1 pixel = X cm"物理尺度 (在地面上量距离 / 越界检测 / 鸟瞰图) 时才上。
  - **目标**: 算 6 路 IPM LUT (CV_32FC2, Y=0 投到地面), 启动时叠加到 AANAP 路径 (优先级低, 仅用于物理尺度补强)。
  - **新增文件**: `src/calibration.cc::ComputeIpmLut` (与 AANAP 共享标定)。
  - **改动**: `src/image_stitcher.cc::WarpImages` 加 IPM 模式: 在 AANAP 之后叠加 IPM 平移 (从相机坐标系到地面坐标系)。 仅 5-AANAP-B 已完工 + 用户明确需要时再开。
  - **不动**: AANAP 路径 / seam / 背景减除。
  - **风险 / 兜底**:
    - IPM 与 AANAP 路径冲突 → IPM 仅在 5-IPM 用户显式开启时叠加, 默认关闭。
  - **验收**:
    1. `./image-stitching --ipm` 启动后, 地面上 1m 棋盘格测得 100 ± 2 px (1cm/pixel)。
    2. 启动日志 `IPM mode enabled, 1 cm/pixel, panorama 4800x4080`。
### Sprint 6: UI 可视化调参扩展 (预计 1 周)

**目标**: 用户"显式控制界面可以手动调整每一路视频的保留的用作拼接区域的位置与长宽大小"真正落地。

- **6-A. `roi_visualizer` 4 cam → 6 cam + width/height 控制**:
  - `src/roi_visualizer.cc`: 扩 `CAM_COLORS_BGR[6]`, `CAM_LABELS[6]`, `CAM_POSITIONS[6]`; `DrawROIMarkers` `for (int i = 0; i < 6; ++i)`; `HandleDebugAction` 用 `g_config.selected_cam % 6` (当前硬编 `% 4`)。
  - 新增 `kVisStepWidthUp/Down`, `kVisStepHeightUp/Down` action; 键位映射: `Ctrl+Right/Left` 改 width, `Ctrl+Up/Down` 改 height。
  - `src/roi_config.h`: `RoiOffset roi_offsets[6]` → `CameraRoiRect camera_rois[6] { int x, y, w, h };`。 `StitchGlobalConfig::feather_width/strength/save_*/...` 保留。
  - `Save/LoadFromFile` yaml schema 扩展: 每路 cam 按 `x:`, `y:`, `w:`, `h:` 4 个字段; 兼容旧 `offset_x/offset_y` (写时写新格式, 读时识别两种)。
  - 验收: 启动期 ROI bootstrap → 4 帧后保存 (debug + E 键); 重启后直读 `params/roi_tuning.yaml` 跳过 bootstrap, ROI 6 路 per-cam 持久化生效。
- **6-B. `/api/roi` POST 接 x/y/w/h**:
  - `src/http_server.cc::handle_roi_post` 把当前仅识别 `offset_x/offset_y` 扩到识别 `x/y/w/h` (兼容旧的 offset_* 不报错)。
  - 前端 (CameraPage, `scripts/camerapage_server.py`) 改 ROI 模块把 4 字段发送到 `/api/roi`。
  - 验收: `curl -X POST .../api/roi -d '"'"'{"cam":2,"x":100,"y":50,"w":2200,"h":1300}'"'"'` 后 yaml 落 4 个字段。
- **6-C. 视角 / 比例控件 (可选)**:
  - 加 `Ctrl+Shift+方向键` 改 `yaw_h` / `yaw_v`, 让用户在没有 IPM 时也能调单相机的横偏/纵偏 = 简单绿幕变换?。

### Sprint 7: 集成验证 / 多角度回归 / 文档同步 (预计 0.5-1 周)

**目标**: Sprint 4-6 完成后, 端到端, 写文档。

- **7-A. 端到端验收脚本**: `tools/sprint7_e2e.sh` 检查启动日志 `[ImageStitcher] GLES warper initialized.` 或 RGA fallback, FPS ≥ 25, 启动时间 < 5s, 6 路 `decoder_perf` ≥ 25。
- **7-B. CameraPage 一键截图三对比**: 抓启动期 vs 运行期 vs 调参后 3 张全景, 检查无明显错位。
- **7-C. docs 同步**: 把本文件 § 2 中每个 Sprint 状态置 OK, 移入归档段。
- **7-D. 180° 俯视覆盖检查**: 拿一张已知尺寸的标定面板 (横/ A4 / 竖线), 在 GIMP 像素级比对, 满足 ≥ 96% 覆盖 ≈ 180°。

## 3. 风险 / 兜底

- **5-CAL-A 标定精度差**: 棋盘格 20+ 角度 solvePnP, 重投影误差 > 1 px 的路剔除后重标; 6 路 R\|t 两两相对姿态与机械装夹误差 > 1° 时降回 H 估计路径, log ERROR。
- **5-AANAP-A T_sim 估偏 (内容保持正则化不足)**: 加大 λ (保直线权重); 实在不行 fallback 用单 H 无 T_sim。 估 T_sim 时仅用 H 的 inliers 拟合, RANSAC 抗噪。
- **5-AANAP-B RGA 不支持 CV_32FC2 imremap**: 退到 GLES fragment shader 接同一 LUT (Mali GPU 走 EGL 路径也是 0 copy), 性能损耗 < 2ms; 仍不行则 OpenCV `cv::remap` CPU fallback (再 +2-3ms, 仍 30 fps 内)。
- **5-AANAP-B AANAP 估的 T_sim 把墙弄歪了**: 排查 T_sim 训练数据 (要求静态无人在场), 调 λ 加大; 实在不行 T_sim 退化为 identity (退到单 H)。
- **5-IPM (可选) 多平面 Z 重叠错位**: 标定 `cam_i.z_planes` 显式给, 默认 Z 越大优先级越低防穿透; 标定缺失的路只投 Z=0 地面。
- **5-SEAM-A GraphCut 在弱纹理区 seam 跳**: cost 加 Canny 边缘 + (若 5-SAL 已上) saliency heatmap; 实在不行 VoronoiSeamFinder 替代, 1ms 更快但 seam 略差。
- **5-SEAM-B 硬切在曝光/白平衡不同时可见**: 窄带扩到 5 px 仍几乎不可见; 真正曝光差异大时, 5-FG-A 之前加 gain compensation (离线 6 路白平衡标定)。
- **5-FG-A 背景减除误报 (室内阴影/灯光突变)**: 慢更新 α=0.01, 强光突变 frame 跳过背景更新 (亮度跳变 > 阈值时本帧 fg_mask 全 0)。
- **5-FG-A 形变跟不上快速走动**: 单帧 max displacement clamp 20 px; 5-FG-B Kalman 加速度项, 静态稳定, 动态跟得上。
- **5-FG-B Kalman 滞后 vs 抖动**: process noise σ=2 px 是折中; 真嫌滞后可调 σ=4, 嫌抖就 σ=1。
- **5-SAL saliency 累积错误**: 每帧 +0.001 (慢), 异常跳变 frame 不累加 (同 5-FG-A 跳变检测); 1 周后归一化 / max。
- **5-PRE ORB 失配**: 见 5-PRE "风险 / 兜底" 段 (EstimateOverlapByTemplate 模板匹配兜底)。
- **4-B 180° 覆盖不达标**: 可能 GC4683 机壳排布本身就做不到 180°。回退: 用户把 cam 间夹角调更紧 (机械调整), 或接受 ~150-160° 覆盖 (用户提供可调参数)。回退: `yaw_h / yaw_v` 暴露给 UI。

## 4. 每个 Sprint 的 PR 边界 (建议)

| Sprint | 触动文件 | 可验收 (doctest-like) |
|---|---|---|
| 4-A | `include/camera_intrinsics.h` + `src/app.cc` + `params/calibration.yaml` (新) | 启动期日志打印 fx/fy |
| 4-B | `src/app.cc::BuildDefaultTuning / BuildStitchLayout2x3` | 落盘 diag PNG 量化 overlap |
| 4-C | `src/app.cc::BuildCameraRois2x3` | diag PNG 无黑边? |
| 5-PRE | `src/stitching_param_generater.cc` + `include/stitching_param_generater.h` | 编译无 cv::detail::FeaturesFinder/FeaturesMatcher 链接错; 启动日志 `Features in image #N: ~4000`; 端到端 FPS 维持 100+ |
| 5-CAL-A | `src/calibration.cc/.h` (新) + `tools/calibrate_6cam.cpp` (新) + `params/calibration.yaml` (新) | `tools/calibrate_6cam` 跑通, 6 路 K/D/R/t 非零; 6 路 R\|t 两两相对姿态与机械装夹一致 (误差 < 1°) |
| 5-AANAP-A | `src/aanap_warp.cc/.h` (新) + `params/aanap_warp_0..5.png` (新) | 6 个 CV_32FC2 xmap/ymap 落盘; 启动日志 `[AANAP] H + T_sim estimated, 6 LUTs written`; 静态场景墙-地转角对齐误差 < 2 px |
| 5-AANAP-B | `src/image_stitcher.cc::WarpImages` (改 AANAP 路径) + `src/app.cc::InitFromConfig` (读 AANAP LUT) | 启动日志 `AANAP remap enabled, 6 LUTs loaded`; 静态墙-地转角目视 < 2 px 错位; FPS ≥ 60 |
| 5-SEAM-A | `src/app.cc::BootStrapOptimalLayout` (末尾加 GraphCut) + `params/warp_data.yaml` (加 `seam_default_0..6`) | 启动日志 `[App] seam 0..6 computed, avg width = XX px`; 7 个 seam PNG 落盘可读 |
| 5-SEAM-B | `src/image_stitcher.cc::BlendSeams` (OpenCL 硬切+窄带) + `src/calibration.cc` (LoadSeamMasks) | log `[BlendSeams] hard-cut + 3px narrow band`; 静态人跨 seam 不重影; FPS 80+ |
| 5-FG-A | `src/bg_subtractor.cc/.h` (新) + `src/image_stitcher.cc::PushSeam` (新) + `BlendSeams` kernel 切换 (动区单边, 静区 AANAP 混合) | log `[BgSubtractor] sliding-window bg model enabled, τ=30, α=0.01`; 走动人跨 seam 单边完整 + 静态墙-地转角自然过渡; FPS ≥ 60 |
| 5-FG-B | `src/seam_tracker.cc/.h` (新) | log `[SeamTracker] Kalman enabled, process_noise=2.0, max_disp=20`; 30 秒录像 seam 抖 < 2 px/frame |
| 5-SAL | `src/image_stitcher.cc` (累积 saliency) + `params/saliency_0..5.png` (新) | 1 周后 6 张 saliency PNG 落盘; 重新跑默认 seam 落在人流量低区 |
| 5-IPM (可选) | `src/calibration.cc::ComputeIpmLut` (新) + `params/ipm_lut_0..5.png` (新) | `--ipm` 启动后地面上 1m 棋盘格测得 100 ± 2 px; log `IPM mode enabled, 1 cm/pixel` |
| 6-A | `src/roi_config.h/.cc` + `src/roi_visualizer.cc` + `params/roi_tuning.yaml` 兼容 | UI 6 路循环 + width/height 步进 |
| 6-B | `src/http_server.cc::handle_roi_post` | curl 接 4 字段 |
| 6-C | `src/app.cc::CameraTuning` | yaw 暴露给 UI |
| 7-A | `tools/sprint7_e2e.sh` (新) | PASS 退出 0 |
| 7-B | (人工) | 3 张截图入库? |
| 7-C | `docs/` | sprint 状态 → OK |

## 5. 与 "v3.0 / v3.1" 历史的关系

- v3.0 (2026-07-08): 6 路 RTSP IP camera 改造, `tools/sprint0_smoke.sh` 验收。 **当前未跑通** (yaml 默认 IP 未上电 + 验收脚本已被删除)。 代码 (`gst_mpp_decoder.cc::BuildRtspPipeline` + `params/camera_sources.yaml`) 是健康的, 等 PoE 上电随时可跑。
- v3.1 RTSP output plan / QUICK_START_IP_ONLY 已被 git 删除, 不复活; 仅保留 `camera_sources.yaml` 给 PoE 通时调试。
- 之前 v2.5 dataset fallback path 是继续工作的 (100+ fps @ 100% DMA-BUF), **Sprint 4-6 全部完成后仍是兜底**。

## 6. 参考实现点 (供后续 worker 实施时直接读)

| 需要做什么? | 直接读的 文件 / 函数 |
| 6 路 K/D/R/t 标定 | `src/calibration.cc::LoadOrCalibrate` (5-CAL-A 新建) |
| 6 路 AANAP 全局 H + T_sim 估计算法 | `src/aanap_warp.cc::ComputeAanapLut` (5-AANAP-A 新建) |
| 6 路 AANAP LUT (CV_32FC2 xmap/ymap) 落盘 | `src/aanap_warp.cc::WriteAanapLuts` (5-AANAP-A 新建) |
| 启动期加载 AANAP LUT | `src/calibration.cc::LoadAanapLuts(yaml)` (5-AANAP-B 新建) |
| 实时 AANAP warp (RGA imremap) | `src/image_stitcher.cc::WarpImages` 加 AANAP 分支 (5-AANAP-B 改) |
| 启动期算默认 seam (GraphCut) | `src/app.cc::BootStrapOptimalLayout` 末尾 (5-SEAM-A 改) |
| 加载 7 对 seam mask | `src/calibration.cc::LoadSeamMasks(yaml)` (5-SEAM-B 新建) |
| OpenCL 硬切+窄带合成 + fg_mask 切换 | `src/image_stitcher.cc::BlendSeams` 改 kernel (5-SEAM-B + 5-FG-A 改) |
| 背景减除 (滑动平均) | `src/bg_subtractor.cc::BgSubtractor::Update` (5-FG-A 新建) |
| 局部 seam 形变 (PushSeam) | `src/image_stitcher.cc::ImageStitcher::PushSeam` (5-FG-A 新建) |
| Kalman 平滑 seam | `src/seam_tracker.cc::SeamTracker::Update` (5-FG-B 新建) |
| Saliency heatmap 累积 | `src/image_stitcher.cc::ImageStitcher::UpdateSaliency` (5-SAL 新建) |
| 棋盘格标定工具 | `tools/calibrate_6cam.cpp` (5-CAL-A 新建) |
| (可选) IPM 地面尺度 | `src/calibration.cc::ComputeIpmLut` (5-IPM 新建) |
| 6 路 yaml / PNG 持久化 | `src/roi_config.cc::LoadFromFile` 模板 (现有 cv::FileStorage) |
| UI 6 路循环 (4 → 6 步进) | `src/roi_visualizer.cc::DrawROIMarkers` (改 for 循环上限) |
| `/api/roi` 接 x/y/w/h | `src/http_server.cc::handle_roi_post` (现有 regex 加字段) |
| RGA crop 保留 (0/90/180/270) | `src/image_stitcher.cc::WarpImages::ToRgaRotation` (现有) |

## 7. 与架构硬约束的关系

| Sprint | 是否踩到硬约束? | 说明 |
|---|---|---|
| 4-A/B/C | 否 | 仅 layout 计算与 K 矩阵; warp 仍在 RGA / OpenCL 范围内 |
| 5-A | 否 | GLES warper 已可静默 fallback 到 RGA path (原架构允许) |
| 4-A/B/C | 否 | 仅 layout 计算与 K 矩阵; warp 仍在 RGA / OpenCL 范围内 |
| 5-PRE (SIFT->ORB) | 否 | 仅替换特征描述子, 描述子类型对 AffineBasedEstimator / BundleAdjuster 透明; 描述子字节布局之外的事情一概不动 (RGA / OpenCL / 解码 / DRM 全保持) |
| 5-CAL-A | 否 | 离线求解 6 路 K/D/R/t, 写 yaml; 不动实时 |
| 5-AANAP-A | 否 | 离线估 H + T_sim + 写 CV_32FC2 LUT; 不动实时 (跟 5-IPM-A 同样的离线性质) |
| 5-AANAP-B | **是 (RGA imremap 接 CV_32FC2)** | RGA `imremap` 模式接 CV_32FC2 aanap_lut, 仍走 DMA-BUF 零拷贝; 若 RGA 不可用, 退到 GLES fragment shader 走同一 LUT (Mali GPU 走 EGL 路径也是 0 copy) |
| 5-SEAM-A | 否 | GraphCut CPU 跑, 只在 init / refresh 时跑 (5-10 分钟一次); 不影响实时 |
| 5-SEAM-B | **是 (OpenCL 硬切)** | 替代原 alpha blend, 仍走 `clImportMemoryARM` 零拷贝; 硬切省 alpha 算力, 实测略快 |
| 5-FG-A | **是 (新增 OpenCL/CPU 混合路径)** | 背景减除 CPU (0.3ms) + seam 形变 CPU (1.4ms) + OpenCL 合成仍 0 copy; 合成 kernel 按 fg_mask 切换 (动区单边, 静区 AANAP 混合); 总 wall clock 加 5ms 内 |
| 5-FG-B | 否 | Kalman 标量运算 < 0.1ms, 纯 CPU |
| 5-SAL | 否 | 离线累积 1 周, 不影响实时 |
| 5-IPM (可选) | **是 (RGA imremap)** | 跟 5-AANAP-B 同套路 (RGA imremap 接 CV_32FC2); 触发条件: 用户需要物理尺度 (1 pixel = X cm) |
| 6-A/B/C | 否 | UI 扩展 + HTTP 端点 |
| 5-SAL | 否 | 离线累积 1 周, 不影响实时 |
| 6-A/B/C | 否 | UI 扩展 + HTTP 端点 |
| 6-A/B | 否 | UI 扩展 + HTTP 端点 |
