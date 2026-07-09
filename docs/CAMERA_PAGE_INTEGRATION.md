# CameraPage × image-stitching 接入方案 (v2.4 → v3.0, 2026-07-08)

> **状态**: 阶段 1 完成（HTTP + 模拟 status）, 阶段 2（视频流）等 vpu 固件
> **板端运行**: PID 监控中, 端口 8080
> **架构**: C++ image-stitching 内嵌 cpp-httplib HTTP server + 独立 status writer 线程

阶段 1 已落地, 详细内容按"设计决策 / 当前状态 / API 端点 / 部署运维 / 局限"组织。**原 `CAMERAPAGE_TEST_DEPLOY.md` 已折叠入本文（删除了冗余独立的部署文档）**。

---

## 0. 设计决策（锁定 v2.4 → v3.0 仍生效）

| # | 决策项 | **最终选择** |
|---|---|---|
| 1 | 接入架构 | 方案 A：image-stitching 内嵌 HTTP server（单进程, 符合"板上自主"原则）|
| 2 | 视频流 | 方案 X：全分辨率 4613×3888 H.264 给浏览器（局域网带宽够）|
| 3 | 视频编码 | libx264（软编，现可用）/ **MPP h264_rkmpp**（vpu 修好后切）|
| 4 | RGA 缩放 | **完全删除**（NV12→NV12 不需要转换；方案 X 不缩放）|
| 5 | HTTP 库 | cpp-httplib（header-only, 零依赖, ~1MB）|
| 6 | WebSocket | WebSocket + MSE + fMP4（浏览器原生支持, 延迟 100-300ms）|
| 7 | 状态同步 | `/tmp/stitch_status.json`（POSIX 原子 rename）+ 独立 worker 线程 |
| 8 | 端口 | **8080**（复用 CameraPage 默认）|
| 9 | 算法管理模块 | v1 隐藏整个模块 |
| 10 | 网络配置模块 | 改读 `/sys/class/net/...` + `ip` 命令 |

---

## 1. 当前进度（2026-07-08）

| 阶段 | 内容 | 状态 | 备注 |
|---|---|---|---|
| **1. 阶段 1: 后端基础设施** | HTTP server + 状态桥接（模拟数据）| ✅ 完成 | 板端跑通（PID 监控中, 端口 8080）|
| 1a | `status_writer.h/cc`（独立线程 + 模拟）| ✅ | vpu/kmpp 卡死时, 500ms 写模拟 status |
| 1b | `http_server.h/cc`（cpp-httplib）| ✅ | CameraPage 静态 + 5 个 API 端点 |
| 1c | `third_party/cpp-httplib/httplib.h`（v0.49.0）| ✅ | header-only |
| 1d | CMakeLists.txt 集成 | ✅ | vendor 检测, 缺则降级 stub |
| 1e | 板端 `image-stitching` 构建通过 | ✅ | — |
| 1f | CameraPage 静态文件 serve | ✅ | `curl /` 返回 DOCTYPE |
| 1g | `/api/health` | ✅ | `{"ok": true}` |
| 1h | `/api/status`（JSON 模拟）| ✅ | 6 路 cam online, `simulated: true` |
| 1i | `/api/devices`（yaml 解析）| ✅ | 6 路 |
| 1j | `/api/config`（yaml 读）| ✅ | 完整 yaml |
| 1k | `/api/network`（sys/class/net）| ✅ | primary_ip |
| 1l | `POST /api/roi`（原子 yaml 写）| ✅ | tmp + rename 原子替换 |
| **2. 阶段 2: H.264 + fMP4 + WebSocket** | libx264 → fMP4 → WS → MSE | ⏳ 暂停 | 等 vpu 修好后改用 MPP；当前可继续 libx264 |
| **3. 阶段 3: CameraPage 前端** | 4 模块接真 API + `<video>` 替换 mock | ⏳ | 等阶段 2 |
| **4. 阶段 4: 真数据接入** | vpu/kmpp/ffmpeg 修好后 | ⏳ 等厂商 | 切 `g_have_real` 自动覆盖模拟 |
| **5. 阶段 5: 生产化** | 多客户端 / RTSP / 鉴权 / 热更新 | ⏳ 远期 | — |

