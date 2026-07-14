# USER_GOAL_ROADMAP.md

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

### 0.5.5 不接受的方案 (写在前面防回退)

- ❌ **DH 双单应 / APAP / AANAP**: 6 路 2D warp 假设不可能覆盖立体人, 工程复杂度高但视觉收益不抵 seam-based。
- ❌ **α 羽化 / 多频段全幅混合**: 重影的物理根源没解决, 只能压低不能消除, 对前景无效。
- ❌ **CPU 软解 / 软件 fallback**: 架构硬约束禁止。
- ❌ **IPM LUT 在 CPU 算**: RGA `imremap` 或 GLES fragment shader 走 GPU 零拷贝; CPU `cv::remap` 仅作 fallback。

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

### Sprint 5: 前景/后景分离 + IPM 俯视投影 + Seam-based 合成 (预计 7-8 周, 主路径重构)

**核心原则 (v3.4 架构, 2026-07-14 重新规划)**:
- **后景 (静态地面 + 静态家具)**: 相机固定、背景不变, 去畸变 / IPM / 拼接对齐 / 默认 seam 全部 **离线标定一次存下来**, 实时只做查表和合成, 算力几乎为 0
- **前景 (走动的人)**: **不试图把两路对齐** (几何不可能 — 立体人两台相机视线夹角不同, 任何 2D 变换都对不齐), 改为**保证同一时刻一个人只由一台相机画**: 重叠区不做 alpha/多频段混合, 用一条 seam 切开, 检出人跨在 seam 上则把 seam 局部推开
- **纯 IPM 单平面 (Y=0) 拼接** 对地面零重影, 对静态低位物 (桌椅腿) 几乎零重影, 对静态高位物 (桌面) 仍有拖影 (多平面 IPM 子项目 5-IPM-C 补), 对移动人靠 seam 物理隔离消除

**架构总览**:

```
离线 (启动 / 周期刷新, 5-10 分钟):
  1. 标定 6 路 K + D + R + t + 离地高 H + 俯角 θ        → params/calibration.yaml
  2. 算 6 路去畸变 LUT: undistort_LUT_i[u,v] = (u',v')   → params/undistort_0..5.png
  3. 算 6 路 IPM LUT:   ipm_LUT_i[u,v] = (Xpan,Ypan)     → params/ipm_lut_0..5.png (CV_32FC2, 8MB/路)
  4. 算 7 对相邻默认 seam (GraphCut on 静态多帧)          → params/seam_default_0..6.png
  5. 累积 saliency heatmap (1 周运行后)                   → params/saliency_0..5.png

实时 (每帧, 6×2K @ 30fps, ~10ms):
  for each camera:                                       (并行, ~3ms wall)
    1. 去畸变: out = undistort_LUT[input]                ← 0.5ms (RGA colorkey/remap)
    2. 背景减除: fg_mask = |out - bg_model| > τ         ← 0.3ms (CPU OpenCV)
    3. IPM remap: img_pan = IPM_remap[out]               ← 0.5ms (RGA LUT 模式)
  cross-camera:                                          (~7ms wall)
    4. 局部 seam 形变: fg_mask 跨 seam 处把 seam 推开     ← 0.2ms/pair × 7 = 1.4ms (CPU)
    5. Kalman 平滑 seam 位置                             ← < 0.1ms
    6. OpenCL 合成: 硬切 + seam±3px 窄带                  ← 1ms/pair × 7 = 7ms
  per-camera:
    7. 背景模型慢更新: bg = α·out + (1-α)·bg (α=0.01)    ← 0.05ms
```

**算力账 (RK3588, 6×2K @ 30 fps)**: 总 wall clock ~9-10ms, 30 fps 预算 33ms, 余 23ms ✓。 跟现 v2.5 100+ fps 比加 ~5ms, 仍在 80+ fps, 远超 30 fps 要求。

**Sprint 子段**:

