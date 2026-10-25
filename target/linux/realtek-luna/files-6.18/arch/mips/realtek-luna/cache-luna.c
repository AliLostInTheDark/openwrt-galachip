// SPDX-License-Identifier: GPL-2.0-only
/* RLX page-level cache hooks: the D-cache is physically indexed (a store through one
 * page colour reads back through the other: ONU-test-case/cache_coherency.c), so only
 * executable mappings keep the whole-cache blast and a kernel-written page is written
 * back by line. */
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/printk.h>
#include <asm/cacheflush.h>
#include <asm/cpu-info.h>
#include <asm/r4kcache.h>

static void (*luna_blast_page)(struct vm_area_struct *vma, unsigned long addr,
			       unsigned long pfn);
static void (*luna_blast_range)(struct vm_area_struct *vma, unsigned long start,
				unsigned long end);

static void luna_flush_cache_page(struct vm_area_struct *vma, unsigned long addr,
				  unsigned long pfn)
{
	if (vma->vm_flags & VM_EXEC)
		luna_blast_page(vma, addr, pfn);
}

static void luna_flush_cache_range(struct vm_area_struct *vma, unsigned long start,
				   unsigned long end)
{
	if (vma->vm_flags & VM_EXEC)
		luna_blast_range(vma, start, end);
}

static void luna_flush_data_cache_page(unsigned long addr)
{
	blast_dcache32_page(addr & PAGE_MASK);
}

static int __init luna_cache_hooks_init(void)
{
	if (current_cpu_data.dcache.linesz != 32) {
		pr_warn("luna-cache: D-cache line %u, not 32: whole-cache flushes kept\n",
			current_cpu_data.dcache.linesz);
		return 0;
	}
	luna_blast_page = flush_cache_page;
	luna_blast_range = flush_cache_range;
	flush_cache_page = luna_flush_cache_page;
	flush_cache_range = luna_flush_cache_range;
	flush_data_cache_page = luna_flush_data_cache_page;
	return 0;
}
early_initcall(luna_cache_hooks_init);
