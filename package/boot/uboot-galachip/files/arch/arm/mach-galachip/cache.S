/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * D-cache of the GalaChip GC1601 (Faraday FA626TE core)
 *
 * The FA626TE cleans its whole D-cache with one CP15 operation, the one
 * Linux's cache-fa.S uses, not with the ARM926 test-and-clean loop. In ARM
 * code because the SPL is Thumb-1, which has no coprocessor instructions.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <asm/system.h>
#include <linux/linkage.h>

#ifdef CONFIG_XPL_BUILD
/* r0: 16 KiB-aligned section table. Turns the MMU and the D-cache on. */
ENTRY(gc1601_dcache_enable)
	mov	r1, #0
	mcr	p15, 0, r1, c7, c10, 4	/* drain the write buffer: the table is in DDR */
	mcr	p15, 0, r0, c2, c0, 0	/* translation table base */
	mvn	r1, #0
	mcr	p15, 0, r1, c3, c0, 0	/* every domain manager: no permission checks */
	mov	r1, #0
	mcr	p15, 0, r1, c8, c7, 0	/* invalidate the TLBs */
	mcr	p15, 0, r1, c7, c6, 0	/* invalidate the D-cache */
	mrc	p15, 0, r1, c1, c0, 0
	orr	r1, r1, #(CR_M | CR_C)
	mcr	p15, 0, r1, c1, c0, 0
	bx	lr
ENDPROC(gc1601_dcache_enable)

/* Writes the D-cache back to DDR, then turns it and the MMU off. */
ENTRY(gc1601_dcache_disable)
	mov	r0, #0
	mcr	p15, 0, r0, c7, c14, 0	/* clean and invalidate the D-cache */
	mcr	p15, 0, r0, c7, c10, 4	/* drain the write buffer */
	mrc	p15, 0, r1, c1, c0, 0
	bic	r1, r1, #(CR_M | CR_C)
	mcr	p15, 0, r1, c1, c0, 0
	mcr	p15, 0, r0, c7, c5, 0	/* invalidate the I-cache */
	mcr	p15, 0, r0, c8, c7, 0	/* invalidate the TLBs */
	bx	lr
ENDPROC(gc1601_dcache_disable)
#endif

#if !CONFIG_IS_ENABLED(SYS_DCACHE_OFF)
/* Replaces the ARM926 test-and-clean loop of U-Boot proper. */
ENTRY(flush_dcache_all)
	mov	r0, #0
	mcr	p15, 0, r0, c7, c14, 0	/* clean and invalidate the D-cache */
	mcr	p15, 0, r0, c7, c10, 4	/* drain the write buffer */
	bx	lr
ENDPROC(flush_dcache_all)
#endif
