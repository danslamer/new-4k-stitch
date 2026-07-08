#!/usr/bin/env bash
#
# tools/fix_green_stripes.sh — 板端一键修复: 把数据集从 MPEG-4 转 H.264, 改 yaml, 重编, 重启.
# 这是 2026-07-08 绿条纹 bug 的端到端 onboard 修复脚本.
#
# 根因 (AGENTS.md / 报告):
#   datasets/2k-test/*.mp4 是 MPEG-4 Visual (mp4v), 但 gst_mpp_decoder.cc pipeline
#   强制 qtdemux → h264parse → mppvideodec, mp4v 编码被 h264parse 截断 / 失败 / 灰区,
#   最终 mppvideodec 拿到错位 NAL → 解码出绿条纹.
#
# 修复路径 (本脚本自动执行, 全自动, 无需交互):
#   Step 1: 调用 batch_transcode_to_h264.sh 把 2k-test/*.mp4 转 2k-test-h264/*.mp4
#   Step 2: sed 修改 params/camera_sources.yaml 的 uri 路径指向 2k-test-h264/
#           (OpenCV FileStorage 对缩进敏感, 用 sed 字符串替换最稳)
#   Step 3: (可选, SKIP_BUILD=1 跳过) 重编 image-stitching 拿新代码
#   Step 4: (可选, SKIP_RESTART=1 跳过) 先杀孤儿进程, 再启动 camerapage 服务
#
# 失败保护:
#   - 转码后必须 ffprobe 校验 6 路文件都是 H.264, 不通过拒绝 sed yaml
#   - sed 之前先把 yaml 备份到 .bak, 失败可还原
#

set -euo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-/home/rocktech/Projects/new-4k-stitch}"
SKIP_BUILD="${SKIP_BUILD:-0}"
SKIP_RESTART="${SKIP_RESTART:-0}"
SKIP_TRANSCODE="${SKIP_TRANSCODE:-0}"
FORCE_TRANSCODE="${FORCE_TRANSCODE:-0}"

log()  { printf "[fix_green] %s\n" "$*"; }
warn() { printf "[fix_green] [WARN] %s\n"  "$*" >&2; }
err()  { printf "[fix_green] [ERROR] %s\n" "$*" >&2; }

cd "${PROJECT_ROOT}"

log "PROJECT_ROOT=${PROJECT_ROOT}"
log "SKIP_BUILD=${SKIP_BUILD}  SKIP_RESTART=${SKIP_RESTART}  SKIP_TRANSCODE=${SKIP_TRANSCODE}  FORCE_TRANSCODE=${FORCE_TRANSCODE}"

command -v ffmpeg >/dev/null 2>&1 || { err "ffmpeg not found, apt-get install ffmpeg"; exit 1; }
[ -f params/camera_sources.yaml ] || { err "params/camera_sources.yaml not found"; exit 1; }
[ -d datasets/2k-test ] || { err "datasets/2k-test missing"; exit 1; }

# ---------- 1. 转码 ----------
if [ "${SKIP_TRANSCODE}" = "1" ]; then
  log "step 1 SKIP transcode (SKIP_TRANSCODE=1)"
else
  log "step 1 transcode datasets/2k-test/*.mp4 -> datasets/2k-test-h264/*.mp4"
  FORCE="${FORCE_TRANSCODE}" bash tools/batch_transcode_to_h264.sh
fi

H264_DIR="${PROJECT_ROOT}/datasets/2k-test-h264"
[ -d "${H264_DIR}" ] || { err "${H264_DIR} missing"; exit 1; }
missing=0
for base in t50 t51 t52 t53 t40 t41; do
  if [ ! -f "${H264_DIR}/${base}.mp4" ]; then
    err "missing h264 file: ${H264_DIR}/${base}.mp4"
    missing=$((missing + 1))
  else
    if ! ffprobe -v error -select_streams v:0 -show_entries stream=codec_name \
        -of default=nw=1:nk=1 "${H264_DIR}/${base}.mp4" | grep -q "h264"; then
      err "${H264_DIR}/${base}.mp4 is not H.264 (transcode broken)"; missing=$((missing + 1))
    fi
  fi
