# sifli-qemu：构建与验证

这份文档记录本仓库（QEMU 10.2.4 的 SiFli fork）怎么构建、怎么验证，
以及环境里那些不看就会踩的坑。

**所有操作都在 WSL 里做**——Windows 侧没有 gcc/meson，编不了。

脚本都在 `notes/` 下（不在 $HOME）：**本地开发**用前四个，**分发产物**用后两个。

| 文件 | 用途 |
|---|---|
| `build-sifli.sh` | 本地构建（configure 过就跳过，日常只跑 ninja） |
| `verify-sifli.sh` | 一条命令跑完十一项检查，退出码可直接进 CI |
| `qtest-sifli.sh` | 单独验外设写路径，被 verify 的第 7 项调用 |
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

### ② 功能回归 —— 跑真实板子固件

这是唯一一项能证伪的检查：SDK 原样 scons 构建、**固件一行不改**，跑真实板子。

```bash
cd ~/build-sifli
timeout 60 ./qemu-system-arm -M sf32lb52x -nographic \
  -kernel /mnt/e/code2/SiFli-SDK/example/get-started/hello_world/rtt/project/build_sf32lb52-lcd_a128r16_hcpu/main.elf
```

期望最后两行：

```
Hello world!
msh />
```

固件**停在 shell 上等输入、永远不会自己退出**，所以脚本是边看输出边把它杀掉
——看到 `msh />` 就结束，实测 **2.6 秒**。（60 秒是上限，只有跑不到 shell 才会
等满，那正是失败的情形。）

**不带 `-semihosting`，这是重点。** 固件的 `rt_kprintf` 走完整链路：

```
rt_kprintf → drv_usart.c 的 sifli_putc → 写 USART1->TDR
           → sifli-usart 模型 → chardev → stdout
```

链路上任何一环断了，一行字都出不来。而**带上 `-semihosting` 就等于绕开了要验的
东西**——那会让固件用 `BKPT 0xAB` 把字符直接交给宿主（`semihosting/arm-compat-semi.c`），
一个 HAL 寄存器都不碰。

> 早先这里跑的是 `qemu-support` 的 `hello_qemu`（`qemu_cortex_m33` 桩板），
> 它走 semihosting，只证明内存/CPU/NVIC/SysTick 对了。真实 HAL 能跑之后那一项
> 就被这项完全覆盖，删掉了。

固件构建：

```bash
cd <SDK>/example/get-started/hello_world/rtt/project
scons --board=sf32lb52-lcd_a128r16_hcpu -j8
```

**注意两棵树的固件不一样**：`SiFli-SDK` 和 `qemu-support` 的 `hello_world`
源码不同（一个打 `Hello world3!`，一个打 `Hello world!`），`ptab.yaml` 的分区
布局也不同。报问题时要说清用的是哪棵树的固件。

#### EZIP 示例固件 —— 从"跑起来"到"跑对了"

上面那条只证明固件**跑起来了**。`example/hal/ezip` 更进一步：它自带测试向量和
期望资产，`main()` 依次跑 EZIP 私有格式（轮询）、EZIP 私有格式（中断）、LZ4、
GZIP，每一条都把解出来的缓冲和期望资产整个 `memcmp` 一遍，对了才打
`[EZIP]Output is correct.`。

```bash
cd <SDK>/example/hal/ezip/project
scons --board=sf32lb52-lcd_a128r16_hcpu -j8

cd ~/build-sifli
timeout 120 ./qemu-system-arm -M sf32lb52x,ezip-tool=<SDK>/tools/png2ezip/ezip_linux \
  -display none -serial stdio -monitor none \
  -kernel <SDK>/example/hal/ezip/project/build_sf32lb52-lcd_a128r16_hcpu/main.elf
```

期望四句 `[EZIP]Output is correct.`，且没有 `incorrect`。四句齐了说明三条解码
路径和两种完成握手都对：私有格式（真的 spawn 了 `ezip_linux`）、LZ4、GZIP，
以及轮询读 `INT_MASK` 与中断走 `INT_EN`/IRQ 89 这两条路。这个固件最后 `while(1)`
不退出，脚本同样是看着输出把它杀掉。

`ezip-tool=` 不给的话私有格式那两条就解不出来（模型记一笔后照常完成，不挂死），
所以脚本在找不到工具时整项 `[SKIP]`。

SDK 在 `/mnt/e` 上时 `source export.sh` 会卡很久，见 §4⑦。

