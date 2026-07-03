# 江苏攸洋智控管理平台 - B/S 架构连通性测试（Windows 主机执行）
# 模拟主机对虚拟机 Web 服务的 HTTP 调用

param(
    [string]$BaseUrl = "http://127.0.0.1:8080"
)

Write-Host "==========================================" -ForegroundColor Cyan
Write-Host " B/S 架构调用测试" -ForegroundColor Cyan
Write-Host " Client: Windows 主机" -ForegroundColor Cyan
Write-Host " Server: Ubuntu 虚拟机 (VMware NAT 转发)" -ForegroundColor Cyan
Write-Host "==========================================" -ForegroundColor Cyan
Write-Host ""

function Test-Endpoint {
    param([string]$Path, [string]$Desc)
    $url = "$BaseUrl$Path"
    try {
        $resp = Invoke-WebRequest -Uri $url -UseBasicParsing -TimeoutSec 5
        Write-Host "[OK]   $Desc" -ForegroundColor Green
        Write-Host "       $url  ->  HTTP $($resp.StatusCode)" -ForegroundColor DarkGray
        return $true
    } catch {
        Write-Host "[FAIL] $Desc" -ForegroundColor Red
        Write-Host "       $url  ->  $($_.Exception.Message)" -ForegroundColor DarkGray
        return $false
    }
}

$ok = $true
$ok = (Test-Endpoint "/" "首页 index.html") -and $ok
$ok = (Test-Endpoint "/css/style.css" "样式 style.css") -and $ok
$ok = (Test-Endpoint "/js/app.js" "脚本 app.js") -and $ok

Write-Host ""
if ($ok) {
    Write-Host "结论: 主机已成功调用虚拟机 Web 服务，B/S 链路正常。" -ForegroundColor Green
} else {
    Write-Host "结论: 调用失败。请检查虚拟机服务与 VMware NAT 端口转发。" -ForegroundColor Red
}

Write-Host ""
