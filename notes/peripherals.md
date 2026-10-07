# sifli-qemu：外设模型

这份文档说明外设层是怎么组织的、为什么这么组织，以及加一个新外设时要做什么。
构建和验证见 `build-and-verify.md`。

---

## 0. 目标：让真实 HAL 跑起来

固件不做任何裁剪，`bf0_hal_*.c` 一行不改，直接跑在 machine 里。所以模型必须
和真实芯片**寄存器级对齐**——HAL 读哪个位，那个位就得给出正确语义。

同时保持"薄"：**只实现 HAL 真正读到的位，并且立刻给出终态，不建模时序**。
不做 FIFO 深度、不做波特率分频、不做 PLL 锁定过程。

---

## 1. 两种形态

外设按"有没有行为"分成两类，用两套完全不同的做法：

| | 有行为 | 纯配置 |
|---|---|---|
| 例子 | USART、DMA、MPI、以后的 I2C/SPI | RCC、AON、PMUC、HPSYS_CFG、PINMUX |
| 实现 | 独立的 QOM sysbus 设备 | 数据表 + 通用 `sifli-regbank` |
| 文件 | `hw/char/sifli-usart.c`、`hw/dma/sifli-dma.c` | `hw/misc/sifli-regbank.c` + `hw/arm/sf32lb52x-periph.c` |

这么分的原因很直接：RCC + AON + PMUC + CFG 加起来四百多个寄存器，**没有一个是
有行为的**。固件对它们的全部要求就是"写完 enable 之后，某个状态位能读到 1"。
用 switch 一个个写是几千行没有逻辑的代码；写成表之后，加一个寄存器是加一行。

而 USART 有真行为（收、发、中断、清了 TC 才能发下一个字节），表表达不了，就该
老老实实写设备模型。

---

## 2. 表驱动寄存器组（sifli-regbank）

### 2.1 一条表项描述一个寄存器

按**影响读还是影响写**分三组：

```c
typedef struct SifliRegDef {
    /* 存储值 */
    uint32_t off;          /* 块内字节偏移 */
    uint32_t reset;        /* 复位值 */

    /* 写侧 */
    uint32_t readonly;     /* 普通写不改这些位 */
    uint32_t w1c;          /* 写 1 清本寄存器的这些位 */

    /* 读侧 */
    bool     writeonly;    /* 读恒为 0（只写寄存器） */
    uint32_t force1;       /* 这些位读恒为 1 */
    uint32_t force0;       /* 这些位读恒为 0 */

    /* 写重定向：写本寄存器，实际改的是 alias 指向的那个 */
    uint32_t alias;
    uint32_t alias_set;    /* 写 1 置目标位 */
    uint32_t alias_clear;  /* 写 1 清目标位 */
} SifliRegDef;
```

`w1c` 和 `alias` 是**两个正交概念**，别混：

| | 作用对象 | 粒度 |
|---|---|---|
| `w1c` | **本寄存器** | 位掩码（可以和普通可写位、`readonly` 位共存） |
| `alias_set`/`alias_clear` | **别的寄存器** | 整寄存器（命中就接管，不再走存储路径） |

理论上 `w1c` 能用"指向自己的 alias"表达，但那是说反话、而且丢了位粒度，
所以不用那种写法。

**`w1c` 目前零使用**：这颗芯片清标志一律用独立清寄存器（见 §2.5），没有哪个
寄存器是「写 1 清自己」。保留它是作为能力储备——真出现自清的寄存器时，
**记得同时补一条 qtest**，因为在有东西把它置起来之前，它和 `force0` 观察不到差别。

字段全部默认 0，所以**最常见的普通读写寄存器只要写 `.off` 和 `.reset`**。
注意"可写"是默认值：`readonly` 列的是**改不动**的位，不是可写的位。

`alias_set`/`alias_clear` **任一非 0** 就表示这是个别名寄存器，其余寄存器三个字段
全省略。

### 2.2 `force1` / `force0` 是"伪造就绪"的全部手段

硬件上"PLL 锁定""LDO 就绪"这类位是**经过一段时间之后**由硬件置起来的。薄模型
不建模那段时间，直接让它恒为终态：

```c
{ .off = 0x2c, .force1 = BIT(31) },    /* DLL1CR: READY */
```

于是固件里

```c
while (0 == ((*cr) & HPSYS_RCC_DLL1CR_READY)) { }     /* bf0_hal_rcc.c:1424 */
```

第一次判断就通过。

`force0` 用在使用反向逻辑的地方——需要读回 0 才不进入的分支，比如
`LPSYS_AON.SLP_CTRL.SLEEP_STATUS`（`bf0_hal_rcc.c:1714` 在它非 0 时会要求唤醒并等待）。

### 2.3 哪些位需要伪造——**从 HAL 里 grep 出来，不是从手册里猜**

所有会让固件卡死的轮询点都是 grep `while` 找到的。当前强制的位和它们的调用点：

| 寄存器 | 位 | 谁在等 |
|---|---|---|
| `HPSYS_AON.ACR` | 30 `HRC48_RDY`, 31 `HXT48_RDY` | `bf0_hal_hpaon.c:501,514` |
| `HPSYS_AON.ISSR` | 4/5 `HP_ACTIVE`/`LP_ACTIVE` | `bf0_hal_hpaon.c:162,165` |
| `LPSYS_AON.ACR` | 30, 31 | `bf0_hal_rcc.c:1772` |
| `LPSYS_AON.SLP_CTRL` | 4 `SLEEP_STATUS` **清 0** | `bf0_hal_rcc.c:1714` |
| `HPSYS_RCC.DLL1CR/DLL2CR` | 31 `READY` | `bf0_hal_rcc.c:1353,1424` |
| `HPSYS_RCC.HRCCAL1` | 31 `CAL_DONE` | `bf0_hal_rcc.c:2277,2358,2434` |
| `PMUC.LXT_CR` | 31 `RDY` | `bf0_hal_pmu.c:476` |
| `PMUC.LRC10_CR/LRC32_CR` | 31 `RDY` | `bf0_hal_pmu.c:524` |
| `PMUC.VRET_CR` | 31 `RDY` | — |
| `PMUC.BUCK_CR1/BUCK_CR2` | 31 `SS_DONE` / 19 `FORCE_RDY` | — |
| `PMUC.HPSYS_LDO/LPSYS_LDO` | 16 `RDY` | — |
| `PMUC.HPSYS_SWR/LPSYS_SWR` | 31 `RDY` | — |
| `PMUC.DBL96_CALR` | 13 `CAL_LOCK` | — |
| `HPSYS_CFG.IDR` | REVID **必须是 0x03/0x07/0x0f** | `drv_common.c:712` 的 assert |

### 2.4 两个"读回特定值"的坑

**① `HRCCAL2` 必须读 0。** 它上报校准用的两个计数：

```c
hxt_cnt = HRCCAL2[31:16];  hrc_cnt = HRCCAL2[15:0];   /* bf0_hal_rcc.c:2439 */
```

如果给它置 bit31（看着"像"个状态位），`hxt_cnt` 就成了 0x8000，`cnt_diff = 32768`，
超过阈值 160 → 返回 `HAL_ERROR` → `HAL_Init` 里的 `HAL_ASSERT` → **死循环**。
读 0 则两个计数都是 0，第一轮就 `break` 并返回 `HAL_OK`。

**② `HPSYS_CFG.IDR` 的 REVID 必须是 0x03/0x07/0x0f。** `__HAL_SYSCFG_CHECK_REVID()`
是个 assert，冷启动必走；读 0 直接挂。

### 2.5 跨寄存器联动：alias 三件套

RCC 的使能位是三个寄存器联动的：

```c
void HAL_RCC_EnableModule(...)  { *esr = (1UL << offset); }   /* 写 ESR */
void HAL_RCC_DisableModule(...) { *ecr = (1UL << offset); }
bool HAL_RCC_IsModuleEnabled(...) { return *enr & (1UL << offset); }  /* 读 ENR */
```

`ESR`/`ECR` 是只写别名，改的是 `ENR` 的位。注意是**整字赋值不是 `|=`**——
当成普通寄存器的话，写一次就把其它所有使能位全抹了。

这在表里直接声明，**不用写 C 代码**：

```c
{ .off = HPSYS_RCC_ESR1, .writeonly = true,
  .alias = HPSYS_RCC_ENR1, .alias_set = ~0u },
{ .off = HPSYS_RCC_ECR1, .writeonly = true,
  .alias = HPSYS_RCC_ENR1, .alias_clear = ~0u },
```

`writeonly` 让读返回 0（和硬件一致），`alias` 把写重定向到 `ENR1`。
（早先这里是四行手写 `switch case`，挪进表里之后，这条关系和其它寄存器并排可见。）

**注意固件有两条使能路径**，两条都要支持：
- `HAL_RCC_EnableModule()` → 写 ESR（走 alias）
- `HAL_RCC_HCPU_enable()` 宏 → 直接 `ENR1 |=`（普通写，表自己处理）

#### 「写 A 清 B」是同一个机制

这颗芯片清标志一律用**独立的清寄存器**，而不是「写 1 清自己」：

| 写 | 清 | 出处 |
|---|---|---|
| `WCR` | `WSR` | `bf0_hal_pmu.c:364-370` |
| `SCR` | `SR` | `bf0_hal_mpi.c:305,313` |
| `ICR` | `ISR` | USART（在设备里单独做，见下） |

前两个位号对齐，直接一个 alias 搞定：

```c
/* WSR 是普通存储，WCR 只写、写进去的位去清 WSR */
{ .off = 0x24 },                                  /* WSR */
{ .off = 0x28, .writeonly = true,
  .alias = 0x24, .alias_clear = ~0u },            /* WCR */
```

