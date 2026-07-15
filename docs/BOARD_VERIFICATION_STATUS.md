# 板端验证状态 (Board Verification Status)

**最后更新**: 2026-07-15 (北京时间)
**目的**: 跟踪 **PC 端代码就绪** vs **板端验证完成** 的差距, 以及后续 Sprint 5 任务依赖。
**配套**: `docs/HISTORY.md` 时间线 + `docs/USER_GOAL_ROADMAP.md` Sprint 5 子段规划。

---

## 1. 当前一句话状态 (TL;DR)

> **6 路 GC4683 MIPI 接入代码已全部就绪 (Sprint 4-MIPI 在 sandbox 内改完)。
> 板端实跑验收流程也备好 (tools/probe_v4l2.sh + tools/sprint4_mipi_bootstrap.sh)。
> 待用户在 PC PowerShell 跑 `tools/_push_and_verify.ps1` 做 SCP 上传 + 远程验收,
> 或在板端 ssh 直接跑 `bash tools/sprint4_mipi_bootstrap.sh`。**

详细差距和后续步骤见下。

---

## 2. 改动清单 (HEAD 之后, 未 commit)

### 2.1 PC 端已改完代码 (code is ready)

| 文件 | 类型 | 行 | 用途 |
|---|---|---|---|
| `include/gst_mpp_decoder.h` | modified | +14 | 加 `StartMipi()` + `BuildMipiPipeline()` |
| `include/sensor_data_interface.h` | modified | +51 | `BlackFrameHolder` 结构 + `kBlackFrame` storage + `device_path/io_mode/v4l2_buffer_count/use_isp_pipeline` 字段 |
| `src/gst_mpp_decoder.cc` | modified | +34 | `BuildMipiPipeline()` 走 v4l2src + capsfilter(NV12) + appsink; 设备文件 stat 检查 |
| `src/sensor_data_interface.cc` | modified | +186 | `MakeZeroBlackFrame` 一次性 NV12 drm_alloc, `SensorDataInterface::BlackFramePushLoop` 静态成员 30 fps 循环, `ConvertQueuedFrameToDmabuf` 加 kBlackFrame 分支, per-thread mipi 失败 fallback |
| `src/app.cc` | modified | +20 | App ctor 内插 `[App] [Sprint4-MIPI] startup(N)` summary log |
| `params/camera_sources.yaml` | modified | 重写 | 6 路 `type: mipi`, `device_path` 改 HISTORY.md § 2.9 实测的 rkisp_mainpath (`/dev/video66/75/84/93/102/111`) |
| `tools/probe_v4l2.sh` | **new** | 5.9 KB | 自动探测 rkisp_mainpath (`Pixel Format = NV12 + Width = 2560`), `--edit-yaml` 自动改 yaml, 注意必须 `--stream-count=N` (否则 v4l2-ctl 卡 ISP queue, HISTORY.md § 3) |
| `tools/sprint4_mipi_bootstrap.sh` | **new** | 3.2 KB | 一键 探 probe → cmake+make → 启 image-stitching 5s → 抓 log |

### 2.2 PC 端已就绪但未启用 (PC-side helper, 待推到板端但不进入 build)

| 路径 | 用途 |
|---|---|
| `tools/_push_and_verify.ps1` | **未写完** — 用户中断在我设计 PowerShell 一键脚本 (SCP+SSH) 之前, 还没写到磁盘。当前没有这个文件。 |
| `scripts/_dotnet_ssh_test.ps1` | 测试用残余, 可删。 |

### 2.3 与本次工程无关的待处理项 (与 Sprint 4-MIPI 解耦)

- `tools/grab_sync.py`: 用户在 git working tree 改了 `--output-dir` 默认值 (从 `results/grab` → `results/test`)。这是用户自己 commit `105ff5a 多个框识别效果显示完毕` 的延续。
- `GC4683drivers/`: 用户 untracked, 4 个驱动参考文件 (说明 docx + 主板开发指南 pdf + 2 个 .c)。