#### EPIC 示例固件 —— 像素对不对

`example/hal/epic` 跑两级 alpha 混合：前景蓝 150×100 @(50,50)、背景红 150×100
@(100,100)，两个都 `alpha=128`，混到 250×200 输出区的全屏（390 像素宽）buffer
里。三个 buffer 都在 PSRAM（`L2_NON_RET_BSS_SECT` → `.RW_PSRAM_NON_RET`）。

**这个例程自己不查像素**，只查 `HAL_EPIC_BlendStartEx` 的返回值，所以 `EPIC
blend succeeded` 只说明 HAL 的调用序列在模型上走完了。真正的检查是脚本把输出
buffer 从内存里读回来自己比：

```bash
cd <SDK>/example/hal/epic/project
scons --board=sf32lb52-lcd_a128r16_hcpu -j8

cd ~/build-sifli
timeout 120 ./qemu-system-arm -M sf32lb52x \
  -display none -serial file:/tmp/epic.log -monitor stdio \
  -kernel <SDK>/example/hal/epic/project/build_sf32lb52-lcd_a128r16_hcpu/main.elf
```

固件跑起来之后，在 monitor 里敲（`(qemu) ` 是提示符，不是要敲的内容）：

```
stop
xp /1wx 0x60417afc
```

`stop` 把 CPU 停住——例程最后是 `while(1)`，不停机读到的是会变的内存。`xp` 按
**物理**地址读内存，`/1wx` 是"读一个单位、单位宽 4 字节、十六进制显示"。

`0x60417afc` 是重叠区那个采样点：buffer2 的 `0x60400000` + 124 行 × 780 字节 +
150 像素 × 2 字节。一个 word 装两个像素，读回来是 `0x80088008`。

**monitor 不认 `#` 注释**，注释别敲进去（会报 `unknown command: '#'`）。

输出 buffer 的地址是链接期定的（`__PSRAM_BASE`），所以脚本从符号表里读
`buffer2`，不写死。采样点按区域挑，四个区域的值互不相同：

| 区域 | 采样点 | 期望 |
|---|---|---|
| 画布（没被任何图层覆盖） | (10,10) (260,60) | `0x00000000` |
| 只有前景 | (74,74) (198,74) | `0x00100010` |
| 只有背景 | (240,124) (148,196) | `0x80008000` |
| 前景与背景重叠 | (150,124) | `0x80088008` |

蓝和红都是 `0x10`/`0x8000`，即 31 级里的 16 级 ≈ 128/255，alpha 生效了。重叠处
是 `0x8008`：红盖在蓝上，红得 16 级、蓝被压到 8 级（`16 × 127/255`）。**混合的
具体取整值取决于模型怎么算，但这四处各不相同就说明图层位置和混合顺序是对的**
——位置错一个像素，采样点就会落到另一个区域上，值立刻不对。

这一项同时验到了 PSRAM：`.RW_PSRAM_NON_RET` 有 0x868a0 字节，`hello_world`
里这个段是 0，所以第 9 项从来没碰过 PSRAM。顺带也验了 EPIC 的两条地址通路
——`AHB_MEM` 走 CPU 地址、图层 `SRC` 走 SBUS 别名，见 `peripherals.md` §8.2。

#### `example_ezip` 用例 —— co-engine（EZIP 解压直接喂 EPIC）

上面两个例子都**不走**这条配对：`example/hal/ezip` 只做
`HAL_EZIP_OUTPUT_AHB`，`example/hal/epic` 的图层全是 `EPIC_COLOR_RGB565`
（`src/main.c:109/125/146`）。真正走到 `EPIC_ConfigEzipDec` 的是
`example/hal_example` 里的 `example_ezip` 用例——`fg_img.color_mode =
EPIC_COLOR_EZIP`，而且它**自带期望资产、逐像素比对**（`cmp_data()`），
画错一个像素就过不去。

```bash
cd <SDK>/example/hal_example/project
scons --board=sf32lb52-lcd_a128r16_hcpu -j8
```

这个工程是个 utest 容器，`main()` 只挂机，用例从串口敲进去。**stdin 那头要一直
开着**（用 FIFO，别让 chardev 在 EOF 上被摘掉）。注意**它的提示符是 `msh >`，
不是 `msh />`**——提示符来自 `finsh_get_prompt()`，两个固件给的不一样，照抄
hello_world 那条会一直等下去：

