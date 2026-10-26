/* SPDX-License-Identifier: GPL-2.0+ */
.syntax unified
.arch armv7-a
.arm
.section .text.start, "ax"
.global _start
_start:
	mov r12, sp
	mrs r3, cpsr
	and r1, r3, #0x1f
	cmp r1, #0x13
	bne refused
	mrc p15, 0, r1, c1, c0, 0
	tst r1, #5
	bne refused
	tst r1, #0x1000
	bne refused
	/* VBAR requires low ARM vectors with little-endian exception state. */
	tst r1, #0x2000
	bne refused
	tst r1, #0x42000000
	bne refused
	ldr sp, =0x0020a000
	push {r2-r12, lr}
	cpsid if
	mrc p15, 0, r4, c12, c0, 0
	/* Preserve the bank that the deliberate undefined instruction will use. */
	cps #0x1b
	mov r6, sp
	mov r7, lr
	mrs r8, spsr
	cps #0x13
	ldr r1, =0x00200e00
	mov r0, #0
	str r0, [r1]
	ldr r2, =zx279125_early_vectors
	dsb sy
	mcr p15, 0, r2, c12, c0, 0
	isb
.global audit_undefined
audit_undefined:
	.word 0xe7f000f0
.global audit_resume
audit_resume:
	ldr r2, [r1]
	ldr r3, =0x46544c31
	cmp r2, r3
	bne failed
	ldr r2, [r1, #4]
	cmp r2, #1
	bne failed
	mov r0, #0
	b restore
failed:
	mvn r0, #0
restore:
	dsb sy
	mcr p15, 0, r4, c12, c0, 0
	isb
	cps #0x1b
	mov sp, r6
	mov lr, r7
	msr spsr_cxsf, r8
	cps #0x13
	pop {r2-r12, lr}
	mov sp, r12
	msr cpsr_cxsf, r3
	bx lr
refused:
	mvn r0, #0
	bx lr
.ltorg
