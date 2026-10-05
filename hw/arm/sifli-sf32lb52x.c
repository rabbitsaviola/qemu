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
#include "hw/display/sifli-epic.h"
#include "hw/display/sifli-ezip.h"
#include "hw/display/sifli-lcdc.h"
#include "hw/dma/sifli-dma.h"
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
    MemoryRegion psram;

    Clock *sysclk;

    /* Boot flash window; depends on the part number. */
    uint32_t flash_base;
    uint32_t flash_size;

    /* Off-chip PSRAM, on MPI1. */
    uint32_t psram_size;

    /*
     * Path to the vendor's ezip decoder, handed to the EZIP model. Only the
     * proprietary bitstream needs it: it is the one format with no open
     * decoder, and the tool lives in the SDK, wherever that was unpacked.
     */
    char *ezip_tool;
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
    s->psram_size = SF32LB52X_PSRAM_SIZE;
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

static void sifli_get_psram_size(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    Sifli52xMachineState *s = SIFLI_SF32LB52X_MACHINE(obj);
    uint32_t value = s->psram_size;

    visit_type_uint32(v, name, &value, errp);
}

static void sifli_set_psram_size(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    Sifli52xMachineState *s = SIFLI_SF32LB52X_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    s->psram_size = value;
}

static char *sifli_get_ezip_tool(Object *obj, Error **errp)
{
    Sifli52xMachineState *s = SIFLI_SF32LB52X_MACHINE(obj);

    return g_strdup(s->ezip_tool);
}

static void sifli_set_ezip_tool(Object *obj, const char *value, Error **errp)
{
    Sifli52xMachineState *s = SIFLI_SF32LB52X_MACHINE(obj);

    g_free(s->ezip_tool);
    s->ezip_tool = g_strdup(value);
}

