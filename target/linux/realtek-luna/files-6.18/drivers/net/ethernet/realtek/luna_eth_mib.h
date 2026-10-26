/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared switch statistics; attach before registering the netdev. */
#ifndef _LUNA_ETH_MIB_H
#define _LUNA_ETH_MIB_H

#include <linux/netdevice.h>
#include <linux/types.h>

struct luna_sw_map;

int luna_mib_attach(struct net_device *ndev, void __iomem *sw,
		    const struct luna_sw_map *map);

/* One port's standard counters from the switch MIB, for a per-port netdev's
 * ndo_get_stats64. -ENODEV: this chip publishes no such counters. */
struct rtnl_link_stats64;
int luna_mib_port_stats(void __iomem *sw, const struct luna_sw_map *map,
			unsigned int port, struct rtnl_link_stats64 *s);

#endif /* _LUNA_ETH_MIB_H */
