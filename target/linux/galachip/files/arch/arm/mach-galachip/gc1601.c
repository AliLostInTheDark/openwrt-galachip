// SPDX-License-Identifier: GPL-2.0-only
#include <linux/io.h>
#include <linux/reboot.h>
#include <linux/sizes.h>
#include <asm/mach/arch.h>

#define GC1601_WDT_BASE		0x90800000
#define GC1601_WDT_LOAD		0x04
#define GC1601_WDT_RESTART	0x08
#define GC1601_WDT_CR		0x0c

static void __iomem *gc1601_wdt;

static void __init gc1601_init_machine(void)
{
	gc1601_wdt = ioremap(GC1601_WDT_BASE, SZ_4K);
}

/* Same sequence as the Faraday GC1601 BSP: system reset on a 1000-tick pclk count. */
static void gc1601_restart(enum reboot_mode mode, const char *cmd)
{
	if (!gc1601_wdt)
		return;

	writel(0, gc1601_wdt + GC1601_WDT_CR);
	writel(1000, gc1601_wdt + GC1601_WDT_LOAD);
	writel(0x3, gc1601_wdt + GC1601_WDT_CR);
	writel(0x5ab9, gc1601_wdt + GC1601_WDT_RESTART);
}

static const char *const gc1601_dt_compat[] = {
	"galachip,gc1601",
	NULL,
};

DT_MACHINE_START(GC1601_DT, "GalaChip GC1601A")
	.dt_compat = gc1601_dt_compat,
	.init_machine = gc1601_init_machine,
	.restart = gc1601_restart,
MACHINE_END
