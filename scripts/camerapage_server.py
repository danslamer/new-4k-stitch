#!/usr/bin/env python3
"""camerapage_server.py - 江苏攸洋智控管理平台 HTTP 后端 (v2.3 测试版)

职责:
  1. 静态服务 CameraPage 4 个页面 (index.html + css + js)
  2. /api/status   -> 读 /tmp/stitch_status.json 返回
  3. /api/devices  -> 从 CameraSourceList 等价数据生成 (v1 测试: 直接读 yaml)
  4. /api/config   -> 读 params/roi_tuning.yaml
  5. /api/network  -> 读 /sys/class/net/{eth0,end1,wlan0} 等板端网络
  6. POST /api/roi  -> 写 roi_tuning.yaml (新 ROI 配置)

部署:
  python3 scripts/camerapage_server.py [PORT]
  默认 PORT=8080

依赖:
  - Python 3.8+ (仅用 stdlib, 无 pip 依赖)
  - image-stitching 进程必须已启动并写入 /tmp/stitch_status.json
"""
import json
import os
import re
import socket
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

# ============ 配置 ============

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080

# CameraPage 静态文件目录 (相对脚本)
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
CAMERAPAGE_DIR = os.path.join(PROJECT_ROOT, "CameraPage")

# C++ 端写的 status 文件
STATUS_JSON_PATH = "/tmp/stitch_status.json"

# ROI yaml 路径
ROI_YAML_PATH = os.path.join(PROJECT_ROOT, "params", "roi_tuning.yaml")

# ============ MIME 映射 ============

MIME_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".htm":  "text/html; charset=utf-8",
    ".css":  "text/css; charset=utf-8",
    ".js":   "application/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".png":  "image/png",
    ".svg":  "image/svg+xml",
    ".ico":  "image/x-icon",
}

# ============ 工具函数 ============

def read_file(path):
    """读文件, 不存在返回 None"""
    try:
        with open(path, "r", encoding="utf-8") as f:
            return f.read()
    except (FileNotFoundError, PermissionError):
        return None