- **5-PRE. SIFT -> ORB 改造 (前置, 本周必做, 不动 RGA+OpenCL 栈)**:
  - **目标**: 把 `src/stitching_param_generater.cc` 内置的 SIFT 描述子整体切到 ORB, 保持现有 `RGA crop/rotate + OpenCL 接缝羽化` 零拷贝栈不变。
  - **改动**:
    1. `src/stitching_param_generater.cc` 新增内部类 `OrbFeaturesFinder` (继承 `cv::detail::FeaturesFinder`, 使用 `cv::ORB::create(4000)` 检测+计算 binary 描述子) + `OrbPairwiseMatcher` (继承 `cv::detail::FeaturesMatcher`, 用 `BFMatcher(NORM_HAMMING) + knnMatch(2) + Lowe ratio 0.65`)。
    2. `InitCameraParam()`: `SIFT::create()` 替换为 `makePtr<OrbFeaturesFinder>(4000)`; `AffineBestOf2NearestMatcher` / `BestOf2NearestMatcher` / `BestOf2NearestRangeMatcher` 全部替换为 `makePtr<OrbPairwiseMatcher>(match_conf)`。描述子字节布局对 `AffineBasedEstimator` / `HomographyBasedEstimator` / `BundleAdjuster*` 透明, 不动 estimator / BA 入口。
    3. `include/stitching_param_generater.h` 默认值改: `matcher_type="affine"` (was "homography"), `estimator_type="affine"` (was "homography"), `ba_cost_func="no"` (was "reproj"), `warp_type="plane"` (was "spherical")。贴 GC4683 顶视平面 + 6 路固定支架 + Affine 估计场景, 启动后 `cv::detail::PlaneWarper` 算出的 xmap/ymap 正好是 IPM 友好形式。
  - **硬约束 (不破)**: RGA crop/rotate 不动; OpenCL 接缝羽化不动; 描述子类型硬编码 ORB binary; 匹配距离硬编码 NORM_HAMMING。
  - **不动**: `src/app.cc::EstimatePairOverlap` / `BootStrapOptimalLayout` / `src/rk_gles_warper.cc` / `src/gst_mpp_decoder.cc` / `src/drm_allocator.cc` / `src/roi_*.cc` / `src/http_server.cc`。
  - **风险 / 兜底**: ORB 在低纹理场景失配 → `src/app.cc::EstimatePairOverlap` 的 `EstimateOverlapByTemplate` 模板匹配兜底仍生效。 启动时间从 ~600ms 降到 ~200ms。
  - **验收**:
    1. `cmake --build build && ./build/image-stitching` 不报 `cv::detail::FeaturesFinder` / `FeaturesMatcher` 链接错。
    2. dataset 路径启动日志打印 `Features in image #N: ~4000` (代替 SIFT 的 ~1500)。
    3. 端到端 FPS 维持 100+。

- **5-IPM-A. 6 路标定 + IPM LUT 计算 (1.5 周, 离线)**:
  - **目标**: 标定 6 路相机内参 (K, D) + 外参 (R, t) + 离地高 H + 俯角 θ; 计算每路 IPM LUT (地面 Y=0 投影到共享全景坐标系)。
  - **新增文件**:
    - `src/calibration.cc/.h` (新): 6 路 K/D/R/t/H/θ 加载 + save; IPM LUT 计算函数 `ComputeIpmLut(K, R, t, H, pitch, panorama_size, meters_per_pixel) -> Mat<CV_32FC2>`。
    - `tools/calibrate_6cam.cpp` (新): 棋盘格采集 + `cv::solvePnP` 标定工具。 输出 `params/calibration.yaml`。
  - **算法**:
    ```
    for (u, v) in input_image:
      ray_cam = K_inv · (u, v, 1)^T
      ray_world = R^T · ray_cam
      t = -t_world.y / ray_world.y            // 与地面 Y=0 求交
      ground_pt = t_world + t * ray_world      // (X, 0, Z)
      Xpan = ground_pt.x / meters_per_pixel + panorama_cx
      Ypan = ground_pt.z / meters_per_pixel + panorama_cy
      ipm_lut_i.at<Vec2f>(v, u) = (Xpan, Ypan)
    ```
  - **不动**: 主循环 / 解码 / OpenCL blend / GLES warper。
  - **风险 / 兜底**:
    - 标定精度差 → 棋盘格采 20+ 角度, solvePnP 用 `SOLVEPNP_ITERATIVE` + 重投影误差 < 1 px 才接受。
    - 6 路间标定不收敛 → 分组标定, 重叠多的先标。
    - IPM LUT 写满 8MB / 路 → 用 CV_32FC2 二进制 + 简单 zlib 压缩, yaml 存路径。
  - **验收**:
    1. `tools/calibrate_6cam --chessboard 11x8 --square 30mm --cams 6` 跑通, 输出 yaml 6 路 K/D/R/t 都有非零值。
    2. 把 `ipm_lut_i.png` 可视化到地面坐标系, 棋盘格角点两路 LUT 投到同一 (Xpan, Ypan) 误差 < 2 px。
    3. yaml 落盘 `params/calibration.yaml` + `params/ipm_lut_0..5.png` (启动时读, 不重算)。

