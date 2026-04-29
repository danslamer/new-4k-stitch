## 0x00 Intro

> This project is a simple implementation of opencv for the following papers.
>
> Du, Chengyao, et al. (2020). GPU based parallel optimization for real time panoramic video stitching. Pattern
> Recognition Letters, 133, 62-69.

Fast panorama stitching method using UMat.

Speed of 4 cameras at 4k resolution is greater than 200fps in 1080ti.

This project does not provide a dataset so it cannot be used out of the box.

一个使用 OpenCV 进行快速全景视频拼接的方法。通过巧妙的流并行策略，在 rk3576 上可以对4路 4k 视频进行超过 35fps 的图像拼接。

## 0x01 Quick Start

```
mkdir build && cd build
cmake ..
make
./image-stitching
```

## 0x02 Example

> About these procedure below (chinese) http://s1nh.com/post/image-stitching-post-process/ .

| 00.mp4                    | 01.mp4                    | 02.mp4                    | 03.mp4                    |
|---------------------------|---------------------------|---------------------------|---------------------------|
| ![](assets/origin-00.png) | ![](assets/origin-01.png) | ![](assets/origin-02.png) | ![](assets/origin-03.png) |

stitching  
![](assets/01.origin-stitching.png)

exposure-mask  
![](assets/02.exposure-mask.png)

exposure-mask-refine  
![](assets/03.exposure-mask-refine.png)

apply-mask
![](assets/04.apply-mask.png)

final-panorama
![](assets/05.final-stitching.png)

## 0x03 修改时间线

- **初始版本**: 基于OpenCV UMat实现快速全景拼接，支持4k分辨率4路摄像头，1080ti上超过200fps。
- **2026-03-24**: 调整相机内外参数（KMat/RMat），避免畸变。添加运行时微调环境变量。改为4K分辨率
- **2026-03-26**: 发现FPS从50+降至个位数，主因是warp从并行改为串行执行。优化：顺序提交warp，去掉clone/copyTo中间副本，减少GPU抖动。
- **2026-03-26**: 分析GPU瓶颈，发现OpenCL队列同步等待时间大（remap_finish 60-150ms）。建议减少kernel次数。
- **2026-03-26**: 对比原版，发现当前版多额外同步点和日志输出，导致性能下降。原版80+FPS可能为提交速度而非真实吞吐。
- **2026-03-26**: 识别后处理/落盘为瓶颈，关闭保存时FPS提升但暴露解码等待。
- **2026-04-01**: 集成RK硬件解码（rkmpp + RGA），但性能收益被NV12->BGR->UMat转换抵消。目标：实现DMA-BUF零拷贝。
- **2026-04-03**: 在初始化时使用opencv检测ROI感兴趣区域，保存坐标，后续硬件帧复用坐标剪裁拼接。此时没有remap_finish，RGA不支持

## 0x04 配置与环境

### 库版本要求
opencv>=4.5

### 仓库相关
- 旧仓库: `/userdata/Projects/yzy/gpu-based-image-stitching-dataset-new/gpu-based-image-stitching/`
- 新仓库: `/userdata/Projects/yzy/new-4k-stitch`
- OpenCV 4.9: `/userdata/Projects/yzy/opencv/opencv-4.9.0/build`

### 虚拟环境
cd /userdata/Projects  
source rknn-env/bin/activate  
cd /userdata/Projects/rknn-toolkit2/rknn-toolkit-lite2/examples/Detect2/code/UI  
./run_ui.sh 或 python main.py

### Git 操作命令
- 克隆: `git clone <仓库URL>`
- 状态: `git status`
- 添加: `git add .`
- 提交: `git commit -m "说明"`
- 推送: `git push` (首次: `git push -u origin <分支>`)
- 拉取: `git pull` (分支: `git pull origin dataset`)
- 分支: 查看 `git branch`, 新建 `git checkout -b <分支>`
- 回退: `git reset --hard <commit>` (谨慎使用)

### 环境变量
- **保存控制**:
  - `SAVE_STITCH_FRAMES`: 保存最终结果 (默认true)
  - `SAVE_DIAGNOSTIC_FRAMES`: 保存诊断图 (默认true)
  - `SAVE_FRAME_INTERVAL`: 保存间隔 (默认30)
  - `DIAGNOSTIC_FRAME_LIMIT`: 诊断图上限 (默认3)
