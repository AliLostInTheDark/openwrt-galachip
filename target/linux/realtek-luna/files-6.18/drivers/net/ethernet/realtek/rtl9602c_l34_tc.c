// SPDX-License-Identifier: GPL-2.0-only
/* RTL9602C L34 -> the COMMON TC hardware-offload lifecycle. */

#include <linux/if_vlan.h>
#include <linux/inetdevice.h>	/* in_ifaddr, the WAN address notifier */
#include <net/flow_offload.h>	/* struct flow_cls_offload, the block cb type */
#include <net/pkt_cls.h>	/* FLOW_CLS_*, FLOW_BLOCK_*, flow_block_cb_*   */
#include "gpon_flow_block.h"

static const struct net_device_ops rtl9602c_eth_netdev_ops;

/* The driver behind a netdev. */
static struct rtl9602c_eth *rtl9602c_eth_of(struct net_device *dev)
{
	if (!dev)
		return NULL;
	if (dev->netdev_ops == &rtl9602c_eth_wan_ops)
		return *(struct rtl9602c_eth **)netdev_priv(dev);
	if (dev->netdev_ops == &rtl9602c_eth_netdev_ops)
		return netdev_priv(dev);
	return NULL;
}

struct rtl9602c_l34_priv {
	struct l34_flow	f;		/* handed back to flow_del */
	unsigned long	last_hit;	/* fed by the stats op */
};

static bool rtl9602c_l34_is_lan_side(void *sh, struct net_device *dev)
{
	/*
	 * Structural, never a name. A VLAN upper is on its real device's side:
	 * the flowtable offers a tagged WAN->LAN leg from gpon0.<vid> when
	 * gpon0 itself is not a flowtable device.
	 */
	if (dev && is_vlan_dev(dev))
		dev = vlan_dev_real_dev(dev);
	return dev && dev->netdev_ops != &rtl9602c_eth_wan_ops;
}

/* The upstream leg owns NAPT; the downstream leg owns its LAN neighbour. */
static int l34_refuse(struct rtl9602c_l34 *l, const char *why)
{
	unsigned int i;

	l->refusals++;
	l->refuse_why = why;			/* a static literal, always */
	/* Key on the POINTER: the reasons are static literals, so this needs no
	 * allocation and no comparison of text. A reason past the table is still
	 * COUNTED, in the last slot, under a name that says so -- silently
	 * dropping it would be the same blindness one slot smaller. */
	for (i = 0; i < L34_REFUSE_REASONS; i++) {
		if (l->refuse_tally[i].why == why) {		/* seen before */
			l->refuse_tally[i].n++;
			break;
		}
		if (!l->refuse_tally[i].why) {			/* first of its kind */
			l->refuse_tally[i].why = why;
			l->refuse_tally[i].n = 1;
			break;
		}
	}
	if (i == L34_REFUSE_REASONS) {
		/*
		 * Table full and this reason is new: still counted, in the last
		 * slot, under a name saying the split stopped being exact
		 * there.
		 */
		l->refuse_tally[L34_REFUSE_REASONS - 1].why =
			"(further reasons, table full)";
		l->refuse_tally[L34_REFUSE_REASONS - 1].n++;
	}
	pr_debug_ratelimited("rtl9602c-l34: not offloading -- %s\n", why);
	return -EOPNOTSUPP;
}

