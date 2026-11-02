// SPDX-License-Identifier: GPL-2.0-only
#include "decode.h"
extern const struct lzma_header image_header;
extern const unsigned char compressed_start[], compressed_end[];
void uart_init(void);
void uart_puts(const char *s);
static int allocated;

static void report_word(const char *name, uint32_t value)
{
	int shift;

	uart_puts(name);
	for (shift = 28; shift >= 0; shift -= 4)
		uart_put("0123456789abcdef"[(value >> shift) & 15]);
}

static void recover(void)
{
	void *package = (void *)0x40fffff0;
	int length;

	for (;;) {
		uart_puts("Wrapper failed; send xz000-g7-uboot-uart.pkg by XMODEM-CRC\n");
		length = xmodem_receive(package, UART_MAX);
		if (length > 0 && uart_image_valid(package, length))
			stage_jump(0x41000000);
		/* Print only after reception ends, never inside an active packet. */
		report_word("XMODEM packets=", xmodem_status.packets);
		report_word(" timeouts=", xmodem_status.timeouts);
		report_word(" crc_errors=", xmodem_status.crc_errors);
		report_word(" header_errors=", xmodem_status.header_errors);
		report_word(" bytes=", xmodem_status.bytes);
		report_word(" seq=", xmodem_status.sequence);
		report_word(" crc=", xmodem_status.calculated_crc);
		report_word(" wire_crc=", xmodem_status.received_crc);
		report_word(" lsr_errors=", uart_rx_errors);
		uart_puts("\n");
	}
}

static void *alloc(ISzAllocPtr context, size_t size)
{
	(void)context;
	if (allocated || size > 0x10000)
		return NULL;
	allocated = 1;
	return (void *)0x40200000;
}

static void release(ISzAllocPtr context, void *address)
{
	(void)context;
	(void)address;
	allocated = 0;
}

/*
 * The FA626TE I-cache needs no MMU and nothing here rewrites code, so it
 * may run during LZMA decoding. The D-cache stays off. Proper is entered
 * with the I-cache off and invalidated, as before.
 */
static void icache(int on)
{
	uint32_t cr;

	asm volatile("mcr p15, 0, %0, c7, c5, 0" : : "r"(0) : "memory");
	asm volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(cr));
	cr = on ? cr | 0x1000 : cr & ~0x1000U;
	asm volatile("mcr p15, 0, %0, c1, c0, 0" : : "r"(cr) : "memory");
	asm volatile("mcr p15, 0, %0, c7, c5, 0" : : "r"(0) : "memory");
}

void stage_main(void)
{
	const ISzAlloc allocator = { alloc, release };

	uart_init();
	/* The stock SPL leaves its last status digit without a newline. */
	uart_puts("\nGC1601 checked U-Boot wrapper\n");
	icache(1);
	if (decode_image(&image_header, compressed_start,
			 compressed_end - compressed_start, (void *)PROPER_ENTRY,
			 PROPER_MAX, &allocator)) {
		icache(0);
		recover();
	}
	icache(0);
	stage_jump(PROPER_ENTRY);
}
