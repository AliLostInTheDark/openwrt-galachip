// SPDX-License-Identifier: GPL-2.0-only

#include <linux/if_ether.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <net/genetlink.h>
#include <net/xpon.h>

#include "internal.h"

static const struct nla_policy xpon_genl_policy[XPON_ATTR_MAX + 1] = {
	[XPON_ATTR_IFINDEX] = { .type = NLA_U32 },
	[XPON_ATTR_MODE] = { .type = NLA_U8 },
	[XPON_ATTR_TX_ALLOW] = NLA_POLICY_MAX(NLA_U8, 1),
	[XPON_ATTR_TX_HOLD] = NLA_POLICY_MAX(NLA_U8, 1),
	[XPON_ATTR_TX_HOLD_SECONDS] = NLA_POLICY_MAX(NLA_U32, U16_MAX - 1),
	[XPON_ATTR_IDENTITY_WAIT] = NLA_POLICY_MAX(NLA_U8, 0),
	[XPON_ATTR_EPON_MAC] = NLA_POLICY_EXACT_LEN(ETH_ALEN),
	[XPON_ATTR_EPON_OAM_LINK] = NLA_POLICY_MAX(NLA_U8, 1),
	[XPON_ATTR_EPON_GENERATION] = { .type = NLA_U32 },
	[XPON_ATTR_EPON_REGISTER] = NLA_POLICY_MAX(NLA_U8, 1),
	[XPON_ATTR_EPON_KEY_LLID] = { .type = NLA_U8 },
	[XPON_ATTR_EPON_KEY_INDEX] = NLA_POLICY_MAX(NLA_U8, 1),
	[XPON_ATTR_EPON_KEY] = { .type = NLA_U32 },	/* 24 bits: the driver checks */
	[XPON_ATTR_EPON_HOLDOVER_MS] = NLA_POLICY_MAX(NLA_U32, 1000),
	[XPON_ATTR_SFU_OFFLOAD] = NLA_POLICY_MAX(NLA_U8, 1),
	[XPON_ATTR_SFU_UNTAG_VLAN] = NLA_POLICY_MAX(NLA_U16, 4094),
	[XPON_ATTR_SFU_VLAN_MAP] = { .type = NLA_BINARY,
				     .len = XPON_VLAN_MAP_MAX * sizeof(struct xpon_vlan_map) },
};

static struct genl_family xpon_genl_family;