**USART 的 ICR/ISR 用不了 alias**，因为位号不对齐（`TCBGTCF` 是 ICR bit7，
`TCBGT` 是 ISR bit25）。它必须按名字逐个翻译，这也是 USART 写成独立设备、
不塞进 regbank 的原因之一。

### 2.6 写钩子：留给表说不清的事

alias 解决了"写 A 改 B"。剩下的是另一类：**写的副作用不是改寄存器，而是改变系统状态**。

目前只有一处——RCC 的时钟寄存器。写 `CSR`/`CFGR`/`DLL1CR` 会改变 CPU 频率，
QEMU 侧的 SysTick 速率必须跟着走（见 §3），这是表无论如何表达不了的：

```c
bool (*write_hook)(SifliRegBankState *s, uint32_t off, uint32_t value);
```

返回 `true` 表示这次写由钩子接管，表不再处理。钩子里可以用
`sifli_regbank_set_bits()` / `sifli_regbank_clear_bits()` 够到别的寄存器。

### 2.7 读钩子与 peer：GTIMR 用的两件东西

§2.6 那个钩子管"写进去的副作用"。GTIMR 要的是反过来的那一种：**值根本不是存储**。
写什么进去都不决定它读回什么，也没有周期性的更新能追上——**两次读之间它本来就该
变**，所以只能在被读的那一刻算：

```c
void (*read_hook)(SifliRegBankState *s, uint32_t off, uint32_t *value);
```

钩子拿到的是"表本来会返回的值"（存位经 force1/force0 之后），可以整个换掉；只改
自己负责的偏移，别的放着不动。目前只有 GTIMR 一个用户（见 §3.3）。

GTIMR 的速率还得看**另一个 bank** 的寄存器（`RTC_CR.LPCKSEL` 决定数晶振还是
RC），于是 regbank 有一个可选的 `peer`：machine 在两边都建好之后用
`sifli_regbank_set_peer()` 接上，**钩子运行时才去读**，不在设置时取值。没有 peer
的 bank 留 NULL，用之前先判空。

### 2.8 未列出的偏移

读 0，并以 `LOG_GUEST_ERROR` 记一笔。加 `-d guest_errors` 就能看到固件碰了哪些
没建模的寄存器——这是找"下一步该做什么"的主要手段。

---

## 3. 时间基准：RCC、SysTick 和 GTIMR

固件里的"一秒"由三样东西决定：CPU 频率（RCC → SysTick）、SysTick 自己的时钟源
（tick clock）、以及 `HAL_GetTick()` 读的那个自由计数器（GTIMR）。**三个都得对**，
而且坏掉的症状都不长在自己身上——下面每小节对应一类。

### 3.1 RCC 决定 SysTick

这是整个模型里唯一一处**跨模块的强耦合**，容易漏。

固件的 `SystemCoreClock` 是它**自己读 RCC 寄存器算出来的**
（`HAL_RCC_GetSysCLKFreq()` → `GetHCLKFreq()`）：

```c
uint32_t r = 48000000;                          /* 默认 */
if (core_id == CORE_ID_HCPU)
    switch (hwp_hpsys_rcc->CSR & 3) {
    case RCC_SYSCLK_DLL1: r = HAL_RCC_HCPU_GetDLL1Freq(); break;   /* = 3 */
    }
return r;                                        /* 再除以 CFGR 的 HDIV，0 当 1 */
```

而 QEMU 侧 SysTick 的速率由 machine 的 `sysclk` 决定。**两者必须一致**，否则固件
以为的 1 ms 不是 1 ms。

SDK 自己的启动流程（`HAL_PreInit` → `HAL_RCC_HCPU_ConfigHCLK(240)`）最后会把
`CSR[1:0]` 切到 DLL1，频率变成 240 MHz。如果 machine 的时钟还停在复位值
48 MHz，**固件里每一个延时都会变成五倍长**。

所以 RCC 表带钩子，在 `CSR`/`CFGR`/`DLL1CR` 被写时重算
（`sf32lb52x_rcc_update_clocks()`，`hw/arm/sf32lb52x-periph.c:217`）。machine 把
`sysclk` 交给每个 bank（只有 RCC 会用它）。

**新频率要用 `clock_update_hz()` 发出去，不能用 `clock_set_hz()`。** 后者只把值
缓存在 clock 对象上，消费者拿到的还是旧值——`clock_set_source()` 只在 clock
被**连接**时复制一次频率，之后不再管，运行中改频率必须靠 `clock_update_hz()`
里那下 `clock_propagate()`。用错了不会报错，只是时基一直按连接时的值走，差一个
比例。这个坑在下面 3.2 真踩过一次。

顺带一个好消息：**频率能自洽**。`HAL_RCC_HCPU_GetDLLFreq()` 从 `DLL1CR` 的
stage 字段反推（`freq = stg * 24M + 24M`），而 `EnableDLL` 写进去的就是
`stg = (freq - 24M) / 24M`。所以只要 `DLL1CR` 是"存写入值 + 强制 READY"，
写 240 MHz 读回就是 240 MHz，不需要额外维护。

**USART 的波特率反而不用管**：`SystemFixClock` 是编译期常量 48 MHz
（`bf0_hal.h:198`），BRR 由它算出来，和 RCC 寄存器无关。

这条耦合现在有守卫：`notes/qtest-sifli.sh` 的 [13] 证改 HCLK 不影响 refclk 的
周期，[14] 证 `SYST_CSR.CLKSOURCE` 一切过去速率就跟着换。

### 3.2 tick clock：SysTick 的另一个时钟源

52 系列上 SysTick **不一定要**拿 HCLK 当基准。固件在 `rt_hw_systick_init`
（`drv_common.c:126`）里按编译期开关挑时钟源，a128r16 这份走的是高精度那条：

```c
HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_HP_TICK, RCC_CLK_TICK_HRC48); /* CSR[14:13]=2 */
HAL_Delay_us(200);
HAL_RCC_HCPU_SetTickDiv(60);                                      /* CFGR[21:16]=60 */
HAL_SYSTICK_Config(800000 / RT_TICK_PER_SECOND);                  /* LOAD=799, 1 ms */
HAL_SYSTICK_CLKSourceConfig(SYSTICK_CLKSOURCE_TICK_CLK);          /* refclk，不是 HCLK */
```

48 MHz / 60 = 800 kHz。另两条分支（`BSP_PM_FREQ_SCALING` 用 `32768 / 2`，都没定义
时用 HCLK，`drv_common.c:144-158`）不需要模型额外做什么，但**说明这个频率是固件
选的、不是固定的**——所以模型得按寄存体现算（3.1 那个钩子里一并算），固件启动时
走一遍这棵树、每次 PM 变频再走一遍，钉死常数会在第一次变频后悄悄对不上。
machine 侧是**独立的第二个 Clock**（`systickclk`），接到 systick 的 `refclk`。

链路上两个坑：

1. **`refclk` 没接。** `hw/arm/armv7m.c:463` 只在 `refclk` 有源时才把它转给
   systick；没有源时 QEMU 反过来**强制** `SYST_CSR.CLKSOURCE=1`
   （`hw/timer/armv7m_systick.c:143`、`:207`），于是 SysTick 去数 CPU 时钟而不是
   那 800 kHz。症状是**帧循环快了几十倍**——实测帧间隔 0.13 秒，固件要的是 3 秒。
   固件确实选的是 `refclk`：它写完 CMSIS 的 `CLKSOURCE=1` 之后又用一次读改写把
   bit2 清掉（trace 里 `CTRL=0x7`，随后 `CTRL=0x10003`）。
2. **`clock_set_hz()` 不传播**（见 3.1）。用错了不报错，SysTick 一直按连接时的值
   走。中间试出过一版量到 **41/s**，正好是 32768/800——即 tick clock 从头到尾
   停在 LXT 上，而不是那 800 kHz。

另外得连上游一起补一处：`systick_cpuclk_update()` / `systick_refclk_update()`
（`hw/timer/armv7m_systick.c:217`、`:231`）里那两个 `if` 原本只有注释、**没有
`return;`**，注释写的是"可以忽略另一路时钟的变化"，代码却照样把周期改成自己那
一路——于是**后变的那个时钟赢**。上游没暴露是因为真板子的时钟一般启动后就定了，
而模型会在运行中重调两路。补上之后只有被 `CLKSOURCE` 选中的那一路能改周期。

改完实测：45 秒里 14 帧 `Fill framebuffer`、8 行 `__main loop__`，正是
`mdelay(3000)` / `mdelay(5000)` 在 1 kHz 下的样子（坏着的时候 20 秒才 1 帧）。

`notes/qtest-sifli.sh` 的 [13] 把这个行为钉住了：把 tick clock 程序成 800 kHz
之后再把 HCLK 切到 240 MHz，refclk 的周期必须不变（量法是读两次 `SYST_CVR`
取差，写法上的两个坑都写在那一项里）。**这是本树唯一改动的上游 QEMU 文件**
（`da8df8ee59`），rebase 或摘出去合上游时最容易静默丢掉——丢了 [13] 会挂，
以前没有任何测试会报警。

### 3.3 GTIMR：`HAL_GetTick()` 的自由计数器

`HAL_GetTick()`（`drv_common.c:322`）读 `HPSYS_AON.GTIMR`（`0x500c0034`），一个
自由计数器，按 `t * 1000 / 32768` 折成毫秒（`:357`）。**它必须真的在走**——
当普通寄存器存着（永远读回定值）时 `HAL_GetTick() - Tickstart` 恒为 0，SDK 里
**每一个** HAL 超时都永不触发，等待循环从"超时报错"变成**死循环**。

