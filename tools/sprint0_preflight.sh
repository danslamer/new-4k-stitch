#!/usr/bin/env bash
#
# tools/sprint0_preflight.sh — 编译前依赖自检
#
# 板端 cmake 之前先跑, 30 秒内确认:
#   - 工具链齐全 (cmake / gcc / g++ / make / pkg-config)
#   - gstreamer 关键 plugin 在位 (rtspsrc / rtph264depay / mppvideodec / appsink / gst_dmabuf)
#   - 第三方库 pkg-config 在位 (librga / libdrm / OpenCL / cpp-httplib vendor)
#   - yaml 配置文件无 BOM 残留 (OpenCV 4.5.4 解析坑)
#   - 主板 ETH 接口 up, 默认网段路由可达摄像机子网
#
# 用法 (板端): bash tools/sprint0_preflight.sh
#

set -uo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-/home/rocktech/Projects/new-4k-stitch}"
LOG="${LOG:-/tmp/sprint0_preflight.log}"
cd "${PROJECT_ROOT}" || { echo "FATAL: cannot cd to ${PROJECT_ROOT}"; exit 2 }

PASS=0; FAIL=0; WARN=0
ok()   { printf "\033[32m[PASS]\033[0m %s\n" "$*"; PASS=$((PASS+1)); }
warn() { printf "\033[33m[WARN]\033[0m %s\n" "$*"; WARN=$((WARN+1)); }
fail() { printf "\033[31m[FAIL]\033[0m %s\n" "$*"; FAIL=$((FAIL+1)); }
bar()  { printf "\n\033[1;34m===== %s =====\033[0m\n" "$*"; }

# ---------- 1. 工具链 ----------
bar "1. 工具链"
for c in cmake g++ gcc make pkg-config; do
    if command -v "$c" >/dev/null 2>&1; then
        ver=$("$c" --version 2>/dev/null | head -1)
        ok "$c : $ver"
    else
        fail "$c NOT in PATH"
    fi
done

# 板端 gcc 应能编 C++11 + c++17
gxx=$(g++ -dumpversion 2>/dev/null || echo "?")
major=$(echo "$gxx" | cut -d. -f1)
if [ -n "$major" ] && [ "$major" -ge 8 ]; then
    ok "g++ $gxx ≥ 8 (满足 C++11 + 部分 C++17)"
else
    warn "g++ $gxx 较老 (CMakeLists 锁 CXX_STANDARD 11, 应可用)"
fi

# ---------- 2. gstreamer plugin ----------
bar "2. gstreamer 关键 plugin"
for p in rtspsrc rtph264depay rtph265depay h264parse h265parse mppvideodec appsink; do
    if gst-inspect-1.0 "$p" >/dev/null 2>&1; then
        # 提取 version info
        ver=$(gst-inspect-1.0 --version 2>/dev/null | head -1)
        ok "$p registered ($ver)"
    else
        case "$p" in
            mppvideodec)
                fail "mppvideodec 不在 gst registry → vendor gst-rockchip 1.14-4 没装"
                ;;
            rtspsrc|rtph264depay|h264parse|appsink)
                fail "$p 不在 gst registry (装 gstreamer1.0-plugins-good)"
                ;;
            *)
                warn "$p 不在 gst registry"
                ;;
        esac
    fi
done

# 检查 gst-dmabuf (4.20+ 内建) 或 gst-plugins-base
if gst-inspect-1.0 dma-pool 2>/dev/null | grep -q "yes"; then
    ok "gst_dmabuf helpers available"
else
    # gst_dmabuf_memory_get_fd 来自 gstreamer-allocators pkg
    if pkg-config --exists gstreamer-allocators-1.0 2>/dev/null; then
        ok "gstreamer-allocators-1.0 提供 gst_dmabuf_memory_get_fd"
    else
        fail "gstreamer-allocators-1.0 NOT found (供 gst_dmabuf_memory_get_fd)"
    fi
fi

# ---------- 3. 第三方库 pkg-config ----------
bar "3. 关键 pkg-config"
for p in librga libdrm egl glesv2 gbm opencl gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0 sdl2 opencv4 opencv; do
    if pkg-config --exists "$p" 2>/dev/null; then
        ver=$(pkg-config --modversion "$p" 2>/dev/null)
        ok "$p : $ver"
    else
        case "$p" in
            opencv4|opencv)
                fail "OpenCV pkg-config 找不到 (装 libopencv-dev)"
                ;;
            librga)
                fail "librga 找不到 (rockchip SDK / apt: librockchip-mpp-dev 或 librga-dev)"
                ;;
            libdrm|egl|glesv2|gbm)
                warn "$p 找不到 → IMAGE 阶段可能 cast 到 zero-copy 全失败, 但 cmake 还能编 (potentially 链接失败)"
                ;;
            *)
                warn "$p 找不到"
                ;;
        esac
    fi
done

