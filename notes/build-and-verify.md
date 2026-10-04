# sifli-qemu：构建与验证

这份文档记录本仓库（QEMU 10.2.4 的 SiFli fork）怎么构建、怎么验证，
以及环境里那些不看就会踩的坑。

**所有操作都在 WSL 里做**——Windows 侧没有 gcc/meson，编不了。

脚本都在 `notes/` 下（不在 $HOME）：**本地开发**用前四个，**分发产物**用后两个。

| 文件 | 用途 |
|---|---|
| `build-sifli.sh` | 本地构建（configure 过就跳过，日常只跑 ninja） |
| `verify-sifli.sh` | 一条命令跑完九项检查，退出码可直接进 CI |
| `qtest-sifli.sh` | 单独验外设写路径，被 verify 的第 8 项调用 |
| `peripherals.md` | 外设模型是怎么设计的、怎么加新的 |
| `build-and-verify.md` | 本文 |
| `../build-dist.sh` | 构建**可分发的产物**（三平台），见 §6 |
| `../build-static-deps.sh` | macOS 的静态依赖，被 build-dist.sh 调用 |

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
bash notes/verify-sifli.sh            # 默认固件
bash notes/verify-sifli.sh fw.elf     # 换第 5 项那个冒烟固件
bash notes/verify-sifli.sh -          # 跳过第 5 项
```

两个固件是两个独立的东西：第 5 项的冒烟固件走 semihosting 桩，第 9 项的真实板子
固件走完整 HAL。第 9 项那个用环境变量指定：

```bash
SIFLI_REAL_FW=/path/to/main.elf bash notes/verify-sifli.sh
# 或跳过它（也是 "-"）
SIFLI_REAL_FW=- bash notes/verify-sifli.sh
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

---

## 6. 分发的产物（CI）

`.github/workflows/build.yml` 在 GitHub Actions 上为三个平台构建
`qemu-system-arm`，每个平台一个可下载的产物。
`build-dist.sh` 负责构建和打包，`build-static-deps.sh` 提供 macOS 的静态库。

产物里**只有 32 位的 `qemu-system-arm`**。SF32LB52x 是 Cortex-M33（ARMv8-M），
aarch64 那个二进制对这个项目没有用处——虽然它也能跑这块板子，但为主力用途编
一份用不上的二进制，只是让构建时间和下载体积都翻倍。

### 6.1 运行要求

**这是产物真正决定"能在哪儿跑"的东西**，和它打没打包库是两回事：

| 平台 | 要求 | 由什么决定 |
|---|---|---|
| Linux | **glibc ≥ 2.35**<br>（Ubuntu 22.04+ / Debian 12+ / RHEL 9+）<br>另需 `libglib2.0-0`、`libpixman-1-0`、`libpng16-16`、`zlib1g` | CI 用 `ubuntu-22.04` runner——产物链的是 runner 的 glibc |
| macOS | **macOS ≥ 11**，**仅 arm64** | `MACOSX_DEPLOYMENT_TARGET=11.0`；架构取决于 runner |
| Windows | Windows 10+，x86_64 | MSYS2 mingw64 自身的下限 |

**Linux 的下限跟着构建环境走，不是产物的属性。** CI 上编出来是 2.35；而在
WSL（Ubuntu 24.04）用 `notes/build-sifli.sh` 本地编出来是 **glibc ≥ 2.38**，
只能跑在 Ubuntu 23.10+ 上。要更宽的兼容性就用更老的构建环境——这正是 CI 选
`ubuntu-22.04` 而不是 `-latest` 的唯一理由。

**macOS 目前只有 arm64。** `macos-latest` 是 Apple silicon，产物在 Intel Mac 上
跑不了（Rosetta 只能反方向：arm64 二进制不能在 Intel 上跑，反之可以）。要覆盖
Intel 得再加一个 x86_64 的 job，而 GitHub 现在只有付费的 larger runner
（`macos-latest-large`）提供 x86_64。

### 6.2 三个平台为什么打包方式不同

| 平台 | 打什么 | 为什么 |
|---|---|---|
| Linux | **不打** | 见 §6.3 |
| macOS | 静态链接，不打 dylib | 否则二进制里是 `/opt/homebrew/opt/glib/lib/...` 这种绝对路径，机器上没有那个 Homebrew 就跑不起来 |
| Windows | 打 MSYS2 的 DLL | 原装 Windows 上没有包管理器能拿到它们 |

### 6.3 Linux 为什么不打库

和 `pebble-qemu` 一致。把 `.so` 打进包看着更"自包含"，但：

1. **它拿不掉 glibc 下限。** glibc 永远不能打进包，下限照样由构建环境决定。
   产物会显得比实际更能跑——这是最坑的一点。
2. **库版本被冻结，脱离发行版的安全更新。** `pcre2`、`zlib` 都有过 CVE。
   系统包管理器升级了跟你无关，跑的是包里那份。

代价只是要装两个包（`libglib2.0-0`、`libpixman-1-0`），几乎所有桌面发行版都有。

构建脚本会把**依赖清单和 glibc 下限**都打出来——两者都是决定性的，而产物里
都看不出来。清单是从二进制的 `NEEDED` 读的，不是手写的（手写的会漂）：

```
  no libraries bundled; the distribution provides these:
    libglib-2.0.so.0 libpixman-1.so.0 libpng16.so.16 libz.so.1
  and it needs GLIBC_2.35 or newer: the build host's version
  decides that, and nothing in the artifact changes it.
```

（`libbz2`、`libzstd` 本来也在列表里，configure 里补了
`--disable-bzip2 --disable-zstd --disable-lzo --disable-snappy` 之后没了。）

### 6.4 一条容易上当的检查

**"产物能跑起来"证明不了它打包正确。**

构建机上装着同样的库，所以一个**完全没打成功**的包照样能跑——它只是去用系统
那份了。"挪到别处跑一下"也挡不住，因为别处（同一台机器）也有。

要验的是**引用**，不是**行为**：

| 平台 | 检查 | 在哪 |
|---|---|---|
| macOS | `otool -L` 列出的路径里有没有非系统库 | `build-dist.sh` |
| Windows | 每个从 `/mingw64` 解析的 DLL 在不在 exe 旁边 | `build-dist.sh` |

两条不满足都直接 `exit 1`。

（Linux 早先也打过库，当时同样需要这个检查——而且**正是它抓到了真 bug**：我按
真实文件名拷贝，加载器按 SONAME 找，`libglib-2.0.so.0.8000.0` 和
`libglib-2.0.so.0` 对不上，等于没打。当时"能跑"是假象。）
