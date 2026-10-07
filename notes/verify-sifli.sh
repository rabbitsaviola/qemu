#!/bin/bash
#
# sifli-qemu 验证脚本。检查项与说明见同目录的 build-and-verify.md。
#
# 用法：
#   bash notes/verify-sifli.sh
#
# 全部通过退出码为 0，有失败为 1（可直接用于 CI）。
#
# 环境变量可覆盖：
#   SIFLI_QEMU_SRC    源码树   （默认 ~/code/sifli-qemu）
#   SIFLI_QEMU_BUILD  构建目录 （默认 ~/build-sifli）
#   SIFLI_REAL_FW     真实板子固件；传 "-" 跳过那一项
#   SIFLI_EZIP_FW     例程固件；传 "-" 跳过那一项
#   SIFLI_EZIP_TOOL   宿主的 ezip 解码器（私有格式要用）
#   SIFLI_EPIC_FW     EPIC 例程固件；传 "-" 跳过那一项
#   SIFLI_ARM_NM      arm-none-eabi-nm（要从固件里读 buffer2 的地址）

set -u

SRC=${SIFLI_QEMU_SRC:-$HOME/code/sifli-qemu}
BUILD=${SIFLI_QEMU_BUILD:-$HOME/build-sifli}

FAILED=0
pass() { echo "  [PASS] $*"; }
fail() { echo "  [FAIL] $*"; FAILED=$((FAILED + 1)); }
skip() { echo "  [SKIP] $*"; }

echo "sifli-qemu 验证"
echo "  源码: $SRC"
echo "  构建: $BUILD"
echo

# ---------------------------------------------------------------- 1. 二进制
echo "[1] 构建产物"
for bin in qemu-system-arm qemu-system-aarch64; do
    if [ -x "$BUILD/$bin" ]; then
        pass "$bin"
    else
        fail "$bin 不存在，先跑 build-sifli.sh"
    fi
done
echo

# -------------------------------------------- 2. machine 可见性（两个二进制）
# 接口声明漏了的话，machine 会静默从 -M help 消失（编译不报错）。
# 所以两个二进制都必须查。
echo "[2] machine 可见性 —— 两个二进制都要有"
for bin in qemu-system-arm qemu-system-aarch64; do
    [ -x "$BUILD/$bin" ] || { skip "$bin 不存在"; continue; }
    if "$BUILD/$bin" -M help 2>&1 | grep -q '^sf32lb52x'; then
        pass "$bin 能看到 sf32lb52x"
    else
        fail "$bin 里没有 sf32lb52x —— 检查 TypeInfo 的 .interfaces"
    fi
done
echo

# ------------------------------------------------------- 3. 格式（checkpatch）
echo "[3] 代码格式"
NEW_FILES="hw/arm/sifli-sf32lb52x.c
include/hw/arm/sf32lb52x.h
hw/arm/sf32lb52x-periph.c
hw/char/sifli-usart.c
include/hw/char/sifli-usart.h
hw/dma/sifli-dma.c
include/hw/dma/sifli-dma.h
hw/display/sifli-epic.c
include/hw/display/sifli-epic.h
hw/display/sifli-ezip.c
include/hw/display/sifli-ezip.h
include/hw/misc/sifli-sbus.h
hw/misc/sifli-regbank.c
include/hw/misc/sifli-regbank.h
hw/display/sifli-lcdc.c
include/hw/display/sifli-lcdc.h
hw/display/sifli-panel.c
include/hw/display/sifli-panel.h
hw/i2c/sifli-i2c.c
include/hw/i2c/sifli-i2c.h
hw/input/touch-panel.c
include/hw/input/touch-panel.h
hw/input/ft6146.c
include/hw/input/ft6146.h
hw/misc/sifli-gpio.c
include/hw/misc/sifli-gpio.h"

if [ -f "$SRC/scripts/checkpatch.pl" ]; then
    for f in $NEW_FILES; do
        [ -f "$SRC/$f" ] || { skip "$f 不存在"; continue; }
        # 必须给**绝对路径**。checkpatch 有若干规则按 $realfile 里的 "/hw/" 之类
        # 匹配，相对路径 "hw/dma/x.c" 里没有前导那个 "/"，规则就静默不触发——
        # 于是 0 errors 是个假通过。qemu_bh_new 必须换成 aio_bh_new_guarded 那条
        # 就是这么漏掉的。
        out=$(cd "$SRC" && perl scripts/checkpatch.pl --no-tree --file "$SRC/$f" 2>&1)
        # checkpatch 只在有 ERROR 时返回非零，所以要抓 summary 行
        if echo "$out" | grep -qE '^total: 0 errors, 0 warnings'; then
            pass "$f"
        else
            fail "$f"
            echo "$out" | grep -E '^(ERROR|WARNING|total):' | head -5 | sed 's/^/         /'
        fi
    done
