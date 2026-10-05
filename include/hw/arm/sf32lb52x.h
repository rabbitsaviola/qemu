/*
 * SiFli SF32LB52x SoC definitions
 *
 * Addresses and interrupt numbers are taken verbatim from the SiFli SDK:
 *   drivers/cmsis/sf32lb52x/mem_map.h
 *   drivers/cmsis/sf32lb52x/register.h
 *
 * This header is series-specific on purpose: peripheral base addresses and
 * IRQ numbers differ between the SF32LB5x series (see the HPSYS/LPSYS note
 * below), so each series gets its own header rather than sharing one.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_SF32LB52X_H
#define HW_ARM_SF32LB52X_H

#include "qemu/units.h"

/*
 * Boot flash (the QSPI XIP window the CPU fetches from).
 *
 * Which QSPI instance carries the boot flash depends on the part number:
 * some parts map it at QSPI1, others at QSPI2. It is therefore a machine
 * property (-M sf32lb52x,flash-base=...) and not a compile-time constant.
 * mem_map.h defines FLASH_BASE_ADDR as QSPI1_MEM_BASE, but that is only the
 * default for the parts currently in the SDK.
 */
#define SF32LB52X_QSPI1_MEM_BASE    0x10000000ULL
#define SF32LB52X_QSPI2_MEM_BASE    0x12000000ULL

/*
 * Default size of the XIP window, used when the machine is not told otherwise
 * via -M sf32lb52x,flash-size=...
 *
 * This is the size of the address window (mem_map.h QSPI1_MAX_SIZE), not the
 * capacity of any particular flash chip: QSPI2 starts at 0x12000000, exactly
 * 32 MB above QSPI1. Sizing the window to its full extent means a firmware
 * built for a larger part is never silently truncated.
 */
#define SF32LB52X_FLASH_SIZE        (32 * MiB)

/*
 * The clock the firmware runs at, and therefore the one the machine must
 * clock SysTick from.
 *
 * This is not free to choose: HAL_RCC_GetSysCLKFreq() computes the frequency
 * the firmware believes in from the RCC registers, and the RCC table in
 * hw/arm/sf32lb52x-periph.c is set up so that it returns 48 MHz (the board's
 * HXT48 crystal) without consulting the PLL. If the SysTick clock here and
 * that register state ever disagree, the firmware's 1 ms tick is wrong by the
 * ratio.
 */
#define SF32LB52X_HXT48_FRQ         48000000ULL

/* On-chip RAM: HPSYS RAM0 (128K, also DTCM) + RAM1 (128K) + RAM2 (256K) */
#define SF32LB52X_SRAM_BASE         0x20000000ULL
#define SF32LB52X_SRAM_SIZE         (512 * KiB)

/*
 * Off-chip PSRAM, on MPI1.
 *
 * mem_map.h hangs this off the QSPI1 memory size (PSRAM_BASE 0x60000000,
 * PSRAM_SIZE = BSP_QSPI1_MEM_SIZE), so the capacity is a board property like
 * the flash one: -M sf32lb52x,psram-size=... The default is what the boards
 * currently in the SDK carry and what their linker scripts lay out data in
 * (__PSRAM_BASE 0x60400000, 4 MB of it).
 */
#define SF32LB52X_PSRAM_BASE        0x60000000ULL
#define SF32LB52X_PSRAM_SIZE        (16 * MiB)

/*
 * Peripheral windows.
 *
 * HPSYS sits at 0x5000_0000 on this series. Note that 56x/58x swap the
 * HPSYS and LPSYS windows, so any address here must not be hardcoded in a
 * device model -- the machine passes it in as a property.
 */
#define SF32LB52X_HPSYS_BASE        0x50000000ULL