第一次跑 `example/rt_driver` 就撞上了：串口停在 `msh />`、`__main loop__` 一行
不出、16 张 `screendump` 全黑，而 `-d guest_errors` 里**一条 LCDC/QSPI 的报错都
没有**（只有音频那几个未建模偏移）。看着像 LCD 坏了，其实跟 LCD 无关：

- **定位靠 PC/LR 而不是猜**：`info registers` 看到 PC 在 `HAL_GetTick`、LR 在
  `I2C_WaitOnFlagAndDetectError`；再从 `rt_current_thread`（`0x2000d29c`）读 TCB、
  扫栈上的 flash 返回地址做 addr2line，拿到 `tp_init_thread_entry`
  （`drv_touch.c:489`）→ ft6146 `init` → `read_regs` → `rt_i2c_transfer`。
  **是触摸屏初始化在等 I2C。**
- **屏幕为什么跟着黑**：`tp_init` 是优先号 12 的线程，自旋不让出；优先号 15 的
  `main` 一直就绪但轮不到，优先号 30 的 `lcd_refr` 更不用想——所以 LCD 那边一行
  日志都没有。**同一个模型问题，症状长在一个完全无关的外设上。**
- **修法**：GTIMR 不再是普通寄存器，改成**读钩子现算**
  （`sf32lb52x_hpsys_aon_read()`，`hw/arm/sf32lb52x-periph.c:502`），值就是
  `muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), hz, NANOSECONDS_PER_SECOND)`。
  两个机制见 §2.7。**不用 timer 是有意的**：要让 32768 次/秒的唤醒去喂一个
  没人轮询的寄存器，不如读的时候算一次——反正两次读之间它本来就该变。
- **速率不是常数**：`sf32lb52x_gtimr_hz()`（`:493`）按 `RTC_CR.LPCKSEL` 在晶振
  32768 和 RC 10000（`HPSYS_AON_GTIMR_LXT_HZ` / `_RC10K_HZ`，`:490`）之间选。
  这个位在 **RTC bank**、不在 AON bank，靠 regbank 的 `peer` 链过去（§2.7）。
  复位时 LPCKSEL 为 0，报 RC 速率——和硬件一致。
- **故意不做的**：RC 那条路还有个分支，固件存了校准值就按校准频率除而不是除以
  10k（`drv_common.c:351`），模型不产生那个值；对着验的固件跑在晶振上。
- **回归检查**：`notes/qtest-sifli.sh` 的 [15] 查它"在计数"，并跟着
  `RTC_CR.LPCKSEL` 在 32768（晶振）/ 10000（RC）之间变频。这一段原先在
  `verify-sifli.sh` 的第 6 项，搬到 qtest 才进得了 CI（verify 的第 7 项会跑
  qtest-sifli.sh，所以那边的覆盖没丢）。
- **I2C 仍然没有模型**（I2C1–4 在 `0x5009c000` 起，见 §13.1），所以触摸屏在 QEMU
  里用不了——只是现在会干净地超时失败，不把系统挂住。验 LCD 用不着它。

> 日志里 `[152104]` 那样的数字**不是毫秒**：`ulog_get_tick()`
> （`drv_common.c:750`）直接返回 GTIMR 原值，除以 32768 才是秒。当毫秒读会以为
> 固件快了几十倍。

---

## 4. USART 的薄语义

`hw/char/sifli-usart.c`。寄存器定义在 `drivers/cmsis/Include/usart.h`，
**五个系列共享**，所以一个模型管全部。要点：

- **TXE（ISR bit7）恒 1**：没有 FIFO 要填、没有移位寄存器要排空。
- **TC（ISR bit6）复位时置 1，每次写 TDR 后再置 1**，可以由 ICR 清除。
  控制台每一行输出都走这条：
  ```c
  UART_INSTANCE_CLEAR_FUNCTION(&handle, UART_FLAG_TC);   /* ICR |= TCCF */
  __HAL_UART_PUTC(&handle, c);                           /* TDR = ch */
  while (__HAL_UART_GET_FLAG(&handle, UART_FLAG_TC) == RESET);   /* drv_usart.c:263 */
  ```
  最后那行是**没有超时的裸 while**，TC 不置位就是死循环。
- **BUSY（ISR bit16）和 EXR 读 0**：永远没有正在进行的传输。
- 接收一个字节缓冲，RXNE 置位、读 RDR 清位；没被读走时告诉 chardev 先别送。
- `BRR`/`CR2`/`MISCR`/`GTPR`/`RTOR` 只存不生效——波特率、流控、采样点
  都不影响主机看到的一个字符。`CR3` 也只存，**只有 `DMAR`(bit6) 例外**：
  它决定 `dma-rx` 那条请求线举不举（见 §5.1）。

**不用做**：TEACK/REACK（52x 的 `UART_CheckIdleState` 把等待 `#if 0` 掉了）、
DRDR/DTDR（HAL 从不碰）、FIFO 使能（HAL 里没有 `FIFOEN` 写入）。

### 中断

使能位在 CR1，但**状态位不在同一位置**（`PEIE` 是 CR1 bit8，`PE` 是 ISR bit0），
所以中断条件不能写成两个寄存器相与，要一个个判。ICR→ISR 的位映射同理
（`TCBGTCF` 是 ICR bit7，`TCBGT` 是 ISR bit25），按名字翻译而不是移位。

### 接收走 DMA，不走 RXNE 中断

这块板子的控制台**把 uart1 按 DMA 模式打开**（`board.conf` 的
`CONFIG_BSP_UART1_RX_USING_DMA=y`），所以模型里那条 RXNE→中断的路固件根本
不走：字节要经 USART 的请求线 → DMA 通道 → 再由 IDLE 中断反推收到了多少，
才能到 shell。只把 USART 做对，表现是提示符打得出来、敲什么都没反应。

---

## 5. DMA 控制器

`hw/dma/sifli-dma.c`。寄存器定义在 `drivers/cmsis/sf32lb52x/dmac.h`（**每系列
一份**，和 USART 不同），基址由 machine 传进来：DMAC1 在 HPSYS、DMAC2 在 LPSYS。
八个通道，每个通道是一段 0x14 字节的平铺寄存器，不是结构体数组。

### 5.1 请求是电平，不是脉冲

DMA 只有 **64 条按请求号索引的输入线**，不是每个通道一条。这是照着
`CSELRn` 那个请求 mux 建的：控制器只知道"请求号 n 到了"，要知道该哪个通道
干活，得去查 `CSELRn`——**而且每次搬运都重查**，因为 HAL 的
`DMA_AllocChannel` 会挑第一个空闲通道，请求随时可能换通道。

请求线是**电平**：源一直举着，直到消费它的那次搬运发生。这一点是必需的，
不是风格问题——控制台重开时固件会先 `HAL_DMA_Start` 再置 `CR3.DMAR`，而在
那之前到达的字节只能靠"线还举着"被重新发现。所以 `CCR` 的 EN 0→1 和
`CSELRn` 的写入都要**重扫一遍所有请求线**，否则重开后敲的第一个字符会烂在
RDR 里，直到下一次按键才出来。

### 5.2 重入守卫是必需的

搬运一字节靠的是 `address_space_read(CPAR)`，而 CPAR 指向 USART 的 RDR——
那个读会清 RXNE 并调 `qemu_chr_fe_accept_input()`，它**同步**回调进
`sifli_usart_receive()`，把下一个字节放进 RDR、又把请求线举起来，于是嵌套
进入 DMA 处理。

没有守卫的话，嵌套那次会用**还没推进的 CM0AR** 写第二个字节，悄悄把环形缓冲
写坏。所以：

- `req_level[n] = level` 在守卫判断**之前**记录——否则嵌套那次的边沿丢了，
  它带来的那个字节就没人搬。
- 外层在守卫内**循环**搬运，直到请求线落下，把嵌套送来的字节在外层 CM0AR
  已经推进之后消费掉。

每次调用最多搬 `SIFLI_DMA_MAX_DRAIN` 个，剩下的交给 bottom half——粘一大段
文本进来时不该把线程占在那儿搬完。

### 5.3 环形模式必须重装地址，不只是重装计数

CIRC 绕回时，硬件**把 CM0AR 也恢复到通道使能时的值**，不只是 `CNDTR`。
只重装计数的话，缓冲区一绕回就一路往后写：uart1 的 DMA 目标是 RT-Thread 的
64 字节接收环（`RT_SERIAL_RB_BUFSZ`），一次粘 420 字节进去就会踩掉后面
356 字节的堆——表现是 RT-Thread 在 `rt_thread_timeout` 里断言失败然后 hardfault。
所以使能时锁存 `cpar_reload`/`cm0ar_reload`，绕回时一起恢复。

> 顺带一提：64 字节的环装不下 420 字节的粘贴，**真板子也一样丢**（IDLE 只在
> 一批结束时来一次）。这种丢数据是固件缓冲区的性质，不是模型的锅；模型要保证
> 的是**不越界**。

### 5.4 不做的事

- **TEIF 不置位**：模型能搬的东西不会失败。
- **优先级（PL）、CBSR、DBGSEL 只存不生效**：没有竞争的总线要仲裁，也没有
  总线错误要上报。
- **HT/TC 标志照置**（`ISR` 里可见），但通道 IRQ 只在对应的 `TCIE`/`HTIE`/`TEIE`
  置位时才拉高。UART RX 这条路上两个回调都没人接（尽头是弱符号空函数），
  真正干活的是 IDLE。
- **MEM2MEM 会在置 EN 时直接跑完**：这类通道没有外设来举请求线。`drv_lcd_fb.c`
  用了它。CIRC+MEM2MEM 会无限重写同一块缓冲，所以同样受搬运预算限制。

---

## 6. EZIP（解压加速器）

LVGL 的 GPU 后端、`middleware/ezipa_dec/` 和 DFU 都走它。窗口
`0x50006000`–`0x5000609c`（40 个字），IRQ 89。

