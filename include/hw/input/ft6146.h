/*
 * FocalTech FT6146 capacitive touch controller
 *
 * The touch controller several of the SiFli panels carry, as an I2C slave at
 * 7-bit address 0x38 by default. It answers the register reads the SDK driver
 * makes and drives one interrupt line low while a touch is waiting to be
 * read.
 *
 * Nothing here is SoC-specific: the device is an ordinary I2C slave, so any
 * machine can hang it off a bus with -device, and the address is whichever
 * the "address" property says. Which bus it goes on and where its interrupt
 * line is wired are the board's business, not the chip's, and both are
 * therefore properties:
 *
 *   -device ft6146,bus=i2c2,irqchip=gpio1,irq-pin=31
 *
 * QEMU's command line cannot connect a GPIO line itself, so the device does
 * it: "irqchip" names the controller its interrupt line reaches and "irq-pin"
 * which line of it. With no irqchip the interrupt goes nowhere, which is what
 * a board that leaves the pin unrouted wants.
 *
 * None of that is particular to this part, and neither is taking an injected
 * touch or holding the line low until it is read. It is shared with the SDK's
 * other touch controllers and lives in hw/input/touch-panel.h; what is here
 * is the register map and the release rule.
 *
 * The register map below is only as large as the driver uses; see
 * customer/peripherals/touch_panel/ft6146/ft6146.c in the SDK.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_INPUT_FT6146_H
#define HW_INPUT_FT6146_H

#include "hw/i2c/i2c.h"
#include "hw/input/touch-panel.h"
#include "qom/object.h"

#define TYPE_FT6146 "ft6146"
OBJECT_DECLARE_SIMPLE_TYPE(Ft6146State, FT6146)

/* 7-bit address. The driver calls it FT_DEV_ADDR. */
#define FT6146_ADDR           0x38

/* Registers the driver names; the rest of the space is answered with zero. */
#define FT6146_TD_STATUS      0x02    /* number of points, low nibble */
#define FT6146_P1_XH          0x03    /* event flag, then x[11:8] */
#define FT6146_P1_XL          0x04
#define FT6146_P1_YH          0x05    /* point id, then y[11:8] */
#define FT6146_P1_YL          0x06
#define FT6146_ID_L           0x9f
#define FT6146_ID_H           0xa3

/* P1_XH bits 7:6. The SDK driver calls these ctp_pen_state_enum. */
#define FT6146_EVENT_DOWN     0
#define FT6146_EVENT_UP       1
#define FT6146_EVENT_MOVE     2

/*
 * Coordinate range of the panel this controller is bonded to, the driver's
 * FT_MAX_WIDTH/FT_MAX_HEIGHT. These are the defaults for the max-x and max-y
 * properties: the model uses them to scale the window's pointer onto the
 * panel, so a board with a differently sized panel overrides them rather
 * than editing this.
 *
 * The driver's mirroring helper, ft6146_correct_pos(), would turn x into
 * 390 - x and y into 450 - y -- but this SDK never calls it (it is defined
 * and left unreferenced), so coordinates reach the application exactly as
 * the controller reports them. A test injecting a point should expect that
 * point back, not its mirror.
 */
#define FT6146_MAX_X          390
#define FT6146_MAX_Y          450

#define FT6146_NREGS          0x100

struct Ft6146State {
    I2CSlave parent_obj;

    /*
     * Everything shared with the SDK's other touch controllers: the injected
     * touch, the interrupt line, and the board wiring that says where that
     * line goes. See hw/input/touch-panel.h.
     */
    TouchPanelState tp;

    uint8_t regs[FT6146_NREGS];

    /* Where the next byte read comes from. */
    uint8_t ptr;

    /*
     * Set between the address byte of a write and the end of that transfer:
     * the first byte written is the register pointer and the rest is data.
     *
     * A read transfer deliberately does not set this. The driver points at a
     * register in one transfer and reads it in the next, and the pointer has
     * to survive the stop in between.
     */
    bool addr_phase;

    /* True once a read has covered TD_STATUS, which is what ends a touch. */
    bool saw_status;

    /* ID bytes, so an override can be given without editing the model. */
    uint32_t id_h;
    uint32_t id_l;
};

#endif /* HW_INPUT_FT6146_H */
