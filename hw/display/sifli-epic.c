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
 *   - The VL layer's rotation and scaling are done, as an inverse map from
 *     output pixels back to source ones; see the comment on
 *     epic_draw_layer_transformed(). A layer with no transform still takes
 *     the rectangle path, byte for byte.
 *   - The EZIP co-engine's decoded frame goes through that same map when the
 *     layer is scaled. The HAL allows scaling a co-engine layer and only
 *     refuses rotating one (bf0_hal_epic.c:6196-6204), so the two cases are
 *     scaled and not-scaled; a co-engine layer with an angle set is not
 *     something the HAL can produce and is drawn untransformed with a log.
 *
 * What is still not here, and is logged rather than silently mis-drawn:
 * mirroring (the HAL supports it, the SDK's own drivers never ask for it),
 * YUV input, dithering, masking (MASK_*), the A8/A4/A2/L8 source formats
 * (they want the colour coordinate engine and the palette), rotation of a
 * co-engine layer, and output straight to the LCD (AHB_CTRL.DESTINATION). A
 * job that uses one of those still completes, so that firmware waiting on it
 * is not left hanging, but the pixels it produced are not the ones hardware
 * would have produced.
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
#include "hw/qdev-properties.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/address-spaces.h"
#include "system/memory.h"

#include <math.h>

/*
 * The most pixels one job may touch. A 10-bit coordinate space is 1024x1024;
 * the cap only bites on a register set that could not have come from the
 * HAL, and keeps a wild rectangle from being turned into a wild allocation.
 */
#define EPIC_MAX_PIXELS         (1024 * 1024)

/* What a layer's registers say, pulled out of the flat register file. */
typedef struct EpicLayer {
    const char *name;       /* for the log messages */
    unsigned channel;       /* what COENG_CFG.CH_SEL calls this layer */
    unsigned cfg_off;       /* SIFLI_EPIC_<name>_CFG */
    unsigned tl_off;
    unsigned br_off;
    unsigned src_off;
    unsigned fill_off;
    bool transformable;     /* VL is the only layer with the ROT/SCALE block */
} EpicLayer;

/*
 * The live layers, listed in the order the hardware composites them:
 * furthest from the viewer first. L0 is the graphics layer the HAL paints
 * backgrounds and copies into; VL is the video layer that goes on top of it,
 * which is also the one HAL_EPIC_ConfigFilling sets up when it fills with an
 * alpha rather than an opaque colour.
 *
 * The channel numbers are HAL LayerIdx2CH's, not the layer index
 * (bf0_hal_epic.c:1445): the co-engine selects a channel, and VL happens to
 * be channel 0 and L0 channel 1.
 */
static const EpicLayer epic_layers[] = {
    { "L0", 1, SIFLI_EPIC_L0_CFG, SIFLI_EPIC_L0_TL_POS, SIFLI_EPIC_L0_BR_POS,
      SIFLI_EPIC_L0_SRC, SIFLI_EPIC_L0_FILL, false },
    { "VL", 0, SIFLI_EPIC_VL_CFG, SIFLI_EPIC_VL_TL_POS, SIFLI_EPIC_VL_BR_POS,
      SIFLI_EPIC_VL_SRC, SIFLI_EPIC_VL_FILL, true },
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
 * Composite one pixel onto the canvas and re-encode it.
 *
 * Which of the two alpha bits is set is decided by the source format, not by
 * the firmware's intent: a source that carries an alpha channel blends per
 * pixel (ALPHA_BLEND), one that does not blends by the constant in the
 * register (ALPHA_SEL), and the HAL sets neither when it means the layer to
 * be opaque. The register's ALPHA field is written in both cases, so reading
 * it when ALPHA_BLEND is what was meant would ignore the pixel.
 */
static void epic_composite(uint8_t *dst, unsigned o_format, unsigned format,
                           EpicPixel px, unsigned a)
{
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

/*
 * Draw a layer whose pixels come from the EZIP co-engine instead of memory.
 *
 * The decoded frame is the window EZIP was asked for, in source-image
 * coordinates: frame pixel (0, 0) is source pixel (start_col, start_row), and
 * the layer's TL_POS is where source pixel (0, 0) sits on the canvas. The HAL
 * refuses rotating such a layer (bf0_hal_epic.c:6200) but allows scaling one,
 * so this is only the untransformed case; epic_draw_coeng_ezip_scaled() picks
 * up a scaled one. Here the frame lands at TL_POS plus the window's own
 * origin.
 *
 * The decoded pixels are ARGB8888 and their alpha is always applied, which is
 * not what the ALPHA_SEL/ALPHA_BLEND reading of the layer CFG would give: the
 * HAL passes EPIC_LAYER_OPAQUE for the blend and so leaves both bits clear
 * (bf0_hal_epic.c:3098). But the SDK's own example_ezip check expects the
 * decoded alpha to be blended in -- it compares against the image composited
 * onto the background by that alpha -- so the co-engine's pixels must carry
 * it into the blend whatever the CFG says.
 */
static void epic_draw_coeng_ezip(SifliEpicState *s, const EpicLayer *l,
                                 uint8_t *canvas, unsigned cx0, unsigned cy0,
                                 unsigned cw, unsigned ch, unsigned dst_bpp,
                                 unsigned o_format, unsigned lx0, unsigned ly0,
                                 unsigned lx1, unsigned ly1)
{
    SifliEzipFrame frame;
    unsigned fx0, fy0, x0, y0, x1, y1, x, y;

    if (s->ezip == NULL || !sifli_ezip_coeng_frame(s->ezip, &frame)) {
        /*
         * COENG_CFG says this layer's input is EZIP but nothing has been
         * decoded. Reading SRC instead would take the compressed stream for
         * pixels, and drawing nothing quietly would hide the mistake.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sifli-epic: %s is fed by the EZIP co-engine but no "
                      "decoded frame is available; the layer is skipped\n",
                      l->name);
        return;
    }

    /* Where the window sits on the canvas: layer TL plus the window origin. */
    fx0 = lx0 + frame.start_col;
    fy0 = ly0 + frame.start_row;
    x0 = MAX(fx0, MAX(lx0, cx0));
    y0 = MAX(fy0, MAX(ly0, cy0));
    x1 = MIN(fx0 + frame.width - 1, MIN(lx1, cx0 + cw - 1));
    y1 = MIN(fy0 + frame.height - 1, MIN(ly1, cy0 + ch - 1));
    if (x1 < x0 || y1 < y0) {
        return;
    }

    for (y = y0; y <= y1; y++) {
        const uint8_t *row = frame.pixels +
                             (size_t)(y - fy0) * frame.width * 4;

        for (x = x0; x <= x1; x++) {
            const uint8_t *raw = row + (size_t)(x - fx0) * 4;
            uint8_t *dst = canvas + ((y - cy0) * cw + (x - cx0)) * dst_bpp;
            EpicPixel px = epic_decode_pixel(raw, EPIC_FMT_ARGB8888);

            epic_composite(dst, o_format, EPIC_FMT_ARGB8888, px, px.a);
        }
    }
}

/* ------------------------------------------------------------------ */
/* VL layer rotation and scaling.                                      */
/* ------------------------------------------------------------------ */

/*
 * Sign-extend one of the 11-bit fields of ROT_M_CFG2/3. They are signed on
 * 52x; the HAL only had to fit them in an unsigned field on 55x
 * (bf0_hal_epic.c:3193).
 */
static int32_t epic_vl_signed(uint32_t reg, uint32_t msk, unsigned pos)
{
    int32_t v = (reg & msk) >> pos;

    return (v & (1 << EPIC_VL_ROT_M_SIGN_BIT)) ?
           v - (1 << (EPIC_VL_ROT_M_SIGN_BIT + 1)) : v;
}

/*
 * The sine and cosine the hardware rotates by, as Q16.16.
 *
 * The HAL computes them itself and leaves them in MISC_CFG as magnitudes in
 * Q1.12, with the signs coming from ROT_DEG's quadrant (bf0_hal_epic.c:3168).
 * When DEG_FORCE is clear the hardware would run its own calculator from
 * ROT_DEG, so derive them from the angle instead.
 *
 * Note ROT_DEG is whole degrees: the HAL's input angle is in 0.1 degrees and
 * it divides before writing, bumping an exact 0/90/180/270 by one so that the
 * quadrant is defined (bf0_hal_epic.c:2551).
 */
static void epic_vl_sin_cos(uint32_t rot, uint32_t misc,
                            int32_t *sin_q16, int32_t *cos_q16)
{
    unsigned deg = (rot & EPIC_VL_ROT_DEG_Msk) >> EPIC_VL_ROT_DEG_Pos;
    int32_t s, c;

    if (!(misc & EPIC_VL_MISC_CFG_DEG_FORCE)) {
        const double rad = deg * 0.017453292519943295;

        *sin_q16 = lround(sin(rad) * 65536.0);
        *cos_q16 = lround(cos(rad) * 65536.0);
        return;
    }

    s = ((misc & EPIC_VL_MISC_CFG_SIN_FORCE_VALUE_Msk) >>
         EPIC_VL_MISC_CFG_SIN_FORCE_VALUE_Pos) <<
        (16 - EPIC_VL_SIN_COS_FRAC_BIT);
    c = ((misc & EPIC_VL_MISC_CFG_COS_FORCE_VALUE_Msk) >>
         EPIC_VL_MISC_CFG_COS_FORCE_VALUE_Pos) <<
        (16 - EPIC_VL_SIN_COS_FRAC_BIT);

    /* The registers hold |sin| and |cos|; ROT_DEG says which quadrant. */
    if (deg >= 180) {
        s = -s;
    }
    if (deg >= 90 && deg < 270) {
        c = -c;
    }
    *sin_q16 = s;
    *cos_q16 = c;
}

/*
 * Whether the VL registers ask for a transform at all. With none of these
 * set the layer is a plain rectangle copy and takes the byte-for-byte path
 * above, which is what keeps an untransformed job identical.
 */
static bool epic_vl_transform_active(SifliEpicState *s)
{
    uint32_t rot = s->reg[SIFLI_EPIC_VL_ROT / 4];
    uint32_t misc = s->reg[SIFLI_EPIC_VL_MISC_CFG / 4];
    uint32_t mcfg1 = s->reg[SIFLI_EPIC_VL_ROT_M_CFG1 / 4];
    uint32_t pitch_h = s->reg[SIFLI_EPIC_VL_SCALE_RATIO_H / 4] &
                       EPIC_VL_SCALE_RATIO_XPITCH_Msk;
    uint32_t pitch_v = s->reg[SIFLI_EPIC_VL_SCALE_RATIO_V / 4] &
                       EPIC_VL_SCALE_RATIO_YPITCH_Msk;

    if ((rot & EPIC_VL_ROT_DEG_Msk) || (misc & EPIC_VL_MISC_CFG_DEG_FORCE) ||
        (mcfg1 & EPIC_VL_ROT_M_CFG1_M_MODE)) {
        return true;
    }
    /*
     * A step of zero is not a step. The HAL writes EPIC_SCALE_1 for a layer
     * it is not scaling -- both when it configures one and when it resets the
     * video layer's transform (bf0_hal_epic.c:3375-3379, :3479-3483) -- and
     * never leaves zero behind, so zero is a register set the hardware is not
     * asked to run. Reading it as a transform would collapse the whole
     * rectangle onto one source pixel; treat it as "not scaled".
     */
    if ((pitch_h != EPIC_VL_SCALE_1 && pitch_h != 0) ||
        (pitch_v != EPIC_VL_SCALE_1 && pitch_v != 0)) {
        return true;
    }
    if ((s->reg[SIFLI_EPIC_VL_SCALE_INIT_CFG1 / 4] &
         EPIC_VL_SCALE_INIT_CFG1_X_VAL_Msk) ||
        (s->reg[SIFLI_EPIC_VL_SCALE_INIT_CFG2 / 4] &
         EPIC_VL_SCALE_INIT_CFG2_Y_VAL_Msk)) {
        return true;
    }
    return false;
}

/*
 * The transform registers, read together and then used to walk an output
 * pixel back to the source coordinate it samples. Keeping them in one place
 * lets a co-engine layer go through the same mapping a memory layer does:
 * the HAL programs the scaler for both, and only the pixel source differs.
 */
typedef struct EpicVlTransform {
    unsigned lx0, ly0;          /* the layer's TL; lx is measured from it */
    int64_t pitch_x, pitch_y;   /* SCALE_RATIO_H/V, 16.16 */
    int64_t init_x, init_y;     /* SCALE_INIT_CFG1/2, 16.16 */
    int64_t pivot_x_16p16, pivot_y_16p16;
    int64_t xtl, ytl;           /* pre-rotation source TL, signed */
    int32_t sin_q16, cos_q16;
    bool rotating;
} EpicVlTransform;

static void epic_vl_transform_init(SifliEpicState *s, const EpicLayer *l,
                                   EpicVlTransform *t)
{
    uint32_t tl = s->reg[l->tl_off / 4];
    uint32_t rot = s->reg[SIFLI_EPIC_VL_ROT / 4];
    uint32_t misc = s->reg[SIFLI_EPIC_VL_MISC_CFG / 4];
    uint32_t mcfg1 = s->reg[SIFLI_EPIC_VL_ROT_M_CFG1 / 4];
    uint32_t mcfg2 = s->reg[SIFLI_EPIC_VL_ROT_M_CFG2 / 4];
    uint32_t mcfg3 = s->reg[SIFLI_EPIC_VL_ROT_M_CFG3 / 4];
    int32_t pivot_x = epic_vl_signed(mcfg2, EPIC_VL_ROT_M_CFG2_M_PIVOT_X_Msk,
                                     EPIC_VL_ROT_M_CFG2_M_PIVOT_X_Pos);
    int32_t pivot_y = epic_vl_signed(mcfg2, EPIC_VL_ROT_M_CFG2_M_PIVOT_Y_Msk,
                                     EPIC_VL_ROT_M_CFG2_M_PIVOT_Y_Pos);

    t->lx0 = tl & EPIC_CANVAS_X_Msk;
    t->ly0 = (tl & EPIC_CANVAS_Y_Msk) >> EPIC_CANVAS_Y_Pos;
    t->pitch_x = s->reg[SIFLI_EPIC_VL_SCALE_RATIO_H / 4] &
                 EPIC_VL_SCALE_RATIO_XPITCH_Msk;
    t->pitch_y = s->reg[SIFLI_EPIC_VL_SCALE_RATIO_V / 4] &
                 EPIC_VL_SCALE_RATIO_YPITCH_Msk;
    t->init_x = s->reg[SIFLI_EPIC_VL_SCALE_INIT_CFG1 / 4] &
                EPIC_VL_SCALE_INIT_CFG1_X_VAL_Msk;
    t->init_y = s->reg[SIFLI_EPIC_VL_SCALE_INIT_CFG2 / 4] &
                EPIC_VL_SCALE_INIT_CFG2_Y_VAL_Msk;
    t->pivot_x_16p16 = (int64_t)pivot_x << 16;
    t->pivot_y_16p16 = (int64_t)pivot_y << 16;
    t->xtl = epic_vl_signed(mcfg3, EPIC_VL_ROT_M_CFG3_M_XTL_Msk,
                            EPIC_VL_ROT_M_CFG3_M_XTL_Pos);
    t->ytl = epic_vl_signed(mcfg3, EPIC_VL_ROT_M_CFG3_M_YTL_Msk,
                            EPIC_VL_ROT_M_CFG3_M_YTL_Pos);
    t->rotating = (rot & EPIC_VL_ROT_DEG_Msk) ||
                  (misc & EPIC_VL_MISC_CFG_DEG_FORCE) ||
                  (mcfg1 & EPIC_VL_ROT_M_CFG1_M_MODE);
    t->sin_q16 = 0;
    t->cos_q16 = 0;
    if (t->rotating) {
        epic_vl_sin_cos(rot, misc, &t->sin_q16, &t->cos_q16);
    }
}

/*
 * Where the output pixel (x, y) samples the source. The HAL's own arithmetic
 * gives the un-rotated source coordinate as
 *
 *     r = (scale_init + lx * pitch) >> 16
 *
 * with lx the pixel's offset from the layer's top-left. That is literally the
 * expression the HAL uses to size a co-engine window (bf0_hal_epic.c:3755),
 * and it is what DISABLE_SCALE turns into lx. Rotation is undone about the
 * pivot, then the pre-rotation source origin is taken back off:
 *
 *     source = R(-angle) * (r - pivot) + pivot - src_tl
 *
 * so that a source pixel u lands at canvas TL + (R(angle) * (u + src_tl -
 * pivot) + pivot) / factor, which is the placement the HAL's bounding box was
 * built around. The sampled point is truncated, not rounded.
 */
static void epic_vl_transform_apply(const EpicVlTransform *t,
                                    unsigned x, unsigned y,
                                    int32_t *sx, int32_t *sy)
{
    int64_t rx = t->init_x + (int64_t)(x - t->lx0) * t->pitch_x;
    int64_t ry = t->init_y + (int64_t)(y - t->ly0) * t->pitch_y;
    int64_t sx_16p16, sy_16p16;

    if (t->rotating) {
        int64_t dx = rx - t->pivot_x_16p16;
        int64_t dy = ry - t->pivot_y_16p16;

        sx_16p16 = ((t->cos_q16 * dx + t->sin_q16 * dy) >> 16) +
                   t->pivot_x_16p16 - (t->xtl << 16);
        sy_16p16 = ((-t->sin_q16 * dx + t->cos_q16 * dy) >> 16) +
                   t->pivot_y_16p16 - (t->ytl << 16);
    } else {
        sx_16p16 = rx;
        sy_16p16 = ry;
    }
    *sx = sx_16p16 >> 16;
    *sy = sy_16p16 >> 16;
}

/*
 * The co-engine twin of epic_draw_layer_transformed(): the same mapping, with
 * the sampled pixel taken from the EZIP decoder's frame instead of SRC.
 *
 * The HAL lets a co-engine layer be scaled but not rotated
 * (bf0_hal_epic.c:6196-6204): such a job still goes through
 * EPIC_ConfigRotation, so SCALE_RATIO, SCALE_INIT, EXTENTS and the TL/BR
 * bounding box are programmed exactly as they are for a memory layer, and only
 * the pixel source differs. The frame is the window EZIP was asked to decode,
 * which EPIC_CalcDecImgArea derived from the same source-side clip area that
 * produced EXTENTS (bf0_hal_epic.c:3696-3790). A canvas pixel that maps to the
 * source coordinate (sx, sy) is therefore frame pixel
 *
 *     (sx - start_col, sy - start_row)
 *
 * and anything that lands outside the window is left undrawn -- the frame is
 * the clipped region, so it bounds the read on its own. Sampling and blending
 * match the untransformed co-engine path, alpha included.
 */
static void epic_draw_coeng_ezip_scaled(SifliEpicState *s, const EpicLayer *l,
                                        const EpicVlTransform *t,
                                        uint8_t *canvas, unsigned cx0,
                                        unsigned cy0, unsigned cw, unsigned ch,
                                        unsigned dst_bpp, unsigned o_format,
                                        unsigned lx1, unsigned ly1)
{
    SifliEzipFrame frame;
    unsigned x, y;

    if (s->ezip == NULL || !sifli_ezip_coeng_frame(s->ezip, &frame)) {
        /* Same report as the untransformed path: nothing to sample. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sifli-epic: %s is fed by the EZIP co-engine but no "
                      "decoded frame is available; the layer is skipped\n",
                      l->name);
        return;
    }

    for (y = MAX(t->ly0, cy0); y <= MIN(ly1, cy0 + ch - 1); y++) {
        for (x = MAX(t->lx0, cx0); x <= MIN(lx1, cx0 + cw - 1); x++) {
            const uint8_t *raw;
            uint8_t *dst;
            EpicPixel px;
            int32_t sx, sy;
            unsigned fx, fy;

            epic_vl_transform_apply(t, x, y, &sx, &sy);
            if (sx < (int32_t)frame.start_col ||
                sy < (int32_t)frame.start_row) {
                continue;
            }
            fx = sx - frame.start_col;
            fy = sy - frame.start_row;
            if (fx >= frame.width || fy >= frame.height) {
                continue;
            }

            raw = frame.pixels + ((size_t)fy * frame.width + fx) * 4;
            dst = canvas + ((y - cy0) * cw + (x - cx0)) * dst_bpp;
            px = epic_decode_pixel(raw, EPIC_FMT_ARGB8888);
            epic_composite(dst, o_format, EPIC_FMT_ARGB8888, px, px.a);
        }
    }
}

/*
 * Draw the VL layer through the rotation and scaling unit.
 *
 * The HAL does the forward transform on the CPU -- it works out where the
 * rotated and scaled image lands and programs TL_POS/BR_POS, the rotated
 * extent, the pivot, the source top-left and the scale step accordingly
 * (bf0_hal_epic.c:2506-2966, written out at :3145-3250). The hardware then
 * runs that backwards, one output pixel at a time, and that is what this does:
 * epic_vl_transform_apply() maps the pixel, and the sampled point is read from
 * SRC. The source region the layer may read is the clipped one EXTENTS
 * describes -- the same bound EPIC_CalcDecImgArea walks. Nothing outside it is
 * drawn.
 *
 * Sampling is nearest-neighbour: FILTER_EN is the monochrome colour
 * substitute, not an interpolation control (see the header).
 */
static void epic_draw_layer_transformed(SifliEpicState *s, const EpicLayer *l,
                                        uint8_t *canvas, unsigned cx0,
                                        unsigned cy0, unsigned cw, unsigned ch,
                                        unsigned dst_bpp, unsigned o_format)
{
    uint32_t cfg = s->reg[l->cfg_off / 4];
    uint32_t br = s->reg[l->br_off / 4];
    uint32_t misc = s->reg[SIFLI_EPIC_VL_MISC_CFG / 4];
    uint32_t extents = s->reg[SIFLI_EPIC_VL_EXTENTS / 4];
    unsigned format = cfg & EPIC_L_CFG_FORMAT_Msk;
    unsigned src_bpp = epic_src_bpp(format);
    unsigned src_stride = (cfg & EPIC_L_CFG_WIDTH_Msk) >> EPIC_L_CFG_WIDTH_Pos;
    unsigned alpha = (cfg & EPIC_L_CFG_ALPHA_Msk) >> EPIC_L_CFG_ALPHA_Pos;
    bool alpha_sel = (cfg & EPIC_L_CFG_ALPHA_SEL) != 0;
    bool blend = (cfg & EPIC_L_CFG_ALPHA_BLEND) != 0;
    uint32_t src = sifli_sbus_to_cpu_addr(s->reg[l->src_off / 4]);
    unsigned max_col = (extents & EPIC_VL_EXTENTS_MAX_COL_Msk) >>
                       EPIC_VL_EXTENTS_MAX_COL_Pos;
    unsigned max_line = (extents & EPIC_VL_EXTENTS_MAX_LINE_Msk) >>
                        EPIC_VL_EXTENTS_MAX_LINE_Pos;
    unsigned lx1 = br & EPIC_CANVAS_X_Msk;
    unsigned ly1 = (br & EPIC_CANVAS_Y_Msk) >> EPIC_CANVAS_Y_Pos;
    EpicVlTransform t;
    unsigned x, y;

    epic_vl_transform_init(s, l, &t);

    if (misc & (EPIC_VL_MISC_CFG_H_MIRROR | EPIC_VL_MISC_CFG_V_MIRROR)) {
        /*
         * Mirroring is part of the same unit but is not modelled: the HAL
         * only reaches it from a caller that sets h_mirror/v_mirror, and the
         * SDK's own drivers never do. Drawing it unmirrored is wrong in the
         * same way the whole layer used to be, but it is not silent.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sifli-epic: %s asks for mirroring, which is not "
                      "implemented; it is drawn unmirrored\n", l->name);
    }

    for (y = MAX(t.ly0, cy0); y <= MIN(ly1, cy0 + ch - 1); y++) {
        for (x = MAX(t.lx0, cx0); x <= MIN(lx1, cx0 + cw - 1); x++) {
            int64_t addr;
            int32_t sx, sy;
            uint8_t raw[4];
            EpicPixel px;
            uint8_t *dst;
            unsigned a;

            epic_vl_transform_apply(&t, x, y, &sx, &sy);

            /*
             * Only the region the HAL clipped -- and told the hardware about
             * through EXTENTS -- is drawn. Everything else is left as it was.
             */
            if (sx < 0 || sy < 0 || sx > (int32_t)max_col ||
                sy > (int32_t)max_line) {
                continue;
            }

            addr = (int64_t)src + (int64_t)sy * src_stride +
                   (int64_t)sx * src_bpp;
            if (addr < 0 || addr > UINT32_MAX ||
                !epic_guest_read(addr, raw, src_bpp)) {
                continue;
            }

            px = epic_decode_pixel(raw, format);
            if (alpha_sel) {
                a = alpha;
            } else if (blend) {
                a = px.a;
            } else {
                a = 255;
            }

            dst = canvas + ((y - cy0) * cw + (x - cx0)) * dst_bpp;
            epic_composite(dst, o_format, format, px, a);
        }
    }
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
    uint32_t coeng = s->reg[SIFLI_EPIC_COENG_CFG / 4];
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

    /*
     * One layer at a time takes its input from the co-engine, whichever one
     * COENG_CFG.EZIP_CH_SEL names; the rest keep reading SRC.
     */
    if ((coeng & EPIC_COENG_CFG_EZIP_EN) &&
        ((coeng & EPIC_COENG_CFG_EZIP_CH_SEL_Msk) >>
         EPIC_COENG_CFG_EZIP_CH_SEL_Pos) == l->channel) {
        if (l->transformable && epic_vl_transform_active(s)) {
            EpicVlTransform t;

            epic_vl_transform_init(s, l, &t);
            if (!t.rotating) {
                /*
                 * The HAL allows scaling a co-engine layer
                 * (bf0_hal_epic.c:6196-6204), and the scaler is programmed
                 * for it just as it is for a memory layer, so run the coded
                 * frame through the same inverse map and sample it there.
                 */
                epic_draw_coeng_ezip_scaled(s, l, &t, canvas, cx0, cy0, cw,
                                            ch, dst_bpp, o_format, lx1, ly1);
                return;
            }
            /*
             * Rotation is the one transform the HAL refuses for an EZIP
             * layer (bf0_hal_epic.c:6200), so this register set cannot come
             * from it. Draw the window untransformed rather than guess, and
             * say so.
             */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sifli-epic: %s is fed by the EZIP co-engine with "
                          "a rotation configured; the co-engine path does "
                          "not apply it\n", l->name);
        }
        epic_draw_coeng_ezip(s, l, canvas, cx0, cy0, cw, ch, dst_bpp,
                             o_format, lx0, ly0, lx1, ly1);
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

    /*
     * A transform is the one thing that stops the layer being a plain
     * rectangle read: it re-reads the same registers as a mapping from
     * output pixels back to source ones.
     */
    if (l->transformable && epic_vl_transform_active(s)) {
        epic_draw_layer_transformed(s, l, canvas, cx0, cy0, cw, ch, dst_bpp,
                                    o_format);
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

            if (alpha_sel) {
                a = alpha;
            } else if (blend) {
                a = px.a;
            } else {
                a = 255;
            }

            epic_composite(dst, o_format, format, px, a);
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

static const Property sifli_epic_properties[] = {
    /*
     * The EZIP decoder this engine's co-engine reads pixels from. It is a
     * link rather than a plain pointer so the machine wires it the way it
     * wires every other device relationship, and so a board without one
     * still has a working EPIC.
     */
    DEFINE_PROP_LINK("ezip", SifliEpicState, ezip, TYPE_SIFLI_EZIP,
                     SifliEzipState *),
};

static void sifli_epic_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, sifli_epic_reset);
    device_class_set_props(dc, sifli_epic_properties);
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
