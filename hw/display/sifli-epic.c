/*
 * SiFli EPIC 2D graphics engine
 *
 * Register map from the SiFli SDK's drivers/cmsis/sf32lb52x/epic.h.
 *
 * EPIC has no operation code. EPIC_RUN() in the HAL is a single
 * "COMMAND |= START" (bf0_hal_epic.c:522), and COMMAND only defines START
 * and RESET, so what a job does is implied entirely by which layer registers
 * are live and what the output registers say. This model reads the job the
 * same way the hardware would: fill the canvas rectangle if it is asked to,
 * then composite each active layer onto it.
 *
 * That collapses the HAL's fill, copy and blend entry points into one path,
 * because on hardware they are one path -- HAL_EPIC_Copy_IT and
 * HAL_EPIC_BlendStart* differ only in the registers they leave behind.
 *
 * Thin semantics again:
 *
 *   - The job finishes inside the write that sets START, so STATUS reads
 *     back zero. This is not an optimisation: EPIC_WaitDone() on 52x is a
 *     bare "while (STATUS != 0)", and EPIC_WaitValidInstance() redirects
 *     writes to a RAM shadow instance and then calls it, so a model that
 *     reported a busy engine would spin forever rather than finish.
 *   - EOF_IRQ is sticky and the interrupt is a level, gated by
 *     SETTING.EOF_IRQ_MASK. The HAL clears it by writing the status back.
 *
 * What is not here, and is logged rather than silently mis-drawn: rotation
 * (VL_ROT, VL_ROT_M_*), scaling (SCALE_RATIO_*, SCALE_INIT_*), YUV input,
 * dithering, masking (MASK_*) and output straight to the LCD
 * (AHB_CTRL.DESTINATION). A job that uses one of those still completes, so
 * that firmware waiting on it is not left hanging, but the pixels it
 * produced are not the ones hardware would have produced.
 *
 * L1 and L2 are absent on this part: the HAL defines EPIC_L2_L1_INVALID for
 * SF32LB52X. Their registers exist and are stored, but no job can reach
 * them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/display/sifli-epic.h"
#include "hw/irq.h"
#include "hw/misc/sifli-sbus.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/address-spaces.h"
#include "system/memory.h"

/*
 * The most pixels one job may touch. A 10-bit coordinate space is 1024x1024;
 * the cap only bites on a register set that could not have come from the
 * HAL, and keeps a wild rectangle from being turned into a wild allocation.
 */
#define EPIC_MAX_PIXELS         (1024 * 1024)

/* What a layer's registers say, pulled out of the flat register file. */
typedef struct EpicLayer {
    const char *name;       /* for the log messages */
    unsigned cfg_off;       /* SIFLI_EPIC_<name>_CFG */
    unsigned tl_off;
    unsigned br_off;
    unsigned src_off;
    unsigned fill_off;
} EpicLayer;

/*
 * The live layers, listed in the order the hardware composites them:
 * furthest from the viewer first. L0 is the graphics layer the HAL paints
 * backgrounds and copies into; VL is the video layer that goes on top of it,
 * which is also the one HAL_EPIC_ConfigFilling sets up when it fills with an
 * alpha rather than an opaque colour.
 */
static const EpicLayer epic_layers[] = {
    { "L0", SIFLI_EPIC_L0_CFG, SIFLI_EPIC_L0_TL_POS, SIFLI_EPIC_L0_BR_POS,
      SIFLI_EPIC_L0_SRC, SIFLI_EPIC_L0_FILL },
    { "VL", SIFLI_EPIC_VL_CFG, SIFLI_EPIC_VL_TL_POS, SIFLI_EPIC_VL_BR_POS,
      SIFLI_EPIC_VL_SRC, SIFLI_EPIC_VL_FILL },
};

typedef struct EpicPixel {
    uint8_t r, g, b, a;
} EpicPixel;

/* ------------------------------------------------------------------ */
/* Pixel formats.                                                      */
/* ------------------------------------------------------------------ */

/*
 * Bytes per pixel of a source layer, by CFG.FORMAT. A4 and A2 are packed,
 * so they have no whole number of bytes per pixel; they are reported as one
 * and refused by epic_layer_pixel(), which is honest about not supporting
 * them rather than indexing off the end of a row.
 */
