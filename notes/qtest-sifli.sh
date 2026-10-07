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
# [13]–[16] 是时基那几件（SysTick / tick clock / GTIMR / DWT），要推时钟，
# 所以单独走 run_clock()（多一个 -accel qtest），见那里的注释。
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

# 面板是 -device 加进来的，所以要单独起一次机器。设备在 machine init 之后
# 才建，面板 realize 时把自己反向注册给 LCDC 的 QSPI 总线。
run_panel() {
    printf '%s\n' "$@" quit \
        | timeout 30 "$QEMU" -M sf32lb52x -display none \
              -serial none -device sifli-panel -qtest stdio 2>/dev/null | tr -d '\r'
}

# 要用 clock_step 推时基的那几项走这一条，必须显式 -accel qtest。
# 这台机器没有 -kernel，CPU 从全零 ROM 起跑，resume 后几毫秒就撞进 HardFault
# 把 QEMU 打死；monitor 那几条命令什么时候被处理又取决于主机调度，读第二次
# 就没了。`-accel qtest` 不跑 CPU（accel/qtest/qtest.c 的 create_vcpu_thread
# 是空实现，虚拟时钟就是它自己那个计数器），clock_step 只推计数器，读多少
# 遍都准。
#
# 收尾靠 timeout：qtest 没有 quit 命令，stdin 关了它也不退出，所以每一轮
# 都要等满这个超时。会话别起太多。
run_clock() {
    printf '%s\n' "$@" quit \
        | timeout 30 "$QEMU" -M sf32lb52x -display none \
              -serial none -accel qtest -qtest stdio 2>/dev/null | tr -d '\r'
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
    'readl 0x50081000' \
    \
    'write 0x20001000 0x10 0x13543276094d2bef98b96af719000000' \
    'writel 0x50006018 0x00030000' \
    'writel 0x50006008 0x20002000' \
    'writel 0x50006004 0x20001000' \
    'writel 0x5000600c 0x00000003' \
    'writel 0x50006000 0x00000001' \
    'readl 0x20002000' \
    'readl 0x20002004' \
    'readl 0x20002008' \
    'readl 0x5000606c' \
    'readl 0x50006070' \
    'readl 0x50006028' \
    \
    'write 0x20001100 0xf 0x0b0000008001020304050607080800' \
    'writel 0x50006018 0x00000000' \
    'writel 0x50006008 0x20002040' \
    'writel 0x50006004 0x20001100' \
    'writel 0x5000600c 0x00000005' \
    'writel 0x50006000 0x00000001' \
    'readl 0x20002040' \
    'readl 0x20002044' \
    'readl 0x20002048' \
    'readl 0x50006070' \
    \
    'writel 0x50006024 0x00000001' \
    'readl 0x50006024' \
    'readl 0x50006028' \
    'writel 0x50006028 0x00000001' \
    'readl 0x50006028' \
    \
    'write 0x20001180 0x8 0x0400000010aabb00' \
    'writel 0x20002080 0xdeadbeef' \
    'writel 0x50006024 0xffffffff' \
    'writel 0x50006028 0xffffffff' \
    'writel 0x50006008 0x20002080' \
    'writel 0x50006004 0x20001180' \
    'writel 0x5000600c 0x00000005' \
    'writel 0x50006000 0x00000001' \
    'readl 0x50006028' \
    'readl 0x20002080' \
    \
    'writel 0x50007010 0x00000002' \
    'writel 0x50007014 0x00000004' \
    'writel 0x50007018 0x00112233' \
    'writel 0x500070f8 0x00000002' \
    'writel 0x500070fc 0x20003000' \
    'writel 0x50007100 0x00000000' \
    'writel 0x50007000 0x00000001' \
    'readl 0x20003000' \
    'readl 0x20003004' \
    'readl 0x20003008' \
    'readl 0x50007004' \
    'readl 0x50007008' \
    'readl 0x50007130' \
    \
    'write 0x20003100 0x4 0xffff0000' \
    'write 0x20003200 0x4 0x00f800f8' \
    'writel 0x50007018 0x02000000' \
    'writel 0x50007010 0x00000000' \
    'writel 0x50007014 0x00000001' \
    'writel 0x500070fc 0x20003100' \
    'writel 0x500070f8 0x00000000' \
    'writel 0x50007054 0x00000000' \
    'writel 0x50007058 0x00000001' \
    'writel 0x50007060 0x20003200' \
    'writel 0x50007050 0x40041010' \
    'writel 0x50007000 0x00000001' \
    'readl 0x20003100')

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

# check 的容差版，给定时器用：读 CVR 的时刻和拍边界不一定对齐，窗口里少
# 一拍或多一拍都正常（窗口越短越明显）。want 是期望拍数，tol 是允许的偏差。
check_near() {
    i=$((i + 1))
    local want=$1 tol=$2 what=$3 got
    got=$(echo "$vals" | sed -n "${i}p" | sed 's/^OK //')
    if [ -n "$got" ] && [ $((got)) -ge $((want - tol)) ] \
                   && [ $((got)) -le $((want + tol)) ]; then
        printf '  [PASS] %-40s = %s\n' "$what" "$got"
    else
        printf '  [FAIL] %-40s 期望 %s±%s，实际 %s\n' \
            "$what" "$want" "$tol" "${got:-<无>}"
        FAILED=$((FAILED + 1))
    fi
}

