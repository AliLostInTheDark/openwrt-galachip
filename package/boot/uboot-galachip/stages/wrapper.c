// SPDX-License-Identifier: GPL-2.0-only
#include "decode.h"
extern const struct lzma_header image_header;
extern const unsigned char compressed_start[], compressed_end[];
void uart_init(void);
void uart_puts(const char *s);
static int allocated;

static void recover(void)
{
	void *package = (void *)0x40fffff0;
	int length;

	for (;;) {
		uart_puts("Wrapper failed; send xz000-g7-uboot-uart.pkg by XMODEM-CRC\n");
		length = xmodem_receive(package, UART_MAX);
		if (length > 0 && uart_image_valid(package, length))
			stage_jump(0x41000000);
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

void stage_main(void)
{
	const ISzAlloc allocator = { alloc, release };

	uart_init();
	uart_puts("GC1601 checked U-Boot wrapper\n");
	if (decode_image(&image_header, compressed_start,
			 compressed_end - compressed_start, (void *)PROPER_ENTRY,
			 PROPER_MAX, &allocator))
		recover();
	stage_jump(PROPER_ENTRY);
}
