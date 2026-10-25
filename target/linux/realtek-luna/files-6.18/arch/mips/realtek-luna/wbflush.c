// SPDX-License-Identifier: GPL-2.0-only
/*
 * The Luna SoCs post uncached stores in a bus write buffer; a descriptor or a
 * doorbell can still sit there when a PCIe device fetches from DRAM. An
 * uncached store followed by an uncached load of the same word cannot complete
 * before every earlier write has left the buffer, which is the drain. The
 * vendor kernels of this family select CPU_HAS_WB and drain exactly this way;
 * with CPU_HAS_WB every mb()/iob() -- hence every MMIO accessor -- ends here.
 * MEASURED 2026-09-28 on the RTL9603CVD (dev/FINDING-rtl8192fe-txdma-0x6000-restart-2026-09-25.md).
 */
#include <linux/export.h>
#include <linux/types.h>
#include <asm/addrspace.h>
#include <asm/wbflush.h>

static u32 luna_wb_scratch __aligned(32);

static void luna_wbflush(void)
{
	/* the KSEG1 alias of the word: raw accesses, never writel/readl (they barrier) */
	volatile u32 *p = (volatile u32 *)CKSEG1ADDR(CPHYSADDR(&luna_wb_scratch));

	*p = 0;
	(void)*p;
}

void (*__wbflush)(void) = luna_wbflush;
EXPORT_SYMBOL(__wbflush);
