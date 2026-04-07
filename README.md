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

### 测试解码器
- 无AFBC: `./ffmpeg -hwaccel rkmpp -hwaccel_output_format drm_prime -i <video> -f null -`
- 有AFBC: `./ffmpeg -hwaccel rkmpp -hwaccel_output_format drm_prime -afbc 1 -i <video> -f null -`

确认FFmpeg版本: `ldd ./image-stitching | grep avcodec` (应为so.60)

### 性能优化路线
- 当前瓶颈: 解码/取帧 (fetch ~121ms), 拼接 ~16ms
- 目标: rkmpp -> DMA-BUF -> RGA/GPU零拷贝
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