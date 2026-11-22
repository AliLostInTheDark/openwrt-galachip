// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/arch/arm/mm/copypage-fa626te.c
 *
 *  Based on copypage-v6.c:
 *  Copyright (C) 2002 Deep Blue Solutions Ltd, All Rights Reserved.
 *
 *  Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * The FA626TE D-cache is VIPT and aliases, like an aliasing ARMv6 one, so
 * user pages are copied and cleared through mappings of the same cache colour
 * as the user address. The FA626TE has no MCRR range operation, so the old
 * kernel data is discarded one cache line at a time.
 */
#include <linux/highmem.h>
#include <linux/init.h>
#include <linux/pagemap.h>
#include <linux/spinlock.h>

#include <asm/cacheflush.h>
#include <asm/shmparam.h>
#include <asm/tlbflush.h>

#include "mm.h"

#define FA626TE_DCACHE_LINE	32

static DEFINE_RAW_SPINLOCK(fa626te_lock);

/*
 * Discard data in the kernel mapping for the new page. The GC1601 has only
 * lowmem, so page_address() is always valid.
 */
static void fa626te_discard_old_kernel_data(struct page *page)
{
	unsigned long addr = (unsigned long)page_address(page);
	unsigned long end = addr + PAGE_SIZE;

	for (; addr < end; addr += FA626TE_DCACHE_LINE)
		asm volatile("mcr p15, 0, %0, c7, c6, 1	@ invalidate D entry"
			     : : "r" (addr) : "memory");
}

/*
 * Copy the page, taking account of the cache colour.
 */
void fa626te_copy_user_highpage(struct page *to, struct page *from,
				unsigned long vaddr, struct vm_area_struct *vma)
{
	struct folio *src = page_folio(from);
	unsigned long offset = CACHE_COLOUR(vaddr) << PAGE_SHIFT;
	unsigned long kfrom = COPYPAGE_V6_FROM + offset;
	unsigned long kto = COPYPAGE_V6_TO + offset;
	unsigned long flags;

	if (!test_and_set_bit(PG_dcache_clean, &src->flags.f))
		__flush_dcache_folio(folio_flush_mapping(src), src);

	fa626te_discard_old_kernel_data(to);

	raw_spin_lock_irqsave(&fa626te_lock, flags);
	set_top_pte(kfrom, mk_pte(from, PAGE_KERNEL));
	set_top_pte(kto, mk_pte(to, PAGE_KERNEL));
	copy_page((void *)kto, (void *)kfrom);
	raw_spin_unlock_irqrestore(&fa626te_lock, flags);
}

/*
 * Clear the user page through a mapping of the same cache colour as the
 * user address.
 */
void fa626te_clear_user_highpage(struct page *page, unsigned long vaddr)
{
	unsigned long to = COPYPAGE_V6_TO + (CACHE_COLOUR(vaddr) << PAGE_SHIFT);
	unsigned long flags;

	fa626te_discard_old_kernel_data(page);

	raw_spin_lock_irqsave(&fa626te_lock, flags);
	set_top_pte(to, mk_pte(page, PAGE_KERNEL));
	clear_page((void *)to);
	raw_spin_unlock_irqrestore(&fa626te_lock, flags);
}

struct cpu_user_fns fa626te_user_fns __initdata = {
	.cpu_clear_user_highpage = fa626te_clear_user_highpage,
	.cpu_copy_user_highpage = fa626te_copy_user_highpage,
};