static int rtl9602c_l34_op_install(void *sh, const struct gpon_flow_key *k,
				   const struct gpon_flow_act *a,
				   const struct gpon_flow_ctx *ctx, void *priv,
				   u32 *idx_out)
{
	struct rtl9602c_eth *ep = sh;
	struct rtl9602c_l34_priv *p = priv;
	struct l34_flow *f = &p->f;
	struct gpon_edge edge;
	const char *why = "";
	int ret;

	*idx_out = 0;
	if (!ep || !ep->l34.ready)
		return -ENODEV;
	/* Record the key BEFORE any refusal: a flow that is declined is exactly
	 * the one nothing else names.  See struct l34_offer. */
	{
		struct l34_offer *o = &ep->l34.recent_offer[ep->l34.offer_n %
							    L34_RECENT];

		o->sip   = k->ip_sa;
		o->dip   = k->ip_da;
		o->sport = k->l4_sport;
		o->dport = k->l4_dport;
		o->proto = k->ip_protocol;
		o->ds_leg = ctx->ds_leg;
		strscpy(o->iif, ctx->idev ? ctx->idev->name : "-", sizeof(o->iif));
		ep->l34.offer_n++;
	}
	if (ctx->ds_leg || a->nat_is_da) {
		ep->l34.ds_legs++;
		if (!a->dmac_valid || !ctx->odev ||
		    !rtl9602c_l34_is_lan_side(sh, ctx->odev))
			return -EOPNOTSUPP;
		ret = l34_neigh_add(&ep->l34, f, a->nat_addr, a->gw_dmac);
		if (!ret) {
			*idx_out = f->hw_index;
			p->last_hit = jiffies;
		}
		return ret;
	}

	/* The values first, the flow second. */
	/*
	 * Tell it what the flowtable already resolved. ctx->odev is the egress
	 * the infrastructure picked (FLOW_ACTION_REDIRECT) after walking past
	 * any VLAN or PPPoE device, and a->gw_dmac is the L2 destination this
	 * flow rewrites to: on a PPPoE link that is the access concentrator,
	 * which no neighbour table can supply.
	 */
	ret = gpon_edge_read_via(ep->ndev, ep->wan_ndev, k->ip_da, ctx->odev,
				 a->dmac_valid ? a->gw_dmac : NULL, &edge, &why);
	if (ret)
		return l34_refuse(&ep->l34, why);
	/*
	 * The session comes from the flow, not the netdevice. gpon_edge reads
	 * the live kernel, which offers no portable way to ask a ppp device for
	 * its session id, but the flowtable parsed it as FLOW_ACTION_PPPOE_PUSH
	 * and the core kept it in a->pppoe_sid. Putting it in the edge makes a
	 * re-dialled session a different edge, so the interface tables are
	 * rewritten instead of encapsulating with a session the far end has
	 * forgotten.
	 */
	edge.wan_pppoe_sid = a->pppoe_sid;
	/* The tag comes from the flow too: on PPPoE over gpon0.<vid> the routed
	 * device is the ppp one, which carries no VID of its own. */
	if (gpon_flow_wan_vid(a, edge.wan_vlan, &edge.wan_vlan))
		return l34_refuse(&ep->l34,
				  "the rule's VLAN push disagrees with the WAN "
				  "device's own tag");

	/* The rule's own next hop and NAT address must be the */
	if (a->dmac_valid && !ether_addr_equal(a->gw_dmac, edge.gw_mac))
		return l34_refuse(&ep->l34,
				  "the rule's next hop is not the WAN gateway "
				  "the interface tables describe");
	if (a->nat_addr != edge.wan_ip)
		return l34_refuse(&ep->l34,
				  "the rule NATs to an address the WAN "
				  "interface does not hold");

	if (!ep->l34.provisioned || !gpon_edge_same(&edge, &ep->l34.edge)) {
		/* The installed flows belong to the old edge. */
		/*
		 * One next hop, and an on-link peer wants its own. The core
		 * serves on-link destinations (the next hop is the peer), so a
		 * second WAN-subnet peer produces an edge that differs only in
		 * the next hop, and these interface tables hold exactly one.
		 * Flushing here would retire every flow the first peer is
		 * using, once per new peer. Refuse this flow instead: it stays
		 * on the software path and the installed ones keep theirs.
		 * Lifting this needs a per-peer NEXTHOP/ARP allocator; l34_flow
		 * already carries a nexthop index for it.
		 */
		if (ep->l34.provisioned &&
		    gpon_edge_same_iface(&edge, &ep->l34.edge) &&
		    rtl9602c_l34_has_owners(&ep->l34))
			return l34_refuse(&ep->l34,
					  "this destination needs its own next "
					  "hop and the interface tables hold one");
		if (ep->l34.provisioned) {
			ret = gpon_flow_offload_flush(ep->fo);
			/*
			 * A flow that would not retire still forwards, through
			 * the interface tables about to be re-pointed at the
			 * new edge. Re-pointing them now sends that live flow
			 * to a gateway it was never NAT'd for.
			 */
			if (ret)
				return l34_refuse(&ep->l34,
						  "a flow installed for the "
						  "previous edge would not "
						  "retire, so its hardware "
						  "entry still forwards");
		}
		ret = rtl9602c_l34_provision(&ep->l34, &edge);
		if (ret) {
			pr_warn_ratelimited("rtl9602c-l34: the interface tables would not program (%d); staying on software forwarding\n",
				ret);
			return l34_refuse(&ep->l34,
					  "the interface tables would not "
					  "program (see dmesg)");
		}
	}

	/* Everything the engine needs about the FLOW is */
	f->l4proto    = k->ip_protocol;
	f->orig_sip   = k->ip_sa;
	f->orig_dip   = k->ip_da;
	f->orig_sport = k->l4_sport;
	f->orig_dport = k->l4_dport;
	f->nat_sip    = a->nat_addr;
	f->nat_sport  = a->nat_port;
	f->egress_netif = L34_NETIF_WAN;
	f->nexthop      = L34_NETIF_WAN;

	ret = rtl9602c_l34_flow_add(&ep->l34, f);
	if (ret) {
		l34_refuse(&ep->l34, ret == -ENOSPC
			   ? "every way of the flow's hash bucket is taken"
			   : "the engine refused the NAPT entry");
		return ret;
	}
	*idx_out = f->hw_index;
	/*
	 * Seed the liveness clock, or the flow is torn down before it can be
	 * hit. priv arrives zeroed, op_stats writes last_hit only inside its if
	 * (active) branch and reports *lastused = p->last_hit, so a flow polled
	 * before its first matched packet answers lastused = 0, which the
	 * flowtable GC reads as idle since jiffies 0 and retires immediately.
	 */
	p->last_hit = jiffies;
	/* Remember it so the dump can read the entry back -- see L34_RECENT. */
	ep->l34.recent_idx[ep->l34.recent_n % L34_RECENT] = (u16)f->hw_index;
	ep->l34.recent_n++;
	return 0;
}

