/*
 * SiFli SF32LB52x peripheral tables
 *
 * The register maps for the configuration blocks -- RCC, AON and PMUC -- and
 * the list of instances the machine wires up. The layouts come from the SDK:
 *
 *   drivers/cmsis/sf32lb52x/hpsys_rcc.h
 *   drivers/cmsis/sf32lb52x/lpsys_rcc.h
 *   drivers/cmsis/sf32lb52x/hpsys_aon.h
 *   drivers/cmsis/sf32lb52x/lpsys_aon.h
 *   drivers/cmsis/sf32lb52x/pmuc.h
 *
 * None of these blocks do anything a model would want to reproduce. They are
 * a few hundred bits of configuration, and the only thing firmware asks of
 * them is whether some clock, regulator or PLL has finished starting. So the
 * tables below store what is written and force the handful of status bits
 * that hardware would raise, which is enough to make the HAL's
 *
 *     write enable; while (!(reg & RDY));
 *
 * settle on the first test instead of spinning forever.
 *
 * The forced bits were found by grepping the HAL for its wait loops, not by
 * reading the datasheet: see the comment on each one for the call site that
 * needs it. Anything not listed there is plain storage.
 *
 * One thing a table cannot express, and so needs a write hook, is RCC's
 * effect on the clocks: writing CSR, CFGR or DLL1CR changes how fast the
 * machine's CPU runs and how fast the tick clock SysTick counts. See
 * sf32lb52x_hpsys_rcc_write() and sf32lb52x_rcc_update_clocks().
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/arm/sf32lb52x.h"
#include "hw/misc/sifli-regbank.h"
#include "qemu/bitops.h"
#include "qemu/host-utils.h"
#include "qemu/module.h"
#include "qemu/timer.h"

/* HPSYS_RCC register offsets */
enum {
    HPSYS_RCC_RSTR1     = 0x00,
    HPSYS_RCC_RSTR2     = 0x04,
    HPSYS_RCC_ENR1      = 0x08,
    HPSYS_RCC_ENR2      = 0x0c,
    HPSYS_RCC_ESR1      = 0x10,
    HPSYS_RCC_ESR2      = 0x14,
    HPSYS_RCC_ECR1      = 0x18,
    HPSYS_RCC_ECR2      = 0x1c,
    HPSYS_RCC_CSR       = 0x20,
    HPSYS_RCC_CFGR      = 0x24,
    HPSYS_RCC_DLL1CR    = 0x2c,
    HPSYS_RCC_DLL2CR    = 0x30,
    HPSYS_RCC_HRCCAL1   = 0x34,
    HPSYS_RCC_HRCCAL2   = 0x38,
};

/* HPSYS_RCC bit fields the clock calculation needs */
#define HPSYS_RCC_DLL1CR_EN         BIT(0)
#define HPSYS_RCC_DLL1CR_STG_POS    2
#define HPSYS_RCC_DLL1CR_STG_MSK    (0xfUL << HPSYS_RCC_DLL1CR_STG_POS)

/*
 * The clock sources CSR.CSR[1:0] can select (bf0_hal_rcc.h:77-87). Only DLL1
 * changes what HAL_RCC_GetSysCLKFreq() returns.
 */
#define SF32LB52X_RCC_SYSCLK_DLL1   3

/* DLL step, from bf0_hal_rcc.c:20-26: freq = stg * step + min. */
#define SF32LB52X_DLL_STEP_FRQ      24000000

/*
 * Tick-clock fields: CSR.SEL_TICK picks the source, CFGR.TICKDIV divides it.
 * Both are plain RCC registers the firmware writes through
 * HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_HP_TICK, ...) and
 * HAL_RCC_HCPU_SetTickDiv() -- RCC_CLK_MOD_HP_TICK is literally
 * HPSYS_RCC_CSR_SEL_TICK_Pos, so the select is just a field write
 * (bf0_hal_rcc.h:181, hpsys_rcc.h:523,540).
 */
#define HPSYS_RCC_CSR_SEL_TICK_POS  13
#define HPSYS_RCC_CSR_SEL_TICK_MSK  (3UL << HPSYS_RCC_CSR_SEL_TICK_POS)
#define HPSYS_RCC_CFGR_TICKDIV_POS  16
#define HPSYS_RCC_CFGR_TICKDIV_MSK  (0x3fUL << HPSYS_RCC_CFGR_TICKDIV_POS)

/* What CSR.SEL_TICK can select (bf0_hal_rcc.h:176-178). */
#define SF32LB52X_RCC_TICK_LP       0
#define SF32LB52X_RCC_TICK_HRC48    2
#define SF32LB52X_RCC_TICK_HXT48    3

/* HPSYS_CFG register offsets */
enum {
    HPSYS_CFG_BMR           = 0x00,
    HPSYS_CFG_IDR           = 0x04,
    HPSYS_CFG_SWCR          = 0x08,
    HPSYS_CFG_SCR           = 0x0c,
    HPSYS_CFG_SYSCR         = 0x10,
    HPSYS_CFG_RTC_TR        = 0x14,
    HPSYS_CFG_RTC_DR        = 0x18,
    HPSYS_CFG_ULPMCR        = 0x1c,
    HPSYS_CFG_DBGR          = 0x20,
    HPSYS_CFG_MDBGR         = 0x24,
    HPSYS_CFG_BISTCR        = 0x28,
    HPSYS_CFG_BISTR         = 0x2c,
    HPSYS_CFG_ROMCR0        = 0x30,
    HPSYS_CFG_ROMCR1        = 0x34,
    HPSYS_CFG_ROMCR2        = 0x38,
    HPSYS_CFG_LPIRQ         = 0x3c,
    HPSYS_CFG_USBCR         = 0x40,
    HPSYS_CFG_SYS_RSVD      = 0x44,
    HPSYS_CFG_I2C1_PINR     = 0x48,
    HPSYS_CFG_I2C2_PINR     = 0x4c,
    HPSYS_CFG_I2C3_PINR     = 0x50,
    HPSYS_CFG_I2C4_PINR     = 0x54,
    HPSYS_CFG_USART1_PINR   = 0x58,
    HPSYS_CFG_USART2_PINR   = 0x5c,
    HPSYS_CFG_USART3_PINR   = 0x60,
    HPSYS_CFG_GPTIM1_PINR   = 0x64,
    HPSYS_CFG_GPTIM2_PINR   = 0x68,
    HPSYS_CFG_ETR_PINR      = 0x6c,
    HPSYS_CFG_LPTIM1_PINR   = 0x70,
    HPSYS_CFG_LPTIM2_PINR   = 0x74,
    HPSYS_CFG_ATIM1_PINR1   = 0x78,
    HPSYS_CFG_ATIM1_PINR2   = 0x7c,
    HPSYS_CFG_ATIM1_PINR3   = 0x80,
    HPSYS_CFG_PTA_PINR      = 0x84,
    HPSYS_CFG_ANAU_CR       = 0x88,
    HPSYS_CFG_ANAU_RSVD     = 0x8c,
    HPSYS_CFG_ANATR         = 0x90,
    HPSYS_CFG_CAU2_CR       = 0x94,
    HPSYS_CFG_CAU2_RSVD     = 0x98,
};

