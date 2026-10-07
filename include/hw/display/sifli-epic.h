/*
 * SiFli EPIC 2D graphics engine
 *
 * Register map from the SiFli SDK's drivers/cmsis/sf32lb52x/epic.h. See
 * hw/display/sifli-epic.c for what the model does and does not reproduce.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_SIFLI_EPIC_H
#define HW_DISPLAY_SIFLI_EPIC_H

#include "hw/display/sifli-ezip.h"
#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_SIFLI_EPIC "sifli-epic"
OBJECT_DECLARE_SIMPLE_TYPE(SifliEpicState, SIFLI_EPIC)

/*
 * Register offsets, in bytes: the whole EPIC_TypeDef, in the order the SDK
 * declares it. The layer blocks are the interesting part -- each is a run of
 * registers, and the live ones on 52x are VL and L0 (the HAL defines
 * EPIC_L2_L1_INVALID for SF32LB52X, so L1 and L2 do not exist on the part).
 */
enum {
    SIFLI_EPIC_COMMAND          = 0x000,
    SIFLI_EPIC_STATUS           = 0x004,
    SIFLI_EPIC_EOF_IRQ          = 0x008,
    SIFLI_EPIC_SETTING          = 0x00c,
    SIFLI_EPIC_CANVAS_TL_POS    = 0x010,
    SIFLI_EPIC_CANVAS_BR_POS    = 0x014,
    SIFLI_EPIC_CANVAS_BG        = 0x018,
    SIFLI_EPIC_VL_CFG           = 0x01c,
    SIFLI_EPIC_VL_TL_POS        = 0x020,
    SIFLI_EPIC_VL_BR_POS        = 0x024,
    SIFLI_EPIC_VL_EXTENTS       = 0x028,
    SIFLI_EPIC_VL_FILTER        = 0x02c,
    SIFLI_EPIC_VL_SRC           = 0x030,
    SIFLI_EPIC_VL_ROT           = 0x034,
    SIFLI_EPIC_VL_ROT_STAT      = 0x038,
    SIFLI_EPIC_VL_SCALE_RATIO_H = 0x03c,
    SIFLI_EPIC_VL_SCALE_RATIO_V = 0x040,
    SIFLI_EPIC_VL_FILL          = 0x044,
    SIFLI_EPIC_VL_MISC_CFG      = 0x048,
    SIFLI_EPIC_L0_CFG           = 0x050,
    SIFLI_EPIC_L0_TL_POS        = 0x054,
    SIFLI_EPIC_L0_BR_POS        = 0x058,
    SIFLI_EPIC_L0_FILTER        = 0x05c,
    SIFLI_EPIC_L0_SRC           = 0x060,
    SIFLI_EPIC_L0_FILL          = 0x064,
    SIFLI_EPIC_L0_MISC_CFG      = 0x068,
    SIFLI_EPIC_L1_CFG           = 0x070,
    SIFLI_EPIC_L1_TL_POS        = 0x074,
    SIFLI_EPIC_L1_BR_POS        = 0x078,
    SIFLI_EPIC_L1_FILTER        = 0x07c,
    SIFLI_EPIC_L1_SRC           = 0x080,
    SIFLI_EPIC_L1_FILL          = 0x084,
    SIFLI_EPIC_L1_MISC_CFG      = 0x088,
    SIFLI_EPIC_L2_CFG           = 0x090,
    SIFLI_EPIC_L2_TL_POS        = 0x094,
    SIFLI_EPIC_L2_BR_POS        = 0x098,
    SIFLI_EPIC_L2_EXTENTS       = 0x09c,
    SIFLI_EPIC_L2_FILTER        = 0x0a0,
    SIFLI_EPIC_L2_SRC           = 0x0a4,
    SIFLI_EPIC_L2_ROT           = 0x0a8,
    SIFLI_EPIC_L2_ROT_STAT      = 0x0ac,
    SIFLI_EPIC_L2_SCALE_RATIO_H = 0x0b0,
    SIFLI_EPIC_L2_SCALE_RATIO_V = 0x0b4,
    SIFLI_EPIC_L2_FILL          = 0x0b8,
    SIFLI_EPIC_L2_MISC_CFG      = 0x0bc,
    SIFLI_EPIC_MASK_CFG         = 0x0c0,
    SIFLI_EPIC_MASK_TL_POS      = 0x0c4,
    SIFLI_EPIC_MASK_BR_POS      = 0x0c8,
    SIFLI_EPIC_MASK_SRC         = 0x0cc,
    SIFLI_EPIC_COENG_CFG        = 0x0d0,
    SIFLI_EPIC_YUV_ENG_CFG0     = 0x0d4,
    SIFLI_EPIC_YUV_ENG_CFG1     = 0x0d8,
    SIFLI_EPIC_Y_SRC            = 0x0dc,
    SIFLI_EPIC_U_SRC            = 0x0e0,
    SIFLI_EPIC_V_SRC            = 0x0e4,
    SIFLI_EPIC_COEF0            = 0x0e8,
    SIFLI_EPIC_COEF1            = 0x0ec,
    SIFLI_EPIC_DITHER_CONF      = 0x0f0,
    SIFLI_EPIC_DITHER_LFSR      = 0x0f4,
    SIFLI_EPIC_AHB_CTRL         = 0x0f8,
    SIFLI_EPIC_AHB_MEM          = 0x0fc,
    SIFLI_EPIC_AHB_STRIDE       = 0x100,
    SIFLI_EPIC_DEBUG            = 0x104,
    SIFLI_EPIC_VL_ROT_M_CFG1    = 0x108,
    SIFLI_EPIC_VL_ROT_M_CFG2    = 0x10c,
    SIFLI_EPIC_VL_ROT_M_CFG3    = 0x110,
    SIFLI_EPIC_VL_SCALE_INIT_CFG1 = 0x114,
    SIFLI_EPIC_VL_SCALE_INIT_CFG2 = 0x118,
    SIFLI_EPIC_L2_ROT_M_CFG1    = 0x11c,
    SIFLI_EPIC_L2_ROT_M_CFG2    = 0x120,
    SIFLI_EPIC_L2_ROT_M_CFG3    = 0x124,
    SIFLI_EPIC_L2_SCALE_INIT_CFG1 = 0x128,
    SIFLI_EPIC_L2_SCALE_INIT_CFG2 = 0x12c,
    SIFLI_EPIC_PERF_CNT         = 0x130,
    SIFLI_EPIC_CANVAS_STAT      = 0x140,
    SIFLI_EPIC_EZIP_STAT        = 0x144,
    SIFLI_EPIC_OL_STAT          = 0x148,
    SIFLI_EPIC_OL2_STAT         = 0x14c,
    SIFLI_EPIC_VL_STAT          = 0x150,
    SIFLI_EPIC_ML_STAT          = 0x154,
    SIFLI_EPIC_MEM_IF_STAT      = 0x158,