/* Peripheral base addresses (register.h) */
#define SF32LB52X_USART1_BASE       0x50084000ULL
#define SF32LB52X_USART2_BASE       0x50085000ULL
#define SF32LB52X_USART3_BASE       0x50086000ULL
#define SF32LB52X_USART4_BASE       0x40005000ULL
#define SF32LB52X_USART5_BASE       0x40006000ULL
#define SF32LB52X_GPTIM1_BASE       0x50090000ULL
#define SF32LB52X_GPTIM2_BASE       0x500b0000ULL
#define SF32LB52X_BTIM1_BASE        0x50092000ULL
#define SF32LB52X_BTIM2_BASE        0x500b1000ULL
#define SF32LB52X_DMAC1_BASE        0x50081000ULL
#define SF32LB52X_DMAC2_BASE        0x40001000ULL
#define SF32LB52X_CRC1_BASE         0x50048000ULL
#define SF32LB52X_GPIO1_BASE        0x500a0000ULL
#define SF32LB52X_GPIO2_BASE        0x40080000ULL
#define SF32LB52X_I2C1_BASE         0x5009c000ULL
#define SF32LB52X_SPI1_BASE         0x50095000ULL
#define SF32LB52X_EZIP1_BASE        0x50006000ULL
#define SF32LB52X_EPIC_BASE         0x50007000ULL
#define SF32LB52X_MPI1_BASE         0x50041000ULL
#define SF32LB52X_MPI2_BASE         0x50042000ULL
#define SF32LB52X_HPSYS_RCC_BASE    0x50000000ULL
#define SF32LB52X_LPSYS_RCC_BASE    0x40000000ULL
#define SF32LB52X_HPSYS_CFG_BASE    0x5000b000ULL
#define SF32LB52X_HPSYS_AON_BASE    0x500c0000ULL
#define SF32LB52X_LPSYS_AON_BASE    0x40040000ULL
#define SF32LB52X_PMUC_BASE         0x500ca000ULL
#define SF32LB52X_RTC_BASE          0x500cb000ULL
#define SF32LB52X_AUDPRC_BASE       0x50005000ULL
#define SF32LB52X_AUDCODEC_BASE     0x50088000ULL

/*
 * The USARTs are split across the two buses: 1-3 sit in HPSYS, 4 and 5 in
 * LPSYS. Their interrupt numbers are not contiguous either.
 */
#define SF32LB52X_IRQ_USART4        12
#define SF32LB52X_IRQ_USART5        13

/* Descriptions of the peripherals the machine instantiates. */
typedef struct Sf32lb52xRegBank {
    const char *bank;   /* name of the sifli-regbank table to instantiate */
    uint64_t base;
} Sf32lb52xRegBank;

/* Which DMA controller serves a peripheral; SF32LB52X_DMA_NONE for neither. */
#define SF32LB52X_DMA_NONE          0
#define SF32LB52X_DMA_1             1
#define SF32LB52X_DMA_2             2

/*
 * DMA request numbers -- the value firmware writes into CSELRn.CnS to make a
 * channel serve a given peripheral. Transcribed from the SDK's
 * customer/boards/include/config/sf32lb52x/dma_config.h, which calls the same
 * signals UARTn_*_DMA_REQUEST; the PWMT/PWMA entries are that file's aliases
 * for the PWM mode of the same timer, kept so a lookup by either name works.
 *
 * A number means nothing on its own: it is an index into one controller's
 * request table, and the numbering restarts at zero for each, so USART1_RX is
 * request 5 of DMAC1 while USART4_RX is request 1 of DMAC2. No signal appears
 * on both controllers, so the names carry only the signal -- which controller
 * a number belongs to is what the two groups below are for, as in the SDK.
 */

