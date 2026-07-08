#!/usr/bin/env bash
#
# tools/diagnose_pipeline.sh — 板端诊断: 看 camera_sources.yaml 加载 + mppvideodec 支持 + 真实 gst-launch 推一行.
# 这是绿条纹 bug 的第一道证据: 是否 qtdemux 输出 mp4v 而 pipeline 强制 h264parse.
#
# 用法:
#   bash tools/diagnose_pipeline.sh                       # 用 params/camera_sources.yaml 第 1 路
#   bash tools/diagnose_pipeline.sh datasets/2k-test/t50.mp4
#   bash tools/diagnose_pipeline.sh --all                 # 跑 yaml 里所有 6 路
#
# 输出:
#   - stanza 1: yaml 加载 + INPUT_SOURCE_MODE
#   - stanza 2: 对每路文件 ffprobe 探测 codec
#   - stanza 3: gst-inspect mppvideodec src pad caps (看支持哪些 codec)
#   - stanza 4: gst-launch 试拉 5 帧, 捕获 mppvideodec 的 src caps (✅ / ❌)
#

set -uo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-/home/rocktech/Projects/new-4k-stitch}"

# 解析参数.
TARGET_FILE=""
ALL=0
for arg in "$@"; do
  case "${arg}" in
    --all) ALL=1 ;;
    -h|--help)
      sed -n '4,20p' "$0"; exit 0 ;;
    *) TARGET_FILE="${arg}" ;;
  esac
done

bar() { printf "\n===== %s =====\n" "$*"; }
ok()  { printf "[OK]    %s\n" "$*"; }
warn(){ printf "[WARN]  %s\n" "$*"; }
err() { printf "[ERR]   %s\n" "$*"; }

cd "${PROJECT_ROOT}"

# ---------- 1. yaml 状态 ----------
bar "1. camera_sources.yaml + INPUT_SOURCE_MODE"
echo "  INPUT_SOURCE_MODE=${INPUT_SOURCE_MODE:-<unset, defaults to dataset>}"
ls -la params/camera_sources.yaml 2>&1 | sed 's/^/    /'

# 抽取 yaml 内的 uri (粗略, 不依赖 yaml parser).
if [ -f params/camera_sources.yaml ]; then
  echo "  URIs in yaml:"
  grep -E '^\s*uri:' params/camera_sources.yaml | sed 's/^/    /'
fi

# ---------- 2. ffprobe codec ----------
bar "2. ffprobe codec of input file(s)"
declare -a files
if [ -n "${TARGET_FILE}" ]; then
  files=("${TARGET_FILE}")
elif [ "${ALL}" = "1" ]; then
  if [ -f params/camera_sources.yaml ]; then
    mapfile -t files < <(grep -E '^\s*uri:' params/camera_sources.yaml | awk '{print $2}')
  else
    files=(datasets/2k-test/t50.mp4)
  fi
else
  if [ -f params/camera_sources.yaml ]; then
    first_uri=$(grep -E '^\s*uri:' params/camera_sources.yaml | head -1 | awk '{print $2}')
    files=("${first_uri}")
  else
    files=(datasets/2k-test/t50.mp4)
  fi
fi

probe_one() {
  local f="$1"
  if [ ! -f "${f}" ]; then
    err "${f} not found"; return 1
  fi
  local info
  info="$(ffprobe -v error -select_streams v:0 -show_entries \
    stream=codec_name,codec_type,width,height,pix_fmt -of default=nw=1 \
    "${f}" 2>&1)"
  echo "    ${f}"
  echo "${info}" | sed 's/^/      /'
  if echo "${info}" | grep -q "codec_name=mpeg4"; then
    err "${f} is MPEG-4 (mp4v) — pipeline (qtdemux → h264parse → mppvideodec) WILL mismatch and produce green stripes"
    return 1
  fi
  if echo "${info}" | grep -q "codec_name=h264"; then
    ok "${f} is H.264 — pipeline should work"
    return 0
  fi
  return 0
}

probe_status=0
for f in "${files[@]}"; do
  probe_one "${f}" || probe_status=1
done

# ---------- 3. mppvideodec 静态能力 ----------
bar "3. gst-inspect mppvideodec src pad caps"
if command -v gst-inspect-1.0 >/dev/null 2>&1; then
  src_caps="$(gst-inspect-1.0 mppvideodec 2>/dev/null | sed -n '/^src/,/^sink/p')"
  echo "${src_caps}" | head -40 | sed 's/^/    /'
  if echo "${src_caps}" | grep -q "video/mpeg"; then
    ok "mppvideodec accepts MPEG-4 ASP (mp4v)";
  else
    warn "mppvideodec does NOT expose video/mpeg support — mpeg4 input WILL fail at link";
  fi
  if echo "${src_caps}" | grep -q "video/x-h264"; then
    ok "mppvideodec accepts H.264";
  else
    err "mppvideodec does NOT expose video/x-h264? — pipeline broken";
  fi
else
  err "gst-inspect-1.0 not found — install gstreamer1.0-tools";
fi

# ---------- 4. gst-launch 5 帧烟雾测试 ----------
bar "4. gst-launch smoke test (5 frames)"
smoke_one() {
  local f="$1"
  echo "    testing ${f}:"
  timeout 8 gst-launch-1.0 -v \
    filesrc location="${f}" \
    ! qtdemux ! h264parse \
    ! mppvideodec dma-feature=true format=NV12 \
    ! "video/x-raw(memory:DMABuf),format=NV12" \
    ! fakesink num-buffers=3 2>&1 \
    | grep -E "ERROR|WARN|mppvideodec|caps|negotiated|No valid|src pad" \
    | head -20 | sed 's/^/      /'
}
for f in "${files[@]}"; do
  smoke_one "${f}"
done

# ---------- 总结 ----------
bar "summary"
if [ "${probe_status}" -eq 0 ]; then
  ok "all input files are H.264 — pipeline SHOULD work"
  echo "    若实际仍出绿条纹, 跑 image-stitching 看 [gst_mpp_decoder][DIAG] 段:"
  echo "      - layout=1 (单 fd)   → NV12 单 DMA-BUF, OK"
  echo "      - layout>=2 (多 fd)  → 多 DMA-BUF (Y/UV 分离), 当前代码只取首个, UV 丢失"
else
  err "input is MPEG-4 ASP — pipeline WILL produce green stripes (codec mismatch)"
  echo "    修复: bash tools/batch_transcode_to_h264.sh 后 image-stitching 即可"
fi
exit 0