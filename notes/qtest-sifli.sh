#!/bin/bash
#
# 用 qtest 验证外设模型的**写路径**。verify-sifli.sh 的第 7 项只能读，
# 这里补上写。
#
# 为什么用 qtest 而不是 -device loader：loader 的写在设备 reset **之前**，
# 寄存器会被复位冲掉。qtest 的 writel/readl 走 address_space_write/read，
# 和外设 MMIO 是同一条路，而且在机器建好、复位完成之后。
#
# 重点验 RCC 的使能别名 —— 这是 regbank 里最绕的一处：
#   HAL 写 ESR 置位、写 ECR 清位，但读的是 ENR
#   （bf0_hal_rcc.c:1959 / :1998 / :2037）
# 纯表表达不了这种跨寄存器关系，靠 write_hook 实现。
#
# 用法：bash notes/qtest-sifli.sh
#
# 环境变量可覆盖：
#   SIFLI_QEMU_BUILD  构建目录（默认 ~/build-sifli）

set -u

BUILD=${SIFLI_QEMU_BUILD:-$HOME/build-sifli}
QEMU=$BUILD/qemu-system-arm
FAILED=0

[ -x "$QEMU" ] || { echo "找不到 $QEMU，先跑 notes/build-sifli.sh"; exit 1; }

run() {
    # 不合并 stderr：那里的 [R ...]/[S ...] 是 qtest 的 trace，会打乱取值顺序
    printf '%s\n' "$@" quit \
        | timeout 30 "$QEMU" -M sf32lb52x -display none \
              -serial none -qtest stdio 2>/dev/null | tr -d '\r'
}

out=$(run \
    'writel 0x50000010 0x10' \
    'readl 0x50000008' \
    'writel 0x50000018 0x10' \
    'readl 0x50000008' \
    'writel 0x50000014 0x3c' \
    'readl 0x5000000c' \
    'writel 0x5000001c 0x04' \
    'readl 0x5000000c' \
    'writel 0x50000020 0x3' \
    'readl 0x50000020' \
    'writel 0x50000034 0x40000026' \
    'readl 0x50000034' \
    'writel 0x500c0024 0xffffffff' \
    'readl 0x500c0024' \
    'writel 0x500c0028 0x0000000f' \
    'readl 0x500c0024' \
    'readl 0x500c0028')

if [ -z "$out" ]; then
    echo "拿不到 qtest 输出"
    exit 1
fi

# 只取读命令的返回（写命令只回一个 "OK"）。qtest 回的是 64 位十六进制。
vals=$(echo "$out" | grep '^OK 0x')
i=0
check() {
    i=$((i + 1))
    local want=$1 what=$2 got
    got=$(echo "$vals" | sed -n "${i}p" | sed 's/^OK //')
    if [ -n "$got" ] && [ $((got)) -eq $((want)) ]; then
        printf '  [PASS] %-40s = %s\n' "$what" "$got"
    else
        printf '  [FAIL] %-40s 期望 %s，实际 %s\n' "$what" "$want" "${got:-<无>}"
        FAILED=$((FAILED + 1))
    fi
}

echo "qtest 写路径验证（$QEMU）"
echo
echo "[1] RCC 使能别名：ESR 置位 / ECR 清位，读 ENR"
check 0x10 "ESR1 写 bit4 后读 ENR1"
check 0x0  "ECR1 写 bit4 后读 ENR1"
check 0x3c "ESR2 写 0x3c 后读 ENR2"
check 0x38 "ECR2 写 bit2 后读 ENR2"

echo
echo "[2] 普通寄存器"
check 0x3        "CSR 整字读写"
check 0xc0000026 "HRCCAL1 存写入值，bit31 强制置位"

echo
echo "[3] 写 A 清 B：HPSYS_AON.WCR 清 WSR"
# WSR 是普通存储，所以可以先把它写满，再用 WCR 清掉一部分 —— 这样
# 清的是"另一个寄存器"这件事就可见了。HAL_PMU_CheckBootMode() 正是
# 读 WSR 再写回 WCR（bf0_hal_pmu.c:364-370）。
check 0xffffffff "先直接写 WSR（它是普通存储）"
check 0xfffffff0 "WCR 写 0xf 后，WSR 低 4 位被清"
check 0x0        "WCR 自身只写，读回 0"

echo
if [ "$FAILED" -eq 0 ]; then
    echo "全部通过。"
    exit 0
fi
echo "有 $FAILED 项失败。"
exit 1
