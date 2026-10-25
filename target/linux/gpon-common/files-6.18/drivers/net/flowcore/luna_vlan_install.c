// SPDX-License-Identifier: GPL-2.0-only
/* luna_vlan_install.c -- the Luna family's gpon_vlan_ops. ...
 * dev/MEASURED-luna_vlan_install.c.md sec 1. */
#include <linux/errno.h>
#include <linux/types.h>

#include "luna_vlan_install.h"
#include "regtable.h"		/* struct reg / reg_rc / REG_AT */

/* The three dies. Addresses: VLAN_PB_VID's base, width and ...
 * dev/MEASURED-luna_vlan_install.c.md sec 2. */
const struct luna_vlan_regs luna_vlan_rtl9602c = {
	.name		= "RTL9602C",
	.pb_vid		= REG_AT(0x1300c),
	.vlan_ingress	= REG_AT(0x13004),
	.vlan_ctrl	= REG_AT(0x13008),
	.egress_tag	= REG_AT(0x2a000),
	.n_ports	= 4,		/* chipdef port index 0..3 */
	.tbl_ctrl	= REG_AT(0x12000),
	.tbl_sts	= REG_AT(0x12004),
	.tbl_wrdata	= REG_AT(0x12008),
	.tbl_rddata	= REG_AT(0x1201c),
	.tbl_addr_lsb	= 9,
	.tbl_method_lsb	= 4,
	.tbl_cmd_lsb	= 3,
	.tbl_cmd_bits	= 1,
	.cf_action_us	= REG_ABSENT,	/* the engine that reaches it is OWED */
	.has_cf_block	= true,
};

const struct luna_vlan_regs luna_vlan_rtl9603cvd = {
	.name		= "RTL9603CVD",
	.pb_vid		= REG_AT(0x1300c),
	.vlan_ingress	= REG_AT(0x13004),
	.vlan_ctrl	= REG_AT(0x13008),
	.egress_tag	= REG_AT(0x2a000),
	.n_ports	= 6,		/* chipdef port index 0..5 */
	.tbl_ctrl	= REG_AT(0x12000),
	.tbl_sts	= REG_AT(0x12004),
	.tbl_wrdata	= REG_AT(0x12008),
	.tbl_rddata	= REG_AT(0x1201c),
	.tbl_addr_lsb	= 12,
	.tbl_method_lsb	= 5,
	.tbl_cmd_lsb	= 3,
	.tbl_cmd_bits	= 2,
	.cf_action_us	= REG_ABSENT,
	/* ★ A SILICON FACT, NOT OUR BLIND SPOT: this die declares no CF_*
	 * register and no CF_* table anywhere.  Its classifier is ACL, and a
	 * different shape again. */
	.has_cf_block	= false,
};

const struct luna_vlan_regs luna_vlan_rtl9607c = {
	.name		= "RTL9607C",
	.pb_vid		= REG_AT(0x1300c),
	.vlan_ingress	= REG_AT(0x13004),
	.vlan_ctrl	= REG_AT(0x13008),
	.egress_tag	= REG_AT(0x2a000),
	.n_ports	= 11,		/* chipdef port index 0..10 */
	.tbl_ctrl	= REG_AT(0x12000),
	.tbl_sts	= REG_AT(0x12004),
	.tbl_wrdata	= REG_AT(0x12008),
	.tbl_rddata	= REG_AT(0x1201c),
	.tbl_addr_lsb	= 12,
	.tbl_method_lsb	= 5,
	.tbl_cmd_lsb	= 3,
	.tbl_cmd_bits	= 2,
	.cf_action_us	= REG_ABSENT,
	.has_cf_block	= true,
};

u32 luna_vlan_packed_off(u32 base, unsigned int port, unsigned int bits)
{
	return base + 4u * (port / (32u / bits));
}

unsigned int luna_vlan_packed_lsb(unsigned int port, unsigned int bits)
{
	return (port % (32u / bits)) * bits;
}

u32 luna_vlan_tbl_ctrl_word(const struct luna_vlan_regs *r, u32 type, u32 addr)
{
	u32 cmd_mask;

	if (!r)
		return 0;
	cmd_mask = (1u << r->tbl_cmd_bits) - 1u;
	return (type & 0x7u) |
	       ((1u & cmd_mask) << r->tbl_cmd_lsb) |	/* CMD_TYPE = write */
	       (1u << r->tbl_method_lsb) |		/* ACCESS_METHOD = 1 */
	       ((addr & 0xfffu) << r->tbl_addr_lsb);
}