static int xpon_genl_put_state(struct sk_buff *msg, struct xpon_device *xpon)
{
	if (nla_put_u32(msg, XPON_ATTR_IFINDEX, xpon->netdev->ifindex) ||
	    nla_put_u8(msg, XPON_ATTR_MODE, xpon->state.mode) ||
	    nla_put_u32(msg, XPON_ATTR_AVAILABLE_MODES, xpon->modes) ||
	    nla_put_u8(msg, XPON_ATTR_REGISTRATION, xpon->state.registration) ||
	    nla_put_u8(msg, XPON_ATTR_CARRIER, xpon->state.carrier) ||
	    nla_put_u8(msg, XPON_ATTR_SIGNAL_DETECT, xpon->state.signal_detect) ||
	    nla_put_u8(msg, XPON_ATTR_LOS, xpon->state.los))
		return -EMSGSIZE;

	if (xpon->ops && xpon->ops->get_tx) {
		struct xpon_tx_state tx = {};

		xpon->ops->get_tx(xpon, &tx);
		if (nla_put_u8(msg, XPON_ATTR_TX_ALLOW, tx.allow) ||
		    nla_put_u8(msg, XPON_ATTR_TX_HOLD, tx.hold) ||
		    nla_put_u32(msg, XPON_ATTR_TX_HOLD_SECONDS, tx.hold_seconds) ||
		    nla_put_u8(msg, XPON_ATTR_IDENTITY_WAIT, tx.identity_wait))
			return -EMSGSIZE;
	}
	if (xpon->ops && xpon->ops->get_epon) {
		struct xpon_epon_state epon = {};

		xpon->ops->get_epon(xpon, &epon);
		if (nla_put(msg, XPON_ATTR_EPON_MAC, ETH_ALEN, epon.mac) ||
		    nla_put_u8(msg, XPON_ATTR_EPON_OAM_LINK, epon.oam_link) ||
		    nla_put_u8(msg, XPON_ATTR_EPON_REGISTER, epon.register_en) ||
		    nla_put_u32(msg, XPON_ATTR_EPON_HOLDOVER_MS, epon.holdover_ms) ||
		    nla_put_u32(msg, XPON_ATTR_EPON_GENERATION, epon.generation) ||
		    nla_put_u8(msg, XPON_ATTR_EPON_SYNC, epon.sync) ||
		    nla_put_u8(msg, XPON_ATTR_EPON_NACK_SILENT, epon.nack_silent) ||
		    nla_put_u32(msg, XPON_ATTR_EPON_REGISTERED, epon.registered))
			return -EMSGSIZE;
	}
	if (xpon->ops && xpon->ops->get_sfu) {
		struct xpon_sfu_state sfu = {};

		xpon->ops->get_sfu(xpon, &sfu);
		if (nla_put_u8(msg, XPON_ATTR_SFU_OFFLOAD, sfu.offload) ||
		    nla_put_u16(msg, XPON_ATTR_SFU_UNTAG_VLAN, sfu.untag_vlan) ||
		    nla_put(msg, XPON_ATTR_SFU_VLAN_MAP,
			    sfu.vlan_maps * sizeof(sfu.vlan_map[0]), sfu.vlan_map))
			return -EMSGSIZE;
	}
	return 0;
}

/* The module's diagnostics: absent while it cannot be read. */
static int xpon_genl_put_module(struct sk_buff *msg, struct xpon_device *xpon)
{
	u8 diag[XPON_MODULE_DIAG_LEN], thresholds[XPON_MODULE_THRESHOLDS_LEN];

	if (!xpon->ops || !xpon->ops->get_module ||
	    xpon->ops->get_module(xpon, diag, thresholds))
		return 0;
	if (nla_put(msg, XPON_ATTR_MODULE_DIAG, sizeof(diag), diag) ||
	    nla_put(msg, XPON_ATTR_MODULE_THRESHOLDS, sizeof(thresholds), thresholds))
		return -EMSGSIZE;
	return 0;
}

static int xpon_genl_set_sfu(struct xpon_device *xpon, struct nlattr **a)
{
	const struct xpon_device_ops *ops = xpon->ops;
	int ret = 0;

	if (a[XPON_ATTR_SFU_VLAN_MAP]) {
		int len = nla_len(a[XPON_ATTR_SFU_VLAN_MAP]);

		if (len % sizeof(struct xpon_vlan_map))
			return -EINVAL;
		ret = ops->set_sfu_vlan_map ?
		      ops->set_sfu_vlan_map(xpon, nla_data(a[XPON_ATTR_SFU_VLAN_MAP]),
					    len / sizeof(struct xpon_vlan_map)) :
		      -EOPNOTSUPP;
	}
	if (!ret && a[XPON_ATTR_SFU_UNTAG_VLAN])
		ret = ops->set_sfu_untag_vlan ?
		      ops->set_sfu_untag_vlan(xpon, nla_get_u16(a[XPON_ATTR_SFU_UNTAG_VLAN])) :
		      -EOPNOTSUPP;
	if (!ret && a[XPON_ATTR_SFU_OFFLOAD])
		ret = ops->set_sfu_offload ?
		      ops->set_sfu_offload(xpon, nla_get_u8(a[XPON_ATTR_SFU_OFFLOAD])) :
		      -EOPNOTSUPP;
	return ret;
}