/* Served by DMAC1 (HPSYS). */
#define SF32LB52X_REQ_FLASH1          0
#define SF32LB52X_REQ_FLASH2          1
#define SF32LB52X_REQ_I2C4            3
#define SF32LB52X_REQ_USART1_TX       4
#define SF32LB52X_REQ_USART1_RX       5
#define SF32LB52X_REQ_USART2_TX       6
#define SF32LB52X_REQ_USART2_RX       7
#define SF32LB52X_REQ_GPTIM1_UPDATE   8
#define SF32LB52X_REQ_GPTIM1_TRIGGER  9
#define SF32LB52X_REQ_GPTIM1_CC1      10
#define SF32LB52X_REQ_GPTIM1_CC2      11
#define SF32LB52X_REQ_GPTIM1_CC3      12
#define SF32LB52X_REQ_GPTIM1_CC4      13
#define SF32LB52X_REQ_PWMT1_UPDATE    SF32LB52X_REQ_GPTIM1_UPDATE
#define SF32LB52X_REQ_PWMT1_TRIGGER   SF32LB52X_REQ_GPTIM1_TRIGGER
#define SF32LB52X_REQ_PWMT1_CC1       SF32LB52X_REQ_GPTIM1_CC1
#define SF32LB52X_REQ_PWMT1_CC2       SF32LB52X_REQ_GPTIM1_CC2
#define SF32LB52X_REQ_PWMT1_CC3       SF32LB52X_REQ_GPTIM1_CC3
#define SF32LB52X_REQ_PWMT1_CC4       SF32LB52X_REQ_GPTIM1_CC4
#define SF32LB52X_REQ_BTIM1           14
#define SF32LB52X_REQ_BTIM2           15
#define SF32LB52X_REQ_ATIM1_UPDATE    16
#define SF32LB52X_REQ_ATIM1_TRIGGER   17
#define SF32LB52X_REQ_ATIM1_CC1       18
#define SF32LB52X_REQ_ATIM1_CC2       19
#define SF32LB52X_REQ_ATIM1_CC3       20
#define SF32LB52X_REQ_ATIM1_CC4       21
#define SF32LB52X_REQ_PWMA1_UPDATE    SF32LB52X_REQ_ATIM1_UPDATE
#define SF32LB52X_REQ_PWMA1_TRIGGER   SF32LB52X_REQ_ATIM1_TRIGGER
#define SF32LB52X_REQ_PWMA1_CC1       SF32LB52X_REQ_ATIM1_CC1
#define SF32LB52X_REQ_PWMA1_CC2       SF32LB52X_REQ_ATIM1_CC2
#define SF32LB52X_REQ_PWMA1_CC3       SF32LB52X_REQ_ATIM1_CC3
#define SF32LB52X_REQ_PWMA1_CC4       SF32LB52X_REQ_ATIM1_CC4
#define SF32LB52X_REQ_I2C1            22
#define SF32LB52X_REQ_I2C2            23
#define SF32LB52X_REQ_I2C3            24
#define SF32LB52X_REQ_ATIM1_COM       25
#define SF32LB52X_REQ_USART3_TX       26
#define SF32LB52X_REQ_USART3_RX       27
#define SF32LB52X_REQ_SPI1_TX         28
#define SF32LB52X_REQ_SPI1_RX         29
#define SF32LB52X_REQ_SPI2_TX         30
#define SF32LB52X_REQ_SPI2_RX         31
#define SF32LB52X_REQ_I2S_TX          32
#define SF32LB52X_REQ_I2S_RX          33
#define SF32LB52X_REQ_PDM1_L          36
#define SF32LB52X_REQ_PDM1_R          37
#define SF32LB52X_REQ_GPADC           38
#define SF32LB52X_REQ_AUDCODEC_ADC0   39
#define SF32LB52X_REQ_AUDCODEC_ADC1   40
#define SF32LB52X_REQ_AUDCODEC_DAC0   41
#define SF32LB52X_REQ_AUDCODEC_DAC1   42
#define SF32LB52X_REQ_GPTIM2_UPDATE   43
#define SF32LB52X_REQ_GPTIM2_TRIGGER  44
#define SF32LB52X_REQ_GPTIM2_CC1      45
#define SF32LB52X_REQ_PWMT2_UPDATE    SF32LB52X_REQ_GPTIM2_UPDATE
#define SF32LB52X_REQ_PWMT2_TRIGGER   SF32LB52X_REQ_GPTIM2_TRIGGER
#define SF32LB52X_REQ_PWMT2_CC1       SF32LB52X_REQ_GPTIM2_CC1
#define SF32LB52X_REQ_AUDPRC_TX_OUT1  46
#define SF32LB52X_REQ_AUDPRC_TX_OUT0  47
#define SF32LB52X_REQ_AUDPRC_TX3      48
#define SF32LB52X_REQ_AUDPRC_TX2      49
#define SF32LB52X_REQ_AUDPRC_TX1      50
#define SF32LB52X_REQ_AUDPRC_TX0      51
#define SF32LB52X_REQ_AUDPRC_RX1      52
#define SF32LB52X_REQ_AUDPRC_RX0      53
#define SF32LB52X_REQ_GPTIM2_CC2      54
#define SF32LB52X_REQ_GPTIM2_CC3      55
#define SF32LB52X_REQ_GPTIM2_CC4      56
#define SF32LB52X_REQ_PWMT2_CC2       SF32LB52X_REQ_GPTIM2_CC2
#define SF32LB52X_REQ_PWMT2_CC3       SF32LB52X_REQ_GPTIM2_CC3
#define SF32LB52X_REQ_PWMT2_CC4       SF32LB52X_REQ_GPTIM2_CC4
#define SF32LB52X_REQ_SDMMC1          57

