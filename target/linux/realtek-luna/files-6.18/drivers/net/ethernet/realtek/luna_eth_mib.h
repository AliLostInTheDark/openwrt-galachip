/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared switch statistics; attach before registering the netdev. */
#ifndef _LUNA_ETH_MIB_H
#define _LUNA_ETH_MIB_H

#include <linux/netdevice.h>
#include <linux/types.h>

struct luna_sw_map;

int luna_mib_attach(struct net_device *ndev, void __iomem *sw,
		    const struct luna_sw_map *map);

#endif /* _LUNA_ETH_MIB_H */
