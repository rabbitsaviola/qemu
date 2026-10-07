/*
 * Shared plumbing for the capacitive touch controllers SiFli panels carry.
 * See include/hw/input/touch-panel.h for what is here, and for the line
 * between what is shared and what each controller keeps to itself.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/visitor.h"
#include "hw/input/touch-panel.h"
#include "qom/object.h"

void touch_panel_set_int(TouchPanelState *tp, bool asserted)
{
    if (tp->int_asserted == asserted) {
        return;
    }

    tp->int_asserted = asserted;
    /* Active low: raising an interrupt drives the line down. */
    qemu_set_irq(tp->int_line, asserted ? 0 : 1);
}

void touch_panel_reset_hold(TouchPanelState *tp)
{
    tp->int_asserted = false;
    tp->x = 0;
    tp->y = 0;
    tp->down = false;
}

void touch_panel_reset_exit(TouchPanelState *tp)
{
    qemu_set_irq(tp->int_line, 1);
}

void touch_panel_realize(TouchPanelState *tp, DeviceState *dev, Error **errp)
{
    qdev_init_gpio_out(dev, &tp->int_line, 1);

    if (tp->irqchip) {
        qdev_connect_gpio_out(dev, 0,
                              qdev_get_gpio_in(tp->irqchip, tp->irq_pin));
    }
}

/*
 * Injecting a coordinate while the touch is already down is a move, and a
 * move is reported the same way a press is: the SDK drivers map both to
 * TOUCH_EVENT_DOWN.
 */
static void touch_panel_set_x(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    TouchPanelState *tp = opaque;
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }

    tp->x = value;
    if (tp->down) {
        tp->report(tp->opaque);
    }
}

static void touch_panel_set_y(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    TouchPanelState *tp = opaque;
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }

    tp->y = value;
    if (tp->down) {
        tp->report(tp->opaque);
    }
}

static void touch_panel_set_down(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    TouchPanelState *tp = opaque;
    bool value;

    if (!visit_type_bool(v, name, &value, errp)) {
        return;
    }

    tp->down = value;
    tp->report(tp->opaque);
}

void touch_panel_init(TouchPanelState *tp, Object *obj,
                      void (*report)(void *opaque), void *opaque)
{
    tp->report = report;
    tp->opaque = opaque;

    /*
     * Injection interface. These have side effects, so they are plain
     * properties rather than DEFINE_PROP: writing one republishes the
     * register file and raises the interrupt line.
     */
    object_property_add(obj, "touch-x", "uint32",
                        NULL, touch_panel_set_x, NULL, tp);
    object_property_set_description(obj, "touch-x",
        "X of the touch to inject, in panel coordinates");
    object_property_add(obj, "touch-y", "uint32",
                        NULL, touch_panel_set_y, NULL, tp);
    object_property_set_description(obj, "touch-y",
        "Y of the touch to inject, in panel coordinates");
    object_property_add(obj, "touch-down", "bool",
                        NULL, touch_panel_set_down, NULL, tp);
    object_property_set_description(obj, "touch-down",
        "Whether the injected touch is a press (true) or a release (false)");
}
