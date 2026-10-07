/*
 * SiFli SF32LB52x I2C controller
 *
 * A "register shell + thin semantics" model in the same spirit as the other
 * SiFli devices: the register layout is the SDK's, but nothing is timed. A
 * transfer runs to completion inside the write that asks for it, so the
 * status bits the HAL polls are already in their final state by the time it
 * looks at them.
 *
 * The HAL drives this block from its interrupt handler, not by polling: the
 * board's I2Cx_IRQHandler calls handle->XferISR, which is I2C_Master_ISR_IT.
 * That handler reads SR, writes it straight back to clear it (SR is
 * write-1-to-clear), and then issues the next TCR command. Modelling the
 * block therefore means modelling that handshake -- see sifli_i2c_do_tcr().
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/sifli-i2c.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

static void sifli_i2c_update_irq(SifliI2CState *s)
{
    /*
     * SR and IER put each status bit and its enable at the same position, so
     * "an enabled status bit is set" is just the AND of the two registers.
     * The line is level-triggered: the HAL's handler clears SR by writing it
     * back, which is what drops it again.
     */
    qemu_set_irq(s->irq,
                 (s->regs[SIFLI_I2C_SR] & s->regs[SIFLI_I2C_IER]) != 0);
}

/*
 * Run one TCR command. The HAL writes this register outright (never
 * read-modify-write), so every bit is a strobe rather than stored state.
 */
static void sifli_i2c_do_tcr(SifliI2CState *s, uint32_t cmd)
{
    uint32_t sr = s->regs[SIFLI_I2C_SR];

    if (cmd & SIFLI_I2C_TCR_START) {
        /*
         * Address phase. The byte staged in DBR is the slave address with the
         * R/W bit in bit 0, and the direction has to be latched here: the
         * interrupt handler that drives the data phase never tells the
         * controller which way the transfer is going.
         *
         * A repeated START arrives the same way, with the bus still active --
         * that is how the HAL's memory-read does write-then-read with a
         * second address byte.
         */
        uint8_t addr = (s->regs[SIFLI_I2C_DBR] & 0xff) >> 1;
        bool is_recv = s->regs[SIFLI_I2C_DBR] & 1;

        if (i2c_start_transfer(s->bus, addr, is_recv) != 0) {
            /* Nobody answered. The HAL's error paths key off BED. */
            sr |= SIFLI_I2C_SR_BED;
        } else {
            s->active = true;
            s->recv = is_recv;
        }
        sr |= SIFLI_I2C_SR_TE;
    } else if (cmd & SIFLI_I2C_TCR_TB) {
        if (s->active) {
            if (s->recv) {
                s->rx_byte = i2c_recv(s->bus);
                sr |= SIFLI_I2C_SR_RF;
            } else {
                if (i2c_send(s->bus, s->regs[SIFLI_I2C_DBR] & 0xff) != 0) {
                    sr |= SIFLI_I2C_SR_BED;
                }
                sr |= SIFLI_I2C_SR_TE;
            }
        }
    }

    if (cmd & SIFLI_I2C_TCR_STOP) {
        if (s->active) {
            if (cmd & SIFLI_I2C_TCR_NACK) {
                /* The HAL NACKs the last byte of a read to end it. */
                i2c_nack(s->bus);
            }
            i2c_end_transfer(s->bus);
            s->active = false;
            sr |= SIFLI_I2C_SR_MSD;
        }
    }

    s->regs[SIFLI_I2C_SR] = sr;
    sifli_i2c_update_irq(s);
}

static uint32_t sifli_i2c_read_reg(SifliI2CState *s, unsigned idx)
{
    switch (idx) {
    case SIFLI_I2C_TCR:
        /* Command strobes; there is no state behind them. */
        return 0;
    case SIFLI_I2C_DBR:
        return s->rx_byte;
    default:
        return s->regs[idx];
    }
}