### 6.1 完成握手：两个中断寄存器，且都不受 INT_EN 管

HAL 有两条路，等的**不是同一个寄存器**：

| 路径 | 等什么 | 出处 |
|---|---|---|
| `HAL_EZIP_Decode`（轮询） | `while (0 == INT_MASK)` | `bf0_hal_ezip.c:599` |
| `HAL_EZIP_Decode_IT`（中断） | `INT_EN = END\|…`，ISR 里读 `INT_STA` | 同上 `:678`、`:496` |

所以模型完成时**同时**置 `INT_STA` 和 `INT_MASK`，两个都是 W1C、也都不受
`INT_EN` 影响；`INT_EN` 只管中断线（`INT_EN & INT_MASK` 的按位与，电平）。
只做一个是会挂的：轮询那条永远等不到，而它从头到尾没碰过 `INT_EN`。

`EZIP_CTRL` bit0 写 1 启动，模型**在这次 MMIO 写里就解完**——写返回时
`INT_*` 已经置好、IRQ 89 已经拉高，固件接着去取信号量就能过。

### 6.2 输入长度：52x 上 `SRC_LEN` 根本没人写

`HAL_EZIP_MULTI_BLOCK_DECODING_SUPPORTED` 只在 `EZIP_EZIP_PARA_LAST` 存在时
才定义（`drivers/Include/bf0_hal_ezip.h:36`），而 52x 的 `ezip.h` 里没有这个
字段——于是 `HAL_EZIP_Decode` 里写 `SRC_LEN` 的那段**被编译掉了**，寄存器
恒为 0。这不是模型抄近路，是这颗芯片的真实行为。

模型于是从位流自己读长度：

| 模式 | `SRC_ADDR` 指着什么 | 长度从哪来 |
|---|---|---|
| GZIP | 裸 deflate 流 | zlib 解到 `Z_STREAM_END` 自己停 |
| LZ4 | `[LE u32 块长][LZ4 块]` | 先读那 4 字节 |
| EZIP 私有 | 私有比特流 | 交给工具，它自己认 |

**"GZIP 模式"吃的不是 gzip 流，是裸 deflate。** SDK 的资产生成命令是
`ezip -gzip <file> -length -noheader`（`docs/source/zh_CN/app_note/ezip_tool_usage.md`
里写着"4 字节长度后面的都是 gzip 压缩数据，即直接作为硬件 ezip 的输入部分"）
——头已经被剥掉了。示例资产印证了这点：`assets/gzip_input.dat` 里看着像 gzip
头的 `1f 8b 08 08` 其实在注释掉的行里，真正的流是个匿名 deflate 块，而且末尾
还挂着 gzip 的 CRC32/ISIZE（deflate 解码器根本走不到那儿，硬件也照样不管）。
所以模型是 `inflateInit2(&z, -15)`：喂一个完整的 gzip 流给它反而会解错，
这一点和硬件一致。

读取上界是设备属性 `window-bytes`（默认 64 KiB），再被夹到源地址所在内存
区域的末尾。这里有个 `address_space_translate()` 的坑：**它的 `len` 是入出
参**，进来是调用者的上界、出去被 `physmem.c:395` 夹到区域尾；不初始化就拿
栈上的垃圾去夹，结果是随机的 `len == 0`。

### 6.3 私有格式：真的去跑 SDK 的 ezip 工具

SDK 里没有私有格式的软件解码器（`external/ffmpeg/libavcodec/ezipdec.c` 只是
把 packet 拷进帧缓冲）。所以机器属性 `ezip-tool=`（或环境变量
`SIFLI_EZIP_TOOL`）指向 `tools/png2ezip/ezip_linux`，模型用 `g_spawn_sync()`
同步调它：

```
ezip -convert in/image.ezip -spt 1 -dpt 1 -binfile 1 \
     -dec_off_no_header 0 -outdir out
```

两个细节是拿 SDK 自带资产标定出来的，不是猜的：`-binfile 1` 会把扩展名换成
`.bin`（产出叫 `image.bin` 而不是 `image.ezip`）；产出文件前面还有 4 字节容器
头（宽高大端 u16），要跳过再写进 `DST_ADDR`，顺便用它填 `DB_DATA1`。

**这次调用是在 MMIO 写处理里同步阻塞的**——开发用模型可以接受，但它会让
vCPU 停住几十毫秒。只有私有格式走这条路。工具路径没给或者找不到，就记一笔
`LOG_GUEST_ERROR` 照常完成，不挂死。

### 6.4 不做的事

- AEZIP / animation（`AEZIP_CTRL`、`FRAME_*`、`SEQ_NUM`）只存寄存器。
- `OUT_SEL = EPIC` 配非私有格式：HAL 自己就挡掉了（`bf0_hal_ezip.c:103`），
  模型也记一笔 `LOG_GUEST_ERROR`，用 `END` 收尾——**只有私有格式带容器头，
  才知道图有多大**，`ezip_coeng_store()` 靠它算每像素几字节（见 §6.5）。
- `IN_SEL` 选 NAND/QSPI 输入：只做 AHB 输入。
- 多块解码：52x 上本来就编译掉了。

### 6.5 `OUT_SEL = EPIC`：解码结果不落内存，喂给 EPIC 的 2D 流水线

`OUT_SEL = EPIC` 时 `DST_ADDR` 根本不参与——像素进的是 EPIC 的输入，不是地址
空间。所以模型不能像 AHB 那样写内存，而要把解出来的帧**留在 EZIP 里**，等
`epic_run()` 来取。两个设备之间是一条 QOM link（EPIC 的 `ezip` 属性，机器在
`hw/arm/sifli-sf32lb52x.c` 里接上），**不是**让 EPIC 去读 EZIP 的 MMIO 窗口
——那不是内存，按内存读什么也解不出来。

谁吃这份数据由 EPIC 侧决定：`COENG_CFG.EZIP_EN`(bit0) 置起，`EZIP_CH_SEL`
(bits[2:1]) 选中某一层的 channel。**channel 不是图层号**：HAL 的 `LayerIdx2CH()`
（`bf0_hal_epic.c:1445`）把 VL 编成 0、L0 编成 1。52x 上
`EPIC_COENG_CFG_EZIP_EN` 存在，所以 HAL 走 `COENG_CFG` 这条路，不是每图层
`CFG.EZIP_EN` 那条（`bf0_hal_epic.c:1725`）。

**窗口（`START_POINT`/`END_POINT`）在 EPIC 输出模式下确实会写进寄存器**
（`bf0_hal_ezip.c:288-324`），列在 bit31:16、行在 bit15:0。硬件只解这一块矩形
——图层被画布裁掉时用不着整幅图。模型这边宿主工具总会吐整幅，所以按窗口裁。
窗口原点是源图的 `(start_col, start_row)`，而图层的 `TL_POS` 是源图 `(0,0)`
落画布的位置，于是**未缩放**时帧落在 `TL_POS + 窗口原点`。带缩放时帧不走这个
落点，而是和普通图层一样过 VL 的变换单元（§7.7）：输出像素先反算成源坐标
`s`，再取帧内 `(s - 窗口原点)` 那个像素，落在窗口外的就不画。HAL 允许缩放
EZIP 图层、只拒绝旋转它（`bf0_hal_epic.c:6196-6204`），缩放时 `SCALE_RATIO`、
`SCALE_INIT`、`EXTENTS`、TL/BR 照常下发。

**像素按 ARGB8888 喂给 EPIC。** HAL 把 EZIP 图层的格式一律折成
`EPIC_L0_CFG_FMT_ARGB8888`（`bf0_hal_epic.c:722`），而工具产出的是
565/565A/888/888A（见 §6.3 的容器头）。模型按容器头 + 解码长度定出每像素
几字节：

| 字节/像素 | 容器头 format | 布局 |
|---|---|---|
| 2 | 4 | RGB565（无 alpha） |
| 3 | 4 | RGB888（B,G,R） |
| 3 | 5 | ARGB8565（565 小端 + alpha） |
| 4 | 5 | ARGB8888（B,G,R,A） |

565 系按位复制扩成 8888——EPIC 再打包回 565 输出时无损；888/888A 与 EPIC 的
字节序本来就一致，直接搬。**认不出的 format 记一笔并丢帧**，绝不按别的格式硬解。

**解码出来的 alpha 一定会参与混合**，哪怕 `Ln_CFG` 里 `ALPHA_SEL` 和
`ALPHA_BLEND` 都没置。HAL 给这种图层传的是 `EPIC_LAYER_OPAQUE`，于是两个位
都不置（`bf0_hal_epic.c:3098`），按普通图层的读法会被当成全不透明。但 SDK 自带
的 `example_ezip` 用例（`example/hal_example/src/example/example_ezip.c`）拿
资产自己算了期望像素，期望值就是按解码 alpha 混到背景上的，所以真机确实在混。

**`OUT_SEL = EPIC` 的作业永远用 `END` 收尾，绝不用错误位。** EPIC 清它的
"EZIP 在跑"标志**只**从完成回调里做（`EPIC_EzipCpltCallback`，
`bf0_hal_epic.c:6138`），而 `HAL_EZIP_IRQHandler` 只在 END 时调这个回调
（`bf0_hal_ezip.c:530`）——报错误位等于让固件永远卡在
`while (epic->coeng_state)`（`bf0_hal_epic.c:4688`）。所以模型这一侧无论
解不出来（缺工具、坏流）还是存不下（认不出的格式），都记一笔 `LOG_GUEST_ERROR`
再用 `END` 收尾：**故障通过日志和"EPIC 找不到帧"两层可见**，而不是靠一个会挂死
的状态位。AHB 那条路的调用方是轮询 `INT_MASK` 的，照旧报 `BTYPE_ERR`/`ETYPE_ERR`
（qtest [7d] 钉着）。