- **输入源**: `INPUT_SOURCE_MODE`: dataset/camera (默认dataset)
- **KMat调试**:
  - 全局: `STITCH_K_FOCAL_SCALE`, `STITCH_K_FX_SCALE`, `STITCH_K_FY_SCALE`, `STITCH_K_CX_OFFSET`, `STITCH_K_CY_OFFSET`
  - 单相机: `STITCH_K_FOCAL_SCALE_CAM_0` 等
  - 调参指南: 整体鼓瘪调FOCAL_SCALE, 接缝不顺调CY_OFFSET, 左右不接调CX_OFFSET

## 0x05 硬件解码
宏 `RK_HARDWARE_DECODING` 控制解码方式。


### FFmpeg 编译 (ARM/ARM64)
参考: https://github.com/nyanmisaka/ffmpeg-rockchip/wiki/Compilation

- 构建MPP, RGA, FFmpeg
- 测试: `./ffmpeg -decoders | grep rkmpp`

### DMA-BUF机制
DMA-BUF是一种用于内存管理机制，它允许设备访问内存，而无需复制数据。
数据始终在同一块物理内存中，零拷贝。各个设备操作完毕后，需要通过一套同步机制告知其他设备“我读完了”或“我写完了”，以避免数据竞争。
参考：https://zhuanlan.zhihu.com/p/1942149087869800464

### 测试解码器
- 无AFBC: `./ffmpeg -hwaccel rkmpp -hwaccel_output_format drm_prime -i <video> -f null -`
- 有AFBC: `./ffmpeg -hwaccel rkmpp -hwaccel_output_format drm_prime -afbc 1 -i <video> -f null -`

确认FFmpeg版本: `ldd ./image-stitching | grep avcodec` (应为so.60)

### GPU监控
sudo cat /sys/class/devfreq/27800000.gpu/load

### EGL环境检查
RK3576 无桌面环境下，建议按下面顺序检查 EGL 适配情况：

1. 确认当前会话没有被桌面显示服务干扰：
  ```bash
  echo "$DISPLAY"
  echo "$WAYLAND_DISPLAY"
  echo "$XDG_RUNTIME_DIR"
  ```
  如果是纯终端环境且变量被错误设置，可以先执行 `unset DISPLAY WAYLAND_DISPLAY` 再启动程序。

2. 确认 DRM 设备节点存在：
  ```bash
  ls -l /dev/dri
  ```
  重点看是否存在 `card0` 和 `renderD128` 这类节点。

3. 确认系统已经安装 EGL/GLES 运行库：
  ```bash
  ldconfig -p | grep -E 'libEGL|libGLESv2'
  ```

4. 直接查看 EGL 能力：
  ```bash
  eglinfo --display surfaceless
  ```
  如果系统没有 `eglinfo`，先安装 Mesa 工具包或使用本程序启动日志代替。
  apt install -y mesa-utils mesa-utils-extra libegl1-mesa libegl1-mesa-dev libgles2-mesa libgles2-mesa-dev libgbm1 libdrm2 libdrm-dev

5. 启动程序后检查 `[RkGlesWarper]` 日志，重点看这些信息：
  - `EGL_VENDOR` / `EGL_VERSION` / `EGL_CLIENT_APIS`
  - `EGL_EXTENSIONS` 里是否包含 `EGL_KHR_image_base`、`EGL_EXT_image_dma_buf_import`、`EGL_KHR_gl_texture_2D_image`
  - 是否成功走到 `using EGL_PLATFORM_SURFACELESS_MESA display path` 或至少 `eglInitialize succeeded`
  - `GL_RENDERER` 是否指向 RK3576 对应的 GPU 驱动

6. 适配判定标准：
  - `eglInitialize` 成功
  - `eglCreateContext` 成功
  - `eglMakeCurrent` 成功
  - EGLImage 相关入口点可用
  - DMA-BUF 导入扩展存在

如果以上任一项不满足，优先从系统驱动、`/dev/dri` 节点权限和 EGL 扩展缺失这三类问题排查。

### CPU监控
htop

### 性能优化路线
- 当前瓶颈: 解码/取帧 (fetch ~121ms), 拼接 ~16ms
- 目标: rkmpp -> DMA-BUF -> RGA/GPU零拷贝
- 优化: 硬件解码/DMA-BUF/RGA/GPU零拷贝
- 低风险: 去UMat用cv::Mat全链路
- 高性能: 重构为硬件buffer直接拼接

