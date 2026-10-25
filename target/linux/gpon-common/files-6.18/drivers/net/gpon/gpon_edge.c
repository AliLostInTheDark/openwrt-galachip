// SPDX-License-Identifier: GPL-2.0-only
/*
 * gpon_edge -- read the router edge out of the live kernel.  See gpon_edge.h
 * for why this is core code and why it is pulled rather than pushed.
 *
 * Copyright (C) 2026 Confiared <contact@confiared.com>
 */
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/inetdevice.h>
#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <net/arp.h>
#include <net/neighbour.h>
#include <net/route.h>

#include "gpon_edge.h"

/* The L3 interface above a driver's netdev. On a router built ...
 * dev/MEASURED-gpon_edge.c.md sec 1. */
static struct net_device *gpon_edge_l3_dev(struct net_device *dev)
{
	struct net_device *master = netdev_master_upper_dev_get_rcu(dev);

	return master ? master : dev;
}

static bool gpon_edge_ipv4(struct net_device *dev, u32 *ip, u8 *prefix)
{
	struct in_device *in = __in_dev_get_rcu(dev);
	const struct in_ifaddr *ifa;

	if (!in)
		return false;
	in_dev_for_each_ifa_rcu(ifa, in) {
		if (ifa->ifa_scope != RT_SCOPE_UNIVERSE)
			continue;
		*ip = ntohl(ifa->ifa_local);
		if (prefix)
			*prefix = ifa->ifa_prefixlen;
		return true;
	}
	return false;
}

static u16 gpon_edge_vlan(struct net_device *dev)
{
	return is_vlan_dev(dev) ? vlan_dev_vlan_id(dev) : 0;
}

/* Is @l3 the driver's own @port, or a VLAN riding directly on it? */
static bool gpon_edge_rides(struct net_device *l3, struct net_device *port)
{
	if (l3 == port)
		return true;
	return is_vlan_dev(l3) && vlan_dev_real_dev(l3) == port;
}

static bool gpon_edge_gw_mac(struct net_device *dev, __be32 gw, u8 *mac)
{
	struct neighbour *n = __ipv4_neigh_lookup_noref(dev, (__force u32)gw);

	if (!n || !(READ_ONCE(n->nud_state) & NUD_VALID))
		return false;
	neigh_ha_snapshot((char *)mac, n, dev);
	return is_valid_ether_addr(mac);
}

/* The edge, told what the FLOWTABLE already resolved.
 *
 * ★★★ WHY THE EXTRA ARGUMENTS EXIST. This function follows the ROUTE to learn
 * the WAN address, the next hop and its MAC -- which is right, and is its job.
 * What was wrong was requiring the ROUTED device to BE the port: on a PPPoE WAN
 * the route egresses the `ppp` netdev, so 1056 flows were refused on the X111W
 * (MEASURED 2026-09-18) for a path the infrastructure had already resolved onto
 * our port. The netfilter flowtable "discovers the real netdevice behind VLAN
 * and PPPoE netdevices" and hands the encapsulation over as an ACTION, so the
 * caller knows the true egress and we should not re-derive it and then disagree
 * with ourselves.
 *
 * @odev: the egress the flowtable resolved (FLOW_ACTION_REDIRECT), or NULL.
 * @dmac: the L2 destination that flow carries, or NULL. On a PPPoE link there
 *        is no ARP and no neighbour entry -- the frame goes to the access
 *        concentrator, whose MAC only the session knows. The flow's own mangle
 *        action carries it, so a caller that has it may supply it rather than
 *        asking a neighbour table that will never answer.
 */
int gpon_edge_read_via(struct net_device *lan, struct net_device *wan,
		       u32 peer, struct net_device *odev, const u8 *dmac,
		       struct gpon_edge *e, const char **why)
{
	struct net_device *lan_l3, *wan_l3;
	struct flowi4 fl4 = { .daddr = htonl(peer) };
	struct rtable *rt;
	__be32 next_hop;
	int ret = -EAGAIN;

	*why = "";
	if (!lan || !wan) {
		*why = "the driver has no LAN/WAN netdev pair";
		return -ENODEV;
	}

	memset(e, 0, sizeof(*e));
	rcu_read_lock();

