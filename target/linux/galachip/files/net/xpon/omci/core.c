// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic ONU Management and Control Interface core
 *
 * The core carries OMCI between the provider and one Generic Netlink owner,
 * omcid, and validates its typed hardware requests. The protocol, MIB and
 * OLT policy live in omcid; the kernel keeps identity and configuration.
 */

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/netlink.h>
#include <linux/notifier.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <uapi/linux/omci-backend.h>
#include <net/genetlink.h>
#include <net/netlink.h>

#include "../internal.h"
#include "internal.h"

static LIST_HEAD(omci_devices);
static DEFINE_MUTEX(omci_devices_lock);
static atomic_t omci_next_id = ATOMIC_INIT(0);
static struct genl_family omci_genl_family;

static void omci_trace_tx(struct omci_device *odev, const void *data,
			  size_t len, u32 generation);

static const struct nla_policy omci_policy[OMCI_ATTR_MAX + 1] = {
	[OMCI_ATTR_BACKEND_OP] = { .type = NLA_U16 },
	[OMCI_ATTR_DIAGNOSTICS] = { .type = NLA_U8 },
	[OMCI_ATTR_BACKEND_DATA] = { .type = NLA_BINARY, .len = OMCI_BACKEND_MAX_DATA },
	[OMCI_ATTR_GENERATION] = { .type = NLA_U32 },
	[OMCI_ATTR_BIND_FLAGS] = { .type = NLA_U32 },
	[OMCI_ATTR_DEV_ID] = { .type = NLA_U32 },
	[OMCI_ATTR_PDU] = {
		.type = NLA_BINARY,
		.len = OMCI_MAX_PDU_LEN,
	},
	[OMCI_ATTR_AGENT_ENABLED] = { .type = NLA_U8 },
	[OMCI_ATTR_AGENT_PERMISSIVE] = { .type = NLA_U8 },
	[OMCI_ATTR_AGENT_FAKE_OMCI] = { .type = NLA_U8 },
	[OMCI_ATTR_ONU_TYPE] = { .type = NLA_U8 },
	[OMCI_ATTR_CLASS_ID] = { .type = NLA_U16 },
	[OMCI_ATTR_ENTITY_ID] = { .type = NLA_U16 },
	[OMCI_ATTR_ATTR_MASK] = { .type = NLA_U16 },
	[OMCI_ATTR_ATTR_DATA] = {
		.type = NLA_BINARY,
		.len = OMCI_MAX_ATTR_DATA,
	},
	[OMCI_ATTR_ORIGIN] = { .type = NLA_U8 },
	[OMCI_ATTR_INDEX] = { .type = NLA_U32 },
	[OMCI_ATTR_CONFIG_KEY] = { .type = NLA_U16 },
	[OMCI_ATTR_CONFIG_VALUE] = {
		.type = NLA_BINARY,
		.len = OMCI_MAX_CONFIG_VALUE,
	},
	[OMCI_ATTR_VLAN_TAGGING_FILTER] = { .type = NLA_NESTED },
	[OMCI_ATTR_EXTENDED_VLAN] = { .type = NLA_NESTED },
	[OMCI_ATTR_CLASS_CATEGORY] = { .type = NLA_U8 },
	[OMCI_ATTR_CLASS_SUPPORT] = { .type = NLA_U8 },
	[OMCI_ATTR_CLASS_FLAGS] = { .type = NLA_U32 },
	[OMCI_ATTR_CONFIG_SOURCE] = { .type = NLA_U8 },
	[OMCI_ATTR_OLT_PROFILE_CONFIGURED] = { .type = NLA_U8 },
	[OMCI_ATTR_OLT_PROFILE_FORCED] = { .type = NLA_U8 },
};

void omci_for_each_device(void (*fn)(struct omci_device *odev))
{
	struct omci_device *odev;

	mutex_lock(&omci_devices_lock);
	list_for_each_entry(odev, &omci_devices, list)
		fn(odev);
	mutex_unlock(&omci_devices_lock);
}

static struct omci_device *omci_find_locked(u32 id)
{
	struct omci_device *odev;

	list_for_each_entry(odev, &omci_devices, list)
		if (odev->id == id)
			return odev;

	return NULL;
}

static struct omci_device *omci_get_from_info(struct genl_info *info)
{
	u32 id = 0;

	if (info->attrs[OMCI_ATTR_DEV_ID])
		id = nla_get_u32(info->attrs[OMCI_ATTR_DEV_ID]);

	return omci_find_locked(id);
}

static int omci_put_telemetry(struct sk_buff *msg, struct omci_device *odev)
{
	struct omci_telemetry telemetry = {};

	if (odev->ops->get_telemetry)
		odev->ops->get_telemetry(odev, &telemetry);

	if (nla_put_u32(msg, OMCI_ATTR_TELEMETRY_VALID, telemetry.valid))
		return -EMSGSIZE;

	if ((telemetry.valid & OMCI_TELEMETRY_F_FEC_DOWNSTREAM) &&
	    nla_put_u8(msg, OMCI_ATTR_FEC_DOWNSTREAM,
		       telemetry.downstream_fec))
		return -EMSGSIZE;
	if ((telemetry.valid & OMCI_TELEMETRY_F_FEC_UPSTREAM) &&
	    nla_put_u8(msg, OMCI_ATTR_FEC_UPSTREAM, telemetry.upstream_fec))
		return -EMSGSIZE;
	if ((telemetry.valid & OMCI_TELEMETRY_F_BOSA_TEMPERATURE) &&
	    nla_put_s32(msg, OMCI_ATTR_BOSA_TEMPERATURE_MC,
			telemetry.bosa_temperature_mc))
		return -EMSGSIZE;
	if ((telemetry.valid & OMCI_TELEMETRY_F_BOSA_VOLTAGE) &&
	    nla_put_u32(msg, OMCI_ATTR_BOSA_VOLTAGE_UV,
			telemetry.bosa_voltage_uv))
		return -EMSGSIZE;
	if ((telemetry.valid & OMCI_TELEMETRY_F_BOSA_BIAS) &&
	    nla_put_u32(msg, OMCI_ATTR_BOSA_BIAS_UA, telemetry.bosa_bias_ua))
		return -EMSGSIZE;
	if ((telemetry.valid & OMCI_TELEMETRY_F_BOSA_TX_POWER) &&
	    nla_put_u32(msg, OMCI_ATTR_BOSA_TX_POWER_NW,
			telemetry.bosa_tx_power_nw))
		return -EMSGSIZE;
	if ((telemetry.valid & OMCI_TELEMETRY_F_BOSA_RX_POWER) &&
	    nla_put_u32(msg, OMCI_ATTR_BOSA_RX_POWER_NW,
			telemetry.bosa_rx_power_nw))
		return -EMSGSIZE;
	if ((telemetry.valid & OMCI_TELEMETRY_F_BOSA_ALARMS) &&
	    nla_put_u32(msg, OMCI_ATTR_BOSA_ALARMS, telemetry.bosa_alarms))
		return -EMSGSIZE;

	return 0;
}

