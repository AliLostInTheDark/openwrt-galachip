/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __XZ000_G7_H
#define __XZ000_G7_H
#define CFG_SYS_SDRAM_BASE 0x40000000
#define CFG_SYS_INIT_RAM_ADDR 0x40000000
#define CFG_SYS_INIT_RAM_SIZE 0x02000000
/*
 * The UART runs from a fixed 33 MHz clock (divisor = 33 MHz / 16 / baud):
 * 115200 and 230400 are within 0.6 %; 1031250 is exact but non-standard.
 * 460800 and 921600 have no usable divisor.
 */
#define CFG_SYS_BAUDRATE_TABLE { 9600, 19200, 38400, 57600, 115200, 230400, 1031250 }
#endif
