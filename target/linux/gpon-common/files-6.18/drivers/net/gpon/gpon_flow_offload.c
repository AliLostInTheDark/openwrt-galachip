// SPDX-License-Identifier: GPL-2.0-or-later
/* gpon_flow_offload */
#include <linux/errno.h>
#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/netdevice.h>
#include <linux/rhashtable.h>
#include <linux/slab.h>
#include <net/flow_offload.h>
#include <net/pkt_cls.h>

#include "gpon_flow_offload.h"

struct gpon_flow_entry {
	struct rhash_head	node;
	struct list_head	link;		/* fo->entries */
	unsigned long		cookie;
	u32			idx;		/* the engine's, opaque */
	bool			ds;
	bool			installing, failed, removed;
	/* ops->priv_size bytes of family state follow */
};

static void *entry_priv(struct gpon_flow_entry *e)
{
	return (void *)(e + 1);
}

struct gpon_flow_offload {
	const struct gpon_flow_ops	*ops;
	void				*sh;
	struct rhashtable		table;
	/* Every entry the table holds, for flush: rhashtable's walker may MISS
	 * objects removed between walk_stop and walk_start, which is exactly
	 * what a flush does (X111W 2026-09-23: rc 0, flows left behind). */
	struct list_head		entries;
	bool				table_ready;
	GPON_FLOW_TALLY_FIELD
};

static const struct rhashtable_params gpon_flow_ht_params = {
	.head_offset	= offsetof(struct gpon_flow_entry, node),
	.key_offset	= offsetof(struct gpon_flow_entry, cookie),
	.key_len	= sizeof(unsigned long),
	.automatic_shrinking = true,
};

struct gpon_flow_offload *gpon_flow_offload_new(const struct gpon_flow_ops *ops,
						void *sh)
{
	struct gpon_flow_offload *fo;

	if (!ops || !ops->is_lan_side || !ops->install || !ops->remove)
		return NULL;

	fo = kzalloc(sizeof(*fo), GFP_KERNEL);
	if (!fo)
		return NULL;
	fo->ops = ops;
	fo->sh = sh;
	INIT_LIST_HEAD(&fo->entries);
	if (rhashtable_init(&fo->table, &gpon_flow_ht_params)) {
		kfree(fo);
		return NULL;
	}
	fo->table_ready = true;
	return fo;
}

static void gpon_flow_entry_drop(void *ptr, void *arg)
{
	struct gpon_flow_offload *fo = arg;
	struct gpon_flow_entry *e = ptr;
	int err = 0;

	if (e->removed)
		err = 0;
	else if (!e->failed)
		err = fo->ops->remove(fo->sh, e->idx, entry_priv(e));
	else if (fo->ops->abort_install)
		err = fo->ops->abort_install(fo->sh, e->idx, entry_priv(e));
	if (err)
		pr_err("gpon-flow: teardown of cookie %lx failed (%d); the family must quiesce hardware before freeing the lifecycle\n",
		       e->cookie, err);
	kfree(e);
}

void gpon_flow_offload_free(struct gpon_flow_offload *fo)
{
	if (!fo)
		return;
	if (fo->table_ready) {
		fo->table_ready = false;
		rhashtable_free_and_destroy(&fo->table, gpon_flow_entry_drop, fo);
	}
	kfree(fo);
}

/* One WAN tag as DATA, for a family that declared ops->wan_vlan: a PUSH on the
 * upstream leg, a POP on the downstream one, and never a second tag. */
static bool gpon_flow_vlan_carry(const struct gpon_flow_offload *fo,
				 const struct flow_action_entry *fa, bool ds_leg,
				 struct gpon_flow_act *act)
{
	bool push = fa->id == FLOW_ACTION_VLAN_PUSH;

	if (!fo || !fo->ops->wan_vlan || act->vlan_vid || act->vlan_pop ||
	    push == ds_leg)
		return false;
	if (!push) {
		act->vlan_pop = 1;
		return true;
	}
	if (fa->vlan.proto != htons(ETH_P_8021Q) || !fa->vlan.vid ||
	    fa->vlan.vid >= VLAN_N_VID - 1)
		return false;
	act->vlan_vid = fa->vlan.vid;
	return true;
}

