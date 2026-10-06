/*
 * FocalTech FT6146 capacitive touch controller
 *
 * The chip the a128r16 board's panel carries, as an I2C slave at 7-bit
 * address 0x38. It answers the register reads the SDK driver makes and drives
 * one interrupt line low while a touch is waiting to be read.
 *
 * The register map below is only as large as the driver uses; see
 * customer/peripherals/touch_panel/ft6146/ft6146.c in the SDK.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_I2C_SIFLI_FT6146_H
#define HW_I2C_SIFLI_FT6146_H

#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "qom/object.h"

#define TYPE_SIFLI_FT6146 "sifli-ft6146"
OBJECT_DECLARE_SIMPLE_TYPE(SifliFt6146State, SIFLI_FT6146)

/* 7-bit address. The driver calls it FT_DEV_ADDR. */
#define SIFLI_FT6146_ADDR           0x38

/* Registers the driver names; the rest of the space is answered with zero. */
#define SIFLI_FT6146_TD_STATUS      0x02    /* number of points, low nibble */
#define SIFLI_FT6146_P1_XH          0x03    /* event flag, then x[11:8] */
#define SIFLI_FT6146_P1_XL          0x04
#define SIFLI_FT6146_P1_YH          0x05    /* point id, then y[11:8] */
#define SIFLI_FT6146_P1_YL          0x06
#define SIFLI_FT6146_ID_L           0x9f
#define SIFLI_FT6146_ID_H           0xa3

/* P1_XH bits 7:6. The SDK driver calls these ctp_pen_state_enum. */
#define SIFLI_FT6146_EVENT_DOWN     0
#define SIFLI_FT6146_EVENT_UP       1
#define SIFLI_FT6146_EVENT_MOVE     2

/*
 * Coordinate range of the panel this controller is bonded to, the driver's
 * FT_MAX_WIDTH/FT_MAX_HEIGHT. Nothing in the model uses them; they are here
 * so a reader can tell what a coordinate means.
 *
 * The driver's mirroring helper, ft6146_correct_pos(), would turn x into
 * 390 - x and y into 450 - y -- but this SDK never calls it (it is defined
 * and left unreferenced), so coordinates reach the application exactly as
 * the controller reports them. A test injecting a point should expect that
 * point back, not its mirror.
 */
#define SIFLI_FT6146_MAX_X          390
#define SIFLI_FT6146_MAX_Y          450

#define SIFLI_FT6146_NREGS          0x100

struct SifliFt6146State {
    I2CSlave parent_obj;

    uint8_t regs[SIFLI_FT6146_NREGS];

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

    /* The interrupt line, active low. */
    qemu_irq int_line;
    bool int_asserted;

    /* Last injected touch. Written through the touch-* properties. */
    uint32_t touch_x;
    uint32_t touch_y;
    bool touch_down;

    /* ID bytes, so an override can be given without editing the model. */
    uint32_t id_h;
    uint32_t id_l;
};

#endif /* HW_I2C_SIFLI_FT6146_H */
