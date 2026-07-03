// http_server.cc - image-stitching 嵌入式 HTTP server (cpp-httplib 后端)
//
// 路由:
//   GET  /                       -> CameraPage/index.html
//   GET  /index.html             -> CameraPage/index.html
//   GET  /css/*, /js/*           -> CameraPage/{css,js}/*
//   GET  /api/health             -> {"ok": true}
//   GET  /api/status             -> /tmp/stitch_status.json 内容
//   GET  /api/devices            -> 从 params/camera_sources.yaml 解析
//   GET  /api/config             -> params/roi_tuning.yaml 内容
//   GET  /api/network            -> /sys/class/net/* + ip 命令
//   POST /api/roi                -> 原子改 roi_tuning.yaml
//
// 设计要点:
//   - 后台 std::thread 跑 cpp-httplib::Server
//   - 静态文件 + API 全在一个 svr 里
//   - 网络信息用 popen("ip ...") 拉, 不要重新发明轮子
#include "http_server.h"
#include "status_writer.h"  // 复用 status 文件路径常量

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>

#ifndef IMAGE_STITCHING_NO_HTTP
#include <httplib.h>
#endif

namespace http_server {

namespace {

std::atomic<bool> g_running{false};
std::thread       g_thread;
std::string       g_camera_page_dir;
std::string       g_status_path = "/tmp/stitch_status.json";
std::string       g_project_root = "..";  // 相对 CWD

#ifndef IMAGE_STITCHING_NO_HTTP

std::string read_file_str(const std::string& path) {
    std::ifstream f(path);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string get_mime(const std::string& path) {
    auto ends_with = [&](const std::string& s) {
        return path.size() >= s.size() &&
               path.compare(path.size() - s.size(), s.size(), s) == 0;
    };
    if (ends_with(".html")) return "text/html; charset=utf-8";
    if (ends_with(".css"))  return "text/css; charset=utf-8";
    if (ends_with(".js"))   return "application/javascript; charset=utf-8";
    if (ends_with(".json")) return "application/json; charset=utf-8";
    if (ends_with(".png"))  return "image/png";
    if (ends_with(".svg"))  return "image/svg+xml";
    return "application/octet-stream";
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// 简单 trim
std::string trim(const std::string& s) {
    auto a = s.find_first_not_of(" \t\r\n");
    auto b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

// GET /api/devices: 解析 params/camera_sources.yaml, 简化为 grep-style
std::string handle_devices() {
    std::string yaml_path = g_project_root + "/params/camera_sources.yaml";
    std::string content = read_file_str(yaml_path);
    std::ostringstream out;
    out << "{\n  \"devices\": [\n";

    // 极简 yaml 解析: 按行匹配 "uri:", "width:", "height:", "type:"
    std::vector<std::string> uris, types, widths, heights;
    std::string cur_type, cur_uri, cur_w, cur_h;
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        // cam 块开始 (以 "- type:" 开头)
        if (t.rfind("- type:", 0) == 0) {
            if (!cur_uri.empty()) {
                uris.push_back(cur_uri); types.push_back(cur_type);
                widths.push_back(cur_w); heights.push_back(cur_h);
            }
            cur_type = trim(t.substr(7));
            cur_uri.clear(); cur_w.clear(); cur_h.clear();
            continue;
        }
        size_t colon = t.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(t.substr(0, colon));
        std::string val = trim(t.substr(colon + 1));
        if (key == "uri")   cur_uri = val;
        else if (key == "width")  cur_w = val;
        else if (key == "height") cur_h = val;
    }
    if (!cur_uri.empty()) {
        uris.push_back(cur_uri); types.push_back(cur_type);
        widths.push_back(cur_w); heights.push_back(cur_h);
    }

    int n = std::min((int)uris.size(), 6);
    for (int i = 0; i < n; ++i) {
        out << "    {\"id\": " << i
            << ", \"type\": \"" << json_escape(types[i]) << "\""
            << ", \"uri\": \"" << json_escape(uris[i]) << "\""
            << ", \"width\": " << (widths[i].empty() ? 0 : std::atoi(widths[i].c_str()))
            << ", \"height\": " << (heights[i].empty() ? 0 : std::atoi(heights[i].c_str()))
            << "}";
        if (i < n - 1) out << ",";
        out << "\n";
    }
    out << "  ],\n  \"total\": " << n << "\n}\n";
    return out.str();
}

// GET /api/network: 读 /sys/class/net + ip 命令
std::string handle_network() {
    std::ostringstream out;
    out << "{\n  \"interfaces\": [\n";

    FILE* p = popen("ls /sys/class/net 2>/dev/null", "r");
    if (!p) return "{\"interfaces\":[]}\n";
    char buf[256];
    std::vector<std::string> ifaces;
    while (fgets(buf, sizeof(buf), p)) {
        std::string s = trim(buf);
        if (!s.empty() && s != "lo") ifaces.push_back(s);
    }
    pclose(p);

    std::string primary_ip, primary_iface;
    for (size_t i = 0; i < ifaces.size(); ++i) {
        const std::string& iface = ifaces[i];
        // 读 MAC
        std::string mac = trim(read_file_str("/sys/class/net/" + iface + "/address"));
        // 读 IP via "ip" 命令
        std::string ip = "";
        std::string cmd = "ip -4 -o addr show dev " + iface + " 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -1";
        FILE* p2 = popen(cmd.c_str(), "r");
        if (p2) {
            if (fgets(buf, sizeof(buf), p2)) ip = trim(buf);
            pclose(p2);
        }
        // 读 state
        std::string state = trim(read_file_str("/sys/class/net/" + iface + "/operstate"));

        out << "    {\"name\": \"" << json_escape(iface) << "\""
            << ", \"mac\": \"" << json_escape(mac) << "\""
            << ", \"ip\": \"" << json_escape(ip) << "\""
            << ", \"state\": \"" << json_escape(state) << "\"}";
        if (i < ifaces.size() - 1) out << ",";
        out << "\n";

        if (state == "up" && !ip.empty() && primary_ip.empty()) {
            primary_ip = ip;
            primary_iface = iface;
        }
    }
    out << "  ],\n  \"primary_ip\": \"" << json_escape(primary_ip) << "\""
        << ",\n  \"primary_iface\": \"" << json_escape(primary_iface) << "\"\n}\n";

    // hostname
    char host[256] = {0};
    gethostname(host, sizeof(host) - 1);
    // 简单嵌入到 primary_ip 之后
    std::string result = out.str();
    size_t pos = result.rfind("\n}\n");
    if (pos != std::string::npos) {
        result.insert(pos, ",\n  \"hostname\": \"" + json_escape(host) + "\"");
    }
    return result;
}

// POST /api/roi: 原子改 roi_tuning.yaml
// body 形如: {"cam": 0, "offset_x": 10, "offset_y": -5}
std::string handle_roi_post(const std::string& body) {
    // 极简 JSON 解析: 找 "cam": N, "offset_x": N, "offset_y": N
    auto find_int = [&](const std::string& key) -> int {
        std::regex re("\"" + key + "\"\\s*:\\s*(-?\\d+)");
        std::smatch m;
        if (std::regex_search(body, m, re)) return std::atoi(m[1].str().c_str());
        return INT_MIN;
    };

    int cam = find_int("cam");
    int off_x = find_int("offset_x");
    int off_y = find_int("offset_y");
    if (cam < 0 || cam >= 6 || off_x == INT_MIN || off_y == INT_MIN) {
        return "{\"ok\": false, \"error\": \"invalid JSON, need cam/offset_x/offset_y\"}\n";
    }

    std::string yaml_path = g_project_root + "/params/roi_tuning.yaml";
    std::string content = read_file_str(yaml_path);
    if (content.empty()) {
        return "{\"ok\": false, \"error\": \"roi_tuning.yaml not found\"}\n";
    }

    // 找 camX: 块, 改 offset_x 和 offset_y
    std::string key = "cam" + std::to_string(cam) + ":";
    std::vector<std::string> lines;
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) lines.push_back(line);

    bool in_target = false;
    bool replaced_x = false, replaced_y = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string t = trim(lines[i]);
        if (t == key) { in_target = true; continue; }
        if (in_target && t.rfind("cam", 0) == 0 && t.find(':') != std::string::npos
            && t != key) {
            in_target = false;  // 进入下一个 cam 块
        }
        if (in_target && t.rfind("offset_x:", 0) == 0) {
            lines[i] = "   offset_x: " + std::to_string(off_x);
            replaced_x = true;
        } else if (in_target && t.rfind("offset_y:", 0) == 0) {
            lines[i] = "   offset_y: " + std::to_string(off_y);
            replaced_y = true;
        }
    }
    if (!replaced_x && !replaced_y) {
        return "{\"ok\": false, \"error\": \"cam" + std::to_string(cam) + " not found in yaml\"}\n";
    }

    // 写临时文件 + rename 原子替换
    std::string tmp = yaml_path + ".tmp";
    std::ofstream of(tmp);
    if (!of) {
        return "{\"ok\": false, \"error\": \"cannot open tmp file\"}\n";
    }
    for (size_t i = 0; i < lines.size(); ++i) {
        of << lines[i];
        if (i + 1 < lines.size()) of << "\n";
    }
    of.close();
    if (std::rename(tmp.c_str(), yaml_path.c_str()) != 0) {
        return "{\"ok\": false, \"error\": \"rename failed\"}\n";
    }
    return "{\"ok\": true, \"message\": \"cam" + std::to_string(cam)
        + " updated, restart image-stitching to apply\"}\n";
}

// 静态文件 handler: 读 file + 设 MIME
void serve_static_file(httplib::Response& res, const std::string& path) {
    std::string content = read_file_str(path);
    if (content.empty()) {
        res.status = 404;
        res.set_content("404 Not Found", "text/plain");
        return;
    }
    res.set_content(content, get_mime(path).c_str());
}

void run_server(const Config& cfg) {
    httplib::Server svr;

    // CORS (允许跨域, 调试方便)
    svr.set_post_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
    });

