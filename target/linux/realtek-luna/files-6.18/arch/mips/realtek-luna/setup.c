// SPDX-License-Identifier: GPL-2.0-only
/*
 * "Luna" GPON ONU (RTL960xC, RLX/Taroko core) — platform setup.
 *
 * Independent implementation from the SoC's register interface (register bases,
 * reset and watchdog programming) and mainline MIPS DT-platform conventions.
 * The generic arch/mips device_tree_init() (unflatten) is used as-is.
 * Interrupts are handled by the SoC INTC (irqchip driver) via the standard
 * irqchip_init() entry; the system tick comes from the SoC TC timer (clocksource
 * driver) because the Taroko core's CP0 Count is unreliable.
 *
 * Copyright (C) 2026 Confiared <contact@confiared.com>
 */

#include <linux/bits.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/clk-provider.h>
#include <linux/clocksource.h>
#include <linux/irqchip.h>
#include <linux/of_clk.h>
#include <linux/of.h>
#include <linux/of_fdt.h>

#include <asm/addrspace.h>
#include <asm/bootinfo.h>
#include <asm/cpu-features.h>
#include <asm/cpu-info.h>
#include <asm/mipsregs.h>
#include <asm/prom.h>
#include <asm/reboot.h>
#include <asm/time.h>

#include "luna.h"

/* SoC watchdog timer (KSEG1), TC block. */
#define LUNA_RSTWDT_CTRL		((void __iomem *)CKSEG1ADDR(0x18003268))
#define LUNA_RSTWDT_EN		BIT(31)		/* watchdog enable          */
#define LUNA_RSTWDT_CLK_SC_SHIFT	29		/* overflow scale           */
#define LUNA_RSTWDT_PH1_TO_SHIFT	22		/* phase-1 timeout          */
#define LUNA_RSTWDT_PH2_TO_SHIFT	15		/* phase-2 timeout          */
#define LUNA_RSTWDT_RST_FULLCHIP	0u		/* RESET_MODE = full chip   */

extern char __dtb_start[];

static void luna_machine_restart(char *command)
{
	local_irq_disable();
	pr_emerg("Restarting via SoC watchdog full-chip reset...\n");
	/* Full-chip reset, fastest scale */
	__raw_writel(LUNA_RSTWDT_EN |
		     (0u << LUNA_RSTWDT_CLK_SC_SHIFT) |
		     (1u << LUNA_RSTWDT_PH1_TO_SHIFT) |
		     (0u << LUNA_RSTWDT_PH2_TO_SHIFT) |
		     LUNA_RSTWDT_RST_FULLCHIP,
		     LUNA_RSTWDT_CTRL);
	while (1)
		cpu_relax();
}

static void luna_machine_halt(void)
{
	local_irq_disable();
	pr_emerg("System halted.\n");
	while (1)
		cpu_relax();
}

void __init plat_mem_setup(void)
{
	luna_mark('M');
	/* The preloader may arm the SoC hardware watchdog */
	__raw_writel(0, LUNA_RSTWDT_CTRL);

	/* MMIO/peripheral window: SPI-NOR + SoC registers. */
	ioport_resource.start = 0x14000000;
	ioport_resource.end   = 0x1fffffff;
	iomem_resource.start  = 0x14000000;
	iomem_resource.end    = 0x1fffffff;

	_machine_restart = luna_machine_restart;
	_machine_halt    = luna_machine_halt;
	pm_power_off     = luna_machine_halt;

	/* The board's bootloader passes no device tree, so the */
	__dt_setup_arch(get_fdt());
}

void __init device_tree_init(void)
{
	unflatten_and_copy_device_tree();
}

void __init plat_time_init(void)
{
	luna_mark('T');
	of_clk_init(NULL);
	timer_probe();
}

void __init arch_init_irq(void)
{
	luna_mark('I');
	irqchip_init();
	luna_mark('i');		/* irqchip/GIC probe returned */
}
