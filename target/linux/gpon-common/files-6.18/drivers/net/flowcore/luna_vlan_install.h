/* SPDX-License-Identifier: GPL-2.0-only */
/* luna_vlan_install.h -- the LUNA family's install for the ...
 * dev/MEASURED-luna_vlan_install.h.md sec 1. */
#ifndef _LUNA_VLAN_INSTALL_H
#define _LUNA_VLAN_INSTALL_H

#include <linux/types.h>

#include "hwio.h"
#include "regtable.h"	/* struct reg: a field nobody REGISTERED is not offset 0 */
#include "gpon_omci_vlan.h"

/* VLAN_PB_VID element width -- 12 bits, packed two per 32-bit word. */
#define LUNA_VLAN_PB_VID_BITS	12u

/* One Luna die's VLAN surface. A PORT ADDS ONE INITIALISER. ⚠ ...
 * dev/MEASURED-luna_vlan_install.h.md sec 2. */
struct luna_vlan_regs {
	const char *name;
	struct reg pb_vid;	/* VLAN_PB_VID array base */
	struct reg vlan_ingress;/* VLAN_INGRESS, one bit per port */
	struct reg vlan_ctrl;	/* VLAN_CTRL, bit0 = VLAN_FILTERING */
	/* VLAN_EGRESS_TAG, one 32-bit word per port, EGRESS_MODE at [1:0].
	 * The ADDRESS is established on all three dies; the four MODE VALUES
	 * are not, so nothing here writes it.  Carried so the refusal has a
	 * subject and so a later RE lands in one place. */
	struct reg egress_tag;
	u8	n_ports;	/* port index range is 0..n_ports-1 */

	/* ★★★ THE INDIRECT TABLE-ACCESS ENGINE, AND ITS BIT LAYOUT IS ...
	 * dev/MEASURED-luna_vlan_install.h.md sec 3. */
	struct reg tbl_ctrl;
	struct reg tbl_sts;
	struct reg tbl_wrdata;
	struct reg tbl_rddata;
	u8	tbl_addr_lsb;
	u8	tbl_method_lsb;
	u8	tbl_cmd_lsb;
	u8	tbl_cmd_bits;

	/* The per-flow classifier ACTION table -- where an ME 171 row ...
	 * dev/MEASURED-luna_vlan_install.h.md sec 4. */
	struct reg cf_action_us;
	bool	has_cf_block;
};

extern const struct luna_vlan_regs luna_vlan_rtl9602c;
extern const struct luna_vlan_regs luna_vlan_rtl9603cvd;
extern const struct luna_vlan_regs luna_vlan_rtl9607c;

struct luna_vlan_ctx {
	struct hwio	io;		/* over the switch core */
	const struct luna_vlan_regs *regs;
	u8		uni_port;	/* the switch port the association names */
	void		(*pause)(void);
};

/* Where element @port of a @bits-wide packed array lives. PURE. */
u32 luna_vlan_packed_off(u32 base, unsigned int port, unsigned int bits);
unsigned int luna_vlan_packed_lsb(unsigned int port, unsigned int bits);

/* The TABLE-ACCESS control word for this die. PURE, and the whole point is
 * that the SHIFTS come from @r and not from a literal. */
u32 luna_vlan_tbl_ctrl_word(const struct luna_vlan_regs *r, u32 type, u32 addr);

/* > 0 and *@vid filled when @r is a rule this family CAN ...
 * dev/MEASURED-luna_vlan_install.h.md sec 5. */
int luna_vlan_rule_to_pvid(const struct gpon_vlan_rule *r, u16 *vid);

extern const struct gpon_vlan_ops luna_vlan_ops;

#endif /* _LUNA_VLAN_INSTALL_H */
