# HISTORY

设计演进时间线（精简）+ **完整操作手册** + **踩过的坑** + **周期性回顾节**。

`AANAP (静态多平面) + Seam (动态) + 背景减除切换`

## v3.2 实测代码能力回顾（2026-07-14）

> **本节周期性整理**：用"代码即真"原则列出当前仓库实测的能力/差距。每个 Sprint 末尾做一次增量。

### 当前实测能力（OK）

- **6 路 2x3 layout dataset 端到端跑通**：`datasets/2k-test-h264/t{50..53,40,41}.mp4` 经 gstreamer `mppvideodec dma-feature=true` 输出 NV12 DMA-BUF fd → RGA crop/rotate → OpenCL 羽化 → DRM。`v2.5` 后 100+ fps @ 100% DMA-BUF（2026-07-07 绿条纹修复后）。
- **6 路 RTSP pipeline 已编码**（`src/gst_mpp_decoder.cc::BuildRtspPipeline`）：`rtspsrc → rtph264depay → h264parse → mppvideodec → appsink`，yaml 驱动。**但 `params/camera_sources.yaml` 默认 IP `192.168.10.21..26` 未上电 + Sprint 0 验收脚本已被删除，未端到端跑通**。
- **`stitching_param_generater.cc` 已编译**：内置 OpenCV `stitching_detailed` 全套（SIFT + AffineBestOf2NearestMatcher + 1-径 Bundle Adjustment + Wave Correct + Spherical Warper + MultiBand Blend）；**App 主循环未调用**。
- **多帧 ROI bootstrap**：`App::BootStrapOptimalLayout` 用 ORB + BFMatcher (KNN + Lowe ratio) + RANSAC + `estimateAffinePartial2D`，跑多帧取最优 confidence。
- **参数持久化**：`RoiConfig::SaveToFile/LoadFromFile` + cv::FileStorage 读写 `params/roi_tuning.yaml`；`/api/roi` POST offset 改 yaml（原子 tmp+rename）。
- **CameraPage 管理面**：cpp-httplib 端口 8080 + `/api/stream` MJPEG 推流 + `/api/snapshot` 单帧 + 5+ API 端点。

### 仍是 MISS / 警告的工作（对齐用户最新目标 GC4683 MIPI / FOV 101x68° / ~180°）

| 目标点                                                                                     | 当前状态                                                                  | 应在 Sprint     |
| ------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------- | --------------- |
| `camera_intrinsics.h` 重算 fx/fy 对应 FOV 101x68°                                       | 警告：用的是 102.5°/55.2° 占位；且`App::InitFromConfig` 未读此 header | Sprint 4-A      |
| 2x3 layout 横向/纵向 overlap 按 FOV+姿态计算，而非硬编码`W/8` `H/12`                   | MISS：`BuildStitchLayout2x3` 写死比例                                   | Sprint 4-B      |
| `stitching_param_generater.cc` 主 pipeline 接入：`SetWarpData(StitchingWarpData)` 实装 | MISS：`ImageStitcher::SetWarpData` 已存在但 App 不传任何 warp_data      | Sprint 5-A      |
| bootstrap 估出的 Affine/Homography 持久化到 yaml                                           | MISS：仅`cached_overlaps_` 内存对象；yaml 仅持 offset                   | Sprint 5-B/C    |
| 启动期优先读`warp_data.yaml` 跳过 bootstrap                                              | MISS：`App::InitFromConfig` 不读 warp_data                              | Sprint 5-C 收尾 |
| `roi_visualizer.cc` 4 → 6 路循环；支持 width/height                                     | MISS：`DrawROIMarkers` 硬编码 4，`HandleDebugAction` 仅 offset        | Sprint 6-A      |
| `/api/roi` POST 接 `x/y/w/h`                                                           | MISS：仅接`offset_x/offset_y`                                           | Sprint 6-B      |
| 180° 俯视 FOV 覆盖 (≥ 96% ~ 180°)                                                       | MISS：当前 layout 横向 ≈ 25° / 纵向 ≈ 13.8°（远小于目标）             | Sprint 4-B/C    |

详细 Sprint 计划：见 [`USER_GOAL_ROADMAP.md`](USER_GOAL_ROADMAP.md)。

### 已删除的旧文档（不要复活）

- `docs/NETWORK_CAMERA_PLAN.md` (v3.0 IP camera 改造, 2026-07-08)
- `docs/RTSP_OUTPUT_PLAN.md` (v3.1 RTSP 输出)
- `docs/QUICK_START_IP_ONLY.md` (改 IP 就能用)
- `tools/sprint0_smoke.sh` 等 Sprint 0 验收脚本

---

## v3.3 SIFT → ORB 改造（2026-07-14）

> 衔接 v3.2：本节记录把 `src/stitching_param_generater.cc` 内置特征描述子从 SIFT 切到 ORB 的过程。RGA+OpenCL 加速栈零变化，约束见 `docs/USER_GOAL_ROADMAP.md` § 5-PRE。

### 变更内容

- **新增** `src/stitching_param_generater.cc` 内部类：
  - `OrbFeaturesFinder` (继承 `cv::detail::FeaturesFinder`) — `cv::ORB::create(4000)` 检测+计算 binary 32 字节描述子；BGR→gray→CV_8U 预处理。
  - `OrbPairwiseMatcher` (继承 `cv::detail::FeaturesMatcher`) — `BFMatcher(NORM_HAMMING) + knnMatch(2) + Lowe ratio 0.65`；不依赖 ORB 之外的描述子类型。
- **修改** `InitCameraParam()`:
  - `SIFT::create()` → `makePtr<OrbFeaturesFinder>(4000)`
  - `AffineBestOf2NearestMatcher` / `BestOf2NearestMatcher` / `BestOf2NearestRangeMatcher` → `makePtr<OrbPairwiseMatcher>(match_conf)`
  - `AffineBasedEstimator` / `HomographyBasedEstimator` / `BundleAdjuster*` 不动（描述子类型对它们透明）。
- **修改** `include/stitching_param_generater.h` 默认值:
  - `matcher_type = "affine"` (was "homography")
  - `estimator_type = "affine"` (was "homography")
  - `ba_cost_func = "no"` (was "reproj"，6 路 Affine 场景下 BA 收益有限)
  - `warp_type = "plane"` (was "spherical"，GC4683 顶视平面场景)

### 不动的部分

- RGA crop/rotate 0/90/180/270 路径 (`src/image_stitcher.cc::WarpImages` 中的 `BlitByRect`)
- OpenCL 接缝羽化 (`src/image_stitcher.cc::BlendSeams` 中的 `clImportMemoryARM` + `cl_kern_blend_v_` / `cl_kern_blend_h_`)
- DMA-BUF fd 池化 (`dma_buf_cache_` unordered_map)
- 主路径 bootstrap (`src/app.cc::EstimatePairOverlap`，本来就走 ORB)
- 解码 (`src/gst_mpp_decoder.cc`)
- 持久化 / UI (`src/roi_*.cc`, `src/http_server.cc`)

### 风险 / 兜底

- **ORB 在低纹理场景失配**：本场景 GC4683 顶视拍摄地面/桌面纹理足够。
- **描述子字节布局兼容性**：`cv::detail::AffineBasedEstimator` / `BundleAdjusterReproj` 等只看 `ImageFeatures.keypoints` 与 `MatchesInfo.matches`，不看描述子字节。如果 OpenCV 升级后这些类内部逻辑变了需要重新评估。
- **兜底**：如果 Sprint 5-A 接入主 pipeline 后 ORB 估 Affine 失败，临时把 `matcher_type` / `estimator_type` 改回去"homography" + BundleAdjusterReproj 即可，不需要改代码（但默认已经"affine" / "no"）。

### 验收 (板上 e2e)

1. `cmake --build build && ./build/image-stitching` 不报 `cv::detail::FeaturesFinder` / `FeaturesMatcher` 链接错误。
2. dataset 路径启动日志 `Features in image #N: ~4000` (代替 SIFT 的 ~1500)。
3. 端到端 FPS 维持 100+ (与 v2.5 一致)。
4. Sprint 5-C 落盘 `params/warp_data.yaml` 后，`xmap/ymap` 维度与 SIFT 时代一致 (PlaneWarper 输出仅依赖 K + R，与特征点类型无关)。

## v3.4 架构重新规划: 前景/后景分离 + AANAP (静态) + Seam-based 合成 (2026-07-14 规划, 2026-07-15 AANAP 复活修正, 计划, 未实施)

> 本节是文档化阶段, 无代码改动。 落地按 `docs/USER_GOAL_ROADMAP.md` § 0.5 核心原则 + § 2 Sprint 5-IPM/SEAM/FG/SAL 子段逐项推进。

### 决策依据

- **症状**: 6 路顶视 (室内) 拼接, 移动人跨重叠区出现"半透明 + 重影"。
- **根因 (数学)**: 立体 3D 点 P=(X,Y,Z) 在两台相机像素 (u0,v0) 和 (u1,v1) 不重合, **没有任何 2D warp 能让两路像素投到同一全景像素还保持人形状不变**。 除非 Z=0 (地面, IPM 才能两路对齐) 或 Z=∞ (无穷远, 单 H 才能对齐)。 人既不在地面也不在无穷远, 2D warp 必坏。
- **结论**: 试图用 2D warp (DH / APAP / AANAP / 全幅 α 混合) 把两路"对齐" = 物理上不可能, 只会做出更怪的"半透明"。 真解是**同一个人只由一台相机画, 重叠区硬切 + 缝局部形变**。

### 核心架构