else
    skip "找不到 checkpatch.pl"
fi
echo

# --------------------------------------------------------------- 4. 行尾
echo "[4] 工作区行尾（应为 LF）"
# -e 而不是 -d：在 git worktree 里 .git 是个指向主仓库的文件，git 命令照样能用。
if [ -e "$SRC/.git" ]; then
    bad=$(cd "$SRC" && git ls-files --eol \
              $NEW_FILES \
              hw/arm/Kconfig hw/arm/meson.build \
              hw/char/Kconfig hw/char/meson.build \
              hw/dma/Kconfig hw/dma/meson.build \
              hw/display/Kconfig hw/display/meson.build \
              hw/i2c/Kconfig hw/i2c/meson.build \
              hw/input/Kconfig hw/input/meson.build \
              hw/misc/Kconfig hw/misc/meson.build 2>/dev/null \
          | grep 'w/crlf' || true)
    if [ -z "$bad" ]; then
        pass "新文件与改动的构建文件都是 LF"
    else
        fail "有文件在工作区是 CRLF，git diff 会显示整个文件被改："
        echo "$bad" | sed 's/^/         /'
        echo "         修：sed -i 's/\r$//' <文件>"
    fi
else
    skip "不是 git 仓库"
fi
echo

# ----------------------------------------------------------- 5. 提交完整性
echo "[5] 提交完整性"
if [ -e "$SRC/.git" ]; then
    last=$(cd "$SRC" && git log -1 --format=%B)
    author=$(cd "$SRC" && git log -1 --format='%an <%ae>')
    sob=$(echo "$last" | grep -m1 '^Signed-off-by:' | sed 's/^Signed-off-by: *//')
    if [ -z "$sob" ]; then
        fail "最新提交没有 Signed-off-by（CI 的 check-dco 会挂）"
    elif [ "$sob" = "$author" ]; then
        pass "Signed-off-by 与 author 一致（$author）"
    else
        fail "Signed-off-by 与 author 不一致"
        echo "         author: $author"
        echo "         sob   : $sob"
    fi
else
    skip "不是 git 仓库"
fi
echo

# --------------------------------------------------------------- 6. 外设模型
# 表注册 / 设备创建 / 地址映射 / 强制位，这条链上任何一环断了，机器照样能启动，
# 只是固件读到一片 0 然后卡在某个 while 里。所以直接从 monitor 读回寄存器。
echo "[6] 外设模型（寄存器回读）"
if [ ! -x "$BUILD/qemu-system-arm" ]; then
    skip "二进制不存在"
else
    probe_in=$(cat <<'EOF'
info mtree
xp /1wx 0x50006000
xp /1wx 0x50007000
xp /1wx 0x40040040
xp /1wx 0x50000020
xp /1wx 0x5000002c
xp /1wx 0x5000b004
xp /1wx 0x5008401c
xp /1wx 0x500c0010
xp /1wx 0x500c002c
xp /1wx 0x500ca01c
xp /1wx 0x500ca020
xp /1wx 0x50008004
xp /1wx 0x50008008
xp /1wx 0x50008090
EOF
)
    probe=$(printf '%s\nquit\n' "$probe_in" \
                | timeout 40 "$BUILD/qemu-system-arm" -M sf32lb52x \
                      -display none -monitor stdio -serial none -S 2>&1 \
                | tr -d '\r')

    if [ -z "$probe" ]; then
        fail "拿不到 monitor 输出"
    else
        for r in sf32lb52x.hpsys_rcc sf32lb52x.lpsys_rcc sf32lb52x.hpsys_cfg \
                 sf32lb52x.hpsys_aon sf32lb52x.lpsys_aon sf32lb52x.pmuc; do
            if echo "$probe" | grep -q "$r"; then
                pass "$r 已映射"
            else
                fail "$r 不在 info mtree 里"
            fi
        done

        # 两个 DMAC 的 region 同名，所以按地址认，两个都必须在。
        while read -r a region what; do
            [ -n "$a" ] || continue
            if echo "$probe" | grep -q "$a-.*$region"; then
                pass "$what 已映射（$a）"
            else
                fail "$what 不在 info mtree 里（$a）"
            fi
        done <<'EOF'
