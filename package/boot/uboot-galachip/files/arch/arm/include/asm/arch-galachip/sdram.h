/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_ARCH_GC1601_SDRAM_H
#define __ASM_ARCH_GC1601_SDRAM_H

#include <linux/types.h>

struct gc1601_ddr_reg {
	u32 offset;
	u32 value;
};

/* The board's DDR controller settings, written in order */
struct gc1601_ddr_params {
	const struct gc1601_ddr_reg *regs;
	unsigned int count;
};

extern const struct gc1601_ddr_params gc1601_ddr_params;

int gc1601_sdram_init(void);

#endif