**当前真实状态**：

- ✅ 后端基础架构完整（HTTP server, 5 个 API 端点, 板端进程在 8080 监听）
- ✅ CameraPage 静态文件 serve 正常（浏览器能打开, 但看到 mock 数据, 4 模块显示 fake setInterval）
- ⏳ 视频流：未做（浏览器看到 CameraPage mock 占位图）
- ⏳ 真实数据：等 vpu 修好才能看到真 stitch 结果（v3.0 后改用 RTSP, "真实"含义变化）

---

## 2. 板端硬件约束（2026-07-08 实测 + 2026-07-08 v3.0 更新）

| 项 | 状态 | 影响 |
|---|---|---|
| kernel 6.1.75 + Ubuntu 22.04 | ✅ OK | — |
| GC4683 驱动 v00.01.01 | ✅ OK | **v3.0 起不再直接驱动 GC4683**, 走 IP camera RTSP |
| ffmpeg 6.1（含 h264_rkmpp, --enable-rkmpp）| ⚠️ 编码器可识别但 VPU 缺固件 | 不能用 h264_rkmpp 编码 |
| libx264 v0.163（apt 装）| ✅ OK | 当前软编兜底 |
| 板载 DSI 屏（card0-DSI-…）| — | CameraPage 主要远程访问, 不依赖屏 |
| gstreamer1.0-rockchip1 mppvideodec | ✅ OK | **解码路径唯一硬约束** |
| **6 路 IP camera RTSP** | ✅ Sprint 0 yaml 装好, IPC 接通待 PoE 上电 | v3.0 输入 |

**注意**：v3.0 起, 项目主要输入源从"GC4683 MIPI"或"dataset mp4"统一改为"6 路 RTSP"。CameraPage `/api/cameras` 会显示真实 RTSP URI（`uri: rtsp://...`), 前端能区分。

---

## 3. 状态 JSON schema（v3.0 扩展）

`/tmp/stitch_status.json`（status_writer 每 500ms 原子 rename 写入）：

```json
{
  "num_cameras": 6,
  "mode": 1,             // 1=camera (yaml/rtsp), 0=dataset, 2=mipi(deprecated)
  "panorama_w": 4613,
  "panorama_h": 3888,
  "current_fps": 30.0,
  "frame_idx": 12345,
  "cams": [
    {"online": 1, "fps": 30, "is_rtsp": 1, "width": 2560, "height": 1440,
     "name": "cam0", "uri": "rtsp://admin:Abcd1234@192.168.10.21:554/Streaming/Channels/101"},
    ...
  ],
  "simulated": false
}
```

| 字段 | 取值 | 备注 |
|---|---|---|
| `online` | 0/1, `set_online(cam_idx, x)` 控制 | rtsp 拔线 / watchdog 重连中 → 0 |
| `fps` | 期望 fps（online=0 时也写 0）| 真实 decode fps 见 `decoder_perf` 日志 |
| `is_rtsp` | 0/1, 从 yaml `type: rtsp` 推断 | 让 CameraPage 把 rtsp 来源标出 |
| `uri` | 真实 rtsp URL（从 yaml 来）| 不再是 `../datasets/2k-test/camX.mp4` 占位 |
| `mode` | 1=camera, 0=dataset | CameraPage 用这个区分服务是不是走网络摄像头 |

---

## 4. 5 个 API 端点（板端 curl 验证示例, 2026-07-08）

