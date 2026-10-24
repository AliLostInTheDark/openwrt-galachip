/* SPDX-License-Identifier: GPL-2.0+ */
/* Cold-stage support only; not a RAM application or ROM-packaged image. */
.syntax unified
.arch armv7-a
.arm

#ifndef ZX_EARLY_TRACE
#define ZX_EARLY_TRACE 0x00200e00
#endif
.equ TRACE, ZX_EARLY_TRACE
#ifndef ZX_EARLY_FAULT_STACK
#define ZX_EARLY_FAULT_STACK 0x0020a000
#endif
.equ STACK_TOP, ZX_EARLY_FAULT_STACK

.section .text.early_cpu, "ax"
.global zx279125_early_cpu_init
.type zx279125_early_cpu_init, %function
zx279125_early_cpu_init:
	mov r1, pc
	and r1, r1, #0xf0000000
	cmp r1, #0x40000000
	beq refused
	and r1, sp, #0xf0000000
	cmp r1, #0x40000000
	beq refused
	mrs r1, cpsr
	and r1, r1, #0x1f
	cmp r1, #0x13
	bne refused
	mrc p15, 0, r1, c1, c0, 0
	tst r1, #5
	bne refused
	tst r1, #0x42000000
	bne refused
	/* A running controller must never enter this cold-start helper. */
	ldr r2, =0x9430000c
	ldr r2, [r2]
	cmp r2, #0
	bne refused
	cpsid if
	mov r0, #0
	mcr p15, 0, r0, c8, c7, 0
	mcr p15, 0, r0, c7, c5, 0
	mcr p15, 0, r0, c7, c5, 6
	dsb sy
	isb
	bic r1, r1, #0x2000
	bic r1, r1, #7
	orr r1, r1, #0x1800
	orr r1, r1, #2
	mcr p15, 0, r1, c1, c0, 0
	isb
	ldr r2, =zx279125_early_vectors
	mcr p15, 0, r2, c12, c0, 0
	isb
	bx lr
refused:
	mvn r0, #0
	bx lr
.size zx279125_early_cpu_init, .-zx279125_early_cpu_init
.ltorg

.section .note.GNU-stack, "", %progbits

.section .text.early_vectors, "ax"
.balign 32
.global zx279125_early_vectors
zx279125_early_vectors:
	b fault_reset
	b fault_undefined
	b fault_svc
	b fault_prefetch
	b fault_data
	b fault_reserved
	b fault_irq
	b fault_fiq

.macro fault_entry name, number
\name:
	ldr sp, =STACK_TOP
	stmdb sp!, {r0-r12, lr}
	mov r0, #\number
	b fault_record
.endm
fault_entry fault_reset, 0
fault_entry fault_undefined, 1
fault_entry fault_svc, 2
fault_entry fault_prefetch, 3
fault_entry fault_data, 4
fault_entry fault_reserved, 5
fault_entry fault_irq, 6
fault_entry fault_fiq, 7

fault_record:
	ldr r1, =TRACE
	mov r2, #0
	str r2, [r1]
	str r0, [r1, #4]
	mrs r2, cpsr
	str r2, [r1, #8]
	mrs r2, spsr
	str r2, [r1, #12]
	mrc p15, 0, r2, c5, c0, 0
	str r2, [r1, #16]
	mrc p15, 0, r2, c5, c0, 1
	str r2, [r1, #20]
	mrc p15, 0, r2, c6, c0, 0
	str r2, [r1, #24]
	mrc p15, 0, r2, c6, c0, 2
	str r2, [r1, #28]
	/* Saved r0-r12 and raw exception LR; interpretation depends on type. */
	mov r2, sp
	add r3, r1, #32
	mov r0, #14
1:
	ldr r12, [r2], #4
	str r12, [r3], #4
	subs r0, r0, #1
	bne 1b
	ldr r2, =0x46544c31
	str r2, [r1]
	dsb sy
#ifdef ZX_EARLY_VECTOR_AUDIT
	ldr r2, [r1, #4]
	cmp r2, #1
	bne 2f
	ldmia sp!, {r0-r12, lr}
	movs pc, lr
#endif
#ifdef ZX_EARLY_FAULT_UART
	/* A cold hang leaves no readable SRAM: report type, DFAR and raw LR. */
	adr r0, fault_text
	bl fault_puts
	ldr r4, =TRACE
	ldr r0, [r4, #4]
	bl fault_puthex
	ldr r0, [r4, #24]
	bl fault_puthex
	ldr r0, [r4, #84]
	bl fault_puthex
	adr r0, fault_eol
	bl fault_puts
#endif
.global zx279125_fault_halt
zx279125_fault_halt:
2:
	b 2b

#ifdef ZX_EARLY_FAULT_UART
/* ROM-configured UART; each character waits a bounded time for FIFO space. */
fault_putc:
	ldr r1, =0x94405000
	ldr r2, =100000
1:
	ldr r3, [r1, #0x14]
	tst r3, #0x20
	beq 2f
	subs r2, r2, #1
	bne 1b
2:
	str r0, [r1, #4]
	bx lr

fault_puts:
	push {r5, lr}
	mov r5, r0
1:
	ldrb r0, [r5], #1
	cmp r0, #0
	popeq {r5, pc}
	bl fault_putc
	b 1b

fault_puthex:
	push {r5, r6, lr}
	mov r5, r0
	mov r0, #' '
	bl fault_putc
	mov r6, #28
1:
	lsr r0, r5, r6
	and r0, r0, #15
	cmp r0, #10
	addlo r0, r0, #'0'
	addhs r0, r0, #('a' - 10)
	bl fault_putc
	subs r6, r6, #4
	bpl 1b
	pop {r5, r6, pc}

fault_text:
	.asciz "\r\nZXS FAULT"
fault_eol:
	.asciz "\r\n"
	.balign 4
#endif
.ltorg