def read_file_bytes(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except (FileNotFoundError, PermissionError):
        return None

def read_status_json():
    """读 C++ 端写的 status, 缓存 200ms"""
    if not hasattr(read_status_json, "_cache"):
        read_status_json._cache = None
        read_status_json._time = 0.0

    now = time.time()
    if now - read_status_json._time < 0.2 and read_status_json._cache is not None:
        return read_status_json._cache

    content = read_file(STATUS_JSON_PATH)
    if content:
        try:
            data = json.loads(content)
            read_status_json._cache = data
            read_status_json._time = now
            return data
        except json.JSONDecodeError:
            pass
    return None

# ============ 网络信息采集 ============

def get_network_info():
    """从 /sys/class/net 读板端网络状态"""
    result = {"interfaces": []}

    net_dir = "/sys/class/net"
    if not os.path.isdir(net_dir):
        return result

    for iface in sorted(os.listdir(net_dir)):
        if iface == "lo":  # 跳过 loopback
            continue
        info = {"name": iface}
        # MAC
        try:
            with open(f"{net_dir}/{iface}/address") as f:
                info["mac"] = f.read().strip()
        except Exception:
            info["mac"] = ""
        # IP (从 /proc/net/fib_trie 不好解析, 改用 socket)
        info["ip"] = ""
        try:
            import subprocess
            out = subprocess.check_output(
                ["ip", "-4", "-o", "addr", "show", "dev", iface],
                stderr=subprocess.DEVNULL, timeout=2
            ).decode()
            m = re.search(r"inet\s+(\d+\.\d+\.\d+\.\d+)", out)
            if m:
                info["ip"] = m.group(1)
        except Exception:
            pass
        # 状态
        try:
            with open(f"{net_dir}/{iface}/operstate") as f:
                info["state"] = f.read().strip()
        except Exception:
            info["state"] = "unknown"
        result["interfaces"].append(info)

    # 默认 IP: 第一个 up 的非 lo 接口
    for iface in result["interfaces"]:
        if iface.get("state") == "up" and iface.get("ip"):
            result["primary_ip"] = iface["ip"]
            result["primary_iface"] = iface["name"]
            break
    else:
        result["primary_ip"] = ""
        result["primary_iface"] = ""

    # hostname
    try:
        result["hostname"] = socket.gethostname()
    except Exception:
        result["hostname"] = "unknown"

    return result

# ============ HTTP 处理器 ============

class Handler(BaseHTTPRequestHandler):
    """单 handler 处理所有路由"""

    def log_message(self, fmt, *args):
        # 简化的访问日志
        sys.stderr.write(f"[{time.strftime('%H:%M:%S')}] {fmt % args}\n")

    # ----- 通用响应工具 -----

    def send_json(self, data, status=200):
        body = json.dumps(data, ensure_ascii=False, indent=2).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")  # 允许跨域
        self.end_headers()
        self.wfile.write(body)

    def send_text(self, text, status=200, content_type="text/plain; charset=utf-8"):
        body = text.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def send_file(self, path):
        """发送本地文件, 按扩展名推断 MIME"""
        ext = os.path.splitext(path)[1].lower()
        mime = MIME_TYPES.get(ext, "application/octet-stream")
        body = read_file_bytes(path)
        if body is None:
            self.send_text("404 Not Found", status=404)
            return
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(body)

    # ----- 路由分发 -----

    def do_GET(self):
        path = urlparse(self.path).path
        # 静态文件 (CameraPage)
        if path in ("/", "/index.html"):
            self.send_file(os.path.join(CAMERAPAGE_DIR, "index.html"))
            return
        if path.startswith("/css/") or path.startswith("/js/"):
            full_path = os.path.join(CAMERAPAGE_DIR, path.lstrip("/"))
            if os.path.isfile(full_path):
                self.send_file(full_path)
            else:
                self.send_text("404 Not Found", status=404)
            return
        # API
        if path == "/api/status":
            data = read_status_json()
            if data is None:
                # C++ 还没写 status 文件, 返回空但保留 schema
                self.send_json({
                    "num_cameras": 0, "mode": 0,
                    "panorama_w": 0, "panorama_h": 0,
                    "current_fps": 0.0, "frame_idx": 0,
                    "timestamp_us": 0,
                    "blend_ms": 0, "warp_ms": 0,
                    "cams": [],
                    "warning": "status file not yet written by C++"
                })
            else:
                self.send_json(data)
            return
        if path == "/api/devices":
            self.handle_devices()
            return
        if path == "/api/config":
            content = read_file(ROI_YAML_PATH)
            if content is None:
                self.send_text("# roi_tuning.yaml not found", status=404)
            else:
                self.send_text(content, content_type="text/yaml; charset=utf-8")
            return
        if path == "/api/network":
            self.send_json(get_network_info())
            return
        if path == "/api/health":
            self.send_json({"ok": True, "ts": int(time.time())})
            return
        # 兜底
        self.send_text("404 Not Found", status=404)

    def do_POST(self):
        path = urlparse(self.path).path
        if path == "/api/roi":
            self.handle_roi_post()
            return
        if path == "/api/echo":
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length).decode("utf-8") if length else ""
            self.send_json({"echo": body})
            return
        self.send_text("404 Not Found", status=404)

    # ----- 具体 handler -----

    def handle_devices(self):
        """从 roi_tuning.yaml + camera_sources.yaml 解析 6 路设备状态"""
        devices = []

        # 优先从 camera_sources.yaml 拿 uri
        cam_yaml = read_file(os.path.join(PROJECT_ROOT, "params", "camera_sources.yaml"))
        sources = []
        if cam_yaml:
            for line in cam_yaml.splitlines():
                m = re.match(r"\s*-\s+type:\s*(\S+)", line)
                if m:
                    sources.append({"type": m.group(1), "uri": "", "width": 0, "height": 0})
                m = re.match(r"\s*uri:\s*(.+)", line)
                if m and sources:
                    sources[-1]["uri"] = m.group(1).strip()
                m = re.match(r"\s*width:\s*(\d+)", line)
                if m and sources:
                    sources[-1]["width"] = int(m.group(1))
                m = re.match(r"\s*height:\s*(\d+)", line)
                if m and sources:
                    sources[-1]["height"] = int(m.group(1))

        # 拿 C++ 实时状态 (online/fps)
        status = read_status_json() or {}
        cams_status = status.get("cams", [])

        for i, src in enumerate(sources[:6]):
            cs = cams_status[i] if i < len(cams_status) else {}
            devices.append({
                "id": i,
                "name": f"cam{i}",
                "type": src.get("type", "file"),
                "uri": src.get("uri", ""),
                "width": src.get("width", cs.get("width", 0)),
                "height": src.get("height", cs.get("height", 0)),
                "online": cs.get("online", 0),
                "fps": cs.get("fps", 0),
            })

        self.send_json({
            "devices": devices,
            "total": len(devices),
            "timestamp_us": status.get("timestamp_us", 0),
        })

    def handle_roi_post(self):
        """POST /api/roi: body = {"cam": 0, "x": 100, "y": 200, "width": 1920, "height": 1080}
        v3.x (2026-07-09): 改成绝对 ROI. {x, y, width, height} 任意子集, 没传字段保留 yaml 原值.
          旧的 offset_x/offset_y 不再读, 用旧 API 会直接返回 400.
        v3.x.1 (2026-07-09): 新增 "affine": [a, b, c, d, tx, ty] 字段. 6 个数字.
          不传 → 保留 yaml 原值. 与 x/y/width/height 独立, 可以只改 affine 不动 ROI.
        """
        length = int(self.headers.get("Content-Length", 0))
        if length == 0 or length > 4096:
            self.send_text("Invalid request body", status=400)
            return
        try:
            body = json.loads(self.rfile.read(length).decode("utf-8"))
        except json.JSONDecodeError as e:
            self.send_text(f"Invalid JSON: {e}", status=400)
            return

        cam_id = body.get("cam")
        new_x = body.get("x")
        new_y = body.get("y")
        new_w = body.get("width")
        new_h = body.get("height")
        new_affine = body.get("affine")
        if not isinstance(cam_id, int) or cam_id < 0 or cam_id >= 6:
            self.send_text("cam must be int 0..5", status=400)
            return

        # 校验 affine: 必须是长度为 6 的 list, 元素都是数字
        if new_affine is not None:
            if (not isinstance(new_affine, list)) or len(new_affine) != 6:
                self.send_text("affine must be list of 6 numbers [a, b, c, d, tx, ty]", status=400)
                return
            for v in new_affine:
                if not isinstance(v, (int, float)):
                    self.send_text("affine elements must be numbers", status=400)
                    return

        # 至少要传一个字段
        if all(v is None for v in (new_x, new_y, new_w, new_h)) and new_affine is None:
            self.send_text("need at least one of x/y/width/height/affine", status=400)
            return

        # 读 yaml, 修改, 写回 (用临时文件 + rename 原子替换)
        content = read_file(ROI_YAML_PATH)
        if content is None:
            self.send_text("roi_tuning.yaml not found", status=404)
            return

        key = f"cam{cam_id}:"
        lines = content.splitlines()
        new_lines = []
        in_target = False
        replaced = False
        affine_inserted = False  # 跟踪 affine 是替换还是插入

        def maybe_replace(line, field, value):
            """in_target 时, 如果 line 是 field: 开头, 替换之; 否则原样返回"""
            nonlocal replaced
            if value is None:
                return line
            if re.match(rf"^\s*{field}\s*:", line):
                replaced = True
                if field == "affine":
                    formatted = ", ".join(f"{float(v):.6f}" for v in value)
                    return f"   affine: [{formatted}]"
                return f"   {field}: {int(value)}"
            return line

        for line in lines:
            if re.match(rf"^\s*{key}\s*$", line):
                in_target = True
                new_lines.append(line)
                continue
            if in_target:
                if re.match(r"^\s*cam\d+:", line):
                    in_target = False
                else:
                    line = maybe_replace(line, "x", new_x)
                    line = maybe_replace(line, "y", new_y)
                    line = maybe_replace(line, "width", new_w)
                    line = maybe_replace(line, "height", new_h)
                    line = maybe_replace(line, "affine", new_affine)
            new_lines.append(line)

        # 处理 affine 是新增 (yaml 里没有 affine 行) 的情况: 在 cam 块末尾插入
        if new_affine is not None and not replaced:
            # 重新扫一遍, 这次专门做 affine 插入
            out_lines = []
            in_target = False
            for line in new_lines:
                if re.match(rf"^\s*{key}\s*$", line):
                    in_target = True
                    out_lines.append(line)
                    continue
                if in_target:
                    if re.match(r"^\s*cam\d+:", line):
                        # 到达下一个 cam 块, 在这里之前先插入 affine
                        if not affine_inserted:
                            formatted = ", ".join(f"{float(v):.6f}" for v in new_affine)
                            out_lines.append(f"   affine: [{formatted}]")
                            affine_inserted = True
                            replaced = True
                        in_target = False
                out_lines.append(line)
            # 如果 cam 是最后一个 cam 块, 上面不会触发; 在末尾追加
            if in_target and not affine_inserted:
                formatted = ", ".join(f"{float(v):.6f}" for v in new_affine)
                out_lines.append(f"   affine: [{formatted}]")
                affine_inserted = True
                replaced = True
            new_lines = out_lines

        if not replaced:
            self.send_text(f"cam{cam_id} not found in yaml", status=404)
            return

        new_content = "\n".join(new_lines) + "\n"
        tmp_path = ROI_YAML_PATH + ".tmp"
        try:
            with open(tmp_path, "w", encoding="utf-8") as f:
                f.write(new_content)
            os.replace(tmp_path, ROI_YAML_PATH)  # 原子替换
        except OSError as e:
            self.send_text(f"Write failed: {e}", status=500)
            return

        # v3.x: C++ 端有 RoiYamlWatcher (500ms 轮询 mtime), <1s 内 RebuildLayout 自动生效.
        # v3.x.1: affine 改动也会触发 RebuildLayout (内含 SetWarpData 重算 xmap/ymap).
        self.send_json({
            "ok": True,
            "message": f"cam{cam_id} updated, yaml watcher will reload in <1s",
            "cam": cam_id,
            "x": new_x,
            "y": new_y,
            "width": new_w,
            "height": new_h,
            "affine": new_affine,
        })

