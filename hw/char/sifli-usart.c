/*
 * SiFli USART
 *
 * Register map from the SiFli SDK's drivers/cmsis/Include/usart.h. See
 * include/hw/char/sifli-usart.h.
 *
 * Thin semantics: the model reproduces the states the HAL waits for, and
 * nothing about how long they take to arrive.
 *
 *   - TXE (transmit holding empty) always reads set. There is no FIFO to
 *     fill and no shift register to drain, so a "write TDR, spin until TXE"
 *     loop falls through on the first test.
 *   - TC (transmission complete) is set at reset and after every write to
 *     TDR. Software can still clear it through ICR, and the next byte sets
 *     it again, so the usual "clear TC, send, wait for TC" sequence works.
 *   - TEACK/REACK mirror TE/RE, which is what the enable sequence polls.
 *   - BUSY (ISR bit 16) and EXR read 0: nothing is ever in flight.
 *   - The receiver holds one byte. RXNE is raised when a character arrives
 *     and cleared by reading RDR; until then the chardev is asked to hold
 *     off, so no character is dropped and no overrun is reported.
 *   - The DMA receive request mirrors RXNE while CR3.DMAR is set, and IDLE is
 *     raised once a burst has been drained; see sifli_usart_update_dma_req()
 *     and sifli_usart_idle_bh().
 *   - BRR, GTPR, RTOR, CR2 and MISCR are stored and read back but have no
 *     effect. Baud rate, flow control and the sampling point do not change
 *     what a character looks like to the host.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/char/sifli-usart.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"

/* CR1 */
#define USART_CR1_UE        BIT(0)
#define USART_CR1_RE        BIT(2)
#define USART_CR1_TE        BIT(3)
#define USART_CR1_IDLEIE    BIT(4)
#define USART_CR1_RXNEIE    BIT(5)
#define USART_CR1_TCIE      BIT(6)
#define USART_CR1_TXEIE     BIT(7)
#define USART_CR1_PEIE      BIT(8)
#define USART_CR1_RTOIE     BIT(25)
#define USART_CR1_EOBIE     BIT(26)

/*
 * ISR.
 *
 * Note that the error/enable flags are not at the same bit positions as
 * their CR1 counterparts (PEIE is CR1 bit 8 but PE is ISR bit 0), so the
 * interrupt condition cannot be expressed as a single AND of the two
 * registers. ABREN sits at bit 14, not 13.
 */
#define USART_ISR_PE        BIT(0)
#define USART_ISR_FE        BIT(1)
#define USART_ISR_NF        BIT(2)
#define USART_ISR_ORE       BIT(3)
#define USART_ISR_IDLE      BIT(4)
#define USART_ISR_RXNE      BIT(5)
#define USART_ISR_TC        BIT(6)
#define USART_ISR_TXE       BIT(7)
#define USART_ISR_LBDF      BIT(8)
#define USART_ISR_CTSIF     BIT(9)
#define USART_ISR_RTOF      BIT(11)
#define USART_ISR_EOBF      BIT(12)
#define USART_ISR_BUSY      BIT(16)
#define USART_ISR_CMF       BIT(17)
#define USART_ISR_WUF       BIT(20)
#define USART_ISR_TEACK     BIT(21)
#define USART_ISR_REACK     BIT(22)
#define USART_ISR_TCBGT     BIT(25)

/* ICR -- write 1 to clear the matching ISR bit. */
#define USART_ICR_PECF      BIT(0)
#define USART_ICR_FECF      BIT(1)
#define USART_ICR_NCF       BIT(2)
#define USART_ICR_ORECF     BIT(3)
#define USART_ICR_IDLECF    BIT(4)
#define USART_ICR_TCCF      BIT(6)
#define USART_ICR_TCBGTCF   BIT(7)
#define USART_ICR_LBDCF     BIT(8)
#define USART_ICR_CTSCF     BIT(9)
#define USART_ICR_RTOCF     BIT(11)
#define USART_ICR_EOBCF     BIT(12)
#define USART_ICR_CMCF      BIT(17)
#define USART_ICR_WUCF      BIT(20)

