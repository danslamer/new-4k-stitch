#!/usr/bin/env bash
#
# tools/validate_ba_pipeline.sh
#
# v3.x.3 (2026-07-09): SIFT + BundleAdjusterAffinePartial 路径验证.
#   1. Cold-boot: STITCH_BA_SKIP=0 (默认). 等 pipeline 起来 → 看 log 有 BA succeeded 行
#      (cam0..cam5 R=... 输出).
#   2. Hot-boot: SKIP_BOOTSTRAP=1 + USE_ROI_CONFIG=1. 应该不跑 SIFT/BA, 但 yaml 里
#      cam[*].have_ba_R=1 持久化, log 显示 "yaml has have_ba_R for all 6 cams".
#   3. Fallback: STITCH_BA_SKIP=1. log 显示 "STITCH_BA_SKIP=1, skipping BA", 走
#      BuildAffineWarpData 老路径. 用于验证兜底路径不挂.
#
# 用法 (板端):
#   cd ~/Projects/new-4k-stitch
#   bash tools/validate_ba_pipeline.sh
#
# 期望: 三轮都启动起来且 stitch 进入稳态 (FPS > 25), grep 关键字命中.
# 失败: 任何一轮启动失败 / log 缺关键字 → 退出码 = 1.

set -uo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "${PROJECT_ROOT}"

if [ ! -x "${PROJECT_ROOT}/build/image-stitching" ]; then
    echo "[FATAL] ${PROJECT_ROOT}/build/image-stitching not found; run cmake/make first"
    exit 1
fi

LOG_DIR="${PROJECT_ROOT}/logs"
mkdir -p "${LOG_DIR}"

ROUNDS=("cold" "hot" "fallback")

PASS=0; FAIL=0
ok()   { printf "\033[32m[PASS]\033[0m %s\n" "$*"; PASS=$((PASS+1)); }
fail() { printf "\033[31m[FAIL]\033[0m %s\n" "$*"; FAIL=$((FAIL+1)); }
bar()  { printf "\n\033[1;34m===== %s =====\033[0m\n" "$*"; }
hr()   { printf "%s\n" "------------------------------------------------------------"; }

run_round() {
    local label="$1"; shift
    local log="${LOG_DIR}/ba_${label}.log"
    local pid

    cd "${PROJECT_ROOT}/build"
    "$@" > "${log}" 2>&1 &
    pid=$!

    # 给 12s 启动 + SIFT/BA, SIFT 大概在 6 路 2K 上要 2-3s.
    sleep 12
    if ! kill -0 "${pid}" 2>/dev/null; then
        fail "${label}: image-stitching died within 12s"
        cd "${PROJECT_ROOT}"
        return 1
    fi
    cd "${PROJECT_ROOT}"

    # 检查关键 log 行
    if [ "${label}" = "cold" ]; then
        if grep -qE "\[ba_estimator\] BA succeeded" "${log}" \
                && grep -qE "\[App\] \[BA\] running SIFT" "${log}"; then
            ok "${label}: SIFT+BA ran, BA succeeded"
        elif grep -qE "\[App\] \[BA\] failed" "${log}"; then
            fail "${label}: BA failed; check ba_estimator log lines"
        else
            fail "${label}: neither BA succeeded nor failed line in log"
        fi
        # R[] 一行
        if grep -qE "cam[0-5] R=\[" "${log}"; then
            ok "${label}: R matrix logged for at least one cam"
        else
            fail "${label}: no per-cam R in log"
        fi
    elif [ "${label}" = "hot" ]; then
        if grep -qE "yaml has have_ba_R for all 6 cams" "${log}"; then
            ok "${label}: yaml BA reused, no SIFT re-run"
        else
            fail "${label}: 'yaml has have_ba_R' line missing (yaml too old?)"
        fi
        if grep -qE "\[ba_estimator\] BuildStitcherWarpMaps succeeded" "${log}"; then
            ok "${label}: BuildStitcherWarpMaps produced maps"
        else
            warn "${label}: BuildStitcherWarpMaps did not run (check log)"
        fi
    elif [ "${label}" = "fallback" ]; then
        if grep -qE "STITCH_BA_SKIP=1, skipping BA" "${log}"; then
            ok "${label}: BA skipped, affine fallback"
        else
            fail "${label}: STITCH_BA_SKIP not honored"
        fi
        # BA 路径应不出现 (SIFT/BA succeeded/failed 不应有)
        if ! grep -qE "\[ba_estimator\] BA succeeded" "${log}"; then
            ok "${label}: BA path correctly disabled"
        else
            fail "${label}: BA path ran despite STITCH_BA_SKIP=1"
        fi
    fi

    # FPS 自检: 期望 ≥ 25.
    local fps
    fps=$(grep -oE "STITCH_PERF_FPS=[0-9.]+" "${log}" | tail -1 | cut -d= -f2)
    if [ -n "${fps}" ]; then
        local fps_int
        fps_int=$(printf "%.0f" "${fps}")
        if [ "${fps_int}" -ge 25 ]; then
            ok "${label}: FPS=${fps} (>= 25)"
        else
            fail "${label}: FPS=${fps} (< 25)"
        fi
    else
        warn "${label}: no STITCH_PERF_FPS found in log"
    fi

    kill "${pid}" 2>/dev/null
    wait "${pid}" 2>/dev/null
    return 0
}

bar "Cold-boot (BA runs once, ~3 s on first frame)"
run_round cold ./image-stitching

bar "Hot-boot (yaml has BA, no SIFT re-run)"
run_round hot env SKIP_BOOTSTRAP=1 USE_ROI_CONFIG=1 ./image-stitching

bar "Fallback (STITCH_BA_SKIP=1, BuildAffineWarpData path)"
run_round fallback env STITCH_BA_SKIP=1 SKIP_BOOTSTRAP=1 USE_ROI_CONFIG=1 ./image-stitching

hr
printf "PASS=%d FAIL=%d\n" "${PASS}" "${FAIL}"
if [ "${FAIL}" -gt 0 ]; then
    exit 1
fi
exit 0