| 类别                                 | 几何性质               | 实时策略                                                             | 算力 (RK3588)                                   |
| ------------------------------------ | ---------------------- | -------------------------------------------------------------------- | ----------------------------------------------- |
| **后景 (静态地面 + 静态家具)** | 离线标定后两路天然对齐 | 离线算 IPM LUT + 默认 seam + 背景模型, 实时只查表                    | ~3ms / 6 路                                     |
| **前景 (走动的人)**            | 立体物, 2D warp 必坏   | 不试图对齐, 重叠区硬切 (single-side), 检出人跨 seam 把 seam 局部推开 | ~7ms / 7 pair                                   |
| **混合带 (seam ± 3 px)**      | 缝隐藏                 | α smoothstep 0→3 px 渐变, 之外硬切                                 | (计入上面)                                      |
| **总 wall clock**              |                        |                                                                      | **~10ms / 帧** (30 fps 预算 33ms 余 23ms) |

### 新增文件 / 改动

- 新增: `src/calibration.cc/.h` (6 路 K/D/R/t 加载 + IPM LUT 计算 + seam mask 加载), `src/bg_subtractor.cc/.h` (滑动平均背景模型), `src/seam_tracker.cc/.h` (Kalman `[seam_x, velocity_x]`), `tools/calibrate_6cam.cpp` (棋盘格标定工具)
- 改: `src/image_stitcher.cc::WarpImages` 加 IPM 路径 (RGA `imremap` 接 CV_32FC2 ipm_lut, RGA 不可用退到 GLES fragment shader); `src/image_stitcher.cc::BlendSeams` 改 OpenCL kernel 加 seam mask 输入 + 硬切+窄带 (替代 α blend); `src/app.cc::BootStrapOptimalLayout` 末尾加 GraphCut 求默认 seam; `src/app.cc::InitFromConfig` 启动读 IPM LUT + seam mask + 标定 yaml
- 落盘: `params/calibration.yaml` (6 路 K/D/R/t/H/θ/z_planes), `params/ipm_lut_0..5.png` (CV_32FC2, 8MB/路), `params/seam_default_0..6.png` (7 对相邻, 8-bit 单通道, ~20MB/个压缩), `params/saliency_0..5.png` (CV_32FC1, 1 周累积)

### 不接受的方案 (写在前面防回退)

- ❌ **DH 双单应 / APAP / AANAP**: 立体人 2D warp 假设不成立, 工程复杂但视觉收益不抵 seam-based。 Sprint 5-DH 砍掉。
- ❌ **α 羽化 / 多频段全幅混合**: 重影的物理根源没解决, 只能压低不能消除, 对前景无效。 原 `src/image_stitcher.cc::BlendSeams` 路径将被 seam 硬切+窄带替代。
- ❌ **CPU 软解 / 软件 fallback**: 架构硬约束禁止 (2026-07-06 锁定)。
- ❌ **IPM LUT 在 CPU 算**: RGA `imremap` 或 GLES fragment shader 走 GPU 零拷贝; CPU `cv::remap` 仅作最后 fallback。
- ❌ **多频段 / Voronoi 全 2D 划分**: 留给 4 路汇合区 (2x3 中央), 不作为主路径。

### 算力账 vs 架构硬约束

| 操作                    | 路径                           | 耗时                 | 硬约束状态                          |
| ----------------------- | ------------------------------ | -------------------- | ----------------------------------- |
| 去畸变 LUT 查表         | RGA colorkey/remap             | 0.3ms / 路           | ✅ 仍 DMA-BUF 零拷贝                |
| IPM remap (CV_32FC2)    | RGA`imremap` 或 GLES         | 0.5ms / 路           | ✅ 仍 DMA-BUF 零拷贝                |
| 背景减除                | CPU OpenCV                     | 0.3ms / 路           | ✅ 数据已在系统内存 (解码后)        |
| 局部 seam 形变          | CPU 距离变换 + 形态学          | 0.2ms / pair         | ✅ 小数据量                         |
| Kalman 平滑             | CPU 标量                       | <0.1ms               | ✅                                  |
| OpenCL 硬切+窄带        | Mali GPU,`clImportMemoryARM` | 1ms / pair           | ✅ 替代 α blend, 仍零拷贝          |
| 背景模型慢更新          | CPU                            | 0.05ms               | ✅                                  |
| **总 wall clock** | (6 路并行)                     | **~10ms / 帧** | **30 fps 预算 33ms, 余 23ms** |

### 与 v2.5 (现 100+ fps) 的性能对比

- v2.5: 6×2K dataset, RGA crop + α blend, ~10ms / 帧 = 100 fps
- v3.4: 6×2K IP camera, RGA IPM + 硬切+窄带 + 背景减除 + 形变, ~10-15ms / 帧 = 65-100 fps
- 仍**远超 30 fps 目标**, 8 倍余量。

### 落地 Sprint 总工时

| 子段                          | 周             | 性质                   |
| ----------------------------- | -------------- | ---------------------- |
| 5-PRE (SIFT→ORB)             | 0.5            | 必做 (已开工)          |
| 5-IPM-A (标定+IPM LUT)        | 1.5            | 离线                   |
| 5-IPM-B (实时 IPM remap)      | 1.0            | 在线                   |
| 5-IPM-C (多平面 IPM)          | 1.0            | 离线 + 在线混合 (可选) |
| 5-SEAM-A (GraphCut 默认 seam) | 1.0            | 离线 + 在线混合        |
| 5-SEAM-B (硬切+窄带)          | 1.0            | 在线                   |
| 5-FG-A (背景减除+局部形变)    | 1.0            | 在线                   |
| 5-FG-B (Kalman 平滑)          | 0.5            | 在线                   |
| 5-SAL (saliency heatmap)      | 0.5            | 离线累积               |
| **总**                  | **~8.0** |                        |

### 验证标准 (板上 e2e, 每个子段完成时)

1. **5-IPM-A**: `tools/calibrate_6cam` 跑通, 6 路 K/D/R/t 非零; 棋盘格角点两路 IPM 投到同一 (Xpan,Ypan) 误差 < 2 px
2. **5-IPM-B**: 启动后日志 `IPM remap enabled, 1 cm/pixel, panorama 4800x4080`; 1m 棋盘格测得 100±2 px (物理尺度验证)
3. **5-SEAM-B**: 室内有 1 个静止的人跨在 seam 上, 全景图里完整, 没有"半透明 + 重影"
4. **5-FG-A**: 室内有人走动跨 seam, 全景图里这个人**不重影** (单边完整呈现)
5. **5-FG-B**: 录像 30 秒对比, 不开 Kalman 缝抖 5-10 px/frame, 开了 < 2 px/frame

### 关联文档

- `docs/USER_GOAL_ROADMAP.md` § 0.5 核心原则 (新增): 详细论证 + 拒绝方案 + 整条 pipeline
- `docs/USER_GOAL_ROADMAP.md` § 2 Sprint 5: 9 个子段 (5-PRE / 5-IPM-A/B/C / 5-SEAM-A/B / 5-FG-A/B / 5-SAL) 详细计划
- `docs/USER_GOAL_ROADMAP.md` § 3 风险: 9 个子段对应风险 + 兜底
- `docs/USER_GOAL_ROADMAP.md` § 4 PR 边界: 9 个子段对应 PR 边界
- `docs/USER_GOAL_ROADMAP.md` § 6 参考实现点: 17 个文件/函数指针
- `docs/USER_GOAL_ROADMAP.md` § 7 硬约束关系: 9 个子段对应硬约束影响

### 2026-07-15 修正: AANAP 复活 (Zhihu 文章 + 用户反馈触发)

> **触发**: 用户引用 Zhihu `question/34535199/answer/135169187` (截图 `docs/PixPin_2026-07-15_13-00-22.png`) 关于多平面拼接算法的讨论, 指出 IPM 是单 H 不能处理室内墙-地转角等 3D 多平面场景, AANAP 的 content-preserving warp (per-image 相似变换 + 全局 H) 能显著改善。

**修正前 vs 修正后**:

- **修正前 (2026-07-14 v3.4 初始)**: 拒绝 AANAP, 主路径 IPM-only + Seam。 假设"Y=0 单平面"已够。
- **修正后 (2026-07-15)**: AANAP 接受为**静态多平面主路径**, IPM 降为可选物理尺度。 AANAP 解决静态多平面 (墙-地转角), Seam 解决动态立体人, 两者各管一摊, **合成 kernel 按 fg_mask 切换**:
  - 动区 (fg_mask > 0): seam 单边, 物理隔离
  - 静区 (fg_mask = 0): AANAP 混合, 墙-地自然过渡 + seam ±3px 窄带

**为什么 IPM 不够 (修正前过度简化)**:

- IPM 是单 H, 假设整个场景在 Y=0 地面。 把 Z>0 的 3D 点 (墙、桌面、人) 强行投到地面, 立体物"拍扁"失真。
- 室内多平面 (墙-地转角 90°, 墙-天花 90°): cam0 和 cam1 看同一转角, 投到地面的"地面位置"不同, 转角处重影。
- AANAP 解决: 全局 H 估大平面姿态, per-image T_sim 保持直线 (墙保持直, 转角处自然过渡)。

**为什么 AANAP 也不能解决动态 parallax (但不冲突)**:

- AANAP 是 2D warp, 对立体动态人 (头在 Z=0.4m) 仍然"画错", 跟 IPM 一样。 但 AANAP 不会让 seam 单边的方案失效 — 动态区走 fg_mask 单边, 不依赖 AANAP 估的 warp。
- **两者互补不互斥**: AANAP 管静区自然过渡, Seam 管动区物理隔离, 合成 kernel 按 fg_mask 切换。

**算法选型矩阵 (修正后)**:

| 场景                | AANAP                    | Seam                   | 谁来做               |
| ------------------- | ------------------------ | ---------------------- | -------------------- |
| 静态地面 (Y=0)      | OK                       | OK                     | AANAP 更自然         |
| 静态墙 (Z 不同)     | OK (T_sim 保持直线)      | bad (硬切会看到两段墙) | AANAP 必上           |
| 墙-地转角 (3D 折线) | OK (T_sim 沿直线 anchor) | so-so 硬切会切到转角   | AANAP                |
| 静态高位物 (桌面)   | so-so 透视仍拖           | bad                    | AANAP + 5-IPM-C 多 Z |
| 立体动态人          | bad 2D warp 搞不定       | OK 单边无重影          | Seam 必上            |
| 阴影在地面          | OK AANAP 不分            | OK 切到阴影外          | AANAP 自然融合       |
| 快速走动跨 seam     | bad                      | OK 形变跟上            | Seam + PushSeam      |

**文档同步**:

- `docs/USER_GOAL_ROADMAP.md` § 0.5.5 接受/拒绝清单: AANAP 从拒绝移到接受 (主路径)
- `docs/USER_GOAL_ROADMAP.md` § 2 Sprint 5: 重新组织为 5-PRE → 5-CAL-A → 5-AANAP-A → 5-AANAP-B → 5-SEAM-A/B → 5-FG-A/B → 5-SAL → 5-IPM (可选)
- `docs/USER_GOAL_ROADMAP.md` § 3 风险 / § 4 PR / § 6 参考 / § 7 硬约束: 同步 AANAP
- `AGENTS.md` 核心模块表: 新增 `src/aanap_warp.cc/.h`
- `HISTORY.md` v3.4 段 (本节): 标题 + 修正小节
- `HISTORY.md` timeline: v3.4 行更新措辞

**Sprint 总工时调整**:

- 修正前: 5-PRE → 5-IPM-A/B/C → 5-SEAM-A/B → 5-FG-A/B → 5-SAL, ~7.5 周
- 修正后: 5-PRE → 5-CAL-A → 5-AANAP-A → 5-AANAP-B → 5-SEAM-A/B → 5-FG-A/B → 5-SAL → 5-IPM (可选), ~8.5 周 (MVS 不含 5-IPM, 7 周)
- `AGENTS.md` 当前状态: v3.4 forward-looking note + 核心模块表 (新增 4 个文件) + 6路硬编码表 (新增 6 行)

---

## 0. 设计演进（一句话过完）

| 时点          | 关键事件                                                                                                                                                                                                                                                                                                                                                 |
| ------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 初始          | OpenCV UMat, 4K×4-cam > 200fps（1080Ti）                                                                                                                                                                                                                                                                                                                |
| 2026-03       | 切 4K 输入；运行时调参 env vars；KMat/RMat 改动                                                                                                                                                                                                                                                                                                          |
| 2026-03-26    | **性能回归**: warp 并行改串行，FPS 50→个位；修复                                                                                                                                                                                                                                                                                                  |
| 2026-04-01    | 集成 RK 硬件解码 (rkmpp + RGA)；目标转向 DMA-BUF 零拷贝                                                                                                                                                                                                                                                                                                  |
| 2026-04-03    | bootstrap 用 OpenCV 特征检测 ROI → 裁剪坐标复用（无 remap, RGA 不支持）                                                                                                                                                                                                                                                                                 |
| 2026-04-17 起 | SDL2 可视化调参；ROI YAML 持久化；多帧 ROI bootstrap (`NUM_BOOTSTRAP_FRAMES=3`, 阈值 0.25/0.7)                                                                                                                                                                                                                                                         |
| 2026-06-29    | **板子选型**: rocktech RK3588 (6 路物理 MIPI 直连) 中选                                                                                                                                                                                                                                                                                            |
| 2026-06-30    | DTS 6 路 sensor 节点验证（详 §1 DTS）                                                                                                                                                                                                                                                                                                                   |
| 2026-07-06    | **架构硬约束锁定**: 弃 FFmpeg rkmpp（vendor 不维护 + ABI 不兼容, 0 帧），改 gstreamer1.0-rockchip1 mppvideodec `dma-feature=true`                                                                                                                                                                                                                |
| 2026-07-07    | **绿条纹修复**: `mppvideodec` stride 上报偏小 → RGA + cvtColor 错位；POSIX `realpath()` 解决 yaml 路径; batch_transcode (mp4v→h264)。**6 路 100+ fps @ 100% DMA-BUF, 2×3 端到端跑通**                                                                                                                                                 |
| 2026-07-08    | **v3.0**: 6 路 IP camera RTSP 改造（PoE 8+2 交换机, 镜头 2.8mm 102.5°），取代 v2.x 的本地 mp4 / MIPI 计划                                                                                                                                                                                                                                         |
| 2026-07-15    | **v3.5 (in-progress)**: 6 路 GC4683 MIPI 真接入 (rkisp_mainpath /dev/video66..111, sensor 拔占位走 BlackFrameProvider); 详见 `docs/BOARD_VERIFICATION_STATUS.md`                                                                                                                                                                                 |
| 2026-07-14    | **v3.4 架构重新规划**: 前景/后景分离 (后景 IPM LUT + 静态 seam 离线算; 前景硬切+局部形变+背景减除实时), 拒绝 DH/APAP/全幅 α blend; 详见本文件 v3.4 节 + `docs/USER_GOAL_ROADMAP.md` § 0.5 + § 2 Sprint 5 (5-IPM-A/B/C + 5-SEAM-A/B + 5-FG-A/B + 5-SAL)                                                                                        |
| 2026-07-15    | **v3.4 修正: AANAP 复活**: 用户引用 Zhihu 截图 (`docs/PixPin_2026-07-15_13-00-22.png`) 指出 IPM (单 H) 不能处理室内墙-地等多平面, AANAP 的 content-preserving warp (per-image 相似变换 + 全局 H) 显著改善; AANAP 从拒绝清单移到主路径 (静态多平面), IPM 降为可选物理尺度; 详见本节 2026-07-15 修正 小节 + `docs/USER_GOAL_ROADMAP.md` § 0.5.5 |
| 2026-07-15    | **v4l2-ctl ISP 取流确认**: 厂商命令 `width=1280,height=960,pixelformat='UYVY' --stream-mmap=4` 是 `rkisp_mainpath` 标准取流法 (≥ 2.4MB/帧)。**板上默认格式 2560×1440 NV12 ISP (= 5.5MB/帧, 6 路 33GB/s) 是 2K stitch pipeline 唯一正解**。GC4683 raw sensor 没内置 ISP，是 SoC 端 rkisp 处理。详见本节 §2.9 + §3 两条 🔴             |

---

## v3.5 当前快照 - Sprint 4-MIPI 代码已就绪 (2026-07-15)

> **一句话**: 6 路 GC4683 MIPI 接入代码全部写完, 待用户在板端做 SCP+SSH 实跑验收。
> 详细 checklist / 后续 Sprint 依赖图见 **[`docs/BOARD_VERIFICATION_STATUS.md`](BOARD_VERIFICATION_STATUS.md)**。

### v3.5 已落地的代码 (HEAD 之后未 commit)

| 文件                                          | 类型      | 行         | 关键点                                                                                                                                                                                      |
| --------------------------------------------- | --------- | ---------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `include/gst_mpp_decoder.h` / `.cc`       | modified  | +14 / +34  | `StartMipi(device, io_mode, n_buf, w, h)` + `BuildMipiPipeline()`: v4l2src + capsfilter NV12 + appsink; device 节点 stat 失败立即返 false                                               |
| `include/sensor_data_interface.h` / `.cc` | modified  | +51 / +186 | `BlackFrameHolder { DrmBuffer drm; w, h; }` + `kBlackFrame` storage + `static SensorDataInterface::BlackFramePushLoop(i, holder, this)` 30 fps 占位; per-thread mipi 失败 -> 占位循环 |
| `src/app.cc`                                | modified  | +20        | App ctor 加`[App] [Sprint4-MIPI] startup(N)` summary log                                                                                                                                  |
| `params/camera_sources.yaml`                | rewritten | -          | 6 路`type: mipi`; `device_path` 改 HISTORY.md § 2.9 实测 rkisp_mainpath (`/dev/video66/75/84/93/102/111`); cam1 / cam5 物理 sensor 暂拔, 由 BlackFrameProvider 接住                  |
| `tools/probe_v4l2.sh`                       | new       | 5.9 KB     | 优先枚举 NV12 + 2560x1440 的 rkisp_mainpath 节点;`--edit-yaml` 自动 patch yaml; 强制 `--stream-count=N` (HISTORY.md § 3 坑)                                                            |
| `tools/sprint4_mipi_bootstrap.sh`           | new       | 3.2 KB     | 一键 probe -> cmake + make -> 启 image-stitching 5s -> 抓 log                                                                                                                               |

### v3.5 板端实跑路径 (用户 PowerShell 跑)

> ⚠️ **sandbox TCP 出站被拦**: 我在 sandbox 内无法 SSH 到板端 (Permission denied port 22)。
> 用户需在他自己的 PowerShell 跑:
>
> ```powershell
> scp -r include/src/params/tools git push  # 或 scp 文件清单 (见 BOARD_VERIFICATION_STATUS § 3.1)
> ssh rocktech@192.168.137.100 "cd ~/Projects/new-4k-stitch && bash tools/sprint4_mipi_bootstrap.sh"
> ssh rocktech@192.168.137.100 "grep -E 'decoder_perf|BlackFrame|gst_mpp_decoder|Sprint4-MIPI' /tmp/stitch.log"
> ```
>
> 或者直接在板端 ssh 跑 `bash tools/sprint4_mipi_bootstrap.sh`。
> PC 端一键 SCP+SSH 脚本 **`tools/_push_and_verify.ps1`** - 我中断在 Step 3, **没写到磁盘**; 用户要的话给个 "yes" 我接着干。

