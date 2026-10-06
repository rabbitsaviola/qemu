/*
 * SiFli thin register bank
 *
 * A data-driven model for the SoC's pure-configuration register blocks
 * (RCC, AON, PMU and the like). It stores what software writes, and lets the
 * table declare which bits read back as something else -- typically a "ready"
 * or "locked" status bit that hardware would raise once an operation settled.
 *
 * The point is to model only what the HAL actually looks at. These blocks have
 * a few hundred registers between them and no behaviour to speak of: firmware
 * writes an enable, then polls a status bit. Returning the terminal state
 * immediately is enough for that, and it is what makes one model per block
 * feasible across five SoC series.
 *
 * Registers that do have behaviour (a UART, a DMA controller, a flash
 * interface) get their own device model instead. This is only for the ones
 * that are a bag of bits.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_SIFLI_REGBANK_H
#define HW_MISC_SIFLI_REGBANK_H

#include "hw/clock.h"
#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_SIFLI_REGBANK "sifli-regbank"
OBJECT_DECLARE_SIMPLE_TYPE(SifliRegBankState, SIFLI_REGBANK)

/*
 * One 32-bit register.
 *
 * Every field defaults to 0 when left out of an initialiser, which makes the
 * common case -- a plain read/write register -- just
 * { .off = ..., .reset = ... }. Note that "writable" is therefore the
 * default: readonly lists the bits software may *not* change, rather than the
 * other way round.
 *
 * The fields fall into three groups by which side of an access they affect.
 */
typedef struct SifliRegDef {
    /* The stored value. */
    uint32_t off;       /* byte offset within the bank */
    uint32_t reset;     /* value of the stored bits after reset */

    /*
     * Write side.
     *
     * readonly: bits an ordinary write leaves alone, so they keep whatever
     * is stored -- the reset value, unless a write hook has changed it.
     * Hardware-owned fields go here: a chip ID, a status register. The rest
     * of the register stays writable.
     *
     * w1c: bits a write of 1 clears. They are not ordinary storage either,
     * because a write can only ever take them away -- whatever set them
     * (hardware, or the reset value) stands until software clears it. A
     * write of 0 leaves them alone. This is the shape of an interrupt status
     * register: hardware raises the flag, software writes 1 to acknowledge.
     *
     * Note this is the register acting on itself. A write that changes some
     * *other* register is an alias, below -- and that is how this SoC
     * actually clears flags, through dedicated registers (WCR clears WSR,
     * SCR clears SR, USART's ICR clears ISR). No table below uses w1c yet.
     * It is kept because the pattern is common enough to be worth having
     * ready; the first register that needs it should arrive with a qtest
     * check, since until then nothing can tell it apart from force0.
     */
    uint32_t readonly;
    uint32_t w1c;

    /*
     * Read side.
     *
     * writeonly: reads of the whole register return zero. For strobes that
     * exist only to be written -- a command register, a clear register.
     *
     * force1/force0: same idea for individual bits. These are ORed into (or
     * ANDed out of) the value a read returns; the stored copy is left alone.
     *
     * force1 is how a "ready" is faked. A block that raises a lock bit once
     * its PLL settles is modelled with force1 = that bit, so the HAL's
     * "write enable, spin until locked" loop falls through on the first test.
     * Use force0 for bits that read back clear in hardware -- self-clearing
     * reset pulses, write-only strobes.
     */
    bool writeonly;
    uint32_t force1;
    uint32_t force0;

    /*
     * Write redirection. When alias_set or alias_clear is non-zero, a write
     * to this register is applied to the register at offset `alias` instead
     * of being stored here -- so this register reads back as its reset value
     * for ever, which is what a write-only alias looks like.
     *
     * alias_set bits are ORed into the target, alias_clear bits are ANDed
     * out. This is the RCC's enable registers: HAL_RCC_EnableModule() does
     *
     *     hwp_hpsys_rcc->ESR1 = 1UL << offset;
     *
     * and HAL_RCC_IsModuleEnabled() then reads ENR1. Note the assignment,
     * not |=: an alias must not let one enable bit wipe the others, which is
     * why the target is modified with set/clear bits rather than assigned.
     *
     * Either mask being non-zero is what marks an alias as present, so the
     * three fields can be left out entirely on every other register.
     */
    uint32_t alias;
    uint32_t alias_set;
    uint32_t alias_clear;
} SifliRegDef;

/*
 * A whole block: its size, its registers, and an optional hook.
 *
 * Blocks are looked up by name so that the machine can name one as a property
 * (and so that -device sifli-regbank,bank=... works for poking at it by hand).
 * Register a table with SIFLI_REGBANK_REGISTER().
 */
