#!/usr/bin/env bash
# sync_mjpeg_stage2.sh - 同步阶段 2 (MJPEG panorama 预览) 改动到板.
# 注: /home/rocktech/Projects/new-4k-stitch 在板上可能是 git checkout 或
#     同一仓库的 samba 挂载, 本脚本假设是 rsync 目录.
set -e

REMOTE="rk3588-6"
DST="/home/rocktech/Projects/new-4k-stitch"
LOCAL="$(cd "$(dirname "$0")" && pwd)"

echo "[sync] local = $LOCAL"
echo "[sync] remote = $REMOTE:$DST"

# 新文件 (scp - 上传到板)
echo "[sync] new files ..."
scp -q "$LOCAL/include/mjpeg_streamer.h"            "$REMOTE:$DST/include/"
scp -q "$LOCAL/src/mjpeg_streamer.cc"              "$REMOTE:$DST/src/"

# 修改文件 (scp 上传, 板上若在另一 shell 编辑会触发冲突, 用 --backup 留一份)
echo "[sync] modified files ..."
scp -q "$LOCAL/include/app.h"                      "$REMOTE:$DST/include/"
scp -q "$LOCAL/src/app.cc"                         "$REMOTE:$DST/src/"
scp -q "$LOCAL/src/http_server.cc"                 "$REMOTE:$DST/src/"
scp -q "$LOCAL/CMakeLists.txt"                     "$REMOTE:$DST/"

# CameraPage (前端)
echo "[sync] CameraPage ..."
scp -q "$LOCAL/CameraPage/index.html"              "$REMOTE:$DST/CameraPage/"
scp -q "$LOCAL/CameraPage/css/style.css"           "$REMOTE:$DST/CameraPage/css/"
scp -q "$LOCAL/CameraPage/js/app.js"               "$REMOTE:$DST/CameraPage/js/"

echo "[sync] verify on remote ..."
ssh -q "$REMOTE" "ls -la $DST/include/mjpeg_streamer.h $DST/src/mjpeg_streamer.cc $DST/include/app.h $DST/src/app.cc $DST/src/http_server.cc $DST/CMakeLists.txt $DST/CameraPage/index.html $DST/CameraPage/css/style.css $DST/CameraPage/js/app.js 2>&1"

echo "[sync] DONE"
