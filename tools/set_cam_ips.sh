#!/usr/bin/env bash
#
# tools/set_cam_ips.sh — "只改 IP 段, 不动其它字段" 的 yaml 编辑器.
#
# 用法:
#   bash tools/set_cam_ips.sh 192.168.0.123,192.168.0.124,...,192.168.0.128
#   bash tools/set_cam_ips.sh --file cams.txt
#   bash tools/set_cam_ips.sh --prefix 192.168.0 --start 123
#
# 配合 run_rtsp_stitching.sh 用:
#   bash tools/set_cam_ips.sh --user admin --password 123456 --path /video1 192.168.0.123,...,128
#

set -uo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-/home/rocktech/Projects/new-4k-stitch}"
YAML="${PROJECT_ROOT}/params/camera_sources.yaml"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    cat <<USAGE
Usage:
  $0 <ip1,ip2,ip3,ip4,ip5,ip6>                          comma-separated 6 IPs
  $0 --file <cams.txt>                                  one IP per line
  $0 --prefix 192.168.0 --start 123                     123..128

Options:
  --yaml <path>        yaml path (default params/camera_sources.yaml)
  --user <name>        set user_id field on every cam (default keep)
  --password <pwd>     set URI embedded password + user_pw field
  --path <path>        set URI path part (e.g. /video1)
  --force              force write when IP count != yaml cam count
  -h, --help           help
USAGE
}

PREFIX=""
START_IP=""
FILE_PATH=""
RAW_IPS=""
NEW_USER=""
NEW_PWD=""
NEW_PATH=""
FORCE=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        -h|--help)         usage; exit 0 ;;
        --yaml)            shift; YAML="$1"; shift ;;
        --user)            shift; NEW_USER="$1"; shift ;;
        --password|--pwd)  shift; NEW_PWD="$1"; shift ;;
        --path)            shift; NEW_PATH="$1"; shift ;;
        --file)            shift; FILE_PATH="$1"; shift ;;
        --prefix)          shift; PREFIX="$1"; shift ;;
        --start)           shift; START_IP="$1"; shift ;;
        --force)           FORCE=1; shift ;;
        --*)               echo "unknown flag: $1" >&2; usage; exit 1 ;;
        *)
            if [ -n "$RAW_IPS" ]; then
                echo "duplicate args" >&2; usage; exit 1
            fi
            RAW_IPS="$1"; shift
            ;;
    esac
done

cd "${PROJECT_ROOT}"

if [ ! -f "${YAML}" ]; then
    echo "[ERR] yaml not found: ${YAML}" >&2
    exit 1
fi

IPS=()
if [ -n "${FILE_PATH}" ]; then
    if [ ! -f "${FILE_PATH}" ]; then
        echo "[ERR] file not found: ${FILE_PATH}" >&2
        exit 1
    fi
    while IFS= read -r line; do
        ip=$(echo "${line}" | tr -d ' \t"\047#')
        if [ -n "$ip" ]; then
            IPS+=("$ip")
        fi
    done < "${FILE_PATH}"
elif [ -n "${PREFIX}" ] && [ -n "${START_IP}" ]; then
    off=0
    while [ $off -le 5 ]; do
        IPS+=("${PREFIX}.$((START_IP + off))")
        off=$((off + 1))
    done
elif [ -n "${RAW_IPS}" ]; then
    IFS=',' read -ra parts <<< "${RAW_IPS}"
    for p in "${parts[@]}"; do
        ip=$(echo "${p}" | tr -d ' \t"\047')
        if [ -n "$ip" ]; then
            IPS+=("$ip")
        fi
    done
else
    echo "[ERR] no IPs given. Use --help." >&2
    exit 1
fi

N_IPS=${#IPS[@]}
if [ "${N_IPS}" -eq 0 ]; then
    echo "[ERR] no valid IPs" >&2
    exit 1
fi

echo "yaml path : ${YAML}"
echo "IP count   : ${N_IPS}"
echo "first IPs  : ${IPS[@]:0:6}"

N_YAML=$(grep -c '^[[:space:]]*-[[:space:]]*type:[[:space:]]*rtsp[[:space:]]*$' "${YAML}" 2>/dev/null || true)
echo "yaml rtsp count: ${N_YAML}"

if [ "${N_YAML}" -eq 0 ]; then
    echo "[ERR] yaml has no rtsp source, edit yaml to add a - type: rtsp block first" >&2
    exit 1
fi

if [ "${N_IPS}" -ne "${N_YAML}" ] && [ "${FORCE}" -ne 1 ]; then
    echo "[ERR] IP count ${N_IPS} != yaml rtsp count ${N_YAML}. Use --force to override." >&2
    exit 1
fi

BAK="${YAML}.bak.$(date +%Y%m%d-%H%M%S)"
cp "${YAML}" "${BAK}"
echo "backup: ${BAK}"

python3 "${SCRIPT_DIR}/_yaml_set_cams.py"
    "${YAML}"
    "${IPS[@]}"
    "${NEW_USER}" "${NEW_PWD}" "${NEW_PATH}"

echo
echo "after:"
grep -E "uri: rtsp://" "${YAML}" | head -10
echo
echo "Run: bash tools/run_rtsp_stitching.sh"