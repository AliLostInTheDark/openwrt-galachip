// SPDX-License-Identifier: GPL-2.0-only
/* cortina_vlan_install.c -- the Cortina family's gpon_vlan_ops
 * dev/MEASURED-cortina_vlan_install.c.md sec 1. */
#include <linux/errno.h>
#include <linux/types.h>

#include "cortina_vlan_install.h"
#include "regtable.h"		/* struct reg / reg_rc / gpon_ind_go */

void cortina_vlan_aft_words(u16 vid, u8 tag_cnt, int tpid_slot,
			    u32 *d0, u32 *d1, u32 *d2)
{
	*d0 = 0;
	*d1 = 0;
	/* SET mode: the frame LEAVES with exactly @tag_cnt tags. */
	*d2 = CORTINA_AFT_D2_VLAN_SET_MODE |
	      (((u32)tag_cnt << CORTINA_AFT_D2_TAG_CNT_LSB) &
	       CORTINA_AFT_D2_TAG_CNT_MASK);
	if (!tag_cnt)
		return;
	*d1 = (((u32)vid << CORTINA_AFT_D1_TOP_VID_LSB) &
	       CORTINA_AFT_D1_TOP_VID_MASK) | CORTINA_AFT_D1_TPID_SRC_LO;
	/* 1-BASED: 0 means "no tag", so slot 0 (0x8100) is written as 1.
	 * Writing the raw slot number disables the tag on the very slot the
	 * TPID matched. */
	*d2 |= ((u32)(tpid_slot + 1) << CORTINA_AFT_D2_TPID_SLOT_LSB) &
	       CORTINA_AFT_D2_TPID_SLOT_MASK;
}

u32 cortina_vlan_aft_map_word(u8 lspid_map, u8 fib)
{
	return CORTINA_AFT_MAP_VLD | CORTINA_AFT_MAP_EN |
	       (((u32)lspid_map << CORTINA_AFT_MAP_LSPID_LSB) &
		CORTINA_AFT_MAP_LSPID_MASK) |
	       ((u32)fib & CORTINA_AFT_MAP_FIB_MASK);
}

int cortina_vlan_rule_to_aft(const struct gpon_vlan_rule *r, u16 *vid,
			     u8 *tag_cnt)
{
	struct gpon_vlan_want w;

	if (!r || !vid || !tag_cnt)
		return -EINVAL;
	gpon_vlan_rule_want(r, &w);
	switch (w.shape) {
	case GPON_VLAN_SHAPE_DISCARD:
		/* the DMA-AFT edits a header; it cannot drop a frame */
		return -EOPNOTSUPP;
	case GPON_VLAN_SHAPE_TRANSPARENT:
		return -EOPNOTSUPP;	/* nothing to edit: no entry is right */
	case GPON_VLAN_SHAPE_POP:
		if (w.strip != 1)
			return -EOPNOTSUPP;
		*tag_cnt = 0;
		*vid = 0;
		return 0;
	case GPON_VLAN_SHAPE_PUSH:
	case GPON_VLAN_SHAPE_SWAP:
		/* In SET mode push-one and swap-one are the SAME instruction:
		 * the count is what the frame carries AFTER the edit. */
		if (w.write != 1 || !w.all_assigned)
			return -EOPNOTSUPP;
		*tag_cnt = 1;
		*vid = r->t.outer.written ? r->t.outer.vid : r->t.inner.vid;
		return 0;
	default:
		/* REWRITE, and every two-tag shape: inner_vid is SPLIT across
		 * two words the other way round from the TPID selector and its
		 * consumer is not proven on this die, so a second tag is OWED,
		 * never guessed. */
		return -EOPNOTSUPP;
	}
}

