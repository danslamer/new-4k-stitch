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

### 性能优化路线
- 当前瓶颈: 解码/取帧 (fetch ~121ms), 拼接 ~16ms
- 目标: rkmpp -> DMA-BUF -> RGA/GPU零拷贝
- 优化: 硬件解码/DMA-BUF/RGA/GPU零拷贝
- 低风险: 去UMat用cv::Mat全链路
- 高性能: 重构为硬件buffer直接拼接

## 0x06 SSH连接
1. 设置IP: Windows/Linux设置静态IP (e.g. 192.168.0.100)
在终端执行：
bash
**sudo ifconfig end1 192.168.1.100 netmask 255.255.255.0 up**
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
         -> rkmppp硬件解码器    -> 硬件帧队列    ->RGA库进行拼接
视频源 -> SensorDataInterface -> NV12Frame[] -> ImageStitcher -> DRM缓冲区 -> 保存/显示
                              -> Logger (统计信息)
         -> 下载软件帧用于初始化                -> 复用软件帧所获取的ROI区域
```

### 配置文件
- **相机标定**: params/camchain_*.yaml (内参、外参、畸变系数)
- **环境变量**: 运行时调参 (裁剪、偏移、保存控制)