#define HPSYS_CFG_SIZE          0x9c

/* RTC register offsets */
enum {
    RTC_TR          = 0x00,
    RTC_DR          = 0x04,
    RTC_CR          = 0x08,
    RTC_ISR         = 0x0c,
    RTC_PSCLR       = 0x10,
    RTC_WUTR        = 0x14,
    RTC_ALRMTR      = 0x18,
    RTC_ALRMDR      = 0x1c,
    RTC_SHIFTR      = 0x20,
    RTC_TSTR        = 0x24,
    RTC_TSDR        = 0x28,
    RTC_OR          = 0x2c,
    RTC_BKP0R       = 0x30,
    RTC_BKP1R       = 0x34,
    RTC_BKP2R       = 0x38,
    RTC_BKP3R       = 0x3c,
    RTC_BKP4R       = 0x40,
    RTC_BKP5R       = 0x44,
    RTC_BKP6R       = 0x48,
    RTC_BKP7R       = 0x4c,
    RTC_BKP8R       = 0x50,
    RTC_BKP9R       = 0x54,
    RTC_PBRCR       = 0x58,
    RTC_PBR0R       = 0x5c,
    RTC_PBR1R       = 0x60,
    RTC_PBR2R       = 0x64,
    RTC_PBR3R       = 0x68,
    RTC_PAWK1R      = 0x6c,
    RTC_PAWK2R      = 0x70,
    RTC_PAWK3R      = 0x74,
};

#define RTC_SIZE        0x78

/* LPSYS_RCC register offsets */
enum {
    LPSYS_RCC_RSTR1     = 0x00,
    LPSYS_RCC_ENR1      = 0x04,
    LPSYS_RCC_ESR1      = 0x08,
    LPSYS_RCC_ECR1      = 0x0c,
};

/*
 * Warp both clock outputs of the RCC to the rates the firmware would compute
 * for itself, so that SysTick keeps matching what the firmware believes.
 *
 * System clock (drives the CPU, and SysTick when it selects HCLK). This
 * mirrors HAL_RCC_GetSysCLKFreq() (bf0_hal_rcc.c:1570) followed by
 * HAL_RCC_GetHCLKFreq() (:1586): 48 MHz unless CSR selects DLL1, and when it
 * does, the DLL1 stage register gives stg * 24 MHz + 24 MHz; then divide by
 * CFGR's HDIV, where zero counts as one.
 *
 * It matters because the firmware's millisecond comes from SystemCoreClock,
 * which it derives from these same registers -- and the SDK's own start-up
 * (HAL_RCC_HCPU_ConfigHCLK, reached from the board's HAL_PreInit) finishes by
 * selecting DLL1 at 240 MHz. A machine clock left at its reset 48 MHz would
 * make every delay in the firmware five times too long.
 *
 * Tick clock (drives SysTick when it selects the tick clock instead, which is
 * what this firmware does). The rate is source / TICKDIV, with the source
 * from CSR.SEL_TICK: HRC48 or HXT48 at 48 MHz, or the LP clock at the LXT's
 * 32 kHz. rt_hw_systick_init() (drv_common.c:126) asks for HRC48 over 60,
 * giving the 800 kHz it then programs SysTick's reload against.
 *
 * Both have to be modelled from the registers rather than pinned, because the
 * firmware walks the clock tree at start-up and again on every PM frequency
 * change; a tick clock fixed at whatever boot happened to leave behind would
 * silently disagree after the first change.
 *
 * Note the rates go out with clock_update_hz(), not clock_set_hz(). The latter
 * only caches the value on the clock object and leaves every consumer holding
 * the old one -- clock_set_source() copies a rate when a clock is *connected*,
 * but nothing after that, so a clock tuned at run time needs the explicit
 * clock_propagate() that clock_update_hz() does for it. Getting this wrong is
 * silent: SysTick keeps counting at whatever rate it was connected with, and
 * the firmware's millisecond is wrong by the ratio.
 */
static void sf32lb52x_rcc_update_clocks(SifliRegBankState *s)
{
    uint32_t csr, cfgr, dll1cr, div, hz;

    if (!s->clk && !s->tick_clk) {
        return;
    }

    csr = sifli_regbank_reg(s, HPSYS_RCC_CSR);
    cfgr = sifli_regbank_reg(s, HPSYS_RCC_CFGR);
    dll1cr = sifli_regbank_reg(s, HPSYS_RCC_DLL1CR);

    if (s->tick_clk) {
        switch ((csr & HPSYS_RCC_CSR_SEL_TICK_MSK)
                >> HPSYS_RCC_CSR_SEL_TICK_POS) {
        case SF32LB52X_RCC_TICK_HRC48:
            hz = SF32LB52X_HRC48_FRQ;
            break;
        case SF32LB52X_RCC_TICK_HXT48:
            hz = SF32LB52X_HXT48_FRQ;
            break;
        default:
            /* TICK_CLK_LP, the low-power clock. */
            hz = SF32LB52X_LXT_FRQ;
            break;
        }
        div = (cfgr & HPSYS_RCC_CFGR_TICKDIV_MSK)
              >> HPSYS_RCC_CFGR_TICKDIV_POS;
        if (div == 0) {
            div = 1;
        }
        clock_update_hz(s->tick_clk, hz / div);
    }

    if (!s->clk) {
        return;
    }

    if ((csr & 3) == SF32LB52X_RCC_SYSCLK_DLL1) {
        if (!(dll1cr & HPSYS_RCC_DLL1CR_EN)) {
            /*
             * DLL1 selected but not running. The firmware would read zero
             * here too; leaving the clock where it was is better than
             * driving SysTick to a standstill.
             */
            return;
        }
        hz = (((dll1cr & HPSYS_RCC_DLL1CR_STG_MSK)
               >> HPSYS_RCC_DLL1CR_STG_POS) + 1) * SF32LB52X_DLL_STEP_FRQ;
    } else {
        hz = SF32LB52X_HXT48_FRQ;
    }

    div = cfgr & 0xff;
    if (div == 0) {
        div = 1;
    }
    clock_update_hz(s->clk, hz / div);
}

