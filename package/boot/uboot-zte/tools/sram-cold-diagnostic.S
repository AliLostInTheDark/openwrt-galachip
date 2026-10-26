/* SPDX-License-Identifier: GPL-2.0+ */
/* Unpackaged cold diagnostic, or its DDR-free warm guard audit. */
.syntax unified
.arch armv7-a
.arm
.equ PHASE_TRACE, 0x00200e80
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
	ldr sp, =0x0020a000
	push {r2-r12, lr}
	ldr r4, =PHASE_TRACE
	mov r0, #14
	mov r1, r4
	mov r2, #0
1:
	str r2, [r1], #4
	subs r0, r0, #1
	bne 1b
	ldr r0, =0x43535431
	str r0, [r4]
	mrc p15, 0, r1, c1, c0, 0
	str r1, [r4, #0x10]
	mrc p15, 0, r1, c12, c0, 0
	str r1, [r4, #0x18]
	ldr r1, =0x9430000c
	ldr r1, [r1]
	str r1, [r4, #0x20]
#ifdef ZX_COLD_GUARD_AUDIT
	/* The audit must not accept reset-state hardware, even accidentally. */
	cmp r1, #0
	beq audit_failed
#endif
	mov r0, #1
	str r0, [r4, #4]
	bl zx279125_early_cpu_init
	str r0, [r4, #8]
	mrc p15, 0, r1, c1, c0, 0
	str r1, [r4, #0x14]
	mrc p15, 0, r1, c12, c0, 0
	str r1, [r4, #0x1c]
	cmp r0, #0
	beq cpu_ready
	mov r0, #2
	str r0, [r4, #4]
	/* Return only if the rejected call left CPU control state unchanged. */
	ldr r1, [r4, #0x10]
	ldr r2, [r4, #0x14]
	cmp r1, r2
	bne halted
	ldr r1, [r4, #0x18]
	ldr r2, [r4, #0x1c]
	cmp r1, r2
	bne halted
	mov r0, #0
	b return_to_monitor
cpu_ready:
	mov r0, #3
	str r0, [r4, #4]
#ifdef ZX_COLD_GUARD_AUDIT
	/* The guard-only image has no DDR initializer linked into it. */
	b halted
audit_failed:
	mov r0, #15
	str r0, [r4, #4]
	mvn r0, #0
	b return_to_monitor
#else
	mov r0, #1
	str r0, [r4, #0x24]
	mov r0, #4
	str r0, [r4, #4]
	bl zx279125_ddr_init
	str r0, [r4, #0x0c]
	cmp r0, #0
	moveq r0, #5
	movne r0, #15
	str r0, [r4, #4]
	b halted
#endif
return_to_monitor:
	pop {r2-r12, lr}
	mov sp, r12
	msr cpsr_cxsf, r3
	bx lr
refused:
	mvn r0, #0
	bx lr
halted:
	dsb sy
2:
	b 2b
.ltorg

.section .note.GNU-stack, "", %progbits