/* ── the ACTION */
int gpon_flow_act_from_tc(struct gpon_flow_offload *fo, struct flow_rule *rule,
			  bool ds_leg, struct gpon_flow_act *act,
			  struct net_device **odev_out,
			  enum gpon_flow_refusal *why)
{
	struct flow_action_entry *fa;
	bool got_dmac_lo = false, got_dmac_hi = false;
	int i;

	*odev_out = NULL;

	flow_action_for_each(i, fa, &rule->action) {
		switch (fa->id) {
		case FLOW_ACTION_REDIRECT:
			*odev_out = fa->dev;
			break;
		case FLOW_ACTION_MANGLE:
			switch (fa->mangle.htype) {
			case FLOW_ACT_MANGLE_HDR_TYPE_IP4:
				/* nf_flow_table's two legs of a masqueraded flow are */
				if (fa->mangle.offset !=
				    (ds_leg ? offsetof(struct iphdr, daddr)
					    : offsetof(struct iphdr, saddr))) {
					if (why)
						*why = GPON_FLOW_REF_NAT_OFFSET;
					return -EOPNOTSUPP;
				}
				act->nat_valid = 1;
				act->nat_is_da = ds_leg;
				act->nat_addr = ntohl(fa->mangle.val);
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_TCP:
			case FLOW_ACT_MANGLE_HDR_TYPE_UDP:
				/* The port rewrite is ONE big-endian 32-bit word at offset 0 */
				if (fa->mangle.offset != 0 ||
				    (fa->mangle.mask == ~htonl(0xffff)) != ds_leg) {
					if (why)
						*why = GPON_FLOW_REF_PORT_SHAPE;
					return -EOPNOTSUPP;
				}
				act->nat_port = ds_leg ?
					(ntohl(fa->mangle.val) & 0xffff) :
					(ntohl(fa->mangle.val) >> 16);
				act->port_valid = 1;
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_ETH:
				/* This leg's next-hop DMAC, emitted as TWO ETH mangles */
				if (fa->mangle.offset == 0) {
					u32 w = fa->mangle.val;

					memcpy(act->gw_dmac, &w, 4);
					got_dmac_lo = true;
				} else if (fa->mangle.offset == 4 &&
					   (fa->mangle.mask & 0xffff) == 0) {
					u16 w = (u16)fa->mangle.val;

					memcpy(act->gw_dmac + 4, &w, 2);
					got_dmac_hi = true;
				}
				break;
			default:
				if (why)
					*why = GPON_FLOW_REF_MANGLE_HTYPE;
				return -EOPNOTSUPP;
			}
			break;
		case FLOW_ACTION_CSUM:
			break;		/* implicit in the HW rewrite */
		case FLOW_ACTION_PPPOE_PUSH:
			/* The LIVE negotiated session id */
			act->pppoe_sid = fa->pppoe.sid;
			break;
		case FLOW_ACTION_VLAN_PUSH:
		case FLOW_ACTION_VLAN_POP:
			/* Reached only when the WAN sub-interface's LOWER device */
			if (gpon_flow_vlan_carry(fo, fa, ds_leg, act))
				break;
			if (fo && fo->ops->note_vlan_action)
				fo->ops->note_vlan_action(fo->sh, ds_leg,
					fa->id == FLOW_ACTION_VLAN_PUSH ?
					fa->vlan.vid : 0);
			if (why)
				*why = GPON_FLOW_REF_VLAN_ACTION;
			return -EOPNOTSUPP;
		default:
			if (why)
				*why = GPON_FLOW_REF_ACTION_ID;
			return -EOPNOTSUPP;
		}
	}

	act->dmac_valid = got_dmac_lo && got_dmac_hi;

	/*
	 * Keep to the proven shape: a full inline NAT rewrite plus a redirect.
	 * Anything less cannot be expressed as one entry.
	 */
	if (!*odev_out || !act->nat_valid || !act->port_valid) {
		if (why)
			*why = GPON_FLOW_REF_INCOMPLETE;
		return -EOPNOTSUPP;
	}
	return 0;
}

