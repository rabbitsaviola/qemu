/*
 * SiFli SF32LB52x LCD controller (LCDC)
 *
 * Register map from the SiFli SDK's drivers/cmsis/sf32lb52x/lcd_if.h
 * (LCD_IF_TypeDef is a flat array of uint32_t, so an offset divided by four
 * is the register's index).
 *
 * The controller is an AHB bus master: it fetches the layer's pixels out of
 * guest memory itself, so nothing on this path goes through the SiFli DMA
 * model. That also means the model has to be the one to read the framebuffer.
 *
 * The panel hanging off the controller's QSPI bus is a separate device
 * (-device sifli-lcd-panel), not part of this one; see sifli-lcd-panel.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_SIFLI_LCDC_H
#define HW_DISPLAY_SIFLI_LCDC_H

#include "hw/sysbus.h"
#include "qom/object.h"
#include "ui/console.h"

#define TYPE_SIFLI_LCDC "sifli-lcdc"
OBJECT_DECLARE_SIMPLE_TYPE(SifliLcdcState, SIFLI_LCDC)

/*
 * The controller's register window. Round up past PERF_CNT (0x124): the SDK's
 * register.h gives LCDC1 a 4K page, and nothing else in it answers.
 */
#define SIFLI_LCDC_MMIO_SIZE    0x1000

/*
 * Registers the model gives behaviour to. The ones in between (the layer 1
 * block, the JDI/DPI interfaces, the decompression configuration) are only
 * stored and read back.
 */
enum {
    LCDC_COMMAND        = 0x000 / 4,
    LCDC_STATUS         = 0x004 / 4,
    LCDC_IRQ            = 0x008 / 4,
    LCDC_SETTING        = 0x00c / 4,
    LCDC_CANVAS_TL_POS  = 0x010 / 4,
    LCDC_CANVAS_BR_POS  = 0x014 / 4,
    LCDC_CANVAS_BG      = 0x018 / 4,
    LCDC_LAYER0_CONFIG  = 0x01c / 4,
    LCDC_LAYER0_TL_POS  = 0x020 / 4,
    LCDC_LAYER0_BR_POS  = 0x024 / 4,
    LCDC_LAYER0_FILTER  = 0x028 / 4,
    LCDC_LAYER0_SRC     = 0x02c / 4,
    LCDC_LAYER0_FILL    = 0x030 / 4,
    LCDC_LCD_CONF       = 0x080 / 4,
    LCDC_LCD_IF_CONF    = 0x084 / 4,
    LCDC_LCD_MEM        = 0x088 / 4,
    LCDC_LCD_O_WIDTH    = 0x08c / 4,
    LCDC_LCD_SINGLE     = 0x090 / 4,
    LCDC_LCD_WR         = 0x094 / 4,
    LCDC_LCD_RD         = 0x098 / 4,
    LCDC_SPI_IF_CONF    = 0x09c / 4,
    LCDC_TE_CONF        = 0x0a0 / 4,
    LCDC_CANVAS_STAT0   = 0x110 / 4,
    LCDC_PERF_CNT       = 0x124 / 4,
    LCDC_NREGS          = 0x124 / 4 + 1,
};

struct SifliLcdcState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    QemuConsole *con;

    uint32_t regs[LCDC_NREGS];

    /*
     * Command path -- LCD_WR + LCD_SINGLE, the controller talking to the
     * panel rather than to memory.
     */
    uint32_t cmd_word;      /* last complete command word written */
    uint32_t cmd_acc;       /* byte-wise accumulator, see sifli_lcdc_write() */
    uint32_t rd_data;       /* what the panel put on the bus last */

    /* Frame path */
    BusState *qspi;         /* the bus a panel attaches to */
    DeviceState *panel_dev; /* held as DeviceState to keep the headers apart */
    /*
     * Shadow framebuffer, one RGB565 pixel per element. It is typed as the
     * pixels rather than as bytes so that the index in the blit is a pixel
     * index; the display surface made from it wants a byte pointer instead.
     */
    uint16_t *fb;
    size_t fb_size;
    uint32_t fb_width;
    uint32_t fb_height;
    DisplaySurface *surface;
    bool eof;
    bool warned_no_panel;
};

/*
 * Attach a panel. Called from the panel's realize: -device devices are
 * created after the machine, so the panel always arrives after the LCDC has
 * realized and cannot be found by the controller itself.
 */
void sifli_lcdc_set_panel(SifliLcdcState *s, DeviceState *panel);

#endif /* HW_DISPLAY_SIFLI_LCDC_H */
