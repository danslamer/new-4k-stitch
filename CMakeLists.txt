cmake_minimum_required(VERSION 3.10)
project(image-stitching LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_RK_HARDWARE_DECODING "Enable FFmpeg Rockchip hardware decoding" ON)

find_package(OpenCV 4 REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat
        libavcodec
        libavutil
        libswscale)

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
        include/logger.h)

target_include_directories(${PROJECT_NAME} PRIVATE
        include
        ${OpenCV_INCLUDE_DIRS})

target_link_libraries(${PROJECT_NAME} PRIVATE
        ${OpenCV_LIBS}
        Threads::Threads
        PkgConfig::FFMPEG)

if (ENABLE_RK_HARDWARE_DECODING)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_RK_HARDWARE_DECODING=1)
endif ()
