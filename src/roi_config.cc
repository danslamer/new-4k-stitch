#include "roi_config.h"

#include <opencv2/opencv.hpp>
#include <algorithm>

namespace {
int NormalizeEvenFloor(int value) {
    return std::max(0, value & ~1);
}
}

bool RoiConfig::LoadFromFile(const std::string& path, StitchGlobalConfig& config) {
    // ★ v3.0.1 防护: 之前 cv::FileStorage 解析错抛 cv::Exception 直接未捕获到 terminate().
    // 镜像 LoadCameraSourceList 的 try/catch 逻辑 (sensor_data_interface.cc).
    try {
    cv::FileStorage fs(path, cv::FileStorage::READ);
    if (!fs.isOpened()) return false;

    if (!fs["mode"].empty()) {
        config.mode = fs["mode"].string();
    }
    if (!fs["step_size"].empty()) {
        config.step_size = static_cast<int>(fs["step_size"]);
    }
    if (!fs["selected_cam"].empty()) {
        config.selected_cam = static_cast<int>(fs["selected_cam"]);
    }

    cv::FileNode feather_node = fs["feather"];
    if (!feather_node.empty()) {
        if (!feather_node["enabled"].empty()) {
            config.feather_enabled = static_cast<int>(feather_node["enabled"]) != 0;
        }
        if (!feather_node["width"].empty()) {
            int width = static_cast<int>(feather_node["width"]);
            config.feather_width = NormalizeEvenFloor(std::max(20, width));
        }
        if (!feather_node["strength"].empty()) {
            config.feather_strength = static_cast<double>(feather_node["strength"]);
        }
    }

    cv::FileNode save_node = fs["save"];
    if (!save_node.empty()) {
        if (!save_node["enabled"].empty()) {
            config.save_enabled = static_cast<int>(save_node["enabled"]) != 0;
        }
        if (!save_node["interval"].empty()) {
            int interval = static_cast<int>(save_node["interval"]);
            config.save_interval = std::max(1, interval);
        }
    }

    // v3.0 (2026-07-08): 6 路 ROI 占位 (cam0..cam5). Sprint 0 接受 offset 全 (0,0);
    //   Sprint 2 标定结果从 camchain_*.yaml 覆盖.
    static const char* cam_names[] = {"cam0", "cam1", "cam2", "cam3", "cam4", "cam5"};
    for (int i = 0; i < 6; ++i) {
        cv::FileNode cam_node = fs[cam_names[i]];
        if (!cam_node.empty()) {
            config.roi_offsets[i].offset_x = static_cast<int>(cam_node["offset_x"]);
            config.roi_offsets[i].offset_y = static_cast<int>(cam_node["offset_y"]);
        }
    }

    fs.release();
    return true;
    } catch (const cv::Exception& e) {
        return false;
    } catch (const std::exception& e) {
        return false;
    }
}

bool RoiConfig::SaveToFile(const std::string& path, const StitchGlobalConfig& config) {
    // ★ v3.0.1 防护: WRITE path 不可写 (perm/perm-path) 时 cv::FileStorage 构造函数抛 cv::Exception.
    // App 主循环在 setup_stitch_state 末尾调本函数, 没 try/catch 会 terminate() 全进程.
    // 像 LoadFromFile 一样, write-failure 当 false 返回, 让 bundle_video 流程继续.
    cv::FileStorage fs;
    try {
        fs.open(path, cv::FileStorage::WRITE);
    } catch (const cv::Exception& ) {
        return false;
    } catch (const std::exception& ) {
        return false;
    }
    if (!fs.isOpened()) return false;

    fs << "mode" << config.mode;
    fs << "step_size" << config.step_size;
    fs << "selected_cam" << config.selected_cam;

    fs << "feather" << "{";
    fs << "enabled" << (config.feather_enabled ? 1 : 0);
    fs << "width" << config.feather_width;
    fs << "strength" << config.feather_strength;
    fs << "}";

    fs << "save" << "{";
    fs << "enabled" << (config.save_enabled ? 1 : 0);
    fs << "interval" << config.save_interval;
    fs << "}";

    // v3.0: 同步 cam0..cam5 (与 LoadFromFile 对称)
    static const char* cam_names[] = {"cam0", "cam1", "cam2", "cam3", "cam4", "cam5"};
    for (int i = 0; i < 6; ++i) {
        fs << cam_names[i] << "{";
        fs << "offset_x" << config.roi_offsets[i].offset_x;
        fs << "offset_y" << config.roi_offsets[i].offset_y;
        fs << "}";
    }

    fs.release();
    return true;
}