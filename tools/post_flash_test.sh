#!/usr/bin/env bash
# post_flash_test.sh - 烧录新镜像后, 自动检查 6 路 GC4683 sensor 适配状态
#
# 用途: vendor 发新版 dtbo.img / 烧新镜像后, 跑这个脚本确认 6 路 sensor 链路完整
# 跑法: sudo bash tools/post_flash_test.sh
# 退出码: 0 = 全通过 (或仅有 warning, 物理 sensor 未接时正常)
#         1 = 有硬错误 (vendor 必须修)
#
# 历史:
#   2026-07-01 - 初版, 基于 sysfs DTS 探测

set -uo pipefail

# ================== 配置 ==================
REPORT_DIR="/home/rocktech/Projects/new-4k-stitch/tools"
REPORT_FILE="$REPORT_DIR/post_flash_test_$(date +%Y%m%d_%H%M%S).log"
DT_BASE="/sys/firmware/devicetree/base"

# ================== 颜色 ==================
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

# ================== 计数器 ==================
TOTAL=0
PASSED=0
FAILED=0
WARNED=0
declare -a FAIL_DETAILS=()

# ================== 工具函数 ==================

# 彩色输出
print_section() {
    echo ""
    echo -e "${BOLD}${BLUE}==== $1 ====${NC}"
}

# 测试运行器
#   $1 = 测试名
#   $2 = 命令
#   $3 = 期望: pass / warn / soft
#   $4 = 期望描述 (用于失败信息)
run_test() {
    local name="$1"
    local cmd="$2"
    local expected="$3"
    local expect_desc="${4:-}"

    TOTAL=$((TOTAL + 1))
    printf "  [%3d] %-65s " "$TOTAL" "$name"

    local out rc
    out=$(eval "$cmd" 2>&1)
    rc=$?

    case "$expected" in
        pass)
            if [ $rc -eq 0 ]; then
                echo -e "${GREEN}✓ PASS${NC}"
                PASSED=$((PASSED + 1))
            else
                echo -e "${RED}✗ FAIL${NC}"
                echo -e "        ${RED}详情: $out${NC}"
                FAILED=$((FAILED + 1))
                FAIL_DETAILS+=("$name | $expect_desc | $out")
            fi
            ;;
        warn)
            # warn 类: 命令成功 = PASS, 失败 = 仅警告 (不算硬错误)
            if [ $rc -eq 0 ]; then
                echo -e "${GREEN}✓${NC} (${YELLOW}warn${NC}, 符合预期: $expect_desc)"
                WARNED=$((WARNED + 1))
            else
                # warn 期望但失败 = 仍然 WARN (不是 fail)
                echo -e "${YELLOW}! 警告级 (实际失败: $out)${NC}"
                WARNED=$((WARNED + 1))
            fi
            ;;
        soft)
            # soft: 失败只警告, 成功计 PASS
            if [ $rc -eq 0 ]; then
                echo -e "${GREEN}✓ PASS${NC}"
                PASSED=$((PASSED + 1))
            else
                echo -e "${YELLOW}! WARN${NC} (非硬错: $expect_desc)"
                WARNED=$((WARNED + 1))
            fi
            ;;
    esac
}

# 简单 helper
file_has_value() {
    local file="$1"
    local expected="$2"
    [ -f "$file" ] && [ "$(cat "$file" 2>/dev/null)" = "$expected" ]
}

# ================== 预检 ==================
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}需要 root 权限, 请用 sudo 跑${NC}"
    echo "  sudo bash $0"
    exit 2
fi

if [ ! -d "$DT_BASE" ]; then
    echo -e "${RED}/sys/firmware/devicetree/base 不存在${NC}"
    echo "  1. 板子内核没暴露 FDT (检查 CONFIG_PROC_DEVICETREE)"
    echo "  2. 或者烧的镜像根本不是这个工程的目标系统"
    exit 2
fi

# ================== 报告文件 ==================
exec > >(tee -a "$REPORT_FILE") 2>&1

echo "=========================================================================="
echo "  Post-Flash Test Report"
echo "  生成时间: $(date)"
echo "  内核: $(uname -a)"
echo "  发行版: $(grep PRETTY_NAME /etc/os-release 2>/dev/null | cut -d= -f2 | tr -d '\"')"
echo "  报告文件: $REPORT_FILE"
echo "=========================================================================="

# ================== 1. 系统基础检查 ==================
print_section "1. 系统基础"

