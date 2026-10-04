/*
 * SiFli USART
 *
 * Register map from the SiFli SDK's drivers/cmsis/Include/usart.h, which is
 * shared by every SF32LB5x series (unlike RCC/AON/PMU, whose layouts differ
 * per series and live in each series' own directory). One model therefore
 * serves all of them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHAR_SIFLI_USART_H
#define HW_CHAR_SIFLI_USART_H

#include "chardev/char-fe.h"
#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_SIFLI_USART "sifli-usart"
OBJECT_DECLARE_SIMPLE_TYPE(SifliUsartState, SIFLI_USART)

/*
 * Register offsets, in words. The block is a flat array of 32-bit registers
 * with no bitfields, so an index is the whole layout.
 *
 * DRDR/DTDR/EXR only exist on series other than 55x/58x, but their offsets
 * are inside the window on every series, so the array covers them throughout.
 */
enum {
    USART_CR1 = 0,
    USART_CR2,
    USART_CR3,
    USART_BRR,
    USART_GTPR,
    USART_RTOR,
    USART_RQR,
    USART_ISR,
    USART_ICR,
    USART_RDR,
    USART_TDR,
    USART_MISCR,
    USART_DRDR,
    USART_DTDR,
    USART_EXR,
    USART_NREGS,
};

/* The registers end at 0x3C; the window is rounded up to a power of two. */
#define SIFLI_USART_MMIO_SIZE   0x40

struct SifliUsartState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    CharFrontend chr;
    qemu_irq irq;

    /*
     * Indexed by the enum above. USART_ISR holds only the sticky bits
     * (RXNE, TC, ORE, the error flags); the bits that describe the
     * transmitter's instantaneous state are synthesised on read.
     */
    uint32_t regs[USART_NREGS];
};

#endif /* HW_CHAR_SIFLI_USART_H */
