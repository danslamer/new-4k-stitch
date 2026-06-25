# HISTORY

Archived from the previous monolithic README. Read this for design rationale, debug history, and operational recipes. For the current 2×2 → 2×3 (6-cam) migration scope and gotchas, see `AGENTS.md`.

> Reference paper: Du, Chengyao, et al. (2020). *GPU based parallel optimization for real time panoramic video stitching.* Pattern Recognition Letters, 133, 62-69.

---

## 0x01 工程升级迭代路线

按时间顺序的工程演进:

1. **初始版本 (OpenCV UMat)**: Fast panorama via UMat, 4K × 4-cam > 200fps on 1080Ti. Pure CPU/GPU stitching using `cv::Stitcher`. 在 rk3576 上对4路 4k 视频可超过 35fps。
2. **2026-03-24**: 调整相机内外参数（KMat/RMat）避免畸变; 添加运行时微调环境变量 `STITCH_K_*`; 输入改为 4K。
3. **2026-03-26 (性能回归分析)**:
   - FPS 从 50+ 跌到个位数。根因: warp 从并行改为串行。
   - 修复: 顺序提交 warp, 去掉 `clone/copyTo` 中间副本, 减少 GPU 抖动。
   - 进一步分析: OpenCL 队列同步等待 (remap_finish 60-150ms) 是新瓶颈; 对比原版发现当前版多了额外同步点和日志输出, 导致表观成本被放大; 原版 80+FPS 可能为提交速度而非真实吞吐。
   - 后处理/落盘是隐藏瓶颈, 关闭保存时 FPS 提升但暴露解码等待。
4. **2026-04-01**: 集成 RK 硬件解码 (rkmpp + RGA)。性能收益被 `NV12 → BGR → UMat` 转换抵消。目标转向 DMA-BUF 零拷贝。
5. **2026-04-03**: 引导阶段用 OpenCV 特征检测 ROI, 保存坐标, 运行时复用坐标做裁剪拼接 (没有 remap_finish, RGA 不支持)。该方案一直保留至今。
6. **2026-04-17 起**: 加入 SDL2 可视化 (FPS 叠加、ROI 标记、步进控件、调试模式进出); ROI 配置 YAML 持久化 (`params/roi_tuning.yaml`); 多帧 ROI 引导 + 置信度投票 (`NUM_BOOTSTRAP_FRAMES = 3`, 阈值 0.25, 早退 0.7)。
7. **GLES warp 路线**: 尝试 `RkGlesWarper` 处理 RGA 搞不定的非仿射 warp (柱面/球面/透视), 走 EGLImageKHR + DMA-BUF 导入。GLES 初始化失败时静默回退到 ROI+RGA+OpenCL。当前已初始化但**不作为主路径** — 布局仍由 ROI 驱动。
8. **当前默认**: ROI 引导 (多帧, 取最高置信度) → 2×2 布局 → RGA 裁剪/拷贝 → OpenCL 羽化。GLES warp 是透明可选的零拷贝加速器, 不决定布局。

---

## 0x02 遇到过的问题 (保留简要描述)

### A. 4K 搜索带宽硬编码导致左右拼接瑕疵

- **症状**: 左右相邻图片检测得分极低 (`h01.score=0.105, h23.score=0.082`), 而上下正常 (`v02=0.584, v13=0.469`), 最终全景图出现"画面重复"瑕疵。
- **根因**: `EstimatePairOverlap` 中水平方向搜索带宽被硬编码上限 1200 px。4K 视频真实左右重叠约 1400~1600 px, 特征匹配在受限的 1200 px 局域内找不到公共参考物, 返回低置信度 → 推导出虚假的偏小重叠值 (1104 px) → 裁切接缝时没有削去足够边缘。
- **修复**: 将 `band_w` / `search_w` 上限提升到 4K 中心线 1920 px (见 `src/app.cc:190` 和 262); 微小偏移可通过 `CameraTuning.offset_x` 调。
- **2×3 迁移注意 (v2 已更新)**: 不要直接复用 1920 — v2 阶段已把 1920 magic number 改为 `common_w / 2` 自适应 (`src/app.cc:192` 和 `:267`), 对 2K/4K 都自动覆盖. 2×3 迁移时**不需要再重新推导上限**, 因为已经是分辨率自适应.

### B. NV12 ↔ RGBA 多次 RGA 转换极其消耗带宽

- 不要用多次 RGA 做裁剪+旋转+羽化; 颜色空间逆转换浪费内存带宽。
- **推荐方案**: RGA 只做裁剪/旋转 (仿射部分), 重叠的一小条接缝区域交给 OpenCL, 通过 ARM 扩展 `cl_arm_import_memory` 零拷贝导入 NV12 的 DMA-BUF, 在 kernel 中直接对 Y + 交织 UV 做加权混合, 直接输出到目标 NV12 缓冲区。
- **不要**用 OpenGL ES + FBO 输出 RGBA 再转 NV12 (需要 `GL_EXT_YUV_target` 罕见扩展, 且多一次 RGA 颜色空间逆转换)。

### C. RGA 只能做仿射变换

- RK3576 上 librga 只支持平移、缩放、正交旋转, 无法做柱面/球面/透视等非线性 warp。
- 解决: 用 Mali GPU + GLES Fragment Shader 走 EGLImageKHR 实现 DMA-BUF 零拷贝导入与非线性形变渲染 (见 `src/rk_gles_warper.cc`)。

### D. MMU / IOMMU 建表性能暴降 (45 → 20 FPS)

