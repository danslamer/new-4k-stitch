@echo off
chcp 65001 >nul
set PORT=8080

echo ==========================================
echo  江苏攸洋智控管理平台 - 主机客户端访问
echo  B/S 架构：Windows 主机 Browser ^<-^> Ubuntu VM Server
echo ==========================================
echo.
echo 桥接模式（推荐）：请在下方输入虚拟机 IP
echo 示例：http://192.168.110.124:8080/index.html
echo.
set /p VM_IP=请输入虚拟机 IP（hostname -I 第一个地址）:
if "%VM_IP%"=="" (
    echo [错误] 未输入 IP
    pause
    exit /b 1
)

set URL=http://%VM_IP%:%PORT%/index.html
echo.
echo 目标地址: %URL%
echo 正在检测服务是否可达...

powershell -NoProfile -Command "try { $r = Invoke-WebRequest -Uri '%URL%' -UseBasicParsing -TimeoutSec 5; if ($r.StatusCode -eq 200) { exit 0 } else { exit 1 } } catch { exit 1 }"

if %ERRORLEVEL% NEQ 0 (
    echo [失败] 无法连接虚拟机 Web 服务
    echo.
    echo 请确认：
    echo   1. Ubuntu 内已运行: bash deploy/start-server.sh
    echo   2. 虚拟机网络为桥接模式且 IP 正确
    echo   3. 服务监听 0.0.0.0:8080
    echo.
    pause
    exit /b 1
)

echo [成功] 服务可达，正在打开浏览器...
start "" "%URL%"
echo.
echo 演示说明：请在 Windows 主机浏览器中操作，勿在 Ubuntu 虚拟机内打开页面。
pause
