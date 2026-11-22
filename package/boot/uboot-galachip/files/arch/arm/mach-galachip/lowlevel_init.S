/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Entry of the GalaChip GC1601 (Faraday FA626TE core)
 *
 * U-Boot runs the ARM926EJ-S CPU code with its low-level init skipped: that
 * init cleans the D-cache with the ARM926 test-and-clean loop, which is not
 * verified on the FA626TE. The earlier boot stage has already set up DDR.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <asm/system.h>
#include <linux/linkage.h>

ENTRY(save_boot_params)
	mrs	r0, cpsr
	orr	r0, r0, #0xc0		/* mask IRQ and FIQ */
	msr	cpsr_c, r0

	/*
	 * The ARM926 code below cannot clean the FA626TE D-cache, so an entry
	 * with the MMU or the D-cache on would lose dirty lines: refuse it. The
	 * caller cleans and disables them first, as the SPL does.
	 */
	mrc	p15, 0, r0, c1, c0, 0
	tst	r0, #(CR_M | CR_C)
	bne	unsupported_entry

	bic	r0, r0, #(CR_I | CR_V)
	bic	r0, r0, #(CR_R | CR_S)
	bic	r0, r0, #(CR_B | CR_A)
	mcr	p15, 0, r0, c1, c0, 0
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4	/* drain the write buffer */
	mcr	p15, 0, r0, c7, c7, 0	/* invalidate the I- and D-cache */
	mcr	p15, 0, r0, c8, c7, 0	/* invalidate the TLBs */
	b	save_boot_params_ret

unsupported_entry:
	b	unsupported_entry
ENDPROC(save_boot_params)

/*
 * The mask ROM sits at address 0 and the core has no VBAR: the exception
 * vectors stay the ROM's, never copy U-Boot's over them.
 */
ENTRY(relocate_vectors)
	bx	lr
ENDPROC(relocate_vectors)
