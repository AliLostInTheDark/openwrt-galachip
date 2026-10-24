/* SPDX-License-Identifier: GPL-2.0+ */
.syntax unified
.arch armv7-a
.arm
.equ RECORD, 0x00200e00
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
#ifdef ZX_READ_ALIGNMENT_TEST
	/* Deliberate fault at known SRAM; require alignment checking already on. */
	tst r1, #2
	beq refused
#endif
	ldr sp, =0x0020a000
	push {r2-r12, lr}
	cpsid if
	mrc p15, 0, r4, c12, c0, 0
	cps #0x17
	mov r6, sp
	mov r7, lr
	mrs r8, spsr
	ldr sp, =0x00200d00
	cps #0x13
	ldr r1, =RECORD
	mov r0, #0
	str r0, [r1, #8]
	str r0, [r1, #12]
	str r0, [r1, #16]
	str r0, [r1, #20]
	str r0, [r1, #24]
	ldr r9, [r1]
	ldr r10, [r1, #4]
#ifdef ZX_READ_ALIGNMENT_TEST
	ldr r2, =0x00200001
	cmp r9, r2
	bne bad_input
	cmp r10, #1
	bne bad_input
#else
	tst r9, #3
	bne bad_input
	cmp r10, #1
	blo bad_input
	cmp r10, #64
	bhi bad_input
	adds r2, r9, r10, lsl #2
	bcs bad_input
#endif
	ldr r2, =vectors
	dsb sy
	mcr p15, 0, r2, c12, c0, 0
	isb
	add r11, r1, #0x40
read_next:
.global probe_load
probe_load:
	ldr r2, [r9]
	ldr r0, [r1, #8]
	cmp r0, #0
	bne restore
	str r2, [r11], #4
	add r9, r9, #4
	ldr r0, [r1, #12]
	add r0, r0, #1
	str r0, [r1, #12]
	subs r10, r10, #1
	bne read_next
	b restore
bad_input:
	mov r0, #2
	str r0, [r1, #8]
restore:
	ldr r0, [r1, #8]
	dsb sy
	mcr p15, 0, r4, c12, c0, 0
	isb
	cps #0x17
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
.balign 32
vectors:
	b unexpected
	b unexpected
	b unexpected
	b unexpected
	b data_abort
	b unexpected
	b unexpected
	b unexpected
data_abort:
	push {r0-r3}
	sub r0, lr, #8
	ldr r1, =probe_load
	cmp r0, r1
	bne unexpected
	ldr r1, =RECORD
	str r0, [r1, #24]
	mrc p15, 0, r2, c5, c0, 0
	str r2, [r1, #16]
	mrc p15, 0, r2, c6, c0, 0
	str r2, [r1, #20]
	mov r2, #1
	str r2, [r1, #8]
	pop {r0-r3}
	/* Skip only the faulting load; exception return restores SVC CPSR. */
	subs pc, lr, #4
unexpected:
	b unexpected
.ltorg
.section .note.GNU-stack, "", %progbits