- **5-IPM-B. 实时 IPM remap (1 周, 在线)**:
  - **目标**: 实时跑通 IPM 投地面, 输出有物理尺度的全景 (1 pixel = 1 cm, 中心在世界原点)。
  - **改动**:
    1. `src/image_stitcher.cc::WarpImages` 增加 IPM 路径分支: 当 `params/warp_data.yaml` 含 `ipm_lut_i` 时, 用 RGA `imremap` 模式 (CV_32FC2 xmap/ymap) 替代原 RGA 0/90 旋转 + BlitByRect 路径。
    2. `src/app.cc::InitFromConfig` 启动时读 `ipm_lut_0..5.png` + `calibration.yaml`, 喂给 `ImageStitcher::SetIpmLuts(...)`。
    3. panorama 尺寸: `panorama_w = 2 * max_ground_extent / meters_per_pixel` (默认 1cm/pixel → ~4800x4080)。
  - **不动**: 解码 / RGA 主路径 / OpenCL blend / 持久化。
  - **风险 / 兜底**:
    - RGA `imremap` 不支持 CV_32FC2 → 退到 GLES warper 走 LUT, 或 OpenCV `cv::remap` CPU fallback (1-2ms 损耗可接受)。
    - 标定文件缺失 → fall back 到原 H 估计路径, log 警告。
  - **验收**:
    1. `./image-stitching --ipm` 启动后, 全景图上画 1m × 1m 棋盘格, 测得像素距离 100 px ± 2 px (物理尺度验证)。
    2. 端到端 FPS ≥ 60 (留余量给后续 seam 改造)。
    3. 日志打印 `IPM remap enabled, 1 cm/pixel, panorama 4800x4080`。

- **5-IPM-C. 多平面 IPM (1 周, 离线) — 可选**:
  - **目标**: 解决静态高位物 (桌面、椅背) 的 IPM 拖影: 标定后手动给每路相机的"已知 Z 平面"列表, IPM 投到对应平面。
  - **改动**:
    1. `params/calibration.yaml` 扩字段: `cam_i.z_planes: [0.0, 0.45, 0.75]` (地面 / 椅面 / 桌面高度, 单位米)。
    2. `src/calibration.cc::ComputeIpmLut` 改多平面: 同一像素按 Z 值分到对应平面的 (X, Z) 坐标, 生成多层 LUT。
    3. 合成时按 Z 优先级: 高 Z 物覆盖低 Z 物 (Z-buffer 思路简化版)。
  - **风险 / 兜底**:
    - 标定需要人工事先知道家具 Z 高度, 工程量大。
    - 多层 Z 重叠时 Z 优先级顺序会出错 → 默认 Z 单调 (Z 越大优先级越低) 防穿透。
  - **验收**:
    1. 室内有 75cm 桌面, 桌面 4 角在 IPM 后两路重合误差 < 5 px。
    2. 启动期读 `cam_i.z_planes`, 落盘 `ipm_lut_z0_i.png / z1_i.png / ...`。

