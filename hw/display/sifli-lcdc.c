/*
 * SiFli SF32LB52x LCD controller (LCDC)
 *
 * Register map and semantics from the SiFli SDK:
 * drivers/cmsis/sf32lb52x/lcd_if.h for the registers,
 * drivers/hal/bf0_hal_lcdc.c for what the HAL does with them.
 * See include/hw/display/sifli-lcdc.h.
 *
 * Thin semantics, as everywhere else in this machine: the model reproduces
 * the states the HAL waits for and nothing about how long they take.
 *
 *   - STATUS.LCD_BUSY and LCD_SINGLE.LCD_BUSY always read clear. Nothing is
 *     ever in flight, so WaitBusy() and WAIT_LCDC_SINGLE_BUSY() fall through
 *     on the first test instead of running to their timeouts.
 *   - A write of COMMAND.START finishes the frame there and then: the pixels
 *     are fetched, the shadow framebuffer is updated and IRQ.EOF_STAT is
 *     raised before the write returns. Firmware never sees a partial frame.
 *   - IRQ is write-one-to-clear and *not* read-to-clear. The HAL both polls
 *     it bare (bf0_hal_lcdc.c:2026) and clears it by writing back what it
 *     read (:5784), and a read that cleared would spin the poll forever.
 *
 * There are two independent paths through this device and they do not meet:
 *
 *   (a) The command path -- LCD_WR and LCD_SINGLE. This is the controller
 *       talking to the panel: sending it a register address, and reading one
 *       back. Nothing here touches guest memory.
 *   (b) The frame path -- COMMAND.START. The controller is an AHB bus master
 *       and fetches the layer's pixels itself, so this model reads them out
 *       of guest memory with dma_memory_read() rather than waiting for a DMA
 *       channel to push them.
 *
 * Output format is fixed at RGB565, which is what these panels take and what
 * the display surface holds. The source format is whatever firmware put in
 * LAYER0_CONFIG.FORMAT: RGB565 and RGB888 are handled, anything else is
 * reported and the frame is dropped rather than drawn wrongly.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/display/sifli-lcdc.h"
#include "hw/display/sifli-panel.h"
#include "hw/irq.h"
#include "hw/misc/sifli-sbus.h"
#include "hw/qdev-properties.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/address-spaces.h"
#include "system/dma.h"

/* COMMAND */
#define LCDC_COMMAND_START      BIT(0)
#define LCDC_COMMAND_RESET      BIT(1)

/* IRQ. The same EOF event seen two ways: masked and raw. */
#define LCDC_IRQ_EOF_STAT       BIT(0)
#define LCDC_IRQ_EOF_RAW_STAT   BIT(16)

/* SETTING */
#define LCDC_SETTING_EOF_MASK   BIT(0)

/* LCD_SINGLE */
#define LCDC_SINGLE_TYPE        BIT(0)
#define LCDC_SINGLE_WR_TRIG     BIT(1)
#define LCDC_SINGLE_RD_TRIG     BIT(2)
#define LCDC_SINGLE_LCD_BUSY    BIT(3)

/* SPI_IF_CONF */
#define LCDC_SPI_RD_LEN_Pos     20
#define LCDC_SPI_RD_LEN_LEN     2
#define LCDC_SPI_WR_LEN_Pos     22
#define LCDC_SPI_WR_LEN_LEN     2
#define LCDC_SPI_CS_AUTO_DIS    BIT(27)

/* LAYER0_CONFIG */
#define LCDC_LAYER0_FORMAT_Pos  0
#define LCDC_LAYER0_FORMAT_LEN  3
#define LCDC_LAYER0_WIDTH_Pos   13
#define LCDC_LAYER0_WIDTH_LEN   13
#define LCDC_LAYER0_ACTIVE      BIT(28)

/* LAYER0_TL_POS / LAYER0_BR_POS */
#define LCDC_LAYER_X_Pos        0
#define LCDC_LAYER_Y_Pos        16
#define LCDC_LAYER_XY_LEN       11

/* LAYER0_CONFIG.FORMAT */
#define LCDC_FORMAT_RGB565      0
#define LCDC_FORMAT_RGB888      1

/* LCD_CONF.SPI_LCD_FORMAT, the one output format this model can present. */
#define LCDC_SPI_LCD_FORMAT_Pos 10
#define LCDC_SPI_LCD_FORMAT_LEN 2
#define LCDC_SPI_LCD_FORMAT_RGB565  1