static unsigned epic_src_bpp(unsigned format)
{
    switch (format) {
    case EPIC_FMT_RGB565:
    case EPIC_FMT_ARGB8565:
        return 2;
    case EPIC_FMT_RGB888:
        return 3;
    case EPIC_FMT_ARGB8888:
        return 4;
    case EPIC_FMT_A8:
    case EPIC_FMT_L8:
    case EPIC_FMT_A4:
    case EPIC_FMT_A2:
    default:
        return 1;
    }
}

/* Bytes per pixel of the destination, by AHB_CTRL.O_FORMAT. */
static unsigned epic_dst_bpp(unsigned o_format)
{
    switch (o_format) {
    case EPIC_OUT_RGB565:
        return 2;
    case EPIC_OUT_RGB888:
    case EPIC_OUT_ARGB8565:
        return 3;
    case EPIC_OUT_ARGB8888:
    default:
        return 4;
    }
}

/* --- the 565 family, which both the source and the destination speak --- */

static void epic_unpack565(uint16_t v, EpicPixel *px)
{
    unsigned r = (v >> 11) & 0x1f;
    unsigned g = (v >> 5) & 0x3f;
    unsigned b = v & 0x1f;

    px->r = (r << 3) | (r >> 2);
    px->g = (g << 2) | (g >> 4);
    px->b = (b << 3) | (b >> 2);
}

static uint16_t epic_pack565(EpicPixel px)
{
    return ((px.r >> 3) << 11) | ((px.g >> 2) << 5) | (px.b >> 3);
}

static EpicPixel epic_decode_pixel(const uint8_t *p, unsigned format)
{
    EpicPixel px = { 0, 0, 0, 0xff };

    switch (format) {
    case EPIC_FMT_RGB565:
        epic_unpack565(lduw_le_p(p), &px);
        break;
    case EPIC_FMT_RGB888:
        px.b = p[0];
        px.g = p[1];
        px.r = p[2];
        break;
    case EPIC_FMT_ARGB8888:
        px.b = p[0];
        px.g = p[1];
        px.r = p[2];
        px.a = p[3];
        break;
    case EPIC_FMT_ARGB8565:
        epic_unpack565(lduw_le_p(p), &px);
        px.a = p[2];
        break;
    case EPIC_FMT_A8:
        /* Alpha only: the colour comes from the layer's FILL register. */
        px.a = p[0];
        px.r = px.g = px.b = 0;
        break;
    case EPIC_FMT_L8:
    default:
        /* Luminance, and the palette formats: not decoded. */
        px.a = 0;
        break;
    }

    return px;
}