typedef struct SifliRegBankDef {
    const char *name;
    uint32_t size;              /* size of the MMIO window */
    const SifliRegDef *regs;
    unsigned nregs;

    /*
     * Called with the raw value before the table's write semantics apply.
     * Return true if the write was handled here, false to let the table take
     * it.
     *
     * This exists for registers that act on *each other*. The RCC is the
     * reason: its ESR and ECR registers are write-only aliases that set and
     * clear bits in the readable ENR, and the HAL writes
     *
     *     *esr = 1UL << offset;          // HAL_RCC_EnableModule()
     *     *ecr = 1UL << offset;          // HAL_RCC_DisableModule()
     *
     * and then checks the result through
     *
     *     *enr & (1UL << offset)         // HAL_RCC_IsModuleEnabled()
     *
     * A per-register mask cannot express that, because the register being
     * written is not the register being read. Use sifli_regbank_reg() and
     * sifli_regbank_set_reg() to reach the others from the hook.
     */
    bool (*write_hook)(SifliRegBankState *s, uint32_t off, uint32_t value);

    /*
     * Called with the value a read would otherwise return -- the stored bits
     * after force1/force0 -- in *value, and may replace it. Write only the
     * offsets the hook owns; leaving *value alone lets the table's value
     * stand.
     *
     * This exists for the one register whose value is not storage at all: a
     * free-running counter. Nothing software writes decides what it reads
     * back, and no periodic update can produce it either -- the answer
     * changes between any two reads, so it has to be computed when asked.
     *
     * GTIMR is why. HAL_GetTick() is that counter scaled to milliseconds
     * (drv_common.c:320), and every HAL wait loop in the SDK bounds itself
     * with HAL_GetTick(). Leave the counter frozen and those loops do not
     * time out, they spin for ever: an unmodelled peripheral turns into a
     * hung board rather than a clean HAL_TIMEOUT. That is worth a hook.
     */
    void (*read_hook)(SifliRegBankState *s, uint32_t off, uint32_t *value);
} SifliRegBankDef;

struct SifliRegBankState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    char *bank_name;
    const SifliRegBankDef *def;

    /* Current value of each register, parallel to def->regs[]. */
    uint32_t *regs;

    /*
     * Optional. A block that decides how fast the CPU runs -- the RCC -- is
     * given the machine's system clock here so its write hook can retune it
     * with clock_set_hz() as software changes the clock tree. Banks with no
     * use for it simply leave it alone.
     */
    Clock *clk;

    /*
     * Optional second output, for the one block that produces two clocks the
     * machine has to wire separately. The RCC again: besides the CPU clock
     * above it also divides out a "tick clock", a separate input to SysTick
     * that software normally selects instead of the CPU clock. Both have to
     * reach the machine or the firmware's millisecond is wrong -- see
     * sf32lb52x_rcc_update_clocks().
     */
    Clock *tick_clk;

    /*
     * Optional. A second bank whose registers this one's hooks have to read.
     *
     * The AON's GTIMR is the reason: it counts the low-power clock, and which
     * clock that is -- the 32 kHz crystal or the ~10 kHz RC -- is decided by
     * RTC_CR.LPCKSEL, a register in the RTC bank rather than in the AON one.
     * The machine links the two after creating both. A bank with no peer
     * leaves this NULL, and its hooks check before reaching through it.
     */
    SifliRegBankState *peer;
};

/*
 * Make a bank table findable by name. Call from a type_init() or a
 * constructor so that it is registered before any machine asks for it.
 */
void sifli_regbank_register(const SifliRegBankDef *def);

/* Look up a previously registered table; NULL if there is no such bank. */
const SifliRegBankDef *sifli_regbank_lookup(const char *name);

/* Names of every registered bank, for error messages. Caller frees. */
char *sifli_regbank_list(void);

/*
 * Register access for write hooks. These work on the stored value, with none
 * of the read/write masking applied, so a hook sees and writes exactly the
 * bits the table holds. Unknown offsets are ignored.
 */
uint32_t sifli_regbank_reg(SifliRegBankState *s, uint32_t off);
void sifli_regbank_set_reg(SifliRegBankState *s, uint32_t off, uint32_t value);

/* Hand a bank the clock its write hook may drive. Call before realize. */
void sifli_regbank_set_clock(SifliRegBankState *s, Clock *clk);

/*
 * Same, for a block with a second clock output. Call before realize; a bank
 * given no tick clock simply never touches s->tick_clk.
 */
void sifli_regbank_set_tick_clock(SifliRegBankState *s, Clock *clk);

/*
 * Hand a bank another bank whose registers its hooks may read. Call before
 * realize, or any time after -- the hooks read the peer when they run, not
 * when it is set. See the peer field above.
 */
void sifli_regbank_set_peer(SifliRegBankState *s, SifliRegBankState *peer);

#endif /* HW_MISC_SIFLI_REGBANK_H */