---

## 3. 板端验证 checklist (待用户在板端跑)

### 3.1 先决条件 (PC PowerShell, 一次性)

用户需要在他自己的 PowerShell (管理员) 跑 (按 sandbox outbound TCP 22 被拦截, 我们没法在 sandbox 内 SSH):

```powershell
# (1) 一次性准备: PowerShell 装 psftp client (vscode remote ssh 也许已带)
# OpenSSH + ssh agent + sshpass 也可, 推荐 Microsoft.BizTalk.Adapter.Sftp 或系统自带的 scp/ssh.

# (2) 一次性 scp 上传 (按需修改 $board_user / $board_ip):
$board = "rocktech@192.168.137.100"
$proj = "~/Projects/new-4k-stitch"

# Windows scp 默认 OK; 用 LibreSSL 9.5p1 + plink/scp
scp -r include\ gst_mpp_decoder.h "${board}:${proj}/include/"   # 如果 path 拼装不工作, 用 git pull
# 现在代替方案: 用户在他 PC 已能 push 或 scp; 最稳是手动 ssh + tar 打包推送.

# (3) 一次性 ssh 远程跑 (前台 5s 抓 log):
ssh ${board} "cd ${proj} && bash tools/sprint4_mipi_bootstrap.sh --probe-only"

# (4) 看结果 (log 在 /tmp/stitch.log):
ssh ${board} "grep -E 'decoder_perf|BlackFrame|gst_mpp_decoder' /tmp/stitch.log | head -80"
```

> 我**没**把 `tools/_push_and_verify.ps1` 写到磁盘 (Step 3 中断后没继续)。
> 用户如果要我接着干 SCP 端 PowerShell 脚本, 单开需求就行。

### 3.2 板端一键脚本 (ssh 内部, 用户手动跑)

```bash
# 全流程: probe + cmake + make + 启动 5s + 抓 log
bash tools/sprint4_mipi_bootstrap.sh

# 仅 probe + 改 yaml
bash tools/sprint4_mipi_bootstrap.sh --probe-only

# 跳过 probe, 直接用现有 yaml
bash tools/sprint4_mipi_bootstrap.sh --skip-probe

# 后台跑, log 到 /tmp/stitch.log
bash tools/sprint4_mipi_bootstrap.sh --bg

# 单跑 probe + 自动改 yaml
bash tools/probe_v4l2.sh --edit-yaml
```

### 3.3 预期日志 (成功全 4 路 v4l2src + 2 路 BlackFrameProvider)

```
[App] [Sprint4-MIPI] startup(6): cam0=v4l2src-ok cam1=v4l2src-ok cam2=pending cam3=v4l2src-ok cam4=v4l2src-ok cam5=pending | real=4 pending=2
[sensor_data_interface] InitVideoCapture routes 6 cam(s): cam0=mipi cam1=mipi ... cam5=mipi
[gst_mpp_decoder] build MIPI pipeline: device=/dev/video66 io-mode=dmabuf w=2560 h=1440
[gst_mpp_decoder][mipi] OK device=/dev/video66 w=2560 h=1440
[decoder 0] gstreamer-rockchip mppvideodec path (mipi): /dev/video66
[gst_mpp_decoder] build MIPI pipeline: device=/dev/video111 io-mode=dmabuf w=2560 h=1440
[gst_mpp_decoder][mipi] device not found: /dev/video111 (errno=2), falling back to BlackFrameProvider
[decoder 5] mipi start failed, falling back to BlackFrame placeholder
[BlackFrame] cam5 placeholder running, w=2560 h=1440 fd=...
[decoder_perf 0] decoder=gst_mppvideodec ... fps=30
[decoder_perf 5] decoder=blackframe ... fps=30 uri=black-placeholder
```