    SIFLI_EPIC_NUM_REGS         = 0x15c / 4,
};

/* COMMAND */
#define EPIC_COMMAND_START          BIT(0)
#define EPIC_COMMAND_RESET          BIT(1)

/* STATUS: both bits stay clear, because a job finishes as it is started. */
#define EPIC_STATUS_IA_BUSY         BIT(0)
#define EPIC_STATUS_LCD_BUSY        BIT(4)

/* EOF_IRQ: cause bits at the bottom, sticky status at bit 16 up. */
#define EPIC_EOF_IRQ_CAUSE          BIT(0)
#define EPIC_EOF_IRQ_LINE_HIT_CAUSE BIT(1)
#define EPIC_EOF_IRQ_STATUS         BIT(16)
#define EPIC_EOF_IRQ_LINE_HIT_STATUS BIT(17)

/* SETTING */
#define EPIC_SETTING_EOF_IRQ_MASK   BIT(0)
#define EPIC_SETTING_LINE_IRQ_MASK  BIT(1)

/* CANVAS_TL_POS / CANVAS_BR_POS: the target rectangle. */
#define EPIC_CANVAS_X_Pos           0
#define EPIC_CANVAS_X_Msk           (0x3ffu << 0)
#define EPIC_CANVAS_Y_Pos           16
#define EPIC_CANVAS_Y_Msk           (0x3ffu << 16)

/* CANVAS_BG: the colour the canvas is cleared to, and the fill colour. */
#define EPIC_CANVAS_BG_BLUE_Pos     0
#define EPIC_CANVAS_BG_BLUE_Msk     (0xffu << 0)
#define EPIC_CANVAS_BG_GREEN_Pos    8
#define EPIC_CANVAS_BG_GREEN_Msk    (0xffu << 8)
#define EPIC_CANVAS_BG_RED_Pos      16
#define EPIC_CANVAS_BG_RED_Msk      (0xffu << 16)
#define EPIC_CANVAS_BG_RGB_Msk      0xffffff
/* The doubled "BG" is the SDK's spelling, kept so the two grep alike. */
#define EPIC_CANVAS_BG_BG_BLENDING_BYPASS BIT(24)
#define EPIC_CANVAS_BG_ALL_BLENDING_BYPASS BIT(25)