### v3.5 已知风险 + 仍待补小坑

| 项                                  | 详情                                                                                                                             |
| ----------------------------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| raw-Bayer 防呆                      | `BuildMipiPipeline()` 没在开头加 "pixelformat 必须是 NV12" 检查 - 用户中断在 Step 3。建议补 `v4l2-ctl -d $node --get-fmt-video |
| `tools/_push_and_verify.ps1`      | **不存在** (中断在 Step 3)。                                                                                               |
| `tools/sprint4_mipi_bootstrap.sh` | 默认行为已 OK, 但`--bg` 模式下还需要 `kill <pid>` 收尾 (脚本会 print PID)。                                                  |
| Board IP / 板用户名                 | yaml / scripts 都用`rocktech@192.168.137.100`, 不一致请手动 sed。                                                              |

### v3.5 后续 Sprint 5/6 任务依赖 (per USER_GOAL_ROADMAP § 2)

```
Sprint 4-MIPI -> board verify  ->
   Sprint 5-IPM-A   1.5 周   离线 6 路 K/R/t 标定 + IPM LUT
   Sprint 5-IPM-B   1 周     实时 IPM remap (RGA + CV_32FC2)
   Sprint 5-SEAM-A   1 周     GraphCut 默认 seam 落盘
   Sprint 5-SEAM-B   1 周     硬切 + 3px 窄带
   Sprint 5-FG-A     1 周     BgSubtractor + PushSeam
   Sprint 5-FG-B     0.5 周   Kalman
   Sprint 5-SAL      0.5 周   saliency
   Sprint 6-A        1 周     UI 4->6 cam + width/height
   Sprint 6-B        0.5 周   /api/roi POST x/y/w/h
   Sprint 7          1 周     end-to-end 180° 覆盖验证
合计 ~7.5 周 (per 文档)
```

---

## 1. DTS 实测结论（2026-06-30, 厂商 2026-07-02 确认）

**6 路物理 MIPI 直连硬件 OK**, DTS 6 个 sensor 节点分布在 6 个 i2c 控制器下：

| Cam  | sensor 节点 | i2c 控制器   | mipi-csi2  | dphy                | xvclk |
| ---- | ----------- | ------------ | ---------- | ------------------- | ----- |
| cam1 | gc4683@31   | i2c@feaa0000 | mipi0-csi2 | csi2-dcphy0         | 0x176 |
| cam2 | gc4683_1@31 | i2c@feab0000 | mipi1-csi2 | csi2-dcphy1         | 0x17a |
| cam3 | gc4683_2@31 | i2c@fead0000 | mipi5-csi2 | dphy0/3（厂商不用） | 0x183 |
| cam4 | gc4683_3@31 | i2c@fec80000 | mipi2-csi2 | csi2-dphy2          | 0x1b0 |
| cam5 | gc4683_4@31 | i2c@feac0000 | mipi3-csi2 | csi2-dphy4          | 0x17f |
| cam6 | gc4683_5@31 | i2c@fec90000 | mipi4-csi2 | csi2-dphy5          | 0x1b3 |

**厂商确认 intentional 缺口（不需修）**：

- `csi2-dphy0` / `csi2-dphy3` `status=disabled` → 该板这 2 个 dphy 不用
- `dovdd/dvdd/avdd-supply "supply not found"` warnings → GC4683 不用 DTS 的 supply 节点控制电压

**DTS 自动验证**：`sudo bash tools/post_flash_test.sh`（烧录新镜像后一键跑 11 个测试项，PASS/WARN/FAIL 报告）

**详细 sysfs 探测命令** 详 `AGENTS.md` 历史版块（保留在 git log）；debug 时优先跑 `post_flash_test.sh`。

---

## 2. 操作手册

### 2.1 构建

```bash
# 板上（自主编译, 板载 gcc/make/cmake）
mkdir -p build && cd build
cmake ..
make -j$(nproc)
./image-stitching
```

- 依赖：OpenCV ≥ 4.5、gstreamer-1.0 + gstreamer1.0-rockchip1（mppvideodec 必备）、OpenCL、EGL、GLESv2、GBM、librga、libdrm、SDL2
- FFmpeg 仍链接（仅做转码工具用，不走解码）
- **PC 不交叉编译 image-stitching**（板上 cmake/make 自主）

### 2.2 FFmpeg (rkmpp) 多架构编译 — 教程坑

来源：[https://github.com/nyanmisaka/ffmpeg-rockchip/wiki/Compilation](https://github.com/nyanmisaka/ffmpeg-rockchip/wiki/Compilation)

**⚠️ 不要照搬教程默认 configure**：`--prefix=/usr --enable-rkmpp ...` 在多架构 Debian/Ubuntu 上会装错路径（`.so` 到 `/usr/lib/`, ld 找 `/usr/lib/aarch64-linux-gnu/`），"装上了但项目链不到，继续 fallback 系统 ffmpeg 4.4（无 rkmpp）"。

**必须改的 configure**：

```bash
./configure --prefix=/usr \
            --libdir=/usr/lib/aarch64-linux-gnu \
            --shlibdir=/usr/lib/aarch64-linux-gnu \
            --incdir=/usr/include/aarch64-linux-gnu \
            --enable-gpl --enable-version3 \
            --enable-libdrm --enable-rkmpp --enable-rkrga \
            --enable-shared --disable-static
make -j$(nproc)
sudo make install
sudo ldconfig
```

- 漏 `--enable-shared`：只编 `.a` 静态库，项目链不到
- 漏 `--libdir=`：装到 `/usr/lib/`（无 arch），同样链不到
- ffmpeg 6.x `install-pkgconfig` target 不存在，`.pc` 由 `install-headers` 装，不要手拼

**验证**：`./ffmpeg -decoders | grep rkmpp` 列出 `h264_rkmpp`, `hevc_rkmpp` 等。

### 2.3 SSH 连接 — 网络配置

**rocktech 镜像网络模型（2026-07-06 实测）**：

- **没有 `networking.service`**，ifupdown 未装
- **NetworkManager 是唯一网络管家**
- `/etc/network/interfaces` 不被读（触发 NM ifupdown plugin 把接口标 `unmanaged` → `nmcli con up` 报 "No suitable device found"）
- **结论**：**任何网络配置走 `nmcli`**，**不要**动 `/etc/network/interfaces` 和 `systemctl restart networking`

#### 临时连接配置（重启失效）

1. 手动启用网卡并设置 IP（假设您想用 192.168.137.100）

sudo ifconfig eth0 192.168.137.100 netmask 255.255.255.0 up

2. 添加默认网关（根据您的网络环境调整）

sudo route add default gw 192.168.137.1

3. 测试能否 ping 通 PC

ping 192.168.137.1   # 请替换为您的 PC 以太网实际 IP

#### 以太网 + Windows ICS（推荐现场）

PC 端开启 ICS（自动 IP `192.168.137.1`），板子用网线直连。板端：

```bash
sudo cp /etc/network/interfaces /etc/network/interfaces.bak.$(date +%Y%m%d)
sudo tee /etc/network/interfaces > /dev/null <<EOF
# Managed by NetworkManager (this file is not read on this image)
EOF

sudo nmcli con add type ethernet ifname eth1 con-name "netplan-eth1" \
    ipv4.method manual \
    ipv4.addresses 192.168.137.100/24 \
    ipv4.gateway 192.168.137.1 \
    ipv4.dns "8.8.8.8 114.114.114.114" \
    autoconnect yes

sudo systemctl restart NetworkManager   # 不重启接口仍 unmanaged!
sleep 3
sudo nmcli con up "netplan-eth1"

nmcli con show --active
ip -4 addr show eth1 | grep inet
ping 192.168.137.1   # PC 端
```

PC 连入：`ssh rocktech@192.168.137.100`（默认用户；sudo -i 进 root）

#### WiFi 直接连（推荐桌面开发）

```bash
nmcli device wifi connect "MyWiFi" password "MyPasswd" ifname wlan0
sudo ip link set wlan0 up   # 若 soft blocked
```

**⚠️ Windows 移动热点 DHCP 兼容性差**：经常报 `IP configuration could not be reserved`。**直接绑静态 IP**：

```bash
sudo nmcli connection modify "MyWiFi" \
    ipv4.method manual \
    ipv4.addresses 192.168.137.200/24 \
    ipv4.gateway 192.168.137.1 \
    ipv4.dns "8.8.8.8 114.114.114.114"
sudo nmcli connection down "MyWiFi" && sudo nmcli connection up "MyWiFi"
```

（Windows 移动热点自带网段 `192.168.137.0/24`，PC 端 IP 通常是 `192.168.137.1`，用 `ipconfig` 确认）

#### 持久化静态 IP（实测有效 2026-07-14, 目标 192.168.137.100）

如果 `ifconfig` 临时设 IP 后 `sudo reboot`，会发现 IP 丢了 — 因为临时 ifconfig 不落盘。本节给出**断电重启后 IP/路由/DNS 仍然保留**的标准做法。**所有命令以 `rocktech` 用户跑（`sudo -i` 进 root）**，符合 §2.4 账户约定。

**实测发现：本板 image 默认状态有 4 个坑需要先填，再做 nmcli 静态配置**：

| # | 坑                                                                                                                                   | 现象                                           | 修复                                                                             |
| - | ------------------------------------------------------------------------------------------------------------------------------------ | ---------------------------------------------- | -------------------------------------------------------------------------------- |
| 1 | `NetworkManager.service` `disabled`                                                                                              | 开机 NM 不起，依赖 NM 的连接也不起             | `sudo systemctl enable --now NetworkManager`                                   |
| 2 | `netplan-eth0` 是 DHCP（`ipv4.method=auto`，`autoconnect=yes`）                                                                | 重启后 DHCP 抢占 eth0，把我们的静态 IP 抢走    | `sudo nmcli connection delete netplan-eth0`                                    |
| 3 | `netplan-eth1` 是 192.168.1.10/24 静态 + `/etc/network/interfaces` 里也有 `auto eth1` 块                                       | eth1 被 NM 标`unmanaged`，扰乱 device 状态   | 删`netplan-eth1` 并把 `/etc/network/interfaces` 里 eth1 块清空（备份后覆盖） |
| 4 | NM 自动给 eth0 创的内存`eth0` device profile（`autoconnect=no`，**未落盘到 `/etc/NetworkManager/system-connections/`**） | `nmcli con show` 看着像有静态 IP，重启后消失 | 用`nmcli connection add` 显式创建并 `autoconnect-priority=100` 让它真的落盘  |

**完整命令（一次执行，重启后 IP 锁死 192.168.137.100）**：

```bash
# 0. 备份现有 /etc/network/interfaces（带时间戳）
sudo cp /etc/network/interfaces /etc/network/interfaces.bak.$(date +%Y%m%d_%H%M%S)