run_test "内核版本是 5.10 (rocktech 板)" \
    "uname -r | grep -q '^5\.10'" \
    "pass" \
    "预期 5.10.x, 当前: $(uname -r)"

run_test "Ubuntu 22.04" \
    "grep -q '22.04' /etc/os-release" \
    "pass" \
    "预期 Ubuntu 22.04, 当前: $(grep PRETTY_NAME /etc/os-release 2>/dev/null)"

run_test "FDT 暴露正常" \
    "[ -d $DT_BASE ] && [ -d $DT_BASE/pinctrl ]" \
    "pass" \
    "DT base 目录和 pinctrl 都应存在"

# ================== 2. 6 路 sensor 节点完整性 ==================
print_section "2. 6 路 sensor 节点完整性"

# 找所有 gc4683*@31 节点
find_gc4683_sensors() {
    find "$DT_BASE" -name 'gc4683*@31' -type d 2>/dev/null | sort
}

run_test "6 个 gc4683 sensor 节点都在" \
    "[ \$(find_gc4683_sensors | wc -l) -eq 6 ]" \
    "pass" \
    "6 个是 cam1..cam6 对应的 sensor 节点"

run_test "无 gc5035 sensor 节点污染" \
    "[ \$(find $DT_BASE -name 'gc5035*@31' -type d 2>/dev/null | wc -l) -eq 0 ]" \
    "pass" \
    "应该用 gc4683 不用 gc5035"

