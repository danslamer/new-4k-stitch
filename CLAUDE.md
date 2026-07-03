# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 入口说明 (重要)

本文件是项目 Agent/Claude 入口，集中所有开发规范、迁移清单、调试开关和操作手册。**详细设计文档**在:

- [README.md](README.md) — 项目目标、Quick start、架构总览、环境变量
- [docs/HISTORY.md](docs/HISTORY.md) — 完整设计迭代时间线、踩过的坑、v1/v2 决策依据、FFmpeg/EGL/SSH/性能调优操作手册
- [docs/DEVELOPMENT_PLAN.md](docs/DEVELOPMENT_PLAN.md) — **完整开发方案 v2.2** (6 阶段详细步骤、命令、验收标准、故障排查), 主入口文档
- [tools/post_flash_test.sh](tools/post_flash_test.sh) — 烧录新镜像后, 自动测试 6 路 sensor 适配状态 (一键 PASS/WARN/FAIL 报告)
- [docs/CAMERA_PAGE_INTEGRATION.md](docs/CAMERA_PAGE_INTEGRATION.md) — CameraPage 浏览器管理平台接入方案 (v2.4, 当前阶段 1 完成)
- [docs/CAMERAPAGE_TEST_DEPLOY.md](docs/CAMERAPAGE_TEST_DEPLOY.md) — CameraPage 测试版部署指南 (v2.4 阶段 1, 板端已跑通 PID 84321)

## 仓库定位

实时多摄像头视频拼接器，Rockchip ARM64 板端运行：

- **Pipeline**: FFmpeg rkmpp HW decode → DMA-BUF 零拷贝 → RGA 裁剪/旋转 (+ 可选 GLES warp) → OpenCL 接缝羽化 → DRM 输出
- **主开发板**: **rocktech RK3588** (Ubuntu 22.04.5 LTS, kernel 5.10.226, 内置 GC4683 驱动, 厂商规格 6 路物理 MIPI 直连)
- **CameraPage 适配** (v2.4): C++ 嵌入式 cpp-httplib HTTP server (端口 8080), 浏览器查看状态/配置; 阶段 1 完成, 阶段 2 视频流暂停 (等 vpu 固件)
- **当前输入**: 2K (2560×1440) @ 30fps
- **当前布局**: 2×2 (4 路) → **迁移中** → 2×3 (6 路, 2 列 × 3 行)

## 当前 DTS 适配状态 (2026-07-01 实测)

**6 路 sensor 节点已全部在 DTS**, 分布在 6 个 i2c 控制器下, 链路基本完整.

| Cam | sensor 节点 | i2c 控制器 | csi2-dphy | mipi-csi2 | xvclk |
|---|---|---|---|---|---|
| cam1 | gc4683@31 | i2c@feaa0000 | csi2-dcphy0 | mipi0-csi2 | external-camera-clock (0x176) |
| cam2 | gc4683_1@31 | i2c@feab0000 | csi2-dcphy1 | mipi1-csi2 | external-camera2-clock (0x17a) |
| cam3 | gc4683_2@31 | i2c@fead0000 | csi2-dphy0/3 (待 enable) | mipi5-csi2 | external-camera3-clock (0x183) |
| cam4 | gc4683_3@31 | i2c@fec80000 | csi2-dphy2 | mipi2-csi2 | external-camera4-clock (0x1b0) |
| cam5 | gc4683_4@31 | i2c@feac0000 | csi2-dphy4 | mipi3-csi2 | external-camera5-clock (0x17f) |
| cam6 | gc4683_5@31 | i2c@fec90000 | csi2-dphy5 | mipi4-csi2 | external-camera6-clock (0x1b3) |

**厂商已确认 intentional 的"缺口" (2026-07-02 厂商回复)**:
- ✅ csi2-dphy0 / csi2-dphy3 status=disabled → **没用这两个 dphy**, 设计如此, 不需 enable ([拓扑图](docs/拓扑图.png))
- ✅ dovdd/dvdd/avdd-supply "supply not found" warnings → **sensor 不用这个节点控制电压**, 设计如此, 不需补
- ⏳ GPIO 故意空 (等物理 sensor 接入后, 按 FPC 原理图填)