**co-engine 开着却没有解码帧**（EPIC 先跑、EZIP 没跑或解失败）：EPIC 记一笔
`LOG_GUEST_ERROR` 并跳过该图层——绝不拿 `SRC`（那是压缩流地址）当像素读。

---

## 7. EPIC（2D 图形引擎）

窗口 `0x50007000`–`0x5000715c`，IRQ 62。

### 7.1 没有操作码

`EPIC_RUN()` 就是 `COMMAND |= START`（`bf0_hal_epic.c:522`），而 `COMMAND`
只有 `START`(bit0) / `RESET`(bit1)。**硬件没有"这次干什么"这个字段**——一次
作业是什么完全由当时活着的寄存器决定：`CANVAS_*` + `AHB_*` 给出目标矩形、
内存地址、行距和输出格式，各 `Ln_CFG.ACTIVE` 决定哪些图层参与。

模型就照着读题：先把 `CANVAS_BG` 铺满矩形（除非置了 bypass），再按硬件顺序
（L0 是背景先画、VL 是前景后画）把每个 ACTIVE 图层混上去。填充、拷贝、混合
在硬件上本来是同一条路，在模型里也是——`HAL_EPIC_Copy_IT` 和
`HAL_EPIC_BlendStart*` 的差别只是留下的寄存器不同。

### 7.2 为什么必须同步完成

`EPIC_WaitDone()` 在 52x 上是裸 `while (STATUS != 0)`（`bf0_hal_epic.c:4431`）。
更麻烦的是 `EPIC_WaitValidInstance()`：`STATUS != 0` 且存在 RAM shadow 时，它
会把后续寄存器写**改道到 shadow**，然后再调 `EPIC_WaitDone()`——于是模型只要
报"忙"，固件就永久自旋，而且看栈还看不出来。所以 STATUS 恒为 0：活在这次写
里干完。

### 7.3 alpha 的极性由源格式决定

`Ln_CFG` 里有 `ALPHA_SEL`(bit4)、`ALPHA[12:5]`、`ALPHA_BLEND`(bit31)。
HAL 的写法（`bf0_hal_epic.c:1897`、`:4025`）是：

- 源带 alpha 通道（非 RGB565/RGB888/MONO）→ 置 `ALPHA_BLEND`，逐像素取 `px.a`
- 源不带 alpha → 置 `ALPHA_SEL`，用 `ALPHA` 常量
- 整层不透明 → 两个都不置，`ALPHA` 填 255

注意 `ALPHA` 字段在**前两种情况下都会被写**，所以判断顺序是
**先 `ALPHA_SEL`、再 `ALPHA_BLEND`**；反过来会把该逐像素混的图层当成常量混，
而常量恰好也是调用者传的那个值，看起来"差不多对"——qtest 那节就是钉这个的。

### 7.4 `CANVAS_BG` 的两个 bypass 位

`BG_BLENDING_BYPASS`(bit24，SDK 里就是这个叠了两个 BG 的拼写)、
`ALL_BLENDING_BYPASS`(bit25)，**任一个置起都不清画布**。拷贝那条路置
`ALL_BLENDING_BYPASS` 且把 `CANVAS_BG` 留在 0——照清不误的话，会把马上要读的
目标先擦掉。

### 7.5 仍不做的事（记一笔、照常置 STATUS=0，但画出来的是错的）

镜像（`VL_MISC_CFG.H_MIRROR`/`V_MIRROR`：HAL 支持，但 SDK 自己的驱动没有一处
置它）、YUV 输入、dither、`MASK_*`、A8/A4/A2/L8 源格式（要色彩坐标引擎和调色
板）、co-engine 图层的**旋转**（HAL 自己就拒，见 §6.5），以及
`AHB_CTRL.DESTINATION = LCD`（只做写内存）。VL 的旋转和缩放、co-engine 图层的
**缩放**都**不在**这一类了，见 §7.7。

**遇到这些也照常完成**是刻意的：作业用了它们，固件就在等它，卡死比画错更难
查。L1/L2 在这颗芯片上不存在（HAL 为 `SF32LB52X` 定义 `EPIC_L2_L1_INVALID`，
`bf0_hal_epic.c:154`），寄存器存着但够不到。

### 7.6 co-engine：图层的输入来自 EZIP（已做）

`COENG_CFG.EZIP_EN` 置起时，被 `EZIP_CH_SEL` 选中的那一层**不从 `SRC` 取像素**
——它的输入是 EZIP 解码出来的帧。`VL_SRC` 这时仍然指着压缩流，照普通图层读会
把位流当像素画。模型从 EZIP 设备的 link（属性 `ezip`，机器接的）取帧，几何和
alpha 的规矩见 §6.5；没有帧可用时记一笔并跳过该层。

只有 `CH_SEL` 指到的那一层吃 co-engine，其余照旧读 `SRC`。带缩放时这条路和
普通内存图层共用同一套反算（`epic_vl_transform_apply()`），只是采样点换成帧内
下标——见 §6.5 和 §7.7。HAL 拒收 co-engine 图层的旋转，所以旋转那条不用担心。

### 7.7 VL 的旋转与缩放（已做）

VL 是 52x 上唯一带变换单元的图层：`VL_ROT`、`VL_ROT_M_CFG1/2/3`、
`VL_SCALE_RATIO_H/V`、`VL_SCALE_INIT_CFG1/2`、`VL_EXTENTS`、`VL_MISC_CFG` 都是
它专属的（L0 没有这套，L1/L2 在 52x 不存在）。HAL 在 CPU 上把正变换算完，把
结果当寄存器留给硬件（`bf0_hal_epic.c:2506-2966` 算、`:3145-3250` 写）；硬件
反过来按输出像素逐个反算源坐标。模型做的是同一件事。

**读题。** 这些寄存器一旦不表示"原样"，VL 图层就走变换那条路：

| 寄存器 | 意思 |
|---|---|
| `ROT.DEG_FORCE` / `ROT_DEG` / `ROT_M_CFG1.M_MODE` | 有旋转 |
| `SCALE_RATIO_H/V` ≠ `0x10000`、`SCALE_INIT_CFG1/2` ≠ 0 | 有缩放 |

三者都不成立时走原来的矩形快路径，逐字节结果不变——[8]/[8b] 和 `example/hal/epic`
那四个采样点钉的就是这条。

**反算。** 对画布上属于该图层的像素（相对左上角偏移 `l`）：

```
r        = (SCALE_INIT + l * SCALE_RATIO) >> 16          // 未旋转的源坐标
source   = R(-ROT_DEG) * (r - PIVOT) + PIVOT - SRC_TL    // 有旋转时
```

`r` 的表达式不是猜的：`EPIC_CalcDecImgArea()`（`bf0_hal_epic.c:3755`）给
co-engine 算窗口起点时用的就是 `(start_col * scale_x + scale_init_x) >> 16`，
`start_col` 正是像素相对图层 `TL_POS` 的偏移。旋转那一步是 HAL 正变换
（`TRANSFORM_POINT`，`bf0_hal_epic.c:1301`）的逆：源点 `u` 落画布
`TL + (R(θ)·(u + SRC_TL - PIVOT) + PIVOT) / 缩放`。`ROT_M_CFG2` 是 pivot、
`ROT_M_CFG3` 是旋转前的源图左上角，都以图层左上角为原点、11 位有符号
（52x 起才有符号，55x 的正寄存器容不下负值，HAL 为此加了 `d2/d3` 平移，
`bf0_hal_epic.c:2627-2680`）。

**角度与 sin/cos。** `ROT_DEG` 的单位是**整度**：HAL 的输入是 0.1 度，它先除以
10 再下发，遇到恰好 0/90/180/270 会 +1 让象限确定（`bf0_hal_epic.c:2551`）。
HAL 还把 `|sin|`/`|cos|` 强行放进 `VL_MISC_CFG` 的 `SIN_FORCE_VALUE`/
`COS_FORCE_VALUE`（Q1.12，即 HAL 里 Q1.15 表右移 3 位，`EPIC_SIN_COS_FRAC_BIT`
与 `EPIC_VL_MISC_CFG_SIN_FRAC_BIT` 之差），符号由 `ROT_DEG` 的象限给。模型照
这个读；只有 `DEG_FORCE` 没置时才退回去按角度现算。

**采样是最近邻。** `VL_FILTER`/`CFG.FILTER_EN` 不是插值开关：那个 FILTER 寄存器
装的是 R/G/B，HAL 用它给 A8/L8 源替换颜色（`bf0_hal_epic.c:3128`），寄存器图里
没有任何选插值核的位。源坐标直接截断（`>> 16`），不四舍五入。

**源外怎么办。** 只画 `EXTENTS` 框住的那块源区——它就是 HAL 裁好的
`clip_area`，也是它交给 co-engine 的窗口边界（`bf0_hal_epic.c:3145`、
`:3755`）。反算落在框外的输出像素原样留下，不取样、不混。

**co-engine 图层走同一套反算。** `COENG_CFG` 把某层的输入换成 EZIP 解码帧时
（§6.5/§7.6），若该层带缩放，反算不变，只是把"在 `SRC` 里按 `(sx, sy)` 取样"
换成"取帧内 `(sx - start_col, sy - start_row)`"。帧本身就是 HAL 按同一份
`clip_area` 裁出来的窗口（`EPIC_CalcDecImgArea()`，`bf0_hal_epic.c:3696-3790`），
所以帧边界就是读取的边界，落在帧外的输出像素不画。旋转对 co-engine 图层不可达
（HAL 入口就拒，`bf0_hal_epic.c:6200`）：真遇到带角度的寄存器组，模型记一笔并
按未变换画，不猜。

