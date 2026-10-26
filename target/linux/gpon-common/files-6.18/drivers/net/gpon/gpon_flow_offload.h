/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef GPON_FLOW_OFFLOAD_H
#define GPON_FLOW_OFFLOAD_H

#include <linux/errno.h>
#include <linux/types.h>

#include "gpon_flow.h"

struct net_device;
struct flow_cls_offload;

/* WHAT TO DO to the flow, in vocabulary no accelerator owns. */
struct gpon_flow_act {
	u32 nat_addr;		/* host order					*/
	u16 nat_port;		/* host order					*/
	u16 pppoe_sid;		/* 0 = not a PPPoE leg; the LIVE negotiated id	*/
	u8  gw_dmac[6];		/* next hop, from the rule's ETH mangle		*/
	u8  nat_is_da;		/* 0 = rewrite the SA (US), 1 = the DA (DS)	*/
	u8  nat_valid;
	u8  port_valid;
	u8  dmac_valid;		/* BOTH ETH mangle halves were seen		*/
	/* ONE 802.1Q tag on the WAN, carried only for a family declaring
	 * ops->wan_vlan: the VID the US leg pushes, or the DS leg's pop.	*/
	u16 vlan_vid;		/* US: 1..4094, 0 = no tag pushed		*/
	u8  vlan_pop;		/* DS: 1 = the rule pops one tag		*/
};

/* The devices the rule names, and which leg this is. */
struct gpon_flow_ctx {
	struct net_device *idev;	/* the rule's META ingress	*/
	struct net_device *odev;	/* the REDIRECT target		*/
	bool ds_leg;			/* the WAN->LAN reply leg	*/
};

/* ⚠ `install` OWNS THE INDEX IT RETURNS. */
struct gpon_flow_ops {
	/* Which side of the box is this netdev on?  The core needs it before it
	 * can decode the actions at all: the leg discriminates every mangle. */
	bool (*is_lan_side)(void *sh, struct net_device *dev);

	/* EVERY decision that must read or write silicon, AND */
	/* -> 0 installed; <0 the engine REFUSED; GPON_FLOW_DECLINED the family
	 * declined BY DESIGN -- this leg needs no flow of its own, which is
	 * not a failure and may not be tallied as one.  ⚠ THE ERRNO CANNOT
	 * CARRY THAT DISTINCTION: Luna returns -EOPNOTSUPP both for the reply
	 * leg and for every real refusal, so the core would have to guess. */
	int (*install)(void *sh, const struct gpon_flow_key *key,
		       const struct gpon_flow_act *act,
		       const struct gpon_flow_ctx *ctx, void *priv,
		       u32 *idx_out);
	int (*remove)(void *sh, u32 idx, void *priv);
	/* Optional rollback when a failed install can retain hardware resources. */
	int (*abort_install)(void *sh, u32 idx, void *priv);

	/* Fill `*lastused` (jiffies).  A family with no per-flow counters
	 * reports LIVENESS only, which is what TC actually asks for. */
	int (*stats)(void *sh, u32 idx, void *priv, unsigned long *lastused);

	/* Optional: a VLAN push/pop the core does not carry is refused, but a
	 * family that keeps a refusal ledger wants to ATTRIBUTE it rather than
	 * count it as one of N anonymous unsupported reasons. */
	void (*note_vlan_action)(void *sh, bool ds_leg, u16 vid);

	/* The engine can put ONE 802.1Q tag on the WAN: the core then carries
	 * a US push / DS pop as DATA (act->vlan_*) and install() decides.
	 * false = refused and attributed through note_vlan_action, as before. */
	bool wan_vlan;

	/* Bytes of per-entry family state, appended to the */
	size_t priv_size;
};

/* The VID an upstream leg egresses with: the rule's push, which must agree with
 * the tag the WAN L3 device carries when it has one. -> 0 and *vid (0 =
 * untagged), or -EINVAL when the two disagree. PURE. */
static inline int gpon_flow_wan_vid(const struct gpon_flow_act *a, u16 dev_vid,
				    u16 *vid)
{
	if (a->vlan_vid && dev_vid && a->vlan_vid != dev_vid)
		return -EINVAL;
	*vid = a->vlan_vid ? a->vlan_vid : dev_vid;
	return 0;
}

struct gpon_flow_offload;

struct gpon_flow_offload *gpon_flow_offload_new(const struct gpon_flow_ops *ops,
						void *sh);
void gpon_flow_offload_free(struct gpon_flow_offload *fo);

/* Tear down EVERY installed flow. */
int gpon_flow_offload_flush(struct gpon_flow_offload *fo);

/* The three TC verbs, dispatched by cookie. */
int gpon_flow_offload_replace(struct gpon_flow_offload *fo,
			      struct flow_cls_offload *f,
			      struct net_device *blockdev);
int gpon_flow_offload_destroy(struct gpon_flow_offload *fo,
			      struct flow_cls_offload *f);
int gpon_flow_offload_stats(struct gpon_flow_offload *fo,
			    struct flow_cls_offload *f);
/* One TC_SETUP_CLSFLOWER command, dispatched to the verb above. */
int gpon_flow_offload_tc(struct gpon_flow_offload *fo, struct flow_cls_offload *f,
			 struct net_device *blockdev);

/* Decode a rule's ACTIONS. */
int gpon_flow_act_from_tc(struct gpon_flow_offload *fo, struct flow_rule *rule,
			  bool ds_leg, struct gpon_flow_act *act,
			  struct net_device **odev_out,
			  enum gpon_flow_refusal *why);

/* CONFIG_GPON_FLOW_DIAG: render the lifecycle's own line.  @d carries what only
 * the family can read (its offer count, the engine's live/capacity/hits), each
 * field n/a unless its GPON_FDIAG_HAS_* bit is set.  Returns the length; with
 * the flag off the one line says the facility is ABSENT, never zeros. */
int gpon_flow_offload_diag(const struct gpon_flow_offload *fo,
			   const struct gpon_flow_diag *d, char *out, size_t sz);

#endif /* GPON_FLOW_OFFLOAD_H */
