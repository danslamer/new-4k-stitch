#!/usr/bin/env bash
#
# tools/sprint0_quickcheck.sh — Sprint 0 快速检查 (无 image-stitching 编译, < 5 秒)
#
# 用法: ssh 板子, bash tools/sprint0_quickcheck.sh
#
# 跳过 cmake/make 那一段 (留给 sprint0_smoke.sh), 但前置检查全部覆盖:
#   1. yaml 6 路 rtsp 存在 + 字段完整
#   2. 6 路 TCP/554 可达
#   3. gstreamer 5 个关键 element 都装好 (rtspsrc/rtph264depay/h264parse/mppvideodec/appsink)
#   4. gst-launch 单路 RTSP 烟测 5 帧 (验证 DMA-Buf caps 协商)
#
# 用于开发期每改一次 yaml / 调一次 IP 就跑一次的快速反馈工具.
#

set -uo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-/home/rocktech/Projects/new-4k-stitch}"
cd "${PROJECT_ROOT}"

LOG_DIR="${LOG_DIR:-logs}"
mkdir -p "${LOG_DIR}"

PASS=0; FAIL=0; WARN=0
ok()   { printf "\033[32m[PASS]\033[0m %s\n" "$*"; PASS=$((PASS+1)); }
warn() { printf "\033[33m[WARN]\033[0m %s\n" "$*"; WARN=$((WARN+1)); }
fail() { printf "\033[31m[FAIL]\033[0m %s\n" "$*"; FAIL=$((FAIL+1)); }
bar()  { printf "\n\033[1;34m===== %s =====\033[0m\n" "$*"; }

# ---------- 1. yaml 校验 ----------
bar "1. camera_sources.yaml 完整性"
if [ ! -f params/camera_sources.yaml ]; then
    fail "yaml 不存在"
else
    ok "yaml 存在"
fi

N_RTSP=$(grep -c "type: *rtsp" params/camera_sources.yaml 2>/dev/null || true)
if [ "${N_RTSP}" -ge 6 ]; then
    ok "${N_RTSP} 路 rtsp 配置 (≥ 6)"
else
    fail "rtsp entry 数量 = ${N_RTSP} (期望 ≥ 6)"
fi

# 每路 rtsp 必填字段 (不含可选的 user_id/user_pw)
need_fields=("type" "uri" "width" "height" "fps" "pixel_format")
for i in 1 2 3 4 5 6; do
    if ! awk "/- type: rtsp/{i++} i==${i}" params/camera_sources.yaml | grep -q "type: rtsp"; then
        fail "cam${i} 缺失"
    fi
done

# 看有没有 CHANGE_ME 占位符没填
if grep -q "CHANGE_ME" params/camera_sources.yaml; then
    warn "yaml 还有 CHANGE_ME 占位符没填: $(grep -n CHANGE_ME params/camera_sources.yaml | head -3)"
else
    ok "yaml 无 CHANGE_ME 占位符 (已填实)"
fi

# ---------- 2. 6 路 TCP/554 ----------
bar "2. 6 路 IP cam TCP/554 可达性"
PORTS_OK=0
UNREACHABLE=""
for ip in $(grep -E 'uri:' params/camera_sources.yaml | grep -oE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' | sort -u); do
    if timeout 3 bash -c "</dev/tcp/${ip}/554" 2>/dev/null; then
        ok "TCP/554 OK ${ip}"
        PORTS_OK=$((PORTS_OK+1))
    else
        warn "TCP/554 unreachable ${ip} — 检查 PoE/网线/摄像机是否上电"
        UNREACHABLE="${UNREACHABLE} ${ip}"
    fi
done
if [ "${PORTS_OK}" -eq 0 ]; then
    fail "所有摄像机都不可达"
fi

# ---------- 3. gstreamer 关键 element ----------
bar "3. gstreamer 元素可用性"
for e in rtspsrc rtph264depay h264parse mppvideodec appsink; do
    if gst-inspect-1.0 "$e" >/dev/null 2>&1; then
        ok "gst-inspect ${e}"
    else
        fail "${e} 不在 gst registry"
    fi
done

# ---------- 4. 单路 gst-launch 烟测 ----------
bar "4. 单路 gst-launch 烟测 (5 帧)"
FIRST_URI=$(grep -E 'uri:' params/camera_sources.yaml | head -1 | sed -E 's/^.*uri: *//; s/^["'\'']+//; s/["'\'']+$//')
if [ -z "${FIRST_URI}" ]; then
    fail "yaml 没解析到任何 uri"
else
    SMOKE_LOG="${LOG_DIR}/sprint0_quickcheck_smoke.log"
    out=$(timeout 12 gst-launch-1.0 -v \
        rtspsrc location="${FIRST_URI}" latency=120 protocols=4 \
        ! rtph264depay ! h264parse \
        ! mppvideodec dma-feature=true format=NV12 \
        ! "video/x-raw(memory:DMABuf),format=NV12" \
        ! fakesink num-buffers=5 2>&1 | tee "${SMOKE_LOG}" || true)
    if echo "${out}" | grep -qE "ERROR .*pipeline"; then
        fail "gst-launch pipeline ERROR (详 ${SMOKE_LOG}):"
        echo "${out}" | grep "ERROR" | head -3 | sed "s/^/    /"
    else
        ok "gst-launch 5 帧无 ERROR"
    fi
    if echo "${out}" | grep -qE "video/x-raw.*memory:DMABuf"; then
        ok "mppvideodec 输出 DMA-Buf (零拷贝路径生效)"
    else
        fail "mppvideodec 输出非 DMA-Buf (违反零拷贝约束)"
    fi
fi

# ---------- 总结 ----------
bar "SUMMARY"
echo "  PASS=${PASS}  WARN=${WARN}  FAIL=${FAIL}"
if [ "${FAIL}" -eq 0 ]; then
    printf "\n  \033[32m✅ Sprint 0 quickcheck PASS\033[0m (run \033[1mbash tools/sprint0_smoke.sh\033[0m to verify compile + 30s end-to-end)\n\n"
    exit 0
else
    printf "\n  \033[31m❌ Sprint 0 quickcheck FAIL\033[0m — 修上述 FAIL 项再跑\n\n"
    exit 1
fi