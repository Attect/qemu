/*
 * Noah NP1380 board support
 *
 * Copyright (c) 2024 Norman Zhi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/datadir.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
#include "hw/mips/mips.h"
#include "hw/char/serial.h"
#include "system/system.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "qemu/error-report.h"
#include "system/qtest.h"
#include "system/reset.h"
#include "qemu/log.h"

#include "hw/mips/ingenic_jz4740.h"
#include "hw/block/ingenic_emc.h"
#include "hw/input/fixed_irq.h"
#include "hw/input/gpio_matrix_keypad.h"

#include "elf.h"
#include "hw/mips/bootloader.h"

#define FIRMWARE_UPGRADE    0

/* SDRAM layout (JZ4740 memory map):
 *   SDRAM at physical 0x20000000, size 0x08000000 (128MB)
 *   Aliased at physical 0x00000000 for MIPS kseg0/kseg1 access
 *
 * Kernel boot convention (MIPS):
 *   a0 = argc (1)
 *   a1 = argv (pointer to cmdline in kseg0)
 *   a2 = envp (NULL)
 *   PC jumps to kernel entry (kseg0 virtual address)
 */

/* SDRAM alias at physical 0x00000000 for kseg0/kseg1 access */
static MemoryRegion *sdram_kseg0_alias;

/* Kernel boot mode flag */
static bool kernel_boot_mode;

/* CPU reset handler: clear BEV/ERL for direct kernel boot */
static void np1380_cpu_reset(void *opaque)
{
    MIPSCPU *cpu = opaque;
    CPUMIPSState *env = &cpu->env;

    cpu_reset(CPU(cpu));
    if (kernel_boot_mode) {
        /* Clear BEV and ERL so the kernel uses normal exception vectors
         * (0x80000000 for EXL=0, BEV=0) instead of boot vector 0xbfc00000.
         * This is required because the bootloader at 0xbfc00000 is only
         * a trampoline; once the kernel starts, exceptions should go to
         * the kernel's own exception handlers. */
        env->CP0_Status &= ~((1 << CP0St_BEV) | (1 << CP0St_ERL));
    }
}