```bash
fifo=$(mktemp -u); log=/tmp/ezip.log; mkfifo "$fifo"; exec 3<>"$fifo"
./qemu-system-arm -M sf32lb52x,ezip-tool=<SDK>/tools/png2ezip/ezip_linux \
    -display none -serial stdio -monitor none \
    -kernel <SDK>/example/hal_example/project/build_sf32lb52-lcd_a128r16_hcpu/main.elf \
    < "$fifo" > "$log" 2>&1 &
# 等 "msh >" 出现（启动要几十秒）之后：
printf 'utest_run example_ezip\r\n' > "$fifo"
```

期望：

```
[----------] [ testcase ] (example_ezip) started
blending done
check done
blending done
check done
[  PASSED  ] [ result   ] testcase (example_ezip)
[==========] [ utest    ] Total: 1, Fail: 0
```

用例跑两遍混合：整幅 88×88 @(10,5)，和裁到 40×50 的那半幅——第二遍验的正是
窗口裁剪（`EPIC_CalcDecImgArea` 把窗口缩到可见部分）。

**不给 `ezip-tool=` 时这一项必须干净地失败**（`Total: 1, Fail: 1`，几秒内退出），
不能挂住。EPIC 清"EZIP 在跑"的标志只在完成回调里做、回调只在 END 时调，所以
输出给 EPIC 的作业报错误位会把固件钉死在 `while (epic->coeng_state)`。
这是模型上真踩过的坑，见 `peripherals.md` §6.5。

#### `single_mode` —— co-engine 图层**带缩放**（EZIP + `SCALE_RATIO`）

上面的 `example_ezip` 用例是 1:1 的 co-engine。带缩放的那条在
`example/rt_device/gpu/single_mode`：`scale_down_demo(multiple, 205, 208)`
（`src/main.c:186`）把同一份 EZIP 资产按 `scale_x = scale_y = 1024 * multiple`
缩放贴到 `buffer0`（390×450 RGB565），`main()` 依次跑 1/2/3 倍
（`src/main.c:547-551`），每次 `drv_epic_blend()` 返回后打一行 `show lcd ...`
——**这一行就是"`buffer0` 已经画好了"的信号**。

```bash
cd <SDK>/example/rt_device/gpu/single_mode/project
scons --board=sf32lb52-lcd_a128r16_hcpu -j8
```

这个例程也不打印校验和，所以还是"把 `buffer0` 从内存里读回来自己比"。难点是
时机：`show lcd ...` 之后固件马上 `lcd_display_update()` 接着往下跑。用
`-gdb tcp::<port> -S` 起来、盯着串口文件，看到那一行就打断冻住 vCPU，再按从
ELF 取的符号地址读 `buffer0`。**别写死地址**——重编一次就变：

```bash
arm-none-eabi-nm -S <SDK>/example/rt_device/gpu/single_mode/project/build_sf32lb52-lcd_a128r16_hcpu/main.elf | grep -w buffer0
# 604ab680 00055b18 b buffer0        （351000 字节 = 390*450*2）
```

```bash
timeout 300 ~/build-sifli/qemu-system-arm -M sf32lb52x,ezip-tool=<SDK>/tools/png2ezip/ezip_linux \
  -device sifli-panel -display none \
  -serial file:/tmp/scale.log \
  -monitor unix:/tmp/scale-mon.sock,server,nowait \
  -gdb tcp::45505 -S \
  -kernel <SDK>/example/rt_device/gpu/single_mode/project/build_sf32lb52-lcd_a128r16_hcpu/main.elf
```

一条 gdb RSP 客户端连着 `:45505`：`cont`，轮询 `/tmp/scale.log` 里
`show lcd ...` 的次数（第 n 次就是 `multiple = (n - 1) % 3 + 1`，`main()` 那个
`while(1)` 会一轮轮重来），出现后打断、读 `buffer0` 那 351000 字节——**读内存
时 CPU 必须停住**。想顺带截 LCD 那份就发 `screendump /tmp/scale.ppm`（走
monitor socket）；**390 像素宽时 PPM 的行距是 1172 不是 1170**。

期望（**非黑**包围盒，坐标是 `buffer0` 里的绝对位置）：

| multiple | bbox | 含义 |
|---|---|---|
| 1 | (92,121) 205×208 | 不缩放，1:1 贴上去（co-engine 那条未变换路；**寄存器读不出这一档**，见下） |
| 2 | (143,173) 103×104 | 整个 205×208 缩成 2× |
| 3 | (160,191) 69×69 | 整个 205×208 缩成 3×（`BR` 由 HAL 取整成 69×69，纵向相位由 `SCALE_INIT_CFG2` = 0x1fffe 偏 1 行） |

