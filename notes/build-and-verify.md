# sifli-qemu：构建与验证

这份文档记录本仓库（QEMU 10.2.4 的 SiFli fork）怎么构建、怎么验证，
以及环境里那些不看就会踩的坑。

**所有操作都在 WSL 里做**——Windows 侧没有 gcc/meson，编不了。

`notes/` 下的东西：

| 文件 | 用途 |
|---|---|
| `build-sifli.sh` | 构建（`notes/build-and-verify.md` 就是本文） |
| `verify-sifli.sh` | 一条命令跑完八项检查，退出码可直接进 CI |
| `qtest-sifli.sh` | 单独验外设写路径，被 verify 的第 8 项调用 |
| `peripherals.md` | 外设模型是怎么设计的、怎么加新的 |

---

## 0. 两个环境前提

**① 从 Git Bash 调 wsl 时必须加 `MSYS_NO_PATHCONV=1`**

否则 MSYS 会把 `/home/...` 当成 Windows 路径转换成 `C:/Program Files/Git/home/...`：

```bash
# 错
wsl.exe -d ubuntu-24.04 -- bash /home/x/y.sh
#   bash: C:/Program Files/Git/home/x/y.sh: No such file or directory

# 对
MSYS_NO_PATHCONV=1 wsl.exe -d ubuntu-24.04 -- bash /home/x/y.sh
```

**② meson / ninja 装在 venv 里，系统没有**

```
$HOME/.venvs/qemu/bin/     # meson 1.12 + ninja
```

原因：Ubuntu 24.04 自带的 pip 被 PEP 668 挡住，而 `apt install` 需要 sudo 密码。
venv 是免 sudo 的绕法。构建脚本里已经 `export PATH` 了。

---

## 1. 构建

```bash
bash notes/build-sifli.sh
```

脚本做的事：
1. 补齐 wrap 子项目（见 §4 坑 ③）
2. `configure --target-list=arm-softmmu,aarch64-softmmu ...`（已配置过会跳过）
3. `ninja -j6`（见 §4 坑 ⑤）

产物在 `~/build-sifli/`：
```
qemu-system-arm          ~95 MB
qemu-system-aarch64     ~116 MB
```

**为什么两个 target 都编**：要验证 machine 在两个二进制里都可见（见 §2 第 1 项）。

---

## 2. 验证

### ① machine 可见性 —— **两个二进制都要查**

```bash
cd ~/build-sifli
./qemu-system-arm     -M help | grep sf32lb52x
./qemu-system-aarch64 -M help | grep sf32lb52x
```

两边都必须输出：

```
sf32lb52x            SiFli SF32LB52x (Cortex-M33)
```

**只出现一边、或都不出现 = TypeInfo 的 `.interfaces` 写错了。**

原因：`-M help` 和 `-M <name>` 的查找都走接口过滤（`qom/object.c:1117`）：

```c
if (data->implements_type &&
    !object_class_dynamic_cast(k, data->implements_type)) {
    return;                     /* 不实现该 interface 的类被跳过 */
}
```

调用方 `object_class_get_list(target_machine_typename(), false)`
（`system/vl.c:1570, 1680`）。

| 定义方式 | 实现的 interface | `-M help` 里 |
|---|---|---|
| `DEFINE_MACHINE_ARM(...)` | `arm_machine_interfaces`（arm + aarch64 两个） | 两个二进制都有 |
| 手写 TypeInfo 带 `.interfaces = arm_machine_interfaces` | 同上 | 同上 |
| **裸 `DEFINE_MACHINE(...)`** | **无** | **两边都没有（静默失效）** |

最后一行的真实事故：上游 `max78000fthr` 曾经因为漏掉接口**从 `-M help` 里消失**，
编译不报错、`-M` 也选不到，后来靠 `d9dd5dad31` 补回。

**为什么 aarch64 也能跑 Cortex-M**：两个二进制编译自同一份 `target/arm/`，
`cortex-m33` 就在 `target/arm/tcg/cpu-v7m.c`，区别只是 `TARGET_AARCH64` /
`TARGET_LONG_BITS` 宏。实测同一份固件在 aarch64 上输出完全一致。

### ② 功能回归 —— 跑真实固件

```bash
cd ~/build-sifli
timeout 10 ./qemu-system-arm -M sf32lb52x -nographic \
  -semihosting -semihosting-config enable=on,target=native \
  -kernel /mnt/e/code2/qemu-support/example/qemu/hello_qemu/project/build_qemu_cortex_m33_hcpu/main.elf
```