static int omci_put_status(struct sk_buff *msg, struct omci_device *odev)
{
	u32 owner_portid;
	u32 owner_flags;
	u32 generation;
	u32 flags = 0;
	u16 onu_id;
	u16 gem_port_id;
	u8 state;
	u8 mode;

	mutex_lock(&odev->owner_lock);
	owner_portid = odev->owner_portid;
	owner_flags = odev->owner_flags;
	mode = !odev->userspace_mode ? OMCI_RESPONDER_NONE :
		(owner_flags & OMCI_BIND_F_AGENT) ? OMCI_RESPONDER_USERSPACE :
		OMCI_RESPONDER_ORPHANED;
	mutex_unlock(&odev->owner_lock);

	spin_lock_bh(&odev->state_lock);
	onu_id = odev->onu_id;
	gem_port_id = odev->gem_port_id;
	state = odev->state;
	generation = odev->generation;
	if (odev->channel_up)
		flags |= OMCI_F_CHANNEL_UP;
	spin_unlock_bh(&odev->state_lock);

	if (nla_put_u32(msg, OMCI_ATTR_DEV_ID, odev->id) ||
	    nla_put_u32(msg, OMCI_ATTR_IFINDEX, odev->ifindex) ||
	    nla_put_u16(msg, OMCI_ATTR_ONU_ID, onu_id) ||
	    nla_put_u16(msg, OMCI_ATTR_GEM_PORT_ID, gem_port_id) ||
	    nla_put_u8(msg, OMCI_ATTR_STATE, state) ||
	    nla_put_u32(msg, OMCI_ATTR_FLAGS, flags) ||
	    nla_put_u32(msg, OMCI_ATTR_CAPABILITIES, odev->capabilities) ||
	    nla_put_u32(msg, OMCI_ATTR_OWNER_PORTID, owner_portid) ||
	    nla_put_u32(msg, OMCI_ATTR_BIND_FLAGS, owner_flags) ||
	    nla_put_u8(msg, OMCI_ATTR_RESPONDER_MODE, mode) ||
	    nla_put_u32(msg, OMCI_ATTR_GENERATION, generation) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_TRACE_DROPPED,
			      atomic64_read(&odev->trace_dropped), OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_RX_PACKETS,
			      atomic64_read(&odev->rx_packets), OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_RX_BYTES,
			      atomic64_read(&odev->rx_bytes), OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_RX_DROPPED,
			      atomic64_read(&odev->rx_dropped), OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_TX_PACKETS,
			      atomic64_read(&odev->tx_packets), OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_TX_BYTES,
			      atomic64_read(&odev->tx_bytes), OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_TX_ERRORS,
			      atomic64_read(&odev->tx_errors), OMCI_ATTR_PAD))
		return -EMSGSIZE;

	if (omci_put_telemetry(msg, odev))
		return -EMSGSIZE;

	return omci_agent_put_status(msg, odev);
}

static int omci_cmd_get(struct sk_buff *skb, struct genl_info *info)
{
	struct omci_device *odev;
	struct sk_buff *msg;
	void *hdr;
	int ret;

	mutex_lock(&omci_devices_lock);
	odev = omci_get_from_info(info);
	if (!odev) {
		ret = -ENODEV;
		goto out_unlock;
	}

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	hdr = genlmsg_put_reply(msg, info, &omci_genl_family, 0,
				OMCI_CMD_GET);
	if (!hdr) {
		ret = -EMSGSIZE;
		goto free_msg;
	}

	ret = omci_put_status(msg, odev);
	if (ret)
		goto cancel_msg;

	genlmsg_end(msg, hdr);
	mutex_unlock(&omci_devices_lock);
	return genlmsg_reply(msg, info);

cancel_msg:
	genlmsg_cancel(msg, hdr);
free_msg:
	nlmsg_free(msg);
out_unlock:
	mutex_unlock(&omci_devices_lock);
	return ret;
}

static int omci_cmd_bind(struct sk_buff *skb, struct genl_info *info)
{
	struct net *net = genl_info_net(info);
	struct omci_device *odev;
	u32 flags = 0;
	bool provider_locked = false;
	int ret = 0;

	if (info->attrs[OMCI_ATTR_BIND_FLAGS])
		flags = nla_get_u32(info->attrs[OMCI_ATTR_BIND_FLAGS]);
	if ((flags & ~(OMCI_BIND_F_READ_ONLY | OMCI_BIND_F_AGENT)) ||
	    flags == (OMCI_BIND_F_READ_ONLY | OMCI_BIND_F_AGENT))
		return -EINVAL;

	mutex_lock(&omci_devices_lock);
	odev = omci_get_from_info(info);
	if (!odev) {
		ret = -ENODEV;
		goto out_devices;
	}
	if (odev->userspace_mode && !flags) {
		ret = -EPERM;
		goto out_devices;
	}
	mutex_lock(&odev->owner_lock);
	if (odev->owner_portid &&
	    (odev->owner_portid != info->snd_portid ||
	     odev->owner_net != net || odev->owner_flags != flags)) {
		ret = -EBUSY;
		goto out_owner;
	}
	mutex_unlock(&odev->owner_lock);
	if (flags & OMCI_BIND_F_AGENT) {
		if (!odev->ops->backend_begin || !odev->ops->backend_end) {
			ret = -EOPNOTSUPP;
			goto out_devices;
		}
		if (!info->attrs[OMCI_ATTR_GENERATION]) {
			ret = -EINVAL;
			goto out_devices;
		}
		ret = odev->ops->backend_begin(odev);
		if (ret)
			goto out_devices;
		provider_locked = true;
		spin_lock_bh(&odev->state_lock);
		if (nla_get_u32(info->attrs[OMCI_ATTR_GENERATION]) != odev->generation)
			ret = -ESTALE;
		spin_unlock_bh(&odev->state_lock);
		if (ret)
			goto out_devices;
		if (!odev->userspace_mode) {
			WRITE_ONCE(odev->userspace_mode, true);
			skb_queue_purge(&odev->rx_queue);
		}
	}
	mutex_lock(&odev->owner_lock);
	if (!odev->owner_portid) {
		odev->owner_net = get_net(net);
		odev->owner_portid = info->snd_portid;
		odev->owner_flags = flags;
		schedule_work(&odev->rx_work);
	}
out_owner:
	mutex_unlock(&odev->owner_lock);
out_devices:
	if (provider_locked)
		odev->ops->backend_end(odev);
	mutex_unlock(&omci_devices_lock);
	return ret;
}

