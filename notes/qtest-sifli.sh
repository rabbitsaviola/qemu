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

# 面板是 -device 加进来的，所以要单独起一次机器。设备在 machine init 之后
# 才建，面板 realize 时把自己反向注册给 LCDC 的 QSPI 总线。
run_panel() {
    printf '%s\n' "$@" quit \
        | timeout 30 "$QEMU" -M sf32lb52x -display none \
              -serial none -device sifli-panel -qtest stdio 2>/dev/null | tr -d '\r'
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
if [ "$FAILED" -eq 0 ]; then
    echo "全部通过。"
    exit 0
fi
echo "有 $FAILED 项失败。"
exit 1