- 原因: 每一帧 `clImportMemoryARM` 都要唤醒 IOMMU 重新建立物理页表, 帧末 `clReleaseMemObject` 又销毁, 涉及系统调用、TLB flush、Pin Pages, 是几 ms 级开销。
- 解决: `std::unordered_map<int, cl_mem> dma_buf_cache_` (在 `src/image_stitcher.cc`)。key 是 DMA-BUF fd, value 是建好的 `cl_mem`。Linux V4L2/RKMPP 用 Ring Buffer 池化, FD 集合极小 (4-16 个), 缓存稳态 O(1) 查表, 100% 命中。生命周期在 `ImageStitcher::CleanupOpenCL()` 一次性释放。
- **2×3 迁移注意**: FD 池会大致翻倍, 验证 cache 命中仍覆盖工作集。

### E. AFBC DRM_PRIME 不可读

- `av_hwframe_transfer_data` 无法读取 AFBC 压缩面, 必须强制线性 (非 AFBC) 输出。
- `src/sensor_data_interface.cc:609` 已经 `av_opt_set(codec_context->priv_data, "afbc", "0", 0)`, 改解码器时不要去掉。

### F. 数据集质量差异

- 较好: `t40`, `h40` (重叠充分、光照稳定)。
- 较差: `t00`, `t30`, `t50` (低重叠或光照漂移, 容易触发 "画面重复" 瑕疵)。
- 2×3 测试时也要选匹配对。

---

## 0x03 操作步骤 (从 README 收集)

### 1. 构建

```bash
mkdir build && cd build
cmake ..                # CMake 3.10+, C++11, 默认 ENABLE_RK_HARDWARE_DECODING=ON, ENABLE_RGA_DMA_STITCHING=ON
make
./image-stitching
```

### 2. FFmpeg (rkmpp) 编译参考

- 来源: <https://github.com/nyanmisaka/ffmpeg-rockchip/wiki/Compilation>
- 步骤: 构建 MPP, RGA, FFmpeg (arm64)
- 验证: `./ffmpeg -decoders | grep rkmpp` 应列出 `h264_rkmpp`, `hevc_rkmpp` 等。

### 3. 解码器测试

- 无 AFBC: `./ffmpeg -hwaccel rkmpp -hwaccel_output_format drm_prime -i <video> -f null -`
- 有 AFBC: `./ffmpeg -hwaccel rkmpp -hwaccel_output_format drm_prime -afbc 1 -i <video> -f null -`
- 确认 FFmpeg 链接: `ldd ./image-stitching | grep avcodec` 应为 `so.60`。

### 4. EGL 环境检查 (在 RK3588 / RK3576 终端, 无桌面)

```bash
# 1. 看会话变量
echo "$DISPLAY" "$WAYLAND_DISPLAY" "$XDG_RUNTIME_DIR"
# 纯终端被错误设置时, 先 unset
unset DISPLAY WAYLAND_DISPLAY

# 2. 看 DRM 节点
ls -l /dev/dri
# 需要 card0 和 renderD128

# 3. 看 EGL/GLES 库
ldconfig -p | grep -E 'libEGL|libGLESv2'

# 4. 看 EGL 能力
eglinfo --display surfaceless
# 没有 eglinfo 时装一个
# apt install -y mesa-utils mesa-utils-extra libegl1-mesa libegl1-mesa-dev libgles2-mesa libgles2-mesa-dev libgbm1 libdrm2 libdrm-dev
```

启动程序后, 在 `[RkGlesWarper]` 日志中检查:

- `EGL_VENDOR` / `EGL_VERSION` / `EGL_CLIENT_APIS`
- `EGL_EXTENSIONS` 含 `EGL_KHR_image_base`, `EGL_EXT_image_dma_buf_import`, `EGL_KHR_gl_texture_2D_image`
- 走到 `using EGL_PLATFORM_SURFACELESS_MESA display path` 或至少 `eglInitialize succeeded`
- `GL_RENDERER` 指向 RK 板载 GPU 驱动名

判定标准: `eglInitialize` + `eglCreateContext` + `eglMakeCurrent` + EGLImage 入口点全部成功, 且 `EGL_EXT_image_dma_buf_import` 存在。

### 5. 硬件监控

```bash
# GPU 负载
sudo cat /sys/class/devfreq/27800000.gpu/load

# CPU
htop
```

历史瓶颈: decode/fetch ~121ms, stitch ~16ms。目标: rkmpp → DMA-BUF → RGA/GPU 端到端零拷贝。

### 6. SSH 连接

```bash
# 设主机端静态 IP (网卡名按实际情况替换)
# 开启了ICS网络共享之后PC的IP固定为192.168.137.1
#在开发板上运行以下命令
sudo ifconfig end1 192.168.137.100 netmask 255.255.255.0 up
ping 192.168.1.10
ssh root@192.168.1.10
```

### 7. 性能调优开关 (环境变量)

| 变量 | 含义 |
|------|------|
| `SAVE_STITCH_FRAMES=0` | 关闭最终结果落盘, 纯测 FPS |
| `SAVE_DIAGNOSTIC_FRAMES=0` | 关闭诊断图落盘 |
| `SAVE_FRAME_INTERVAL` | 保存间隔, 默认 30 |
| `DIAGNOSTIC_FRAME_LIMIT` | 诊断图上限, 默认 3 |
| `INPUT_SOURCE_MODE` | `dataset` / `camera`, 默认 dataset |
| `STITCH_K_FOCAL_SCALE` | 全局 鼓瘪 调整 |
| `STITCH_K_FX_SCALE` | 水平鼓瘪 |
| `STITCH_K_FY_SCALE` | 垂直鼓瘪 |
| `STITCH_K_CX_OFFSET` | 垂直接缝对齐 |
| `STITCH_K_CY_OFFSET` | 水平接缝对齐 |
| `STITCH_K_FOCAL_SCALE_CAM_0..3` | 单相机焦距缩放 (2×3 需扩展到 `_CAM_5`) |
| `STITCH_DEBUG_LEVEL` | 拼接调试等级 |
| `RK_GLES_WARPER_DEBUG_LEVEL` | GLES warp 调试等级 |
| `ENABLE_VISUAL_TUNING` | SDL 可视化窗口, 默认 1 |
| `SHOW_ROI_MARKERS` | ROI 边框叠加, 默认 1 |
| `USE_ROI_CONFIG` | 启动时加载 `params/roi_tuning.yaml`; 0=强制重检并覆盖 |