/*
 * The enable set/clear aliases need nothing here: they are declared in the
 * table, and sifli_regbank_write() applies them. What is left is the one
 * thing a table cannot say -- that writing the clock registers changes how
 * fast the CPU and its tick clock run.
 */
static bool sf32lb52x_hpsys_rcc_write(SifliRegBankState *s, uint32_t off,
                                      uint32_t value)
{
    switch (off) {
    /*
     * The three registers the two clocks are derived from. They are stored
     * here rather than by the table so that the new rates go out in the
     * same step, and so that a change to any one of them re-evaluates the
     * whole calculation.
     */
    case HPSYS_RCC_CSR:
    case HPSYS_RCC_CFGR:
    case HPSYS_RCC_DLL1CR:
        sifli_regbank_set_reg(s, off, value);
        sf32lb52x_rcc_update_clocks(s);
        return true;

    default:
        return false;
    }
}

static const SifliRegDef sf32lb52x_hpsys_rcc_regs[] = {
    /* Reset control. The HAL writes a bit, then clears it again. */
    { .off = HPSYS_RCC_RSTR1 },
    { .off = HPSYS_RCC_RSTR2 },

    /* Module clock enables; the readable state. */
    { .off = HPSYS_RCC_ENR1 },
    { .off = HPSYS_RCC_ENR2 },

    /*
     * Enable set / clear aliases. Write-only: the write lands on the ENR
     * pair, and a read of these returns zero as it does on hardware.
     */
    { .off = HPSYS_RCC_ESR1, .writeonly = true,
      .alias = HPSYS_RCC_ENR1, .alias_set = ~0u },
    { .off = HPSYS_RCC_ESR2, .writeonly = true,
      .alias = HPSYS_RCC_ENR2, .alias_set = ~0u },
    { .off = HPSYS_RCC_ECR1, .writeonly = true,
      .alias = HPSYS_RCC_ENR1, .alias_clear = ~0u },
    { .off = HPSYS_RCC_ECR2, .writeonly = true,
      .alias = HPSYS_RCC_ENR2, .alias_clear = ~0u },

    /*
     * Clock source and dividers.
     *
     * Reset to HXT48 (CSR[1:0] = 1), which makes HAL_RCC_GetSysCLKFreq()
     * short-circuit to a flat 48 MHz, and to no division at all (CFGR == 0,
     * which HAL_RCC_GetHCLKFreq() reads as divide-by-one). The start-up code
     * moves this on to DLL1 at 240 MHz; the hook recomputes the machine's
     * clock whenever it does.
     */
    { .off = HPSYS_RCC_CSR, .reset = 1 },
    { .off = HPSYS_RCC_CFGR },

    { .off = 0x28 },                    /* USBCR */

    /*
     * The DLLs and the RC calibration. bf0_hal_rcc.c spins on READY/CAL_DONE
     * at bit 31 of each (lines 1353, 1424, 2277, 2358, 2434), and
     * HAL_RCC_HCPU_GetDLLFreq() reads the stage field back out of the same
     * register -- so storing the written value and forcing READY makes the
     * frequency round-trip consistently. Writing stg = 9 for 240 MHz reads
     * back as 9 * 24 + 24 = 240 MHz.
     */
    { .off = HPSYS_RCC_DLL1CR, .force1 = BIT(31) },
    { .off = HPSYS_RCC_DLL2CR, .force1 = BIT(31) },
    { .off = HPSYS_RCC_HRCCAL1, .force1 = BIT(31) },

    /*
     * HRCCAL2 reads zero, and that is load-bearing. It reports the two
     * counts the calibration compares, HRCCAL2[31:16] and HRCCAL2[15:0]
     * (bf0_hal_rcc.c:2439). With both zero the first iteration finds
     * cnt_diff == 0, breaks out of the loop and returns HAL_OK. Forcing
     * bit 31 here instead would report hxt_cnt = 0x8000, cnt_diff = 32768,
     * and bf0_hal_rcc.c:2463 would return HAL_ERROR -- which HAL_Init()
     * turns into HAL_ASSERT, a hang.
     */
    { .off = HPSYS_RCC_HRCCAL2 },

    { .off = 0x3c },                    /* DBGCLKR */
    { .off = 0x40 },                    /* DBGR    */
    { .off = 0x44 },                    /* DWCFGR  */
    /* 0x48-0x78 are reserved. */
    { .off = 0x7c },                    /* TESTR   */
};

static const SifliRegDef sf32lb52x_lpsys_rcc_regs[] = {
    /*
     * RSTR1 carries RFC (bit 18), which HAL_RCC_ResetBluetoothRF() sets
     * itself and then polls, so plain storage already satisfies it. The
     * "while (!RSTR1)" loops in HAL_RCC_ResetLCPU() are the same story:
     * the HAL writes a nonzero value on the line before.
     */
    { .off = LPSYS_RCC_RSTR1 },
    { .off = LPSYS_RCC_ENR1 },
    { .off = LPSYS_RCC_ESR1, .writeonly = true,
      .alias = LPSYS_RCC_ENR1, .alias_set = ~0u },
    { .off = LPSYS_RCC_ECR1, .writeonly = true,
      .alias = LPSYS_RCC_ENR1, .alias_clear = ~0u },
    { .off = 0x10 },                    /* CSR  */
    { .off = 0x14 },                    /* CFGR */
    { .off = 0x18 },                    /* DBGR */
};

