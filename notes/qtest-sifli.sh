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
# DMA 那一段除了寄存器回读，还跑一次真的 memory-to-memory 搬运：DMA 的
# 请求线是外设驱动的，qtest 里没有外设，只有 MEM2MEM 这种"置 EN 就跑"的
# 通道能在没有请求的情况下证明 address_space 那两下真的搬了字节。
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
    'readl 0x500c0028' \
    \
    'writel 0x500810ac 0x00050000' \
    'readl 0x500810ac' \
    'writel 0x500810a8 0x04000000' \
    'readl 0x500810a8' \
    \
    'writel 0x5008100c 0x00000040' \
    'readl 0x5008100c' \
    'writel 0x50081010 0x50084024' \
    'readl 0x50081010' \
    'writel 0x50081014 0x20000100' \
    'readl 0x50081014' \
    \
    'writel 0x20000000 0x04030201' \
    'writel 0x50081010 0x20000000' \
    'writel 0x50081014 0x20000010' \
    'writel 0x5008100c 0x00000004' \
    'writel 0x50081004 0xffffffff' \
    'writel 0x50081008 0x000040c1' \
    'readl 0x20000010' \
    'readl 0x5008100c' \
    'readl 0x50081010' \
    'readl 0x50081014' \
    'readl 0x50081000' \
    'writel 0x50081004 0xffffffff' \
    'readl 0x50081000')

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
echo "[4] DMA：CSELR 请求号字段布局"
# C 字段 6 bit，通道 n 在 (n-1)%4 那个字节里（dmac.h DMAC_CSELR1_C1S_Pos）。
# HAL_DMA_Init 就是按这个布局写的（bf0_hal_dma.c），写错了请求号就选不中
# 通道，串口收字节会静默地没人搬。
check 0x50000    "CSELR2 通道 7 写 5（bit23:16）"
check 0x4000000  "CSELR1 通道 4 写 4（bit31:24）"

echo
echo "[5] DMA：通道寄存器"
check 0x40       "CNDTR1 写 64"
check 0x50084024 "CPAR1 写 USART1.RDR"
check 0x20000100 "CM0AR1 写内存地址"

echo
echo "[6] DMA：一次真的搬运（MEM2MEM，置 EN 就跑）"
# 源在 0x20000000，目的在 0x20000010，4 个字节，MINC|PINC。
check 0x04030201 "目的内存拿到源内存的内容"
check 0x0        "CNDTR1 减到 0"
check 0x20000004 "CPAR1 按 PINC 前进 4（PSIZE=byte）"
check 0x20000014 "CM0AR1 按 MINC 前进 4"
# HTIF 在还剩 NDT/2 时置（SVD：half NDT are transferred）——4 字节的第 2 个
# 字节搬完就置上了，所以是 7 不是 3。
check 0x7        "ISR 置 GIF1|TCIF1|HTIF1"
# CGIF 是整组的清位（SVD：各标志"write 1 to CTCIF or CGIF"清除）。
check 0x0        "IFCR 写 CGIF 后 ISR 清零"

echo
if [ "$FAILED" -eq 0 ]; then
    echo "全部通过。"
    exit 0
fi
echo "有 $FAILED 项失败。"
exit 1