# 1. 清空 /etc/network/interfaces (避免 NM ifupdown plugin 标 unmanaged)
sudo tee /etc/network/interfaces > /dev/null <<'EOF'
# Managed by NetworkManager. /etc/network/interfaces is NOT used on this image.
# (no networking.service; NM is the sole network daemon).
# Kept as fallback if ifupdown is later installed.
EOF

# 2. 启 + 开机自启 NetworkManager
sudo systemctl enable --now NetworkManager

# 3. 删干扰连接（DHCP 抢占 + eth1 残留）
sudo nmcli connection delete netplan-eth0   # DHCP 抢占
sudo nmcli connection delete netplan-eth1   # eth1 干扰

# 4. 删 NM 自动建的内存 eth0 device profile（autoconnect=no, 未落盘）
sudo nmcli connection delete eth0           # 若提示 "not found" 可忽略

# 5. 创建持久化静态连接（关键：autoconnect-priority=100 才会落盘生效）
sudo nmcli -w 15 connection add \
    type ethernet \
    con-name eth0-static \
    ifname eth0 \
    ip4 192.168.137.100/24 \
    gw4 192.168.137.1 \
    ipv4.dns "223.5.5.5 8.8.8.8" \
    connection.autoconnect yes \
    connection.autoconnect-priority 100

# 6. 立即激活（这一步会替换临时 ifconfig 的 IP，SSH 闪断一次重连即可）
sudo nmcli connection up eth0-static
```

**落盘文件位置**：`/etc/NetworkManager/system-connections/eth0-static.nmconnection`（NM 启动时读取，断电后保留）：

```ini
[connection]
id=eth0-static
type=ethernet
autoconnect-priority=100
interface-name=eth0

[ipv4]
address1=192.168.137.100/24,192.168.137.1
dns=223.5.5.5;8.8.8.8;
method=manual
```

**验证**：

```bash
# 落盘检查
sudo cat /etc/NetworkManager/system-connections/eth0-static.nmconnection
nmcli -t -f NAME,DEVICE,STATE,AUTOCONNECT,AUTOCONNECT-PRIORITY connection show eth0-static
# 期望: eth0-static  eth0  activated  yes  100

# 当前状态
nmcli -t -f DEVICE,STATE,CONNECTION device status | grep eth0
ip -4 addr show eth0 | grep inet
ip route | grep default

# === 关键：断电重启验证 ===
sudo reboot
# 等板子起来后 SSH 进去
ssh rocktech@192.168.137.100
uptime                                            # 应显示 "up 0/1/2 min"
nmcli -t -f DEVICE,STATE,CONNECTION device status | grep eth0   # connected:eth0-static
ip -4 addr show eth0 | grep inet                  # 192.168.137.100/24
ip route | grep default                           # default via 192.168.137.1 dev eth0
systemctl is-active NetworkManager                # active
systemctl is-enabled NetworkManager               # enabled
ping -c2 192.168.137.1                            # 通
```

**回滚**（如果想恢复 DHCP）：

```bash
sudo nmcli connection delete eth0-static
sudo nmcli connection up eth0   # NM 重建 device profile, 走 DHCP
# 或者重建 netplan-eth0 (DHCP)：
sudo nmcli -w 10 connection add type ethernet con-name netplan-eth0 ifname eth0 \
    ipv4.method auto autoconnect yes autoconnect-priority 0
```

#### 通用排错

| 症状                                       | 处理                                                                                                             |
| ------------------------------------------ | ---------------------------------------------------------------------------------------------------------------- |
| `No route to host`                       | 不在同一子网；ICS 确认 PC 端`192.168.137.1`，WiFi 确认同一 SSID                                                |
| `Permission denied (publickey)`          | 清旧 host key:`ssh-keygen -R 192.168.137.100`                                                                  |
| `nmcli: command not found`               | 镜像不带 NetworkManager，改走 wpa_supplicant                                                                     |
| `IP configuration could not be reserved` | WiFi 关联成功但 DHCP 拿不到 IP，绑静态 IP 跳过 DHCP                                                              |
| IP 变了后 VSCode Remote 拒连               | `ssh-keygen -R <new_ip>`                                                                                       |
| `nmcli: NetworkManager is not running`   | 开机 NM 没自启：`sudo systemctl enable --now NetworkManager`                                                   |
| 设了静态 IP 重启后还是 DHCP                | image 自带`netplan-eth0` DHCP `autoconnect=yes` 抢占；先 `nmcli connection delete netplan-eth0` 再新建静态 |
| `eth1: unmanaged` (预期外)               | `/etc/network/interfaces` 里残留 `auto eth1` 块；清空该文件后 `sudo systemctl restart NetworkManager`      |

### 2.4 账户与项目目录

- 默认用户 `rocktech`（`/home/rocktech`），sudo 提权，**不要日常用 root**
- 项目目录：★ `/home/rocktech/image-stitching/`（推荐，无需 sudo，重烧后 `git clone` 拉回即可）
- ❌ `/userdata/` **不是抗重烧分区**：主板 A 的 `/userdata` 与 `/` 同 rootfs（实测 `df -h` 没第二条 mount），刷系统后**全部丢失**
- board CMakeLists.txt:10 默认 `$HOME/dev/ffmpeg60`，rocktech 账户下解析为 `/home/rocktech/dev/ffmpeg60`

### 2.5 EGL 环境检查（无桌面）

```bash
echo "$DISPLAY" "$WAYLAND_DISPLAY" "$XDG_RUNTIME_DIR"
unset DISPLAY WAYLAND_DISPLAY   # 纯终端被错误设置时, 先 unset

ls -l /dev/dri   # 需要 card0 + renderD128
ldconfig -p | grep -E 'libEGL|libGLESv2'
eglinfo --display surfaceless
# 无 eglinfo: apt install -y mesa-utils mesa-utils-extra libegl1-mesa libegl1-mesa-dev \
#   libgles2-mesa libgles2-mesa-dev libgbm1 libdrm2 libdrm-dev
```

启动后在 `[RkGlesWarper]` 日志检查：`EGL_VENDOR` / `EGL_VERSION` / `EGL_CLIENT_APIS` 应指 RK GPU 驱动；`EGL_EXTENSIONS` 含 `EGL_KHR_image_base`, `EGL_EXT_image_dma_buf_import`, `EGL_KHR_gl_texture_2D_image`。`eglInitialize` + `eglCreateContext` + `eglMakeCurrent` + EGLImage 入口点全成功。

### 2.6 硬件监控

```bash
sudo cat /sys/class/devfreq/27800000.gpu/load    # GPU 负载
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
free -h
cat /sys/class/thermal/thermal_zone*/temp
top -p $(pidof image-stitching)
```

历史瓶颈（v1, 4K）：decode/fetch ~121ms, stitch ~16ms。目标（v2.5+, 2K）：rkmpp → DMA-BUF → RGA/GPU 端到端零拷贝 → 当前实测 **6 路全 100+ fps @ 100% DMA-BUF**。

### 2.7 ROI bootstrap 行为（`SKIP_BOOTSTRAP`）

ROI bootstrap（`BootStrapOptimalLayout`）是为**相机位置不确定**的 PC 桌面调试场景设计（用 `t40`/`h40` 等数据集反复试不同位置）。**车载 / 固定支架**场景跑 bootstrap 是浪费：

- 启动慢 ~600ms（捕获 3 帧 + OpenCV feature detect）
- 低质量视频（如 `t00`/`t30`）触发低置信度分支，需要二次重试
- 输出 `roi_tuning.yaml` 是 feature matching 结果，**不是几何真值**

**`SKIP_BOOTSTRAP` 语义矩阵**（与用户 2026-06-30 确认）：

| `USE_ROI_CONFIG` | `SKIP_BOOTSTRAP` | YAML 存在 | YAML 缺失                              | 行为                                    |
| ------------------ | ------------------ | --------- | -------------------------------------- | --------------------------------------- |
| 0                  | 任意               | 忽略      | 忽略                                   | 强制跑 bootstrap，不读 yaml（旧调试用） |
| 1（默认）          | 0（默认）          | 用 yaml   | 跑 bootstrap 兜底                      | 旧行为，PC 调试友好                     |
| 1                  | **1**        | 用 yaml   | **跑 bootstrap 兜底 + 警告日志** | **固定支架标准姿势**              |

车载标准：`USE_ROI_CONFIG=1 SKIP_BOOTSTRAP=1 ./image-stitching`，缺文件不崩。

代码改动位置：`src/app.cc:35` 定义、`src/app.cc:1066-1067` env 解析、`src/app.cc:909-925` 入口模式日志。

### 2.8 性能调参口诀

整体鼓瘪调 `FOCAL_SCALE`；接缝不顺调 `CY_OFFSET`；左右不接调 `CX_OFFSET`。

### 2.9 摄像头 ISP 取流 (v4l2-ctl, 6 路 2K ISP pipeline) — 2026-07-15 实测

**✅ 推荐（板子 2K stitch pipeline 唯一正解）**：抓 ISP 处理后的 2K NV12（5.5MB/帧），不是 raw BA10。

```bash
# 单路抓 N 帧 ISP NV12 2560×1440 = 5,529,600 B/帧
v4l2-ctl -d /dev/video<NNN> \
  --set-fmt-video=width=2560,height=1440,pixelformat=NV12 \
  --stream-mmap=3 --stream-count=<F> \
  --stream-to=./cam<N>_2k_nv12.yuv
