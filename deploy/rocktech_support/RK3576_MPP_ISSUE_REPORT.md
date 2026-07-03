# rocktech RK3576 板:MPP 解码链路问题报告

> **收件方**: rocktech 厂商技术支持
> **报告人**: 开发方
> **报告日期**: 2026-07-02
> **目的**: 请求补充出厂镜像缺失的 mpp 用户态驱动 / 内核模块 / 固件
> **严重程度**: P0 — 阻塞所有 6 路硬件解码相关业务

---

## 1. 板子环境

| 项 | 值 |
|---|---|
| 板子型号 | rocktech RK3576 主板 (6 路物理 MIPI 直连版) |
| hostname | `rockemb` |
| 用户 | `rocktech` (在 `video` + `render` 组) |
| 内核 | `6.1.75` |
| 操作系统 | Ubuntu 22.04.5 LTS |
| 镜像创建时间 | 2025-08-26 (从文件 mtime 看) |
| 内核 mpp driver 内置版本 | `ea6192a78167 2024-06-14 (rockchip: mpp: rk3576: fix enc err when rec_fbc_dis=1)` |

---

## 2. 现象 (硬数据, 可复现)

### 2.1 用户态现象

- 跑 6 路 4K 拼接(`./image-stitching`, 输入 `dataset` 模式)
- FFmpeg 选对了 `mpeg4_rkmpp` 解码器
- **6 路 decoder 全部静默 — 0 帧出来, 0 报错**(部分情况会出 `Timeout getting decoded frame`)
- 主循环没拼出帧, 程序卡死

### 2.2 strace 抓 mpp_init 完整流程

最小 mpp_test.c (绕过 ffmpeg, 直接调 librockchip_mpp) 跑 strace 的关键输出:

```
openat("/dev/mpp_service", O_RDWR|O_CLOEXEC) = 3      ✓ 成功
ioctl(3, _IOC(_IOC_READ|_IOC_WRITE, ...)) = 0          ✓ 老 mpp_service ioctl 成功
... (重复多次, 全部成功)
openat("/dev/kmpp_ioctl", O_RDWR) = ENOENT            ✗ 不存在!
openat("/dev/kmpp_objs", O_RDWR) = ENOENT             ✗ 不存在!
exit status 1
mpp_init(DEC, BLOCK) returns -2 (= MPP_ERR_UNKNOW)
```

### 2.3 gst-launch 也同样

```
$ gst-launch-1.0 filesrc location=t50.mp4 ! qtdemux ! mpeg4videoparse ! mppvideodec ! fakesink num-buffers=5
ERROR: from element /GstMppVideoDec:mppvideodec0: No valid frames decoded before end of stream
```

### 2.4 /proc/mpp_service 状态

| 节点 | task_count | disable_work | session_buffers |
|---|---|---|---|
| rkvdec-core0 | **0** | 0 (启用) | 40 |
| rkvdec-core1 | **0** | 0 (启用) | 40 |
| vdpu | (无值) | 0 (启用) | 40 |

`task_count = 0` 说明**内核 mpp 驱动从未实际派发过 task**,所有 task 都在 init/queue 阶段失败。

### 2.5 dmesg 实时跟踪

跑 mpp_test 时 `dmesg -w` + `dynamic_debug` 打开 mpp 全部 debug,**完全无 mpp / kmpp / vpu / rkv / firmware 任何 log**。内核 mpp 驱动接到 ioctl 但没打 log(很可能是 ioctl 直接返回 0 但 mpp 用户态识别不出 session 格式而放弃)。

---

## 3. 缺什么 (vendor 需要补的内容)

### 3.1 缺失 1: VPU/RKVDEC 固件

`/lib/firmware/rockchip/` 当前内容:
```
-rw-r--r-- 1 root root 98320 Nov 26  2025 dptx.bin    ← 只有 DP TX 固件
```

**缺** (RK3588/RK3576 通用, kernel 6.1.75 mpp_rkvdec2.c / mpp_vdpu1.c 期望):

| 期望文件 | 大小估算 | 用途 |
|---|---|---|
| `rockchip/rk3588_vpu.bin` | ~70-100 KB | VPU1 (含 VDPU1 + VEPU1) |
| `rockchip/rk3588_vpu121.bin` | ~100-150 KB | VPU2 (含 RKVDEC2 + VEPU2) |
| `rockchip/rk3588_av1d.bin` | ~50-80 KB | AV1 decoder (RK3588 独有, RK3576 可能不需要) |
| `rockchip/rk3588_rkvdec2.bin` | ~80-120 KB | RKVDEC2 (h264/hevc/mpeg4) |