注意: 目前 yaml 默认 `/dev/video111` 是 cam5 但 sensor 物理上没接。
**修正待用户跑完 probe 后**: probe 输出 → patch yaml → 重启。

### 3.4 必看 / 必问诊

```
[App] [Sprint4-MIPI]  startup(N) line              # 多少路 v4l2src-ok vs pending
[decoder_perf N]        line                       # 每路 fps (期望 30, 实际可能 25-28 网卡抖动)
[BlackFrame N]           line                       # 哪几路 placeholder
[gst_mpp_decoder][mipi]  line (vendor sensor 没接报 device not found / errno=2/6/19)
[App] [2x3][sprint4-B]   line                       # panorama_w × panorama_h 应 ~ 4670 × 2826
```

可能 block:

- **板端缺 gstreamer1.0-plugins-good (v4l2src)** → `factory_make failed v4l2src`. 兜底: 用户 `dpkg -l gstreamer1.0-plugins-good`.
- **板端 videoN 节点权限** → v4l2src 报 Permission denied. 兜底: `/dev/video66` 是 rocktech 用户可读吗? `ls -l /dev/video*`.
- **板端缺 ION/Mali/DMA-BUF heap** → v4l2src dmabuf 导出失败. 兜底: `ls /dev/dri /dev/ion`.

---

## 4. Sprint 5 任务依赖链 (board 验证通过后立刻启动)

主计划见 `docs/USER_GOAL_ROADMAP.md § 2 Sprint 5`。这里只列 **依赖图** + **我们当前 Sprint 4-MIPI 通过后立刻能开干的下一段**:

```
Sprint 4-MIPI ✅ code ready, board verify pending
   ↓ (用户验证成功)
Sprint 5-IPM-A      1.5 周   离线标定 (拿 6 路 K + R + t) + 算 IPM LUT
  ├─ 新 src/calibration.cc/.h
  ├─ 新 tools/calibrate_6cam.cpp
  └─ sensors.yaml + calibration.yaml  字段扩到 R/t/yaw + ipm_lut_i.png 落盘
   ↓
Sprint 5-IPM-B      1 周     实时 IPM remap (RGA imremap + CV_32FC2)
  ├─ src/image_stitcher.cc::WarpImages 加 IPM 分支 (当前 0/90 直拷)
  └─ 启动期读 ipm_lut_*.png 注入 ImageStitcher::SetIpmLuts()
   ↓
Sprint 5-SEAM-A      1 周     GraphCut 默认 seam 落盘
  ├─ src/app.cc::BootStrapOptimalLayout 末尾接 GraphCutSeamFinder
  └─ warp_data.yaml 增 seam_default_0..6.png
   ↓
Sprint 5-SEAM-B      1 周     硬切 + 3 px 窄带 OpenCL kernel (取代 α blend)
   ↓
Sprint 5-FG-A        1 周     BgSubtractor + ImageStitcher::PushSeam
  ├─ 新 src/bg_subtractor.cc/.h
  └─ 前景人跨 seam 时局部推开
   ↓
Sprint 5-FG-B        0.5 周   Kalman seam 平滑 (src/seam_tracker.cc/.h 新建)
   ↓
Sprint 5-SAL         0.5 周   saliency heatmap 累积 (离线 1 周运行)
   ↓
Sprint 6-A          1 周     UI 4→6 cam 循环 + width/height 步进
Sprint 6-B          0.5 周   /api/roi POST 接 x/y/w/h
   ↓
Sprint 7            1 周     end-to-end 回归 + GIMP 截图比对 180° 覆盖
```

**Sprint 5 子段累计 ~7.5 周** (per `USER_GOAL_ROADMAP.md § 2`)。

### 4.1 Sprint 4-MIPI 收尾后才解锁的工作 (按 BLOCK 关系)

