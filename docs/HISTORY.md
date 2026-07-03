# HISTORY

Archived from the previous monolithic README. Read this for design rationale, debug history, and operational recipes. For the current 2×2 → 2×3 (6-cam) migration scope and gotchas, see `CLAUDE.md`.

> Reference paper: Du, Chengyao, et al. (2020). *GPU based parallel optimization for real time panoramic video stitching.* Pattern Recognition Letters, 133, 62-69.
# 记账
百度网盘会员 24.82
标定纸打印

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

> **⚠️ 不要照搬教程的 configure 行 (rocktech 主板 / Ubuntu 22.04 aarch64 验证, 2026-07-01)**
>
> 教程的 `./configure --prefix=/usr --enable-rkmpp ...` **在多架构 Debian/Ubuntu 上会装错路径**——`.so` 会到 `/usr/lib/`,但 ld 找的是 `/usr/lib/aarch64-linux-gnu/`,结果是"装上了但项目链不到,继续 fallback 到系统 FFmpeg 4.4 (无 rkmpp)"。
>
> **教程里 RGA / MPP 的 build 方法可以直接参考,FFmpeg 这一段必须改**:
>
> ```bash
> # 1. configure (关键是 --enable-shared + 显式多架构路径)
> ./configure --prefix=/usr \
>             --libdir=/usr/lib/aarch64-linux-gnu \
>             --shlibdir=/usr/lib/aarch64-linux-gnu \
>             --incdir=/usr/include/aarch64-linux-gnu \
>             --enable-gpl --enable-version3 \
>             --enable-libdrm --enable-rkmpp --enable-rkrga \
>             --enable-shared --disable-static
>
> # 2. build
> make -j$(nproc)
>
> # 3. install (教程是 sudo make install, 顺序对就行)
> #    注意: ffmpeg 6.x 的 Makefile 里 install-pkgconfig 这个 target 不存在,
> #    .pc 文件实际由 install-headers 那一段装. 不要拼 install-pkgconfig.
> sudo make install
>
> # 4. ld 缓存更新
> sudo ldconfig
>
> # 5. 验证
> ls -la /usr/lib/aarch64-linux-gnu/libavformat.so*
> # 期望: libavformat.so.60 -> libavformat.so.60.16.100
> # 系统里残留的 libavformat.so.58 (FFmpeg 4.4) 不影响,
> # 因为 libavformat.so 符号链接已指向 .so.60
> ```
>
> 漏了 `--enable-shared` 会只编 .a (静态库),项目链不到。漏了 `--libdir=...` 会装到 `/usr/lib/`(无 arch),同样链不到。两个都必加。

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

两条路径，根据场景选其一：

- **6.1 以太网 + ICS 共享**：工业现场稳定性最佳，不依赖 WiFi 信号
- **6.2 WiFi 直连路由/PC 热点**：新开发板自带 WiFi，桌面开发无需插网线

#### 6.1 以太网 + Windows ICS 共享（推荐现场）

前提：PC 与板子用网线直连，PC 端以太网适配器开启 "Internet 连接共享 (ICS)"，共享后 PC 这端 IP 自动变为 `192.168.137.1`。

```bash
# === 在板子上配置静态 IP (网卡名按实际情况替换, Rockchip 板常见 end1/eth0) ===
sudo vim /etc/network/interfaces
# 按 i 进入插入模式，追加 (假设 PC ICS 端 IP 是 192.168.137.1)：
auto eth0
iface eth0 inet static
    address 192.168.137.100
    netmask 255.255.255.0
    gateway 192.168.137.1
    dns-nameservers 8.8.8.8 114.114.114.114
# Esc -> :wq 保存退出

# 重启网络（或直接 reboot）
sudo systemctl restart networking

# 验证链路
ping 192.168.137.1        # PC 端
ssh rocktech@192.168.137.100  # 从 PC 连入板子 (默认用户)
# 如需 root 权限: sudo -i
```

#### 6.2 WiFi 连接（新开发板支持，推荐桌面开发）

分两步：(a) 板子连上 AP；(b) PC / VSCode 远程连入板子 IP。

**(a) 板子连 AP — 优先用 NetworkManager (nmcli)**

大多数新出厂的 Rockchip Debian/Ubuntu 镜像自带 NetworkManager + `nmcli`。先确认：

```bash
nmcli --version                 # 应有版本输出
nmcli device status            # 看到 wlan0 处于 disconnected / unavailable 即正常
rfkill list                     # 若 soft blocked: yes，执行 sudo rfkill unblock wifi
sudo ip link set wlan0 up
```

连入 AP 并开启自动重连：

```bash
# 连接 WiFi (SSID 无空格可省引号)
sudo nmcli device wifi connect "MyWiFi" password "MyPasswd" ifname wlan0
  
# 已保存网络的常用维护
sudo nmcli connection show
sudo nmcli connection up   "MyWiFi"   # 手动启用
sudo nmcli connection down "MyWiFi"   # 断连
sudo nmcli connection delete "MyWiFi" # 删除已保存配置
```

查看拿到的 IP：

```bash
ip -4 addr show wlan0 | grep inet      # 通常是 192.168.x.x
hostname -I
```

如果想给板子绑固定 IP（便于 VSCode 保存 SSH 配置，且避免与 6.1 以太网方案的 IP 混淆）：

Windows 自带移动热点的网段固定为 `192.168.137.0/24`（与 ICS 共享同子网，Windows 不让改），所以**只换最后一段**：

- **6.1 以太网 ICS**：板子 `192.168.137.100` / PC `192.168.137.1`
- **6.2 WiFi 移动热点**：板子 `192.168.137.200` / PC `192.168.137.1`（热点自带）

日志、`~/.ssh/config`、VSCode Remote 里一眼能分清走的是哪条链路。

```bash
sudo nmcli connection modify "MyWiFi" \
    ipv4.method manual \
    ipv4.addresses 192.168.137.200/24 \
    ipv4.gateway 192.168.137.1 \
    ipv4.dns "8.8.8.8 114.114.114.114"
sudo nmcli connection down "MyWiFi" && sudo nmcli connection up "MyWiFi"
```

> **⚠️ 连 Windows 移动热点时建议直接绑静态 IP**：Windows 移动热点的内置 DHCP（基于 Wi-Fi Direct 虚拟适配器）和 Rockchip 板端 DHCP 客户端常对不上节奏，现象是 `nmcli device wifi connect` 转圈后报 `error: connection activation failed: (5) IP configuration could not be reserved (no available address, timeout, etc.)`。WiFi 关联其实已经成功，只是拿不到 IP。直接把 `ipv4.method` 改成 `manual`、按上面的表格绑 `192.168.137.200/24`（WiFi 热点）即可（PC 端热点默认 `192.168.137.1`，先在 PC 上 `ipconfig` 确认 "本地连接*X" 适配器的 IPv4 地址再用）。