期望输出：

```
 \ | /
- SiFli Corporation
 / | \     build on Aug  2 2026, 0.0.0 build "Unknown"
 2020 - 2022 Copyright by SiFli team
Hello SiFli on QEMU!
```

把 `qemu-system-arm` 换成 `qemu-system-aarch64` 应该完全一样。

**注意**：这个输出走的是 **ARM semihosting**，不是 UART——固件通过 `BKPT 0xAB`
调用宿主服务（`SYS_WRITEC`/`SYS_WRITE0`），QEMU 在 `semihosting/arm-compat-semi.c` 里实现。
**不加 `-semihosting` 会 fault**。所以「能打印」只证明内存/CPU/NVIC/SysTick/启动流程对了，
**一个字的外设都没验证**。

### ③ 代码格式

```bash
cd ~/code/sifli-qemu
perl scripts/checkpatch.pl --no-tree --file hw/arm/sifli-sf32lb52x.c
perl scripts/checkpatch.pl --no-tree --file include/hw/arm/sf32lb52x.h
```

期望各自以这句收尾：

```
... has no obvious style problems and is ready for submission.
```

`--no-tree` 必须加——这棵树不是 kernel tree，不加会被 `top_of_kernel_tree` 挡下。

**最常见的两条**：
- `ERROR: New file '...' requires 'SPDX-License-Identifier'` —— 新 `.c`/`.h` 必须有 SPDX 头
- `WARNING: Block comments use a leading /* on a separate line` —— QEMU 要求多行块注释的
  `/*` 和 `*/` **各占一行**（不能写成 `/* 正文… */`）

### ④ 属性生效

```bash
# 默认 0x10000000；显式给 QSPI2 的基址会 fault（固件按 0x10000000 链接，取不到向量表）
./qemu-system-arm -M sf32lb52x,flash-base=0x12000000 -nographic -kernel <fw.elf>
./qemu-system-arm -M sf32lb52x,flash-size=0x800000 -nographic -kernel <fw.elf>
```

### ⑤ 提交完整性

```bash
cd ~/code/sifli-qemu
git ls-files --eol hw/arm/Kconfig hw/arm/meson.build   # 期望 i/lf w/lf
git diff --stat                                        # 应只剩自己的改动
git log -1 --format="%h  %an <%ae>"
git log -1 --format=%B | grep -E "^(Signed-off-by|Co-Authored-By)"
```

`Signed-off-by:` 的邮箱**必须和 author 一致**，否则 CI 的 `check-dco` 直接失败。

---

## 3. 一条命令跑完

上面每一项都是 `notes/verify-sifli.sh` 里的一个检查项：

```bash
bash notes/verify-sifli.sh            # 固件默认用 qemu-support 的 hello_qemu
bash notes/verify-sifli.sh fw.elf     # 换个固件
bash notes/verify-sifli.sh -          # 跳过固件那一项
```

九项检查，全部通过退出码 0，可直接进 CI：

| # | 检查 | 性质 |
|---|---|---|
| 1 | 两个二进制都编出来了 | |
| 2 | 两个二进制里 `-M help` 都能看到 sf32lb52x | |
| 3 | checkpatch 0 errors 0 warnings | |
| 4 | 工作区行尾是 LF（不是 CRLF） | |
| 5 | `hello_qemu` 能跑出 banner | 冒烟（走 semihosting，不碰 HAL）|
| 6 | 提交带 Signed-off-by 且与 author 一致 | |
| 7 | 外设区域都映射了，寄存器读回值正确 | 结构性 |
| 8 | 外设写路径（qtest） | 结构性 |
| **9** | **真实板子固件跑到 `main()` 和 msh 提示符** | **真正的验收** |

**第 9 项才是关键。** 第 5 项的 `hello_qemu` 走 semihosting 桩，一个 HAL 寄存器
都不碰；第 7、8 项只是读几个寄存器。只有 SDK 原样构建的真实板子固件、一行不改
地跑到 `main()`，才能证明整个 HAL 真的在模型上跑起来了——时钟树、电源、RTC、
MPI、音频、控制台，整条链路。详见 `peripherals.md`。

不跑脚本时手工至少查这几条：

```
□ 两个二进制的 -M help 都能看到 sf32lb52x
□ 固件能跑出 "Hello SiFli on QEMU!"
□ checkpatch 0 errors 0 warnings
□ git ls-files --eol 显示 w/lf（不是 w/crlf）
□ git diff --stat 只有自己的改动
□ 提交带 Signed-off-by 且与 author 一致
```

