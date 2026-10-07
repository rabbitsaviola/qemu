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
 * The rest of what a touch controller has to do -- taking an injected touch,
 * holding one interrupt line low until the host has read the data, and the
 * board wiring that says where that line goes -- is shared with the SDK's
 * other controllers and lives in hw/input/touch-panel.h. What is left here is
 * this chip's register map and this chip's rule for when the line drops.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/input/ft6146.h"
#include "hw/input/touch-panel.h"
#include "hw/qdev-properties.h"
#include "qemu/log.h"
#include "qemu/module.h"

/*
 * Publish the injected touch into the register file and raise the interrupt.
 * Host writes are not locked out: the driver programs a couple of registers
 * of its own, and letting it overwrite one of these is a more faithful model
 * than not.
 */
static void ft6146_report(void *opaque)
{
    Ft6146State *s = opaque;
    unsigned x = s->tp.x & 0xfff;
    unsigned y = s->tp.y & 0xfff;
    unsigned event = s->tp.down ? FT6146_EVENT_DOWN : FT6146_EVENT_UP;

    s->regs[FT6146_TD_STATUS] = s->tp.down ? 1 : 0;
    s->regs[FT6146_P1_XH] = (event << 6) | ((x >> 8) & 0x0f);
    s->regs[FT6146_P1_XL] = x & 0xff;
    /* Bits 7:4 of P1_YH are the point id, and this controller reports one. */
    s->regs[FT6146_P1_YH] = (y >> 8) & 0x0f;
    s->regs[FT6146_P1_YL] = y & 0xff;

    touch_panel_set_int(&s->tp, true);
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
        /*
         * This chip's rule for releasing the line: a read that walked over
         * the status register. A read of anything else does not do it, and
         * neither does the stop on its own.
         */
        if (s->saw_status) {
            touch_panel_set_int(&s->tp, false);
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

static void ft6146_reset_hold(Object *obj, ResetType type)
{
    Ft6146State *s = FT6146(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[FT6146_ID_H] = s->id_h;
    s->regs[FT6146_ID_L] = s->id_l;

    s->ptr = 0;
    s->addr_phase = false;
    s->saw_status = false;
    touch_panel_reset_hold(&s->tp);
}

static void ft6146_reset_exit(Object *obj, ResetType type)
{
    Ft6146State *s = FT6146(obj);

    touch_panel_reset_exit(&s->tp);
}

static void ft6146_realize(DeviceState *dev, Error **errp)
{
    Ft6146State *s = FT6146(dev);

    touch_panel_realize(&s->tp, dev, errp);
}

static void ft6146_unrealize(DeviceState *dev)
{
    Ft6146State *s = FT6146(dev);

    touch_panel_unrealize(&s->tp);
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

    touch_panel_init(&s->tp, obj, ft6146_report, s);
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
     * that leaves the pin unrouted sets neither. See touch-panel.h.
     */
    DEFINE_PROP_LINK("irqchip", Ft6146State, tp.irqchip, TYPE_DEVICE,
                     DeviceState *),
    DEFINE_PROP_UINT32("irq-pin", Ft6146State, tp.irq_pin, 0),

    /*
     * The panel this controller is bonded to, the driver's FT_MAX_WIDTH and
     * FT_MAX_HEIGHT. The defaults are the ones ft6146.c uses in the SDK, but
     * the size is the board's to state, because the same part turns up
     * behind differently sized panels. Stating it is also what turns on the
     * UI front-end, so the window's pointer can drive the panel.
     */
    DEFINE_PROP_UINT32("max-x", Ft6146State, tp.max_x, FT6146_MAX_X),
    DEFINE_PROP_UINT32("max-y", Ft6146State, tp.max_y, FT6146_MAX_Y),
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
    dc->unrealize = ft6146_unrealize;
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
