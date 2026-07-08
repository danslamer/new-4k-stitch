﻿# rocktech 厂商沟通包

> 跟 rocktech 厂商技术支持的沟通材料. 烧新镜像后, 删掉这个目录或保留作历史.

> **重要前提**: 本项目架构硬约束 (6 路 4K @ 30 FPS) 要求 **rkmpp 硬件解码 + DMA-BUF 零拷贝** 整条链路. **不能** 临时改用 sw 解码 fallback, 性能达不到业务要求. 见 `AGENTS.md` "架构硬约束".

## 当前文件

- `RK3576_MPP_ISSUE_REPORT.md` - **发给厂商的完整技术报告** (主文件, 必发)
  - 包含 板子环境 / 现象 / strace 证据 / 缺的 4 项内容 / 期望 vendor 提供 / 自助 debug 命令
  - 中文 + 英文, 方便厂商不同技术栈的人都能读

## 完整内容备份 (同步给 vendor 时建议一起附)

发邮件时把整个 `rocktech_support/` 目录打包成 zip 给 vendor, 让他们能复现. 关键附件:

1. `RK3576_MPP_ISSUE_REPORT.md` (主报告)
2. `/tmp/mpp_test.c` 源文件 (板子上的最小 mpp 测试)
3. `strace` 完整 log (板子上的 `/tmp/mpp_full.log` 或 `/tmp/ffmpeg_strace.log`)
4. `dmesg` 输出 (板子上的, 跑 mpp_test 时的)
5. `dpkg -l | grep -iE "mpp|rockchip"` 输出 (板子上的, 当前装的所有相关包)

## 板子 SSH 信息 (vendor 远程 debug 用)

- IP: 192.168.137.100 (WiFi)
- 用户: rocktech
- 密码: rocktech (已通过 SSH 密钥免密, 详见 `../ssh/`)
- 部署目录: `/home/rocktech/Projects/new-4k-stitch/`

## 等 vendor 反馈时, 同步需要做

1. 收新镜像 / .deb → 重烧系统 / 装 deb
2. 验证脚本: 跑 `RK3576_MPP_ISSUE_REPORT.md` 第 5 节的 8 条命令
3. 期望:
   - `ls /lib/firmware/rockchip/` 有 vpu*.bin / rkvdec*.bin
   - `ls /dev/kmpp_ioctl` 存在
   - `/tmp/mpp_test` 跑出 mpp_init=0
   - `task_count > 0` 跑 ffmpeg rkmpp 后
4. 通过: 跑 `image-stitching`, 6 路拼接出图
