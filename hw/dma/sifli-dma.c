/*
 * SiFli DMA controller
 *
 * Register map from the SiFli SDK's drivers/cmsis/sf32lb52x/dmac.h. See
 * include/hw/dma/sifli-dma.h.
 *
 * Thin semantics: a request moves one item, immediately. There is no bus to
 * arbitrate for, no priority to apply and no time to take, so the states the
 * HAL waits for are reached on the first look.
 *
 *   - A transfer completes inside the write that starts it, or inside the
 *     request that triggers it. Software never observes a channel with EN set
 *     and work outstanding.
 *   - CNDTR counts down by one per item and is reloaded from what was written
 *     to it when a circular channel wraps, so software reading it to find how
 *     far a transfer has got sees a sane number.
 *   - PSIZE/MSIZE are used only to advance the addresses; the item always
 *     moves as one access of that width. PL, CBSR and DBGSEL are stored and
 *     read back but have no effect -- there is no competing traffic for
 *     priority to arbitrate, and no bus error to report.
 *   - TEIF is never raised: nothing this model can be asked to move fails.
 *   - The transfer complete and half transfer flags are raised, but a channel
 *     only interrupts for them if the matching enable is set.
 *
 * Requests arrive on one input line per request number rather than one per
 * channel, mirroring the CSELRn mux on the real part: the controller only
 * knows which channel serves a request by looking the number up in CSELRn,
 * and re-reads it every time, because the HAL is free to move a request to
 * another channel and often does.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/dma/sifli-dma.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/address-spaces.h"
#include "system/memory.h"

/*
 * How many items sifli_dma_service() moves before handing the rest to a
 * bottom half. A console paste can be thousands of characters, and there is
 * no reason to hold the thread for all of them at once; see
 * sifli_dma_drain_bh().
 */
#define SIFLI_DMA_MAX_DRAIN     256

/* The request number a channel is currently wired to, from CSELRn. */
static unsigned sifli_dma_req_of_channel(SifliDmaState *s, unsigned i)
{
    uint32_t sel = s->cselr[i / 4];

    return (sel >> ((i % 4) * 8)) & 0x3f;
}

static unsigned sifli_dma_flags_of(SifliDmaState *s, unsigned i)
{
    return (s->isr >> (i * 4)) & DMA_ISR_CH_MASK;
}

static void sifli_dma_update_irq(SifliDmaState *s, unsigned i)
{
    unsigned flags = sifli_dma_flags_of(s, i);
    uint32_t ccr = s->ch[i].ccr;
    int level = 0;

    if ((flags & DMA_ISR_TCIF) && (ccr & DMA_CCR_TCIE)) {
        level = 1;
    }
    if ((flags & DMA_ISR_HTIF) && (ccr & DMA_CCR_HTIE)) {
        level = 1;
    }
    if ((flags & DMA_ISR_TEIF) && (ccr & DMA_CCR_TEIE)) {
        level = 1;
    }

    qemu_set_irq(s->ch[i].irq, level);
}

static void sifli_dma_set_flag(SifliDmaState *s, unsigned i, uint32_t flag)
{
    s->isr |= (flag | DMA_ISR_GIF) << (i * 4);
    sifli_dma_update_irq(s, i);
}

/*
 * The width of one item. Every configuration the HAL builds for this board
 * pairs matching peripheral and memory sizes, and CNDTR counts items, so
 * either field gives the same answer; MSIZE is the one read here. The fourth
 * encoding is reserved and is treated as a byte.
 */
static unsigned sifli_dma_item_size(uint32_t ccr)
{
    static const unsigned size[4] = { 1, 2, 4, 1 };

    return size[(ccr >> 10) & 3];
}