static void sifli_i2c_write_reg(SifliI2CState *s, unsigned idx, uint32_t v,
                                uint32_t field)
{
    switch (idx) {
    case SIFLI_I2C_TCR:
        sifli_i2c_do_tcr(s, v);
        return;
    case SIFLI_I2C_SR:
        /*
         * Write 1 to clear. The HAL clears by writing the register back to
         * itself, so a plain store here would leave every flag set and the
         * interrupt line stuck high.
         */
        s->regs[SIFLI_I2C_SR] &= ~v;
        sifli_i2c_update_irq(s);
        return;
    case SIFLI_I2C_DBR:
        s->regs[SIFLI_I2C_DBR] = v & 0xff;
        return;
    case SIFLI_I2C_CR:
        /*
         * RSTREQ is a self-clearing strobe: HAL_I2C_Reset() sets it and then
         * spins until it reads back clear. Not storing it is the whole
         * behaviour, and it lets the loop exit on the first read.
         */
        s->regs[SIFLI_I2C_CR] = (s->regs[SIFLI_I2C_CR] & ~field) |
                                (v & field & ~SIFLI_I2C_CR_RSTREQ);
        return;
    case SIFLI_I2C_IER:
        s->regs[SIFLI_I2C_IER] = (s->regs[SIFLI_I2C_IER] & ~field) |
                                 (v & field);
        sifli_i2c_update_irq(s);
        return;
    default:
        s->regs[idx] = (s->regs[idx] & ~field) | (v & field);
        return;
    }
}

/*
 * Sub-word accesses are folded onto the containing 32-bit register. The HAL
 * only touches these as words, but a stray byte access should read something
 * sane rather than silently return zero.
 */
static uint64_t sifli_i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    SifliI2CState *s = opaque;
    unsigned idx = addr >> 2;

    if (idx >= SIFLI_I2C_NREGS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: read of unmodelled offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }

    return extract32(sifli_i2c_read_reg(s, idx), (addr & 3) * 8, size * 8);
}

static void sifli_i2c_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    SifliI2CState *s = opaque;
    unsigned idx = addr >> 2;
    unsigned shift = (addr & 3) * 8;
    uint32_t field, v;

    if (idx >= SIFLI_I2C_NREGS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write of unmodelled offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return;
    }

    if (size >= 4) {
        field = ~0u;
    } else {
        field = ((1u << (size * 8)) - 1) << shift;
    }
    v = ((uint32_t)val << shift) & field;

    sifli_i2c_write_reg(s, idx, v, field);
}

static const MemoryRegionOps sifli_i2c_ops = {
    .read = sifli_i2c_read,
    .write = sifli_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void sifli_i2c_reset(DeviceState *dev)
{
    SifliI2CState *s = SIFLI_I2C(dev);

    if (s->active) {
        i2c_end_transfer(s->bus);
    }

    memset(s->regs, 0, sizeof(s->regs));
    s->rx_byte = 0;
    s->active = false;
    s->recv = false;

    sifli_i2c_update_irq(s);
}

static void sifli_i2c_realize(DeviceState *dev, Error **errp)
{
    SifliI2CState *s = SIFLI_I2C(dev);

    /*
     * The bus has to carry the controller's name: a slave is attached from
     * the command line with -device ...,bus=<name>, and QEMU looks a bus up
     * by name. Its own default for an unnamed bus is i2c-bus.0, i2c-bus.1,
     * ... in creation order, which says nothing about which controller is
     * which once there is more than one.
     */
    s->bus = i2c_init_bus(dev, s->bus_name ? s->bus_name : "i2c");
}

static void sifli_i2c_instance_init(Object *obj)
{
    SifliI2CState *s = SIFLI_I2C(obj);

    memory_region_init_io(&s->mmio, obj, &sifli_i2c_ops, s,
                          TYPE_SIFLI_I2C, SIFLI_I2C_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const Property sifli_i2c_properties[] = {
    DEFINE_PROP_STRING("bus-name", SifliI2CState, bus_name),
};

static void sifli_i2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, sifli_i2c_properties);
    dc->realize = sifli_i2c_realize;
    device_class_set_legacy_reset(dc, sifli_i2c_reset);
    /* No migration support: the SiFli machines do not vmstate their devices. */
    dc->user_creatable = false;
}

static const TypeInfo sifli_i2c_types[] = {
    {
        .name          = TYPE_SIFLI_I2C,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SifliI2CState),
        .instance_init = sifli_i2c_instance_init,
        .class_init    = sifli_i2c_class_init,
    },
};

DEFINE_TYPES(sifli_i2c_types)
