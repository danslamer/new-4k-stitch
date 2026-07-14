# HISTORY

设计演进时间线（精简）+ **完整操作手册** + **踩过的坑**。当前活跃计划见 `NETWORK_CAMERA_PLAN.md`（v3.0 IP camera 改造）。

---

## 0. 设计演进（一句话过完）

| 时点          | 关键事件                                                                                                                                                                                                 |
| ------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 初始          | OpenCV UMat, 4K×4-cam > 200fps（1080Ti）                                                                                                                                                                |
| 2026-03       | 切 4K 输入；运行时调参 env vars；KMat/RMat 改动                                                                                                                                                          |
| 2026-03-26    | **性能回归**: warp 并行改串行，FPS 50→个位；修复                                                                                                                                                  |
| 2026-04-01    | 集成 RK 硬件解码 (rkmpp + RGA)；目标转向 DMA-BUF 零拷贝                                                                                                                                                  |
| 2026-04-03    | bootstrap 用 OpenCV 特征检测 ROI → 裁剪坐标复用（无 remap, RGA 不支持）                                                                                                                                 |
| 2026-04-17 起 | SDL2 可视化调参；ROI YAML 持久化；多帧 ROI bootstrap (`NUM_BOOTSTRAP_FRAMES=3`, 阈值 0.25/0.7)                                                                                                         |
| 2026-06-29    | **板子选型**: rocktech RK3588 (6 路物理 MIPI 直连) 中选                                                                                                                                            |
| 2026-06-30    | DTS 6 路 sensor 节点验证（详 §1 DTS）                                                                                                                                                                   |
| 2026-07-06    | **架构硬约束锁定**: 弃 FFmpeg rkmpp（vendor 不维护 + ABI 不兼容, 0 帧），改 gstreamer1.0-rockchip1 mppvideodec `dma-feature=true`                                                                |
| 2026-07-07    | **绿条纹修复**: `mppvideodec` stride 上报偏小 → RGA + cvtColor 错位；POSIX `realpath()` 解决 yaml 路径; batch_transcode (mp4v→h264)。**6 路 100+ fps @ 100% DMA-BUF, 2×3 端到端跑通** |
| 2026-07-08    | **v3.0**: 6 路 IP camera RTSP 改造（PoE 8+2 交换机, 镜头 2.8mm 102.5°），取代 v2.x 的本地 mp4 / MIPI 计划                                                                                         |

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

#### 通用排错

| 症状                                       | 处理                                                              |
| ------------------------------------------ | ----------------------------------------------------------------- |
| `No route to host`                       | 不在同一子网；ICS 确认 PC 端`192.168.137.1`，WiFi 确认同一 SSID |
| `Permission denied (publickey)`          | 清旧 host key:`ssh-keygen -R 192.168.137.100`                   |
| `nmcli: command not found`               | 镜像不带 NetworkManager，改走 wpa_supplicant                      |
| `IP configuration could not be reserved` | WiFi 关联成功但 DHCP 拿不到 IP，绑静态 IP 跳过 DHCP               |
| IP 变了后 VSCode Remote 拒连               | `ssh-keygen -R <new_ip>`                                        |

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

### ⚫ g_feather_width 必须偶数

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
