/*
 * SiFli thin register bank
 *
 * See include/hw/misc/sifli-regbank.h for what this is for.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/misc/sifli-regbank.h"
#include "hw/qdev-properties.h"
#include "qemu/bitops.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"

/*
 * Registered tables.
 *
 * These are populated from type_init() constructors in the per-series files
 * and read when a machine realizes a bank, so a handful of entries is all we
 * will ever need.
 */
#define SIFLI_REGBANK_MAX   16

static const SifliRegBankDef *sifli_banks[SIFLI_REGBANK_MAX];
static unsigned sifli_nbanks;

void sifli_regbank_register(const SifliRegBankDef *def)
{
    unsigned i;

    for (i = 0; i < sifli_nbanks; i++) {
        if (strcmp(sifli_banks[i]->name, def->name) == 0) {
            /*
             * Re-registering the same table happens when a series file is
             * linked into both qemu-system-arm and qemu-system-aarch64.
             */
            if (sifli_banks[i] != def) {
                error_report("sifli-regbank: duplicate bank name '%s'",
                             def->name);
            }
            return;
        }
    }

    if (sifli_nbanks == SIFLI_REGBANK_MAX) {
        error_report("sifli-regbank: too many banks (max %d)",
                     SIFLI_REGBANK_MAX);
        return;
    }
    sifli_banks[sifli_nbanks++] = def;
}

const SifliRegBankDef *sifli_regbank_lookup(const char *name)
{
    unsigned i;

    for (i = 0; i < sifli_nbanks; i++) {
        if (strcmp(sifli_banks[i]->name, name) == 0) {
            return sifli_banks[i];
        }
    }
    return NULL;
}

char *sifli_regbank_list(void)
{
    GString *s = g_string_new(NULL);
    unsigned i;

    for (i = 0; i < sifli_nbanks; i++) {
        if (i) {
            g_string_append(s, ", ");
        }
        g_string_append(s, sifli_banks[i]->name);
    }
    return g_string_free(s, false);
}

static int sifli_regbank_index(const SifliRegBankState *s, uint32_t off)
{
    unsigned i;

    for (i = 0; i < s->def->nregs; i++) {
        if (s->def->regs[i].off == off) {
            return i;
        }
    }
    return -1;
}

static int sifli_regbank_find(const SifliRegBankState *s, hwaddr addr)
{
    return sifli_regbank_index(s, addr & ~(hwaddr)3);
}

uint32_t sifli_regbank_reg(SifliRegBankState *s, uint32_t off)
{
    int i = sifli_regbank_index(s, off);

    return i < 0 ? 0 : s->regs[i];
}

void sifli_regbank_set_reg(SifliRegBankState *s, uint32_t off, uint32_t value)
{
    int i = sifli_regbank_index(s, off);

    if (i >= 0) {
        s->regs[i] = value;
    }
}

void sifli_regbank_set_clock(SifliRegBankState *s, Clock *clk)
{
    s->clk = clk;
}

void sifli_regbank_set_tick_clock(SifliRegBankState *s, Clock *clk)
{
    s->tick_clk = clk;
}

void sifli_regbank_set_peer(SifliRegBankState *s, SifliRegBankState *peer)
{
    s->peer = peer;
}

/* Used by the alias redirection in sifli_regbank_write(). */
static void sifli_regbank_set_bits(SifliRegBankState *s, uint32_t off,
                                   uint32_t bits)
{
    int i = sifli_regbank_index(s, off);

    if (i >= 0) {
        s->regs[i] |= bits;
    }
}

static void sifli_regbank_clear_bits(SifliRegBankState *s, uint32_t off,
                                     uint32_t bits)
{
    int i = sifli_regbank_index(s, off);

    if (i >= 0) {
        s->regs[i] &= ~bits;
    }
}

