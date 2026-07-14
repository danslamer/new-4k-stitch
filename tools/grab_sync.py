#!/usr/bin/env python3
"""
grab_sync.py — 同步采集当前所有可用 GC4683 视频，按 video node 命名保存。

特点:
  - 自动探测 /dev/video* 里 BA10 (10-bit packed Bayer GRBG) 可用的节点
  - 多线程 + threading.Barrier 几乎同时启动 4 路 v4l2-ctl
  - 每路默认 10s × 30fps = 300 帧, raw BA10 文件约 1.32 GB / 路
  - 可选 --encode mp4 用 ffmpeg 边抓边编码

用法:
  ./grab_sync.py                                      # 默认 10s raw, 自动探测
  ./grab_sync.py --duration 30 --fps 25 --encode mp4  # 30s @25fps mp4
  ./grab_sync.py --cams video11,video22,video33,video44  # 指定节点

依赖:
  - v4l2-ctl (v4l-utils)
  - ffmpeg (仅 --encode mp4 需要)
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from datetime import datetime

DEFAULT_WIDTH = 2560
DEFAULT_HEIGHT = 1440
DEFAULT_FPS = 30
DEFAULT_DURATION = 10


# ---------------------------------------------------------------------------
# cam id 推断: 从 sysfs / DT 找 videoN ↔ i2c 总线 ↔ cam 编号
# ---------------------------------------------------------------------------

def _read_file(path: str) -> str:
    try:
        with open(path) as f:
            return f.read()
    except Exception:
        return ""


def build_i2c_to_cam_map() -> dict:
    """返回 {i2c_bus_num: cam_id}, 从 sysfs 读 gc4683_N DT node name 推断 cam 编号.

    约定: gc4683      → cam1
           gc4683_1    → cam2
           gc4683_2    → cam3
           ...
           gc4683_5    → cam6
    """
    cam_map = {}
    base = "/sys/bus/i2c/devices"
    if not os.path.isdir(base):
        return cam_map
    for entry in sorted(os.listdir(base)):
        m = re.match(r"^(\d+)-[0-9a-f]+$", entry)
        if not m:
            continue
        bus = int(m.group(1))
        of_link = f"{base}/{entry}/of_node"
        if not os.path.exists(of_link):
            continue
        of_path = os.path.realpath(of_link)
        mm = re.search(r"gc4683(?:_(\d+))?@", of_path)
        if mm:
            suffix = mm.group(1)
            cam_id = f"cam{int(suffix) + 1}" if suffix is not None else "cam1"
            cam_map[bus] = cam_id
    return cam_map


def build_video_to_cam_map(cam_map: dict) -> dict:
    """通过 media-ctl 拓扑, 解析 videoN ↔ sensor entity ↔ i2c 总线 ↔ cam.

    每个 /dev/mediaN 对应一个 csi2 pipeline, 里面只有一个 sensor entity
    (m00_f_gc4683 N-XXXX), 其 video output (stream_cif_mipi_id0 → /dev/videoN)
    唯一对应那个 sensor.

    返回 {"/dev/videoN": cam_id_or_None}.
    """
    result = {}
    for m in range(20):
        media = f"/dev/media{m}"
        if not os.path.exists(media):
            break
        try:
            txt = subprocess.run(
                ["media-ctl", "-d", media, "-p"],
                capture_output=True, text=True, timeout=5,
            ).stdout
        except Exception:
            continue
        if "rkcif" not in txt:
            continue

        # 1. 找 sensor entity 名字, 提取 i2c bus
        #    "entity X: m00_f_gc4683 2-0010 (1 pad, ..."
        sm = re.search(
            r"gc4683\s+(\d+)-[0-9a-f]+",
            txt,
        )
        if not sm:
            continue
        i2c_bus = int(sm.group(1))

        # 2. 找 stream_cif_mipi_id0 对应的 /dev/videoN (主输出, 用于 v4l2-ctl 抓帧)
        vm = re.search(
            r"entity\s+\d+:\s+stream_cif_mipi_id0[^\n]*\n[^\n]*\n"
            r"[^\n]*device node name\s+(\S+)",
            txt,
        )
        if not vm:
            continue
        video_node = vm.group(1)

        cam_id = cam_map.get(i2c_bus)
        if cam_id:
            result[video_node] = cam_id
    return result


def detect_working_cams(min_index: int = 0, max_index: int = 200) -> list:
    """扫描 /dev/videoN, 返回能 BA10 输出的 video node 列表。"""
    cams = []
    for v in range(min_index, max_index):
        node = f"/dev/video{v}"
        if not os.path.exists(node):
            continue
        try:
            r = subprocess.run(
                ["v4l2-ctl", "-d", node, "--get-fmt-video"],
                capture_output=True, timeout=2, text=True,
            )
            if "BA10" in r.stdout:
                cams.append(node)
        except Exception:
            pass
    return cams


def grab_raw(video_node: str, output_file: str, fps: int, duration: int):
    """v4l2-ctl 抓 raw BA10, 直接落盘."""
    count = fps * duration
    cmd = [
        "v4l2-ctl", "-d", video_node,
        "--stream-mmap=3",
        f"--stream-count={count}",
        f"--stream-to={output_file}",
    ]
    return subprocess.run(cmd, capture_output=True, text=True)


def grab_mp4(video_node: str, output_file: str, fps: int, duration: int,
             width: int, height: int):
    """ffmpeg 从 /dev/videoX 抓 rawvideo 然后编码 libx264 mp4."""
    # 注: ffmpeg v4l2 输入需要 rawvideo 像素格式, 我们这里用 bayer_grbg10le
    # 板子上 ffmpeg 必须带 v4l2 + libx264 支持
    cmd = [
        "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
        "-f", "v4l2",
        "-input_format", "rawvideo",
        "-pix_fmt", "bayer_grbg10le",
        "-video_size", f"{width}x{height}",
        "-framerate", str(fps),
        "-i", video_node,
        "-t", str(duration),
        "-c:v", "libx264", "-preset", "ultrafast", "-crf", "23",
        "-pix_fmt", "yuv420p",
        output_file,
    ]
    return subprocess.run(cmd, capture_output=True, text=True)


def grab_one(idx: int, cam: str, out_path: str, fps: int, duration: int,
             encode: str, barrier: threading.Barrier, results: list):
    """thread worker: barrier 同步 + 抓帧 + 落盘."""
    results[idx] = {"cam": cam, "ok": False, "size": 0, "elapsed": 0.0, "err": None}
    try:
        # barrier 等所有 worker 准备好, 一起放行
        barrier.wait(timeout=10)
    except threading.BrokenBarrierError:
        results[idx]["err"] = "barrier timeout"
        return

    t0 = time.monotonic()
    if encode == "raw":
        r = grab_raw(cam, out_path, fps, duration)
    else:
        r = grab_mp4(cam, out_path, fps, duration, DEFAULT_WIDTH, DEFAULT_HEIGHT)
    t1 = time.monotonic()

    results[idx]["ok"] = (r.returncode == 0)
    results[idx]["elapsed"] = t1 - t0
    results[idx]["size"] = os.path.getsize(out_path) if os.path.exists(out_path) else 0
    if r.returncode != 0:
        results[idx]["err"] = (r.stderr or r.stdout or "")[:200]


def main():
    ap = argparse.ArgumentParser(
        description="同步采集多路 GC4683 raw/mp4 视频（按 cam 编号命名）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--duration", type=int, default=DEFAULT_DURATION,
                    help=f"采集时长（秒），默认 {DEFAULT_DURATION}")
    ap.add_argument("--fps", type=int, default=DEFAULT_FPS,
                    help=f"帧率，默认 {DEFAULT_FPS}")
    ap.add_argument("--output-dir", default="/home/rocktech/Projects/new-4k-stitch/results/grab",
                    help="输出目录")
    ap.add_argument("--encode", choices=["raw", "mp4"], default="raw",
                    help="raw=原始 BA10 (~1.3GB/路/10s)；mp4=H264 编码 (需 ffmpeg)")
    ap.add_argument("--cams", default="",
                    help="指定 video node, 逗号分隔 (e.g. video11,video22)；默认自动探测")
    ap.add_argument("--no-cam-name", action="store_true",
                    help="用 video node 命名 (videoN) 而非 cam 编号 (cam1..cam6)")
    args = ap.parse_args()

    if args.encode == "mp4" and not shutil.which("ffmpeg"):
        print("ERROR: --encode mp4 需要 ffmpeg, 但板子上没找到 ffmpeg")
        sys.exit(1)

    if args.cams:
        cams = [f"/dev/{c.strip().lstrip('/dev/')}" if not c.startswith("/dev/")
                else c for c in args.cams.split(",") if c.strip()]
    else:
        cams = detect_working_cams()
    if not cams:
        print("ERROR: 没探测到任何 BA10 video node")
        sys.exit(1)

    # cam 编号映射
    i2c_to_cam = build_i2c_to_cam_map()
    video_to_cam = build_video_to_cam_map(i2c_to_cam)
    print(f"  video→cam 映射: {video_to_cam}")

    os.makedirs(args.output_dir, exist_ok=True)
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    ext = "raw" if args.encode == "raw" else "mp4"

    print(f"=== grab_sync 启动 ===")
    print(f"  duration={args.duration}s  fps={args.fps}  encode={args.encode}")
    print(f"  output_dir={args.output_dir}")
    print(f"  cams ({len(cams)}): {cams}")

    barrier = threading.Barrier(len(cams))
    results = [None] * len(cams)
    manifest = []
    threads = []
    for i, cam_node in enumerate(cams):
        cam_id = video_to_cam.get(cam_node)
        # 文件名优先 cam 编号, fallback video node
        if args.no_cam_name or not cam_id:
            tag = os.path.basename(cam_node)
        else:
            tag = cam_id
        out_path = os.path.join(args.output_dir, f"{ts}_{tag}.{ext}")
        manifest.append({
            "file": os.path.basename(out_path),
            "video_node": cam_node,
            "cam_id": cam_id,
        })
        t = threading.Thread(
            target=grab_one,
            args=(i, cam_node, out_path, args.fps, args.duration, args.encode, barrier, results),
            name=f"grab-{os.path.basename(cam_node)}",
        )
        t.start()
        threads.append(t)

    for t in threads:
        t.join()

    # 报告
    total_size = 0
    print(f"\n=== 完成 ===")
    for r in results:
        if r["ok"]:
            mb = r["size"] / 1e6
            total_size += r["size"]
            cam_id = video_to_cam.get(r["cam"]) or "?"
            print(f"  ✅ {cam_id:<6s} ({r['cam']:<13s})  {mb:8.1f} MB  {r['elapsed']:5.2f}s")
        else:
            print(f"  ❌ {r['cam']:<13s}  失败  err={r.get('err', '')!r}")
    print(f"  总计: {total_size/1e9:.2f} GB")
    print(f"  文件位于: {args.output_dir}/")

    # 写 manifest
    manifest_path = os.path.join(args.output_dir, f"{ts}_manifest.json")
    with open(manifest_path, "w") as f:
        json.dump({
            "timestamp": ts,
            "duration_s": args.duration,
            "fps": args.fps,
            "encode": args.encode,
            "i2c_to_cam": i2c_to_cam,
            "video_to_cam": video_to_cam,
            "sessions": manifest,
        }, f, indent=2, ensure_ascii=False)
    print(f"  manifest: {manifest_path}")


if __name__ == "__main__":
    main()