/* The EPON attributes of a SET; the key needs its LLID and slot with it. */
static int xpon_genl_set_epon(struct xpon_device *xpon, struct nlattr **a)
{
	const struct xpon_device_ops *ops = xpon->ops;
	u32 generation;
	int ret = 0;

	if (a[XPON_ATTR_EPON_MAC])
		ret = ops->set_epon_mac ? ops->set_epon_mac(xpon, nla_data(a[XPON_ATTR_EPON_MAC])) :
					  -EOPNOTSUPP;
	if (!ret && a[XPON_ATTR_EPON_REGISTER])
		ret = ops->set_epon_register ?
		      ops->set_epon_register(xpon, nla_get_u8(a[XPON_ATTR_EPON_REGISTER])) :
		      -EOPNOTSUPP;
	if (!ret && a[XPON_ATTR_EPON_HOLDOVER_MS])
		ret = ops->set_epon_holdover ?
		      ops->set_epon_holdover(xpon, nla_get_u32(a[XPON_ATTR_EPON_HOLDOVER_MS])) :
		      -EOPNOTSUPP;
	if (!ret && a[XPON_ATTR_EPON_KEY]) {
		if (!a[XPON_ATTR_EPON_KEY_LLID] || !a[XPON_ATTR_EPON_KEY_INDEX])
			return -EINVAL;
		ret = ops->set_epon_key ?
		      ops->set_epon_key(xpon, nla_get_u8(a[XPON_ATTR_EPON_KEY_LLID]),
					nla_get_u8(a[XPON_ATTR_EPON_KEY_INDEX]),
					nla_get_u32(a[XPON_ATTR_EPON_KEY])) :
		      -EOPNOTSUPP;
	}
	if (!ret && a[XPON_ATTR_EPON_OAM_LINK]) {
		if (a[XPON_ATTR_EPON_GENERATION])
			generation = nla_get_u32(a[XPON_ATTR_EPON_GENERATION]);
		ret = ops->set_epon_oam_link ?
		      ops->set_epon_oam_link(xpon, nla_get_u8(a[XPON_ATTR_EPON_OAM_LINK]),
					     a[XPON_ATTR_EPON_GENERATION] ? &generation : NULL) :
		      -EOPNOTSUPP;
	}
	return ret;
}

static int xpon_genl_get(struct sk_buff *skb, struct genl_info *info)
{
	struct xpon_device *xpon;
	struct sk_buff *msg;
	void *hdr;
	int ret;

	if (!info->attrs[XPON_ATTR_IFINDEX])
		return -EINVAL;

	xpon = xpon_device_find_by_ifindex(
		nla_get_u32(info->attrs[XPON_ATTR_IFINDEX]));
	if (!xpon)
		return -ENODEV;

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		ret = -ENOMEM;
		goto out;
	}

	hdr = genlmsg_put_reply(msg, info, &xpon_genl_family, 0, XPON_CMD_GET);
	if (!hdr) {
		nlmsg_free(msg);
		ret = -EMSGSIZE;
		goto out;
	}

	ret = xpon_genl_put_state(msg, xpon) ?: xpon_genl_put_module(msg, xpon);
	if (ret) {
		nlmsg_free(msg);
		goto out;
	}
	genlmsg_end(msg, hdr);
	ret = genlmsg_reply(msg, info);
out:
	xpon_device_put(xpon);
	return ret;
}

static int xpon_genl_set_mode(struct sk_buff *skb, struct genl_info *info)
{
	struct xpon_device *xpon;
	u8 mode;
	int ret;

	if (!info->attrs[XPON_ATTR_IFINDEX] || !info->attrs[XPON_ATTR_MODE])
		return -EINVAL;
	mode = nla_get_u8(info->attrs[XPON_ATTR_MODE]);

	xpon = xpon_device_find_by_ifindex(
		nla_get_u32(info->attrs[XPON_ATTR_IFINDEX]));
	if (!xpon)
		return -ENODEV;
	ret = xpon_device_set_mode(xpon, mode);
	xpon_device_put(xpon);
	return ret;
}