逐像素比法：宿主跑一次同一个 `ezip_linux` 把资产解出来（容器头给宽高，见
`peripherals.md` §6.3），再按寄存器反算采样点对——`buffer0` 上图层
`TL + (x, y)` 处应等于资产
`((SCALE_INIT_X + x*SCALE_RATIO_H) >> 16, (SCALE_INIT_Y + y*SCALE_RATIO_V) >> 16)`
处的像素。寄存器值在冻住的那一刻从 `0x50007020` 起读（TL/BR、`0x5000703c`/`:40`
是 `SCALE_RATIO_H/V`、`0x50007114`/`:118` 是 `SCALE_INIT_CFG1/2`）。

**`multiple = 1` 这一档不能按寄存器判。** 曾经这里写的是"`SCALE_RATIO` 是
0x10000、`SCALE_INIT` 全 0"，实抓**不是**：m1 冻住时读到 `SCALE_RATIO_H` =
0x140、`VL_EXTENTS` 是 `max_col` = `max_line` = 2、`VL_TL_POS` = (0,0)、
`VL_BR_POS` = (389,449)——整画布，根本不是这次作业的值。原因是未变换的
co-engine 路 HAL 压根不碰 VL 那几个寄存器，读回来的是**上一个作业的残留**；
m2/m3 才自洽（`EXTENTS` 的 max_col/max_line = 204/207，正好对上 205×208 的资产）。
所以 m1 只按内容判：包围盒 (92,121) 205×208、且与当场解出来的资产 1:1 逐像素
相等（42640/42640），负对照在这一档没有意义——它本来就是"1:1 原样贴"。

#### `single_mode` —— **内存图层**带缩放（RGB565 + `SCALE_RATIO`）

上面那节缩的是 co-engine 那条路（源是 EZIP 解出来的帧）。源换成**内存里的
普通图层**走的是另一条代码路径，`single_mode` 里也有：`scale_memory_demo(multiple)`
（`src/main.c:243`）把一张 270×270 的 RGB565 位图 `mask_2_data` 按
`scale_x = scale_y = 1024 * multiple` 贴到 `buffer0`，`main()` 跑 2 倍和 3 倍
（`src/main.c:602-604`），每轮 `drv_epic_blend()` 返回后打一行
`scale_mem start--- N`——**这一行就是"`buffer0` 已经画好了"的信号**。

同一份固件、同一个 ELF，所以符号地址的取法（`nm -S` 取 `buffer0` 和
`mask_2_data`）和起机器的方式跟上一节完全一样，只是盯的 marker 换成
`scale_mem start--- N`。`mask_2_data` 也要从 guest 内存里读——它在 PSRAM 的
`.data` 里，开板之后启动代码才从 flash 拷过去，**复位后立刻读是零**。

一次实抓的样子（`VL_EXTENTS` 的 max_col/max_line 都 = 269，源就是 270×270）：

| multiple | `VL_TL_POS` | 框 | `SCALE_RATIO_H/V` | `SCALE_INIT_CFG1/2` | 框内非黑 |
|---|---|---|---|---|---|
| 2 | (128,158) | 135×135 | 0x20000 | 0x10000 / 0x10000 | 7305 |
| 3 | (150,180) | 90×90 | 0x30000 | 0x0 / 0x0 | 3249 |

这张表只当**量级**参考，别当断言：`SCALE_INIT` 那两列尤其不要写死——它由 HAL
按相位算出来，抓的时机不同就可能不一样。真正稳的是 `SCALE_RATIO`（2×/3×）、
框的大小和"框内非黑"的像素数。判据也不比这些常数，一律按冻住那一刻的寄存器
反算——常数写进断言，模型改了寄存器用法也照样"通过"。

逐像素比法和上一节一样（按寄存器反算采样点），只是源图换成 `mask_2_data`。
**这一档必须看"框内非黑"那个数**：整块画布大部分是黑的，拿 m3 的帧去顶 m2 的
寄存器，全帧还有 0.9590，离 0.97 的门槛很近；框内非黑立刻掉到 0.0311。全帧
那个数只用来证明框外确实是黑的。负对照（1:1 原样贴）实测 0.3866 / 0.4873。

