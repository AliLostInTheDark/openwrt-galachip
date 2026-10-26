/* SPDX-License-Identifier: GPL-2.0 */
/* luna_ponmac.h - board-agnostic bring-up of the */
#ifndef _LUNA_PONMAC_H
#define _LUNA_PONMAC_H

#include <linux/types.h>

/* The chips this library actually serves. */
enum luna_chip {
	LUNA_CHIP_9602C = 0,	/* + 9601C / 9601C_VB subtypes */
};

/* Chip revision id as read from HW: A=0x1, B=0x2, C=0x3, ... rev>A => B+ */
#define LUNA_REV_A		0x1
#define LUNA_REV_B		0x2
#define LUNA_REV_C		0x3

/* Subtype (9602C family): pick GponModeV3 (9601C) vs V2 on rev>A. */
#define LUNA_SUBTYPE_NONE		0x00
#define LUNA_SUBTYPE_9601C_VB	0x01
#define LUNA_SUBTYPE_9601C		0x03

/*
 * Board-agnostic register accessor. phys is the ABSOLUTE physical address from
 * the chip's register map. The board driver maps it (KSEG1 or ioremap).
 */
struct luna_ops {
	u32  (*rd)(u32 phys);
	void (*wr)(u32 phys, u32 val);
};

/* The field-mask owner lives in gpon/, which this family already has on its
 * include path (realtek-luna/Makefile: -I$(srctree)/drivers/net/gpon). */
#include "gpon_regseq.h"

/* read-modify-write bits [msb:lsb] at absolute phys address */
static inline void luna_rfwr(const struct luna_ops *o, u32 phys,
				u8 msb, u8 lsb, u32 val)
{
	o->wr(phys, gpon_field_set(o->rd(phys), msb, lsb, val));
}

/* Bring-up entry points. */
int luna_ponmac_init(enum luna_chip chip, int rev, int subtype,
			const struct luna_ops *o);
int luna_ponmac_mode_set(enum luna_chip chip, int rev, int subtype,
			    const struct luna_ops *o);
int luna_ponmac_serdes_cdr_reset(enum luna_chip chip,
				    const struct luna_ops *o);

extern int luna_c2_postmode_perturb;
extern int luna_c2_sds_cfgrst;
extern int luna_c2_stock_analog;
extern int luna_c2_analog_postreset;
extern int luna_c2_cmu_settle_ms;
extern int luna_c2_clkgate_rstb;
extern int luna_c2_skip_rstb_dance;
extern int luna_c2_minimal_analog;
#endif /* _LUNA_PONMAC_H */
