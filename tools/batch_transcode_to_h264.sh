#!/usr/bin/env bash
#
# tools/batch_transcode_to_h264.sh — 板端: 一次性把 datasets/2k-test/*.mp4 (MPEG-4 ASP / mp4v)
# 批量转码为 datasets/2k-test-h264/*.mp4 (H.264 + faststart).
#
# 触发原因 (2026-07-08 实测确认):
#   - gst_mpp_decoder.cc pipeline 走 qtdemux + h264parse + mppvideodec, 强制 H.264 入口
#   - 实际 datasets/2k-test/*.mp4 是 MPEG-4 Visual (mp4v)
#   - qtdemux 输出 video/mpeg 与 h264parse 的 video/x-h264 caps 不匹配, link 失败,
#     部分数据被 gstreamer 用 identity bypass, mppvideodec 拿到错位 bitstream → 绿条纹.
#
# 修复路径: ffmpeg + 板端可用的 H.264 编码器. 优先级:
#   1. h264_rkmpp       (Rockchip vendor, 板上 ffmpeg --enable-rkmpp 已带, 0.8s/2K 帧, 板上最快)
#   2. h264_v4l2m2m     (V4L2 通用, 板未带 v4l2-h264 时 fail)
#   3. c:v libx264       (要求 ffmpeg 含 libx264; 板上 SDK 默认不带, 可 apt-get install x264 后改用 x264 CLI)
#   **绝不** 用 mpeg4 sw enc 重 encode (要保留原码体系, 不是再换码; 也不解决 codec 不匹配).
#
# v2.5 (2026-07-08): 实测 h264_rkmpp 在板端可用 (vendor SDK 自带 ffmpeg 已开启 rkmpp). 改用它.
#   之前 AGENTS.md 提到的 "h264_rkmpp segfault" 是 gstreamer-rkmpp 链路问题, 与 ffmpeg 链接 rkmpp 无关.
#
# 用法:
#   bash tools/batch_transcode_to_h264.sh                       # 默认 src/dst
#   SRC=./data DST=./out bash tools/batch_transcode_to_h264.sh  # 自定义
#   FORCE=1 bash tools/batch_transcode_to_h264.sh               # 强制覆盖
#   ONLY=t50,t51 bash tools/batch_transcode_to_h264.sh          # 只转指定 (空格或逗号分隔)
#

set -euo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-/home/rocktech/Projects/new-4k-stitch}"
SRC="${SRC:-${PROJECT_ROOT}/datasets/2k-test}"
DST="${DST:-${PROJECT_ROOT}/datasets/2k-test-h264}"
ONLY="${ONLY:-t50 t51 t52 t53 t40 t41}"
FORCE="${FORCE:-0}"
# 编码器设置 (允许外部覆盖): "h264_rkmpp" (默认) / "h264_v4l2m2m" / "libx264"
ENC="${ENC:-h264_rkmpp}"
# x264 CLI 路径 (仅当 ENC=x264_cli 时生效)
X264_CLI="${X264_CLI:-/usr/bin/x264}"

log() { printf "[batch_transcode] %s\n" "$*"; }
err() { printf "[batch_transcode][ERROR] %s\n" "$*" >&2; }

log "PROJECT_ROOT=${PROJECT_ROOT}"
log "SRC=${SRC}"
log "DST=${DST}"
log "files (raw): ${ONLY}"
log "ENC=${ENC}"
log "FORCE=${FORCE} (set FORCE=1 to overwrite existing H.264 outputs)"

# 把 ONLY 兼容逗号 / 空格.
ONLY_WORDS="$(echo "${ONLY}" | tr ', ' '  ')"

# 前置检查.
command -v ffmpeg >/dev/null 2>&1 || { err "ffmpeg not found"; exit 1; }
command -v ffprobe >/dev/null 2>&1 || { err "ffprobe not found"; exit 1; }
[ -d "${SRC}" ] || { err "SRC not found: ${SRC}"; exit 1; }
mkdir -p "${DST}"

