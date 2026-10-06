/*
 * SiFli SF32LB52x GPIO
 *
 * Same register shell as the other SiFli devices, with one thing that is not
 * a register: the pins. A bank can be driven from outside (the panel's touch
 * controller pulls its interrupt line low), and a level change on an enabled
 * pin has to latch a status bit and raise the shared interrupt line, because
 * that is the only way firmware ever learns a pin moved.
 *
 * The HAL's handler reads ISR, asserts that the matching IER bit is set --
 * HAL_ASSERT, which is a while(1) trap -- and then clears the bit by writing
 * it back. So IER has to read back faithfully and ISR has to be
 * write-1-to-clear; getting either wrong hangs the firmware rather than
 * failing visibly.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/misc/sifli-gpio.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qemu/log.h"
#include "qemu/module.h"

/*
 * What the pins sit at. A pin driven as an output reads back its own latch;
 * otherwise it reads whatever is on it from outside.
 */
static uint32_t sifli_gpio_levels(SifliGpioBank *b)
{
    return (b->dor & b->doer) | (b->in & ~b->doer);
}

static void sifli_gpio_update_irq(SifliGpioState *s)
{
    unsigned i;

    for (i = 0; i < s->num_banks; i++) {
        if (s->bank[i].isr & s->bank[i].ier) {
            qemu_set_irq(s->irq, 1);
            return;
        }
    }

    qemu_set_irq(s->irq, 0);
}

/*
 * Latch a status bit for every pin among @moved whose new level is one the
 * bank was configured for.
 *
 * Only edge triggering is modelled. Nothing on this board asks for a level
 * interrupt, and supporting one would mean re-running this whenever ITR, IPHR
 * or IPLR is written, not only when a pin moves.
 */
static void sifli_gpio_detect_edges(SifliGpioState *s, unsigned bank,
                                    uint32_t moved, uint32_t level)
{
    SifliGpioBank *b = &s->bank[bank];

    if (!(b->ier & moved) || !(b->itr & moved)) {
        return;
    }

    if ((level & moved & b->iphr) || (~level & moved & b->iplr)) {
        b->isr |= moved;
        sifli_gpio_update_irq(s);
    }
}

/* Re-check one bank's pins after something other than the pins moved. */
static void sifli_gpio_recheck(SifliGpioState *s, unsigned bank,
                               uint32_t before)
{
    uint32_t after = sifli_gpio_levels(&s->bank[bank]);

    if (before != after) {
        sifli_gpio_detect_edges(s, bank, before ^ after, after);
    }
}

static void sifli_gpio_set(void *opaque, int line, int level)
{
    SifliGpioState *s = opaque;
    SifliGpioBank *b = &s->bank[line >> 5];
    uint32_t mask = 1u << (line & 31);
    uint32_t before = sifli_gpio_levels(b);

    if (level) {
        b->in |= mask;
    } else {
        b->in &= ~mask;
    }

    sifli_gpio_recheck(s, line >> 5, before);
}

static uint32_t sifli_gpio_read_reg(SifliGpioBank *b, unsigned reg)
{
    switch (reg) {
    case SIFLI_GPIO_DIR:
        return sifli_gpio_levels(b);
    case SIFLI_GPIO_DOR:
        return b->dor;
    case SIFLI_GPIO_DOER:
        return b->doer;
    case SIFLI_GPIO_IER:
        return b->ier;
    case SIFLI_GPIO_ITR:
        return b->itr;
    case SIFLI_GPIO_IPHR:
        return b->iphr;
    case SIFLI_GPIO_IPLR:
        return b->iplr;
    case SIFLI_GPIO_ISR:
        return b->isr;
    case SIFLI_GPIO_IER_EXT:
        return b->ier_ext;
    case SIFLI_GPIO_ISR_EXT:
        return b->isr_ext;
    case SIFLI_GPIO_OEMR:
        return b->oemr;
    default:
        /* The set and clear strobes have no readback of their own. */
        return 0;
    }
}