50081000 sifli-dma  DMAC1
40001000 sifli-dma  DMAC2
50006000 sifli-ezip EZIP1
50007000 sifli-epic EPIC
EOF

        # 地址 region名 说明。PSRAM 必须真的是块 RAM 而不是落到
        # ignore_memory_transaction_failures 的兜底上——固件的 514 KiB 显存
        # 在 0x60400000，两种情况下读写都不报错、读回都是 0，只有 mtree 分得清。
        while read -r a name what; do
            [ -n "$a" ] || continue
            if echo "$probe" | grep -q "$a-.*$name"; then
                pass "$what 已映射（$a → $name）"
            else
                fail "$what 不在 info mtree 里（$a → $name）"
            fi
        done <<'EOF'
50008000 sifli-lcdc  LCDC1
60000000 sifli.psram PSRAM
EOF

        # 地址 期望值 说明
        while read -r addr want what; do
            [ -n "$addr" ] || continue
            got=$(echo "$probe" | grep -i "$addr:" | awk '{print $2}' \
                      | tr 'A-F' 'a-f')
            want=$(echo "$want" | tr 'A-F' 'a-f')
            if [ "$got" = "$want" ]; then
                pass "$what = $got"
            else
                fail "$what：期望 $want，实际 ${got:-<读不到>}"
            fi
        done <<'EOF'
50000020 0x00000001 HPSYS_RCC.CSR 复位选 HXT48
5000002c 0x80000000 HPSYS_RCC.DLL1CR READY 强制置位
5000b004 0x00000707 HPSYS_CFG.IDR REVID=A4
500c0010 0xc0000000 HPSYS_AON.ACR HXT48/HRC48 RDY
500c002c 0x00000030 HPSYS_AON.ISSR HP+LP ACTIVE
500ca01c 0x80000000 PMUC.LRC32_CR RDY
500ca020 0x80000000 PMUC.LXT_CR RDY
40040040 0x00000000 LPSYS_AON.SLP_CTRL SLEEP_STATUS 读 0
5008401c 0x000000c0 USART1.ISR TXE|TC 恒置
50006000 0x00000000 EZIP1.CTRL 复位后为 0（弱检查，写路径在 qtest 里）
50007000 0x00000000 EPIC.COMMAND 复位后为 0（同上）
50008004 0x00000000 LCDC.STATUS 不 busy
50008008 0x00000000 LCDC.IRQ 无未决 EOF
50008090 0x00000000 LCDC.LCD_SINGLE 不 busy
EOF

    fi
fi
echo

# --------------------------------------------------------- 7. 写路径（qtest）
# 第 6 项只能读，写路径单独用 qtest 验。说明见 qtest-sifli.sh。
echo "[7] 写路径（qtest）"
if [ -f "$SRC/notes/qtest-sifli.sh" ]; then
    if out=$(SIFLI_QEMU_BUILD="$BUILD" bash "$SRC/notes/qtest-sifli.sh" 2>&1); then
        pass "RCC 使能别名（ESR/ECR→ENR）与普通寄存器读写"
    else
        fail "qtest 写路径验证失败："
        echo "$out" | sed 's/^/         /'
    fi
else
    skip "找不到 notes/qtest-sifli.sh"
fi
echo

# --------------------------------------------------------------- 8. 画面
# 前面几项只证明寄存器读写对；这一项证明**像素真的出去了**：qtest 在显存里
# 放一块已知颜色、START、然后让 monitor 截图，再解 PPM 断言具体像素和尺寸。
#
# 不需要 SDL，也不需要固件。ui/console.c 是无条件编进 system_ss 的，而且
# dpy_gfx_replace_surface() 不管有没有显示后端都会把 surface 存进 con->surface
# （ui/console.c:837），所以 -display none 下 screendump 照样有东西可截。
# 也就是说这一项在没有 SDL 的构建和 CI 上都要能过。
#
# qtest 和 monitor 各要一条输入通道，所以 qtest 仍走 stdio（配 FIFO，和第 9 项
# 同一个写法），monitor 另开一条 unix socket —— 只为了发一句 screendump。
echo "[8] 画面输出（qtest 写显存 + screendump 截图）"
if [ ! -x "$BUILD/qemu-system-arm" ]; then
    skip "二进制不存在"
elif ! command -v nc >/dev/null 2>&1 || ! nc -h 2>&1 | grep -q -- '-U'; then
    skip "没有支持 -U 的 nc，发不了 screendump 给 monitor"
elif ! command -v python3 >/dev/null 2>&1; then
    skip "没有 python3，解不了 PPM"
