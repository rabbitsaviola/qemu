/*
 * SiFli SBUS address translation
 *
 * The HCPU addresses flash through the MPI CBUS window at 0x1000_0000. A
 * peripheral that reads flash on its own behalf -- DMA, EZIP, EPIC -- is
 * handed the address in the SBUS window instead, which is the same memory
 * 0x5000_0000 higher, so that the MPI can tell a peripheral access from a
 * CPU one:
 *
 *   drivers/cmsis/sf32lb52x/register.h:
 *     #define HCPU_MPI_SBUS_ADDR(addr) \
 *       (HCPU_IS_MPI_CBUS_ADDR(addr) ? HCPU_MPI_CBUS_ADDR_2_SBUS_ADDR(addr) \
 *                                    : ((uint32_t)(addr)))
 *
 * SRAM and peripherals are not translated: a peripheral sees those at the
 * same address the CPU does, and the macro passes them through unchanged.
 *
 * A device model has to undo that, because the address space it reads the
 * guest through is laid out the way the CPU sees it. Translating blind --
 * subtracting from every address -- would break SRAM, so the window is
 * checked first.
 *
 * The window is 4 MB, not the whole 32 MB XIP range. PSRAM lives at
 * 0x6000_0000, which is where the alias of the first flash address lands, and
 * the SDK's linker starts its PSRAM data at 0x6040_0000 (__PSRAM_BASE in the
 * generated link script). Up to there the two do not overlap, which is what
 * makes the alias usable; past it neither the address alone nor anything else
 * a model can see says which of the two memories was meant. Nothing in this
 * tree hands a peripheral a flash address above 4 MB.
 *
 * The offset is the per-series HPSYS_MPI_MEM_CBUS_2_SBUS_OFFSET. It has the
 * same value on 52x/56x/57x/58x; a series that changes it would need this
 * to become a property of the machine like the peripheral bases are.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_SIFLI_SBUS_H
#define HW_MISC_SIFLI_SBUS_H

#include "qemu/osdep.h"

/* The flash, as the CPU sees it and as a peripheral sees it. */
#define SIFLI_CBUS_FLASH_BASE   0x10000000u
#define SIFLI_SBUS_FLASH_BASE   0x60000000u
#define SIFLI_SBUS_FLASH_SIZE   0x00400000u
#define SIFLI_SBUS_OFFSET       0x50000000u

/* An address a peripheral was given, as an address the CPU could use. */
static inline uint32_t sifli_sbus_to_cpu_addr(uint32_t addr)
{
    if (addr >= SIFLI_SBUS_FLASH_BASE &&
        addr < SIFLI_SBUS_FLASH_BASE + SIFLI_SBUS_FLASH_SIZE) {
        return addr - SIFLI_SBUS_OFFSET;
    }

    return addr;
}

#endif /* HW_MISC_SIFLI_SBUS_H */