/* Each attribute present is applied, in this order; an absent op refuses it. */
static int xpon_genl_set(struct sk_buff *skb, struct genl_info *info)
{
	const struct xpon_device_ops *ops;
	struct nlattr **a = info->attrs;
	struct xpon_device *xpon;
	int ret = 0;

	if (!a[XPON_ATTR_IFINDEX])
		return -EINVAL;
	xpon = xpon_device_find_by_ifindex(nla_get_u32(a[XPON_ATTR_IFINDEX]));
	if (!xpon)
		return -ENODEV;
	ops = xpon->ops;
	if (!ops) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	if (a[XPON_ATTR_TX_HOLD])
		ret = ops->set_tx_hold ? ops->set_tx_hold(xpon, nla_get_u8(a[XPON_ATTR_TX_HOLD])) :
					 -EOPNOTSUPP;
	if (!ret && a[XPON_ATTR_TX_HOLD_SECONDS])
		ret = ops->set_tx_hold_seconds ?
		      ops->set_tx_hold_seconds(xpon, nla_get_u32(a[XPON_ATTR_TX_HOLD_SECONDS])) :
		      -EOPNOTSUPP;
	if (!ret && a[XPON_ATTR_IDENTITY_WAIT])
		ret = ops->release_identity ? ops->release_identity(xpon) : -EOPNOTSUPP;
	if (!ret && a[XPON_ATTR_TX_ALLOW])
		ret = ops->set_tx_allow ? ops->set_tx_allow(xpon, nla_get_u8(a[XPON_ATTR_TX_ALLOW])) :
					  -EOPNOTSUPP;
	if (!ret)
		ret = xpon_genl_set_epon(xpon, a);
	if (!ret)
		ret = xpon_genl_set_sfu(xpon, a);
out:
	xpon_device_put(xpon);
	return ret;
}

static const struct genl_ops xpon_genl_ops[] = {
	{
		.cmd = XPON_CMD_GET,
		.policy = xpon_genl_policy,
		.doit = xpon_genl_get,
	},
	{
		.cmd = XPON_CMD_GET_STATE,
		.policy = xpon_genl_policy,
		.doit = xpon_genl_get,
	},
	{
		.cmd = XPON_CMD_SET_MODE,
		.flags = GENL_ADMIN_PERM,
		.policy = xpon_genl_policy,
		.doit = xpon_genl_set_mode,
	},
	{
		.cmd = XPON_CMD_SET,
		.flags = GENL_ADMIN_PERM,
		.policy = xpon_genl_policy,
		.doit = xpon_genl_set,
	},
};

static const struct genl_multicast_group xpon_mcgrps[] = {
	{ .name = "events" },
};

static struct genl_family xpon_genl_family = {
	.name = XPON_GENL_NAME,
	.version = XPON_GENL_VERSION,
	.maxattr = XPON_ATTR_MAX,
	.module = THIS_MODULE,
	.ops = xpon_genl_ops,
	.n_ops = ARRAY_SIZE(xpon_genl_ops),
	.mcgrps = xpon_mcgrps,
	.n_mcgrps = ARRAY_SIZE(xpon_mcgrps),
};

void xpon_genl_notify(struct xpon_device *xpon, unsigned long changed)
{
	struct sk_buff *msg;
	void *hdr;

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg)
		return;
	hdr = genlmsg_put(msg, 0, 0, &xpon_genl_family, 0, XPON_CMD_EVENT);
	if (!hdr)
		goto free;
	if (xpon_genl_put_state(msg, xpon) ||
	    nla_put_u32(msg, XPON_ATTR_CHANGED, changed))
		goto free;
	genlmsg_end(msg, hdr);
	genlmsg_multicast(&xpon_genl_family, msg, 0, 0, GFP_KERNEL);
	return;
free:
	nlmsg_free(msg);
}

int xpon_genl_init(void)
{
	return genl_register_family(&xpon_genl_family);
}

void xpon_genl_exit(void)
{
	genl_unregister_family(&xpon_genl_family);
}