static void epic_encode_pixel(uint8_t *p, unsigned o_format, EpicPixel px)
{
    switch (o_format) {
    case EPIC_OUT_RGB565:
        stw_le_p(p, epic_pack565(px));
        break;
    case EPIC_OUT_RGB888:
        p[0] = px.b;
        p[1] = px.g;
        p[2] = px.r;
        break;
    case EPIC_OUT_ARGB8565:
        stw_le_p(p, epic_pack565(px));
        p[2] = px.a;
        break;
    case EPIC_OUT_ARGB8888:
    default:
        p[0] = px.b;
        p[1] = px.g;
        p[2] = px.r;
        p[3] = px.a;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Guest memory helpers.                                               */
/* ------------------------------------------------------------------ */

static bool epic_guest_read(uint32_t addr, void *buf, size_t len)
{
    return address_space_read(&address_space_memory, addr,
                              MEMTXATTRS_UNSPECIFIED, buf, len) == MEMTX_OK;
}

static bool epic_guest_write(uint32_t addr, const void *buf, size_t len)
{
    return address_space_write(&address_space_memory, addr,
                               MEMTXATTRS_UNSPECIFIED, buf, len) == MEMTX_OK;
}

/* ------------------------------------------------------------------ */
/* The job.                                                            */
/* ------------------------------------------------------------------ */

/*
 * The interrupt is the level of "end of frame latched" masked by "firmware
 * wants to hear about it", so both registers change what the line does and
 * both go through here.
 */
static void epic_update_irq(SifliEpicState *s)
{
    bool armed = (s->reg[SIFLI_EPIC_SETTING / 4] & EPIC_SETTING_EOF_IRQ_MASK) &&
                 (s->reg[SIFLI_EPIC_EOF_IRQ / 4] & EPIC_EOF_IRQ_STATUS);

    qemu_set_irq(s->irq, armed);
}

static void epic_finish(SifliEpicState *s)
{
    s->reg[SIFLI_EPIC_STATUS / 4] = 0;
    s->reg[SIFLI_EPIC_EOF_IRQ / 4] |= EPIC_EOF_IRQ_STATUS;
    epic_update_irq(s);
}

/*
 * Composite one layer onto the canvas.
 *
 * The layer's own rectangle is in canvas coordinates; the canvas rectangle is
 * the clip. The source buffer is addressed through the layer's CFG.WIDTH,
 * which is a byte stride, so a source row may be wider than the rectangle
 * drawn from it.
 */
static void epic_draw_layer(SifliEpicState *s, const EpicLayer *l,
                            uint8_t *canvas, unsigned cx0, unsigned cy0,
                            unsigned cw, unsigned ch, unsigned dst_bpp,
                            unsigned o_format)
{
    uint32_t cfg = s->reg[l->cfg_off / 4];
    uint32_t tl = s->reg[l->tl_off / 4];
    uint32_t br = s->reg[l->br_off / 4];
    unsigned format = cfg & EPIC_L_CFG_FORMAT_Msk;
    unsigned src_bpp = epic_src_bpp(format);
    unsigned src_stride = (cfg & EPIC_L_CFG_WIDTH_Msk) >> EPIC_L_CFG_WIDTH_Pos;
    unsigned alpha = (cfg & EPIC_L_CFG_ALPHA_Msk) >> EPIC_L_CFG_ALPHA_Pos;
    bool alpha_sel = (cfg & EPIC_L_CFG_ALPHA_SEL) != 0;
    bool blend = (cfg & EPIC_L_CFG_ALPHA_BLEND) != 0;
    uint32_t src = sifli_sbus_to_cpu_addr(s->reg[l->src_off / 4]);
    unsigned lx0 = tl & EPIC_CANVAS_X_Msk;
    unsigned ly0 = (tl & EPIC_CANVAS_Y_Msk) >> EPIC_CANVAS_Y_Pos;
    unsigned lx1 = br & EPIC_CANVAS_X_Msk;
    unsigned ly1 = (br & EPIC_CANVAS_Y_Msk) >> EPIC_CANVAS_Y_Pos;
    unsigned x, y;

    if (!(cfg & EPIC_L_CFG_ACTIVE)) {
        return;
    }
    if (lx1 < lx0 || ly1 < ly0) {
        return;
    }
    if (src_stride == 0) {
        /*
         * A stride of zero is not a stride the hardware can walk, and the
         * HAL never leaves one behind. Without it every row would land on
         * the first, so there is nothing useful to draw.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sifli-epic: %s is active with a zero row stride\n",
                      l->name);
        return;
    }
    if (format != EPIC_FMT_RGB565 && format != EPIC_FMT_RGB888 &&
        format != EPIC_FMT_ARGB8888 && format != EPIC_FMT_ARGB8565) {
        /*
         * A8/A4/A2/L8 want the colour coordinate engine, the palette or the
         * mask registers, none of which are modelled.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sifli-epic: %s uses source format %u, which is not "
                      "implemented; the layer is skipped\n", l->name, format);
        return;
    }

    for (y = MAX(ly0, cy0); y <= MIN(ly1, cy0 + ch - 1); y++) {
        for (x = MAX(lx0, cx0); x <= MIN(lx1, cx0 + cw - 1); x++) {
            uint32_t src_addr = src + (y - ly0) * src_stride +
                                (x - lx0) * src_bpp;
            uint8_t raw[4];
            EpicPixel px;
            uint8_t *dst = canvas + ((y - cy0) * cw + (x - cx0)) * dst_bpp;
            unsigned a;

            if (!epic_guest_read(src_addr, raw, src_bpp)) {
                continue;
            }
            px = epic_decode_pixel(raw, format);

            /*
             * Which of the two alpha bits is set is decided by the source
             * format, not by the firmware's intent: a source that carries an
             * alpha channel blends per pixel (ALPHA_BLEND), one that does not
             * blends by the constant in the register (ALPHA_SEL), and the
             * HAL sets neither when it means the layer to be opaque. The
             * register's ALPHA field is written in both cases, so reading it
             * when ALPHA_BLEND is what was meant would ignore the pixel.
             */
            if (alpha_sel) {
                a = alpha;
            } else if (blend) {
                a = px.a;
            } else {
                a = 255;
            }

            if (a != 255) {
                EpicPixel under = epic_decode_pixel(dst, o_format);

                px.r = (px.r * a + under.r * (255 - a) + 127) / 255;
                px.g = (px.g * a + under.g * (255 - a) + 127) / 255;
                px.b = (px.b * a + under.b * (255 - a) + 127) / 255;
                px.a = a + (under.a * (255 - a) + 127) / 255;
            } else if (format == EPIC_FMT_RGB565 || format == EPIC_FMT_RGB888) {
                /* No alpha channel in the source; the output has one. */
                px.a = 255;
            }

            epic_encode_pixel(dst, o_format, px);
        }
    }
}

static void epic_run(SifliEpicState *s)
{
    uint32_t tl = s->reg[SIFLI_EPIC_CANVAS_TL_POS / 4];
    uint32_t br = s->reg[SIFLI_EPIC_CANVAS_BR_POS / 4];
    uint32_t bg = s->reg[SIFLI_EPIC_CANVAS_BG / 4];
    uint32_t ahb_ctrl = s->reg[SIFLI_EPIC_AHB_CTRL / 4];
    uint32_t mem = s->reg[SIFLI_EPIC_AHB_MEM / 4];
    uint32_t gap = s->reg[SIFLI_EPIC_AHB_STRIDE / 4];
    unsigned o_format = (ahb_ctrl & EPIC_AHB_CTRL_O_FORMAT_Msk) >>
                        EPIC_AHB_CTRL_O_FORMAT_Pos;
    unsigned dst_bpp = epic_dst_bpp(o_format);
    unsigned x0 = tl & EPIC_CANVAS_X_Msk;
    unsigned y0 = (tl & EPIC_CANVAS_Y_Msk) >> EPIC_CANVAS_Y_Pos;
    unsigned x1 = br & EPIC_CANVAS_X_Msk;
    unsigned y1 = (br & EPIC_CANVAS_Y_Msk) >> EPIC_CANVAS_Y_Pos;
    unsigned w, h, row_bytes, stride, i;
    g_autofree uint8_t *canvas = NULL;
    /*
     * The colour the canvas is cleared to, which is also the fill colour: an
     * opaque fill programs no layer at all, just CANVAS_BG and this
     * rectangle. Either bypass bit says the canvas is meant to be read, not
     * painted -- the copy path sets ALL_BLENDING_BYPASS and leaves a zero
     * CANVAS_BG behind, so clearing would erase the destination it is about
     * to read.
     */
    bool clear_canvas = !(bg & (EPIC_CANVAS_BG_BG_BLENDING_BYPASS |
                                EPIC_CANVAS_BG_ALL_BLENDING_BYPASS));
    EpicPixel fill = {
        .r = (bg & EPIC_CANVAS_BG_RED_Msk) >> EPIC_CANVAS_BG_RED_Pos,
        .g = (bg & EPIC_CANVAS_BG_GREEN_Msk) >> EPIC_CANVAS_BG_GREEN_Pos,
        .b = (bg & EPIC_CANVAS_BG_BLUE_Msk) >> EPIC_CANVAS_BG_BLUE_Pos,
        .a = 255,
    };

    if (ahb_ctrl & EPIC_AHB_CTRL_DESTINATION) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sifli-epic: output to the LCD is not implemented; the "
                      "job completes without drawing\n");
        epic_finish(s);
        return;
    }

    if (x1 < x0 || y1 < y0) {
        /* Nothing to draw, which is what a zero-width fill looks like. */
        epic_finish(s);
        return;
    }
    w = x1 - x0 + 1;
    h = y1 - y0 + 1;

    if ((uint64_t)w * h > EPIC_MAX_PIXELS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sifli-epic: refusing a %ux%u job\n", w, h);
        epic_finish(s);
        return;
    }

    /*
     * The destination is walked as the rectangle's own rows, with AHB_STRIDE
     * bytes skipped at the end of each. That is what the HAL computes the
     * register from: it stores (total_width - rect_width) * depth there, so
     * the row advance comes back out as the full canvas width.
     */
    row_bytes = w * dst_bpp;
    stride = row_bytes + gap;

    canvas = g_malloc((size_t)row_bytes * h);

    for (i = 0; i < h; i++) {
        uint32_t row = mem + i * stride;
        uint8_t *p = canvas + (size_t)i * row_bytes;

        if (!epic_guest_read(row, p, row_bytes)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sifli-epic: cannot read the canvas row at "
                          "0x%08x\n", row);
            epic_finish(s);
            return;
        }

        if (clear_canvas) {
            unsigned x;

            for (x = 0; x < w; x++) {
                epic_encode_pixel(p + (size_t)x * dst_bpp, o_format, fill);
            }
        }
    }

    for (i = 0; i < ARRAY_SIZE(epic_layers); i++) {
        epic_draw_layer(s, &epic_layers[i], canvas, x0, y0, w, h, dst_bpp,
                        o_format);
    }

    for (i = 0; i < h; i++) {
        uint32_t row = mem + i * stride;
        const uint8_t *p = canvas + (size_t)i * row_bytes;

        if (!epic_guest_write(row, p, row_bytes)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sifli-epic: cannot write the canvas row at "
                          "0x%08x\n", row);
            break;
        }
    }

    epic_finish(s);
}