else
    fifo=$(mktemp -u)
    sock=$(mktemp -u)
    log=$(mktemp)
    ppm=$(mktemp -u).ppm
    mkfifo "$fifo"

    # -S 是必须的：qtest 加速器下 vCPU 仍然会跑，而这里没有 -kernel，CPU 从
    # 一片全零的 ROM 开始执行，几秒后必然撞进 HardFault 把 QEMU 打死
    # （qemu: fatal: Lockup: can't escalate 3 to HardFault）。这一项要的只是
    # 寄存器写和截图，不需要客户机执行任何指令。
    exec 3<>"$fifo"
    timeout 60 "$BUILD/qemu-system-arm" -M sf32lb52x \
        -display none -serial none -device sifli-panel -S \
        -qtest stdio -monitor unix:$sock,server=on,wait=off \
        < "$fifo" > "$log" 2>&1 &
    qpid=$!

    for _ in $(seq 1 100); do
        [ -S "$sock" ] && break
        kill -0 "$qpid" 2>/dev/null || break
        sleep 0.1
    done

    # 等第 n 次 START 的回读出现。不等的话截到的可能是画之前的那一帧 ——
    # qtest 是异步收命令的，写完就截图会和 START 抢。
    wait_eof() {
        for _ in $(seq 1 100); do
            [ "$(grep -c '^OK 0x10001' "$log" 2>/dev/null)" -ge "$1" ] \
                && return 0
            kill -0 "$qpid" 2>/dev/null || return 1
            sleep 0.1
        done
        return 1
    }

    # 第一帧：2x1 的纯红（RGB565 0xF800）画在 (10,20)。LAYER0_CONFIG.WIDTH 是
    # **字节**行距（2 像素 × 2 字节 = 4），所以是 4 << 13；LCD_CONF 的
    # SPI_LCD_FORMAT 写 1（bit10）表示面板侧 RGB565。SRC 指向这一块的首像素
    # （见下一帧的说明），这里它正好就是 0x20000000，所以图案也放这儿。
    printf '%s\n' \
        'writel 0x20000000 0xf800f800' \
        'writel 0x5000801c 0x10008000' \
        'writel 0x50008020 0x0014000a' \
        'writel 0x50008024 0x0014000b' \
        'writel 0x5000802c 0x20000000' \
        'writel 0x50008080 0x00000400' \
        'writel 0x5000800c 0x00000001' \
        'writel 0x50008000 0x00000001' \
        'readl 0x50008008' >&3
    wait_eof 1

    # 第二帧走 rt_driver 那条路：RGB888 源、显存在 PSRAM、行距 390×3 = 1170
    # 字节、一个像素画在 (100,50)。验收固件正是这么配的，先把这条几何验掉，
    # 免得等固件编出来才发现转换或 PSRAM 取数是错的。
    #
    # LAYER0_SRC 给的是**这一块自己的首像素**（0x60400000 + 50×1170 + 100×3），
    # 不是显存基址：硬件从 SRC 起按相对坐标寻址，TL/BR 只决定画到屏幕哪儿。
    # SDK 就是这么算的——SetupLineIrq()（bf0_hal_lcdc.c:1150-1165）先把
    # data_area 的角点减掉，再去预测 LCDC 会读哪个地址；drv_lcd 也是把脏矩形
    # 的首像素指针交给 LayerUpdate 的（drv_lcd.c:1503、:3290）。给成基址的话
    # 模型会老老实实从基址读，画出来一片黑。
    #
    # 颜色特意选**纯蓝**：RGB888 在内存里是 B,G,R，所以字节是 FF,00,00。
    # 要是模型把字节序弄反了，读出来就是纯红，(10,20) 那块也是红的，一眼可见。
    printf '%s\n' \
        'writel 0x50008008 0x00010001' \
        'writeb 0x6040e5b0 0xff' \
        'writeb 0x6040e5b1 0x00' \
        'writeb 0x6040e5b2 0x00' \
        'writel 0x5000801c 0x10924001' \
        'writel 0x50008020 0x00320064' \
        'writel 0x50008024 0x00320064' \
        'writel 0x5000802c 0x6040e5b0' \
        'writel 0x50008000 0x00000001' \
        'readl 0x50008008' >&3
    wait_eof 2

    printf 'screendump %s\n' "$ppm" \
        | timeout 10 nc -U -q 1 "$sock" >/dev/null 2>&1
    for _ in $(seq 1 50); do
        [ -s "$ppm" ] && break
        sleep 0.1
    done

    kill "$qpid" 2>/dev/null
    wait "$qpid" 2>/dev/null
    exec 3>&-
    rm -f "$fifo" "$sock"

    if [ ! -s "$ppm" ]; then
        fail "screendump 没产出文件"
        sed 's/^/         /' "$log" | tail -6
    else
        # P6 的头是变长的（还可能夹注释），所以按 token 解而不是按行。
        # 期望：面板给的 390x450；第一帧画的红块还在（影子显存把两帧叠起来了，
        # 不是每帧清屏）；第二帧那个 RGB888 像素是**蓝**不是红（字节序对了）；
        # 没画到的角落是黑（影子建的时候清过零，所以"只有那两块被画上去"
        # 这件事也一起验了）。
        bad=$(python3 - "$ppm" <<'PY' 2>&1
import sys

raw = open(sys.argv[1], 'rb').read()
if raw[:2] != b'P6':
    print('不是 P6 PPM（头是 %r）' % raw[:2])
    sys.exit(0)

i, tok = 2, []
while len(tok) < 3:
    while raw[i:i + 1] and raw[i:i + 1].isspace():
        i += 1
    if raw[i:i + 1] == b'#':
        while raw[i:i + 1] and raw[i:i + 1] != b'\n':
            i += 1
        continue
    j = i
    while raw[j:j + 1] and not raw[j:j + 1].isspace():
        j += 1
    if j == i:
        print('PPM 头解析失败')
        sys.exit(0)
    tok.append(int(raw[i:j]))
    i = j
i += 1
w, h = tok[0], tok[1]

# 行距不是 w*3。ppm_save()（ui/ui-qmp-cmds.c:321）每行写的是
# pixman_image_get_stride(linebuf)，而那个 linebuf 是 24bpp 的，pixman 会把
# 行距按 4 字节对齐 —— 390 像素时 1170 变成 1172。头里写的还是 390 像素宽，
# 所以这个 PPM 严格来说对不上，按 w*3 去解会逐行错位。两个都试，以文件实际
# 长度为准，这样上游哪天改成紧凑排布也不会挂。
tight, aligned = w * 3, (w * 3 + 3) & ~3
if len(raw) - i == tight * h:
    pitch = tight
elif len(raw) - i == aligned * h:
    pitch = aligned
else:
    print('数据长度 %d 既不是 %d 也不是 %d' % (len(raw) - i, tight * h, aligned * h))
    sys.exit(0)

def px(x, y):
    o = i + y * pitch + x * 3
    return tuple(raw[o:o + 3])

if (w, h) != (390, 450):
    print('尺寸是 %dx%d，期望 390x450（面板给的）' % (w, h))
elif px(10, 20) != (255, 0, 0):
    print('(10,20) 是 %s，期望红 (255,0,0)' % (px(10, 20),))
elif px(11, 20) != (255, 0, 0):
    print('(11,20) 是 %s，期望红 (255,0,0)' % (px(11, 20),))
elif px(12, 20) != (0, 0, 0) or px(10, 21) != (0, 0, 0):
    print('(12,20)/(10,21) 是 %s/%s，期望黑（只该画 2x1 那一块）'
          % (px(12, 20), px(10, 21)))
elif px(100, 50) != (0, 0, 255):
    print('(100,50) 是 %s，期望蓝 (0,0,255)——RGB888 的 B,G,R 字节序，'
          '以及从 PSRAM 取数' % (px(100, 50),))
elif px(0, 0) != (0, 0, 0):
    print('(0,0) 是 %s，期望黑（没画到的地方）' % (px(0, 0),))
elif px(389, 449) != (0, 0, 0):
    print('(389,449) 是 %s，期望黑（没画到的地方）' % (px(389, 449),))
PY
)
        rm -f "$ppm"
        if [ -z "$bad" ]; then
            pass "390x450；RGB565 红块与 PSRAM 里 RGB888 蓝点各就各位，未画处黑"
        else
            fail "截图像素不对：$bad"
            sed 's/^/         /' "$log" | tail -6
        fi
    fi
    rm -f "$log"