## 0x06 SSH连接
1. 设置IP: Windows/Linux设置静态IP (e.g. 192.168.1.100)
在终端执行：
bash
**sudo ifconfig end1 192.168.1.10 netmask 255.255.255.0 up**
（将 eth0 替换为实际网卡名，如 enp0s 等）
2. 测试: `ping 192.168.1.10`
3. 连接: `ssh root@192.168.1.10` 或 PuTTY


## 0x07 输出统计
程序输出两类统计:
- **初始化**: input_init, param_generation, total等
- **每帧**: clear, load, warp_cpu_dispatch, total, fps等

## 0x08 运行逻辑

### 程序架构
- **主入口**: `main()` 创建App实例，调用`run_stitching()`
- **App类**: 封装整个拼接应用，包含初始化和运行时逻辑

### 初始化阶段 (App构造函数)
1. **日志初始化**: Logger::GetInstance().Initialize()
2. **视频捕获初始化**: sensorDataInterface_.InitVideoCapture(num_img_)
3. **引导帧捕获**: 获取第一帧硬件帧，转换为BGR格式
4. **重叠检测**: 
   - `EstimateOverlaps()` 调用 `EstimatePairOverlap()` 检测相邻相机重叠
   - 使用ORB特征匹配 + 模板匹配相结合的方法
5. **ROI构建**: `BuildCameraRois()` 基于重叠估计构建相机感兴趣区域
6. **布局构建**: `BuildStitchLayout()` 计算拼接任务和全景图尺寸
7. **拼接器设置**: 配置ImageStitcher参数和布局
8. **输出缓冲区分配**: drm_alloc_nv12() 分配DRM DMA-BUF

### 运行时循环 (run_stitching())
无限循环执行以下步骤:
1. **帧获取**: sensorDataInterface_.get_image_vector() 获取所有相机帧
2. **输出清空**: image_stitcher_.ClearOutput() 清空输出缓冲区
3. **并行拼接**: 对每个相机调用 image_stitcher_.WarpImages() 进行变形拼接
4. **性能统计**: 计算解码时间、拼接时间、FPS等指标
5. **结果保存**: 按间隔保存拼接结果图像 (可选)

### 关键技术点
- **零拷贝**: 使用DRM_PRIME硬件帧，避免CPU内存拷贝
- **硬件加速**: RK硬件解码 + RGA图像处理
- **并行优化**: 多相机并行处理，GPU加速变形
- **自适应ROI**: 运行时检测相机重叠区域，动态裁剪

### 核心模块依赖关系

#### 主程序模块 (app.cc/app.h)
- **功能**: 应用入口和主循环控制
- **依赖**: 
  - SensorDataInterface: 视频数据获取
  - ImageStitcher: 图像拼接处理
  - Logger: 日志记录
  - DrmAllocator: DRM缓冲区管理

#### 传感器数据接口 (sensor_data_interface.cc/h)
- **功能**: 多路视频流捕获和解码
- **依赖**: 
  - NV12Frame: 硬件帧结构定义
  - FFmpeg: 硬件解码 (rkmpp)
- **输出**: NV12硬件帧队列

#### 图像拼接器 (image_stitcher.cc/h)
- **功能**: GPU加速的图像变形和拼接
- **依赖**: 
  - DrmAllocator: 临时缓冲区分配
  - NV12Frame: 帧数据结构
- **输入**: 多路NV12帧 + 拼接任务配置
- **输出**: 拼接后的NV12全景图

#### DRM分配器 (drm_allocator.cc/h)
- **功能**: DRM DMA-BUF内存管理
- **依赖**: Linux DRM/KMS API
- **提供**: 零拷贝缓冲区分配/释放

#### 日志器 (logger.cc/h)
- **功能**: 统一日志输出和性能统计
- **依赖**: 标准输出流
- **功能**: 帧级日志、图像保存、FPS计算

#### NV12帧定义 (nv12_frame.h)
- **功能**: 硬件帧数据结构定义
- **依赖**: 无
- **用途**: 在各模块间传递帧数据

