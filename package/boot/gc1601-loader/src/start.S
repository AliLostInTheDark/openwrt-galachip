/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Entered from the GC1601 mask ROM, which copies this image to on-chip SRAM
 * at 0x80000000 and jumps to it with the MMU and caches in reset state.
 */

	.section .text.start, "ax"
	.arm
	.global _start
_start:
	b	reset
	b	hang		/* undefined */
	b	hang		/* swi */
	b	hang		/* prefetch abort */
	b	hang		/* data abort */
	b	hang		/* reserved */
	b	hang		/* irq */
	b	hang		/* fiq */

reset:
	/* SVC mode, IRQ and FIQ off */
	mrs	r0, cpsr
	bic	r0, r0, #0x1f
	orr	r0, r0, #0xd3
	msr	cpsr_c, r0

	/* As the stock SPL: invalidate caches and TLBs, drain the write buffer */
	mov	r0, #0
	mcr	p15, 0, r0, c7, c7, 0
	mcr	p15, 0, r0, c8, c7, 0
	mcr	p15, 0, r0, c7, c10, 4

	/* MMU, D-cache, alignment checks and high vectors off; I-cache on */
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #0x2000
	bic	r0, r0, #0x0300
	bic	r0, r0, #0x0087
	orr	r0, r0, #0x1000
	mcr	p15, 0, r0, c1, c0, 0

	ldr	sp, =__stack_top

	ldr	r0, =__bss_start
	ldr	r1, =__bss_end
	mov	r2, #0
1:	cmp	r0, r1
	strlo	r2, [r0], #4
	blo	1b

	bl	main
hang:
	b	hang

/*
 * jump(entry, r0, r1, r2): I-cache off and invalidated, then enter the
 * next stage as Linux expects it (MMU and D-cache already off).
 */
	.global jump
jump:
	mov	r12, r0
	mov	r0, r1
	mov	r1, r2
	mov	r2, r3
	mrc	p15, 0, r3, c1, c0, 0
	bic	r3, r3, #0x1000
	mcr	p15, 0, r3, c1, c0, 0
	mov	r3, #0
	mcr	p15, 0, r3, c7, c5, 0
	bx	r12
