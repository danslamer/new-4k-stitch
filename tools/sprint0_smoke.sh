#!/usr/bin/env bash
#
# tools/sprint0_smoke.sh
#
# Sprint 0 一键验收脚本 (matches docs/NETWORK_CAMERA_PLAN.md §16):
#   6 路 IP cam RTSP → 6 路 DMA-BUF → 6 路 2×3 拼接出图
#
# 用法 (板端):
#   ssh rocktech@192.168.10.100
#   cd ~/Projects/new-4k-stitch
#   bash tools/sprint0_smoke.sh
#
# 不依赖 image-stitching 完整跑通: 6 步独立 PASS/FAIL, 任意一步失败可定位。
#

set -uo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-/home/rocktech/Projects/new-4k-stitch}"
cd "${PROJECT_ROOT}"

LOG_DIR="${LOG_DIR:-logs}"
mkdir -p "${LOG_DIR}"

PASS=0; FAIL=0; WARN=0
ok()    { printf "\033[32m[PASS]\033[0m %s\n" "$*"; PASS=$((PASS+1)); }
warn()  { printf "\033[33m[WARN]\033[0m %s\n" "$*"; WARN=$((WARN+1)); }
fail()  { printf "\033[31m[FAIL]\033[0m %s\n" "$*"; FAIL=$((FAIL+1)); }
bar()   { printf "\n\033[1;34m===== %s =====\033[0m\n" "$*"; }
hr()    { printf "%s\n" "------------------------------------------------------------"; }

EXITED_PIDS=()
cleanup() {
    for pid in "${EXITED_PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done
    sleep 0.2
    jobs -p 2>/dev/null | xargs -r kill 2>/dev/null || true
}
trap cleanup EXIT

# ---------- 1. yaml + INPUT_SOURCE_MODE ----------
bar "1. camera_sources.yaml 配置"

if [ ! -f params/camera_sources.yaml ]; then
    fail "params/camera_sources.yaml not found"
else
    ok "params/camera_sources.yaml 存在"
fi

if [ ! -r params/camera_sources.yaml ]; then
    warn "yaml 不可读 (权限不足)"
fi

echo "  INPUT_SOURCE_MODE=${INPUT_SOURCE_MODE:-<unset, 默认 dataset (不适用于 Sprint 0)>}"
case "${INPUT_SOURCE_MODE:-dataset}" in
    camera|rtsp|streaming)
        ok "INPUT_SOURCE_MODE 走 camera/rtsp 分支" ;;
    dataset)
        warn "INPUT_SOURCE_MODE=dataset (Sprint 0 应改 camera/yaml 切换)" ;;
    *)
        fail "INPUT_SOURCE_MODE 未知值" ;;
esac

# 6 路 rtsp 计数
N_RTSP=$(grep -c 'type: *rtsp' params/camera_sources.yaml 2>/dev/null || echo 0)
N_FILE=$(grep -c 'type: *file' params/camera_sources.yaml 2>/dev/null || echo 0)
echo "  yaml: rtsp=${N_RTSP}, file=${N_FILE}"
if [ "${N_RTSP}" -ge 1 ] && [ "${N_RTSP}" -ge "${N_FILE}" ]; then
    ok "${N_RTSP} 路 rtsp (≥ file 数 ${N_FILE}, 配置以 rtsp 为主)"
elif [ "${N_RTSP}" -eq 0 ]; then
    fail "yaml 没有任何 rtsp 源 — Sprint 0 起跑前提失败"
else
    warn "yaml 同时含 rtsp + file, Sprint 0 一律走 rtsp 优先"
fi

# ---------- 2. 6 路 TCP port 可达 ----------
bar "2. 6 路 IP cam TCP/554 可达性"