```

**⚠ 一定写 `--stream-count=N`**。缺了它 v4l2-ctl 无限抓流，**`kill -9` 会让 ISP queue 卡死**（见 §3 🔴 坑）。

#### 6 路 rkisp_mainpath 节点对应（2026-07-15 实测）

| /dev/video        | 驱动           | cam  | 物理状态                             | `BA10` raw 节点                            |
| ----------------- | -------------- | ---- | ------------------------------------ | -------------------------------------------- |
| `/dev/video66`  | rkisp_mainpath | cam1 | ✅ 接 cam1 (gc4683@10)               | `/dev/video0`  (rkcif stream_cif_mipi_id0) |
| `/dev/video75`  | rkisp_mainpath | cam3 | ✅ 接 cam3 (gc4683_2@31)             | `/dev/video3`  (rkcif-mipi5-csi2)          |
| `/dev/video84`  | rkisp_mainpath | cam4 | ✅ 接 cam4 (gc4683_3@10)             | `/dev/video5`  (rkcif-mipi2-csi2)          |
| `/dev/video93`  | rkisp_mainpath | cam5 | ⚠ sensor 已断（cam2/cam5 拔下待换） | —                                           |
| `/dev/video102` | rkisp_mainpath | cam6 | ✅ 接 cam6 (gc4683_5@31)             | `/dev/video13` (rkcif-mipi4-csi2)          |
| `/dev/video111` | rkisp_mainpath | cam2 | ⚠ sensor 已断（待换）               | —                                           |

> cam↔video 映射（DT suffix: gc4683_N → camN+1），cam2/cam5 物理断开但 media entity 仍在。

#### 完整 v4l2 输出格式（rkisp_mainpath 支持）

按 `--list-formats` 实测：

```
'UYVY'  (UYVY 4:2:2, 1280×960 = 2,457,600 B/帧)   # 厂商文档范例
'NV16'  (Y/UV 4:2:2)
'NV61'  (Y/VU 4:2:2)
'NV21'  (Y/VU 4:2:0)
'NV12'  (Y/UV 4:2:0, 2560×1440 = 5,529,600 B/帧) # 2K pipeline 用
'NM21'  (Y/VU 4:2:0 N-C, AfbcOutputCompressed)
'NM12'  (Y/UV 4:2:0 N-C, AfbcOutputCompressed)
```

#### 解码 UYVY 2K NV12 → PNG 快速验证（板上）

```python
# /tmp/uyvy_to_png.py — UYVY 4:2:2 packed → PNG
import numpy as np, cv2
data = np.fromfile("cam1.yuv", dtype=np.uint8)
img = cv2.cvtColor(data.reshape(960, 1280, 2), cv2.COLOR_YUV2BGR_UYVY)
cv2.imwrite("cam1.png", cv2.convertScaleAbs(img, alpha=2.0, beta=20), [cv2.IMWRITE_PNG_COMPRESSION, 3])

# NV12 4:2:0 → PNG (默认 2560×1440)
data = np.fromfile("cam1.yuv", dtype=np.uint8)
img = cv2.cvtColor(data.reshape(2160, 2560), cv2.COLOR_YUV2BGR_NV12)
cv2.imwrite("cam1.png", cv2.convertScaleAbs(img, alpha=2.0, beta=20))
```

#### IPC pipeline (`media-ctl -p /dev/media6` 实测链路)

```
gc4683 (SGRBG10_1X10 2560×1440)
  └─► csi2-dphy → mipi-csi2 → rkcif-mipi-lvds
                     └─► rkisp-isp-subdev (demosaic + 3A + ISP tuning + CCM + gamma)
                          ├─► rkisp_mainpath  → /dev/video66 (cam1)
                          ├─► rkisp_selfpath  → /dev/video67 (缩放下采样)
                          ├─► rkisp_fbcpath   → /dev/video68 (AFBC)
                          └─► rkisp_iqtool    → /dev/video69 (调参)

