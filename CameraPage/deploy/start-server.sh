#!/bin/bash
# 江苏攸洋智控管理平台 - Ubuntu 服务端启动脚本
# 用途：在虚拟机（模拟嵌入式主板）上启动 Web 服务，供 Windows 主机浏览器访问

PORT=8080
DIR="$(cd "$(dirname "$0")/.." && pwd)"

cd "$DIR" || exit 1

VM_IP=$(hostname -I | awk '{print $1}')

echo "=========================================="
echo " 江苏攸洋智控管理平台 - Web 服务"
echo " 服务目录: $DIR"
echo " 监听端口: $PORT (0.0.0.0)"
echo " 虚拟机 IP: ${VM_IP:-未知}"
echo "=========================================="
echo ""
echo "【推荐】桥接模式 - Windows 主机浏览器访问："
echo "  http://${VM_IP:-<虚拟机IP>}:$PORT/index.html"
echo ""
echo "NAT 模式（需配置端口转发）主机访问："
echo "  http://127.0.0.1:$PORT/index.html"
echo ""
echo "虚拟机内自检："
echo "  http://127.0.0.1:$PORT"
echo ""
echo "按 Ctrl+C 停止服务"
echo "=========================================="

exec python3 -m http.server "$PORT" --bind 0.0.0.0