/* ------------------------------------------------------------------ */
/* MMIO.                                                               */
/* ------------------------------------------------------------------ */

static uint64_t epic_read(void *opaque, hwaddr addr, unsigned size)
{
    SifliEpicState *s = opaque;
    unsigned index = addr / 4;
    unsigned shift = (addr & 3) * 8;
    uint32_t mask;

    if (size > 4 || addr + size > SIFLI_EPIC_MMIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad access size %u at 0x%"
                      HWADDR_PRIx "\n", __func__, size, addr);
        return 0;
    }
    if (index >= SIFLI_EPIC_NUM_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: offset 0x%" HWADDR_PRIx " is not a register\n",
                      __func__, addr);
        return 0;
    }

    mask = (size == 4) ? ~0u : ((1u << (size * 8)) - 1);
    return (s->reg[index] >> shift) & mask;
}

static void epic_write(void *opaque, hwaddr addr, uint64_t value,
                       unsigned size)
{
    SifliEpicState *s = opaque;
    unsigned index = addr / 4;
    unsigned shift = (addr & 3) * 8;
    uint32_t field;
    uint32_t v;

    if (size > 4 || addr + size > SIFLI_EPIC_MMIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad access size %u at 0x%"
                      HWADDR_PRIx "\n", __func__, size, addr);
        return;
    }
    if (index >= SIFLI_EPIC_NUM_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: offset 0x%" HWADDR_PRIx " is not a register\n",
                      __func__, addr);
        return;
    }

    field = (size == 4) ? ~0u : (((1u << (size * 8)) - 1) << shift);
    v = ((uint32_t)value << shift) & field;

    switch (index * 4) {
    case SIFLI_EPIC_COMMAND:
        /*
         * RESET is accepted and does nothing beyond the clearing the write
         * itself implies: nothing here is mid-flight to cancel.
         */
        if (v & EPIC_COMMAND_START) {
            epic_run(s);
        }
        return;

    case SIFLI_EPIC_EOF_IRQ:
        /*
         * Every bit the HAL writes here is write-one-to-clear: it
         * acknowledges by writing back the status it read. The cause bits
         * share the register and clear the same way.
         */
        s->reg[index] &= ~v;
        epic_update_irq(s);
        return;

    case SIFLI_EPIC_SETTING:
        s->reg[index] = (s->reg[index] & ~field) | v;
        epic_update_irq(s);
        return;

    default:
        s->reg[index] = (s->reg[index] & ~field) | v;
        return;
    }
}

static const MemoryRegionOps sifli_epic_ops = {
    .read = epic_read,
    .write = epic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void sifli_epic_reset(DeviceState *dev)
{
    SifliEpicState *s = SIFLI_EPIC(dev);

    memset(s->reg, 0, sizeof(s->reg));
    qemu_set_irq(s->irq, 0);
}

static void sifli_epic_init(Object *obj)
{
    SifliEpicState *s = SIFLI_EPIC(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);

    memory_region_init_io(&s->mmio, obj, &sifli_epic_ops, s,
                          TYPE_SIFLI_EPIC, SIFLI_EPIC_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void sifli_epic_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, sifli_epic_reset);
}

static const TypeInfo sifli_epic_info = {
    .name          = TYPE_SIFLI_EPIC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SifliEpicState),
    .instance_init = sifli_epic_init,
    .class_init    = sifli_epic_class_init,
};

static void sifli_epic_register_types(void)
{
    type_register_static(&sifli_epic_info);
}

type_init(sifli_epic_register_types)