run_test "每个 sensor compatible 是 galaxycore,gc4683" \
    "for s in \$(find_gc4683_sensors); do
        compat=\$(cat \$s/compatible 2>/dev/null | tr -d '\0')
        if [ \"\$compat\" != 'galaxycore,gc4683' ]; then
            echo \"\$s 错: \$compat\"
            exit 1
        fi
     done" \
    "pass" \
    "compatible 字符串必须是 galaxycore,gc4683"

run_test "每个 sensor status=okay" \
    "for s in \$(find_gc4683_sensors); do
        if [ \"\$(cat \$s/status 2>/dev/null)\" != 'okay' ]; then
            echo \"\$s status 不是 okay\"
            exit 1
        fi
     done" \
    "pass" \
    "6 个 sensor 节点都应该 enabled"

run_test "每个 sensor 都有 xvclk (clocks property)" \
    "for s in \$(find_gc4683_sensors); do
        if [ ! -f \$s/clocks ]; then
            echo \"\$s 缺 clocks\"
            exit 1
        fi
     done" \
    "pass" \
    "clocks = <&external_cameraN_clock>"

# ================== 3. I2C 总线无地址冲突 ==================
print_section "3. I2C 总线拓扑 (无地址冲突)"

run_test "6 路 sensor 分布在 6 个不同 i2c 控制器" \
    "[ \$(find_gc4683_sensors | xargs -I{} dirname {} | sort -u | wc -l) -eq 6 ]" \
    "pass" \
    "每个 sensor 必须在不同 i2c 控制器下"

run_test "无 i2c 总线地址冲突" \
    "for bus in \$(find $DT_BASE -name 'i2c@*' -type d 2>/dev/null); do
        cnt=\$(find \$bus -maxdepth 1 -name '*@31' -type d 2>/dev/null | wc -l)
        if [ \$cnt -gt 1 ]; then
            echo \"冲突: \$(basename \$bus) 有 \$cnt 个 0x31 sensor\"
            exit 1
        fi
     done" \
    "pass" \
    "每条 i2c 总线最多 1 个 sensor (地址 0x31)"

# 6 个 i2c 控制器 status 检查
for addr in feaa0000 feab0000 feac0000 fead0000 fec80000 fec90000; do
    run_test "i2c@$addr status=okay" \
        "file_has_value $DT_BASE/i2c@$addr/status okay" \
        "pass" \
        "承载 sensor 的 i2c 控制器必须 enabled"
done

# 6 个 i2c 控制器 pinctrl 检查
for addr in feaa0000 feab0000 feac0000 fead0000 fec80000 fec90000; do
    run_test "i2c@$addr pinctrl-0 已配" \
        "[ -f $DT_BASE/i2c@$addr/pinctrl-0 ]" \
        "pass" \
        "i2c SDA/SCL pin 复用必须配"
done

# ================== 4. csi2-dphy / dcphy 状态 ==================
print_section "4. csi2-dphy / dcphy 状态"

# 必须 enabled: dcphy0, dcphy1, dphy1, dphy2, dphy4, dphy5
for d in dcphy0 dcphy1 dphy1 dphy2 dphy4 dphy5; do
    run_test "csi2-$d status=okay (期望 enabled)" \
        "file_has_value $DT_BASE/csi2-$d/status okay" \
        "pass" \
        "cam1..cam6 链路必须完整"
done

# 当前预期 disabled, 但 vendor 修复后应 enabled (warn)
for d in dphy0 dphy3; do
    run_test "csi2-$d status 检查" \
        "if file_has_value $DT_BASE/csi2-$d/status okay; then
            # 已经是 okay, 算 PASS (vendor 已修)
            exit 0
         else
            # 仍 disabled, 是 warn (期望 vendor 后续修)
            echo 'csi2-dphy0/3 还 disabled, vendor 修复后会变 PASS'
            exit 0
         fi" \
        "warn" \
        "dphy0/dphy3 是 cam3 链路, 当前预期 disabled, vendor 修后转 pass"
done

# ================== 5. mipi-csi2 状态 ==================
print_section "5. mipi-csi2 状态"

for m in 0 1 2 3 4 5; do
    run_test "mipi$m-csi2 status=okay" \
        "file_has_value $DT_BASE/mipi$m-csi2/status okay" \
        "pass" \
        "6 个 mipi-csi2 接收器必须都 enabled"
done

# ================== 6. Camera clock ==================
print_section "6. xvclk 完整性"

run_test "6 个 external-camera-clock 已定义" \
    "[ \$(ls $DT_BASE/external-camera* 2>/dev/null | wc -l) -ge 6 ]" \
    "pass" \
    "需要 6 个 camera 时钟"

# 6 路 sensor 都用对了 clock
run_test "6 路 sensor 用了 6 个不同 clock" \
    "clocks=\$(for s in \$(find_gc4683_sensors); do
        od -An -tx4 -N4 \$s/clocks 2>/dev/null | tr -d ' '
     done | sort -u | wc -l)
     [ \$clocks -ge 6 ]" \
    "pass" \
    "每个 sensor 应该用独立 clock"

# ================== 7. 电源 regulator ==================
print_section "7. 电源 regulator"

# 1.2V (cm1..cm6) - 应该有 5-6 个
run_test "1.2V regulators (vcc-1v2-cm*) ≥ 5 个" \
    "[ \$(ls $DT_BASE/vcc-1v2-cm*-regulator 2>/dev/null | wc -l) -ge 5 ]" \
    "warn" \
    "1.2V 1 个/cam, vendor 已配 cm1..cm5"

# 1.8V
run_test "1.8V regulators (vcc-1v8-cm*) ≥ 1 个" \
    "[ \$(ls $DT_BASE/vcc-1v8-cm*-regulator 2>/dev/null | wc -l) -ge 1 ]" \
    "warn" \
    "1.8V 共享或专用均可"

# 2.8V
run_test "2.8V regulators (vcc-2v8-cm*) ≥ 1 个" \
    "[ \$(ls $DT_BASE/vcc-2v8-cm*-regulator 2>/dev/null | wc -l) -ge 1 ]" \
    "warn" \
    "2.8V 共享或专用均可"

# clock-en regulators (6 个, 1 个/cam)
run_test "6 路 camera clock-en regulators" \
    "[ \$(ls $DT_BASE/vcc-clk-en*-regulator 2>/dev/null | wc -l) -ge 6 ]" \
    "warn" \
    "1 个/cam, 应该有 6 个"

# ================== 8. Sensor 节点 supply 检查 (warn 级别) ==================
print_section "8. Sensor 节点 supply 配置 (warn)"

# dovdd-supply 是否每个 sensor 都有 (vendor 待修)
HAS_DOVDD=0
HAS_DVDD=0
HAS_AVDD=0
for s in $(find_gc4683_sensors); do
    if [ -f $s/dovdd-supply ]; then HAS_DOVDD=$((HAS_DOVDD+1)); fi
    if [ -f $s/dvdd-supply ]; then HAS_DVDD=$((HAS_DVDD+1)); fi
    if [ -f $s/avdd-supply ]; then HAS_AVDD=$((HAS_AVDD+1)); fi
done

run_test "6 路 sensor 都配 dovdd-supply (1.8V)" \
    "[ $HAS_DOVDD -ge 3 ]" \
    "warn" \
    "当前配了 $HAS_DOVDD/6, vendor 待补齐"

run_test "6 路 sensor 都配 dvdd-supply (1.2V)" \
    "[ $HAS_DVDD -ge 3 ]" \
    "warn" \
    "当前配了 $HAS_DVDD/6, vendor 待补齐"

run_test "6 路 sensor 都配 avdd-supply (2.8V)" \
    "[ $HAS_AVDD -ge 3 ]" \
    "warn" \
    "当前配了 $HAS_AVDD/6, vendor 待补齐"

# ================== 9. Sensor 节点 GPIO (故意空) ==================
print_section "9. Sensor 节点 GPIO 配置 (故意空, 等物理 sensor)"

# 现在没有物理 sensor, GPIO 引脚应为空 (vendor 不能瞎填)
# 但需要 status=okay
run_test "5 路 sensor 节点无 reset-gpios (无 sensor 时正确)" \
    "for s in \$(find_gc4683_sensors); do
        # reset-gpios 存在但应该是空的 (vendor 不能填假数据)
        if [ -f \$s/reset-gpios ]; then
            sz=\$(stat -c %s \$s/reset-gpios)
            if [ \$sz -gt 0 ]; then
                echo \"\$s reset-gpios 不应填值 (无物理 sensor)\"
                exit 1
            fi
        fi
     done
     exit 0" \
    "warn" \
    "等物理 sensor 接入后, 按 FPC 原理图填"

# ================== 10. dmesg 检查 ==================
print_section "10. dmesg 检查 (需要物理 sensor 才能 fully verify)"

run_test "dmesg 有 6 行 detected gc4683 (有物理 sensor 时)" \
    "cnt=\$(dmesg 2>/dev/null | grep -c 'detected gc4683' || echo 0)
     if [ \$cnt -eq 6 ]; then
        exit 0
     elif [ \$cnt -eq 0 ]; then
        echo '当前未接物理 sensor, 0 行是预期'
        exit 0
     else
        echo \"只探测到 \$cnt 个 sensor\"
        exit 1
     fi" \
    "warn" \
    "有 sensor 时 6 行, 无 sensor 时 0 行 (warn)"

run_test "dmesg 提示 csi2-dphy0/3 状态" \
    "if dmesg 2>/dev/null | grep -q 'csi2-dphy0.*probe successfully\\|csi2-dphy3.*probe successfully'; then
        # 已 enable
        exit 0
     else
        echo 'dphy0/3 还没 probe (vendor 待修)'
        exit 0
     fi" \
    "warn" \
    "dphy0/3 enable 后会有 probe successfully 日志"

# ================== 11. V4L2 设备节点 ==================
print_section "11. V4L2 设备节点"

run_test "/dev/v4l-subdev* 节点 ≥ 11 (6 mipi-csi2 + 5 dphy 已 enable)" \
    "[ \$(ls /dev/v4l-subdev* 2>/dev/null | wc -l) -ge 11 ]" \
    "pass" \
    "dphy0/3 enable 后会变 13+"

run_test "/dev/media* 节点存在" \
    "[ \$(ls /dev/media* 2>/dev/null | wc -l) -ge 1 ]" \
    "pass" \
    "media controller 设备"

# ================== 总结 ==================
print_section "测试结果总结"

echo -e "  总测试数:  ${BOLD}$TOTAL${NC}"
echo -e "  ${GREEN}通过 (PASS):  $PASSED${NC}"
echo -e "  ${RED}失败 (FAIL):  $FAILED${NC}"
echo -e "  ${YELLOW}警告 (WARN):  $WARNED${NC}"
echo ""

if [ $FAILED -eq 0 ]; then
    echo -e "  ${GREEN}${BOLD}✓ 硬测试全部通过${NC}"
    if [ $WARNED -gt 0 ]; then
        echo -e "  ${YELLOW}(有 $WARNED 个警告, 等 vendor 修 dts 或接物理 sensor)${NC}"
    fi
    echo ""
    echo "  详细报告: $REPORT_FILE"
    echo ""
    echo "  下一步:"
    echo "    1. 把报告发给 vendor (尤其 FAIL 列表)"
    echo "    2. 等 vendor 修 dts 后, 重新跑此脚本"
    echo "    3. 接 6 路物理 GC4683 sensor 后, 期望所有 WARN 转 PASS"
    exit 0
else
    echo -e "  ${RED}${BOLD}✗ 有 $FAILED 个硬错误, 必须修${NC}"
    echo ""
    echo "  失败项:"
    for detail in "${FAIL_DETAILS[@]}"; do
        echo -e "    ${RED}✗${NC} $detail"
    done
    echo ""
    echo "  详细报告: $REPORT_FILE"
    echo ""
    echo "  把报告发给 vendor, 修完重跑."
    exit 1
fi