/* Served by DMAC2 (LPSYS). */
#define SF32LB52X_REQ_USART4_TX       0
#define SF32LB52X_REQ_USART4_RX       1
#define SF32LB52X_REQ_USART5_TX       2
#define SF32LB52X_REQ_USART5_RX       3
#define SF32LB52X_REQ_BTIM3           6
#define SF32LB52X_REQ_BTIM4           7

typedef struct Sf32lb52xDma {
    unsigned id;        /* SF32LB52X_DMA_1 or _2 */
    uint64_t base;
    unsigned irq;       /* interrupt number of channel 1 */
} Sf32lb52xDma;

typedef struct Sf32lb52xUsart {
    uint64_t base;
    unsigned irq;
    /*
     * The DMA request the receiver is wired to, and on which controller.
     * From the board's dma_config.h; a USART with SF32LB52X_DMA_NONE has no
     * request line and its dma_rx_req is not looked at.
     */
    unsigned dma_ctrl;
    unsigned dma_rx_req;
} Sf32lb52xUsart;

extern const Sf32lb52xRegBank sf32lb52x_reg_banks[];
extern const unsigned sf32lb52x_num_reg_banks;
extern const Sf32lb52xDma sf32lb52x_dmas[];
extern const unsigned sf32lb52x_num_dmas;
extern const Sf32lb52xUsart sf32lb52x_usarts[];
extern const unsigned sf32lb52x_num_usarts;

/*
 * External interrupt numbers (IRQn_Type, HCPU side).
 *
 * The set below is identical across 52x/56x/57x/58x; numbers that differ
 * per series are listed at the bottom and must be checked before use.
 */
/*
 * Each DMA controller interrupts on one line per channel, numbered
 * consecutively from the first.
 */
#define SF32LB52X_IRQ_DMA_NUM       8
#define SF32LB52X_IRQ_DMAC1_CH1     50
#define SF32LB52X_IRQ_DMAC2_CH1     2
#define SF32LB52X_IRQ_USART1        59
#define SF32LB52X_IRQ_SPI1          60
#define SF32LB52X_IRQ_I2C1          61
#define SF32LB52X_IRQ_EPIC          62
#define SF32LB52X_IRQ_GPTIM1        70
#define SF32LB52X_IRQ_GPTIM2        71
#define SF32LB52X_IRQ_BTIM1         72
#define SF32LB52X_IRQ_BTIM2         73
#define SF32LB52X_IRQ_USART2        74
#define SF32LB52X_IRQ_SPI2          75
#define SF32LB52X_IRQ_I2C2          76
#define SF32LB52X_IRQ_GPIO1         84
#define SF32LB52X_IRQ_MPI1          85
#define SF32LB52X_IRQ_MPI2          86
#define SF32LB52X_IRQ_EZIP          89
#define SF32LB52X_IRQ_I2C3          93
#define SF32LB52X_IRQ_USART3        95
/* CRC has no interrupt line on this series. */

/*
 * Number of NVIC external interrupt lines to instantiate.
 * The highest number used is SECU1_IRQn = 98. armv7m's limit is 496.
 */
#define SF32LB52X_NUM_IRQ           99

/*
 * Numbers that differ between series (check before use on another series):
 *   LPTIM2: 52x/57x = 47, 56x/58x = 42
 *   GPIO2 : 52x = 20,      56x/58x = 35
 *   GPADC : 52x/57x = 65,  56x/58x = 28
 */

#endif /* HW_ARM_SF32LB52X_H */
