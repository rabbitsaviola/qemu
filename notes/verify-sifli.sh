#!/bin/bash
#
# sifli-qemu 验证脚本。检查项与说明见同目录的 build-and-verify.md。
#
# 用法：
#   bash notes/verify-sifli.sh [固件.elf]
#
# 固件默认用 qemu-support 里的 hello_qemu；传 "-" 可跳过固件这一项。
# 全部通过退出码为 0，有失败为 1（可直接用于 CI）。
#
# 环境变量可覆盖：
#   SIFLI_QEMU_SRC    源码树   （默认 ~/code/sifli-qemu）
#   SIFLI_QEMU_BUILD  构建目录 （默认 ~/build-sifli）

set -u

SRC=${SIFLI_QEMU_SRC:-$HOME/code/sifli-qemu}
BUILD=${SIFLI_QEMU_BUILD:-$HOME/build-sifli}
FW=${1:-/mnt/e/code2/qemu-support/example/qemu/hello_qemu/project/build_qemu_cortex_m33_hcpu/main.elf}

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
if [ -f "$SRC/scripts/checkpatch.pl" ]; then
    for f in hw/arm/sifli-sf32lb52x.c include/hw/arm/sf32lb52x.h; do
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
              hw/arm/sifli-sf32lb52x.c include/hw/arm/sf32lb52x.h \
              hw/arm/Kconfig hw/arm/meson.build 2>/dev/null \
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

# ------------------------------------------------------------ 5. 固件功能
echo "[5] 固件功能回归"
if [ "$FW" = "-" ]; then
    skip "按要求跳过"
elif [ ! -f "$FW" ]; then
    fail "固件不存在：$FW"
else
    out=$(timeout 15 "$BUILD/qemu-system-arm" -M sf32lb52x -nographic \
              -semihosting -semihosting-config enable=on,target=native \
              -kernel "$FW" 2>&1 | grep -v '^QEMU [0-9]\|^(qemu)\|terminating')
    if echo "$out" | grep -q "Hello SiFli on QEMU"; then
        pass "固件启动并打印 banner"
    else
        fail "固件没打出预期内容，实际输出："
        echo "$out" | head -8 | sed 's/^/         /'
    fi
fi
echo

# ----------------------------------------------------------- 6. 提交完整性
echo "[6] 提交完整性"
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

# ------------------------------------------------------------------- 结果
if [ "$FAILED" -eq 0 ]; then
    echo "全部通过。"
    exit 0
else
    echo "有 $FAILED 项失败。"
    exit 1
fi