static void sifli_dma_transfer(SifliDmaState *s, unsigned i)
{
    SifliDmaChannel *ch = &s->ch[i];
    unsigned size = sifli_dma_item_size(ch->ccr);
    bool to_mem = !(ch->ccr & DMA_CCR_DIR) || (ch->ccr & DMA_CCR_MEM2MEM);
    uint64_t src = to_mem ? ch->cpar : ch->cm0ar;
    uint64_t dst = to_mem ? ch->cm0ar : ch->cpar;
    uint8_t buf[4];

    /*
     * Going through the address space rather than straight to a memory
     * region is what makes a peripheral address work: the read lands in the
     * peripheral's own MMIO handler, so a USART hands over its RDR and drops
     * the request line that led here, all before this function returns.
     */
    address_space_read(&address_space_memory, src, MEMTXATTRS_UNSPECIFIED,
                       buf, size);
    address_space_write(&address_space_memory, dst, MEMTXATTRS_UNSPECIFIED,
                        buf, size);

    if (ch->ccr & DMA_CCR_PINC) {
        ch->cpar += size;
    }
    if (ch->ccr & DMA_CCR_MINC) {
        ch->cm0ar += size;
    }
    if (ch->cndtr) {
        ch->cndtr--;
    }

    if (!ch->cndtr) {
        sifli_dma_set_flag(s, i, DMA_ISR_TCIF);
        if (ch->ccr & DMA_CCR_CIRC) {
            /*
             * A wrap starts the buffer again, so the addresses go back to
             * where they were when the channel was enabled alongside the
             * count. A peripheral that does not advance has nothing to
             * restore and its reload value is the same as its current one.
             */
            ch->cndtr = ch->n_reload;
            ch->cpar = ch->cpar_reload;
            ch->cm0ar = ch->cm0ar_reload;
        } else {
            ch->ccr &= ~DMA_CCR_EN;
        }
    } else if (ch->cndtr * 2 == ch->n_reload) {
        sifli_dma_set_flag(s, i, DMA_ISR_HTIF);
    }
}

/*
 * Move everything the given request currently has waiting.
 *
 * The loop is load-bearing. Finishing a transfer can raise the same request
 * again from inside the transfer: reading a USART's RDR hands the chardev the
 * character behind it, and the chardev calls back into the USART, which sets
 * RXNE and drives the line high before this frame is done. That nested call
 * is turned away by the guard below and leaves only the level recorded, so
 * the byte it brought is picked up here -- after CM0AR has been advanced past
 * the first one. Servicing it from the nested call instead would write both
 * bytes to the same address.
 */
static void sifli_dma_service(SifliDmaState *s, unsigned req)
{
    bool more = true;
    unsigned pass;

    if (s->servicing) {
        return;
    }
    s->servicing = true;

    for (pass = 0; pass < SIFLI_DMA_MAX_DRAIN; pass++) {
        bool moved = false;
        unsigned i;

        /* The source has let go; the burst is over. */
        if (!s->req_level[req]) {
            more = false;
            break;
        }

        for (i = 0; i < SIFLI_DMA_NUM_CHANNELS; i++) {
            SifliDmaChannel *ch = &s->ch[i];

            if (!(ch->ccr & DMA_CCR_EN)) {
                continue;
            }
            if (sifli_dma_req_of_channel(s, i) != req) {
                continue;
            }
            sifli_dma_transfer(s, i);
            moved = true;
        }

        /*
         * Nobody is listening to this request. The level is left up so that
         * a channel pointed at it later can still find the work, and nothing
         * more is done here -- re-running would just find it again.
         */
        if (!moved) {
            more = false;
            break;
        }
    }

    s->servicing = false;

    if (more) {
        /*
         * The budget ran out with the source still holding its request up,
         * which is what a paste into the console looks like. Finish the job
         * once this device callback has unwound.
         */
        s->drain_req = req;
        qemu_bh_schedule(s->drain_bh);
    }
}

static void sifli_dma_drain_bh(void *opaque)
{
    SifliDmaState *s = opaque;

    sifli_dma_service(s, s->drain_req);
}

/*
 * A memory-to-memory channel has no peripheral to raise a request, so it
 * starts the moment it is enabled and runs to completion. The budget stops a
 * circular one -- which on hardware would rewrite the same buffer forever --
 * from holding the thread here.
 */
static void sifli_dma_run_mem2mem(SifliDmaState *s, unsigned i)
{
    unsigned pass;

    for (pass = 0; pass < SIFLI_DMA_MAX_DRAIN; pass++) {
        if (!(s->ch[i].ccr & DMA_CCR_EN)) {
            break;
        }
        sifli_dma_transfer(s, i);
    }
}

/*
 * Re-check every request line.
 *
 * A source can raise its line while no channel is watching it -- the console
 * reopens, for instance, and a character lands in RDR before the HAL has
 * pointed a channel at it and set EN. The edge is gone by then, so enabling a
 * channel and rewriting CSELRn both have to go looking for stranded work.
 */