/* Upper bound on a layer, so a nonsense register cannot ask for a huge read. */
#define LCDC_MAX_DIM            4096

static uint32_t sifli_lcdc_read_reg(SifliLcdcState *s, unsigned idx)
{
    switch (idx) {
    case LCDC_COMMAND:
    case LCDC_STATUS:
    case LCDC_LCD_SINGLE:
        /*
         * COMMAND is a strobe and the two status registers report states
         * that never occur here, so all three read back clear. In
         * particular a zero LCD_SINGLE is what WAIT_LCDC_SINGLE_BUSY wants.
         */
        return 0;
    case LCDC_IRQ: {
        uint32_t v = 0;

        if (s->eof) {
            v |= LCDC_IRQ_EOF_RAW_STAT;
            if (s->regs[LCDC_SETTING] & LCDC_SETTING_EOF_MASK) {
                v |= LCDC_IRQ_EOF_STAT;
            }
        }
        return v;
    }
    case LCDC_LCD_RD:
        return s->rd_data;
    default:
        return s->regs[idx];
    }
}

static void sifli_lcdc_update_irq(SifliLcdcState *s)
{
    bool level = s->eof && (s->regs[LCDC_SETTING] & LCDC_SETTING_EOF_MASK);

    qemu_set_irq(s->irq, level);
}

/* (a) The command path: the controller sending the panel a register address. */

/*
 * Work out the register a command word is addressing.
 *
 * The word is built by the caller, not by the hardware: co5300.c sends
 * (0x03 << 24) | (reg << 8) for a read, and HAL_LCDC_ReadU32Reg() passes
 * addr_len = 4 regardless of how many bytes come back. So the top byte is an
 * opcode, the next two are the register, and the byte count is not in the
 * word at all -- it is in SPI_IF_CONF.RD_LEN, which is where the HAL puts it
 * just before triggering the read.
 */
static unsigned sifli_lcdc_cmd_reg(uint32_t cmd_word)
{
    return (cmd_word >> 8) & 0xffff;
}

static void sifli_lcdc_write_cmd(SifliLcdcState *s, uint32_t v)
{
    uint32_t wr_len = extract32(s->regs[LCDC_SPI_IF_CONF],
                                LCDC_SPI_WR_LEN_Pos, LCDC_SPI_WR_LEN_LEN);

    if (wr_len == 0) {
        /*
         * Byte-at-a-time mode, used when the panel needs a gap between
         * bytes. SendSingleCmd() writes the word one byte at a time, most
         * significant first; reassemble them so the register can still be
         * read out of the word afterwards.
         */
        s->cmd_acc = (s->cmd_acc << 8) | (v & 0xff);
    } else {
        s->cmd_acc = v;
    }

    s->cmd_word = s->cmd_acc;
}

static void sifli_lcdc_write_single(SifliLcdcState *s, uint32_t v)
{
    if (v & LCDC_SINGLE_WR_TRIG) {
        /* The command word has gone out; it is no longer just a register. */
        s->cmd_word = s->cmd_acc;
    }

    if (v & LCDC_SINGLE_RD_TRIG) {
        unsigned len = extract32(s->regs[LCDC_SPI_IF_CONF],
                                 LCDC_SPI_RD_LEN_Pos, LCDC_SPI_RD_LEN_LEN) + 1;

        if (!s->panel_dev) {
            if (!s->warned_no_panel) {
                s->warned_no_panel = true;
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: firmware is reading panel register 0x%02x "
                              "but no panel is attached; pass "
                              "-device sifli-panel\n",
                              TYPE_SIFLI_LCDC, sifli_lcdc_cmd_reg(s->cmd_word));
            }
            s->rd_data = 0;
            return;
        }

        s->rd_data = sifli_panel_read_reg(SIFLI_PANEL(s->panel_dev),
                                          sifli_lcdc_cmd_reg(s->cmd_word), len);
    }
}

