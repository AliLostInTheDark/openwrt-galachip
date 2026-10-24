/* SPDX-License-Identifier: GPL-2.0+ */
.syntax unified
.arch armv7-a
.arm
.section .text.start, "ax"
.global _start
_start:
	b entry
entry:
	mov r12, sp
	mrs r3, cpsr
	and r1, r3, #0x1f
	cmp r1, #0x13
	bne refused
	mrc p15, 0, r1, c1, c0, 0
	ldr r2, =0x42003005
	tst r1, r2
	bne refused
	ldr sp, =0x00204000
	push {r2-r12, lr}
	cpsid if
#ifdef ZX_STAGE_RAM_AUDIT
	bl zx_first_stage_audit
	b return_to_monitor
#elif defined(ZX_STAGE_SPI_AUDIT)
	bl zx_first_stage_spi_audit
	b return_to_monitor
#else
	bl zx279125_early_cpu_init
	cmp r0, #0
	bne return_to_monitor
	bl zx_first_stage_cold
	cmp r0, #0
	bne halted
	dsb sy
	mov r0, #0
	mcr p15, 0, r0, c7, c5, 0
	isb
	ldr r0, =0x40400000
	bx r0
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
1:
	b 1b
.ltorg
.section .note.GNU-stack, "", %progbits