注：`rkisp0-vir0` 包含 cam1/2/3（一个 ISP 硬件服务 3 路由轮流），
`rkisp1-vir0` 服务 cam4/5/6。vir0 = ISP 输入端主导。
```

#### GC4683 没有 built-in ISP（厂商说错！）

- 驱动源 `gc4683_主板.c:488` / `gc4683_模组.c:487`：`.bus_fmt = MEDIA_BUS_FMT_SGRBG10_1X10` (仅 raw bayer)
- media-ctl 拓扑：sensor entity `m00_f_gc4683 N-XXXX` 只有 1 个 Source pad → csi2-dphy
- `v4l2-ctl ... --get-subdev-fmt` 输出 `MEDIA_BUS_FMT_SGRBG10_1X10`，无 YUV 能力
- 厂商应是把 GC4683 当 GC4653 谈的（GC4653 出 YUV，GC4683 只出 raw）
- **真实 ISP：Rockchip SoC 的 rkisp**，需要 `/etc/iqfiles/gc4653_*.json` tuning（当前 vendor 用 GC4653 tuning 顶替，偏绿系）



按场景分四档，全部在开发板上直接跑：

| 场景                                                       | 命令                                                                                                                                          | 关键参数                                                                            |
| ---------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------- |
| **🟢 1 行单帧快照（最快）**                          | `v4l2-ctl -d /dev/video66 --set-fmt-video=width=2560,height=1440,pixelformat=NV12 --stream-mmap=3 --stream-count=1 --stream-to=/tmp/f.nv12` | 改`videoNN` 选 cam；加 `--stream-count=N` 多帧                                  |
| **🟢 多路同步 + 自动命名** （最常用）                | `python3 tools/grab_sync.py --mode isp --duration 5`                                                                                        | 自动扫 rkisp_mainpath → cam1..N；可加`--cams 66,75,84,102`                       |
| **🟢 4 路 H264 mp4** （要喂 OpenCV 直接读）          | `bash tools/capture_mp4_4cam.sh`                                                                                                            | mp4mux + SIGINT 优雅 EOS，否则 moov 丢                                              |
| **🟢 探 + patch + 构建 + 启** （Sprint 4-MIPI 一键） | `bash tools/sprint4_mipi_bootstrap.sh --probe-only` 或 `--bg`                                                                             | probe 出 cam 节点后自动改`camera_sources.yaml`；不接 `--bg` 直接前台 5 秒后退出 |

---

## 3. 踩过的坑（必读, 2026-07-08 汇总）

### 🟢 绿条纹 (2026-07-07 发现, 2026-07-08 修复)

**症状**：image-stitching 跑起来后, 拼接输出每路都有大量绿色条纹，不是原数据集画面。

**根因（双层）**：

1. **Codec mismatch**：`datasets/2k-test/*.mp4` 是 MPEG-4 Visual (mp4v)，但 pipeline 强制 `qtdemux → h264parse → mppvideodec`。caps 不匹配时 gstreamer 用 identity bypass 部分数据 → 链路碎裂
2. **Stride 上报偏小（主因）**：`mppvideodec dma-feature=true` 输出的 DMA-BUF 实际 stride = **2816**（kernel 64-byte 对齐），gstreamer caps 上报 = **2560**。后端 RGA + `cv::cvtColor(NV12 → BGR)` 拿 2560 算 UV 偏移, UV 整段错位 → cvtColor 把字节当 UV 重采样 → 满屏绿条纹

**修复方案**：

```bash
# 一次性转码 + 一键修复（板上）
bash tools/fix_green_stripes.sh    # 转码 + sed yaml + rebuild + orphan-kill + restart + log tail
tail -f logs/image-stitching.log | grep DIAG    # 看 layout=1（单 fd）还是 layout>=2（多 fd）
```

**PC 端推送**：`bash sync_green_stripe_fix.sh`（默认 `rockemb` alias）

**转码器**：`tools/batch_transcode_to_h264.sh` 用 `ffmpeg -c:v libx264 -movflags +faststart`。**绝不**用 `h264_rkmpp`（vendor 已确认会 segfault）。libx264 sw 编码在 aarch64 板上 ~100x 实时, 6 路 2K 数据集 < 1 分钟转完。

**诊断 `[DIAG]` 段** (`src/gst_mpp_decoder.cc:DIAG`, v2.5 新增)：

| `[DIAG]` 输出                                                | 含义                                                                                                                                                                                                    |
| -------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `caps: video/x-raw(memory:DMABuf),format=NV12,w=2560,h=1440` | mppvideodec 正确输出 DMA-BUF + NV12                                                                                                                                                                     |
| `mem_count=1 ... dmabuf=yes fd=N`                            | **单 fd, Y+UV 连续, OK**                                                                                                                                                                          |
| `mem_count=2 fds=[Y_fd,UV_fd]`                               | **POTENTIAL ROOT CAUSE**：两块 DMA-BUF, 当前只取首个 fd → UV 平面丢失 → 绿条纹。修复路径：扩展 `struct GstMppFrame` 加 `int dma_buf_fd_uv; gsize offset_uv;`, 把两个 fd 都喂给 RGA / OpenCL |
| `stride[0,1,2]=2560,2560,0`                                  | NV12 平面 stride 正常（UV stride = Y stride, 最后一平面 padding 0）                                                                                                                                     |
| `offset[0,1,2]=0,Y*H,0`                                      | UV offset = Y plane size, NV12 标准布局                                                                                                                                                                 |

**手动诊断**：`bash tools/diagnose_pipeline.sh [--all]`（yaml + ffprobe + gst-inspect + gst-launch 烟测，一眼定 codec）

### ⚫ VPU / IOMMU 内存耗尽 (2026-07-07)

`image-stitching` 跑超时 (`timeout 15`) 但没优雅退出 → DMA-BUF fd 累计（单进程 56644 个）→ IOMMU 虚拟地址耗尽 (`out of I/O virtual memory -28`) → 后续 mppvideodec **0 帧**, `mpi_dec_test` segfault。

**修复**：`ps aux | grep image-stitch; kill -9 <pid>` 后再跑, 内存从 2.6G free 释放到 7.1G。

**预防**：加 `--max-frame` 上限或 SIGTERM handler。

### ⚫ 不要相信 gstreamer 上报的 stride

**任何 mppvideodec / v4l2 / 自定义 dma-feature 路径都应断言** `buf_size >= stride * 1.5 * height`, 否则被绿条纹咬。`src/gst_mpp_decoder.cc:stride_override` 用 `actual_stride = buf_size / (height * 1.5)` 优先于 gstreamer 上报。

后端下游（RGA + cv::cvtColor）必须用 **kernel stride**（kernel 64-byte 对齐后的实际 stride）, 不能用 gstreamer caps 上报宽度。

### ⚫ C++11 项目不要用 std::filesystem

项目 CMakeLists 锁 C++11, `std::filesystem` 是 C++17。POSIX `realpath(uri.c_str(), resolved)` 替代方案只 5 行, 在 `src/sensor_data_interface.cc:CanonicalizeUri()`。

### ⚫ gstreamer filesrc 不接受路径里有 `..`

即使能 resolve, 含 `..` 时 capsnegotiate 可能 abort。**启动前将 yaml + default 都 `realpath()` 一下**（同样在 `CanonicalizeUri`）。

### ⚫ vendor SDK ffmpeg 不一定带 libx264

板上 RK 自定义 ffmpeg 只 `--enable-rkmpp` 不带 sw encoder, 无法转码 dataset。**data 转码方案**：

- sudo `apt install x264` 装 CLI，写 `.264` → mp4 muxer
- 或 `tools/batch_transcode_to_h264.sh`（项目已用 ffmpeg + libx264 sw encoder）

### ⚫ h264_rkmpp 编码/vendor wrapper 维护不周

`h264_rkmpp` 编码器实测 segfault；FFmpeg rkmpp 解码 0 帧（vendor 明确"FFmpeg 存在版权, 不做支持", kmpp 内核模块**仅编码器**部分）。**不要尝试这些路径**, 一律 `gstreamer1.0-rockchip1 mppvideodec`。

### ⚫ 4K 搜索带宽硬编码历史 bug（v2 已修复）

`src/app.cc` 早期 `search_w = NormalizeEvenFloor(std::min(common_w / 2, 1920))` 硬编码 1920。**v2 阶段已改为 `search_w = common_w / 2`**（2K = 1280, 4K = 1920 自动覆盖），2×3 迁移**不需要再重新推导上限**。

### ⚫ MMU / IOMMU 建表性能暴降 (45 → 20 FPS)

**原因**：每帧 `clImportMemoryARM` 唤醒 IOMMU 重建物理页表, `clReleaseMemObject` 又销毁（系统调用 + TLB flush + Pin Pages, ms 级）。

**解决**：`std::unordered_map<int, cl_mem> dma_buf_cache_`（`src/image_stitcher.cc`）, key = DMA-BUF fd, value = 建好的 `cl_mem`。V4L2/RKMPP Ring Buffer 池化（4-16 个 FD）, 缓存稳态 O(1) 查表, 100% 命中。生命周期在 `ImageStitcher::CleanupOpenCL()` 一次性释放。

**2×3 迁移**：FD 池翻倍, 验证 cache 命中仍覆盖工作集。

### ⚫ AFBC DRM_PRIME 不可读

`av_hwframe_transfer_data` 无法读 AFBC 压缩面, **必须强制线性 (非 AFBC) 输出**。`src/sensor_data_interface.cc:609` 已经 `av_opt_set(codec_context->priv_data, "afbc", "0", 0)`, 改解码器时**不要去掉**。

### ⚫ NV12 → RGBA 多次 RGA 转换极其消耗带宽

**不要**用多次 RGA 做裁剪+旋转+羽化（颜色空间逆转换浪费带宽）。

**推荐方案**：RGA 只做裁剪/旋转（仿射部分），重叠的一小条接缝区域交给 OpenCL（ARM 扩展 `cl_arm_import_memory` 零拷贝导入 NV12 DMA-BUF, kernel 直接对 Y + 交织 UV 加权混合, 输出到目标 NV12 缓冲区）。

**不要**用 OpenGL ES + FBO 输出 RGBA 再转 NV12（需 `GL_EXT_YUV_target` 罕见扩展, 再多一次 RGA 颜色空间逆转换）。

### ⚫ RGA 只能做仿射

RK3576 librga 只支持平移/缩放/正交旋转, 无法柱面/球面/透视等非线性 warp。

**解**：Mali GPU + GLES Fragment Shader 走 EGLImageKHR 实现 DMA-BUF 零拷贝导入 + 非线性形变渲染（`src/rk_gles_warper.cc`）。GLES 初始化失败时静默回退 ROI+RGA+OpenCL, **当前不作为主路径**（布局由 ROI 驱动）。

### ⚫ 数据集质量差异（PC 调试选匹配对）

- 较好：`t40`, `h40`（重叠充分、光照稳定）
- 较差：`t00`, `t30`, `t50`（低重叠或光照漂移, 易触发"画面重复"瑕疵）
- 2×3 测试也要选匹配对

### ⚫ v4l2-ctl 流命令缺 `--stream-count` 会无限抓流 + ISP queue 卡死（2026-07-15）

**症状**：照搬厂商文档的 `v4l2-ctl --stream-mmap=4 --stream-to=./out.yuv`（没 `--stream-count=`）跑测试，`v4l2-ctl` 永远不退出。`kill -9` 后再跑任何 ISP 取流命令都失败：

- `VIDIOC_STREAMON returned -1 (Operation not permitted)` (EPERM)
- 或 `rkisp0-vir0: check rkisp_mainpath link or isp input` 在 dmesg 里反复
- `media-ctl --reset` 救不回，反而让所有 link 失能 + STREAMON 变 EPERM

**根因**：`--stream-mmap=N` 配合缺 `--stream-count` 时 v4l2-ctl 默认无限抓流（`-1`），并在按 `| head -30` 截 stdout 时被 SIGPIPE 杀掉。`kill -9` 让 REQBUFS 中已分配的 mmap buffer 留在 ISP 队列里，driver 内部 `rkisp_s_fmt_vid_cap_mplane queue busy` 卡死。

**修法**：

1. **预防**：所有 v4l2-ctl 流命令**必须**加 `--stream-count=N`（要多少帧写多少；`--stream-count=1` 验证路径、`--stream-count=$(($FPS*$DURATION))` 抓视频）。
2. **复位（如果忘了 count）**：

```bash
# 先看 dmesg 是否有 "queue busy" 决定是否要重启
dmesg | grep -E "queue busy|check rkisp_mainpath"

# 试 soft reset
media-ctl -d /dev/media6 --reset
media-ctl -d /dev/media6 -l '"rkisp-isp-subdev":2->"rkisp_mainpath":0[1]'

# soft reset 救不回来（出现 EPERM），只能重启
sudo reboot
```

重启后 6 个 rkisp_mainpath（v66/75/84/93/102/111）会自动重新分配。

参考：[`docs/HISTORY.md §2.9`](#29-摄像头-isp-取流-v4l2-ctl-6-路-2k-isp-pipeline--2026-07-15-实测)。

---

### ⚫ GC4683 没有 built-in ISP（厂商口径错误，2026-07-15）

**症状**：厂商回复"GC4683 自带 ISP，你抓 raw 就是没走 ISP"。但实测无 YUV 能力。

**根因**：

- 驱动源（[GC4683drivers/主板厂家/gc4683_主板.c:488](../GC4683drivers/主板厂家/gc4683_主板.c) / [模组原厂](../GC4683drivers/模组原厂/gc4683_模组.c)）：`.bus_fmt = MEDIA_BUS_FMT_SGRBG10_1X10` —— **sensor 只声明 10-bit raw bayer，没任何 YUV 出口**
- `media-ctl -d /dev/media6 -p`：sensor entity `m00_f_gc4683 2-0010` 只有 1 个 Source pad → csi2-dphy，**不是 sensor-ISP 输出**
- `v4l2-ctl -d /dev/media0 --subdev-get-fmt` 返回 `MEDIA_BUS_FMT_SGRBG10_1X10`，没 YUV 能力
- 厂商应是把 GC4683 当 **GC4653** 谈：GC4653 内部 ISP 出 YUV，GC4683 是 raw sensor 出 bayer

**真相**：ISP 是在 **Rockchip SoC 端** rkisp 硬件做的（demosaic + 3A + CCM + gamma），需要 `/etc/iqfiles/` 下放 tuning json（当前 vendor 用 GC4653 tuning 顶替 GC4683，全局偏绿）。

**捕捉 ISP 处理结果**：必须走 `/dev/videoN` 的 **rkisp_mainpath 节点**，不是 raw 入口。参考 [`docs/HISTORY.md §2.9`](#29-摄像头-isp-取流-v4l2-ctl-6-路-2k-isp-pipeline--2026-07-15-实测)。

kernel 除以 2。同样的约束对单相机裁剪宽度生效（`NormalizeEvenFloor` / `NormalizeEvenCeil`, `app.cc:63-69`）。

### ⚫ BuildCameraRois2xN() 抛 std::runtime_error

当 ROI < 2×2 时抛 "invalid 2xN crop mapping for camera N"。2×3 版必须保留这个安全 throw。

### ⚫ BootStrapOptimalLayout 保留第一帧

保留 `image_vector_` 即便后来选了更优的帧 — **有意的**。循环后的 `ExportHardwareFrameToBgr` 复用 `image_vector_` 来保存 debug 裁剪图。不要"修"这个行为。

### ⚫ assets/ 是历史源码（不参与 build）

`.github/industrial-coding.instructions.md` 错误地说头文件可以放那里 — **忽略该建议**。同样 `StitchingParamGenerator` 已初始化但**不参与**主 pipeline；ROI 驱动布局是当前活跃路径。

### ⚫ NV12 格式贯穿

Y 平面 + 交织 UV。`stride_w`/`stride_h` 可能 ≠ `width`/`height`（查 `nv12_frame.h`）。

---

## 4. 调试开关速查

| 变量                           | 默认 | 含义                                   |
| ------------------------------ | ---- | -------------------------------------- |
| `SAVE_STITCH_FRAMES`         | 1    | 落盘拼接结果图；**性能测时置 0** |
| `SAVE_DIAGNOSTIC_FRAMES`     | 1    | 落盘诊断图（ROI 置信度等）             |
| `SAVE_FRAME_INTERVAL`        | 30   | 保存间隔（帧）                         |
| `DIAGNOSTIC_FRAME_LIMIT`     | 3    | 诊断图上限                             |
| `STITCH_DEBUG_LEVEL`         | 0    | 拼接调试 verbosity                     |
| `RK_GLES_WARPER_DEBUG_LEVEL` | 0    | GLES warp 调试 verbosity               |

**性能测标准姿势**：`SAVE_STITCH_FRAMES=0 SAVE_DIAGNOSTIC_FRAMES=0 ./image-stitching`

---

## 5. DMA-BUF / 零拷贝机制（背景）

- DMA-BUF 是内核内存管理机制, 设备可直接访问物理内存而无需复制
- 数据始终在同一块物理内存, 各设备操作完后通过同步机制（fence 等）互相告知"我读/写完了", 避免数据竞争
- 参考：[https://zhuanlan.zhihu.com/p/1942149087869800464](https://zhuanlan.zhihu.com/p/1942149087869800464)
- 池化分配两个特征：(1) **集合有界** V4L2/RKMPP 一次性划拨固定数量（4-16）物理内存块；(2) **时间局部性** 帧连续处理时用 Ring Buffer 轮转
- 二者结合使 Hash Map Cache 缓存冷启动后达到 100% 命中, 把昂贵的系统调用 + IOMMU 建表开销降为 O(1)

---

## 6. 引用

- `AGENTS.md` — Agent 入口 / 编码规范 / 6 路硬编码位置速查 / 环境变量 / 可视化键盘映射
- `docs/NETWORK_CAMERA_PLAN.md` — v3.0 当前活跃计划（6 路 IP camera, Sprint 0/1/2）
- `docs/CAMERA_PAGE_INTEGRATION.md` — 浏览器管理平台（C++ http server, 端口 8080）方案 + 阶段 1 部署
- docs/RTSP_OUTPUT_PLAN.md — v3.1 输出侧计划（拼接全景 RTSP 服务化, 端口 8554, `rtsp://<board>:8554/stitch`）
- `src/gst_mpp_decoder.cc:DIAG 段` — 现场一拉 log 就看到 caps + stride 真相
- `tools/post_flash_test.sh` — 烧录新镜像后一键 DTS 验证
- `tools/sprint0_smoke.sh` — v3.0 Sprint 0 骨架贯通验收（PASS 才进 Sprint 1）

> 参考论文：Du, Chengyao, et al. (2020). *GPU based parallel optimization for real time panoramic video stitching.* Pattern Recognition Letters, 133, 62-69.

## 7. v3.2 (2026-07-09): 帧差掩码 + 2 路 RTSP 推流

**目标**: 拼接完 1 帧后, 计算前后两帧差值生成掩码, 推两路 RTSP (`/stitch` 全景 + `/stitch_diff` 帧差掩码).

**新增模块**:

- `include/frame_diff.h/cc` (`FrameDiff`): NV12→BGR→absdiff→threshold→findContours. 输出 BGR mask (背景 = current × 0.4, 前景 = 红色高亮 + bbox). 持 prev frame 缓存, 线程不安全 (假定 stitch 线程单线程).
- `include/output_streams.h/cc`: yaml 顶层 `output:` 块解析. 加载 `ServerConfig` (port, auth, on_demand, diff_threshold...) 和 `StreamConfig[]` (path, width, height, fps, bitrate, encoder).
- `include/gst_rtsp_server.h/cc` (`GstRtspServer`): 单例, gst-rtsp-server 包装. 每路输出 = 1 个 GstRTSPMediaFactory (shared media) + 1 个 per-stream `StreamPipeline` (生产者 → mutex+cv 队列 → pump 线程 → appsrc → `mpph264enc` → `rtph264pay`).

**Pipeline 模板** (per stream):

```
appsrc name=appsrc_<path> format=time is-live=true do-timestamp=true block=false max-bytes=0 \
    caps="video/x-raw,format=BGR,width=W,height=H,framerate=F/1" \
  ! videoconvert ! video/x-raw,format=NV12 \
  ! mpph264enc bps=BITRATE*1000 rc-mode=cbr gop=FPS*2 \
  ! h264parse ! rtph264pay name=pay0 pt=96 config-interval=1
```

**集成点**:

- `App::InitRtspOutput()` (run_stitching 开头调一次): 读 yaml `output:` 块 → 构造 FrameDiff → GstRtspServer::Start(port=8554, streams=[/stitch, /stitch_diff])
- stitch loop 内 (BlendSeams 之后): 转 BGR → push `/stitch`; FrameDiff::ComputeMask → push `/stitch_diff`. 每帧都跑, appsrc 端 `max-buffers=1` 丢旧帧.
- `App::~App()`: `GstRtspServer::Stop()` (detach glib loop, join pump threads).

**yaml 配置** (params/camera_sources.yaml 顶层 `output:`):

```yaml
output:
  enabled: true
  bind_address: 0.0.0.0
  port: 8554
  auth_enabled: false
  auth_user: admin
  auth_pass: CHANGE_ME
  on_demand: false
  diff_threshold: 30
  diff_bbox_min_area: 100
  streams:
    - path: /stitch
      width: 0
      height: 0
      fps: 30
      bitrate_kbps: 4000
      encoder: h264_hw
    - path: /stitch_diff
      width: 0
      height: 0
      fps: 30
      bitrate_kbps: 2000
      encoder: h264_hw
```

**操作**:

```bash
ffplay rtsp://board-ip:8554/stitch
ffplay rtsp://board-ip:8554/stitch_diff
ffplay -fflags nobuffer -rtsp_transport tcp rtsp://board-ip:8554/stitch
```

**新增编译依赖**:

- 板端: `apt install gstreamer1.0-rtsp-server-1.0`
- CMake: `pkg_check_modules(GSTRTSPSERVER REQUIRED IMPORTED_TARGET gstreamer-rtsp-server-1.0)` + 链 `PkgConfig::GSTRTSPSERVER`

**性能影响 (估)**:

- `FrameDiff` 1 帧: NV12→BGR (5-15ms) + absdiff+threshold+findContours (3-8ms) = ~10-23ms@2K
- 2 路 mpph264enc 1080p30 HW: ~5ms×2 = 10ms
- 总: stitch (~13ms) + diff (~15ms) + 2x encode (~10ms) = ~38ms, 略超 33ms 预算. 实际可能在 RGA NV12→BGR 复用上抢回来. 不行就把 diff fps 限到 15.

**踩坑提示**:

- 不要把 `stitch_status::shutdown` 之类的清理写在 `}` 外面 (file scope). 手动加 cleanup 时容易踩 — 已修.
- OpenCV FileStorage 在 GBK locale 的 PC 上读 UTF-8 yaml 会报 `Input file is invalid` — PC 问题不是 board 问题, 板端 UTF-8 locale 正常.

**踩坑提示 (2026-07-09 实测补充)**:

- 板端 apt 找不到 `gstreamer1.0-rtsp-server-1.0` 时 (rocktech 部分镜像裁过 universe):
  - 试 `sudo apt install libgstrtspserver-1.0-dev` (有的镜像只用这个名, pkg-config 能识别 `libgstrtspserver-1.0`)
  - 还不行: 编译会 WARN, 链不会失败 (CMake 自动把 `HAVE_GST_RTSP_SERVER` 设 0)
  - 运行时: RTSP 相关函数变成 no-op, 拼接功能不受影响
  - yaml 端: 此时即使设 `output.enabled: true` 也不会启 server, 但也不会崩
  - 想强制关掉 RTSP: yaml 顶层 `output.enabled: false`
- 写 `cmake_config.h.in` 让 CMake 把 pkg 检测结果传成宏 (`HAVE_GST_RTSP_SERVER`), 源码 #if 跳过.
- `appsrc_/stitch` 这种带 / 的 element name 在 gstreamer 0.x 是非法的, 现版本基本宽容但最好 sanitize. v3.2 已经做 `c == '\\\'' ? '\\\''_\\\'' : c` 把 / 换成 _.
