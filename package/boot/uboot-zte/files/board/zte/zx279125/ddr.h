/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __ZX279125_DDR_H
#define __ZX279125_DDR_H

/*
 * Board-specific reconstruction of the V2801SG DDR startup sequence.
 * Must execute outside DDR, with an SRAM stack, data cache/MMU disabled and
 * interrupts masked. The SRAM entry must establish instruction-cache state
 * and visibility. The complete call path and literal pools must also live
 * outside DDR. The decoded NOR ROM path loads 8 KiB at 00200000; integrated
 * cold execution remains unverified.
 * Never call from a RAM-chainloaded U-Boot; DDR contents are destroyed.
 * Returns -EPERM for execution/stack in the 0x4xxxxxxx DDR aperture, or
 * -ETIMEDOUT if a vendor readiness poll does not complete. On failure the
 * caller must halt without touching DDR. There is no rollback sequence.
 */
int zx279125_ddr_init(void);

#endif
