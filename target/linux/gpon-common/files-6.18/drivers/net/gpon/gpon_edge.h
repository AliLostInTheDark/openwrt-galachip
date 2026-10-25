/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * TIER: CORE, not in the STRICT subset -- it reads Linux's own routing state on
 * purpose.  No MMIO, no device pointer, no table word, no chip fact below.
 *
 * gpon_edge -- the router EDGE, read from the LIVE kernel.  Every flow
 * accelerator here needs the same five things before it can rewrite a packet:
 * which source address and MAC the WAN egresses with, which next hop it
 * egresses THROUGH, and which subnet is behind the LAN.  None is a property of
 * any silicon, so deriving it once costs the next board a TABLE, not a copy.
 *
 * ★ PULLED, NOT PUSHED.  A notifier chain was the obvious shape and is the
 * wrong one: the values are wanted at exactly one moment, when a flow is being
 * installed, and are all resolved by construction then (the flow exists because
 * Linux forwarded a packet along them).  Pulling keeps the mechanism free of a
 * workqueue, a timer and a lock, and it cannot go stale.
 *
 * Copyright (C) 2026 Confiared <contact@confiared.com>
 */
#ifndef GPON_EDGE_H
#define GPON_EDGE_H

#include <linux/types.h>
#include <linux/if_ether.h>

struct net_device;

/* Addresses are HOST order. Every engine packs its own table ...
 * dev/MEASURED-gpon_edge.h.md sec 2. */
struct gpon_edge {
	u32	wan_ip;			/* the address a US frame is NAT'd to	*/
	u32	gw_ip;			/* the next hop that address egresses to */
	u32	lan_ip;			/* the ONU's own address on the LAN	*/
	u32	lan_net;		/* the LAN subnet, masked		*/
	u8	wan_mac[ETH_ALEN];	/* the WAN egress source MAC		*/
	u8	gw_mac[ETH_ALEN];	/* the next hop's resolved MAC		*/
	u8	lan_mac[ETH_ALEN];	/* the LAN L3 interface's MAC		*/
	u16	wan_vlan;		/* 0 = untagged				*/
	u16	lan_vlan;
	u8	lan_prefix;		/* LAN prefix length, 1..32		*/
	/* The LIVE negotiated PPPoE session, 0 when the WAN is not PPPoE. It is
	 * NOT read from the netdevice here: the routing layer has no portable
	 * way to ask a ppp device for its session, while the flowtable already
	 * hands it to us as FLOW_ACTION_PPPOE_PUSH. So the SHIM fills this from
	 * the action, and it rides in the edge because that is what decides when
	 * the interface tables must be rewritten -- a re-dialled session is a new
	 * edge, and an engine still holding the old one encapsulates with a
	 * session the far end has forgotten. */
	u16	wan_pppoe_sid;
};

/* Fill *e from the live kernel state. @lan and @wan are the ...
 * dev/MEASURED-gpon_edge.h.md sec 1. */
int gpon_edge_read(struct net_device *lan, struct net_device *wan,
		   u32 peer, struct gpon_edge *e, const char **why);
/* The same read, told what the flowtable already resolved: @odev is the egress
 * it picked and @dmac the L2 destination the flow carries. Either may be NULL,
 * and gpon_edge_read() is this with both NULL. */
int gpon_edge_read_via(struct net_device *lan, struct net_device *wan,
		       u32 peer, struct net_device *odev, const u8 *dmac,
		       struct gpon_edge *e, const char **why);

/* Byte-equality, so a caller can tell "the edge moved" from "nothing changed"
 * without re-deriving what any individual field means. */
bool gpon_edge_same(const struct gpon_edge *a, const struct gpon_edge *b);
/* True when everything EXCEPT the next hop matches -- i.e. the same WAN and LAN
 * interfaces, reached for a destination that resolves to a different next hop.
 * That is what an ON-LINK peer looks like, and it is a different question from
 * gpon_edge_same(): the interface tables can stay, the next hop cannot. */
bool gpon_edge_same_iface(const struct gpon_edge *a, const struct gpon_edge *b);

#endif /* GPON_EDGE_H */