```bash
# 健康检查
curl -s --max-time 3 http://localhost:8080/api/health
# {"ok": true, "ts": 17...}

curl -s --max-time 3 http://localhost:8080/api/status | head -c 500
# 含 6 路 cams + "simulated": true + frame_idx 持续递增

curl -s --max-time 3 http://localhost:8080/api/devices | head -c 400
# 6 路 cam from camera_sources.yaml

curl -s --max-time 3 http://localhost:8080/api/network | head -c 500
# primary_ip = 192.168.10.100 (eth0, v3.0 网段)

curl -s --max-time 3 http://localhost:8080/api/config | head -c 80
# %YAML:1.0 ...  (yaml 全文)

# POST 调 ROI（原子替换 yaml）
# v3.x (2026-07-09): 改成绝对 ROI. {x, y, width, height} 任意子集都可传, 没传的保留 yaml 原值.
curl -X POST http://localhost:8080/api/roi \
     -H "Content-Type: application/json" \
     -d ''"'"'{"cam":0,"x":100,"y":50,"width":2360,"height":1340}'"'"''
# {"ok":true,"message":"cam0 updated, yaml watcher will reload in <1s"}
grep -A 4 "cam0:" ~/Projects/new-4k-stitch/params/roi_tuning.yaml
# 应看到 cam0 下的 x/y/width/height 已被覆盖

# 静态文件
curl -s --max-time 3 http://localhost:8080/ | head -c 60
# DOCTYPE html ...
curl -s --max-time 3 http://localhost:8080/css/style.css | head -c 50
curl -s --max-time 3 http://localhost:8080/js/app.js | head -c 50
```

---

## 5. 部署运维（折叠自 `CAMERAPAGE_TEST_DEPLOY.md`, 2026-07-03）

### 5.1 PC 端推代码 + 板端 vendor cpp-httplib

```bash
# PC 端: 提交代码（third_party/ 已在 .gitignore）
cd g:/github/video-stitch/new-4k-stitch
git add include/status_writer.h include/http_server.h \
        src/status_writer.cc src/http_server.cc src/app.cc \
        CMakeLists.txt docs/CAMERA_PAGE_INTEGRATION.md

# third_party/ 不提交（700KB, 每次 clone 重新拉）
echo "third_party/" >> .gitignore
echo "third_party/.gitkeep" >> .gitignore
mkdir -p third_party && touch third_party/.gitkeep
git add .gitignore third_party/.gitkeep

git commit -m "v2.4 阶段 1: HTTP server + 模拟 status"
git push

# 板端 vendor cpp-httplib（国内可达推荐 gh-proxy.com）
ssh rocktech@<ip>
cd ~/Projects/new-4k-stitch
git pull
mkdir -p third_party/cpp-httplib
curl -fSL --max-time 60 \
  ''"'"'https://gh-proxy.com/https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h'"'"' \
  -o third_party/cpp-httplib/httplib.h
ls -la third_party/cpp-httplib/httplib.h   # 应 ~700KB
```

### 5.2 板端构建 + 启动

```bash
mkdir -p build && cd build
cmake ..    # 应看到 [cmake] Using vendored cpp-httplib at ...
make -j$(nproc)

# 关键: ENABLE_VISUAL_TUNING=0 (板端无 X server, SDL2 会拖崩)
ENABLE_VISUAL_TUNING=0 nohup ./image-stitching > /tmp/stitch.log 2>&1 &
echo "PID=$!"

sleep 3
ss -tlnp | grep 8080
# 预期: LISTEN ... :8080 ... image-stitching

tail -5 /tmp/stitch.log
# 预期最后几行: [http_server] Listening on 0.0.0.0:8080
```

### 5.3 浏览器访问

PC 浏览器打开：`http://<板子 IP>:8080/`

- ✅ 看到 CameraPage UI（4 模块：图像输出 / 网络配置 / 算法管理 / 设备管理）
- ✅ 顶部 hostname + IP（来自 `/api/network`, 但当前前端还没 fetch, 仍是 mock）
- ❌ 4 模块内部仍是 mock（前端 fetch 改造 = 阶段 3, 等阶段 2 视频流）

**实际有效验证**：浏览器能打开 → C++ HTTP server 在跑；DevTools Network tab 能看到 5 个 `/api/*` 调用。

### 5.4 停止

```bash
pkill -f image-stitching
rm -f /tmp/stitch.log /tmp/stitch_status.json
```

---

## 6. 已知限制（v2.4 阶段 1）