### 数据流向
```
         -> FFmpeg(rkmpp)硬件解码 -> 零拷贝(DMA-BUF)流转       -> RGA(裁剪/变形) + OpenCL(核心羽化计算)
视频源 -> SensorDataInterface   -> NV12Frame Queue         -> ImageStitcher                    -> DRM缓冲区 -> 保存/显示
                                -> Logger (运行时性能统计)    (含FD Hash Cache机制规避IOMMU建表开销)
         -> 抽帧转BGR用于初始化特征检测                        -> 常态化复用初始化时定型的ROI几何边界参数
```

### 配置文件
- **相机标定**: params/camchain_*.yaml (内参、外参、畸变系数)
- **环境变量**: 运行时调参 (裁剪、偏移、保存控制)


### 左右拼接瑕疵
1. 运行日志深入分析
通过你的日志 log_20260409_191630.log 可以发现几个非常关键的数据：

上下相邻图片的检测得分非常高（正常）：
v02{overlap_y=774 shift_x=-7 score=0.584}
v13{overlap_y=826 shift_x=-17 score=0.469}
左右相邻图片的检测得分极低（异常）：
h01{overlap=1104 shift_y=53 score=0.105}
h23{overlap=1126 shift_y=-45 score=0.082}

原因剖析：
在 app.cc 的 EstimatePairOverlap 函数中，对水平方向用来做特征匹配的搜索带宽 band_w 和 search_w 此前存在一个硬编码的边界约束：

这原本是为了在小分辨率下加速匹配，强制最多只在图像边缘向内搜索 1200 像素。
由于当前升级到了 4K视频（分辨率 3840x2160） 作为输入，左右照片真正的重叠部分跨度极可能超过了 1200 像素（例如实际重叠约为 1400~1600 像素）。
特征匹配算法在受限的 1200 像素局域内找不到相同的参考物，只好返回一个错误极低的置信度（~0.104），推导出了一个虚假的偏小重叠值（1104 像素）。
最终，在裁切（Cut）拼接缝时，由于计算出的重叠距离过短，算法没有削去足够的边缘，从而将本该裁掉的画面一并留下来了，造成了你在最终图像上看到的“画面重复”瑕疵。


解除 4K 宽度的硬编码截断约束：将原先强制上限 1200 提升至 4K 屏幕的理论中心线 1920 像素。
同时将模板匹配（Template Matching）的参数 search_w 亦提升至 1920。

如果有微小偏移，可以适当调节 CameraTuning 中的 offset_x 即可。


由于 NV12 到 RGBA 的转换极其消耗带宽，不建议使用多次 RGA 操作。推荐方案是使用 Mali GPU 和 OpenCL，通过 ARM 的扩展 cl_arm_import_memory 零拷贝直接导入 NV12 的 DMA-buf（文件描述符 fd）。在 OpenCL Kernel 中直接对 Y 和交织的 UV 分量进行数学加权混合，直接输出到目标 NV12 缓冲区，从而实现高性能且无额外内存拷贝的边缘羽化。

RGA 方案在裁剪、旋转的时候保留，而仅仅对于重叠的一小条交界区域使用 OpenCL 处理。

不使用 OpenGL ES 和 FBO：在标准的移动端 OpenGL ES 环境下，渲染目标往往不支持 NV12 (需要罕见的 GL_EXT_YUV_target)，强行通过 FBO 输出 RGBA 还需要一步额外的 RGA 色彩空间逆转换，严重浪费内存带宽。OpenCL 直接支持线性内存操作。


相机的 V4L2 DMA_BUF fd 和输出画布的 fd 往往在程序生命周期内是复用、有限个的池子（典型情况下总计不会超过 15~20 个 FD）。
那么我们可以给程序增加一个轻量级的 Hash Map Cache（缓存字典）：只在第一次时导入它并持久化到内存缓存，后续帧再来直接复用，完全绕过 MMU 导入开销。

这是一个非常好的问题！理解了这个缓存字典的原理，你基本上就掌握了在嵌入式 Linux（如 Rockchip / V4L2 系统）下进行 **GPU 零拷贝（Zero-copy）** 的核心性能密码。

简单来说，加入 `std::unordered_map<int, cl_mem> dma_buf_cache_` 并不是为了缓存“图像画面”本身，而是为了**“缓存 GPU 的内存页表（MMU映射关系）”**。

下面我为你详细拆解它的功能和底层原理：

