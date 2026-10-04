/*
 * ARMv7-M Data Watchpoint and Trace unit (DWT)
 *
 * Only the cycle counter, which is the part firmware uses as a stopwatch:
 * HAL_Delay_us_() writes DWT_CYCCNT and spins until it has advanced by the
 * number of cycles the delay should take.
 *
 * QEMU's M-profile CPU does not implement the DWT at all -- accesses to the
 * 0xe0001000 page land in the armv7m's "nvic-default" region, which swallows
 * them and reads back zero. A counter that never moves turns that delay loop
 * into an infinite one, so boards whose firmware uses it have to provide a
 * counter of their own.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_ARMV7M_DWT_H
#define HW_MISC_ARMV7M_DWT_H

#include "hw/clock.h"
#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_ARMV7M_DWT "armv7m-dwt"
OBJECT_DECLARE_SIMPLE_TYPE(ArmV7mDwtState, ARMV7M_DWT)

/* Architectural address of the DWT, in the M-profile PPB. */
#define ARMV7M_DWT_BASE         0xe0001000

/* Offsets within that page. */
#define DWT_CTRL    0x000
#define DWT_CYCCNT  0x004

/*
 * The DWT shares its page with the trace units; the whole page is claimed so
 * that a stray access reports against one region rather than falling through.
 */
#define ARMV7M_DWT_MMIO_SIZE    0x1000

struct ArmV7mDwtState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;

    /* The CPU clock, which is what DWT_CYCCNT counts. */
    Clock *clk;

    uint32_t ctrl;

    /*
     * Host time at which the cycle counter last read zero. The count is
     * derived from the clock at read time rather than incremented per
     * instruction; see the comment in armv7m_dwt_cycles().
     */
    int64_t base_ns;
};

#endif /* HW_MISC_ARMV7M_DWT_H */