/* One indirect transaction on this engine: data words, then GO, then poll. */
static int aft_write(const struct cortina_vlan_ctx *c, struct reg access,
		     u32 idx, const u32 *data, const struct reg *off,
		     unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		int rc = reg_rc(off[i]);

		if (rc)
			return rc;
		hwio_wr(&c->io, reg_at(off[i]), data[i]);
	}
	return gpon_ind_go(&c->io, access,
			   CORTINA_AFT_ACCESS_GO | CORTINA_AFT_ACCESS_WRITE |
			   (idx & CORTINA_AFT_ACCESS_IDX_MASK),
			   CORTINA_AFT_ACCESS_GO, CORTINA_AFT_GO_TRIES,
			   c->pause) < 0 ? -ETIMEDOUT : 0;
}

static int cortina_vlan_ctx_ok(const struct cortina_vlan_ctx *c)
{
	int rc;

	if (!c || !c->regs || !c->pause)
		return -EINVAL;
	if (c->fib_idx >= c->regs->fib_entries ||
	    c->map_idx >= c->regs->map_entries)
		return -ERANGE;
	rc = reg_rc(c->regs->l2fib_access);
	if (!rc)
		rc = reg_rc(c->regs->map_access);
	if (rc)
		return rc;
	return 0;
}

static int cortina_vlan_rule_install(void *ctx, const struct gpon_vlan_rule *r)
{
	struct cortina_vlan_ctx *c = ctx;
	u32 data[3];
	struct reg off[3];
	u16 vid = 0;
	u8 tag_cnt = 0;
	int rc;

	rc = cortina_vlan_ctx_ok(c);
	if (rc)
		return rc;
	rc = cortina_vlan_rule_to_aft(r, &vid, &tag_cnt);
	if (rc)
		return rc;
	/* ⚠ REFUSED BEFORE ANY BUS TRAFFIC.  A tag whose TPID is in no slot
	 * makes the hardware silently drop the whole edit, so an entry written
	 * for it would read back perfect and do nothing. */
	if (tag_cnt && (c->tpid_slot < 0 || c->tpid_slot > 3))
		return -ENOENT;

	cortina_vlan_aft_words(vid, tag_cnt, c->tpid_slot,
			       &data[0], &data[1], &data[2]);
	off[0] = c->regs->l2fib_data0;
	off[1] = c->regs->l2fib_data1;
	off[2] = c->regs->l2fib_data2;
	/* DATA0, DATA1, DATA2, then GO -- the order the shipping path emits */
	rc = aft_write(c, c->regs->l2fib_access, c->fib_idx, data, off, 3);
	if (rc)
		return rc;
	data[0] = cortina_vlan_aft_map_word(c->lspid_map, c->fib_idx);
	off[0] = c->regs->map_data;
	return aft_write(c, c->regs->map_access, c->map_idx, data, off, 1);
}

/* Remove: clear the MAP entry FIRST, so a freed fib stops ...
 * dev/MEASURED-cortina_vlan_install.c.md sec 3. */
static int cortina_vlan_rule_remove(void *ctx, const struct gpon_vlan_rule *r)
{
	struct cortina_vlan_ctx *c = ctx;
	u32 data[3];
	struct reg off[3];
	int rc;

	(void)r;
	rc = cortina_vlan_ctx_ok(c);
	if (rc)
		return rc;
	data[0] = 0;
	off[0] = c->regs->map_data;
	rc = aft_write(c, c->regs->map_access, c->map_idx, data, off, 1);
	if (rc)
		return rc;
	data[0] = data[1] = data[2] = 0;
	off[0] = c->regs->l2fib_data0;
	off[1] = c->regs->l2fib_data1;
	off[2] = c->regs->l2fib_data2;
	return aft_write(c, c->regs->l2fib_access, c->fib_idx, data, off, 3);
}

/* ⚠ THE FIVE SLOTS LEFT NULL ARE A STATEMENT, NOT AN ...
 * dev/MEASURED-cortina_vlan_install.c.md sec 2. */
const struct gpon_vlan_ops cortina_vlan_ops = {
	.rule_install	= cortina_vlan_rule_install,
	.rule_remove	= cortina_vlan_rule_remove,
};