/* CR3 */
#define USART_CR3_DMAR      BIT(6)

/* RDR and TDR carry 9 data bits. */
#define USART_DATA_MASK     0x1ff

/*
 * The value software sees in ISR: the sticky bits plus the ones that
 * describe the transmitter's state right now.
 */
static uint32_t sifli_usart_isr(SifliUsartState *s)
{
    uint32_t cr1 = s->regs[USART_CR1];
    uint32_t v = s->regs[USART_ISR] | USART_ISR_TXE;

    if (cr1 & USART_CR1_TE) {
        v |= USART_ISR_TEACK;
    }
    if (cr1 & USART_CR1_RE) {
        v |= USART_ISR_REACK;
    }
    return v;
}

/*
 * Translate an ICR write into the ISR bits it clears. The positions do not
 * line up (TCBGTCF is ICR bit 7 but TCBGT is ISR bit 25), so this is done by
 * name rather than by shifting a mask across.
 */
static uint32_t sifli_usart_icr_to_isr(uint32_t icr)
{
    uint32_t isr = 0;

    if (icr & USART_ICR_PECF) {
        isr |= USART_ISR_PE;
    }
    if (icr & USART_ICR_FECF) {
        isr |= USART_ISR_FE;
    }
    if (icr & USART_ICR_NCF) {
        isr |= USART_ISR_NF;
    }
    if (icr & USART_ICR_ORECF) {
        isr |= USART_ISR_ORE;
    }
    if (icr & USART_ICR_IDLECF) {
        isr |= USART_ISR_IDLE;
    }
    if (icr & USART_ICR_TCCF) {
        isr |= USART_ISR_TC;
    }
    if (icr & USART_ICR_TCBGTCF) {
        isr |= USART_ISR_TCBGT;
    }
    if (icr & USART_ICR_LBDCF) {
        isr |= USART_ISR_LBDF;
    }
    if (icr & USART_ICR_CTSCF) {
        isr |= USART_ISR_CTSIF;
    }
    if (icr & USART_ICR_RTOCF) {
        isr |= USART_ISR_RTOF;
    }
    if (icr & USART_ICR_EOBCF) {
        isr |= USART_ISR_EOBF;
    }
    if (icr & USART_ICR_CMCF) {
        isr |= USART_ISR_CMF;
    }
    if (icr & USART_ICR_WUCF) {
        isr |= USART_ISR_WUF;
    }
    return isr;
}

static void sifli_usart_update_irq(SifliUsartState *s)
{
    uint32_t isr = sifli_usart_isr(s);
    uint32_t cr1 = s->regs[USART_CR1];
    int level = 0;

    if ((isr & USART_ISR_RXNE) && (cr1 & USART_CR1_RXNEIE)) {
        level = 1;
    }
    if ((isr & USART_ISR_TC) && (cr1 & USART_CR1_TCIE)) {
        level = 1;
    }
    if ((isr & USART_ISR_TXE) && (cr1 & USART_CR1_TXEIE)) {
        level = 1;
    }
    if ((isr & USART_ISR_IDLE) && (cr1 & USART_CR1_IDLEIE)) {
        level = 1;
    }
    if ((isr & USART_ISR_PE) && (cr1 & USART_CR1_PEIE)) {
        level = 1;
    }
    if ((isr & USART_ISR_RTOF) && (cr1 & USART_CR1_RTOIE)) {
        level = 1;
    }
    if ((isr & USART_ISR_EOBF) && (cr1 & USART_CR1_EOBIE)) {
        level = 1;
    }

    qemu_set_irq(s->irq, level);
}

/*
 * The DMA receive request.
 *
 * It mirrors the receiver's state instead of latching an edge, so a request
 * raised while no channel was listening is still there to be found when one
 * is pointed at it -- which is what happens when the console is reopened and
 * a character lands before the HAL has set the channel up.
 */