- **5-SEAM-A. GraphCut 默认 seam (1 周, 离线 + 在线混合)**:
  - **目标**: 启动时跑一次 GraphCut 求 7 对相邻的默认 seam mask, 落盘 yaml。
  - **改动**:
    1. `src/app.cc::BootStrapOptimalLayout` 末尾加 `seam_finder->find(warped_images, warped_masks)` 用 `cv::detail::GraphCutSeamFinder(COST_COLOR_GRAD)`。
    2. 7 对相邻 (2x3 布局): 3 横 (cam0-1, cam2-3, cam4-5) + 2 左列纵 (cam0-2, cam2-4) + 2 右列纵 (cam1-3, cam3-5)。
    3. 落盘 `params/warp_data.yaml`: `seam_default_0..6` (8-bit 单通道 PNG, ~20MB/个, 压缩存)。
  - **风险 / 兜底**:
    - GraphCut 在 4800x4080 跑 50-100ms, 但只在 init / refresh 跑, 不影响实时。
    - 弱纹理区 seam 跳来跳去 → cost 加 saliency (若 5-SAL 已上) / Canny 边缘强度。
  - **验收**:
    1. 启动日志 `[App] seam 0..6 computed, average width = XX px`。
    2. `params/warp_data.yaml` 7 个 PNG 可读, 与 stitched 图像对齐 (imgviz 抽检 seam 不穿过明显物体)。

- **5-SEAM-B. OpenCL 硬切 + 窄带合成 (1 周, 在线)**:
  - **目标**: 替代原 alpha 羽化为 seam 硬切 (主) + 窄带 3px 渐变 (缝隐藏), 立即消除移动物重影。
  - **改动**:
    1. `src/image_stitcher.cc::BlendSeams` 改 OpenCL kernel: 加 `seam_mask_buffer` 输入 (8-bit R8), 输出 = `smoothstep(0, 3, dist_to_seam) * cam_a + (1-...) * cam_b`。
    2. `ImageStitcher` 加 `seam_masks_[7]` 成员 (cv::Mat, 单通道), `SetLayout` 多接 `seam_masks` 参数。
    3. `src/calibration.cc/.h` (沿用 5-IPM-A 新建文件) 加 `LoadSeamMasks(yaml_path) -> array<Mat, 7>`。
  - **不动**: 解码 / RGA crop / IPM remap / 持久化。
  - **风险 / 兜底**:
    - 硬切在曝光/白平衡不同时可见 → 窄带扩到 5 px, 视觉几乎不可见。
    - seam mask 单帧硬切不跟随人 → 由 5-FG-A 局部形变补; 本段先验证静态 seam 效果。
  - **验收**:
    1. log `[BlendSeams] hard-cut + 3px narrow band, 0 alpha`。
    2. 室内有 1 个静止的人跨在 seam 上 → 人在画面里完整, 没有"半透明 + 重影"。
    3. 端到端 FPS 维持 80+ (硬切省 alpha 算力, 实际比 alpha 略快)。

- **5-FG-A. 背景减除 + 局部 seam 形变 (1 周, 在线)**:
  - **目标**: 检测人跨在 seam 上时, 把 seam 局部推开, 人完整地由一台相机呈现。
  - **改动**:
    1. `src/bg_subtractor.cc/.h` (新): 简化版 MOG2 / 滑动平均, 输出 0/255 fg_mask。 6 路线程安全 (各路独立 bg_model)。
    2. `src/image_stitcher.cc::PushSeam` (新成员): 接收 fg_mask (投到全景后) + 当前 seam, 用距离变换 + 形态学推开 seam, 输出新 seam_mask。
    3. `ImageStitcher::WarpImages` 之后: 把单路 fg_mask 通过 IPM LUT 投到全景, 喂给 `PushSeam`。
  - **算法**:
    ```
    for each pair:
      fg_pan = ipm_remap(fg_mask_a) | ipm_remap(fg_mask_b)  // OR
      dist_to_seam = distanceTransform(seam == 1 ? 0 : 255)
      push_zone = (dist_to_seam < PUSH_WIDTH) & (fg_pan > 0)
      seam_pushed = push_seam_around_fg(seam, push_zone, PUSH_WIDTH=20px)
    ```
  - **风险 / 兜底**:
    - 背景减除在室内阴影/灯光变化下误报 → 慢更新 (α=0.01), 强光突变 frame 跳过背景更新。
    - 形变跟不上快速走动 → 单帧 max displacement 限制 (20 px) + 5-FG-B Kalman。
  - **验收**:
    1. 启动日志 `[BgSubtractor] sliding-window bg model enabled, τ=30, α=0.01`。
    2. 室内有人走动跨 seam, 全景图里这个人**不重影** (单边完整呈现)。
    3. 端到端 FPS ≥ 60 (开背景减除后损耗 ~3ms)。

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