/* Layer CFG, shared by VL and L0 (and by L1/L2 where they exist). */
#define EPIC_L_CFG_FORMAT_Pos       0
#define EPIC_L_CFG_FORMAT_Msk       (0xfu << 0)
#define EPIC_L_CFG_ALPHA_SEL        BIT(4)
#define EPIC_L_CFG_ALPHA_Pos        5
#define EPIC_L_CFG_ALPHA_Msk        (0xffu << 5)
#define EPIC_L_CFG_WIDTH_Pos        16
#define EPIC_L_CFG_WIDTH_Msk        (0x1fffu << 16)
#define EPIC_L_CFG_ACTIVE           BIT(30)
#define EPIC_L_CFG_ALPHA_BLEND      BIT(31)
/*
 * CFG.FILTER_EN only ever accompanies the FILTER register's RGB, which the
 * HAL uses to substitute a colour for the ones an A8/L8 source cannot carry
 * (bf0_hal_epic.c:3128). It is not a resampling filter: no register selects
 * an interpolation kernel for the scaler, so a scaled VL layer is sampled
 * nearest-neighbour.
 */
#define EPIC_L_CFG_FILTER_EN        BIT(15)

/* Source pixel formats, from EPIC_L0_CFG_FMT_*. */
enum {
    EPIC_FMT_RGB565 = 0,
    EPIC_FMT_RGB888 = 1,
    EPIC_FMT_ARGB8888 = 2,
    EPIC_FMT_ARGB8565 = 3,
    EPIC_FMT_A8 = 4,
    EPIC_FMT_A4 = 5,
    EPIC_FMT_L8 = 6,
    EPIC_FMT_A2 = 7,
};

/* AHB_CTRL: where the result goes, and in what format. */
#define EPIC_AHB_CTRL_DESTINATION   BIT(0)
#define EPIC_AHB_CTRL_DEST_RAM      0
#define EPIC_AHB_CTRL_DEST_LCD      1
#define EPIC_AHB_CTRL_O_FORMAT_Pos  1
#define EPIC_AHB_CTRL_O_FORMAT_Msk  (3u << 1)

/*
 * COENG_CFG: which layer's input comes from a co-engine rather than from its
 * SRC register. EZIP_CH_SEL is the layer's channel, which the HAL's
 * LayerIdx2CH() numbers VL=0, L0=1 (bf0_hal_epic.c:1445) -- not the layer
 * index. EPIC_COENG_CFG_EZIP_EN exists on 52x, so the HAL takes this path
 * rather than the per-layer CFG.EZIP_EN one.
 */
#define EPIC_COENG_CFG_EZIP_EN          BIT(0)
#define EPIC_COENG_CFG_EZIP_CH_SEL_Pos  1
#define EPIC_COENG_CFG_EZIP_CH_SEL_Msk  (3u << 1)

/*
 * The VL transform registers. VL is the only layer on 52x with them: L0 has
 * no ROT/SCALE block, and L1/L2 do not exist on the part. The HAL computes
 * the whole transform on the CPU and leaves these as the result, so the model
 * reads them as the hardware would (bf0_hal_epic.c:3145-3250).
 */

/*
 * VL_ROT. ROT_DEG is in whole degrees, not tenths: the HAL's input angle is
 * in 0.1 degrees and it divides by ten first, then bumps an exact 0/90/180/270
 * to the neighbouring degree so the quadrant is defined (bf0_hal_epic.c:2551).
 * DEG_FORCE carries "use the forced sin/cos below" and lives in MISC_CFG on
 * this part, not here.
 */
#define EPIC_VL_ROT_CALC_REQ        BIT(0)
#define EPIC_VL_ROT_CALC_CLR        BIT(1)
#define EPIC_VL_ROT_DEG_Pos         2
#define EPIC_VL_ROT_DEG_Msk         (0x1ffu << 2)

/*
 * VL_MISC_CFG: the forced sine and cosine, the mirror bits, and the palette
 * select. The sin/cos are magnitudes only -- the quadrant comes from ROT_DEG
 * -- and are Q1.12, which is the HAL's Q1.15 table shifted down by
 * EPIC_SIN_COS_FRAC_BIT - EPIC_VL_MISC_CFG_SIN_FRAC_BIT (15 - 12).
 */
#define EPIC_VL_MISC_CFG_CLUT_SEL   BIT(0)
#define EPIC_VL_MISC_CFG_V_MIRROR   BIT(1)
#define EPIC_VL_MISC_CFG_H_MIRROR   BIT(2)
#define EPIC_VL_MISC_CFG_COS_FORCE_VALUE_Pos 3
#define EPIC_VL_MISC_CFG_COS_FORCE_VALUE_Msk (0x1fffu << 3)
#define EPIC_VL_MISC_CFG_SIN_FORCE_VALUE_Pos 16
#define EPIC_VL_MISC_CFG_SIN_FORCE_VALUE_Msk (0x1fffu << 16)
#define EPIC_VL_MISC_CFG_DEG_FORCE  BIT(29)
#define EPIC_VL_SIN_COS_FRAC_BIT    12