#### 6.3 通用：清理 VSCode / ssh 旧密钥

板子 IP 变更、系统重烧或 SSH 主机 key 变化时，VSCode Remote-SSH 会拒绝连接。先清掉本地缓存：

```bash
# Linux / macOS / Git Bash
ssh-keygen -R 192.168.137.100   # 6.1 以太网方案
ssh-keygen -R 192.168.137.200   # 6.2 WiFi 方案 (Windows 移动热点)
ssh-keygen -R 192.168.99.100    # 6.2 WiFi 方案 (Connectify / 第三方工具)
```

Windows PowerShell 同义命令：

```powershell
ssh-keygen -R 192.168.137.100
ssh-keygen -R 192.168.137.200
```

> **排错速查**：
> - `No route to host`：PC 与板子不在同一子网；ICS 方案确认 PC 端 IP 是 `192.168.137.1`，WiFi 方案确认两者连同一个 SSID。
> - `Permission denied (publickey)`：执行 6.3 清掉旧 host key 后重试。
> - `nmcli: command not found`：镜像不带 NetworkManager，改走 6.2 (a') 的 wpa_supplicant 路线。
> - `error: connection activation failed: (5) IP configuration could not be reserved`：WiFi 关联成功但 DHCP 拿不到 IP，Windows 移动热点的内置 DHCP 兼容性差。先 `sudo iw dev wlan0 link` 确认 SSID 已 Connected，然后**给板子绑静态 IP**（见上方 ⚠️ 提示框），跳过 DHCP。
> - WiFi 反复掉线：检查 `sudo iw dev wlan0 link` 是否能看到 AP；信号弱时优先 5GHz 或就近部署。

#### 6.4 账户与项目目录约定 (2026-06-30 实测校正)

##### 6.4.1 账户约定

| 用户 | 默认 home | 用途 |
|---|---|---|
| `rocktech` | `/home/rocktech` | **日常 SSH 开发**（首选，避免误操作） |
| `root` | `/root` | 系统级配置；root 无密码，`sudo -i` / `su -` 切换 |

SSH 文档示例统一用 `rocktech@<ip>`，要 root 权限的命令显式标 `sudo`：

```bash
ssh rocktech@192.168.137.100          # 默认登入
sudo -i                                # 进交互式 root shell
sudo systemctl restart networking      # 单条命令提权
```

CMakeLists.txt:10 的 `$ENV{HOME}/dev/ffmpeg60` 在 rocktech 账户下解析为 `/home/rocktech/dev/ffmpeg60`。

##### 6.4.2 关于 `/userdata` 的重要修正

**实测**：主板 A (rocktech) 镜像上 `/userdata` **不是独立分区**，就是 rootfs 下的一个普通目录：

```bash
$ df -h | grep -E "/$|/userdata"
/dev/root        57G  5.8G   49G  11% /          # 没有第二行 /userdata 的 mount

$ ls -ld /userdata
drwxr-xr-x 2 root root 4096 Mar 25  2026 /userdata  # 4096 = 空目录
```

**结论**：不要相信 `/userdata` 是"抗重烧的安全区"。整个 57G 都共享一个 rootfs，刷系统后**全部丢失**。

##### 6.4.3 推荐的项目目录

按推荐度排序：

1. **`/home/rocktech/image-stitching/`** ★ 推荐
   - 默认用户的 home，无需 sudo
   - VSCode Remote 打开路径最自然
   - 49G 空间够 build + 调试落盘
   - 如果重烧系统，源码从 git clone 拉回来即可，build 缓存可丢弃

2. **`/userdata/Projects/rocktech/new-4k-stitch/`** (沿用历史 yzy 路径风格)
   - 与 § 8 历史路径风格一致
   - 但**没有抗重烧优势**，仅风格延续
   - 需要 `sudo mkdir` 创建

3. ❌ 不推荐放 `/userdata/image-stitching/`
   - 平铺在 `/userdata/` 下不便分类管理
   - 没分区优势（见 6.4.2）

##### 6.4.4 板端操作流程 (rocktech 账户)

```bash
# 1) SSH 登入 (默认用户)
ssh rocktech@192.168.137.100

# 2) 创建项目目录
mkdir -p ~/image-stitching
cd ~/image-stitching

# 3) 拉取代码 (git clone 或 scp/rsync 从 PC 推)
#    git clone <repo-url> .

# 4) FFmpeg 路径 (CMakeLists.txt:10 期望 $HOME/dev/ffmpeg60)
mkdir -p ~/dev
# 把编译好的 ffmpeg60 放到 ~/dev/ 下, 或软链:
ln -s /opt/ffmpeg60 ~/dev/ffmpeg60   # 视实际位置

# 5) 板上构建 (有 gcc/make/cmake, 无需交叉编译)
mkdir build && cd build
cmake ..
make -j$(nproc)
./image-stitching

# 6) 系统级改动才用 root
sudo systemctl restart networking
sudo iw dev wlan0 link
```

#### 6.5 SKIP_BOOTSTRAP 与 GC4683 固定云台场景 (2026-06-30)

**为什么需要这个开关**

ROI bootstrap (`BootStrapOptimalLayout`) 是为**相机位置不确定**的 PC 桌面调试场景设计的（用 `t40`/`h40` 这类数据集反复试不同位置）。车载固定云台场景（GC4683 焊在支架上不动、几何由 `camchain_*.yaml` 决定）跑 bootstrap 是浪费：

- 启动慢 ~600ms（捕获 3 帧 + OpenCV feature detect）
- 低质量视频（如 `t00`/`t30`）会触发低置信度分支，需要二次重试
- 输出的 `roi_tuning.yaml` 是 feature matching 的结果，**不是几何真值**

但完全去掉 bootstrap 又会让"yaml 还没填"或"yaml 误删"的场景**直接报错退出**，对调试不友好。

**SKIP_BOOTSTRAP 的语义** (与用户 2026-06-30 确认)

| `USE_ROI_CONFIG` | `SKIP_BOOTSTRAP` | YAML 存在 | YAML 缺失 | 行为 |
|---|---|---|---|---|
| 0 | 任意 | 忽略 | 忽略 | 强制跑 bootstrap，不读 yaml (旧调试用) |
| 1 (默认) | 0 (默认) | 用 yaml | 跑 bootstrap 兜底 | 旧行为，PC 调试友好 |
| 1 | **1** | 用 yaml | **跑 bootstrap 兜底 + 警告日志** | 固定云台场景，意图明确 |

**结论**：第三行是车载场景的标准姿势——`USE_ROI_CONFIG=1 SKIP_BOOTSTRAP=1 ./image-stitching`，缺文件不崩。

**代码改动** (2026-06-30)

- [src/app.cc:35](src/app.cc#L35) — 加 `bool g_skip_bootstrap = false;`
- [src/app.cc:1066-1067](src/app.cc#L1066-L1067) — `main()` 解析 `SKIP_BOOTSTRAP` env var
- [src/app.cc:909-925](src/app.cc#L909-L925) — 入口加 SKIP_BOOTSTRAP 模式日志

**GC4683 几何参数** (固定云台场景)

| 项 | 值 | 备注 |
|---|---|---|
| HFOV | 101° | 2560 px / 101° ≈ 25.35 px/° |
| VFOV | 68° | 1440 px / 68° ≈ 21.18 px/° |
| 分辨率 | 2560 × 1440 | 已写入 [params/camera_sources.yaml](params/camera_sources.yaml) |
| 相邻重叠 | ~20° | 水平 ~507 px, 垂直 ~424 px |
| fx 期望 | ~1055 px | (1280 / tan(50.5°)) |
| fy 期望 | ~1067 px | (720 / tan(34°)) |
| 布局 | 2×3 (6 路) → 阶段 4 迁移 | 当前 v2 跑 2×2 (4 路), 设计目标是 2×3 |

**ROI 几何验证**

| 检查 | 值 | 状态 |
|---|---|---|
| `search_w` ([src/app.cc:196](src/app.cc#L196)) | common_w/2 = 1280 | ✅ ≥ 507 px 重叠 |
| `band_w` ([src/app.cc:269](src/app.cc#L269)) | common_w/2 = 1280 | ✅ ≥ 507 px 重叠 |
| `band_h` ([src/app.cc:198](src/app.cc#L198)) | min(288, 480) = 288 | ✅ |
| `template_w` ([src/app.cc:197](src/app.cc#L197)) | 853 px | ✅ 远大于重叠区 507 px, 留 1.7x 余量 |
| `max_shift_y` ([src/app.cc:199](src/app.cc#L199)) | min(240, 120) = 120 | ✅ GC4683 微小角度安装偏差 ≤ 120 px |

**当前 `params/camchain_0.yaml` 是历史数据**（1920×1080, fx~1980, 旧相机），GC4683 上线后必须重跑 `tools/calibrate_intrinsics.py` 生成新版本（2560×1440, fx~1055）。

**`USE_ROI_CONFIG` vs `SKIP_BOOTSTRAP` 的取舍**

| 场景 | 推荐 | 理由 |
|---|---|---|
| PC 上调数据集 (`t40`, `h40`, ...) | 默认（`USE_ROI_CONFIG=1 SKIP_BOOTSTRAP=0`） | 第一次 bootstrap 自动生成 yaml, 后续启动直接用 |
| 车载固定云台, yaml 已通过 calibrate 准备好 | `USE_ROI_CONFIG=1 SKIP_BOOTSTRAP=1` | 显式声明"我信任 yaml"; 万一误删 yaml 不会崩 |
| 临时强制重检 (调参后想覆盖 yaml) | `USE_ROI_CONFIG=0` | 旧路径, 强制 bootstrap + 写 yaml |
| yaml 已确认无误, 但相机挪了位置 | 删 yaml, 默认启动 | bootstrap 重算覆盖旧 yaml |

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

**当前 (rocktech 账户, 2026-06 起)**

| 用途 | 路径 | 说明 |
|---|---|---|
| 项目仓库 | `/home/rocktech/image-stitching/` | ★ 推荐位置（参见 6.4.3） |
| FFmpeg rkmpp | `/home/rocktech/dev/ffmpeg60/` | `CMakeLists.txt:10` 期望 `$HOME/dev/ffmpeg60` |
| 调试落盘 | `/home/rocktech/image-stitching/stitched_frames/` | `SAVE_STITCH_FRAMES` 输出 |
| OpenCV 4.9 | `/home/rocktech/dev/opencv-4.9.0/build/` | 板上编译的 OpenCV |
| rknn 虚拟环境 | `/home/rocktech/rknn-env/bin/activate` | `source ...` 激活 |

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
opencode --session ses_102488216ffe5ZYVE3jOAQNt20
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

## 0x04 v2 阶段: GC4683 + 6 路 MIPI (厂商规格, 当前运行时仍 4 路)

> **📋 9 阶段开发计划已迁移到 [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md)**。本节保留设计约束、参数速查、决策依据与 v1/v2 差异分析, 不再重复阶段细节。
>
> **执行状态**: 见 §5 (阶段实操进展)。**完整开发方案**: 见 [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md)。

> **⚠️ 重要事实区分**:
> - **厂商规格**: rocktech 主板原生 6 路物理 MIPI 直连
> - **当前 DTS 实例化**: **4 路** (i2c-2/3/4/6, 各 1 个 gc4683)
> - **当前运行时**: 全部走 **2×2 = 4 路** ([src/sensor_data_interface.cc](src/sensor_data_interface.cc) 默认 t50-t53.mp4, [src/app.cc](src/app.cc) `BuildCameraRois2x2`, [src/image_stitcher.cc](src/image_stitcher.cc) `BlendSeams` 硬编码 4)
> - **6 路 yaml / mipi / 2×3 代码迁移**: 全部属**阶段 4**, 未完成, 见 [CLAUDE.md §"2×3 迁移硬编码点速查"](CLAUDE.md)
>
> 阶段 1 实测前**先确认**: 板上 DTS 是否需要厂商补齐 2 路 (见 §0x06 验证清单)

> 本节替代上一版 "MIPI 4 路 + USB 2 路" 假设, 反映 **2026-06 厂商规格确认**: 当前 rocktech 主板原生 **6 路物理 MIPI 直连**, USB 摄像头**完全废弃**。

### 4.1 新约束摘要 (与 v1 假设的差异)

| 维度 | v1 假设 (MIPI 4 + USB 2) | **v2 实际 (MIPI 6)** | 影响 |
|---|---|---|---|
| sensor 型号 | 通用 2K 摄像头 | **GC4683** (Galaxycore, 4MPixel, 1/2.5", 2.5μm, BSI, RAW 输出, 60fps, DAG/DOL HDR/AOV/Fast AE/Quick start, CSP 封装) | sensor 出 **MIPI RAW**, 必须过 RKISP 才能得到 NV12; sensor 分辨率 2688×1520 (4:3) 而非 2560×1440 (16:9), 需要明确 ROI 模式 |
| MIPI 路数 | 4 | **6** | 当前板原生 6 路直连, 直接做 2×3 拼接; 原本 2×3 + USB 混接的 4 类问题简化为只剩 2 类 (格式 / 时钟) |
| USB 摄像头 | 2 | **0** | 删除: Phase 5 USB 软解 + YUYV→NV12 RGA 转换路径, 4.1 格式不一致问题**全部消失** |
| 帧率 | 30fps | **60fps** (GC4683 物理能力) | 6 路 2K@60 总带宽 ≈ 1.98 GB/s, 是 30fps 的 2 倍; 解码 + 拼接双倍压力 |
| HDR | 无 | DOL HDR (sensor 级) | 决定 ISP 3A 收敛时间; 标定采集时必须等收敛 |
| 时钟同步 | 4 MIPI 软同步 | **6 MIPI 软同步** | 6 路 MIPI 软同步, 同 ISP, 漂移可忽略 |
| 光度一致性 | 4 同型号 | **6 同型号** | 同型号 sensor, ISP 3A 共享, 一致性好; 但 DOL HDR 模式下 EV 切换, 需要确认 6 路 EV 一致 |

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

### 4.3 v2 工程改造时间线

**完整 9 阶段计划 (含详细命令与验收标准) 已迁移到 [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md)。**

简版时间线:

| 阶段 | 时间 | 依赖 | 详细内容 |
|---|---|---|---|
| 1. GC4683 + RKISP 通路验证 | 1-2 周 | 板子到位 | [DEVELOPMENT_PLAN § 阶段 1](DEVELOPMENT_PLAN.md#阶段-1-gc4683--rkisp-通路验证-1-2-周) |
| 2. CameraSource 抽象 (YAML) | 2-3 天 | — | [§ 阶段 2](DEVELOPMENT_PLAN.md#阶段-2-camerasource-抽象-yaml-驱动--已完成) ✅ |
| 3. V4L2 采集线程 | 1 周 | 阶段 1 | [§ 阶段 3](DEVELOPMENT_PLAN.md#阶段-3-v4l2-采集线程-mipi-专用-1-周) |
| 4. 软件时间戳同步 | 1 周 | 阶段 3 | [§ 阶段 4](DEVELOPMENT_PLAN.md#阶段-4-软件时间戳同步-1-周) |
| 5. 帧率决策 (30fps) | 半天 | — | [§ 阶段 5](DEVELOPMENT_PLAN.md#阶段-5-帧率决策-30fps--已完成) ✅ |
| 6. 相机标定 | 1 周 | 阶段 1 (并行) | [§ 阶段 6](DEVELOPMENT_PLAN.md#阶段-6-相机标定-1-周-与-2-4-并行) |
| 7. 2×2 真机跑通 | 1 周 | 阶段 1-6 | [§ 阶段 7](DEVELOPMENT_PLAN.md#阶段-7-22-拼接跑通-1-周) |
| 8. 2×3 扩展 (6 路板) | 1-2 周 | 远期, 换板后 | [§ 阶段 8](DEVELOPMENT_PLAN.md#阶段-8-23-扩展-远期-换-6-路板后-1-2-周) |
| 9. 收尾归档 | 1 周 | 阶段 7 | [§ 阶段 9](DEVELOPMENT_PLAN.md#阶段-9-收尾归档-1-周) |

甘特图: 见 [DEVELOPMENT_PLAN § 0.3](DEVELOPMENT_PLAN.md#03-甘特图)。

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

**关键并行**: 阶段 6 (标定) 与阶段 2-5 (代码) 完全独立, 可同时进行; 阶段 6 不需要等代码就绪, 只需 6 路物理能出图。

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
5. **MIPI 线序**: 6 路 MIPI 的物理走线长度差异 (信号完整性, 间接影响同步精度) — 一般 < 5cm 差异不影响, 但要确认没有超过 15cm 的差异。
6. **AOV 模式**: 4 路是否长期在线? 如果是 24/7 拼接, AOV 模式没必要, sensor 全速跑; AOV 适合 1Hz 待机 + 触发唤醒。

### 4.9 与 CLAUDE.md 的同步关系

**CLAUDE.md 主入口**已记录:
- 当前布局: 2×2 (4 路) → **迁移中** → 2×3 (6 路, 2 列 × 3 行)
- 板上物理 MIPI: 厂商规格 6 路直连, 当前 2×2 是过渡, 直接进 2×3
- 硬编码点速查: 见 CLAUDE.md §"2×3 迁移硬编码点速查"

---

## 0x05 阶段实操进展 (2026-06-29 精简版更新)

> **v2.2 精简原则**:
> - 内核驱动适配 / 厂商发包 → **rocktech 厂商负责**, 我方只做镜像烧写后的可用性测试
> - 应用编译 → **开发板自主** (板上 gcc/make/cmake), **PC 不交叉编译** image-stitching
> - 时间戳同步 → **删除** (6 路 MIPI 硬件同步)
> - 2×2 拼接 → **跳过**, 直接做 2×3
>
> **🛠 板子选型校正 (2026-06-29 实测)**: 经实测确认,
> - **主板 A (rocktech)**: **6 路物理 MIPI 直连**, **作为主开发板**
> - **主板 B (Neardi)**: **仅 2 路物理 MIPI** (需外接串行器扩展到 6 路), **仅作测试用**
> - 注: [docs/主板系统环境差异.docx](主板系统环境差异.docx) 的 "V4L2 框架探测" 行记录的是 dmesg 输出, 两块板都显示 "6 · MIPI CSI 成功", 但这是虚拟 CSI 设备数, **不代表物理端口数**。物理端口以实测为准。
> - 详细对比见 [CLAUDE.md § 开发板选型](../CLAUDE.md#开发板选型-2026-06-29-决策-实测校正)
>
> **完整方案**: [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md) **v2.3** (2026-06-30 重构, 跳过 4 路过渡态)

### 5.1 当前阶段状态 (精简版, 6 阶段, 2026-06-30)

| # | 阶段 | 状态 | 备注 |
|---|---|---|---|
| 1 | 板子与驱动可用性测试 | ⏳ 进行中 | SSH ✅, 驱动 ✅, 6 路拓扑 ⏳ (详 [HISTORY §0x06](HISTORY.md)) |
| **2** | **6 路数据集输入适配** | **⏳ 下一步** | 当前 4 路 yaml/默认文件/roi_offsets → 6 路 |
| 3 | 6 路 2×3 代码迁移 | ⏳ | 2×2 函数 → 2×3 |
| 4 | 6 路 V4L2 摄像头采集 | ⏳ 等镜头 | 写 v4l2_capture_thread.cc |
| 5 | 6 路相机标定 | ⏳ 与 3-4 并行 | 工具就绪, 待镜头 |
| 6 | 6 路 2×3 真机跑通 | ⏳ 等 3-5 | 实测 + 归档 |

各阶段详述见 [DEVELOPMENT_PLAN v2.3](DEVELOPMENT_PLAN.md).

---

## 0x05 历史 (9 阶段方案, 已废弃)

> 下方是 v1 时代的 9 阶段方案, 已被 v2.2 精简版替代。新阶段编号见上方 [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md)。历史背景保留供参考。

### 5.0 9 阶段方案 (历史, v2.2 已精简)

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

- `tools/calibrate_intrinsics.py` — **Step A 单目内参**: 11×14 棋盘 22mm, 6 路独立标定, 输出 `params/intrinsics/cam{N}.npz` + `params/camchain_{N}.yaml` + `params/intrinsics/summary.txt` (6 路一致性报告)
- `tools/calibrate_extrinsics.py` — **Step B 立体外参**: 6 路同步视频 (cam 0 为参考) → 写回 `camchain_{1..5}.yaml` 的 R, T 字段 + `params/extrinsics/summary.txt` (T 模长 sanity)
- `tools/undistort_preview.py` — **验证**: 单图加载 `camchain_N.yaml` 的 K, D → `cv2.undistort()` → 左右对比存 `params/intrinsics/preview_cam{N}.png`

#### 标定板规格 (已确认)

- A3 横向 (420×297 mm), 哑光激光打印, 标定区 308×242 mm, 边距各 25mm
- 11 行 × 14 列黑白方格, 单格 22 mm, 左上角**黑色**
- 距摄像头 20-40 cm, 板占画面 35-60%
- 室内漫射光, 关闭 DOL HDR, 等 ISP 3A 收敛 3 秒再录

#### 验收标准 (硬指标)

- 单路 RMS < 0.5 px
- 6 路 fx 极差 < 5%
- 6 路 fx 接近理论值 1055 px (从 101° FOV 推算)

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
2. 拿到驱动后, 按 §2.1 跑 v4l2-ctl 链路验证 + 启动 rkaiq_3A_server, 让 6 路 2560x1440 NV12 抓帧成功
3. 把 `params/camera_sources.yaml` 的 6 路 mipi 真正跑通后, 阶段 7 启动 2×3 拼接
4. 标定流程**独立**于驱动, 可以并行做: 打印 A3 棋盘 → 录 6 路同步视频 → 跑 `calibrate_intrinsics.py` + `calibrate_extrinsics.py`

### 5.7 driver 代码已到位 (2026-06-26)

- 文件: `drivers/gc4683.c`, 1773 行
- 协议: GPL-2.0, Driver version 0.0x01.0x01
- compatible: `"galaxycore,gc4683"`, I2C 地址 0x30
- 内置完整 2560×1440 10bit linear init table (driver line 199 起)
- XVCLK: **24 MHz** (不是 datasheet 27 MHz, 是模组厂参考设计值)
- 链路: I2C → sensor subdev → mipi-csi2 → rkcif → rkisp0
- 验证机制: `gc4683_check_sensor_id` 读 0x03f0/0x03f1 应为 0x46/0x83

**当前 driver 配置的 init table 是按 30fps 配的**:
- `hts_def = 0x0226*4 = 2200` 行周期
- `vts_def = 0x0c80 = 3200` 帧长
- `max_fps = 10000/300000 = 30fps`
- `link_freq = 400 MHz` (4-lane × 800 Mbps/lane)
- 理论最大帧率 1 / (2200 × 3200 / 320e6) ≈ **45 fps**, 配 30fps 还有 50% 余量

**用户决策 (2026-06-26)**: sensor 物理能力 60fps, 应让 BSP 厂商按**最大性能 60fps** 配 init table + link freq + DTS, 工程层只选 30fps 用 (sensor 能力 50%, 留 50% 余量做拼接). 后续 60fps 真要跑 (远期), 改 yaml `fps: 60` 即可, 不动代码.

**厂商发包要求 (按 60fps 适配)**:
1. 重新配 init table: 减 hts (可能 0x0226*2) 和 vts (保持 0x0c80 或更小), 让 max_fps = 60
2. link freq 升到 800 MHz (4-lane × 1.6 Gbps/lane) 或保持 400 MHz, 视 ISP 能力
3. pixel rate 升到 640 MHz
4. DTS `assigned-clock-rates = <24000000>` (保持 24 MHz)
5. 提供 4 路 (cam0..cam3) 完整 DTS 节点: I2C 总线 / GPIO / regulator / pinctrl / data-lanes / endpoint
6. 提供 IQ JSON: `/etc/iqfiles/gc4683_*.json` (或告知软链哪个通用 xml)
7. 烧写包: kernel Image + DTB + 烧写脚本
8. 启动后 `dmesg | grep gc46` 应看到 4 行 "detected gc4683 sensor"

**阶段 1 验收标准 (修订)**:
- 6 路 sensor probe 成功 (dmesg 6 行)
- `/dev/v4l-subdev*` 出现 6 个 gc4683 subdev
- `media-ctl -p` 显示 6 路 sensor → mipi-csi2 → rkcif 链路
- rkisp_mainpath 输出 2560×1440 NV12 (v4l2-ctl --get-fmt-video)
- 抓 1 帧文件 5529600 bytes
- rkaiq_3A_server 启动成功 (无段错误)
- 6 路 60fps 跑通 (`fps: 60` 测一遍, 记录最大稳定帧率)

**阶段 3 (V4L2 采集线程) 计划**: 等阶段 1 实际跑通 (你跑出 dmesg 4 行 + 抓帧成功) 再切 build mode 写, 避免现在写出来要重调.

---

## 0x06 6 路 MIPI 板上验证清单 (2026-06-30, 镜头未到位)

**目的**: 厂商规格说 6 路, 实际 DTS 只看到 4 路 gc4683 probe。需要在不接摄像头的情况下验证:
1. 硬件到底是 4 路还是 6 路
2. 如果是 6 路, DTS 缺什么需要厂商补齐
3. 哪些节点已 ready, 哪些还要等

### 验证命令 (sysfs 优先, /proc/device-tree 不可靠)

> **⚠️ 2026-06-30 实测发现**: `/proc/device-tree` 在本镜像上**返回空** (用户跑 `find /proc/device-tree -name "*gc4683*"` 和 `-path "*dphy*"` 都 0 条). 可能原因: CONFIG_PROC_DEVICETREE 未启用, 或 DT 暴露位置在 `/sys/firmware/devicetree/base/`. 改用 **sysfs** 命令更可靠:

```bash
# === A. DT 路径探查 (确认哪个路径可用) ===
ls /proc/device-tree/ 2>/dev/null | head -5
ls /sys/firmware/devicetree/base/ 2>/dev/null | head -5

# === B. 数 gc4683 实例 (sysfs 路径, 不依赖 /proc/device-tree) ===
find /sys/bus/i2c/devices -maxdepth 1 -name "*-003[01]" 2>/dev/null | wc -l
# 期望 6 (gc4683 i2c 地址 0x30 或 0x31)

# === C. 数 dphy 实例 ===
ls /sys/class/phy/ 2>/dev/null | grep -i dphy | wc -l
find /sys/bus/platform/devices -maxdepth 1 -name "*dphy*" 2>/dev/null | wc -l

# === D. 数 mipi-csi2 receiver ===
grep -l "rockchip-mipi-csi2" /sys/class/video4linux/v4l-subdev*/name 2>/dev/null | wc -l

# === E. 找 DT 里所有 galaxycore compatible (覆盖节点命名差异) ===
grep -rl "galaxycore" /sys/firmware/devicetree/base/ 2>/dev/null
grep -rl "galaxycore" /proc/device-tree/ 2>/dev/null

# === F. dmesg sensor 实际数量 (排除 warnings, 只数 driver version) ===
dmesg | grep "gc4683 .*driver version" | wc -l

# === G. i2c 总线 → sensor 对应关系 ===
for d in /sys/bus/i2c/devices/i2c-*/0-0031; do
    [ -e "$d" ] && echo "$d: $(cat $d/name 2>/dev/null)"
done 2>/dev/null
```

### 判定表

| 验证项 | 期望 (6 路) | 判定 |
|---|---|---|
| `*-003[01]` 实例数 | 6 | <6 → 厂商 DTS 缺 sensor 节点 |
| dphy 实例数 | 6 (0..5) | <6 → DTS 缺 dphy |
| `rockchip-mipi-csi2` receiver | 6 | <6 → 缺 receiver 绑定 |
| `galaxycore` DT 节点数 | 6 | <6 → DTS 缺 sensor (独立于 sysfs 二重确认) |
| `driver version` dmesg 行数 | 6 (镜头未接时 4) | <4 → DTS 缺 sensor |
| i2c 适配器总数 | ≥6 (含 5/8) | 缺 5/8 → i2c 总线不够挂 6 个 sensor |

**当前 (2026-06-30) 已知**:
- `/proc/device-tree` 命令返回空 (路径不可用, 改用 sysfs)
- dmesg: 4 路 sensor probe (每路 7 行警告 = 28 行总, 全部 `Unexpected sensor id(000000)` 因为镜头物理未接)
- v4l-subdev: 17 个, 见 **7 个 rockchip-mipi-csi2 + 4 个 dphy (0/2/4/5, 缺 1/3)**
- i2c adapter: 0/1/2/3/4/6/7/9 (缺 5/8)
- /dev/video*: 11 个 (足够 6 路)

### 🔍 关键新发现 (2026-06-30): DTS sensor 节点命名是 `gc5035` 不是 `gc4683`

跑 `grep -rl galaxycore /sys/firmware/devicetree/base/` 找到 **6 个节点, 全部 `gc5035@31`, 0 个 `gc4683`**:

```
/sys/firmware/devicetree/base/i2c@feaa0000/gc5035@31/compatible
/sys/firmware/devicetree/base/i2c@fec80000/gc5035_3@31/compatible
/sys/firmware/devicetree/base/i2c@feab0000/gc5035_1@31/compatible
/sys/firmware/devicetree/base/i2c@feac0000/gc5035_5@31/compatible
/sys/firmware/devicetree/base/i2c@feac0000/gc5035_4@31/compatible
/sys/firmware/devicetree/base/i2c@fead0000/gc5035_2@31/compatible
```

**dphy 数 = 6** (在 `/sys/bus/platform/devices/`, 不是 `/sys/class/phy/`):
```
$ find /sys/bus/platform/devices -maxdepth 1 -name "*dphy*" | wc -l
6
```

**Sensor 实例数 = 4** (在 `/sys/bus/i2c/devices/`):
```
$ find /sys/bus/i2c/devices -maxdepth 1 -name "*-003[01]" | wc -l
4
```

### ✅ 厂商回复 (2026-07-02): 两点都是 intentional, 不需要修

**1. dovdd/dvdd/avdd-supply "supply not found" warnings 是预期**:
> "6 路 sensor 节点应该都补 dovdd-supply / dvdd-supply / avdd-supply → **这条是camera 3路电压，没用这个节点来控制**"

含义: GC4683 不用 DTS 的 supply 节点控制电压 (sensor 自己管理), 所以 dmesg 4 路都报 `supply dovdd/dvdd/avdd not found, using dummy regulator` 是 **设计如此**, **不需修**.

**2. csi2_dphy0 / csi2_dphy3 status=disabled 是 intentional**:
> "**没用 csi2_dphy0 和 csi2_dphy3**"

含义: 这两个 dphy 在当前板子上确实不接 sensor, DTS 里 disabled 是设计如此, **不需 enable**.

**厂商提供的拓扑图**: [docs/拓扑图.png](拓扑图.png)

```
dcphy 软件通路:
  dcphy0 → mipi0_csi2 → rkcif_mipi_lvds0 → rkcif_mipi_lvds0_sdift → rkisp0_vir0
  dcphy1 → mipi1_csi2 → rkcif_mipi_lvds1 → rkcif_mipi_lvds1_sdift → rkisp0_vir1

dphy0 软件通路 (full mode):
  dphy0 → mipi2_csi2 → rkcif_mipi_lvds2 → rkcif_mipi_lvds2_sdift → rkisp0_vir2

dphy0 split mode:
  dphy0 1/3 lane → mipi2_csi2
  dphy1 2/3 lane → mipi3_csi2

dphy1 软件通路 (full mode):
  dphy3 → mipi4_csi2 → rkcif_mipi_lvds4 → rkcif_mipi_lvds4_sdift → rkisp1_vir1

dphy1 split mode:
  dphy4 1/3 lane → mipi4_csi2
  dphy4 2/3 lane → mipi5_csi2

→ 6 路 mipi_csi2 (mipi0..mipi5) 配齐, 但 dphy 实际只用了 4 个物理口
   (dcphy0/1 + dphy2/4/5, 或 split 模式时 dcphy0/1 + dphy0/1/2/3/4/5 中一部分)
```

### 已修订结论 (基于 sysfs + 厂商确认)

| 项 | 状态 | 含义 |
|---|---|---|
| 6 路 sensor 节点 DTS | ✅ 配齐 | 6 个 galaxycore 节点, 6 个 i2c 控制器, 符合厂商规格 |
| 6 路 mipi-csi2 receiver | ✅ 配齐 | mipi0..mipi5 全在 |
| dovdd/dvdd/avdd warnings | ✅ 预期 | sensor 不用 supply 节点控制, 不需修 |
| dphy0/dphy3 status=disabled | ✅ 预期 | 厂商确认这 2 个 dphy 没用, 不需 enable |
| dmesg 4 个 sensor probe (i2c-2/3/4/6) | ✅ 预期 | DT 6 节点, sysfs 看到 4 个, 因为部分节点驱动未匹配 (待镜头到位后 chip id 验证) |
| 驱动 (GC4683 v00.01.01) | ✅ ready | built-in, 自动 probe |
| 物理 MIPI 端口 | ✅ 6 路 | 厂商规格, 拓扑图证实 |

---

## 0x07 DTS 实测完整结论 (2026-06-30)

### 完整 DTS 节点探查结果 (sysfs, 覆盖 6 个 sensor 节点)

```
i2c@feaa0000/gc5035@31:    compatible=galaxycore,gc4683 (✓)
i2c@feab0000/gc5035_1@31:  compatible=galaxycore,gc4683 (✓)
i2c@feac0000/gc5035_4@31:  compatible=galaxycore,gc4683 (✓)
i2c@feac0000/gc5035_5@31:  compatible=galaxycore,gc5035 ⚠️ (型号错!)
i2c@fead0000/gc5035_2@31:  compatible=galaxycore,gc4683 (✓)
i2c@fec80000/gc5035_3@31:  compatible=galaxycore,gc4683 (✓)
```

### i2c 控制器 status (9 个)

```
i2c@fd880000: status=okay     (Linux i2c-7)
i2c@fea90000: status=okay     (Linux i2c-1)
i2c@feaa0000: status=okay     (Linux i2c-0)
i2c@feab0000: status=okay     (Linux i2c-2)
i2c@feac0000: status=okay     (Linux i2c-3, 挂 2 个 sensor)
i2c@fead0000: status=okay     (Linux i2c-4)
i2c@fec80000: status=okay     (Linux i2c-6)
i2c@fec90000: status=okay     (Linux i2c-9)
i2c@feca0000: status=disabled ⚠️ (无 Linux adapter)
```

### 总结

**真实状态**: **5 路 GC4683 + 1 路 GC5035 + 1 个 i2c 控制器 disabled**

不是干净的 6 路 GC4683. 厂商 DTS 2 处错误需修:

1. `i2c@feac0000/gc5035_5@31`: `compatible` 应从 `galaxycore,gc5035` 改为 `galaxycore,gc4683`
2. `i2c@feca0000`: `status` 应从 `disabled` 改为 `okay`, 并补一个 GC4683 sensor 节点

**dmesg 显示 4 路 probe** 的原因: gc4683 driver 实际只跟 5 个兼容节点中的 4 个绑定 (可能因 i2c 总线竞争 / 加载顺序), GC5035 那一路被 gc5035 driver 抢走 (`gc5035 4-0031-1: Unexpected sensor id` 那行对应 gc5035 driver 在某个总线上 probe). 实际 usable sensor 实例 = **4 (gc4683) + 1 (gc5035) = 5**, 不是 6.

### 当前可推进方案

| 方案 | 优点 | 缺点 |
|---|---|---|
| A. 等厂商修 DTS, 跑 6 路 | 干净, 不妥协 | 等 1-2 周 |
| B. 当前跑 5 路 (4 gc4683 + 1 gc5035) | 不等, 立可跑 | 后期还要加回第 6 路, gc5035 sensor 模型兼容性问题 |
| C. 当前跑 4 路 (只 GC4683), 后期加 2 路 | 干净, 不混型号 | 等厂商或镜头到位后再扩 |

**推荐**: **C** (现在跑 4 路 GC4683, 厂商 DTS 修好 + 镜头到位后扩到 6 路). 阶段 2 的 yaml 加 cam4/cam5 暂时预留, 但实际只跑 4 路; 厂商补完 + 镜头到位后改 5/6 路.

### 已废弃的判断 (不要被前面的"4 路"误导)

- ❌ "dmesg 4 行 = 4 路 sensor" → 不准确, DTS 实际配了 5+1 = 6 个 sensor 节点, dmesg 4 行只是驱动绑定数
- ❌ "厂商 6 路是虚标" → 正确, 厂商确实配了 6 个 sensor 节点 + 6 个 dphy, 只是 1 个型号错 + 1 个控制器 disabled

### 结论一句话

**板上物理硬件支持 6 路 MIPI** (6 个 dphy, 9 个 i2c 控制器), **DTS 配了 5+1 = 6 个 sensor 节点但 1 个型号错**, **驱动只成功绑定 4 个**. **当前跑项目实际可用 4 路 GC4683**, 阶段 4 时镜头到位 + 厂商 DTS 修好后扩到 6 路.

### 0x07.1 厂商 DTS 修补工单 (待转发, 2026-06-30, 已根据 i2c 冲突发现修订)

> **状态**: 工单已修订, 等用户转发给 rocktech 厂商
>
> **⚠️ 重要修订 (2026-06-30)**: 实测发现 `i2c@feac0000` 同时挂了 `gc5035_4@31` 和 `gc5035_5@31`, 两个 sensor 在同一 I2C 总线同一地址 (0x31), 物理上不可能 (无 I2C mux). 厂商 DTS 的真实错误可能是: **第 6 个 sensor (gc5035_5) 错放到了 feac0000, 应放到 feca0000**, 加上 compatible 写错成 gc5035. 修法见工单正文.

**主题**: `[DTS 修补] 6 路 GC4683 MIPI 适配 — 1 处节点错位 + 1 处控制器 disabled`

**正文**:

```
项目: 6 路 GC4683 MIPI 实时视频拼接 (2×3 布局, 2K 2560×1440 @ 30fps)
板端: rocktech 主板, kernel 6.1.75, Ubuntu 22.04
当前: GC4683 驱动 v00.01.01 已内置, 我们这边工程已 ready, 现在等你方 DTS 修补

[实测发现]
2026-06-30 我们在板端跑 sysfs 探测, 发现 2 处 DTS 错误:

#1. i2c@feac0000 同时挂了 2 个 sensor 节点 (gc5035_4@31 + gc5035_5@31),
   都是 reg=<0x31>, 同一 I2C 总线同一地址, 物理上不可能 (我们没看到 I2C mux 节点).
   推测: gc5035_5 错放到了 feac0000, 应在 i2c@feca0000 (那个被禁用的控制器) 下.

#2. i2c@feca0000 控制器 status="disabled", 没挂 sensor 节点, 应启用并补 GC4683 sensor.

[需要修的]

#1. 把 gc5035_5@31 从 i2c@feac0000 下删除 (移到 i2c@feca0000 下):
    - 删除 i2c@feac0000/gc5035_5@31 节点
    - compatible 同时改对: galaxycore,gc5035 → galaxycore,gc4683
    - reg: <0x31>

#2. i2c@feca0000: 把 status 从 "disabled" 改为 "okay", 并添加 sensor 节点:
    i2c@feca0000 {
        status = "okay";
        gc4683@31 {
            compatible = "galaxycore,gc4683";
            reg = <0x31>;
            /* clk/gpio/regulator/pinctrl 等属性参考 i2c@feaa0000/gc5035@31 节点 */
        };
    };

[修后预期分布 (6 个 sensor 在 6 个不同 i2c 控制器上)]
- i2c@feaa0000/gc5035@31 (gc4683)
- i2c@feab0000/gc5035_1@31 (gc4683)
- i2c@feac0000/gc5035_4@31 (gc4683, 移除 gc5035_5)
- i2c@fead0000/gc5035_2@31 (gc4683)
- i2c@fec80000/gc5035_3@31 (gc4683)
- i2c@feca0000/gc4683@31 (gc4683, 新加)

[验证方法]
修补后请在板端跑以下命令确认:

1. 数 sensor 节点 (期望 6 个 gc4683@31):
   for s in /sys/firmware/devicetree/base/i2c*; do
       for n in $s/gc4683*; do
           [ -d "$n" ] && echo "$(basename $s) / $(basename $n)"
       done
   done
   期望: 看到 6 行, 每个在不同 i2c 控制器下

2. 数 dphy (期望 6):
   find /sys/bus/platform/devices -maxdepth 1 -name "*dphy*" | wc -l

3. 镜头物理到位后, dmesg 应有 6 个 gc4683 probe 行:
   dmesg | grep "gc4683 .*driver version" | wc -l

[期望交付]
- DTS patch (1 个 dtsi 文件改动即可)
- 编译出新 boot.img 或单独 DTB
- 烧写后告诉我们, 我们这边立即跑端到端验证
```



---

## 0x08 CameraPage 浏览器管理平台接入 (2026-07-03, 阶段 1 完成)

**目标**: 在 PC 浏览器中通过 CameraPage UI (4 模块) 监控板端 stitch 状态.

**架构**:
- 单进程 `./image-stitching` (板端跑, 端口 8080)
- C++ 嵌入式 cpp-httplib HTTP server (header-only, vendor 进 `third_party/cpp-httplib/`)
- 独立 `status_writer` 线程 (500ms 写 `/tmp/stitch_status.json`, 模拟数据 + 真数据接口)

**详细设计**:  [docs/CAMERA_PAGE_INTEGRATION.md](docs/CAMERA_PAGE_INTEGRATION.md) (v2.4)
**部署指南**:  [docs/CAMERAPAGE_TEST_DEPLOY.md](docs/CAMERAPAGE_TEST_DEPLOY.md) (v2.4 阶段 1)

### 完成项 (2026-07-03)

| 项 | 文件 | 状态 |
|---|---|---|
| 状态结构 + 桥接接口 (模拟数据) | [include/status_writer.h](../include/status_writer.h), [src/status_writer.cc](../src/status_writer.cc) | ✅ 跑通 |
| HTTP server (cpp-httplib 5 API 端点) | [include/http_server.h](../include/http_server.h), [src/http_server.cc](../src/http_server.cc) | ✅ 跑通 |
| CameraPage 静态文件 serve (index.html/css/js) | 同上 | ✅ 跑通 |
| CMakeLists.txt 集成 (vendor 检测) | [CMakeLists.txt](../CMakeLists.txt) | ✅ |
| cpp-httplib v0.49.0 vendor | [third_party/cpp-httplib/httplib.h](../third_party/cpp-httplib/httplib.h) | ✅ (gh-proxy.com 拉取) |
| main() 串接 init() + http_server::start() | [src/app.cc](../src/app.cc) | ✅ |
| 板端 build 跑通 (PID 84321) | — | ✅ |
| 板端 API 验证 (5 端点) | — | ✅ |

### 5 个 API 端点 (板端 curl 验证通过)

| 端点 | 作用 | 数据源 |
|---|---|---|
| `GET /api/health` | 健康检查 | C++ 直接返回 |
| `GET /api/status` | 实时状态 (FPS, frame_idx, 6 cams) | `/tmp/stitch_status.json` (worker 线程写) |
| `GET /api/devices` | 6 路 cam 信息 | `params/camera_sources.yaml` 解析 |
| `GET /api/config` | roi_tuning.yaml 全文 | 文件直接读 |
| `GET /api/network` | 板端网络 (IP/MAC/接口) | `/sys/class/net/*` + `ip` 命令 |
| `POST /api/roi` | 改 ROI offset (原子写 yaml) | 文件读 + tmp + rename |

### 未做 (阶段 2-5 暂停)

| 阶段 | 原因 |
|---|---|
| 2. H.264 编码 + fMP4 + WebSocket + 浏览器 MSE | 暂停 (vpu 缺固件, 软编路径可选但未实施) |
| 3. CameraPage 前端 fetch API 改造 (4 模块接真数据) | 等阶段 2 视频流一起做 |
| 4. 真数据接入 (status_writer 从模拟切真) | 等厂商修 vpu/kmpp/ffmpeg 三方不兼容 |
| 5. 多客户端/RTSP/鉴权/yaml 热更新 | 远期 |

### 板上硬件约束 (2026-07-03 实测)

| 项 | 状态 | 影响 |
|---|---|---|
| kernel 6.1.75 + Ubuntu 22.04 | ✅ OK |  |
| GC4683 驱动 v00.01.01 (built-in) | ✅ OK | 6 路 sensor 节点齐 (5 gc4683 + 1 gc5035) |
| ffmpeg 6.1 + h264_rkmpp (可识别) | ⚠️ **VPU 缺固件, 编码失败** | 阶段 2 必须改用 libx264 软编 |
| libx264 v0.163 (apt 装) | ✅ OK | 软编兜底, 1.5 核 CPU, 20Mbps |
| 板载 DSI 屏 (card0-DSI-1) | disconnected | 无物理显示, 走 HTTP CameraPage |
| 6 路 mpp_dec 线程在跑 | ✅ 等帧 (等不到因为 vpu 缺) |  |

### 部署关键命令 (板端)

```bash
# 拉 cpp-httplib (板端走 gh-proxy.com, 国内可达)
cd ~/Projects/new-4k-stitch
mkdir -p third_party/cpp-httplib
curl -fSL --max-time 60 'https://gh-proxy.com/https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h' \
  -o third_party/cpp-httplib/httplib.h

# 编译
mkdir -p build && cd build
cmake ..
make -j$(nproc)

# 启动 (ENABLE_VISUAL_TUNING=0 关键, 板端无 X server)
ENABLE_VISUAL_TUNING=0 nohup ./image-stitching > /tmp/stitch.log 2>&1 &

# 浏览器打开
# http://192.168.137.100:8080/
```
