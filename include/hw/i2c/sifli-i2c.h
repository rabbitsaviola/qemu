/*
 * SiFli SF32LB52x I2C controller
 *
 * Register map from the SiFli SDK's drivers/cmsis/sf32lb52x/i2c.h. The block
 * is a flat array of 32-bit registers; the layout is the same on every
 * SF32LB5x series, so one model serves them all. The base address is a
 * machine property (52x/57x and 56x/58x put HPSYS in different windows).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_I2C_SIFLI_I2C_H
#define HW_I2C_SIFLI_I2C_H

#include "hw/i2c/i2c.h"
#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_SIFLI_I2C "sifli-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(SifliI2CState, SIFLI_I2C)

/*
 * Register offsets, in words. DBR and FIFO are byte-wide ports on the same
 * bus; the blocking and interrupt paths use DBR, the DMA engine uses FIFO.
 * Only DBR is modelled -- the DMA path would need the DMA engine to drive it.
 */
enum {
    SIFLI_I2C_CR = 0,
    SIFLI_I2C_TCR,
    SIFLI_I2C_IER,
    SIFLI_I2C_SR,
    SIFLI_I2C_DBR,
    SIFLI_I2C_SAR,
    SIFLI_I2C_LCR,
    SIFLI_I2C_WCR,
    SIFLI_I2C_RCCR,
    SIFLI_I2C_BMR,
    SIFLI_I2C_DNR,
    SIFLI_I2C_RSVD1,
    SIFLI_I2C_FIFO,
    SIFLI_I2C_NREGS,
};

/* The registers end at 0x30; the window is rounded up to a power of two. */
#define SIFLI_I2C_MMIO_SIZE     0x40

/* CR */
#define SIFLI_I2C_CR_IUE        BIT(2)      /* unit enable */
#define SIFLI_I2C_CR_DMAEN      BIT(4)
#define SIFLI_I2C_CR_LASTNACK   BIT(5)
#define SIFLI_I2C_CR_LASTSTOP   BIT(6)
#define SIFLI_I2C_CR_MSDE       BIT(8)      /* master mode enable */
#define SIFLI_I2C_CR_RSTREQ     BIT(30)     /* self-clearing reset request */
#define SIFLI_I2C_CR_UR         BIT(31)

/*
 * TCR. Every bit is a command strobe, not a stored state: the HAL assigns the
 * register outright and the hardware acts on the write.
 */
#define SIFLI_I2C_TCR_TB        BIT(0)      /* send/receive one byte */
#define SIFLI_I2C_TCR_START     BIT(1)
#define SIFLI_I2C_TCR_STOP      BIT(2)
#define SIFLI_I2C_TCR_NACK      BIT(3)      /* NACK the byte just received */

/*
 * SR and IER share bit positions, which is what makes the interrupt test a
 * plain mask. SR bits 0-3 (RWM, NACK, UB, IBB) have no IER counterpart.
 */
#define SIFLI_I2C_SR_NACK       BIT(1)
#define SIFLI_I2C_SR_UB         BIT(2)      /* unit busy */
#define SIFLI_I2C_SR_ALD        BIT(5)      /* arbitration lost */
#define SIFLI_I2C_SR_TE         BIT(6)      /* tx empty: byte went out */
#define SIFLI_I2C_SR_RF         BIT(7)      /* rx full: byte came in */
#define SIFLI_I2C_SR_SAD        BIT(9)      /* slave address detected */
#define SIFLI_I2C_SR_BED        BIT(10)     /* byte error / NACK received */
#define SIFLI_I2C_SR_EBB        BIT(11)     /* bus error */
#define SIFLI_I2C_SR_MSD        BIT(12)     /* master stop detected */
#define SIFLI_I2C_SR_OF         BIT(14)
#define SIFLI_I2C_SR_UF         BIT(15)

#define SIFLI_I2C_IER_TEIE      SIFLI_I2C_SR_TE
#define SIFLI_I2C_IER_RFIE      SIFLI_I2C_SR_RF
#define SIFLI_I2C_IER_BEDIE     SIFLI_I2C_SR_BED
#define SIFLI_I2C_IER_MSDIE     SIFLI_I2C_SR_MSD

struct SifliI2CState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;

    /* The bus the controller masters. Created in realize. */
    I2CBus *bus;

    /*
     * What that bus is called. QEMU addresses a bus by name, so this is how a
     * slave picks a controller: -device ft6146,bus=i2c2. The machine names
     * them after the controllers; unset falls back to the generic "i2c".
     */
    char *bus_name;

    /* Indexed by the enum above. TCR is write-only and never stored. */
    uint32_t regs[SIFLI_I2C_NREGS];

    /* Last byte received; a DBR read returns it. */
    uint8_t rx_byte;

    /*
     * Set between a START and the matching STOP. The HAL's interrupt handler
     * never tells the controller which direction the transfer is in, so the
     * direction has to be latched off the address byte at START time.
     */
    bool active;
    bool recv;
};

#endif /* HW_I2C_SIFLI_I2C_H */