static int omci_cmd_unbind(struct sk_buff *skb, struct genl_info *info)
{
	struct omci_device *odev;
	struct net *owner_net = NULL;
	int ret = 0;

	mutex_lock(&omci_devices_lock);
	odev = omci_get_from_info(info);
	if (!odev) {
		ret = -ENODEV;
		goto out_unlock_devices;
	}

	mutex_lock(&odev->owner_lock);
	if (odev->owner_portid != info->snd_portid ||
	    odev->owner_net != genl_info_net(info)) {
		ret = -EPERM;
		goto out_unlock_owner;
	}

	owner_net = odev->owner_net;
	odev->owner_net = NULL;
	odev->owner_portid = 0;
	odev->owner_flags = 0;

out_unlock_owner:
	mutex_unlock(&odev->owner_lock);
out_unlock_devices:
	mutex_unlock(&omci_devices_lock);
	if (owner_net)
		put_net(owner_net);
	return ret;
}

int omci_validate_tx(struct omci_device *odev, struct sk_buff *skb)
{
	const u8 *data = skb->data;
	u16 content_len;
	u16 pdu_len;

	if (skb->len < 4)
		return -EMSGSIZE;

	switch (data[3]) {
	case OMCI_BASELINE_DEV_ID:
		if (skb->len != OMCI_BASELINE_LEN_NO_MIC &&
		    skb->len != OMCI_BASELINE_LEN)
			return -EMSGSIZE;
		if ((odev->capabilities & OMCI_CAP_HW_MIC) &&
		    skb->len == OMCI_BASELINE_LEN)
			skb_trim(skb, OMCI_BASELINE_LEN_NO_MIC);
		break;
	case OMCI_EXTENDED_DEV_ID:
		if (skb->len < OMCI_EXTENDED_HEADER_LEN)
			return -EMSGSIZE;
		content_len = get_unaligned_be16(data + 8);
		pdu_len = OMCI_EXTENDED_HEADER_LEN + content_len;
		if (pdu_len > OMCI_MAX_PDU_LEN || skb->len < pdu_len)
			return -EMSGSIZE;
		if (skb->len > pdu_len) {
			if (skb->len - pdu_len != OMCI_MIC_LEN)
				return -EMSGSIZE;
			if (odev->capabilities & OMCI_CAP_HW_MIC)
				skb_trim(skb, pdu_len);
		}
		break;
	default:
		return -EPROTO;
	}

	return 0;
}

int omci_device_xmit(struct omci_device *odev, const void *data, size_t len)
{
	struct sk_buff *tx_skb;
	u16 gem_port_id;
	bool channel_up;
	u32 tx_len;
	u32 generation;
	int ret;

	if (!READ_ONCE(odev->started))
		return -ENETDOWN;

	spin_lock_bh(&odev->state_lock);
	gem_port_id = odev->gem_port_id;
	channel_up = odev->channel_up;
	generation = odev->generation;
	spin_unlock_bh(&odev->state_lock);
	if (!channel_up)
		return -ENOLINK;

	tx_skb = alloc_skb(len, GFP_KERNEL);
	if (!tx_skb)
		return -ENOMEM;
	skb_put_data(tx_skb, data, len);
	ret = omci_validate_tx(odev, tx_skb);
	if (ret)
		goto free_skb;

	tx_len = tx_skb->len;
	ret = odev->ops->xmit(odev, tx_skb, gem_port_id);
	if (ret)
		goto free_skb;

	atomic64_inc(&odev->tx_packets);
	atomic64_add(tx_len, &odev->tx_bytes);
	omci_trace_tx(odev, data, tx_len, generation);
	return 0;

free_skb:
	atomic64_inc(&odev->tx_errors);
	dev_kfree_skb_any(tx_skb);
	return ret;
}

static int omci_cmd_tx(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *pdu_attr = info->attrs[OMCI_ATTR_PDU];
	struct omci_device *odev;
	struct sk_buff *tx_skb;
	u16 gem_port_id;
	bool channel_up;
	u32 tx_len;
	bool provider_locked = false;
	bool agent_owner;
	int ret;

	if (!pdu_attr)
		return -EINVAL;

	mutex_lock(&omci_devices_lock);
	odev = omci_get_from_info(info);
	if (!odev) {
		ret = -ENODEV;
		goto out_unlock_devices;
	}

	mutex_lock(&odev->owner_lock);
	if (odev->owner_portid != info->snd_portid ||
	    odev->owner_net != genl_info_net(info) ||
	    (odev->owner_flags & OMCI_BIND_F_READ_ONLY)) {
		ret = -EPERM;
		goto out_unlock_owner;
	}
	agent_owner = odev->owner_flags & OMCI_BIND_F_AGENT;
	mutex_unlock(&odev->owner_lock);
	if (agent_owner) {
		if (!info->attrs[OMCI_ATTR_GENERATION]) {
			ret = -EINVAL;
			goto out_unlock_devices;
		}
		ret = odev->ops->backend_begin(odev);
		if (ret)
			goto out_unlock_devices;
		provider_locked = true;
	}

	if (!READ_ONCE(odev->started)) {
		ret = -ENETDOWN;
		goto out_unlock_devices;
	}

	spin_lock_bh(&odev->state_lock);
	gem_port_id = odev->gem_port_id;
	channel_up = odev->channel_up;
	if (info->attrs[OMCI_ATTR_GENERATION] &&
	    nla_get_u32(info->attrs[OMCI_ATTR_GENERATION]) != odev->generation) {
		spin_unlock_bh(&odev->state_lock);
		ret = -ESTALE;
		goto out_unlock_devices;
	}
	spin_unlock_bh(&odev->state_lock);
	if (!channel_up) {
		ret = -ENOLINK;
		goto out_unlock_devices;
	}

	tx_skb = alloc_skb(nla_len(pdu_attr), GFP_KERNEL);
	if (!tx_skb) {
		ret = -ENOMEM;
		goto out_unlock_devices;
	}

	skb_put_data(tx_skb, nla_data(pdu_attr), nla_len(pdu_attr));
	ret = omci_validate_tx(odev, tx_skb);
	if (ret)
		goto free_tx_skb;

	tx_len = tx_skb->len;
	ret = odev->ops->xmit(odev, tx_skb, gem_port_id);
	if (ret)
		goto free_tx_skb;

	atomic64_inc(&odev->tx_packets);
	atomic64_add(tx_len, &odev->tx_bytes);
	if (provider_locked)
		odev->ops->backend_end(odev);
	mutex_unlock(&omci_devices_lock);
	return 0;

free_tx_skb:
	atomic64_inc(&odev->tx_errors);
	dev_kfree_skb_any(tx_skb);
	goto out_unlock_devices;

out_unlock_owner:
	mutex_unlock(&odev->owner_lock);
out_unlock_devices:
	if (provider_locked)
		odev->ops->backend_end(odev);
	mutex_unlock(&omci_devices_lock);
	return ret;
}