PORTS_OK=0
for ip in $(grep -E 'uri:' params/camera_sources.yaml 2>/dev/null \
            | grep -oE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' | sort -u); do
    if timeout 3 bash -c "</dev/tcp/${ip}/554" 2>/dev/null; then
        ok "TCP/554 reachable on ${ip}"
        PORTS_OK=$((PORTS_OK+1))
    else
        warn "TCP/554 unreachable on ${ip} — 检查 PoE/网线/摄像机是否上电"
    fi
done

if [ "${PORTS_OK}" -eq 0 ]; then
    fail "所有摄像机都不可达 — 不能进入解码测试"
fi

# ---------- 3. gst-launch 单路烟测 ----------
bar "3. 单路 gst-launch 烟测 (5 帧 + DMABuf 校验)"

FIRST_URI=$(grep -E 'uri:' params/camera_sources.yaml \
            | head -1 | sed -E 's/^.*uri: *//; s/^["'\'']+//; s/["'\'']+$//')

if [ -z "${FIRST_URI}" ]; then
    fail "yaml 没解析到任何 uri (字段可能是 type:file 的路径)"
else
    ok "首路 URI: ${FIRST_URI}"

    SMOKE_LOG="${LOG_DIR}/sprint0_smoke_gst.log"
    SMOKE_OUT=$(timeout 15 gst-launch-1.0 -v \
        rtspsrc location="${FIRST_URI}" latency=120 protocols=4 \
        ! rtph264depay ! h264parse \
        ! mppvideodec dma-feature=true format=NV12 \
        ! "video/x-raw(memory:DMABuf),format=NV12" \
        ! fakesink num-buffers=5 2>&1 | tee "${SMOKE_LOG}" || true)

    if echo "${SMOKE_OUT}" | grep -qE "ERROR .*pipeline"; then
        fail "gst-launch pipeline ERROR (详 ${SMOKE_LOG}):"
        echo "${SMOKE_OUT}" | grep -E "ERROR" | head -5 | sed 's/^/    /'
    else
        ok "gst-launch 5 帧 pipeline 不报 ERROR"
    fi

    if echo "${SMOKE_OUT}" | grep -qE "video/x-raw.*memory:DMABuf"; then
        ok "mppvideodec 输出 video/x-raw(memory:DMABuf) — 零拷贝路径生效"
    else
        fail "mppvideodec 输出 caps 不含 memory:DMABuf (违反零拷贝约束)"
        echo "${SMOKE_OUT}" | grep -E "caps|negotiated" | head -5 | sed 's/^/    /'
    fi

    if echo "${SMOKE_OUT}" | grep -qE "fakesink0.*handoff|state.*PLAYING"; then
        ok "fakesink 进入 PLAYING (5 帧完成)"
    else
        warn "fakesink handoff 没看到 — 烟测可能超时但 mppvideodec 已协商"
    fi
fi

# ---------- 4. gstreamer1.0-plugins-good 包含 rtspsrc ----------
bar "4. gstreamer 元素可用性"

need="rtspsrc rtph264depay h264parse mppvideodec appsink"
for e in $need; do
    if gst-inspect-1.0 "$e" >/dev/null 2>&1; then
        ok "gst-inspect $e"
    else
        fail "$e 不在 gstreamer registry (装 gstreamer1.0-plugins-good / gst-rockchip)"
    fi
done

# ---------- 5. image-stitching 编译 ----------
bar "5. image-stitching 编译"

if [ ! -f CMakeLists.txt ]; then
    fail "CMakeLists.txt 缺失"; exit 1
fi

mkdir -p build
if ! (cd build && cmake -DCMAKE_BUILD_TYPE=Release .. > /tmp/sprint0_cmake.log 2>&1); then
    fail "cmake 配置失败:"
    tail -20 /tmp/sprint0_cmake.log | sed 's/^/    /'
else
    ok "cmake 配置 OK"
fi

if ! (cd build && make -j"$(nproc 2>/dev/null || echo 2)" > /tmp/sprint0_make.log 2>&1); then
    fail "make 编译失败:"
    tail -30 /tmp/sprint0_make.log | sed 's/^/    /'
else
    ok "make 编译 OK"
fi

if [ -x build/image-stitching ]; then
    ok "build/image-stitching 可执行"
else
    fail "build/image-stitching 不存在或不可执行"
fi

# ---------- 6. 跑 image-stitching 30 秒, 解码 & stitch FPS ----------
bar "6. image-stitching 端到端 30s 跑通"

SPT_LOG="${LOG_DIR}/sprint0_image.log"
rm -f /tmp/stitch_status.json

INPUT_SOURCE_MODE="${INPUT_SOURCE_MODE:-camera}" \
    ./build/image-stitching > "${SPT_LOG}" 2>&1 &
SPT_PID=$!
EXITED_PIDS+=("${SPT_PID}")

sleep 30

if ! kill -0 "${SPT_PID}" 2>/dev/null; then
    fail "image-stitching 在 30s 内退出 (PID=${SPT_PID} 已死)"
    tail -30 "${SPT_LOG}" | sed 's/^/    /'
else
    ok "image-stitching 跑 30s 仍存活 (PID=${SPT_PID})"
fi

# 解码 FPS (5 路以上 ≥ 20 fps 视为正常)
ALL_DECODE_OK=true
for i in 0 1 2 3 4 5; do
    line=$(grep "decoder_perf ${i}" "${SPT_LOG}" | tail -1 || true)
    fps=$(echo "${line}" | grep -oE "fps=[0-9.]+" | head -1 | sed 's/fps=//' || true)
    if [ -z "${fps}" ]; then
        warn "cam${i} 无 decoder_perf 日志 (可能 init 中)"
        ALL_DECODE_OK=false
    else
        fps_int=$(printf "%.0f" "${fps}")
        if [ "${fps_int}" -lt 20 ]; then
            warn "cam${i} fps=${fps} (< 20 fps)"
            ALL_DECODE_OK=false
        else
            ok "cam${i} fps=${fps}"
        fi
    fi
done

# DIAG 段验证: 每路 layout=1 (单 fd NV12)
DIAG_LAYOUT1=$(grep -c "gst_mpp_decoder.*DIAG.*layout=1" "${SPT_LOG}" 2>/dev/null || echo 0)
if [ "${DIAG_LAYOUT1}" -ge 6 ]; then
    ok "${DIAG_LAYOUT1} 个 DIAG 段 layout=1 (单 fd NV12, 零拷贝)"
else
    warn "只有 ${DIAG_LAYOUT1} 个 DIAG 段 layout=1 (期望 ≥ 6, 可能 init 未全部触发)"
fi

# layout>=2 警告
DIAG_LAYOUT_GE2=$(grep -c "gst_mpp_decoder.*DIAG.*layout=[2-9]" "${SPT_LOG}" 2>/dev/null || echo 0)
if [ "${DIAG_LAYOUT_GE2}" -gt 0 ]; then
    fail "${DIAG_LAYOUT_GE2} 路 mppvideodec 输出多 fd (UV 平面丢失, 必触发绿条纹)"
fi

# CameraPage JSON
if [ -f /tmp/stitch_status.json ]; then
    if grep -q "rtsp://" /tmp/stitch_status.json; then
        ok "/tmp/stitch_status.json 含 rtsp:// URL"
    else
        fail "/tmp/stitch_status.json 不含 rtsp:// URL (status_writer 没接 yaml)"
    fi
else
    fail "/tmp/stitch_status.json 没生成"
fi

# ---------- 清理 & 总结 ----------
bar "清理后台进程"
kill "${SPT_PID}" 2>/dev/null || true
sleep 1

hr
bar "SUMMARY"
echo "  PASS=${PASS}  WARN=${WARN}  FAIL=${FAIL}"

if [ "${FAIL}" -eq 0 ]; then
    printf "\n  \033[32m✅ Sprint 0 PASS — 进入 Sprint 1 (同步 + 网络稳定)\033[0m\n\n"
    echo "  下一阶段:"
    echo "    - 在 6 路摄像机 web 后台开 NTP 客户端 (→ 板端 chrony)"
    echo "    - 调整 yaml latency_ms = 80~200 试延迟最佳点"
    echo "    - 实现 get_frame_vector_lockstep (文档 §6.4)"
    exit 0
else
    printf "\n  \033[31m❌ Sprint 0 FAIL — 修 FAIL 项再跑\033[0m\n\n"
    echo "  快速排查:"
    echo "    - logs/sprint0_smoke_gst.log    (gst-launch 烟测)"
    echo "    - logs/sprint0_image.log         (image-stitching 主循环)"
    echo "    - /tmp/sprint0_cmake.log         (cmake 配置)"
    echo "    - /tmp/sprint0_make.log          (make 编译)"
    exit 1
fi