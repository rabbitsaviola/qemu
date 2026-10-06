/*
 * SiFli SF32LB52x GPIO
 *
 * The block is a set of 32-pin register banks, one bank every 0x80 bytes:
 * GPIO1 covers PA00-PA63 in two banks, GPIO2 PB00-PB31 in one. Everything a
 * pin needs is in its own bank, so the model is the same code repeated per
 * bank rather than anything that knows about ports.
 *
 * The bank size is not sizeof(register struct): the register map has a
 * reserved gap between bank0 and bank1, and the SDK says so in as many words
 * (drivers/ll/sf32lb52x/ll_gpio.h). Bank1 therefore sits at base + 0x80.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_SIFLI_GPIO_H
#define HW_MISC_SIFLI_GPIO_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_SIFLI_GPIO "sifli-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(SifliGpioState, SIFLI_GPIO)

#define SIFLI_GPIO_BANK_SIZE    0x80
#define SIFLI_GPIO_PINS_PER_BANK 32
#define SIFLI_GPIO_MAX_BANKS    2

/*
 * Offsets within a bank. The set and clear registers are write-1 strobes that
 * update the readback register beside them; the HAL reads those back, so the
 * pairing has to be honoured rather than treated as two independent latches.
 */
enum {
    SIFLI_GPIO_DIR = 0x00,      /* input levels, read-only */
    SIFLI_GPIO_DOR = 0x04,      /* output latch */
    SIFLI_GPIO_DOSR = 0x08,     /* W1S: DOR */
    SIFLI_GPIO_DOCR = 0x0c,     /* W1C: DOR */
    SIFLI_GPIO_DOER = 0x10,     /* output enable */
    SIFLI_GPIO_DOESR = 0x14,    /* W1S: DOER */
    SIFLI_GPIO_DOECR = 0x18,    /* W1C: DOER */
    SIFLI_GPIO_IER = 0x1c,      /* interrupt enable */
    SIFLI_GPIO_IESR = 0x20,     /* W1S: IER */
    SIFLI_GPIO_IECR = 0x24,     /* W1C: IER */
    SIFLI_GPIO_ITR = 0x28,      /* 1 = edge triggered, 0 = level */
    SIFLI_GPIO_ITSR = 0x2c,     /* W1S: ITR */
    SIFLI_GPIO_ITCR = 0x30,     /* W1C: ITR */
    SIFLI_GPIO_IPHR = 0x34,     /* high level / rising edge */
    SIFLI_GPIO_IPHSR = 0x38,    /* W1S: IPHR */
    SIFLI_GPIO_IPHCR = 0x3c,    /* W1C: IPHR */
    SIFLI_GPIO_IPLR = 0x40,     /* low level / falling edge */
    SIFLI_GPIO_IPLSR = 0x44,    /* W1S: IPLR */
    SIFLI_GPIO_IPLCR = 0x48,    /* W1C: IPLR */
    SIFLI_GPIO_ISR = 0x4c,      /* W1C: latched interrupt status */
    SIFLI_GPIO_IER_EXT = 0x50,
    SIFLI_GPIO_IESR_EXT = 0x54,
    SIFLI_GPIO_IECR_EXT = 0x58,
    SIFLI_GPIO_ISR_EXT = 0x5c,
    SIFLI_GPIO_OEMR = 0x60,     /* open-drain mode */
    SIFLI_GPIO_OEMSR = 0x64,    /* W1S: OEMR */
    SIFLI_GPIO_OEMCR = 0x68,    /* W1C: OEMR */
    SIFLI_GPIO_NREGS = 0x6c / 4,
};

typedef struct SifliGpioBank {
    /*
     * Level driven onto the pins from outside the block -- by another device,
     * or by a pulse generator. What a pin reads back at is this or the
     * output latch, depending on whether its driver is on; see
     * sifli_gpio_levels().
     */
    uint32_t in;

    uint32_t dor;
    uint32_t doer;
    uint32_t ier;
    uint32_t itr;
    uint32_t iphr;
    uint32_t iplr;
    uint32_t isr;

    /*
     * The second core's copies. On this SoC they are how the *other* core's
     * GPIO2 raises an interrupt on this one; nothing here uses them, so they
     * are stored and otherwise ignored.
     */
    uint32_t ier_ext;
    uint32_t isr_ext;

    uint32_t oemr;
} SifliGpioBank;

struct SifliGpioState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;

    /* Set from the machine; GPIO1 has two banks, GPIO2 one. */
    uint32_t num_banks;

    SifliGpioBank bank[SIFLI_GPIO_MAX_BANKS];
};

#endif /* HW_MISC_SIFLI_GPIO_H */