---

## 4. 环境坑（踩过的）

### ① 仓库里的 CRLF

Windows 上以 `core.autocrlf=true` checkout 时，shell 脚本/Makefile 全是 CRLF，
Linux 下 `configure` 的 shebang 变成 `#!/bin/sh\r`，内核找不到解释器：

```
configure: cannot execute: required file not found
```

QEMU 的 `.gitattributes` 只标了 `*.patch`，**没保护脚本**。

→ 所以本仓库只在 WSL 这份 Linux checkout 上工作。Windows 那份
`E:\code2\sifli-qemu` 编不了（`pebble-qemu` 同理）。

**改文件时注意**：用工具编辑后要确认没被写成 CRLF：

```bash
git ls-files --eol <file>      # 期望 w/lf
# 万一变成 crlf：
sed -i 's/\r$//' <file>
```

否则 `git diff` 会把整个文件显示成改动。

### ② 符号链接被展平

`core.symlinks=false` 时 git 把符号链接写成「内容是目标路径的普通文件」，
本仓库有 14 个（`rust/*/build.rs`、`subprojects/libv*-user/standard-headers/linux` 等）。
构建时会报找不到 `standard-headers/linux/virtio_ring.h`。

```bash
# 列出全部
git ls-files -s | awk '$1=="120000" {print $4}'
```

WSL 这份是正常 checkout，没这个问题。

### ③ wrap 子项目要手动准备

meson 自己下载 wrap 在这个环境不可靠，留下不完整目录后会一直报
`Subproject exists but has no meson.build file`。要按 `.wrap` 钉的 revision 手动
`git clone`，**并且把 `subprojects/packagefiles/<name>/` 覆盖进去**——
`berkeley-softfloat-3` 的 `meson.build` 就是这么来的。

构建脚本里已经处理了。

### ④ 缺 `bzip2`

QEMU 的 `meson.build:177` 要求它（只为解包 EDK2 UEFI blob）。
用 `--disable-install-blobs` 绕开。

### ⑤ 内存

WSL 只分到 ~11 GiB，而 QEMU 默认按核数（12）并行编译，每路 `cc1` 几百 MB，
会被 OOM reaper 杀掉（表现为 `rsync`/`ninja` 收到 SIGTERM）。

→ 用 `ninja -j6`。想彻底解决就在 `C:\Users\rabbi\.wslconfig` 里加：

```ini
[wsl2]
memory=16GB
```

### ⑥ `subprojects/libblkio/` 一直显示 untracked

上游 `subprojects/.gitignore` 列了所有 wrap 子项目，**唯独漏了 `libblkio`**。
不影响构建（我们没开 libblkio），是纯噪音。

---

## 5. 排查技巧

**看固件访问了哪些没实现的地址**：临时打开 `hw/arm/sifli-sf32lb52x.c` 里的
`create_unimplemented_device()`（需 `#include "hw/misc/unimp.h"`）。

**拿调用栈**：WSL 里没有 gdb；临时插 `fprintf(stderr, ...)` 打地址是最快的办法
（上次就是靠打出的指针地址，发现崩溃对象恰好是 machine 自己，从而定位到
`memory_region_init_rom()` 会对 owner 做 `DEVICE()`）。

**不开固件就读写外设寄存器**：用 qtest。它的 `writel`/`readl` 走
`address_space_write/read`，和外设 MMIO 是同一条路，而且在机器建好、复位完成
之后：

```bash
printf 'readl 0x5000001c\nwritel 0x50000010 0x10\nreadl 0x50000008\nquit\n' \
  | ./qemu-system-arm -M sf32lb52x -display none -serial none -qtest stdio 2>/dev/null
```

返回值是 64 位十六进制。**别合并 stderr**——那里有 `[R ...]`/`[S ...]` 的 trace，
会打乱取值顺序。

`-device loader,addr=...,data=...,data-len=4` 也能写物理地址，但它在设备 reset
**之前**执行，写进寄存器会被复位冲掉（写 SRAM 不受影响）。所以测寄存器要用 qtest。

**确认接口过滤逻辑**：
```bash
sed -n '1113,1122p' qom/object.c
```

外设模型的详细设计（薄语义、就绪位伪造、表驱动、加新外设的步骤）见
`peripherals.md`。
