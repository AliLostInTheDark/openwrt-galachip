// SPDX-License-Identifier: GPL-2.0-only
/* Existing administration API, served locally by the authoritative engine. */
#include "daemon.h"

static int put_status(struct omcid *ctx, struct sk_buff *out, bool diagnostics)
{
	struct omci_device *d = &ctx->engine;
	struct omci_telemetry t = {};
	int ret = d->ops->get_telemetry(d, &t);

	if (ret)
		return ret;
	if (nla_put_u32(out, OMCI_ATTR_DEV_ID, ctx->transport.device) ||
	    nla_put_u32(out, OMCI_ATTR_IFINDEX, d->ifindex) ||
	    nla_put_u16(out, OMCI_ATTR_ONU_ID, d->onu_id) ||
	    nla_put_u16(out, OMCI_ATTR_GEM_PORT_ID, d->gem_port_id) ||
	    nla_put_u8(out, OMCI_ATTR_STATE, d->state) ||
	    nla_put_u32(out, OMCI_ATTR_FLAGS, d->channel_up ? OMCI_F_CHANNEL_UP : 0) ||
	    nla_put_u32(out, OMCI_ATTR_CAPABILITIES, d->capabilities | OMCI_CAP_BASELINE_AGENT) ||
	    nla_put_u32(out, OMCI_ATTR_OWNER_PORTID, d->owner_portid) ||
	    nla_put_u32(out, OMCI_ATTR_GENERATION, ctx->transport.generation) ||
	    nla_put_u32(out, OMCI_ATTR_BIND_FLAGS, OMCI_BIND_F_AGENT) ||
	    nla_put_u8(out, OMCI_ATTR_RESPONDER_MODE, OMCI_RESPONDER_USERSPACE) ||
	    nla_put_u64_64bit(out, OMCI_ATTR_RX_PACKETS, d->rx_packets, OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(out, OMCI_ATTR_RX_BYTES, d->rx_bytes, OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(out, OMCI_ATTR_RX_DROPPED, d->rx_dropped, OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(out, OMCI_ATTR_TX_PACKETS, d->tx_packets, OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(out, OMCI_ATTR_TX_BYTES, d->tx_bytes, OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(out, OMCI_ATTR_TX_ERRORS, d->tx_errors, OMCI_ATTR_PAD) ||
	    nla_put_u32(out, OMCI_ATTR_TELEMETRY_VALID, t.valid))
		return -EMSGSIZE;
	if ((t.valid & OMCI_TELEMETRY_F_FEC_DOWNSTREAM) &&
	    nla_put_u8(out, OMCI_ATTR_FEC_DOWNSTREAM, t.downstream_fec))
		return -EMSGSIZE;
	if ((t.valid & OMCI_TELEMETRY_F_FEC_UPSTREAM) &&
	    nla_put_u8(out, OMCI_ATTR_FEC_UPSTREAM, t.upstream_fec))
		return -EMSGSIZE;
#define PUT_TELEMETRY(flag, attr, field, fn) \
	if ((t.valid & OMCI_TELEMETRY_F_##flag) && fn(out, OMCI_ATTR_##attr, t.field)) \
		return -EMSGSIZE
	PUT_TELEMETRY(BOSA_TEMPERATURE, BOSA_TEMPERATURE_MC, bosa_temperature_mc, nla_put_s32);
	PUT_TELEMETRY(BOSA_VOLTAGE, BOSA_VOLTAGE_UV, bosa_voltage_uv, nla_put_u32);
	PUT_TELEMETRY(BOSA_BIAS, BOSA_BIAS_UA, bosa_bias_ua, nla_put_u32);
	PUT_TELEMETRY(BOSA_TX_POWER, BOSA_TX_POWER_NW, bosa_tx_power_nw, nla_put_u32);
	PUT_TELEMETRY(BOSA_RX_POWER, BOSA_RX_POWER_NW, bosa_rx_power_nw, nla_put_u32);
	PUT_TELEMETRY(BOSA_ALARMS, BOSA_ALARMS, bosa_alarms, nla_put_u32);
#undef PUT_TELEMETRY
	if (diagnostics) {
		char alarms[PAGE_SIZE] = {};

		ret = omci_agent_show_alarms(d, alarms);
		if (ret < 0)
			return ret;
		if (nla_put_string(out, OMCI_ATTR_ALARM_TEXT, alarms))
			return -EMSGSIZE;
	}
	return omci_agent_put_status(out, d);
}

static int set_agent(struct omcid *ctx, const struct nlattr **tb)
{
	static const struct { unsigned int attr; u16 key; } keys[] = {
		{ OMCI_ATTR_AGENT_ENABLED, OMCI_CONFIG_AGENT_ENABLED },
		{ OMCI_ATTR_AGENT_PERMISSIVE, OMCI_CONFIG_AGENT_PERMISSIVE },
		{ OMCI_ATTR_AGENT_FAKE_OMCI, OMCI_CONFIG_AGENT_FAKE_OMCI },
		{ OMCI_ATTR_OLT_PROFILE_CONFIGURED, OMCI_CONFIG_OLT_PROFILE },
		{ OMCI_ATTR_OLT_PROFILE_FORCED, OMCI_CONFIG_OLT_PROFILE_FORCE },
	};
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(keys); i++) {
		u8 value;

		if (!tb[keys[i].attr])
			continue;
		if (omcid_scalar(tb[keys[i].attr], &value, sizeof(value)))
			return -EINVAL;
		ret = omci_agent_config_set(&ctx->engine, keys[i].key, &value, sizeof(value));
		if (ret)
			return ret;
	}
	return 0;
}

int omcid_control(struct omcid *ctx, const struct nlmsghdr *request,
		  struct omcid_message *reply)
{
	const struct genlmsghdr *g = NLMSG_DATA(request);
	const struct nlattr *tb[OMCI_ATTR_MAX + 1];
	struct sk_buff out = { .data = reply->data, .capacity = sizeof(reply->data),
		.len = NLMSG_HDRLEN + GENL_HDRLEN };
	struct nlmsghdr *n = (void *)reply->data;
	struct genlmsghdr *response_g = (void *)(reply->data + NLMSG_HDRLEN);
	struct omci_mib_object object = {};
	struct omci_me_class class;
	u32 device = ctx->transport.device, index = 0, next = 0;
	u16 class_id = 0, entity = 0, key = 0;
	const char *name;
	bool mutating = false;
	u8 value[OMCI_MAX_CONFIG_VALUE], source, diagnostics = 0;
	size_t len = sizeof(value);
	int ret = omcid_parse(request, tb, OMCI_ATTR_MAX);

	if (ret)
		return ret;
	if (g->version != OMCI_GENL_VERSION)
		return -EPROTONOSUPPORT;
	if (tb[OMCI_ATTR_DEV_ID] && omcid_scalar(tb[OMCI_ATTR_DEV_ID], &device, sizeof(device)))
		return -EINVAL;
	if (device != ctx->transport.device)
		return -ENODEV;
	if (tb[OMCI_ATTR_INDEX] && omcid_scalar(tb[OMCI_ATTR_INDEX], &index, sizeof(index)))
		return -EINVAL;
	if (tb[OMCI_ATTR_CLASS_ID] && omcid_scalar(tb[OMCI_ATTR_CLASS_ID], &class_id, sizeof(class_id)))
		return -EINVAL;
	if (tb[OMCI_ATTR_ENTITY_ID] && omcid_scalar(tb[OMCI_ATTR_ENTITY_ID], &entity, sizeof(entity)))
		return -EINVAL;
	if (tb[OMCI_ATTR_CONFIG_KEY] && omcid_scalar(tb[OMCI_ATTR_CONFIG_KEY], &key, sizeof(key)))
		return -EINVAL;
	memset(reply, 0, sizeof(*reply));
	n->nlmsg_type = request->nlmsg_type;
	n->nlmsg_seq = request->nlmsg_seq;
	response_g->cmd = g->cmd;
	response_g->version = OMCI_GENL_VERSION;
	ret = nla_put_u32(&out, OMCI_ATTR_DEV_ID, device);
	if (ret)
		return ret;
	switch (g->cmd) {
	case OMCI_CMD_GET:
		if (tb[OMCI_ATTR_DIAGNOSTICS] &&
		    (omcid_scalar(tb[OMCI_ATTR_DIAGNOSTICS], &diagnostics, sizeof(diagnostics)) ||
		     diagnostics > 1))
			return -EINVAL;
		/* put_status includes the device attribute itself. */
		out.len = NLMSG_HDRLEN + GENL_HDRLEN;
		ret = put_status(ctx, &out, diagnostics);
		break;
	case OMCI_CMD_CONFIG_GET:
		if (!key)
			return -EINVAL;
		ret = omci_agent_config_get(&ctx->engine, key, value, &len);
		if (ret)
			break;
		ret = omci_agent_config_source_get(&ctx->engine, key, &source);
		if (ret)
			source = OMCI_CONFIG_SOURCE_UNSPEC;
		ret = nla_put_u16(&out, OMCI_ATTR_CONFIG_KEY, key) ||
		      nla_put(&out, OMCI_ATTR_CONFIG_VALUE, len, value) ||
		      nla_put_u8(&out, OMCI_ATTR_CONFIG_SOURCE, source) ? -EMSGSIZE : 0;
		break;
	case OMCI_CMD_CONFIG_SET:
		if (!key || !tb[OMCI_ATTR_CONFIG_VALUE] ||
		    omcid_length(tb[OMCI_ATTR_CONFIG_VALUE]) > OMCI_MAX_CONFIG_VALUE)
			return -EINVAL;
		mutating = true;
		ret = omci_agent_config_set(&ctx->engine, key, omcid_data(tb[OMCI_ATTR_CONFIG_VALUE]),
					    omcid_length(tb[OMCI_ATTR_CONFIG_VALUE]));
		break;
	case OMCI_CMD_AGENT_SET:
		mutating = true;
		ret = set_agent(ctx, tb);
		break;
	case OMCI_CMD_MIB_GET:
	case OMCI_CMD_MIB_NEXT:
		if (g->cmd == OMCI_CMD_MIB_GET) {
			if (!tb[OMCI_ATTR_CLASS_ID] || !tb[OMCI_ATTR_ENTITY_ID])
				return -EINVAL;
			ret = omci_agent_mib_get(&ctx->engine, class_id, entity, &object);
			name = omci_me_class_name(class_id);
		} else {
			ret = omci_agent_mib_next(&ctx->engine, index, &object, &next, &name);
		}
		if (!ret)
			ret = omci_native_put_mib_object(&out, &object, next, name);
		break;
	case OMCI_CMD_MIB_SET:
		if (!tb[OMCI_ATTR_CLASS_ID] || !tb[OMCI_ATTR_ENTITY_ID])
			return -EINVAL;
		mutating = true;
		object.class_id = class_id;
		object.entity_id = entity;
		object.attr_mask = 0xffff;
		if (tb[OMCI_ATTR_ATTR_MASK] &&
		    omcid_scalar(tb[OMCI_ATTR_ATTR_MASK], &object.attr_mask, sizeof(object.attr_mask)))
			return -EINVAL;
		if (tb[OMCI_ATTR_ATTR_DATA]) {
			len = omcid_length(tb[OMCI_ATTR_ATTR_DATA]);
			if (len > sizeof(object.data))
				return -E2BIG;
			memcpy(object.data, omcid_data(tb[OMCI_ATTR_ATTR_DATA]), len);
			object.data_len = len;
		}
		ret = omci_agent_mib_set(&ctx->engine, &object);
		break;
	case OMCI_CMD_MIB_DELETE:
		if (!tb[OMCI_ATTR_CLASS_ID] || !tb[OMCI_ATTR_ENTITY_ID])
			return -EINVAL;
		mutating = true;
		ret = omci_agent_mib_delete(&ctx->engine, class_id, entity);
		break;
	case OMCI_CMD_MIB_RESET:
		mutating = true;
		ret = omci_agent_mib_reset_result(&ctx->engine, true);
		break;
	case OMCI_CMD_CLASS_GET:
	case OMCI_CMD_CLASS_NEXT:
		if (g->cmd == OMCI_CMD_CLASS_GET) {
			if (!tb[OMCI_ATTR_CLASS_ID])
				return -EINVAL;
			ret = omci_me_class_get(&ctx->engine, class_id, &class);
		} else {
			ret = omci_me_class_next(&ctx->engine, index, &class, &next);
		}
		if (!ret)
			ret = nla_put_u16(&out, OMCI_ATTR_CLASS_ID, class.class_id) ||
			      nla_put_string(&out, OMCI_ATTR_NAME, class.name) ||
			      nla_put_u8(&out, OMCI_ATTR_CLASS_CATEGORY, class.category) ||
			      nla_put_u8(&out, OMCI_ATTR_CLASS_SUPPORT, class.support) ||
			      nla_put_u32(&out, OMCI_ATTR_CLASS_FLAGS, class.flags) ||
			      nla_put_u32(&out, OMCI_ATTR_INDEX, next) ? -EMSGSIZE : 0;
		break;
	default:
		return -EOPNOTSUPP;
	}
	if (!ret && ctx->error)
		ret = ctx->error;
	if (!ret && mutating)
		omcid_checkpoint_defer(ctx);
	if (ret)
		return ret;
	if (mutating) {
		struct nlmsgerr *ack = (void *)(reply->data + NLMSG_HDRLEN);

		memset(reply->data, 0, NLMSG_LENGTH(sizeof(*ack)));
		n->nlmsg_type = NLMSG_ERROR;
		n->nlmsg_seq = request->nlmsg_seq;
		ack->msg = *request;
		out.len = NLMSG_LENGTH(sizeof(*ack));
	}
	n->nlmsg_len = out.len;
	reply->len = out.len;
	return 0;
}