**SCALE_RATIO 为 0 算"没缩放"。** HAL 给不缩放的图层写的永远是
`EPIC_SCALE_1`（`bf0_hal_epic.c:3375-3379`，`EPIC_ContResetVideoLayer` 也一样，
`:3479-3483`），从不下发 0。按变换读 0 会把整个矩形塌到一个源像素上，所以模型把
0 当作"没有缩放"——[9b] 那段手写的寄存器组正是这种情况。

**验证。** `notes/qtest-sifli.sh` 的 [8c] 各跑一次 90° 旋转和 2× 放大，期望像素
手工可推（见那节的注释）；[9b] 钉 co-engine 的未变换那条，[9d] 在同一个窗口上
加一条横向 2× 钉带缩放的 co-engine 那条。真机固件那一侧现在
有了 co-engine 缩放的样本：`example/rt_device/gpu/single_mode` 的
`scale_down_demo()` 依次跑 `multiple = 1/2/3`（`src/main.c:534-552`），模型把
`buffer0` 抓出来、宿主解码同一份资产逐像素比对——1× 与改动前逐字节相同，2×/3×
与寄存器反算的采样完全一致（跑法见 `notes/build-and-verify.md`）。旋转仍没有
可比对样本：SDK 里真做旋转的例程（`single_mode` 的旋转+mask、LVGL 那条
`lv_draw_epic_img.c` → `HAL_EPIC_Adv`）只把结果打到 LCD 上，既不打印校验和也不
比期望像素，`docs/source/en/hal/epic.md` 也没有逐像素算法的描述。

---

## 8. 内存：PSRAM 与两条地址通路

### 8.1 PSRAM 就是一块 RAM

`mem_map.h` 把 PSRAM 挂在 QSPI1 的容量上（`PSRAM_BASE 0x60000000`，
`PSRAM_SIZE = BSP_QSPI1_MEM_SIZE`），所以容量是**板子属性**，做成 machine 属性
`psram-size`，默认 16 MB——`sf32lb52-lcd_a128r16` 的板子配置就是这个数。

模型侧只有 `memory_region_init_ram()`。把它拉起来是 MPI 的事（读延迟、写延迟、
QSPI 模式），HAL 问的那些寄存器 RCC/MPI 那几张表已经在答了；固件认为它起来了
之后，对这块内存做的事就是普通的 load/store。

**`hello_world` 用不到它**：它的 `.RW_PSRAM_NON_RET` 段长度是 0，所以第 9 项检查
从来没碰过 PSRAM。`example/rt_driver` 和 `example/hal/epic` 才是真往里放东西的
（EPIC 的 0x868a0 字节三个 buffer；rt_driver 的 351000 字节显存 = 390×450×2，
RGB565），加 PSRAM 就是为了让它们能跑。

### 8.2 图层 `SRC` 和 `AHB_MEM` 不是一回事

**同一个 buffer，交给 EPIC 的两个寄存器，地址形式不一样：**

| 寄存器 | HAL 怎么写 | 出处 |
|---|---|---|
| 图层 `VL_SRC`/`L0_SRC`/`MASK_SRC`/`Y_SRC`… | `HCPU_MPI_SBUS_ADDR(config->data)` | `bf0_hal_epic.c:1885,2070,3050,3357,3535,4015` |
| `AHB_MEM`（输出） | 裸指针 `(uint32_t)output->data + offset` | `bf0_hal_epic.c:2255,2377` |

原因是 MPI 要能分辨"这次访问是 CPU 发的还是外设发的"：外设读 flash 时 HAL 把
地址 **+0x50000000**（`HPSYS_MPI_MEM_CBUS_2_SBUS_OFFSET`）再给它，落进 SBUS 窗口。
`HCPU_MPI_SBUS_ADDR` 只对 `[0x10000000, 0x20000000)` 里的地址做这个加法，别的
原样传出去——所以 PSRAM 指针（0x6040_0000 起）不会被加，`AHB_MEM` 更是根本不走
这个宏。

模型要把它倒回来，因为模型读 guest 用的是 CPU 视角的地址空间。
`sifli_sbus_to_cpu_addr()`（`include/hw/misc/sifli-sbus.h`）就干这个。

**还有第三条路：根本不经过地址。** 图层被 EZIP co-engine 接管时（§6.5/§7.6），
像素来自 EZIP 设备里那份解码帧，`SRC` 只是压缩流地址、不参与取像素。这条路上
既没有 SBUS 别名也没有 `AHB_MEM` 的事，两个设备之间是一条 QOM link。

### 8.3 翻译窗口为什么只有 4 MB

**这是 PSRAM 加进来之后才暴露的**：SBUS 窗口从 0x6000_0000 起，PSRAM 也在
0x6000_0000，两者重叠。`HCPU_MPI_SBUS_ADDR` 加出来的地址和 PSRAM 自己的地址，
**光看地址分不出来**——真硬件是按 MPI 实例分的，一个平坦地址空间里表达不了。

好在 SDK 自己的布局把它们错开了：链接脚本的 `__PSRAM_BASE` 是 **0x6040_0000**，
即 PSRAM 的数据区从 4 MB 处才开始。所以窗口取 `[0x6000_0000, 0x6040_0000)`：

```c
#define SIFLI_SBUS_FLASH_SIZE   0x00400000u
```

- EZIP 例程的资产在 flash 0x1007_71EC → 寄存器里是 0x6007_71EC → 减回
  0x1007_71EC，**在窗口内**，对。
- EPIC 例程的图层 buffer 在 PSRAM 0x6046_E200 → 寄存器里还是 0x6046_E200
  → 窗口外，不动，对。

**超过 4 MB 的 flash 地址会被翻错**（`addr - 0x50000000` 落到 PSRAM 上），
这棵树里没有固件这么干。真要做对得给外设单开一个 SBUS 的 `AddressSpace`，
把两条通路在地址空间层面分开——现在不值得，但要知道这个洞在哪。

---

## 9. LCDC 与面板（显示）

`hw/display/sifli-lcdc.c`（控制器，machine 建，`0x50008000` / IRQ 63）+
`hw/display/sifli-panel.c`（面板，`-device`）。画面直接进 QEMU 的显示控制台，
`-display sdl` 就是屏幕。

### 9.1 两条互不相干的路径

```
(a) 命令/读数路径  LCD_WR + LCD_SINGLE   —— 控制器跟**面板**说话，不碰内存
(b) 帧路径         COMMAND.START         —— 控制器是 AHB 总线主控，自己取显存
```

**(b) 不经过我们的 DMA 模型**：LCDC 自己按 `LAYER0_SRC` 去 AHB 上取像素
（`bf0_hal_lcdc.c:1692`），所以模型直接用 `dma_memory_read()` 读 guest 内存。
这也是为什么刷屏链路完全不依赖 DMAC——调试时别往 DMA 那边找。

### 9.2 帧路径

固件侧 `LayerUpdate()` 配 `LAYER0_CONFIG`（源格式 + **字节**行距）、`LAYER0_SRC`、
`TL/BR_POS`，然后 `COMMAND.START` 一写，硬件送完一整帧才置 `IRQ.EOF`。

**`LAYER0_SRC` 指的是这一块自己的首像素，不是显存基址。** 取数地址是

```
SRC + (y - y0) * stride + (x - x0) * bytes_per_pixel
```

`TL/BR` 只决定画到屏幕哪儿。这条不是我猜的：`SetupLineIrq()`
（`bf0_hal_lcdc.c:1150-1165`）为了用 bus monitor 抓住这一层的读，先把
`data_area` 的角点减掉再算预期地址；`drv_lcd` 也是把脏矩形的首像素指针交给
`LayerUpdate` 的（`drv_lcd.c:1503`、`:3290`）。**照基址去读会画出一片黑**，
而且全屏刷新时两种解释恰好重合，所以只有部分刷新才暴露——写测试时专门验它。

两个字段容易记反：

| 字段 | 含义 |
|---|---|
| `LAYER0_CONFIG.WIDTH`（bit13，13 位） | **字节**行距，不是像素数（`:1661`） |
| `LAYER0_SRC_ADDR_Pos` | **0**（`lcd_if.h:291`），所以寄存器里就是裸地址 |

**源格式和输出格式是分开的**：`LAYER0_CONFIG.FORMAT` 是源（`drv_lcd` 按
`RTGRAPHIC_CTRL_SET_BUF_FORMAT` 设成 RGB565/RGB888），面板侧输出由 `LCD_CONF`
的 `SPI_LCD_FORMAT` 决定。转换是硬件做的，所以模型自己做 RGB888→RGB565。
RGB888 在内存里是 **B,G,R**（小端），别按 R 开头解。

**影子显存是必需的**，不是优化：LVGL 每次只送脏矩形，没有影子的话屏幕只会
留下最后那一小块。推画面用 `qemu_create_displaysurface_from()` 零拷贝包一层
（`ramfb.c` 的路子）+ `dpy_gfx_update_full()`，**不需要定时器**——UI 自己以
30–60 Hz 轮询 `gfx_update`。

**SBUS 别名要翻回来**：`LAYER0_SRC` 在 HAL 里过了一道 `HCPU_MPI_SBUS_ADDR()`
（`bf0_hal_lcdc.c:1727`），显存落在 flash 时寄存器里拿到的是 `+0x50000000` 之后
的地址。模型读 guest 走的是 CPU 视角的地址空间，所以取数前先过
`sifli_sbus_to_cpu_addr()` —— 和 EPIC 的图层 `SRC` 是同一件事，窗口怎么划、
那条 4 MB 边界为什么在那儿，见 §8.2、§8.3。PSRAM/SRAM 里的显存不在窗口内，
原样通过，而这棵树里所有显存都在那儿。

### 9.3 EOF 中断是命门，不是可选项

