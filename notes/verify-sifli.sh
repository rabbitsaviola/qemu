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
hw/misc/sifli-regbank.c
include/hw/misc/sifli-regbank.h"

if [ -f "$SRC/scripts/checkpatch.pl" ]; then
    for f in $NEW_FILES; do
        [ -f "$SRC/$f" ] || { skip "$f 不存在"; continue; }
        out=$(cd "$SRC" && perl scripts/checkpatch.pl --no-tree --file "$f" 2>&1)
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
if [ -d "$SRC/.git" ]; then
    bad=$(cd "$SRC" && git ls-files --eol \
              $NEW_FILES \
              hw/arm/Kconfig hw/arm/meson.build \
              hw/char/Kconfig hw/char/meson.build \
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
if [ -d "$SRC/.git" ]; then
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
xp /1wx 0x40040040
xp /1wx 0x50000020
xp /1wx 0x5000002c
xp /1wx 0x5000b004
xp /1wx 0x5008401c
xp /1wx 0x500c0010
xp /1wx 0x500c002c
xp /1wx 0x500ca01c
xp /1wx 0x500ca020
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

# ------------------------------------------------------- 8. 真实板子固件
# 这一项才是真正的验收。
#
# 第 6、7 项只是结构性地读几个寄存器，模型写错了它们照样能过
# （peripherals.md §6.2 那个 DWT 映射错位的坑就骗过了它们全部）。只有这里
# ——SDK 原样构建的真实板子固件、一行不改——能证明整个 HAL 真的在模型上跑
# 起来了：时钟树、电源、RTC、MPI、音频、控制台，一路到 main() 和 RT-Thread
# 的 shell。
#
# 固件用 scons 构建：
#   cd <SDK>/example/get-started/hello_world/rtt/project
#   scons --board=sf32lb52-lcd_a128r16_hcpu -j8
echo "[8] 真实板子固件（sf32lb52-lcd_a128r16 hello_world）"
REAL_FW=${SIFLI_REAL_FW:-/mnt/e/code2/SiFli-SDK/example/get-started/hello_world/rtt/project/build_sf32lb52-lcd_a128r16_hcpu/main.elf}
if [ "$REAL_FW" = "-" ]; then
    skip "按要求跳过"
elif [ ! -f "$REAL_FW" ]; then
    skip "固件不存在，先用 scons 构建：$REAL_FW"
else
    # 不带 -semihosting：固件必须走真实的 HAL UART 驱动，走我们的 USART 模型，
    # 输出经 chardev 到 stdout。带了 -semihosting 就等于绕开了要验的东西。
    # sed 去掉 RT-Thread 日志的 ANSI 颜色码（[32;22m...）。
    out=$(timeout 60 "$BUILD/qemu-system-arm" -M sf32lb52x -nographic \
              -kernel "$REAL_FW" 2>&1 | sed 's/\x1b\[[0-9;]*m//g')

    missing=""
    echo "$out" | grep -q "Hello world" || missing="$missing main()输出"
    echo "$out" | grep -q "msh />"      || missing="$missing msh提示符"
    if [ -z "$missing" ]; then
        pass "启动到 main()，停在 RT-Thread msh 提示符"
    else
        fail "没看到：$missing。最后几行："
        echo "$out" | tail -6 | sed 's/^/         /'
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