#### `example/rt_driver` —— 整条显示链路

前面几项各钉一个环节；这一项走完整条**显示**链路：HAL 读数 → 驱动认屏 → PSRAM
落点 → LCDC 出帧。跑法见下面 ③（它就是要 SDL 的那个固件），这里记**答案**。

```bash
cd <SDK>/example/rt_driver/project
scons --board=sf32lb52-lcd_a128r16_hcpu -j8
```

串口三行，各钉一个独立环节（`\r\n` 已省）：

```
CO5300_ReadID 0x331100
Lcd info w:390, h450, bits_per_pixel 16, draw_align:2
Fill framebuffer addr=0x60400000, w=390, h=450, size=351000(Bytes)
```

| 那行 | 证明 | 出处 |
|---|---|---|
| `CO5300_ReadID 0x331100` | LCDC 读数通路通 + 面板真的挂上了 | `co5300.c:295` |
| `Lcd info w:390, h450, …` | 驱动认出了屏 | `rt_driver/src/main.c:330` |
| `Fill framebuffer addr=0x…` | PSRAM 映射对，固件在刷屏 | `main.c:353` |

两个容易看错的点：

- **`h450` 没有冒号**——固件自己的格式串（`"h%d"`）就是这样，不是你眼花。
- **`size=351000` = 390 × 450 × 2**（RGB565）。`main.c:286` 那句注释写着"PSRAM 板上
  用 RGB888"是**过期的**，两个 `#ifdef` 分支的 `#define` 其实都是 RGB565——别信
  注释，看 `FB_PIXEL_BYTES`。
- `addr` 是**链接期**定的，随板和链接脚本走（dpi-hdk 落在 `0x62040000`，a128r16
  是 `0x60400000`），落在 PSRAM 区（`0x60000000`–`0x63000000`）就对。要从 ELF 里
  核：`arm-none-eabi-nm <elf> | grep framebuffer`。

之后**每 3 秒**重复一行 `Fill framebuffer`，另有**每 5 秒**一行 `__main loop__`
（主循环的存活信号）。这两个周期是时间基准的判据，见 `peripherals.md` §3——
tick 修好之前 45 秒只出一帧。

屏幕是整屏换帧、七帧循环（`main.c:354-361`），顺序是答案的一部分：

| 帧 | 画面 |
|---|---|
| 0 | 8 段横向渐变彩条 |
| 1 | 纵向灰度渐变 |
| 2–6 | 纯红 → 绿 → 蓝 → 白 → 黑 |

没有窗口时用 `screendump` 取证（③ 的排查表最后两行分别对应 GTIMR 和 tick clock）。
**注意 PPM 的行距不是 `宽 × 3`**，390 像素时是 1172——见 §5，按 1170 解会从第二行
起逐行错位。

取一整轮循环：每 1.5 秒一张、连取 16 张，正好盖住 7 帧 × 3 秒 = 21 秒。**判据是
顺序**（0→6 再回到 0），不是单张长什么样——tick 坏着的时候帧长会被拉到 70 多秒，
1.5 秒的间隔根本追不上，取样顺序是乱的。顺带：每帧占两张是这个取样的自然结果，
不是画面在抖。

### ③ 看画面

**必须带 `-device sifli-panel`**，否则 LCDC 不知道该画多大、驱动也认不出屏
（固件打 `unknow lcd!`，屏幕黑但不崩）：

```bash
~/build-sifli/qemu-system-arm -M sf32lb52x -device sifli-panel -display sdl \
    -serial stdio \
    -kernel <SDK>/example/rt_driver/project/build_sf32lb52-lcd_a128r16_hcpu/main.elf
```

WSL 里 `DISPLAY=:0` 由 WSLg 提供，不用额外配置。换屏是换命令行，不改 QEMU：

```bash
-device sifli-panel,id=0x60834200,width=480,height=272
```

#### hal/epic 也送屏，而且走的是另一条路

`example/hal/epic` 混完把 `buffer2` 交给 `lcd_display_update()`。它到的不是上面
这条 `draw_rect`，而是 `draw_rect_async`——SDK 里那个函数不直接碰 LCDC，只往
LCD task 的消息队列塞一条 `LCD_MSG_DRAW_RECT_ASYNC`（`drv_lcd.c:1824`），由任务
稍后处理。所以它同时压到了 rt-thread 的 task/消息队列这一层，而不只是寄存器。

