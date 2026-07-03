#!/bin/bash
# deploy/camerapage_test_backend.sh - 启动 CameraPage 测试后端
#
# 用法:
#   bash deploy/camerapage_test_backend.sh start [port]
#   bash deploy/camerapage_test_backend.sh stop
#   bash deploy/camerapage_test_backend.sh status
#   bash deploy/camerapage_test_backend.sh restart
#
# 启动两个进程:
#   1. image-stitching (C++): 每帧写 /tmp/stitch_status.json
#   2. camerapage_server.py (Python): 静态服务 CameraPage + JSON API
#
# 端口: 默认 8080 (与 CameraPage demo 一致)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
LOG_DIR="$PROJECT_ROOT/logs"
STATUS_JSON="/tmp/stitch_status.json"
PID_STITCH="$LOG_DIR/image-stitching.pid"
PID_PYTHON="$LOG_DIR/camerapage_server.pid"
LOG_STITCH="$LOG_DIR/image-stitching.log"
LOG_PYTHON="$LOG_DIR/camerapage_server.log"

PORT="${2:-8080}"

mkdir -p "$LOG_DIR"

ensure_built() {
    if [ ! -f "$PROJECT_ROOT/build/image-stitching" ]; then
        echo "[deploy] build/image-stitching not found, building..."
        mkdir -p "$PROJECT_ROOT/build"
        cd "$PROJECT_ROOT/build"
        cmake .. >/dev/null
        make -j$(nproc)
        cd "$PROJECT_ROOT"
    fi
}

ensure_camera_page() {
    if [ ! -f "$PROJECT_ROOT/CameraPage/index.html" ]; then
        echo "[deploy] ERROR: CameraPage/index.html not found at $PROJECT_ROOT/CameraPage/"
        echo "[deploy] Make sure the CameraPage folder is in the project root."
        exit 1
    fi
}

start_stitch() {
    ensure_built
    if [ -f "$PID_STITCH" ] && kill -0 $(cat "$PID_STITCH") 2>/dev/null; then
        echo "[deploy] image-stitching already running (PID $(cat "$PID_STITCH"))"
        return 0
    fi
    echo "[deploy] Starting image-stitching..."
    cd "$PROJECT_ROOT/build"
    ./image-stitching >"$LOG_STITCH" 2>&1 &
    echo $! >"$PID_STITCH"
    cd "$PROJECT_ROOT"
    sleep 2
    if kill -0 $(cat "$PID_STITCH") 2>/dev/null; then
        echo "[deploy] image-stitching started, PID $(cat "$PID_STITCH")"
        echo "[deploy]   log: $LOG_STITCH"
    else
        echo "[deploy] ERROR: image-stitching failed to start, check $LOG_STITCH"
        return 1
    fi
}

start_python() {
    ensure_camera_page
    if [ -f "$PID_PYTHON" ] && kill -0 $(cat "$PID_PYTHON") 2>/dev/null; then
        echo "[deploy] camerapage_server already running (PID $(cat "$PID_PYTHON"))"
        return 0
    fi
    echo "[deploy] Starting camerapage_server on port $PORT..."
    python3 "$PROJECT_ROOT/scripts/camerapage_server.py" "$PORT" >"$LOG_PYTHON" 2>&1 &
    echo $! >"$PID_PYTHON"
    sleep 2
    if kill -0 $(cat "$PID_PYTHON") 2>/dev/null; then
        echo "[deploy] camerapage_server started, PID $(cat "$PID_PYTHON")"
        echo "[deploy]   log: $LOG_PYTHON"
    else
        echo "[deploy] ERROR: camerapage_server failed to start, check $LOG_PYTHON"
        return 1
    fi
}

stop_one() {
    local pidfile="$1"
    local name="$2"
    if [ -f "$pidfile" ] && kill -0 $(cat "$pidfile") 2>/dev/null; then
        echo "[deploy] Stopping $name (PID $(cat "$pidfile"))..."
        kill $(cat "$pidfile")
        sleep 1
        kill -9 $(cat "$pidfile") 2>/dev/null || true
        rm -f "$pidfile"
    else
        echo "[deploy] $name not running"
        rm -f "$pidfile"
    fi
}

status() {
    echo "=== image-stitching ==="
    if [ -f "$PID_STITCH" ] && kill -0 $(cat "$PID_STITCH") 2>/dev/null; then
        echo "  running, PID $(cat "$PID_STITCH")"
        echo "  status file: $STATUS_JSON"
        if [ -f "$STATUS_JSON" ]; then
            echo "  last update: $(stat -c %y "$STATUS_JSON" 2>/dev/null)"
            echo "  content preview:"
            head -10 "$STATUS_JSON" | sed 's/^/    /'
        else
            echo "  status file NOT yet written"
        fi
    else
        echo "  NOT running"
    fi
    echo ""
    echo "=== camerapage_server ==="
    if [ -f "$PID_PYTHON" ] && kill -0 $(cat "$PID_PYTHON") 2>/dev/null; then
        echo "  running, PID $(cat "$PID_PYTHON") on port $PORT"
    else
        echo "  NOT running"
    fi
}

case "${1:-start}" in
    start)
        start_stitch
        start_python
        echo ""
        echo "=== Access URLs ==="
        LOCAL_IP=$(hostname -I | awk '{print $1}')
        echo "  Browser: http://${LOCAL_IP}:${PORT}/"
        echo "  API health: http://${LOCAL_IP}:${PORT}/api/health"
        echo "  Status JSON: http://${LOCAL_IP}:${PORT}/api/status"
        echo "  Devices: http://${LOCAL_IP}:${PORT}/api/devices"
        echo "  Network: http://${LOCAL_IP}:${PORT}/api/network"
        echo ""
        echo "Logs:"
        echo "  image-stitching: tail -f $LOG_STITCH"
        echo "  camerapage_server: tail -f $LOG_PYTHON"
        ;;
    stop)
        stop_one "$PID_PYTHON" "camerapage_server"
        stop_one "$PID_STITCH" "image-stitching"
        rm -f "$STATUS_JSON"
        ;;
    restart)
        "$0" stop
        sleep 1
        "$0" start "$PORT"
        ;;
    status)
        status
        ;;
    *)
        echo "Usage: $0 {start|stop|restart|status} [port]"
        exit 1
        ;;
esac