# CameraPage 测试后端部署指南 (v2.4 阶段 1, 2026-07-03)

> **状态**: 阶段 1 完成, 板端跑通 (PID 84321 在跑, 端口 8080)
> **架构**: C++ (image-stitching) 嵌入式 cpp-httplib HTTP server + 独立 status writer 线程 (模拟数据)
> **后续**: 阶段 2 加 H.264 编码 + WebSocket (暂停, 等 vpu 固件或用 libx264 软编绕过); 阶段 3 改 CameraPage 前端

## 0. 当前实现状态 (2026-07-03)

| 阶段 | 状态 | 备注 |
|---|---|---|
| 1. HTTP server + 状态桥接 (模拟) | ✅ 完成 | 板端 PID 84321, 5 个 API 端点全通 |
| 2. H.264 编码 + WebSocket | ⏸️ 暂停 | 等 vpu 修或走 libx264 软编 |
| 3. CameraPage 前端改造 | ❌ 未做 | 浏览器看到 CameraPage 原 mock |
| 4. 真数据接入 | ❌ 等厂商 | vpu 缺固件, stitch loop 取不到帧 |

**当前阶段能验证的**:
- ✅ C++ 嵌入式 HTTP server 跑通
- ✅ CameraPage 静态文件 serve
- ✅ 5 个 API 端点返回正确 JSON
- ✅ 6 路 cam 状态实时显示 (模拟数据, `simulated: true` 标记)
- ✅ 板端网络信息 (IP/MAC/接口)
- ✅ POST 改 yaml 原子生效
- ❌ 看不到真视频流 (浏览器看到 CameraPage 原 placeholder, 仍是静态图)
- ❌ 看不到真实拼接结果 (vpu 缺固件)

## 1. 涉及的文件

### 1.1 新增 (CameraPage 适配代码)

| 文件 | 行数估 | 说明 |
|---|---|---|
| [include/status_writer.h](../include/status_writer.h) | 50 | StitchStatus struct + init/update/shutdown API |
| [src/status_writer.cc](../src/status_writer.cc) | 180 | 独立 worker 线程 + 模拟数据 (500ms 写 status JSON) |
| [include/http_server.h](../include/http_server.h) | 30 | http_server::start/stop/is_running + Config |
| [src/http_server.cc](../src/http_server.cc) | 350 | cpp-httplib routes (5 API + CameraPage 静态) |
| [third_party/cpp-httplib/httplib.h](../third_party/cpp-httplib/httplib.h) | 700KB | cpp-httplib v0.49.0, header-only |

### 1.2 修改

| 文件 | 改动 |
|---|---|
| [src/app.cc](../src/app.cc) | main() 开头 `stitch_status::init()`; http server `start()` |
| [CMakeLists.txt](../CMakeLists.txt) | 加 status_writer.cc, http_server.cc 到 target; vendor cpp-httplib |

### 1.3 已废弃 (阶段 1 早期方案, 不用了)

| 文件 | 状态 |
|---|---|
| [scripts/camerapage_server.py](../scripts/camerapage_server.py) | ❌ 已被 C++ cpp-httplib 替代 |
| [deploy/camerapage_test_backend.sh](../deploy/camerapage_test_backend.sh) | ❌ 同上, 现在直接启 image-stitching 即可 |

## 2. 部署步骤 (PC → 板端)

### 2.1 PC 端: 拉 cpp-httplib (vendor)

```bash
# 方法 A: 板端用 gh-proxy.com 拉 (推荐, 国内可达)
ssh -i ~/.ssh/rockemb_board_key rocktech@rockemb "
  mkdir -p ~/Projects/new-4k-stitch/third_party/cpp-httplib
  cd ~/Projects/new-4k-stitch/third_party/cpp-httplib
  curl -fSL --max-time 60 \
    'https://gh-proxy.com/https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h' \
    -o httplib.h
  ls -la httplib.h   # 应 ~700KB
"

# 方法 B: PC 用浏览器下载 vendor/cpp-httplib-v0.49.0/httplib.h 进 third_party/
# 方法 C: 用 PC 直连 GitHub (要能访问 raw.githubusercontent.com)
```

**⚠️ 当前 PC 端 `third_party/cpp-httplib/` 是空的** (文件只在板端) — 后续其他开发者 clone 后需手动 vendor, 或写 `scripts/vendor_httplib.sh` 自动拉

### 2.2 PC 端: 提交代码

```bash
cd g:/github/video-stitch/new-4k-stitch
git add include/status_writer.h include/http_server.h \
        src/status_writer.cc src/http_server.cc src/app.cc \
        CMakeLists.txt docs/CAMERA_PAGE_INTEGRATION.md docs/CAMERAPAGE_TEST_DEPLOY.md

# third_party/ 不提交 (700KB, 每次 clone 都重新拉)
echo "third_party/" >> .gitignore
echo "!" -m "# allow .gitkeep" 
echo "third_party/.gitkeep" >> .gitignore
mkdir -p third_party && touch third_party/.gitkeep
git add .gitignore third_party/.gitkeep

git commit -m "v2.4 阶段 1: HTTP server + 模拟 status (cpp-httplib 嵌入式)"
git push
```

### 2.3 板端: 编译

```bash
ssh -i ~/.ssh/rockemb_board_key rocktech@rockemb
cd ~/Projects/new-4k-stitch
git pull

# 拉 httplib (按 2.1 的方法 A)
mkdir -p third_party/cpp-httplib
curl -fSL --max-time 60 'https://gh-proxy.com/https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h' \
  -o third_party/cpp-httplib/httplib.h
ls -la third_party/cpp-httplib/httplib.h   # 应 ~700KB

# 构建
mkdir -p build && cd build
cmake ..  # 应看到: [cmake] Using vendored cpp-httplib at ...
make -j$(nproc)
ls -la image-stitching
```

