// SPDX-License-Identifier: GPL-2.0-or-later
#include <cpu_func.h>
#include <irq_func.h>
#include <hang.h>
#include <asm/io.h>

int __weak gc1601_eth_handoff(void)
{
	return 0;
}

void enable_caches(void) {}
void invalidate_icache_all(void) {}
void invalidate_dcache_all(void) {}
void flush_dcache_all(void) {}

int cleanup_before_linux(void)
{
	if (gc1601_eth_handoff())
		hang();
	disable_interrupts();
	asm volatile("mcr p15, 0, %0, c7, c10, 4" : : "r"(0) : "memory");
	return 0;
}

void reset_cpu(void)
{
	/* FTWDT010: reset, reload, enable system reset. */
	writel(0, (void *)0x9080000c);
	writel(1000, (void *)0x90800004);
	writel(0x5ab9, (void *)0x90800008);
	writel(3, (void *)0x9080000c);
	for (;;)
		;
}
