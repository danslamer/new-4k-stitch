#!/usr/bin/env bash
#
# tools/run_rtsp_stitching.sh — "改一下 IP 就能用" 的入口命令.
#
# 三种用法, 任选其一:
#
#   A) 改完 yaml 直接 run (当前 yaml 已有真实 IP)
#      bash tools/run_rtsp_stitching.sh
#
#   B) 一行: 编辑 + 运行
#      IP_LIST="192.168.0.123,192.168.0.124,192.168.0.125,192.168.0.126,192.168.0.127,192.168.0.128" \
#          bash tools/run_rtsp_stitching.sh
#
#   C) cams.txt 每行一个 IP (适合 git 仓库化 / 自动同步)
#      cat > cams.txt <<EOF
#      192.168.0.123
#      192.168.0.124
#      ...
#      EOF
#      bash tools/run_rtsp_stitching.sh
#
# 可选环境变量:
#   IP_LIST              6 个 IP 逗号分隔, 见用法 B
#   RTSP_USER            同时改 user_id / URI 内嵌的密码
#   RTSP_PASSWORD        同上
#   RTSP_PATH            同时改 URI path (例 /video1, /live/main)
#   SKIP_PRECHECK=1      跳过 yaml 真实 IP 检查 (信任 YAML)
#

set -uo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-/home/rocktech/Projects/new-4k-stitch}"
cd "${PROJECT_ROOT}"

YAML="${PROJECT_ROOT}/params/camera_sources.yaml"
BIN="${PROJECT_ROOT}/build/image-stitching"
CAMS_FILE="${PROJECT_ROOT}/cams.txt"
SET_IPS="${PROJECT_ROOT}/tools/set_cam_ips.sh"

bar()  { printf "\n\033[1;34m===== %s =====\033[0m\n" "$*"; }
ok()   { printf "\033[32m[OK]\033[0m %s\n" "$*"; }
warn() { printf "\033[33m[WARN]\033[0m %s\n" "$*"; }
err()  { printf "\033[31m[ERR]\033[0m %s\n" "$*" >&2; }

bar "0. 预检"

[ ! -f "${YAML}" ] && { err "yaml 不存在: ${YAML}"; exit 1; }
ok "yaml 存在: ${YAML}"

[ ! -x "${BIN}" ] && { err "binary 不存在或无 execute 权限: ${BIN}"; err "先 cd build && cmake .. && make"; exit 1; }
ok "binary 在位: ${BIN}"

# 数 yaml 里的 rtsp 路数
N_RTSP=$(grep -c "^\s*-\s*type:\s*rtsp\b" "${YAML}" 2>/dev/null || echo 0)
[ "${N_RTSP}" -lt 1 ] && { err "yaml 没有 rtsp 源 (type: rtsp), 没法跑"; exit 1; }
ok "yaml rtsp 路数: ${N_RTSP}"

# --------------------------------------------------------------------
# A) IP_LIST env 优先: 一行命令改 IP + 跑.
# B) cams.txt 优先: 仓库根 cams.txt 每行一个 IP.
# --------------------------------------------------------------------

MADE_CHANGES=0
if [ -n "${IP_LIST:-}" ]; then
    bar "1. 应用 IP_LIST (env)"
    ok "IP_LIST=${IP_LIST}"
    if [ "${N_RTSP}" -eq 1 ] && [ "$(echo "${IP_LIST}" | tr ',' '\n' | wc -l)" -gt 1 ]; then
        # 用户给更多 IP 但 yaml 只有 1 路 → 强制写入 (yaml 1 路也行)
        bash "${SET_IPS}" --force --user "${RTSP_USER:-}" --password "${RTSP_PASSWORD:-}" --path "${RTSP_PATH:-}" "${IP_LIST}"
        MADE_CHANGES=1
    else
        bash "${SET_IPS}" --user "${RTSP_USER:-}" --password "${RTSP_PASSWORD:-}" --path "${RTSP_PATH:-}" "${IP_LIST}"
        MADE_CHANGES=1
    fi
elif [ -f "${CAMS_FILE}" ]; then
    bar "1. 应用 cams.txt"
    ok "cams.txt 存在: ${CAMS_FILE}"
    bash "${SET_IPS}" --user "${RTSP_USER:-}" --password "${RTSP_PASSWORD:-}" --path "${RTSP_PATH:-}" --file "${CAMS_FILE}"
    MADE_CHANGES=1
fi

# 占位 IP / CHANGE_ME 检查 (没有 IP_LIST 又没 cams.txt, 提示用户)
PLACEHOLDER=$(grep -E "rtsp://[^@]+@(192\.168\.10|CHANGE_ME)" "${YAML}" | wc -l)
if [ "${PLACEHOLDER}" -gt 0 ] && [ "${MADE_CHANGES}" -eq 0 ] && [ "${SKIP_PRECHECK:-0}" -ne 1 ]; then
    warn "yaml 还有 ${PLACEHOLDER} 行带占位 IP (192.168.10.x / CHANGE_ME)"
    cat <<EOF
请二选一:

  # 方式 1: 一行命令 (推荐, 临时)
  IP_LIST="192.168.0.123,192.168.0.124,192.168.0.125,192.168.0.126,192.168.0.127,192.168.0.128" \\
      bash tools/run_rtsp_stitching.sh

  # 方式 2: 创建 cams.txt (适合 git 化, 改一次长期生效)
  cat > cams.txt <<EOC
192.168.0.123
192.168.0.124
192.168.0.125
192.168.0.126
192.168.0.127
192.168.0.128
EOC
  bash tools/run_rtsp_stitching.sh

  # 方式 3: 直接编辑 yaml 后 run
  vim params/camera_sources.yaml
  bash tools/run_rtsp_stitching.sh

EOF
    exit 1
fi
ok "yaml 看起来有真实 IP (无占位符)"

bar "2. 启动 image-stitching"
cat <<EOF
  INPUT_SOURCE_MODE=camera  (v3.1 自动探测 YAML 有 rtsp 时也会用 camera, 这里显式保险)
  ENABLE_VISUAL_TUNING=0   (板端没 X server, 设 0 避免 SDL 拖崩; 想要 SDL 窗口就 1)
  STITCH_K_FOCAL_SCALE 等调参 env 可选
EOF

exec env INPUT_SOURCE_MODE=camera ENABLE_VISUAL_TUNING=0 USE_ROI_CONFIG=0 "${BIN}"