static void mips_noah_np1380_init(MachineState *machine)
{
    IngenicJZ4740 *soc = ingenic_jz4740_init(machine);

    // Register SDRAM at DCS 0
    IngenicEmcSdram *sdram = INGENIC_EMC_SDRAM(qdev_new(TYPE_INGENIC_EMC_SDRAM));
    object_property_set_uint(OBJECT(sdram), "cs", 0, &error_fatal);
    object_property_set_uint(OBJECT(sdram), "size", 0x08000000, &error_fatal);
    qdev_realize_and_unref(DEVICE(sdram), NULL, &error_fatal);

    // Register NAND at CS 1
    IngenicEmcNand *nand = INGENIC_EMC_NAND(qdev_new(TYPE_INGENIC_EMC_NAND));
    object_property_set_uint(OBJECT(nand), "cs",          1,            &error_fatal);
    object_property_set_str( OBJECT(nand), "nand-id",     "ecd514b674", &error_fatal);
    object_property_set_uint(OBJECT(nand), "block-pages", 128,          &error_fatal);
    object_property_set_uint(OBJECT(nand), "page-size",   4096,         &error_fatal);
    object_property_set_uint(OBJECT(nand), "oob-size",    128,          &error_fatal);
    qdev_realize_and_unref(DEVICE(nand), NULL, &error_fatal);

    // Connect GPIOs
    // PB27: MSC CD, 1: inserted
    qdev_connect_gpio_out_named(DEVICE(soc->msc), "io-cd", 0,
        qdev_get_gpio_in_named(DEVICE(soc->gpio['B' - 'A']), "gpio-in", 27));

    // Keypad matrix
    GpioMatrixKeypad *kp = GPIO_MATRIX_KEYPAD(qdev_new(TYPE_GPIO_MATRIX_KEYPAD));
    object_property_set_uint(OBJECT(kp), "num-rows", 3, &error_fatal);
    object_property_set_uint(OBJECT(kp), "num-cols", 4, &error_fatal);
    object_property_set_uint(OBJECT(kp), "num-pins", 32, &error_fatal);
    // Attach pull-ups to all rows and cols
    object_property_set_uint(OBJECT(kp), "row-pull", 0xffffffff, &error_fatal);
    object_property_set_uint(OBJECT(kp), "row-pull-value", 0xffffffff, &error_fatal);
    object_property_set_uint(OBJECT(kp), "col-pull", 0xffffffff, &error_fatal);
    object_property_set_uint(OBJECT(kp), "col-pull-value", 0xffffffff, &error_fatal);
    object_property_set_uint(OBJECT(kp), "pin-invert", 0, &error_fatal);
    object_property_set_uint(OBJECT(kp), "pin-pull", 0xffffffff, &error_fatal);
    object_property_set_uint(OBJECT(kp), "pin-pull-value", 0xffffffff, &error_fatal);
    qdev_realize_and_unref(DEVICE(kp), NULL, &error_fatal);

    // Keypad IO connections
    const struct {
        bool row;
        char group;
        uint8_t pin;
    } kp_ios[] = {
        { true, 'D',  2},   // LT7
        { true, 'D',  3},   // LT6
        { true, 'D',  7},   // RT7
#if !FIRMWARE_UPGRADE
        {false, 'D',  1},   // LT3/RT4
#endif
        {false, 'D', 17},   // LT4/RT5
        {false, 'D', 15},   // LT5/RT6
        {false, 'D',  0},   // RT3
    };
    int i_row = 0, i_col = 0;
    for (int i = 0; i < ARRAY_SIZE(kp_ios); i++) {
        const char *name = kp_ios[i].row ? "row-in" : "col-in";
        int *pi_kp = kp_ios[i].row ? &i_row : &i_col;
        qemu_irq irq = qdev_get_gpio_in_named(DEVICE(kp), name, *pi_kp);
        qdev_connect_gpio_out_named(DEVICE(soc->gpio[kp_ios[i].group - 'A']), "gpio-out", kp_ios[i].pin, irq);
        name = kp_ios[i].row ? "row-out" : "col-out";
        irq = qdev_get_gpio_in_named(DEVICE(soc->gpio[kp_ios[i].group - 'A']), "gpio-in", kp_ios[i].pin);
        qdev_connect_gpio_out_named(DEVICE(kp), name, *pi_kp, irq);
        *pi_kp += 1;
    }

#if FIRMWARE_UPGRADE
    // Fixed GPIO for triggering firmware upgrade
    FixedIrq *fixed = FIXED_IRQ(qdev_new(TYPE_FIXED_IRQ));
    object_property_set_int(OBJECT(fixed), "irq-value", 0, &error_fatal);
    qdev_realize_and_unref(DEVICE(fixed), NULL, &error_fatal);

    // PD1 is keypad RIGHT
    qdev_connect_gpio_out(DEVICE(fixed), 0,
        qdev_get_gpio_in_named(DEVICE(soc->gpio['D' - 'A']), "gpio-in", 1));
#endif

    // PD29: POWER key, 0: pressed
    qdev_connect_gpio_out_named(DEVICE(kp), "pin-out", 0,
        qdev_get_gpio_in_named(DEVICE(soc->gpio['D' - 'A']), "gpio-in", 29));
    // PB30: Charging status, 1: charging done
    qdev_connect_gpio_out_named(DEVICE(kp), "pin-out", 1,
        qdev_get_gpio_in_named(DEVICE(soc->gpio['B' - 'A']), "gpio-in", 30));
    // PB29: USB device port, 1: connected
    qdev_connect_gpio_out_named(DEVICE(kp), "pin-out", 2,
        qdev_get_gpio_in_named(DEVICE(soc->gpio['B' - 'A']), "gpio-in", 29));
    // PB27: SD card, 1: inserted
    qdev_connect_gpio_out_named(DEVICE(kp), "pin-out", 3,
        qdev_get_gpio_in_named(DEVICE(soc->gpio['B' - 'A']), "gpio-in", 27));
    // PC23: LCD select, 0: KD035G6, 1: PT035TN01_V5
    qdev_connect_gpio_out_named(DEVICE(kp), "pin-out", 31,
        qdev_get_gpio_in_named(DEVICE(soc->gpio['C' - 'A']), "gpio-in", 23));

    /*
     * Create SDRAM alias at physical 0x00000000 so that MIPS kseg0/kseg1
     * addresses correctly reach the SDRAM:
     *   kseg0: 0x80000000-0x9fffffff → physical 0x00000000-0x1fffffff
     *   kseg1: 0xa0000000-0xbfffffff → physical 0x00000000-0x1fffffff
     *
     * The JZ4740 EMC places SDRAM at physical 0x20000000.  Without this
     * alias, kseg0 addresses would not reach the SDRAM, breaking standard
     * MIPS kernel execution.
     *
     * Note: The cached SRAM (16KB at physical 0x00000000) has higher
     * priority (overlap = 1), so it takes precedence at 0x00000000-
     * 0x00003fff.  The alias fills the rest (0x00004000-0x07ffffff).
     */
    MemoryRegion *sys_mem = get_system_memory();
    sdram_kseg0_alias = g_new(MemoryRegion, 1);
    memory_region_init_alias(sdram_kseg0_alias, NULL, "sdram.kseg0",
                             &sdram->mr, 0, 0x08000000);
    memory_region_add_subregion(sys_mem, 0, sdram_kseg0_alias);

    /* Register CPU reset handler for kernel boot mode */
    qemu_register_reset(np1380_cpu_reset, soc->cpu);

    /* Handle -kernel / -append if requested */
    if (machine->kernel_filename) {
        uint64_t kernel_entry, kernel_low, kernel_high;
        long kernel_size;

        kernel_boot_mode = true;

        /* Load ELF kernel.
         * MIPS kernel ELF is typically linked for kseg0 (e.g., 0x80100000).
         * cpu_mips_kseg0_to_phys translates kseg0 → physical 0x00000000,
         * which our alias redirects to the actual SDRAM. */
        kernel_size = load_elf(machine->kernel_filename, NULL,
                               cpu_mips_kseg0_to_phys, NULL,
                               &kernel_entry, &kernel_low, &kernel_high,
                               NULL, ELFDATA2LSB, EM_MIPS, 1, 0);
        if (kernel_size < 0) {
            error_report("could not load kernel '%s': %s",
                         machine->kernel_filename,
                         load_elf_strerror(kernel_size));
            return;
        }

        /* Store cmdline at SDRAM alias + 128MB - 64KB
         * Physical: 0x07ff0000
         * kseg0:    0x87ff0000 */
        const char *cmdline = machine->kernel_cmdline ?: "";
        hwaddr cmdline_phys = 0x07ff0000;
        rom_add_blob_fixed("cmdline", cmdline, strlen(cmdline) + 1,
                           cmdline_phys);

        /* Generate bootloader code at reset vector (0xbfc00000 kseg1,
         * physical 0x1fc00000) that sets up kernel arguments and jumps
         * to the kernel entry.
         *
         * Arguments:
         *   a0 = argc = 1
         *   a1 = argv = kseg0 address of cmdline (0x87ff0000)
         *   a2 = envp = NULL
         *   SP = kseg0 address near SDRAM top (0x87fc0000) */
        uint8_t bl_code[1024];
        void *p = bl_code;

        bl_gen_jump_kernel(&p,
                           true,  0x87fc0000,   /* set SP */
                           true,  1,            /* set a0 = argc = 1 */
                           true,  0x87ff0000,   /* set a1 = argv = cmdline */
                           true,  0,            /* set a2 = envp = NULL */
                           false, 0,            /* don't set a3 */
                           kernel_entry);

        size_t bl_size = (uint8_t *)p - bl_code;
        rom_add_blob_fixed("bootloader", bl_code, bl_size, 0x1fc00000);
    }
}

static void mips_noah_np1380_machine_init(MachineClass *mc)
{
    mc->desc = "MIPS Noah NP1380 platform";
    mc->init = mips_noah_np1380_init;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("JZ4740");
    mc->default_ram_id = "unused";
    mc->default_ram_size = 0;
}

DEFINE_MACHINE("noah_np1380", mips_noah_np1380_machine_init)
