// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2002 Deep Blue Solutions Ltd, All Rights Reserved.
 * Copyright (C) 2005 Faraday Corp.
 * Copyright (C) 2008-2009 Paulius Zaleckas <paulius.zaleckas@teltonika.lt>
 * Adapted from copypage-v6.c and the Faraday GPL page-copy implementation.
 */
#include <linux/highmem.h>
#include <linux/init.h>
#include <linux/pagemap.h>
#include <linux/spinlock.h>

#include <asm/cacheflush.h>
#include <asm/shmparam.h>
#include <asm/tlbflush.h>

#include "mm.h"

static DEFINE_RAW_SPINLOCK(fa626te_copy_lock);

static void fa626te_discard_kernel_data(struct page *page)
{
	unsigned long addr = (unsigned long)page_address(page);
	unsigned long end = addr + PAGE_SIZE;

	for (; addr < end; addr += 32)
		asm volatile("mcr p15, 0, %0, c7, c6, 1"
			     : : "r" (addr) : "memory");
}

void fa626te_copy_user_highpage(struct page *to, struct page *from,
			      unsigned long vaddr, struct vm_area_struct *vma)
{
	struct folio *src = page_folio(from);
	unsigned long offset = CACHE_COLOUR(vaddr) << PAGE_SHIFT;
	unsigned long flags;

	if (!test_and_set_bit(PG_dcache_clean, &src->flags.f))
		__flush_dcache_folio(folio_flush_mapping(src), src);

	/* GC1601 has only lowmem; colour both temporary mappings together. */
	fa626te_discard_kernel_data(to);
	raw_spin_lock_irqsave(&fa626te_copy_lock, flags);
	set_top_pte(COPYPAGE_V6_FROM + offset, mk_pte(from, PAGE_KERNEL));
	set_top_pte(COPYPAGE_V6_TO + offset, mk_pte(to, PAGE_KERNEL));
	copy_page((void *)(COPYPAGE_V6_TO + offset),
		  (void *)(COPYPAGE_V6_FROM + offset));
	raw_spin_unlock_irqrestore(&fa626te_copy_lock, flags);
}

void fa626te_clear_user_highpage(struct page *page, unsigned long vaddr)
{
	unsigned long to = COPYPAGE_V6_TO + (CACHE_COLOUR(vaddr) << PAGE_SHIFT);
	unsigned long flags;

	fa626te_discard_kernel_data(page);
	raw_spin_lock_irqsave(&fa626te_copy_lock, flags);
	set_top_pte(to, mk_pte(page, PAGE_KERNEL));
	clear_page((void *)to);
	raw_spin_unlock_irqrestore(&fa626te_copy_lock, flags);
}

struct cpu_user_fns fa626te_user_fns __initdata = {
	.cpu_clear_user_highpage = fa626te_clear_user_highpage,
	.cpu_copy_user_highpage = fa626te_copy_user_highpage,
};