- **5-IPM-A 标定精度差**: 棋盘格 20+ 角度 solvePnP, 重投影误差 > 1 px 的路剔除后重标; 最终标定误差 > 3 px 时降回 H 估计路径, log ERROR。
- **5-IPM-B RGA 不支持 CV_32FC2 remap**: 退到 GLES fragment shader 接同一 LUT (Mali GPU 走 EGL 零拷贝), 性能损耗 < 2ms; 仍不行则 OpenCV `cv::remap` CPU fallback (再 +2-3ms, 仍 30 fps 内)。
- **5-IPM-C 多平面 Z 重叠错位**: 标定 `cam_i.z_planes` 显式给, 默认 Z 越大优先级越低防穿透; 标定缺失的路只投 Z=0 地面。
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
| 5-IPM-A | `src/calibration.cc/.h` (新) + `tools/calibrate_6cam.cpp` (新) + `params/calibration.yaml` (新) + `params/ipm_lut_0..5.png` (新) | `tools/calibrate_6cam` 跑通, 6 路 K/D/R/t 非零; 棋盘格角点两路 IPM 投到同一 (Xpan,Ypan) 误差 < 2 px |
| 5-IPM-B | `src/image_stitcher.cc::WarpImages` (改 IPM 路径) + `src/app.cc::InitFromConfig` (读 IPM LUT) | 启动后日志 `IPM remap enabled, 1 cm/pixel, panorama 4800x4080`; 1m 棋盘格测得 100±2 px; FPS ≥ 60 |
| 5-IPM-C | `src/calibration.cc` (多平面扩) + `params/calibration.yaml` (z_planes 字段) | 75cm 桌面 4 角两路 IPM 误差 < 5 px; 启动读 `cam_i.z_planes` 落多张 z-N PNG |
| 5-SEAM-A | `src/app.cc::BootStrapOptimalLayout` (末尾加 GraphCut) + `params/warp_data.yaml` (加 `seam_default_0..6`) | 启动日志 `[App] seam 0..6 computed, avg width = XX px`; 7 个 seam PNG 落盘可读 |
| 5-SEAM-B | `src/image_stitcher.cc::BlendSeams` (OpenCL 硬切+窄带) + `src/calibration.cc` (LoadSeamMasks) | log `[BlendSeams] hard-cut + 3px narrow band`; 静态人跨 seam 不重影; FPS 80+ |
| 5-FG-A | `src/bg_subtractor.cc/.h` (新) + `src/image_stitcher.cc::PushSeam` (新成员) | log `[BgSubtractor] sliding-window bg model enabled, τ=30, α=0.01`; 走动人跨 seam 单边完整; FPS ≥ 60 |
| 5-FG-B | `src/seam_tracker.cc/.h` (新) | log `[SeamTracker] Kalman enabled, process_noise=2.0, max_disp=20`; 30 秒录像 seam 抖 < 2 px/frame |
| 5-SAL | `src/image_stitcher.cc` (累积 saliency) + `params/saliency_0..5.png` (新) | 1 周后 6 张 saliency PNG 落盘; 重新跑默认 seam 落在人流量低区 |
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
| 6 路 K/D/R/t 标定 | `src/calibration.cc::LoadOrCalibrate` (5-IPM-A 新建) |
| 6 路 IPM LUT 计算 | `src/calibration.cc::ComputeIpmLut` (5-IPM-A 新建) |
| 启动期加载 IPM LUT | `src/calibration.cc::LoadIpmLuts(yaml)` (5-IPM-A 新建) |
| 实时 IPM remap (RGA) | `src/image_stitcher.cc::WarpImages` 加 IPM 分支 (5-IPM-B 改) |
| 启动期算默认 seam (GraphCut) | `src/app.cc::BootStrapOptimalLayout` 末尾 (5-SEAM-A 改) |
| 加载 7 对 seam mask | `src/calibration.cc::LoadSeamMasks(yaml)` (5-SEAM-B 新建) |
| OpenCL 硬切+窄带合成 | `src/image_stitcher.cc::BlendSeams` 改 kernel (5-SEAM-B 改) |
| 背景减除 (滑动平均) | `src/bg_subtractor.cc::BgSubtractor::Update` (5-FG-A 新建) |
| 局部 seam 形变 | `src/image_stitcher.cc::ImageStitcher::PushSeam` (5-FG-A 新建) |
| Kalman 平滑 seam | `src/seam_tracker.cc::SeamTracker::Update` (5-FG-B 新建) |
| Saliency heatmap 累积 | `src/image_stitcher.cc::ImageStitcher::UpdateSaliency` (5-SAL 新建) |
| 棋盘格标定工具 | `tools/calibrate_6cam.cpp` (5-IPM-A 新建) |
| 6 路 yaml / PNG 持久化 | `src/roi_config.cc::LoadFromFile` 模板 (现有 cv::FileStorage) |
| UI 6 路循环 (4 → 6 步进) | `src/roi_visualizer.cc::DrawROIMarkers` (改 for 循环上限) |
| `/api/roi` 接 x/y/w/h | `src/http_server.cc::handle_roi_post` (现有 regex 加字段) |
| RGA crop 保留 (0/90/180/270) | `src/image_stitcher.cc::WarpImages::ToRgaRotation` (现有) |