static void sifli_usart_update_dma_req(SifliUsartState *s)
{
    bool rx = (s->regs[USART_ISR] & USART_ISR_RXNE) &&
              (s->regs[USART_CR3] & USART_CR3_DMAR);

    qemu_set_irq(s->dma_rx, rx);
}

/*
 * IDLE, meaning "the DMA has drained this burst".
 *
 * On hardware this is the line sitting quiet for a character time after the
 * last character. Here it is raised when that character is taken out of RDR
 * and nothing follows it, which is the condition the SDK's uart_isr acts on:
 * it reads the DMA channel's remaining count to work out how much arrived.
 * Reaching it needs no baud rate, and this model has none to offer.
 *
 * The work is deferred to a bottom half rather than done in the read path so
 * that it lands after the DMA transfer has finished. The vCPU runs in
 * parallel with the thread that got here, so a guest woken immediately could
 * read CNDTR before the transfer which emptied RDR had decremented it, and
 * conclude that one character less than really arrived had turned up. Every
 * burst would then come out one character short.
 */
static void sifli_usart_idle_bh(void *opaque)
{
    SifliUsartState *s = opaque;

    s->regs[USART_ISR] |= USART_ISR_IDLE;
    sifli_usart_update_irq(s);
}

static int sifli_usart_can_receive(void *opaque)
{
    SifliUsartState *s = opaque;

    return !(s->regs[USART_ISR] & USART_ISR_RXNE);
}

static void sifli_usart_receive(void *opaque, const uint8_t *buf, int size)
{
    SifliUsartState *s = opaque;
    uint32_t cr1 = s->regs[USART_CR1];

    /*
     * Characters that arrive before the receiver is switched on are lost,
     * as they are on hardware.
     */
    if (!(cr1 & USART_CR1_UE) || !(cr1 & USART_CR1_RE)) {
        return;
    }

    s->regs[USART_RDR] = buf[0];
    s->regs[USART_ISR] |= USART_ISR_RXNE;
    sifli_usart_update_dma_req(s);
    sifli_usart_update_irq(s);
}

static uint32_t sifli_usart_read_reg(SifliUsartState *s, unsigned idx)
{
    switch (idx) {
    case USART_ISR:
        return sifli_usart_isr(s);
    case USART_ICR:
    case USART_RQR:
        /* Write-only; the hardware reads back zero. */
        return 0;
    case USART_RDR:
    case USART_DRDR: {
        uint32_t data = s->regs[idx] & USART_DATA_MASK;

        s->regs[USART_ISR] &= ~USART_ISR_RXNE;
        s->regs[USART_RDR] = 0;
        qemu_chr_fe_accept_input(&s->chr);

        /*
         * Nothing followed this character, so the burst is over. Only of
         * interest in DMA mode: the IDLE interrupt is how the DMA receive
         * path learns a burst ended, and a receiver being polled a character
         * at a time has no use for it.
         */
        if (!(s->regs[USART_ISR] & USART_ISR_RXNE) &&
            (s->regs[USART_CR3] & USART_CR3_DMAR)) {
            qemu_bh_schedule(s->idle_bh);
        }
        sifli_usart_update_dma_req(s);
        sifli_usart_update_irq(s);
        return data;
    }
    case USART_EXR:
        /* BUSY and the ID field: nothing is in flight, no ID to report. */
        return 0;
    default:
        return s->regs[idx];
    }
}