```bash
cd <SDK>/example/hal/epic/project
scons --board=sf32lb52-lcd_a128r16_hcpu -j8

~/build-sifli/qemu-system-arm -M sf32lb52x -device sifli-panel -display sdl \
    -serial stdio \
    -kernel <SDK>/example/hal/epic/project/build_sf32lb52-lcd_a128r16_hcpu/main.elf
```

窗口里是静止画面（`main()` 只混一次，之后 `while(1) rt_thread_mdelay(1000)` 空转），
串口先 `HAL_EPIC_Init ok`、之后每送一次屏一句 `draw_rect_async called`。**先看重叠
区**：前景蓝 x=[50,199] y=[50,149] 和背景红 x=[100,249] y=[100,199] 相交的那块
x=[100,199] y=[100,149] 应当是**偏暗的紫**，不是纯蓝也不是纯红。三块颜色互不相同
才说明图层位置和叠加顺序都对，采样值见 §2② 那张表。像素对不对那一半仍由第 11 项
自动比，这里看的是它自己能不能把画面送出去。

上面 rt_driver 那条看不到画面时按这个顺序查：`-display none` 下 `screendump`
有没有内容（第 8 项就是自动化的这一条）→ 串口有没有 `CO5300_ReadID 0x331100`
→ 有没有 `Fill framebuffer addr=`。三者依次对应读数路径、`-device` 有没有给、
固件有没有真的在刷屏。（hal/epic 的串口是另一套字，见上。）

还有两种**跟 LCD 无关**的坏法，症状却长在屏幕上，`-d guest_errors` 里一条
LCDC/QSPI 的报错都没有：

| 症状 | 原因 |
|---|---|
| 串口停在 `msh />`，`__main loop__` 一行不出，截图全黑 | `HAL_GetTick()` 冻住了——GTIMR 没做成自由计数器，每个 HAL 超时都变成死循环 |
| 三行都有、屏幕也真在换帧，但**慢得离谱** | SysTick 的时钟源不对——tick clock 没接到 `refclk`，或频率没用 `clock_update_hz()` 发出去 |

两种都见 `peripherals.md` §3。前两种和这两种都是**静默失败**：固件不崩、刷屏
线程不卡，只是屏幕不对——所以那三行必须显式对，不能凭"没报错"判过。

### ④ 代码格式

```bash
cd ~/code/sifli-qemu
perl scripts/checkpatch.pl --no-tree --file $PWD/hw/arm/sifli-sf32lb52x.c
perl scripts/checkpatch.pl --no-tree --file $PWD/include/hw/arm/sf32lb52x.h
```

期望各自以这句收尾：

```
... has no obvious style problems and is ready for submission.
```

`--no-tree` 必须加——这棵树不是 kernel tree，不加会被 `top_of_kernel_tree` 挡下。

**文件名要给绝对路径（或者任何带 `/hw/` 的路径）。** checkpatch 里有按 `$realfile`
匹配的规则，比如

```perl
if ($realfile =~ /.*\/hw\/.*/ && $line =~ /\bqemu_bh_new(_guarded)?\s*\(/) {
    ERROR("use aio_bh_new_guarded() instead of qemu_bh_new*() ...");
}
```

相对路径 `hw/dma/sifli-dma.c` 里**没有** `/hw/` 这个子串（它以 `hw/` 开头，前面
没有斜杠），规则不触发，报出来的是 0 errors——**假通过**。这一条真的漏过一次：
两个新设备都用了 `qemu_bh_new()`，相对路径下一路全绿，换成 `$PWD/...` 才报出来。

`verify-sifli.sh` 第 3 项已经改成传绝对路径了。

**最常见的两条**：
- `ERROR: New file '...' requires 'SPDX-License-Identifier'` —— 新 `.c`/`.h` 必须有 SPDX 头
- `WARNING: Block comments use a leading /* on a separate line` —— QEMU 要求多行块注释的
  `/*` 和 `*/` **各占一行**（不能写成 `/* 正文… */`）

**写寄存器掩码时别用 SDK 的 `_Pos`/`_Msk` 命名。** SDK 的头文件长这样：

```c
#define EPIC_L0_CFG_ALPHA_Pos   5
#define EPIC_L0_CFG_ALPHA_Msk   (0xff << 5)
```