/*
 * System configuration: boot mode, chip identity, pin muxing, and assorted
 * odds and ends. Nothing here is polled; two registers have to report
 * particular values, and the rest is plain storage.
 */
static const SifliRegDef sf32lb52x_hpsys_cfg_regs[] = {
    /* BMR: BOOT_MODE (bit 0) reads 0, i.e. not a ROM boot. */
    { .off = HPSYS_CFG_BMR },

    /*
     * IDR -- SID[31:24], CID[23:16], PID[15:8], REVID[7:0].
     *
     * drv_common.c:712 calls __HAL_SYSCFG_CHECK_REVID(), which is a
     * HAL_ASSERT that REVID is one of 0x03, 0x07 or 0x0f. Reading zero
     * fails it, and HAL_AssertFailed() is a while(1). Report revision A4.
     */
    { .off = HPSYS_CFG_IDR, .reset = 0x00000707, .readonly = ~0u },

    { .off = HPSYS_CFG_SWCR },
    { .off = HPSYS_CFG_SCR },
    { .off = HPSYS_CFG_SYSCR },
    { .off = HPSYS_CFG_RTC_TR },

    /*
     * RTC_DR: bit 31 is ERR, and drv_common.c:282 is a bare
     *
     *     while (HAL_RTC_GetDate(...) == HAL_ERROR);
     *
     * so the error bit has to stay clear. Zero is the only safe answer.
     */
    { .off = HPSYS_CFG_RTC_DR },

    { .off = HPSYS_CFG_ULPMCR },
    { .off = HPSYS_CFG_DBGR },
    { .off = HPSYS_CFG_MDBGR },
    { .off = HPSYS_CFG_BISTCR },
    { .off = HPSYS_CFG_BISTR },
    { .off = HPSYS_CFG_ROMCR0 },
    { .off = HPSYS_CFG_ROMCR1 },
    { .off = HPSYS_CFG_ROMCR2 },
    { .off = HPSYS_CFG_LPIRQ },
    { .off = HPSYS_CFG_USBCR },
    { .off = HPSYS_CFG_SYS_RSVD },

    /* Per-peripheral pin routing. Written by HAL_PIN_Set(), never polled. */
    { .off = HPSYS_CFG_I2C1_PINR },
    { .off = HPSYS_CFG_I2C2_PINR },
    { .off = HPSYS_CFG_I2C3_PINR },
    { .off = HPSYS_CFG_I2C4_PINR },
    { .off = HPSYS_CFG_USART1_PINR },
    { .off = HPSYS_CFG_USART2_PINR },
    { .off = HPSYS_CFG_USART3_PINR },
    { .off = HPSYS_CFG_GPTIM1_PINR },
    { .off = HPSYS_CFG_GPTIM2_PINR },
    { .off = HPSYS_CFG_ETR_PINR },
    { .off = HPSYS_CFG_LPTIM1_PINR },
    { .off = HPSYS_CFG_LPTIM2_PINR },
    { .off = HPSYS_CFG_ATIM1_PINR1 },
    { .off = HPSYS_CFG_ATIM1_PINR2 },
    { .off = HPSYS_CFG_ATIM1_PINR3 },
    { .off = HPSYS_CFG_PTA_PINR },

    { .off = HPSYS_CFG_ANAU_CR },
    { .off = HPSYS_CFG_ANAU_RSVD },
    { .off = HPSYS_CFG_ANATR },
    { .off = HPSYS_CFG_CAU2_CR },
    { .off = HPSYS_CFG_CAU2_RSVD },
};

#define HPSYS_AON_GTIMR     0x34

/* RTC_CR bit 0: set selects the 32 kHz crystal, clear the ~10 kHz RC. */
#define RTC_CR_LPCKSEL      BIT(0)

/*
 * GTIMR is a free-running up-counter with no control bits of its own, so its
 * rate is the only thing to get right -- and the rate is not fixed. It counts
 * whichever low-power clock the RTC has selected, and that is RTC_CR.LPCKSEL:
 * set means the 32 kHz crystal, clear means the ~10 kHz RC.
 *
 * The bit is not merely a detail of the counter, because HAL_GetTick() picks
 * its conversion from the same bit (drv_common.c:328, via HAL_LXT_ENABLED()):
 * t * 1000 / 32768 on the crystal (:360), t / 10 on the RC (:342). A counter
 * running at one rate while the firmware divides by the other makes every HAL
 * timeout in the SDK wrong by the ratio -- the touch driver's I2C waits are
 * the first to care, and on this model they are what turns an unmodelled bus
 * into a hung board rather than a clean HAL_TIMEOUT.
 *
 * LPCKSEL is clear on reset, so a machine whose firmware never touches the
 * RTC reports the RC rate. That is what the hardware does too.
 *
 * The calibrated RC path is deliberately not modelled: when the RC is
 * selected and the firmware holds a stored calibration value, HAL_GetTick()
 * divides by the calibrated frequency rather than 10 kHz (drv_common.c:351).
 * Nothing here produces that value, and the firmware this model is checked
 * against runs on the crystal.
 *
 * Ticks come from the virtual clock rather than a timer: the answer has to be
 * current at the instant of the read, and the alternative is 32768 timer
 * wake-ups a second to keep a register nobody polls.
 */
#define HPSYS_AON_GTIMR_LXT_HZ      32768
#define HPSYS_AON_GTIMR_RC10K_HZ    10000

static uint32_t sf32lb52x_gtimr_hz(SifliRegBankState *s)
{
    if (s->peer && (sifli_regbank_reg(s->peer, RTC_CR) & RTC_CR_LPCKSEL)) {
        return HPSYS_AON_GTIMR_LXT_HZ;
    }

    return HPSYS_AON_GTIMR_RC10K_HZ;
}

static void sf32lb52x_hpsys_aon_read(SifliRegBankState *s, uint32_t off,
                                     uint32_t *value)
{
    if (off == HPSYS_AON_GTIMR) {
        *value = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                          sf32lb52x_gtimr_hz(s), NANOSECONDS_PER_SECOND);
    }
}

/*
 * The always-on blocks hold the 48 MHz and 32 kHz clock controls and the
 * wake-up sources. Nothing here is polled except the clock-ready flags and
 * the activity status.
 */