/*
 * VL_SCALE_RATIO_H/V: the scale step, 16.16, so 1.0 is 0x10000. The HAL
 * converts its 1024-is-1.0 input by shifting left by (16 - 10)
 * (EPIC_CONV_SCALE_FACTOR, bf0_hal_epic.c:64) and refuses anything above the
 * field, so the value here is what the hardware steps the source by for each
 * output pixel.
 */
#define EPIC_VL_SCALE_RATIO_XPITCH_Pos  0
#define EPIC_VL_SCALE_RATIO_XPITCH_Msk  (0x3ffffffu << 0)
#define EPIC_VL_SCALE_RATIO_YPITCH_Pos  0
#define EPIC_VL_SCALE_RATIO_YPITCH_Msk  (0x3ffffffu << 0)
#define EPIC_VL_SCALE_1             (1u << 16)

/*
 * VL_EXTENTS: the size, as a max index, of the source region SRC points at.
 * The HAL fills it from the clipped source area
 * (bf0_hal_epic.c:3145), which is also the region it walks in
 * EPIC_CalcDecImgArea (bf0_hal_epic.c:3755) -- so it is the bound a scaled or
 * rotated layer may sample within.
 */
#define EPIC_VL_EXTENTS_MAX_LINE_Pos 0
#define EPIC_VL_EXTENTS_MAX_LINE_Msk (0x3ffu << 0)
#define EPIC_VL_EXTENTS_MAX_COL_Pos 16
#define EPIC_VL_EXTENTS_MAX_COL_Msk (0x3ffu << 16)

/* VL_ROT_M_CFG1: the rotated image's extent, before scaling, and M_MODE. */
#define EPIC_VL_ROT_M_CFG1_M_ROT_MAX_LINE_Pos 0
#define EPIC_VL_ROT_M_CFG1_M_ROT_MAX_LINE_Msk (0x7ffu << 0)
#define EPIC_VL_ROT_M_CFG1_M_ROT_MAX_COL_Pos 16
#define EPIC_VL_ROT_M_CFG1_M_ROT_MAX_COL_Msk (0x7ffu << 16)
#define EPIC_VL_ROT_M_CFG1_M_MODE            BIT(31)

/*
 * VL_ROT_M_CFG2 and VL_ROT_M_CFG3: the pivot and the pre-rotation source
 * top-left, both relative to the layer's own top-left and both signed on
 * 52x (the 55x HAL had to keep them non-negative). Eleven bits each.
 */
#define EPIC_VL_ROT_M_CFG2_M_PIVOT_X_Pos 0
#define EPIC_VL_ROT_M_CFG2_M_PIVOT_X_Msk (0x7ffu << 0)
#define EPIC_VL_ROT_M_CFG2_M_PIVOT_Y_Pos 16
#define EPIC_VL_ROT_M_CFG2_M_PIVOT_Y_Msk (0x7ffu << 16)
#define EPIC_VL_ROT_M_CFG3_M_XTL_Pos     0
#define EPIC_VL_ROT_M_CFG3_M_XTL_Msk     (0x7ffu << 0)
#define EPIC_VL_ROT_M_CFG3_M_YTL_Pos     16
#define EPIC_VL_ROT_M_CFG3_M_YTL_Msk     (0x7ffu << 16)
#define EPIC_VL_ROT_M_SIGN_BIT           10

/* VL_SCALE_INIT_CFG1/2: the initial scaling phase, in 16.16. */
#define EPIC_VL_SCALE_INIT_CFG1_X_VAL_Pos 0
#define EPIC_VL_SCALE_INIT_CFG1_X_VAL_Msk (0x3ffffffu << 0)
#define EPIC_VL_SCALE_INIT_CFG2_Y_VAL_Pos 0
#define EPIC_VL_SCALE_INIT_CFG2_Y_VAL_Msk (0x3ffffffu << 0)

enum {
    EPIC_OUT_RGB565 = 0,
    EPIC_OUT_RGB888 = 1,
    EPIC_OUT_ARGB8888 = 2,
    EPIC_OUT_ARGB8565 = 3,
};

/*
 * The register block ends at 0x15C. The window is rounded up to the 4K the
 * machine maps: EPIC spans 0x5000_7000-0x5000_7FFF.
 */
#define SIFLI_EPIC_MMIO_SIZE        0x1000

struct SifliEpicState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;

    uint32_t reg[SIFLI_EPIC_NUM_REGS];

    /*
     * The EZIP decoder a co-engine layer reads its pixels from. Wired by the
     * machine; NULL leaves the co-engine path reporting rather than drawing.
     * The coupling is a QOM link rather than EPIC walking the EZIP MMIO
     * window, which is not memory and would decode nothing.
     */
    SifliEzipState *ezip;
};

#endif /* HW_DISPLAY_SIFLI_EPIC_H */