# 取这一轮里第 n 个读命令的返回值（十进制）。读不到当 0，由 check 去报。
rd() {
    local x
    x=$(echo "$vals" | sed -n "${1}p" | sed 's/^OK //')
    echo $(( ${x:-0} ))
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
echo "[7] EZIP：解压到 AHB"
# 12 个字节的明文 11 22 .. cc，raw deflate 之后 14 字节。走的是真实的解码路径：
# PARA 选 MOD_GZIP + 输出到 AHB，写 CTRL.START 就出结果。
#
# 注意喂进去的是**裸 deflate，没有 gzip 头也没有尾**。SDK 的资产就是这么
# 造的（`ezip -gzip <file> -length -noheader`，见 peripherals.md §6.2），
# 硬件也只认这个：给它一个完整的 gzip 流，它会把头当成压缩数据，直接解错。
check 0x44332211 "GZIP 前 4 字节"
check 0x88776655 "GZIP 次 4 字节"
check 0xccbbaa99 "GZIP 末 4 字节"
# DB_DATA1 是几何：START/END_POINT 给的是 col 0..3、row 0，所以 4x1。
# 位序是 row 在低半、col 在高半，写反了这里会是 2x3。
check 0x40001    "DB_DATA1 = 宽 4 高 1"
# DB_DATA2 是这次真正吃掉的输入字节数（deflate 流自己说自己到哪儿结束）。
check 0xe        "DB_DATA2 = 14（裸 deflate 流的长度）"
# HAL_EZIP_Decode 轮询的是 INT_MASK，不是 INT_STA，而且它从没碰过 INT_EN。
check 0x1        "INT_MASK 置 END"

echo
echo "[7b] EZIP：LZ4"
# 手工构造的裸 LZ4 块（前面 4 字节是块长，和 SDK 自己的 .dat 资产一致）：
#   80           token：8 个字面量，匹配长度码 0
#   01..08       8 个字面量
#   08 00        偏移 8
# 匹配长度码 0 即 4 字节，于是把前 8 个字节的头 4 个再吐一遍，共 12 字节。
check 0x04030201 "LZ4 前 4 字节（字面量）"
check 0x08070605 "LZ4 次 4 字节（字面量）"
check 0x04030201 "LZ4 末 4 字节（按偏移 8 回拷）"
check 0xf        "DB_DATA2 = 15（4 字节块长 + 11 字节块）"

echo
echo "[7c] EZIP：两个中断寄存器各自 W1C"
check 0x0        "INT_STA 写 1 清位"
# INT_MASK 是独立的一份，清 INT_STA 不该把它也清了 —— 轮询路径读的是它。
check 0x1        "清 INT_STA 之后 INT_MASK 还在"
check 0x0        "INT_MASK 自己写 1 也清得掉"

echo
echo "[7d] EZIP：坏块只报错，不动目标"
# 偏移 0xbb 指向还没解出来的地方，解码器必须拒绝而不是把内存读穿。
check 0x8        "INT_MASK 置 BTYPE_ERR"
check 0xdeadbeef "目标内存原样没动"

echo
echo "[8] EPIC：不透明填充"
# HAL_EPIC_Fill 的 alpha==0xFF 那一路根本不配图层，只写 CANVAS_BG 和矩形，
# 所以这一段同时也在证明"没有图层也要画"。RGB888 输出，每像素 B,G,R。
check 0x33112233 "填充像素 0（B=33 G=22 R=11）"
check 0x22331122 "填充像素 1"
check 0x11       "填充像素 2 的首字节"
check 0x0        "STATUS 读回 0（作业在 START 那次写里做完）"
check 0x10000    "EOF_IRQ 置 STATUS 位"
# EPIC_WaitDone 在 52x 上会读它两次再写 0，所以它必须可读可写。
check 0x0        "PERF_CNT 可读"

echo
echo "[8b] EPIC：图层混合"
# 目标两个 RGB565 像素：白 0xFFFF、黑 0x0000，由 ALL_BLENDING_BYPASS 保住
# 不被 CANVAS_BG 冲掉；L0 两个纯红 0xF800，ALPHA_SEL + ALPHA=128 常量混合。
#   alpha = 128，out = (src*128 + dst*127 + 127) / 255
#   白底：R=(255*128+255*127+127)/255=255  G=B=(0+255*127+127)/255=127
#         -> 565 里 R=31 G=31 B=15 -> 0xFBEF
#   黑底：R=(255*128+0+127)/255=128        G=B=0
#         -> 565 里 R=16 -> 0x8000
check 0x8000fbef "白底混出 0xFBEF，黑底混出 0x8000"

echo
echo "[8c] EPIC：VL 图层的旋转与缩放"
# 这两段钉的是 VL 的变换单元，寄存器值都按 HAL 会算出来的样子摆，期望像素
# 手工可推。源图 2x2 RGB565 放在 0x20002000（行距 4 字节）：
#   (0,0)=0xF800 红  (1,0)=0x07E0 绿
#   (0,1)=0x001F 蓝  (1,1)=0xFFFF 白
# 画布清成 CANVAS_BG=0x80（B 通道），RGB565 里是 0x0010。
#
# 旋转：HAL 把输入角度（0.1 度）除以 10 再下发，90 度就是 ROT_DEG=90，并且
# 用 DEG_FORCE 送进 |sin|=1/|cos|=0（Q1.12，4096 和 0，见 §7.5 与文件头）。
# 源绕 pivot(2,2) 转 90 度、再平移，落在 TL(0,0) 起 5x5 的窗口里；反算
# src_x = ly-1、src_y = 3-lx，于是四个源像素只落在 (2,1)(3,1)(2,2)(3,2)。
out=$(run \
    'write 0x20002000 0x8 0x00f8e0071f00ffff' \
    'writel 0x50007018 0x00000080' \
    'writel 0x50007010 0x00000000' \
    'writel 0x50007014 0x00040004' \
    'writel 0x5000701c 0x40040000' \
    'writel 0x50007020 0x00000000' \
    'writel 0x50007024 0x00040004' \
    'writel 0x50007028 0x00010001' \
    'writel 0x50007030 0x20002000' \
    'writel 0x50007034 0x00000168' \
    'writel 0x5000703c 0x00010000' \
    'writel 0x50007040 0x00010000' \
    'writel 0x50007048 0x30000000' \
    'writel 0x50007108 0x80040004' \
    'writel 0x5000710c 0x00020002' \
    'writel 0x50007110 0x00010001' \
    'writel 0x50007114 0x00000000' \
    'writel 0x50007118 0x00000000' \
    'writel 0x500070f8 0x00000000' \
    'writel 0x500070fc 0x20003000' \
    'writel 0x50007100 0x00000000' \
    'writel 0x50007000 0x00000001' \
    'readl 0x20003000' \
    'readl 0x2000300e' \
    'readl 0x20003018')
vals=$(echo "$out" | grep '^OK 0x')
i=0
check 0x00100010 "旋转 90°：没被源盖住的地方留 CANVAS_BG"
check 0xf800001f "旋转 90°：源 (0,1) 蓝 / (0,0) 红 落到 (2,1)(3,1)"
check 0x07e0ffff "旋转 90°：源 (1,1) 白 / (1,0) 绿 落到 (2,2)(3,2)"

# 缩放：2 倍放大。HAL 的输入是 1024=1.0 的定点，放大 2 倍写 512，换算到
# 寄存器是 512<<6 = 0x8000（16.16 的 0.5）。源不动，(init=0) 时反算
# src = (lx * 0x8000) >> 16 = lx/2，所以 2x2 的源变成 4x4 的方块。
out=$(run \
    'write 0x20002000 0x8 0x00f8e0071f00ffff' \
    'writel 0x50007018 0x00000080' \
    'writel 0x50007010 0x00000000' \
    'writel 0x50007014 0x00030003' \
    'writel 0x5000701c 0x40040000' \
    'writel 0x50007020 0x00000000' \
    'writel 0x50007024 0x00030003' \
    'writel 0x50007028 0x00010001' \
    'writel 0x50007030 0x20002000' \
    'writel 0x50007034 0x00000000' \
    'writel 0x5000703c 0x00008000' \
    'writel 0x50007040 0x00008000' \
    'writel 0x50007048 0x00000000' \
    'writel 0x50007114 0x00000000' \
    'writel 0x50007118 0x00000000' \
    'writel 0x500070f8 0x00000000' \
    'writel 0x500070fc 0x20003000' \
    'writel 0x50007100 0x00000000' \
    'writel 0x50007000 0x00000001' \
    'readl 0x20003000' \
    'readl 0x20003004' \
    'readl 0x20003010' \
    'readl 0x20003014')
vals=$(echo "$out" | grep '^OK 0x')
i=0
check 0xf800f800 "放大 2×：源 (0,0) 红铺成 (0,0)(1,0)"
check 0x07e007e0 "放大 2×：源 (1,0) 绿铺成 (2,0)(3,0)"
check 0x001f001f "放大 2×：源 (0,1) 蓝落到第 2 行前两个"
check 0xffffffff "放大 2×：源 (1,1) 白落到第 2 行后两个"

echo
echo "[8d] EPIC：内存图层的缩小（RGB565 源）"
# 钉的是 epic_draw_layer_transformed() 的缩小路径——[8c] 只钉了放大 2×，
# 内存路径的缩小在 qtest 里一直是空的。源 4x4 RGB565 放在 0x20002000，
# 行距 4 像素 = 8 字节（VL_CFG 的 width 字段就是这个**字节**数，见 [8c]），
# 画布 4x4，不覆盖的地方留 CANVAS_BG=0x0010：
#   row0 红 绿 蓝 白     row2 绿 蓝 白 红
#   row1 row3 是干扰行，缩小 2× 不该采到它们
# 缩小 2×：SCALE_RATIO = 0x20000（16.16 的 2.0），init = 0，反算
#   src = (0 + lx*0x20000) >> 16 = 2*lx，只采偶数下标，正好是四个角
#   (0,0)红 (2,0)蓝 (0,2)绿 (2,2)白，缩成 2x2 贴在 TL(0,0)。
out=$(run \
    'write 0x20002000 0x20 0x00f8e0071f00ffff1111222233334444e0071f00ffff00f85555666677778888' \
    'writel 0x50007018 0x00000080' \
    'writel 0x50007010 0x00000000' \
    'writel 0x50007014 0x00030003' \
    'writel 0x5000701c 0x40080000' \
    'writel 0x50007020 0x00000000' \
    'writel 0x50007024 0x00010001' \
    'writel 0x50007028 0x00030003' \
    'writel 0x50007030 0x20002000' \
    'writel 0x50007034 0x00000000' \
    'writel 0x5000703c 0x00020000' \
    'writel 0x50007040 0x00020000' \
    'writel 0x50007048 0x00000000' \
    'writel 0x50007114 0x00000000' \
    'writel 0x50007118 0x00000000' \
    'writel 0x500070f8 0x00000000' \
    'writel 0x500070fc 0x20003000' \
    'writel 0x50007100 0x00000000' \
    'writel 0x50007000 0x00000001' \
    'readl 0x20003000' \
    'readl 0x20003008' \
    'readl 0x20003010' \
    'readl 0x20003018')
vals=$(echo "$out" | grep '^OK 0x')
i=0
check 0x001ff800 "缩小 2×：源 (0,0) 红 / (2,0) 蓝 -> 输出行 0"
check 0xffff07e0 "缩小 2×：源 (0,2) 绿 / (2,2) 白 -> 输出行 1"
check 0x00100010 "缩小 2×：输出行 2 在框外，留 CANVAS_BG"
check 0x00100010 "缩小 2×：输出行 3 在框外，留 CANVAS_BG"

echo
echo "[9] EZIP：私有格式（真的去跑 SDK 的 ezip_linux）"
# 这一节验的是模型和外部工具之间那段：把位流交给工具、把工具吐出来的
# 4 字节头解析成宽高、再把头后面那 7548 个像素放进 DST。
#
# 用 SDK 自己的示例资产：源是 2980 字节的位流，期望结果是 7548 = 68*37*3
# 字节的像素。比对整块内存，因为 readl 逐字读要 1887 次。
#
# 工具和资产都可能不在（比如只 checkout 了 qemu 这个仓库），那就跳过；
# 真正的验收在真机固件那一步，这里只是把工具调用单独钉死。
SDK=${SIFLI_SDK:-/mnt/e/code2/SiFli-SDK}
TOOL=$SDK/tools/png2ezip/ezip_linux
ASSET_SRC=$SDK/example/hal/ezip/assets/clock_mickey_shoe01_565A_s_ezip.dat
ASSET_PIX=$SDK/example/hal/ezip/assets/clock_mickey_shoe01_565A.dat

strip_array() {
    sed 's,/\*.*\*/,,g; s,//.*,,' "$1" | grep -o '0x[0-9a-fA-F]\{2\}' \
        | tr -d '\n' | sed 's/0x//g'
}

if [ ! -x "$TOOL" ] || [ ! -r "$ASSET_SRC" ] || [ ! -r "$ASSET_PIX" ]; then
    echo "  [SKIP] 缺 $TOOL 或 SDK 资产，设 SIFLI_SDK 指向 SDK 根目录可打开"
else
    src_hex=$(strip_array "$ASSET_SRC")
    pix_hex=$(strip_array "$ASSET_PIX")
    src_len=$(( ${#src_hex} / 2 ))
    pix_len=$(( ${#pix_hex} / 2 ))
    if [ "$src_len" -ne 2980 ] || [ "$pix_len" -ne 7548 ]; then
        echo "  [FAIL] 资产长度不对：源 $src_len，像素 $pix_len"
        FAILED=$((FAILED + 1))
    else
        # 源放 0x20004000，结果放 0x20005000。
        out9=$(printf '%s\n' \
                "write 0x20004000 $src_len 0x$src_hex" \
                'writel 0x50006008 0x20005000' \
                'writel 0x50006004 0x20004000' \
                'writel 0x5000600c 0x00000001' \
                'writel 0x50006000 0x00000001' \
                "b64read 0x20005000 $pix_len" \
                quit \
            | timeout 90 "$QEMU" -M "sf32lb52x,ezip-tool=$TOOL" -display none \
                  -serial none -qtest stdio 2>/dev/null | tr -d '\r')
        # b64read 是这一轮里唯一带载荷的 OK（write/writel 只回 OK）。
        got=$(echo "$out9" | grep '^OK .' | tail -1 | sed 's/^OK //')
        if [ -z "$got" ]; then
            echo "  [FAIL] 拿不到 b64read 结果"
            FAILED=$((FAILED + 1))
        else
            got_sha=$(printf '%s' "$got" | base64 -d | sha1sum | cut -d' ' -f1)
            want_sha=$(printf '%s' "$pix_hex" | xxd -r -p | sha1sum \
                       | cut -d' ' -f1)
            if [ "$got_sha" = "$want_sha" ]; then
                printf '  [PASS] %-40s = %s\n' \
                    "私有格式 2980B -> 7548B 逐字节一致" "${got_sha:0:12}"
            else
                printf '  [FAIL] %-40s 期望 %s，实际 %s\n' \
                    "私有格式解码" "${want_sha:0:12}" "${got_sha:0:12}"
                FAILED=$((FAILED + 1))
            fi
        fi
    fi
fi

echo
echo "[9b] EZIP → EPIC：解码结果喂进 2D 流水线"
# 与 [9] 同一条私有格式路径，但 PARA.OUT_SEL 选 EPIC（bit0 = 0）：像素不落
# 内存，而是留在 EZIP 里等 EPIC 的 co-engine 来取。EPIC 侧由 COENG_CFG 指定
# 哪一层吃它：EZIP_EN(bit0) + EZIP_CH_SEL(bits[2:1])，而 CH_SEL = 0 是 VL
# （HAL 的 LayerIdx2CH，不是图层号）。VL_SRC 这时还是压缩流地址，绝不能被
# 当成像素读 —— 这正是模型要和内存取像素那条路分开的地方。
#
# 窗口取解码图第 10 行 x=28..31（START/END_POINT 里列在 bit31:16、行在
# bit15:0，即 START=0x001c000a、END=0x001f000a）。前两个像素 alpha=255，
# 输出应等于资产里的 565 原值 0xffc9/0xb4e6；后两个 alpha=248/243，输出是
# 按解码 alpha 混到黑底上的 0x0820/0x5aa2 —— 注意 x=31 那个：原值 0x62c3
# 混完是 0x5aa2，不混就是原色。HAL 给 EZIP 图层传的是 EPIC_LAYER_OPAQUE，
# CFG 里两个 alpha 位都不置，按普通图层的读法这会画成 0x62c3，所以这一格
# 钉的就是"co-engine 的像素带自己的 alpha"。
#
# 图层的 (0,0) 是源图原点，帧落在 TL_POS + 窗口原点，所以窗口在 (28,10)
# 就要求画布覆盖到那里。画布 32x11、VL 铺满，采样点 (28,10) 的字偏移是
# (10*32+28)*2 = 696。
if [ ! -x "$TOOL" ] || [ ! -r "$ASSET_SRC" ]; then
    echo "  [SKIP] 缺 $TOOL 或 SDK 资产，设 SIFLI_SDK 指向 SDK 根目录可打开"
else
    src_hex=$(strip_array "$ASSET_SRC")
    src_len=$(( ${#src_hex} / 2 ))
    out9b=$(printf '%s\n' \
            "write 0x20004000 $src_len 0x$src_hex" \
            'writel 0x50006004 0x20004000' \
            'writel 0x5000600c 0x00000000' \
            'writel 0x50006014 0x001c000a' \
            'writel 0x50006018 0x001f000a' \
            'writel 0x50006000 0x00000001' \
            'writel 0x500070f8 0x00000000' \
            'writel 0x500070fc 0x20005000' \
            'writel 0x50007100 0x00000000' \
            'writel 0x50007010 0x00000000' \
            'writel 0x50007014 0x000a001f' \
            'writel 0x50007018 0x01000000' \
            'writel 0x5000701c 0x40000002' \
            'writel 0x50007020 0x00000000' \
            'writel 0x50007024 0x000a001f' \
            'writel 0x500070d0 0x00000001' \
            'writel 0x50007000 0x00000001' \
            'readl 0x200052b8' \
            'readl 0x200052bc' \
            quit \
        | timeout 90 "$QEMU" -M "sf32lb52x,ezip-tool=$TOOL" -display none \
              -serial none -qtest stdio 2>/dev/null | tr -d '\r')
    # 两个读的返回。这一轮没有别的载荷读，所以直接取最后两行。
    got0=$(echo "$out9b" | grep '^OK 0x' | tail -2 | sed -n '1p' | sed 's/^OK //')
    got1=$(echo "$out9b" | grep '^OK 0x' | tail -2 | sed -n '2p' | sed 's/^OK //')
    for pair in "0xb4e6ffc9:$got0:不透明像素按资产原值落位" \
                "0x5aa20820:$got1:半透明像素按解码 alpha 混到黑底"; do
        want=${pair%%:*}; rest=${pair#*:}; got=${rest%%:*}; what=${rest#*:}
        if [ -n "$got" ] && [ $((got)) -eq $((want)) ]; then
            printf '  [PASS] %-40s = %s\n' "$what" "$got"
        else
            printf '  [FAIL] %-40s 期望 %s，实际 %s\n' \
                "$what" "$want" "${got:-<无>}"
            FAILED=$((FAILED + 1))
        fi
    done
fi

echo
echo "[9c] EZIP：EPIC 输出下解不出来也必须报 END，不能报错误位"
# EPIC 的 co-engine 握手只认完成回调，而 HAL_EZIP_IRQHandler 只在 END 时调它
# （bf0_hal_ezip.c:530），错误位会让固件永远卡在 while (epic->coeng_state)
# （bf0_hal_epic.c:4688）。所以 EPIC 输出模式下模型必须记一笔、用 END 收尾；
# AHB 那条路照旧报错误位（[7d] 仍是 BTYPE_ERR）。
#
# 把 SRC_ADDR 指到一个读不了的地方，制造一次解码失败。
out=$(run \
    'writel 0x50006004 0xffffffff' \
    'writel 0x5000600c 0x00000000' \
    'writel 0x50006014 0x00000000' \
    'writel 0x50006018 0x00000001' \
    'writel 0x50006000 0x00000001' \
    'readl 0x50006024' \
    'readl 0x50006028')
vals=$(echo "$out" | grep '^OK 0x')
i=0
check 0x1 "INT_STA 报 END 而不是 BTYPE_ERR"
check 0x1 "INT_MASK 报 END 而不是 BTYPE_ERR"

echo
echo "[9d] EZIP → EPIC：co-engine 图层带缩放"
# 和 [9b] 同一条路，只是多写一条 SCALE_RATIO。HAL 允许缩放 co-engine 图层，
# 只拒旋转（bf0_hal_epic.c:6196-6204），缩放时 EPIC 照常被编程：像素来源换
# 了，反算不变，采样点从"SRC 里的 (sx,sy)"改成"帧内的 (sx-start_col,
# sy-start_row)"，落在帧外的不画。
#
# 窗口还是源图第 10 行 x=28..31 那 4 个像素，H=0x8000 表示一个源像素铺两个
# 输出像素。图层 TL=(0,0)、BR=(63,10)，输出 x 反算回源 x>>1：
#   x=56,57 -> 源 28 -> 帧[0] alpha=255 -> 0xffc9
#   x=58,59 -> 源 29 -> 帧[1] alpha=255 -> 0xb4e6
#   x=60,61 -> 源 30 -> 帧[2] alpha=248 -> 混到黑底 0x0820
#   x=62,63 -> 源 31 -> 帧[3] alpha=243 -> 混到黑底 0x5aa2
# x<56 反算出的源坐标小于 start_col=28，在帧外，不画（底由 bypass 保住，
# 是全 0 内存）。y 不缩放（0x10000），start_row=10 所以画在第 10 行。
# 画布 64x11，读点 (10*64+56)*2 = 1392 = 0x570。
if [ ! -x "$TOOL" ] || [ ! -r "$ASSET_SRC" ]; then
    echo "  [SKIP] 缺 $TOOL 或 SDK 资产，设 SIFLI_SDK 指向 SDK 根目录可打开"
else
    src_hex=$(strip_array "$ASSET_SRC")
    src_len=$(( ${#src_hex} / 2 ))
    out9d=$(printf '%s\n' \
            "write 0x20004000 $src_len 0x$src_hex" \
            'writel 0x50006004 0x20004000' \
            'writel 0x5000600c 0x00000000' \
            'writel 0x50006014 0x001c000a' \
            'writel 0x50006018 0x001f000a' \
            'writel 0x50006000 0x00000001' \
            'writel 0x500070f8 0x00000000' \
            'writel 0x500070fc 0x20005000' \
            'writel 0x50007100 0x00000000' \
            'writel 0x50007010 0x00000000' \
            'writel 0x50007014 0x000a003f' \
            'writel 0x50007018 0x01000000' \
            'writel 0x5000701c 0x40000002' \
            'writel 0x50007020 0x00000000' \
            'writel 0x50007024 0x000a003f' \
            'writel 0x5000703c 0x00008000' \
            'writel 0x50007040 0x00010000' \
            'writel 0x50007114 0x00000000' \
            'writel 0x50007118 0x00000000' \
            'writel 0x500070d0 0x00000001' \
            'writel 0x50007000 0x00000001' \
            'readl 0x20005570' \
            'readl 0x20005574' \
            'readl 0x20005578' \
            'readl 0x2000557c' \
        | timeout 90 "$QEMU" -M "sf32lb52x,ezip-tool=$TOOL" -display none \
              -serial none -qtest stdio 2>/dev/null | tr -d '\r')
    vals=$(echo "$out9d" | grep '^OK 0x' | tail -4)
    i=0
    check 0xffc9ffc9 "源 28 铺成 x=56,57"
    check 0xb4e6b4e6 "源 29 铺成 x=58,59"
    check 0x08200820 "源 30（alpha 248）铺成 x=60,61"
    check 0x5aa25aa2 "源 31（alpha 243）铺成 x=62,63"
fi

echo
echo "[10] LCDC：命令路径（控制器问面板「你是谁」）"
# 这一串完全照抄 co5300.c 的 LCD_ReadID()：
#   LCD_ReadMode(true)        SPI_IF_CONF 分频
#   HAL_LCDC_SPI_Sequence(0)  SPI_CS_AUTO_DIS(bit27) 落沿 = 一次事务开始
#   SendSingleCmd(0x03000400, 4)
#                             SPI_IF_CONF.WR_LEN = 3（addr_len-1），
#                             LCD_WR = 命令字，LCD_SINGLE = WR_TRIG
#   HAL_LCDC_SPI_Sequence(1)  置回 bit27
#   SPI_IF_CONF ← RD_LEN=2 | SPI_RD_MODE，LCD_SINGLE = RD_TRIG
#   data = LCD_RD             面板此刻把 3 字节摆在总线上
# 注意命令字里的 0x03 不是长度：HAL_LCDC_ReadU32Reg 是宏，addr_len 恒为 4，
# 要读几个字节写在 SPI_IF_CONF.RD_LEN 里。寄存器号在 (cmd >> 8) & 0xffff。
out=$(run_panel \
    'writel 0x5000809c 0x00000000' \
    'writel 0x5000809c 0x00c00000' \
    'writel 0x50008094 0x03000400' \
    'writel 0x50008090 0x00000002' \
    'writel 0x5000809c 0x09200000' \
    'writel 0x50008090 0x00000005' \
    'readl 0x50008098' \
    'readl 0x50008004' \
    'readl 0x50008090' \
    'writel 0x5000809c 0x00000000' \
    'writel 0x5000809c 0x00c00000' \
    'writel 0x50008094 0x03000a00' \
    'writel 0x50008090 0x00000002' \
    'writel 0x5000809c 0x09200000' \
    'writel 0x50008090 0x00000005' \
    'readl 0x50008098')
vals=$(echo "$out" | grep '^OK 0x')
i=0
check 0x331100 "读 LCD_RD 拿到 a128r16 的 CO5300 面板 ID"
check 0x0      "STATUS 恒不 busy（WaitBusy 不会超时）"
check 0x0      "LCD_SINGLE 恒不 busy（WAIT_LCDC_SINGLE_BUSY 同理）"
check 0x0      "读面板上不存在的寄存器 0x0A 回 0"

echo
echo "[11] LCDC：帧路径（COMMAND.START 就地出帧）"
# 2x1 的 RGB565 图案放在 SRAM 里，LAYER0_SRC 指过去。LAYER0_CONFIG.WIDTH
# 是**字节**行距（LayerUpdate 把 layer_1line_total_bytes 移进去），不是像素数。
# HAL 的顺序是先把 EOF 中断解除屏蔽，再 START。
out=$(run_panel \
    'writel 0x20000000 0x001f001f' \
    'writel 0x5000801c 0x10008000' \
    'writel 0x50008020 0x00000000' \
    'writel 0x50008024 0x00000001' \
    'writel 0x5000802c 0x20000000' \
    'writel 0x50008080 0x00000400' \
    'writel 0x5000800c 0x00000001' \
    'writel 0x50008000 0x00000001' \
    'readl 0x50008008' \
    'writel 0x50008008 0x00010001' \
    'readl 0x50008008')
vals=$(echo "$out" | grep '^OK 0x')
i=0
# SETTING 里 EOF 已解除屏蔽，所以 STAT(bit0) 和 RAW(bit16) 一起置起来。
check 0x10001 "START 后 IRQ 置 EOF_STAT|EOF_RAW"
check 0x0     "写 1 清后 IRQ 读回 0"

echo
echo "[12] LCDC：忘了 -device sifli-panel 时不崩，只是读回 0"
out=$(run \
    'writel 0x5000809c 0x00000000' \
    'writel 0x5000809c 0x00c00000' \
    'writel 0x50008094 0x03000400' \
    'writel 0x50008090 0x00000002' \
    'writel 0x5000809c 0x09200000' \
    'writel 0x50008090 0x00000005' \
    'readl 0x50008098')
vals=$(echo "$out" | grep '^OK 0x')
i=0
check 0x0 "没有面板时读数路径回 0"

echo
echo "[13] SysTick：tick clock（refclk）速率"
# 这一项钉的是 hw/timer/armv7m_systick.c 里 **systick_cpuclk_update() 那个**
# return; —— 本树唯一改动的上游 QEMU 文件。SYST_CSR.CLKSOURCE=0 时 SysTick
# 数的是 refclk，这时改 CPU 时钟**不能**动到它的周期；丢了那个 return，末段
# 把 HCLK 切到 240 MHz 会把周期一起改掉，100 ms 读回 7222784 拍而不是
# 80000，check_near 直接 FAIL。
#
# 另一个 return;（systick_refclk_update() 里的）这一项盖不到：全程
# CLKSOURCE=0，refclk 变化时它本来就不短路，有没有那行行为一样。两个 return
# 的守卫条件正好互补（一个 `if (!(csr & CLKSOURCE))`、一个 `if (csr & ...)`），
# 所以得分处在两种 CLKSOURCE 下各钉一个 —— 那个由 [14] 的第二段负责。
#
# 量法：把 RVR 写满（24 位，100 ms 窗口内不会 wrap），读 CVR 记初值，
# clock_step 之后读 CVR 记末值，差值就是走过的拍数。
#   - **不要拿"写 CVR 清零"当取基准的办法**：systick_write() 的 case 0x8 确实
#     清计数，但 ptimer 带 NO_IMMEDIATE_RELOAD，重装要等下一拍，写完之后紧接
#     的那次读是 ~0 而不是 RVR；窗口跨过这次重装，差值就是错的。改用"先垫
#     一步"：开 ENABLE 之后第一次读还在重装前（读回 0），所以每段窗口前面先
#     推 10 ms，等它稳定下来。
#   - 读的时刻和拍边界不一定对齐，所以断言带几拍容差（check_near）。
#
# 复位：machine 给 refclk 接了源（systickclk），QEMU 那条"没有 refclk 就
# 强制 CLKSOURCE=1"的分支因此不生效，CSR 复位读回 0 —— 计数器是停的，
# 得自己写 ENABLE。此刻 refclk 就是 RCC 复位选中的 LXT 32768 Hz。
#
# 然后按固件的做法把它程序成 800 kHz（HRC48 / TICKDIV=60，见 §3.2），量一次；
# 再把 HCLK 切到 240 MHz（DLL1CR 的 EN + STG=9）量一次 —— 两次都必须是
# 800 kHz，这是 systick_cpuclk_update() 那个 return 管的事（refclk 那个见 [14]）。
out=$(run_clock \
    'readl 0xe000e010' \
    'writel 0xe000e014 0x00ffffff' \
    'writel 0xe000e010 0x1' \
    'clock_step 10000000' \
    'readl 0xe000e018' \
    'clock_step 100000000' \
    'readl 0xe000e018' \
    'writel 0x50000024 0x003c0000' \
    'writel 0x50000020 0x00004001' \
    'clock_step 10000000' \
    'readl 0xe000e018' \
    'clock_step 100000000' \
    'readl 0xe000e018' \
    'writel 0x5000002c 0x00000025' \
    'writel 0x50000020 0x00004003' \
    'clock_step 10000000' \
    'readl 0xe000e018' \
    'clock_step 100000000' \
    'readl 0xe000e018')
vals=$(echo "$out" | grep '^OK 0x')
# CVR 是往下数的，所以窗口 = 前一次读 - 后一次读。
csr=$(( $(rd 1) ))
lxt=$(( $(rd 2) - $(rd 3) ))
hrc=$(( $(rd 4) - $(rd 5) ))
dll=$(( $(rd 6) - $(rd 7) ))
vals=$(printf 'OK 0x%x\nOK 0x%x\nOK 0x%x\nOK 0x%x\n' "$csr" "$lxt" "$hrc" "$dll")
i=0
check 0x0 "复位 CSR：refclk 有源，计数器默认停着"
check_near 3276 4 "复位 tick（LXT 32768 Hz）：100 ms 走 3276 拍"
check_near 80000 5 "程序成 HRC48/60 后：100 ms 走 80000 拍"
check_near 80000 5 "HCLK 切到 240 MHz 后，refclk 不动：还是 80000"

echo
echo "[14] RCC → SysTick：CLKSOURCE 门控"
# [13] 只盖到了 systick_cpuclk_update() 那个 return;（它在 CLKSOURCE=0 下
# 全程有效）。这一项在 CLKSOURCE=1 下盖另一个，顺带把 RCC→sysclk 那条耦合
# 也钉住，而不是只证"CLKSOURCE 真的在选时钟"：
#   - 把 CSR 置 1（0x5 = ENABLE | CLKSOURCE）—— 立刻按 HCLK 走；复位的
#     sysclk 是 48 MHz（HXT48，[2] 那边量过 CSR），1 ms 正好 48000 拍。
#   - **在 CLKSOURCE=1 期间改 tick clock**（TICKDIV 60→30，800 kHz→1.6 MHz）。
#     这一改只走 refclk 那条回调，它必须因为 CLKSOURCE=1 而短路；丢了
#     systick_refclk_update() 里的 return;，周期会被改成 1.6 MHz，这一段
#     读回 1600 拍而不是 48000，直接 FAIL。
#   - **再在 CLKSOURCE=1 期间把 HCLK 切到 240 MHz**（DLL1CR 的 EN + STG=9，
#     同 [13]）。这一段要读到 240000 拍/ms，而不是复位那个 48000 —— 也就是
#     说它验的是 peripherals.md §3.1 那条耦合："RCC 算出来的 sysclk 真的喂
#     到了 SysTick"。RCC 钩子末尾那行 clock_update_hz(s->clk, …) 丢了、
#     sysclk 永远停在 48 MHz，[13] 和本项前面两段都不会红（它们要么
#     CLKSOURCE=0、要么只动 tick），只有这一段会读到 48000 而 FAIL。
#   - 最后把 CSR 切回 0x1，立刻回到 800 kHz。
out=$(run_clock \
    'writel 0x50000024 0x003c0000' \
    'writel 0x50000020 0x00004001' \
    'writel 0xe000e014 0x00ffffff' \
    'writel 0xe000e010 0x5' \
    'clock_step 1000000' \
    'readl 0xe000e018' \
    'clock_step 1000000' \
    'readl 0xe000e018' \
    'writel 0x50000024 0x001e0000' \
    'clock_step 1000000' \
    'readl 0xe000e018' \
    'writel 0x50000024 0x003c0000' \
    'writel 0x5000002c 0x00000025' \
    'writel 0x50000020 0x00004003' \
    'clock_step 1000000' \
    'readl 0xe000e018' \
    'clock_step 1000000' \
    'readl 0xe000e018' \
    'writel 0xe000e010 0x1' \
    'clock_step 1000000' \
    'readl 0xe000e018' \
    'clock_step 1000000' \
    'readl 0xe000e018')
vals=$(echo "$out" | grep '^OK 0x')
cpu=$(( $(rd 1) - $(rd 2) ))
gated=$(( $(rd 2) - $(rd 3) ))
hclk=$(( $(rd 4) - $(rd 5) ))
tick=$(( $(rd 6) - $(rd 7) ))
vals=$(printf 'OK 0x%x\nOK 0x%x\nOK 0x%x\nOK 0x%x\n' \
       "$cpu" "$gated" "$hclk" "$tick")
i=0
check_near 48000 8    "CLKSOURCE=1：HCLK 48 MHz，1 ms 走 48000 拍"
check_near 48000 8    "CLKSOURCE=1 时把 tick 改成 1.6 MHz：cpuclk 路不动"
check_near 240000 12  "CLKSOURCE=1 时 HCLK 切到 240 MHz：1 ms 走 240000 拍"
check_near 800 4      "切回 CLKSOURCE=0：立刻回到 800 kHz"

echo
echo "[15] GTIMR：自由计数器，跟着 RTC_CR.LPCKSEL 变频"
# 这一段原在 verify-sifli.sh 第 6 项，搬到 qtest 里才进得了 CI（第 7 项会跑
# 本脚本，所以 verify 那边的覆盖没丢）。
#
# GTIMR 是自由计数器，不能按固定值查——只能看它动没动。HAL_GetTick() 就是
# 它折成毫秒的结果，除以 32768 还是除以 10 取决于 RTC_CR.LPCKSEL 选的是
# 晶振还是 RC（drv_common.c:342、:360）；冻住的话 SDK 里每个 HAL 等待循环
# 都不再超时，未建模的外设会变成死循环而不是干净的 HAL_TIMEOUT。rt_driver
# 的触摸初始化踩过这个坑（§3.3）。
#
# 频率不是常数：RTC_CR.LPCKSEL 置位是 32 kHz 晶振，清零是约 10 kHz 的 RC
# （复位默认清零），HAL_GetTick() 拿同一个位决定除 32768 还是除 10
# （drv_common.c:342、:360），所以两边必须一致。两次读和后面变频的读数共用
# 一次 qtest——clock_step 会话都要靠 timeout 收尾，多起一次就多等 30 秒。
out=$(run_clock \
    'readl 0x500c0034' \
    'clock_step 1000000' 'readl 0x500c0034' \
    'writel 0x500cb008 0x1' \
    'clock_step 1000000000' 'readl 0x500c0034' \
    'clock_step 1000000000' 'readl 0x500c0034' \
    'writel 0x500cb008 0x0' \
    'clock_step 1000000000' 'readl 0x500c0034' \
    'clock_step 1000000000' 'readl 0x500c0034')
vals=$(echo "$out" | grep '^OK 0x')
# 每个设置各走 1 秒，数这 1 秒里涨了多少；[6] 那类寄存器回读用不上它。
rc=$(( $(rd 2) - $(rd 1) ))
xt=$(( $(rd 4) - $(rd 3) ))
rc2=$(( $(rd 6) - $(rd 5) ))
vals=$(printf 'OK 0x%x\nOK 0x%x\nOK 0x%x\n' "$rc" "$xt" "$rc2")
i=0
check 10    "RC 下 1 ms 走 10 拍（计数器是活的）"
check 32768 "LPCKSEL=1：1 秒走 32768 拍（晶振）"
check 10000 "LPCKSEL=0：1 秒走 10000 拍（RC）"

echo
echo "[16] DWT：周期计数器在动"
# 本树新写的设备（hw/misc/armv7m_dwt.c，落在 0xe0001000）。固件的
# HAL_Delay_us_ 自旋在 DWT_CYCCNT 上，冻住就是死循环——而且 HAL_PreInit
# 在能打印任何东西之前就会调到它（§11.1）。弱检查：CTRL 能读写（证明那一页
# 是我们的，而不是被 armv7m 的 nvic-default 吞掉），清过 CYCCNT 之后推一次
# 时钟，计数器必须往前走。
#
# CYCCNT 是按**主机**墙钟现算的（设备文件头解释了为什么不用虚拟时钟），
# 所以这一推主要是让真实时间过去；clock_step 本身推不动它。
out=$(run_clock \
    'readl 0xe0001000' \
    'writel 0xe0001000 0x1' \
    'readl 0xe0001000' \
    'writel 0xe0001004 0x0' \
    'readl 0xe0001004' \
    'clock_step 1000000000' \
    'readl 0xe0001004')
vals=$(echo "$out" | grep '^OK 0x')
ctrl0=$(( $(rd 1) ))
ctrl1=$(( $(rd 2) ))
base=$(( $(rd 3) ))
now=$(( $(rd 4) ))
vals=$(printf 'OK 0x%x\nOK 0x%x\nOK 0x%x\n' \
       "$ctrl0" "$ctrl1" "$(( now > base ? 1 : 0 ))")
i=0
check 0x0 "DWT_CTRL 复位为 0"
check 0x1 "DWT_CTRL 可读写（0xe0001000 这一页是我们的）"
check 0x1 "清 CYCCNT 后推一次时钟，计数器在涨"

echo
if [ "$FAILED" -eq 0 ]; then
    echo "全部通过。"
    exit 0
fi
echo "有 $FAILED 项失败。"
exit 1
