#!/usr/bin/env bash
#
# sync_green_stripe_fix.sh — PC 端 (开发机): 把本地新增/改动的绿条纹修复相关文件
# 推到开发板 (SSH 配置 rockemb 或 rk3588-6), 然后在板端执行 fix_green_stripes.sh.
#
# 与 sync_mjpeg_stage2.sh 同思路, 但只关注绿条纹修复这一组文件:
#   1. src/gst_mpp_decoder.cc                 (含 [DIAG] 日志)
#   2. include/gst_mpp_decoder.h              (含 first_frame_dumped_)
#   3. tools/batch_transcode_to_h264.sh       (新增)
#   4. tools/diagnose_pipeline.sh             (新增)
#   5. tools/fix_green_stripes.sh             (新增)
#   6. params/camera_sources.yaml             (改 url 到 2k-test-h264)
#
# 用法 (PC 上, pwsh/git-bash 都可):
#   bash sync_green_stripe_fix.sh                 # 默认同步 + 板上跑 fix
#   bash sync_green_stripe_fix.sh --no-run        # 只同步, 不在板上跑 fix
#   REMOTE=rockemb bash sync_green_stripe_fix.sh # 指明别名
#

set -euo pipefail

REMOTE="${REMOTE:-rk3588-6}"
DST="${DST:-/home/rocktech/Projects/new-4k-stitch}"
LOCAL="$(cd "$(dirname "$0")" && pwd)"
RUN_ON_BOARD=1
DRY_RUN=0

for arg in "$@"; do
  case "${arg}" in
    --no-run)  RUN_ON_BOARD=0 ;;
    --run)     RUN_ON_BOARD=1 ;;
    --dry-run) DRY_RUN=1 ;;
    -h|--help)
      sed -n '3,16p' "$0"; exit 0 ;;
    *) err "unknown arg: ${arg}" ;;
  esac
done

log() { printf "[sync] %s\n" "$*"; }
err() { printf "[sync][ERROR] %s\n" "$*" >&2; }

log "LOCAL = ${LOCAL}"
log "REMOTE = ${REMOTE}:${DST}"
log "RUN_ON_BOARD = ${RUN_ON_BOARD}"
log "DRY_RUN = ${DRY_RUN}"

# 前置: 远端可达.
if [ "${DRY_RUN}" != "1" ]; then
  ssh -o ConnectTimeout=4 -o BatchMode=yes "${REMOTE}" "echo BOARD_OK" >/dev/null 2>&1 \
    || { err "cannot reach ${REMOTE} (ssh config? ssh-copy-id?)"; exit 1; }
fi

# 同步文件列表.
sync_one() {
  local rel="$1"
  if [ "${DRY_RUN}" = "1" ]; then
    log "DRY: would scp ${LOCAL}/${rel} -> ${REMOTE}:${DST}/${rel}"
  else
    scp -q "${LOCAL}/${rel}" "${REMOTE}:${DST}/${rel}"
    log "scp ${rel}"
  fi
}

log "new files:"
sync_one "tools/batch_transcode_to_h264.sh"
sync_one "tools/diagnose_pipeline.sh"
sync_one "tools/fix_green_stripes.sh"

log "modified files:"
sync_one "src/gst_mpp_decoder.cc"
sync_one "include/gst_mpp_decoder.h"
sync_one "params/camera_sources.yaml"

# 把脚本设可执行.
if [ "${DRY_RUN}" != "1" ]; then
  ssh -o BatchMode=yes "${REMOTE}" "chmod +x ${DST}/tools/fix_green_stripes.sh ${DST}/tools/diagnose_pipeline.sh ${DST}/tools/batch_transcode_to_h264.sh 2>/dev/null || true"
fi

# 在板端跑修复.
if [ "${RUN_ON_BOARD}" = "1" ] && [ "${DRY_RUN}" != "1" ]; then
  log "invoke fix_green_stripes.sh on board"
  ssh -o BatchMode=yes "${REMOTE}" "cd ${DST} && bash tools/fix_green_stripes.sh"
else
  log "skipped board-side fix (RUN_ON_BOARD=${RUN_ON_BOARD})"
fi

log "DONE"