static int gpon_flow_entry_retire(struct gpon_flow_offload *fo,
				  struct gpon_flow_entry *entry)
{
	int err = 0;

	if (entry->installing)
		return -EBUSY;
	if (entry->removed)
		err = 0;
	else if (!entry->failed)
		err = fo->ops->remove(fo->sh, entry->idx, entry_priv(entry));
	else if (fo->ops->abort_install)
		err = fo->ops->abort_install(fo->sh, entry->idx, entry_priv(entry));
	if (err)
		return err;
	entry->removed = true;
	err = rhashtable_remove_fast(&fo->table, &entry->node, gpon_flow_ht_params);
	if (!err) {
		list_del(&entry->link);
		kfree(entry);
	}
	return err;
}

static int gpon_flow_install_entry(struct gpon_flow_offload *fo,
				   const struct gpon_flow_key *key,
				   const struct gpon_flow_act *act,
				   const struct gpon_flow_ctx *ctx,
				   unsigned long cookie,
				   enum gpon_flow_refusal *why)
{
	struct gpon_flow_entry *entry;
	int err, cleanup;

	entry = kzalloc(sizeof(*entry) + fo->ops->priv_size, GFP_KERNEL);
	if (!entry) {
		*why = GPON_FLOW_REF_NOMEM;
		return -ENOMEM;
	}
	entry->cookie = cookie;
	entry->ds = ctx->ds_leg;
	entry->installing = true;
	/* Claim the cookie before hardware; an install may flush the old edge. */
	err = rhashtable_insert_fast(&fo->table, &entry->node, gpon_flow_ht_params);
	if (err) {
		*why = GPON_FLOW_REF_TABLE_INSERT;
		kfree(entry);
		return err;
	}
	list_add_tail(&entry->link, &fo->entries);
	err = fo->ops->install(fo->sh, key, act, ctx, entry_priv(entry), &entry->idx);
	entry->installing = false;
	if (!err)
		return 0;
	entry->failed = true;
	*why = gpon_flow_install_cause(err);
	cleanup = gpon_flow_entry_retire(fo, entry);
	if (cleanup)
		pr_err_ratelimited("gpon-flow: cookie %lx rollback failed (%d); retaining its owner\n",
				   cookie, cleanup);
	return err > 0 ? -EOPNOTSUPP : err;
}

int gpon_flow_offload_replace(struct gpon_flow_offload *fo,
			      struct flow_cls_offload *f,
			      struct net_device *blockdev)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct gpon_flow_entry *entry;
	struct gpon_flow_ctx ctx = {};
	struct gpon_flow_act act = {};
	struct gpon_flow_key key = {};
	enum gpon_flow_refusal why = GPON_FLOW_OK;
	int err;

	if (!fo)
		return -EOPNOTSUPP;
	if (!fo->table_ready) {
		GPON_FLOW_NOTE(fo, GPON_FLOW_REF_NO_ENGINE);
		return -EOPNOTSUPP;
	}

	/* ⚠ NOT "the other direction": on 6.18 the cookie is the address of THIS * direction's own tuple, so the two legs never share one. A dup-cookie is * the SAME cookie delivered again -- most plausibly by the block-cb * fan-out. */
	entry = rhashtable_lookup_fast(&fo->table, &f->cookie, gpon_flow_ht_params);
	if (entry) {
		if (!entry->failed) {
			GPON_FLOW_NOTE(fo, GPON_FLOW_REF_DUP_COOKIE);
			return -EEXIST;
		}
		err = gpon_flow_entry_retire(fo, entry);
		if (err)
			return err;
	}

	err = gpon_flow_key_from_tc(rule, &key, &why);
	if (err) {
		GPON_FLOW_NOTE(fo, why);
		return err;
	}

	/* The block-cb device is NOT the flow's ingress */
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META)) {
		struct flow_match_meta m;

		flow_rule_match_meta(rule, &m);
		ctx.idev = dev_get_by_index(dev_net(blockdev),
					    m.key->ingress_ifindex);
		if (ctx.idev)
			ctx.ds_leg = !fo->ops->is_lan_side(fo->sh, ctx.idev);
	}

	err = gpon_flow_act_from_tc(fo, rule, ctx.ds_leg, &act, &ctx.odev, &why);
	if (err)
		goto out_put;

	err = gpon_flow_install_entry(fo, &key, &act, &ctx, f->cookie, &why);
	if (err)
		goto out_put;

	GPON_FLOW_NOTE(fo, GPON_FLOW_OK);
	if (ctx.idev)
		dev_put(ctx.idev);
	return 0;

