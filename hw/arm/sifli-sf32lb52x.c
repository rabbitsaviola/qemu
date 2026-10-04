/*
 * SiFli SF32LB52x SoC machine (Cortex-M33)
 *
 * This is the starting point of the "register-level thin emulation" approach:
 * get the machine up so that the real ARM firmware built by the SiFli SDK
 * (the SOC_QEMU board) runs on it. Peripheral models are added on demand.
 *
 * Contract with the firmware (see SiFli-SDK,
 * customer/boards/qemu_cortex_m33/hcpu/link.lds and
 * rtos/rtthread/bsp/sifli/drivers_qemu/):
 *   - Code region holds the vector table at its start; CPU init-svtor points
 *     at it. The base address is part-number dependent, so it is a machine
 *     property rather than a constant.
 *   - RAM at 0x20000000.
 *   - Cortex-M33 with FPU (the firmware is built hard-float).
 *   - SysTick at 1 ms, SystemCoreClock = 48 MHz (HXT48).
 *
 * Initialisation order follows hw/arm/mps2.c:
 *   clock_new -> qdev_prop_set_* -> qdev_connect_clock_in
 *     -> set_link("memory") -> sysbus_realize -> then mmio_map/connect_irq
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/boards.h"
#include "hw/qdev-clock.h"
#include "hw/qdev-properties.h"
#include "hw/arm/boot.h"
#include "hw/arm/armv7m.h"
#include "hw/arm/machines-qom.h"
#include "hw/arm/sf32lb52x.h"
#include "hw/char/sifli-usart.h"
#include "hw/misc/armv7m_dwt.h"
#include "hw/misc/sifli-regbank.h"
#include "system/system.h"
#include "system/address-spaces.h"
#include "qom/object.h"

#define TYPE_SIFLI_SF32LB52X_MACHINE MACHINE_TYPE_NAME("sf32lb52x")

struct Sifli52xMachineState {
    MachineState parent;

    ARMv7MState armv7m;
    MemoryRegion flash;
    MemoryRegion sram;

    Clock *sysclk;

    /* Boot flash window; depends on the part number. */
    uint32_t flash_base;
    uint32_t flash_size;
};

OBJECT_DECLARE_SIMPLE_TYPE(Sifli52xMachineState, SIFLI_SF32LB52X_MACHINE)

static void sifli_machine_instance_init(Object *obj)
{
    Sifli52xMachineState *s = SIFLI_SF32LB52X_MACHINE(obj);

    /*
     * Defaults for the parts currently in the SDK; overridable on the
     * command line, e.g. -M sf32lb52x,flash-base=0x12000000
     */
    s->flash_base = SF32LB52X_QSPI1_MEM_BASE;
    s->flash_size = SF32LB52X_FLASH_SIZE;
}

static void sifli_get_flash_base(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    Sifli52xMachineState *s = SIFLI_SF32LB52X_MACHINE(obj);
    uint32_t value = s->flash_base;

    visit_type_uint32(v, name, &value, errp);
}

static void sifli_set_flash_base(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    Sifli52xMachineState *s = SIFLI_SF32LB52X_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    s->flash_base = value;
}

static void sifli_get_flash_size(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    Sifli52xMachineState *s = SIFLI_SF32LB52X_MACHINE(obj);
    uint32_t value = s->flash_size;

    visit_type_uint32(v, name, &value, errp);
}

static void sifli_set_flash_size(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    Sifli52xMachineState *s = SIFLI_SF32LB52X_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    s->flash_size = value;
}

