// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * U-Boot SPL of the GalaChip GC1601
 *
 * The SPL runs from DDR, set up by the stage before it. It loads U-Boot from
 * the SPI flash, or over YMODEM when that copy is missing or broken.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <cpu_func.h>
#include <hang.h>
#include <spl.h>
#include <asm/system.h>
#include <asm/arch/gc1601.h>
#include <linux/sizes.h>

/*
 * A flat section map just above the SPL's stack. The SPL works in the first
 * 32 MiB of DDR (YMODEM stages at 0x41000000); caching sections a smaller
 * DDR lacks is harmless, nothing reads them.
 */
#define GC1601_SPL_TTB		(CFG_SYS_SDRAM_BASE + 0x300000)
#define GC1601_SPL_DDR_SIZE	SZ_32M

/*
 * Unpacking U-Boot with the D-cache off took 2.5 s, with it on 0.12 s: LZMA
 * keeps reading its tables and the output it has written. DDR is cached
 * write-back, everything else, the registers included, is not.
 */
static void gc1601_spl_dcache_enable(void)
{
	u32 *ttb = (u32 *)GC1601_SPL_TTB;
	ulong addr;
	unsigned int i;

	for (i = 0; i < 4096; i++) {	/* 1 MiB sections */
		addr = (ulong)i << MMU_SECTION_SHIFT;
		ttb[i] = addr | TTB_SECT_AP |
			 (addr - CFG_SYS_SDRAM_BASE < GC1601_SPL_DDR_SIZE ?
			  DCACHE_WRITEBACK : DCACHE_OFF);
	}

	gc1601_dcache_enable(ttb);
}

void board_init_f(ulong dummy)
{
	/*
	 * Uncached instruction fetches from DDR are too slow to drain the UART
	 * FIFO during YMODEM. The entry code invalidated the I-cache.
	 */
	icache_enable();
	gc1601_spl_dcache_enable();

	if (spl_early_init())
		hang();

	preloader_console_init();
}

/* The next stage refuses an entry with the D-cache or the MMU on. */
void spl_board_prepare_for_boot(void)
{
	gc1601_dcache_disable();
}