static int omci_cmd_agent_set(struct sk_buff *skb, struct genl_info *info)
{
	struct omci_device *odev;
	bool profile_changed = false;
	u8 value;
	int ret = 0;

	mutex_lock(&omci_devices_lock);
	odev = omci_get_from_info(info);
	if (!odev) {
		ret = -ENODEV;
		goto out;
	}
	if (info->attrs[OMCI_ATTR_AGENT_ENABLED]) {
		value = nla_get_u8(info->attrs[OMCI_ATTR_AGENT_ENABLED]);
		ret = omci_agent_config_set(odev, OMCI_CONFIG_AGENT_ENABLED,
					    &value, sizeof(value));
		if (ret)
			goto out;
	}
	if (info->attrs[OMCI_ATTR_AGENT_PERMISSIVE]) {
		value = nla_get_u8(info->attrs[OMCI_ATTR_AGENT_PERMISSIVE]);
		ret = omci_agent_config_set(odev, OMCI_CONFIG_AGENT_PERMISSIVE,
					    &value, sizeof(value));
		if (ret)
			goto out;
	}
	if (info->attrs[OMCI_ATTR_AGENT_FAKE_OMCI]) {
		value = nla_get_u8(info->attrs[OMCI_ATTR_AGENT_FAKE_OMCI]);
		ret = omci_agent_config_set(odev, OMCI_CONFIG_AGENT_FAKE_OMCI,
					    &value, sizeof(value));
		if (ret)
			goto out;
	}
	if (info->attrs[OMCI_ATTR_OLT_PROFILE_CONFIGURED]) {
		value = nla_get_u8(info->attrs[OMCI_ATTR_OLT_PROFILE_CONFIGURED]);
		ret = omci_agent_config_set(odev, OMCI_CONFIG_OLT_PROFILE,
					    &value, sizeof(value));
		if (ret)
			goto out;
		profile_changed = true;
	}
	if (info->attrs[OMCI_ATTR_OLT_PROFILE_FORCED]) {
		value = nla_get_u8(info->attrs[OMCI_ATTR_OLT_PROFILE_FORCED]);
		ret = omci_agent_config_set(odev, OMCI_CONFIG_OLT_PROFILE_FORCE,
					    &value, sizeof(value));
		if (ret)
			goto out;
		profile_changed = true;
	}
	if (!ret)
		omci_device_notify(odev, profile_changed ?
				   OMCI_EVENT_PROFILE_CHANGE :
				   OMCI_EVENT_CONFIG_CHANGE);
out:
	mutex_unlock(&omci_devices_lock);
	return ret;
}

static int omci_cmd_config_get(struct sk_buff *skb, struct genl_info *info)
{
	u8 value[OMCI_MAX_CONFIG_VALUE];
	struct omci_device *odev;
	struct sk_buff *msg;
	size_t len = sizeof(value);
	u16 key;
	u8 source = OMCI_CONFIG_SOURCE_UNSPEC;
	void *hdr;
	int ret;

	if (!info->attrs[OMCI_ATTR_CONFIG_KEY])
		return -EINVAL;
	key = nla_get_u16(info->attrs[OMCI_ATTR_CONFIG_KEY]);

	mutex_lock(&omci_devices_lock);
	odev = omci_get_from_info(info);
	if (!odev) {
		ret = -ENODEV;
		goto out_unlock;
	}
	ret = omci_agent_config_get(odev, key, value, &len);
	if (ret)
		goto out_unlock;
	omci_agent_config_source_get(odev, key, &source);

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE + len, GFP_KERNEL);
	if (!msg) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	hdr = genlmsg_put_reply(msg, info, &omci_genl_family, 0,
				OMCI_CMD_CONFIG_GET);
	if (!hdr || nla_put_u32(msg, OMCI_ATTR_DEV_ID, odev->id) ||
	    nla_put_u16(msg, OMCI_ATTR_CONFIG_KEY, key) ||
	    nla_put(msg, OMCI_ATTR_CONFIG_VALUE, len, value) ||
	    nla_put_u8(msg, OMCI_ATTR_CONFIG_SOURCE, source)) {
		nlmsg_free(msg);
		ret = -EMSGSIZE;
		goto out_unlock;
	}
	genlmsg_end(msg, hdr);
	mutex_unlock(&omci_devices_lock);
	return genlmsg_reply(msg, info);

out_unlock:
	mutex_unlock(&omci_devices_lock);
	return ret;
}