### 2.4 板端: 启动

```bash
cd ~/Projects/new-4k-stitch/build
# 关键: 必须设 ENABLE_VISUAL_TUNING=0 (板端无 X server, SDL2 会崩)
ENABLE_VISUAL_TUNING=0 nohup ./image-stitching > /tmp/stitch.log 2>&1 &
echo "PID=$!"

# 验证 HTTP 起来
sleep 3
ss -tlnp | grep 8080
```

**预期**:
- 进程在跑, PID 输出
- 端口 8080 监听 (`LISTEN ... :8080 ... image-stitching`)
- 日志最后几行有 `[http_server] Listening on 0.0.0.0:8080`

### 2.5 验证

```bash
# 进程在
pgrep -af image-stitching | head -2

# API 全部应该返回 JSON
curl -s --max-time 3 http://localhost:8080/api/health
# {"ok": true, "ts": 17...}

curl -s --max-time 3 http://localhost:8080/api/status | head -c 500
# 含 6 路 cams + "simulated": true + frame_idx 持续递增

curl -s --max-time 3 http://localhost:8080/api/devices | head -c 400
# 6 路 cam from camera_sources.yaml

curl -s --max-time 3 http://localhost:8080/api/network | head -c 500
# 含 primary_ip = 192.168.137.100 (eth0) + hostname=rockemb

curl -s --max-time 3 http://localhost:8080/api/config | head -3
# yaml 内容 (%YAML:1.0 ...)

# 静态文件
curl -s --max-time 3 http://localhost:8080/ | head -3
# DOCTYPE html ...
curl -s --max-time 3 http://localhost:8080/css/style.css | head -c 50
curl -s --max-time 3 http://localhost:8080/js/app.js | head -c 50

# POST 测试
curl -X POST http://localhost:8080/api/roi -H "Content-Type: application/json" -d '{"cam":0,"offset_x":5,"offset_y":-3}'
# {"ok":true,"message":"cam0 updated, restart image-stitching to apply"}
grep "offset_x" ~/Projects/new-4k-stitch/params/roi_tuning.yaml
# 应看到 cam0: offset_x: 5
```

### 2.6 PC 浏览器打开

```
http://192.168.137.100:8080/
```

**能看到什么**:
- ✅ CameraPage UI (4 模块: 图像输出 / 网络配置 / 算法管理 / 设备管理)
- ✅ 顶部 hostname=rockemb + IP 192.168.137.100 (来自 /api/network, 但前端没 fetch, 还是 mock)
- ❌ 4 模块内部数据仍是 mock (前端 fetch API 改造未做)

**实际有用的验证**:
- 浏览器能打开 → C++ HTTP server 跑通
- DevTools Network tab 能看到 `/api/status` 等 5 个 API 调用
- 用 curl 直测 API 验证数据正确

### 2.7 停止

```bash
pkill -f image-stitching
rm -f /tmp/stitch.log /tmp/stitch_status.json
```

## 3. 已知限制 (v2.4 阶段 1)

| 项 | 原因 | 影响 |
|---|---|---|
| `frame_idx` 持续递增但不是真帧 | stitch loop 卡 vpu, worker 用模拟数据 | 浏览器看到 fps 在变, 但不是真数据 |
| 6 路 cam 都是 `online=1` | 模拟数据没有真实 probe 状态 | 跟实际硬件状态无关 |
| CameraPage 4 模块内部还是 mock | 前端 fetch API 改造没做 (阶段 3) | 看不到真数据 |
| 看不到真视频流 | 阶段 2 H.264 编码没做 | 浏览器看到 CameraPage 原 placeholder |
| 看不到真拼接结果 | vpu 缺固件 (厂商待修) | stitch loop 取不到帧 |

## 4. 验证清单

| 项 | 状态 |
|---|---|
| 板端 C++ 编译通过 | ✅ `[100%] Built target image-stitching` |
| 板端进程能起 | ✅ PID 84321 |
| HTTP server 监听 8080 | ✅ `ss -tlnp` |
| `/api/health` 返回 ok | ✅ `{"ok":true,...}` |
| `/api/status` 返回 6 路 cam + `simulated: true` | ✅ |
| `/api/devices` 返回 6 路 (从 yaml 解析) | ✅ |
| `/api/config` 返回 yaml 全文 | ✅ |
| `/api/network` 返回 eth0 (192.168.137.100) | ✅ |
| `POST /api/roi` 原子改 yaml | ✅ |
| CameraPage 静态首页可 serve | ✅ |
| CameraPage 4 模块 UI 渲染 | ✅ (但显示 mock 数据) |
| 真视频流 | ❌ (阶段 2) |
| 真 stitch 结果 | ❌ (vpu 缺) |

## 5. 下一阶段 (待做)

| 阶段 | 工作量 | 阻塞 |
|---|---|---|
| 2. H.264 + fMP4 + WebSocket | 3-4 天 (libx264 软编) 或 等 vpu 走 MPP | 当前暂停 |
| 3. CameraPage 前端 fetch API 改造 | 2 天 | 等阶段 2 (视频流) |
| 4. 真数据接入 | 0.5 天 (改几行) | 等厂商修 vpu/kmpp |
| 5. 生产化 (多客户端/RTSP/鉴权) | 5+ 天 | 远期 |