static void sifli_sf32lb52x_init(MachineState *machine)
{
    Sifli52xMachineState *s = SIFLI_SF32LB52X_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *armv7m;
    unsigned i;

    /*
     * Clocks. The rate has to agree with what the RCC model makes the
     * firmware believe: see SF32LB52X_HXT48_FRQ.
     */
    s->sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(s->sysclk, SF32LB52X_HXT48_FRQ);

    /*
     * Memory.
     *
     * ROM semantics: on real silicon the boot QSPI window is read-only XIP.
     * SRAM is sized to the part's real capacity (512K) rather than to the
     * 4M declared in the firmware's link.lds, so that running out of memory
     * shows up the same way it would on hardware.
     *
     * The owner is NULL on purpose: the migration-enabled variants of these
     * helpers call DEVICE(owner) to register RAM migration state, and a
     * MachineState is not a Device. hw/arm/mps2.c does the same.
     */
    memory_region_init_rom(&s->flash, NULL, "sifli.flash",
                           s->flash_size, &error_fatal);
    memory_region_add_subregion(sysmem, s->flash_base, &s->flash);

    memory_region_init_ram(&s->sram, NULL, "sifli.sram",
                           SF32LB52X_SRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, SF32LB52X_SRAM_BASE, &s->sram);

    /* CPU, NVIC and SysTick */
    object_initialize_child(OBJECT(s), "armv7m", &s->armv7m, TYPE_ARMV7M);
    armv7m = DEVICE(&s->armv7m);

    /*
     * Highest interrupt number in use is SECU1_IRQn = 98, so at least 99
     * lines are needed. The NVIC limit is 496.
     */
    qdev_prop_set_uint32(armv7m, "num-irq", SF32LB52X_NUM_IRQ);

    /*
     * The firmware's CMSIS header sets __NVIC_PRIO_BITS = 3. Leaving the
     * ARMv8-M default of 8 in place breaks the BASEPRI comparison after
     * CMSIS priority shifting (FreeRTOS critical sections stop working).
     */
    qdev_prop_set_uint8(armv7m, "num-prio-bits", 3);

    qdev_prop_set_string(armv7m, "cpu-type", ARM_CPU_TYPE_NAME("cortex-m33"));

    /*
     * The firmware's vector table sits at the start of the code region
     * (startup_qemu.S puts .vectors first, link.lds places it at the top of
     * ROM). Without this the CPU would fetch vectors from 0x0.
     */
    qdev_prop_set_uint32(armv7m, "init-svtor", s->flash_base);
    qdev_prop_set_uint32(armv7m, "init-nsvtor", s->flash_base);

    qdev_connect_clock_in(armv7m, "cpuclk", s->sysclk);
    object_property_set_link(OBJECT(&s->armv7m), "memory",
                             OBJECT(sysmem), &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(&s->armv7m), &error_fatal);

    /*
     * The DWT cycle counter.
     *
     * QEMU's M-profile CPU implements no DWT: the whole trace page at
     * 0xe0000000 is covered by armv7m's "nvic-default" region, which
     * swallows the access and reads back zero. The firmware's HAL_Delay_us_()
     * spins on DWT_CYCCNT, so a counter that never moves turns every delay
     * into an infinite loop -- and the board's HAL_PreInit calls it before
     * anything can be printed.
     *
     * It has to go into the armv7m container rather than into this board's
     * system memory. armv7m layers nvic-default over the whole PPB at the
     * same priority it gives board memory, and
     * memory_region_update_container_subregions() inserts a new subregion
     * *before* an existing one of equal priority -- so nvic-default, added
     * second, renders first and claims 0xe0001000. Anything inside board
     * memory only ever gets the gaps, which is why mapping it there left
     * DWT_CTRL reading back zero.
     *
     * Adding it here at priority 0 puts it ahead of nvic-default's -1.
     */
    {
        DeviceState *dwt = qdev_new(TYPE_ARMV7M_DWT);

        qdev_connect_clock_in(dwt, "clk", s->sysclk);
        object_property_add_child(OBJECT(machine), "dwt", OBJECT(dwt));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dwt), &error_fatal);
        memory_region_add_subregion_overlap(&s->armv7m.container,
                                            ARMV7M_DWT_BASE,
                                            sysbus_mmio_get_region(
                                                SYS_BUS_DEVICE(dwt), 0),
                                            0);
    }

    /*
     * Peripherals.
     *
     * The configuration blocks -- RCC, AON, PMUC -- carry no behaviour, so
     * they are table-driven: one sifli-regbank device per block, told which
     * table to use and where to live. The tables are in sf32lb52x-periph.c.
     */
    for (i = 0; i < sf32lb52x_num_reg_banks; i++) {
        const Sf32lb52xRegBank *b = &sf32lb52x_reg_banks[i];
        DeviceState *dev = qdev_new(TYPE_SIFLI_REGBANK);

        qdev_prop_set_string(dev, "bank", b->bank);

        /*
         * Every bank is offered the system clock; only the RCC has a hook
         * that drives it, retuning SysTick as the firmware walks the clock
         * tree. The others ignore it.
         */
        sifli_regbank_set_clock(SIFLI_REGBANK(dev), s->sysclk);

        object_property_add_child(OBJECT(machine), b->bank, OBJECT(dev));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, b->base);
    }

    /*
     * All five USARTs exist so that firmware probing them finds something.
     * Only the first is given a backend: it is the one a board wires to the
     * console, and the one "-serial" reaches through serial_hd(0).
     */
    for (i = 0; i < sf32lb52x_num_usarts; i++) {
        const Sf32lb52xUsart *u = &sf32lb52x_usarts[i];
        g_autofree char *name = g_strdup_printf("usart%u", i + 1);
        DeviceState *dev = qdev_new(TYPE_SIFLI_USART);

        if (i == 0) {
            qdev_prop_set_chr(dev, "chardev", serial_hd(0));
        }
        object_property_add_child(OBJECT(machine), name, OBJECT(dev));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, u->base);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                           qdev_get_gpio_in(armv7m, u->irq));
    }

    /*
     * Anything the firmware touches beyond this reads back as zero, because
     * mc->ignore_memory_transaction_failures is set. To find out what is
     * still missing, add create_unimplemented_device("sifli.<name>", base,
     * size) (needs hw/misc/unimp.h) and run with -d guest_errors.
     */

    /*
     * Load the firmware.
     *
     * This also registers the CPU's system reset handler; every M-profile
     * board must call it (see the comment in hw/arm/armv7m.c).
     */
    armv7m_load_kernel(s->armv7m.cpu, machine->kernel_filename,
                       s->flash_base, s->flash_size);
}

