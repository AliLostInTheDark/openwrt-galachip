/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __ZX279125_EARLY_BOOT_H
#define __ZX279125_EARLY_BOOT_H

#include <linux/types.h>

/*
 * Cold-start primitive, not a RAM-monitor command. The caller, code, literals
 * and stack must be outside DDR, with MMU/caches off and interrupts masked.
 * The decoded NOR ROM path loads 8 KiB at 00200000. This primitive is not
 * yet a hardware-verified integrated cold boot path.
 * On failure, halt in SRAM; DDR and controller state cannot be rolled back.
 * The exact stock restoration tuple (41f00000, 0, 40000) is also accepted,
 * separately from the bounded low-DRAM target used by future source images.
 */
int zx279125_bootstrap_load(unsigned long destination, u32 offset, u32 size);

#endif
