/*
 * FocalTech FT6146 capacitive touch controller
 *
 * An I2C slave with a register pointer, in the shape every one of these
 * controllers has: a write sets the register to talk to, and a read returns
 * registers from there on. The SDK driver does it as two transfers with a
 * stop in between -- point at TD_STATUS, then read fourteen bytes from it --
 * so the pointer has to outlive the transfer that set it. That is the one
 * thing here worth getting right, and it is why the address phase is entered
 * on a write and not on a read.
 *
 * The second half is the interrupt line. A touch raises it, and reading the
 * status register drops it again, which is what a real controller does: the
 * line stays low until the host has taken the data, so the next touch is a
 * fresh falling edge for the board's GPIO bank to latch.
 *
 * Touches are injected from outside -- see the touch-* properties. Nothing
 * here invents one on its own.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/visitor.h"
#include "hw/input/ft6146.h"
#include "hw/qdev-properties.h"
#include "qemu/log.h"
#include "qemu/module.h"

static void ft6146_set_int(Ft6146State *s, bool asserted)
{
    if (s->int_asserted == asserted) {
        return;
    }

    s->int_asserted = asserted;
    /* Active low: raising an interrupt drives the line down. */
    qemu_set_irq(s->int_line, asserted ? 0 : 1);
}

/*
 * Publish the injected touch into the register file and raise the interrupt.
 * Host writes are not locked out: the driver programs a couple of registers
 * of its own, and letting it overwrite one of these is a more faithful model
 * than not.
 */
static void ft6146_report(Ft6146State *s)
{
    unsigned x = s->touch_x & 0xfff;
    unsigned y = s->touch_y & 0xfff;
    unsigned event = s->touch_down ? FT6146_EVENT_DOWN
                                   : FT6146_EVENT_UP;

    s->regs[FT6146_TD_STATUS] = s->touch_down ? 1 : 0;
    s->regs[FT6146_P1_XH] = (event << 6) | ((x >> 8) & 0x0f);
    s->regs[FT6146_P1_XL] = x & 0xff;
    /* Bits 7:4 of P1_YH are the point id, and this controller reports one. */
    s->regs[FT6146_P1_YH] = (y >> 8) & 0x0f;
    s->regs[FT6146_P1_YL] = y & 0xff;

    ft6146_set_int(s, true);
}

static int ft6146_event(I2CSlave *i2c, enum i2c_event event)
{
    Ft6146State *s = FT6146(i2c);

    switch (event) {
    case I2C_START_SEND:
    case I2C_START_SEND_ASYNC:
        /* A write opens with the register pointer. */
        s->addr_phase = true;
        break;
    case I2C_START_RECV:
        s->saw_status = false;
        break;
    case I2C_FINISH:
        s->addr_phase = false;
        if (s->saw_status) {
            ft6146_set_int(s, false);
        }
        break;
    case I2C_NACK:
        break;
    }

    return 0;
}

static int ft6146_send(I2CSlave *i2c, uint8_t data)
{
    Ft6146State *s = FT6146(i2c);

    if (s->addr_phase) {
        s->ptr = data;
        s->addr_phase = false;
    } else {
        s->regs[s->ptr] = data;
        s->ptr++;
    }

    return 0;
}

static uint8_t ft6146_recv(I2CSlave *i2c)
{
    Ft6146State *s = FT6146(i2c);
    uint8_t val = s->regs[s->ptr];

    if (s->ptr == FT6146_TD_STATUS) {
        s->saw_status = true;
    }
    s->ptr++;

    return val;
}

/*
 * Injecting a coordinate while the touch is already down is a move, and a
 * move is reported the same way a press is: the driver maps both to
 * TOUCH_EVENT_DOWN.
 */
static void ft6146_set_touch_x(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    Ft6146State *s = FT6146(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }

    s->touch_x = value;
    if (s->touch_down) {
        ft6146_report(s);
    }
}

static void ft6146_set_touch_y(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    Ft6146State *s = FT6146(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }

    s->touch_y = value;
    if (s->touch_down) {
        ft6146_report(s);
    }
}

static void ft6146_set_touch_down(Object *obj, Visitor *v,
                                  const char *name, void *opaque,
                                  Error **errp)
{
    Ft6146State *s = FT6146(obj);
    bool value;

    if (!visit_type_bool(v, name, &value, errp)) {
        return;
    }

    s->touch_down = value;
    ft6146_report(s);
}