	/* ★★ THE ROUTE COMES FIRST, AND IT NAMES THE WAN L3 DEVICE. ⚠ ...
	 * dev/MEASURED-gpon_edge.c.md sec 2. */
	rt = ip_route_output_key(dev_net(wan), &fl4);
	if (IS_ERR(rt)) {
		*why = "no route to the flow's destination";
		ret = PTR_ERR(rt);
		goto out;
	}
	/* ★★ AN ON-LINK DESTINATION HAS A NEXT HOP, AND IT IS THE PEER ITSELF.
	 * AF_UNSPEC means the route needs no gateway because the destination is
	 * on this interface's own subnet -- rt_nexthop() is the kernel's own
	 * answer to "who do I hand this frame to", and it returns the daddr
	 * there.  Refusing AF_UNSPEC turned every WAN-subnet destination into
	 * "no IPv4 gateway", which is what the X111W's engine said to 1572
	 * flows on 2026-09-15 for a bench sink on the ONU's own /24. */
	if (rt->rt_gw_family != AF_INET && rt->rt_gw_family != AF_UNSPEC) {
		*why = "the WAN route uses an unsupported gateway family";
		ret = -EOPNOTSUPP;
		goto out_put;
	}
	wan_l3 = rt->dst.dev;
	/* ⚠ AND IT MUST LEAVE THROUGH THE PORT THIS ENGINE OWNS. A ...
	 * dev/MEASURED-gpon_edge.c.md sec 3. */
	if (!gpon_edge_rides(wan_l3, wan) && odev != wan) {
		*why = "the WAN route leaves through a device this engine does not drive";
		ret = -EOPNOTSUPP;
		goto out_put;
	}
	if (!gpon_edge_ipv4(wan_l3, &e->wan_ip, NULL)) {
		*why = "the WAN interface carries no IPv4 address yet";
		goto out_put;
	}
	e->wan_vlan = gpon_edge_vlan(wan_l3);
	/* ⚠ A `ppp` netdev HAS NO ETHERNET ADDRESS, so taking the egress source
	 * MAC from it would program the interface table with zeroes and put
	 * frames on the wire from 00:00:00:00:00:00. A VLAN device inherits its
	 * real device's address, so the two cases that RIDE the port keep using
	 * the L3 device; anything else takes the port's own. */
	ether_addr_copy(e->wan_mac, gpon_edge_rides(wan_l3, wan)
				    ? wan_l3->dev_addr : wan->dev_addr);
	next_hop = rt_nexthop(rt, fl4.daddr);
	e->gw_ip = ntohl(next_hop);
	if (!gpon_edge_gw_mac(wan_l3, next_hop, e->gw_mac)) {
		if (dmac && is_valid_ether_addr(dmac)) {
			/* the access concentrator, from the flow's own rewrite */
			ether_addr_copy(e->gw_mac, dmac);
		} else {
			*why = "the next hop's MAC is not resolved yet";
			goto out_put;
		}
	}

	/* The LAN side IS reached through the master upper: an ordinary router
	 * enslaves the LAN port to a bridge and the address lives there. */
	lan_l3 = gpon_edge_l3_dev(lan);
	if (!gpon_edge_ipv4(lan_l3, &e->lan_ip, &e->lan_prefix)) {
		*why = "the LAN interface carries no IPv4 address yet";
		goto out_put;
	}
	e->lan_net = e->lan_ip & (e->lan_prefix == 32
				  ? ~0u : ~0u << (32 - e->lan_prefix));
	e->lan_vlan = gpon_edge_vlan(lan_l3);
	ether_addr_copy(e->lan_mac, lan_l3->dev_addr);
	ret = 0;
out_put:
	ip_rt_put(rt);
out:
	rcu_read_unlock();
	return ret;
}

/* The original spelling, for every caller that has neither: it asks exactly what
 * it used to ask, so nothing that compiled before behaves differently now. */
int gpon_edge_read(struct net_device *lan, struct net_device *wan,
		   u32 peer, struct gpon_edge *e, const char **why)
{
	return gpon_edge_read_via(lan, wan, peer, NULL, NULL, e, why);
}

bool gpon_edge_same_iface(const struct gpon_edge *a, const struct gpon_edge *b)
{
	return a->wan_ip == b->wan_ip && a->lan_ip == b->lan_ip &&
	       a->lan_net == b->lan_net && a->wan_vlan == b->wan_vlan &&
	       a->lan_vlan == b->lan_vlan && a->lan_prefix == b->lan_prefix &&
	       a->wan_pppoe_sid == b->wan_pppoe_sid &&
	       ether_addr_equal(a->wan_mac, b->wan_mac) &&
	       ether_addr_equal(a->lan_mac, b->lan_mac);
}

bool gpon_edge_same(const struct gpon_edge *a, const struct gpon_edge *b)
{
	return gpon_edge_same_iface(a, b) && a->gw_ip == b->gw_ip &&
	       ether_addr_equal(a->gw_mac, b->gw_mac);
}
