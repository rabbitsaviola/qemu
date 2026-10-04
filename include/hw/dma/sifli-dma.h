/*
 * SiFli DMA controller
 *
 * Register map from the SiFli SDK's drivers/cmsis/sf32lb52x/dmac.h, which is
 * shared by every SF32LB5x series (unlike RCC/AON/PMU, whose layouts differ
 * per series). One model therefore serves all of them; the base address
 * differs per instance and is the machine's business.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DMA_SIFLI_DMA_H
#define HW_DMA_SIFLI_DMA_H

#include "hw/sysbus.h"
#include "qemu/bitops.h"
#include "qemu/main-loop.h"
#include "qom/object.h"

#define TYPE_SIFLI_DMA "sifli-dma"
OBJECT_DECLARE_SIMPLE_TYPE(SifliDmaState, SIFLI_DMA)

/* Eight channels per controller; the request select field is six bits wide. */
#define SIFLI_DMA_NUM_CHANNELS  8
#define SIFLI_DMA_NUM_REQUESTS  64

/*
 * Register offsets, in bytes.
 *
 * The channel block is a flat 0x14-byte run per channel, not a structure of
 * arrays, so a channel's registers are found by arithmetic rather than by
 * indexing a field name.
 */
enum {
    SIFLI_DMA_ISR = 0x00,
    SIFLI_DMA_IFCR = 0x04,
    SIFLI_DMA_CH_BASE = 0x08,
    SIFLI_DMA_CH_STRIDE = 0x14,
    SIFLI_DMA_CSELR1 = 0xa8,
    SIFLI_DMA_CSELR2 = 0xac,
    SIFLI_DMA_DBGSEL = 0xb0,
};

/* Channel register offsets, relative to the channel's base. */
enum {
    SIFLI_DMA_CCR = 0x00,
    SIFLI_DMA_CNDTR = 0x04,
    SIFLI_DMA_CPAR = 0x08,
    SIFLI_DMA_CM0AR = 0x0c,
    SIFLI_DMA_CBSR = 0x10,
};

/* The registers end at 0xB4; the window is rounded up to a power of two. */
#define SIFLI_DMA_MMIO_SIZE     0x100

/* CCR */
#define DMA_CCR_EN          BIT(0)
#define DMA_CCR_TCIE        BIT(1)
#define DMA_CCR_HTIE        BIT(2)
#define DMA_CCR_TEIE        BIT(3)
#define DMA_CCR_DIR         BIT(4)
#define DMA_CCR_CIRC        BIT(5)
#define DMA_CCR_PINC        BIT(6)
#define DMA_CCR_MINC        BIT(7)
#define DMA_CCR_PSIZE       (3u << 8)
#define DMA_CCR_MSIZE       (3u << 10)
#define DMA_CCR_PL          (3u << 12)
#define DMA_CCR_MEM2MEM     BIT(14)

/* ISR and IFCR share a layout: four bits per channel. */
#define DMA_ISR_GIF         BIT(0)
#define DMA_ISR_TCIF        BIT(1)
#define DMA_ISR_HTIF        BIT(2)
#define DMA_ISR_TEIF        BIT(3)
#define DMA_ISR_CH_MASK     0xf

typedef struct SifliDmaChannel {
    uint32_t ccr;
    /* Remaining transfers; CNDTR on hardware. */
    uint32_t cndtr;
    uint32_t cpar;
    uint32_t cm0ar;
    uint32_t cbsr;

    /*
     * What a circular transfer is reloaded with when it wraps: the count
     * written to CNDTR, and the addresses as they stood when the channel was
     * enabled. Without the address half, a burst longer than the buffer
     * walks off the end of it.
     */
    uint32_t n_reload;
    uint32_t cpar_reload;
    uint32_t cm0ar_reload;
    qemu_irq irq;
} SifliDmaChannel;

struct SifliDmaState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    SifliDmaChannel ch[SIFLI_DMA_NUM_CHANNELS];

    /* Sticky per-channel flags, with GIF as the OR of the other three. */
    uint32_t isr;
    uint32_t cselr[2];
    uint32_t dbgsel;

    /*
     * The level on each peripheral request line, indexed by request number.
     *
     * A request is a level, not a pulse: its source holds it up until the
     * transfer that consumes it has happened. That is what lets the model
     * re-check a line which went high before its channel was enabled.
     */
    uint8_t req_level[SIFLI_DMA_NUM_REQUESTS];

    /* Set while requests are being drained, to make the drain re-entrant. */
    bool servicing;

    /* Finishes a drain that ran past its per-call budget. */
    QEMUBH *drain_bh;
    unsigned drain_req;
};

#endif /* HW_DMA_SIFLI_DMA_H */