static int rtl9602c_l34_op_remove(void *sh, u32 idx, void *priv)
{
	struct rtl9602c_eth *ep = sh;
	struct rtl9602c_l34_priv *p = priv;

	/*
	 * idx is the core's published index, not an identity: a cleanup after
	 * an install that failed before publishing arrives with 0, which would
	 * retire entry zero, belonging to somebody else. The flow's own
	 * retained indices name what it actually claimed.
	 */
	(void)idx;
	if (!p->f.out_owned && !p->f.in_owned && !p->f.neigh_owned)
		return 0;	/* nothing claimed: cleanup succeeds, engine or not */
	if (!ep || !ep->l34.ready)
		return -ENODEV;
	return rtl9602c_l34_flow_del(&ep->l34, &p->f);
}

static int rtl9602c_l34_op_stats(void *sh, u32 idx, void *priv,
				 unsigned long *lastused)
{
	struct rtl9602c_eth *ep = sh;
	struct rtl9602c_l34_priv *p = priv;
	bool active = false;

	if (!ep || !ep->l34.ready)
		return -ENODEV;
	if (p->f.neigh_owned) {
		*lastused = p->last_hit;
		return 0;
	}
	/* The engine reports LIVENESS, not counters */
	if (!rtl9602c_l34_flow_hit(&ep->l34, (u16)idx, &active) && active)
		p->last_hit = jiffies;
	*lastused = p->last_hit;
	return 0;
}

/* devm release */
static void rtl9602c_l34_fo_release(void *data)
{
	struct rtl9602c_eth *ep = data;

	unregister_inetaddr_notifier(&ep->l34_addr_nb);
	gpon_flow_offload_free(ep->fo);
	ep->fo = NULL;
}