static int omci_cmd_config_set(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *value = info->attrs[OMCI_ATTR_CONFIG_VALUE];
	struct omci_device *odev;
	u16 key;
	int ret;

	if (!info->attrs[OMCI_ATTR_CONFIG_KEY] || !value)
		return -EINVAL;
	key = nla_get_u16(info->attrs[OMCI_ATTR_CONFIG_KEY]);

	mutex_lock(&omci_devices_lock);
	odev = omci_get_from_info(info);
	if (!odev) {
		ret = -ENODEV;
		goto out;
	}
	if (odev->userspace_mode) {
		ret = -EBUSY;
		goto out;
	}
	ret = omci_agent_config_set(odev, key, nla_data(value),
				    nla_len(value));
	if (!ret) {
		u8 event = OMCI_EVENT_CONFIG_CHANGE;

		if (key == OMCI_CONFIG_OLT_PROFILE ||
		    key == OMCI_CONFIG_OLT_PROFILE_FORCE)
			event = OMCI_EVENT_PROFILE_CHANGE;
		omci_device_notify(odev, event);
	}
out:
	mutex_unlock(&omci_devices_lock);
	return ret;
}

static int omci_cmd_backend(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *payload = info->attrs[OMCI_ATTR_BACKEND_DATA];
	struct omci_device *odev;
	struct sk_buff *msg;
	u8 output[OMCI_BACKEND_MAX_DATA] __aligned(8);
	size_t output_len;
	void *hdr;
	u16 op;
	bool read;
	int ret;

	if (!info->attrs[OMCI_ATTR_BACKEND_OP] || !info->attrs[OMCI_ATTR_GENERATION])
		return -EINVAL;
	op = nla_get_u16(info->attrs[OMCI_ATTR_BACKEND_OP]);
	read = op == OMCI_BACKEND_TOPOLOGY || op == OMCI_BACKEND_TELEMETRY ||
	       op == OMCI_BACKEND_UNI_LINK;
	mutex_lock(&omci_devices_lock);
	odev = omci_get_from_info(info);
	if (!odev) {
		ret = -ENODEV;
		goto out;
	}
	if (!odev->ops->backend_begin || !odev->ops->backend_end) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	if (!read) {
		mutex_lock(&odev->owner_lock);
		ret = odev->userspace_mode && (odev->owner_flags & OMCI_BIND_F_AGENT) &&
		      odev->owner_portid == info->snd_portid &&
		      odev->owner_net == genl_info_net(info) ? 0 : -EPERM;
		mutex_unlock(&odev->owner_lock);
		if (ret)
			goto out;
	}
	ret = odev->ops->backend_begin(odev);
	if (ret)
		goto out;
	spin_lock_bh(&odev->state_lock);
	ret = nla_get_u32(info->attrs[OMCI_ATTR_GENERATION]) == odev->generation ? 0 : -ESTALE;
	spin_unlock_bh(&odev->state_lock);
	if (!ret)
		ret = omci_backend_execute(odev, op, payload ? nla_data(payload) : NULL,
					   payload ? nla_len(payload) : 0, output, &output_len);
	odev->ops->backend_end(odev);
	if (ret)
		goto out;
	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		ret = -ENOMEM;
		goto out;
	}
	hdr = genlmsg_put_reply(msg, info, &omci_genl_family, 0, OMCI_CMD_BACKEND);
	if (!hdr || nla_put_u16(msg, OMCI_ATTR_BACKEND_OP, op) ||
	    nla_put_u32(msg, OMCI_ATTR_GENERATION,
			nla_get_u32(info->attrs[OMCI_ATTR_GENERATION])) ||
	    (output_len && nla_put(msg, OMCI_ATTR_BACKEND_DATA, output_len, output))) {
		nlmsg_free(msg);
		ret = -EMSGSIZE;
		goto out;
	}
	genlmsg_end(msg, hdr);
	mutex_unlock(&omci_devices_lock);
	return genlmsg_reply(msg, info);
out:
	mutex_unlock(&omci_devices_lock);
	return ret;
}

static const struct genl_ops omci_genl_ops[] = {
	{
		.cmd = OMCI_CMD_BACKEND,
		.flags = GENL_ADMIN_PERM,
		.policy = omci_policy,
		.doit = omci_cmd_backend,
	},
	{
		.cmd = OMCI_CMD_GET,
		.policy = omci_policy,
		.doit = omci_cmd_get,
	},
	{
		.cmd = OMCI_CMD_BIND,
		.flags = GENL_ADMIN_PERM,
		.policy = omci_policy,
		.doit = omci_cmd_bind,
	},
	{
		.cmd = OMCI_CMD_UNBIND,
		.flags = GENL_ADMIN_PERM,
		.policy = omci_policy,
		.doit = omci_cmd_unbind,
	},
	{
		.cmd = OMCI_CMD_TX,
		.flags = GENL_ADMIN_PERM,
		.policy = omci_policy,
		.doit = omci_cmd_tx,
	},
	{
		.cmd = OMCI_CMD_AGENT_SET,
		.flags = GENL_ADMIN_PERM,
		.policy = omci_policy,
		.doit = omci_cmd_agent_set,
	},
	{
		.cmd = OMCI_CMD_CONFIG_GET,
		.flags = GENL_ADMIN_PERM,
		.policy = omci_policy,
		.doit = omci_cmd_config_get,
	},
	{
		.cmd = OMCI_CMD_CONFIG_SET,
		.flags = GENL_ADMIN_PERM,
		.policy = omci_policy,
		.doit = omci_cmd_config_set,
	},
};

static struct genl_family omci_genl_family __ro_after_init = {
	.name = OMCI_GENL_NAME,
	.version = OMCI_GENL_VERSION,
	.maxattr = OMCI_ATTR_MAX,
	.module = THIS_MODULE,
	.ops = omci_genl_ops,
	.n_ops = ARRAY_SIZE(omci_genl_ops),
};

static int omci_send(struct omci_device *odev, struct sk_buff *msg,
		     u32 portid, struct net *net)
{
	int ret;

	ret = genlmsg_unicast(net, msg, portid);
	if (ret == -ESRCH || ret == -ECONNREFUSED) {
		struct net *owner_net = NULL;

		mutex_lock(&odev->owner_lock);
		if (odev->owner_portid == portid && odev->owner_net == net) {
			owner_net = odev->owner_net;
			odev->owner_net = NULL;
			odev->owner_portid = 0;
			odev->owner_flags = 0;
		}
		mutex_unlock(&odev->owner_lock);
		if (owner_net)
			put_net(owner_net);
	}

	return ret;
}