/*
 * (b) The frame path.
 *
 * The controller fetches a rectangle of the layer straight out of guest
 * memory and hands it to the display surface. TL_POS/BR_POS are inclusive
 * panel coordinates and LAYER0_CONFIG.WIDTH is the source row stride in
 * bytes, not a pixel count -- LayerUpdate() shifts the byte count into that
 * field (bf0_hal_lcdc.c:1661).
 *
 * LAYER0_SRC points at the rectangle's own first pixel rather than at the
 * framebuffer origin, so the fetch address is
 * SRC + (y - y0) * stride + (x - x0) * bytes_per_pixel and TL/BR only say
 * where on the panel the rectangle lands. SetupLineIrq() relies on exactly
 * that when it programs the bus monitor to catch this layer's reads: it
 * subtracts data_area's corner first (bf0_hal_lcdc.c:1150-1165), and the
 * driver hands LayerUpdate the dirty rectangle's own first pixel
 * (drv_lcd.c:1503, :3290).
 *
 * The value in the register is not quite a CPU address: the HAL puts it
 * through HCPU_MPI_SBUS_ADDR() (bf0_hal_lcdc.c:1727), which lifts the MPI
 * CBUS window -- flash -- by 0x50000000 so the memory interface can tell an
 * external access from a CPU one. This model reads guest memory the way the
 * CPU sees it, so it undoes that with sifli_sbus_to_cpu_addr() -- the same
 * translation EPIC's layer sources need (peripherals.md §8.2). PSRAM and
 * SRAM sit outside the window and pass through untouched, which is where
 * every framebuffer in this tree lives.
 */

static uint16_t sifli_lcdc_to_rgb565(const uint8_t *p, unsigned format)
{
    if (format == LCDC_FORMAT_RGB888) {
        /*
         * RGB888 sits in memory little-endian, so the bytes come out B, G,
         * R. This is how the SDK's own example packs the format:
         * (r << 16) | (g << 8) | b, stored a byte at a time.
         */
        return ((p[2] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[0] >> 3);
    }

    return p[0] | (p[1] << 8);
}

static void sifli_lcdc_blit(SifliLcdcState *s, uint32_t src, unsigned format,
                            unsigned x0, unsigned y0, unsigned x1, unsigned y1,
                            uint32_t stride)
{
    unsigned bpp = (format == LCDC_FORMAT_RGB888) ? 3 : 2;
    unsigned dw = x1 - x0 + 1;
    unsigned row_bytes = dw * bpp;
    uint8_t *row = g_malloc(row_bytes);
    unsigned x, y;

    for (y = y0; y <= y1; y++) {
        hwaddr off = (hwaddr)(y - y0) * stride;

        if (dma_memory_read(&address_space_memory, src + off, row, row_bytes,
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: cannot read %u bytes of layer data at 0x%"
                          HWADDR_PRIx "\n", TYPE_SIFLI_LCDC, row_bytes,
                          src + off);
            break;
        }

        if (y >= s->fb_height) {
            continue;
        }

        for (x = x0; x <= x1 && x < s->fb_width; x++) {
            s->fb[y * s->fb_width + x] =
                sifli_lcdc_to_rgb565(row + (x - x0) * bpp, format);
        }
    }

    g_free(row);
}

static void sifli_lcdc_gfx_update(void *opaque);

static void sifli_lcdc_resize(SifliLcdcState *s, uint32_t w, uint32_t h)
{
    if (w == s->fb_width && h == s->fb_height) {
        return;
    }

    s->fb_width = w;
    s->fb_height = h;
    s->fb_size = (size_t)w * h * 2;
    s->fb = g_realloc(s->fb, s->fb_size);
    memset(s->fb, 0, s->fb_size);

    if (s->con) {
        /* qemu_console_resize() frees the surface this pointed at. */
        s->surface = NULL;
        qemu_console_resize(s->con, w, h);
    }
}

static void sifli_lcdc_send_frame(SifliLcdcState *s)
{
    uint32_t cfg = s->regs[LCDC_LAYER0_CONFIG];
    unsigned format = extract32(cfg, LCDC_LAYER0_FORMAT_Pos,
                                LCDC_LAYER0_FORMAT_LEN);
    uint32_t stride = extract32(cfg, LCDC_LAYER0_WIDTH_Pos,
                                LCDC_LAYER0_WIDTH_LEN);
    uint32_t src = sifli_sbus_to_cpu_addr(s->regs[LCDC_LAYER0_SRC]);
    unsigned x0 = extract32(s->regs[LCDC_LAYER0_TL_POS], LCDC_LAYER_X_Pos,
                            LCDC_LAYER_XY_LEN);
    unsigned y0 = extract32(s->regs[LCDC_LAYER0_TL_POS], LCDC_LAYER_Y_Pos,
                            LCDC_LAYER_XY_LEN);
    unsigned x1 = extract32(s->regs[LCDC_LAYER0_BR_POS], LCDC_LAYER_X_Pos,
                            LCDC_LAYER_XY_LEN);
    unsigned y1 = extract32(s->regs[LCDC_LAYER0_BR_POS], LCDC_LAYER_Y_Pos,
                            LCDC_LAYER_XY_LEN);
    unsigned bytes_per_pixel;

    if (!(cfg & LCDC_LAYER0_ACTIVE)) {
        /* Layer switched off: nothing to fetch, but the frame still ends. */
        goto done;
    }

    if (format != LCDC_FORMAT_RGB565 && format != LCDC_FORMAT_RGB888) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: source format %u is not modelled, dropping frame\n",
                      TYPE_SIFLI_LCDC, format);
        goto done;
    }

    if (x1 < x0 || y1 < y0 || x1 >= LCDC_MAX_DIM || y1 >= LCDC_MAX_DIM) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: layer rectangle %ux%u+%u+%u is not usable\n",
                      TYPE_SIFLI_LCDC, x1 - x0 + 1, y1 - y0 + 1, x0, y0);
        goto done;
    }

    if (extract32(s->regs[LCDC_LCD_CONF], LCDC_SPI_LCD_FORMAT_Pos,
                  LCDC_SPI_LCD_FORMAT_LEN) != LCDC_SPI_LCD_FORMAT_RGB565) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: panel output format is not RGB565; the drawn "
                      "colours will be wrong\n", TYPE_SIFLI_LCDC);
    }

    bytes_per_pixel = (format == LCDC_FORMAT_RGB888) ? 3 : 2;
    if (stride == 0) {
        stride = (x1 - x0 + 1) * bytes_per_pixel;
    }

    if (!s->panel_dev) {
        /*
         * Without a panel there is nothing to say how big the screen is, so
         * grow to fit whatever has been drawn. The guest error above has
         * already pointed at the missing -device.
         */
        sifli_lcdc_resize(s, MAX(s->fb_width, x1 + 1),
                          MAX(s->fb_height, y1 + 1));
    }

    if (s->fb) {
        sifli_lcdc_blit(s, src, format, x0, y0, x1, y1, stride);
    }

