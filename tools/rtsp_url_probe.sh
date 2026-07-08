#!/usr/bin/env bash
#
# tools/rtsp_url_probe.sh
#
# 独立 RTSP URL 探测脚本: 不依赖 image-stitching 编译, 板端拿到摄像机
# IP 后第一件事就能跑。验证 RTSP URL / 凭据 / 编码 / GOP 大小等核心参数,
# 避免后面 yaml 配错来回改。
#
# 用法:
#   # 探测 yaml 里所有 6 路
#   bash tools/rtsp_url_probe.sh
#
#   # 探测单个 rtsp URL
#   bash tools/rtsp_url_probe.sh "rtsp://admin:Abcd1234@192.168.10.21:554/Streaming/Channels/101"
#
#   # 探测一组 IP (默认端口 554, 默认路径 /Streaming/Channels/101)
#   bash tools/rtsp_url_probe.sh -i 192.168.10.21 192.168.10.22 ...
#
#   # 探测 ONVIF 风格的路径
#   bash tools/rtsp_url_probe.sh -i 192.168.10.21 -p 554 -u admin -w Abcd1234 \
#                              -t /live/main
#
#   # 探测默认海康 RTSP URL 格式
#   bash tools/rtsp_url_probe.sh -i 192.168.10.21 -u admin -w Abcd1234 \
#                              -t /Streaming/Channels/101
#

set -uo pipefail

PROBE_TIMEOUT=10      # 单 URL 探测超时 (秒)
TCP_TIMEOUT=3         # TCP 端口可达检查超时 (秒)
LOG="${LOG:-/tmp/rtsp_probe.log}"

# args
USER_ID="admin"
USER_PW="CHANGE_ME"
TARGETS=()
INDIVIDUAL_URL=""
PORT=554
PATH_TEMPLATE="/Streaming/Channels/101"

usage() {
    sed -n '3,40p' "$0"
    exit "${1:-0}"
}

# parse args
while [ "$#" -gt 0 ]; do
    case "$1" in
        -h|--help)       usage 0 ;;
        -i|--ip)         shift; while [ "$#" -gt 0 ] && [[ ! "$1" =~ ^- ]]; do TARGETS+=("$1"); shift; done ;;
        -u|--user)       shift; USER_ID="$1"; shift ;;
        -w|--password)   shift; USER_PW="$1"; shift ;;
        -p|--port)       shift; PORT="$1"; shift ;;
        -t|--path)       shift; PATH_TEMPLATE="$1"; shift ;;
        -l|--log)        shift; LOG="$1"; shift ;;
        --tcp-timeout)   shift; TCP_TIMEOUT="$1"; shift ;;
        --probe-timeout) shift; PROBE_TIMEOUT="$1"; shift ;;
        -*)
            echo "unknown flag: $1" >&2; usage 1 ;;
        *)
            INDIVIDUAL_URL="$1"; shift ;;
    esac
done

mkdir -p "$(dirname "${LOG}")"
: > "${LOG}"

bar() { printf "\n\033[1;34m===== %s =====\033[0m\n" "$*"; }
ok()   { printf "  \033[32m[OK]\033[0m %s\n" "$*"; }
warn() { printf "  \033[33m[WARN]\033[0m %s\n" "$*"; }
fail() { printf "  \033[31m[FAIL]\033[0m %s\n" "$*"; }

# 构造 URL
build_url() {
    local ip="$1"
    echo "rtsp://${USER_ID}:${USER_PW}@${ip}:${PORT}${PATH_TEMPLATE}"
}

# 探测单个 URL
probe_one() {
    local url="$1"
    local ip
    ip=$(echo "${url}" | sed -E 's|rtsp://[^@]+@([^:/]+).*|\1|')
    local port
    port=$(echo "${url}" | sed -E 's|.*:([0-9]+).*|\1|')

    bar "PROBE  ${url}"

    # 1. TCP port
    if timeout "${TCP_TIMEOUT}" bash -c "</dev/tcp/${ip}/${port}" 2>/dev/null; then
        ok "TCP/${port} reachable on ${ip}"
    else
        fail "TCP/${port} unreachable on ${ip} (PoE/网线?)"
        return 1
    fi

    # 2. gst-launch 拉 30 帧, 检查 mppvideodec 输出
    local out
    out=$(timeout "${PROBE_TIMEOUT}" gst-launch-1.0 -v \
        rtspsrc location="${url}" latency=120 protocols=4 \
        ! rtph264depay ! h264parse \
        ! mppvideodec dma-feature=true format=NV12 \
        ! "video/x-raw(memory:DMABuf),format=NV12" \
        ! fakesink num-buffers=30 2>&1 | tee -a "${LOG}" || true)

    local has_err has_dmabuf has_play has_negotiated has_pm_codec

    if echo "${out}" | grep -qE "ERROR .*pipeline"; then
        fail "pipeline ERROR:"
        echo "${out}" | grep "ERROR" | head -3 | sed "s/^/    /"
        warn "可能原因: URL 路径错 / 凭据错 / 摄像机 RTSP 未开启"
        return 1
    fi
    ok "pipeline 不报 ERROR"

    if echo "${out}" | grep -qE "video/x-raw.*memory:DMABuf"; then
        ok "mppvideodec 输出 DMA-Buf (零拷贝路径生效)"
    else
        fail "mppvideodec 没输出 DMA-Buf caps"
    fi

    if echo "${out}" | grep -qE "state.*PLAYING|fakesink.*handoff"; then
        ok "pipeline 进入 PLAYING (拉到 ≥ 1 帧)"
    else
        warn "没看到 PLAYING/handoff (可能 0 帧, 检查 GOP/NAL)"
    fi

    # negotiated caps 看一下实际尺寸
    if echo "${out}" | grep -qE "caps: video/x-raw.*width"; then
        caps_line=$(echo "${out}" | grep -E "caps: video/x-raw" | head -1)
        echo "    ${caps_line}"
        ok "mppvideodec 协商通过"
    fi

    return 0
}

# 收集 URLs
URLS=()
if [ -n "${INDIVIDUAL_URL}" ]; then
    URLS+=("${INDIVIDUAL_URL}")
elif [ "${#TARGETS[@]}" -gt 0 ]; then
    for ip in "${TARGETS[@]}"; do
        URLS+=("$(build_url "${ip}")")
    done
elif [ -f params/camera_sources.yaml ]; then
    while IFS= read -r url; do
        [ -z "${url}" ] && continue
        URLS+=("${url}")
    done < <(grep -E 'uri:' params/camera_sources.yaml \
             | sed -E "s/^[[:space:]]*uri:[[:space:]]*['\"]?//; s/['\"][[:space:]]*$//" \
             | head -10)
else
    fail "yaml 不存在且未传 URL"
    usage 1
fi

if [ "${#URLS[@]}" -eq 0 ]; then
    fail "没找到任何 URL"
    exit 1
fi

# 逐个探测
PASS=0; FAIL=0
for url in "${URLS[@]}"; do
    if probe_one "${url}"; then
        PASS=$((PASS+1))
    else
        FAIL=$((FAIL+1))
    fi
done

bar "SUMMARY"
echo "  探测 ${#URLS[@]} 路, PASS=${PASS}, FAIL=${FAIL}"
echo "  详 log: ${LOG}"

if [ "${FAIL}" -ne 0 ]; then
    exit 1
fi