static void omci_trace_tx(struct omci_device *odev, const void *data,
			  size_t len, u32 generation)
{
	struct sk_buff *msg;
	void *hdr;

	/* Best effort diagnostics must not change the result of an OMCI TX. */
	mutex_lock(&odev->owner_lock);
	if (!odev->owner_portid ||
	    !(odev->owner_flags & OMCI_BIND_F_READ_ONLY))
		goto out;
	msg = genlmsg_new(NLMSG_DEFAULT_SIZE + len, GFP_KERNEL);
	if (!msg) {
		atomic64_inc(&odev->trace_dropped);
		goto out;
	}
	hdr = genlmsg_put(msg, 0, 0, &omci_genl_family, 0, OMCI_CMD_TRACE_TX);
	if (!hdr || nla_put_u32(msg, OMCI_ATTR_DEV_ID, odev->id) ||
	    nla_put_u32(msg, OMCI_ATTR_GENERATION, generation) ||
	    nla_put(msg, OMCI_ATTR_PDU, len, data)) {
		nlmsg_free(msg);
		atomic64_inc(&odev->trace_dropped);
		goto out;
	}
	genlmsg_end(msg, hdr);
	/* Do not call omci_send(): its dead-owner cleanup takes owner_lock. */
	if (genlmsg_unicast(odev->owner_net, msg, odev->owner_portid))
		atomic64_inc(&odev->trace_dropped);
out:
	mutex_unlock(&odev->owner_lock);
}

static void omci_rx_work(struct work_struct *work)
{
	struct omci_device *odev;
	struct sk_buff *skb;

	odev = container_of(work, struct omci_device, rx_work);
	while ((skb = skb_dequeue(&odev->rx_queue))) {
		struct omci_skb_cb *cb = OMCI_SKB_CB(skb);
		struct net *net = NULL;
		struct sk_buff *msg;
		u32 portid = 0;
		u16 onu_id;
		void *hdr;
		int ret;

		spin_lock_bh(&odev->state_lock);
		onu_id = odev->onu_id;
		if (cb->generation != odev->generation) {
			spin_unlock_bh(&odev->state_lock);
			atomic64_inc(&odev->rx_dropped);
			dev_kfree_skb_any(skb);
			continue;
		}
		spin_unlock_bh(&odev->state_lock);

		mutex_lock(&odev->owner_lock);
		if (odev->owner_portid && odev->owner_net) {
			portid = odev->owner_portid;
			net = get_net(odev->owner_net);
		}
		mutex_unlock(&odev->owner_lock);

		if (!net) {
			dev_kfree_skb_any(skb);
			continue;
		}

		msg = genlmsg_new(NLMSG_DEFAULT_SIZE + skb->len, GFP_KERNEL);
		if (!msg) {
			atomic64_inc(&odev->rx_dropped);
			atomic64_inc(&odev->trace_dropped);
			put_net(net);
			dev_kfree_skb_any(skb);
			continue;
		}

		hdr = genlmsg_put(msg, 0, 0, &omci_genl_family, 0,
				  OMCI_CMD_RX);
		if (!hdr ||
		    nla_put_u32(msg, OMCI_ATTR_DEV_ID, odev->id) ||
		    nla_put_u16(msg, OMCI_ATTR_ONU_ID, onu_id) ||
		    nla_put_u16(msg, OMCI_ATTR_GEM_PORT_ID, cb->gem_port_id) ||
		    nla_put_u32(msg, OMCI_ATTR_FLAGS, cb->flags) ||
		    nla_put_u32(msg, OMCI_ATTR_GENERATION, cb->generation) ||
		    nla_put_u64_64bit(msg, OMCI_ATTR_SEQUENCE, cb->sequence,
				      OMCI_ATTR_PAD) ||
		    nla_put(msg, OMCI_ATTR_PDU, skb->len, skb->data)) {
			nlmsg_free(msg);
			atomic64_inc(&odev->rx_dropped);
			atomic64_inc(&odev->trace_dropped);
			put_net(net);
			dev_kfree_skb_any(skb);
			continue;
		}

		genlmsg_end(msg, hdr);
		ret = omci_send(odev, msg, portid, net);
		if (ret) {
			atomic64_inc(&odev->rx_dropped);
			atomic64_inc(&odev->trace_dropped);
		}
		put_net(net);
		dev_kfree_skb_any(skb);
	}
}

void omci_device_notify(struct omci_device *odev, u8 event)
{
	struct net *net = NULL;
	struct sk_buff *msg;
	u32 portid = 0;
	void *hdr;

	mutex_lock(&odev->owner_lock);
	if (odev->owner_portid && odev->owner_net) {
		portid = odev->owner_portid;
		net = get_net(odev->owner_net);
	}
	mutex_unlock(&odev->owner_lock);
	if (!net)
		return;

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		atomic64_inc(&odev->trace_dropped);
		goto out_put_net;
	}

	hdr = genlmsg_put(msg, 0, 0, &omci_genl_family, 0,
			  OMCI_CMD_EVENT);
	if (!hdr || nla_put_u8(msg, OMCI_ATTR_EVENT, event) ||
	    omci_put_status(msg, odev)) {
		nlmsg_free(msg);
		atomic64_inc(&odev->trace_dropped);
		goto out_put_net;
	}

	genlmsg_end(msg, hdr);
	if (omci_send(odev, msg, portid, net))
		atomic64_inc(&odev->trace_dropped);

out_put_net:
	put_net(net);
}

static int omci_netlink_notify(struct notifier_block *nb, unsigned long state,
			       void *data)
{
	struct netlink_notify *notify = data;
	struct omci_device *odev;

	if (state != NETLINK_URELEASE || notify->protocol != NETLINK_GENERIC)
		return NOTIFY_DONE;

	mutex_lock(&omci_devices_lock);
	list_for_each_entry(odev, &omci_devices, list) {
		struct net *owner_net = NULL;

		mutex_lock(&odev->owner_lock);
		if (odev->owner_portid == notify->portid &&
		    odev->owner_net == notify->net) {
			owner_net = odev->owner_net;
			odev->owner_net = NULL;
			odev->owner_portid = 0;
			odev->owner_flags = 0;
		}
		mutex_unlock(&odev->owner_lock);
		if (owner_net)
			put_net(owner_net);
	}
	mutex_unlock(&omci_devices_lock);

	return NOTIFY_DONE;
}

static struct notifier_block omci_netlink_nb = {
	.notifier_call = omci_netlink_notify,
};

