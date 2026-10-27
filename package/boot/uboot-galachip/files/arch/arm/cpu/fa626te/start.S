/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FA626TE ARMv5TE entry. DDR is owned by the preceding SPL in both variants. */
#include <config.h>
#include <linux/linkage.h>

.arm
.globl reset
reset:
	mrs	r0, cpsr
	bic	r0, r0, #0x1f
	orr	r0, r0, #0xd3
	msr	cpsr_c, r0
	mrc	p15, 0, r0, c1, c0, 0
	/* No verified clean-by-index operation: do not discard dirty cache lines. */
	tst	r0, #5
	bne	unsupported_entry
	bic	r0, r0, #0x1000
	bic	r0, r0, #0x2000
	bic	r0, r0, #0x300
	bic	r0, r0, #0x82
	mcr	p15, 0, r0, c1, c0, 0
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4
	mcr	p15, 0, r0, c7, c7, 0
	mcr	p15, 0, r0, c8, c7, 0
	bl	_main
unsupported_entry:
	/* The caller must disable/clean its MMU and D-cache before entry. */
	b	unsupported_entry

.globl c_runtime_cpu_setup
c_runtime_cpu_setup:
	bx	lr

/* No VBAR and ROM at zero: never copy the vectors into read-only ROM. */
.globl relocate_vectors
relocate_vectors:
	bx	lr
.section .note.GNU-stack, "", %progbits