static const SifliRegDef sf32lb52x_hpsys_aon_regs[] = {
    { .off = 0x00 },                    /* PMR   */
    { .off = 0x04 },                    /* CR1   */
    { .off = 0x08 },                    /* CR2   */
    { .off = 0x0c },                    /* CR3   */

    /*
     * ACR: HRC48_RDY (30) and HXT48_RDY (31). bf0_hal_hpaon.c:501,514 waits
     * on each after starting the corresponding oscillator.
     */
    { .off = 0x10, .force1 = BIT(30) | BIT(31) },

    { .off = 0x14 },                    /* LSCR  */
    { .off = 0x18 },                    /* DSCR  */
    { .off = 0x1c },                    /* SBCR  */
    { .off = 0x20 },                    /* WER   */
    { .off = 0x24 },                    /* WSR   */
    /* WCR clears WSR: HAL_PMU_CheckBootMode() reads one, writes the other. */
    { .off = 0x28, .writeonly = true,
      .alias = 0x24, .alias_clear = ~0u },

    /*
     * ISSR: HP_ACTIVE (4) and LP_ACTIVE (5). bf0_hal_hpaon.c:162,165 waits
     * for LP_ACTIVE, so both are reported set -- this model has no second
     * core that could be genuinely inactive.
     */
    { .off = 0x2c, .force1 = BIT(4) | BIT(5) },

    { .off = 0x30 },                    /* ANACR   */
    /* GTIMR: free-running, so its value comes from the read hook. */
    { .off = HPSYS_AON_GTIMR },
    { .off = 0x38 },                    /* RESERVE0 */
    { .off = 0x3c },                    /* RESERVE1 */
};

static const SifliRegDef sf32lb52x_lpsys_aon_regs[] = {
    { .off = 0x00 },                    /* PMR   */
    { .off = 0x04 },                    /* CR1   */
    { .off = 0x08 },                    /* CR2   */
    { .off = 0x0c },                    /* CR3   */

    /* ACR, as in HPSYS_AON; bf0_hal_rcc.c:1772 is the wait. */
    { .off = 0x10, .force1 = BIT(30) | BIT(31) },

    { .off = 0x14 },                    /* LSCR     */
    { .off = 0x18 },                    /* DSCR     */
    { .off = 0x1c },                    /* SBCR     */
    { .off = 0x20 },                    /* WER      */
    { .off = 0x24 },                    /* WSR      */
    /* WCR clears WSR, as in HPSYS_AON. */
    { .off = 0x28, .writeonly = true,
      .alias = 0x24, .alias_clear = ~0u },
    /* ISSR, as in HPSYS_AON but with the two active bits swapped. */
    { .off = 0x2c, .force1 = BIT(4) | BIT(5) },
    { .off = 0x30 },                    /* TARGET   */
    { .off = 0x34 },                    /* ACTUAL   */
    { .off = 0x38 },                    /* PRE_WKUP */
    { .off = 0x3c },                    /* SLP_CFG  */

    /*
     * SLP_CTRL: SLEEP_STATUS (4) reads clear, so the "if we are asleep, ask
     * to wake up and wait" branch in bf0_hal_rcc.c:1711 is never entered.
     */
    { .off = 0x40, .force0 = BIT(4) },

    { .off = 0x44 },                    /* ANACR    */
    /*
     * GTIMR, the LCPU's twin of the HPSYS one. Left as storage: only the
     * LCPU build reaches it. drv_common.c takes HPSYS's copy under
     * SOC_BF0_HCPU and this one otherwise, and this machine runs the HCPU
     * firmware.
     */
    { .off = 0x48 },
    { .off = 0x4c },                    /* RESERVE0 */
    { .off = 0x50 },                    /* RESERVE1 */
    /* 0x54-0xfc are reserved. */
    { .off = 0x100 },                   /* SPR */
    { .off = 0x104 },                   /* PCR */
};

/*
 * The power management controller. Every regulator the HAL switches on has a
 * ready bit it then waits for; bf0_hal_pmu.c has the loops (lines 469, 476,
 * 524, 586).
 */
static const SifliRegDef sf32lb52x_pmuc_regs[] = {
    { .off = 0x00 },                    /* CR  */
    { .off = 0x04 },                    /* WER */
    { .off = 0x08 },                    /* WSR */
    /* WCR clears WSR, as in the AON blocks. */
    { .off = 0x0c, .writeonly = true,
      .alias = 0x08, .alias_clear = ~0u },

    { .off = 0x10 },                    /* VRTC_CR  */
    { .off = 0x14, .force1 = BIT(31) },    /* VRET_CR  */
    { .off = 0x18, .force1 = BIT(31) },    /* LRC10_CR */
    { .off = 0x1c, .force1 = BIT(31) },    /* LRC32_CR */
    { .off = 0x20, .force1 = BIT(31) },    /* LXT_CR   */
    { .off = 0x24 },                    /* AON_BG   */
    { .off = 0x28 },                    /* AON_LDO  */
    { .off = 0x2c, .force1 = BIT(31) },    /* BUCK_CR1: SS_DONE  */
    { .off = 0x30, .force1 = BIT(19) },    /* BUCK_CR2: FORCE_RDY */

    { .off = 0x34 },                    /* CHG_CR1 */
    { .off = 0x38 },                    /* CHG_CR2 */
    { .off = 0x3c },                    /* CHG_CR3 */
    { .off = 0x40 },                    /* CHG_CR4 */
    { .off = 0x44 },                    /* CHG_CR5 */
    { .off = 0x48 },                    /* CHG_SR  */

    { .off = 0x4c, .force1 = BIT(16) },    /* HPSYS_LDO: RDY */
    { .off = 0x50, .force1 = BIT(16) },    /* LPSYS_LDO: RDY */
    { .off = 0x54, .force1 = BIT(31) },    /* HPSYS_SWR: RDY */
    { .off = 0x58, .force1 = BIT(31) },    /* LPSYS_SWR: RDY */
    { .off = 0x5c },                    /* PERI_LDO */

    { .off = 0x60 },                    /* PMU_TR   */
    { .off = 0x64 },                    /* PMU_RSVD */
    { .off = 0x68 },                    /* HXT_CR1  */
    { .off = 0x6c },                    /* HXT_CR2  */
    { .off = 0x70 },                    /* HXT_CR3  */
    { .off = 0x74 },                    /* HRC_CR   */
    { .off = 0x78 },                    /* DBL96_CR */
    { .off = 0x7c, .force1 = BIT(13) },    /* DBL96_CALR: CAL_LOCK */

    { .off = 0x80 },                    /* CAU_BGR   */
    { .off = 0x84 },                    /* CAU_TR    */
    { .off = 0x88 },                    /* CAU_RSVD  */
    { .off = 0x8c },                    /* WKUP_CNT  */
    { .off = 0x90 },                    /* PWRKEY_CNT */
    { .off = 0x94 },                    /* HPSYS_VOUT */
    { .off = 0x98 },                    /* LPSYS_VOUT */
    { .off = 0x9c },                    /* BUCK_VOUT  */
};