done
[ "${missing}" -eq 0 ] || { err "${missing} h264 files missing/invalid, abort"; exit 1; }
log "all 6 h264 files validated"

# ---------- 2. yaml 改 path ----------
log "step 2 patch params/camera_sources.yaml (2k-test -> 2k-test-h264)"
backup="params/camera_sources.yaml.$(date +%Y%m%d_%H%M%S).bak"
cp params/camera_sources.yaml "${backup}"
log "    backup -> ${backup}"

before=$(grep -c 'datasets/2k-test/t' params/camera_sources.yaml || true)
sed -i 's|datasets/2k-test/t|datasets/2k-test-h264/t|g' params/camera_sources.yaml
after=$(grep -c 'datasets/2k-test-h264/t' params/camera_sources.yaml || true)
log "    yaml uri replace: before=${before} after=${after}"
if [ "${after}" -lt 6 ]; then
  err "yaml 改完后 h264 uri 数量 ${after} < 6, rolling back"
  cp "${backup}" params/camera_sources.yaml
  exit 1
fi

# ---------- 3. 重编 ----------
if [ "${SKIP_BUILD}" = "1" ]; then
  log "step 3 SKIP build (SKIP_BUILD=1)"
else
  log "step 3 rebuild image-stitching (cmake/make)"
  mkdir -p build
  (cd build && cmake .. >/dev/null && make -j"$(nproc)") || {
    err "build failed"; exit 1;
  }
  log "    build ok -> build/image-stitching"
fi

# ---------- 4. 重启 ----------
if [ "${SKIP_RESTART}" = "1" ]; then
  log "step 4 SKIP restart (SKIP_RESTART=1)"
  log "DONE (build ok, please restart manually: bash deploy/camerapage_test_backend.sh restart)"
  exit 0
fi

log "step 4 ensure no orphan image-stitching, then restart camerapage service"
# 杀所有孤儿 image-stitching (camerapage_test_backend.sh 的 restart 只管 PID 文件里的;
# 用户手工启动 / 之前跑挂的进程都要清掉, 否则新 binary 会复用旧 mapping).
for sig in TERM KILL; do
  pids=$(pgrep -f 'image-stitching' || true)
  if [ -z "${pids}" ]; then break; fi
  log "    kill -${sig} orphan image-stitching: $(echo "${pids}" | tr '\n' ' ')"
  kill -${sig} $(echo "${pids}" | tr '\n' ' ') 2>/dev/null || true
  sleep 1
  if pgrep -f 'image-stitching' >/dev/null 2>&1; then
    if [ "${sig}" = "TERM" ]; then continue; fi
  fi
  break
done
if pgrep -f 'image-stitching' >/dev/null 2>&1; then
  err "image-stitching 仍存活, 请手工 kill 后重试"
  pgrep -af 'image-stitching' || true
  exit 1
fi

if [ -x deploy/camerapage_test_backend.sh ]; then
  bash deploy/camerapage_test_backend.sh restart || true
else
  warn "deploy/camerapage_test_backend.sh not found — 手工启动 image-stitching"
  if [ -x build/image-stitching ]; then
    nohup ./build/image-stitching >/tmp/image-stitching.log 2>&1 &
    log "    started build/image-stitching (PID $!)"
  else
    err "build/image-stitching 不存在, 请先 build"
    exit 1
  fi
fi

# 尾日志.
LOG_DIR="${PROJECT_ROOT}/logs"
mkdir -p "${LOG_DIR}"
STITCH_LOG="${LOG_DIR}/image-stitching.log"
if [ -f "${STITCH_LOG}" ]; then
  log "tail 60s of ${STITCH_LOG}:"
  timeout 60 tail -n 200 -f "${STITCH_LOG}" | grep -E '\[(decoder |App|gst_mpp_decoder|camerapage|ERROR)' || true
fi

log "DONE"
echo ""
echo "post-fix verification:"
echo "  bash tools/diagnose_pipeline.sh            # 应看到 'all input files are H.264'"
echo "  tail -f logs/image-stitching.log | grep DIAG   # 应看到 layout=1 (single DMA-BUF)"
exit 0