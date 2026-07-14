//
// Created for Sprint 4-A (2026-07-14).
// camera_intrinsics.cc - mutable storage + yaml LoadOrDefault/SaveToFile.
//

#include "camera_intrinsics.h"

#include <opencv2/opencv.hpp>

#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>

namespace camera_intrinsics {

namespace {

// 单例存储 - 6 路 Intrinsics. App::App() 启动期会覆盖.
//   注意: 这里用局部 static, 第一次调用时构造默认. 之后 MutableIntrinsics()
//   返回引用, App / BuildStitchLayout2x3 等直接读写即可.
std::array<Intrinsics, kNumCams>& Storage() {
    static std::array<Intrinsics, kNumCams> kStore{};
    return kStore;
}

constexpr double kPi = 3.14159265358979323846;

}  // namespace

std::array<Intrinsics, kNumCams>& MutableIntrinsics() {
    return Storage();
}

const Intrinsics& ForCam(int i) {
    auto& store = Storage();
    if (i < 0 || i >= kNumCams) {
        // 越界: 返回 cam0 默认 (防御性; 让 layout 计算仍然有值而不是崩)
        return store[0];
    }
    return store[i];
}

bool CalibrationConfig::LoadOrDefault(const std::string& path) {
    // 安全加载: yaml 不存在 / 解析失败 / 部分字段缺失 都不应抛 cv::Exception ->
    // v3.0.1 (参 roi_config.cc) 同款防护: 全 try/catch + 单独读字段 isempty 检查.
    cv::FileStorage fs;
    try {
        fs.open(path, cv::FileStorage::READ);
    } catch (const cv::Exception&) {
        return false;
    } catch (const std::exception&) {
        return false;
    }
    if (!fs.isOpened()) {
        return false;
    }

    auto& store = Storage();
    static const char* kCamNames[kNumCams] = {
        "cam0", "cam1", "cam2", "cam3", "cam4", "cam5"
    };

    for (int i = 0; i < kNumCams; ++i) {
        cv::FileNode node = fs[kCamNames[i]];
        if (node.empty() || !node.isMap()) continue;

        // 1) 解析 image_w / image_h (优先, 否则 fov->fx 反推需要 image_w).
        if (!node["image_w"].empty())
            store[i].image_w = static_cast<int>(node["image_w"]);
        if (!node["image_h"].empty())
            store[i].image_h = static_cast<int>(node["image_h"]);

        // 2) 直接 fx/fy 优先级最高; 否则 h_fov_deg / v_fov_deg 反推.
        if (!node["fx"].empty()) {
            store[i].fx = static_cast<double>(node["fx"]);
        } else if (!node["h_fov_deg"].empty()) {
            store[i].fx = Intrinsics::FovToFx(
                store[i].image_w, static_cast<double>(node["h_fov_deg"]));
        }
        if (!node["fy"].empty()) {
            store[i].fy = static_cast<double>(node["fy"]);
        } else if (!node["v_fov_deg"].empty()) {
            store[i].fy = Intrinsics::FovToFy(
                store[i].image_h, static_cast<double>(node["v_fov_deg"]));
        }

        // 3) cx/cy: 优先显式, 否则 image center.
        if (!node["cx"].empty()) store[i].cx = static_cast<double>(node["cx"]);
        else store[i].cx = store[i].image_w * 0.5;
        if (!node["cy"].empty()) store[i].cy = static_cast<double>(node["cy"]);
        else store[i].cy = store[i].image_h * 0.5;

        // 4) yaw + 畸变.
        if (!node["yaw_h_deg"].empty())
            store[i].yaw_h_deg = static_cast<double>(node["yaw_h_deg"]);
        if (!node["yaw_v_deg"].empty())
            store[i].yaw_v_deg = static_cast<double>(node["yaw_v_deg"]);
        if (!node["k1"].empty()) store[i].k1 = static_cast<double>(node["k1"]);
        if (!node["k2"].empty()) store[i].k2 = static_cast<double>(node["k2"]);
        if (!node["p1"].empty()) store[i].p1 = static_cast<double>(node["p1"]);
        if (!node["p2"].empty()) store[i].p2 = static_cast<double>(node["p2"]);
    }

    fs.release();
    return true;
}

bool CalibrationConfig::SaveToFile(const std::string& path) {
    cv::FileStorage fs(path, cv::FileStorage::WRITE);
    if (!fs.isOpened()) return false;

    static const char* kCamNames[kNumCams] = {
        "cam0", "cam1", "cam2", "cam3", "cam4", "cam5"
    };
    const auto& store = Storage();
    for (int i = 0; i < kNumCams; ++i) {
        fs << kCamNames[i] << "{";
        fs << "image_w"   << store[i].image_w;
        fs << "image_h"   << store[i].image_h;
        fs << "fx"        << store[i].fx;
        fs << "fy"        << store[i].fy;
        fs << "cx"        << store[i].cx;
        fs << "cy"        << store[i].cy;
        fs << "yaw_h_deg" << store[i].yaw_h_deg;
        fs << "yaw_v_deg" << store[i].yaw_v_deg;
        fs << "k1"        << store[i].k1;
        fs << "k2"        << store[i].k2;
        fs << "p1"        << store[i].p1;
        fs << "p2"        << store[i].p2;
        fs << "}";
    }
    fs.release();
    return true;
}

}  // namespace camera_intrinsics
