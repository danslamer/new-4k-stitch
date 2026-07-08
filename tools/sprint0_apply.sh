#!/usr/bin/env bash
#
# tools/sprint0_apply.sh — YAML 装填助手 (板端跑)
#
# 用法 1 (命令行参数):
#   bash tools/sprint0_apply.sh \
#       -p 192.168.10 -s 21 -u admin -w Abcd1234
#
# 用法 2 (海康典型):
#   bash tools/sprint0_apply.sh -p 192.168.10 -s 21 -u admin -w Abcd1234
# 用法 3 (大华):
#   bash tools/sprint0_apply.sh -p 192.168.10 -s 21 -u admin -w Abcd1234 -t /live/main
#
# 用法 4 (环境变量):
#   export RTSP_PREFIX=192.168.10
#   export RTSP_START_IP=21
#   export RTSP_USER=admin
#   export RTSP_PASSWORD=Abcd1234
#   export RTSP_PATH=/Streaming/Channels/101
#   bash tools/sprint0_apply.sh
#
# 用法 5 (直接跑 rtsp 烟测):
#   bash tools/sprint0_apply.sh --probe -p 192.168.10.21..30  ...
#
# 该脚本的策略: 用 sed 替换每路 camera 条目的 IP, 改 6 个 IP, 不动其他字段
# (user_id, password, path 等可以事后手改).
#

set -uo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-/home/rocktech/Projects/new-4k-stitch}"
YAML="${PROJECT_ROOT}/params/camera_sources.yaml"
PROBE="${PROJECT_ROOT}/tools/rtsp_url_probe.sh"

PREFIX="${RTSP_PREFIX:-192.168.10}"
START_IP="${RTSP_START_IP:-21}"
USER="${RTSP_USER:-admin}"
PASSWORD="${RTSP_PASSWORD:-CHANGE_ME}"
PATH_TEMPLATE="${RTSP_PATH:-/Streaming/Channels/101}"
RUN_PROBE=0

usage() {
    sed -n "3,30p" "$0"
    exit "${1:-0}"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        -h|--help)               usage 0 ;;
        -p|--prefix)            shift; PREFIX="$1"; shift ;;
        -s|--start-ip)          shift; START_IP="$1"; shift ;;
        -u|--user)              shift; USER="$1"; shift ;;
        -w|--password)          shift; PASSWORD="$1"; shift ;;
        -t|--path)              shift; PATH_TEMPLATE="$1"; shift ;;
        --probe)                RUN_PROBE=1; shift ;;
        --yaml)                 shift; YAML="$1"; shift ;;
        *) echo "unknown flag: $1" >&2; usage 1 ;;
    esac
done

cd "${PROJECT_ROOT}"

if [ ! -f "${YAML}" ]; then
    echo "[ERR] yaml not found: ${YAML}" >&2
    exit 1
fi

bar() { printf "\n\033[1;34m===== %s =====\033[0m\n" "$*"; }
ok()  { printf "\033[32m[OK]\033[0m %s\n" "$*"; }
warn(){ printf "\033[33m[WARN]\033[0m %s\n" "$*"; }

bar "Apply plan"
echo "  prefix    = ${PREFIX}"
echo "  start_ip  = ${START_IP}  (6 路 21..26 递增)"
echo "  user      = ${USER}"
echo "  password  = $(printf '%s' "${PASSWORD}" | head -c 3)***  (len ${#PASSWORD})"
echo "  path      = ${PATH_TEMPLATE}"
echo "  yaml      = ${YAML}"

# 备份
BAK="${YAML}.bak.$(date +%Y%m%d-%H%M%S)"
cp "${YAML}" "${BAK}"
ok "backed up yaml → ${BAK}"

# 改 IP / uri / user_id / user_pw (用 python, 干净处理 6 处)
python3 - "${YAML}" "${PREFIX}" "${START_IP}" "${USER}" "${PASSWORD}" "${PATH_TEMPLATE}" <<'PYEOF'
import sys, re
yaml_path, prefix, start_ip, user, pw, path_tmpl = sys.argv[1:7]
start_ip = int(start_ip)
with open(yaml_path, "r", encoding="utf-8") as f:
    content = f.read()

# 匹配每一路 rtsp 条目 (- type: rtsp 块), 给 cam 序计数
cam = 0
out_lines = []
in_rtsp_block = False
for line in content.splitlines():
    if re.match(r"^\s*-\s*type:\s*rtsp\b", line):
        cam += 1
        in_rtsp_block = True
        ip_suffix = start_ip + (cam - 1)
        ip = f"{prefix}.{ip_suffix}"
        out_lines.append(line)
        # 注: 原 uri 行紧接着写进来, 我们在下一行做替换
        continue
    if in_rtsp_block and re.match(r"^\s*uri:\s*rtsp://", line):
        ip_suffix = start_ip + (cam - 1)
        ip = f"{prefix}.{ip_suffix}"
        new_uri = f"rtsp://{user}:{pw}@{ip}:554{path_tmpl}"
        # 缩进保持
        indent = re.match(r"^(\s*)", line).group(1)
        out_lines.append(f"{indent}uri: {new_uri}")
        continue
    if in_rtsp_block and re.match(r"^\s*user_id:", line):
        indent = re.match(r"^(\s*)", line).group(1)
        out_lines.append(f"{indent}user_id: {user}")
        continue
    if in_rtsp_block and re.match(r"^\s*user_pw:", line):
        indent = re.match(r"^(\s*)", line).group(1)
        out_lines.append(f"{indent}user_pw: {pw}")
        continue
    # 空行或下一个相机块: 关闭 in_rtsp_block
    if not line.strip():
        in_rtsp_block = False
        out_lines.append(line)
        continue
    if re.match(r"^\s*-\s*type:", line):
        in_rtsp_block = False  # 下个相机开始时, 也关闭 (cam 递增在 if type 处发生)
    out_lines.append(line)

with open(yaml_path, "w", encoding="utf-8") as f:
    f.write("\n".join(out_lines) + "\n")
print(f"  python: {cam} rtsp camera blocks rewritten")
PYEOF

ok "yaml rewritten"

bar "新 yaml 预览 (6 路 uri)"
grep -E "uri: rtsp://" "${YAML}" | head -10

bar "Diff (新 vs 上一版)"
diff -u "${BAK}" "${YAML}" | head -80 || true
echo "..."

if [ "${RUN_PROBE}" = "1" ]; then
    bar "Running rtsp_url_probe.sh"
    if [ -x "${PROBE}" ]; then
        bash "${PROBE}" || warn "probe failed (continuing anyway)"
    else
        warn "no probe at ${PROBE}"
    fi
fi

bar "DONE"
echo "  下一步:"
echo "    bash tools/sprint0_smoke.sh"
echo "    或: export INPUT_SOURCE_MODE=camera && ./build/image-stitching"