struct omci_device *
omci_device_register(struct xpon_device *xpon, u32 capabilities,
		     const struct omci_device_ops *ops, void *priv)
{
	struct omci_device *odev;

	if (!xpon || !ops || !ops->xmit)
		return ERR_PTR(-EINVAL);
	if (xpon->omci)
		return ERR_PTR(-EBUSY);

	odev = kzalloc(sizeof(*odev), GFP_KERNEL);
	if (!odev)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&odev->list);
	mutex_init(&odev->lifecycle_lock);
	mutex_init(&odev->owner_lock);
	spin_lock_init(&odev->state_lock);
	skb_queue_head_init(&odev->rx_queue);
	INIT_WORK(&odev->rx_work, omci_rx_work);
	odev->xpon = xpon;
	odev->parent = xpon->class_dev;
	odev->ifindex = xpon->netdev->ifindex;
	odev->capabilities = capabilities | OMCI_CAP_TRACE;
	if (ops->backend_begin && ops->backend_end)
		odev->capabilities |= OMCI_CAP_BACKEND;
	odev->ops = ops;
	odev->priv = priv;
	odev->onu_id = 0xffff;
	odev->gem_port_id = 0xffff;
	odev->id = atomic_inc_return(&omci_next_id) - 1;

	{
		int ret = omci_agent_init(odev);

		if (ret) {
			kfree(odev);
			return ERR_PTR(ret);
		}

		xpon->omci = odev;
		ret = omci_sysfs_register(odev);
		if (ret) {
			xpon->omci = NULL;
			omci_agent_cleanup(odev);
			kfree(odev);
			return ERR_PTR(ret);
		}
	}

	mutex_lock(&omci_devices_lock);
	list_add_tail(&odev->list, &omci_devices);
	mutex_unlock(&omci_devices_lock);

	dev_dbg(odev->parent, "registered OMCI agent device %u\n", odev->id);
	return odev;
}
EXPORT_SYMBOL_GPL(omci_device_register);