> 注: `dptx.bin` 是显示输出固件, 跟解码无关, 已正确装入。

**判断依据**:
- 内核 mpp driver (`mpp_rkvdec2` / `mpp_vdpu1` / `mpp_vepu2`) 已加载
- `/proc/mpp_service/rkvdec-core0/1` / `vdpu` 设备存在
- 但 `task_count` 永远 0 (即使 rkmpp 选对了 decoder)
- 跟 `gst-launch-1.0 mppvideodec` 一样 0 帧
- strace 看到 `/dev/mpp_service` ioctl 成功但 mpp 用户态判 -2 (kernel 不能返回有效 session)

典型场景: 固件缺失 → rkvdec 内核驱动 probe 失败 / hardware 不能 init → mpp_service 调度 task 失败 → mpp_init 返回错误。

### 3.2 缺失 2: kmpp 内核模块 (新 mpp 接口)

**期望**:
- `/dev/kmpp_ioctl` (主设备)
- `/dev/kmpp_objs` (辅助设备)

**实际**:
- `/dev/` 下只有老的 `mpp_service` (主设备号 239:0)
- `lsmod` / `ls /sys/module/` 都没有 `kmpp` 模块
- `ls /sys/bus/platform/drivers/` 也没有 `kmpp` 驱动
- DTS 里 `mpp-srv` 节点 compatible 是 `rockchip,mpp-service` (老的)

**判断依据**:
- ffmpeg-rockchip-master (FFmpeg 6.1 + jellyfin-mpp 1.3.9) 期望新接口 `/dev/kmpp_ioctl`
- strace 看到 mpp 用户态先试 `/dev/kmpp_ioctl` 失败, fallback 到 `/dev/mpp_service`, 仍失败
- 内核 mpp 模块**只有 mpp_service 老接口, 没有 kmpp 新接口**

**建议**:
- kernel 6.1.75 rocktech 镜像需要打 vendor 补丁, 加入 kmpp 模块
- 或者: 提供 6.1.75 的 mpp 驱动 source patch, 把 kmpp 补上

### 3.3 缺失 3: 电源域 / devfreq 治理 (次要)

`/sys/class/regulator/` 看不到 vpu/rkvdec/vdpu regulator 节点
`/sys/class/devfreq/` 没有 rkvdec/vdpu 节点 (只有 npu/gpu/vop/dmc)

**判断依据**:
- DTS 节点 `rkvdec-core@fdc48000` 有 `power-domains` 属性, 但 sysfs 没暴露
- 内核 mpp 驱动可能因为电源域未上电而 init 失败
- 跟"缺固件"耦合, 任何一个都会导致 rkvdec 不工作

### 3.4 缺失 4 (可选): RKISP IQ 文件

`/etc/iqfiles/` 当前应该是空的或没有 GC4683 对应的 xml。这是 RKISP 3A 用的, 跟 mpp 解码无关, 但 6 路 GC4683 真机启动时也会卡这。

---

## 4. 期望 vendor 提供的内容

### 选项 A (推荐): 一份新出厂镜像
- 内核: 6.1.75 (保持, 不要随便升, 兼容性已验证) + 加 kmpp 补丁
- `/lib/firmware/rockchip/`: 补全 rk3588_vpu.bin / rk3588_vpu121.bin / rk3588_rkvdec2.bin
- `/etc/iqfiles/`: 提供 GC4683 的 IQ xml (3A 用)
- 镜像大小: 跟现有 5.8GB 相当即可

### 选项 B: 单独几个 deb 文件
- `rockchip-mpp-firmware_<version>_arm64.deb` (装到 `/lib/firmware/rockchip/`)
- `rockchip-kmpp-modules-<kernel-version>_arm64.deb` (装到 `/lib/modules/6.1.75/updates/`)
- `rockchip-3a-iqfiles_<version>_all.deb` (装到 `/etc/iqfiles/`)

### 选项 C: 上游 source 地址
- kmpp 内核驱动 patch (kernel.org 6.1.75 + 哪些 commit)
- 固件 git repo (e.g. github.com/JeffyCN/rockchip_mirrors 路径)
- RK3576 / RK3588 BSP 镜像打包脚本