static void ft6146_reset_hold(Object *obj, ResetType type)
{
    Ft6146State *s = FT6146(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[FT6146_ID_H] = s->id_h;
    s->regs[FT6146_ID_L] = s->id_l;

    s->ptr = 0;
    s->addr_phase = false;
    s->saw_status = false;
    s->touch_x = 0;
    s->touch_y = 0;
    s->touch_down = false;
    s->int_asserted = false;
}

/*
 * Drive the interrupt line to its idle level once the whole machine has been
 * reset, rather than leaving it where it was found.
 *
 * QEMU's output lines start at zero and the board connects this one after
 * realize has run, so a drive from realize would go nowhere; and reset is not
 * a safe place either, because the board's GPIO bank resets its input levels
 * in its own hold phase and the two devices' hold order is not defined. Doing
 * it in reset exit is what makes the idle level survive regardless of order,
 * and without it the line would already be low, making the first touch -- a
 * drive low -- a 0-to-0 non-event with no edge for the bank to latch.
 */
static void ft6146_reset_exit(Object *obj, ResetType type)
{
    Ft6146State *s = FT6146(obj);

    qemu_set_irq(s->int_line, 1);
}

static void ft6146_realize(DeviceState *dev, Error **errp)
{
    Ft6146State *s = FT6146(dev);

    qdev_init_gpio_out(dev, &s->int_line, 1);

    /*
     * Take the interrupt line to the pin the board routed it to. The command
     * line can pick the bus but cannot join two devices' GPIO lines, so the
     * chip is told where its line goes and makes the connection itself, the
     * same way riscv-iommu-sys does with its irqchip.
     */
    if (s->irqchip) {
        qdev_connect_gpio_out(dev, 0,
                              qdev_get_gpio_in(s->irqchip, s->irq_pin));
    }
}

static void ft6146_instance_init(Object *obj)
{
    Ft6146State *s = FT6146(obj);

    /*
     * The address the chip straps to. I2CSlave already carries an "address"
     * property, so this is a default rather than a property of its own, and
     * "-device ft6146,address=..." still overrides it.
     */
    s->parent_obj.address = FT6146_ADDR;

    /*
     * Injection interface. These have side effects, so they are plain
     * properties rather than DEFINE_PROP: writing one republishes the
     * register file and raises the interrupt line.
     */
    object_property_add(obj, "touch-x", "uint32",
                        NULL, ft6146_set_touch_x, NULL, NULL);
    object_property_set_description(obj, "touch-x",
        "X of the touch to inject, in panel coordinates (0..389)");
    object_property_add(obj, "touch-y", "uint32",
                        NULL, ft6146_set_touch_y, NULL, NULL);
    object_property_set_description(obj, "touch-y",
        "Y of the touch to inject, in panel coordinates (0..449)");
    object_property_add(obj, "touch-down", "bool",
                        NULL, ft6146_set_touch_down, NULL, NULL);
    object_property_set_description(obj, "touch-down",
        "Whether the injected touch is a press (true) or a release (false)");
}

static const Property ft6146_properties[] = {
    /*
     * The driver prints both of these and checks neither, so the defaults
     * are zero rather than a guess at the part's real ID bytes. Set them if
     * a firmware ever does compare.
     */
    DEFINE_PROP_UINT32("id-h", Ft6146State, id_h, 0),
    DEFINE_PROP_UINT32("id-l", Ft6146State, id_l, 0),

    /*
     * Where the interrupt line goes: which GPIO controller, and which pin of
     * it. Both are board wiring, so both are the board's to state; a board
     * that leaves the pin unrouted sets neither.
     */
    DEFINE_PROP_LINK("irqchip", Ft6146State, irqchip, TYPE_DEVICE,
                     DeviceState *),
    DEFINE_PROP_UINT32("irq-pin", Ft6146State, irq_pin, 0),
};

static void ft6146_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    device_class_set_props(dc, ft6146_properties);
    rc->phases.hold = ft6146_reset_hold;
    rc->phases.exit = ft6146_reset_exit;
    dc->realize = ft6146_realize;
    dc->desc = "FocalTech FT6146 touch controller";

    sc->event = ft6146_event;
    sc->send = ft6146_send;
    sc->recv = ft6146_recv;
}

static const TypeInfo ft6146_types[] = {
    {
        .name          = TYPE_FT6146,
        .parent        = TYPE_I2C_SLAVE,
        .instance_size = sizeof(Ft6146State),
        .instance_init = ft6146_instance_init,
        .class_init    = ft6146_class_init,
    },
};

DEFINE_TYPES(ft6146_types)