fi
echo

# ------------------------------------------------------- 9. 真实板子固件
# 这一项才是真正的验收。
#
# 第 6、7 项只是结构性地读几个寄存器，模型写错了它们照样能过
# （peripherals.md §12.2 那个 DWT 映射错位的坑就骗过了它们全部）。只有这里
# ——SDK 原样构建的真实板子固件、一行不改——能证明整个 HAL 真的在模型上跑
# 起来了：时钟树、电源、RTC、MPI、音频、控制台，一路到 main() 和 RT-Thread
# 的 shell。
#
# 而且还要往提示符里敲命令 —— 这是唯一能验到 **接收** 通路的地方。控制台把
# uart1 按 DMA 模式打开（board.conf 的 CONFIG_BSP_UART1_RX_USING_DMA），字节
# 要经 USART 的 dma-rx 请求线、DMA 通道、再由 IDLE 中断反推长度，才能到 shell。
# 这条链上任何一环断了，提示符照样打得出来，只是敲什么都没反应。
#
# 固件用 scons 构建：
#   cd <SDK>/example/get-started/hello_world/rtt/project
#   scons --board=sf32lb52-lcd_a128r16_hcpu -j8
echo "[9] 真实板子固件（sf32lb52-lcd_a128r16 hello_world）"
REAL_FW=${SIFLI_REAL_FW:-/mnt/e/code2/SiFli-SDK/example/get-started/hello_world/rtt/project/build_sf32lb52-lcd_a128r16_hcpu/main.elf}
if [ "$REAL_FW" = "-" ]; then
    skip "按要求跳过"