---

## 5. 自助 debug 命令 (可贴给 vendor 重现)

```bash
# 1. mpp 用户态库版本
md5sum /usr/lib/aarch64-linux-gnu/librockchip_mpp.so.0
strings /usr/lib/aarch64-linux-gnu/librockchip_mpp.so.0 | grep -E "/dev/mpp|/dev/kmpp" | head -5

# 2. /dev 下 mpp 设备
ls -la /dev/mpp_service /dev/kmpp_ioctl /dev/kmpp_objs 2>&1

# 3. /lib/firmware/rockchip/ 内容
ls -la /lib/firmware/rockchip/

# 4. 内核 mpp 拓扑 (task_count 全 0 表示 mpp 没真正工作)
for core in rkvdec-core0 rkvdec-core1 vdpu; do
  echo "=== $core ==="
  cat /proc/mpp_service/$core/task_count
  cat /proc/mpp_service/$core/disable_work
done

# 5. 内核 mpp 模块 + driver
lsmod | grep -iE "mpp|vpu"
ls /sys/bus/platform/drivers/ | grep -iE "mpp|kmpp"

# 6. mpp_init 最小测试
cat > /tmp/mpp_test.c <<'EOF'
#include <stdio.h>
#include "rockchip/rk_mpi.h"
int main() {
  MppCtx ctx; MppApi *mpi;
  printf("mpp_create: %d\n", mpp_create(&ctx, &mpi));
  printf("mpp_init  : %d\n", mpp_init(ctx, MPP_CTX_DEC, MPP_POLL_BLOCK));
  return 0;
}
EOF
gcc /tmp/mpp_test.c -I/usr/include -L/usr/lib/aarch64-linux-gnu -lrockchip_mpp -o /tmp/mpp_test
LD_LIBRARY_PATH=/usr/lib/aarch64-linux-gnu /tmp/mpp_test
# 期望: mpp_init 返回 0
# 实际: mpp_init 返回 -2

# 7. strace 完整流程
strace -f -e openat,ioctl -o /tmp/mpp.log /tmp/mpp_test
grep -E "kmpp_ioctl|mpp_service|firmware" /tmp/mpp.log
# 期望看到 /dev/kmpp_ioctl 成功 + /lib/firmware/.../xxx.bin 加载
# 实际看到: /dev/kmpp_ioctl ENOENT + 没 firmware 访问

# 8. dmesg 跑 mpp_test 时
sudo dmesg -c; LD_LIBRARY_PATH=/usr/lib/aarch64-linux-gnu /tmp/mpp_test; sudo dmesg
# 期望看到 mpp_service / rkvdec init log
# 实际: 完全无 mpp 相关 log
```

---

## 6. 时间线 (供 vendor 复盘)

| 时间 | 事件 |
|---|---|
| 2026-07-01 | 板子上 image-stitching 跑通编译, ffmpeg 6.1 rkmpp 装好 |
| 2026-07-02 | 6 路拼接跑不通, decoder 0 帧 |
| 2026-07-02 | 诊断: 内核 mpp 拓扑有, task_count=0; /dev/mpp_service 可访问; rocktech 用户在 video/render 组 |
| 2026-07-02 | 尝试 jellyfin-mpp 1.3.9: mpp_init 返回 -2 (kmpp_ioctl 不存在) |
| 2026-07-02 | strace 确认: kmpp 接口完全不存在, mpp_service 老接口被新 mpp 误用 |
| 2026-07-02 | 检查 firmware: 只有 dptx.bin, 缺 vpu/rkvdec 固件 |
| 2026-07-02 | 准备本报告 |

---

## 7. 紧急 workaround (供 vendor 知晓, 不指望长期用)

开发方临时绕开 rkmpp 走 sw decode 路径的方案:
- 在 `sensor_data_interface.cc` 加 sw decoder fallback
- mpeg4 软解后用 RGA 转 NV12 DMA-BUF
- 性能 5-10 FPS (vs 30 FPS 目标)
- 仅作功能验证用, 不是生产方案

但**根本问题**必须由 vendor 修, 否则项目无法上 30 FPS。

---

**联系人**: [开发方]
**板子 SN**: [板子序列号, 开发方填]
**回邮**: [开发方邮箱]
