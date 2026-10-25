/* SPDX-License-Identifier: GPL-2.0-only */
/* cortina_vlan_install.h -- the CORTINA family's install for ...
 * dev/MEASURED-cortina_vlan_install.h.md sec 1. */
#ifndef _CORTINA_VLAN_INSTALL_H
#define _CORTINA_VLAN_INSTALL_H

#include <linux/types.h>

#include "hwio.h"
#include "regtable.h"	/* struct reg: a field nobody REGISTERED is not offset 0 */
#include "gpon_omci_vlan.h"	/* the core's decided rule and the op table */

/* The DMA-AFT L2FIB word layout. MOVED from ... -- dev/MEASURED-cortina_vlan_install.h.md sec 2. */
#define CORTINA_AFT_D2_VLAN_SET_MODE	(1u << 8)
#define CORTINA_AFT_D2_TAG_CNT_LSB	6		/* [7:6] */
#define CORTINA_AFT_D2_TAG_CNT_MASK	0xc0u
#define CORTINA_AFT_D2_TPID_SLOT_LSB	1		/* [3:1], 1-BASED */
#define CORTINA_AFT_D2_TPID_SLOT_MASK	0x0eu
#define CORTINA_AFT_D1_TOP_VID_LSB	19		/* [30:19] */
#define CORTINA_AFT_D1_TOP_VID_MASK	0x7ff80000u
#define CORTINA_AFT_D1_TPID_SRC_LO	(1u << 31)	/* selector bit0; bit1 is D2[0] */

/* The MAP word: "frames from this lspid use fib @fib". */
#define CORTINA_AFT_MAP_FIB_MASK	0x3fu		/* [5:0]  */
#define CORTINA_AFT_MAP_EN		(1u << 6)
#define CORTINA_AFT_MAP_LSPID_LSB	7		/* [10:7] */
#define CORTINA_AFT_MAP_LSPID_MASK	0x780u
#define CORTINA_AFT_MAP_VLD		(1u << 11)

/* The indirect ACCESS word: idx[5:0] plus the NI-wide GO and WRITE bits. */
#define CORTINA_AFT_ACCESS_IDX_MASK	0x3fu
#define CORTINA_AFT_ACCESS_WRITE	(1u << 30)
#define CORTINA_AFT_ACCESS_GO		(1u << 31)
#define CORTINA_AFT_GO_TRIES		1000u

/* Where the DMA-AFT lives on one die. A PORT ADDS ONE ...
 * dev/MEASURED-cortina_vlan_install.h.md sec 3. */
struct cortina_vlan_regs {
	const char *name;
	struct reg l2fib_access;
	struct reg l2fib_data2;
	struct reg l2fib_data1;
	struct reg l2fib_data0;
	struct reg map_access;
	struct reg map_data;
	u8	fib_entries;
	u8	map_entries;
};

/* One decided rule, per install. @tpid_slot is the ...
 * dev/MEASURED-cortina_vlan_install.h.md sec 4. */
struct cortina_vlan_ctx {
	struct hwio	io;		/* over the DMA window */
	const struct cortina_vlan_regs *regs;
	int		tpid_slot;	/* 0..3, or -1 = not placed */
	u8		fib_idx;
	u8		map_idx;
	u8		lspid_map;	/* lspid - CPU0, what the MAP field holds */
	void		(*pause)(void);
};

/* The three L2FIB data words for an edit that leaves @tag_cnt tags on the
 * frame, the top one carrying @vid from TPID slot @tpid_slot.  PURE. */
void cortina_vlan_aft_words(u16 vid, u8 tag_cnt, int tpid_slot,
			    u32 *d0, u32 *d1, u32 *d2);

/* The MAP word for "@lspid_map uses fib @fib", or 0 to clear the entry. PURE. */
u32 cortina_vlan_aft_map_word(u8 lspid_map, u8 fib);

/* What this engine would have to do for @r, or a named ...
 * dev/MEASURED-cortina_vlan_install.h.md sec 5. */
int cortina_vlan_rule_to_aft(const struct gpon_vlan_rule *r, u16 *vid,
			     u8 *tag_cnt);

extern const struct gpon_vlan_ops cortina_vlan_ops;

#endif /* _CORTINA_VLAN_INSTALL_H */
