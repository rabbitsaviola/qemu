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

### 2.7 未列出的偏移

读 0，并以 `LOG_GUEST_ERROR` 记一笔。加 `-d guest_errors` 就能看到固件碰了哪些
没建模的寄存器——这是找"下一步该做什么"的主要手段。

---

## 3. 时钟耦合：RCC 决定 SysTick

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

所以 RCC 表带钩子，在 `CSR`/`CFGR`/`DLL1CR` 被写时重算并
`clock_set_hz(s->clk, ...)`。machine 把 `sysclk` 交给每个 bank（只有 RCC 会用它）。

顺带一个好消息：**频率能自洽**。`HAL_RCC_HCPU_GetDLLFreq()` 从 `DLL1CR` 的
stage 字段反推（`freq = stg * 24M + 24M`），而 `EnableDLL` 写进去的就是
`stg = (freq - 24M) / 24M`。所以只要 `DLL1CR` 是"存写入值 + 强制 READY"，
写 240 MHz 读回就是 240 MHz，不需要额外维护。

**USART 的波特率反而不用管**：`SystemFixClock` 是编译期常量 48 MHz
（`bf0_hal.h:198`），BRR 由它算出来，和 RCC 寄存器无关。

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

## 6. 加一个新外设

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

## 7. 真实固件 bring-up 记录

用 `sf32lb52-lcd_a128r16` 的 `hello_world`（SDK 原样构建，固件一行不改）逐个
排查出来的。**这些用 `hello_qemu` 那种 semihosting 桩永远发现不了**——它一个
HAL 寄存器都不碰。

排查手法：固件跑起来后从 monitor 采 `info registers` 看 PC，两次采样相同就是
死循环，再用 `arm-none-eabi-addr2line` 把地址翻回源码行。

### 7.1 DWT 周期计数器（QEMU 的缺口）

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
见 §7.2 关于它为什么必须挂进 armv7m 的 container。

### 7.2 armv7m container 的优先级陷阱

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

### 7.3 RTC 的 LXT 使能位（分支走错）

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

### 7.4 MPI 的 CALCR.DONE

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

### 7.5 RTC_ISR 的六个就绪标志

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

### 7.6 AUDPRC 与 AUDCODEC（只列被等的寄存器）

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

### 7.7 结果

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

跑到 `main()` 并停在 RT-Thread 的 msh 提示符上。`notes/verify-sifli.sh` 第 8 项
就是跑这个。

### 7.8 规律

到目前为止**每一个坑都是同一个形状**：没建模的寄存器读回 0，固件据此做了
一个真实硬件上不会做的判断，然后走进死循环或错误分支。所以薄模型的重点
从来不是"存得住写入的值"，而是**让固件读到它会做的那个判断所需的终态**。

---

## 8. 排查

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
`notes/verify-sifli.sh` 的第 7 项就是自动做这件事。

**加一个没被强制但固件在等的位**：直接 grep HAL：

```bash
grep -n "while\s*(" drivers/hal/bf0_hal_xxx.c
```

凡是轮询**硬件**才会置的位的循环，就是需要 `force1` 的地方；凡是 HAL 自己先写再读的
（比如 `RSTR1 |= x; while (!RSTR1);`），普通存储就够了，不要画蛇添足。