调参口诀: 整体鼓瘪调 `FOCAL_SCALE`; 接缝不顺调 `CY_OFFSET`; 左右不接调 `CX_OFFSET`。

### 8. 仓库与虚拟环境路径参考

- 旧仓库: `/userdata/Projects/yzy/gpu-based-image-stitching-dataset-new/gpu-based-image-stitching/`
- 新仓库: `/userdata/Projects/yzy/new-4k-stitch`
- OpenCV 4.9: `/userdata/Projects/yzy/opencv/opencv-4.9.0/build`
- rknn 虚拟环境: `source /userdata/Projects/rknn-env/bin/activate`

### 9. Git 速查

```bash
git clone <url>
git status
git add . && git commit -m "说明"
git push                  # 首次: git push -u origin <branch>
git pull                  # 分支: git pull origin <branch>
git branch                # 列出
git checkout -b <branch>  # 新建
git reset --hard <commit> # 回退 (谨慎)
```

### 10. OpenCode 会话管理

```bash
opencode session list             # 列出所有会话
opencode --session <ID>           # 恢复指定会话
opencode session delete <ID>      # 删除会话
```

### 11. DMA-BUF / 零拷贝机制

- DMA-BUF 是一种内核内存管理机制, 设备可直接访问物理内存而无需复制。
- 数据始终在同一块物理内存, 各设备操作完后通过同步机制 (fence 等) 互相告知 "我读/写完了", 避免数据竞争。
- 参考: <https://zhuanlan.zhihu.com/p/1942149087869800464>
- 池化分配 (Pool Allocation) 的两个特征:
  1. **集合有界**: V4L2/RKMPP 一次性划拨固定数量 (4-16) 物理内存块。
  2. **时间局部性**: 帧连续处理时用 Ring Buffer 轮转写入 (15→16→17→18→15→…)。
- 二者结合使 `Hash Map Cache` 缓存冷启动后达到 100% 命中, 把昂贵的系统调用 + IOMMU 建表开销降为 O(1) 哈希查表。

---

## 0x04 v2 阶段: GC4683 + 4 路 MIPI + 远期 6 路板

> 本节替代上一版 "MIPI 4 路 + USB 2 路" 假设, 反映 **2026-06 实物确认**: 只用 4 路同型号 GC4683 MIPI 摄像头, 后续更换支持 6 路 MIPI 的开发板, USB 路**完全废弃**。

### 4.1 新约束摘要 (与 v1 假设的差异)

