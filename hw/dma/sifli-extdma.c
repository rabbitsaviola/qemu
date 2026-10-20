/*
 * SF32LB52x external DMA: uncompressed memory copies.
 * Register layout: SiFli SDK drivers/cmsis/sf32lb52x/extdma.h.
 * Transfers complete synchronously, like the other SiFli DMA models.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/misc/sifli-sbus.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/address-spaces.h"
#include "system/memory.h"

#define TYPE_SIFLI_EXTDMA "sifli-extdma"
OBJECT_DECLARE_SIMPLE_TYPE(SifliExtdmaState, SIFLI_EXTDMA)

enum {
    EDMA_ISR = 0x00,
    EDMA_IFCR = 0x04,
    EDMA_CCR = 0x08,
    EDMA_CNDTR = 0x0c,
    EDMA_SRCAR = 0x10,
    EDMA_DSTAR = 0x14,
    EDMA_CMPRCR = 0x20,
};

struct SifliExtdmaState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;
    uint32_t regs[0x40 / 4];
};

static void extdma_update_irq(SifliExtdmaState *s)
{
    qemu_set_irq(s->irq, (s->regs[EDMA_ISR / 4] &
                         s->regs[EDMA_CCR / 4] & 0x1e) != 0);
}

static void extdma_run(SifliExtdmaState *s)
{
    uint32_t ccr = s->regs[EDMA_CCR / 4];
    unsigned src_size = 1u << ((ccr >> 10) & 3);
    unsigned dst_size = 1u << ((ccr >> 8) & 3);
    uint32_t src = sifli_sbus_to_mapped_cpu_addr(s->regs[EDMA_SRCAR / 4]);
    uint32_t dst = sifli_sbus_to_mapped_cpu_addr(s->regs[EDMA_DSTAR / 4]);
    uint32_t remaining = s->regs[EDMA_CNDTR / 4] & 0xfffff;
    uint32_t flag = BIT(1);

    if ((s->regs[EDMA_CMPRCR / 4] & BIT(0)) ||
        src_size > 4 || dst_size > 4 || src_size != dst_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sifli-extdma: unsupported compression or item sizes\n");
        flag = BIT(3);
    } else {
        while (remaining) {
            uint8_t item[4];

            if (address_space_read(&address_space_memory, src,
                                   MEMTXATTRS_UNSPECIFIED, item, src_size) !=
                MEMTX_OK ||
                address_space_write(&address_space_memory, dst,
                                    MEMTXATTRS_UNSPECIFIED, item, dst_size) !=
                MEMTX_OK) {
                flag = BIT(3);
                break;
            }
            if (ccr & BIT(7)) {
                src += src_size;
            }
            if (ccr & BIT(6)) {
                dst += dst_size;
            }
            remaining--;
        }
    }
    s->regs[EDMA_CNDTR / 4] = remaining;
    s->regs[EDMA_CCR / 4] &= ~BIT(0);
    s->regs[EDMA_ISR / 4] |= BIT(0) | flag;
    if (!remaining) {
        s->regs[EDMA_ISR / 4] |= BIT(2);
    }
    extdma_update_irq(s);
}

static uint64_t extdma_read(void *opaque, hwaddr offset, unsigned size)
{
    SifliExtdmaState *s = opaque;

    return s->regs[offset / 4];
}

static void extdma_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    SifliExtdmaState *s = opaque;

    switch (offset) {
    case EDMA_ISR:
        return;
    case EDMA_IFCR:
        s->regs[EDMA_ISR / 4] &= (value & BIT(0)) ? 0 : ~value;
        if (!(s->regs[EDMA_ISR / 4] & 0x1e)) {
            s->regs[EDMA_ISR / 4] = 0;
        }
        break;
    case EDMA_CCR:
        if (value & BIT(31)) {
            memset(s->regs, 0, sizeof(s->regs));
        } else {
            s->regs[offset / 4] = value;
            if (value & BIT(0)) {
                extdma_run(s);
            }
        }
        break;
    default:
        s->regs[offset / 4] = value;
        break;
    }
    extdma_update_irq(s);
}

static const MemoryRegionOps extdma_ops = {
    .read = extdma_read,
    .write = extdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void extdma_reset(DeviceState *dev)
{
    SifliExtdmaState *s = SIFLI_EXTDMA(dev);

    memset(s->regs, 0, sizeof(s->regs));
    extdma_update_irq(s);
}

static void extdma_init(Object *obj)
{
    SifliExtdmaState *s = SIFLI_EXTDMA(obj);

    memory_region_init_io(&s->mmio, obj, &extdma_ops, s,
                          TYPE_SIFLI_EXTDMA, sizeof(s->regs));
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void extdma_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), extdma_reset);
}

static const TypeInfo extdma_info = {
    .name = TYPE_SIFLI_EXTDMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SifliExtdmaState),
    .instance_init = extdma_init,
    .class_init = extdma_class_init,
};

static void extdma_register_types(void)
{
    type_register_static(&extdma_info);
}

type_init(extdma_register_types)
