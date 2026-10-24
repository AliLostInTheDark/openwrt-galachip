/* SPDX-License-Identifier: GPL-2.0+ */
/* SRAM diagnostic: reinitialize DDR, reload installed stock U-Boot, and jump. */
.syntax unified
.arm
#if defined(ZX_DDR_STOP_ENGINE) && defined(ZX_DDR_UART_AUDIT)
#error "The UART-only audit must not reset the packet engine"
#endif
.section .text.start, "ax"
.global _start
_start:
#ifdef ZX_DDR_UART_AUDIT
	mov r12, sp
	mrs r3, cpsr
	ldr sp, =0x0020a000
	push {r2-r11, r12, lr}
#endif
	cpsid aif
#ifndef ZX_DDR_UART_AUDIT
	ldr sp, =0x0020a000
#endif
	mrc p15, 0, r0, c1, c0, 0
	tst r0, #0x1000
	bne unsafe
	tst r0, #5
	bne unsafe
	ldr r0, =0x92200000
	ldr r0, [r0]
	tst r0, #4
	bne unsafe
#ifdef ZX_DDR_STOP_ENGINE
	/* The separately tested SRAM probe leaves DMA/BMU off after PP reset. */
	bl engine_probe
	cmp r0, #0
	bne failed
	ldr r0, =0x92164000
	ldr r0, [r0]
	tst r0, #0xf0000
	bne unsafe
	ldr r0, =0x92144048
	ldr r0, [r0]
	ldr r1, =0xfffff
	tst r0, r1
	bne unsafe
#endif
#ifndef ZX_DDR_UART_AUDIT
	/* MAC RX off is not proof that the initialized packet engine is idle. */
	ldr r0, =0x92168000
	ldr r0, [r0]
	cmp r0, #0
	bne unsafe
	ldr r0, =0x921c0000
	ldr r0, [r0]
	tst r0, #12
	bne unsafe
	ldr r0, =0x92200000
	ldr r0, [r0]
	tst r0, #12
	bne unsafe
#endif
	ldr r0, =banner
	bl puts
	bl uart_drain
	cmp r0, #0
#ifdef ZX_DDR_UART_AUDIT
	bne audit_return
	ldr r0, =audit_msg
	bl puts
	bl uart_drain
	b audit_return
#else
	bne halted
#endif
	ldr r0, =0x41f00000
	mov r1, #0
	ldr r2, =0x40000
	bl zx279125_bootstrap_load
	cmp r0, #0
	bne failed
	ldr r4, =0x41f00000
	ldr r0, [r4]
	ldr r1, =0xea000014
	cmp r0, r1
	bne invalid_image
	ldr r0, [r4, #0x3c]
	ldr r1, =0x12345678
	cmp r0, r1
	bne invalid_image
	ldr r0, =handoff
	bl puts
	bl uart_drain
	cmp r0, #0
	bne halted
	bx r4
unsafe:
	ldr r0, =unsafe_msg
	bl puts
#ifdef ZX_DDR_UART_AUDIT
	mvn r0, #0
	b audit_return
#endif
	b halted
invalid_image:
	ldr r0, =invalid_msg
	bl puts
	b halted
failed:
	mov r4, r0
	ldr r0, =failed_msg
	bl puts
	mov r0, r4
	bl hex32
	ldr r0, =newline
	bl puts
halted:
	b halted

#ifdef ZX_DDR_UART_AUDIT
audit_return:
	mov r1, r0
	pop {r2-r11, r12, lr}
	msr cpsr_c, r3
	mov sp, r12
	mov r0, r1
	bx lr
#endif

/* Do not change clocks while the banner is still queued in the UART. */
uart_drain:
	ldr r1, =0x94405000
	ldr r2, =1000000
1:
	ldr r3, [r1, #0x14]
	tst r3, #0x80
	bne 2f
	subs r2, r2, #1
	bne 1b
	mvn r0, #109
	bx lr
2:
	dsb sy
	mov r0, #0
	bx lr

/* Only the existing UART TX FIFO and its read-only flag register. */
putc:
	ldr r1, =0x94405000
	ldr r2, =1000000
1:
	ldr r3, [r1, #0x14]
	tst r3, #0x20
	beq 2f
	subs r2, r2, #1
	bne 1b
	bx lr
2:
	str r0, [r1, #4]
	bx lr
puts:
	push {r4, lr}
	mov r4, r0
1:
	ldrb r0, [r4], #1
	cmp r0, #0
	beq 2f
	bl putc
	b 1b
2:
	pop {r4, pc}
hex32:
	push {r4-r6, lr}
	mov r4, r0
	mov r5, #28
1:
	lsr r0, r4, r5
	and r0, r0, #15
	cmp r0, #9
	addls r0, r0, #48
	addhi r0, r0, #87
	bl putc
	subs r5, r5, #4
	bpl 1b
	pop {r4-r6, pc}
.ltorg
.section .rodata
banner: .asciz "\r\nSRAM DDR recovery: initializing DDR, then loading stock U-Boot.\r\n"
handoff: .asciz "\r\nDDR/SPI/image header OK; entering stock U-Boot.\r\n"
#ifdef ZX_DDR_UART_AUDIT
unsafe_msg: .asciz "\r\nUnsafe CPU cache/MMU or active Ethernet RX; halted before DDR.\r\n"
#else
unsafe_msg: .asciz "\r\nUnsafe CPU/cache or initialized packet engine; halted before DDR.\r\n"
#endif
invalid_msg: .asciz "\r\nInvalid stock image header; halted in SRAM.\r\n"
failed_msg: .asciz "\r\nDDR/SPI loader failed, rc=0x"
newline: .asciz "\r\nHalted in SRAM. Power-cycle to recover.\r\n"
#ifdef ZX_DDR_UART_AUDIT
audit_msg: .asciz "UART drain audit only: DDR loader was not called; returning.\r\n"
#endif
.section .note.GNU-stack,"",%progbits