static void sifli_sf32lb52x_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m33"),
        NULL
    };

    mc->desc = "SiFli SF32LB52x (Cortex-M33)";
    mc->init = sifli_sf32lb52x_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m33");
    mc->valid_cpu_types = valid_cpu_types;
    mc->max_cpus = 1;

    /*
     * Accesses to unimplemented peripherals read back as zero instead of
     * aborting, so the firmware can probe for peripherals that QEMU does
     * not model yet. Tighten this as models are added.
     */
    mc->ignore_memory_transaction_failures = true;

    object_class_property_add(oc, "flash-base", "uint32",
                              sifli_get_flash_base, sifli_set_flash_base,
                              NULL, NULL);
    object_class_property_set_description(oc, "flash-base",
        "Base address of the boot QSPI flash XIP window");

    object_class_property_add(oc, "flash-size", "uint32",
                              sifli_get_flash_size, sifli_set_flash_size,
                              NULL, NULL);
    object_class_property_set_description(oc, "flash-size",
        "Size of the boot QSPI flash XIP window in bytes");
}

static const TypeInfo sifli_sf32lb52x_machine_typeinfo = {
    .name          = TYPE_SIFLI_SF32LB52X_MACHINE,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(Sifli52xMachineState),
    .instance_init = sifli_machine_instance_init,
    .class_init    = sifli_sf32lb52x_class_init,
    /*
     * Written out by hand rather than using DEFINE_MACHINE_ARM() because the
     * latter fixes instance_size to MachineState and we carry extra state.
     * arm_machine_interfaces is what DEFINE_MACHINE_ARM would have used --
     * and it is mandatory: a machine without it is silently absent from
     * qemu-system-arm. Upstream hit exactly that with max78000fthr
     * (commit d9dd5dad31).
     */
    .interfaces    = arm_machine_interfaces,
};

static void sifli_sf32lb52x_machine_register_types(void)
{
    type_register_static(&sifli_sf32lb52x_machine_typeinfo);
}

type_init(sifli_sf32lb52x_machine_register_types)