# ============ 启动 ============

def main():
    # 检查 CameraPage 目录存在
    if not os.path.isdir(CAMERAPAGE_DIR):
        print(f"ERROR: CameraPage dir not found: {CAMERAPAGE_DIR}", file=sys.stderr)
        sys.exit(1)
    if not os.path.isfile(os.path.join(CAMERAPAGE_DIR, "index.html")):
        print(f"ERROR: CameraPage/index.html not found", file=sys.stderr)
        sys.exit(1)

    server = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print(f"[CameraPage Server] Listening on 0.0.0.0:{PORT}", file=sys.stderr)
    print(f"[CameraPage Server] CameraPage dir: {CAMERAPAGE_DIR}", file=sys.stderr)
    print(f"[CameraPage Server] Status file: {STATUS_JSON_PATH}", file=sys.stderr)
    print(f"[CameraPage Server] ROI yaml: {ROI_YAML_PATH}", file=sys.stderr)

    # 拿本机 IP 给用户参考
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        local_ip = s.getsockname()[0]
        s.close()
        print(f"[CameraPage Server] Local IP: {local_ip}", file=sys.stderr)
        print(f"[CameraPage Server] Browser URL: http://{local_ip}:{PORT}/", file=sys.stderr)
    except Exception:
        pass

    print(f"[CameraPage Server] Press Ctrl+C to stop", file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print(f"\n[CameraPage Server] Shutting down...", file=sys.stderr)
        server.shutdown()

if __name__ == "__main__":
    main()