static uint64_t sifli_regbank_read(void *opaque, hwaddr addr, unsigned size)
{
    SifliRegBankState *s = opaque;
    const SifliRegDef *r;
    uint32_t v;
    int i = sifli_regbank_find(s, addr);

    if (i < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: read of unmodelled offset 0x%" HWADDR_PRIx "\n",
                      s->def->name, addr);
        return 0;
    }

    r = &s->def->regs[i];

    if (r->writeonly) {
        v = 0;
    } else {
        v = (s->regs[i] | r->force1) & ~r->force0;
    }

    /*
     * Last, because a counter's value replaces all of the above rather than
     * adding to it.
     */
    if (s->def->read_hook) {
        s->def->read_hook(s, r->off, &v);
    }

    return extract32(v, (addr & 3) * 8, size * 8);
}

static void sifli_regbank_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    SifliRegBankState *s = opaque;
    const SifliRegDef *r;
    uint32_t field, ordinary, v, t;
    int i = sifli_regbank_find(s, addr);

    if (i < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write of 0x%" PRIx64 " to unmodelled offset "
                      "0x%" HWADDR_PRIx "\n", s->def->name, val, addr);
        return;
    }

    r = &s->def->regs[i];

    /*
     * Sub-word accesses are re-based onto the containing 32-bit register,
     * and the mask keeps them from touching the bytes they did not cover.
     */
    if (size >= 4) {
        field = ~0u;
    } else {
        field = ((1u << (size * 8)) - 1) << ((addr & 3) * 8);
    }
    v = ((uint32_t)val << ((addr & 3) * 8)) & field;

    /*
     * A redirecting alias takes the write and applies it elsewhere. It must
     * not also be stored here, or the register would stop looking write-only.
     */
    if (r->alias_set || r->alias_clear) {
        uint32_t set = v & r->alias_set;
        uint32_t clr = v & r->alias_clear;

        if (set) {
            sifli_regbank_set_bits(s, r->alias, set);
        }
        if (clr) {
            sifli_regbank_clear_bits(s, r->alias, clr);
        }
        return;
    }

    /*
     * The hook goes next and may also take the write entirely, for the few
     * registers whose effect is something other than storage.
     */
    if (s->def->write_hook && s->def->write_hook(s, r->off, v)) {
        return;
    }

    ordinary = ~(r->readonly | r->w1c) & field;

    t = s->regs[i];
    /* A write of 1 clears the w1c bits; a write of 0 leaves them. */
    t &= ~(v & r->w1c);
    t = (t & ~ordinary) | (v & ordinary);

    s->regs[i] = t;
}

static const MemoryRegionOps sifli_regbank_ops = {
    .read = sifli_regbank_read,
    .write = sifli_regbank_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void sifli_regbank_reset(DeviceState *dev)
{
    SifliRegBankState *s = SIFLI_REGBANK(dev);
    unsigned i;

    for (i = 0; i < s->def->nregs; i++) {
        s->regs[i] = s->def->regs[i].reset;
    }
}

static void sifli_regbank_realize(DeviceState *dev, Error **errp)
{
    SifliRegBankState *s = SIFLI_REGBANK(dev);

    s->def = sifli_regbank_lookup(s->bank_name);
    if (!s->def) {
        g_autofree char *names = sifli_regbank_list();

        error_setg(errp, "sifli-regbank: no bank named '%s'%s%s",
                   s->bank_name, sifli_nbanks ? "; known banks: " : "",
                   names);
        return;
    }

    s->regs = g_new0(uint32_t, s->def->nregs);

    memory_region_init_io(&s->mmio, OBJECT(dev), &sifli_regbank_ops, s,
                          s->def->name, s->def->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static const Property sifli_regbank_properties[] = {
    DEFINE_PROP_STRING("bank", SifliRegBankState, bank_name),
};

static void sifli_regbank_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, sifli_regbank_reset);
    device_class_set_props(dc, sifli_regbank_properties);
    dc->realize = sifli_regbank_realize;
}

static const TypeInfo sifli_regbank_info = {
    .name          = TYPE_SIFLI_REGBANK,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SifliRegBankState),
    .class_init    = sifli_regbank_class_init,
};

static void sifli_regbank_register_types(void)
{
    type_register_static(&sifli_regbank_info);
}

type_init(sifli_regbank_register_types)