done:
    s->eof = true;
    sifli_lcdc_update_irq(s);
    sifli_lcdc_gfx_update(s);
}

static void sifli_lcdc_write_reg(SifliLcdcState *s, unsigned idx,
                                 uint32_t v, uint32_t field)
{
    switch (idx) {
    case LCDC_COMMAND:
        if (v & LCDC_COMMAND_RESET) {
            s->eof = false;
            sifli_lcdc_update_irq(s);
        }
        if (v & LCDC_COMMAND_START) {
            sifli_lcdc_send_frame(s);
        }
        return;
    case LCDC_IRQ:
        /*
         * Write one to clear. Either view of the EOF event clears the one
         * event behind it: the HAL clears through EOF_STAT, the synchronous
         * path through EOF_RAW.
         */
        if (v & (LCDC_IRQ_EOF_STAT | LCDC_IRQ_EOF_RAW_STAT)) {
            s->eof = false;
        }
        sifli_lcdc_update_irq(s);
        return;
    case LCDC_STATUS:
        /* Read-only. */
        return;
    case LCDC_LCD_SINGLE:
        /*
         * The trigger bits are write-only strobes; the busy bit is status and
         * is not stored, so a write here leaves nothing behind to read back.
         */
        sifli_lcdc_write_single(s, v & field);
        return;
    case LCDC_LCD_WR:
        sifli_lcdc_write_cmd(s, v & field);
        return;
    case LCDC_SPI_IF_CONF: {
        uint32_t old = s->regs[LCDC_SPI_IF_CONF];
        uint32_t new = (old & ~field) | (v & field);

        /*
         * SPI_CS_AUTO_DIS falling is the HAL bracketing a transaction
         * (HAL_LCDC_SPI_Sequence), so it is where a byte-wise command word
         * starts over.
         */
        if ((old & LCDC_SPI_CS_AUTO_DIS) && !(new & LCDC_SPI_CS_AUTO_DIS)) {
            s->cmd_acc = 0;
        }
        s->regs[LCDC_SPI_IF_CONF] = new;
        return;
    }
    case LCDC_SETTING:
        s->regs[LCDC_SETTING] = (s->regs[LCDC_SETTING] & ~field) | (v & field);
        sifli_lcdc_update_irq(s);
        return;
    default:
        s->regs[idx] = (s->regs[idx] & ~field) | (v & field);
        return;
    }
}

/*
 * Sub-word accesses are folded onto the containing 32-bit register. The HAL
 * only ever touches these as words, but a stray byte access should read
 * something sane rather than silently return zero.
 */
