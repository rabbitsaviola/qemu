#!/bin/bash
#
# 真机固件验证：三个 EPIC 场景跑一遍，每个都"起 QEMU → 盯串口 marker →
# 冻住 vCPU → 读回真机 buffer → 按寄存器反算采样点 → 全分辨率逐像素比 →
# 1:1 负对照必须不匹配"。
#
# 和 notes/verify-sifli.sh、notes/qtest-sifli.sh 是同一套口径：全部通过退出
# 码 0，有失败为 1；缺 SDK / 固件 / ezip 工具就打 [SKIP] 跳过，不算失败。
#
# 为什么必须有这一项：qtest 只能按人写的值敲寄存器，验证不了"HAL 自己算出
# 来的寄存器对不对"。这三个场景跑的是真实板子固件，寄存器是唯一真相。
#
# 用法：
#   bash notes/realboard/run.sh
#
# 环境变量可覆盖：
#   SIFLI_QEMU_BUILD     构建目录（默认 ~/build-sifli）
#   SIFLI_SDK            SDK 根目录（默认 /mnt/e/code2/SiFli-SDK）
#   SIFLI_ARM_NM         arm-none-eabi-nm（默认 arm-none-eabi-nm）
#   SIFLI_REALBOARD_FW   固件 ELF，三个场景共用（默认 single_mode 那份）
#   SIFLI_EZIP_TOOL      宿主 ezip 解码器（默认 $SIFLI_SDK/tools/png2ezip/ezip_linux）
#   SIFLI_REALBOARD_WORK 工作目录（默认 $TMPDIR/sifli-realboard）
#   SIFLI_REALBOARD_TIMEOUT  单个场景的超时秒数（默认 900）

set -u

HERE=$(cd "$(dirname "$0")" && pwd)

BUILD=${SIFLI_QEMU_BUILD:-$HOME/build-sifli}
SDK=${SIFLI_SDK:-/mnt/e/code2/SiFli-SDK}
ARM_NM=${SIFLI_ARM_NM:-arm-none-eabi-nm}
EZIP_TOOL=${SIFLI_EZIP_TOOL:-$SDK/tools/png2ezip/ezip_linux}
FW=${SIFLI_REALBOARD_FW:-$SDK/example/rt_device/gpu/single_mode/project/build_sf32lb52-lcd_a128r16_hcpu/main.elf}
WORK=${SIFLI_REALBOARD_WORK:-${TMPDIR:-/tmp}/sifli-realboard}
TIMEOUT=${SIFLI_REALBOARD_TIMEOUT:-900}

QEMU=$BUILD/qemu-system-arm
FAILED=0
SUMMARY=""

# 环境变量要一路传到 python 那层；python 侧读的是同名的变量。
export SIFLI_QEMU_BUILD="$BUILD" SIFLI_SDK="$SDK" SIFLI_ARM_NM="$ARM_NM"
export SIFLI_EZIP_TOOL="$EZIP_TOOL" SIFLI_REALBOARD_FW="$FW"

mkdir -p "$WORK"

echo "sifli-qemu 真机固件验证"
echo "  构建:   $BUILD"
echo "  SDK:    $SDK"
echo "  固件:   $FW"
echo "  工作区: $WORK"
echo

# ------------------------------------------------------------ 前置条件
# QEMU 不在是构建问题，和 verify-sifli.sh 一样算失败；SDK 那边的资产缺了
# 是环境问题，逐场景 [SKIP]。
if [ ! -x "$QEMU" ]; then
    echo "  [FAIL] 找不到 $QEMU，先跑 notes/build-sifli.sh"
    echo
    echo "有 1 项失败。"
    exit 1
fi
echo "  [PASS] $QEMU"
echo

run_scenario() {
    # run_scenario <显示名> <tag> <抓取脚本> <比对脚本> [跳过原因]
    # tag 只用 ASCII：它要当串口/monitor 日志的文件名。
    local name=$1 tag=$2 cap=$3 ver=$4 reason=${5:-}
    echo "[$name]"

    if [ -n "$reason" ]; then
        echo "  [SKIP] $reason"
        SUMMARY="$SUMMARY
  $name SKIP"
        echo
        return
    fi

    local log=$WORK/$tag.log
    if ! timeout "$TIMEOUT" python3 "$HERE/$cap" "$WORK" "$tag" \
            >"$log" 2>&1; then
        echo "  [FAIL] 抓取失败或超时（$TIMEOUT 秒），日志 $log 末尾："
        tail -8 "$log" | sed 's/^/         /'
        SUMMARY="$SUMMARY
  $name FAIL"
        FAILED=$((FAILED + 1))
        echo
        return
    fi
    tail -3 "$log" | sed 's/^/         /'

    local out=$WORK/$tag.verify.log
    if python3 "$HERE/$ver" "$WORK" >"$out" 2>&1; then
        sed 's/^/  /' "$out"
        SUMMARY="$SUMMARY
  $name PASS"
    else
        sed 's/^/  /' "$out"
        echo "  [FAIL] 比对没过，日志 $out"
        SUMMARY="$SUMMARY
  $name FAIL"
        FAILED=$((FAILED + 1))
    fi
    echo
}

# 每个场景单独起一次机器：互不影响，一个失败不会把别的拖下水。
skip_reason=""
if [ ! -e "$FW" ]; then
    skip_reason="缺固件 $FW（设 SIFLI_REALBOARD_FW 或 SIFLI_SDK）"
elif ! command -v "$ARM_NM" >/dev/null 2>&1; then
    skip_reason="找不到 $ARM_NM（设 SIFLI_ARM_NM）"
fi

# co-engine 那条路还要宿主解码器：QEMU 自己解 EZIP 也得靠它。
scale_reason=$skip_reason
if [ -z "$scale_reason" ] && [ ! -x "$EZIP_TOOL" ]; then
    scale_reason="缺 ezip 工具 $EZIP_TOOL（设 SIFLI_EZIP_TOOL）"
fi

run_scenario 旋转      rot       cap_rot.py      verify_rot.py      "$skip_reason"
run_scenario coeng缩放 scale     cap_scale.py    verify_scale.py    "$scale_reason"
run_scenario 内存缩放  scalemem  cap_scalemem.py verify_scalemem.py "$skip_reason"

# ------------------------------------------------------------------- 结果
echo "===== 汇总 ====="
echo "$SUMMARY" | sed '/^$/d'
echo
if [ "$FAILED" -eq 0 ]; then
    echo "全部通过。"
    exit 0
fi
echo "有 $FAILED 项失败。"
exit 1
