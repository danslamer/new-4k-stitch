cmake_minimum_required(VERSION 3.10)
project(image-stitching LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_RK_HARDWARE_DECODING "Enable FFmpeg Rockchip hardware decoding" ON)

# -----------------------------
# 设置自定义 FFmpeg 6 路径
# -----------------------------
# 假设你安装 FFmpeg 6 到 ~/dev/ffmpeg60
set(FFMPEG_ROOT "$ENV{HOME}/dev/ffmpeg60")

# 设置 pkg-config 搜索路径和运行时库路径
set(ENV{PKG_CONFIG_PATH} "${FFMPEG_ROOT}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
set(ENV{LD_LIBRARY_PATH} "${FFMPEG_ROOT}/lib:$ENV{LD_LIBRARY_PATH}")

# -----------------------------
# 查找依赖库
# -----------------------------
find_package(OpenCV 4 REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# 使用 pkg-config 查找 FFmpeg 6
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)

# -----------------------------
# 编译源文件
# -----------------------------
add_executable(${PROJECT_NAME}
    src/app.cc
    include/app.h
    src/sensor_data_interface.cc
    include/sensor_data_interface.h
    src/stitching_param_generater.cc
    include/stitching_param_generater.h
    src/image_stitcher.cc
    include/image_stitcher.h
    src/logger.cc
    include/logger.h
)

# 包含头文件
target_include_directories(${PROJECT_NAME} PRIVATE
    include
    ${OpenCV_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
)

# 链接库
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${OpenCV_LIBS}
    Threads::Threads
    PkgConfig::AVFORMAT
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
    PkgConfig::SWSCALE
)

# 开启硬解码宏
if(ENABLE_RK_HARDWARE_DECODING)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_RK_HARDWARE_DECODING=1)
endif()