static uint64_t sifli_lcdc_read(void *opaque, hwaddr addr, unsigned size)
{
    SifliLcdcState *s = opaque;
    unsigned idx = addr >> 2;

    if (idx >= LCDC_NREGS) {
        return 0;
    }
    return extract32(sifli_lcdc_read_reg(s, idx), (addr & 3) * 8, size * 8);
}

static void sifli_lcdc_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    SifliLcdcState *s = opaque;
    unsigned idx = addr >> 2;
    unsigned shift = (addr & 3) * 8;
    uint32_t field, v;

    if (idx >= LCDC_NREGS) {
        return;
    }

    if (size >= 4) {
        field = ~0u;
    } else {
        field = ((1u << (size * 8)) - 1) << shift;
    }
    v = ((uint32_t)val << shift) & field;

    sifli_lcdc_write_reg(s, idx, v, field);
}

static const MemoryRegionOps sifli_lcdc_ops = {
    .read = sifli_lcdc_read,
    .write = sifli_lcdc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/*
 * Both of these are the same job: make the surface point at the shadow
 * framebuffer and push all of it. The shadow is needed because firmware
 * redraws only the rectangle that changed, so without one the screen would
 * show just the last small patch that was sent.
 */
static void sifli_lcdc_gfx_update(void *opaque)
{
    SifliLcdcState *s = opaque;

    if (!s->con || !s->fb || !s->fb_width || !s->fb_height) {
        return;
    }

    if (!s->surface) {
        s->surface = qemu_create_displaysurface_from(s->fb_width, s->fb_height,
                                                     PIXMAN_r5g6b5,
                                                     s->fb_width * 2,
                                                     (uint8_t *)s->fb);
        dpy_gfx_replace_surface(s->con, s->surface);
    }

    dpy_gfx_update_full(s->con);
}

static void sifli_lcdc_gfx_invalidate(void *opaque)
{
    sifli_lcdc_gfx_update(opaque);
}

static const GraphicHwOps sifli_lcdc_gfx_ops = {
    .invalidate = sifli_lcdc_gfx_invalidate,
    .gfx_update = sifli_lcdc_gfx_update,
};

void sifli_lcdc_set_panel(SifliLcdcState *s, DeviceState *panel)
{
    SifliPanelState *p = SIFLI_PANEL(panel);

    s->panel_dev = panel;

    /*
     * Only the panel knows how big the screen is: LCD_CONF carries formats
     * and interfaces but no resolution, and the layer's TL/BR rectangle is
     * the region being refreshed, not the panel.
     */
    sifli_lcdc_resize(s, p->width, p->height);
}

static void sifli_lcdc_reset(DeviceState *dev)
{
    SifliLcdcState *s = SIFLI_LCDC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->cmd_word = 0;
    s->cmd_acc = 0;
    s->rd_data = 0;
    s->eof = false;
    s->warned_no_panel = false;

    if (s->fb) {
        memset(s->fb, 0, s->fb_size);
    }
    qemu_set_irq(s->irq, 0);
}

static void sifli_lcdc_init(Object *obj)
{
    SifliLcdcState *s = SIFLI_LCDC(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);

    memory_region_init_io(&s->mmio, obj, &sifli_lcdc_ops, s,
                          TYPE_SIFLI_LCDC, SIFLI_LCDC_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);

    /* The panel attaches here; a bare -device sifli-panel finds it. */
    s->qspi = qbus_new(TYPE_SIFLI_QSPI_BUS, DEVICE(obj), "qspi");
}

static void sifli_lcdc_realize(DeviceState *dev, Error **errp)
{
    SifliLcdcState *s = SIFLI_LCDC(dev);

    /*
     * The console comes up empty and is sized by the panel when it arrives:
     * -device devices are created after the machine, so the panel is always
     * later than this.
     */
    s->con = graphic_console_init(dev, 0, &sifli_lcdc_gfx_ops, s);
}

static void sifli_lcdc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, sifli_lcdc_reset);
    dc->realize = sifli_lcdc_realize;
}

static const TypeInfo sifli_lcdc_info = {
    .name          = TYPE_SIFLI_LCDC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SifliLcdcState),
    .instance_init = sifli_lcdc_init,
    .class_init    = sifli_lcdc_class_init,
};

static void sifli_lcdc_register_types(void)
{
    type_register_static(&sifli_lcdc_info);
}

type_init(sifli_lcdc_register_types)
