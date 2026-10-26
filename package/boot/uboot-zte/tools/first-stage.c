// SPDX-License-Identifier: GPL-2.0+
#include <asm/io.h>
#include <linux/errno.h>
#include "ddr.h"
#include "early_spi.h"
#include "stage-payload.h"

#ifdef ZX_STAGE_SPI_AUDIT
/* Audit the same flash extent using the captured stock contents. */
#undef ZX_PAYLOAD_CRC
#define ZX_PAYLOAD_CRC ZX_STAGE_SPI_CRC
#endif

#define TRACE ((volatile u32 *)0x00202400)
#define DESTINATION 0x40400000UL
#define AUDIT_SOURCE 0x40800000UL

#if !defined(ZX_STAGE_RAM_AUDIT) && !defined(ZX_STAGE_SPI_AUDIT)
/* The ROM has already configured this UART at 25 MHz / 115200 8N1. */
#define UART_DATA ((void __iomem *)0x94405004)
#define UART_FLAGS ((void __iomem *)0x94405014)
#define UART_TX_FULL 0x20
#define UART_POLL_LIMIT 100000U

static void stage_putc(char c)
{
	unsigned int count;

	/* Drop the character rather than let a stuck FIFO block the boot. */
	for (count = 0; count < UART_POLL_LIMIT; count++)
		if (!(readl_relaxed(UART_FLAGS) & UART_TX_FULL))
			break;
	writel_relaxed(c, UART_DATA);
}

static void stage_puts(const char *s)
{
	while (*s)
		stage_putc(*s++);
}

static void stage_puthex(u32 value)
{
	int shift;

	stage_putc(' ');
	for (shift = 28; shift >= 0; shift -= 4)
		stage_putc("0123456789abcdef"[(value >> shift) & 15]);
}
#endif

static u32 payload_crc(const void *data)
{
	const u8 *bytes = data;
	u32 crc = ~0U;
	unsigned int i, bit;

	for (i = 0; i < ZX_PAYLOAD_SIZE; i++) {
		crc ^= bytes[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
	}
	return ~crc;
}

static void trace_start(void)
{
	unsigned int i;

	for (i = 0; i < 12; i++)
		TRACE[i] = 0;
	TRACE[0] = 0x53544731;
	TRACE[4] = ZX_PAYLOAD_SIZE;
	TRACE[5] = ZX_PAYLOAD_CRC;
	TRACE[6] = readl_relaxed((void __iomem *)0x9430000c);
}

static int verify_destination(void)
{
	TRACE[1] = 4;
	TRACE[3] = payload_crc((void *)DESTINATION);
	if (TRACE[3] != ZX_PAYLOAD_CRC) {
		TRACE[2] = -EBADMSG;
		return -EBADMSG;
	}
	TRACE[1] = 5;
	return 0;
}

#ifndef ZX_STAGE_RAM_AUDIT
static int load_flash_pages(void)
{
	u32 copied;
	int ret;

	TRACE[1] = 3;
	ret = zx279125_early_spi_setup();
	if (ret)
		return ret;
	for (copied = 0; copied < ((ZX_PAYLOAD_SIZE + 255) & ~255U);
	     copied += 256) {
		ret = zx279125_early_spi_read((void *)(DESTINATION + copied),
					    0x2000 + copied, 256);
		if (ret)
			return ret;
		TRACE[7] = copied + 256;
	}
	return verify_destination();
}
#endif

#ifdef ZX_STAGE_RAM_AUDIT
int zx_first_stage_audit(void)
{
	const u32 *source = (void *)AUDIT_SOURCE;
	u32 *destination = (void *)DESTINATION;
	unsigned int i;

	trace_start();
	TRACE[1] = 1;
	if (!TRACE[6]) {
		TRACE[2] = -EPERM;
		return -EPERM;
	}
	/* Check the complete source before overwriting any destination bytes. */
	TRACE[3] = payload_crc(source);
	if (TRACE[3] != ZX_PAYLOAD_CRC) {
		TRACE[2] = -EBADMSG;
		return -EBADMSG;
	}
	TRACE[1] = 3;
	for (i = 0; i < (ZX_PAYLOAD_SIZE + 3) / 4; i++)
		destination[i] = source[i];
	return verify_destination();
}
#elif defined(ZX_STAGE_SPI_AUDIT)
int zx_first_stage_spi_audit(void)
{
	int ret;

	trace_start();
	TRACE[1] = 1;
	if (!TRACE[6]) {
		TRACE[2] = -EPERM;
		return -EPERM;
	}
	ret = load_flash_pages();
	if (ret)
		TRACE[2] = ret;
	return ret;
}
#else
int zx_first_stage_cold(void)
{
	u32 value;
	int ret;

	trace_start();
	stage_puts("\r\nZXS ddr");
	TRACE[1] = 2;
	ret = zx279125_ddr_init();
	if (ret)
		goto failed;
	value = readl_relaxed((void __iomem *)0x94200000);
	writel_relaxed(value & ~0x3c030000, (void __iomem *)0x94200000);
	value = readl_relaxed((void __iomem *)0x94200004);
	writel_relaxed(value | 0x3c0, (void __iomem *)0x94200004);
	stage_puts(" spi");
	ret = load_flash_pages();
	if (!ret) {
		stage_puts(" ok\r\n");
		return 0;
	}
failed:
	TRACE[2] = ret;
	/* phase, result, DDR phase, copied bytes: the SRAM trace is unreadable after a hang. */
	stage_puts(" FAIL");
	stage_puthex(TRACE[1]);
	stage_puthex(ret);
	stage_puthex(TRACE[8]);
	stage_puthex(TRACE[7]);
	stage_puts("\r\n");
	return ret;
}
#endif
