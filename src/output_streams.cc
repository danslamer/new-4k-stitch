// output_streams.cc
#include "output_streams.h"

#include "logger.h"

#include <opencv2/core.hpp>

#include <cstdlib>
#include <mutex>

namespace output_streams {

namespace {

ServerConfig g_config;
std::once_flag g_loaded;

void ParseStreamNode(const cv::FileNode& node, StreamConfig& sc) {
  // OpenCV FileNode::operator>> 返回 void, 写 if (node["x"] >> v) 编译不过.
  if (node["path"].isString()) {
    std::string p; node["path"] >> p; sc.path = p;
  }
  if (node["width"].isInt())      { int v; node["width"]      >> v; sc.width       = v; }
  if (node["height"].isInt())     { int v; node["height"]     >> v; sc.height      = v; }
  if (node["fps"].isInt())        { int v; node["fps"]        >> v; sc.fps         = v; }
  if (node["bitrate_kbps"].isInt()){ int v; node["bitrate_kbps"]>> v; sc.bitrate_kbps= v; }
  if (node["encoder"].isString()) {
    std::string enc; node["encoder"] >> enc; sc.encoder = enc;
  }
}

}  // namespace (anonymous, closes inner)

ServerConfig LoadFromYaml(const std::string& yaml_path) {
    ServerConfig cfg;  // start from default (enabled=false)

    try {
        cv::FileStorage fs(yaml_path, cv::FileStorage::READ);
        if (!fs.isOpened()) {
            Logger::GetInstance().Log(
                "[output_streams] cannot open " + yaml_path +
                ", RTSP server disabled (default)");
            return cfg;
        }
        cv::FileNode root = fs.root();
        cv::FileNode out = root["output"];
        if (out.empty()) {
            Logger::GetInstance().Log(
                "[output_streams] no `output:` block in " + yaml_path +
                ", RTSP server disabled");
            return cfg;
        }
        // OpenCV FileNode::operator>> 返回 void, 写 if (node["x"] >> v) 编译不过.
        // 标准做法: 先 isInt()/isString() 检查再读.
        if (out["enabled"].isInt())  { int v; out["enabled"] >> v; cfg.enabled = (v != 0); }
        if (out["port"].isInt())     { int v; out["port"] >> v; cfg.port = v; }
        if (out["bind_address"].isString()) { std::string s; out["bind_address"] >> s; cfg.bind_address = s; }
        if (out["auth_enabled"].isInt()) { int v; out["auth_enabled"] >> v; cfg.auth_enabled = (v != 0); }
        if (out["auth_user"].isString())  { std::string s; out["auth_user"] >> s; cfg.auth_user = s; }
        if (out["auth_pass"].isString())  { std::string s; out["auth_pass"] >> s; cfg.auth_pass = s; }
        if (out["on_demand"].isInt())  { int v; out["on_demand"] >> v; cfg.on_demand = (v != 0); }
        if (out["diff_threshold"].isInt()) { int v; out["diff_threshold"] >> v; cfg.diff_threshold = v; }
        if (out["diff_bbox_min_area"].isInt()) { int v; out["diff_bbox_min_area"] >> v; cfg.diff_bbox_min_area = v; }

        cv::FileNode streams_node = out["streams"];
        if (!streams_node.empty() && streams_node.isSeq()) {
            for (auto it = streams_node.begin(); it != streams_node.end(); ++it) {
                StreamConfig sc;
                ParseStreamNode(*it, sc);
                cfg.streams.push_back(sc);
            }
        }

        std::call_once(g_loaded, [&] { g_config = cfg; });
    } catch (const cv::Exception& e) {
        Logger::GetInstance().LogError(
            std::string("[output_streams] cv::Exception: ") + e.what());
    }
    return cfg;
}

const ServerConfig& GetConfig() {
    return g_config;
}

}  // namespace output_streams