**详细命令 + 验收标准**: [DEVELOPMENT_PLAN.md § 1.3](docs/DEVELOPMENT_PLAN.md#13-6-路-mipi-拓扑验证-核心--已完成-dts-层)

**自动测试**: 烧录新镜像后, `sudo bash tools/post_flash_test.sh` 一键跑 11 个测试项, 输出 PASS/WARN/FAIL 报告.

## 架构硬约束 (2026-07-02 锁定, 不可改)

> 6 路 4K 拼接要求 30 FPS 端到端, 整个 pipeline 必须满足:

| 层 | 必须用 | 不接受 |
|---|---|---|
| **解码** | FFmpeg rkmpp (`mpeg4_rkmpp` / `h264_rkmpp` / `hevc_rkmpp`) | 软件解码器 (mpeg4 sw 等) |
| **帧缓冲** | DMA-BUF (DRM_PRIME) 零拷贝 | memcpy 到系统内存 (YUV420P / RGBA) |
| **RGA 路径** | RGA2 HW 裁剪/旋转/羽化 | CPU 端的 OpenCV 仿射 (warpAffine) |
| **OpenCL** | Mali GPU 接缝羽化 (clImportMemoryARM) | CPU 端 alpha blend |

**禁止任何 sw fallback** (例如 "临时软解跑一下看效果"):

- sw 帧在 `sensor_data_interface.cc:639-645` 直接 reject + 报错, 不会进入 stitch 路径
- `FindPreferredDecoder()` 强制先 `avcodec_find_decoder_by_name("*_rkmpp")`, 找不到才允许回退 (回退也只是 sw path 之前是 fallback, 但 sw 帧到 stitch 阶段仍 reject)
- `cmake .. -DENABLE_RK_HARDWARE_DECODING=OFF` 是已知错误配置, 会导致 sw 帧进 stitch 路径报错

**理由**: sw mpeg4 软解单路就 30ms+, 6 路就 200ms+, 加上 stitch + 显示肯定 < 5 FPS, 业务目标达不到。

**当前阻塞** (2026-07-02): rkmpp 链路已配齐 (ffmpeg 6.1 rkmpp + mpp 1.3.9), 但 rocktech 镜像缺 vpu/rkvdec 固件 + 缺 kmpp 内核模块, 硬件解码 0 帧. 修法是 vendor 给新镜像 (`deploy/rocktech_support/RK3576_MPP_ISSUE_REPORT.md`), **不需要改任何 C++ 代码**.

## 关键状态：v2 阶段 (2026-07-01 更新)

6 阶段开发方案（完整方案见 [docs/DEVELOPMENT_PLAN.md](docs/DEVELOPMENT_PLAN.md) v2.2）当前进度：

| 阶段 | 状态 | 备注 |
|---|---|---|
| 1. 板子与镜像可用性测试 | ✅ 板端工具链 OK | [§ 1.3 已完成 6 路 sensor 节点](docs/DEVELOPMENT_PLAN.md#13-6-路-mipi-拓扑验证-核心--已完成-dts-层), 自动化测试见 [post_flash_test.sh](tools/post_flash_test.sh), **ffmpeg-rockchip 6.1 rkmpp 多架构装好** ([HISTORY §0x03-2](docs/HISTORY.md#2-ffmpeg-rkmpp-编译参考)) |
| 2. 6 路 CameraSource 配置 (yaml) | ✅ 编译通过 + 6 路 fallback 跑通 | yaml 缩进修对 + try/catch 防解析崩, dataset fallback 6 文件 `t50-t53 + t40 + t41` 已工作, 待真机 rkmpp DRM_PRIME 验证 |
| 3. V4L2 6 路采集线程 | ⏳ 等物理 sensor | V4L2CaptureThread 写 6 路, 暂未跑 |
| 4. 2×3 代码迁移 | ⏳ | 4 路 → 6 路代码改造, 阶段 2 验证完即可开始 |
| 5. 6 路相机标定 (与 3-4 并行) | ⏳ 工具就绪 | calibrate_intrinsics/extrinsics.py 已写, 待 6 路视频 |
| 6. 2×3 真机跑通与收尾 | ⏳ 等 3-5 | 物理 sensor 接入后端到端跑 |

**📋 拿到开发板后的具体操作**: 立刻看 [DEVELOPMENT_PLAN.md §1 板子与驱动可用性测试](docs/DEVELOPMENT_PLAN.md#1-板子与驱动可用性测试-1-2-天-) (★ SSH → 驱动 probe → 6 路 MIPI 拓扑验证)

**精简原则** (vs v1):
- 内核驱动适配 / 厂商发包 → **rocktech 厂商负责**, 我方只做镜像烧写后的可用性测试 (kernel 6.1.75 已内置 GC4683 v00.01.01)
- 应用编译 → **开发板自主** (板上 gcc/make/cmake), **PC 不交叉编译** image-stitching
- 时间戳同步 → **删除** (6 路 MIPI 硬件同步, 同 ISP, 漂移可忽略)
- **4 路过渡态 → 跳过**, 直接做 6 路 2×3 (从当前 4 路代码改造, 不做 4 路单独验证)

**v2 阶段输入从 4K 改为 2K** (2560×1440 @ 30fps)，已完成的具体改动：

| 位置 | v1 (4K) | v2 (2K) |
|---|---|---|
| `src/app.cc:192` `EstimateOverlapByTemplate` 搜索带宽 | `min(common_w/2, 1920)` (4K 中心线) | `common_w/2` (自适应, 2K 时 = 1280) |
| `src/app.cc:267` `EstimatePairOverlap` ORB 带宽 | 同上 magic number | 同上 |
| `src/roi_visualizer.cc:171` SDL 窗口标题 | `"4K Stitch ROI Tuner"` | `"2K Stitch ROI Tuner"` |
| `params/camera_sources.yaml` 4 路 width/height | (无 yaml) | 2560×1440, 6 路 (`t50-t53 + t40 + t41`) |
| `tools/calibrate_intrinsics.py` 注释 | (无) | 2560×1440, fx ~ 1055 px |
| `CMakeLists.txt:10-17` FFmpeg 路径 | `$ENV{HOME}/dev/ffmpeg60` (别板遗留) | `/usr/lib/aarch64-linux-gnu` 默认 + `CACHE PATH` 可覆盖, 配多架构 ffmpeg-rockchip |
| `src/sensor_data_interface.cc:347-392` `LoadCameraSourceList` | 无 try/catch (yaml 解析崩会 terminate) | try/catch 包住 cv::Exception + std::exception, 解析报错 fallback 到默认 dataset |
| `src/roi_visualizer.cc:158` `SDL_SetHint(SDL_HINT_VIDEODRIVER, ...)` | 老 SDL 2.0.22+ 头文件已删, 编译失败 | `setenv("SDL_VIDEODRIVER", ...)` 跨版本兼容 |

**为什么 1920 magic number 改成 `common_w/2`**：2K 输入时自然 = 1280, 4K 输入时自然 = 1920, 不再硬编码, 对任何分辨率自适应；2×3 迁移时**不需要重新推导上限**。

## 开发板选型 (2026-06-29 决策)

两块备选主板实测对比, 详见 [docs/主板系统环境差异.docx](docs/主板系统环境差异.docx)。

**注**: .docx 的 "V4L2 框架探测" 行记录了两块板 dmesg 都显示 "6 · MIPI CSI 成功", 但这是 dmesg 报的虚拟 CSI 设备数, **不代表物理 MIPI 端口数**。物理硬件以实测为准:

| 维度 | **主板 A (rocktech) ✅** | 主板 B (Neardi) |
|---|---|---|
| 内核 | 5.10.226 | 6.1.118 |
| **物理 MIPI 端口** | **6 路直连** (无桥接 / 串行器) | **2 路** (需外接串行器扩展到 6 路) |
| 预置 sensor 节点 | 无 | 有 (os04a10/os08a20/ov13855/ov16880) |
| DTOverlay | 不支持 | 支持 |
| 板载 gcc/make | **有** | 无 |
| **GC4683 适配** | **驱动源码已到位** (`drivers/gc4683.c`), **rocktech 厂商在配合适配** | 无 |

**推荐: 主板 A (rocktech)**。原因:
- 项目目标是 6 路 MIPI 拼接, A 原生 **6 路直连** (无桥接 / 串行器, 信号完整性和时钟同步最优)
- rocktech 厂商正在配合做 GC4683 驱动适配, 仓库内已有 `drivers/gc4683.c` 作为基线
- A 板载 gcc/make, 满足板上自主编译需求

**主板 B 用途**: 仅 2 路物理 MIPI, 不足以支撑 6 路方案; 保留为**单 / 双路测试板**或开发机使用。

## 常用命令

### 构建

```bash
mkdir -p build && cd build
cmake ..                # CMake 3.10+, C++11; ENABLE_RK_HARDWARE_DECODING=ON, ENABLE_RGA_DMA_STITCHING=ON 默认
make
./image-stitching
```

- FFmpeg 路径在 `CMakeLists.txt:10-17` 默认指向 `/usr/lib/aarch64-linux-gnu` (rocktech 板 ffmpeg-rockchip 多架构安装位置). 可 `-DFFMPEG_LIB_DIR=...` 覆盖. **教程的 `--prefix=/usr` 默认配置在多架构系统上会装错路径, 必加 `--libdir=/usr/lib/aarch64-linux-gnu --enable-shared --disable-static`**, 详 [HISTORY §0x03-2](docs/HISTORY.md#2-ffmpeg-rkmpp-编译参考)
- 依赖：OpenCV ≥ 4.5, FFmpeg (rkmpp), OpenCL, EGL, GLESv2, GBM, librga, libdrm, SDL2
- **无单元测试、无 linter、无 CI**——仅在 Rockchip 板端手动验证

### 输入源切换

```bash
# 默认: 数据集模式 (t50..t53.mp4)
unset INPUT_SOURCE_MODE

# 摄像头模式: 走 params/camera_sources.yaml (阶段 1 完成后)
export INPUT_SOURCE_MODE=camera
```

### 性能调试

```bash
SAVE_STITCH_FRAMES=0 SAVE_DIAGNOSTIC_FRAMES=0 ./image-stitching  # 纯 FPS 测
eglinfo --display surfaceless                                       # EGL 链路检查
sudo cat /sys/class/devfreq/27800000.gpu/load                      # GPU 负载
```

## 架构 (高层)

单可执行 `image-stitching`。入口 `src/app.cc` `main()` → `App::run_stitching()` (noreturn loop)。

**三大模式** (`src/app.cc` 定义):
1. **ROI + RGA + OpenCL** (默认): 多帧 ROI → 2×2 布局 → RGA 裁剪 → OpenCL 羽化
2. **GLES Warp + RGA + OpenCL**: GLES 非仿射 warp → RGA 拷贝 → OpenCL 羽化
3. 模式 2 GLES 初始化失败时静默回退到模式 1

**核心模块**:

| 文件 | 职责 |
|---|---|
| `src/app.cc` | 主循环、ROI bootstrap、布局 |
| `src/sensor_data_interface.cc` | 每路相机一个解码/采集线程，队列帧供应 |
| `src/image_stitcher.cc` | RGA/GLES warp, OpenCL 接缝, **`dma_buf_cache_`** |
| `src/rk_gles_warper.cc` | EGL+GLES warp via DMA-BUF import |
| `src/drm_allocator.cc` | DRM dumb buffer 分配 |
| `src/roi_config.cc` + `src/roi_visualizer.cc` | ROI YAML 读写 + SDL2 可视化调参 |
| `drivers/gc4683.c` | GC4683 sensor 驱动**基线参考** (kernel 6.1.75 已内置 v00.01.01, 真跑用板端内核驱动, 这个源不进 build) |
| `tools/calibrate_*.py` | 离线相机标定工具 (A3 棋盘 11×14, 22mm 方格) |

**Pipeline 细节**: FFmpeg rkmpp HW decode → DRM_PRIME zero-copy → RGA crop/rotate (and optionally GLES warp) → OpenCL seam feathering → DRM output buffer.

## 2×3 迁移 (6 路) 硬编码点速查

| 位置 | 硬编码内容 | 应改为 (2×3 = 6 cams) |
|---|---|---|
| `include/roi_config.h:14` | `RoiOffset roi_offsets[4]` | 扩到 `[6]` |
| `src/app.cc:172-180` `BuildDefaultTuning()` | `for (i = 0; i < num_cameras && i < 4; ++i)` | 解除 `i < 4` 上限 |
| `include/app.h:21-35` `CachedOverlap` | 4 对字段 (h01/h23/v02/v13) | 扩到 7 对 (h01/h12/h34/h45 + v03/v14/v25) |
| `src/app.cc:369-440` `EstimateOverlaps2x2()` | 4 对 overlap | 新增 `EstimateOverlaps2x3()` |
| `src/app.cc:442-534` `BuildCameraRois2x2()` | 2×2 `(x,y,w,h)` 布局 | 新增 `BuildCameraRois2x3()` (2 列 × 3 行) |
| `src/app.cc:536-570` `BuildStitchLayout2x2()` | 单象限 output | 新增 `BuildStitchLayout2x3()`，panorama height = 三行之和 |
| `src/app.cc:592-749` `BootStrapOptimalLayout()` | 打印 `h01/h23/v02/v13_score` 日志 | 改为新 overlap key |
| `src/image_stitcher.cc` `WarpImages` & `BlendSeams` | 循环 `for (i = 0; i < num_img_; ++i)` | 已数据驱动, 6 路自动 work |
| `src/roi_visualizer.cc` Tab 键 | 4 个 cam ID | 扩展到 6 个 |
| `params/camchain_0..3.yaml` | 4 份标定 | 扩到 `camchain_0..5.yaml` (6 份) |
| `params/roi_tuning.yaml` | `cam0..cam3` keys | 扩到 `cam0..cam5` |
| `RoiConfig::LoadFromFile` | 固定 4-cam 解析 | 改为动态解析 |

**yaml 框架已支持 6 路**: `InitVideoCapture` 用 `INPUT_SOURCE_MODE` 二选一 (`dataset` 默认, `camera` 走 yaml)。阶段 3 之前, `camera` 模式下 yaml 的 `type: mipi` 会被跳过 (日志告警)。6 路扩展时只需在 yaml 加 cam4/cam5 块, 不动 C++ 代码。

## 命名规范

- 类: PascalCase (`ImageStitcher`)
- 函数: snake_case (`load_parameters`)
- 常量: UPPER_SNAKE_CASE
- 全局变量: `g_` 前缀 + snake_case (`g_debug_level`, `g_feather_width`)
- 相机调参 offset 数组按**物理相机编号**索引, 不按网格位置

## 调试开关 (均在 `src/app.cc` 或 `include/app.h`)

- `g_debug_level` (0=OFF, 1=INFO, 2=DEBUG, 3=VERBOSE)
- `g_debug_opencl_feathering`, `g_save_roi_confidence_debug`, `g_save_stitched_frames`
- `g_feather_width` (像素, **必须偶数** — kernel 除以 2), `g_feather_strength` (S-curve, >1.0 = 更平滑)
- `g_multi_frame_roi_debug_level` (在 `include/app.h:16`, 默认 1)
- `g_enable_visual_tuning` (env `ENABLE_VISUAL_TUNING`, 默认 ON via `app.cc:30`)
- `g_show_roi_markers` (env `SHOW_ROI_MARKERS`, 默认 1; debug 模式下 M 键切换)
- `g_use_roi_config` (env `USE_ROI_CONFIG`, 默认 1 = 读 `params/roi_tuning.yaml`; 0 = 强制重检并覆盖)
- `g_skip_bootstrap` (env `SKIP_BOOTSTRAP`, 默认 0; 车载 GC4683 固定云台场景置 1, 详 [HISTORY.md §6.5](docs/HISTORY.md))

## 环境变量

| 变量 | 用途 |
|---|---|
| `SAVE_STITCH_FRAMES`, `SAVE_DIAGNOSTIC_FRAMES`, `SAVE_FRAME_INTERVAL`, `DIAGNOSTIC_FRAME_LIMIT` | 落盘控制 |
| `INPUT_SOURCE_MODE` | `dataset` (默认) 或 `camera` |
| `STITCH_K_FOCAL_SCALE`, `STITCH_K_FX/FY_SCALE`, `STITCH_K_CX/CY_OFFSET` | 全局 K 矩阵调参 |
| `STITCH_K_FOCAL_SCALE_CAM_0..3` | 单相机焦距缩放 (2×3 需扩到 `_CAM_5`) |
| `STITCH_DEBUG_LEVEL`, `RK_GLES_WARPER_DEBUG_LEVEL` | 调试 verbosity |
| `ENABLE_VISUAL_TUNING` (默认 1) | 显示 SDL2 窗口 |
| `SHOW_ROI_MARKERS` (默认 1) | 画 ROI 边框 |
| `USE_ROI_CONFIG` (默认 1) | 启动时读 `params/roi_tuning.yaml`; `0` = 强制重检并覆盖 |
| `SKIP_BOOTSTRAP` (默认 0) | 固定支架场景显式声明意图: YAML 存在 → 用 YAML; YAML 缺失 → 仍跑一次 bootstrap 兜底 (不报错退出) |

## 可视化调参 (SDL2) 键盘映射

| 键 | 功能 |
|---|---|
| `↑`/`↓`/`←`/`→` 或 `W`/`A`/`S`/`D` | ROI 步进 |
| `Tab` | 上/下一相机 |
| `1` / `5` / `0` / `P` | 步长 1 / 5 / 10 / 50+ |
| `F` | 羽化 toggle |
| `+` / `-` | 羽化宽度 ±10 |
| `B` | 保存 toggle |
| `L` / `K` | 保存间隔 ±10 |
| `E` | 保存配置到 YAML |
| `M` | ROI marker toggle |
| `Q` / `Esc` | 退出 debug 模式 |
| `Enter` / `D` | 进入 debug 模式 |

行为：进入 debug 模式 (`Enter`) 保存当前帧、锁帧索引、暂停新帧拉取。`↑↓←→` 对已保存帧实时重拼。`F/+/-` 触发完整布局重建。`E` 写 YAML。`Q/Esc` 清空已保存帧并恢复实时拼接。

## 已知问题与坑

- **NV12 ↔ RGBA 是带宽杀手** — 多次 RGA 往返不行。用 RGA 做仿射 (裁剪/旋转), 接缝条交给 OpenCL；不要用 GLES FBO + RGBA→NV12 转换代替。
- **MMU / IOMMU 建表代价** — 每次 `clImportMemoryARM` + `clReleaseMemObject` 都重建 IOMMU 页表。`image_stitcher.cc` 的 `dma_buf_cache_` (hash map) 已稳态 O(1)；2×3 时 FD 池翻倍, 验证 cache 命中率仍覆盖工作集。
- **RGA 只能做仿射** (平移/缩放/正交旋转, RK3576 已验证, RK3588 库相同)。非线性 warp (柱面/球面/透视) 必须走 GLES fragment shader (EGLImageKHR → DMA-BUF)。
- **4K 搜索带宽硬编码 bug** (历史, v2 已自适应) — `src/app.cc:192` 早期 `search_w = NormalizeEvenFloor(std::min(common_w / 2, 1920))` 硬编码 1920。v2 阶段改为 `search_w = common_w / 2`, 2K/4K 都自适应。2×3 迁移时**不需要重新推导上限**。
- **Linear (non-AFBC) DRM_PRIME 必须** — `sensor_data_interface.cc:609` 强制 `afbc=0`, 因 `av_hwframe_transfer_data` 无法读 AFBC 面。改 decoder 时**不要去掉**。
- **数据集质量差异** — `t40`/`h40` 质量好, `t00`/`t30`/`t50` 低重叠/光照漂移 (易触发"画面重复"瑕疵)。2×3 测试也要挑匹配对。
- **`g_feather_width` 必须偶数** — kernel 除以 2。同样的约束对单相机裁剪宽度生效 (见 `NormalizeEvenFloor` / `NormalizeEvenCeil` `app.cc:63-69`)。
- **`BuildCameraRois2x2()` 抛 `std::runtime_error`** — 当 ROI < 2×2 时抛 "invalid 2x2 crop mapping for camera N"。2×3 版必须保留这个安全 throw。
- **`BootStrapOptimalLayout`** 保留第一帧作为 `image_vector_` 即便它后来选了更优的帧 — 这是有意的, 循环后的 `ExportHardwareFrameToBgr` 复用当前 `image_vector_` 来保存 debug 裁剪图。不要"修"这个行为。
- **`assets/` 是历史/备份源码** — **不参与 build**，不要编辑以为会生效。`.github/industrial-coding.instructions.md` 错误地说头文件可以放那里 — 忽略该建议。
- **`StitchingParamGenerator`** 已初始化但**不参与**主 pipeline；ROI 驱动布局是当前活跃路径。不要因文件存在而误判。
- **NV12 格式贯穿** — Y 平面 + 交织 UV。`stride_w`/`stride_h` 可能 ≠ `width`/`height` (查 `nv12_frame.h`)。
- **DMA-BUF FD 跨帧复用** — `image_stitcher.cc` 缓存 `dma_buf_cache_` 避免重映射。2×3 时 FD 池翻倍, 验证 cache 命中率。

## 操作手册 (摘要)

**完整操作步骤 (FFmpeg 编译、EGL 检查、SSH、硬件监控、性能调优)** 详见 [docs/HISTORY.md](docs/HISTORY.md) 0x03 章节。关键点速记：

- **解码器测试**: `./ffmpeg -decoders | grep rkmpp` 应列出 `h264_rkmpp`, `hevc_rkmpp` 等
- **AFBC 测试**: `./ffmpeg -hwaccel rkmpp -hwaccel_output_format drm_prime -afbc 1 -i <video> -f null -` (验证 AFBC 路径)
- **EGL 检查**: `unset DISPLAY WAYLAND_DISPLAY` 后 `eglinfo --display surfaceless`；`/dev/dri` 需 `card0` + `renderD128`
- **GPU 监控**: `sudo cat /sys/class/devfreq/27800000.gpu/load`
- **SSH**: `sudo ifconfig end1 192.168.1.123 netmask 255.255.255.0 up` (网卡名按实际替换)
- **性能口诀**: 整体鼓瘪调 `FOCAL_SCALE`；接缝不顺调 `CY_OFFSET`；左右不接调 `CX_OFFSET`
- **历史瓶颈**: decode/fetch ~121ms, stitch ~16ms。目标: rkmpp → DMA-BUF → RGA/GPU 端到端零拷贝

## 工程演进时间线 (摘要)

1. **初始 (OpenCV UMat)**: Fast panorama via UMat, 4K × 4-cam > 200fps on 1080Ti
2. **2026-03-24**: 切 4K 输入；修 `KMat`/`RMat` 避免畸变；加运行时调参 env vars
3. **2026-03-26** (性能回归分析): FPS 从 50+ 跌到个位数。根因: warp 从并行改串行。修: 顺序提交 warp, 去掉 `clone/copyTo`, 减少 GPU 抖动
4. **2026-04-01**: 集成 RK 硬件解码 (rkmpp + RGA)。`NV12 → BGR → UMat` 抵消性能收益 → 目标转向 DMA-BUF 零拷贝
5. **2026-04-03**: bootstrap 改 OpenCV 特征检测 ROI, 保存坐标, 运行时复用 (无 `remap_finish`, RGA 不支持)
6. **2026-04-17 起**: 加 SDL2 可视化 (FPS 叠加、ROI 标记、步进控件、调试模式)；ROI YAML 持久化 (`params/roi_tuning.yaml`)；多帧 ROI bootstrap + 置信度投票 (`NUM_BOOTSTRAP_FRAMES = 3`, 阈值 0.25, 早退 0.7)
7. **GLES warp 路线**: 尝试 `RkGlesWarper` 处理 RGA 搞不定的非仿射 warp, 走 EGLImageKHR + DMA-BUF 导入。初始化失败时静默回退到 ROI+RGA+OpenCL。**当前不作为主路径** — 布局由 ROI 驱动
8. **当前默认**: ROI bootstrap (多帧, 取最高置信度) → 2×2 布局 → RGA 裁剪/拷贝 → OpenCL 羽化。GLES warp 是透明可选的零拷贝加速器, 不决定布局

## 引用论文

> Du, Chengyao, et al. (2020). *GPU based parallel optimization for real time panoramic video stitching.* Pattern Recognition Letters, 133, 62-69.