static void sifli_dma_rescan(SifliDmaState *s)
{
    unsigned req;

    for (req = 0; req < SIFLI_DMA_NUM_REQUESTS; req++) {
        if (s->req_level[req]) {
            sifli_dma_service(s, req);
        }
    }
}

static void sifli_dma_set_req(void *opaque, int n, int level)
{
    SifliDmaState *s = opaque;

    /*
     * The level is recorded before the guard in sifli_dma_service() can turn
     * the call away: a transfer that raises its own source's request again
     * would otherwise lose that edge, and with it the character behind it.
     */
    s->req_level[n] = level;

    if (level) {
        sifli_dma_service(s, n);
    }
}

static void sifli_dma_clear_flags(SifliDmaState *s, uint32_t v)
{
    unsigned i;

    for (i = 0; i < SIFLI_DMA_NUM_CHANNELS; i++) {
        unsigned bits = (v >> (i * 4)) & DMA_ISR_CH_MASK;

        /*
         * CGIF is a global clear for its channel: the SVD documents each of
         * the other flags as cleared by a write of 1 to itself *or* to CGIF.
         */
        if (bits & DMA_ISR_GIF) {
            bits = DMA_ISR_CH_MASK;
        }
        s->isr &= ~(bits << (i * 4));
        sifli_dma_update_irq(s, i);
    }
}

/* Does this offset fall in one of the per-channel register blocks? */
static bool sifli_dma_is_channel_reg(hwaddr off)
{
    hwaddr end = SIFLI_DMA_CH_BASE +
                 SIFLI_DMA_NUM_CHANNELS * SIFLI_DMA_CH_STRIDE;

    return off >= SIFLI_DMA_CH_BASE && off < end;
}

static uint32_t sifli_dma_read_reg(SifliDmaState *s, hwaddr off)
{
    if (off == SIFLI_DMA_ISR) {
        return s->isr;
    }
    if (off == SIFLI_DMA_IFCR) {
        /* Write-only; the hardware reads back zero. */
        return 0;
    }
    if (sifli_dma_is_channel_reg(off)) {
        SifliDmaChannel *ch = &s->ch[(off - SIFLI_DMA_CH_BASE) /
                                     SIFLI_DMA_CH_STRIDE];

        switch ((off - SIFLI_DMA_CH_BASE) % SIFLI_DMA_CH_STRIDE) {
        case SIFLI_DMA_CCR:
            return ch->ccr;
        case SIFLI_DMA_CNDTR:
            return ch->cndtr & 0xffff;
        case SIFLI_DMA_CPAR:
            return ch->cpar;
        case SIFLI_DMA_CM0AR:
            return ch->cm0ar;
        case SIFLI_DMA_CBSR:
            return ch->cbsr;
        }
    }
    if (off == SIFLI_DMA_CSELR1) {
        return s->cselr[0];
    }
    if (off == SIFLI_DMA_CSELR2) {
        return s->cselr[1];
    }
    if (off == SIFLI_DMA_DBGSEL) {
        return s->dbgsel;
    }

    qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                  __func__, off);
    return 0;
}

static void sifli_dma_write_reg(SifliDmaState *s, hwaddr off, uint32_t v,
                                uint32_t field)
{
    if (off == SIFLI_DMA_ISR) {
        /* Status bits are not writable; only IFCR clears them. */
        return;
    }
    if (off == SIFLI_DMA_IFCR) {
        sifli_dma_clear_flags(s, v);
        return;
    }
    if (sifli_dma_is_channel_reg(off)) {
        unsigned i = (off - SIFLI_DMA_CH_BASE) / SIFLI_DMA_CH_STRIDE;
        SifliDmaChannel *ch = &s->ch[i];

        switch ((off - SIFLI_DMA_CH_BASE) % SIFLI_DMA_CH_STRIDE) {
        case SIFLI_DMA_CCR: {
            bool was_enabled = ch->ccr & DMA_CCR_EN;

            ch->ccr = (ch->ccr & ~field) | (v & field);
            if (!was_enabled && (ch->ccr & DMA_CCR_EN)) {
                /*
                 * Enabling latches the addresses a circular transfer will be
                 * reloaded with, and can expose a request that arrived while
                 * no channel was listening for it.
                 */
                ch->cpar_reload = ch->cpar;
                ch->cm0ar_reload = ch->cm0ar;
                if (ch->ccr & DMA_CCR_MEM2MEM) {
                    sifli_dma_run_mem2mem(s, i);
                } else {
                    sifli_dma_rescan(s);
                }
            }
            return;
        }
        case SIFLI_DMA_CNDTR:
            ch->cndtr = v & 0xffff;
            /* The count to reload with is latched here, as on hardware. */
            ch->n_reload = ch->cndtr;
            return;
        case SIFLI_DMA_CPAR:
            ch->cpar = v;
            return;
        case SIFLI_DMA_CM0AR:
            ch->cm0ar = v;
            return;
        case SIFLI_DMA_CBSR:
            ch->cbsr = v & 0xff;
            return;
        }
    }
    if (off == SIFLI_DMA_CSELR1 || off == SIFLI_DMA_CSELR2) {
        uint32_t *sel = &s->cselr[off == SIFLI_DMA_CSELR1 ? 0 : 1];

        *sel = (*sel & ~field) | (v & field);
        sifli_dma_rescan(s);
        return;
    }
    if (off == SIFLI_DMA_DBGSEL) {
        s->dbgsel = v;
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                  __func__, off);
}

