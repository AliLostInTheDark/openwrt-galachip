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
		uart_puts("\nWrapper failed; send xz000-g7-uboot-uart.pkg by XMODEM-CRC\n");
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
/*
 * Flat MMU table for the decode: DDR (0x40000000-0x41ffffff) write-through
 * cacheable, everything else uncached. Write-through never leaves dirty
 * lines, so turning the D-cache off needs only an invalidate. 16 KiB at
 * 0x40400000, clear of the output, wrapper, heap, stack and DMA ranges.
 */
#define MMU_TABLE 0x40400000U

static void dcache(int on)
{
	uint32_t *table = (uint32_t *)MMU_TABLE, cr, i;

	asm volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(cr));
	if (on) {
		/* Section: AP read/write, domain 0, bit 4 set, C for DDR, B clear. */
		for (i = 0; i < 4096; i++)
			table[i] = i << 20 | 3 << 10 | 0x12 |
				   (i >= 0x400 && i < 0x420 ? 8 : 0);
		asm volatile("mcr p15, 0, %0, c7, c10, 4" : : "r"(0) : "memory");
		asm volatile("mcr p15, 0, %0, c2, c0, 0" : : "r"(MMU_TABLE));
		asm volatile("mcr p15, 0, %0, c3, c0, 0" : : "r"(1));
		asm volatile("mcr p15, 0, %0, c8, c7, 0" : : "r"(0));
		asm volatile("mcr p15, 0, %0, c7, c6, 0" : : "r"(0));
		cr |= 5;
	} else {
		cr &= ~5U;
	}
	asm volatile("mcr p15, 0, %0, c1, c0, 0" : : "r"(cr) : "memory");
	if (!on) {
		asm volatile("mcr p15, 0, %0, c7, c6, 0" : : "r"(0) : "memory");
		asm volatile("mcr p15, 0, %0, c8, c7, 0" : : "r"(0));
		asm volatile("mcr p15, 0, %0, c7, c10, 4" : : "r"(0) : "memory");
	}
}

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

	int ret;

	/* Silent on success, as upstream decompression is; recover() reports. */
	uart_init();
	icache(1);
	dcache(1);
	ret = decode_image(&image_header, compressed_start,
			   compressed_end - compressed_start, (void *)PROPER_ENTRY,
			   PROPER_MAX, &allocator);
	dcache(0);
	/* Check the decoded image again in DDR itself, with the D-cache off. */
	if (!ret && stage_crc((void *)PROPER_ENTRY, image_header.expanded) !=
	    image_header.expanded_crc)
		ret = -1;
	icache(0);
	if (ret)
		recover();
	stage_jump(PROPER_ENTRY);
}
