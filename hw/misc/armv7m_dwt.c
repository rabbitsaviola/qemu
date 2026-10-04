/*
 * ARMv7-M DWT cycle counter. See include/hw/misc/armv7m_dwt.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/misc/armv7m_dwt.h"
#include "hw/qdev-clock.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"

/*
 * Nothing here executes at a known rate, so rather than counting instructions
 * the register is derived from the host clock: how long ago software zeroed
 * it, times the CPU clock.
 *
 * That is enough because of how the count is used. HAL_Delay_us_() does
 *
 *     start = DWT_CYCCNT;
 *     while (DWT_CYCCNT - start < sysclk_m * us);
 *
 * so it only asks that the register move forward at roughly the right rate.
 * Being cycle-accurate would need instruction counting and would cost far
 * more than the delays are worth.
 *
 * QEMU_CLOCK_REALTIME rather than QEMU_CLOCK_VIRTUAL, deliberately. Without
 * icount -- which is the normal case here -- the virtual clock is the vCPU's
 * vm_clock, and the only thing that advances it is
 * qemu_clock_advance_virtual_time(), which is called from qtest alone. Left
 * on the virtual clock the counter would sit at zero during a normal run,
 * which is exactly the hang this device exists to prevent. Wall-clock time
 * is also the honest reading of "delay for 10 microseconds".
 */
static uint32_t armv7m_dwt_cycles(ArmV7mDwtState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    uint64_t ns, hz;

    if (now <= s->base_ns) {
        return 0;
    }

    ns = now - s->base_ns;
    hz = clock_get_hz(s->clk);

    /*
     * Split the multiply: (ns % 1s) * hz stays under 2^58 for any clock
     * this can be handed, where ns * hz would not.
     */
    return (ns / 1000000000) * hz
         + ((ns % 1000000000) * hz) / 1000000000;
}

static uint64_t armv7m_dwt_read(void *opaque, hwaddr addr, unsigned size)
{
    ArmV7mDwtState *s = opaque;

    switch (addr) {
    case DWT_CTRL:
        return s->ctrl;
    case DWT_CYCCNT:
        return armv7m_dwt_cycles(s);
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "armv7m-dwt: read of unimplemented offset 0x%"
                      HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void armv7m_dwt_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    ArmV7mDwtState *s = opaque;

    switch (addr) {
    case DWT_CTRL:
        s->ctrl = value;
        return;
    case DWT_CYCCNT:
        /* Writing restarts the count; hardware ignores the value written. */
        s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        return;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "armv7m-dwt: write of 0x%" PRIx64 " to unimplemented "
                      "offset 0x%" HWADDR_PRIx "\n", value, addr);
        return;
    }
}

static const MemoryRegionOps armv7m_dwt_ops = {
    .read = armv7m_dwt_read,
    .write = armv7m_dwt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void armv7m_dwt_reset(DeviceState *dev)
{
    ArmV7mDwtState *s = ARMV7M_DWT(dev);

    s->ctrl = 0;
    s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
}

static void armv7m_dwt_init(Object *obj)
{
    ArmV7mDwtState *s = ARMV7M_DWT(obj);

    s->clk = qdev_init_clock_in(DEVICE(obj), "clk", NULL, NULL, 0);

    memory_region_init_io(&s->mmio, obj, &armv7m_dwt_ops, s,
                          TYPE_ARMV7M_DWT, ARMV7M_DWT_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void armv7m_dwt_realize(DeviceState *dev, Error **errp)
{
    ArmV7mDwtState *s = ARMV7M_DWT(dev);

    /*
     * Without a clock the counter would sit at zero and reintroduce exactly
     * the hang this device exists to prevent, so refuse to come up.
     */
    if (!clock_has_source(s->clk)) {
        error_setg(errp, "armv7m-dwt: clk must be connected");
    }
}

static void armv7m_dwt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, armv7m_dwt_reset);
    dc->realize = armv7m_dwt_realize;
    /* Not user-creatable: it belongs at a fixed address in the PPB. */
    dc->user_creatable = false;
}

static const TypeInfo armv7m_dwt_info = {
    .name          = TYPE_ARMV7M_DWT,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ArmV7mDwtState),
    .instance_init = armv7m_dwt_init,
    .class_init    = armv7m_dwt_class_init,
};

static void armv7m_dwt_register_types(void)
{
    type_register_static(&armv7m_dwt_info);
}

type_init(armv7m_dwt_register_types)