### 1. 之前的痛点：为什么不加缓存帧率会暴降？
在未加入缓存时，我们的代码每一帧都在做一件极其消耗 CPU 和操作系统内核资源的事情：
* **每一帧开始时 `clImportMemoryARM`**：你把一个硬件内存句柄（DMA-BUF FD）交给了 OpenCL 驱动。Linux 内核必须停下手头的工作，唤醒 **IOMMU** (输入输出内存管理单元)，为这个内存地址动态建立一套 GPU 能看得懂的物理页表（Page Table mapping）。这个过程涉及到系统调用（System Call）和硬件等待。
* **每一帧结束时 `clReleaseMemObject`**：你又告诉驱动把刚刚建好的这套映射表给销毁掉。

这就好比：你是一名外卖员，每天要去同一个高档小区送 30 次外卖。但你每次进门都要**重新登记身份证、拍照、等待保安核实并办理一次性临时通行证（导入耗时），出门时再把通行证还给保安销毁（释放）**。尽管送外卖跑路只要 1 分钟，但办证花掉了你 20 分钟！这就是帧率从 45 掉到 20 的根本原因。

### 2. Hash Map Cache 的原理是什么？
在 Linux 的 V4L2 摄像头驱动或 RKMPP 硬件解码器中，底层的内存其实是**轮转复用（Ring Buffer）**的。
系统在初始化时，通常只会分配有限个（比如 4 个、8 个或 16 个）DMA-BUF 物理内存块。每个内存块有一个固定唯一的身份证号，也就是文件描述符（`fd`）。运行时，画面总是在这几个 `fd` 之间循环填充数据。

**缓存字典的结构：**
* **Key (`int`)**: 内存块的身份证号 `fd`。
* **Value (`cl_mem`)**: 办好的“GPU 长期通行证”（包含了已经建好的 MMU 映射）。

**它的工作流程如下：**
1. **拦截请求（Cache Miss）**：当第一帧（或前几帧）的 `fd` 到来时，字典查不到这个 `fd`。于是程序老老实实地调用 `clImportMemoryARM` 让内核去建表（办证），得到 `cl_mem` 后，不仅用来计算，还**顺手把它存进了字典里**：`dma_buf_cache_[fd] = mem`。
2. **瞬间起飞（Cache Hit）**：当后续数百数千帧到来时，因为底层的 `fd` 是那几个固定值轮转的，字典瞬间发现：“这个 `fd` 之前已经映射过了！”于是直接抛出缓存的 `cl_mem`。
3. **零内核开销**：既然已经有了 `cl_mem`，无需通过内核做任何页表重建，Mali GPU 直接通过之前的映射“瞬移”访问物理内存，执行光速的 Alpha 羽化计算。

在这个过程中，**你的程序从“每秒办 45 次证”，变成了“程序生命周期内只办 10 几张长期通行证，以后无限次直接刷卡放行”**。

### 3. 生命周期管理
既然我们变成了“长期办证”，那什么时候销毁呢？
我们在 `ImageStitcher::CleanupOpenCL()` 函数（即程序由于 Ctrl+C 退出的最后阶段）中，遍历了整个字典 `dma_buf_cache_`，把里面囤积的所有 `cl_mem` 一口气释放掉，将物理内存干干净净地归还给操作系统。

### 总结
这就是 `Hash Map Cache` 对嵌入式 GPU 计算至关重要的原因。它完美利用了多媒体框架 **池化分配（Pool Allocation）的局部性特征**，将最沉重的 Linux 内存导入/导出开销化解为了 $\mathcal{O}(1)$ 复杂度的普通哈希查表，从而让 Mali GPU 发挥出了真实的纯算力性能！计 算的真实性能得以 100% 释放。

在视频处理和底层硬件驱动（如 Linux V4L2、RKMPP 解码、DRM 控制器）中，“池化分配（Pool Allocation）的局部性特征”主要指内存资源在申请和使用时表现出的**高度重复性**和**集合有界性**。

具体表现在以下两个维度：

### 1. 标识块的有限性（集合有界）
在处理 4K 等高分辨率图传时，频繁向操作系统申请和释放几十兆的连续物理大内存会引发极高的碎片化和内核开销。因此，底层多媒体框架会在程序初始化阶段**一次性划拨**一个固定数量的内存块集合（即“内存池”，通常为 4 到 16 个 Buffer）。
这意味着在整个流水线的生命周期中，参与流转的物理内存底层句柄（DMA-BUF FD）是一个极小且数量确定的集合（例如全局仅存在 FD: 15, 16, 17, 18）。

