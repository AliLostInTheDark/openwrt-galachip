// SPDX-License-Identifier: GPL-2.0+
/* V2801SG stock bootloader 0x41f004a8..0x41f00610, reconstructed as source. */
#include <asm/io.h>
#include <linux/errno.h>
#include "ddr.h"

#define ZX_CLOCK_BASE		0x94000000UL
#define ZX_DDR_BASE		0x94300000UL
#define ZX_DDR_PHY_BASE		0x94302000UL
/* An iteration ceiling, not a calibrated wall-clock timeout. */
#define ZX_DDR_POLL_LIMIT	1000000U

#ifdef ZX_DDR_TRACE
#ifndef ZX_DDR_TRACE_BASE
#define ZX_DDR_TRACE_BASE 0x00200ea8
#endif
/* Optional SRAM diagnostics; never enabled in the normal monitor build. */
static inline void zx_ddr_phase(unsigned int phase)
{
	*(volatile unsigned int *)ZX_DDR_TRACE_BASE = phase;
}
#else
#define zx_ddr_phase(phase) do { } while (0)
#endif

/* Match the vendor cold-start MMIO sequence without per-access DMBs. */
static inline unsigned int zx_ddr_read(unsigned long address)
{
	return readl_relaxed((void __iomem *)address);
}

static inline void zx_ddr_write(unsigned long address, unsigned int value)
{
#ifdef ZX_DDR_TRACE
	*(volatile unsigned int *)(ZX_DDR_TRACE_BASE + 4) = address;
	*(volatile unsigned int *)(ZX_DDR_TRACE_BASE + 8) = value;
	*(volatile unsigned int *)(ZX_DDR_TRACE_BASE + 12) = 1;
#endif
	writel_relaxed(value, (void __iomem *)address);
#ifdef ZX_DDR_TRACE
	*(volatile unsigned int *)(ZX_DDR_TRACE_BASE + 12) = 2;
#endif
}

static void zx_ddr_delay(void)
{
	volatile unsigned int count = 0;

	/* Stock 0x41f00474: argument 2, multiplied by 1000. Not microseconds. */
	while (count < 2000)
		count++;
}

static int zx_ddr_wait_nonzero(unsigned long address)
{
	unsigned int count;

	for (count = 0; count < ZX_DDR_POLL_LIMIT; count++) {
		if (zx_ddr_read(address))
			return 0;
	}

	return -ETIMEDOUT;
}

int zx279125_ddr_init(void)
{
	unsigned long pc, sp;
	unsigned int count, status;
	int ret;

	/* Check actual execution address, not the function's link-time address. */
	asm volatile("mov %0, pc\n\tmov %1, sp" : "=r" (pc), "=r" (sp));
	if ((pc & 0xf0000000UL) == 0x40000000UL ||
	    (sp & 0xf0000000UL) == 0x40000000UL)
		return -EPERM;

	zx_ddr_phase(1);
	zx_ddr_write(ZX_CLOCK_BASE + 0x18, 0x00040c19);
	zx_ddr_write(ZX_CLOCK_BASE + 0x20, 0x00040a0d);
	zx_ddr_write(ZX_CLOCK_BASE + 0x28, 0x80040d62);
	zx_ddr_delay();
	zx_ddr_write(ZX_CLOCK_BASE + 0x28, 0x00040d62);

	zx_ddr_phase(2);
	for (count = 0; count < ZX_DDR_POLL_LIMIT; count++) {
		status = zx_ddr_read(ZX_CLOCK_BASE + 0x1c);
		status &= zx_ddr_read(ZX_CLOCK_BASE + 0x24);
		status &= zx_ddr_read(ZX_CLOCK_BASE + 0x2c);
		if (status & 0x10000000)
			break;
	}
	if (count == ZX_DDR_POLL_LIMIT)
		return -ETIMEDOUT;

	zx_ddr_phase(3);
	zx_ddr_write(ZX_CLOCK_BASE + 0x0c, 0x01a0060e);
	zx_ddr_write(ZX_CLOCK_BASE + 0x10, 0x00000010);
	zx_ddr_write(ZX_CLOCK_BASE + 0x14, 0xffffffff);
	zx_ddr_write(ZX_CLOCK_BASE + 0x08, 0xffffffff);
	zx_ddr_write(ZX_CLOCK_BASE + 0x04, 0x00000008);
	zx_ddr_write(0x9a100010, 0x00027303);
	zx_ddr_write(0x94400010, 0x00000303);
	zx_ddr_write(0x94400014, 0x00000303);
	zx_ddr_write(0x94400018, 0x00000303);

	zx_ddr_phase(4);
	zx_ddr_write(ZX_DDR_BASE + 0x00, 0x66);
	zx_ddr_write(ZX_DDR_BASE + 0x04, 0x31);
	zx_ddr_write(ZX_DDR_BASE + 0x0c, 0);
	zx_ddr_write(ZX_DDR_BASE + 0x80, 20);
	zx_ddr_write(ZX_DDR_BASE + 0x84, 3);
	zx_ddr_write(ZX_DDR_BASE + 0x88, 10);
	zx_ddr_write(ZX_DDR_BASE + 0x8c, 7);
	zx_ddr_write(ZX_DDR_BASE + 0x90, 3);
	zx_ddr_write(ZX_DDR_BASE + 0x94, 10);
	zx_ddr_write(ZX_DDR_BASE + 0x98, 3);
	zx_ddr_write(ZX_DDR_BASE + 0x9c, 2);
	zx_ddr_write(ZX_DDR_BASE + 0xa0, 2);
	zx_ddr_write(ZX_DDR_BASE + 0xa4, 16);
	zx_ddr_write(ZX_DDR_BASE + 0xa8, 12);
	zx_ddr_write(ZX_DDR_BASE + 0xac, 50);
	zx_ddr_write(ZX_DDR_BASE + 0xb4, 51);
	zx_ddr_write(ZX_DDR_PHY_BASE + 0x18, 4);
	zx_ddr_write(ZX_DDR_PHY_BASE + 0x24, 4);

	status = zx_ddr_read(ZX_CLOCK_BASE + 0x08);
	zx_ddr_write(ZX_CLOCK_BASE + 0x08, status | 0x180);
	zx_ddr_delay();
	zx_ddr_phase(5);
	zx_ddr_write(ZX_DDR_BASE + 0x08, 0);
	ret = zx_ddr_wait_nonzero(ZX_DDR_BASE + 0x104);
	if (ret)
		return ret;
	zx_ddr_phase(6);
	zx_ddr_write(ZX_DDR_BASE + 0x0c, 1);
	ret = zx_ddr_wait_nonzero(ZX_DDR_BASE + 0x100);
	if (ret)
		return ret;
	zx_ddr_phase(7);
	status = zx_ddr_read(ZX_CLOCK_BASE + 0x08);
	zx_ddr_write(ZX_CLOCK_BASE + 0x08, status | 0xc00);
	zx_ddr_delay();
	zx_ddr_phase(8);

	return 0;
}
