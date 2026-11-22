// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * U-Boot TPL of the GalaChip GC1601
 *
 * The mask ROM runs the TPL from SRAM. It sets up DDR and loads the SPL
 * from the SPI flash, without driver model, or over YMODEM when the copy in
 * the flash is broken. Its stack and BSS move to DDR before board_init_r().
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <cpu_func.h>
#include <hang.h>
#include <spl.h>
#include <version.h>
#include <asm/io.h>
#include <asm/arch/gc1601.h>
#include <asm/arch/sdram.h>
#include <linux/bitops.h>
#include <faraday/ftwdt010_wdt.h>

#define TIMER2_COUNT		0x10
#define TIMER2_LOAD		0x14
#define TIMER2_MATCH1		0x18
#define TIMER2_MATCH2		0x1c
#define TIMER_CR		0x30
#define TIMER_INTR_STATE	0x34
#define TIMER_INTR_MASK		0x38
#define TIMER_2_CR_ENABLE	BIT(3)
#define TIMER_2_CR_ALL		(TIMER_2_CR_ENABLE | BIT(4) | BIT(5) | BIT(10))
#define TIMER_2_INT_ALL		GENMASK(5, 3)

void board_init_f(ulong dummy)
{
	struct ftwdt010_wdt __iomem *wdt = (void __iomem *)GC1601_WDT_BASE;
	int ret;

	/* An earlier boot may have left the watchdog running: stop it. */
	writel(0, &wdt->wdcr);

	/* As in the SPL: the entry code invalidated it, the D-cache stays off. */
	icache_enable();

	/* The console keeps its state in BSS, which is in DDR. */
	ret = gc1601_sdram_init();
	preloader_console_init();
	/*
	 * TPL_BANNER_PRINT depends on DEBUG_UART, which this TPL does not use.
	 * Without the build date, which the SPL prints: the OpenWrt revision
	 * in the version varies in length and the mask ROM's limit is close.
	 */
	puts("\nU-Boot " PHASE_NAME " " PLAIN_VERSION "\n");
	if (ret) {
		puts("DDR setup failed\n");
		hang();
	}
}

/*
 * lib/time.c reads FTTMR010 timer 2 as CFG_SYS_TIMER_COUNTER: run it as the
 * fttmr010 driver does, counting down from the top on PCLK.
 */
int timer_init(void)
{
	void __iomem *base = (void __iomem *)GC1601_TIMER_BASE;

	clrbits_le32(base + TIMER_CR, TIMER_2_CR_ALL);
	setbits_le32(base + TIMER_INTR_MASK, TIMER_2_INT_ALL);
	writel(TIMER_2_INT_ALL, base + TIMER_INTR_STATE);
	writel(0, base + TIMER2_MATCH1);
	writel(0, base + TIMER2_MATCH2);
	writel(~0, base + TIMER2_LOAD);
	writel(~0, base + TIMER2_COUNT);
	setbits_le32(base + TIMER_CR, TIMER_2_CR_ENABLE);

	return 0;
}