/*
 * The real-time clock.
 *
 * Only one bit of this matters for getting firmware up, and it is not a
 * timekeeping one: RTC_CR.LPCKSEL. bf0_hal_rtc.h resolves the 52x macros to
 *
 *     HAL_RTC_ENABLE_LXT()   ->  hwp_rtc->CR |= RTC_CR_LPCKSEL
 *     HAL_LXT_DISABLED()     ->  !(hwp_rtc->CR & RTC_CR_LPCKSEL)
 *
 * and the board's HAL_PreInit enables the low-speed crystal and then asks
 * whether it is there. With this register reading zero the answer is no, so
 * the firmware takes the LRC calibration path instead -- which spins on the
 * Bluetooth MAC's RCCAL_RESULT (bf0_hal_lrc_cal.c:894), a block far outside
 * anything worth modelling. Storing the bit keeps it out of that path.
 *
 * The rest is storage: the backup registers HAL_Get_backup() uses, the
 * prescaler, and date/time registers that read harmlessly as zero.
 */
static const SifliRegDef sf32lb52x_rtc_regs[] = {
    { .off = RTC_TR },
    { .off = RTC_DR },
    { .off = RTC_CR },              /* LPCKSEL; see above */

    /*
     * ISR. Every flag here is one hardware raises once an operation has
     * settled, and each has a "write something, then spin on this" loop in
     * bf0_hal_rtc.c waiting for it:
     *
     *   ALRMWF (0)  alarm write ok          :588,  :635
     *   ALRMF  (1)  alarm matched           :750
     *   WUTWF  (2)  wakeup timer write ok   :164,  :1093
     *   RSF    (7)  registers synchronised  :810
     *   INITS  (8)  shadow registers loaded
     *   INITF  (9)  init mode entered       :1009  <- the first one reached
     *
     * SHPF (6) is the opposite: "a shift is pending", waited on to clear.
     * Note that WUTWF is also waited on to *clear* at :1074, but only
     * underneath a check of RTC_CR.WUTE, which nothing here sets.
     */
    { .off = RTC_ISR,
      .force1 = BIT(0) | BIT(1) | BIT(2) | BIT(7) | BIT(8) | BIT(9),
      .force0 = BIT(6) },

    { .off = RTC_PSCLR },
    { .off = RTC_WUTR },
    { .off = RTC_ALRMTR },
    { .off = RTC_ALRMDR },
    { .off = RTC_SHIFTR },
    { .off = RTC_TSTR },
    { .off = RTC_TSDR },
    { .off = RTC_OR },

    /* Backup registers, reachable as HAL_Get_backup()/HAL_Set_backup(). */
    { .off = RTC_BKP0R },
    { .off = RTC_BKP1R },
    { .off = RTC_BKP2R },
    { .off = RTC_BKP3R },
    { .off = RTC_BKP4R },
    { .off = RTC_BKP5R },
    { .off = RTC_BKP6R },
    { .off = RTC_BKP7R },
    { .off = RTC_BKP8R },
    { .off = RTC_BKP9R },

    { .off = RTC_PBRCR },
    { .off = RTC_PBR0R },
    { .off = RTC_PBR1R },
    { .off = RTC_PBR2R },
    { .off = RTC_PBR3R },
    { .off = RTC_PAWK1R },
    { .off = RTC_PAWK2R },
    { .off = RTC_PAWK3R },
};

/*
 * MPI1 and MPI2, the QSPI/PSRAM controllers.
 *
 * These are behavioural hardware: they drive a serial protocol to a flash or
 * PSRAM chip and report progress through a status register. None of that is
 * implemented here. What the table does is make the handshakes the firmware
 * waits on resolve at once:
 *
 *   SR.TCF    (bit 0)   transfer complete -- HAL_FLASH_IS_CMD_DONE()
 *   SR.SMF    (bit 3)   status match      -- HAL_FLASH_STATUS_MATCH()
 *   SR.BUSY   (bit 31)  never busy
 *   CALCR.DONE (bit 8)  what bf0_hal_mpi_psram.c:1386 spins on after setting
 *                       CALCR.EN, the first thing that hangs in HAL_PreInit
 *                       once the clock and power blocks are up
 *
 * The command and data registers are plain storage: a command written to
 * CMDR1 reads back but does nothing. Firmware that only wants the controller
 * to come up is satisfied; a driver that actually reads a chip ID back would
 * not be.
 */
