/*
 * Shared plumbing for the capacitive touch controllers SiFli panels carry.
 *
 * The SDK ships twenty of these -- FT3168, FT5446U, CST816, GT911 and the
 * rest -- and no two share a register map. What they do share is only this: a
 * touch is injected from outside, the chip publishes it into whatever
 * registers it keeps its coordinates in, and it pulls one interrupt line low
 * until the host has taken the data. That much lives here, so a controller
 * model is its register map and little else.
 *
 * This is a struct a controller embeds, not a QOM class, for the same reason
 * hw/input/hid.c is: the controllers do not all sit on the same bus. The
 * SDK's ADS7846 is an SPI part, and a base class under TYPE_I2C_SLAVE would
 * have nothing to say about it.
 *
 * The line between here and the chip is drawn on purpose. This file owns the
 * line's *level*: active low, driven to idle once the machine has settled,
 * and only ever driven on a real change, so the board's edge-triggered GPIO
 * bank gets one edge per touch rather than a repeat for every register the
 * host reads. It does not own *when* the line drops -- that is chip policy
 * and it differs. FT6146 releases it when a read walks over TD_STATUS; GT911
 * releases it when the host writes 0x814e; a chip that wants something else
 * is free to.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_INPUT_TOUCH_PANEL_H
#define HW_INPUT_TOUCH_PANEL_H

#include "hw/irq.h"
#include "hw/qdev-core.h"

typedef struct TouchPanelState {
    /*
     * The interrupt line, active low, and its last driven level so a repeat
     * is not mistaken for a new touch.
     */
    qemu_irq int_line;
    bool int_asserted;

    /*
     * Where that line goes. Both are board wiring, so both are properties
     * rather than constants -- see DEFINE_PROP_TOUCH_PANEL below.
     */
    DeviceState *irqchip;
    uint32_t irq_pin;

    /*
     * The injected touch, in panel coordinates. Written through the touch-x,
     * touch-y and touch-down properties touch_panel_init() installs.
     */
    uint32_t x;
    uint32_t y;
    bool down;

    /*
     * How this controller publishes a touch into its own registers, called
     * with opaque whenever x, y or down changes. Raising the line is the
     * chip's call, made from there with touch_panel_set_int(), because only
     * the chip knows what it is reporting and when it is done reporting it.
     */
    void (*report)(void *opaque);
    void *opaque;
} TouchPanelState;

/*
 * Every controller declares these two properties in its own property list,
 * over the TouchPanelState it embeds:
 *
 *   DEFINE_PROP_LINK("irqchip", Ft6146State, tp.irqchip, TYPE_DEVICE,
 *                    DeviceState *),
 *   DEFINE_PROP_UINT32("irq-pin", Ft6146State, tp.irq_pin, 0),
 *
 * They are spelled out rather than hidden behind a macro because every
 * DEFINE_PROP_* in QEMU expands to exactly one property (see
 * include/hw/qdev-properties.h), and a macro that expanded to two would be
 * the only one of its kind.
 */

/*
 * Bring the panel up on a controller: remember how to publish a touch, and
 * install the touch-x, touch-y and touch-down properties on obj.
 *
 * Those three report asymmetrically, which is what a caller wants. A
 * coordinate publishes only while a touch is down; touch-down publishes
 * either way. So a press needs x and y written before it, and a release
 * needs nothing else written at all.
 */
void touch_panel_init(TouchPanelState *tp, Object *obj,
                      void (*report)(void *opaque), void *opaque);

/*
 * Claim the interrupt output line and take it to irqchip/irq-pin. QEMU's
 * command line cannot join two devices' GPIO lines, so the chip is told where
 * its line goes and makes the connection itself, the same way
 * riscv-iommu-sys does with its irqchip.
 */
void touch_panel_realize(TouchPanelState *tp, DeviceState *dev, Error **errp);

/*
 * Reset is split the way QEMU splits it generally. Hold forgets the touch and
 * the line state; exit drives the line to its idle level, once the whole
 * machine has settled.
 *
 * The exit half is not optional. QEMU's output lines start at zero, and the
 * board connects this one only after realize has run, so a drive from realize
 * would go nowhere; reset is no safer, because the board's GPIO bank resets
 * its input levels in its own hold phase and the two devices' hold order is
 * not defined. Without it the line would already be low, and the first touch
 * -- a drive low -- would be a 0-to-0 non-event with no edge to latch.
 */
void touch_panel_reset_hold(TouchPanelState *tp);
void touch_panel_reset_exit(TouchPanelState *tp);

/*
 * Drive the line. Zero is asserted, because it is active low. A repeat of the
 * current level does nothing, so a chip that republishes on every register
 * write does not hand the bank a second edge for one touch.
 */
void touch_panel_set_int(TouchPanelState *tp, bool asserted);

#endif /* HW_INPUT_TOUCH_PANEL_H */