elif [ ! -f "$REAL_FW" ]; then
    skip "固件不存在，先用 scons 构建：$REAL_FW"
else
    # 不带 -semihosting：固件必须走真实的 HAL UART 驱动，走我们的 USART 模型，
    # 输出经 chardev 到 stdout。带了 -semihosting 就等于绕开了要验的东西。
    #
    # 用 -serial stdio -monitor none 而不是 -nographic：后者是 mon:stdio，
    # 输入要先过 monitor 的 mux，敲进去的字符走的是另一条路。这里要验的正是
    # "-serial" 那条。
    #
    # 固件跑到 shell 就停在那儿等输入，永远不会自己退出，所以只能看着输出把它
    # 杀掉。不这么做的话这一项要白等满 60 秒——实测 3 秒就到底了。
    fifo=$(mktemp -u)
    log=$(mktemp)
    mkfifo "$fifo"

    # FIFO 读写都开着（<>）：O_RDWR 打开 FIFO 不阻塞，而且只要这个 fd 还在，
    # 写端就没全关。全关掉的话 QEMU 会从 stdin 读到 EOF、把串口关掉，之后
    # 再往里写就没人接了。
    exec 3<>"$fifo"
    timeout 60 "$BUILD/qemu-system-arm" -M sf32lb52x \
        -display none -serial stdio -monitor none \
        -kernel "$REAL_FW" < "$fifo" > "$log" 2>&1 &
    qpid=$!

    # 600 × 0.1s，和上面那个 timeout 对齐
    for _ in $(seq 1 600); do
        grep -q 'msh />' "$log" 2>/dev/null && break
        kill -0 "$qpid" 2>/dev/null || break   # QEMU 自己退了（出错）就别等了
        sleep 0.1
    done

    # 提示符出来后再敲命令，然后等 shell 的回应 —— 而不只是等命令被回显。
    # "RT-Thread shell commands:" 是 msh_help() 的第一行（finsh/msh.c）。
    printf 'help\r' >&3
    for _ in $(seq 1 150); do
        grep -q 'RT-Thread shell commands:' "$log" 2>/dev/null && break
        kill -0 "$qpid" 2>/dev/null || break
        sleep 0.1
    done

    kill "$qpid" 2>/dev/null
    wait "$qpid" 2>/dev/null
    exec 3>&-
    rm -f "$fifo"

    # sed 去掉 RT-Thread 日志的 ANSI 颜色码（[32;22m...）
    out=$(sed 's/\x1b\[[0-9;]*m//g' "$log")
    rm -f "$log"

    missing=""
    echo "$out" | grep -q "Hello world"              || missing="$missing main()输出"
    echo "$out" | grep -q "msh />"                   || missing="$missing msh提示符"
    echo "$out" | grep -q "RT-Thread shell commands:" \
        || missing="$missing 敲 help 的回应（收通路没通）"
    if [ -z "$missing" ]; then
        pass "启动到 main()，msh 提示符接受输入并回 help 的命令表"
    else
        fail "没看到：$missing。最后几行："
        echo "$out" | tail -6 | sed 's/^/         /'
    fi
fi
echo

