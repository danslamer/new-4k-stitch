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

    // v3.x (2026-07-09): 直接读 cam{i}.{x, y, width, height, affine}. valid 由 width/height>0 推导.
    //   老的 offset_x/offset_y 字段被忽略 — 旧 yaml 第一次加载会全部 valid=false,
    //   App::App() 看到 valid 全 false 就重跑 bootstrap, 把新格式写回.
    static const char* cam_names[] = {"cam0", "cam1", "cam2", "cam3", "cam4", "cam5"};
    for (int i = 0; i < 6; ++i) {
        cv::FileNode cam_node = fs[cam_names[i]];
        if (cam_node.empty()) continue;

        CameraRoiRect& r = config.camera_rois[i];
        // 老字段存在 -> 打日志, 忽略 (用 Bootstrap 兜底)
        if (!cam_node["offset_x"].empty() || !cam_node["offset_y"].empty()) {
            // 不做 in-place 迁移, 因为缺 overlap 信息. 触发 Bootstrap 即可.
        }

        if (!cam_node["x"].empty())       r.x      = static_cast<int>(cam_node["x"]);
        if (!cam_node["y"].empty())       r.y      = static_cast<int>(cam_node["y"]);
        if (!cam_node["width"].empty())   r.width  = static_cast<int>(cam_node["width"]);
        if (!cam_node["height"].empty())  r.height = static_cast<int>(cam_node["height"]);
        r.valid = (r.width > 0 && r.height > 0);

        // v3.x.1 (2026-07-09): affine 2x3 矩阵 [a b c d tx ty]. 缺失 = 单位阵 (无 warp).
        cv::FileNode affine_node = cam_node["affine"];
        if (!affine_node.empty() && affine_node.size() == 6) {
            int idx = 0;
            for (cv::FileNodeIterator it = affine_node.begin();
                 it != affine_node.end() && idx < 6; ++it, ++idx) {
                r.affine[idx] = static_cast<double>(*it);
            }
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

    // v3.x (2026-07-09): 写 cam{i}.{x, y, width, height, affine}. valid 不写, 由 w/h>0 推导.
    // v3.x.1 (2026-07-09): 同步写 affine: 2x3 行主序 [a b c d tx ty] (cv::Mat 表示法).
    static const char* cam_names[] = {"cam0", "cam1", "cam2", "cam3", "cam4", "cam5"};
    for (int i = 0; i < 6; ++i) {
        fs << cam_names[i] << "{";
        fs << "x"      << config.camera_rois[i].x;
        fs << "y"      << config.camera_rois[i].y;
        fs << "width"  << config.camera_rois[i].width;
        fs << "height" << config.camera_rois[i].height;
        fs << "affine" << "[" << config.camera_rois[i].affine[0]
                          << config.camera_rois[i].affine[1]
                          << config.camera_rois[i].affine[2]
                          << config.camera_rois[i].affine[3]
                          << config.camera_rois[i].affine[4]
                          << config.camera_rois[i].affine[5] << "]";
        fs << "}";
    }

    fs.release();
    return true;
}