static void sifli_sf32lb52x_init(MachineState *machine)
{
    Sifli52xMachineState *s = SIFLI_SF32LB52X_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *armv7m;
    /* Indexed by SF32LB52X_DMA_*, so the USARTs can find their controller. */
    DeviceState *dmas[SF32LB52X_DMA_2 + 1] = { NULL };
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

    /*
     * PSRAM is plain RAM here. Bringing it up is the MPI's job -- read
     * latency, write latency, the QSPI mode -- and the HAL asks for that
     * through registers the RCC/MPI banks already answer; once the firmware
     * believes it is up, what it does with the memory is ordinary loads and
     * stores.
     *
     * The firmware's LCD framebuffer lands here too -- at 0x60400000 on the
     * a128r16 board, which is why the region starts at 0x60000000.
     */
    memory_region_init_ram(&s->psram, NULL, "sifli.psram",
                           s->psram_size, &error_fatal);
    memory_region_add_subregion(sysmem, SF32LB52X_PSRAM_BASE, &s->psram);

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
     * Both DMA controllers, with all eight channels of each.
     *
     * The requests they serve are wired up by the peripherals below, not
     * here: a controller only knows a request by its number, so it is the
     * source, which knows which number it was given, that has to say so.
     * DMAC2 is built even though nothing on this board drives it from the
     * HCPU -- the firmware still probes it, and a peripheral it does not
     * find is a peripheral it may try to bring up differently.
     */
    for (i = 0; i < sf32lb52x_num_dmas; i++) {
        const Sf32lb52xDma *d = &sf32lb52x_dmas[i];
        g_autofree char *name = g_strdup_printf("dmac%u", d->id);
        DeviceState *dev = qdev_new(TYPE_SIFLI_DMA);
        unsigned c;

        object_property_add_child(OBJECT(machine), name, OBJECT(dev));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, d->base);

        for (c = 0; c < SF32LB52X_IRQ_DMA_NUM; c++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(dev), c,
                               qdev_get_gpio_in(armv7m, d->irq + c));
        }

        dmas[d->id] = dev;
    }

    /*
     * All five USARTs exist so that firmware probing them finds something.
     * Only the first is given a backend: it is the one a board wires to the
     * console, and the one "-serial" reaches through serial_hd(0).
     *
     * The receiver of each is wired to its DMA request, which is what a
     * board's dma_config.h does on hardware. Nothing here depends on that
     * request being served: a USART whose controller is absent simply never
     * sees RDR read, which is also what happens when firmware leaves DMA
     * receive switched off.
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

        if (u->dma_ctrl != SF32LB52X_DMA_NONE) {
            qdev_connect_gpio_out_named(dev, "dma-rx", 0,
                                        qdev_get_gpio_in_named(
                                            dmas[u->dma_ctrl], "request",
                                            u->dma_rx_req));
        }
    }

    /*
     * The two graphics accelerators. Neither is touched during boot -- the
     * EPIC driver only memsets its handle in an INIT_PRE_APP_EXPORT hook,
     * and the EZIP driver does not run at all until LVGL opens the GPU -- so
     * they exist here for the sake of firmware that goes on to use them.
     *
     * The EZIP decoder path comes from the machine property, or the
     * environment when a script would rather not spell it out on the command
     * line. Neither is an error to leave unset; see hw/display/sifli-ezip.c.
     */
    {
        DeviceState *epic = qdev_new(TYPE_SIFLI_EPIC);
        DeviceState *ezip = qdev_new(TYPE_SIFLI_EZIP);
        const char *tool = s->ezip_tool;

        if (tool == NULL) {
            tool = g_getenv("SIFLI_EZIP_TOOL");
        }
        if (tool != NULL) {
            qdev_prop_set_string(ezip, "tool", tool);
        }

        object_property_add_child(OBJECT(machine), "epic", OBJECT(epic));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(epic), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(epic), 0, SF32LB52X_EPIC_BASE);
        sysbus_connect_irq(SYS_BUS_DEVICE(epic), 0,
                           qdev_get_gpio_in(armv7m, SF32LB52X_IRQ_EPIC));

        object_property_add_child(OBJECT(machine), "ezip", OBJECT(ezip));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(ezip), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(ezip), 0, SF32LB52X_EZIP1_BASE);
        sysbus_connect_irq(SYS_BUS_DEVICE(ezip), 0,
                           qdev_get_gpio_in(armv7m, SF32LB52X_IRQ_EZIP));
    }

    /*
     * The LCD controller. Its panel is not built here: a panel is a part of
     * the board rather than of the SoC, so it is whatever "-device
     * sifli-panel" the user passed, and it attaches itself to this device's
     * QSPI bus when it realizes. Without one the controller still runs and
     * still draws, sized from the layer's own rectangle, but the panel ID
     * read comes back zero and the firmware will not recognise the screen.
     */
    {
        DeviceState *lcdc = qdev_new(TYPE_SIFLI_LCDC);

        object_property_add_child(OBJECT(machine), "lcdc", OBJECT(lcdc));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(lcdc), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(lcdc), 0, SF32LB52X_LCDC1_BASE);
        sysbus_connect_irq(SYS_BUS_DEVICE(lcdc), 0,
                           qdev_get_gpio_in(armv7m, SF32LB52X_IRQ_LCDC1));
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

    object_class_property_add(oc, "psram-size", "uint32",
                              sifli_get_psram_size, sifli_set_psram_size,
                              NULL, NULL);
    object_class_property_set_description(oc, "psram-size",
        "Size of the MPI1 PSRAM in bytes (board property: mem_map.h takes it "
        "from BSP_QSPI1_MEM_SIZE)");

    /*
     * This one cannot have a default: QEMU is not run from the SDK and has
     * no way to find its tools/png2ezip directory. Unset, the EZIP model
     * still decodes gzip and LZ4; only the proprietary format stops, with a
     * guest error saying so.
     */
    object_class_property_add_str(oc, "ezip-tool",
                                  sifli_get_ezip_tool, sifli_set_ezip_tool);
    object_class_property_set_description(oc, "ezip-tool",
        "Path to the SDK's png2ezip host decoder (ezip_linux/ezip.exe), "
        "needed for the proprietary EZIP format");
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