| 维度 | v1 假设 (MIPI 4 + USB 2) | **v2 实际 (MIPI 4 + 远期 6)** | 影响 |
|---|---|---|---|
| sensor 型号 | 通用 2K 摄像头 | **GC4683** (Galaxycore, 4MPixel, 1/2.5", 2.5μm, BSI, RAW 输出, 60fps, DAG/DOL HDR/AOV/Fast AE/Quick start, CSP 封装) | sensor 出 **MIPI RAW**, 必须过 RKISP 才能得到 NV12; sensor 分辨率 2688×1520 (4:3) 而非 2560×1440 (16:9), 需要明确 ROI 模式 |
| MIPI 路数 | 4 | **4 (当下)**, **6 (换板后)** | 当下仍是 2×2 拼接, 2×3 推到换板后; 原本 2×3 + USB 混接的 4 类问题简化为只剩 2 类 (格式 / 时钟) |
| USB 摄像头 | 2 | **0** | 删除: Phase 5 USB 软解 + YUYV→NV12 RGA 转换路径, 4.1 格式不一致问题**全部消失** |
| 帧率 | 30fps | **60fps** (GC4683 物理能力) | 4 路 2K@60 总带宽 ≈ 1.32 GB/s, 是 30fps 的 2 倍; 解码 + 拼接双倍压力 |
| HDR | 无 | DOL HDR (sensor 级) | 决定 ISP 3A 收敛时间; 标定采集时必须等收敛 |
| 时钟同步 | 4 MIPI 软同步 | **4 MIPI 软同步 (现在), 6 MIPI 软同步 (换板后)** | 远期换板后才有 6 路时钟同步问题, 当下只面对 4 路 |
| 光度一致性 | 4 同型号 | **4 同型号 (现在), 6 同型号 (远期)** | 同型号 sensor, ISP 3A 共享, 一致性好; 但 DOL HDR 模式下 EV 切换, 需要确认 4 路 EV 一致 |

### 4.2 GC4683 sensor 关键参数速查

| 参数 | 值 | 标定 / 集成含义 |
|---|---|---|
| 输出格式 | **MIPI RAW** (RGB Bayer) | sensor → MIPI CSI-2 → `rkcif` → **`rkisp` (必走)** → NV12 → 你的工程; **不可绕过 ISP** |
| 物理分辨率 | 2688 × 1520 (4:3) | sensor 全分辨率; 工程输入可能是 2560 × 1440 (16:9 裁剪) 或 sensor ROI 模式输出 |
| 输出帧率 | 60fps @ 全分辨率 | MIPI 带宽够; 拼接侧瓶颈在 ISP 3A + 解码, 评估 30fps 还是 60fps 进工程 |
| 像素尺寸 | 2.5μm × 2.5μm | 推算焦距时按此计算; 等效焦距 50mm 对应镜头焦距 ≈ 50mm × (2.5μm / 36mm) 不适用, 相机镜头标的是物理焦距 |
| DAG 电路 | 数字增益 | 低照度下用, 标定环境**避免** (DAG 增益不稳, 角点检测受影响) |
| DOL HDR | 长短帧合成 | RKISP 必须配 HDR 合成模式 (2-frame DOL); 否则 sensor 出双帧需要你自己合 |
| AOV / Fast AE / Quick start | sensor 自身低功耗特性 | 拼接无影响; 优势: sensor 冷启动 < 100ms, 适合热插拔与节能 |
| BSI | 背照式 | 黑光级 4M, 暗光性能好; 标定时**不要**在暗光下做 (角点检测需足够对比度) |
| 封装 | CSP | 板上集成, 不可换 sensor 模组 |

### 4.3 v2 工程改造时间线 (按依赖顺序)

#### 阶段 1: GC4683 + RKISP 通路验证 (1-2 周)

> **目标**: 在 RK3588 上让 4 路 GC4683 跑出 4 路 NV12 DMA-BUF 帧, 同步进你的工程 `get_image_vector`。

1. **硬件层确认**
   - 确认 RK3588 BSP 内核已带 GC4683 driver (`find / -name "gc4653*" 2>/dev/null` / `dmesg | grep -i gc46`); 若无, 找厂商提供 4-lane MIPI 驱动 patch。
   - 4 路同时接 MIPI CSI-2 4-lane × 4 路, RK3588 ISP 总带宽够; 验证方法: `media-ctl -p` 看 4 个 `rkcif` / `rkisp` 节点都枚举出来。
   - Sensor 工作模式: 在 DTS / driver 里配 2688×1520@60fps + DOL HDR; 或 ROI 到 2560×1440@60fps 单帧 (工程输入要明确)。
2. **ISP 3A 配置**
   - RKISP 3A 配置文件 `/etc/iqfiles/`, 需要 GC4683 的 calibration xml (lens shading, AEC, AWB, CCM, gamma); 找 sensor 模组厂要 IQ 文件。
   - 3A 收敛时间: 冷启动 1-2 秒, 标定采集必须**先空跑 3 秒再录**。
   - DOL HDR 模式下, RKISP 出 1 帧 HDR 合成 NV12, **不要**配成 2 帧输出 (会增加拼接负担)。
3. **通路打通信心检查**
   - 用 `v4l2-ctl -d /dev/video0 --get-fmt-video` 看到 NV12 像素格式 + 期望宽高 = 通路通。
   - 用 `gst-launch-1.0 v4l2src device=/dev/video0 ! ... ! fakesink` 看实时画面, 4 路分别能出图。
   - 你的工程 boot 时 `[decoder N]` 日志必须出现 `drm_prime_frames > 0` (用 `SAVE_STITCH_FRAMES=0` 跑 5 秒看日志); 否则回到 `Phase 0` 之前的调查。

#### 阶段 2: Phase 0 落地 — 输入源描述抽象 (2-3 天)

> **目标**: 把 `src/sensor_data_interface.cc:357-358` 的 `t50..t53.mp4` 替换成可配置 4 路 MIPI 节点, 为后续 6 路扩展铺路。

1. 新增 `struct CameraSource` (已在 AGENTS.md 给过规格), 支持 `mipi` / `file` 两种 type; 删掉 `usb` 分支。
2. 新增 `params/camera_sources.yaml` 4 个 cam 块, 每个写 `type: mipi, uri: /dev/videoN, width, height, fps, pixel_format: NV12`。
3. `SensorDataInterface::InitVideoCapture` 按 type 路由: `mipi` → 走 V4L2 采集线程 (阶段 3), `file` → 走现有 FFmpeg 线程。
4. 兼容: 没有 YAML 时 fallback 到 `t50..t53.mp4`, **数据集调试不被破坏**。
5. `g_is_using_camera` 死代码删掉。

#### 阶段 3: V4L2 采集线程 (MIPI 专用) (1 周)

> **目标**: 4 路 GC4683 → V4L2 → 推入 `image_queue_vector_`, 输出与现有 `QueuedFrame + NV12Frame` 完全一致。

1. 新建 `src/v4l2_capture_thread.cc`, 接口与现有 `decode_threads_` 对齐。
2. 用 `v4l2_open + VIDIOC_DQBUF` 直接抓 NV12 DMA-BUF 帧; **不要**走 FFmpeg (FFmpeg 也能读 `/dev/videoN`, 但会做一次 mmap 复制, 损失零拷贝)。
3. **关键约束**: GC4683 → rkisp 输出已经是 NV12, 直接 push 到 `kDrmPrime` 路径, **不**经 mjpeg 软解, **不**经 RGA YUYV→NV12 转换, **不**改 `dma_buf_cache_`。
4. 每帧加 `int64_t monotonic_ns` (为阶段 4 时间戳对齐准备)。
5. 与现 FFmpeg 线程的差异: V4L2 线程**没有** decoder_perf 日志, **没有** `hw_transfers` 统计, 改 `DecoderPerfStats` 为可选或拆出新 `V4L2CaptureStats`。
6. 验收: 4 路 `drm_prime_frames > 0`, FPS 接近 60 (sensor 端) / 30 (工程端, 取决于阶段 5 选 30 还是 60)。

#### 阶段 4: 软件时间戳同步 (1 周)

> **目标**: `get_image_vector` 拿到的是同一时间窗的 4 (后续 6) 路帧。

1. 复用 v1 阶段的"软件时间戳对齐"方案, 落到 4 路 V4L2 线程。
2. 同步窗口: `params/camera_sources.yaml::sync.window_ms = 16` (60fps 一帧 16.7ms) 起步, 后续按需调到 8 或 33。
3. `monotonic_ns` 来自 V4L2 线程取帧的 `std::chrono::steady_clock::now()`; **不要**用 sensor 寄存器时间戳 (需要驱动支持, GC4683 一般不暴露)。
4. 当下 4 路是同一颗 sensor, 漂移极小; 远期 6 路 (换板后) 才需要严格同步, 但代码现在就要写对。

#### 阶段 5: 帧率决策 (半天)

> **目标**: 决定工程跑 30fps 还是 60fps, 影响后续 4×4 拼接/解码压力。

| 选项 | 优势 | 代价 |
|---|---|---|
| **30fps** | 与数据集阶段一致; 解码/拼接压力减半; 满足人眼观察 | sensor 物理能力未用满, 高速场景丢细节 |
| **60fps** | 高速运动场景清晰; 用满 sensor 能力 | 4 路 2K@60 总带宽 1.32 GB/s, RK3588 ISP + rkmpp 能否扛住要实测; ROI bootstrap 时间翻倍 |

**建议**: **30fps 起步**, 在 `camera_sources.yaml` 配 `fps: 30`; sensor 物理 60fps 不丢, 后续按实测切 60fps, 仅改配置不动代码。

#### 阶段 6: 相机标定 (1 周, 与阶段 2-4 并行)

> **目标**: 4 路 GC4683 的 K, D, R, T 写入 `params/camchain_0..3.yaml`, ROI bootstrap 收敛更快, 拼接精度更高。

1. **打印**: 按上一轮建议的 A3 棋盘格 (11 行 14 列, 22mm, 哑光激光打印) 印 1 张。
2. **采集**: 4 路同时录 15-20 个位姿, 距离 20-40cm, 占画面 35-60%, 涵盖 5 类位姿 (中心 / 4 角 / 倾斜 / 近 / 远)。
3. **运行**: `tools/calibrate_intrinsics.py` (内参) + `tools/calibrate_extrinsics.py` (外参), Step A + Step B (跳过 Step C 全 BA, 按你之前选择)。
4. **写文件**: 替换 `params/camchain_0..3.yaml`。
5. **不达标处理**: RMS > 0.5px 或 fx 极差 > 5% → 加位姿 / 换打印; 4 路 K 矩阵应高度一致 (同型号 sensor)。
6. **GC4683 特殊注意**:
   - 采集环境光**不要**触发 DAG 增益 (室内漫射光, 避免低照度);
   - **关闭** DOL HDR 跑标定 (HDR 模式会改变有效焦距与畸变模型, 标定不准确), 标定完再开;
   - 3A 收敛**前 3 秒**的录视频丢弃, 不参与标定。

#### 阶段 7: 2×2 拼接跑通 (1 周)

> **目标**: 在 RK3588 + 4 路 GC4683 上跑通当前 2×2 拼接主路径, 不改拼接代码。

1. 用阶段 1-5 准备好的输入 (4 路 NV12 DMA-BUF), 跑工程 boot。
2. 调 ROI bootstrap: `NUM_BOOTSTRAP_FRAMES` 保持 3, 4 路置信度阈值 0.25, 早退 0.7。
3. 调可视化: `ENABLE_VISUAL_TUNING=1` + `SHOW_ROI_MARKERS=1` 跑, 检查 4 路画面是否对齐。
4. 调光度一致: 利用 GC4683 同型号 + RKISP 共享 3A 优势, 4 路亮度应该一致; 若不一致, 用 `STITCH_K_FOCAL_SCALE_CAM_N` (或新增的曝光系数) 微调。
5. FPS 验收: 30fps 输入下, 拼接端能否稳定 30fps? 用 `SAVE_STITCH_FRAMES=0` 跑 10 秒看日志。
6. 写 README + HISTORY 一段: v2 阶段 4 路 2×2 在 RK3588 + GC4683 上的实测数据。

#### 阶段 8: 2×3 扩展 (远期, 换 6 路板后, 1-2 周)

> **目标**: 当前开发板换为支持 6 路 MIPI 的 RK3588 (或 RK3588S 等), 跑 2×3 (6-cam) 拼接。

1. **硬件**: 6 路 MIPI CSI-2 通道, 总带宽 = 6 × 2K@30 ≈ 2 GB/s; 选板时要确认 ISP 通道数。
2. **软件前置**: 阶段 2-7 的所有代码已经按 6 路设计 (`CameraSource` 列表驱动), 改 6 路只需:
   - `params/camera_sources.yaml` 加 cam4 / cam5 块
   - `params/camchain_4.yaml`, `camchain_5.yaml` 标定文件
   - `params/roi_tuning.yaml` 加 cam4 / cam5 块
   - `include/roi_config.h::roi_offsets[4]` → `[6]`
   - `src/app.cc::BuildDefaultTuning` 的 `i < 4` 限制解除
   - 新增 `EstimateOverlaps2x3`, `BuildCameraRois2x3`, `BuildStitchLayout2x3` (AGENTS.md 已有清单)
3. **标定重做**: 6 路一次标定 (Step A + B, 不做 Step C 全 BA), 用同一个 A3 板。
4. **时戳同步**: 阶段 4 的代码要确保 6 路对齐, 同步窗口可能要从 16ms 调小到 8ms。
5. **光度**: 6 路 (假设 6 路同型号 GC4683) 一致性应该仍好; 但 ISP 3A 在 6 路并发下收敛时间可能翻倍, 标定采集多等 2 秒。

#### 阶段 9: 收尾 (1 周)

1. 把 4 路与 6 路的工程参数、实测 FPS、拼接质量、标定数据归档到 `docs/CALIBRATION_REPORT.md` (新建)。
2. 更新 `AGENTS.md` 的 "Current scope" 章节: 从 "2×2 → 2×3" 改为 "2×2 已跑通 + 2×3 远期", 把 2×3 章节移到 "Future work"。
3. 更新 `README.md` 的 "Current status" 段, 标注当前在 v2 阶段几。

### 4.4 v2 阶段甘特图 (依赖关系)

```
阶段1 (ISP 通路) ══════════════════► [1-2 周]
                       │
阶段2 (CameraSource 抽象) ═══► [2-3 天, 依赖阶段1确认能出 NV12]
                       │
阶段3 (V4L2 线程) ═══════► [1 周, 依赖阶段2]
                       │
阶段4 (时戳同步) ════════► [1 周, 依赖阶段3]
                       │
阶段5 (帧率决策) ═► [半天, 依赖阶段3 验收 FPS]
                       │
阶段6 (标定) ════════════► [1 周, 依赖阶段1, 与阶段2-5 并行]
                       │
阶段7 (2×2 跑通) ════════► [1 周, 依赖阶段1-6 全部]
                       │
阶段8 (2×3 扩展) ════════► [1-2 周, 依赖换 6 路板]
                       │
阶段9 (收尾归档) ═► [1 周, 依赖阶段7]
```

**关键并行**: 阶段 6 (标定) 与阶段 2-5 (代码) 完全独立, 可同时进行; 阶段 6 不需要等代码就绪, 只需 4 路物理能出图。

### 4.5 v2 阶段已删除 / 简化的项 (相对 v1 假设)

| v1 假设 | v2 状态 | 原因 |
|---|---|---|
| USB 2 路 + 软解 + YUYV→NV12 RGA 转换 | **删除** | 改用全 6 路 MIPI 板 |
| `kSoftwareNV12` 软帧 fallback 路径 | **可不做** | 全 MIPI, 不会出软解帧 |
| `sw_buf_cache_` 软帧缓存 | **删除** | 不需要 |
| MJPEG / YUYV 像素格式支持 | **删除** | GC4683 出 RAW → ISP 出 NV12, 中间格式都在 ISP 内核内, 用户态看不见 |
| USB 摄像头曝光差异问题 (4.3) | **删除** | 同型号 GC4683 + RKISP 共享 3A |
| USB 摄像头帧率时钟漂移问题 (4.2) | **简化** | 4 路同 sensor 软同步, 远期 6 路也同 sensor, 问题在阶段 8 再说 |
| USB 设备节点竞争问题 (4.4) | **删除** | 全 MIPI, 没有 USB 节点 |

### 4.6 v2 阶段新增的关注点 (相对 v1)

1. **GC4683 RAW → ISP NV12 整条管道**: 阶段 1 是 v2 最大的工作量, v1 假设 sensor 直接出 NV12 是错的。
2. **DOL HDR 与 RKISP 3A 协作**: sensor 出双帧 (长短曝光), RKISP 合成单帧 NV12; 任何一边配置错都会黑屏或撕裂。
3. **60fps 帧率**: GC4683 物理 60fps, 工程是否要跑满, 是新决策点 (阶段 5)。
4. **GC4683 DAG 增益影响**: 标定 / 拼接 都要避开 DAG 工作区间 (低照度), 否则色彩/增益不稳。
5. **远期 6 路板的 ISP 通道数**: 阶段 8 的硬件前提, 选板时要确认 6 路 MIPI 都能被 RK3588 同时处理 (新板可能是 RK3588S, 通道数与原 RK3588 核心板可能不同)。
6. **BSI 黑光级特性**: 暗光下表现好, 反而要**避免**室内太暗, 否则 3A 不收敛。
7. **CSP 封装不可换模组**: sensor 模组一旦选型固定, 光学中心 / 焦距等不可调, 标定是唯一出路。

### 4.7 GC4683 与 4M 像素 / 2K 的关系 (回答你的疑问)

你的描述里 "2K" 与 sensor 标称 "4M 像素" 的关系:

- **sensor 物理像素**: 2688 × 1520 = 4,085,760 ≈ **4M 像素** (sensor datasheet 标称)
- **你工程要用的 "2K"**: 一般指 2560 × 1440 (16:9, 2K 标准)
- **两者关系**: sensor 全像素 4:3 模式, 工程若用 16:9 是 sensor 内部做了 ROI 裁剪 (从 2688×1520 裁到 2560×1440, 丢 128 列 80 行像素, 但 2.5μm 像素下视角变化 < 1°)
- **建议**:
  - **工程用 2560 × 1440 (16:9)**: 标定 + 拼接 + 显示都按 16:9, 与行业 "2K" 命名一致; 让 sensor 在 ROI 模式跑 2560 × 1440 @ 60fps (MIPI 带宽更低, ISP 压力更小)
  - **如果 sensor 模组固定出 2688 × 1520**: 工程也用 2688 × 1520, 这是 sensor 全分辨率, 拼接质量最好; 只是命名上不叫 "2K" 而叫 "4MP 16:9" 或 "4MP 4:3"
- **决策时机**: 阶段 1 通路验证时确认 sensor 出图尺寸, 据此确定工程 `image_width / image_height` 与 `camera_sources.yaml` 的 width / height; 这个数字写错, 标定与拼接全部错。

### 4.8 v2 阶段需要你立即确认的事

1. **sensor 输出模式**: GC4683 是 4:3 全像素 (2688×1520) 还是 16:9 ROI (2560×1440) ? 直接问模组厂或 `media-ctl -p` 看 `fmt: YUV_4_2_0 ... WxH`。
2. **DOL HDR 开 / 关**: 拼接对 HDR 输入需求? (4M 暗光下 DOL HDR 增益明显, 但标定难度大) — 建议先**关闭** HDR 跑通, 再加。
3. **ISP 3A IQ 文件**: 模组厂是否提供? 没有的话, RK3588 BSP 默认 IQ 也能跑, 但颜色与白平衡精度差。
4. **换板时间表**: 6 路板什么时候到? 这决定阶段 8 的优先级; 现在重点是阶段 1-7。
5. **MIPI 线序**: 4 路 MIPI 的物理走线长度差异 (信号完整性, 间接影响同步精度) — 一般 < 5cm 差异不影响, 但要确认没有超过 15cm 的差异。
6. **AOV 模式**: 4 路是否长期在线? 如果是 24/7 拼接, AOV 模式没必要, sensor 全速跑; AOV 适合 1Hz 待机 + 触发唤醒。

### 4.9 与 AGENTS.md 的同步关系

完成本节后, 建议在 `AGENTS.md` 的 "Current scope" 章节把:
- "Current scope: 2×2 → 2×3 (6 cameras)" 改为
- "Current scope: v2 阶段 — 4 路 GC4683 MIPI 2K@30fps 跑通 2×2 拼接; 2×3 推到 6 路板换板后"

并把 "AGENTS.md 的 13 行 4-cam 硬编码位置" 表移到 "Future work (阶段 8)" 章节, 加上 "远期 (换板后) 处理" 标记, 避免当前任务被干扰。

---

## 0x05 阶段实操进展 (2026-06-25 更新)

### 5.1 当前阶段状态

| 阶段 | 状态 | 说明 |
|---|---|---|
| 1. ISP 通路 + GC4683 sensor driver | **🔴 阻塞中** | 详见 §5.2 |
| 2. CameraSource 抽象 (YAML) | ✅ **已完成** | 详见 §5.3 |
| 3. V4L2 采集线程 | ⏳ 等阶段 1 | 驱动到位后做 |
| 4. 软件时间戳同步 | ⏳ 等阶段 3 | yaml 已预留 `sync_window_ms` |
| 5. 帧率配置 | ✅ **完成** (30fps) | yaml 写死 fps: 30 |
| 6. 相机标定 (工具) | ✅ **完成** | 详见 §5.4 |
| 7. 2×2 真机跑通 | ⏳ 等阶段 1 | 不依赖标定 |
| 8. 2×3 扩展 (6 路板) | ⏳ 远期 | 换 6 路板后 |
| 9. 收尾 | ⏳ 等阶段 7 | |

### 5.2 阶段 1 阻塞点详查 (2026-06-25)

#### 板子现状

- 板子: RK3588 核心板 + Debian 11 (bullseye) + kernel 5.10.198
- 4 颗 GC4683 sensor **物理接好** (CSI + DPHY 链路 OK)
- 板子 vendor / BSP 厂商**未知** (linaro-alip 主机名, 厂商丝印未查)

#### 实测证据

| 检查项 | 结果 | 含义 |
|---|---|---|
| `ls /dev/video*` | 0..19 (含 rkisp0 + rkcif 4 路) | 内核枚举 OK |
| `media-ctl -p` | 1 个 mipi-csi2 + 4 个 stream_cif_mipi_id0..3 | MIPI 物理 OK |
| `dmesg \| grep gc46` | **空** | **GC4683 驱动未加载** |
| `find /lib/modules/... -name "*gc46*"` | **空** | driver 没有 .ko |
| `find /proc/device-tree -name "*gc46*"` | **空** | DTS 无 sensor 节点 |
| `ls /dev/v4l-subdev*` | 3 个 (csi2-dphy, mipi-csi2, rkisp-isp-subdev) | **无 sensor subdev** |
| `cat /sys/class/video4linux/v4l-subdev2/name` | `rkisp-isp-subdev` | 之前的"subdev2 是 GC4683"猜测**错误** |
| `v4l2-ctl -d /dev/video11 --get-fmt-video` | 800/600 NV12 | rkisp 跑 fallback (无 sensor 数据) |
| `v4l2-ctl --stream-mmap` | `Operation not permitted` | 用户态权限不足 (linaro 不在 video 组) |
| `/etc/iqfiles/` | 11 个 JSON (imx327/415/464, ov02b10/13855/50c40, gc8034, os04a10, s5kjn1) | **无 GC4683 专用 IQ** |
| `which rkaiq_*` | rkaiq_3A_server / rkaiq_tool_server 都在 | 3A 工具可用 |
| 启动 rkaiq_3A_server | `Bad media topology for: /dev/media0..15`, 段错误退出 | **因为无 sensor**, 拓扑空, 3A 起不来 |
| `sudo usermod -aG video linaro` + `chmod 666 /dev/video*` | (未做) | 修权限后 stream 应可, 但仍无图 (无 sensor) |

#### 根因总结

**当前 BSP 不带 GC4683 sensor driver, 4 颗 sensor 物理接好但内核完全看不见**:

1. 没有 `gc4683.c` 内核模块 (`.ko` 也不存在, 推测也没编进内核)
2. DTS 里没有 `gc4683` 节点 (I2C 总线 + reset/pwdn GPIO + mipi-csi2 端点)
3. 4 路 MIPI 物理信号进 RK3588 OK (`media-ctl` 拓扑可看), 但 DPHY 收完没人解码
4. rkisp0 跑 fallback 800x600 (sensor 没数据时, ISP 默认输出)
5. 没有 GC4683 专用 IQ JSON, 即便驱动装上, 3A 颜色也会偏

#### 解决路径 (按可行性重排)

| 路径 | 难度 | 时间 | 备注 |
|---|---|---|---|
| A. 找模组厂 / 板子供应商要 patch | 等待 | 1-2 周 | **首选**, 但当前没有供应商联系方式 |
| B. 从 GC4653 移植 (同系列, 改 register table) | 中 | 2-3 天 | init table 是关键, 模组厂 NDA 后才能拿全 |
| C. GitHub 找社区 patch (Toybrick / 友善 / 迅为 / 野火 等 BSP) | 中 | 1-2 天 | 不一定找得到, 但**最可能快速解决** |
| D. 换用 BSP 已支持的 sensor (如 imx415, imx464) | 高 (换模组) | 1-2 周 (重采购) | 不推荐, 硬件已买 |

**当前选择**: 你**自己**联系板子供应商/模组厂, 期间**同步**做 C (GitHub 搜) + B (从我提供 GC4653 移植指南起步).

### 5.3 阶段 2 已完成 (2026-06-25)

#### 改动清单

- `include/sensor_data_interface.h`: 新增 `CameraSource` (type/uri/width/height/fps/pixel_format) + `CameraSourceList` (sync_window_ms/auto_calibrate) + `GetCameraSourceList()` accessor
- `src/sensor_data_interface.cc`:
  - 加 `LoadCameraSourceList()` 用 OpenCV `FileStorage` 读 `params/camera_sources.yaml`
  - 加 `LoadDefaultDatasetSources()` 抽 4 路 t50..t53.mp4 路径 (一处写, 多次调)
  - 重写 `InitVideoCapture()`: 按 `INPUT_SOURCE_MODE` 二选一 (`dataset` 默认, `camera` 走 yaml, 无 `auto` 中间态)
  - `type: mipi` 暂时**跳过** (阶段 3 实现 V4L2 线程), 仅日志告警
  - 删 `extern bool g_is_using_camera;` 与 `if (g_is_using_camera) {...}` 死代码
- `src/app.cc`: 删 `bool g_is_using_camera = false;` 定义
- `params/camera_sources.yaml`: **新建**, 4 个 cam 块 (`type: file` 默认, 注释说明改 `type: mipi` 即切摄像头) + `sync_window_ms: 16` + `auto_calibrate: 0`

#### 行为 (按 `INPUT_SOURCE_MODE` 二选一)

- `INPUT_SOURCE_MODE` 不设 / `dataset` → 走 t50..t53.mp4 数据集, 忽略 yaml, 行为与 v1 完全一致
- `INPUT_SOURCE_MODE=camera` → 走 `params/camera_sources.yaml`, 加载失败报错退出; 全 mipi 且阶段 3 未完成报错退出
- **无** `auto` 中间态, 避免 yaml/数据集地址在 3 个 if 分支重复写

#### 临时屏蔽 mipi 的原因

- 阶段 1 阻塞, V4L2 采集线程没写, 即使读 `type: mipi` 也跑不通
- 让工程**仍能编译跑数据集**, 不破坏调试

#### 阶段 3 解锁后

`InitVideoCapture` 只需加 `type: mipi` 路由分支 (走新写的 `V4L2CaptureThread`), 不动 yaml 与其他模块.

### 5.4 阶段 6 已完成 (2026-06-25)

#### 新增工具 (不入 CMake, 仅做离线标定)

- `tools/calibrate_intrinsics.py` — **Step A 单目内参**: 11×14 棋盘 22mm, 4 路独立标定, 输出 `params/intrinsics/cam{N}.npz` + `params/camchain_{N}.yaml` + `params/intrinsics/summary.txt` (4 路一致性报告)
- `tools/calibrate_extrinsics.py` — **Step B 立体外参**: 4 路同步视频 (cam 0 为参考) → 写回 `camchain_{1,2,3}.yaml` 的 R, T 字段 + `params/extrinsics/summary.txt` (T 模长 sanity)
- `tools/undistort_preview.py` — **验证**: 单图加载 `camchain_N.yaml` 的 K, D → `cv2.undistort()` → 左右对比存 `params/intrinsics/preview_cam{N}.png`

#### 标定板规格 (已确认)

- A3 横向 (420×297 mm), 哑光激光打印, 标定区 308×242 mm, 边距各 25mm
- 11 行 × 14 列黑白方格, 单格 22 mm, 左上角**黑色**
- 距摄像头 20-40 cm, 板占画面 35-60%
- 室内漫射光, 关闭 DOL HDR, 等 ISP 3A 收敛 3 秒再录

#### 验收标准 (硬指标)

- 单路 RMS < 0.5 px
- 4 路 fx 极差 < 5%
- 4 路 fx 接近理论值 1055 px (从 101° FOV 推算)

### 5.5 6 路扩展的代码增量 (阶段 8 待做)

阶段 2 已把输入源描述数据化, 6 路扩展**C++ 代码几乎零改动**:

- `params/camera_sources.yaml` 加 cam4, cam5 块即可 (待 6 路板到位后)
- `include/roi_config.h` `RoiOffset roi_offsets[4]` → `[6]`
- `src/app.cc::BuildDefaultTuning` `i < 4` cap 解除
- 新增 `EstimateOverlaps2x3`, `BuildCameraRois2x3`, `BuildStitchLayout2x3` (替换 2×2 版)
- `src/image_stitcher.cc` 循环已按 `num_img_` 走, 6 路自动 work
- 标定跑 `calibrate_extrinsics.py --videos cam0..5.mp4`, 物理排布从 2×2 改 2×3 即可

### 5.6 接下来需要你做的 (按优先级)

1. **找板子供应商/原厂** (阶段 1 解锁的**唯一**路径): 问 GC4683 driver patch + IQ JSON
   - 如果联系不到, 走 GitHub 搜 (`gc4683 rk3588`) 或从 GC4653 移植 (1-2 天)
2. 拿到驱动后, 按 §2.1 跑 v4l2-ctl 链路验证 + 启动 rkaiq_3A_server, 让 4 路 2560x1440 NV12 抓帧成功
3. 把 `params/camera_sources.yaml` 的 4 路 mipi 真正跑通后, 阶段 7 启动 2×2 拼接
4. 标定流程**独立**于驱动, 可以并行做: 打印 A3 棋盘 → 录 4 路同步视频 → 跑 `calibrate_intrinsics.py` + `calibrate_extrinsics.py`

