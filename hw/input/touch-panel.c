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
    /*
     * The button is cleared too, though the front-end may still have it
     * held: after a reset the controller has forgotten the touch, and a
     * pointer that never left the window is not a finger arriving. The
     * position goes with it -- a press flushed against a remembered
     * coordinate would report (0,0) as if that were where it landed.
     */
    tp->ui_pressed = false;
    tp->ui_have_pos = false;
}

void touch_panel_reset_exit(TouchPanelState *tp)
{
    qemu_set_irq(tp->int_line, 1);
}

/*
 * The front-end that lets the display's own pointer drive the panel, so a
 * touch can be made by clicking the SDL window instead of by a QMP script.
 *
 * Absolute rather than relative, because a panel is: a finger is somewhere,
 * it does not move by so many pixels. Registering as absolute also puts the
 * display into absolute mouse mode, so the host cursor is not grabbed and a
 * click lands where it was aimed.
 */
static void touch_panel_ui_event(DeviceState *dev, QemuConsole *src,
                                 InputEvent *evt)
{
    /*
     * qemu_input_handler_register() was handed this struct rather than a
     * device, because the handler needs somewhere to keep the press state
     * and QemuInputHandler has no opaque field of its own. hw/input/hid.c
     * reaches its state the same way.
     */
    TouchPanelState *tp = (TouchPanelState *)dev;

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS: {
        InputMoveEvent *move = evt->u.abs.data;
        bool x_axis = move->axis == INPUT_AXIS_X;
        /* The UI layer works in 0..0x7FFF; a panel is max_x pixels wide. */
        uint32_t value = qemu_input_scale_axis(move->value,
                                               INPUT_EVENT_ABS_MIN,
                                               INPUT_EVENT_ABS_MAX, 0,
                                               x_axis ? tp->max_x - 1
                                                      : tp->max_y - 1);

        if (x_axis) {
            tp->x = value;
        } else {
            tp->y = value;
        }
        tp->ui_have_pos = true;

        if (tp->ui_pressed) {
            tp->down = true;
            tp->report(tp->opaque);
        }
        break;
    }
    case INPUT_EVENT_KIND_BTN: {
        InputBtnEvent *btn = evt->u.btn.data;

        if (btn->button != INPUT_BUTTON_LEFT) {
            break;
        }

        if (btn->down) {
            /*
             * The display sends the button before the position, so
             * publishing here would report wherever the last click left
             * off. Wait for the coordinates that follow.
             */
            tp->ui_pressed = true;
        } else if (tp->ui_pressed) {
            tp->ui_pressed = false;
            tp->down = false;
            tp->report(tp->opaque);
        }
        /*
         * A release with no press behind it is dropped. A display sends one
         * when it loses the pointer without the button ever having been
         * ours -- and after a reset, which forgot the press, the release
         * that follows is not a release of anything.
         */
        break;
    }
    default:
        break;
    }
}

/*
 * The front-end has finished a batch of events. Reached only when something
 * was delivered to us, and only after all of it was.
 *
 * A press that never got coordinates is flushed here. SDL sends the button
 * and then the position, so by now it has been published and this does
 * nothing; GTK sends the button and no position at all, taking it from
 * pointer motion instead, so without this a click that does not move
 * publishes nothing and the window looks dead. Flushing reports wherever the
 * pointer last was, which is where the click landed.
 */
static void touch_panel_ui_sync(DeviceState *dev)
{
    TouchPanelState *tp = (TouchPanelState *)dev;

    if (tp->ui_pressed && !tp->down && tp->ui_have_pos) {
        tp->down = true;
        tp->report(tp->opaque);
    }
}

static const QemuInputHandler touch_panel_ui_handler = {
    .name  = "touch panel",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = touch_panel_ui_event,
    .sync  = touch_panel_ui_sync,
};

void touch_panel_realize(TouchPanelState *tp, DeviceState *dev, Error **errp)
{
    qdev_init_gpio_out(dev, &tp->int_line, 1);

    if (tp->irqchip) {
        qdev_connect_gpio_out(dev, 0,
                              qdev_get_gpio_in(tp->irqchip, tp->irq_pin));
    }

    /*
     * Registering here rather than in touch_panel_init() is deliberate:
     * qmp_device_list_properties() instantiates a device and throws it away
     * again, and a handler registered then would be left pointing at freed
     * memory.
     */
    if (tp->max_x && tp->max_y) {
        tp->input = qemu_input_handler_register((DeviceState *)tp,
                                                &touch_panel_ui_handler);
    }
}

void touch_panel_unrealize(TouchPanelState *tp)
{
    if (tp->input) {
        qemu_input_handler_unregister(tp->input);
        tp->input = NULL;
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