# ---------- 4. yaml 文件 BOM 预检 (OpenCV 4.5.4 解析坑) ----------
bar "4. yaml 文件 (OpenCV 4.5.4 不接受 BOM)"
BOM=$'\xef\xbb\xbf'
for f in params/camera_sources.yaml params/roi_tuning.yaml; do
    if [ ! -f "$f" ]; then
        warn "missing $f"
        continue
    fi
    # Read first 3 bytes
    first3=$(head -c 3 "$f" | od -An -tx1 | tr -d ' \n')
    if [ "$first3" = "efbbbf" ]; then
        fail "$f 起始还有 UTF-8 BOM (用 strip BOM 后重跑: sed -i '1s/^\xef\xbb\xbf//' $f)"
    else
        ok "$f : no BOM, first bytes $(echo "$first3" | sed 's/\(..\)/\\x\1 /g')"
    fi
done

# 检查 yaml 能否被 python3 解析
if command -v python3 >/dev/null 2>&1; then
    for f in params/camera_sources.yaml params/roi_tuning.yaml; do
        if [ -f "$f" ]; then
            out=$(python3 -c "import yaml; d=yaml.safe_load(open('$f',encoding='utf-8-sig').read().split('---',1)[1] if '---' in open('$f',encoding='utf-8-sig').read() else open('$f',encoding='utf-8-sig').read()); print('OK %d cameras' % len(d.get('cameras',[])) if isinstance(d,dict) else 'OK')" 2>&1)
            if [[ "$out" == OK* ]]; then
                ok "$f 解析 OK ($out)"
            else
                warn "$f 解析警告: $out"
            fi
        fi
    done
fi

# ---------- 5. cpp-httplib 是不是 vendor 了 ----------
bar "5. cpp-httplib vendor 状态"
if [ -f third_party/cpp-httplib/httplib.h ]; then
    sz=$(stat -c%s third_party/cpp-httplib/httplib.h 2>/dev/null || stat -f%z third_party/cpp-httplib/httplib.h 2>/dev/null || echo 0)
    if [ "$sz" -gt 100000 ]; then
        ok "third_party/cpp-httplib/httplib.h : $sz bytes (vendor OK)"
    else
        warn "third_party/cpp-httplib/httplib.h : $sz bytes (可能未下完整, run sync script)"
    fi
else
    fail "third_party/cpp-httplib/httplib.h MISSING (HTTP server fallback: disabled)"
fi

# ---------- 6. ETH 接口 ----------
bar "6. ETH 接口"
if command -v ip >/dev/null 2>&1; then
    # 看 up 的 ETH 有没有
    ifaces=$(ip -br link show 2>/dev/null | awk '$2=="UP" {print $1}')
    eth_up=""
    for i in $ifaces; do
        # 取第一块 eth / end / enp 类的
        case "$i" in
            eth*|en*|end*)
                eth_up="$i"
                break
                ;;
        esac
    done
    if [ -n "$eth_up" ]; then
        ok "找到 UP ETH: $eth_up"
        # 检查 IP 地址
        ip_addr=$(ip -4 addr show "$eth_up" 2>/dev/null | awk '/inet / {print $2}' | head -1)
        [ -n "$ip_addr" ] && ok "  $eth_up : $ip_addr" || warn "  $eth_up 没有 IPv4"
    else
        warn "没找到 UP 的 eth*/en* 接口 (可能用 wlan, manual check)"
        # 看看任意 up 接口
        for i in $ifaces; do
            ip_addr=$(ip -4 addr show "$i" 2>/dev/null | awk '/inet / {print $2}' | head -1)
            [ -n "$ip_addr" ] && echo "    $i : $ip_addr"
        done
    fi
else
    warn "ip 命令找不到"
fi

# ---------- 7. 默认路由可达测试 ----------
bar "7. 默认路由 (摄像机子网 192.168.10.x)"
if command -v ping >/dev/null 2>&1; then
    # 取 1 个 yaml 里的 IP 测
    cam_ip=""
    if [ -f params/camera_sources.yaml ]; then
        cam_ip=$(grep -oE "rtsp://[^@]+@192\.168\.[0-9]+\.[0-9]+" params/camera_sources.yaml 2>/dev/null | head -1 | grep -oE "192\.168\.[0-9]+\.[0-9]+")
    fi
    if [ -n "$cam_ip" ]; then
        # 测 2 跳 (1 cam 占位 + 一个 zero-pad 替代)
        # 用 ping -c 1 -W 1 因为 ICMP 经常被墙
        if ping -c 1 -W 2 "$cam_ip" >/dev/null 2>&1; then
            ok "ping $cam_ip 通过"
        else
            warn "ping $cam_ip 失败 (ICMP 可能被防火墙阻, 但 TCP/554 可能通; 用 tools/rtsp_url_probe.sh 测)"
        fi
    fi
fi

# ---------- 总结 ----------
bar "SUMMARY"
echo "  PASS=$PASS  WARN=$WARN  FAIL=$FAIL"
[ $FAIL -eq 0 ] && {
    printf "\n  \033[32m✅ preflight PASSED (警告可忽略)\033[0m — 可以跑 cmake\n\n"
    exit 0
} || {
    printf "\n  \033[31m❌ preflight 有 $FAIL 处 FAIL\033[0m — 装齐再 cmake\n\n"
    exit 1
}