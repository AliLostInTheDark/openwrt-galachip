/* SPDX-License-Identifier: GPL-2.0+ */
/* Read the privileged ARMv7 state into a bounded initialized-DRAM buffer. */
.syntax unified
.arch armv7-a
.arm
.section .text.start, "ax"
.global _start
_start:
	mrs r3, cpsr
	ldr r0, =0x40530000
	str r3, [r0, #4]
	str sp, [r0, #8]
	str lr, [r0, #12]
	mov r1, pc
	str r1, [r0, #16]
.macro sample offset, crn, crm, opcode
	mrc p15, 0, r1, \crn, \crm, \opcode
	str r1, [r0, #\offset]
.endm
	sample 20, c0, c0, 0
	sample 24, c0, c0, 5
	sample 28, c1, c0, 0
	sample 32, c1, c0, 1
	sample 36, c2, c0, 0
	sample 40, c2, c0, 1
	sample 44, c2, c0, 2
	sample 48, c3, c0, 0
	sample 52, c12, c0, 0
	sample 56, c5, c0, 0
	sample 60, c5, c0, 1
	sample 64, c6, c0, 0
	sample 68, c6, c0, 2
	ldr r1, =0x534e5031
	str r1, [r0]
	dsb sy
	mov r0, #0
	bx lr
.ltorg
.section .note.GNU-stack, "", %progbits