static void sifli_usart_write_reg(SifliUsartState *s, unsigned idx,
                                  uint32_t v, uint32_t field)
{
    uint8_t ch;

    switch (idx) {
    case USART_TDR:
    case USART_DTDR:
        ch = v & 0xff;
        /*
         * XXX this blocks the whole vCPU thread. Using qemu_chr_fe_write
         * with a background handler is the usual follow-up; every other
         * in-tree USART model starts out the same way.
         */
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
        /* The byte has left; the shift register is already idle again. */
        s->regs[USART_ISR] |= USART_ISR_TC;
        sifli_usart_update_irq(s);
        return;
    case USART_ICR:
        s->regs[USART_ISR] &= ~sifli_usart_icr_to_isr(v);
        sifli_usart_update_irq(s);
        return;
    case USART_RQR:
        /* Send-break, mute and friends: self-clearing, no effect here. */
        return;
    case USART_ISR:
        /* Status bits are not writable; only ICR clears them. */
        return;
    default:
        s->regs[idx] = (s->regs[idx] & ~field) | (v & field);
        if (idx == USART_CR1) {
            sifli_usart_update_irq(s);
        } else if (idx == USART_CR3) {
            /*
             * Switching the receiver into or out of DMA mode moves the
             * request line, which can set a waiting channel going.
             */
            sifli_usart_update_dma_req(s);
        }
        return;
    }
}

/*
 * Sub-word accesses are folded onto the containing 32-bit register. The
 * HAL only ever touches these as words, but a stray byte access should
 * read something sane rather than silently return zero.
 */
static uint64_t sifli_usart_read(void *opaque, hwaddr addr, unsigned size)
{
    SifliUsartState *s = opaque;

    return extract32(sifli_usart_read_reg(s, addr >> 2), (addr & 3) * 8,
                     size * 8);
}

static void sifli_usart_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    SifliUsartState *s = opaque;
    unsigned shift = (addr & 3) * 8;
    uint32_t field, v;

    if (size >= 4) {
        field = ~0u;
    } else {
        field = ((1u << (size * 8)) - 1) << shift;
    }
    v = ((uint32_t)val << shift) & field;

    sifli_usart_write_reg(s, addr >> 2, v, field);
}

static const MemoryRegionOps sifli_usart_ops = {
    .read = sifli_usart_read,
    .write = sifli_usart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void sifli_usart_reset(DeviceState *dev)
{
    SifliUsartState *s = SIFLI_USART(dev);

    memset(s->regs, 0, sizeof(s->regs));

    /*
     * The transmitter starts idle with an empty holding register, which is
     * what TC and TXE report. Setting TC here means firmware that waits for
     * it before the first byte does not wait forever.
     */
    s->regs[USART_ISR] = USART_ISR_TC;

    qemu_bh_cancel(s->idle_bh);
    qemu_set_irq(s->dma_rx, 0);
    sifli_usart_update_irq(s);
}

static const Property sifli_usart_properties[] = {
    DEFINE_PROP_CHR("chardev", SifliUsartState, chr),
};

static void sifli_usart_init(Object *obj)
{
    SifliUsartState *s = SIFLI_USART(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_out_named(DEVICE(obj), &s->dma_rx, "dma-rx", 1);
    /*
     * Guarded, as all bottom halves in hw/ are: it is scheduled from the
     * RDR read path, which is this device's own MMIO handling.
     */
    s->idle_bh = aio_bh_new_guarded(qemu_get_aio_context(),
                                    sifli_usart_idle_bh, s,
                                    &DEVICE(obj)->mem_reentrancy_guard);

    memory_region_init_io(&s->mmio, obj, &sifli_usart_ops, s,
                          TYPE_SIFLI_USART, SIFLI_USART_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void sifli_usart_realize(DeviceState *dev, Error **errp)
{
    SifliUsartState *s = SIFLI_USART(dev);

    qemu_chr_fe_set_handlers(&s->chr, sifli_usart_can_receive,
                             sifli_usart_receive, NULL, NULL, s, NULL, true);
}

static void sifli_usart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, sifli_usart_reset);
    device_class_set_props(dc, sifli_usart_properties);
    dc->realize = sifli_usart_realize;
}

static const TypeInfo sifli_usart_info = {
    .name          = TYPE_SIFLI_USART,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SifliUsartState),
    .instance_init = sifli_usart_init,
    .class_init    = sifli_usart_class_init,
};

static void sifli_usart_register_types(void)
{
    type_register_static(&sifli_usart_info);
}

type_init(sifli_usart_register_types)