驱动匹配上之后 `drv_lcd.assert_timeout = 1`，而 `draw_core` 里
`rt_sem_take(&draw_sem, MAX_LCD_DRAW_TIME)` 一超时就 `RT_ASSERT(0)`
（`drv_lcd.c:1728-1736`）。而放行这个信号量的正是 EOF：

```
IRQ.EOF → HAL_LCDC_IRQHandler → LCDC_TransCpltCallback → SendLayerDataCpltCbk
        → rt_sem_release(&draw_sem)
```

所以模型不能"把 START 收下就算了"——**必须在写 START 时就把整帧做完并置 EOF**，
固件才不会被断言打死。`IRQ` 是**写 1 清**、不是读清：HAL 既裸轮询它
（`bf0_hal_lcdc.c:2026`）又把读到的值写回去清（`:5784`），读清会让轮询死循环。

固件会等的两个 BUSY 位（`STATUS.LCD_BUSY`、`LCD_SINGLE.LCD_BUSY`）**恒读 0**，
`WaitBusy()` 和 `WAIT_LCDC_SINGLE_BUSY()` 第一次判断就过，不必跑满超时。

### 9.4 面板为什么做成 `-device`

不同板子接不同型号的屏，**ID 和分辨率都不一样**，而在设备里硬编码就不是
"换命令行就能换屏"了：

```bash
-device sifli-panel                                   # 默认就是 a128r16 的 CO5300
-device sifli-panel,id=0x60834200,width=480,height=272
```

**面板比 LCDC 晚 realize，所以只能由面板反向注册。** `-device` 的处理在
`machine_run_board_init()` **之后**（`system/vl.c:2751` vs `:2716`），LCDC 在
自己 realize 时看不到面板。于是面板 realize 里反过来调
`sifli_lcdc_set_panel()`，把尺寸带过去。

**分辨率只能由面板给**：`LCD_CONF` 只有格式/接口字段、没有分辨率；
`CANVAS_TL/BR_POS` 写的是刷新脏矩形，不是屏的物理尺寸。忘了 `-device` 的话
模型打 `LOG_GUEST_ERROR`（带 "pass -device sifli-panel"），固件自己也会打
`unknow lcd!`，屏幕黑但不崩——两条都值得留痕。

面板侧**不建模 QSPI 协议、不建模命令序列、不建模面板 GRAM**：初始化的那一长串
`LCD_WriteReg` 收下丢掉，QEMU 控制台的 surface 就是 GRAM。

### 9.5 读数路径：面板对固件唯一要回答的问题

固件只是问"你是谁"。这一路要看清楚**模型怎么知道固件在读哪个寄存器**：

```
co5300.c:291   LCD_ReadID()  → LCD_ReadData(hlcdc, 0x04, 3)
co5300.c:415   LCD_ReadData()→ HAL_LCDC_ReadU32Reg(hlcdc, (0x03<<24)|(0x04<<8), buf, 3)
bf0_hal_lcdc.c:2793  HAL_LCDC_ReadDatas()  ── SPI 分支 ──
    清 SPI_IF_CONF.SPI_CS_AUTO_DIS(bit27)   ← 事务开始
    SendSingleCmd(addr, 4)                  → LCD_WR = 命令字
                                            → SPI_IF_CONF.WR_LEN = 3
                                            → LCD_SINGLE = WR_TRIG(bit1)
    置 bit27；SPI_IF_CONF ← RD_LEN=2
    LCD_SINGLE = TYPE|RD_TRIG(bit0|bit2)    ← 面板此刻把 3 字节摆上总线
    data = LCD_RD   → 按小端拆成 3 字节
```

两个关键点：

1. **读发生时 `LCD_WR` 里还留着命令字**（`SendSingleCmd` 先写 `LCD_WR` 再写
   `LCD_SINGLE`，中间没人碰）。所以模型能看见在读哪个寄存器：
   `reg = (cmd_word >> 8) & 0xffff`。字节数不在命令字里，在 `SPI_IF_CONF.RD_LEN`。
2. **`SendSingleCmd` 在 `bytes_gap_us > 0` 时逐字节写 `LCD_WR`**，那时只留最后
   一字节。所以模型盯 `SPI_CS_AUTO_DIS` 的 1→0 边沿重置累加器，并在
   `WR_LEN == 0` 时把字节左移拼起来。CO5300 走整字路径，但别的屏不一定。

**为什么"寄存器 → ID"就够覆盖所有能选的屏**：把 SDK 里 ~48 个面板驱动的
`ReadID` 过了一遍，这个 SoC 上能选的 SPI/QADSPI 屏只有两种写法——直接返回
寄存器读到的值（CO5300、ST7789H2），或返回驱动里写死的常量（GC9A01A、GC9107、
NV3041A、SH8603B、SPD2012、FT2308…）。两种情况下"面板返回自己的 ID"都是对的。
（例外是 ST7789V 那种对读到的值做位运算的，它在本 SoC 上不可选。）

⚠️ a128r16 这块屏的 ID 是 **`0x331100`**，不是 `0x530001`：`co5300.c:45-56` 按
模组挑 ID，`AM196Q…`/`H0198S…` 一个都没定义，于是落到 `#else` 分支。模型不能
偷懒——读回的 3 字节必须真的是 `0x331100`。

---

## 10. 加一个新外设

**纯配置的（GPIO 配置、PINMUX、LPSYS_CFG…）**：
1. 在 `sf32lb52x-periph.c` 里加一张 `SifliRegDef[]` 表
2. 在 `sf32lb52x_banks[]` 里加一条 `.name` / `.size` / `.regs`
3. 在 `sf32lb52x_reg_banks[]` 里加 `.bank` 和 `.base`
4. 基址宏加进 `include/hw/arm/sf32lb52x.h`

**先别急着写全**：只列会读的寄存器，其余交给 `-d guest_errors` 报出来。

**有行为的**：照 `hw/char/sifli-usart.c` 写一个 sysbus 设备，在 machine 里
`sysbus_mmio_map` + `sysbus_connect_irq`。跨系列共享的寄存器头（`drivers/cmsis/Include/`）
放 `hw/char` 或 `hw/misc`；每系列一份的（`drivers/cmsis/sf32lb52x/`）把基址做成
设备属性，由 machine 传进去——**绝不能在设备里硬编码**，因为 52x/57x 与 56x/58x
的 HPSYS/LPSYS 窗口是交换的。

---

## 11. 真实固件 bring-up 记录

用 `sf32lb52-lcd_a128r16` 的 `hello_world`（SDK 原样构建，固件一行不改）逐个
排查出来的。**这些用 `hello_qemu` 那种 semihosting 桩永远发现不了**——它一个
HAL 寄存器都不碰。

排查手法：固件跑起来后从 monitor 采 `info registers` 看 PC，两次采样相同就是
死循环，再用 `arm-none-eabi-addr2line` 把地址翻回源码行。

### 11.1 DWT 周期计数器（QEMU 的缺口）

PC 停在 `HAL_Delay_us_`（`bf0_hal.c:407`）：

```asm
ldr r1, [r2, #4]      @ start = DWT_CYCCNT
ldr r3, [r2, #4]      @ ← 卡死
cmp r3, r4
bcc.n ...
```

**QEMU 的 M-profile CPU 完全不实现 DWT**，`0xE0001000` 那一页被 `armv7m` 的
`nvic-default` 区域吞掉、恒读 0。而 `HAL_PreInit` 在能打印任何东西之前就会调
`HAL_RCC_HCPU_ConfigHCLK(240)` → `EnableDLL1` → `HAL_Delay_us(10)`。

补了 `hw/misc/armv7m_dwt.c`（和 `armv7m_ras.c` 并列，同挂 `CONFIG_ARM_V7M`）。
见 §11.2 关于它为什么必须挂进 armv7m 的 container。守卫是
`notes/qtest-sifli.sh` 的 [16]：`DWT_CTRL` 能读写（证明那一页真的归我们，
而不是被 `nvic-default` 吞掉），清过 `DWT_CYCCNT` 之后推一次时钟，计数器
必须往前走——冻住的话那一项挂。

### 11.2 armv7m container 的优先级陷阱

把 DWT 映射进 board 的 system memory **不生效**，尽管 `info mtree -f` 里能看到它：

```
e0000000-e000dfff (prio -1): nvic-default
e0001000-e0001fff (prio 0):  armv7m-dwt      ← 显示了，但访问到不了
```

原因在 `system/memory.c:2665`：

```c
if (subregion->priority >= other->priority) {   /* >= 不是 > */
    QTAILQ_INSERT_BEFORE(other, subregion, subregions_link);
```

**同优先级时后加入的插到前面。** `armv7m.c` 里 `board_memory` 先以 -1 加入、
`nvic-default` 后以 -1 加入，于是 `nvic-default` 先渲染、先占住整个 PPB，
`board_memory` 里的东西只能捡空隙。

正解：挂到 `armv7m.container` 上（该字段在 `include/hw/arm/armv7m.h` 里是
public），优先级给 0 —— 高于 `nvic-default` 的 -1。

**教训**：`info mtree` 会把重叠的两段都列出来，光看它会被骗。要确认一个
区域真的生效，得实际读写它。

### 11.3 RTC 的 LXT 使能位（分支走错）

越过 DWT 后卡在 `bf0_hal_lrc_cal.c:894`，轮询 BT MAC 的 `RCCAL_RESULT`。

根因不是 BT MAC，是**分支判断**。`bf0_hal_rtc.h` 对 52x 解析成：

```c
#define HAL_RTC_ENABLE_LXT()    hwp_rtc->CR |= RTC_CR_LPCKSEL   /* RTC_CR bit0 */
#define HAL_LXT_DISABLED()      (!(hwp_rtc->CR & RTC_CR_LPCKSEL))
```

