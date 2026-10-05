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
};

#endif /* HW_DISPLAY_SIFLI_EPIC_H */