| 工作 | 依赖 Sprint 4-MIPI 板端 OK | 备注 |
|---|---|---|
| 拿 N 段真实 GC4683 raw 序列喂 calibrate 工具 | ✅ | 必须 Sprint 4-MIPI 板端 decode 稳定 30 fps 1 分钟以上才能抽帧 |
| saliency heatmap 累积 (5-SAL) | ✅ | 必须 Sprint 4-MIPI 板端 1 周无人场景 |
| PushSeam 实际效果联调 (5-FG-A) | ✅ | 必须有真实人在场景中走动 |
| UI ROI width/height 调整 (6-A) | 不严格 | 6-A 解 6 路循环可在 dataset fallback 测, 但 6 路同时 (2x3) 评测要 Sprint 4-MIPI 验证 |

---

## 5. 已知风险 / 仍要做的小事 (per 文件, 当前 session)

| 文件 | 风险 |
|---|---|
| `params/camera_sources.yaml` cam0..cam5 | `/dev/video66` 等路径写死, 不同主板重新装载可能漂. **板端跑 probe_v4l2.sh --edit-yaml 后自动修正**. |
| `tools/probe_v4l2.sh` | 已经要求 `--stream-count=N` 防 v4l2-ctl 卡 ISP queue (HISTORY.md § 3)。grab-test 仍可能因 v4l2-ctl 新版本默认行为变化 fail, 用户可禁用 grab-test 那段 (5 秒注释)。 |
| `src/gst_mpp_decoder.cc::BuildMipiPipeline` capsfilter | 当前没加 "拒 raw-Bayer" 检查 (用户中断在 Step 3 — 没做完)。理论上 v4l2src 自动协商应该拒绝, 但建议补一道 `v4l2-ctl -d $node --get-fmt-video \| grep NV12` 在 BuildMipiPipeline 开头。若拿到 BA10 / YUYV 而不是 NV12, 报 "this is wrong node, want rkisp_mainpath"。下一步待续。 |
| `src/sensor_data_interface.cc::BlackFramePushLoop` | 只是用 30 fps 循环, 没把 BlackFrame 帧的 mjpeg snapshot 标记 (即 `/api/snapshot` 仍会拿黑帧). 验收以 stitcher 主链路为准。 |
| `src/app.cc::Sprint4-MIPI log` | 在 calibration 装载之后但 sensor decode 启动之前 log, 故只能看到 "pending" 数, 真正 v4l2src-ok 要等第一个 frame。 |
| `tools/_push_and_verify.ps1` | **未写到磁盘** (Step 3 中断), 用户如果需要给我提一句, 我接着干。 |
| `scripts/_dotnet_ssh_test.ps1` / `scripts/_probe.py` 等历史残余 | 可清理, 优先保留 sprint4_mipi_bootstrap.sh 这一条路径。 |

---

## 6. 已退出但本会话未删除的语句

无。本会话没做 `git reset`, 没删除任何 tracked 文件。HISTRY.md top section 的 v3.2 节 (commit aa3d3b7) 是另一会话历史, 不在本次改动范围。

---

## 7. 给用户的下一步问题 (待用户决定)

1. **立即**: 您想让我把 `tools/_push_and_verify.ps1` (PC 一键 SCP+SSH) 写出来吗? 写完后您双击运行, 板端结果会自动 SCP 回 PC。
2. **立即**: 您想自己 ssh 上板, 然后给我贴回 probe_v4l2 + image-stitching 启动日志? 我可以基于真板端结果继续调 yaml / 加 raw 检测 / 加 fallback。
3. **立即**: 现在的 Sprint 4-MIPI 是否要切到 dataset fallback 测试 build (先在板端 `unset INPUT_SOURCE_MODE` 跑 4 路 t{50..53,t40,t41}.mp4), 排查编译错?
4. **下周**: Sprint 5-IPM-A 离线标定启动前需要做棋盘格采图工具 (tools/calibrate_6cam.cpp), 是您自己写还是要我开 worker 起草?
