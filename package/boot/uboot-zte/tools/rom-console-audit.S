/* SPDX-License-Identifier: GPL-2.0+ */
/* Enter only the captured ROM console, never its reset or boot dispatcher. */
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
	ldr r2, =0x42003005
	tst r1, r2
	bne refused
	/* Refuse a different ROM revision/mapping before changing the stack. */
	ldr r0, =0x00000918
	ldr r1, [r0]
	ldr r2, =0xe24dd040
	cmp r1, r2
	bne refused
	ldr r1, [r0, #4]
	ldr r2, =0xe3a06000
	cmp r1, r2
	bne refused
	ldr r1, [r0, #8]
	ldr r2, =0xe1a0500d
	cmp r1, r2
	bne refused
	ldr r1, [r0, #12]
	ldr r2, =0xe3a04000
	cmp r1, r2
	bne refused
	ldr sp, =0x00204000
	push {r2-r12, lr}
	cpsid if
	/* Console has no normal exit; ROM command g 200100 reaches resume. */
	blx r0
	b resume
refused:
	mvn r0, #0
	bx lr
.ltorg

.org 0x100
.global resume
resume:
	ldr sp, =0x00203fd0
	pop {r2-r12, lr}
	mov sp, r12
	mov r0, #0
	msr cpsr_cxsf, r3
	bx lr
.ltorg
.section .note.GNU-stack, "", %progbits
