cmake_minimum_required(VERSION 3.10)
project(image-stitching LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_RK_HARDWARE_DECODING "Enable FFmpeg Rockchip hardware decoding" ON)
option(ENABLE_RGA_DMA_STITCHING "Enable the in-progress DMA-BUF/RGA stitch backend" ON)

set(FFMPEG_ROOT "$ENV{HOME}/dev/ffmpeg60")
set(ENV{PKG_CONFIG_PATH} "${FFMPEG_ROOT}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
set(ENV{LD_LIBRARY_PATH} "${FFMPEG_ROOT}/lib:$ENV{LD_LIBRARY_PATH}")

find_package(OpenCV 4 REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
if(ENABLE_RGA_DMA_STITCHING)
    pkg_check_modules(RGA REQUIRED IMPORTED_TARGET librga)
    pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
endif()

add_executable(${PROJECT_NAME}
    src/app.cc
    include/app.h
    src/sensor_data_interface.cc
    include/sensor_data_interface.h
    src/stitching_param_generater.cc
    include/stitching_param_generater.h
    src/drm_allocator.cc
    include/drm_allocator.h
    include/nv12_frame.h
    src/image_stitcher.cc
    include/image_stitcher.h
    src/logger.cc
    include/logger.h
)

target_include_directories(${PROJECT_NAME} PRIVATE
    include
    ${OpenCV_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${OpenCV_LIBS}
    Threads::Threads
    PkgConfig::AVFORMAT
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
)

if(ENABLE_RK_HARDWARE_DECODING)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_RK_HARDWARE_DECODING=1)
endif()

if(ENABLE_RGA_DMA_STITCHING)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_RGA_DMA_STITCHING=1)
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::RGA PkgConfig::LIBDRM)
endif()