# 检查编码器是否在板端 ffmpeg 内.
ENC_LIST="$(ffmpeg -hide_banner -encoders 2>/dev/null | awk 'NR>9 {print $2}' | sort -u)"
if ! printf '%s\n' "${ENC_LIST}" | grep -qx "${ENC}"; then
  err "requested encoder '${ENC}' not available on this ffmpeg build"
  err "available encoders (subset): $(printf '%s\n' "${ENC_LIST}" | grep -E '^(h264|libx|x264|h265|hevc|av1|mpeg4)' || echo '<none matching>')"
  exit 1
fi

probe_codec() {
  ffprobe -v error -select_streams v:0 -show_entries stream=codec_name \
    -of default=nw=1:nk=1 "$1" 2>/dev/null || true
}

# 把编 h264 字符串封装成可调用函数, 让下面循环统一调.
encode_h264() {
  local src="$1" dst="$2"
  case "${ENC}" in
    h264_rkmpp)
      # h264_rkmpp 不支持 -preset, 用 -rc_mode VBR + -b:v 控制大小, 也可用 CQP.
      # 板上实测 0.8s/2K 文件, 输出 moov 自动 faststart 是 ffmpeg 容器层决定的.
      ffmpeg -hide_banner -loglevel error -y \
        -i "${src}" \
        -c:v "${ENC}" -rc_mode VBR -b:v 8M -maxrate 12M \
        -pix_fmt yuv420p -an -sn \
        -movflags +faststart \
        "${dst}"
      ;;
    h264_v4l2m2m)
      ffmpeg -hide_banner -loglevel error -y \
        -i "${src}" \
        -c:v "${ENC}" \
        -pix_fmt yuv420p -an -sn \
        -movflags +faststart \
        "${dst}"
      ;;
    libx264)
      ffmpeg -hide_banner -loglevel error -y \
        -i "${src}" \
        -c:v libx264 -preset ultrafast -crf 23 \
        -pix_fmt yuv420p -an -sn \
        -movflags +faststart \
        "${dst}"
      ;;
    x264_cli)
      # 较复杂: x264 不直接接 mp4 input. 走 ffmpeg 解 -> raw -> x264 -> raw h264 -> mp4box/ffmpeg muxer.
      # 暂不实现, 仅 ENC=x264_cli 时提示. 板上已优先选 rkmpp, 一般走不到这.
      err "ENC=x264_cli pipeline not implemented in this version, use ENC=h264_rkmpp"
      exit 1
      ;;
    *)
      err "unknown ENC='${ENC}'"; exit 1
      ;;
  esac
}

run=0; skip=0; fail=0
for base in ${ONLY_WORDS}; do
  src_file="${SRC}/${base}.mp4"
  dst_file="${DST}/${base}.mp4"

  if [ ! -f "${src_file}" ]; then
    err "missing source: ${src_file}"; fail=$((fail + 1)); continue
  fi
  if [ -f "${dst_file}" ] && [ "${FORCE}" != "1" ]; then
    log "skip (exists, set FORCE=1 to overwrite): ${base}.mp4"
    skip=$((skip + 1)); continue
  fi

  codec="$(probe_codec "${src_file}")"
  src_sz=$(stat -c%s "${src_file}" 2>/dev/null || stat -f%z "${src_file}")
  log "transcoding ${base}.mp4  codec=${codec}  src=${src_sz} bytes"

  if ! encode_h264 "${src_file}" "${dst_file}"; then
    err "encode_h264 failed on ${base}"; fail=$((fail + 1)); continue
  fi

  # 验证产物: 必须是 H.264.
  if ! ffprobe -v error -select_streams v:0 -show_entries stream=codec_name \
      -of default=nw=1:nk=1 "${dst_file}" | grep -q "h264"; then
    err "post-check: ${dst_file} is NOT H.264"; fail=$((fail + 1)); continue
  fi

  dst_sz=$(stat -c%s "${dst_file}" 2>/dev/null || stat -f%z "${dst_file}")
  log "OK  ${base}.mp4  -> ${dst_sz} bytes"
  run=$((run + 1))
done

log "summary: transcode=${run} skip=${skip} fail=${fail}"
[ "${fail}" -eq 0 ] || exit 1
exit 0