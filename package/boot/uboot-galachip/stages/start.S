/* SPDX-License-Identifier: GPL-2.0-only */
.syntax unified
.arm
.section .text.entry, "ax"
.global _start
_start:
	b reset
	.rept 7
	b stage_halt
	.endr
#ifdef WRAPPER
.global image_header
image_header:
	.incbin "meta.bin"
#endif
reset:
	mrs r0, cpsr
	bic r0, r0, #0x1f
	orr r0, r0, #0xd3
	msr cpsr_c, r0
#ifndef WRAPPER
	mov r0, #0
	mcr p15, 0, r0, c7, c7, 0
	mcr p15, 0, r0, c8, c7, 0
#endif
	mrc p15, 0, r0, c1, c0, 0
#ifdef WRAPPER
	tst r0, #5
	bne stage_halt
#endif
	bic r0, r0, #0x1000
	bic r0, r0, #0x2000
	bic r0, r0, #0x300
	bic r0, r0, #0x87
	mcr p15, 0, r0, c1, c0, 0
	mov r0, #0
	mcr p15, 0, r0, c7, c10, 4
	ldr sp, =__stack_top
	ldr r0, =__bss_start
	ldr r1, =__bss_end
	mov r2, #0
1:	cmp r0, r1
	strlo r2, [r0], #4
	blo 1b
	bl stage_main
stage_halt:
	b stage_halt

.global stage_jump
stage_jump:
	mov r12, r0
	mov r0, #0
	mcr p15, 0, r0, c7, c10, 4
	mcr p15, 0, r0, c7, c5, 0
	mov r1, #0
	mov r2, #0
	bx r12
#ifdef WRAPPER
.section .payload, "a"
.global compressed_start, compressed_end
compressed_start:
	.incbin "proper.lzma"
compressed_end:
#endif
.section .note.GNU-stack, "", %progbits
