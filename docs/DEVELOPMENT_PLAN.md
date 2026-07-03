# 开发方案 v2.3 (精简版, 2026-06-30 重构)

> **目标**: 6 路 GC4683 MIPI → 2×3 (6-cam, 2 列 × 3 行) 拼接, rocktech RK3576 主板 (Ubuntu 22.04, kernel 6.1.75)
>
> **精简原则**:
> - 内核驱动适配 / 厂商发包 → **rocktech 厂商负责** (kernel 6.1.75 已内置 GC4683 v00.01.01), 我方只做可用性测试
> - 应用编译 → **开发板自主** (板上 gcc/make/cmake), **PC 不交叉编译** image-stitching
> - 时间戳同步 → **删除** (6 路 MIPI 硬件同步, 同 ISP, 漂移可忽略)
> - **跳过 4 路过渡态**: 当前 4 路代码直接改造为 6 路 2×3, 不做 4 路单独验证
>
> **设计文档**: [README.md](../README.md) · **历史归档**: [HISTORY.md](HISTORY.md) · **Agent 入口**: [CLAUDE.md](../CLAUDE.md) · **板子对比**: [主板系统环境差异.docx](主板系统环境差异.docx)

---

## 目录

- [0. 整体概览](#0-整体概览)
- [1. 板子与驱动可用性测试](#1-板子与驱动可用性测试-1-2-天-) (★ 拿到开发板后立刻做)
  - [1.1 SSH 连接](#11-ssh-连接-开发板) ✅
  - [1.2 GC4683 驱动 probe 验证](#12-gc4683-驱动-probe-验证-核心) ✅
  - [1.3 6 路 MIPI 拓扑验证](#13-6-路-mipi-拓扑验证-核心) ⏳ (待厂商确认 DTS)
  - [1.4 RKISP 3A 启动](#14-rkisp-3a-启动) ⏳ (等镜头)
- [2. 6 路数据集输入适配](#2-6-路数据集输入适配-2-3-天-) ⏳ **★ 下一步**
- [3. 6 路 2×3 代码迁移](#3-6-路-23-代码迁移-1-2-周)
- [4. 6 路 V4L2 摄像头采集](#4-6-路-v4l2-摄像头采集-1-周)
- [5. 6 路相机标定](#5-6-路相机标定-1-周-与-3-4-并行)
- [6. 6 路 2×3 真机跑通](#6-6-路-23-真机跑通-1-2-周)
- [附录 A: 6 路 V4L2 验证命令合集](#附录a-6-路-v4l2-验证命令合集)
- [附录 B: GC4683 关键参数速查](#附录b-gc4683-关键参数速查)
- [附录 C: 故障排查命令合集](#附录c-故障排查命令合集)
- [附录 D: 文件改动总览](#附录d-文件改动总览)
- [附录 E: 关键阈值与硬指标速查](#附录e-关键阈值与硬指标速查)

---

## 0. 整体概览

### 0.1 项目目标

- **输入**: 6 路 GC4683 MIPI RAW → RKISP NV12 (2K 2560×1440 @ 30fps)
- **布局**: 2×3 (2 列 × 3 行 = 6 个 camera)
- **目标芯片**: RK3588 (代码路径相同, 未在硬件验证); **已验证**: RK3576 (rocktech 主板, kernel 6.1.75)
- **Pipeline**: V4L2 6 路采集 → DMA-BUF 零拷贝 → RGA 裁剪/旋转 → OpenCL 接缝羽化 → DRM 输出

### 0.2 当前进度 (2026-07-01)

| # | 阶段 | 内容 | 状态 | 备注 |
|---|---|---|---|---|
| 1 | 板子与驱动 | SSH / 驱动 probe / 6 路拓扑 / RKISP | ✅ 板端工具链 OK | SSH ✅, GC4683 驱动 v00.01.01 ✅, **ffmpeg-rockchip 6.1 (rkmpp) 多架构装好** ✅, OpenCL/Mali ICD 配齐 ✅; 6 路物理 MIPI 等厂商 DTS 补丁 |
| **2** | **6 路数据集适配** | **改 yaml + 默认文件 + roi_offsets** | **✅ 编译通过, ✅ 6 路 fallback 跑通** | yaml 缩进修对 (`- type:` 单空格), try/catch 包住解析, dataset fallback 6 文件 `t50-t53 + t40 + t41` 已工作; 待真机验证 rkmpp DRM_PRIME 路径 |
| 3 | 6 路 2×3 代码 | BuildCameraRois2x3 / BlendSeams 改 6 | ⏳ | 等阶段 2 验证完 |
| 4 | 6 路 V4L2 采集 | 写 v4l2_capture_thread.cc | ⏳ | 等阶段 1 镜头到位 |
| 5 | 6 路相机标定 | calibrate_intrinsics + extrinsics | ⏳ | 与 3-4 并行 |
| 6 | 6 路 2×3 真机跑通 | 实测 + 归档 | ⏳ | 等 3-5 |

#### 2026-07-01 阶段 1+2 关键节点 (本次会话)

| 节点 | 状态 | 备注 |
|---|---|---|
| 板端 OpenCL 链路 | ✅ | `libmali-valhall-g610-g13p0-x11-wayland-gbm 1.9-1` + `ocl-icd-opencl-dev` 装好 |
| ffmpeg-rockchip 6.1 rkmpp | ✅ | `--libdir=/usr/lib/aarch64-linux-gnu --enable-shared --disable-static` 多架构正确路径装到位; 教程的 `--prefix=/usr` 默认配置在多架构系统上会装错路径, 详 [HISTORY §0x03-2](HISTORY.md) |
| `CMakeLists.txt:10-12` 默认路径 | ✅ | `~/dev/ffmpeg60` → `/usr/lib/aarch64-linux-gnu`, 加 `CACHE PATH` 可 `-DFFMPEG_LIB_DIR=` 覆盖 |
| `src/sensor_data_interface.cc:347-392` | ✅ | `LoadCameraSourceList` 加 try/catch, yaml 解析报错不再 crash, 直接 fallback 到默认 dataset (修复 v2 设计/实现错位) |
| `src/roi_visualizer.cc:158` | ✅ | `SDL_SetHint(SDL_HINT_VIDEODRIVER, ...)` → `setenv("SDL_VIDEODRIVER", ...)` 兼容 SDL 2.0.22+ 改名 |
| `params/camera_sources.yaml` | ✅ | 6 路 `t50-t53 + t40 + t41`; 顶层 key 对齐列 0, list 用 `- type:` (单空格) 兼容 OpenCV 4.5.4 严格解析 |
| `image-stitching` 编译链接 | ✅ | 链 `libavformat.so.60` + `libmali.so.1` 全部到位 |
| 真机 rkmpp 解码 + DRM_PRIME 帧 | ⏳ | 启动日志验证中, 期望看到 `actual decoder=mpeg4_rkmpp` + `received DRM_PRIME frame` |

### 0.3 时间线 (5-8 周)

```
阶段1 (SSH + 驱动 + 6路拓扑) ═► [1-2 天, 已部分完成]
                          │
阶段2 (6路数据集适配) ════════► [2-3 天, ★ 下一步]
                          │
阶段3 (6路 2×3 代码迁移) ══════► [1-2 周]
                          │
阶段4 (6路 V4L2 摄像头采集) ══════► [1 周, 依赖镜头]
                          │
阶段5 (6路相机标定) ═══════════► [1 周, 依赖镜头, 与 3-4 并行]
                          │
阶段6 (6路 2×3 真机跑通) ═══════► [1-2 周, 依赖 3-5]
```

**关键路径**: 阶段 2 (数据集适配) 是当前卡点, 完成后立刻推进阶段 3 (2×3 代码迁移). 阶段 4-5 需等镜头到位才能跑.

---

## 1. 板子与驱动可用性测试 (1-2 天) ★

> **目标**: 拿到 rocktech 板 + 厂商镜像后, **立刻验证** 6 路 GC4683 通路是否可用, 决定是否需要走返修流程
>
> **当前状态 (2026-06-30)**:
> - ✅ SSH 通过 WiFi 移动热点连接 (192.168.137.200, 见 [HISTORY §6.2](HISTORY.md))
> - ✅ GC4683 驱动 built-in v00.01.01, 自动 probe 4 路 (i2c-2/3/4/6)
> - ⏳ **6 路 MIPI 拓扑**: DTS 显示 4 路 gc4683 + 4 个 dphy (0/2/4/5), 厂商规格说 6 路. **待用户跑 §0x06 验证清单确认** ([HISTORY §0x06](HISTORY.md))

### 1.1 SSH 连接开发板 ✅

> **目标**: 通过 SSH 连接开发板, 方便后续传输文件和工程调试
>
> **已用方案**: 板子 WiFi 移动热点 (PC 端 IP `192.168.137.1`, 板子端 `192.168.137.200`), rocktech 用户登录, 提权用 `sudo -i`. 详细命令 / 故障排查见 [HISTORY §6](HISTORY.md)

**验收标准** (已通过):
- [x] 板子端 `ping 192.168.137.1` 收到回复
- [x] PC 端 `ping 192.168.137.200` 收到回复
- [x] PC 端 `ssh rocktech@192.168.137.200` 能登入板子
- [x] `uname -a` 显示 `Linux rockemb 6.1.75`
- [x] `/etc/os-release` 显示 `Ubuntu 22.04.5 LTS`

### 1.2 GC4683 驱动 probe 验证 (核心) ✅

```bash
dmesg | grep -iE 'gc46|galaxycore'
# 期望 6 行 (实际 4 行, 等 §1.3 确认 DTS 是否需补齐)
```

**实际结果 (2026-06-30)**:
```
gc4683 2-0031: driver version: 00.01.01
gc4683 3-0031: driver version: 00.01.01
gc4683 4-0031: driver version: 00.01.01
gc4683 6-0031: driver version: 00.01.01
```
- ✅ 驱动 built-in (`lsmod | grep gc46` 空)
- ✅ 驱动版本 v00.01.01
- ⚠️ Sensor ID 全 0x000000 (预期: 镜头物理未接)
- ⚠️ DTS 警告 (4 路都有): pwren-gpios 缺失, pinctrl 缺失, dovdd/dvdd/avdd 用 dummy regulator (**厂商 2026-07-02 确认: sensor 不用 supply 节点控制电压, 警告是预期, 不需修**)

### 1.3 6 路 MIPI 拓扑验证 (核心) ✅ 已完成 (DTS 层, 厂商 2026-07-02 确认)

> **目的**: 厂商规格 6 路, sysfs 实测 6 路 gc4683 sensor 节点, 分布在 6 个 i2c 控制器下, 链路基本完整.
>
> **更新日期**: 2026-07-01 实测

**6 路 sensor 节点实测分布**:

| Cam | 节点名 | i2c 控制器 | mipi-csi2 | csi2-dphy | xvclk |
|---|---|---|---|---|---|
| cam1 | gc4683@31 | i2c@feaa0000 | mipi0-csi2 | csi2-dcphy0 | external-camera-clock (0x176) |
| cam2 | gc4683_1@31 | i2c@feab0000 | mipi1-csi2 | csi2-dcphy1 | external-camera2-clock (0x17a) |
| **cam3** | **gc4683_2@31** | **i2c@fead0000** (新) | mipi5-csi2 | **dphy0/3 (厂商确认不用)** | external-camera3-clock (0x183) |
| cam4 | gc4683_3@31 | i2c@fec80000 | mipi2-csi2 | csi2-dphy2 | external-camera4-clock (0x1b0) |
| cam5 | gc4683_4@31 | i2c@feac0000 | mipi3-csi2 | csi2-dphy4 | external-camera5-clock (0x17f) |
| cam6 | gc4683_5@31 | i2c@fec90000 | mipi4-csi2 | csi2-dphy5 | external-camera6-clock (0x1b3) |

**关键 sysfs 命令**:

```bash
# 看 6 路 sensor 节点
find /sys/firmware/devicetree -name "gc4683*@31" -type d | sort
# 期望 6 个, 分布在 6 个不同 i2c 控制器

# 看 i2c 控制器状态
for addr in feaa0000 feab0000 feac0000 fead0000 fec80000 fec90000; do
    echo -n "i2c@$addr: "
    cat /sys/firmware/devicetree/base/i2c@$addr/status 2>/dev/null || echo "MISSING"
done

# 看 csi2-dphy 状态
for d in dcphy0 dcphy1 dphy0 dphy1 dphy2 dphy3 dphy4 dphy5; do
    echo -n "csi2-$d: "
    cat /sys/firmware/devicetree/base/csi2-$d/status 2>/dev/null || echo "MISSING"
done

# 看 mipi-csi2 状态
for m in 0 1 2 3 4 5; do
    echo -n "mipi$m-csi2: "
    cat /sys/firmware/devicetree/base/mipi$m-csi2/status 2>/dev/null || echo "MISSING"
done
```

**当前已知缺口 (vendor 待修)**:

| 缺口 | 状态 | 何时修 |
|---|---|---|
| csi2-dphy0 / csi2-dphy3 status | **disabled** ❌ | **vendor 改 dts, 必须** |
| 6 路 sensor 节点 dovdd/dvdd/avdd-supply | 部分缺失 ⚠️ | **vendor 改 dts, 应该** |
| 6 路 sensor 节点 reset-gpios / pwren-gpios 实际引脚 | 空 (故意) ✅ | **等物理 sensor 接入后, 按 FPC 原理图填** |
| 6 路 sensor 节点 pinctrl-0 | 1/6 已加 (gc4683_4) | 其他 5 路可等物理 sensor 接入后补 |
| i2c@feca0000 | disabled (但 6 路已用 fec90000, 实际不需要) | 不用管 |

**验收标准** (新镜像烧录后跑 [post_flash_test.sh](tools/post_flash_test.sh) 自动测):

- [x] 6 个 gc4683 sensor 节点, compatible=galaxycore,gc4683, status=okay
- [x] 6 路 sensor 分布在 6 个不同 i2c 控制器, 无地址冲突
- [x] 6 个 i2c 控制器 status=okay
- [x] 4 个 csi2-dphy (dcphy0/1, dphy1/2/4/5) status=okay
- [ ] 2 个 csi2-dphy (dphy0/3) status=okay — **vendor 待修**
- [x] 6 个 mipi-csi2 status=okay
- [x] 6 个 external-camera-clock 已定义
- [ ] 6 路 sensor 都配了 dovdd/dvdd/avdd-supply — **vendor 待修**
- [x] 5 个 i2c 控制器 pinctrl-0 已配

**自动测试**: `sudo bash tools/post_flash_test.sh` 一键跑完上面所有项, 输出 PASS/WARN/FAIL 报告.

### 1.4 RKISP 3A 启动 ⏳

> **依赖**: 镜头物理到位 + DTS 6 路验证通过

```bash
# 看 IQ 文件
ls /etc/iqfiles/

# 启动 rkaiq_3A_server
sudo systemctl restart rkaiq_3A_server
sudo /usr/bin/rkaiq_3A_server --media=/dev/media0 --iq=/etc/iqfiles/gc4683.xml &

# 看 3A 状态
sudo rkaiq_tool --get-ae-state
sudo rkaiq_tool --get-awb-state
```

**验收**: 3A 收敛 (≥5 秒), 抓帧文件 5,529,600 bytes (2560×1440 NV12)

---

## 2. 6 路数据集输入适配 (2-3 天) ★ 下一步

> **目标**: 当前 4 路代码直接改成 6 路配置, 不做 4 路中间态验证. 跑 dataset 模式 (6 个 mp4) 端到端通.
>
> **本阶段完成后**: `INPUT_SOURCE_MODE=dataset` 跑 6 路 t 文件; yaml mipi 模式先空着 (阶段 4 写)

### 2.1 前置条件

- 阶段 1.1 (SSH) ✅
- 阶段 1.2 (驱动) ✅
- `datasets/2k-test/` 至少有 6 个 .mp4 (现有 29 个, 选 6 个即可, 见 §2.2)

### 2.2 准备 6 路数据集

> **现状**: `datasets/2k-test/` 现有 29 个 2K mp4 (h40-h63, t00-t53). 选 6 个作为默认 6 路数据集, 不再额外生成.

**推荐选择** (同一组, 物理位置接近, 方便后续对比):
- cam0-cam3: `t50.mp4, t51.mp4, t52.mp4, t53.mp4` (现有默认)
- cam4-cam5: **`t40.mp4, t41.mp4`** (同一类型 t, 不同组, 临时占位)
- 或更优: **同组 6 路**: 把 `t50-t53.mp4` cp 成 `t54.mp4, t55.mp4` 不行 (内容相同); 真实 6 路数据集需等镜头到位后录

**最简单做法**: `t50.mp4..t53.mp4 + t40.mp4 + t41.mp4` 临时占位, 阶段 6 替换.

### 2.3 改动清单

| 文件 | 改动 | 验证 |
|---|---|---|
| [params/camera_sources.yaml](../params/camera_sources.yaml) | 加 `cam4`, `cam5` 两个 block (uri 指 t40, t41) | `cat` 看 6 个 cam |
| [src/sensor_data_interface.cc:399](../src/sensor_data_interface.cc#L399) | `default_files` 加 `t40.mp4, t41.mp4` (fallback 时用 6 个) | 删 yaml, 重启, 看日志 "use 6 file sources" |
| [params/roi_tuning.yaml](../params/roi_tuning.yaml) | 加 `cam4:`, `cam5:` block | `cat` 看 6 个 cam |
| [include/roi_config.h:14](../include/roi_config.h#L14) | `RoiOffset roi_offsets[4]` → `[6]` | 编译不报错 |
| [src/app.cc:178](../src/app.cc#L178) | `for (... i < 4; ++i)` → `i < 6` | 编译不报错 |
| [src/image_stitcher.cc:301](../src/image_stitcher.cc#L301) | `cl_in(4, nullptr)` → `(6, nullptr)`, loop 改 6 | 编译不报错 |

> **⚠️ 阶段 2 完成后, 2×2 函数 (`BuildCameraRois2x2`, `BlendSeams` 4 个 dispatch) 仍是 4 路实现**. 跑 dataset 6 路时, cam0-cam3 正常拼, cam4-cam5 被静默忽略. 这是**预期内**的中间态, 阶段 3 改 2×3 函数才能完整处理 6 路.

### 2.4 验收标准

```bash
cd ~/image-stitching/build
cmake .. && make -j$(nproc)
./image-stitching 2>&1 | tee /tmp/stitch_6file.log
```

- [ ] 启动日志显示 `INPUT_SOURCE_MODE=dataset, use 6 file sources`
- [ ] 无 segfault, 无 runtime error
- [ ] `/tmp/stitched_frames/` 有连续帧 (说明 main loop 在跑)
- [ ] **暂时**输出只反映 cam0-cam3 (cam4-cam5 被静默 drop, 阶段 3 解决)
- [ ] `num_img_=6` 在日志里能看到 (通过改 debug log 验证)

### 2.5 故障排查

| 现象 | 可能原因 | 处理 |
|---|---|---|
| `roi_offsets[4]` 数组越界 (i=4,5 写越界) | 没改 [roi_config.h:14](../include/roi_config.h#L14) | 改成 `[6]` |
| `BuildDefaultTuning` 只读到 cam0-cam3 的 offset | 没改 [src/app.cc:178](../src/app.cc#L178) 的 `i < 4` | 改成 `i < 6` |
| 启动报 `no usable file sources` | 默认文件 yaml 都失效 | 检查 `t40.mp4, t41.mp4` 路径是否在 `datasets/2k-test/` |

---

## 3. 6 路 2×3 代码迁移 (1-2 周)

> **目标**: 把 2×2 函数全部改成 2×3, 让 6 路输入真正被全部处理. 阶段 2 的"cam4-cam5 被静默 drop"问题在本阶段彻底解决.

### 3.1 前置条件

- 阶段 2 ✅ (6 路 yaml + 默认文件 + roi_offsets 都已扩展)
- 阶段 4 (V4L2) 还在并行, 不依赖本阶段

### 3.2 硬编码点速查

| 位置 | 当前 (2×2) | 改为 (2×3) |
|---|---|---|
| [src/app.cc:434](../src/app.cc#L434) `EstimateOverlaps2x2` | 4 对 overlap (h01/h23/v02/v13) | 新增 `EstimateOverlaps2x3` (h01/h12/h34/h45 + v03/v14/v25, **7 对**) |
| [src/app.cc:449](../src/app.cc#L449) `BuildCameraRois2x2` | 2×2 布局 (4 个 quadrant) | 新增 `BuildCameraRois2x3` (2 列 × 3 行 = 6 个 cell) |
| [src/app.cc:543](../src/app.cc#L543) `BuildStitchLayout2x2` | 单象限 output, 4 个 task | 新增 `BuildStitchLayout2x3`, panorama height = 三行之和, 6 个 task |
| [src/app.cc:598](../src/app.cc#L598) `BootStrapOptimalLayout` | 打印 `h01/h23/v02/v13_score` | 改为 7 对 overlap key |
| [src/app.cc:178](../src/app.cc#L178) `BuildDefaultTuning` | `i < 4` | 已在阶段 2 改 `i < 6` |
| [include/roi_config.h:14](../include/roi_config.h#L14) `roi_offsets[4]` | `[4]` | 已在阶段 2 改 `[6]` |
| [src/image_stitcher.cc:301](../src/image_stitcher.cc#L301) `BlendSeams` | `cl_in(4)`, 4 个 `dispatch_seam` (0+1, 2+3, 0+2, 1+3) | `cl_in(6)`, **9 个 dispatch_seam** (4 水平邻接 + 5 垂直邻接? 见 §3.3) |
| [src/roi_visualizer.cc](../src/roi_visualizer.cc) Tab 键 | 4 个 cam ID | 扩到 6 个 |
| [params/camchain_0..3.yaml](../params/) | 4 份标定 | 扩到 `camchain_0..5.yaml` (6 份) |
| [params/roi_tuning.yaml](../params/roi_tuning.yaml) | `cam0..cam3` keys | 已在阶段 2 改 `cam0..cam5` |
| `RoiConfig::LoadFromFile` | 固定 4-cam 解析 | 改为动态解析 6 cam |

### 3.3 2×3 邻接对设计 (2 列 × 3 行)

```
cam0 cam1
cam2 cam3
cam4 cam5
```

**邻接关系**:
- **水平邻接** (4 对): h01, h12, h23, h34, h45 — 等等, 2 列的话只有 (cam0,cam1), (cam2,cam3), (cam4,cam5) 3 对
- **垂直邻接** (2 对): v02, v13, v24 — 等等, 3 行的话有 (cam0,cam2), (cam1,cam3), (cam2,cam4), (cam3,cam5) 4 对
- **共 7 对**: 3 水平 + 4 垂直 = 7 对 (与 CLAUDE.md 一致)

**BlendSeams 改 9 个 dispatch_seam** (实际是 7 + 2 cross-corner):
- 3 水平: (0,1), (2,3), (4,5)
- 4 垂直: (0,2), (1,3), (2,4), (3,5)

### 3.4 新增函数

```cpp
// src/app.cc 新增 (保留 2×2 用于 fallback)
MatrixOverlap EstimateOverlaps2x3(const std::vector<cv::Mat>& frames);
std::vector<CameraRoi> BuildCameraRois2x3(
    const std::vector<NV12Frame>& frames,
    const MatrixOverlap& overlaps,
    const std::vector<CameraTuning>& tuning);
std::vector<StitchTask> BuildStitchLayout2x3(
    const std::vector<CameraRoi>& rois,
    const std::vector<CameraTuning>& tuning,
    int* panorama_width, int* panorama_height);
```

`BootStrapOptimalLayout` 加分支: `num_img_ == 6` 走 2×3, `num_img_ == 4` 仍走 2×2 (向后兼容).

### 3.5 验收标准

```bash
cd ~/image-stitching/build
cmake .. && make -j$(nproc)
./image-stitching 2>&1 | tee /tmp/stitch_2x3.log
```

- [ ] 启动日志: `using 2x3 layout, 6 cameras`
- [ ] `/tmp/stitched_frames/` 6 路都有内容 (不再是 cam4-cam5 黑屏)
- [ ] 邻接对日志: `h01/h12/h34/h45 + v03/v14/v25` 全打印
- [ ] 视觉检查: 6 路图像拼接整齐, 无明显错位
- [ ] `num_img_=6` 真正生效 (不再静默 drop)

### 3.6 故障排查

| 现象 | 处理 |
|---|---|
| `BuildCameraRois2x3` 抛 `runtime_error("invalid 2x3 crop mapping for camera N")` | 检查该 cam 的 roi 坐标是否在 [0, W) 和 [0, H) 内 |
| BlendSeams 报 `cl_in[i1] null` | 检查对应 cam 的 `tasks_[i].enabled` |
| 邻接对 `h12` 等新 key 找不到 | 检查 `EstimateOverlaps2x3` 是否完整实现 |

---

## 4. 6 路 V4L2 摄像头采集 (1 周)

> **目标**: 写 `v4l2_capture_thread.cc`, 让 yaml 里 `type: mipi` 真正能跑, 取代当前的 fallback 到 file 行为.

### 4.1 前置条件

- 阶段 1.4 (RKISP 3A) ✅ — sensor 物理到位, ISP 通路 OK
- 阶段 1.3 (6 路拓扑) ✅ — 6 个 /dev/video* + 6 个 v4l-subdev 都 ready
- 阶段 3 ✅ — 6 路 2×3 layout ready (本阶段输出会被 2×3 处理)

### 4.2 详细步骤

1. **新建 `src/v4l2_capture_thread.cc`** (CLAUDE.md 速查表已列)
   - 每个 cam 一个线程, `VIDIOC_REQBUFS` + `VIDIOC_STREAMON`
   - 用 `poll()` 等帧, `VIDIOC_DQBUF` 取, 处理完 `VIDIOC_QBUF` 放回
   - 输出 NV12 frame 进 `image_queue_vector_[i]`

2. **`SensorDataInterface::InitVideoCapture` 改 `type: mipi` 分支** ([src/sensor_data_interface.cc:422](../src/sensor_data_interface.cc#L422))
   - 当前: `type: mipi` 跳过 + 日志告警
   - 改为: 创建 `V4L2CaptureThread(uri, width, height, fps)` 加进 `decode_threads_`

3. **`V4L2CaptureThread` 接口设计**
   ```cpp
   class V4L2CaptureThread {
   public:
     V4L2CaptureThread(const std::string& dev, int w, int h, int fps);
     void Start(std::queue<QueuedFrame>* out_q, std::mutex* out_mu);
     void Stop();
   };
   ```

### 4.3 验收标准

```bash
# YAML mipi 模式
export INPUT_SOURCE_MODE=camera
./image-stitching 2>&1 | tee /tmp/stitch_mipi.log
```

- [ ] 启动日志: `6 V4L2 capture threads started`
- [ ] 6 路 video 节点 (`/dev/video11..16` 或实际分配) 都打开成功
- [ ] `/tmp/stitched_frames/` 有连续帧
- [ ] 实际拼接结果与 dataset 模式 (阶段 3) 视觉一致 (同一标定参数下)

### 4.4 故障排查

| 现象 | 处理 |
|---|---|
| `VIDIOC_STREAMON` 失败 (EBUSY) | 6 个 video 节点没正确分配, 看 `ls /dev/video*` |
| `VIDIOC_DQBUF` 持续 EAGAIN | sensor 没出帧, 检查 dmesg MIPI 错误 |
| 6 路只有 4 路有帧 | 阶段 1.3 的 6 路拓扑验证有 2 路没 ready, 找厂商 |

---

## 5. 6 路相机标定 (1 周, 与 3-4 并行)

> **目标**: 录 6 路同步标定视频, 跑 `calibrate_intrinsics.py` + `calibrate_extrinsics.py`, 输出 `camchain_0..5.yaml` (K, D, R, T).

### 5.1 前置条件

- 阶段 1.4 (RKISP 3A) ✅ — sensor 能出图
- 阶段 3 ✅ (并行, 不阻塞) — 不需要等 2×3 代码
- 阶段 4 ✅ (并行, 不阻塞) — 用 dataset 模式录标定视频也行, 但用 mipi 更准

### 5.2 标定板准备

- A3 棋盘 (420×297 mm), **11 行 × 14 列内角点**, 方格 22mm
- 5-10 个位姿, 距离 20-40 cm, 板占比 35-60%
- ISP 3A 收敛后 (≥5 秒) 再录

### 5.3 详细步骤

**步骤 1: 录 6 路同步标定视频 (内参)**

```bash
mkdir -p /tmp/calib
for cam in 0 1 2 3 4 5; do
    gst-launch-1.0 v4l2src device=/dev/video$((11+cam)) num-buffers=60 ! \
        videoconvert ! x264enc ! \
        filesink location=/tmp/calib/cam${cam}_pose01.mp4 &
done
wait
ls /tmp/calib/*.mp4  # 应 6 个
```

**步骤 2: 跑内参标定 (Step A)**

```bash
python3 tools/calibrate_intrinsics.py \
    --input_dir /tmp/calib \
    --output_dir params/intrinsics \
    --board_rows 11 --board_cols 14 --square_size_mm 22
# 输出: params/intrinsics/cam{0..5}.npz + camchain_{0..5}.yaml + summary.txt
```

**验收**: `summary.txt` 里 6 路 fx 极差 < 5%, fx ≈ 1055 px

**步骤 3: 录 6 路同步视频 (外参, 静态棋盘)**

```bash
# 同样 5-10 个位姿, 棋盘同时被 ≥2 路看到
for pose in 01 02 03 04 05 06 07 08 09 10; do
    for cam in 0 1 2 3 4 5; do
        gst-launch-1.0 v4l2src device=/dev/video$((11+cam)) num-buffers=60 ! \
            videoconvert ! x264enc ! \
            filesink location=/tmp/calib_ext/cam${cam}_pose${pose}.mp4 &
    done
    wait
done
```

**步骤 4: 跑外参标定 (Step B)**

```bash
python3 tools/calibrate_extrinsics.py \
    --videos /tmp/calib_ext/cam{0..5}.mp4 \
    --intrinsics params/intrinsics/cam{0..5}.npz \
    --board_rows 11 --board_cols 14 --square_size_mm 22
# 输出: 写回 camchain_{1..5}.yaml 的 R, T (cam 0 是参考) + extrinsics/summary.txt
```

### 5.4 验收标准 (硬指标)

- [ ] 6 路内参 RMS < 0.5 px
- [ ] 6 路 fx 极差 < 5%
- [ ] 6 路 fx ≈ 1055 px (从 101° FOV 推算)
- [ ] 外参 T 模长符合物理基线 (summary.txt sanity check)
- [ ] 外参闭环误差 < 1° (4 闭环的旋转角度差)

---

## 6. 6 路 2×3 真机跑通 (1-2 周)

> **目标**: 镜头到位 + 阶段 3 + 阶段 4 + 阶段 5 全部完成后, 真机 6 路拼接稳定运行

### 6.1 前置条件

- 阶段 1.4 ✅
- 阶段 3 ✅
- 阶段 4 ✅
- 阶段 5 ✅

### 6.2 详细步骤

1. **板上实跑 mipi 模式**
   ```bash
   export INPUT_SOURCE_MODE=camera
   USE_ROI_CONFIG=1 SKIP_BOOTSTRAP=1 ./image-stitching
   # SKIP_BOOTSTRAP=1 跳过 ROI bootstrap (固定支架场景, 见 HISTORY §6.5)
   ```

2. **调 ROI 微调** ([params/roi_tuning.yaml](../params/roi_tuning.yaml))
   - 看输出画面, 微调 `cam0..cam5: offset_x/offset_y` 各 ±20 px
   - SDL2 可视化 (`ENABLE_VISUAL_TUNING=1`) 实时看 ROI 边框

3. **长跑稳定性测试**
   ```bash
   SAVE_STITCH_FRAMES=0 SAVE_DIAGNOSTIC_FRAMES=0 ./image-stitching
   # 跑 1 小时, 看 FPS 是否稳定 ≥30
   ```

4. **生成 calibration report**
   - 录 1 张拼接结果图 + 标定参数表
   - 写到 [docs/CALIBRATION_REPORT.md](CALIBRATION_REPORT.md)

### 6.3 验收标准

- [ ] 6 路拼接画面无明显错位 (< 5 px)
- [ ] 接缝处羽化无明显色差
- [ ] FPS ≥ 30 稳定
- [ ] 1 小时长跑无漂移
- [ ] 标定报告归档

### 6.4 文档归档

阶段 6 完成后更新:
- [HISTORY.md](HISTORY.md) — 加实测数据
- [CLAUDE.md](../CLAUDE.md) — 状态表全部 ✅
- [README.md](../README.md) — Current status 段更新
- [docs/CALIBRATION_REPORT.md](CALIBRATION_REPORT.md) — 实测数据

---

## 附录 A: 6 路 V4L2 验证命令合集

### A.1 板子基础信息

```bash
uname -a
cat /etc/os-release

ls -l /dev/video* | head -30
ls -l /dev/v4l-subdev*
ls -l /dev/media*

for i in $(seq 0 15); do
    if [ -e /dev/v4l-subdev$i ]; then
        cat /sys/class/video4linux/v4l-subdev$i/name
    fi
done
```

### A.2 GC4683 驱动验证

```bash
dmesg | grep -iE 'gc46|galaxycore'
dmesg | grep -E '0x4683|chip.id|sensor.id'
lsmod | grep gc46
```

### A.3 media-ctl 拓扑

```bash
media-ctl -p
media-ctl -d /dev/media0 -p
```

### A.4 I2C 验证

```bash
i2cdetect -l

for bus in $(i2cdetect -l | awk -F'[ \t]+' '{print $1}' | grep -oE '[0-9]+'); do
    echo "=== i2c-$bus ==="
    i2cdetect -y $bus 0x20 0x3F
done

# 直接读 sensor ID (镜头到位后)
i2cget -y <bus> 0x30 0x03f0    # 0x46
i2cget -y <bus> 0x30 0x03f1    # 0x83
```

### A.5 v4l2-ctl 抓帧

```bash
v4l2-ctl -d /dev/video11 --set-fmt-video=width=2560,height=1440,pixelformat=NV12
v4l2-ctl -d /dev/video11 --get-fmt-video

v4l2-ctl -d /dev/video11 --stream-mmap --stream-count=1 --stream-to=/tmp/frame.raw
ls -l /tmp/frame.raw  # 应 5,529,600 bytes

# 6 路批量
for n in 11 12 13 14 15 16; do
    v4l2-ctl -d /dev/video$n --set-fmt-video=width=2560,height=1440,pixelformat=NV12
    v4l2-ctl -d /dev/video$n --stream-mmap --stream-count=1 --stream-to=/tmp/cam${n}_frame.raw
    echo "cam${n}: $(ls -l /tmp/cam${n}_frame.raw | awk '{print $5}') bytes"
done
```

### A.6 gst-launch 实时画面

```bash
sudo apt install -y gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good

# 看 6 路实时画面
for n in 11 12 13 14 15 16; do
    gst-launch-1.0 v4l2src device=/dev/video$n ! videoconvert ! xvimagesink &
done

# 保存到文件
gst-launch-1.0 v4l2src device=/dev/video11 num-buffers=300 ! filesink location=/tmp/cam0.yuv
```

### A.7 ISP 3A

```bash
ls -l /etc/iqfiles/

sudo systemctl restart rkaiq_3A_server
sudo /usr/bin/rkaiq_3A_server --media=/dev/media0 --iq=/etc/iqfiles/gc4683.xml &

sudo rkaiq_tool --get-ae-state
sudo rkaiq_tool --get-awb-state
```

### A.8 性能监控

```bash
sudo cat /sys/class/devfreq/27800000.gpu/load
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
free -h
cat /sys/class/thermal/thermal_zone*/temp
top -p $(pidof image-stitching)
```

### A.9 工程调试

```bash
# 跑工程 (camera 模式, 6 路 mipi)
INPUT_SOURCE_MODE=camera ./image-stitching 2>&1 | tee /tmp/stitch.log

# 跑工程 (dataset 模式, 6 路 mp4)
unset INPUT_SOURCE_MODE && ./image-stitching 2>&1 | tee /tmp/stitch.log

# 性能测 (不落盘)
SAVE_STITCH_FRAMES=0 SAVE_DIAGNOSTIC_FRAMES=0 ./image-stitching

# 调可视化
ENABLE_VISUAL_TUNING=1 SHOW_ROI_MARKERS=1 ./image-stitching

# 跳过 ROI bootstrap (固定支架场景, 详 HISTORY §6.5)
USE_ROI_CONFIG=1 SKIP_BOOTSTRAP=1 ./image-stitching
```

### A.10 标定

```bash
# 录 6 路同步标定视频
for cam in 0 1 2 3 4 5; do
    gst-launch-1.0 v4l2src device=/dev/video$((11+cam)) num-buffers=60 ! \
        videoconvert ! x264enc ! \
        filesink location=/tmp/calib/cam${cam}_pose01.mp4 &
done
wait

# 跑内参标定
python3 tools/calibrate_intrinsics.py \
    --input_dir /tmp/calib \
    --output_dir params/intrinsics \
    --board_rows 11 --board_cols 14 --square_size_mm 22

# 跑外参标定
python3 tools/calibrate_extrinsics.py \
    --videos /tmp/calib/cam{0..5}.mp4 \
    --intrinsics params/intrinsics/cam{0..5}.npz \
    --board_rows 11 --board_cols 14 --square_size_mm 22

# 验证内参
python3 tools/undistort_preview.py \
    --input /tmp/calib/cam0_frame.jpg \
    --calib params/camchain_0.yaml \
    --output params/intrinsics/preview_cam0.png
```

---

## 附录 B: GC4683 关键参数速查

| 参数 | 值 | 标定 / 集成含义 |
|---|---|---|
| 输出格式 | **MIPI RAW** (RGB Bayer) | sensor → MIPI CSI-2 → `rkcif` → `rkisp` → NV12; **不可绕过 ISP** |
| 物理分辨率 | 2688×1520 (4:3) | sensor 全分辨率; 工程用 2560×1440 (16:9 ROI 模式) |
| 输出帧率 | 60fps @ 全分辨率 | MIPI 带宽够; 工程起步 30fps, 远期切 60fps |
| 像素尺寸 | 2.5μm × 2.5μm | 推算焦距; 等效焦距 50mm 对应镜头焦距 ≈ 50mm × (2.5μm / 36mm) 不适用 |
| DAG 电路 | 数字增益 | 低照度下用, 标定环境**避免** |
| DOL HDR | 长短帧合成 | RKISP 必须配 HDR 合成模式; 标定期间**关闭** |
| AOV / Fast AE / Quick start | 低功耗特性 | 拼接无影响; 冷启动 < 100ms |
| BSI | 背照式 | 黑光级 4M, 暗光性能好; 标定时**不要**在暗光下做 |
| 封装 | CSP | 板上集成, 不可换模组 |
| 镜头焦距 | (待模组厂确认) | fx ≈ 2560 / (2 × tan(FOV/2)); FOV 101° 对应 fx ≈ 1055 |
| I2C 地址 | 0x30 | 8-bit, 7-bit 地址是 0x18 |
| Chip ID | 0x4683 | 0x03f0=0x46, 0x03f1=0x83 |
| XVCLK | 24 MHz | 不是 27 MHz |
| link freq | 400 MHz (30fps) / 800 MHz (60fps) | 4-lane MIPI |
| 物理接口 | 4-lane MIPI CSI-2 | data-lanes = <1 2 3 4> |
| 镜头 HFOV | 101° | 2560 / 101° ≈ 25.35 px/° |
| 镜头 VFOV | 68° | 1440 / 68° ≈ 21.18 px/° |

### 4M vs 2K 关系

- sensor 物理像素: 2688×1520 ≈ **4M 像素** (4:3 全像素)
- 工程 "2K": 2560×1440 (16:9, 2K 标准)
- 两者关系: sensor 内部做 ROI 裁剪 (2688×1520 → 2560×1440, 丢 128 列 80 行)
- 2.5μm 像素下, 视角变化 < 1° (裁剪影响可忽略)
- 建议: 工程用 2560×1440 (16:9), sensor 在 ROI 模式跑

---

## 附录 C: 故障排查命令合集

### C.1 dmesg | grep gc46 不足 6 行

- 驱动未加载: 板端 kernel 6.1.75 已内置, 不需 modprobe. 如 `lsmod | grep gc46` 空, 检查 `cat /proc/config.gz | grep GC4683` (应 `=y`)
- 部分 sensor 失败: 看完整 dmesg 找具体哪路, 检查 FPC 排线
- DTS 节点不全: 见 §1.3, 找厂商要完整 DTB

### C.2 I2C 扫描无 0x30

- 硬件不通: 检查 FPC 排线 / I2C 上拉电阻 (4.7kΩ)
- sensor 没上电: 万用表量 AVDD (2.8V) / DOVDD (1.8V) / DVDD (1.2V)
- reset GPIO 没拉高: 检查 DTS 的 reset-gpios, 测量 reset pin
- I2C 地址错: 模组厂确认 (GC4683 一般 0x30)

### C.3 抓帧文件大小不是 5,529,600

- ISP 输出格式不是 NV12: 检查 `--get-fmt-video`
- 分辨率不对: 检查 sensor init table 是否配 2560×1440
- link freq 不匹配: `media-ctl -p` 看 link-frequencies, 找厂商

### C.4 实时画面黑屏

- ISP 没起来: 见 §1.4
- sensor 输出格式与 ISP 期望不符: 检查 sensor 输出的 MIPI 格式
- XVCLK (24 MHz) 没起: 用示波器量 sensor 的 XVCLK pin

### C.5 rkaiq_3A_server 段错误

- IQ 文件路径错或格式不对, 看 stack trace
- 联系厂商

### C.6 V4L2 线程 DQBUF 持续 EAGAIN

- sensor 没出帧, 检查 dmesg 看 MIPI 错误

### C.7 6 路画面错位

- 物理排布与标定 yaml 不符: 重新核对接线顺序
- 标定精度不够: 重新标定, 增加位姿

### C.8 FPS 不达标

- 关闭落盘: `SAVE_STITCH_FRAMES=0 SAVE_DIAGNOSTIC_FRAMES=0`
- 关 GLES warp: 让 ROI+RGA+OpenCL 跑
- 调 ROI bootstrap 帧数: `NUM_BOOTSTRAP_FRAMES=2`

### C.9 cam4/cam5 不出图 (dataset 模式)

- 阶段 2 已配置 yaml 但未生效: 检查 yaml 里 cam4/cam5 block 是否正确
- 阶段 2 已配置但 2×3 函数没改: 阶段 3 之前这是**预期行为**, 跑 dataset 6 路时 cam4-cam5 被静默 drop
- `num_img_=6` 但仍 4 输出: 见 §3.6 故障排查

---

## 附录 D: 文件改动总览 (v2.3)

| 文件 | 阶段 | 状态 | 改动说明 |
|---|---|---|---|
| [HISTORY.md](HISTORY.md) §0x06 | 1 | ✅ | 加 6 路 MIPI 验证清单 |
| [params/camera_sources.yaml](../params/camera_sources.yaml) | 2 | ⏳ | 加 cam4, cam5 块 (6 路) |
| [src/sensor_data_interface.cc:399](../src/sensor_data_interface.cc#L399) | 2 | ⏳ | `default_files` 加 t40.mp4, t41.mp4 (临时占位) |
| [include/roi_config.h:14](../include/roi_config.h#L14) | 2 | ⏳ | `roi_offsets[4]` → `[6]` |
| [src/app.cc:178](../src/app.cc#L178) | 2 | ⏳ | `i < 4` → `i < 6` |
| [src/image_stitcher.cc:301](../src/image_stitcher.cc#L301) | 2 | ⏳ | `cl_in(4)` → `(6)`, loop 改 6 |
| [params/roi_tuning.yaml](../params/roi_tuning.yaml) | 2 | ⏳ | 加 cam4, cam5 块 |
| [src/app.cc](../src/app.cc) `EstimateOverlaps2x3` | 3 | ⏳ | 新增函数, 7 对 overlap |
| [src/app.cc](../src/app.cc) `BuildCameraRois2x3` | 3 | ⏳ | 新增函数, 2 列 × 3 行 layout |
| [src/app.cc](../src/app.cc) `BuildStitchLayout2x3` | 3 | ⏳ | 新增函数, 6 个 task |
| [src/app.cc:598](../src/app.cc#L598) `BootStrapOptimalLayout` | 3 | ⏳ | 加 `num_img_ == 6` 分支 |
| [src/image_stitcher.cc](../src/image_stitcher.cc) `BlendSeams` | 3 | ⏳ | 改 9 个 dispatch_seam (3 水平 + 4 垂直 + 2 cross?) |
| [src/roi_visualizer.cc](../src/roi_visualizer.cc) | 3 | ⏳ | Tab 键 4 → 6 |
| [src/v4l2_capture_thread.cc](../src/) | 4 | ⏳ 新建 | V4L2 6 路采集线程 |
| [src/sensor_data_interface.cc:422](../src/sensor_data_interface.cc#L422) `InitVideoCapture` | 4 | ⏳ | `type: mipi` 路由启用 |
| [params/camchain_0..5.yaml](../params/) | 5 | ⏳ | 6 路标定输出 |
| [params/intrinsics/cam0..5.npz](../params/intrinsics/) | 5 | ⏳ | 6 路内参 |
| [params/intrinsics/summary.txt](../params/intrinsics/) | 5 | ⏳ | 6 路一致性报告 |
| [params/extrinsics/summary.txt](../params/extrinsics/) | 5 | ⏳ | 外参 sanity |
| [tools/calibrate_intrinsics.py](../tools/calibrate_intrinsics.py) | 5 | ✅ 已就绪 | Step A 单目内参 |
| [tools/calibrate_extrinsics.py](../tools/calibrate_extrinsics.py) | 5 | ✅ 已就绪 | Step B 立体外参 |
| [tools/undistort_preview.py](../tools/undistort_preview.py) | 5 | ✅ 已就绪 | 单图去畸变验证 |
| [docs/CALIBRATION_REPORT.md](CALIBRATION_REPORT.md) | 6 | ⏳ 新建 | 实测数据归档 |
| [HISTORY.md](HISTORY.md) | 6 | ⏳ | 加实测数据 |
| [CLAUDE.md](../CLAUDE.md) | 6 | ⏳ | 状态表全部 ✅ |
| [README.md](../README.md) | 6 | ⏳ | "Current status" 段更新 |

---

## 附录 E: 关键阈值与硬指标速查

| 项 | 阈值 | 来源 |
|---|---|---|
| 抓帧文件大小 | 5,529,600 bytes | 2560 × 1440 × 1.5 (NV12) |
| sensor probe 行数 | 6 行 (cam0..cam5) | dmesg \| grep gc46 |
| I2C 地址 | 0x30 | GC4683 规格 |
| sensor ID | 0x4683 (0x46, 0x83) | 0x03f0, 0x03f1 |
| 工程帧率 | 30 fps (起步) | yaml |
| sensor 物理帧率 | 60 fps | GC4683 规格 |
| 内参 RMS | < 0.5 px | 标定硬指标 |
| 6 路 fx 极差 | < 5% | 一致性 |
| fx 理论值 | ≈ 1055 px | 101° FOV, 2560 宽 |
| 标定板尺寸 | A3 (420×297 mm) | 已确认 |
| 标定方格 | 22 mm × 22 mm | 已确认 |
| 标定行 × 列 | 11 × 14 | 已确认 |
| 标定距离 | 20-40 cm | 已确认 |
| 标定板占比 | 35-60% | 已确认 |
| 3A 收敛等待 | ≥ 5 秒 (6 路) | RKISP 冷启动 |
| 拼接 FPS | ≥ 30 fps | 验收 |
| 画面错位 | < 5 px | 视觉验收 |
| 长跑时长 | 1 小时无漂移 | 稳定性 |
| 2×3 邻接对 | 7 对 (3 水平 + 4 垂直) | 2 列 × 3 行 |
| ROI bootstrap 帧数 | 3 | 已有 |
| ROI 置信度阈值 | 0.25 (低) / 0.7 (早退) | 已有 |
| Feather width 偶数 | 必为偶数 | kernel 除以 2 |
| xvclk | 24 MHz | GC4683 模组厂参考 |
| link freq (60fps) | 800 MHz | 4-lane × 1.6 Gbps/lane |
| link freq (30fps) | 400 MHz | 4-lane × 800 Mbps/lane |
| pixel rate (60fps) | 640 MHz | GC4683 |

---

**方案版本**: v2.3 (精简版, 2026-06-30 重构)
**精简原则**:
- 内核驱动适配 / 厂商发包 → rocktech 厂商负责 (kernel 6.1.75 已内置 v00.01.01)
- 应用编译 → 开发板自主 (板上 gcc/make/cmake), PC 不交叉编译 image-stitching
- 时间戳同步 → 删除 (6 路 MIPI 硬件同步)
- **跳过 4 路过渡态**: 当前 4 路代码直接改造为 6 路 2×3, 不做 4 路单独验证

**总周期**: 5-8 周
**下一步 (2026-06-30)**: 阶段 2 - 6 路数据集输入适配
**最后更新**: 2026-06-30