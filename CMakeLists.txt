cmake_minimum_required(VERSION 3.10)
project(image-stitching LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_RK_HARDWARE_DECODING "Enable FFmpeg Rockchip hardware decoding" ON)
option(ENABLE_RGA_DMA_STITCHING "Enable the in-progress DMA-BUF/RGA stitch backend" ON)

# FFmpeg 路径 (ffmpeg-rockchip 安装位置).
# 默认是 Debian/Ubuntu 多架构布局 (--prefix=/usr 安装后的位置).
# 若 ffmpeg 装到非标准路径, 用 -DFFMPEG_LIB_DIR=/path/to/lib 覆盖.
# 例: ffmpeg-rockchip-master 用 --prefix=/usr 装完后, .so 在 /usr/lib/aarch64-linux-gnu/ 下.
set(FFMPEG_LIB_DIR "/usr/lib/aarch64-linux-gnu" CACHE PATH
    "FFmpeg library directory (multiarch layout from ffmpeg-rockchip --prefix=/usr)")
set(ENV{PKG_CONFIG_PATH} "${FFMPEG_LIB_DIR}/pkgconfig:$ENV{PKG_CONFIG_PATH}")
set(ENV{LD_LIBRARY_PATH} "${FFMPEG_LIB_DIR}:$ENV{LD_LIBRARY_PATH}")

find_package(OpenCV 4 REQUIRED COMPONENTS core imgproc imgcodecs videoio features2d stitching)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
# 增加 OpenCL 查找
find_package(OpenCL REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

# v2.3 阶段 2: cpp-httplib HTTP server (vendor 进 third_party, header-only)
# 准备: 板端跑 curl 拉取 (网络不通 GitHub, 用 gh-proxy 镜像)
#   mkdir -p third_party/cpp-httplib
#   curl -L 'https://gh-proxy.com/https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h' -o third_party/cpp-httplib/httplib.h
# 注: cpp-httplib 自 v0.x 起就 header-only, 不需要 .cpp 文件
set(CPPHTTPLIB_DIR ${CMAKE_SOURCE_DIR}/third_party/cpp-httplib)
if(EXISTS ${CPPHTTPLIB_DIR}/httplib.h)
    message(STATUS "[cmake] Using vendored cpp-httplib at ${CPPHTTPLIB_DIR}/httplib.h (header-only)")
    add_library(httplib INTERFACE)
    target_include_directories(httplib INTERFACE ${CPPHTTPLIB_DIR})
    target_compile_definitions(httplib INTERFACE CPPHTTPLIB_THREAD_POOL=1)
    add_library(httplib::httplib ALIAS httplib)
else()
    message(WARNING "[cmake] cpp-httplib not found at ${CPPHTTPLIB_DIR}, HTTP server will be disabled")
    add_library(httplib_dummy INTERFACE)
    target_compile_definitions(httplib_dummy INTERFACE IMAGE_STITCHING_NO_HTTP=1)
    add_library(httplib::httplib ALIAS httplib_dummy)
endif()

pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
# v2.4 (2026-07-06): vendor 确认 ffmpeg rkmpp 不维护, kmpp 解码器未完成.
# 改走 gstreamer1.0-rockchip1 (libgstrockchipmpp.so + mppvideodec) + mpp_service 老接口.
# 板端 dpkg: gstreamer1.0-rockchip1 1.14-4, librockchip-mpp 1.5.0.
# gst_dmabuf 是 1.20+ 内建 API, 板端 1.20.3 已带.
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0)
pkg_check_modules(GSTAPP REQUIRED IMPORTED_TARGET gstreamer-app-1.0)
pkg_check_modules(GSTVIDEO REQUIRED IMPORTED_TARGET gstreamer-video-1.0)
# gstreamer 1.20 allocators: 提供 gst_dmabuf_memory_get_fd / gst_is_dmabuf_memory.
# 板端 gst 1.20.3 已带 (gst-plugins-base 包).
pkg_check_modules(GSTALLOC REQUIRED IMPORTED_TARGET gstreamer-allocators-1.0)
# v3.2 (2026-07-09): RTSP server (gst-rtsp-server-1.0).
# 板端先试: sudo apt install gstreamer1.0-rtsp-server-1.0
# 找不到的话, 试 libgstrtspserver-1.0-dev (apt 镜像里有时只用这个名)
# 还不行: 编辑 params/camera_sources.yaml 把 output.enabled 设 false, 拼接功能照常工作
pkg_check_modules(GSTRTSPSERVER IMPORTED_TARGET gstreamer-rtsp-server-1.0)
if(NOT GSTRTSPSERVER_FOUND)
  pkg_check_modules(GSTRTSPSERVER IMPORTED_TARGET libgstrtspserver-1.0)
