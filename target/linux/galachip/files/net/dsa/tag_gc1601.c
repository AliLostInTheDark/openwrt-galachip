// SPDX-License-Identifier: GPL-2.0-only
/*
 * GalaChip GC1601 packet engine tag: a one-byte trailer on transmit
 */

#include <linux/bitops.h>
#include <linux/dsa/gc1601.h>

#include "tag.h"

#define GC1601_NAME "gc1601"

static struct sk_buff *gc1601_tag_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct dsa_port *dp = dsa_user_to_port(dev);

	*(u8 *)skb_put(skb, GC1601_TAG_LEN) = BIT(dp->index);
	return skb;
}

/* The conduit sets a port metadata dst on every frame; anything else is stray. */
static struct sk_buff *gc1601_tag_rcv(struct sk_buff *skb, struct net_device *dev)
{
	return NULL;
}

static const struct dsa_device_ops gc1601_netdev_ops = {
	.name	= GC1601_NAME,
	.proto	= DSA_TAG_PROTO_GC1601,
	.xmit	= gc1601_tag_xmit,
	.rcv	= gc1601_tag_rcv,
	.needed_tailroom = GC1601_TAG_LEN,
};

MODULE_DESCRIPTION("DSA tag driver for the GalaChip GC1601 packet engine");
MODULE_LICENSE("GPL");
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_GC1601, GC1601_NAME);

module_dsa_tag_driver(gc1601_netdev_ops);