/* Every filter bit that NARROWS a row to less than the whole port. */
#define LUNA_VLAN_FILTER_MATCHES	(GPON_VLANF_CARE_TAG | GPON_VLANF_VID | \
					 GPON_VLANF_PRI | GPON_VLANF_TCI | \
					 GPON_VLANF_ETHTYPE | \
					 GPON_VLANF_DSCP_PRI)

int luna_vlan_rule_to_pvid(const struct gpon_vlan_rule *r, u16 *vid)
{
	struct gpon_vlan_want w;
	const struct gpon_vlan_tag_treat *t;

	if (!r || !vid)
		return -EINVAL;
	gpon_vlan_rule_want(r, &w);
	/* One literal tag on, nothing stripped: that and only that is what a
	 * port default VID says. */
	if (w.shape != GPON_VLAN_SHAPE_PUSH || w.write != 1 || !w.all_assigned)
		return -EOPNOTSUPP;
	/* ⚠ AND THE FILTER MUST BE THE WHOLE PORT. A default VID has ...
	 * dev/MEASURED-luna_vlan_install.c.md sec 3. */
	if ((r->f.s_mode & LUNA_VLAN_FILTER_MATCHES) ||
	    (r->f.c_mode & LUNA_VLAN_FILTER_MATCHES))
		return -EOPNOTSUPP;
	if (r->f.ethertype)
		return -EOPNOTSUPP;
	t = r->t.outer.written ? &r->t.outer : &r->t.inner;
	*vid = t->vid & 0xfff;
	return 0;
}

static int luna_vlan_ctx_ok(const struct luna_vlan_ctx *c)
{
	if (!c || !c->regs)
		return -EINVAL;
	if (c->uni_port >= c->regs->n_ports)
		return -ERANGE;
	/* -ENODEV = this die DECLARES no array; -ENXIO = nobody registered
	 * the field, which is our defect and a different repair. */
	return reg_rc(c->regs->pb_vid);
}

/* Read-modify-write one 12-bit element of the packed default-VID array. */
static int luna_vlan_pvid_write(const struct luna_vlan_ctx *c, u16 vid)
{
	u32 off = luna_vlan_packed_off(reg_at(c->regs->pb_vid), c->uni_port,
				       LUNA_VLAN_PB_VID_BITS);
	unsigned int lsb = luna_vlan_packed_lsb(c->uni_port,
						LUNA_VLAN_PB_VID_BITS);
	u32 mask = 0xfffu << lsb;

	hwio_wr(&c->io, off, (hwio_rd(&c->io, off) & ~mask) |
			     (((u32)vid << lsb) & mask));
	return 0;
}

static int luna_vlan_rule_install(void *ctx, const struct gpon_vlan_rule *r)
{
	struct luna_vlan_ctx *c = ctx;
	u16 vid = 0;
	int rc;

	rc = luna_vlan_ctx_ok(c);
	if (rc)
		return rc;
	rc = luna_vlan_rule_to_pvid(r, &vid);
	if (rc)
		return rc;
	return luna_vlan_pvid_write(c, vid);
}

/* Remove: put the port back on the default VID this driver ...
 * dev/MEASURED-luna_vlan_install.c.md sec 4. */
#define LUNA_VLAN_SHIPPED_PVID	1u

static int luna_vlan_rule_remove(void *ctx, const struct gpon_vlan_rule *r)
{
	struct luna_vlan_ctx *c = ctx;
	u16 vid = 0;
	int rc;

	rc = luna_vlan_ctx_ok(c);
	if (rc)
		return rc;
	/* refuse to "remove" a rule we could never have installed */
	rc = luna_vlan_rule_to_pvid(r, &vid);
	if (rc)
		return rc;
	return luna_vlan_pvid_write(c, LUNA_VLAN_SHIPPED_PVID);
}

/* ⚠ THE FIVE NULL SLOTS SAY SOMETHING, and it is not "not ...
 * dev/MEASURED-luna_vlan_install.c.md sec 5. */
const struct gpon_vlan_ops luna_vlan_ops = {
	.rule_install	= luna_vlan_rule_install,
	.rule_remove	= luna_vlan_rule_remove,
};