out_put:
	GPON_FLOW_NOTE_LEG(fo, ctx.idev ? ctx.idev->ifindex : 0, ctx.ds_leg);
	GPON_FLOW_NOTE(fo, why);
	if (ctx.idev)
		dev_put(ctx.idev);
	return err;
}

/* CONFIG_GPON_FLOW_DIAG: the lifecycle's own line.  With the flag off the
 * facility is ABSENT and says so -- it never prints zeros nobody measured. */
int gpon_flow_offload_diag(const struct gpon_flow_offload *fo,
			   const struct gpon_flow_diag *d, char *out, size_t sz)
{
#if IS_ENABLED(CONFIG_GPON_FLOW_DIAG)
	if (!fo)
		return gpon_flow_diag_line(NULL, d, out, sz);
	return gpon_flow_diag_line(&fo->tally, d, out, sz);
#else
	(void)fo; (void)d;
	if (!out || !sz)
		return 0;
	return scnprintf(out, sz,
			 "flow: diagnostics not compiled in (CONFIG_GPON_FLOW_DIAG=n)");
#endif
}

int gpon_flow_offload_flush(struct gpon_flow_offload *fo)
{
	struct gpon_flow_entry *e, *next;
	int ret = 0;

	if (!fo || !fo->table_ready)
		return 0;

	list_for_each_entry_safe(e, next, &fo->entries, link) {
		int err = e->installing ? 0 : gpon_flow_entry_retire(fo, e);

		/* An entry the engine would NOT retire keeps its software owner:
		 * freeing it would drop the only handle that can ever retire it. */
		if (err && !ret)
			ret = err;
	}
	return ret;
}

int gpon_flow_offload_destroy(struct gpon_flow_offload *fo,
			      struct flow_cls_offload *f)
{
	struct gpon_flow_entry *entry;

	if (!fo || !fo->table_ready)
		return -EOPNOTSUPP;

	entry = rhashtable_lookup_fast(&fo->table, &f->cookie,
				       gpon_flow_ht_params);
	if (!entry)
		return -ENOENT;

	return gpon_flow_entry_retire(fo, entry);
}

int gpon_flow_offload_stats(struct gpon_flow_offload *fo,
			    struct flow_cls_offload *f)
{
	struct gpon_flow_entry *entry;
	unsigned long lastused = 0;
	int err;

	if (!fo || !fo->table_ready)
		return -EOPNOTSUPP;

	entry = rhashtable_lookup_fast(&fo->table, &f->cookie,
				       gpon_flow_ht_params);
	if (!entry)
		return -ENOENT;
	if (entry->installing || entry->failed || entry->removed)
		return -EBUSY;

	if (!fo->ops->stats)
		return -EOPNOTSUPP;

	err = fo->ops->stats(fo->sh, entry->idx, entry_priv(entry), &lastused);
	if (err)
		return err;

	f->stats.lastused = lastused;
	return 0;
}

int gpon_flow_offload_tc(struct gpon_flow_offload *fo, struct flow_cls_offload *f,
			 struct net_device *blockdev)
{
	switch (f->command) {
	case FLOW_CLS_REPLACE:
		return gpon_flow_offload_replace(fo, f, blockdev);
	case FLOW_CLS_DESTROY:
		return gpon_flow_offload_destroy(fo, f);
	case FLOW_CLS_STATS:
		return gpon_flow_offload_stats(fo, f);
	default:
		return -EOPNOTSUPP;
	}
}
