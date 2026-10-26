/* SPDX-License-Identifier: GPL-2.0+ */
.syntax unified
.arm
.section .text.start, "ax"
.global _start
_start:
	mov r12, sp
	mrs r3, cpsr
	ldr sp, =0x0020a000
	push {r2-r11, r12, lr}
	cpsid aif
	mrc p15, 0, r0, c1, c0, 0
	tst r0, #0x1000
	bne unsafe
	tst r0, #5
	bne unsafe
	bl engine_probe
	b return
unsafe:
	mvn r0, #0
return:
	mov r1, r0
	pop {r2-r11, r12, lr}
	msr cpsr_c, r3
	mov sp, r12
	mov r0, r1
	bx lr
.ltorg
.section .note.GNU-stack,"",%progbits