static void rtl9602c_l34_op_note_vlan(void *sh, bool ds_leg, u16 vid)
{
	struct rtl9602c_eth *ep = sh;

	if (!ep)
		return;
	ep->l34.vlan_refused++;
	ep->l34.vlan_refused_vid = vid;
}

static const struct gpon_flow_ops rtl9602c_l34_flow_ops = {
	.is_lan_side	= rtl9602c_l34_is_lan_side,
	.note_vlan_action = rtl9602c_l34_op_note_vlan,
	.wan_vlan	= true,		/* NETIF VLANID + CTAG_IF carry the tag */
	.install	= rtl9602c_l34_op_install,
	.remove		= rtl9602c_l34_op_remove,
	.abort_install	= rtl9602c_l34_op_remove,
	.stats		= rtl9602c_l34_op_stats,
	.priv_size	= sizeof(struct rtl9602c_l34_priv),
};

/* The TC block. */
static DEFINE_MUTEX(rtl9602c_l34_tc_mutex);
static LIST_HEAD(rtl9602c_l34_block_cb_list);

static int rtl9602c_l34_block_cb(enum tc_setup_type type, void *type_data,
				 void *cb_priv)
{
	struct flow_cls_offload *f = type_data;
	struct net_device *dev = cb_priv;
	struct rtl9602c_eth *ep;
	int err;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;
	ep = rtl9602c_eth_of(dev);
	if (!ep || !ep->fo)
		return -EOPNOTSUPP;

	mutex_lock(&rtl9602c_l34_tc_mutex);
	/* who refused is named by CONFIG_GPON_FLOW_DIAG at the point of refusal */
	if (f->command == FLOW_CLS_REPLACE)
		ep->l34.offered++;
	err = gpon_flow_offload_tc(ep->fo, f, dev);
	mutex_unlock(&rtl9602c_l34_tc_mutex);
	return err;
}

/* The provisioned WAN address left the kernel (a PPPoE session ended, a lease
 * changed): retire the flows built on it and release the WAN rows, so the next
 * flow provisions the new edge and nothing in between is claimed. */
static int rtl9602c_l34_inetaddr_event(struct notifier_block *nb,
				       unsigned long event, void *ptr)
{
	struct rtl9602c_eth *ep = container_of(nb, struct rtl9602c_eth, l34_addr_nb);
	struct in_ifaddr *ifa = ptr;
	int ret;

	if (event != NETDEV_DOWN || !ep->fo || !ep->l34.provisioned ||
	    ntohl(ifa->ifa_local) != ep->l34.edge.wan_ip)
		return NOTIFY_DONE;
	mutex_lock(&rtl9602c_l34_tc_mutex);
	/* under this mutex no install is in flight, so a flush that succeeded
	 * left the core's table empty */
	ret = gpon_flow_offload_flush(ep->fo);
	if (!ret)
		ret = rtl9602c_l34_wan_release(&ep->l34, true);
	mutex_unlock(&rtl9602c_l34_tc_mutex);
	if (ret)
		netdev_warn(ep->ndev, "L34: WAN %pI4 left and its tables were NOT released (%d)\n",
			    &ifa->ifa_local, ret);
	else
		netdev_info(ep->ndev, "L34: WAN %pI4 left; its tables are released\n",
			    &ifa->ifa_local);
	return NOTIFY_DONE;
}

static int rtl9602c_l34_setup_block(struct net_device *dev,
				    struct flow_block_offload *f)
{
	struct rtl9602c_eth *ep = rtl9602c_eth_of(dev);

	/* Do not claim a block this driver cannot honour. */
	if (!ep || !ep->fo)
		return -EOPNOTSUPP;

	return gpon_flow_block_setup(dev, f, &rtl9602c_l34_block_cb_list,
				     rtl9602c_l34_block_cb, &ep->l34.binds);
}

static int rtl9602c_l34_setup_tc(struct net_device *dev,
				 enum tc_setup_type type, void *type_data)
{
	switch (type) {
	case TC_SETUP_BLOCK:
	case TC_SETUP_FT:
		return rtl9602c_l34_setup_block(dev, type_data);
	default:
		return -EOPNOTSUPP;
	}
}
