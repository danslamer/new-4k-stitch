# Quick start: "改一下 IP 就能用" (5 步)

> 6 路 RTSP 网络摄像头 → 拼接 → 出图.
> **最快路径: 一行命令改 IP + 启动**, 3 步走完.

## 1. 编译 (一次)

SSH 上板后:

```bash
cd ~/Projects/new-4k-stitch
mkdir -p build && cd build && cmake .. && make -j$(nproc) && cd ..
```

依赖 (板端镜像应已装): OpenCV ≥ 4.5, gstreamer1.0-rockchip1 (mppvideodec), OpenCL, EGL, GLESv2, GBM, librga, libdrm, SDL2.

## 2. 改 IP + 启动 (任选)

### 方式 A: 一行命令 (最快)

```bash
IP_LIST="192.168.0.123,192.168.0.124,192.168.0.125,192.168.0.126,192.168.0.127,192.168.0.128" \
    RTSP_USER=admin RTSP_PASSWORD=123456 RTSP_PATH=/video1 \
    bash tools/run_rtsp_stitching.sh
```

脚本自动:
- 改 yaml 的 6 个 IP + user + password + path (其它字段保留)
- 备份 yaml 到 .bak.<时间戳>
- 设 `INPUT_SOURCE_MODE=camera`
- 关 SDL 可视化 (板端没 X)
- 跑 `./build/image-stitching`

### 方式 B: 文件 (适合 git 化, 改一次长期生效)

```bash
cat > cams.txt <<EOF
192.168.0.123
192.168.0.124
192.168.0.125
192.168.0.126
192.168.0.127
192.168.0.128
EOF

bash tools/run_rtsp_stitching.sh
```

后续改 IP: `vim cams.txt && bash tools/run_rtsp_stitching.sh`

### 方式 C: 直接编辑 yaml (传统)

```bash
bash tools/set_cam_ips.sh \
    --user admin --password 123456 --path /video1 \
    192.168.0.123,192.168.0.124,192.168.0.125,192.168.0.126,192.168.0.127,192.168.0.128

bash tools/run_rtsp_stitching.sh
```

## 3. 看 (可选)

```bash
# 主线程: stitch FPS (稳态 ~25-30)
tail -f /tmp/image-stitching.log | grep "STITCH_PERF_FPS\|decode_fps"

# 6 路 RTSP 健康 (字段反映 decoder 真实状态)
curl -s http://localhost:8080/api/status | python3 -m json.tool | head -50
# 或直接读
cat /tmp/stitch_status.json

# CameraPage (如启用)
http://<board-ip>:8080/

# 单路烟测 (排查链路某个 cam)
bash tools/rtsp_url_probe.sh

# 烧新镜像后一键体检
bash tools/post_flash_test.sh
```

## 改 IP 后想再改?

```bash
# 再跑一次 (方式 A 或 B 任意), 改完就生效
IP_LIST="192.168.1.100,...192.168.1.105" bash tools/run_rtsp_stitching.sh
```

## 想回到 dataset (4 个 mp4)?

```bash
INPUT_SOURCE_MODE=dataset bash tools/run_rtsp_stitching.sh
```

## yaml 字段速查 (params/camera_sources.yaml)

```yaml
sync_window_ms: 16         # 6 路 PTS 拉齐窗 (1 帧@60fps); 0 = 不要求同步
auto_calibrate: 0          # Sprint 2 自标定开关 (默认 off)

cameras:
  - type: rtsp                                        # 必填: file | rtsp
    uri: rtsp://admin:123456@192.168.0.123:554/video1
    width: 2560                                        # 期望分辨率 (运行时校验)
    height: 1440
    fps: 30
    pixel_format: NV12
    user_id: admin                                     # rtsp 凭据 (空 = 无认证)
    user_pw: 123456
    latency_ms: 120                                    # rtspsrc jitter buffer (80-200)
    use_tcp: true                                      # TCP 抗丢 vs UDP 低延迟
    connect_timeout_s: 10
    retry_attempts: 5
    reconnect_backoff_ms: 2000
    frame_drop_threshold: 0.8                          # fps / 期望 < 此值告警
```

> 这一段 yaml 默认保留样板, 改了 IP / user/password/path 后其它字段都不需要碰.

## 三种方式的对比

| 方式 | 触发时机 | 适用场景 |
|---|---|---|
| A. `IP_LIST=...` | 一次性 (env) | 临时调试 / 一次性切换 |
| B. `cams.txt` | 仓库内提交 | 团队协作 / git 化 / "改一次长期生效" |
| C. `vim yaml` | 用户手动改 | 已有 yaml, 想精细控制每个字段 |

A 是最快的 (1 行命令). B 是最干净的 (git 友好). C 适合 yaml 里有差异化配置 (例如 4 路 rtsp + 2 路 file).

> 所有方式都自动避开 `INPUT_SOURCE_MODE=camera` 这步 (v3.1 代码自动检测 yaml 有 rtsp 时切到 camera 模式).