endif()
if(NOT GSTRTSPSERVER_FOUND)
  message(WARNING "gstreamer-rtsp-server-1.0 not found; RTSP output disabled (yaml output.enabled will be ignored).")
  set(GSTRTSPSERVER_ENABLED OFF)
  set(HAVE_GST_RTSP_SERVER 0)
else()
  set(GSTRTSPSERVER_ENABLED ON)
  set(HAVE_GST_RTSP_SERVER 1)
endif()
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GLESV2 REQUIRED IMPORTED_TARGET glesv2)
pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm)
if(ENABLE_RGA_DMA_STITCHING)
    pkg_check_modules(RGA REQUIRED IMPORTED_TARGET librga)
    pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
endif()

add_executable(${PROJECT_NAME}
    src/app.cc
    include/app.h
    src/sensor_data_interface.cc
    include/sensor_data_interface.h
    src/gst_mpp_decoder.cc
    include/gst_mpp_decoder.h
    src/stitching_param_generater.cc
    include/stitching_param_generater.h
    # v3.2: 帧差掩码 + 2 路 RTSP 输出
    src/frame_diff.cc
    include/frame_diff.h
    src/output_streams.cc
    include/output_streams.h
    src/gst_rtsp_server.cc
    include/gst_rtsp_server.h
    src/drm_allocator.cc
    include/drm_allocator.h
    include/nv12_frame.h
    src/image_stitcher.cc
    include/image_stitcher.h
    src/rk_gles_warper.cc
    include/rk_gles_warper.h
    src/logger.cc
    include/logger.h
    src/roi_config.cc
    include/roi_config.h
    include/camera_intrinsics.h  # v3.0: K 矩阵占位 (fx=1027 fy=1378 cx=1280 cy=720)
    src/roi_visualizer.cc
    include/roi_visualizer.h
    src/status_writer.cc
    include/status_writer.h
    src/http_server.cc
    include/http_server.h
    # 阶段 2: MJPEG panorama 推流给 CameraPage /api/stream 预览
    src/mjpeg_streamer.cc
    include/mjpeg_streamer.h
)

target_include_directories(${PROJECT_NAME} PRIVATE
    include
    ${CMAKE_CURRENT_BINARY_DIR}/include  # for generated config.h (HAVE_GST_RTSP_SERVER)
    ${OpenCV_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
)

# 条件链接 GSTRTSPSERVER — 用变量代替 if() / endif() 内联, 避免 CMake 把 if/endif
# 关键字当成 link library 名称传给 ld.
set(_rtsp_link_target "")
if(GSTRTSPSERVER_ENABLED)
  set(_rtsp_link_target PkgConfig::GSTRTSPSERVER)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${OpenCV_LIBS}
    Threads::Threads
    PkgConfig::AVFORMAT
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
    # v2.4: gstreamer-rockchip 解码路径 (替代 ffmpeg rkmpp).
    PkgConfig::GSTREAMER
    PkgConfig::GSTAPP
    PkgConfig::GSTVIDEO
    PkgConfig::GSTALLOC
    ${_rtsp_link_target}
    PkgConfig::EGL
    PkgConfig::GLESV2
    PkgConfig::GBM
    PkgConfig::SDL2
    OpenCL::OpenCL
    httplib::httplib  # v2.3: HTTP server (如果 vendor 没拉下来, 用 dummy stub)
)

if(ENABLE_RK_HARDWARE_DECODING)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_RK_HARDWARE_DECODING=1)
endif()

if(ENABLE_RGA_DMA_STITCHING)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_RGA_DMA_STITCHING=1)
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::RGA PkgConfig::LIBDRM)
endif()

# v3.2 (2026-07-09): 由 CMake 生成 include/config.h, 标记可选功能 (RTSP server 等).
# 必须放在所有 set(HAVE_xxx ...) 之后 — 否则 #cmakedefine01 拿不到正确值.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake_config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/config.h"
)