static void sifli_gpio_write_reg(SifliGpioState *s, unsigned bank,
                                 unsigned reg, uint32_t v, uint32_t field)
{
    SifliGpioBank *b = &s->bank[bank];
    uint32_t before = sifli_gpio_levels(b);

    switch (reg) {
    case SIFLI_GPIO_DIR:
        /* Read-only: pin levels come from the pads, not from a store. */
        qemu_log_mask(LOG_GUEST_ERROR, "%s: DIR is read-only\n", __func__);
        return;
    case SIFLI_GPIO_DOSR:
        b->dor |= v;
        break;
    case SIFLI_GPIO_DOCR:
        b->dor &= ~v;
        break;
    case SIFLI_GPIO_DOESR:
        b->doer |= v;
        break;
    case SIFLI_GPIO_DOECR:
        b->doer &= ~v;
        break;
    case SIFLI_GPIO_IESR:
        b->ier |= v;
        sifli_gpio_update_irq(s);
        return;
    case SIFLI_GPIO_IECR:
        b->ier &= ~v;
        sifli_gpio_update_irq(s);
        return;
    case SIFLI_GPIO_ITSR:
        b->itr |= v;
        return;
    case SIFLI_GPIO_ITCR:
        b->itr &= ~v;
        return;
    case SIFLI_GPIO_IPHSR:
        b->iphr |= v;
        return;
    case SIFLI_GPIO_IPHCR:
        b->iphr &= ~v;
        return;
    case SIFLI_GPIO_IPLSR:
        b->iplr |= v;
        return;
    case SIFLI_GPIO_IPLCR:
        b->iplr &= ~v;
        return;
    case SIFLI_GPIO_ISR:
        /* Write 1 to clear: this is the acknowledgement the handler does. */
        b->isr &= ~v;
        sifli_gpio_update_irq(s);
        return;
    case SIFLI_GPIO_IESR_EXT:
        b->ier_ext |= v;
        return;
    case SIFLI_GPIO_IECR_EXT:
        b->ier_ext &= ~v;
        return;
    case SIFLI_GPIO_ISR_EXT:
        b->isr_ext &= ~v;
        return;
    case SIFLI_GPIO_OEMR:
        b->oemr = (b->oemr & ~field) | (v & field);
        return;
    case SIFLI_GPIO_OEMSR:
        b->oemr |= v;
        return;
    case SIFLI_GPIO_OEMCR:
        b->oemr &= ~v;
        return;
    default:
        return;
    }

    /*
     * Only the output registers reach here, and only they can change what a
     * pin reads back at -- an output pin watching its own changes is a real
     * thing firmware does.
     */
    sifli_gpio_recheck(s, bank, before);
}

static uint64_t sifli_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    SifliGpioState *s = opaque;
    unsigned bank = addr / SIFLI_GPIO_BANK_SIZE;
    unsigned reg = addr % SIFLI_GPIO_BANK_SIZE;

    if (bank >= s->num_banks || reg >= SIFLI_GPIO_NREGS * 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: read of unmodelled offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }

    return extract32(sifli_gpio_read_reg(&s->bank[bank], reg),
                     (addr & 3) * 8, size * 8);
}

static void sifli_gpio_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    SifliGpioState *s = opaque;
    unsigned bank = addr / SIFLI_GPIO_BANK_SIZE;
    unsigned reg = addr % SIFLI_GPIO_BANK_SIZE;
    unsigned shift = (addr & 3) * 8;
    uint32_t field, v;

    if (bank >= s->num_banks || reg >= SIFLI_GPIO_NREGS * 4) {
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

    sifli_gpio_write_reg(s, bank, reg, v, field);
}

static const MemoryRegionOps sifli_gpio_ops = {
    .read = sifli_gpio_read,
    .write = sifli_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void sifli_gpio_reset(DeviceState *dev)
{
    SifliGpioState *s = SIFLI_GPIO(dev);

    memset(s->bank, 0, sizeof(s->bank));
    sifli_gpio_update_irq(s);
}

static void sifli_gpio_realize(DeviceState *dev, Error **errp)
{
    SifliGpioState *s = SIFLI_GPIO(dev);

    if (s->num_banks == 0 || s->num_banks > SIFLI_GPIO_MAX_BANKS) {
        error_setg(errp, "sifli-gpio needs 1..%u banks, got %u",
                   SIFLI_GPIO_MAX_BANKS, s->num_banks);
        return;
    }

    qdev_init_gpio_in(dev, sifli_gpio_set,
                      s->num_banks * SIFLI_GPIO_PINS_PER_BANK);
}

static void sifli_gpio_instance_init(Object *obj)
{
    SifliGpioState *s = SIFLI_GPIO(obj);

    memory_region_init_io(&s->mmio, obj, &sifli_gpio_ops, s,
                          TYPE_SIFLI_GPIO,
                          SIFLI_GPIO_MAX_BANKS * SIFLI_GPIO_BANK_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const Property sifli_gpio_properties[] = {
    /* GPIO1 has PA00-PA63 in two banks; GPIO2 has only PB00-PB31. */
    DEFINE_PROP_UINT32("num-banks", SifliGpioState, num_banks, 1),
};

static void sifli_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, sifli_gpio_properties);
    dc->realize = sifli_gpio_realize;
    device_class_set_legacy_reset(dc, sifli_gpio_reset);
    dc->user_creatable = false;
}

static const TypeInfo sifli_gpio_types[] = {
    {
        .name          = TYPE_SIFLI_GPIO,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SifliGpioState),
        .instance_init = sifli_gpio_instance_init,
        .class_init    = sifli_gpio_class_init,
    },
};

DEFINE_TYPES(sifli_gpio_types)