`bsp_init.c` 先 `HAL_RTC_ENABLE_LXT()` 写这一位，几行后 `HAL_LXT_DISABLED()`
读同一位判断晶振在不在。RTC 块没建模 → 读回 0 → 判定"没有 LXT" → 走
`LRC_init()` → 卡在 BT MAC。

把 RTC 块建起来让这一位存住即可，**比为了这一位去建模 BT MAC 便宜得多**，
也更接近真实板子（板上有 LXT 晶振）。

### 11.4 MPI 的 CALCR.DONE

`HAL_MPI_OPSRAM_CAL_DELAY`（`bf0_hal_mpi_psram.c:1386`）：写 `CALCR.EN`(bit31)
启动校准，然后轮询 `CALCR.DONE`(bit8)。和 RCC 的 `HRCCAL1.CAL_DONE` 同型
——只要 `force1 = BIT(8)`。

MPI 整体是行为型设备（要走串行协议），但**启动阶段只需要握手位**：

| 位 | 含义 | 谁在等 |
|---|---|---|
| `SR.TCF` bit0 | 命令完成 | `HAL_FLASH_IS_CMD_DONE()` |
| `SR.SMF` bit3 | 状态匹配 | `HAL_FLASH_STATUS_MATCH()` |
| `SR.BUSY` bit31 | 忙 | `HAL_FLASH_IS_BUSY()` |
| `CALCR.DONE` bit8 | 校准完成 | `bf0_hal_mpi_psram.c:1386` |

### 11.5 RTC_ISR 的六个就绪标志

`RTC_EnterInitMode`（`bf0_hal_rtc.c:1009`）置 `ISR.INIT`(bit10) 后等
`ISR.INITF`(bit9)。把 `bf0_hal_rtc.c` 里所有 `while` 找出来后发现有六个
标志各有一个等待：

| 位 | 含义 | 行 |
|---|---|---|
| `ALRMWF` 0 | 闹钟寄存器可写 | 588, 635 |
| `ALRMF` 1 | 闹钟匹配 | 750 |
| `WUTWF` 2 | 唤醒定时器可写 | 164, 1093 |
| `RSF` 7 | 影子寄存器已同步 | 810 |
| `INITS` 8 | 影子寄存器已加载 | — |
| `INITF` 9 | 已进入初始化模式 | 1009 |

外加 `SHPF`(6) 是反向的——"有移位未完成"，等的是它**清零**。

**教训：`WUTWF` 同时有一个"等它清"的等待（1074 行），方向相反。** 看代码
才知道那个分支被 `if (hrtc->Instance->CR & RTC_CR_WUTE)` 保护着，而 WUTE 我们
从不置位，所以强制置 1 是安全的。**这类冲突必须读代码，猜不出来。**

### 11.6 AUDPRC 与 AUDCODEC（只列被等的寄存器）

音频这块**故意只建了极少的寄存器**，其余留白：

```
AUDPRC    DAC_PATH_CFG1 (0x54)   SRC_CH_CLR_DONE [29:28]   bf0_hal_audprc.c:563
AUDCODEC  PLL_CAL_CFG   (0xa4)   DONE (bit1)               drv_audcodec_m.c:457
```

理由：AUDPRC 光 EQ 系数就有几十个寄存器，AUDCODEC 更大，而固件启动时只会
**等**其中两个位。全部抄进表里既冗长又没有收益；留白之后，万一固件还需要别的，
`-d guest_errors` 会把它报出来，而不是被悄悄编造一个值掩盖过去。

AUDCODEC 的 PLL 校准还说明了一点：**不是每个循环都需要伪造终态**。它是个搜索：

```c
PLL_CAL_CFG |= EN;
while (!(PLL_CAL_CFG & DONE));
pll_cnt = PLL_CAL_RESULT >> PLL_CNT_Pos;
... 调整 FC_VCO 再来 ...
```

步长每轮折半（8→4→2→1→0），**四轮必然退出**，无论读回的计数值多离谱。所以
只要 DONE 位起来，剩下的让它"校准不收敛"就行——启动日志里那句
`fc 31, xtal 0, pll 0` 就是这个结果：钳在边界值退出，不卡。

**先判断循环是否本身有界，再决定要不要伪造终态。**

### 11.7 结果

`sf32lb52-lcd_a128r16` 的 `hello_world`，SDK 原样构建、一行不改：

```
Serial:0,Chip:0,Package:7,Rev:7  Reason:00000000
[I/drv.adc] Get ADC configure fail, use default calibration
 \ | / - SiFli Corporation ... 2020 - 2022 Copyright by SiFli team
[I/drv.rtc] RTC use LXT RTC_CR=00000001
[I/drv.audprc] HAL_AUDPRC_Init res 0
[I/drv.audcodec] HAL_AUDCODEC_Init res 0
[I/TOUCH] Regist touch screen driver
[I/FAL] Flash Abstraction Layer (V0.5.99) initialize success.
...
Hello world3!
msh />
```

跑到 `main()` 并停在 RT-Thread 的 msh 提示符上。`notes/verify-sifli.sh` 第 9 项
就是跑这个。

### 11.8 规律

到目前为止**每一个坑都是同一个形状**：没建模的寄存器读回 0，固件据此做了
一个真实硬件上不会做的判断，然后走进死循环或错误分支。所以薄模型的重点
从来不是"存得住写入的值"，而是**让固件读到它会做的那个判断所需的终态**。

---

## 12. 排查

**看固件碰了哪些没建模的地址**：

```bash
./qemu-system-arm -M sf32lb52x -nographic -kernel fw.elf -d guest_errors
```

未列出的寄存器偏移会以 `LOG_GUEST_ERROR` 打出来。

**想知道还没建的外设被访问了没有**：临时加

```c
create_unimplemented_device("sifli.xxx", BASE, SIZE);   /* 需要 hw/misc/unimp.h */
```

**确认映射对了**：

```bash
./qemu-system-arm -M sf32lb52x -display none -monitor stdio -serial none -S
(qemu) info mtree
```

应能看到 `sf32lb52x.hpsys_rcc`、`sf32lb52x.pmuc`、`sifli-usart`、`sifli-dma`
等区域。
`notes/verify-sifli.sh` 的第 6 项就是自动做这件事。

**加一个没被强制但固件在等的位**：直接 grep HAL：

```bash
grep -n "while\s*(" drivers/hal/bf0_hal_xxx.c
```

凡是轮询**硬件**才会置的位的循环，就是需要 `force1` 的地方；凡是 HAL 自己先写再读的
（比如 `RSTR1 |= x; while (!RSTR1);`），普通存储就够了，不要画蛇添足。

## 13. 待办：还没做、或做了还没验的

### 13.1 模型缺口

**co-engine 图层的缩放已做，旋转仍未做。** HAL 对 EZIP 图层有两条专属限制，
都在 `HAL_EPIC_BlendStartEx` 的入口（`bf0_hal_epic.c:6196-6204`）：**不能旋转**
（`rot_cfg->angle != 0` 直接 `RETURN_ERROR`，注释写着 `don't support ezip
rotation`），以及**两个 EZIP 图层不能一前一后同框**。缩放**不在**拒绝之列——
它和别的图层一样走比例通道（`bf0_hal_epic.c:2831` 那段的 `scale_x/scale_y`）。
缩放那条模型已经跟上了（§7.7），并用 `example/rt_device/gpu/single_mode` 的
`scale_down_demo(1/2/3)` 做了逐像素验收；旋转对 co-engine 图层不可达，模型不
实现，真遇到只记一笔并按未变换画。

**旋转仍没有真机可比对的样本。** §7.7 的旋转反算只由 [8c] 的手推像素钉住。SDK 里
真做旋转的例程（`example/get-started/dualcore` 的 rotation3d、LVGL 那条
`lv_draw_epic_img.c` → `HAL_EPIC_Adv`、`single_mode` 的 `rotate_and_mask_demo()`）
都只把结果打到 LCD 上，既不打印校验和也不比期望像素。缩放那条不同：`single_mode`
的 `scale_down_demo()` 画完把结果留在 `buffer0` 里，抓出来就能逐像素比（§7.7）。
`hal_example` 的两个 EPIC 用例虽然比期望数据，但它们调 `HAL_EPIC_Rotate` 时
`angle`/`scale` 全是默认值（源码注释就写着 "no rotation and scaling"），验的其实
是 EZIP 解码。

**I2C（I2C1–4，`0x5009c000` 起，见 `drivers/cmsis/sf32lb52x/register.h:387`）
没有模型**，板子上的触摸屏因此用不了。现在会干净地超时失败，不挂住系统。

### 13.2 有模型、但还没有真机证据

- **`ezipa_dec`（EZIP 动画中间件）和 LVGL 的两个 GPU 后端没在模型上跑过。**
  真机证据目前只有 `example/hal_example` 的 `example_ezip` 一条（跑法见
  `notes/build-and-verify.md` 的 `example_ezip` 小节）。这几个才是真正的下游：
  `middleware/ezipa_dec/ezipa_dec.c:237`、`middleware/lvgl/lv_drivers/lv_gpu.c:115`、
  `middleware/lvgl/lv_drivers_v9/sifli/lv_draw_epic_img.c:212` 都设
  `EPIC_INPUT_EZIP`。
- **显示通路没在真的 SDL 窗口里用眼睛看过。** 像素是 `-display none` +
  `screendump` 解 PPM 验的（`notes/build-and-verify.md` §2③），窗口本身没开过。
- **macOS / Windows 的 SDL 路径只在 CI 里编过，没跑过。** 2026-10-06 的 CI
  三个平台全绿，链接和 smoke test 都过了，但 smoke test 只跑 `--version` 和
  `-M help`，不开窗——"编得过"和"窗口出得来"是两回事
  （`notes/build-and-verify.md` §6）。