int omci_device_start(struct omci_device *odev)
{
	int ret = 0;

	if (!odev)
		return -EINVAL;

	mutex_lock(&odev->lifecycle_lock);
	if (odev->started)
		goto out;

	/*
	 * Publish the lifecycle state before entering the provider so state and
	 * receive callbacks generated synchronously by start() are accepted.
	 */
	WRITE_ONCE(odev->started, true);
	if (odev->ops->start) {
		ret = odev->ops->start(odev);
		if (ret)
			WRITE_ONCE(odev->started, false);
	}
out:
	mutex_unlock(&odev->lifecycle_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(omci_device_start);

void omci_device_stop(struct omci_device *odev)
{
	if (!odev)
		return;

	mutex_lock(&odev->lifecycle_lock);
	if (!odev->started)
		goto out;

	/* Let the provider drain the active OMCC before rejecting new traffic. */
	if (odev->ops->stop)
		odev->ops->stop(odev);
	WRITE_ONCE(odev->started, false);
out:
	mutex_unlock(&odev->lifecycle_lock);
}
EXPORT_SYMBOL_GPL(omci_device_stop);

void omci_device_unregister(struct omci_device *odev)
{
	struct net *owner_net;

	if (!odev)
		return;

	mutex_lock(&omci_devices_lock);
	list_del_init(&odev->list);
	mutex_unlock(&omci_devices_lock);
	omci_device_stop(odev);

	cancel_work_sync(&odev->rx_work);
	skb_queue_purge(&odev->rx_queue);

	mutex_lock(&odev->owner_lock);
	owner_net = odev->owner_net;
	odev->owner_net = NULL;
	odev->owner_portid = 0;
	odev->owner_flags = 0;
	mutex_unlock(&odev->owner_lock);
	if (owner_net)
		put_net(owner_net);
	omci_sysfs_unregister(odev);
	if (odev->xpon && odev->xpon->omci == odev)
		odev->xpon->omci = NULL;
	omci_agent_cleanup(odev);
	kfree(odev);
}
EXPORT_SYMBOL_GPL(omci_device_unregister);

void *omci_device_priv(const struct omci_device *odev)
{
	return odev->priv;
}
EXPORT_SYMBOL_GPL(omci_device_priv);

bool omci_device_userspace_agent(const struct omci_device *odev)
{
	return READ_ONCE(odev->userspace_mode);
}
EXPORT_SYMBOL_GPL(omci_device_userspace_agent);

u32 omci_device_id(const struct omci_device *odev)
{
	return odev->id;
}
EXPORT_SYMBOL_GPL(omci_device_id);

void omci_device_set_identity_info(struct omci_device *odev,
				   const struct omci_identity *identity)
{
	if (!odev || !identity)
		return;
	if (identity->valid & OMCI_IDENTITY_F_SERIAL_NUMBER)
		omci_agent_config_set_source(odev, OMCI_CONFIG_SERIAL_NUMBER,
					     identity->serial_number,
					     sizeof(identity->serial_number),
					     identity->serial_source);
	if (identity->valid & OMCI_IDENTITY_F_VENDOR_ID)
		omci_agent_config_set_source(odev, OMCI_CONFIG_VENDOR_ID,
					     identity->vendor_id,
					     sizeof(identity->vendor_id),
					     identity->vendor_source);
	if (identity->valid & OMCI_IDENTITY_F_PASSWORD)
		omci_agent_config_set_source(odev, OMCI_CONFIG_PASSWORD,
					     identity->password,
					     sizeof(identity->password),
					     identity->password_source);
	if (identity->valid & OMCI_IDENTITY_F_VERSION)
		omci_agent_config_set_source(odev, OMCI_CONFIG_VERSION,
					     identity->version,
					     sizeof(identity->version),
					     identity->version_source);
	if (identity->valid & OMCI_IDENTITY_F_EQUIPMENT_ID)
		omci_agent_config_set_source(odev, OMCI_CONFIG_EQUIPMENT_ID,
					     identity->equipment_id,
					     sizeof(identity->equipment_id),
					     identity->equipment_source);
	if (identity->valid & OMCI_IDENTITY_F_LOID)
		omci_agent_config_set_source(odev, OMCI_CONFIG_LOID,
					     identity->loid,
					     sizeof(identity->loid),
					     identity->loid_source);
	if (identity->valid & OMCI_IDENTITY_F_LOID_PASSWORD)
		omci_agent_config_set_source(odev, OMCI_CONFIG_LOID_PASSWORD,
					     identity->loid_password,
					     sizeof(identity->loid_password),
					     identity->loid_password_source);
}
EXPORT_SYMBOL_GPL(omci_device_set_identity_info);

void omci_device_set_identity(struct omci_device *odev,
			      const u8 serial_number[8],
			      const u8 password[10])
{
	struct omci_identity identity = {};

	if (serial_number) {
		memcpy(identity.serial_number, serial_number,
		       sizeof(identity.serial_number));
		memcpy(identity.vendor_id, serial_number,
		       sizeof(identity.vendor_id));
		identity.valid |= OMCI_IDENTITY_F_SERIAL_NUMBER |
				  OMCI_IDENTITY_F_VENDOR_ID;
		identity.serial_source = OMCI_CONFIG_SOURCE_DRIVER;
		identity.vendor_source = OMCI_CONFIG_SOURCE_DRIVER;
	}
	if (password) {
		memcpy(identity.password, password, sizeof(identity.password));
		identity.valid |= OMCI_IDENTITY_F_PASSWORD;
		identity.password_source = OMCI_CONFIG_SOURCE_DRIVER;
	}
	omci_device_set_identity_info(odev, &identity);
}
EXPORT_SYMBOL_GPL(omci_device_set_identity);

void omci_device_set_onu_id(struct omci_device *odev, u16 onu_id)
{
	spin_lock_bh(&odev->state_lock);
	odev->onu_id = onu_id;
	spin_unlock_bh(&odev->state_lock);
}
EXPORT_SYMBOL_GPL(omci_device_set_onu_id);

void omci_device_set_channel(struct omci_device *odev, u16 gem_port_id,
			     bool valid)
{
	u8 event = valid ? OMCI_EVENT_CHANNEL_UP : OMCI_EVENT_CHANNEL_DOWN;
	bool changed;

	spin_lock_bh(&odev->state_lock);
	changed = odev->channel_up != valid ||
		  (valid && odev->gem_port_id != gem_port_id);
	if (changed)
		odev->generation++;
	odev->channel_up = valid;
	odev->gem_port_id = valid ? gem_port_id : 0xffff;
	spin_unlock_bh(&odev->state_lock);

	if (!valid)
		skb_queue_purge(&odev->rx_queue);
	if (changed)
		omci_device_notify(odev, event);
}
EXPORT_SYMBOL_GPL(omci_device_set_channel);

void omci_device_set_state(struct omci_device *odev, u8 state)
{
	bool changed;

	spin_lock_bh(&odev->state_lock);
	changed = odev->state != state;
	odev->state = state;
	spin_unlock_bh(&odev->state_lock);

	if (changed)
		omci_device_notify(odev, OMCI_EVENT_STATE_CHANGE);
}
EXPORT_SYMBOL_GPL(omci_device_set_state);

void omci_device_reset_session(struct omci_device *odev)
{
	if (!odev)
		return;

	/*
	 * Finish requests already accepted on the old OMCC before changing
	 * the generation. A Deactivate_ONU-ID PLOAM can race with an OMCI
	 * request carried by the same downstream frame.
	 */
	if (current_work() != &odev->rx_work)
		flush_work(&odev->rx_work);

	spin_lock_bh(&odev->state_lock);
	odev->generation++;
	odev->onu_id = 0xffff;
	odev->gem_port_id = 0xffff;
	odev->channel_up = false;
	spin_unlock_bh(&odev->state_lock);

	if (current_work() != &odev->rx_work)
		cancel_work_sync(&odev->rx_work);
	skb_queue_purge(&odev->rx_queue);
	omci_device_notify(odev, OMCI_EVENT_CHANNEL_DOWN);
}
EXPORT_SYMBOL_GPL(omci_device_reset_session);

void omci_device_receive(struct omci_device *odev, struct sk_buff *skb,
			 u16 gem_port_id, u32 flags)
{
	struct omci_skb_cb *cb;
	bool channel_up;
	u16 configured_gem;
	u32 generation;

	if (!skb)
		return;
	if (!odev) {
		dev_kfree_skb_any(skb);
		return;
	}
	if (!READ_ONCE(odev->started)) {
		atomic64_inc(&odev->rx_dropped);
		dev_kfree_skb_any(skb);
		return;
	}

	spin_lock_bh(&odev->state_lock);
	channel_up = odev->channel_up;
	configured_gem = odev->gem_port_id;
	generation = odev->generation;
	spin_unlock_bh(&odev->state_lock);

	if (!channel_up || gem_port_id != configured_gem || !skb->len ||
	    skb->len > OMCI_MAX_PDU_LEN) {
		atomic64_inc(&odev->rx_dropped);
		dev_kfree_skb_any(skb);
		return;
	}

	if (skb_queue_len(&odev->rx_queue) >= OMCI_RX_QUEUE_LEN) {
		atomic64_inc(&odev->rx_dropped);
		dev_kfree_skb_any(skb);
		return;
	}

	cb = OMCI_SKB_CB(skb);
	memset(cb, 0, sizeof(*cb));
	cb->sequence = atomic64_inc_return(&odev->sequence);
	cb->flags = flags;
	cb->generation = generation;
	cb->gem_port_id = gem_port_id;
	atomic64_inc(&odev->rx_packets);
	atomic64_add(skb->len, &odev->rx_bytes);
	skb_queue_tail(&odev->rx_queue, skb);
	schedule_work(&odev->rx_work);
}
EXPORT_SYMBOL_GPL(omci_device_receive);

static int __init omci_init(void)
{
	int ret;

	ret = omci_sysfs_init();
	if (ret)
		return ret;

	ret = genl_register_family(&omci_genl_family);
	if (ret)
		goto err_sysfs;

	ret = netlink_register_notifier(&omci_netlink_nb);
	if (ret)
		goto err_genl;

	return 0;

err_genl:
	genl_unregister_family(&omci_genl_family);
err_sysfs:
	omci_sysfs_exit();
	return ret;
}
subsys_initcall(omci_init);

static void __exit omci_exit(void)
{
	netlink_unregister_notifier(&omci_netlink_nb);
	genl_unregister_family(&omci_genl_family);
	omci_sysfs_exit();
}
module_exit(omci_exit);

MODULE_DESCRIPTION("Generic OMCI transport and backend core");
MODULE_LICENSE("GPL");