    // GET /api/health
    svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        std::string body = "{\"ok\": true, \"ts\": " +
            std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()) + "}\n";
        res.set_content(body, "application/json");
    });

    // GET /api/status -> 读 /tmp/stitch_status.json
    svr.Get("/api/status", [](const httplib::Request&, httplib::Response& res) {
        std::string body = read_file_str(g_status_path);
        if (body.empty()) {
            body = "{\"warning\": \"status file not yet written\", "
                   "\"cams\": [], \"num_cameras\": 0, \"mode\": 0, "
                   "\"panorama_w\": 0, \"panorama_h\": 0, "
                   "\"current_fps\": 0.0, \"frame_idx\": 0, "
                   "\"timestamp_us\": 0, \"blend_ms\": 0, \"warp_ms\": 0, "
                   "\"simulated\": true}\n";
            res.status = 200;
        }
        res.set_content(body, "application/json");
    });

    // GET /api/devices
    svr.Get("/api/devices", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(handle_devices(), "application/json");
    });

    // GET /api/config -> yaml
    svr.Get("/api/config", [](const httplib::Request&, httplib::Response& res) {
        std::string yaml_path = g_project_root + "/params/roi_tuning.yaml";
        std::string content = read_file_str(yaml_path);
        if (content.empty()) {
            res.status = 404;
            res.set_content("# roi_tuning.yaml not found\n", "text/plain");
        } else {
            res.set_content(content, "text/yaml; charset=utf-8");
        }
    });

    // GET /api/network
    svr.Get("/api/network", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(handle_network(), "application/json");
    });

    // POST /api/roi
    svr.Post("/api/roi", [](const httplib::Request& req, httplib::Response& res) {
        std::string body = req.body;
        if (body.empty()) {
            res.status = 400;
            res.set_content("{\"ok\": false, \"error\": \"empty body\"}\n", "application/json");
            return;
        }
        if (body.size() > 4096) {
            res.status = 400;
            res.set_content("{\"ok\": false, \"error\": \"body too large\"}\n", "application/json");
            return;
        }
        res.set_content(handle_roi_post(body), "application/json");
    });

    // 静态文件: CameraPage
    svr.Get("/", [cfg](const httplib::Request&, httplib::Response& res) {
        serve_static_file(res, std::string(cfg.camera_page_dir) + "/index.html");
    });
    svr.Get("/index.html", [cfg](const httplib::Request&, httplib::Response& res) {
        serve_static_file(res, std::string(cfg.camera_page_dir) + "/index.html");
    });
    // 通配: /css/* /js/*
    svr.Get(R"(/(css|js)/(.+))", [cfg](const httplib::Request& req, httplib::Response& res) {
        std::string path = std::string(cfg.camera_page_dir) + "/" + req.matches[1].str() + "/" + req.matches[2].str();
        serve_static_file(res, path);
    });

    // 启动
    g_running.store(true);
    fprintf(stderr, "[http_server] Listening on 0.0.0.0:%d (camera_page=%s)\n",
            cfg.port, cfg.camera_page_dir);
    if (!svr.listen("0.0.0.0", cfg.port)) {
        fprintf(stderr, "[http_server] listen() failed on port %d\n", cfg.port);
        g_running.store(false);
        return;
    }
    g_running.store(false);
}

#endif  // !IMAGE_STITCHING_NO_HTTP

}  // namespace

int start(const Config& cfg) {
    if (g_running.load()) {
        fprintf(stderr, "[http_server] already running\n");
        return 0;
    }
    g_camera_page_dir = cfg.camera_page_dir;
    // project_root: camera_page_dir 上一级
    size_t slash = g_camera_page_dir.find_last_of('/');
    g_project_root = (slash == std::string::npos) ? "." : g_camera_page_dir.substr(0, slash);

    g_thread = std::thread([cfg]() {
#ifdef IMAGE_STITCHING_NO_HTTP
        fprintf(stderr, "[http_server] DISABLED (cpp-httplib not vendored, run "
                "curl to fetch third_party/cpp-httplib/ then rebuild)\n");
#else
        run_server(cfg);
#endif
    });
    // 不 detach, 让 thread 自然 run. stop() 触发 svr.stop()
    return 0;
}

void stop() {
#ifdef IMAGE_STITCHING_NO_HTTP
    g_running.store(false);
#else
    // 需要访问 svr 实例才能 stop; 这里简化处理: 进程退出时自然终止
#endif
}

bool is_running() {
    return g_running.load();
}

}  // namespace http_server