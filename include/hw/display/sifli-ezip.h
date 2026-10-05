/*
 * SiFli EZIP decompression accelerator
 *
 * Register map from the SiFli SDK's drivers/cmsis/sf32lb52x/ezip.h. See
 * hw/display/sifli-ezip.c for what the model does and does not reproduce.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_SIFLI_EZIP_H
#define HW_DISPLAY_SIFLI_EZIP_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_SIFLI_EZIP "sifli-ezip"
OBJECT_DECLARE_SIMPLE_TYPE(SifliEzipState, SIFLI_EZIP)

/*
 * Register offsets, in bytes: the whole EZIP_TypeDef, in the order the SDK
 * declares it. Only EZIP_CTRL, EZIP_PARA, SRC_ADDR, DST_ADDR and the
 * interrupt group carry behaviour; the rest is storage, including the
 * animation (AEZIP/FRAME_*) and debug (DB_*) blocks.
 */
enum {
    SIFLI_EZIP_CTRL         = 0x00,
    SIFLI_EZIP_SRC_ADDR     = 0x04,
    SIFLI_EZIP_DST_ADDR     = 0x08,
    SIFLI_EZIP_PARA         = 0x0c,
    SIFLI_EZIP_CACHE_CLR    = 0x10,
    SIFLI_EZIP_START_POINT  = 0x14,
    SIFLI_EZIP_END_POINT    = 0x18,
    SIFLI_EZIP_ROW_SIGN     = 0x1c,
    SIFLI_EZIP_INT_EN       = 0x20,
    SIFLI_EZIP_INT_STA      = 0x24,
    SIFLI_EZIP_INT_MASK     = 0x28,
    SIFLI_EZIP_NAP_PARA     = 0x2c,
    SIFLI_EZIP_SRC_LEN      = 0x30,
    SIFLI_EZIP_AEZIP_CTRL   = 0x34,
    SIFLI_EZIP_FRAME_START  = 0x38,
    SIFLI_EZIP_PLAY_START   = 0x3c,
    SIFLI_EZIP_FRAME_NUM    = 0x40,
    SIFLI_EZIP_PLAY_NUM     = 0x44,
    SIFLI_EZIP_SEQ_NUM      = 0x48,
    SIFLI_EZIP_FRAME_AREA   = 0x4c,
    SIFLI_EZIP_FRAME_OFFSET = 0x50,
    SIFLI_EZIP_FRAME_DELAY  = 0x54,
    SIFLI_EZIP_FRAME_TYPE   = 0x58,
    SIFLI_EZIP_FRAME_SIZE   = 0x5c,
    SIFLI_EZIP_GREY_PARA    = 0x60,
    SIFLI_EZIP_DB_SEL       = 0x64,
    SIFLI_EZIP_DB_DATA0     = 0x68,
    SIFLI_EZIP_DB_DATA1     = 0x6c,
    SIFLI_EZIP_DB_DATA2     = 0x70,
    SIFLI_EZIP_DB_DATA3     = 0x74,
    SIFLI_EZIP_DB_DATA4     = 0x78,
    SIFLI_EZIP_DB_DATA5     = 0x7c,
    SIFLI_EZIP_DB_DATA6     = 0x80,
    SIFLI_EZIP_DB_DATA7     = 0x84,
    SIFLI_EZIP_DB_DATA8     = 0x88,
    SIFLI_EZIP_DB_DATA9     = 0x8c,
    SIFLI_EZIP_DB_DATA10    = 0x90,
    SIFLI_EZIP_DB_DATA11    = 0x94,
    SIFLI_EZIP_DB_DATA12    = 0x98,
    SIFLI_EZIP_DB_DATA13    = 0x9c,

    SIFLI_EZIP_NUM_REGS     = 0xa0 / 4,
};

/* EZIP_CTRL */
#define EZIP_CTRL_START             BIT(0)

/* EZIP_PARA */
#define EZIP_PARA_OUT_SEL           BIT(0)
#define EZIP_PARA_OUT_EPIC          0
#define EZIP_PARA_OUT_AHB           1
#define EZIP_PARA_MOD_SEL           (3u << 1)
#define EZIP_PARA_MOD_EZIP          0
#define EZIP_PARA_MOD_GZIP          1
#define EZIP_PARA_MOD_LZ4           2
#define EZIP_PARA_CACHE_EN          BIT(3)
#define EZIP_PARA_IN_SEL            BIT(4)
#define EZIP_PARA_SPI_SEL           BIT(5)

/*
 * INT_EN, INT_STA and INT_MASK share a layout. The three error bits are the
 * ones HAL_EZIP_Decode and the interrupt path react to; END is the one that
 * means the frame is there.
 */
#define EZIP_INT_END                BIT(0)
#define EZIP_INT_ROW                BIT(1)
#define EZIP_INT_ROW_ERR            BIT(2)
#define EZIP_INT_BTYPE_ERR          BIT(3)
#define EZIP_INT_ETYPE_ERR          BIT(4)
#define EZIP_INT_AEZIP              BIT(5)
#define EZIP_INT_ERRORS             (EZIP_INT_ROW_ERR | EZIP_INT_BTYPE_ERR | \
                                     EZIP_INT_ETYPE_ERR)

/*
 * The register block ends at 0xA0. The window is rounded up to the 4K the
 * machine maps: EZIP1 spans 0x5000_6000-0x5000_6FFF, with EPIC immediately
 * above it.
 */
#define SIFLI_EZIP_MMIO_SIZE        0x1000

struct SifliEzipState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;

    uint32_t reg[SIFLI_EZIP_NUM_REGS];

    /*
     * The host binary that decodes the proprietary EZIP bitstream; there is
     * no software decoder for it anywhere in the SDK. Empty means the
     * format cannot be decoded, which is only a problem for firmware that
     * asks for it.
     */
    char *tool;

    /*
     * How much of the source to read when the format does not say. The
     * proprietary bitstream and the LZ4 block both carry a length, and a
     * deflate stream ends itself, so this is a backstop rather than a size.
     */
    uint32_t window_bytes;
};

#endif /* HW_DISPLAY_SIFLI_EZIP_H */