| 项 | 原因 | 影响 |
|---|---|---|
| `frame_idx` 持续递增但不是真帧率 | stitch loop 拿到的是 vpu 卡死时的模拟数据, worker 用模拟 | 浏览器看到 fps 在变, 但不是真实 |
| 6 路 cam 都是 `online=1` | 模拟数据没有真实 probe 状态 | 跟实际硬件状态无关 |
| CameraPage 4 模块内部还是 mock | 前端 fetch API 改造没做（阶段 3）| 看不到真实数据 |
| 看不到真实视频流 | 阶段 2 H.264 编码没做 | 浏览器看到 CameraPage 原 placeholder |
| 看不到真实拼接结果 | vpu 缺固件（v3.0 起 → RTSP 缺连通/凭据）| stitch loop 取不到帧 |

**v3.0 起**限制 5 解读变化：stitch loop 卡死不再是 vpu 驱动问题，而是 RTSP 连通 / 凭据 / 网络抖动问题。

---

## 7. 验证清单（硬指标）

| 项 | 状态 |
|---|---|
| 板端 C++ 编译通过 | ✅ `[100%] Built target image-stitching` |
| 板端进程能起 | ✅ PID 监控中 |
| HTTP server 监听 8080 | ✅ `ss -tlnp` |
| `/api/health` 返回 ok | ✅ `{"ok":true,...}` |
| `/api/status` 返回 6 路 + `simulated: true` | ✅ |
| `/api/devices` 返回 6 路（yaml 解析）| ✅ |
| `/api/config` 返回 yaml 全文 | ✅ |
| `/api/network` 返回 eth0 | ✅ |
| `POST /api/roi` 原子改 yaml | ✅ |
| CameraPage 静态首页可 serve | ✅ |
| CameraPage 4 模块 UI 渲染 | ✅（但显示 mock 数据）|
| 真实视频流 | ❌（阶段 2）|
| 真实 stitch 结果 | ❌（v3.0: 等 RTSP 联通）|

---

## 8. 工作量估算（修订）

| 阶段 | 工作量 | 关键补丁 |
|---|---|---|
| 阶段 1: HTTP + 状态桥接 | 3-4 天（**已完**）| vendor cpp-httplib + 序列号机制 + json 预序列化 |
| 阶段 2: MPP + fMP4 | 3-4 天 | 双 buffer + clEnqueueBarrier + stride 对齐 + WS 大消息 |
| 阶段 3: WebSocket + MSE | 2-3 天 | 单客户端限制 + init segment 重发 + ring buffer |
| 阶段 4: CameraPage 改造 | 2 天 | 不变 |
| 阶段 5: 测试 + 文档 | 1-2 天 | 多坑位测试（autoplay / 多标签 / 断网重连）|
| **合计** | **11-15 天** | |

---

## 9. 待扩展（v2+）

- 多客户端（WS broadcast + 每客户端 ring buffer）
- RTSP 输出（ffmpeg 拉 H.264 → RTSP server）
- H.265（带宽再降一半）
- HTTP 鉴权（login + JWT）
- 配置文件 Web 编辑
- 录像回放
- 算法扩展（ONNX/RKNN 6 路推理）

---

## 10. CameraPage 后续迭代建议（Sprint 1+）

1. **重连次数**：把 `[gst_mpp_decoder][watchdog] reconnect #N` 写到 status JSON, 让 CameraPage 显形拔线恢复
2. **延迟指标**：`rtsp latency_ms` 量化相机到板子延迟, 便于运维调延时
3. **丢包率**：rtspsrc 内部 GstMessage 可以 log, 加 status 字段
4. **温度 / CPU 占用**：板子 sysinfo（`/sys/class/thermal/thermal_zone*/temp`）

---

## 11. 引用

- **`docs/HISTORY.md`** § 踩过的坑 → §绿条纹（v3.0 RTSP 也会触发 stride 上报问题）
- **`docs/NETWORK_CAMERA_PLAN.md`** § 0.5 / § 7 / § 5.4 → RTSP 改造 + status_writer 接入 yaml
- **`docs/HISTORY.md`** § 操作手册 § SSH 连接 → 网络配置 nmcli（v3.0 网段是 192.168.10.x）