### 2. 访问的时间局部性（轮转复用）
在视频帧连续不断的处理过程中，硬件管道采用**环形队列（Ring Buffer）**机制对内存池内的区块进行轮换写入。
第一帧解码至 FD 15，第二帧解码至 FD 16。当内存池用完一圈，最新的帧会覆盖最早的 FD。整体内存的访问面呈现出严格的周期性与高频复用规律：`15 -> 16 -> 17 -> 18 -> 15 -> 16...`

### 对缓存（Cache）架构的决定性意义
利用这种局部性规律，缓存字典（Hash Map Cache）的效率能够发挥到极致：
* **极短的冷启动**：仅在程序刚启动的前几个回合，缓存引擎会遇到未命中（Cache Miss），被迫向 Linux 内核请求建立真实的 MMU 和 OpenCL 上下文映射。
* **100% 的稳态命中**：一旦内存池内所有的（十几个）FD 均已被收录进 Hash Map，流水线随后处理的几万、几百万帧都将处于完全命中状态（Cache Hit），直接复用已有的映射页表。
* **复杂度坍缩**：原本伴随每帧画面的系统陷入（Context Switch）和页表穿梭耗时，从常数级别的高昂单次开销，坍缩为时间复杂度均摊 $\mathcal{O}(1)$ 的纯内存查表操作，从而让 GPU 算力得到无阻力地全速吞吐。


**MMU（Memory Management Unit，内存管理单元）** 是计算机和嵌入式芯片（如你的 Rockchip SoC）中极为核心的硬件组件。要理解它，你可以把它想象成操作系统和物理内存之间的一位**“超级翻译官”与“安保队长”**。

结合你在 4K 视频拼接项目中的遇到的性能问题，我们可以从以下几个层面来透彻理解 MMU 及其变种（IOMMU）在其中的作用：

### 1. MMU 的核心使命：虚实地址转换
在现代操作系统（如 Linux）中，你写的 C++ 代码中打印出的所有指针地址（比如 `0x7fffc000`），都不是真正的物理内存（RAM 芯片上的确切引脚）地址，而是**虚拟地址（Virtual Address, VA）**。
* **原理**：当 CPU 尝试读取这个虚拟地址时，MMU 硬件会瞬间接管，通过查询内存中的“页表（Page Table）”，将其**翻译**成真实的物理地址（Physical Address, PA），再去拿数据。
* **好处**：这种机制让每个程序都以为自己独占了所有内存，不仅避免了程序之间互相踩踏（安全隔离），还能把物理上支离破碎的内存碎片，在虚拟空间中拼凑成连续的大块内存供程序使用。

### 2. 为什么导入 DMA-BUF 会和 MMU 扯上关系？
在你的视频流里，摄像头通过硬件直接把 4K 画面写入到了某块物理 RAM 中。
* **CPU 有自己的 MMU**，它通过一套页表看到了这块内存。
* **Mali GPU（以及 RGA 等外设）有自己的 MMU**（通常称为 **IOMMU**，Input/Output MMU）。GPU 看不懂 CPU 的虚拟地址，它有自己独立的世界。

当你在代码里调用 `pfn_clImportMemoryARM(..., fd, ...)` 时，你是在告诉 Linux 内核：“嘿，请把这个有着几千万像素的物理内存（由文件描述符 `fd` 代表），共享给 Mali GPU 吧！”

### 3. MMU 映射带来的昂贵代价（性能暴降的元凶）
此时，Linux 内核在底层需要做大量繁重的工作：
1. **锁定物理页（Pin/Get User Pages）**：告诉系统这些物理内存正在被 GPU 借用，绝对不能被交换到磁盘或释放。
2. **建立 IOMMU 页表（Mapping）**：内核需要为 GPU 分配新的页表树，把这几十兆图像所在的每一个 4KB 物理页，逐一填写到 GPU 的地址空间中。
3. **刷新硬件缓存（TLB Flush）**：强迫 GPU 清空旧的地址翻译缓存，加载新表。

这就是为什么你之前**每帧**都调用 Import 和 Release 时，帧率从 45FPS 暴跌到 20FPS 的直接原因。**你每一帧都在强迫 Linux 内核与 IOMMU 硬件重新经历一次分配、填表、锁内存、清缓存的极端重负载流程。**

