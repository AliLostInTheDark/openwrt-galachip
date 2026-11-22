// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Data cache of U-Boot proper on the GalaChip GC1601
 *
 * DDR is cached write-back, except the packet engine's fixed DMA area: the
 * Ethernet driver reaches its descriptors and buffers there with plain loads,
 * stores and memcpy, so they must not be cached.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <command.h>
#include <cpu_func.h>
#include <asm/cache.h>
#include <asm/global_data.h>
#include <asm/system.h>
#include <asm/u-boot-arm.h>
#include <asm/arch/gc1601.h>
#include <linux/kernel.h>

DECLARE_GLOBAL_DATA_PTR;

void dram_bank_mmu_setup(int bank)
{
	ulong first = gd->dram[bank].start >> MMU_SECTION_SHIFT;
	ulong last = first + (gd->dram[bank].size >> MMU_SECTION_SHIFT);
	ulong dma_first = GC1601_ETH_DMA_BASE >> MMU_SECTION_SHIFT;
	ulong dma_last = DIV_ROUND_UP(GC1601_ETH_DMA_BASE + GC1601_ETH_DMA_SIZE, MMU_SECTION_SIZE);
	ulong i;

	/* gd->dram is set only after relocation */
	if (!(gd->flags & GD_FLG_RELOC))
		return;

	for (i = first; i < last; i++)
		set_section_dcache(i, i >= dma_first && i < dma_last ?
				   DCACHE_OFF : DCACHE_DEFAULT_OPTION);
}

/*
 * What go starts is often another U-Boot, whose entry refuses the D-cache on.
 * Write the cache back and turn it off first, as bootz does.
 */
unsigned long do_go_exec(ulong (*entry)(int, char * const []), int argc,
			 char *const argv[])
{
	cleanup_before_linux();

	return entry(argc, argv);
}