# ---------------------------------------------------- 10. 真实板子固件：EZIP
# 第 9 项证明固件能跑起来，这一项证明它**跑出了正确结果**。
#
# example/hal/ezip 自带测试向量和期望资产：main() 依次跑 EZIP 私有格式（轮询）、
# EZIP 私有格式（中断）、LZ4、GZIP，每一条都把解出来的缓冲和期望资产整个
# memcmp 一遍，对了才打 "[EZIP]Output is correct."。四句都在，说明三条解码
# 路径（宿主工具 / LZ4 / GZIP）和两种完成握手（轮询读 INT_MASK；中断走
# INT_EN → IRQ 89 → 信号量）都在模型上跑对了。
#
# 这不是第 7 项能替代的：qtest 是自己拼一段位流喂进去，固件这边是**原样的
# SDK 资产、原样的 HAL 调用序列**，而且私有格式那条真的去 spawn 了 ezip_linux。
#
# 私有格式那两条要把工具路径传给机器属性，所以工具不在就整条跳过 —— 那一半
# 验不了，剩下的不值得单独跑。
#
# 固件最后 while(1) 不退出，看着输出把它杀掉。
#
# 构建：
#   cd <SDK>/example/hal/ezip/project
#   scons --board=sf32lb52-lcd_a128r16_hcpu -j8
echo "[10] 真实板子固件（sf32lb52-lcd_a128r16 ezip）"
EZIP_FW=${SIFLI_EZIP_FW:-/mnt/e/code2/SiFli-SDK/example/hal/ezip/project/build_sf32lb52-lcd_a128r16_hcpu/main.elf}
EZIP_TOOL=${SIFLI_EZIP_TOOL:-/mnt/e/code2/SiFli-SDK/tools/png2ezip/ezip_linux}
if [ "$EZIP_FW" = "-" ]; then
    skip "按要求跳过"
elif [ ! -f "$EZIP_FW" ]; then
    skip "固件不存在，先用 scons 构建：$EZIP_FW"
elif [ ! -x "$EZIP_TOOL" ]; then
    skip "找不到宿主解码器 $EZIP_TOOL"
else
    fifo=$(mktemp -u)
    log=$(mktemp)
    mkfifo "$fifo"
    # 和第 9 项一样：让 stdin 那头一直是开着的，免得 chardev 在 EOF 上被摘掉。
    exec 3<>"$fifo"
    timeout 120 "$BUILD/qemu-system-arm" -M "sf32lb52x,ezip-tool=$EZIP_TOOL" \
        -display none -serial stdio -monitor none \
        -kernel "$EZIP_FW" < "$fifo" > "$log" 2>&1 &
    qpid=$!

    for _ in $(seq 1 1200); do
        [ "$(grep -c 'Output is correct' "$log" 2>/dev/null)" -ge 4 ] && break
        grep -q 'Output is incorrect' "$log" 2>/dev/null && break
        kill -0 "$qpid" 2>/dev/null || break
        sleep 0.1
    done

    kill "$qpid" 2>/dev/null
    wait "$qpid" 2>/dev/null
    exec 3>&-
    rm -f "$fifo"

    out=$(sed 's/\x1b\[[0-9;]*m//g' "$log")
    rm -f "$log"

    ok=$(echo "$out" | grep -c '\[EZIP\]Output is correct\.')
    bad=$(echo "$out" | grep -c '\[EZIP\]Output is incorrect\.')
    if [ "$ok" -eq 4 ] && [ "$bad" -eq 0 ]; then
        pass "私有格式/私有格式中断/LZ4/GZIP 四条解出来都与资产逐字节一致"
    else
        fail "通过 $ok/4 条，报错 $bad 条。最后几行："
        echo "$out" | tail -6 | sed 's/^/         /'
    fi
fi
echo

# ---------------------------------------------------- 11. 真实板子固件：EPIC
# 第 9 项证明固件能跑起来，第 10 项证明解压的解对了，这一项证明**画出来的像素
# 对了** —— 而且是 SDK 原样的例程、原样的 EPIC HAL 调用序列。
#
# example/hal/epic 的两级 alpha 混合（前景蓝 150x100 @(50,50)，背景红
# 150x100 @(100,100)，都 alpha=128，输出 250x200 到 390 像素宽的全屏 buffer），
# 三个 buffer 都在 PSRAM 里（L2_NON_RET_BSS_SECT → .RW_PSRAM_NON_RET）。
# 所以这一项同时验了：EPIC 的填充、图层定位、alpha 混合，以及 PSRAM 和
# EPIC 之间的那条通路（AHB_MEM 走 CPU 地址，图层 SRC 走 SBUS 别名）。
#
# 例程自己不查像素，只查 HAL 的返回值，所以这里把输出 buffer 读回来自己比。
# 采样点按区域挑：画布（黑）、只有前景（50% 的蓝）、只有背景（50% 的红）、
# 前景和背景重叠处（红压在蓝上，0x8008）。混出来的具体数值取决于模型怎么
# 取整，这几处不一样就说明图层位置或混合顺序错了。
#
# 固件最后 while(1) 不退出：等它打完 "EPIC blend succeeded"，从 monitor 把
# 机器停下来读内存，再 quit。
#
# 构建：
#   cd <SDK>/example/hal/epic/project
#   scons --board=sf32lb52-lcd_a128r16_hcpu -j8
echo "[11] 真实板子固件（sf32lb52-lcd_a128r16 epic）"
EPIC_FW=${SIFLI_EPIC_FW:-/mnt/e/code2/SiFli-SDK/example/hal/epic/project/build_sf32lb52-lcd_a128r16_hcpu/main.elf}
ARM_NM=${SIFLI_ARM_NM:-arm-none-eabi-nm}
if [ "$EPIC_FW" = "-" ]; then
    skip "按要求跳过"
