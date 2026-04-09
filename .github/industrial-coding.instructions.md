---
description: "新4K视频拼接工程的工业级编码标准、调试规范、文档要求、RK3576开发板适配指南"
applyTo: "**/*.{cc,c,h}"
---

# 新4K视频拼接工程 - 工业级编码指导

## 工程概况

- **项目名称**：new-4k-stitch (新4K视频拼接)
- **目标硬件**：RK3576开发板
- **主要模块**：图像拼接(image_stitcher)、传感器数据接口(sensor_data_interface)、视频解码(decoder)、参数生成(stitching_param_generater)
- **构建系统**：CMake

## 编码标准

### 文件组织
- 头文件位于 `include/` 和 `assets/` 目录
- 源文件位于 `src/` 和 `assets/` 目录
- 参数文件位于 `params/` 目录
- 测试数据集位于 `datasets/` 目录

### 命名规范
- 类名：PascalCase (例: ImageStitcher, SensorDataInterface)
- 函数名：snake_case (例: load_parameters, process_frame)
- 常量：UPPER_SNAKE_CASE (例: DEBUG_LEVEL, MAX_CAMERAS)
- 全局变量：g_开头，snake_case (例: g_debug_mode, g_log_level)

### 调试开关管理
```cpp
// 全局变量定义方式
static int g_debug_level = 0;  // 0: OFF, 1: INFO, 2: DEBUG, 3: VERBOSE
static bool g_enable_timing = false;
static bool g_enable_io_dump = false;

// 使用方式
if (g_debug_level >= 1) {
    LOG_INFO("Processing frame: %d", frame_id);
}
```

### 日志输出规范
- 使用提供的 `logger.h` 的日志宏
- 日志级别：VERBOSE > DEBUG > INFO > WARNING > ERROR > FATAL
- 动态调试结果需要包含足够的上下文信息
- 避免在性能关键路径上添加过多日志

## 代码审查清单

修改前检查：
- [ ] 理解当前代码的设计意图
- [ ] 确认修改的影响范围
- [ ] 检查相关的其他模块

修改中检查：
- [ ] 遵循命名规范
- [ ] 添加必要的错误处理
- [ ] 完成调试日志和开关
- [ ] 符合编码风格

修改后检查：
- [ ] 逻辑正确性验证
- [ ] 是否引入新的依赖
- [ ] 文档是否需要更新
- [ ] CMakeLists.txt 是否需要更新

## RK3576适配指南

- 避免特定于x86的假设
- 注意内存和计算能力限制
- 使用DRM分配器进行内存管理 (drm_allocator.h)
- NV12格式支持 (nv12_frame.h)
- 性能优化需考虑ARM NEON指令集

## 文档要求

- README.md 保持最新
- 复杂模块需要设计文档
- 参数文件需要说明文档
- 重要修改需要更新CHANGELOG

---