/*
 * Sub-word accesses are folded onto the containing 32-bit register, as in the
 * USART model: the HAL only touches these as words, but a stray byte access
 * should read something sane rather than silently return zero.
 */
static uint64_t sifli_dma_read(void *opaque, hwaddr addr, unsigned size)
{
    SifliDmaState *s = opaque;

    if (size > 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad access size %u\n",
                      __func__, size);
        return 0;
    }

    return extract32(sifli_dma_read_reg(s, addr & ~3), (addr & 3) * 8,
                     size * 8);
}

static void sifli_dma_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    SifliDmaState *s = opaque;
    unsigned shift = (addr & 3) * 8;
    uint32_t field, v;

    if (size > 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad access size %u\n",
                      __func__, size);
        return;
    }

    if (size == 4) {
        field = ~0u;
    } else {
        field = ((1u << (size * 8)) - 1) << shift;
    }
    v = ((uint32_t)val << shift) & field;

    sifli_dma_write_reg(s, addr & ~3, v, field);
}

static const MemoryRegionOps sifli_dma_ops = {
    .read = sifli_dma_read,
    .write = sifli_dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void sifli_dma_reset(DeviceState *dev)
{
    SifliDmaState *s = SIFLI_DMA(dev);
    unsigned i;

    s->isr = 0;
    s->cselr[0] = 0;
    s->cselr[1] = 0;
    s->dbgsel = 0;
    s->servicing = false;
    qemu_bh_cancel(s->drain_bh);
    memset(s->req_level, 0, sizeof(s->req_level));

    for (i = 0; i < SIFLI_DMA_NUM_CHANNELS; i++) {
        qemu_irq irq = s->ch[i].irq;

        memset(&s->ch[i], 0, sizeof(s->ch[i]));
        s->ch[i].irq = irq;
    }
}

static void sifli_dma_init(Object *obj)
{
    SifliDmaState *s = SIFLI_DMA(obj);
    unsigned i;

    for (i = 0; i < SIFLI_DMA_NUM_CHANNELS; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->ch[i].irq);
    }

    /*
     * One input line per request number, not per channel: see the request mux
     * note at the top of this file. Naming the array lets the machine, and
     * "info qtree", say which request a source is wired to.
     */
    qdev_init_gpio_in_named(DEVICE(obj), sifli_dma_set_req, "request",
                            SIFLI_DMA_NUM_REQUESTS);

    /*
     * Guarded, as all bottom halves in hw/ are: it is scheduled from inside
     * this device's own MMIO handling (and from a USART's, through the
     * request line), so it must not be allowed to run while one of those is
     * still on the stack.
     */
    s->drain_bh = aio_bh_new_guarded(qemu_get_aio_context(),
                                     sifli_dma_drain_bh, s,
                                     &DEVICE(obj)->mem_reentrancy_guard);

    memory_region_init_io(&s->mmio, obj, &sifli_dma_ops, s,
                          TYPE_SIFLI_DMA, SIFLI_DMA_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void sifli_dma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, sifli_dma_reset);
}

static const TypeInfo sifli_dma_info = {
    .name          = TYPE_SIFLI_DMA,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SifliDmaState),
    .instance_init = sifli_dma_init,
    .class_init    = sifli_dma_class_init,
};

static void sifli_dma_register_types(void)
{
    type_register_static(&sifli_dma_info);
}

type_init(sifli_dma_register_types)