`ALPHA_Pos` 这类名字会被 checkpatch 认成**类型名**——它的 `$typeTypedefs` 里有
一条驼峰启发式 `[A-Z][A-Z\d_]*[a-z][A-Za-z\d_]*`（大写串后面跟小写字母），
SDK 的名字正好都长这样。结果 `(cfg >> EPIC_L0_CFG_ALPHA_Pos) & 0xff` 会被
当成"类型后面跟了个 `&`"：

```
ERROR: space prohibited after that '&' (ctx:WxW)
```

**是假报错，而且只在这一种写法上出现**：checkpatch 的 `)` 会把
`@av_paren_type` 弹掉，但**只在弹出的值不是 `'_'` 时才重置 `$type`**，所以
括号里的最后一个 token 决定了括号外的类型判断。QEMU 自己的宏全是大写，踩不到。

照 SDK 的移位掩码约定写就能绕开——`_Msk` 定义成**已经移好位**的：

```c
#define EPIC_L0_CFG_ALPHA_Pos   5
#define EPIC_L0_CFG_ALPHA_Msk   (0xffu << 5)
/* 取值一律 (v & Msk) >> Pos */
```

`Msk` 以大写 `M` 开头但不是"大写串+小写"的形状，不触发启发式。两种写法结果
一样，SDK 的头文件本身就是这个约定。

### ⑤ 属性生效

```bash
# 默认 0x10000000；显式给 QSPI2 的基址会 fault（固件按 0x10000000 链接，取不到向量表）
./qemu-system-arm -M sf32lb52x,flash-base=0x12000000 -nographic -kernel <fw.elf>
./qemu-system-arm -M sf32lb52x,flash-size=0x800000 -nographic -kernel <fw.elf>
```

### ⑥ 提交完整性

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
bash notes/verify-sifli.sh                       # 默认固件
SIFLI_REAL_FW=/path/to/main.elf bash notes/verify-sifli.sh   # 换固件
SIFLI_REAL_FW=- bash notes/verify-sifli.sh       # 跳过固件那一项
```

第 10、11 项各自要一个例程固件，路径不合适就用 `SIFLI_EZIP_FW=` / `SIFLI_EPIC_FW=`
覆盖（`SIFLI_EZIP_TOOL=` 指宿主的 `ezip_linux`）；固件或工具不在就整项 `[SKIP]`，
不会假装通过。

`verify-sifli.sh` 到真实板子固件为止。模型本身另有两个脚本，都是同一个口径
（全过退出码 0，缺资产 `[SKIP]` 不算失败）：

| 脚本 | 项数 | 管什么 |
|---|---|---|
| `notes/qtest-sifli.sh` | 21 | 按人写的值敲寄存器，查模型**读写路径**。不需要 SDK，进 CI |
| `notes/realboard/run.sh` | 3 场景 | 真实板子固件，查 **HAL 自己算出来的寄存器**对不对。见 §2② |

```bash
SIFLI_QEMU_BUILD=~/build-sifli bash notes/qtest-sifli.sh
bash notes/realboard/run.sh
```

十一项检查，全部通过退出码 0，可直接进 CI：

| # | 检查 | 性质 |
|---|---|---|
| 1 | 两个二进制都编出来了 | |
| 2 | 两个二进制里 `-M help` 都能看到 sf32lb52x | |
| 3 | checkpatch 0 errors 0 warnings | |
| 4 | 工作区行尾是 LF（不是 CRLF） | |
| 5 | 提交带 Signed-off-by 且与 author 一致 | |
| 6 | 外设区域都映射了，寄存器读回值正确 | 结构性 |
| 7 | 外设写路径（qtest） | 结构性 |
| 8 | 画面真的出去了（写显存 + screendump 解像素） | 结构性 |
| **9** | **真实板子固件跑到 `main()` 和 msh 提示符** | **真正的验收** |
| 10 | EZIP 例程四条解码与资产逐字节一致 | 结果正确性 |
| 11 | EPIC 例程混合出来的像素对 | 结果正确性 |

**第 9 项才是关键。** 第 6–8 项都够不着 HAL：第 6、7 只读几个寄存器，
**模型写错了照样能过**——`peripherals.md` §11.2 那个 DWT 映射错位的坑就骗过了
它们全部。只有 SDK 原样构建的真实板子固件、一行不改地跑到 `main()`，才能证明
整个 HAL 真的在模型上跑起来了——时钟树、电源、RTC、MPI、音频、控制台，整条链路。
第 10、11 项再用例程自带的结果验一遍**算得对不对**。

**第 8 项补的是第 6、7 项够不着的另一半**：寄存器读写对，不等于像素出去了。
它往显存放一块已知颜色、START、再让 monitor `screendump` 截图，然后解 PPM
断言具体像素。不需要固件、不需要 SDL，所以进 CI。

不跑脚本时手工至少查这几条：

```
□ 两个二进制的 -M help 都能看到 sf32lb52x
□ -device sifli-panel 下 screendump 的 PPM 里那块颜色是对的
□ 真实板子固件能跑出 "Hello world!" 和 msh 提示符（不带 -semihosting）
□ ezip 例程四句 [EZIP]Output is correct.
□ epic 例程打 "EPIC blend succeeded"
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

