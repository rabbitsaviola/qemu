#!/bin/bash
# 构建 sifli-qemu。可重复执行。
#
# 只在 WSL 的 Linux checkout 上工作 —— 它有 LF 和真符号链接，所以不需要
# crlf / 符号链接的转换步骤（Windows 那份树编不了，原因见 build-and-verify.md）。
#
# 用法：
#   bash notes/build-sifli.sh
#
# 环境变量可覆盖：
#   SIFLI_QEMU_SRC    默认取本脚本所在目录的上一级
#   SIFLI_QEMU_BUILD  默认 $HOME/build-sifli
set -e

export EMSDK_QUIET=1
export PATH="$HOME/.venvs/qemu/bin:$PATH"

SRC=${SIFLI_QEMU_SRC:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
BUILD=${SIFLI_QEMU_BUILD:-$HOME/build-sifli}

echo "=== 源码: $SRC"
echo "=== 构建: $BUILD"

echo "=== 1. 准备 wrap 子项目 ==="
# meson 自己下载 wrap 在这个环境里不可靠（留下不完整目录后会一直报
# "Subproject exists but has no meson.build file"），所以按 .wrap 钉的
# revision 手动 clone，并覆盖 packagefiles（berkeley-softfloat-3 的
# meson.build 就是这么来的）。
cd "$SRC"
for wrap in "$SRC"/subprojects/*.wrap; do
    name=$(basename "$wrap" .wrap)
    url=$(grep -m1 '^url' "$wrap" 2>/dev/null | tr -d '\r' \
          | sed 's/.*=[[:space:]]*//; s/[[:space:]]*$//')
    rev=$(grep -m1 '^revision' "$wrap" 2>/dev/null | tr -d '\r' \
          | sed 's/.*=[[:space:]]*//; s/[[:space:]]*$//')
    [ -n "$url" ] || continue

    dest="$SRC/subprojects/$name"
    if [ -f "$dest/meson.build" ] || [ -f "$dest/setup.py" ]; then
        continue
    fi
    rm -rf "$dest"
    if ! git clone --quiet "$url" "$dest" 2>/dev/null; then
        echo "  $name: clone 失败，跳过"
        continue
    fi
    [ -n "$rev" ] && git -C "$dest" checkout --quiet "$rev" 2>/dev/null || true
    pf="$SRC/subprojects/packagefiles/$name"
    if [ -d "$pf" ]; then
        cp -a "$pf"/. "$dest"/
        echo "  $name @ ${rev:0:12} (+packagefiles)"
    else
        echo "  $name @ ${rev:0:12}"
    fi
done

# 已经配置过就不重来 —— configure 很慢，日常改代码只需要 ninja。
# 要彻底重来就 rm -rf "$BUILD"。
if [ -f "$BUILD/build.ninja" ]; then
    echo "=== 2. configure 已存在，跳过 ==="
else
    echo "=== 2. configure ==="
    mkdir -p "$BUILD"
    cd "$BUILD"
    # aarch64-softmmu 也要编：machine 在 arm 和 aarch64 两个二进制里都得出现
    "$SRC/configure" \
      --target-list=arm-softmmu,aarch64-softmmu \
      --disable-docs \
      --disable-tools \
      --disable-guest-agent \
      --disable-werror \
      --disable-gtk \
      --disable-sdl \
      --disable-vnc \
      --disable-curses \
      --disable-opengl \
      --disable-libssh \
      --disable-tpm \
      --disable-install-blobs
fi

echo "=== 3. ninja ==="
cd "$BUILD"
# WSL 只分到 ~11GiB，默认按 12 核并行会被 OOM reaper 杀掉，限到 6 路
ninja -j6 qemu-system-arm qemu-system-aarch64

echo "=== 构建完成 ==="
ls -la qemu-system-arm qemu-system-aarch64
