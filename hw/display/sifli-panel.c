/*
 * SiFli QSPI panel
 *
 * Thin semantics, and thinner than usual: the panel answers the controller's
 * register reads and does nothing else. Writing to it -- the module's long
 * initialisation sequence, the page unlocks, the memory-write commands --
 * produces no reply and needs none, so the writes are simply dropped by the
 * controller and never reach here.
 *
 * Only the ID register is modelled. A read of anything else is reported and
 * answered with zero rather than silently, because the one thing worth seeing
 * when a new panel is tried is which register its driver asked for.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/display/sifli-lcdc.h"
#include "hw/display/sifli-panel.h"
#include "hw/qdev-properties.h"
#include "qemu/log.h"
#include "qemu/module.h"

uint32_t sifli_panel_read_reg(SifliPanelState *s, unsigned reg, unsigned len)
{
    if (reg == s->id_reg) {
        return s->id;
    }

    /*
     * Either the driver is reading a register this model does not know, or
     * the driver does not match the panel. Both are worth a line in the log:
     * a mismatched panel shows up as a blank screen and nothing else.
     */
    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: read of register 0x%02x (%u bytes) is not modelled, "
                  "returning 0\n", TYPE_SIFLI_PANEL, reg, len);
    return 0;
}

static void sifli_panel_realize(DeviceState *dev, Error **errp)
{
    SifliPanelState *s = SIFLI_PANEL(dev);
    BusState *bus = qdev_get_parent_bus(dev);

    if (!bus || !object_dynamic_cast(OBJECT(bus->parent), TYPE_SIFLI_LCDC)) {
        error_setg(errp, "sifli-panel needs a sifli-lcdc's QSPI bus");
        return;
    }

    if (s->width == 0 || s->height == 0) {
        error_setg(errp, "sifli-panel needs a non-zero width and height");
        return;
    }

    /*
     * Hand ourselves to the controller. It cannot have found us on its own:
     * -device devices are created after the machine has been initialised, so
     * every panel realizes after the LCDC it sits on. This sizes the console,
     * so it has to come after the size check above.
     */
    sifli_lcdc_set_panel(SIFLI_LCDC(bus->parent), dev);
}

static const Property sifli_panel_properties[] = {
    /*
     * Defaults are the a128r16 board's CO5300 module, so a bare
     * "-device sifli-panel" is right for the board the firmware test uses.
     */
    DEFINE_PROP_UINT32("id", SifliPanelState, id, 0x331100),
    DEFINE_PROP_UINT32("id-reg", SifliPanelState, id_reg, 0x04),
    DEFINE_PROP_UINT32("width", SifliPanelState, width, 390),
    DEFINE_PROP_UINT32("height", SifliPanelState, height, 450),
};

static void sifli_panel_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, sifli_panel_properties);
    dc->realize = sifli_panel_realize;
    /*
     * Lets "-device sifli-panel" find the LCDC's bus by itself
     * (qdev_find_default_bus in system/qdev-monitor.c).
     */
    dc->bus_type = TYPE_SIFLI_QSPI_BUS;
}

static const TypeInfo sifli_panel_info = {
    .name          = TYPE_SIFLI_PANEL,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(SifliPanelState),
    .class_init    = sifli_panel_class_init,
};

static const TypeInfo sifli_qspi_bus_info = {
    .name          = TYPE_SIFLI_QSPI_BUS,
    .parent        = TYPE_BUS,
    .instance_size = sizeof(SifliQspiBus),
};

static void sifli_panel_register_types(void)
{
    type_register_static(&sifli_qspi_bus_info);
    type_register_static(&sifli_panel_info);
}

type_init(sifli_panel_register_types)