elif [ ! -f "$EPIC_FW" ]; then
    skip "固件不存在，先用 scons 构建：$EPIC_FW"
elif ! command -v "$ARM_NM" >/dev/null 2>&1; then
    skip "找不到 $ARM_NM —— 输出 buffer 的地址要从固件符号表里读"
else
    # 输出 buffer 的地址是链接期定的（__PSRAM_BASE），所以从符号表读，别写死。
    # 行宽来自板子的 LCD_HOR_RES_MAX=390（例程打印的 "LCD Info: Width=390"
    # 也是它），RGB565 所以一行 780 字节。
    buf2=$("$ARM_NM" "$EPIC_FW" 2>/dev/null | awk '$3 == "buffer2" {print $1}')
    ROW=780
    if [ -z "$buf2" ]; then
        fail "读不到 buffer2 的地址"
    else
        ser=$(mktemp)
        mon=$(mktemp)
        monout=$(mktemp)
        fifo=$(mktemp -u)
        mkfifo "$fifo"

        exec 3<>"$fifo"
        timeout 120 "$BUILD/qemu-system-arm" -M sf32lb52x \
            -display none -serial "file:$ser" -monitor stdio \
            -kernel "$EPIC_FW" < "$fifo" > "$monout" 2>&1 &
        qpid=$!

        for _ in $(seq 1 900); do
            grep -q 'EPIC blend succeeded' "$ser" 2>/dev/null && break
            grep -q 'EPIC blend failed' "$ser" 2>/dev/null && break
            kill -0 "$qpid" 2>/dev/null || break
            sleep 0.1
        done

        # 停住机器再读内存。用 /1wx 按字读，所以采样点的 x 取偶数，一个字里
        # 两个像素都落在同一个区域，期望值就是同一个值写两遍。
        {
            printf 'stop\n'
            while read -r x y want what; do
                [ -n "$x" ] || continue
                printf 'xp /1wx 0x%08x\n' \
                       $(( 0x$buf2 + y * ROW + x * 2 ))
            done <<'EOF'
10  10  0x00000000 画布（左上角）
260 60  0x00000000 画布（右边）
74  74  0x00100010 只有前景 —— alpha 128 的蓝
198 74  0x00100010 只有前景（右边缘）
240 124 0x80008000 只有背景 —— alpha 128 的红
148 196 0x80008000 只有背景（下边缘）
150 124 0x80088008 前景与背景重叠处
EOF
            printf 'quit\n'
        } >&3

        wait "$qpid" 2>/dev/null
        exec 3>&-
        rm -f "$fifo"

        got=$(sed 's/\x1b\[[0-9;]*m//g' "$monout" \
                  | grep -o '^00000000[0-9a-f]*: 0x[0-9a-f]*' | awk '{print $2}')
        rm -f "$monout"

        bad=""
        exec 5<<<"$got"
        while read -r x y want what; do
            [ -n "$x" ] || continue
            read -r have <&5
            [ "$have" = "$want" ] || bad="$bad; $what 期望 $want 实际 ${have:-<读不到>}"
        done <<'EOF'
10  10  0x00000000 画布（左上角）
260 60  0x00000000 画布（右边）
74  74  0x00100010 只有前景 —— alpha 128 的蓝
198 74  0x00100010 只有前景（右边缘）
240 124 0x80008000 只有背景 —— alpha 128 的红
148 196 0x80008000 只有背景（下边缘）
150 124 0x80088008 前景与背景重叠处
EOF
        exec 5<&-

        if [ -z "$bad" ]; then
            pass "填充/图层定位/alpha 混合的 7 个采样点像素都对"
        else
            fail "混合结果不对：$bad。串口最后几行："
            sed 's/\x1b\[[0-9;]*m//g' "$ser" | tail -4 | sed 's/^/         /'
        fi
        rm -f "$ser"
    fi
fi
echo

# ------------------------------------------------------------------- 结果
if [ "$FAILED" -eq 0 ]; then
    echo "全部通过。"
    exit 0
else
    echo "有 $FAILED 项失败。"
    exit 1
fi
