/*
 * SiFli LCD panel
 *
 * A board's LCD module is a separate part from the SoC, and different boards
 * fit different ones. The only thing firmware asks a panel is "which one are
 * you": the driver reads a register over the LCDC's QSPI interface and
 * compares it against the ID it was built for. So the panel is modelled as a
 * device that answers that one question, and everything else about it --
 * resolution, the initialisation sequence, the GRAM -- either belongs to the
 * controller or is not observable.
 *
 * The modules differ only in what they answer, so which one this is comes in
 * as properties. The defaults are the a128r16 board's CO5300 module:
 *
 *   -device sifli-lcd-panel
 *   -device sifli-lcd-panel,id=0x60834200,width=480,height=272
 *
 * A panel attaches to the QSPI bus the LCDC publishes, so plain
 * "-device sifli-lcd-panel" finds it without a bus= argument.
 *
 * The name says what it is rather than which chip it is, so that it reads as
 * the other half of the LCDC and not as the touch controller's panel.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_SIFLI_LCD_PANEL_H
#define HW_DISPLAY_SIFLI_LCD_PANEL_H

#include "hw/qdev-core.h"
#include "qom/object.h"

#define TYPE_SIFLI_LCD_PANEL "sifli-lcd-panel"
OBJECT_DECLARE_SIMPLE_TYPE(SifliLcdPanelState, SIFLI_LCD_PANEL)

/*
 * The bus the panel hangs on. It carries no protocol: the controller and the
 * panel find each other through it and talk directly, because there is no
 * waveform to model between them.
 */
#define TYPE_SIFLI_QSPI_BUS "sifli-qspi-bus"
OBJECT_DECLARE_SIMPLE_TYPE(SifliQspiBus, SIFLI_QSPI_BUS)

struct SifliQspiBus {
    BusState qbus;
};

struct SifliLcdPanelState {
    DeviceState parent_obj;

    /*
     * The ID the panel reports. This is the value the driver's ReadID() is
     * expected to end up with, i.e. the bytes it reads assembled back into a
     * word -- not the value the module's datasheet prints.
     */
    uint32_t id;
    uint32_t id_reg;
    uint32_t width;
    uint32_t height;
};

/*
 * Answer a register read from the controller. Returns the word the panel puts
 * on the bus; the controller decides how many of its bytes the driver asked
 * for.
 */
uint32_t sifli_lcd_panel_read_reg(SifliLcdPanelState *s, unsigned reg,
                                  unsigned len);

#endif /* HW_DISPLAY_SIFLI_LCD_PANEL_H */