static const SifliRegDef sf32lb52x_mpi_regs[] = {
    { .off = 0x00 },                    /* CR     */
    { .off = 0x04 },                    /* DR     */
    { .off = 0x08 },                    /* DCR    */
    { .off = 0x0c },                    /* PSCLR  */

    { .off = 0x10, .force1 = BIT(0) | BIT(3), .force0 = BIT(31) },   /* SR */

    /*
     * SCR clears SR, and the two registers' bit positions line up exactly
     * (SCR.TCFC is bit 0 just as SR.TCF is), so the generic alias does the
     * job: HAL_FLASH_CLR_CMD_DONE() writes the flag it just read.
     */
    { .off = 0x14, .writeonly = true,
      .alias = 0x10, .alias_clear = ~0u },
    { .off = 0x18 },                    /* CMDR1  */
    { .off = 0x1c },                    /* AR1    */
    { .off = 0x20 },                    /* ABR1   */
    { .off = 0x24 },                    /* DLR1   */
    { .off = 0x28 },                    /* CCR1   */
    { .off = 0x2c },                    /* CMDR2  */
    { .off = 0x30 },                    /* AR2    */
    { .off = 0x34 },                    /* ABR2   */
    { .off = 0x38 },                    /* DLR2   */
    { .off = 0x3c },                    /* CCR2   */
    { .off = 0x40 },                    /* HCMDR  */
    { .off = 0x44 },                    /* HRABR  */
    { .off = 0x48 },                    /* HRCCR  */
    { .off = 0x4c },                    /* HWABR  */
    { .off = 0x50 },                    /* HWCCR  */
    { .off = 0x54 },                    /* FIFOCR */
    { .off = 0x58 },                    /* MISCR  */
    { .off = 0x5c },                    /* CTRSAR */
    { .off = 0x60 },                    /* CTREAR */
    { .off = 0x64 },                    /* NONCEA */
    { .off = 0x68 },                    /* NONCEB */
    { .off = 0x6c },                    /* AASAR  */
    { .off = 0x70 },                    /* AAEAR  */
    { .off = 0x74 },                    /* AAOAR  */
    { .off = 0x78 },                    /* CIR    */
    { .off = 0x7c },                    /* SMR    */
    { .off = 0x80 },                    /* SMKR   */
    { .off = 0x84 },                    /* TIMR   */
    { .off = 0x88 },                    /* WDTR   */
    { .off = 0x8c },                    /* PRSAR  */
    { .off = 0x90 },                    /* PREAR  */

    { .off = 0x94, .force1 = BIT(8) },     /* CALCR: DONE */

    { .off = 0x98 },                    /* CALDOR     */
    { .off = 0x9c },                    /* APM32CR    */
    { .off = 0xa0 },                    /* CR2        */
    { .off = 0xa4 },                    /* OPIDLY_OCR1 */
    { .off = 0xa8 },                    /* OPIDLY_ICR1 */
};

#define MPI_SIZE    0xac

/*
 * AUDPRC, the audio processor.
 *
 * Deliberately almost empty. It is a large block -- a bank of EQ
 * coefficients alone runs to dozens of registers -- and the board's BSP
 * configures the whole DAC path at start-up, but only one thing it does is
 * a wait:
 *
 *     haprc->Instance->DAC_PATH_CFG1 = value;
 *     while ((haprc->Instance->DAC_PATH_CFG1
 *             & AUDPRC_DAC_PATH_CFG1_SRC_CH_CLR_DONE) == 0);   :563
 *
 * SRC_CH_CLR_DONE is a two-bit field at [29:28]; any value but zero ends the
 * wait. The rest of the block is left out rather than transcribed, so that
 * anything the firmware turns out to need shows up as a guest-error log
 * instead of being silently invented. See "Adding a peripheral" above.
 */
static const SifliRegDef sf32lb52x_audprc_regs[] = {
    { .off = 0x54, .force1 = BIT(28) | BIT(29) },  /* DAC_PATH_CFG1 */
};

#define AUDPRC_SIZE 0x400

/*
 * AUDCODEC, the audio codec's analogue side.
 *
 * Also nearly empty. The driver calibrates the codec PLL at start-up
 * (drv_audcodec_m.c:440-520), and that is a search loop rather than a single
 * wait:
 *
 *     PLL_CAL_CFG |= EN;
 *     while (!(PLL_CAL_CFG & DONE));        // :457, :495, :510
 *     pll_cnt = PLL_CAL_RESULT >> PLL_CNT_Pos;
 *     ... adjust FC_VCO and try again ...
 *
 * Only DONE has to be invented. The search around it terminates on its own:
 * it halves its step each pass (8, 4, 2, 1, then zero), so it is bounded at
 * four iterations however wrong the counts it reads back are. With
 * PLL_CAL_RESULT reading zero it simply never converges on a good value,
 * which is a calibration that did not work rather than a hang.
 */
static const SifliRegDef sf32lb52x_audcodec_regs[] = {
    { .off = 0x84 },                    /* PLL_CFG0: FC_VCO */
    { .off = 0x88 },                    /* PLL_CFG1 */
    { .off = 0x8c },                    /* PLL_CFG2 */

    { .off = 0xa4, .force1 = BIT(1) },     /* PLL_CAL_CFG: DONE */

    { .off = 0xa8 },                    /* PLL_CAL_RESULT */
};

#define AUDCODEC_SIZE   0x400

static const SifliRegBankDef sf32lb52x_banks[] = {
    {
        .name = "sf32lb52x.hpsys_rcc",
        .size = 0x80,
        .regs = sf32lb52x_hpsys_rcc_regs,
        .nregs = ARRAY_SIZE(sf32lb52x_hpsys_rcc_regs),
        .write_hook = sf32lb52x_hpsys_rcc_write,
    },
    {
        .name = "sf32lb52x.lpsys_rcc",
        .size = 0x1c,
        .regs = sf32lb52x_lpsys_rcc_regs,
        .nregs = ARRAY_SIZE(sf32lb52x_lpsys_rcc_regs),
    },
    {
        .name = "sf32lb52x.hpsys_cfg",
        .size = HPSYS_CFG_SIZE,
        .regs = sf32lb52x_hpsys_cfg_regs,
        .nregs = ARRAY_SIZE(sf32lb52x_hpsys_cfg_regs),
    },
    {
        .name = "sf32lb52x.hpsys_aon",
        .size = 0x40,
        .regs = sf32lb52x_hpsys_aon_regs,
        .nregs = ARRAY_SIZE(sf32lb52x_hpsys_aon_regs),
        .read_hook = sf32lb52x_hpsys_aon_read,
    },
    {
        .name = "sf32lb52x.lpsys_aon",
        .size = 0x108,
        .regs = sf32lb52x_lpsys_aon_regs,
        .nregs = ARRAY_SIZE(sf32lb52x_lpsys_aon_regs),
    },
    {
        .name = "sf32lb52x.pmuc",
        .size = 0xa0,
        .regs = sf32lb52x_pmuc_regs,
        .nregs = ARRAY_SIZE(sf32lb52x_pmuc_regs),
    },
    {
        .name = "sf32lb52x.rtc",
        .size = RTC_SIZE,
        .regs = sf32lb52x_rtc_regs,
        .nregs = ARRAY_SIZE(sf32lb52x_rtc_regs),
    },
    {
        .name = "sf32lb52x.mpi1",
        .size = MPI_SIZE,
        .regs = sf32lb52x_mpi_regs,
        .nregs = ARRAY_SIZE(sf32lb52x_mpi_regs),
    },
    {
        .name = "sf32lb52x.mpi2",
        .size = MPI_SIZE,
        .regs = sf32lb52x_mpi_regs,
        .nregs = ARRAY_SIZE(sf32lb52x_mpi_regs),
    },
    {
        .name = "sf32lb52x.audprc",
        .size = AUDPRC_SIZE,
        .regs = sf32lb52x_audprc_regs,
        .nregs = ARRAY_SIZE(sf32lb52x_audprc_regs),
    },
    {
        .name = "sf32lb52x.audcodec",
        .size = AUDCODEC_SIZE,
        .regs = sf32lb52x_audcodec_regs,
        .nregs = ARRAY_SIZE(sf32lb52x_audcodec_regs),
    },
};