### ⑦ SDK 在 drvfs 上时 `source export.sh` 会卡很久

SDK 放在 `/mnt/e`（WSL 的 drvfs，实际是 Windows 盘）上时，`source export.sh`
要几十秒到几分钟才返回——构建系统会对整棵树做 `git status`，而 drvfs 上每个
文件的 `stat` 都要过一遍 9P，慢在这里。

临时绕开，让 git 别去比 `stat`（只影响这一次构建）：

```bash
export GIT_CONFIG_COUNT=1
export GIT_CONFIG_KEY_0=core.checkStat
export GIT_CONFIG_VALUE_0=minimal
source export.sh
```

实测从"几分钟"降到 **26 秒**。或者把 SDK 放到 WSL 自己的文件系统里（`~/code/`），
那就不用管这一条了。

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

**没有 `-kernel` 时必须加 `-S`**，否则 QEMU 会被 HardFault 打死：

```
qemu: fatal: Lockup: can't escalate 3 to HardFault (current priority -1)
```

qtest **不是**"只跑设备、不跑 CPU"——加速器仍然会让 vCPU 跑。没有 `-kernel`
时它从一片全零的 ROM 开始执行，几秒后必然撞进 HardFault。只要那一项不需要
客户机执行任何指令（读寄存器、写显存、截图），就加 `-S` 把 CPU 冻住。
第 6、8 项都是因为这个原因带 `-S`。

**`screendump` 出的 PPM 行距不是 `宽 × 3`**。`ppm_save()`
（`ui/ui-qmp-cmds.c:321`）每行写的是 `pixman_image_get_stride(linebuf)`，而那个
`linebuf` 是 24bpp 的，pixman 会把行距按 4 字节对齐——390 像素时 1170 变成
1172。**头里写的却还是 390 像素宽**，所以这个文件严格来说对不上标准，按
`宽 × 3` 去解会从第二行起逐行错位。解的时候两个行距都试、以文件实际长度为准。

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
| Linux | **glibc ≥ 2.34**<br>（Ubuntu 21.10+ / Debian 12+ / RHEL 9+）<br>另需 `libglib2.0-0`、`libpixman-1-0`、`libpng16-16`、`zlib1g`、`libsdl2-2.0-0` | 产物里引用到的最高 `GLIBC_x.y` 符号（不是构建环境的版本）|
| macOS | **macOS ≥ 11**，**仅 arm64** | `MACOSX_DEPLOYMENT_TARGET=11.0`；架构取决于 runner |
| Windows | Windows 10+，x86_64 | MSYS2 mingw64 自身的下限 |

**Linux 的下限要读产物，不能拿构建环境的 glibc 顶替。**

glibc 的函数带版本号（`memcpy@GLIBC_2.14`）。链接时二进制记下**用到的最高
版本**，运行时动态链接器检查系统里有没有——没有就直接起不来：

```
version `GLIBC_2.38' not found
```

所以产物能跑的**最低** glibc 版本，就是它引用到的最高 `GLIBC_x.y`：

```bash
objdump -T <二进制> | grep -o 'GLIBC_[0-9.]*' | sort -V | tail -1
```

这个值**不可能超过构建环境的 glibc**（编不出系统里没有的符号），但通常低于它：
CI 的 runner 是 22.04（glibc 2.35），而产物实际只用到 **2.34**。

**在越新的系统上编，这个值只会越高，产物能跑的机器就越少。** 用 Ubuntu 24.04
（glibc 2.39）编出来是 2.38，只能跑在 23.10+ 上。反过来永远成立：glibc 向后
兼容，新系统能跑旧产物。CI 选 `ubuntu-22.04` 而不是 `-latest`，就是为了让产物
跑在**更多**机器上——这是它唯一的理由。

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
  and it needs GLIBC_2.34 or newer: the build host's version
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