## 7. 与架构硬约束的关系

| Sprint | 是否踩到硬约束? | 说明 |
|---|---|---|
| 4-A/B/C | 否 | 仅 layout 计算与 K 矩阵; warp 仍在 RGA / OpenCL 范围内 |
| 5-A | 否 | GLES warper 已可静默 fallback 到 RGA path (原架构允许) |
| 5-B | 否 | yaml 文件读写 |
| 5-C | 否 | yaml 文件读写 + 启动期控制流 |
| 5-PRE (SIFT->ORB) | 否 | 仅替换特征描述子, 描述子类型对 AffineBasedEstimator / BundleAdjuster 透明; 描述子字节布局之外的事情一概不动 (RGA / OpenCL / 解码 / DRM 全保持) |
| 5-IPM-A | 否 | 离线计算 IPM LUT, 走 CV_32FC2 写文件; 不动实时 |
| 5-IPM-B | **是 (RGA LUT remap)** | 用 RGA `imremap` 模式接 CV_32FC2 ipm_lut, 仍走 DMA-BUF 零拷贝; 若 RGA 不可用, 退到 GLES fragment shader 走同一 LUT (Mali GPU 走 EGL 路径也是 0 copy) |
| 5-IPM-C | 否 | 多平面 IPM 仍走 5-IPM-B 的 RGA/GLES remap 路径, 只是 LUT 多了几层 |
| 5-SEAM-A | 否 | GraphCut CPU 跑, 只在 init / refresh 时跑 (5-10 分钟一次); 不影响实时 |
| 5-SEAM-B | **是 (OpenCL 硬切)** | 替代原 alpha blend, 仍走 `clImportMemoryARM` 零拷贝; 硬切省 alpha 算力, 实测略快 |
| 5-FG-A | **是 (新增 OpenCL/CPU 混合路径)** | 背景减除 CPU (0.3ms) + seam 形变 CPU (1.4ms) + OpenCL 合成仍 0 copy; 总 wall clock 加 5ms 内 |
| 5-FG-B | 否 | Kalman 标量运算 < 0.1ms, 纯 CPU |
| 5-SAL | 否 | 离线累积 1 周, 不影响实时 |
| 6-A/B/C | 否 | UI 扩展 + HTTP 端点 |
| 6-A/B | 否 | UI 扩展 + HTTP 端点 |