/*
 * Where the machine should put them. The bank is looked up by name from
 * sf32lb52x_banks above, so the order here is only for readability.
 */
const Sf32lb52xRegBank sf32lb52x_reg_banks[] = {
    { .bank = "sf32lb52x.hpsys_rcc", .base = SF32LB52X_HPSYS_RCC_BASE },
    { .bank = "sf32lb52x.lpsys_rcc", .base = SF32LB52X_LPSYS_RCC_BASE },
    { .bank = "sf32lb52x.hpsys_cfg", .base = SF32LB52X_HPSYS_CFG_BASE },
    { .bank = "sf32lb52x.hpsys_aon", .base = SF32LB52X_HPSYS_AON_BASE },
    { .bank = "sf32lb52x.lpsys_aon", .base = SF32LB52X_LPSYS_AON_BASE },
    { .bank = "sf32lb52x.pmuc",      .base = SF32LB52X_PMUC_BASE },
    { .bank = "sf32lb52x.rtc",       .base = SF32LB52X_RTC_BASE },
    { .bank = "sf32lb52x.mpi1",      .base = SF32LB52X_MPI1_BASE },
    { .bank = "sf32lb52x.mpi2",      .base = SF32LB52X_MPI2_BASE },
    { .bank = "sf32lb52x.audprc",    .base = SF32LB52X_AUDPRC_BASE },
    { .bank = "sf32lb52x.audcodec",  .base = SF32LB52X_AUDCODEC_BASE },
};

const unsigned sf32lb52x_num_reg_banks = ARRAY_SIZE(sf32lb52x_reg_banks);

/*
 * Both DMA controllers are modelled. The HAL allocates channels by taking the
 * first free one and may move a request between them, so all eight channels
 * of each are live; see the CSELR note in hw/dma/sifli-dma.c.
 */
const Sf32lb52xDma sf32lb52x_dmas[] = {
    {
        .id = SF32LB52X_DMA_1,
        .base = SF32LB52X_DMAC1_BASE,
        .irq = SF32LB52X_IRQ_DMAC1_CH1,
    }, {
        .id = SF32LB52X_DMA_2,
        .base = SF32LB52X_DMAC2_BASE,
        .irq = SF32LB52X_IRQ_DMAC2_CH1,
    },
};

const unsigned sf32lb52x_num_dmas = ARRAY_SIZE(sf32lb52x_dmas);

/*
 * All five USARTs are modelled. Only the first is wired to a chardev by the
 * machine; the rest exist so that firmware probing them does not hang.
 *
 * The DMA request numbers come from customer/boards/.../sf32lb52x/dma_config.h
 * and are named in include/hw/arm/sf32lb52x.h. They are a property of the
 * board's routing rather than of the SoC, but every board in the SDK uses the
 * same ones, and a request number that nothing answers to only means a
 * reference firmware never gets its bytes.
 */
const Sf32lb52xUsart sf32lb52x_usarts[] = {
    {
        .base = SF32LB52X_USART1_BASE,
        .irq = SF32LB52X_IRQ_USART1,
        .dma_ctrl = SF32LB52X_DMA_1,
        .dma_rx_req = SF32LB52X_REQ_USART1_RX,
    }, {
        .base = SF32LB52X_USART2_BASE,
        .irq = SF32LB52X_IRQ_USART2,
        .dma_ctrl = SF32LB52X_DMA_1,
        .dma_rx_req = SF32LB52X_REQ_USART2_RX,
    }, {
        .base = SF32LB52X_USART3_BASE,
        .irq = SF32LB52X_IRQ_USART3,
        .dma_ctrl = SF32LB52X_DMA_1,
        .dma_rx_req = SF32LB52X_REQ_USART3_RX,
    }, {
        .base = SF32LB52X_USART4_BASE,
        .irq = SF32LB52X_IRQ_USART4,
        .dma_ctrl = SF32LB52X_DMA_2,
        .dma_rx_req = SF32LB52X_REQ_USART4_RX,
    }, {
        .base = SF32LB52X_USART5_BASE,
        .irq = SF32LB52X_IRQ_USART5,
        .dma_ctrl = SF32LB52X_DMA_2,
        .dma_rx_req = SF32LB52X_REQ_USART5_RX,
    },
};

const unsigned sf32lb52x_num_usarts = ARRAY_SIZE(sf32lb52x_usarts);

/*
 * All four I2C controllers. I2C4 is the odd one: it sits next in the register
 * window but its interrupt line is numbered below I2C3's, which is why the
 * table carries the number rather than deriving it.
 */
const Sf32lb52xI2c sf32lb52x_i2cs[] = {
    {
        .base = SF32LB52X_I2C1_BASE,
        .irq = SF32LB52X_IRQ_I2C1,
    }, {
        .base = SF32LB52X_I2C2_BASE,
        .irq = SF32LB52X_IRQ_I2C2,
    }, {
        .base = SF32LB52X_I2C3_BASE,
        .irq = SF32LB52X_IRQ_I2C3,
    }, {
        .base = SF32LB52X_I2C4_BASE,
        .irq = SF32LB52X_IRQ_I2C4,
    },
};

const unsigned sf32lb52x_num_i2cs = ARRAY_SIZE(sf32lb52x_i2cs);

static void sf32lb52x_periph_register_types(void)
{
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(sf32lb52x_banks); i++) {
        sifli_regbank_register(&sf32lb52x_banks[i]);
    }
}

type_init(sf32lb52x_periph_register_types)
