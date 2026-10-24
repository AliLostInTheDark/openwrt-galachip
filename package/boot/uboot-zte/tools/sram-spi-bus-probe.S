/* SPDX-License-Identifier: GPL-2.0+ */
/* Temporarily replay vendor bus setup, read 256 KiB, restore bus registers. */
.syntax unified
.arm
.section .text.start, "ax"
.global _start
_start:
	mov r12, sp
	ldr sp, =0x0020a000
	push {r4-r11, r12, lr}
	mov r8, #0
	adr r6, _start
	ldr r0, =banner
	bl puts
	mov r0, r6
	bl hex32
	ldr r0, =stack_label
	bl puts
	mov r0, sp
	bl hex32
	ldr r0, =done
	bl puts
	mov r4, #0
	ldr r5, =0x40600000
	mov r6, #0x40000
	ldr r9, =0x94200000
	ldr r10, [r9]
	ldr r11, [r9, #4]
	ldr r0, =0x403f1f00
	cmp r10, r0
	bne bus_reject
	cmp r11, #0x3c0
	bne bus_reject
	bic r0, r10, #0x3c000000
	bic r0, r0, #0x30000
	dmb sy
	str r0, [r9]
	orr r0, r11, #0x3c0
	str r0, [r9, #4]
	dsb sy
	bl zx279125_early_spi_setup
	mov r7, r0
	cmp r7, #0
	bne read_done
read_page:
	mov r0, r5
	mov r1, r4
	mov r2, #256
	bl zx279125_early_spi_read
	mov r7, r0
	cmp r7, #0
	bne read_done
	add r4, r4, #256
	add r5, r5, #256
	cmp r4, r6
	blo read_page
read_done:
	dmb sy
	str r11, [r9, #4]
	str r10, [r9]
	dsb sy
	b report_result
bus_reject:
	mvn r7, #21
report_result:
	ldr r0, =result_label
	bl puts
	mov r0, r7
	bl hex32
	ldr r0, =completed_label
	bl puts
	mov r0, r4
	bl hex32
	ldr r0, =return_label
	bl puts
	mov r1, r7
	pop {r4-r11, r12, lr}
	mov sp, r12
	mov r0, r1
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
	orr r8, r8, #1
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
banner: .asciz "\r\nSRAM bus/SPI probe PC=0x"
stack_label: .asciz " SP=0x"
done: .asciz "\r\nReplaying bus/SPI setup; reading flash 0..0x3ffff into 0x40600000.\r\n"
result_label: .asciz "Bus/SPI copy rc=0x"
completed_label: .asciz " completed bytes=0x"
return_label: .asciz "\r\nBus registers restored or unchanged; returning without DDR init.\r\n"

.section .note.GNU-stack,"",%progbits