### 4. 配合池化分配（Hash Map Cache）的终极突围
而在我们加入了 `Hash Map Cache` 后：
* 首次遇到某个 `fd` 时，系统老老实实让 IOMMU 建立好映射表，并返回代表这段虚拟空间入口的 `cl_mem`。
* 之后再次遇到这个 `fd` 时，直接抛出 `cl_mem`。GPU 的 IOMMU 页表**原封不动地保持在那里**。Mali GPU 处理下一帧图像时，硬件电路直接利用现成的页表光速寻址，所谓的 “Zero-Copy（零拷贝）” 此时才真正实现了“零开销调度”。

简而言之，MMU 是掌控地址魔法的硬件枢纽，而我们在代码里所做的跨硬件调度优化，本质上就是**在极力避免去频繁拨动这台庞大机器的开关**。



在 RK3576 上，由于 RGA 只能做仿射变换（平移、缩放、正交旋转），无法进行像柱面、球面或透视投影等复杂的非线性形变（Warp），这就导致全景拼接时的边缘无法完美对齐。
利用 RK3576 强大的 Mali GPU，通过 EGLImageKHR 实现 DMA-BUF（dma_fd）的零拷贝导入，并使用 OpenGL ES 的 Fragment Shader 进行形变渲染，是当前解决此问题的最优工业级方案。

主流程已经改成：
启动时先尝试 OpenCV 投影参数生成
成功则走 RkGlesWarper -> warped NV12 scratch buffer -> RGA copy to panorama -> OpenCL feather
失败则自动回退到你现在的 ROI + RGA + OpenCL 路线

现在默认链路是：初始化阶段做多帧 ROI 检测，选出最优结果后生成 2x2 拼接布局；运行时则保持原版那种节奏，先清空输出画布，再逐路做 WarpImages，最后执行 BlendSeams 羽化。对应改动在 app.cc:760 和 app.cc:980。这样一来，零拷贝、RGA 裁剪/搬运、OpenCL 羽化都还在，但不再让投影 warp 分支去主导布局生成。

主流程重新回到“先做多帧 ROI 检测并生成布局，再在布局固定后尝试启用 GLES warp”的顺序，GLES 现在只是原版流程里的可选零拷贝加速器，不再主导布局生成。
ROI 先确定，布局先落定，然后再尝试用 GLES warp 做硬件零拷贝导入和变换；如果 warp 数据无效或初始化失败，就自动回退到纯 ROI + RGA + OpenCL 路线。

目前较好的数据集：t40，
目前不好的数据集：t00,t30，t50

下一步方案：
将ROI手动位移矩阵的调整操作可视化，使用可视化界面实时流式输出当前帧的拼接结果，并用一定的标记区分四路输入的区域（该标记可通过调整变量取消，默认处于调试阶段下显示标记），并使用选择某一路+点击带有方向的步进按钮的方式手动调整ROI位移矩阵，并保存为配置文件，下次启动时直接读取配置文件，并使用配置文件进行拼接。步进按钮需要有多个步距选择，可视化界面下要显示步距选择，并显示当前ROI位移矩阵的数值。


为双模式状态机（流式输出+按D进入调试+按Q退出）、改造main()读取3个环境变量


opencode --session ses_2663db4c7ffeHs8hhTpL3IZrub
sk-b9d876beac5d42e1afdf92fbc9f2001e

查看所有会话：opencode session list
继续某个会话：opencode --session ses_24bd59cfdffey30xKPIf68AG39
删除无用会话）：opencode session delete <ID>

使用SDL2库实现了可视化

按键映射
按键	功能
↑/↓/←/→ 或 W/A/S/D	ROI 步进
Tab	切换摄像头
1/5/0/P	步距 1/5/10/50-+
F	羽化开关
+/-	羽化宽度 ±10
B	保存开关
L/K	保存间隔 ±10
E	保存配置到 YAML
M	ROI 标记开关
Q/Esc	退出调试模式
Enter/D	进入调试模式

功能行为
操作	行为
进入调试模式 (Enter)	保存当前帧，锁定帧索引，暂停获取新帧
ROI 偏移调整 (↑↓←→)	重新拼接保存的帧，实时显示调整效果
羽化参数调整 (F/+/-)	重新布局 + 重拼接
切换摄像头 (Tab)	仅切换选中状态，不影响帧
保存配置 (E)	写入 YAML 文件
退出调试模式 (Q/Esc)	清除保存帧，恢复实时拼接流程