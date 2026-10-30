// SPDX-License-Identifier: GPL-2.0-only
/*
 * ONU Management and Control Interface engine, run by omcid
 *
 * Formerly the in-kernel agent. It implements the OMCI transaction path
 * and owns the operational MIB. Managed entities without a dedicated hardware callback
 * are stored as opaque baseline attribute data so that userspace can inspect,
 * persist and amend them through Generic Netlink.
 */

#include "native.h"
#include "internal.h"
#include "me.h"
#include "attributes.h"
#include "wire.h"


static void
omci_agent_report_operational(struct omci_device *odev, bool operational)
{
	if (odev->ops->set_operational)
		odev->ops->set_operational(odev, operational);
	omci_device_notify(odev, OMCI_EVENT_OPERATIONAL_CHANGE);
}

#define OMCI_OMCC_VERSION_G988_2010_BASELINE	0xa0

#define OMCI_MSG_TYPE_CREATE		4
#define OMCI_MSG_TYPE_DELETE		6
#define OMCI_MSG_TYPE_SET		8
#define OMCI_MSG_TYPE_GET		9
#define OMCI_MSG_TYPE_GET_ALL_ALARMS	11
#define OMCI_MSG_TYPE_GET_ALL_ALARMS_NEXT 12
#define OMCI_MSG_TYPE_MIB_UPLOAD	13
#define OMCI_MSG_TYPE_MIB_UPLOAD_NEXT	14
#define OMCI_MSG_TYPE_MIB_RESET	15
#define OMCI_MSG_TYPE_ALARM_NOTIFICATION 16
#define OMCI_MSG_TYPE_AVC		17
#define OMCI_MSG_TYPE_TEST		18
#define OMCI_MSG_TYPE_START_SW_DOWNLOAD 19
#define OMCI_MSG_TYPE_DOWNLOAD_SECTION	20
#define OMCI_MSG_TYPE_END_SW_DOWNLOAD	21
#define OMCI_MSG_TYPE_ACTIVATE_SW	22
#define OMCI_MSG_TYPE_COMMIT_SW	23
#define OMCI_MSG_TYPE_SYNC_TIME	24
#define OMCI_MSG_TYPE_REBOOT		25
#define OMCI_MSG_TYPE_GET_NEXT		26
#define OMCI_MSG_TYPE_TEST_RESULT	27
#define OMCI_MSG_TYPE_GET_CURRENT_DATA	28
#define OMCI_MSG_TYPE_SET_TABLE	29

#define OMCI_RESULT_SUCCESS		0
#define OMCI_RESULT_PROCESSING_ERROR	1
#define OMCI_RESULT_NOT_SUPPORTED	2
#define OMCI_RESULT_PARAMETER_ERROR	3
#define OMCI_RESULT_UNKNOWN_ME		4
#define OMCI_RESULT_UNKNOWN_INSTANCE	5
#define OMCI_RESULT_DEVICE_BUSY	6
#define OMCI_RESULT_INSTANCE_EXISTS	7
#define OMCI_RESULT_ATTRIBUTE_FAILED	9

#define OMCI_BRIDGE_TP_PPTP_ETH_UNI	1
#define OMCI_BRIDGE_TP_8021P_MAPPER	3
#define OMCI_BRIDGE_TP_GEM_IWTP	5
#define OMCI_BRIDGE_TP_MULTICAST_GEM_IWTP	6
#define OMCI_BRIDGE_TP_VEIP		11
#define OMCI_EXT_VLAN_ASSOC_VEIP	10

static void omci_agent_reset_table_snapshot_locked(struct omci_agent *agent);
static void omci_agent_alarm_work(struct work_struct *work);

static bool omci_agent_result_can_be_faked(u8 result)
{
	switch (result) {
	case OMCI_RESULT_NOT_SUPPORTED:
	case OMCI_RESULT_UNKNOWN_ME:
	case OMCI_RESULT_UNKNOWN_INSTANCE:
	case OMCI_RESULT_INSTANCE_EXISTS:
		return true;
	default:
		return false;
	}
}

#define OMCI_PRIORITY_QUEUE_ATTR_MASK		0xfff0
#define OMCI_TRAFFIC_SCHEDULER_ATTR_MASK	0xf000

#define OMCI_PRIORITY_QUEUE_QUEUE_CONFIG_MASK	0x8000
#define OMCI_PRIORITY_QUEUE_MAX_SIZE_MASK	0x4000
#define OMCI_PRIORITY_QUEUE_ALLOC_SIZE_MASK	0x2000
#define OMCI_PRIORITY_QUEUE_DISCARD_RESET_MASK	0x1000
#define OMCI_PRIORITY_QUEUE_DISCARD_THRESHOLD_MASK 0x0800
#define OMCI_PRIORITY_QUEUE_RELATED_PORT_MASK	0x0400
#define OMCI_PRIORITY_QUEUE_SCHEDULER_MASK	0x0200
#define OMCI_PRIORITY_QUEUE_WEIGHT_MASK	0x0100
#define OMCI_PRIORITY_QUEUE_BACKPRESSURE_OP_MASK	0x0080
#define OMCI_PRIORITY_QUEUE_BACKPRESSURE_TIME_MASK 0x0040
#define OMCI_PRIORITY_QUEUE_BP_OCCUR_MASK	0x0020
#define OMCI_PRIORITY_QUEUE_BP_CLEAR_MASK	0x0010

#define OMCI_TRAFFIC_SCHEDULER_TCONT_MASK	0x8000
#define OMCI_TRAFFIC_SCHEDULER_PARENT_MASK	0x4000
#define OMCI_TRAFFIC_SCHEDULER_POLICY_MASK	0x2000
#define OMCI_TRAFFIC_SCHEDULER_PRIORITY_MASK	0x1000

#define OMCI_DEFAULT_TCONT_COUNT		8
#define OMCI_DEFAULT_QUEUES_PER_TCONT		1
#define OMCI_MAX_TCONT_RESOURCES		32
#define OMCI_MAX_QUEUES_PER_TCONT		8

#define OMCI_OMCI_ME_TYPE_TABLE_MASK		BIT(15)
#define OMCI_OMCI_MESSAGE_TYPE_TABLE_MASK	BIT(14)
#define OMCI_TABLE_SNAPSHOT_TIMEOUT		(60 * HZ)
#define OMCI_BASELINE_TABLE_FRAGMENT_LEN	29

#define OMCI_MANAGED_ENTITY_NAME_MASK		BIT(15)
#define OMCI_MANAGED_ENTITY_ATTR_TABLE_MASK	BIT(14)
#define OMCI_MANAGED_ENTITY_ACCESS_MASK		BIT(13)
#define OMCI_MANAGED_ENTITY_ALARMS_MASK		BIT(12)
#define OMCI_MANAGED_ENTITY_AVCS_MASK		BIT(11)
#define OMCI_MANAGED_ENTITY_ACTIONS_MASK	BIT(10)
#define OMCI_MANAGED_ENTITY_INSTANCES_MASK	BIT(9)
#define OMCI_MANAGED_ENTITY_SUPPORT_MASK	BIT(8)
#define OMCI_MANAGED_ENTITY_TABLE_MASKS \
	(OMCI_MANAGED_ENTITY_ATTR_TABLE_MASK | \
	 OMCI_MANAGED_ENTITY_ALARMS_MASK | \
	 OMCI_MANAGED_ENTITY_AVCS_MASK | \
	 OMCI_MANAGED_ENTITY_INSTANCES_MASK)

#define OMCI_ATTRIBUTE_NAME_MASK		BIT(15)
#define OMCI_ATTRIBUTE_SIZE_MASK		BIT(14)
#define OMCI_ATTRIBUTE_ACCESS_MASK		BIT(13)
#define OMCI_ATTRIBUTE_FORMAT_MASK		BIT(12)
#define OMCI_ATTRIBUTE_LOWER_LIMIT_MASK	BIT(11)
#define OMCI_ATTRIBUTE_UPPER_LIMIT_MASK	BIT(10)
#define OMCI_ATTRIBUTE_BIT_FIELD_MASK		BIT(9)
#define OMCI_ATTRIBUTE_CODE_POINTS_MASK	BIT(8)
#define OMCI_ATTRIBUTE_SUPPORT_MASK		BIT(7)
#define OMCI_ATTRIBUTE_TABLE_MASKS		OMCI_ATTRIBUTE_CODE_POINTS_MASK

#define OMCI_CAPABILITY_NAME_LEN		25
#define OMCI_ATTRIBUTE_FORMAT_BIT_FIELD	2
#define OMCI_ATTRIBUTE_FORMAT_UNSIGNED		4
#define OMCI_ATTRIBUTE_FORMAT_STRING		5
#define OMCI_ATTRIBUTE_FORMAT_TABLE		7

#define OMCI_ALARM_BITMAP_LEN		28
/* Three bytes of padding sit between the bitmap and the sequence number. */
#define OMCI_ALARM_SEQUENCE_OFFSET	(8 + OMCI_ALARM_BITMAP_LEN + 3)
#define OMCI_ALARM(_n)			BIT(7 - (_n))
#define OMCI_ALARM_POLL_INTERVAL	HZ
#define OMCI_ALARM_MAX_UNIS		4
/* Per entity: an alarm, an ARC AVC and (UNIs) an operational state AVC. */
#define OMCI_ALARM_MAX_PDUS		(3 * (1 + OMCI_ALARM_MAX_UNIS))

/* G.988 9.2.1 ANI-G alarms */
enum {
	OMCI_ANI_G_ALARM_LOW_RX,
	OMCI_ANI_G_ALARM_HIGH_RX,
	OMCI_ANI_G_ALARM_SF,
	OMCI_ANI_G_ALARM_SD,
	OMCI_ANI_G_ALARM_LOW_TX,
	OMCI_ANI_G_ALARM_HIGH_TX,
	OMCI_ANI_G_ALARM_LASER_BIAS,
};

/* G.988 9.5.1 PPTP Ethernet UNI alarms */
#define OMCI_PPTP_ALARM_LAN_LOS		0

#define OMCI_ANI_G_ENTITY_ID		0x8001
#define OMCI_ETHERNET_SLOT		0x01
#define OMCI_NOKIA_VEIP_SLOT		0x06
#define OMCI_VEIP_SLOT			0x0a
#define OMCI_GPON_SLOT			0x80
#define OMCI_ETHERNET_UNIT_TYPE		47
#define OMCI_VEIP_UNIT_TYPE		48
#define OMCI_GPON_UNIT_TYPE		248
#define OMCI_EQUIPMENT_ENTITY_ID(_slot)	(0x0100 | (_slot))
#define OMCI_UNI_ENTITY_ID(_slot, _port)	(((_slot) << 8) | (_port))
#define OMCI_ANI_G_RX_LEVEL_MASK	0x0040
#define OMCI_ANI_G_TX_LEVEL_MASK	0x0004
#define OMCI_ANI_G_OPTICAL_LEVEL_MASK	(OMCI_ANI_G_RX_LEVEL_MASK | \
					 OMCI_ANI_G_TX_LEVEL_MASK)
#define OMCI_ANI_G_ARC_MASK		BIT(7)
#define OMCI_ANI_G_SR_OFFSET		0
#define OMCI_ANI_G_TCONT_COUNT_OFFSET	1
#define OMCI_ANI_G_GEM_BLOCK_OFFSET	3
#define OMCI_ANI_G_SF_OFFSET		7
#define OMCI_ANI_G_SD_OFFSET		8
#define OMCI_ANI_G_ARC_OFFSET		9
#define OMCI_ANI_G_RX_LEVEL_OFFSET	11
#define OMCI_ANI_G_LOWER_RX_OFFSET	13
#define OMCI_ANI_G_UPPER_RX_OFFSET	14
#define OMCI_ANI_G_RESPONSE_TIME_OFFSET	15
#define OMCI_ANI_G_TX_LEVEL_OFFSET	17
#define OMCI_ANI_G_LOWER_TX_OFFSET	19
#define OMCI_ANI_G_UPPER_TX_OFFSET	20
/* G.988 defaults; the threshold defaults select the ONU's own policy */
#define OMCI_ANI_G_DEFAULT_GEM_BLOCK	48
#define OMCI_ANI_G_DEFAULT_SF		5
#define OMCI_ANI_G_DEFAULT_SD		9
#define OMCI_ANI_G_DEFAULT_RESPONSE_NS	35000
#define OMCI_ANI_G_RX_POLICY		0xff
#define OMCI_ANI_G_TX_POLICY		0x81

#define OMCI_PPTP_SENSED_TYPE_OFFSET	1
#define OMCI_PPTP_ADMIN_STATE_OFFSET	4
#define OMCI_PPTP_OP_STATE_OFFSET	5
#define OMCI_PPTP_OP_STATE_MASK		BIT(10)
#define OMCI_PPTP_BRIDGED_IP_OFFSET	12
#define OMCI_CARD_10_100_1000BASE_T	47	/* G.988 Table 9.1.5-1 */
#define OMCI_PPTP_ARC_OFFSET		13
#define OMCI_PPTP_ARC_MASK		BIT(3)

/* GPON downstream line rate in bit/s, for the BER behind SF and SD */
#define OMCI_GPON_DS_RATE		2488320000ULL
#define OMCI_BER_WINDOW			(10 * HZ)

#define OMCI_TEST_TYPE_UNSUPPORTED	0
#define OMCI_TEST_TYPE_VOLTAGE		1
#define OMCI_TEST_TYPE_RX_POWER		3
#define OMCI_TEST_TYPE_TX_POWER		5
#define OMCI_TEST_TYPE_BIAS		9
#define OMCI_TEST_TYPE_TEMPERATURE	12

#define OMCI_OPTICAL_LEVEL_SCALE	500
#define OMCI_NW_TO_DBM_0P002_OFFSET	30000
#define OMCI_NW_TO_DBUW_0P002_OFFSET	15000


#define OMCI_ONU_G_ATTR_MASK		GENMASK(15, 3)
#define OMCI_ONU2_G_ATTR_MASK		GENMASK(15, 2)

#define OMCI_ATTR_VALUE_ZERO		U8_MAX

/* ONU-G LOID attributes do not fit in MIB object data; Gets append them */
#define OMCI_ONU_G_LOID_OFFSET		32
#define OMCI_ONU_G_LOID_PASSWORD_OFFSET	(OMCI_ONU_G_LOID_OFFSET + OMCI_LOID_LEN)
#define OMCI_ONU_G_CREDENTIALS_OFFSET	(OMCI_ONU_G_LOID_PASSWORD_OFFSET + \
					 OMCI_LOID_PASSWORD_LEN)
#define OMCI_ONU_G_GET_LEN		(OMCI_ONU_G_CREDENTIALS_OFFSET + 1)

struct omci_get_attr_layout {
	u16 mask;
	u8 offset;
	u8 len;
};

static const struct omci_get_attr_layout omci_onu_g_attr_layout[] = {
	{ BIT(15), 0, 4 },		/* Vendor ID */
	{ BIT(14), 4, 14 },		/* Version */
	{ BIT(13), 18, 8 },		/* Serial number */
	{ BIT(12), 26, 1 },		/* Traffic management option */
	{ BIT(11), 27, 1 },		/* Deprecated */
	{ BIT(10), 28, 1 },		/* Battery backup */
	{ BIT(9), 29, 1 },		/* Administrative state */
	{ BIT(8), 30, 1 },		/* Operational state */
	{ BIT(7), 31, 1 },		/* ONU survival time */
	{ BIT(6), OMCI_ONU_G_LOID_OFFSET, OMCI_LOID_LEN },	/* Logical ONU ID */
	{ BIT(5), OMCI_ONU_G_LOID_PASSWORD_OFFSET,
	  OMCI_LOID_PASSWORD_LEN },		/* Logical password */
	{ BIT(4), OMCI_ONU_G_CREDENTIALS_OFFSET, 1 },	/* Credentials status */
	{ BIT(3), OMCI_ATTR_VALUE_ZERO, 1 },	/* Extended TC layer */
};

static const struct omci_get_attr_layout omci_onu2_g_attr_layout[] = {
	{ BIT(15), 0, 20 },		/* Equipment ID */
	{ BIT(14), 20, 1 },		/* OMCC version */
	{ BIT(13), 21, 2 },		/* Vendor product code */
	{ BIT(12), 23, 1 },		/* Security capability */
	{ BIT(11), 24, 1 },		/* Security mode */
	{ BIT(10), 25, 2 },		/* Total priority queues */
	{ BIT(9), 27, 1 },		/* Total traffic schedulers */
	{ BIT(8), 28, 1 },		/* Deprecated */
	{ BIT(7), 29, 2 },		/* Total GEM port IDs */
	{ BIT(6), OMCI_ATTR_VALUE_ZERO, 4 },	/* System uptime */
	{ BIT(5), 35, 2 },		/* Connectivity capability */
	{ BIT(4), 31, 1 },		/* Connectivity mode */
	{ BIT(3), 32, 2 },		/* QoS: scheduler and T-CONT policies are writable */
	{ BIT(2), 40, 2 },		/* Priority queue scale factor */
};

static int omci_olt_g_parse_set(struct omci_mib_object *object, u16 mask,
				const u8 *data, size_t len)
{
	return omci_attr_olt_g_set(&object->olt_g, mask, data, len);
}

static void omci_vlan_filter_parse_create(struct omci_mib_object *object,
					  const u8 *content)
{
	omci_attr_vlan_filter_create(&object->vlan_filter, content,
				     sizeof(object->data));
}

static void omci_ext_vlan_parse_create(struct omci_mib_object *object,
				       const u8 *content)
{
	omci_attr_extended_vlan_create(&object->extended_vlan, content,
				       sizeof(object->data));
}

/*
 * The UNI side of a single-tag class 171 rule (G.988 9.3.13): priority 15
 * in a filter means "no such tag", in a treatment "add no tag", and a lone
 * added tag uses the inner treatment fields. VID 4096/4097 copy a received
 * tag, which keeps the UNI VID.
 */
static void omci_agent_service_uni_vlan(const struct omci_extended_vlan_rule *rule,
					struct omci_service_config *service)
{
	bool untagged = rule->filter_outer_pbit == 15 && rule->filter_inner_pbit == 15;
	bool single = rule->filter_outer_pbit == 15 && rule->filter_inner_pbit != 15;
	bool add = rule->treat_outer_pbit == 15 && rule->treat_inner_pbit != 15;
	u16 vid = rule->treat_inner_vid;

	service->uni_vlan_mode = OMCI_UNI_VLAN_SAME;
	if (rule->treat_outer_pbit != 15 || rule->tags_to_remove > 1)
		return;
	if (untagged && add && vid < 4095) {
		service->uni_vlan_mode = OMCI_UNI_VLAN_UNTAGGED;
	} else if (single && add && vid < 4095 &&
		   rule->filter_inner_vid < 4095 && rule->filter_inner_vid != vid) {
		service->uni_vlan_id = rule->filter_inner_vid;
		service->uni_vlan_mode = rule->tags_to_remove ?
					 OMCI_UNI_VLAN_TRANSLATE :
					 OMCI_UNI_VLAN_STACKED;
	}
}

struct omci_attr_layout {
	u16 mask;
	u8 offset;
	u8 length;
};

static const struct omci_attr_layout omci_priority_queue_layout[] = {
	{ OMCI_PRIORITY_QUEUE_QUEUE_CONFIG_MASK, 0, 1 },
	{ OMCI_PRIORITY_QUEUE_MAX_SIZE_MASK, 1, 2 },
	{ OMCI_PRIORITY_QUEUE_ALLOC_SIZE_MASK, 3, 2 },
	{ OMCI_PRIORITY_QUEUE_DISCARD_RESET_MASK, 5, 2 },
	{ OMCI_PRIORITY_QUEUE_DISCARD_THRESHOLD_MASK, 7, 2 },
	{ OMCI_PRIORITY_QUEUE_RELATED_PORT_MASK, 9, 4 },
	{ OMCI_PRIORITY_QUEUE_SCHEDULER_MASK, 13, 2 },
	{ OMCI_PRIORITY_QUEUE_WEIGHT_MASK, 15, 1 },
	{ OMCI_PRIORITY_QUEUE_BACKPRESSURE_OP_MASK, 16, 2 },
	{ OMCI_PRIORITY_QUEUE_BACKPRESSURE_TIME_MASK, 18, 4 },
	{ OMCI_PRIORITY_QUEUE_BP_OCCUR_MASK, 22, 2 },
	{ OMCI_PRIORITY_QUEUE_BP_CLEAR_MASK, 24, 2 },
};

static const struct omci_attr_layout omci_traffic_scheduler_layout[] = {
	{ OMCI_TRAFFIC_SCHEDULER_TCONT_MASK, 0, 2 },
	{ OMCI_TRAFFIC_SCHEDULER_PARENT_MASK, 2, 2 },
	{ OMCI_TRAFFIC_SCHEDULER_POLICY_MASK, 4, 1 },
	{ OMCI_TRAFFIC_SCHEDULER_PRIORITY_MASK, 5, 1 },
};

static int omci_attr_parse_set(struct omci_mib_object *object, u16 mask,
			       const u8 *data, size_t len,
			       const struct omci_attr_layout *layout,
			       size_t layout_count, u16 supported_mask)
{
	size_t i;

	if (mask & ~supported_mask)
		return -EINVAL;

	for (i = 0; i < layout_count; i++) {
		if (!(mask & layout[i].mask))
			continue;
		if (len < layout[i].length)
			return -EINVAL;
		memcpy(object->data + layout[i].offset, data,
		       layout[i].length);
		data += layout[i].length;
		len -= layout[i].length;
	}

	return 0;
}

static int omci_attr_serialize(const struct omci_mib_object *object, u16 mask,
			       u8 *data, size_t len, size_t *encoded_len,
			       const struct omci_attr_layout *layout,
			       size_t layout_count, u16 supported_mask)
{
	size_t used = 0;
	size_t i;

	if (mask & ~supported_mask)
		return -EINVAL;

	for (i = 0; i < layout_count; i++) {
		if (!(mask & layout[i].mask))
			continue;
		if (len - used < layout[i].length)
			return -ENOSPC;
		memcpy(data + used, object->data + layout[i].offset,
		       layout[i].length);
		used += layout[i].length;
	}

	*encoded_len = used;
	return 0;
}

static int
omci_priority_queue_serialize(const struct omci_mib_object *object,
			      u16 mask, u8 *data, size_t len,
			      size_t *encoded_len)
{
	return omci_attr_serialize(object, mask, data, len, encoded_len,
				   omci_priority_queue_layout,
				   ARRAY_SIZE(omci_priority_queue_layout),
				   OMCI_PRIORITY_QUEUE_ATTR_MASK);
}

static int
omci_traffic_scheduler_serialize(const struct omci_mib_object *object,
				 u16 mask, u8 *data, size_t len,
				 size_t *encoded_len)
{
	return omci_attr_serialize(object, mask, data, len, encoded_len,
				   omci_traffic_scheduler_layout,
				   ARRAY_SIZE(omci_traffic_scheduler_layout),
				   OMCI_TRAFFIC_SCHEDULER_ATTR_MASK);
}

static int omci_mib_parse_create(struct omci_mib_object *object,
				 const u8 *content)
{
	switch (object->class_id) {
	case OMCI_CLASS_VLAN_TAGGING_FILTER_DATA:
		omci_vlan_filter_parse_create(object, content);
		break;
	case OMCI_CLASS_EXTENDED_VLAN:
		omci_ext_vlan_parse_create(object, content);
		break;
	default:
		break;
	}

	return 0;
}

static int omci_mib_parse_set(struct omci_mib_object *object, u16 mask,
			      const u8 *data, size_t len)
{
	switch (object->class_id) {
	case OMCI_CLASS_OLT_G:
		return omci_olt_g_parse_set(object, mask, data, len);
	case OMCI_CLASS_VLAN_TAGGING_FILTER_DATA:
		return omci_attr_vlan_filter_set(&object->vlan_filter, mask, data, len);
	case OMCI_CLASS_EXTENDED_VLAN:
		return omci_attr_extended_vlan_set(&object->extended_vlan, mask, data, len);
	case OMCI_CLASS_PRIORITY_QUEUE:
		return omci_attr_parse_set(object, mask, data, len,
				   omci_priority_queue_layout,
				   ARRAY_SIZE(omci_priority_queue_layout),
				   OMCI_PRIORITY_QUEUE_ATTR_MASK);
	case OMCI_CLASS_TRAFFIC_SCHEDULER:
		return omci_attr_parse_set(object, mask, data, len,
				   omci_traffic_scheduler_layout,
				   ARRAY_SIZE(omci_traffic_scheduler_layout),
				   OMCI_TRAFFIC_SCHEDULER_ATTR_MASK);
	default:
		return 0;
	}
}

static unsigned long omci_mib_key(u16 class_id, u16 entity_id)
{
	return ((unsigned long)class_id << 16) | entity_id;
}

static struct omci_mib_object *
omci_mib_lookup(struct omci_agent *agent, u16 class_id, u16 entity_id)
{
	struct omci_mib_object *object;

	object = xa_load(&agent->mib, omci_mib_key(class_id, entity_id));
	if (object && object->pending_delete)
		return NULL;

	return object;
}

static void omci_agent_set_mib_sync_locked(struct omci_agent *agent, u8 sync)
{
	struct omci_mib_object *object;

	agent->mib_sync = sync;
	object = omci_mib_lookup(agent, OMCI_CLASS_ONU_DATA, 0);
	if (object)
		object->data[0] = sync;
}

static void omci_agent_increment_mib_sync_locked(struct omci_agent *agent)
{
	omci_agent_set_mib_sync_locked(agent, (u8)(agent->mib_sync + 1));
}

static int omci_mib_store_locked(struct omci_agent *agent,
				 const struct omci_mib_object *source)
{
	struct omci_mib_object *object;
	void *old;

	object = kmemdup(source, sizeof(*object), GFP_KERNEL);
	if (!object)
		return -ENOMEM;

	old = xa_store(&agent->mib,
		       omci_mib_key(source->class_id, source->entity_id),
		       object, GFP_KERNEL);
	if (xa_is_err(old)) {
		kfree(object);
		return xa_err(old);
	}
	kfree(old);
	return 0;
}

static struct omci_olt_g *omci_agent_olt_g_locked(struct omci_agent *agent)
{
	struct omci_mib_object *object;

	object = omci_mib_lookup(agent, OMCI_CLASS_OLT_G, 0);
	if (!object || !object->olt_g.valid)
		return NULL;

	return &object->olt_g;
}

int omci_agent_olt_g_get(struct omci_device *odev, struct omci_olt_g *olt)
{
	struct omci_olt_g *stored;
	int ret = 0;

	if (!olt)
		return -EINVAL;

	mutex_lock(&odev->agent.lock);
	stored = omci_agent_olt_g_locked(&odev->agent);
	if (!stored)
		ret = -ENODATA;
	else
		*olt = *stored;
	mutex_unlock(&odev->agent.lock);

	return ret;
}

static void
omci_agent_profile_state_locked(struct omci_agent *agent,
				const struct omci_olt_g *olt,
				struct omci_olt_profile_state *state)
{
	u8 effective;

	memset(state, 0, sizeof(*state));
	state->configured = agent->config.olt_profile;
	state->forced = agent->config.olt_profile_force;

	if (state->forced != OMCI_OLT_PROFILE_UNSPEC)
		effective = state->forced;
	else if (state->configured == OMCI_OLT_PROFILE_AUTO)
		effective = omci_profile_detect(olt);
	else
		effective = state->configured;

	if (effective == OMCI_OLT_PROFILE_UNSPEC ||
	    effective == OMCI_OLT_PROFILE_AUTO)
		effective = OMCI_OLT_PROFILE_GENERIC;

	state->effective = effective;
	state->quirks = omci_profile_quirks(effective);
	if (olt)
		state->olt = *olt;
}

static int
omci_agent_profile_reconcile_locked(struct omci_device *odev,
				    u8 old_profile, u8 new_profile);
static int omci_agent_clear_services_locked(struct omci_device *odev);
static int omci_agent_reconcile_services_locked(struct omci_device *odev);
static u8 omci_agent_get_locked(struct omci_device *odev, u16 class_id,
				u16 entity_id, const u8 *content,
				size_t content_len, u8 *response_content,
				size_t response_capacity,
				size_t *response_len);
static void omci_agent_reset_duplicate_locked(struct omci_agent *agent);

static void omci_agent_free_object_array(struct xarray *objects)
{
	struct omci_mib_object *object;
	unsigned long index;

	xa_for_each(objects, index, object) {
		xa_erase(objects, index);
		kfree(object);
	}
}

static int omci_agent_snapshot_vendor_objects_locked(struct omci_agent *agent,
						      struct xarray *snapshot)
{
	struct omci_mib_object *object;
	struct omci_mib_object *copy;
	unsigned long index;
	void *old;

	xa_for_each(&agent->mib, index, object) {
		if (!object->owner_profile)
			continue;
		copy = kmemdup(object, sizeof(*copy), GFP_KERNEL);
		if (!copy)
			return -ENOMEM;
		old = xa_store(snapshot, index, copy, GFP_KERNEL);
		if (xa_is_err(old)) {
			kfree(copy);
			return xa_err(old);
		}
		kfree(old);
	}

	return 0;
}

static int omci_agent_restore_vendor_objects_locked(struct omci_agent *agent,
						     struct xarray *snapshot)
{
	struct omci_mib_object *object;
	unsigned long index;
	void *old;
	int ret = 0;

	xa_for_each(&agent->mib, index, object) {
		if (!object->owner_profile)
			continue;
		xa_erase(&agent->mib, index);
		kfree(object);
	}
	xa_for_each(snapshot, index, object) {
		xa_erase(snapshot, index);
		old = xa_store(&agent->mib, index, object, GFP_KERNEL);
		if (xa_is_err(old)) {
			kfree(object);
			if (!ret)
				ret = xa_err(old);
			continue;
		}
		kfree(old);
	}

	return ret;
}

static int
omci_agent_profile_refresh_locked(struct omci_device *odev,
				  struct omci_olt_g *olt,
				  bool *profile_changed)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_olt_profile_state old_state;
	struct omci_olt_profile_state state;
	struct xarray snapshot;
	u32 old_quirks = agent->profile_quirks;
	u8 old_profile = agent->profile_effective;
	bool changed;
	int ret;

	omci_agent_profile_state_locked(agent, olt, &state);
	omci_profile_sanitize_olt_g(olt, state.quirks);
	if (olt)
		state.olt = *olt;

	changed = old_profile != state.effective || old_quirks != state.quirks;
	if (!changed)
		goto apply_profile;

	xa_init(&snapshot);
	ret = omci_agent_snapshot_vendor_objects_locked(agent, &snapshot);
	if (ret)
		goto destroy_snapshot;
	ret = omci_agent_profile_reconcile_locked(odev, old_profile,
						  state.effective);
	if (ret)
		goto rollback;

apply_profile:
	if (odev->ops->set_olt_profile) {
		ret = odev->ops->set_olt_profile(odev, &state);
		if (ret && changed)
			goto rollback;
		if (ret)
			return ret;
	}

	agent->profile_effective = state.effective;
	agent->profile_quirks = state.quirks;
	if (profile_changed)
		*profile_changed = changed;

	if (changed) {
		omci_agent_reset_table_snapshot_locked(agent);
		agent->upload_index = 0;
		omci_agent_free_object_array(&snapshot);
		xa_destroy(&snapshot);
		/* The provider reports the effective profile; details at debug level. */
		dev_dbg(odev->parent,
			"OMCI OLT profile: configured=%s effective=%s forced=%s quirks=%#x\n",
			omci_olt_profile_name(state.configured),
			omci_olt_profile_name(state.effective),
			omci_olt_profile_name(state.forced), state.quirks);
	}

	return 0;

rollback:
	omci_agent_restore_vendor_objects_locked(agent, &snapshot);
	agent->profile_effective = old_profile;
	agent->profile_quirks = old_quirks;
	omci_agent_reconcile_services_locked(odev);
	if (odev->ops->set_olt_profile) {
		old_state = state;
		old_state.effective = old_profile;
		old_state.quirks = old_quirks;
		odev->ops->set_olt_profile(odev, &old_state);
	}
	omci_agent_free_object_array(&snapshot);

destroy_snapshot:
	xa_destroy(&snapshot);
	return ret;
}

static bool
omci_agent_profile_has_quirk(const struct omci_agent *agent, u32 quirk)
{
	return agent->profile_quirks & quirk;
}

static bool
omci_agent_fakes_unsupported(const struct omci_agent *agent)
{
	return agent->fake_omci ||
	       omci_agent_profile_has_quirk(agent,
					    OMCI_OLT_QUIRK_FAKE_UNSUPPORTED_SUCCESS);
}

static bool
omci_agent_should_fake_result(const struct omci_agent *agent, u16 class_id,
			      u8 result)
{
	const struct omci_me_desc *desc = omci_me_lookup(agent, class_id);

	if (desc && (desc->flags & OMCI_ME_F_DATAPATH))
		return false;

	return omci_agent_fakes_unsupported(agent) &&
	       omci_agent_result_can_be_faked(result);
}

static int
omci_mib_add_default_profile_mask(struct omci_agent *agent, u8 profile,
				  u16 class_id, u16 entity_id,
				  u16 attr_mask, const void *data,
				  size_t len)
{
	const struct omci_me_desc *desc;
	struct omci_mib_object *object;
	int ret;

	desc = omci_me_lookup_profile(profile, class_id);
	object = kzalloc(sizeof(*object), GFP_KERNEL);
	if (!object)
		return -ENOMEM;

	object->class_id = class_id;
	object->entity_id = entity_id;
	object->attr_mask = attr_mask;
	object->origin = OMCI_MIB_ORIGIN_DEFAULT;
	object->owner_profile = desc && (desc->flags & OMCI_ME_F_VENDOR) ?
		profile : OMCI_OLT_PROFILE_UNSPEC;
	object->data_len = desc ? desc->data_len :
		min_t(size_t, len, sizeof(object->data));
	if (data)
		memcpy(object->data, data, min(len, sizeof(object->data)));

	ret = omci_mib_store_locked(agent, object);
	kfree(object);
	return ret;
}

static int omci_mib_add_default_mask(struct omci_agent *agent, u16 class_id,
				     u16 entity_id, u16 attr_mask,
				     const void *data, size_t len)
{
	u8 profile = agent->profile_effective;

	if (!profile)
		profile = OMCI_OLT_PROFILE_GENERIC;

	return omci_mib_add_default_profile_mask(agent, profile, class_id,
					 entity_id, attr_mask, data, len);
}

static int omci_mib_add_default(struct omci_agent *agent, u16 class_id,
				u16 entity_id, const void *data, size_t len)
{
	return omci_mib_add_default_mask(agent, class_id, entity_id, 0xffff,
					 data, len);
}

static int
omci_agent_seed_huawei_locked(struct omci_device *odev, u8 profile)
{
	struct omci_agent *agent = &odev->agent;
	u8 data[OMCI_MAX_ATTR_DATA] = {};
	int ret;

	data[1] = agent->config.traffic_mgmt_option;
	ret = omci_mib_add_default_profile_mask(agent, profile,
			OMCI_CLASS_HUAWEI_FLOW_MAPPING, 0, GENMASK(15, 7),
			data, sizeof(data));
	if (ret)
		return ret;

	memset(data, 0, sizeof(data));
	memcpy(data, agent->config.version, min_t(size_t, 14,
						 sizeof(agent->config.version)));
	data[14] = 1;
	data[15] = 1;
	data[16] = 1;
	ret = omci_mib_add_default_profile_mask(agent, profile,
			OMCI_CLASS_HUAWEI_SW_IMAGE_EXT, 0, GENMASK(15, 11),
			data, sizeof(data));
	if (ret)
		return ret;

	memset(data, 0, sizeof(data));
	ret = omci_mib_add_default_profile_mask(agent, profile,
			OMCI_CLASS_HUAWEI_MULTICAST_367, 0, GENMASK(15, 11),
			data, sizeof(data));
	if (ret)
		return ret;
	ret = omci_mib_add_default_profile_mask(agent, profile,
			OMCI_CLASS_HUAWEI_MULTICAST_370, 0x0101,
			GENMASK(15, 11), data, sizeof(data));
	if (ret)
		return ret;
	ret = omci_mib_add_default_profile_mask(agent, profile,
			OMCI_CLASS_HUAWEI_MULTICAST_373, 0, GENMASK(15, 11),
			data, sizeof(data));
	if (ret)
		return ret;
	ret = omci_mib_add_default_profile_mask(agent, profile,
			OMCI_CLASS_HUAWEI_MULTICAST_65408, 0, GENMASK(15, 11),
			data, sizeof(data));
	if (ret)
		return ret;
	ret = omci_mib_add_default_profile_mask(agent, profile,
			OMCI_CLASS_HUAWEI_MULTICAST_65414, 0, GENMASK(15, 11),
			data, sizeof(data));
	if (ret)
		return ret;

	return omci_mib_add_default_profile_mask(agent, profile,
			OMCI_CLASS_HUAWEI_MULTICAST_65425, 0, GENMASK(15, 11),
			data, sizeof(data));
}

static u8 omci_agent_veip_slot(const struct omci_agent *agent);

static int
omci_agent_seed_nokia_locked(struct omci_device *odev, u8 profile)
{
	struct omci_agent *agent = &odev->agent;
	u8 data[OMCI_MAX_ATTR_DATA] = {};
	unsigned int i;
	int ret;

	data[0] = 1;
	data[3] = 1;
	data[6] = 1;
	data[9] = 1;
	data[12] = 1;
	ret = omci_mib_add_default_profile_mask(agent, profile,
			OMCI_CLASS_NOKIA_OPTICAL_SUPERVISION,
			OMCI_ANI_G_ENTITY_ID,
			GENMASK(15, 6), data, sizeof(data));
	if (ret)
		return ret;

	memset(data, 0, sizeof(data));
	memcpy(data + 15, agent->config.vendor_id,
	       sizeof(agent->config.vendor_id));
	memcpy(data + 19, agent->config.serial_number,
	       sizeof(agent->config.serial_number));
	ret = omci_mib_add_default_profile_mask(agent, profile,
			OMCI_CLASS_NOKIA_ONT_GENERIC_V2, 0,
			GENMASK(15, 4), data, sizeof(data));
	if (ret)
		return ret;

	for (i = 0; i < agent->config.uni_count; i++) {
		memset(data, 0, sizeof(data));
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_NOKIA_UNI_SUPPLEMENTAL_V2,
				OMCI_UNI_ENTITY_ID(OMCI_ETHERNET_SLOT,
						   i + 1),
				GENMASK(15, 9), data, sizeof(data));
		if (ret)
			return ret;
	}

	/*
	 * Some deployments use a reference service graph without OMCI Create.
	 * A provider must opt in; selecting an OLT profile alone does not grant
	 * Alloc-IDs, GEM ports or VLANs on other hardware.
	 */
	if (agent->config.onu_type == OMCI_ONU_TYPE_HGU &&
	    (agent->topology.flags & OMCI_TOPOLOGY_F_PROFILE_SERVICES)) {
		u8 veip_slot = omci_agent_veip_slot(agent);

		/* T-CONT 0x8000 -> Alloc-ID 256 */
		memset(data, 0, sizeof(data));
		put_unaligned_be16(256, data);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_TCONT, 0x8000,
				BIT(15), data, sizeof(data));
		if (ret)
			return ret;

		/* GEM Port Network CTP 256 -> port 256, T-CONT 0x8000, bidir(3) */
		memset(data, 0, sizeof(data));
		put_unaligned_be16(256, data);
		put_unaligned_be16(0x8000, data + 2);
		data[4] = 3;
		put_unaligned_be16(0x8000, data + 5);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_GEM_PORT_CTP, 256,
				GENMASK(15, 6), data, sizeof(data));
		if (ret)
			return ret;

		/*
		 * GEM IWTP 256 -> GEM CTP 256. Interworking option 5 is
		 * "802.1p mapper", so the service profile pointer at offset 3
		 * has to name a real class 130 instance; leaving it zero
		 * advertises mapper interworking with no mapper behind it.
		 */
		memset(data, 0, sizeof(data));
		put_unaligned_be16(256, data);
		data[2] = 5;
		put_unaligned_be16(OMCI_NOKIA_MAPPER_MGMT, data + 3);
		put_unaligned_be16(OMCI_UNI_ENTITY_ID(veip_slot, 1),
				   data + 5);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_GEM_IWTP, 256,
				GENMASK(15, 8), data, sizeof(data));
		if (ret)
			return ret;

		/* T-CONT 0x8001 -> Alloc-ID 257 */
		memset(data, 0, sizeof(data));
		put_unaligned_be16(257, data);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_TCONT, 0x8001,
				BIT(15), data, sizeof(data));
		if (ret)
			return ret;

		/* GEM Port Network CTP 257 -> port 257, T-CONT 0x8001, bidir(3) */
		memset(data, 0, sizeof(data));
		put_unaligned_be16(257, data);
		put_unaligned_be16(0x8001, data + 2);
		data[4] = 3;
		put_unaligned_be16(0x8001, data + 5);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_GEM_PORT_CTP, 257,
				GENMASK(15, 6), data, sizeof(data));
		if (ret)
			return ret;

		/* GEM IWTP 257 -> GEM CTP 257, via 802.1p mapper 2. */
		memset(data, 0, sizeof(data));
		put_unaligned_be16(257, data);
		data[2] = 5;
		put_unaligned_be16(OMCI_NOKIA_MAPPER_HSI, data + 3);
		put_unaligned_be16(OMCI_UNI_ENTITY_ID(veip_slot, 1),
				   data + 5);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_GEM_IWTP, 257,
				GENMASK(15, 8), data, sizeof(data));
		if (ret)
			return ret;

		/*
		 * 802.1p mappers, one per GEM interworking TP. A reference ONU
		 * carrying service on this OLT points every P-bit at the same
		 * interworking TP, so do the same rather than invent a
		 * priority split the OLT never asked for.
		 *
		 * Layout (G.988 9.3.10): TP pointer at 0, then the eight P-bit
		 * interworking TP pointers, unmarked frame option at 18, DSCP
		 * map at 19, default P-bit marking at 43, TP type at 44.
		 */
		for (i = 0; i < 2; i++) {
			u16 mapper = i ? OMCI_NOKIA_MAPPER_HSI :
					 OMCI_NOKIA_MAPPER_MGMT;
			u16 iwtp = i ? 257 : 256;
			unsigned int pbit;

			memset(data, 0, sizeof(data));
			put_unaligned_be16(0xffff, data);
			for (pbit = 0; pbit < 8; pbit++)
				put_unaligned_be16(iwtp, data + 2 + pbit * 2);
			ret = omci_mib_add_default_profile_mask(agent, profile,
					OMCI_CLASS_8021P_MAPPER, mapper,
					GENMASK(15, 3), data, sizeof(data));
			if (ret)
				return ret;
		}

		/* MAC Bridge Service Profile 1 */
		memset(data, 0, sizeof(data));
		data[0] = 1;
		data[1] = 1;
		data[2] = 1;
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_MAC_BRIDGE_SERVICE_PROFILE, 1,
				GENMASK(15, 6), data, sizeof(data));
		if (ret)
			return ret;

		/* MAC Bridge Port Config 1 -> VEIP (TP type 11) */
		memset(data, 0, sizeof(data));
		put_unaligned_be16(1, data);
		data[2] = 1;
		data[3] = 11;
		put_unaligned_be16(OMCI_UNI_ENTITY_ID(veip_slot, 1),
				   data + 4);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA, 1,
				GENMASK(15, 12), data, sizeof(data));
		if (ret)
			return ret;

		/*
		 * MAC Bridge Port 2 -> 802.1p mapper 2 (Internet / HSI).
		 * TP type 3 is "802.1p mapper"; binding the port straight to
		 * the GEM interworking TP with type 5 skips the mapper layer a
		 * working reference ONU on this OLT uses.
		 */
		memset(data, 0, sizeof(data));
		put_unaligned_be16(1, data);
		data[2] = 2;
		data[3] = 3;
		put_unaligned_be16(OMCI_NOKIA_MAPPER_HSI, data + 4);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA, 2,
				GENMASK(15, 12), data, sizeof(data));
		if (ret)
			return ret;

		/* MAC Bridge Port 3 -> 802.1p mapper 1 (management / VoIP). */
		memset(data, 0, sizeof(data));
		put_unaligned_be16(1, data);
		data[2] = 3;
		data[3] = 3;
		put_unaligned_be16(OMCI_NOKIA_MAPPER_MGMT, data + 4);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA, 3,
				GENMASK(15, 12), data, sizeof(data));
		if (ret)
			return ret;

		/*
		 * Class 84 (VLAN tagging filter data), one per mapper: VID 100
		 * for high speed internet and VID 660 for management and VoIP.
		 * data[24] is the forward operation, data[25] the number of
		 * valid entries in the filter list.
		 */
		memset(data, 0, sizeof(data));
		put_unaligned_be16(100, data);
		data[24] = 0x10;
		data[25] = 1;
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_VLAN_TAGGING_FILTER_DATA,
				OMCI_NOKIA_MAPPER_HSI,
				GENMASK(15, 13), data, sizeof(data));
		if (ret)
			return ret;

		memset(data, 0, sizeof(data));
		put_unaligned_be16(660, data);
		data[24] = 0x10;
		data[25] = 1;
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_VLAN_TAGGING_FILTER_DATA,
				OMCI_NOKIA_MAPPER_MGMT,
				GENMASK(15, 13), data, sizeof(data));
		if (ret)
			return ret;

		/*
		 * Class 171 (extended VLAN tagging operation configuration
		 * data) on the VEIP, association type 10 (VEIP).
		 * G.988 9.3.13 layout: association type at 0, table size at 1,
		 * input and output TPID at 3 and 5, downstream mode at 7 and
		 * the associated ME pointer at 24.
		 */
		memset(data, 0, sizeof(data));
		data[0] = 10;
		put_unaligned_be16(OMCI_EXT_VLAN_MAX_RULES, data + 1);
		put_unaligned_be16(ETH_P_8021Q, data + 3);
		put_unaligned_be16(ETH_P_8021Q, data + 5);
		data[7] = 0;
		put_unaligned_be16(OMCI_UNI_ENTITY_ID(veip_slot, 1),
				   data + 24);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_EXTENDED_VLAN,
				OMCI_UNI_ENTITY_ID(veip_slot, 1),
				GENMASK(15, 10), data, sizeof(data));
		if (ret)
			return ret;

		/* Class 131 (OLT-G): pre-seed the Nokia/ALCL OLT identity. */
		memset(data, 0, sizeof(data));
		memcpy(data, "ALCL", 4);
		memcpy(data + 4, "ISAM                ", 20);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_OLT_G, 0,
				GENMASK(15, 14), data, sizeof(data));
		if (ret)
			return ret;
	}

	return 0;
}

static int
omci_agent_seed_profile_locked(struct omci_device *odev, u8 profile)
{
	switch (profile) {
	case OMCI_OLT_PROFILE_HUAWEI:
		return omci_agent_seed_huawei_locked(odev, profile);
	case OMCI_OLT_PROFILE_NOKIA_ALCL:
		return omci_agent_seed_nokia_locked(odev, profile);
	default:
		return 0;
	}
}

static u8 omci_agent_veip_slot_for_profile(const struct omci_agent *agent, u8 profile);
static int omci_agent_move_veip_default(struct omci_device *odev, u8 slot);

static int
omci_agent_profile_reconcile_locked(struct omci_device *odev,
				    u8 old_profile, u8 new_profile)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	unsigned long index;
	int ret;

	if (old_profile == new_profile)
		return 0;

	xa_for_each(&agent->mib, index, object) {
		if (!object->owner_profile || object->owner_profile == new_profile)
			continue;
		xa_erase(&agent->mib, index);
		kfree(object);
	}

	ret = omci_agent_seed_profile_locked(odev, new_profile);
	if (ret)
		return ret;
	agent->profile_effective = new_profile;
	/*
	 * The profile's own MEs name its VEIP slot; without an OLT MIB Reset
	 * nothing else would move the ONU-created VEIP there.
	 */
	if (omci_agent_veip_slot_for_profile(agent, old_profile) !=
	    omci_agent_veip_slot_for_profile(agent, new_profile)) {
		ret = omci_agent_move_veip_default(odev,
				omci_agent_veip_slot_for_profile(agent, new_profile));
		if (ret) {
			agent->profile_effective = old_profile;
			return ret;
		}
	}
	ret = omci_agent_reconcile_services_locked(odev);
	agent->profile_effective = old_profile;
	if (ret && ret != -EOPNOTSUPP)
		return ret;

	return 0;
}

static int
omci_mib_add_priority_queue_default(struct omci_agent *agent,
				    u16 entity_id, const u8 *data)
{
	return omci_mib_add_default_mask(agent, OMCI_CLASS_PRIORITY_QUEUE,
					 entity_id,
					 OMCI_PRIORITY_QUEUE_ATTR_MASK, data,
					 OMCI_MAX_ATTR_DATA);
}

static int
omci_mib_add_traffic_scheduler_default(struct omci_agent *agent,
				       u16 entity_id, const u8 *data)
{
	return omci_mib_add_default_mask(agent, OMCI_CLASS_TRAFFIC_SCHEDULER,
					 entity_id,
					 OMCI_TRAFFIC_SCHEDULER_ATTR_MASK,
					 data, OMCI_MAX_ATTR_DATA);
}

static u16 omci_mib_count_class(struct omci_agent *agent, u16 class_id)
{
	struct omci_mib_object *object;
	unsigned long index;
	u16 count = 0;

	xa_for_each(&agent->mib, index, object) {
		if (object->class_id == class_id && count < U16_MAX)
			count++;
	}

	return count;
}

static void omci_agent_refresh_identity_locked(struct omci_agent *agent)
{
	struct omci_mib_object *object;
	unsigned long index;
	u16 queues, schedulers;

	xa_for_each(&agent->mib, index, object) {
		if (object->class_id == OMCI_CLASS_SOFTWARE_IMAGE &&
		    object->entity_id < ARRAY_SIZE(agent->config.software_version)) {
			memcpy(object->data,
			       agent->config.software_version[object->entity_id],
			       OMCI_SOFTWARE_VERSION_LEN);
		} else if (object->class_id == OMCI_CLASS_CARDHOLDER) {
			memcpy(object->data + 3, agent->config.equipment_id,
			       sizeof(agent->config.equipment_id));
			memcpy(object->data + 23, agent->config.equipment_id,
			       sizeof(agent->config.equipment_id));
		} else if (object->class_id == OMCI_CLASS_CIRCUIT_PACK) {
			memcpy(object->data + 2, agent->config.serial_number,
			       sizeof(agent->config.serial_number));
			memcpy(object->data + 10, agent->config.version,
			       sizeof(agent->config.version));
			memcpy(object->data + 24, agent->config.vendor_id,
			       sizeof(agent->config.vendor_id));
			memcpy(object->data + 31, agent->config.equipment_id,
			       sizeof(agent->config.equipment_id));
		}
	}

	object = omci_mib_lookup(agent, OMCI_CLASS_ONU_G, 0);
	if (object) {
		memset(object->data, 0, sizeof(object->data));
		memcpy(object->data, agent->config.vendor_id,
		       sizeof(agent->config.vendor_id));
		memcpy(object->data + 4, agent->config.version,
		       sizeof(agent->config.version));
		memcpy(object->data + 18, agent->config.serial_number,
		       sizeof(agent->config.serial_number));
		object->data[26] = agent->config.traffic_mgmt_option;
	}

	/*
	 * G.988 9.2.1: SR indication says the ONU reports queue status for
	 * DBA. Turning it off moves the OLT to traffic-monitoring DBA.
	 */
	object = omci_mib_lookup(agent, OMCI_CLASS_ANI_G, OMCI_ANI_G_ENTITY_ID);
	if (object)
		object->data[OMCI_ANI_G_SR_OFFSET] = agent->config.dba_status_reporting;

	object = omci_mib_lookup(agent, OMCI_CLASS_ONU2_G, 0);
	if (object) {
		memset(object->data, 0, sizeof(object->data));
		memcpy(object->data, agent->config.equipment_id,
		       min(sizeof(agent->config.equipment_id),
			   sizeof(object->data)));
		object->data[20] = agent->config.omcc_version;
		queues = omci_mib_count_class(agent, OMCI_CLASS_PRIORITY_QUEUE);
		put_unaligned_be16(queues, object->data + 25);
		schedulers = omci_mib_count_class(agent,
						  OMCI_CLASS_TRAFFIC_SCHEDULER);
		object->data[27] = min_t(u16, schedulers, U8_MAX);
		put_unaligned_be16(agent->topology.gem_port_count, object->data + 29);
		put_unaligned_be16(agent->topology.qos_configuration, object->data + 32);
		/*
		 * G.988 9.1.3. The OLT reads these to decide what it may
		 * provision. Left at zero they say the ONU supports no
		 * security and no connectivity model at all, and an OLT that
		 * honours that identifies the ONU and then has nothing it can
		 * legally create.
		 */
		object->data[23] = OMCI_ONU2G_SECURITY_AES128;
		object->data[24] = OMCI_ONU2G_SECURITY_AES128;
		put_unaligned_be16(OMCI_ONU2G_CONNECTIVITY_CAPABILITY,
				   object->data + 35);
		put_unaligned_be16(1, object->data + 40);
	}

	object = omci_mib_lookup(agent, OMCI_CLASS_HUAWEI_SW_IMAGE_EXT, 0);
	if (object)
		memcpy(object->data, agent->config.software_version[0],
		       min_t(size_t, 14, OMCI_SOFTWARE_VERSION_LEN));

	object = omci_mib_lookup(agent, OMCI_CLASS_NOKIA_ONT_GENERIC_V2, 0);
	if (object) {
		memcpy(object->data + 15, agent->config.vendor_id,
		       sizeof(agent->config.vendor_id));
		memcpy(object->data + 19, agent->config.serial_number,
		       sizeof(agent->config.serial_number));
	}
}

static void omci_agent_apply_identity(struct omci_agent *agent,
				      const struct omci_identity *identity)
{
	if (identity->valid & OMCI_IDENTITY_F_SERIAL_NUMBER) {
		memcpy(agent->config.serial_number, identity->serial_number,
		       sizeof(agent->config.serial_number));
		agent->config.serial_source = identity->serial_source;
	}
	if (identity->valid & OMCI_IDENTITY_F_VENDOR_ID) {
		memcpy(agent->config.vendor_id, identity->vendor_id,
		       sizeof(agent->config.vendor_id));
		agent->config.vendor_source = identity->vendor_source;
	}
	if (identity->valid & OMCI_IDENTITY_F_PASSWORD) {
		memcpy(agent->config.password, identity->password,
		       sizeof(agent->config.password));
		agent->config.password_source = identity->password_source;
	}
	if (identity->valid & OMCI_IDENTITY_F_VERSION) {
		memcpy(agent->config.version, identity->version,
		       sizeof(agent->config.version));
		memcpy(agent->config.software_version[0], identity->version,
		       sizeof(agent->config.software_version[0]));
		memcpy(agent->config.software_version[1], identity->version,
		       sizeof(agent->config.software_version[1]));
		agent->config.version_source = identity->version_source;
		agent->config.software_version_source[0] =
			identity->version_source;
		agent->config.software_version_source[1] =
			identity->version_source;
	}
	if (identity->valid & OMCI_IDENTITY_F_EQUIPMENT_ID) {
		memcpy(agent->config.equipment_id, identity->equipment_id,
		       sizeof(agent->config.equipment_id));
		agent->config.equipment_source = identity->equipment_source;
	}
}

static void
omci_agent_identity_state_locked(const struct omci_agent *agent,
				 struct omci_identity *identity)
{
	memset(identity, 0, sizeof(*identity));
	identity->valid = OMCI_IDENTITY_F_SERIAL_NUMBER |
			  OMCI_IDENTITY_F_VENDOR_ID |
			  OMCI_IDENTITY_F_PASSWORD |
			  OMCI_IDENTITY_F_VERSION |
			  OMCI_IDENTITY_F_EQUIPMENT_ID;
	memcpy(identity->serial_number, agent->config.serial_number,
	       sizeof(identity->serial_number));
	memcpy(identity->vendor_id, agent->config.vendor_id,
	       sizeof(identity->vendor_id));
	memcpy(identity->password, agent->config.password,
	       sizeof(identity->password));
	memcpy(identity->version, agent->config.version,
	       sizeof(identity->version));
	memcpy(identity->equipment_id, agent->config.equipment_id,
	       sizeof(identity->equipment_id));
	identity->serial_source = agent->config.serial_source;
	identity->vendor_source = agent->config.vendor_source;
	identity->password_source = agent->config.password_source;
	identity->version_source = agent->config.version_source;
	identity->equipment_source = agent->config.equipment_source;
}

static int omci_agent_get_ani_topology(struct omci_device *odev,
				       struct omci_ani_topology *topology)
{
	u32 last_tcont, last_scheduler, last_queue;
	int ret = 0;

	*topology = (struct omci_ani_topology) {
		.tcont_base = 0x8000,
		.scheduler_base = 0x8000,
		.queue_base = 0x8000,
		.maximum_queue_size = 0xffff,
		.allocated_queue_size = 4,
		.tcont_count = OMCI_DEFAULT_TCONT_COUNT,
		.queues_per_tcont = OMCI_DEFAULT_QUEUES_PER_TCONT,
		.queue_config_option = 1,
		.scheduler_policy = 1,
		.gem_port_count = 16,
		.downstream_queues_per_uni = 1,
	};

	if (odev->ops && odev->ops->get_ani_topology) {
		ret = odev->ops->get_ani_topology(odev, topology);
		if (ret)
			return ret;
	}

	if (!topology->tcont_count ||
	    topology->tcont_count > OMCI_MAX_TCONT_RESOURCES ||
	    !topology->queues_per_tcont ||
	    topology->queues_per_tcont > OMCI_MAX_QUEUES_PER_TCONT ||
	    !topology->downstream_queues_per_uni ||
	    topology->downstream_queues_per_uni > OMCI_MAX_QUEUES_PER_TCONT ||
	    !topology->gem_port_count || topology->gem_port_count > 4096 ||
	    topology->allocated_queue_size > topology->maximum_queue_size ||
	    (topology->scheduler_policy != 1 && topology->scheduler_policy != 2) ||
	    (topology->flags & ~(OMCI_TOPOLOGY_F_PROFILE_SERVICES |
			       OMCI_TOPOLOGY_F_FULL_UNI_ENTITY)))
		return -EINVAL;

	last_tcont = topology->tcont_base + topology->tcont_count - 1;
	last_scheduler = topology->scheduler_base + topology->tcont_count - 1;
	last_queue = topology->queue_base +
		     topology->tcont_count * topology->queues_per_tcont - 1;
	if (last_tcont > U16_MAX || last_scheduler > U16_MAX ||
	    last_queue > U16_MAX)
		return -ERANGE;

	odev->agent.topology = *topology;
	return 0;
}

static void
omci_priority_queue_default_data(const struct omci_ani_topology *topology,
				 unsigned int tcont, unsigned int queue,
				 u8 data[OMCI_MAX_ATTR_DATA])
{
	u16 tcont_entity = topology->tcont_base + tcont;
	u16 scheduler_entity = topology->scheduler_base + tcont;
	u16 priority = topology->queues_per_tcont - queue - 1;

	memset(data, 0, OMCI_MAX_ATTR_DATA);
	data[0] = topology->queue_config_option;
	put_unaligned_be16(topology->maximum_queue_size, data + 1);
	put_unaligned_be16(topology->allocated_queue_size, data + 3);
	put_unaligned_be16(tcont_entity, data + 9);
	put_unaligned_be16(priority, data + 11);
	put_unaligned_be16(scheduler_entity, data + 13);
	data[15] = 1;
	put_unaligned_be16(0xffff, data + 22);
}

/*
 * G.988 9.2.10: a downstream queue (ME ID MSB 0) relates to its UNI by slot
 * and port, priority 0 highest; strict priority is the downstream default.
 */
static void
omci_downstream_queue_default_data(const struct omci_ani_topology *topology,
				   u16 uni_entity, u16 priority,
				   u8 data[OMCI_MAX_ATTR_DATA])
{
	memset(data, 0, OMCI_MAX_ATTR_DATA);
	data[0] = topology->queue_config_option;
	put_unaligned_be16(topology->maximum_queue_size, data + 1);
	put_unaligned_be16(topology->allocated_queue_size, data + 3);
	put_unaligned_be16(uni_entity, data + 9);
	put_unaligned_be16(priority, data + 11);
	data[15] = 1;
	put_unaligned_be16(0xffff, data + 22);
}

static void
omci_traffic_scheduler_default_data(const struct omci_ani_topology *topology,
				    unsigned int tcont,
				    u8 data[OMCI_MAX_ATTR_DATA])
{
	memset(data, 0, OMCI_MAX_ATTR_DATA);
	put_unaligned_be16(topology->tcont_base + tcont, data);
	data[4] = topology->scheduler_policy;
}

static void
omci_cardholder_default_data(const struct omci_agent *agent, u8 unit_type,
			     u8 port_count, u8 data[OMCI_MAX_ATTR_DATA])
{
	memset(data, 0, OMCI_MAX_ATTR_DATA);
	data[0] = unit_type;
	data[1] = unit_type;
	data[2] = port_count;
	memcpy(data + 3, agent->config.equipment_id,
	       sizeof(agent->config.equipment_id));
	memcpy(data + 23, agent->config.equipment_id,
	       sizeof(agent->config.equipment_id));
}

static void
omci_circuit_pack_default_data(const struct omci_agent *agent,
			       const struct omci_ani_topology *topology,
			       u8 unit_type, u8 port_count, bool ani,
			       u8 data[OMCI_MAX_ATTR_DATA])
{
	u16 queues;

	memset(data, 0, OMCI_MAX_ATTR_DATA);
	data[0] = unit_type;
	data[1] = port_count;
	memcpy(data + 2, agent->config.serial_number,
	       sizeof(agent->config.serial_number));
	memcpy(data + 10, agent->config.version,
	       sizeof(agent->config.version));
	memcpy(data + 24, agent->config.vendor_id,
	       sizeof(agent->config.vendor_id));
	/* Administrative and operational states are enabled. */
	data[28] = 0;
	data[29] = 0;
	/* The default service model is bridged rather than routed. */
	data[30] = 0;
	memcpy(data + 31, agent->config.equipment_id,
	       sizeof(agent->config.equipment_id));
	if (!ani)
		return;

	queues = topology->tcont_count * topology->queues_per_tcont;
	data[52] = min_t(u16, topology->tcont_count, U8_MAX);
	data[53] = min_t(u16, queues, U8_MAX);
	data[54] = min_t(u16, topology->tcont_count, U8_MAX);
}

static int
omci_agent_add_equipment_default(struct omci_agent *agent,
				 const struct omci_ani_topology *topology,
				 u8 slot, u8 unit_type, u8 port_count,
				 bool ani, u8 data[OMCI_MAX_ATTR_DATA])
{
	u16 entity_id = OMCI_EQUIPMENT_ENTITY_ID(slot);
	int ret;

	omci_cardholder_default_data(agent, unit_type, port_count, data);
	ret = omci_mib_add_default_mask(agent, OMCI_CLASS_CARDHOLDER,
					entity_id, GENMASK(15, 10), data,
					OMCI_MAX_ATTR_DATA);
	if (ret)
		return ret;

	omci_circuit_pack_default_data(agent, topology, unit_type, port_count,
				       ani, data);
	return omci_mib_add_default_mask(agent, OMCI_CLASS_CIRCUIT_PACK,
					 entity_id, GENMASK(15, 2), data,
					 OMCI_MAX_ATTR_DATA);
}

/*
 * The slot is the OLT's provisioning choice, not the ONU vendor's: Nokia/ALCL
 * OLTs mostly plan the slot 6 VEIP of their own HGUs, some (Airtel India)
 * plan slot 3, and generic OLTs take the uploaded slot 10. A configured slot
 * wins, then the one the OLT was seen to provision.
 */
static u8 omci_agent_veip_slot_for_profile(const struct omci_agent *agent, u8 profile)
{
	if (agent->config.veip_slot)
		return agent->config.veip_slot;
	if (agent->veip_learned)
		return agent->veip_learned;
	if (profile == OMCI_OLT_PROFILE_NOKIA_ALCL)
		return OMCI_NOKIA_VEIP_SLOT;

	return OMCI_VEIP_SLOT;
}

static u8 omci_agent_veip_slot(const struct omci_agent *agent)
{
	return omci_agent_veip_slot_for_profile(agent, agent->profile_effective);
}

static void omci_mib_drop_default(struct omci_agent *agent, u16 class_id,
				  u16 entity_id)
{
	unsigned long key = omci_mib_key(class_id, entity_id);
	struct omci_mib_object *object = xa_load(&agent->mib, key);

	if (!object || object->origin != OMCI_MIB_ORIGIN_DEFAULT)
		return;
	xa_erase(&agent->mib, key);
	kfree(object);
}

/*
 * Report a single VEIP: drop the ONU-created VEIP, its UNI-G and its
 * equipment of a slot the current profile does not use, so a profile
 * change does not leave a second VEIP for the OLT to provision.
 */
static void omci_agent_drop_veip_default(struct omci_agent *agent, u8 slot)
{
	omci_mib_drop_default(agent, OMCI_CLASS_VEIP,
			      OMCI_UNI_ENTITY_ID(slot, 1));
	omci_mib_drop_default(agent, OMCI_CLASS_UNI_G,
			      OMCI_UNI_ENTITY_ID(slot, 1));
	omci_mib_drop_default(agent, OMCI_CLASS_CARDHOLDER,
			      OMCI_EQUIPMENT_ENTITY_ID(slot));
	omci_mib_drop_default(agent, OMCI_CLASS_CIRCUIT_PACK,
			      OMCI_EQUIPMENT_ENTITY_ID(slot));
}

static void omci_uni_g_default_data(u8 data[OMCI_MAX_ATTR_DATA])
{
	memset(data, 0, OMCI_MAX_ATTR_DATA);
	/* Support both OMCI and non-OMCI management. */
	data[3] = 2;
}

/* Slot of the VEIP the MIB holds, or 0. */
static u8 omci_agent_mib_veip_slot(struct omci_agent *agent)
{
	struct omci_mib_object *object;
	unsigned long index;

	xa_for_each(&agent->mib, index, object)
		if (object->class_id == OMCI_CLASS_VEIP)
			return object->entity_id >> 8;

	return 0;
}

/* Move the ONU-created VEIP, its UNI-G and its equipment to @slot. */
static int omci_agent_move_veip_default(struct omci_device *odev, u8 slot)
{
	struct omci_agent *agent = &odev->agent;
	u8 old_slot = omci_agent_mib_veip_slot(agent);
	struct omci_ani_topology topology;
	u8 data[OMCI_MAX_ATTR_DATA] = {};
	struct omci_mib_object *object;
	unsigned long index;
	int ret;

	if (old_slot == slot)
		return 0;
	ret = omci_agent_get_ani_topology(odev, &topology);
	if (ret)
		return ret;
	if (old_slot) {
		omci_agent_drop_veip_default(agent, old_slot);
		omci_mib_drop_default(agent, OMCI_CLASS_EXTENDED_VLAN,
				      OMCI_UNI_ENTITY_ID(old_slot, 1));
		/* Its downstream queues follow it. */
		xa_for_each(&agent->mib, index, object)
			if (object->class_id == OMCI_CLASS_PRIORITY_QUEUE &&
			    !(object->entity_id & BIT(15)) &&
			    get_unaligned_be16(object->data + 9) == OMCI_UNI_ENTITY_ID(old_slot, 1))
				put_unaligned_be16(OMCI_UNI_ENTITY_ID(slot, 1), object->data + 9);
	}
	ret = omci_agent_add_equipment_default(agent, &topology, slot,
					       OMCI_VEIP_UNIT_TYPE, 1, false,
					       data);
	if (ret)
		return ret;
	memset(data, 0, sizeof(data)); /* Administrative state unlocked. */
	ret = omci_mib_add_default(agent, OMCI_CLASS_VEIP,
				   OMCI_UNI_ENTITY_ID(slot, 1), data,
				   sizeof(data));
	if (ret)
		return ret;
	omci_uni_g_default_data(data);
	ret = omci_mib_add_default_mask(agent, OMCI_CLASS_UNI_G,
					OMCI_UNI_ENTITY_ID(slot, 1),
					GENMASK(15, 12), data, sizeof(data));
	if (ret)
		return ret;
	/* The Nokia seeds point at the VEIP. */
	if (agent->profile_effective == OMCI_OLT_PROFILE_NOKIA_ALCL)
		return omci_agent_seed_nokia_locked(odev, agent->profile_effective);

	return 0;
}

/*
 * A Create that ties a bridge port (TP type 11) or a class 171 rule
 * (association type 10) to a VEIP the MIB lacks names the slot the OLT was
 * provisioned for. Move the VEIP there before the Create is handled and keep
 * the slot, so later MIB uploads already report it. Create carries only the
 * set-by-create attributes: class 47 has the TP type and pointer at 3 and 4,
 * class 171 the association type and pointer at 0 and 1.
 */
static void omci_agent_learn_veip_slot(struct omci_device *odev, u16 class_id,
				       const u8 *content, size_t len)
{
	struct omci_agent *agent = &odev->agent;
	u16 pointer;
	u8 slot;

	if (agent->config.veip_slot)
		return;
	if (class_id == OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA && len >= 6 &&
	    content[3] == OMCI_BRIDGE_TP_VEIP)
		pointer = get_unaligned_be16(content + 4);
	else if (class_id == OMCI_CLASS_EXTENDED_VLAN && len >= 3 &&
		 content[0] == OMCI_EXT_VLAN_ASSOC_VEIP)
		pointer = get_unaligned_be16(content + 1);
	else
		return;

	slot = pointer >> 8;
	if ((pointer & 0xff) != 1 || !slot || slot == OMCI_ETHERNET_SLOT ||
	    slot >= OMCI_GPON_SLOT || omci_mib_lookup(agent, OMCI_CLASS_VEIP, pointer))
		return;
	if (!omci_agent_move_veip_default(odev, slot))
		agent->veip_learned = slot;
}

static int omci_agent_populate_defaults(struct omci_device *odev)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_ani_topology topology;
	u8 data[OMCI_MAX_ATTR_DATA] = {};
	unsigned int i, queue;
	u16 queue_id;
	u8 veip_slot;
	int ret;

	ret = omci_agent_get_ani_topology(odev, &topology);
	if (ret)
		return ret;

	ret = omci_mib_add_default_mask(agent, OMCI_CLASS_ONU_DATA, 0,
					BIT(15), data, 1);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(agent->config.software_version); i++) {
		memset(data, 0, sizeof(data));
		memcpy(data, agent->config.software_version[i],
		       OMCI_SOFTWARE_VERSION_LEN);
		data[14] = i == 0; /* Committed image. */
		data[15] = i == 0; /* Active image. */
		data[16] = 1;      /* Valid image. */
		ret = omci_mib_add_default_mask(agent, OMCI_CLASS_SOFTWARE_IMAGE,
						i, GENMASK(15, 12), data,
						sizeof(data));
		if (ret)
			return ret;
	}

	ret = omci_agent_add_equipment_default(agent, &topology,
					       OMCI_GPON_SLOT, OMCI_GPON_UNIT_TYPE,
					       1, true, data);
	if (ret)
		return ret;
	ret = omci_agent_add_equipment_default(agent, &topology,
					       OMCI_ETHERNET_SLOT,
					       OMCI_ETHERNET_UNIT_TYPE,
					       agent->config.uni_count, false, data);
	if (ret)
		return ret;
	veip_slot = omci_agent_veip_slot(agent);
	i = omci_agent_mib_veip_slot(agent);
	if (i && i != veip_slot)
		omci_agent_drop_veip_default(agent, i);
	ret = omci_agent_add_equipment_default(agent, &topology, veip_slot,
					       OMCI_VEIP_UNIT_TYPE, 1, false,
					       data);
	if (ret)
		return ret;

	ret = omci_mib_add_default(agent, OMCI_CLASS_ONU_G, 0, data,
				   sizeof(data));
	if (ret)
		return ret;
	ret = omci_mib_add_default(agent, OMCI_CLASS_ONU2_G, 0, data,
				   sizeof(data));
	if (ret)
		return ret;
	memset(data, 0, sizeof(data));
	put_unaligned_be16(topology.tcont_count,
			   data + OMCI_ANI_G_TCONT_COUNT_OFFSET);
	put_unaligned_be16(OMCI_ANI_G_DEFAULT_GEM_BLOCK,
			   data + OMCI_ANI_G_GEM_BLOCK_OFFSET);
	data[OMCI_ANI_G_SR_OFFSET] = agent->config.dba_status_reporting;
	data[OMCI_ANI_G_SF_OFFSET] = OMCI_ANI_G_DEFAULT_SF;
	data[OMCI_ANI_G_SD_OFFSET] = OMCI_ANI_G_DEFAULT_SD;
	data[OMCI_ANI_G_LOWER_RX_OFFSET] = OMCI_ANI_G_RX_POLICY;
	data[OMCI_ANI_G_UPPER_RX_OFFSET] = OMCI_ANI_G_RX_POLICY;
	put_unaligned_be16(OMCI_ANI_G_DEFAULT_RESPONSE_NS,
			   data + OMCI_ANI_G_RESPONSE_TIME_OFFSET);
	data[OMCI_ANI_G_LOWER_TX_OFFSET] = OMCI_ANI_G_TX_POLICY;
	data[OMCI_ANI_G_UPPER_TX_OFFSET] = OMCI_ANI_G_TX_POLICY;
	ret = omci_mib_add_default(agent, OMCI_CLASS_ANI_G,
				   OMCI_ANI_G_ENTITY_ID, data,
				   sizeof(data));
	if (ret)
		return ret;

	for (i = 0; i < agent->config.uni_count; i++) {
		u16 entity_id = OMCI_UNI_ENTITY_ID(OMCI_ETHERNET_SLOT, i + 1);

		/*
		 * G.988 9.5.1: the integrated port is always present, so
		 * the sensed type names it; bridged or IP ind starts at 2.
		 */
		memset(data, 0, sizeof(data));
		data[OMCI_PPTP_SENSED_TYPE_OFFSET] = OMCI_CARD_10_100_1000BASE_T;
		data[OMCI_PPTP_ADMIN_STATE_OFFSET] = 0; /* unlocked */
		data[OMCI_PPTP_BRIDGED_IP_OFFSET] = 2;
		ret = omci_mib_add_default(agent, OMCI_CLASS_PPTP_ETHERNET_UNI,
					   entity_id, data, sizeof(data));
		if (ret)
			return ret;

		omci_uni_g_default_data(data);
		ret = omci_mib_add_default_mask(agent, OMCI_CLASS_UNI_G,
						entity_id, GENMASK(15, 12),
						data, sizeof(data));
		if (ret)
			return ret;
	}

	memset(data, 0, sizeof(data));
	data[0] = 0; /* Administrative state unlocked. */
	ret = omci_mib_add_default(agent, OMCI_CLASS_VEIP,
				   OMCI_UNI_ENTITY_ID(veip_slot, 1), data,
				   sizeof(data));
	if (ret)
		return ret;
	omci_uni_g_default_data(data);
	ret = omci_mib_add_default_mask(agent, OMCI_CLASS_UNI_G,
					OMCI_UNI_ENTITY_ID(veip_slot, 1),
					GENMASK(15, 12), data, sizeof(data));
	if (ret)
		return ret;

	for (i = 0; i < topology.tcont_count; i++) {
		u16 scheduler_id = topology.scheduler_base + i;

		memset(data, 0, sizeof(data));
		put_unaligned_be16(0xffff, data);
		data[2] = 1;	/* G.988 9.2.2: the deprecated attribute is 1 */
		ret = omci_mib_add_default(agent, OMCI_CLASS_TCONT,
					   topology.tcont_base + i, data,
					   sizeof(data));
		if (ret)
			return ret;

		omci_traffic_scheduler_default_data(&topology, i, data);
		ret = omci_mib_add_traffic_scheduler_default(agent, scheduler_id, data);
		if (ret)
			return ret;

		for (queue = 0; queue < topology.queues_per_tcont; queue++) {
			u16 entity_id = topology.queue_base +
				i * topology.queues_per_tcont + queue;

			omci_priority_queue_default_data(&topology, i, queue, data);
			ret = omci_mib_add_priority_queue_default(agent, entity_id, data);
			if (ret)
				return ret;
		}
	}

	/* Downstream queues for every Ethernet UNI and the VEIP, as vendor ONUs have. */
	for (i = 0, queue_id = 0; i <= agent->config.uni_count; i++) {
		u16 uni = i < agent->config.uni_count ?
			  OMCI_UNI_ENTITY_ID(OMCI_ETHERNET_SLOT, i + 1) :
			  OMCI_UNI_ENTITY_ID(veip_slot, 1);

		for (queue = 0; queue < topology.downstream_queues_per_uni; queue++) {
			omci_downstream_queue_default_data(&topology, uni, queue, data);
			ret = omci_mib_add_priority_queue_default(agent, queue_id++, data);
			if (ret)
				return ret;
		}
	}

	omci_agent_refresh_identity_locked(agent);
	return 0;
}

void omci_agent_restage(struct omci_device *odev)
{
	struct omci_agent *agent = &odev->agent;

	mutex_lock(&agent->lock);
	if (agent->enabled && agent->operational)
		omci_agent_reconcile_services_locked(odev);
	mutex_unlock(&agent->lock);
}

#define OMCI_PM_INTERVAL	(15 * 60 * HZ)

/* FEC totals since boot: corrected bytes and code words, uncorrectable, total, FEC seconds */
static bool omci_agent_fec_read(struct omci_device *odev, u64 v[5])
{
	struct omci_telemetry t = {};

	if (!odev->ops->get_telemetry || odev->ops->get_telemetry(odev, &t) ||
	    !(t.valid & OMCI_TELEMETRY_F_FEC_COUNTERS))
		return false;
	v[0] = t.fec_corrected_bytes;
	v[1] = t.fec_corrected_codewords;
	v[2] = t.fec_uncorrectable_codewords;
	v[3] = t.fec_total_codewords;
	v[4] = t.fec_seconds;
	return true;
}

/* PM counters saturate at their maximum instead of wrapping (G.988). */
static void omci_agent_fec_pm_fill(u8 *data, const u64 *now, const u64 *base)
{
	unsigned int i;

	for (i = 0; i < 4; i++)
		put_unaligned_be32(min_t(u64, now[i] - base[i], U32_MAX),
				   data + 3 + i * 4);
	put_unaligned_be16(min_t(u64, now[4] - base[4], U16_MAX), data + 19);
}

/* Close the current interval: its counts become every FEC PM's history. */
static void omci_agent_pm_end_interval_locked(struct omci_device *odev,
					      u8 interval)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	unsigned long index;
	u64 now[5];

	if (!omci_agent_fec_read(odev, now))
		memcpy(now, agent->pm_base, sizeof(now));
	xa_for_each(&agent->mib, index, object) {
		if (object->class_id != OMCI_CLASS_FEC_PM)
			continue;
		object->data[0] = interval;
		omci_agent_fec_pm_fill(object->data, now, agent->pm_base);
	}
	memcpy(agent->pm_base, now, sizeof(now));
	agent->pm_interval = interval;
}

static void omci_agent_pm_work(struct work_struct *work)
{
	struct omci_agent *agent = container_of(to_delayed_work(work),
						struct omci_agent, pm_work);
	struct omci_device *odev = container_of(agent, struct omci_device,
						agent);

	mutex_lock(&agent->lock);
	if (agent->pm_running) {
		omci_agent_pm_end_interval_locked(odev, agent->pm_interval + 1);
		schedule_delayed_work(&agent->pm_work, OMCI_PM_INTERVAL);
	}
	mutex_unlock(&agent->lock);
}

/*
 * Synchronize Time starts interval 0 now. The history so far is cleared,
 * not closed, since the OLT starts counting from here.
 */
/*
 * G.988 A.2.33/A.3.33: with a time of day, the running interval ends at the
 * next quarter-hour; without one (or with an invalid one), it restarts.
 * There is no real-time clock to set, so the reply reports the tick only.
 */
static void omci_agent_sync_time_locked(struct omci_device *odev,
					const u8 *content, size_t len)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	unsigned long interval = OMCI_PM_INTERVAL;
	unsigned long index;

	if (len >= 7 && content[4] < 24 && content[5] < 60 && content[6] < 60)
		interval = (900 - (content[5] % 15 * 60 + content[6])) * HZ;

	if (!omci_agent_fec_read(odev, agent->pm_base))
		memset(agent->pm_base, 0, sizeof(agent->pm_base));
	xa_for_each(&agent->mib, index, object)
		if (object->class_id == OMCI_CLASS_FEC_PM)
			omci_agent_fec_pm_fill(object->data, agent->pm_base,
					       agent->pm_base);
	omci_agent_pm_end_interval_locked(odev, 0);
	agent->pm_running = true;
	mod_delayed_work(system_wq, &agent->pm_work, interval);
}

/* Get Current Data reads the running interval, not the last closed one. */
static u8 omci_agent_get_current_locked(struct omci_device *odev, u16 class_id,
					u16 entity_id, const u8 *content,
					size_t content_len, u8 *response,
					size_t capacity, size_t *response_len)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	u8 history[21];
	u64 now[5];
	u8 result;

	object = omci_mib_lookup(agent, class_id, entity_id);
	if (class_id != OMCI_CLASS_FEC_PM || !object || !agent->pm_running ||
	    !omci_agent_fec_read(odev, now))
		return omci_agent_get_locked(odev, class_id, entity_id, content,
					     content_len, response, capacity,
					     response_len);
	memcpy(history, object->data, sizeof(history));
	omci_agent_fec_pm_fill(object->data, now, agent->pm_base);
	result = omci_agent_get_locked(odev, class_id, entity_id, content,
				       content_len, response, capacity,
				       response_len);
	memcpy(object->data, history, sizeof(history));
	return result;
}

static void omci_agent_reboot_work(struct work_struct *work)
{
	/* The response has gone out; let init stop the services cleanly. */
	struct omci_agent *agent = container_of(to_delayed_work(work),
					       struct omci_agent, reboot_work);

	omci_native_reboot(container_of(agent, struct omci_device, agent));
}

/*
 * Board defaults: the ONU type and one PPTP UNI per omci-uni-ports entry,
 * from the PON MAC's node (odev->parent is the xpon class device).
 */
static void omci_agent_load_board(struct omci_device *odev)
{
	omci_native_load_board(odev);
}

int omci_agent_init(struct omci_device *odev)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_identity identity;
	int ret;

	mutex_init(&agent->lock);
	INIT_DELAYED_WORK(&agent->alarm_work, omci_agent_alarm_work);
	xa_init(&agent->mib);
	xa_init(&agent->services);
	xa_init(&agent->uni_attrs);
	agent->enabled = true;
	agent->permissive = false;
	agent->fake_omci = false;
	agent->reboot = true;
	agent->config.reboot_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->voice_calls = 0;
	INIT_DELAYED_WORK(&agent->reboot_work, omci_agent_reboot_work);
	INIT_DELAYED_WORK(&agent->pm_work, omci_agent_pm_work);
	agent->config.uni_count = 4;
	agent->config.onu_type = OMCI_ONU_TYPE_HGU;
	agent->config.onu_type_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.traffic_mgmt_option = 0;
	agent->config.dba_status_reporting = 1;
	agent->config.olt_profile = OMCI_OLT_PROFILE_AUTO;
	agent->config.olt_profile_force = OMCI_OLT_PROFILE_UNSPEC;
	agent->config.olt_profile_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.olt_profile_force_source = OMCI_CONFIG_SOURCE_DEFAULT;
	memcpy(agent->config.serial_number, "OPEN0001", 8);
	memcpy(agent->config.vendor_id, "OPEN", 4);
	strscpy(agent->config.version, "OpenWrt", sizeof(agent->config.version));
	strscpy(agent->config.software_version[0], "OpenWrt",
		sizeof(agent->config.software_version[0]));
	strscpy(agent->config.software_version[1], "OpenWrt",
		sizeof(agent->config.software_version[1]));
	agent->config.omcc_version = OMCI_OMCC_VERSION_G988_2010_BASELINE;
	strscpy(agent->config.equipment_id, "OpenWrt",
		sizeof(agent->config.equipment_id));
	agent->config.serial_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.vendor_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.password_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.version_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.software_version_source[0] = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.software_version_source[1] = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.omcc_version_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.equipment_source = OMCI_CONFIG_SOURCE_DEFAULT;

	ret = omci_identity_load(odev->parent, &identity);
	if (ret)
		return ret;
	omci_agent_apply_identity(agent, &identity);
	omci_agent_load_board(odev);

	mutex_lock(&agent->lock);
	ret = omci_agent_populate_defaults(odev);
	if (!ret)
		ret = omci_agent_profile_refresh_locked(odev, NULL, NULL);
	mutex_unlock(&agent->lock);
	return ret;
}

void omci_agent_cleanup(struct omci_device *odev)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	unsigned long index;

	cancel_delayed_work_sync(&agent->reboot_work);
	cancel_delayed_work_sync(&agent->alarm_work);
	cancel_delayed_work_sync(&agent->pm_work);
	mutex_lock(&agent->lock);
	agent->pm_running = false;
	omci_agent_reset_duplicate_locked(agent);
	omci_agent_reset_table_snapshot_locked(agent);
	omci_agent_clear_services_locked(odev);
	xa_for_each(&agent->mib, index, object) {
		xa_erase(&agent->mib, index);
		kfree(object);
	}
	mutex_unlock(&agent->lock);
	xa_for_each(&agent->uni_attrs, index, object)
		kfree(object);
	xa_destroy(&agent->uni_attrs);
	xa_destroy(&agent->services);
	xa_destroy(&agent->mib);
}

static bool omci_mib_object_active(const struct omci_agent *agent,
				   const struct omci_mib_object *object)
{
	if (object->pending_delete)
		return false;
	if (object->owner_profile &&
	    object->owner_profile != agent->profile_effective)
		return false;

	return omci_me_active(agent, object->class_id);
}

static void omci_agent_reset_table_snapshot_locked(struct omci_agent *agent)
{
	kfree(agent->table_snapshot);
	agent->table_snapshot = NULL;
	agent->table_snapshot_len = 0;
	agent->table_snapshot_jiffies = 0;
	agent->table_snapshot_class_id = 0;
	agent->table_snapshot_entity_id = 0;
	agent->table_snapshot_mask = 0;
}

static void
omci_set_table_snapshot_locked(struct omci_agent *agent, u16 class_id,
			       u16 entity_id, u16 mask, u8 *table,
			       size_t table_len)
{
	omci_agent_reset_table_snapshot_locked(agent);
	agent->table_snapshot = table;
	agent->table_snapshot_len = table_len;
	agent->table_snapshot_jiffies = jiffies;
	agent->table_snapshot_class_id = class_id;
	agent->table_snapshot_entity_id = entity_id;
	agent->table_snapshot_mask = mask;
}

static int omci_agent_build_me_type_table_locked(struct omci_device *odev,
						 u8 **table,
						 size_t *table_len)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	struct omci_me_class class;
	unsigned long *classes;
	unsigned long class_id;
	unsigned long index;
	u32 next = 0;
	size_t count;
	u8 *data;
	int ret;

	classes = bitmap_zalloc(U16_MAX + 1, GFP_KERNEL);
	if (!classes)
		return -ENOMEM;

	for (;;) {
		ret = omci_me_class_next(odev, next, &class, &next);
		if (ret)
			break;
		if (class.support >= OMCI_CLASS_SUPPORT_PARSED)
			set_bit(class.class_id, classes);
	}
	if (ret != -ENOENT)
		goto out_free_bitmap;

	xa_for_each(&agent->mib, index, object) {
		if (omci_mib_object_active(agent, object))
			set_bit(object->class_id, classes);
	}
	/* Stored bridge/GAL profiles are supported as partial shadow MEs. */
	set_bit(OMCI_CLASS_MAC_BRIDGE_SERVICE_PROFILE, classes);
	set_bit(OMCI_CLASS_MAC_BRIDGE_CONFIG_DATA, classes);
	set_bit(OMCI_CLASS_GAL_ETHERNET_PROFILE, classes);
	set_bit(OMCI_CLASS_OMCI, classes);

	count = bitmap_weight(classes, U16_MAX + 1);
	data = kmalloc_array(count, sizeof(u16), GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto out_free_bitmap;
	}

	count = 0;
	for_each_set_bit(class_id, classes, U16_MAX + 1) {
		put_unaligned_be16(class_id, data + count * sizeof(u16));
		count++;
	}
	*table = data;
	*table_len = count * sizeof(u16);
	ret = 0;

out_free_bitmap:
	bitmap_free(classes);
	return ret;
}

static int omci_agent_build_message_type_table(u8 **table, size_t *table_len)
{
	static const u8 message_types[] = {
		OMCI_MSG_TYPE_CREATE,
		OMCI_MSG_TYPE_DELETE,
		OMCI_MSG_TYPE_SET,
		OMCI_MSG_TYPE_GET,
		OMCI_MSG_TYPE_GET_ALL_ALARMS,
		OMCI_MSG_TYPE_GET_ALL_ALARMS_NEXT,
		OMCI_MSG_TYPE_MIB_UPLOAD,
		OMCI_MSG_TYPE_MIB_UPLOAD_NEXT,
		OMCI_MSG_TYPE_MIB_RESET,
		OMCI_MSG_TYPE_ALARM_NOTIFICATION,
		OMCI_MSG_TYPE_TEST,
		OMCI_MSG_TYPE_SYNC_TIME,
		OMCI_MSG_TYPE_REBOOT,
		OMCI_MSG_TYPE_GET_NEXT,
		OMCI_MSG_TYPE_TEST_RESULT,
		OMCI_MSG_TYPE_GET_CURRENT_DATA,
		OMCI_MSG_TYPE_SET_TABLE,
	};

	*table = kmemdup(message_types, sizeof(message_types), GFP_KERNEL);
	if (!*table)
		return -ENOMEM;
	*table_len = sizeof(message_types);
	return 0;
}

static u8 omci_agent_get_omci_table_locked(struct omci_device *odev,
					   u16 entity_id, u16 mask,
					   u8 *response_content,
					   size_t response_capacity,
					   size_t *response_len)
{
	struct omci_agent *agent = &odev->agent;
	u8 *table = NULL;
	size_t table_len = 0;
	int ret;

	if (entity_id || hweight16(mask) != 1 ||
	    !(mask & (OMCI_OMCI_ME_TYPE_TABLE_MASK |
		      OMCI_OMCI_MESSAGE_TYPE_TABLE_MASK)))
		return OMCI_RESULT_PARAMETER_ERROR;
	if (response_capacity < 7)
		return OMCI_RESULT_PARAMETER_ERROR;

	if (mask == OMCI_OMCI_ME_TYPE_TABLE_MASK)
		ret = omci_agent_build_me_type_table_locked(odev, &table,
							    &table_len);
	else
		ret = omci_agent_build_message_type_table(&table, &table_len);
	if (ret)
		return OMCI_RESULT_PROCESSING_ERROR;

	omci_set_table_snapshot_locked(agent, OMCI_CLASS_OMCI, entity_id,
				       mask, table, table_len);

	memset(response_content, 0, response_capacity);
	response_content[0] = OMCI_RESULT_SUCCESS;
	put_unaligned_be16(mask, response_content + 1);
	put_unaligned_be32(table_len, response_content + 3);
	*response_len = 7;
	return OMCI_RESULT_SUCCESS;
}

static bool
omci_agent_me_type_supported_locked(struct omci_device *odev, u16 class_id)
{
	u8 *table;
	size_t table_len;
	size_t offset;
	bool found = false;

	if (omci_agent_build_me_type_table_locked(odev, &table, &table_len))
		return false;
	for (offset = 0; offset + sizeof(u16) <= table_len;
	     offset += sizeof(u16)) {
		if (get_unaligned_be16(table + offset) == class_id) {
			found = true;
			break;
		}
	}
	kfree(table);

	return found;
}

static u8 omci_agent_me_support(const struct omci_me_desc *desc)
{
	if (!desc)
		return 3;

	switch (desc->support) {
	case OMCI_CLASS_SUPPORT_NATIVE:
	case OMCI_CLASS_SUPPORT_PROVISIONED:
		return 1;
	case OMCI_CLASS_SUPPORT_PARSED:
	case OMCI_CLASS_SUPPORT_SHADOW:
		return 3;
	default:
		return 2;
	}
}

static u8 omci_agent_me_access(const struct omci_me_desc *desc)
{
	bool onu_created = desc && (desc->flags & OMCI_ME_F_ONU_CREATED);
	bool olt_created = desc && (desc->actions & OMCI_ME_ACTION_CREATE);

	if (onu_created && olt_created)
		return 3;
	if (olt_created)
		return 2;
	if (onu_created)
		return 1;

	return 3;
}

static u32 omci_agent_me_actions(const struct omci_me_desc *desc,
				 u16 class_id)
{
	if (desc)
		return desc->actions;

	switch (class_id) {
	case OMCI_CLASS_MAC_BRIDGE_SERVICE_PROFILE:
	case OMCI_CLASS_MAC_BRIDGE_CONFIG_DATA:
	case OMCI_CLASS_GAL_ETHERNET_PROFILE:
		return OMCI_ME_ACTION_CREATE | OMCI_ME_ACTION_DELETE |
		       OMCI_ME_ACTION_GET | OMCI_ME_ACTION_SET;
	default:
		return OMCI_ME_ACTION_GET | OMCI_ME_ACTION_SET;
	}
}

static int
omci_build_attr_ids_locked(struct omci_device *odev, u16 class_id,
			   u8 **table, size_t *table_len)
{
	const struct omci_me_desc *desc = omci_me_lookup(&odev->agent, class_id);
	u8 *data;
	unsigned int i;
	int ret;

	if (!desc || !desc->num_attrs) {
		data = kmalloc(1, GFP_KERNEL);
		if (!data)
			return -ENOMEM;
		*table = data;
		*table_len = 0;
		return 0;
	}

	data = kmalloc_array(desc->num_attrs, sizeof(u16), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	for (i = 0; i < desc->num_attrs; i++) {
		u16 attr_id;

		ret = omci_me_attr_id(&odev->agent, class_id, i, &attr_id);
		if (ret) {
			kfree(data);
			return ret;
		}
		put_unaligned_be16(attr_id, data + i * sizeof(u16));
	}
	*table = data;
	*table_len = desc->num_attrs * sizeof(u16);
	return 0;
}

static int
omci_build_instances_locked(struct omci_device *odev, u16 class_id,
			    u8 **table, size_t *table_len)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	unsigned long index;
	unsigned int count = 0;
	u8 *data;

	if (class_id == OMCI_CLASS_MANAGED_ENTITY) {
		return omci_agent_build_me_type_table_locked(odev, table,
						     table_len);
	}
	if (class_id == OMCI_CLASS_ATTRIBUTE) {
		unsigned int attr_count = omci_me_attr_count(agent);
		unsigned int i;

		data = kmalloc_array(max(attr_count, 1U), sizeof(u16),
				     GFP_KERNEL);
		if (!data)
			return -ENOMEM;
		for (i = 0; i < attr_count; i++)
			put_unaligned_be16(i + 1, data + i * sizeof(u16));
		*table = data;
		*table_len = attr_count * sizeof(u16);
		return 0;
	}
	if (class_id == OMCI_CLASS_OMCI) {
		data = kmalloc(sizeof(u16), GFP_KERNEL);
		if (!data)
			return -ENOMEM;
		put_unaligned_be16(0, data);
		*table = data;
		*table_len = sizeof(u16);
		return 0;
	}

	xa_for_each(&agent->mib, index, object)
		if (object->class_id == class_id &&
		    omci_mib_object_active(agent, object))
			count++;
	data = kmalloc_array(max(count, 1U), sizeof(u16), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	count = 0;
	xa_for_each(&agent->mib, index, object) {
		if (object->class_id != class_id ||
		    !omci_mib_object_active(agent, object))
			continue;
		put_unaligned_be16(object->entity_id,
				   data + count * sizeof(u16));
		count++;
	}
	*table = data;
	*table_len = count * sizeof(u16);
	return 0;
}

static int omci_agent_empty_table(u8 **table, size_t *table_len)
{
	*table = kmalloc(1, GFP_KERNEL);
	if (!*table)
		return -ENOMEM;
	*table_len = 0;
	return 0;
}

static u8 omci_agent_get_managed_entity_locked(struct omci_device *odev,
					       u16 entity_id, u16 mask,
					       u8 *response_content,
					       size_t response_capacity,
					       size_t *response_len)
{
	struct omci_agent *agent = &odev->agent;
	const struct omci_me_desc *desc;
	size_t used = 3;
	u16 encoded = 0;
	u8 *table = NULL;
	size_t table_len = 0;
	unsigned int bit;
	int ret;

	if (!mask || mask & ~GENMASK(15, 8))
		return OMCI_RESULT_PARAMETER_ERROR;
	if (!omci_agent_me_type_supported_locked(odev, entity_id))
		return OMCI_RESULT_UNKNOWN_INSTANCE;
	desc = omci_me_lookup(agent, entity_id);

	if (mask & OMCI_MANAGED_ENTITY_TABLE_MASKS) {
		if (hweight16(mask) != 1 || response_capacity < 7)
			return OMCI_RESULT_PARAMETER_ERROR;
		switch (mask) {
		case OMCI_MANAGED_ENTITY_ATTR_TABLE_MASK:
			ret = omci_build_attr_ids_locked(odev, entity_id, &table,
							 &table_len);
			break;
		case OMCI_MANAGED_ENTITY_INSTANCES_MASK:
			ret = omci_build_instances_locked(odev, entity_id,
							  &table, &table_len);
			break;
		default:
			ret = omci_agent_empty_table(&table, &table_len);
			break;
		}
		if (ret)
			return OMCI_RESULT_PROCESSING_ERROR;
		omci_set_table_snapshot_locked(agent, OMCI_CLASS_MANAGED_ENTITY,
					       entity_id, mask, table, table_len);
		memset(response_content, 0, response_capacity);
		response_content[0] = OMCI_RESULT_SUCCESS;
		put_unaligned_be16(mask, response_content + 1);
		put_unaligned_be32(table_len, response_content + 3);
		*response_len = 7;
		return OMCI_RESULT_SUCCESS;
	}

	memset(response_content, 0, response_capacity);
	response_content[0] = OMCI_RESULT_SUCCESS;
	for (bit = 0; bit < 16; bit++) {
		u16 attr_mask = BIT(15 - bit);

		if (!(mask & attr_mask))
			continue;
		switch (attr_mask) {
		case OMCI_MANAGED_ENTITY_NAME_MASK: {
			const char *name = omci_me_class_name(entity_id);
			size_t len = strnlen(name, OMCI_CAPABILITY_NAME_LEN);

			if (used + OMCI_CAPABILITY_NAME_LEN > response_capacity)
				return OMCI_RESULT_PARAMETER_ERROR;
			memcpy(response_content + used, name, len);
			used += OMCI_CAPABILITY_NAME_LEN;
			break;
		}
		case OMCI_MANAGED_ENTITY_ACCESS_MASK:
			if (used + 1 > response_capacity)
				return OMCI_RESULT_PARAMETER_ERROR;
			response_content[used++] = omci_agent_me_access(desc);
			break;
		case OMCI_MANAGED_ENTITY_ACTIONS_MASK:
			if (used + 4 > response_capacity)
				return OMCI_RESULT_PARAMETER_ERROR;
			put_unaligned_be32(omci_agent_me_actions(desc, entity_id),
					   response_content + used);
			used += 4;
			break;
		case OMCI_MANAGED_ENTITY_SUPPORT_MASK:
			if (used + 1 > response_capacity)
				return OMCI_RESULT_PARAMETER_ERROR;
			response_content[used++] = omci_agent_me_support(desc);
			break;
		default:
			return OMCI_RESULT_PARAMETER_ERROR;
		}
		encoded |= attr_mask;
	}
	put_unaligned_be16(encoded, response_content + 1);
	*response_len = used;
	return OMCI_RESULT_SUCCESS;
}

static bool omci_agent_attr_is_table(u16 class_id, u16 mask)
{
	if (class_id == OMCI_CLASS_OMCI)
		return mask & (OMCI_OMCI_ME_TYPE_TABLE_MASK |
			       OMCI_OMCI_MESSAGE_TYPE_TABLE_MASK);
	if (class_id == OMCI_CLASS_MANAGED_ENTITY)
		return mask & OMCI_MANAGED_ENTITY_TABLE_MASKS;
	if (class_id == OMCI_CLASS_ATTRIBUTE)
		return mask & OMCI_ATTRIBUTE_CODE_POINTS_MASK;
	if (class_id == OMCI_CLASS_EXTENDED_VLAN)
		return mask == BIT(8);

	return false;
}

static u8 omci_agent_attr_format(u16 class_id,
				 const struct omci_attr_desc *attr)
{
	if (omci_agent_attr_is_table(class_id, attr->mask))
		return OMCI_ATTRIBUTE_FORMAT_TABLE;
	if (attr->len > 4)
		return OMCI_ATTRIBUTE_FORMAT_STRING;
	if (attr->len == 4 && attr->mask == GENMASK(15, 0))
		return OMCI_ATTRIBUTE_FORMAT_BIT_FIELD;

	return OMCI_ATTRIBUTE_FORMAT_UNSIGNED;
}

static u8 omci_agent_get_attribute_locked(struct omci_device *odev,
					  u16 entity_id, u16 mask,
					  u8 *response_content,
					  size_t response_capacity,
					  size_t *response_len)
{
	struct omci_agent *agent = &odev->agent;
	const struct omci_me_desc *desc;
	const struct omci_attr_desc *attr;
	unsigned int attr_index;
	size_t used = 3;
	u16 class_id;
	u16 encoded = 0;
	unsigned int bit;
	u32 upper;
	int ret;

	if (!mask || mask & ~GENMASK(15, 7))
		return OMCI_RESULT_PARAMETER_ERROR;
	ret = omci_me_attr_get(agent, entity_id, &class_id, &attr_index,
			       &desc, &attr);
	if (ret)
		return OMCI_RESULT_UNKNOWN_INSTANCE;

	if (mask & OMCI_ATTRIBUTE_TABLE_MASKS) {
		u8 *table;
		size_t table_len;

		if (hweight16(mask) != 1 || response_capacity < 7)
			return OMCI_RESULT_PARAMETER_ERROR;
		ret = omci_agent_empty_table(&table, &table_len);
		if (ret)
			return OMCI_RESULT_PROCESSING_ERROR;
		omci_set_table_snapshot_locked(agent, OMCI_CLASS_ATTRIBUTE,
					       entity_id, mask, table, table_len);
		memset(response_content, 0, response_capacity);
		response_content[0] = OMCI_RESULT_SUCCESS;
		put_unaligned_be16(mask, response_content + 1);
		put_unaligned_be32(table_len, response_content + 3);
		*response_len = 7;
		return OMCI_RESULT_SUCCESS;
	}

	memset(response_content, 0, response_capacity);
	response_content[0] = OMCI_RESULT_SUCCESS;
	for (bit = 0; bit < 16; bit++) {
		u16 attr_mask = BIT(15 - bit);

		if (!(mask & attr_mask))
			continue;
		switch (attr_mask) {
		case OMCI_ATTRIBUTE_NAME_MASK: {
			char name[OMCI_CAPABILITY_NAME_LEN + 1];

			if (used + OMCI_CAPABILITY_NAME_LEN > response_capacity)
				return OMCI_RESULT_PARAMETER_ERROR;
			snprintf(name, sizeof(name), "%u attribute %u",
				 class_id, attr_index + 1);
			memcpy(response_content + used, name,
			       strnlen(name, OMCI_CAPABILITY_NAME_LEN));
			used += OMCI_CAPABILITY_NAME_LEN;
			break;
		}
		case OMCI_ATTRIBUTE_SIZE_MASK:
			if (used + 2 > response_capacity)
				return OMCI_RESULT_PARAMETER_ERROR;
			put_unaligned_be16(attr->len, response_content + used);
			used += 2;
			break;
		case OMCI_ATTRIBUTE_ACCESS_MASK:
			if (used + 1 > response_capacity)
				return OMCI_RESULT_PARAMETER_ERROR;
			response_content[used++] = attr->access & 0x7;
			break;
		case OMCI_ATTRIBUTE_FORMAT_MASK:
			if (used + 1 > response_capacity)
				return OMCI_RESULT_PARAMETER_ERROR;
			response_content[used++] =
				omci_agent_attr_format(class_id, attr);
			break;
		case OMCI_ATTRIBUTE_LOWER_LIMIT_MASK:
		case OMCI_ATTRIBUTE_BIT_FIELD_MASK:
			if (used + 4 > response_capacity)
				return OMCI_RESULT_PARAMETER_ERROR;
			put_unaligned_be32(0, response_content + used);
			used += 4;
			break;
		case OMCI_ATTRIBUTE_UPPER_LIMIT_MASK:
			if (used + 4 > response_capacity)
				return OMCI_RESULT_PARAMETER_ERROR;
			upper = attr->len >= sizeof(u32) ? U32_MAX :
				GENMASK(attr->len * 8 - 1, 0);
			put_unaligned_be32(upper, response_content + used);
			used += 4;
			break;
		case OMCI_ATTRIBUTE_SUPPORT_MASK:
			if (used + 1 > response_capacity)
				return OMCI_RESULT_PARAMETER_ERROR;
			response_content[used++] = 3;
			break;
		default:
			return OMCI_RESULT_PARAMETER_ERROR;
		}
		encoded |= attr_mask;
	}
	put_unaligned_be16(encoded, response_content + 1);
	*response_len = used;
	return OMCI_RESULT_SUCCESS;
}

static u8 omci_agent_get_next_locked(struct omci_device *odev,
				     u16 class_id, u16 entity_id,
				     u8 device_id, const u8 *content,
				     size_t content_len, u8 *response_content,
				     size_t response_capacity,
				     size_t *response_len)
{
	struct omci_agent *agent = &odev->agent;
	size_t fragment_len;
	size_t copy_len;
	size_t offset;
	u16 sequence;
	u16 mask;

	if (content_len < 4 || response_capacity < 3)
		return OMCI_RESULT_PARAMETER_ERROR;
	if (!omci_me_active(agent, class_id))
		return OMCI_RESULT_UNKNOWN_ME;
	if (!omci_me_action_allowed(agent, class_id, OMCI_MSG_TYPE_GET_NEXT))
		return OMCI_RESULT_NOT_SUPPORTED;
	mask = get_unaligned_be16(content);
	sequence = get_unaligned_be16(content + 2);
	if (hweight16(mask) != 1)
		return OMCI_RESULT_PARAMETER_ERROR;
	if (!agent->table_snapshot ||
	    time_after(jiffies, agent->table_snapshot_jiffies +
		       OMCI_TABLE_SNAPSHOT_TIMEOUT) ||
	    agent->table_snapshot_class_id != class_id ||
	    agent->table_snapshot_entity_id != entity_id ||
	    agent->table_snapshot_mask != mask)
		return OMCI_RESULT_PARAMETER_ERROR;

	fragment_len = device_id == OMCI_BASELINE_DEV_ID ?
		OMCI_BASELINE_TABLE_FRAGMENT_LEN : response_capacity - 3;
	if (sequence > SIZE_MAX / fragment_len)
		return OMCI_RESULT_PARAMETER_ERROR;
	offset = (size_t)sequence * fragment_len;
	if (offset >= agent->table_snapshot_len)
		return OMCI_RESULT_PARAMETER_ERROR;

	copy_len = min(fragment_len, agent->table_snapshot_len - offset);
	memset(response_content, 0, response_capacity);
	response_content[0] = OMCI_RESULT_SUCCESS;
	put_unaligned_be16(mask, response_content + 1);
	memcpy(response_content + 3, agent->table_snapshot + offset, copy_len);
	*response_len = 3 + copy_len;
	agent->table_snapshot_jiffies = jiffies;
	return OMCI_RESULT_SUCCESS;
}

static bool omci_mib_object_uploadable(const struct omci_agent *agent,
				       const struct omci_mib_object *object)
{
	const struct omci_me_desc *desc;

	if (!omci_mib_object_active(agent, object))
		return false;
	desc = omci_me_lookup(agent, object->class_id);

	return !desc || !(desc->flags & OMCI_ME_F_NO_MIB_UPLOAD);
}

static unsigned int omci_mib_count_locked(struct omci_agent *agent)
{
	struct omci_mib_object *object;
	unsigned long index;
	unsigned int count = 0;

	xa_for_each(&agent->mib, index, object)
		if (omci_mib_object_uploadable(agent, object))
			count++;
	return count;
}

static void omci_agent_reset_olt_objects_locked(struct omci_agent *agent)
{
	struct omci_mib_object *object;
	unsigned long index;

	xa_for_each(&agent->mib, index, object) {
		if (object->origin != OMCI_MIB_ORIGIN_OLT)
			continue;
		xa_erase(&agent->mib, index);
		kfree(object);
	}
}

static int omci_agent_mib_reset_locked(struct omci_device *odev, bool all,
				       bool *profile_changed)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	unsigned long index;
	int reset_ret;
	int ret;

	omci_agent_reset_table_snapshot_locked(agent);
	reset_ret = omci_agent_clear_services_locked(odev);
	if (reset_ret) {
		omci_agent_reconcile_services_locked(odev);
		return reset_ret;
	}

	if (all) {
		xa_for_each(&agent->mib, index, object) {
			xa_erase(&agent->mib, index);
			kfree(object);
		}
	} else {
		omci_agent_reset_olt_objects_locked(agent);
	}

	/*
	 * A MIB reset restores ONU-created MEs to their initial values. Rebuild
	 * them as well as the selected profile's vendor MEs so repeated resets
	 * expose the same MIB and capability table as a fresh agent.
	 */
	ret = omci_agent_populate_defaults(odev);
	if (!ret)
		ret = omci_agent_seed_profile_locked(odev,
						     agent->profile_effective);
	if (!ret)
		ret = omci_agent_profile_refresh_locked(odev, NULL, profile_changed);

	omci_agent_set_mib_sync_locked(agent, 0);
	agent->upload_index = 0;
	agent->loid_status = 0;
	agent->alarm_sequence = 0;
	agent->alarm_snapshot_count = 0;
	omci_agent_reset_duplicate_locked(agent);

	/*
	 * omci_agent_clear_services_locked() above tore down every applied
	 * service. The MIB objects it left in place (or just rebuilt) may
	 * still describe a complete datapath - e.g. a profile's default-
	 * seeded VEIP bridge, which a MIB Reset never touches since it only
	 * erases OLT-origin objects. Without this, a MIB Reset (which every
	 * OLT sends as a normal part of post-activation provisioning)
	 * leaves the ONU in O5 with OMCI up and no datapath, indefinitely,
	 * until some unrelated event happens to trigger reconcile again.
	 */
	if (!ret)
		omci_agent_reconcile_services_locked(odev);

	return ret ?: reset_ret;
}

static int omci_agent_gem_rate_locked(struct omci_device *odev,
				      const struct omci_mib_object *gem, bool remove)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *td;
	u32 rate[2] = {}, burst[2] = {};
	u16 ptr;
	unsigned int i;
	int ret;

	if (!odev->ops->set_gem_rate)
		return 0;
	for (i = 0; i < 2 && !remove; i++) {
		ptr = get_unaligned_be16(gem->data + (i ? 13 : 7));
		if (!ptr)
			continue;
		td = omci_mib_lookup(agent, OMCI_CLASS_TRAFFIC_DESCRIPTOR, ptr);
		if (!td || !omci_mib_object_active(agent, td))
			return -ENOENT;
		/* The decoded vendor path is a peak limiter, not a colour meter. */
		if (get_unaligned_be32(td->data) || get_unaligned_be32(td->data + 8) ||
		    td->data[16] || td->data[17] || td->data[18] || td->data[19])
			return -EOPNOTSUPP;
		rate[i] = get_unaligned_be32(td->data + 4);
		burst[i] = get_unaligned_be32(td->data + 12);
	}
	for (i = 0; i < 2; i++) {
		ret = odev->ops->set_gem_rate(odev, get_unaligned_be16(gem->data),
					     !i, rate[i], burst[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static u16 omci_agent_backend_uni(const struct omci_agent *agent, u16 entity)
{
	if (agent->topology.flags & OMCI_TOPOLOGY_F_FULL_UNI_ENTITY)
		return entity;
	return omci_profile_normalize_uni_entity(agent->profile_effective, entity);
}

static int omci_agent_hw_update(struct omci_device *odev,
				struct omci_mib_object *object,
				u8 action, const u8 *content)
{
	const struct omci_device_ops *ops = odev->ops;
	const struct omci_me_desc *desc;
	bool ignore_unsupported_uni;
	bool enable;
	u16 entity_id;
	u16 value;
	int ret;

	desc = omci_me_lookup(&odev->agent, object->class_id);
	if (action != OMCI_MSG_TYPE_DELETE && desc &&
	    (desc->flags & OMCI_ME_F_DATAPATH) &&
	    desc->support == OMCI_CLASS_SUPPORT_SHADOW)
		return -EOPNOTSUPP;

	switch (object->class_id) {
	case OMCI_CLASS_TCONT:
		if (!ops->set_tcont)
			return 0;
		if (action == OMCI_MSG_TYPE_DELETE)
			return ops->set_tcont(odev, object->entity_id, 0xffff,
					      false);
		value = get_unaligned_be16(object->data);
		return ops->set_tcont(odev, object->entity_id, value, true);
	case OMCI_CLASS_GEM_PORT_CTP:
		if (!ops->set_gem_port)
			return 0;
		ret = omci_agent_gem_rate_locked(odev, object, action == OMCI_MSG_TYPE_DELETE);
		if (ret)
			return ret;
		return ops->set_gem_port(odev, object->entity_id,
					 get_unaligned_be16(object->data),
					 get_unaligned_be16(object->data + 2),
					 object->data[4],
					 action != OMCI_MSG_TYPE_DELETE, false);
	case OMCI_CLASS_PPTP_ETHERNET_UNI:
	case OMCI_CLASS_VEIP:
		if (!ops->set_uni)
			return 0;
		if (action == OMCI_MSG_TYPE_DELETE)
			enable = false;
		else if (object->class_id == OMCI_CLASS_PPTP_ETHERNET_UNI)
			enable = object->data[4] == 0;
		else
			enable = object->data[0] == 0;
		entity_id = omci_agent_backend_uni(&odev->agent, object->entity_id);
		ret = ops->set_uni(odev, entity_id, enable);
		ignore_unsupported_uni = odev->agent.profile_quirks &
					 OMCI_OLT_QUIRK_IGNORE_UNSUPPORTED_UNI;
		if (ret == -EOPNOTSUPP && ignore_unsupported_uni)
			return 0;
		return ret;
	default:
		return 0;
	}
}

static bool
omci_agent_datapath_class(const struct omci_agent *agent, u16 class_id)
{
	const struct omci_me_desc *desc = omci_me_lookup(agent, class_id);

	return class_id == OMCI_CLASS_PRIORITY_QUEUE ||
	       class_id == OMCI_CLASS_TRAFFIC_SCHEDULER ||
	       (desc && (desc->flags & OMCI_ME_F_DATAPATH));
}

static void omci_agent_free_service_array(struct xarray *services)
{
	struct omci_service_state *state;
	unsigned long index;

	xa_for_each(services, index, state) {
		xa_erase(services, index);
		kfree(state);
	}
}

static int omci_agent_clear_services_locked(struct omci_device *odev)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_service_state *state;
	struct omci_mib_object *object;
	const struct omci_tcont_qos defaults = {};
	unsigned long index;
	int first_error = 0;
	int ret;

	xa_for_each(&agent->services, index, state) {
		if (odev->ops->delete_service) {
			ret = odev->ops->delete_service(odev, state->config.cookie);
			if (ret && ret != -ENOENT && !first_error)
				first_error = ret;
		}
		xa_erase(&agent->services, index);
		kfree(state);
	}
	xa_for_each(&agent->mib, index, object) {
		if (object->class_id == OMCI_CLASS_GEM_PORT_CTP) {
			ret = omci_agent_gem_rate_locked(odev, object, true);
			first_error = first_error ?: ret;
		} else if (object->class_id == OMCI_CLASS_TCONT && odev->ops->set_tcont_qos) {
			ret = odev->ops->set_tcont_qos(odev, object->entity_id, &defaults);
			first_error = first_error ?: ret;
		}
	}

	return first_error;
}

static int omci_agent_stage_service(struct xarray *services,
				    const struct omci_service_config *service)
{
	struct omci_service_state *state;
	void *old;

	state = kmalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;
	state->config = *service;

	old = xa_store(services, service->cookie, state, GFP_KERNEL);
	if (xa_is_err(old)) {
		kfree(state);
		return xa_err(old);
	}
	kfree(old);
	return 0;
}

static int omci_agent_apply_services_locked(struct omci_device *odev,
					    struct xarray *desired)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_service_state *state;
	struct omci_service_state *old_state;
	unsigned long index;
	int ret = 0;

	if (!xa_empty(desired) && !odev->ops->replace_service)
		return -EOPNOTSUPP;

	/* Reserve all new cookies before changing hardware state. */
	xa_for_each(desired, index, state) {
		if (xa_load(&agent->services, index))
			continue;
		ret = xa_reserve(&agent->services, index, GFP_KERNEL);
		if (ret)
			goto release_reservations;
	}

	/* Add or replace all desired rules while the previous set remains live. */
	xa_for_each(desired, index, state) {
		ret = odev->ops->replace_service(odev, &state->config);
		if (ret == -EAGAIN) {
			/*
			 * The service's T-CONT names an Alloc-ID the OLT has not
			 * assigned this ONU yet. That is no error: leave it out
			 * of the hardware until the PLOAM assignment reconciles
			 * the services again.
			 */
			if (odev->ops->delete_service)
				odev->ops->delete_service(odev,
							  state->config.cookie);
			xa_erase(desired, index);
			kfree(state);
			if (!xa_load(&agent->services, index))
				xa_release(&agent->services, index);
			ret = 0;
			continue;
		}
		if (ret) {
			dev_warn(odev->parent,
				 "OMCI install failed for service %#x (UNI %#x GEM %u T-CONT %#x queue %u): %d; rolling back\n",
				 state->config.cookie,
				 state->config.uni_entity_id,
				 state->config.gem_port_id,
				 state->config.tcont_entity_id,
				 state->config.queue, ret);
			goto rollback_hardware;
		}
	}

	/* Delete rules that are no longer part of the resolved service graph. */
	xa_for_each(&agent->services, index, old_state) {
		if (xa_load(desired, index))
			continue;
		if (!odev->ops->delete_service)
			continue;
		ret = odev->ops->delete_service(odev, old_state->config.cookie);
		if (ret && ret != -ENOENT)
			goto rollback_hardware;
	}

	/* Hardware is consistent; replace the software snapshot. */
	omci_agent_free_service_array(&agent->services);
	xa_for_each(desired, index, state) {
		void *entry;

		xa_erase(desired, index);
		entry = xa_store(&agent->services, index, state, GFP_KERNEL);
		if (WARN_ON(xa_is_err(entry))) {
			kfree(state);
			continue;
		}
		kfree(entry);
	}
	return 0;

rollback_hardware:
	/* Restore every old rule, including rules deleted before a failure. */
	if (odev->ops->replace_service)
		xa_for_each(&agent->services, index, old_state)
			odev->ops->replace_service(odev, &old_state->config);

	/* Remove newly introduced cookies that were not in the old snapshot. */
	if (odev->ops->delete_service)
		xa_for_each(desired, index, state)
			if (!xa_load(&agent->services, index))
				odev->ops->delete_service(odev,
						  state->config.cookie);

release_reservations:
	xa_for_each(desired, index, state)
		if (!xa_load(&agent->services, index))
			xa_release(&agent->services, index);
	return ret;
}

static bool omci_agent_uni_enabled(struct omci_agent *agent, u8 tp_type,
				   u16 entity_id)
{
	struct omci_mib_object *object;

	if (tp_type == OMCI_BRIDGE_TP_PPTP_ETH_UNI) {
		object = omci_mib_lookup(agent, OMCI_CLASS_PPTP_ETHERNET_UNI,
					 entity_id);
		return object && object->data[4] == 0;
	}
	if (tp_type == OMCI_BRIDGE_TP_VEIP) {
		object = omci_mib_lookup(agent, OMCI_CLASS_VEIP, entity_id);
		return object && object->data[0] == 0;
	}

	return false;
}

static u32 omci_agent_service_cookie(u16 lan_port, u16 gem_ctp,
				     u16 gem_port, u16 selector)
{
	u32 cookie;

	cookie = jhash_3words(((u32)lan_port << 16) | gem_ctp,
			      ((u32)gem_port << 16) | selector,
			      0x4f4d4349, 0);

	return cookie ?: 1;
}

static bool omci_agent_service_vlan_valid(u16 vid)
{
	return vid > 0 && vid < VLAN_VID_MASK;
}

static bool omci_agent_has_olt_gem_ctp_locked(struct omci_agent *agent)
{
	struct omci_mib_object *object;
	unsigned long index;

	xa_for_each(&agent->mib, index, object) {
		if (object->class_id == OMCI_CLASS_GEM_PORT_CTP &&
		    object->origin == OMCI_MIB_ORIGIN_OLT)
			return true;
	}
	return false;
}

/*
 * G.988 9.2.3: with priority-controlled upstream traffic management (ONU-G
 * option 0), a GEM port's upstream traffic management pointer names the
 * priority queue it uses; the queue's related port carries the T-CONT and
 * the priority, 0 highest. Off by default: on Airtel (Nokia OLT) the P-bit
 * queues of the earlier builds upload faster.
 */
static bool upstream_queue_tm;


/*
 * Services whose GEM ports share a priority queue share a hardware queue,
 * so an 802.1p mapper sending every P-bit to one GEM needs one flow.
 * Priority 0 stays on queue 0, where vendor firmware puts all traffic;
 * queue 7 dropped an ONU to O2 on an Airtel (Nokia) OLT.
 */
static int omci_agent_tm_queue_locked(struct omci_agent *agent,
				      const struct omci_mib_object *gem, u8 *queue)
{
	const struct omci_mib_object *pq;
	u16 priority;

	if (agent->config.traffic_mgmt_option == 1 || gem->data_len < 7)
		return -ENOENT;
	pq = omci_mib_lookup(agent, OMCI_CLASS_PRIORITY_QUEUE,
			     get_unaligned_be16(gem->data + 5));
	if (!pq || pq->data_len < 13)
		return -ENOENT;
	priority = get_unaligned_be16(pq->data + 11);
	if (priority >= 8)
		return -EINVAL;
	*queue = priority;
	return 0;
}

static bool omci_agent_gem_qos_locked(struct omci_agent *agent,
				      const struct omci_mib_object *gem)
{
	struct omci_mib_object *object;
	unsigned long index;
	u16 tcont = get_unaligned_be16(gem->data + 2);

	xa_for_each(&agent->mib, index, object) {
		if (!omci_mib_object_active(agent, object) || !object->qos_configured)
			continue;
		if ((object->class_id == OMCI_CLASS_PRIORITY_QUEUE &&
		     get_unaligned_be16(object->data + 9) == tcont) ||
		    (object->class_id == OMCI_CLASS_TRAFFIC_SCHEDULER &&
		     get_unaligned_be16(object->data) == tcont) ||
		    (object->class_id == OMCI_CLASS_TCONT && object->entity_id == tcont))
			return true;
	}
	return false;
}

/*
 * A VLAN tagging filter on the ANI-side bridge port limits which VLANs reach
 * that port's GEMs (Airtel: VLAN 100 on one mapper, 660 on the other), so a
 * UNI rule must not pair a VLAN with a mapper whose port would drop it. An
 * untagged frame does not pass a VID filter either.
 */
static bool
omci_agent_vlan_filter_passes(const struct omci_vlan_tagging_filter *filter,
			      bool vlan_valid, u16 vid)
{
	unsigned int i;

	if (!filter)
		return true;
	if (!vlan_valid)
		return false;
	for (i = 0; i < filter->num_entries; i++)
		if (filter->entries[i].vid == vid)
			return true;
	return false;
}

static int
omci_agent_stage_service_rule_locked(struct omci_device *odev,
				     struct xarray *services,
				     u16 lan_port_entity,
				     u16 uni_entity, u16 gem_iwtp_entity,
				     u8 mapper_pcp, bool mapper_pcp_valid,
				     const struct omci_vlan_filter_entry *filter,
				     const struct omci_extended_vlan_rule *ext_rule,
				     u16 selector, bool *default_installed,
				     bool multicast, u16 ani_entity_id,
				     bool ani_valid,
				     const struct omci_vlan_tagging_filter *ani_filter)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *iwtp;
	struct omci_mib_object *gem;
	struct omci_mib_object *tcont;
	struct omci_service_config service = {};
	u16 gem_ctp_entity;

	/*
	 * A multicast GEM IWTP bridge port points at class 281, whose entity
	 * IDs are independent of the unicast GEM IWTPs of class 266.  Looking
	 * it up as class 266 picks whatever unicast IWTP shares the ID -- on a
	 * Movistar (AR) OLT the internet GEM, flagged multicast -- and the
	 * backend refusal of that bogus rule rolls back the whole service set.
	 * Both classes start with the GEM port network CTP pointer.
	 */
	iwtp = omci_mib_lookup(agent, multicast ? OMCI_CLASS_MULTICAST_GEM_IWTP :
					       OMCI_CLASS_GEM_IWTP,
			       gem_iwtp_entity);
	if (!iwtp)
		return -ENOENT;
	gem_ctp_entity = get_unaligned_be16(iwtp->data);
	gem = omci_mib_lookup(agent, OMCI_CLASS_GEM_PORT_CTP, gem_ctp_entity);
	if (!gem)
		return -ENOENT;
	if (gem->data[4] == OMCI_GEM_PORT_DIRECTION_ANI_TO_UNI)
		return 0;
	/*
	 * Skip the profile reference-seed GEMs (256/257, origin DEFAULT) when
	 * the OLT provisions its own data GEMs (247/248, origin OLT) and assigns
	 * the real Alloc-ID over PLOAM.  The seeded T-CONT 0x8000/0x8001
	 * (Alloc-ID 256/257) otherwise become phantom hardware channels that
	 * displace the real Alloc-ID's T-CONT, so GEM 247 binds to the wrong
	 * (seed) channel.  The seed managed entities stay in the MIB, so an OLT
	 * that really uses GEM 256/257 without OMCI Create (no origin-OLT GEM
	 * CTP) is unaffected.
	 */
	if (gem->origin == OMCI_MIB_ORIGIN_DEFAULT &&
	    omci_agent_has_olt_gem_ctp_locked(agent))
		return 0;

	service.uni_entity_id = omci_agent_backend_uni(agent, uni_entity);
	service.gem_ctp_entity_id = gem_ctp_entity;
	service.gem_port_id = get_unaligned_be16(gem->data);
	service.tcont_entity_id = get_unaligned_be16(gem->data + 2);
	tcont = omci_mib_lookup(agent, OMCI_CLASS_TCONT,
				 service.tcont_entity_id);
	if (!tcont || get_unaligned_be16(tcont->data) == 0xffff)
		return -ENOENT;
	/* Carry the Alloc-ID: the backend may need it to rebuild the channel. */
	service.alloc_id = get_unaligned_be16(tcont->data);
	service.direction = gem->data[4];
	service.multicast = multicast;
	service.multicast_ani_entity_id = ani_entity_id;
	service.multicast_ani_valid = ani_valid;
	if (mapper_pcp_valid) {
		service.pcp = mapper_pcp;
		service.pcp_valid = true;
		service.queue = mapper_pcp;
	}
	if (filter) {
		service.vlan_id = filter->vid;
		service.vlan_valid =
			omci_agent_service_vlan_valid(filter->vid);
		/* VID-only filtering on the ANI port leaves the P-bit alone. */
		if (!mapper_pcp_valid && !ani_filter) {
			service.pcp = filter->pbit;
			service.pcp_valid = true;
			service.queue = filter->pbit;
		}
	}
	if (ext_rule) {
		struct omci_extended_vlan_rule rule = *ext_rule;
		bool network_vlan = false;
		u8 priority;

		omci_profile_normalize_vlan_rule(agent->profile_effective, &rule);
		/* The mapper sees the priority after the VLAN operation. */
		priority = rule.treat_outer_pbit != 15 ?
			   rule.treat_outer_pbit : rule.treat_inner_pbit;
		if (priority == 8)
			priority = rule.filter_inner_pbit;
		else if (priority == 9)
			priority = rule.filter_outer_pbit;
		if (mapper_pcp_valid && priority <= 7 && priority != mapper_pcp)
			return -EOPNOTSUPP;
		memcpy(service.vlan_treatment, rule.raw + 8,
		       sizeof(service.vlan_treatment));
		omci_agent_service_uni_vlan(&rule, &service);

		/*
		 * pon0 exposes the ANI/network side of the service.  Class 171
		 * therefore selects the VLAN after the OMCI treatment, not the
		 * customer-side filter VLAN.  Userspace creates that network VLAN
		 * directly (for example pon0.1894), so the backend only needs the
		 * resulting classifier and does not need to rewrite the skb.
		 */
		if (omci_agent_service_vlan_valid(rule.treat_inner_vid)) {
			service.vlan_id = rule.treat_inner_vid;
			network_vlan = true;
		} else if (omci_agent_service_vlan_valid(rule.treat_outer_vid)) {
			service.vlan_id = rule.treat_outer_vid;
			network_vlan = true;
		} else if (omci_agent_service_vlan_valid(rule.filter_inner_vid)) {
			service.vlan_id = rule.filter_inner_vid;
			network_vlan = true;
		} else if (omci_agent_service_vlan_valid(rule.filter_outer_vid)) {
			service.vlan_id = rule.filter_outer_vid;
			network_vlan = true;
		}
		service.vlan_valid = network_vlan;

		/*
		 * Behind an 802.1p mapper the P-bit that picks this GEM is the
		 * mapper's (Jio: 0 internet, 4 VoIP, ...). The rule's P-bit
		 * filter must not replace it, or every GEM of the VLAN is keyed
		 * on the rule's P-bit and upstream lands on whichever comes first.
		 */
		if (mapper_pcp_valid) {
		} else if (rule.filter_inner_pbit <= 7) {
			service.pcp = rule.filter_inner_pbit;
			service.pcp_valid = true;
			service.queue = rule.filter_inner_pbit;
		} else if (rule.filter_outer_pbit <= 7) {
			service.pcp = rule.filter_outer_pbit;
			service.pcp_valid = true;
			service.queue = rule.filter_outer_pbit;
		}

		/*
		 * Preparatory Class 171 rules can describe only a customer-side
		 * rewrite and still carry no host-facing VLAN.  They are valid OMCI
		 * state, but they must not make the whole mapper Set fail.
		 */
		if (!network_vlan)
			return -EOPNOTSUPP;
	}
	if (!omci_agent_vlan_filter_passes(ani_filter, service.vlan_valid,
					   service.vlan_id))
		return -EOPNOTSUPP;
	if (upstream_queue_tm || omci_agent_gem_qos_locked(agent, gem)) {
		int ret = omci_agent_tm_queue_locked(agent, gem, &service.queue);

		if (ret && ret != -ENOENT)
			return ret;
	} else if (agent->topology.queues_per_tcont == 1) {
		/* PCP still selects the service; this provider has one TX queue. */
		service.queue = 0;
	}
	if (!service.vlan_valid && !service.pcp_valid && !*default_installed) {
		service.default_service = true;
		*default_installed = true;
	}
	service.cookie = omci_agent_service_cookie(lan_port_entity,
			gem_ctp_entity, service.gem_port_id, selector);

	/*
	 * Trace every rule the resolver wants, not only the ones the backend
	 * accepts, so an install failure can be attributed to a specific rule.
	 */
	dev_dbg(odev->parent,
		"OMCI stage service %#x: UNI %#x (raw %#x) LAN port %#x GEM %u T-CONT %#x queue %u PCP %s%u VLAN %s%u selector %u%s%s\n",
		service.cookie, service.uni_entity_id, uni_entity,
		lan_port_entity, service.gem_port_id, service.tcont_entity_id,
		service.queue, service.pcp_valid ? "" : "any/", service.pcp,
		service.vlan_valid ? "" : "any/", service.vlan_id, selector,
		service.multicast ? " multicast" : "",
		service.default_service ? " default" : "");

	return omci_agent_stage_service(services, &service);
}

static bool
omci_agent_vlan_associated(const struct omci_mib_object *object,
			   u16 lan_port_entity, u16 uni_entity,
			   u16 bridge_port_entity, u16 service_entity)
{
	if (object->class_id == OMCI_CLASS_VLAN_TAGGING_FILTER_DATA)
		return object->entity_id == lan_port_entity ||
		       object->entity_id == uni_entity ||
		       object->entity_id == bridge_port_entity ||
		       object->entity_id == service_entity;
	if (object->class_id == OMCI_CLASS_EXTENDED_VLAN)
		return object->extended_vlan.valid &&
		       (object->extended_vlan.associated_me == lan_port_entity ||
			object->extended_vlan.associated_me == uni_entity ||
			object->extended_vlan.associated_me == bridge_port_entity ||
			object->extended_vlan.associated_me == service_entity ||
			object->entity_id == lan_port_entity ||
			object->entity_id == bridge_port_entity ||
			object->entity_id == service_entity);

	return false;
}

static int
omci_agent_stage_path_locked(struct omci_device *odev,
			     struct xarray *services,
			     const struct omci_mib_object *lan_port,
			     u16 uni_entity, u16 bridge_port_entity,
			     u16 service_entity, u16 gem_iwtp_entity,
			     u8 pcp, bool pcp_valid, bool multicast,
			     u16 ani_entity_id, bool ani_valid,
			     bool *default_installed)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	unsigned long index;
	u16 selector = pcp_valid ? 0x100 | pcp : 0;
	const struct omci_vlan_tagging_filter *ani_filter = NULL;
	bool installed = false;
	bool has_vlan_rules = false;
	int ret;

	/*
	 * A UNI-side VLAN table shared by several ANI ports must not bypass any
	 * one port's VID filter (forward operation 0x10, the one a VID list
	 * forwards on). Other operations leave the port unfiltered.
	 */
	object = omci_mib_lookup(agent, OMCI_CLASS_VLAN_TAGGING_FILTER_DATA,
				 bridge_port_entity);
	if (object && omci_mib_object_active(agent, object) &&
	    object->vlan_filter.valid && object->vlan_filter.num_entries &&
	    object->vlan_filter.forward_operation == 0x10)
		ani_filter = &object->vlan_filter;

	xa_for_each(&agent->mib, index, object) {
		unsigned int i;

		if (!omci_mib_object_active(agent, object) ||
		    !omci_agent_vlan_associated(object, lan_port->entity_id,
						 uni_entity, bridge_port_entity,
						 service_entity))
			continue;
		if (object->class_id == OMCI_CLASS_VLAN_TAGGING_FILTER_DATA &&
		    object->vlan_filter.valid) {
			has_vlan_rules |= object->vlan_filter.num_entries != 0;
			for (i = 0; i < object->vlan_filter.num_entries; i++) {
				ret = omci_agent_stage_service_rule_locked(
					odev, services, lan_port->entity_id,
					uni_entity, gem_iwtp_entity, pcp, pcp_valid,
					&object->vlan_filter.entries[i], NULL,
					selector + i + 1, default_installed,
					multicast, ani_entity_id, ani_valid,
					ani_filter);
				if (ret == -EOPNOTSUPP)
					continue;
				if (ret)
					return ret;
				installed = true;
			}
		} else if (object->class_id == OMCI_CLASS_EXTENDED_VLAN &&
			   object->extended_vlan.valid) {
			has_vlan_rules |= object->extended_vlan.rule_count != 0;
			for (i = 0; i < object->extended_vlan.rule_count; i++) {
				ret = omci_agent_stage_service_rule_locked(
					odev, services, lan_port->entity_id,
					uni_entity, gem_iwtp_entity, pcp, pcp_valid,
					NULL, &object->extended_vlan.rules[i],
					selector + i + 0x20, default_installed,
					multicast, ani_entity_id, ani_valid,
					ani_filter);
				if (ret == -EOPNOTSUPP)
					continue;
				if (ret)
					return ret;
				installed = true;
			}
		}
	}

	/* A filtered path carries only what its filter allows: no wildcard. */
	if (installed || has_vlan_rules || ani_filter)
		return 0;

	return omci_agent_stage_service_rule_locked(
		odev, services, lan_port->entity_id, uni_entity, gem_iwtp_entity,
		pcp, pcp_valid, NULL, NULL, selector, default_installed,
		multicast, ani_entity_id, ani_valid, ani_filter);
}

static int
omci_agent_resolve_bridge_locked(struct omci_device *odev,
				 struct xarray *services,
				 const struct omci_mib_object *lan_port,
				 bool *default_installed)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *wan_port;
	struct omci_mib_object *mapper;
	unsigned long index;
	u16 bridge_id = get_unaligned_be16(lan_port->data);
	u16 uni_entity = get_unaligned_be16(lan_port->data + 4);
	u8 lan_type = lan_port->data[3];
	int ret;

	if (!omci_agent_uni_enabled(agent, lan_type, uni_entity))
		return 0;

	xa_for_each(&agent->mib, index, wan_port) {
		u16 mapper_entity;
		u16 gem_iwtp_entity;
		u8 wan_type;
		unsigned int pcp;

		if (wan_port->class_id !=
		    OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA ||
		    wan_port == lan_port ||
		    get_unaligned_be16(wan_port->data) != bridge_id)
			continue;
		wan_type = wan_port->data[3];
		if (wan_type == OMCI_BRIDGE_TP_GEM_IWTP ||
		    wan_type == OMCI_BRIDGE_TP_MULTICAST_GEM_IWTP) {
			bool multicast =
				wan_type == OMCI_BRIDGE_TP_MULTICAST_GEM_IWTP;
			bool ani_valid = false;
			u16 ani_entity_id = 0;

			gem_iwtp_entity = get_unaligned_be16(wan_port->data + 4);
			if (multicast &&
			    !omci_profile_resolve_multicast_ani(
				agent->profile_effective, wan_port->entity_id,
				&ani_entity_id))
				ani_valid = true;
			ret = omci_agent_stage_path_locked(
				odev, services, lan_port, uni_entity,
				wan_port->entity_id, gem_iwtp_entity,
				gem_iwtp_entity, 0, false, multicast,
				ani_entity_id, ani_valid, default_installed);
			if (ret && ret != -ENOENT)
				return ret;
			continue;
		}
		if (wan_type != OMCI_BRIDGE_TP_8021P_MAPPER)
			continue;
		mapper_entity = get_unaligned_be16(wan_port->data + 4);
		mapper = omci_mib_lookup(agent, OMCI_CLASS_8021P_MAPPER,
					 mapper_entity);
		if (!mapper)
			continue;
		for (pcp = 0; pcp < 8; pcp++) {
			gem_iwtp_entity = get_unaligned_be16(mapper->data + 2 +
							 pcp * 2);
			if (!gem_iwtp_entity || gem_iwtp_entity == 0xffff)
				continue;
			ret = omci_agent_stage_path_locked(
				odev, services, lan_port, uni_entity,
				wan_port->entity_id, mapper_entity,
				gem_iwtp_entity, pcp, true, false, 0, false,
				default_installed);
			if (ret && ret != -ENOENT)
				return ret;
		}
	}

	return 0;
}

/* G.988 9.2.3 traffic descriptor: PIR (bytes/s) in kbit/s, CIR when PIR is 0. */
static u32 omci_agent_td_kbps(struct omci_agent *agent, u16 entity_id)
{
	struct omci_mib_object *td;
	u32 rate;

	if (!entity_id)
		return 0;
	td = omci_mib_lookup(agent, OMCI_CLASS_TRAFFIC_DESCRIPTOR, entity_id);
	if (!td || !omci_mib_object_active(agent, td) || td->data_len < 8)
		return 0;
	rate = get_unaligned_be32(td->data + 4) ?: get_unaligned_be32(td->data);
	return DIV_ROUND_UP_ULL(rate, 125);
}

/*
 * G.988 attributes of the Ethernet UNI behind each UNI bridge port: pause
 * time (ME 11 attribute 10), ageing time and MAC learning depth (ME 45
 * attributes 10 and 9, overridden by ME 47 attribute 13) and the bridge
 * port's outbound/inbound traffic descriptors (ME 47 attributes 11 and
 * 12). Handed to the device only when they change; when the OLT removes
 * the bridge port, the defaults (all zero) go back.
 */
static int omci_agent_apply_uni_attrs_locked(struct omci_device *odev)
{
	struct omci_agent *agent = &odev->agent;
	const struct omci_device_ops *ops = odev->ops;
	struct omci_uni_attrs attrs, *cached;
	struct omci_mib_object *port, *pptp, *bsp;
	unsigned long index, port_index;
	u16 entity;
	int ret;

	if (!ops->set_uni_attrs)
		return 0;

	/* A VEIP is a shared service endpoint, not a physical Ethernet port. */
	xa_for_each(&agent->mib, index, pptp) {
		if (!omci_mib_object_active(agent, pptp) ||
		    pptp->class_id != OMCI_CLASS_PPTP_ETHERNET_UNI)
			continue;
		memset(&attrs, 0, sizeof(attrs));
		if (pptp->data_len >= 12) {
			attrs.auto_detection = pptp->data[2];
			attrs.max_frame_size = get_unaligned_be16(pptp->data + 7);
			attrs.pause_quanta = get_unaligned_be16(pptp->data + 10);
		}
		xa_for_each(&agent->mib, port_index, port) {
			if (!omci_mib_object_active(agent, port) ||
			    port->class_id != OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA ||
			    port->data_len < 6 ||
			    port->data[3] != OMCI_BRIDGE_TP_PPTP_ETH_UNI ||
			    get_unaligned_be16(port->data + 4) != pptp->entity_id)
				continue;
			bsp = omci_mib_lookup(agent, OMCI_CLASS_MAC_BRIDGE_SERVICE_PROFILE,
					      get_unaligned_be16(port->data));
			if (bsp && omci_mib_object_active(agent, bsp) && bsp->data_len >= 17) {
				attrs.learn_limit = bsp->data[12];
				attrs.ageing_s = get_unaligned_be32(bsp->data + 13);
			}
			if (port->data_len >= 24 && port->data[23])
				attrs.learn_limit = port->data[23];
			if (port->data_len >= 23) {
				attrs.down_kbps = omci_agent_td_kbps(agent,
					get_unaligned_be16(port->data + 19));
				attrs.up_kbps = omci_agent_td_kbps(agent,
					get_unaligned_be16(port->data + 21));
			}
			break;
		}
		entity = omci_agent_backend_uni(agent, pptp->entity_id);
		cached = xa_load(&agent->uni_attrs, entity);
		if (cached && !memcmp(cached, &attrs, sizeof(attrs)))
			continue;
		if (!cached) {
			cached = kzalloc(sizeof(*cached), GFP_KERNEL);
			if (!cached)
				return -ENOMEM;
			ret = xa_err(xa_store(&agent->uni_attrs, entity, cached, GFP_KERNEL));
			if (ret) {
				kfree(cached);
				return ret;
			}
			/* An unsuccessful first write must be retried, including defaults. */
			ret = ops->set_uni_attrs(odev, entity, &attrs);
			if (ret) {
				xa_erase(&agent->uni_attrs, entity);
				kfree(cached);
				return ret;
			}
		} else {
			ret = ops->set_uni_attrs(odev, entity, &attrs);
			if (ret)
				return ret;
		}
		*cached = attrs;
	}

	/* Clear the original physical port when its PPTP disappears or moves. */
	xa_for_each(&agent->uni_attrs, index, cached) {
		bool present = false;

		xa_for_each(&agent->mib, port_index, pptp) {
			if (pptp->class_id == OMCI_CLASS_PPTP_ETHERNET_UNI &&
			    omci_mib_object_active(agent, pptp) &&
			    omci_agent_backend_uni(agent, pptp->entity_id) == index) {
				present = true;
				break;
			}
		}
		if (present)
			continue;
		memset(&attrs, 0, sizeof(attrs));
		ret = ops->set_uni_attrs(odev, index, &attrs);
		if (ret)
			return ret;
		xa_erase(&agent->uni_attrs, index);
		kfree(cached);
	}
	return 0;
}

static int omci_agent_tcont_qos_locked(struct omci_device *odev,
				       const struct omci_mib_object *tcont)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_tcont_qos qos = {};
	struct omci_mib_object *pq, *ts;
	unsigned long index;
	u8 seen = 0, weight = 0, policy;
	u16 priority, ptr, root = 0;
	bool root_set = false;

	if (!odev->ops->set_tcont_qos)
		return 0;
	qos.enabled = tcont->qos_configured;
	xa_for_each(&agent->mib, index, ts) {
		if (omci_mib_object_active(agent, ts) &&
		    ts->class_id == OMCI_CLASS_TRAFFIC_SCHEDULER &&
		    get_unaligned_be16(ts->data) == tcont->entity_id)
			qos.enabled |= ts->qos_configured;
	}
	xa_for_each(&agent->mib, index, pq) {
		if (omci_mib_object_active(agent, pq) &&
		    pq->class_id == OMCI_CLASS_PRIORITY_QUEUE &&
		    get_unaligned_be16(pq->data + 9) == tcont->entity_id) {
			qos.enabled |= pq->qos_configured;
			ptr = get_unaligned_be16(pq->data + 13);
			ts = ptr ? omci_mib_lookup(agent, OMCI_CLASS_TRAFFIC_SCHEDULER, ptr) : NULL;
			if (ts && omci_mib_object_active(agent, ts))
				qos.enabled |= ts->qos_configured;
		}
	}
	if (!qos.enabled || tcont->pending_delete)
		goto apply;
	xa_for_each(&agent->mib, index, pq) {
		if (!omci_mib_object_active(agent, pq) ||
		    pq->class_id != OMCI_CLASS_PRIORITY_QUEUE ||
		    get_unaligned_be16(pq->data + 9) != tcont->entity_id)
			continue;
		priority = get_unaligned_be16(pq->data + 11);
		if (priority >= 8 || (seen & BIT(priority)))
			return -EINVAL;
		seen |= BIT(priority);
		ptr = get_unaligned_be16(pq->data + 13);
		if (root_set && root != ptr)
			return -EOPNOTSUPP;
		root = ptr;
		root_set = true;
		ts = ptr ? omci_mib_lookup(agent, OMCI_CLASS_TRAFFIC_SCHEDULER, ptr) : NULL;
		if (ptr && (!ts || !omci_mib_object_active(agent, ts)))
			return -ENOENT;
		/* The hardware has one scheduling stage per Alloc-ID. */
		if (ts && (get_unaligned_be16(ts->data) != tcont->entity_id ||
			   get_unaligned_be16(ts->data + 2)))
			return -EOPNOTSUPP;
		policy = ts ? ts->data[4] : tcont->data[3];
		if (policy != 1 && policy != 2)
			return -EOPNOTSUPP;
		if (!qos.policy)
			qos.policy = policy;
		if (qos.policy != policy)
			return -EOPNOTSUPP;
		qos.weight[priority] = pq->data[15];
		weight |= pq->data[15];
	}
	if (!seen || (qos.policy != 1 && qos.policy != 2))
		return -EOPNOTSUPP;
	if (qos.policy == 2 && !weight)
		return -EINVAL;
apply:
	if (tcont->pending_delete)
		qos.enabled = false;
	return odev->ops->set_tcont_qos(odev, tcont->entity_id, &qos);
}

/* PLOAM grant changes must also replay CTPs without an active bridge service. */
static int omci_agent_reconcile_resources_locked(struct omci_device *odev)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	unsigned long index;
	u16 classes[] = { OMCI_CLASS_TCONT, OMCI_CLASS_GEM_PORT_CTP };
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(classes); i++) {
		xa_for_each(&agent->mib, index, object) {
			if (object->class_id != classes[i] ||
			    (!omci_mib_object_active(agent, object) && !object->pending_delete))
				continue;
			/* Default GEM seeds are installed only by resolved services. */
			if (classes[i] == OMCI_CLASS_GEM_PORT_CTP &&
			    object->origin == OMCI_MIB_ORIGIN_DEFAULT) {
				if (!omci_agent_has_olt_gem_ctp_locked(agent)) {
					ret = omci_agent_gem_rate_locked(odev, object,
									 object->pending_delete);
					if (ret)
						return ret;
				}
				continue;
			}
			ret = omci_agent_hw_update(odev, object, object->pending_delete ?
						   OMCI_MSG_TYPE_DELETE : OMCI_MSG_TYPE_SET,
						   object->data);
			if (ret)
				return ret;
			if (classes[i] == OMCI_CLASS_TCONT) {
				ret = omci_agent_tcont_qos_locked(odev, object);
				if (ret)
					return ret;
			}
		}
	}
	return 0;
}

static int omci_agent_reconcile_services_locked(struct omci_device *odev)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	struct xarray desired;
	unsigned long index;
	bool default_installed = false;
	int ret;

	ret = omci_agent_reconcile_resources_locked(odev);
	if (ret)
		return ret;
	xa_init(&desired);
	xa_for_each(&agent->mib, index, object) {
		u8 tp_type;

		if (!omci_mib_object_active(agent, object) ||
		    object->class_id != OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA)
			continue;
		tp_type = object->data[3];
		if (tp_type != OMCI_BRIDGE_TP_PPTP_ETH_UNI &&
		    tp_type != OMCI_BRIDGE_TP_VEIP)
			continue;
		ret = omci_agent_resolve_bridge_locked(
			odev, &desired, object, &default_installed);
		if (ret)
			goto out;
	}

	ret = omci_agent_apply_services_locked(odev, &desired);
	if (!ret)
		ret = omci_agent_apply_uni_attrs_locked(odev);

out:
	omci_agent_free_service_array(&desired);
	xa_destroy(&desired);
	return ret;
}

static struct omci_mib_object *
omci_get_or_create_locked(struct omci_agent *agent, u16 class_id,
			  u16 entity_id, bool create)
{
	struct omci_mib_object *object;
	struct omci_mib_object *stored;
	int ret;

	stored = omci_mib_lookup(agent, class_id, entity_id);
	if (stored || !create)
		return stored;

	object = kzalloc(sizeof(*object), GFP_KERNEL);
	if (!object)
		return NULL;
	object->class_id = class_id;
	object->entity_id = entity_id;
	object->origin = OMCI_MIB_ORIGIN_OLT;

	ret = omci_mib_store_locked(agent, object);
	kfree(object);
	if (ret)
		return NULL;

	return omci_mib_lookup(agent, class_id, entity_id);
}

static u8 omci_agent_create_locked(struct omci_device *odev,
				   u16 class_id, u16 entity_id,
				   const u8 *content, size_t content_len)
{
	struct omci_agent *agent = &odev->agent;
	const struct omci_me_desc *desc;
	struct omci_mib_object *stored = NULL;
	struct omci_mib_object *object;
	u8 result = OMCI_RESULT_SUCCESS;
	bool hardware_applied = false;
	int ret;

	if (!omci_me_active(agent, class_id))
		return OMCI_RESULT_UNKNOWN_ME;
	if (!omci_me_action_allowed(agent, class_id, OMCI_MSG_TYPE_CREATE))
		return OMCI_RESULT_NOT_SUPPORTED;
	/*
	 * A profile seed stands in for OLT provisioning, so the OLT's own
	 * instance replaces it, e.g. Nokia's class 171 on the slot 6 VEIP.
	 */
	omci_agent_learn_veip_slot(odev, class_id, content, content_len);
	omci_mib_drop_default(agent, class_id, entity_id);
	if (omci_mib_lookup(agent, class_id, entity_id))
		return OMCI_RESULT_INSTANCE_EXISTS;

	desc = omci_me_lookup(agent, class_id);
	object = kzalloc(sizeof(*object), GFP_KERNEL);
	if (!object)
		return OMCI_RESULT_DEVICE_BUSY;
	object->class_id = class_id;
	object->entity_id = entity_id;
	object->origin = OMCI_MIB_ORIGIN_OLT;
	object->owner_profile = desc && (desc->flags & OMCI_ME_F_VENDOR) ?
		agent->profile_effective : OMCI_OLT_PROFILE_UNSPEC;
	object->data_len = desc ? desc->data_len :
		min_t(size_t, content_len, sizeof(object->data));

	if (desc) {
		ret = omci_me_decode_create(desc, object, content, content_len);
	} else {
		memcpy(object->data, content,
		       min_t(size_t, content_len, sizeof(object->data)));
		object->attr_mask = 0xffff;
		ret = 0;
	}
	if (ret) {
		result = OMCI_RESULT_PARAMETER_ERROR;
		goto out;
	}
	ret = omci_mib_parse_create(object, object->data);
	if (ret) {
		result = OMCI_RESULT_PARAMETER_ERROR;
		goto out;
	}
	hardware_applied = true;
	ret = omci_agent_hw_update(odev, object, OMCI_MSG_TYPE_CREATE,
				   content);
	if (ret) {
		result = OMCI_RESULT_PROCESSING_ERROR;
		goto rollback;
	}
	ret = omci_mib_store_locked(agent, object);
	if (ret) {
		result = OMCI_RESULT_DEVICE_BUSY;
		goto rollback;
	}
	stored = omci_mib_lookup(agent, class_id, entity_id);
	if (omci_agent_datapath_class(agent, class_id)) {
		ret = omci_agent_reconcile_services_locked(odev);
		if (ret) {
			result = OMCI_RESULT_PROCESSING_ERROR;
			goto rollback;
		}
	}
	omci_agent_increment_mib_sync_locked(agent);
	goto out;

rollback:
	if (stored) {
		xa_erase(&agent->mib, omci_mib_key(class_id, entity_id));
		kfree(stored);
	}
	if (hardware_applied)
		omci_agent_hw_update(odev, object, OMCI_MSG_TYPE_DELETE,
				     object->data);
	if (stored && omci_agent_datapath_class(agent, class_id))
		omci_agent_reconcile_services_locked(odev);
out:
	kfree(object);
	return result;
}

static const char *const omci_credentials_status_names[] = {
	"initial", "successful", "LOID error", "password error",
	"duplicate LOID",
};

/*
 * ONU-G has no ME descriptor. Leave attr_mask alone: MIB upload sends the
 * stored data verbatim.
 */
static int omci_agent_set_onu_g(struct omci_device *odev,
				struct omci_mib_object *object, u16 mask,
				const u8 *data, size_t len)
{
	struct omci_agent *agent = &odev->agent;
	int status = -1;
	unsigned int i;

	if (!mask || mask & ~OMCI_ONU_G_ATTR_MASK)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(omci_onu_g_attr_layout); i++) {
		const struct omci_get_attr_layout *attr = &omci_onu_g_attr_layout[i];

		if (!(mask & attr->mask))
			continue;
		if (len < attr->len)
			return -EINVAL;

		switch (attr->mask) {
		case BIT(10):	/* Battery backup */
		case BIT(9):	/* Administrative state */
			object->data[attr->offset] = data[0];
			break;
		case BIT(4):	/* Credentials status */
			status = data[0];
			break;
		default:
			if (!agent->permissive)
				return -EACCES;
			break;
		}
		data += attr->len;
		len -= attr->len;
	}

	if (status >= 0 && status != agent->loid_status)
		dev_info(odev->parent,
			 "OLT set LOID credentials status %d (%s)\n", status,
			 status < ARRAY_SIZE(omci_credentials_status_names) ?
			 omci_credentials_status_names[status] : "unknown");
	if (status >= 0)
		agent->loid_status = status;

	return 0;
}

static u8 omci_agent_set_locked(struct omci_device *odev, u16 class_id,
				u16 entity_id, u8 action, const u8 *content,
				size_t content_len, bool *profile_changed)
{
	struct omci_agent *agent = &odev->agent;
	const struct omci_me_desc *desc;
	struct omci_mib_object *previous = NULL;
	struct omci_mib_object *object;
	struct omci_mib_object *existing;
	bool hardware_applied = false;
	bool allow_create;
	bool existed;
	u16 mask;
	int ret;

	if (content_len < 2)
		return OMCI_RESULT_PARAMETER_ERROR;
	if (!omci_me_active(agent, class_id))
		return OMCI_RESULT_UNKNOWN_ME;
	if (!omci_me_action_allowed(agent, class_id, action))
		return OMCI_RESULT_NOT_SUPPORTED;

	mask = get_unaligned_be16(content);
	desc = omci_me_lookup(agent, class_id);
	allow_create = agent->permissive || agent->fake_omci ||
		omci_agent_profile_has_quirk(agent,
					     OMCI_OLT_QUIRK_ALLOW_SET_CREATE);
	existing = omci_mib_lookup(agent, class_id, entity_id);
	existed = !!existing;
	if (existing) {
		previous = kmemdup(existing, sizeof(*previous), GFP_KERNEL);
		if (!previous)
			return OMCI_RESULT_DEVICE_BUSY;
	}
	object = omci_get_or_create_locked(agent, class_id, entity_id,
					  allow_create);
	if (!object) {
		ret = allow_create ? OMCI_RESULT_DEVICE_BUSY :
				     OMCI_RESULT_UNKNOWN_INSTANCE;
		goto out_free_previous;
	}
	if (!object->data_len)
		object->data_len = desc ? desc->data_len : sizeof(object->data);
	if (!object->owner_profile && desc && (desc->flags & OMCI_ME_F_VENDOR))
		object->owner_profile = agent->profile_effective;

	if (desc) {
		ret = omci_me_decode_set(desc, object, mask, content + 2,
					 content_len - 2);
	} else if (class_id == OMCI_CLASS_PRIORITY_QUEUE ||
		   class_id == OMCI_CLASS_TRAFFIC_SCHEDULER) {
		ret = omci_mib_parse_set(object, mask, content + 2,
					 content_len - 2);
	} else if (class_id == OMCI_CLASS_ONU_G) {
		ret = omci_agent_set_onu_g(odev, object, mask, content + 2,
					   content_len - 2);
	} else {
		memcpy(object->data, content + 2,
		       min_t(size_t, content_len - 2, sizeof(object->data)));
		object->attr_mask |= mask;
		ret = 0;
	}
	if (!ret && desc && (class_id == OMCI_CLASS_OLT_G ||
				     class_id == OMCI_CLASS_VLAN_TAGGING_FILTER_DATA ||
				     class_id == OMCI_CLASS_EXTENDED_VLAN))
		ret = omci_mib_parse_set(object, mask, content + 2,
					 content_len - 2);
	if (!ret && !desc && class_id == OMCI_CLASS_OLT_G)
		ret = omci_mib_parse_set(object, mask, content + 2,
					 content_len - 2);
	if (ret)
		goto rollback_parameter;

	/* ONU2-G advertises policy changes, not queue/parent-port reassignment. */
	if (previous &&
	    ((class_id == OMCI_CLASS_PRIORITY_QUEUE &&
	      (mask & OMCI_PRIORITY_QUEUE_RELATED_PORT_MASK) &&
	      memcmp(previous->data + 9, object->data + 9, 4)) ||
	     (class_id == OMCI_CLASS_TRAFFIC_SCHEDULER &&
	      (mask & OMCI_TRAFFIC_SCHEDULER_TCONT_MASK) &&
	      memcmp(previous->data, object->data, 2)))) {
		ret = -EINVAL;
		goto rollback_parameter;
	}
	if ((class_id == OMCI_CLASS_PRIORITY_QUEUE &&
	     (!previous || memcmp(previous->data + 9, object->data + 9, 7)) &&
	     (mask & (OMCI_PRIORITY_QUEUE_RELATED_PORT_MASK |
		      OMCI_PRIORITY_QUEUE_SCHEDULER_MASK | OMCI_PRIORITY_QUEUE_WEIGHT_MASK))) ||
	    (class_id == OMCI_CLASS_TRAFFIC_SCHEDULER &&
	     (!previous || memcmp(previous->data, object->data, 5)) &&
	     (mask & (OMCI_TRAFFIC_SCHEDULER_TCONT_MASK | OMCI_TRAFFIC_SCHEDULER_PARENT_MASK |
		      OMCI_TRAFFIC_SCHEDULER_POLICY_MASK))) ||
	    (class_id == OMCI_CLASS_TCONT && (mask & BIT(13)) &&
	     (!previous || previous->data[3] != object->data[3])))
		object->qos_configured = true;

	/* Keep ONU-created instances across a subsequent MIB reset. */
	if (!existed)
		object->origin = OMCI_MIB_ORIGIN_OLT;
	if (class_id == OMCI_CLASS_OLT_G) {
		ret = omci_agent_profile_refresh_locked(odev, &object->olt_g,
							profile_changed);
		if (ret)
			goto rollback_processing;
	}
	hardware_applied = true;
	if (previous && class_id == OMCI_CLASS_GEM_PORT_CTP &&
	    get_unaligned_be16(previous->data) != get_unaligned_be16(object->data)) {
		ret = omci_agent_gem_rate_locked(odev, previous, true);
		if (ret)
			goto rollback_processing;
	}
	ret = omci_agent_hw_update(odev, object, action, content);
	if (ret)
		goto rollback_processing;
	if (omci_agent_datapath_class(agent, class_id)) {
		ret = omci_agent_reconcile_services_locked(odev);
		if (ret)
			goto rollback_processing;
	}
	omci_agent_increment_mib_sync_locked(agent);
	ret = OMCI_RESULT_SUCCESS;
	goto out_free_previous;

rollback_parameter:
	if (existed) {
		*object = *previous;
	} else {
		xa_erase(&agent->mib, omci_mib_key(class_id, entity_id));
		kfree(object);
	}
	ret = ret == -ENOSPC ? OMCI_RESULT_DEVICE_BUSY :
				     OMCI_RESULT_PARAMETER_ERROR;
	goto out_free_previous;

rollback_processing:
	if (existed) {
		if (class_id == OMCI_CLASS_GEM_PORT_CTP &&
		    get_unaligned_be16(previous->data) != get_unaligned_be16(object->data))
			omci_agent_gem_rate_locked(odev, object, true);
		*object = *previous;
		if (hardware_applied)
			omci_agent_hw_update(odev, object, OMCI_MSG_TYPE_SET,
					     object->data);
	} else {
		xa_erase(&agent->mib, omci_mib_key(class_id, entity_id));
		if (hardware_applied)
			omci_agent_hw_update(odev, object, OMCI_MSG_TYPE_DELETE,
					     object->data);
		kfree(object);
	}
	if (omci_agent_datapath_class(agent, class_id))
		omci_agent_reconcile_services_locked(odev);
	ret = OMCI_RESULT_PROCESSING_ERROR;

out_free_previous:
	kfree(previous);
	return ret;
}

static u8 omci_agent_delete_locked(struct omci_device *odev, u16 class_id,
				   u16 entity_id, bool *profile_changed)
{
	struct omci_agent *agent = &odev->agent;
	const struct omci_me_desc *desc;
	struct omci_mib_object *object;
	u8 restore_action;
	int ret;

	if (!omci_me_active(agent, class_id))
		return OMCI_RESULT_UNKNOWN_ME;
	if (!omci_me_action_allowed(agent, class_id, OMCI_MSG_TYPE_DELETE))
		return OMCI_RESULT_NOT_SUPPORTED;
	object = omci_mib_lookup(agent, class_id, entity_id);
	if (!object)
		return OMCI_RESULT_UNKNOWN_INSTANCE;
	if (object->origin == OMCI_MIB_ORIGIN_DEFAULT)
		return OMCI_RESULT_NOT_SUPPORTED;

	desc = omci_me_lookup(agent, class_id);
	restore_action = desc && (desc->flags & OMCI_ME_F_ONU_CREATED) ?
		OMCI_MSG_TYPE_SET : OMCI_MSG_TYPE_CREATE;
	object->pending_delete = true;

	if (class_id == OMCI_CLASS_OLT_G) {
		ret = omci_agent_profile_refresh_locked(odev, NULL,
						profile_changed);
		if (ret)
			goto rollback_mib;
	}
	if (omci_agent_datapath_class(agent, class_id)) {
		ret = omci_agent_reconcile_services_locked(odev);
		if (ret)
			goto rollback_profile;
	}
	ret = omci_agent_hw_update(odev, object, OMCI_MSG_TYPE_DELETE,
				   object->data);
	if (ret)
		goto rollback_services;

	xa_erase(&agent->mib, omci_mib_key(class_id, entity_id));
	kfree(object);
	omci_agent_increment_mib_sync_locked(agent);
	return OMCI_RESULT_SUCCESS;

rollback_services:
	object->pending_delete = false;
	omci_agent_hw_update(odev, object, restore_action, object->data);
	omci_agent_reconcile_services_locked(odev);
	return OMCI_RESULT_PROCESSING_ERROR;
rollback_profile:
	if (class_id == OMCI_CLASS_OLT_G)
		omci_agent_profile_refresh_locked(odev, &object->olt_g, NULL);
rollback_mib:
	object->pending_delete = false;
	return OMCI_RESULT_PROCESSING_ERROR;
}

static s16 omci_power_nw_to_0p002_db(u32 power_nw, s32 offset)
{
	s64 value;

	if (!power_nw)
		return -32768;

	value = DIV_ROUND_CLOSEST_ULL((u64)intlog10(power_nw) *
				      OMCI_OPTICAL_LEVEL_SCALE * 10,
				      1U << 24);
	value -= offset;

	return clamp_t(s64, value, -32768, 32767);
}

static s16 omci_power_nw_to_ani_g_level(u32 power_nw)
{
	return omci_power_nw_to_0p002_db(power_nw,
					 OMCI_NW_TO_DBM_0P002_OFFSET);
}

static s16 omci_power_nw_to_test_level(u32 power_nw)
{
	return omci_power_nw_to_0p002_db(power_nw,
					 OMCI_NW_TO_DBUW_0P002_OFFSET);
}

static s16 omci_temperature_mc_to_test_level(s32 temperature_mc)
{
	s64 value;

	value = DIV_S64_ROUND_CLOSEST((s64)temperature_mc * 256, 1000);

	return clamp_t(s64, value, -32768, 32767);
}

static u16 omci_voltage_uv_to_test_level(u32 voltage_uv)
{
	return min_t(u32, DIV_ROUND_CLOSEST(voltage_uv, 20000), 65535);
}

static u16 omci_bias_ua_to_test_level(u32 bias_ua)
{
	return min_t(u32, DIV_ROUND_CLOSEST(bias_ua, 2), 65535);
}

static u8 omci_agent_get_ani_g_telemetry(struct omci_device *odev, u16 mask,
					 u8 *response_content,
					 size_t response_capacity,
					 size_t *response_len)
{
	struct omci_telemetry telemetry = {};
	u8 *data = response_content + 3;
	size_t used = 3;
	u16 level;
	int ret;

	if (response_capacity < 3)
		return OMCI_RESULT_PARAMETER_ERROR;
	memset(response_content, 0, response_capacity);
	ret = odev->ops->get_telemetry(odev, &telemetry);
	if (ret)
		return OMCI_RESULT_PROCESSING_ERROR;

	if ((mask & OMCI_ANI_G_RX_LEVEL_MASK) &&
	    !(telemetry.valid & OMCI_TELEMETRY_F_BOSA_RX_POWER))
		goto attribute_failed;
	if ((mask & OMCI_ANI_G_TX_LEVEL_MASK) &&
	    !(telemetry.valid & OMCI_TELEMETRY_F_BOSA_TX_POWER))
		goto attribute_failed;
	if (hweight16(mask & OMCI_ANI_G_OPTICAL_LEVEL_MASK) * sizeof(u16) >
	    response_capacity - used)
		goto attribute_failed;

	response_content[0] = OMCI_RESULT_SUCCESS;
	put_unaligned_be16(mask, response_content + 1);
	if (mask & OMCI_ANI_G_RX_LEVEL_MASK) {
		level = (u16)omci_power_nw_to_ani_g_level(
			telemetry.bosa_rx_power_nw);
		put_unaligned_be16(level, data);
		data += 2;
		used += 2;
	}
	if (mask & OMCI_ANI_G_TX_LEVEL_MASK) {
		level = (u16)omci_power_nw_to_ani_g_level(
			telemetry.bosa_tx_power_nw);
		put_unaligned_be16(level, data);
		used += 2;
	}
	*response_len = used;
	return OMCI_RESULT_SUCCESS;

attribute_failed:
	if (response_capacity < 5)
		return OMCI_RESULT_PARAMETER_ERROR;
	response_content[0] = OMCI_RESULT_ATTRIBUTE_FAILED;
	put_unaligned_be16(0, response_content + 1);
	put_unaligned_be16(mask, response_content + 3);
	*response_len = 5;
	return OMCI_RESULT_ATTRIBUTE_FAILED;
}

static bool omci_agent_get_uses_ani_g_telemetry(u16 class_id, u16 entity_id,
						u16 mask)
{
	return class_id == OMCI_CLASS_ANI_G &&
	       entity_id == OMCI_ANI_G_ENTITY_ID &&
	       mask &&
	       !(mask & ~OMCI_ANI_G_OPTICAL_LEVEL_MASK);
}

static u8 omci_agent_get_masked_object(const u8 *object_data,
				       const struct omci_get_attr_layout *layout,
				       size_t layout_len, u16 supported_mask,
				       u16 mask, u8 *response_content,
				       size_t response_capacity,
				       size_t *response_len)
{
	u8 *data = response_content + 3;
	size_t available;
	size_t used = 3;
	unsigned int i;

	if (response_capacity < 3)
		return OMCI_RESULT_PARAMETER_ERROR;
	memset(response_content, 0, response_capacity);
	available = response_capacity - used;
	if (!mask || mask & ~supported_mask)
		goto attribute_failed;

	for (i = 0; i < layout_len; i++) {
		const struct omci_get_attr_layout *attr = &layout[i];

		if (!(mask & attr->mask))
			continue;
		if (attr->len > available)
			goto attribute_failed;

		if (attr->offset == OMCI_ATTR_VALUE_ZERO)
			memset(data, 0, attr->len);
		else
			memcpy(data, object_data + attr->offset, attr->len);
		data += attr->len;
		used += attr->len;
		available -= attr->len;
	}

	response_content[0] = OMCI_RESULT_SUCCESS;
	put_unaligned_be16(mask, response_content + 1);
	*response_len = used;
	return OMCI_RESULT_SUCCESS;

attribute_failed:
	if (response_capacity < 5)
		return OMCI_RESULT_PARAMETER_ERROR;
	response_content[0] = OMCI_RESULT_ATTRIBUTE_FAILED;
	put_unaligned_be16(0, response_content + 1);
	put_unaligned_be16(mask, response_content + 3);
	*response_len = 5;
	return OMCI_RESULT_ATTRIBUTE_FAILED;
}

static int
omci_agent_refresh_nokia_optical(struct omci_device *odev,
				 struct omci_mib_object *object)
{
	struct omci_telemetry telemetry = {};
	u16 value;
	int ret;

	if (!odev->ops->get_telemetry)
		return -EOPNOTSUPP;
	ret = odev->ops->get_telemetry(odev, &telemetry);
	if (ret)
		return ret;

	object->data[0] = !!(telemetry.valid & OMCI_TELEMETRY_F_BOSA_VOLTAGE);
	if (object->data[0]) {
		value = min_t(u32, DIV_ROUND_CLOSEST(telemetry.bosa_voltage_uv,
							  10000), U16_MAX);
		put_unaligned_be16(value, object->data + 1);
	}
	object->data[3] = !!(telemetry.valid & OMCI_TELEMETRY_F_BOSA_RX_POWER);
	if (object->data[3]) {
		value = min_t(u32, DIV_ROUND_CLOSEST(telemetry.bosa_rx_power_nw,
							  10000), U16_MAX);
		put_unaligned_be16(value, object->data + 4);
	}
	object->data[6] = !!(telemetry.valid & OMCI_TELEMETRY_F_BOSA_TX_POWER);
	if (object->data[6]) {
		value = min_t(u32, DIV_ROUND_CLOSEST(telemetry.bosa_tx_power_nw,
							  10000), U16_MAX);
		put_unaligned_be16(value, object->data + 7);
	}
	object->data[9] = !!(telemetry.valid & OMCI_TELEMETRY_F_BOSA_BIAS);
	if (object->data[9]) {
		value = min_t(u64, DIV_ROUND_CLOSEST_ULL(
					(u64)telemetry.bosa_bias_ua * 2, 1000000),
			      U16_MAX);
		put_unaligned_be16(value, object->data + 10);
	}
	object->data[12] = !!(telemetry.valid &
				      OMCI_TELEMETRY_F_BOSA_TEMPERATURE);
	if (object->data[12]) {
		value = clamp_t(s32, DIV_S64_ROUND_CLOSEST(
					telemetry.bosa_temperature_mc, 1000),
				0, U16_MAX);
		put_unaligned_be16(value, object->data + 13);
	}

	return 0;
}

static void omci_agent_refresh_ani_g(struct omci_device *odev,
				     struct omci_mib_object *object)
{
	struct omci_telemetry telemetry = {};
	s16 level;

	if (!odev->ops->get_telemetry ||
	    odev->ops->get_telemetry(odev, &telemetry))
		return;

	if (telemetry.valid & OMCI_TELEMETRY_F_BOSA_RX_POWER) {
		level = omci_power_nw_to_ani_g_level(telemetry.bosa_rx_power_nw);
		put_unaligned_be16(level, object->data + OMCI_ANI_G_RX_LEVEL_OFFSET);
	}
	if (telemetry.valid & OMCI_TELEMETRY_F_BOSA_TX_POWER) {
		level = omci_power_nw_to_ani_g_level(telemetry.bosa_tx_power_nw);
		put_unaligned_be16(level, object->data + OMCI_ANI_G_TX_LEVEL_OFFSET);
	}
}

static u8 omci_agent_get_locked(struct omci_device *odev, u16 class_id,
				u16 entity_id, const u8 *content,
				size_t content_len, u8 *response_content,
				size_t response_capacity,
				size_t *response_len)
{
	struct omci_agent *agent = &odev->agent;
	const struct omci_me_desc *desc;
	struct omci_mib_object *object;
	size_t encoded_len = 0;
	u16 encoded_mask;
	u16 mask;
	int ret;

	if (content_len < 2 || response_capacity < 1)
		return OMCI_RESULT_PARAMETER_ERROR;
	mask = get_unaligned_be16(content);
	if (!omci_me_active(agent, class_id))
		return OMCI_RESULT_UNKNOWN_ME;
	if (!omci_me_action_allowed(agent, class_id, OMCI_MSG_TYPE_GET))
		return OMCI_RESULT_NOT_SUPPORTED;
	if (class_id == OMCI_CLASS_OMCI)
		return omci_agent_get_omci_table_locked(odev, entity_id, mask,
							response_content,
							response_capacity,
							response_len);
	if (class_id == OMCI_CLASS_MANAGED_ENTITY)
		return omci_agent_get_managed_entity_locked(odev, entity_id,
			mask, response_content, response_capacity, response_len);
	if (class_id == OMCI_CLASS_ATTRIBUTE)
		return omci_agent_get_attribute_locked(odev, entity_id, mask,
			response_content, response_capacity, response_len);

	if (omci_agent_get_uses_ani_g_telemetry(class_id, entity_id, mask) &&
	    odev->ops && odev->ops->get_telemetry)
		return omci_agent_get_ani_g_telemetry(odev, mask,
						      response_content,
						      response_capacity,
						      response_len);

	object = omci_get_or_create_locked(agent, class_id, entity_id,
					   agent->permissive ||
					   agent->fake_omci);
	if (!object)
		return OMCI_RESULT_UNKNOWN_INSTANCE;
	if (class_id == OMCI_CLASS_NOKIA_OPTICAL_SUPERVISION &&
	    omci_agent_refresh_nokia_optical(odev, object))
		return OMCI_RESULT_PROCESSING_ERROR;
	if (class_id == OMCI_CLASS_ANI_G)
		omci_agent_refresh_ani_g(odev, object);

	switch (class_id) {
	case OMCI_CLASS_ONU_G: {
		u8 onu_g[OMCI_ONU_G_GET_LEN];

		memcpy(onu_g, object->data, OMCI_ONU_G_LOID_OFFSET);
		memcpy(onu_g + OMCI_ONU_G_LOID_OFFSET, agent->config.loid,
		       OMCI_LOID_LEN);
		memcpy(onu_g + OMCI_ONU_G_LOID_PASSWORD_OFFSET,
		       agent->config.loid_password, OMCI_LOID_PASSWORD_LEN);
		onu_g[OMCI_ONU_G_CREDENTIALS_OFFSET] = agent->loid_status;
		return omci_agent_get_masked_object(
			onu_g, omci_onu_g_attr_layout,
			ARRAY_SIZE(omci_onu_g_attr_layout), OMCI_ONU_G_ATTR_MASK,
			mask, response_content, response_capacity, response_len);
	}
	case OMCI_CLASS_ONU2_G:
		return omci_agent_get_masked_object(
			object->data, omci_onu2_g_attr_layout,
			ARRAY_SIZE(omci_onu2_g_attr_layout), OMCI_ONU2_G_ATTR_MASK,
			mask, response_content, response_capacity, response_len);
	default:
		break;
	}

	if (response_capacity < 3)
		return OMCI_RESULT_PARAMETER_ERROR;
	memset(response_content, 0, response_capacity);
	response_content[0] = OMCI_RESULT_SUCCESS;
	if (class_id == OMCI_CLASS_PRIORITY_QUEUE) {
		put_unaligned_be16(mask, response_content + 1);
		ret = omci_priority_queue_serialize(object, mask,
					    response_content + 3,
					    response_capacity - 3,
					    &encoded_len);
		if (ret)
			goto attribute_failed;
	} else if (class_id == OMCI_CLASS_TRAFFIC_SCHEDULER) {
		put_unaligned_be16(mask, response_content + 1);
		ret = omci_traffic_scheduler_serialize(object, mask,
					       response_content + 3,
					       response_capacity - 3,
					       &encoded_len);
		if (ret)
			goto attribute_failed;
	} else {
		desc = omci_me_lookup(agent, class_id);
		if (!desc) {
			encoded_len = min_t(size_t, object->data_len,
					    response_capacity - 3);
			put_unaligned_be16(mask, response_content + 1);
			memcpy(response_content + 3, object->data, encoded_len);
			*response_len = 3 + encoded_len;
			return OMCI_RESULT_SUCCESS;
		}
		ret = omci_me_encode_attributes(desc, object, mask,
						response_content + 3,
						response_capacity - 3,
						&encoded_mask, &encoded_len);
		if (ret || encoded_mask != mask)
			goto attribute_failed;
		put_unaligned_be16(encoded_mask, response_content + 1);
	}
	*response_len = 3 + encoded_len;
	return OMCI_RESULT_SUCCESS;

attribute_failed:
	if (response_capacity < 5)
		return OMCI_RESULT_PARAMETER_ERROR;
	response_content[0] = OMCI_RESULT_ATTRIBUTE_FAILED;
	put_unaligned_be16(0, response_content + 1);
	put_unaligned_be16(mask, response_content + 3);
	*response_len = 5;
	return OMCI_RESULT_ATTRIBUTE_FAILED;
}

static void omci_test_result_put(u8 *field, u8 type, u16 value)
{
	field[0] = type;
	put_unaligned_be16(value, field + 1);
}

static bool omci_agent_is_ani_g_test(const struct omci_wire_request *request)
{
	return (request->message_type & 0x1f) == OMCI_MSG_TYPE_TEST &&
	       request->class_id == OMCI_CLASS_ANI_G &&
	       request->entity_id == OMCI_ANI_G_ENTITY_ID;
}

static void omci_agent_log_wire(struct omci_device *odev,
				const char *direction, const void *data,
				size_t len)
{
	struct omci_wire_request request;
	bool response;
	u8 action;

	if (omci_wire_decode(data, len, &request)) {
		dev_warn(odev->parent, "OMCI %s invalid PDU: %*phN\n",
			 direction, (int)len, data);
		return;
	}

	action = request.message_type & 0x1f;
	response = request.message_type & 0x20;
	if (action == OMCI_MSG_TYPE_MIB_UPLOAD && response &&
	    request.payload_len >= 2) {
		dev_dbg(odev->parent,
			"OMCI %s: tci=%#06x type=%#04x action=%u device=%#04x class=%u entity=%#06x payload=%u command-count=%u\n",
			direction, request.transaction_id, request.message_type,
			action, request.device_id, request.class_id,
			request.entity_id, request.payload_len,
			get_unaligned_be16(request.payload));
	} else if (action == OMCI_MSG_TYPE_MIB_UPLOAD_NEXT &&
		   request.payload_len >= 2) {
		dev_dbg(odev->parent,
			"OMCI %s: tci=%#06x type=%#04x action=%u device=%#04x class=%u entity=%#06x payload=%u %s=%u\n",
			direction, request.transaction_id, request.message_type,
			action, request.device_id, request.class_id,
			request.entity_id, request.payload_len,
			response ? "returned-class" : "sequence",
			get_unaligned_be16(request.payload));
	} else if (action == OMCI_MSG_TYPE_GET_NEXT &&
		   request.payload_len >= (response ? 3 : 4)) {
		u16 mask = response ? get_unaligned_be16(request.payload + 1) :
				      get_unaligned_be16(request.payload);
		u16 sequence = response ? 0 :
				  get_unaligned_be16(request.payload + 2);

		dev_dbg(odev->parent,
			"OMCI %s: tci=%#06x type=%#04x action=%u device=%#04x class=%u entity=%#06x payload=%u result=%u mask=%#06x sequence=%u\n",
			direction, request.transaction_id, request.message_type,
			action, request.device_id, request.class_id,
			request.entity_id, request.payload_len,
			response ? request.payload[0] : 0, mask, sequence);
	} else {
		u8 result = request.payload_len ? request.payload[0] : 0;

		dev_dbg(odev->parent,
			"OMCI %s: tci=%#06x type=%#04x action=%u device=%#04x class=%u entity=%#06x payload=%u result=%u\n",
			direction, request.transaction_id, request.message_type,
			action, request.device_id, request.class_id,
			request.entity_id, request.payload_len, result);
	}
	dev_dbg(odev->parent, "OMCI %s PDU: %*phN\n",
		direction, (int)len, data);
}

static int
omci_agent_send_ani_g_test_result(struct omci_device *odev,
				  const struct omci_wire_request *request)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_telemetry telemetry = {};
	u8 pdu[OMCI_BASELINE_LEN_NO_MIC];
	u8 content[17] = {};
	size_t pdu_len;
	u16 value;
	int ret;

	if (!odev->ops || !odev->ops->get_telemetry)
		return -EOPNOTSUPP;

	ret = odev->ops->get_telemetry(odev, &telemetry);
	if (ret)
		return ret;

	if (telemetry.valid & OMCI_TELEMETRY_F_BOSA_VOLTAGE) {
		value = omci_voltage_uv_to_test_level(telemetry.bosa_voltage_uv);
		omci_test_result_put(content, OMCI_TEST_TYPE_VOLTAGE, value);
	} else {
		omci_test_result_put(content, OMCI_TEST_TYPE_UNSUPPORTED, 0);
	}

	if (telemetry.valid & OMCI_TELEMETRY_F_BOSA_RX_POWER) {
		value = (u16)omci_power_nw_to_test_level(
			telemetry.bosa_rx_power_nw);
		omci_test_result_put(content + 3, OMCI_TEST_TYPE_RX_POWER,
				     value);
	} else {
		omci_test_result_put(content + 3, OMCI_TEST_TYPE_UNSUPPORTED, 0);
	}

	if (telemetry.valid & OMCI_TELEMETRY_F_BOSA_TX_POWER) {
		value = (u16)omci_power_nw_to_test_level(
			telemetry.bosa_tx_power_nw);
		omci_test_result_put(content + 6, OMCI_TEST_TYPE_TX_POWER,
				     value);
	} else {
		omci_test_result_put(content + 6, OMCI_TEST_TYPE_UNSUPPORTED, 0);
	}

	if (telemetry.valid & OMCI_TELEMETRY_F_BOSA_BIAS) {
		value = omci_bias_ua_to_test_level(telemetry.bosa_bias_ua);
		omci_test_result_put(content + 9, OMCI_TEST_TYPE_BIAS, value);
	} else {
		omci_test_result_put(content + 9, OMCI_TEST_TYPE_UNSUPPORTED, 0);
	}

	if (telemetry.valid & OMCI_TELEMETRY_F_BOSA_TEMPERATURE) {
		value = (u16)omci_temperature_mc_to_test_level(
			telemetry.bosa_temperature_mc);
		omci_test_result_put(content + 12, OMCI_TEST_TYPE_TEMPERATURE,
				     value);
	} else {
		omci_test_result_put(content + 12, OMCI_TEST_TYPE_UNSUPPORTED, 0);
	}
	put_unaligned_be16(0, content + 15);

	ret = omci_wire_encode_response(request, OMCI_MSG_TYPE_TEST_RESULT,
					content, sizeof(content), pdu,
					sizeof(pdu), &pdu_len);
	if (ret)
		return ret;

	omci_agent_log_wire(odev, "TX", pdu, pdu_len);
	ret = omci_device_xmit(odev, pdu, pdu_len);
	if (!ret)
		atomic64_inc(&agent->responses);

	return ret;
}

/* A baseline MIB upload next carries class, instance, mask and 26 value bytes. */
#define OMCI_UPLOAD_VALUES_LEN	26

struct omci_upload_attr {
	u16 mask;
	u8 len;
	const u8 *value;	/* NULL: reported as zero */
};

static unsigned int
omci_upload_attrs_from_layout(const u8 *data,
			      const struct omci_get_attr_layout *layout,
			      size_t layout_len, u16 mask,
			      struct omci_upload_attr *attrs)
{
	unsigned int i, n = 0;

	for (i = 0; i < layout_len; i++) {
		if (!(mask & layout[i].mask))
			continue;
		attrs[n++] = (struct omci_upload_attr) { layout[i].mask, layout[i].len,
			layout[i].offset == OMCI_ATTR_VALUE_ZERO ? NULL :
			data + layout[i].offset };
	}
	return n;
}

/*
 * The attributes an MIB upload reports for @object, in attribute order:
 * readable, non-table ones (G.988 A.2.13). ONU-G reports the same values
 * as a Get, so @scratch holds its LOID view. Objects without a descriptor
 * or layout report their stored data as one block, as they always have.
 */
static unsigned int omci_mib_upload_attrs(const struct omci_agent *agent,
					  const struct omci_mib_object *object,
					  struct omci_upload_attr *attrs, u8 *scratch)
{
	const struct omci_me_desc *desc;
	unsigned int bit, i, n = 0;
	u16 mask;

	switch (object->class_id) {
	case OMCI_CLASS_ONU_G:
		memcpy(scratch, object->data, OMCI_ONU_G_LOID_OFFSET);
		memcpy(scratch + OMCI_ONU_G_LOID_OFFSET, agent->config.loid,
		       OMCI_LOID_LEN);
		memcpy(scratch + OMCI_ONU_G_LOID_PASSWORD_OFFSET,
		       agent->config.loid_password, OMCI_LOID_PASSWORD_LEN);
		scratch[OMCI_ONU_G_CREDENTIALS_OFFSET] = agent->loid_status;
		return omci_upload_attrs_from_layout(scratch, omci_onu_g_attr_layout,
						     ARRAY_SIZE(omci_onu_g_attr_layout),
						     object->attr_mask, attrs);
	case OMCI_CLASS_ONU2_G:
		return omci_upload_attrs_from_layout(object->data, omci_onu2_g_attr_layout,
						     ARRAY_SIZE(omci_onu2_g_attr_layout),
						     object->attr_mask, attrs);
	}

	desc = omci_me_lookup(agent, object->class_id);
	if (!desc) {
		attrs[0] = (struct omci_upload_attr) { object->attr_mask,
			min_t(size_t, object->data_len ?: OMCI_UPLOAD_VALUES_LEN,
			      OMCI_UPLOAD_VALUES_LEN), object->data };
		return 1;
	}
	mask = object->attr_mask & desc->mib_upload_mask & desc->valid_attr_mask;
	for (bit = 0; bit < 16; bit++) {
		for (i = 0; i < desc->num_attrs; i++) {
			const struct omci_attr_desc *attr = &desc->attrs[i];

			if (attr->mask != (mask & BIT(15 - bit)) ||
			    !(attr->access & OMCI_ATTR_ACCESS_READ) ||
			    attr->offset + attr->len > OMCI_MAX_ATTR_DATA)
				continue;
			attrs[n++] = (struct omci_upload_attr) { attr->mask, attr->len,
				object->data + attr->offset };
		}
	}
	return n;
}

/*
 * G.988 A.2.16/A.3.16: attributes that do not fit one MIB upload next
 * response continue in the next, whole attributes only. Returns the
 * number of messages @attrs need and, for @chunk, its first attribute
 * and attribute count. One too long for any message is left out.
 */
static unsigned int omci_mib_upload_chunk(const struct omci_upload_attr *attrs,
					  unsigned int n, unsigned int chunk,
					  unsigned int *first, unsigned int *count)
{
	unsigned int i, chunks = 0, used = 0;

	*first = *count = 0;
	for (i = 0; i < n; i++) {
		if (attrs[i].len > OMCI_UPLOAD_VALUES_LEN)
			continue;
		if (!chunks || used + attrs[i].len > OMCI_UPLOAD_VALUES_LEN) {
			chunks++;
			used = 0;
		}
		used += attrs[i].len;
		if (chunks - 1 == chunk) {
			if (!*count)
				*first = i;
			*count = i + 1 - *first;
		}
	}
	return chunks ?: 1;
}

static unsigned int omci_mib_upload_messages(const struct omci_agent *agent,
					     const struct omci_mib_object *object)
{
	struct omci_upload_attr attrs[16];
	u8 scratch[OMCI_ONU_G_GET_LEN];
	unsigned int first, count;

	return omci_mib_upload_chunk(attrs, omci_mib_upload_attrs(agent, object,
								  attrs, scratch),
				     0, &first, &count);
}

static unsigned int omci_mib_upload_count_locked(struct omci_agent *agent)
{
	struct omci_mib_object *object;
	unsigned long index;
	unsigned int count = 0;

	xa_for_each(&agent->mib, index, object)
		if (omci_mib_object_uploadable(agent, object))
			count += omci_mib_upload_messages(agent, object);
	return count;
}

static int omci_agent_upload_next_locked(struct omci_agent *agent,
					 u16 sequence, u8 *content)
{
	struct omci_upload_attr attrs[16];
	u8 scratch[OMCI_ONU_G_GET_LEN];
	struct omci_mib_object *object;
	unsigned int n, i, first, count, chunks;
	unsigned long index;
	unsigned int upload_pos = 0;
	size_t used = 0;
	u16 mask = 0;

	xa_for_each(&agent->mib, index, object) {
		if (!omci_mib_object_uploadable(agent, object))
			continue;
		n = omci_mib_upload_attrs(agent, object, attrs, scratch);
		chunks = omci_mib_upload_chunk(attrs, n, 0, &first, &count);
		if (sequence < upload_pos + chunks)
			goto found;
		upload_pos += chunks;
	}

	return -ENOENT;

found:
	omci_mib_upload_chunk(attrs, n, sequence - upload_pos, &first, &count);
	put_unaligned_be16(object->class_id, content);
	put_unaligned_be16(object->entity_id, content + 2);
	memset(content + 6, 0, OMCI_UPLOAD_VALUES_LEN);
	for (i = first; i < first + count; i++) {
		if (attrs[i].len > OMCI_UPLOAD_VALUES_LEN)
			continue;
		if (attrs[i].value)
			memcpy(content + 6 + used, attrs[i].value, attrs[i].len);
		used += attrs[i].len;
		mask |= attrs[i].mask;
	}
	put_unaligned_be16(mask, content + 4);
	agent->upload_index = sequence + 1;
	return 0;
}

/* ITU-T G.988 Reboot flags, the first content byte of an ONU-G Reboot */
#define OMCI_REBOOT_UNCONDITIONAL	0
#define OMCI_REBOOT_IF_NO_CALLS		1
#define OMCI_REBOOT_IF_NO_EMERGENCY	2
/* Long enough for the response to leave before services stop */
#define OMCI_REBOOT_DELAY_MS		2000

/*
 * Activate Software Image restarts the ONU into that image (G.988), and
 * OLTs use it as their "reboot ONT" (Nokia among them; vendor OMCI daemons
 * reboot a few seconds after it). There is one image here, so either
 * instance is a restart, under the same switch as an ONU-G Reboot.
 */
static u8 omci_agent_activate_sw_locked(struct omci_device *odev, u16 entity_id)
{
	struct omci_agent *agent = &odev->agent;

	/*
	 * After a download the OLT expects a new image; ours never changes,
	 * so restarting would bring the same download and activate back.
	 */
	if (!agent->reboot || agent->sw_download_seen) {
		dev_warn(odev->parent,
			 "OLT activated software image %#x: no restart (%s)\n", entity_id,
			 agent->reboot ? "it followed an image download" : "OLT reboots are disabled");
		return OMCI_RESULT_SUCCESS;
	}
	dev_warn(odev->parent, "OLT activated software image %#x: rebooting in %u ms\n",
		 entity_id, OMCI_REBOOT_DELAY_MS);
	schedule_delayed_work(&agent->reboot_work, msecs_to_jiffies(OMCI_REBOOT_DELAY_MS));
	return OMCI_RESULT_SUCCESS;
}

static u8 omci_agent_reboot_locked(struct omci_device *odev, u16 class_id,
				   u16 entity_id, const u8 *payload,
				   size_t payload_len)
{
	struct omci_agent *agent = &odev->agent;
	u8 flags = payload_len ? payload[0] : OMCI_REBOOT_UNCONDITIONAL;

	/*
	 * This SFU is one board, so rebooting any of its circuit packs
	 * (G.988 9.1.6) reboots the ONU. Others acknowledge without one.
	 */
	if (class_id != OMCI_CLASS_CIRCUIT_PACK &&
	    (class_id != OMCI_CLASS_ONU_G || entity_id)) {
		dev_info(odev->parent,
			 "OLT reboot request for class %u instance %#x (flags %u): acknowledged, not an ONU reboot\n",
			 class_id, entity_id, flags);
		return OMCI_RESULT_SUCCESS;
	}

	if (!agent->reboot) {
		dev_info(odev->parent,
			 "OLT requested an ONU reboot (flags %u): ignored, OLT reboots are disabled\n",
			 flags);
		return OMCI_RESULT_SUCCESS;
	}
	if (flags > OMCI_REBOOT_IF_NO_EMERGENCY) {
		dev_warn(odev->parent,
			 "OLT requested an ONU reboot with reserved flags %u\n",
			 flags);
		return OMCI_RESULT_PARAMETER_ERROR;
	}
	/*
	 * The voice application reports calls, not which of them are
	 * emergency calls: treat every call as one, so that flag 2 can
	 * never drop an emergency call.
	 */
	if (flags != OMCI_REBOOT_UNCONDITIONAL && agent->voice_calls) {
		dev_warn(odev->parent,
			 "OLT requested an ONU reboot (flags %u): device busy, %u voice call(s) in progress\n",
			 flags, agent->voice_calls);
		return OMCI_RESULT_DEVICE_BUSY;
	}

	dev_warn(odev->parent,
		 "OLT requested an ONU reboot (flags %u): rebooting in %u ms\n",
		 flags, OMCI_REBOOT_DELAY_MS);
	schedule_delayed_work(&agent->reboot_work,
			      msecs_to_jiffies(OMCI_REBOOT_DELAY_MS));
	return OMCI_RESULT_SUCCESS;
}

static const char * const omci_ani_g_alarm_names[] = {
	"low-rx-power", "high-rx-power", "sf", "sd", "low-tx-power",
	"high-tx-power", "laser-bias",
};

static const char * const omci_pptp_alarm_names[] = {
	"lan-los",
};

struct omci_alarm_inputs {
	struct omci_telemetry telemetry;
	bool telemetry_valid;
	unsigned int uni_count;
	unsigned long uni_known;
	unsigned long uni_up;
};

static void omci_alarm_build(u8 *pdu, u8 type, u16 class_id, u16 entity_id)
{
	memset(pdu, 0, OMCI_BASELINE_LEN_NO_MIC);
	pdu[2] = type;
	pdu[3] = OMCI_BASELINE_DEV_ID;
	put_unaligned_be16(class_id, pdu + 4);
	put_unaligned_be16(entity_id, pdu + 6);
	put_unaligned_be32(40, pdu + 40);
}

/* Get All Alarms and MIB Reset restart the sequence; 0 is never sent. */
static void omci_agent_build_alarm_locked(struct omci_agent *agent, u8 *pdu,
					  u16 class_id, u16 entity_id,
					  u8 alarms)
{
	omci_alarm_build(pdu, OMCI_MSG_TYPE_ALARM_NOTIFICATION, class_id,
			 entity_id);
	pdu[8] = alarms;
	if (!++agent->alarm_sequence)
		agent->alarm_sequence = 1;
	pdu[OMCI_ALARM_SEQUENCE_OFFSET] = agent->alarm_sequence;
}

/* Only ANI-G and PPTP Ethernet UNI raise alarms. */
static u8 *omci_alarm_arc(struct omci_mib_object *object, u16 *mask)
{
	if (object->class_id == OMCI_CLASS_ANI_G) {
		*mask = OMCI_ANI_G_ARC_MASK;
		return object->data + OMCI_ANI_G_ARC_OFFSET;
	}

	*mask = OMCI_PPTP_ARC_MASK;
	return object->data + OMCI_PPTP_ARC_OFFSET;
}

/*
 * G.988 alarm reporting control: alarms go unreported while ARC is set, and
 * the ONU clears ARC itself once the entity has been free of alarms for the
 * ARC interval, in minutes, that follows it. 255 never expires.
 */
static bool omci_alarm_arc_expired(struct omci_mib_object *object, u8 *arc,
				   unsigned long now)
{
	if (!arc[0] || object->alarms || arc[1] == 255) {
		object->arc_clean = false;
		return false;
	}
	if (!object->arc_clean) {
		object->arc_clean = true;
		object->arc_clean_since = now;
	}
	if (time_before(now, object->arc_clean_since + arc[1] * 60 * HZ))
		return false;

	arc[0] = 0;
	object->arc_clean = false;
	return true;
}

static void omci_agent_update_alarms_locked(struct omci_agent *agent,
					    struct omci_mib_object *object,
					    u8 alarms,
					    u8 (*pdus)[OMCI_BASELINE_LEN_NO_MIC],
					    unsigned int *count)
{
	u16 arc_mask;
	u8 *arc = omci_alarm_arc(object, &arc_mask);
	u8 *pdu;

	if (alarms != object->alarms) {
		object->alarms = alarms;
		if (!arc[0])
			omci_agent_build_alarm_locked(agent, pdus[(*count)++],
						      object->class_id,
						      object->entity_id, alarms);
	}

	if (omci_alarm_arc_expired(object, arc, jiffies)) {
		pdu = pdus[(*count)++];
		omci_alarm_build(pdu, OMCI_MSG_TYPE_AVC, object->class_id,
				 object->entity_id);
		put_unaligned_be16(arc_mask, pdu + 8);
	}
}

static u8 omci_alarm_set(u8 alarms, u8 bit, bool active)
{
	return active ? alarms | bit : alarms & ~bit;
}

/* Raise @bit past @limit and clear it once 0.5 dB back inside. */
static u8 omci_alarm_level(u8 alarms, u8 bit, s32 level, s32 limit, bool low)
{
	s32 margin = OMCI_OPTICAL_LEVEL_SCALE / 2;

	if (low ? level < limit : level > limit)
		return alarms | bit;
	if (low ? level >= limit + margin : level <= limit - margin)
		return alarms & ~bit;

	return alarms;
}

/*
 * Receive thresholds are -dBm and transmit thresholds signed dBm, both in
 * 0.5 dB steps. Their defaults select the ONU policy, which is the optical
 * frontend's own alarm, as is the laser bias alarm.
 */
static u8 omci_ani_g_optical_alarms(const u8 *data,
				    const struct omci_telemetry *telemetry,
				    u8 alarms)
{
	u32 bosa = telemetry->valid & OMCI_TELEMETRY_F_BOSA_ALARMS ?
		telemetry->bosa_alarms : 0;
	s32 step = OMCI_OPTICAL_LEVEL_SCALE / 2;
	u8 lower, upper;
	s32 level;

	if (telemetry->valid & OMCI_TELEMETRY_F_BOSA_RX_POWER) {
		level = omci_power_nw_to_ani_g_level(telemetry->bosa_rx_power_nw);
		lower = data[OMCI_ANI_G_LOWER_RX_OFFSET];
		upper = data[OMCI_ANI_G_UPPER_RX_OFFSET];
		alarms = lower == OMCI_ANI_G_RX_POLICY ?
			omci_alarm_set(alarms, OMCI_ALARM(OMCI_ANI_G_ALARM_LOW_RX),
				       bosa & OPTICAL_FRONTEND_ALARM_RX_LOW_POWER) :
			omci_alarm_level(alarms, OMCI_ALARM(OMCI_ANI_G_ALARM_LOW_RX),
					 level, -lower * step, true);
		alarms = upper == OMCI_ANI_G_RX_POLICY ?
			omci_alarm_set(alarms, OMCI_ALARM(OMCI_ANI_G_ALARM_HIGH_RX),
				       bosa & OPTICAL_FRONTEND_ALARM_RX_HIGH_POWER) :
			omci_alarm_level(alarms, OMCI_ALARM(OMCI_ANI_G_ALARM_HIGH_RX),
					 level, -upper * step, false);
	}

	if (telemetry->valid & OMCI_TELEMETRY_F_BOSA_TX_POWER) {
		level = omci_power_nw_to_ani_g_level(telemetry->bosa_tx_power_nw);
		lower = data[OMCI_ANI_G_LOWER_TX_OFFSET];
		upper = data[OMCI_ANI_G_UPPER_TX_OFFSET];
		alarms = lower == OMCI_ANI_G_TX_POLICY ?
			omci_alarm_set(alarms, OMCI_ALARM(OMCI_ANI_G_ALARM_LOW_TX),
				       bosa & OPTICAL_FRONTEND_ALARM_TX_LOW_POWER) :
			omci_alarm_level(alarms, OMCI_ALARM(OMCI_ANI_G_ALARM_LOW_TX),
					 level, (s8)lower * step, true);
		alarms = upper == OMCI_ANI_G_TX_POLICY ?
			omci_alarm_set(alarms, OMCI_ALARM(OMCI_ANI_G_ALARM_HIGH_TX),
				       bosa & OPTICAL_FRONTEND_ALARM_TX_HIGH_POWER) :
			omci_alarm_level(alarms, OMCI_ALARM(OMCI_ANI_G_ALARM_HIGH_TX),
					 level, (s8)upper * step, false);
	}

	return omci_alarm_set(alarms, OMCI_ALARM(OMCI_ANI_G_ALARM_LASER_BIAS),
			      bosa & OPTICAL_FRONTEND_ALARM_TX_HIGH_BIAS);
}

static u64 omci_ber_limit(unsigned int secs, u8 exponent)
{
	u64 bits = OMCI_GPON_DS_RATE * secs;

	while (exponent--)
		bits = div_u64(bits, 10);

	return bits;
}

/* SF and SD are raised above a BER of 10^-x and cleared below 10^-(x+1). */
static u8 omci_alarm_ber(u8 alarms, u8 bit, u64 errors, unsigned int secs,
			 u8 exponent)
{
	if (errors > omci_ber_limit(secs, exponent))
		return alarms | bit;
	if (errors <= omci_ber_limit(secs, exponent + 1))
		return alarms & ~bit;

	return alarms;
}

static u8 omci_agent_ber_alarms_locked(struct omci_agent *agent,
				       const u8 *data,
				       const struct omci_telemetry *telemetry,
				       u8 alarms)
{
	u8 sf = data[OMCI_ANI_G_SF_OFFSET];
	u8 sd = data[OMCI_ANI_G_SD_OFFSET];
	unsigned long now = jiffies;
	unsigned int secs;
	u64 errors;

	if (!(telemetry->valid & OMCI_TELEMETRY_F_BIP_ERRORS)) {
		agent->ber_valid = false;
		return alarms;
	}
	if (!agent->ber_valid || telemetry->bip_errors < agent->ber_base) {
		agent->ber_valid = true;
		agent->ber_base = telemetry->bip_errors;
		agent->ber_since = now;
		return alarms;
	}
	if (time_before(now, agent->ber_since + OMCI_BER_WINDOW))
		return alarms;

	secs = (now - agent->ber_since) / HZ;
	errors = telemetry->bip_errors - agent->ber_base;
	agent->ber_base = telemetry->bip_errors;
	agent->ber_since = now;

	if (sf < 3 || sf > 8)
		sf = OMCI_ANI_G_DEFAULT_SF;
	if (sd < 4 || sd > 10)
		sd = OMCI_ANI_G_DEFAULT_SD;
	alarms = omci_alarm_ber(alarms, OMCI_ALARM(OMCI_ANI_G_ALARM_SF),
				errors, secs, sf);
	return omci_alarm_ber(alarms, OMCI_ALARM(OMCI_ANI_G_ALARM_SD),
			      errors, secs, sd);
}

static void omci_agent_read_alarm_inputs(struct omci_device *odev,
					 struct omci_alarm_inputs *in)
{
	unsigned int i;
	bool up;

	if (odev->ops->get_telemetry)
		in->telemetry_valid = !odev->ops->get_telemetry(odev,
								&in->telemetry);

	mutex_lock(&odev->agent.lock);
	in->uni_count = min_t(unsigned int, odev->agent.config.uni_count,
			      OMCI_ALARM_MAX_UNIS);
	mutex_unlock(&odev->agent.lock);

	if (!odev->ops->get_uni_link)
		return;
	for (i = 0; i < in->uni_count; i++) {
		if (odev->ops->get_uni_link(odev,
				OMCI_UNI_ENTITY_ID(OMCI_ETHERNET_SLOT, i + 1),
				&up))
			continue;
		__set_bit(i, &in->uni_known);
		if (up)
			__set_bit(i, &in->uni_up);
	}
}

/*
 * G.988 9.5.1 AVC 6: a locked UNI performs no function, so it is
 * operationally disabled. The OLT learns of the change autonomously.
 */
static void omci_agent_update_op_state_locked(struct omci_mib_object *object,
					      u8 (*pdus)[OMCI_BASELINE_LEN_NO_MIC],
					      unsigned int *count)
{
	u8 disabled = !!object->data[OMCI_PPTP_ADMIN_STATE_OFFSET];
	u8 *pdu;

	if (object->data[OMCI_PPTP_OP_STATE_OFFSET] == disabled)
		return;
	object->data[OMCI_PPTP_OP_STATE_OFFSET] = disabled;
	pdu = pdus[(*count)++];
	omci_alarm_build(pdu, OMCI_MSG_TYPE_AVC, object->class_id,
			 object->entity_id);
	put_unaligned_be16(OMCI_PPTP_OP_STATE_MASK, pdu + 8);
	pdu[10] = disabled;
}

static unsigned int
omci_agent_evaluate_alarms_locked(struct omci_device *odev,
				  const struct omci_alarm_inputs *in,
				  u8 (*pdus)[OMCI_BASELINE_LEN_NO_MIC])
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	unsigned int count = 0;
	unsigned int i;
	u8 alarms;

	object = omci_mib_lookup(agent, OMCI_CLASS_ANI_G, OMCI_ANI_G_ENTITY_ID);
	if (object && in->telemetry_valid) {
		alarms = omci_ani_g_optical_alarms(object->data, &in->telemetry,
						   object->alarms);
		alarms = omci_agent_ber_alarms_locked(agent, object->data,
						      &in->telemetry, alarms);
		omci_agent_update_alarms_locked(agent, object, alarms, pdus,
						&count);
	}

	for (i = 0; i < in->uni_count; i++) {
		object = omci_mib_lookup(agent, OMCI_CLASS_PPTP_ETHERNET_UNI,
				OMCI_UNI_ENTITY_ID(OMCI_ETHERNET_SLOT, i + 1));
		if (!object)
			continue;
		omci_agent_update_op_state_locked(object, pdus, &count);
		if (!test_bit(i, &in->uni_known))
			continue;

		/* An administratively locked UNI raises no alarms. A spare LAN
		 * port is not a fault while another one carries the service:
		 * LAN LOS is raised only once every UNI has lost its link. */
		alarms = object->data[OMCI_PPTP_ADMIN_STATE_OFFSET] ||
			 in->uni_up ?
			 0 : OMCI_ALARM(OMCI_PPTP_ALARM_LAN_LOS);
		omci_agent_update_alarms_locked(agent, object, alarms, pdus,
						&count);
	}

	return count;
}

static void omci_agent_clear_alarms_locked(struct omci_agent *agent)
{
	struct omci_mib_object *object;
	unsigned long index;

	xa_for_each(&agent->mib, index, object) {
		object->alarms = 0;
		object->arc_clean = false;
	}
	agent->alarm_snapshot_count = 0;
	agent->ber_valid = false;
}

static void omci_agent_alarm_work(struct work_struct *work)
{
	struct omci_agent *agent = container_of(to_delayed_work(work),
						struct omci_agent, alarm_work);
	struct omci_device *odev = container_of(agent, struct omci_device,
						agent);
	u8 pdus[OMCI_ALARM_MAX_PDUS][OMCI_BASELINE_LEN_NO_MIC];
	struct omci_alarm_inputs in = {};
	unsigned int count = 0;
	unsigned int i;
	bool channel_up;

	if (READ_ONCE(odev->userspace_mode))
		return;
	spin_lock_bh(&odev->state_lock);
	channel_up = odev->channel_up;
	spin_unlock_bh(&odev->state_lock);
	if (!channel_up)
		return;

	omci_agent_read_alarm_inputs(odev, &in);
	mutex_lock(&agent->lock);
	if (agent->enabled)
		count = omci_agent_evaluate_alarms_locked(odev, &in, pdus);
	mutex_unlock(&agent->lock);

	for (i = 0; i < count; i++) {
		if (omci_device_xmit(odev, pdus[i], sizeof(pdus[i])))
			break;
		omci_agent_log_wire(odev, "TX", pdus[i], sizeof(pdus[i]));
	}

	if (!READ_ONCE(odev->userspace_mode))
		schedule_delayed_work(&agent->alarm_work, OMCI_ALARM_POLL_INTERVAL);
}

/* Retrieval mode 1 leaves out entities whose alarms ARC holds back. */
static u16 omci_agent_snapshot_alarms_locked(struct omci_agent *agent,
					     u8 mode)
{
	struct omci_mib_object *object;
	unsigned long index;
	u8 count = 0;
	u16 arc_mask;

	xa_for_each(&agent->mib, index, object) {
		if (!object->alarms || !omci_mib_object_active(agent, object))
			continue;
		if (mode == 1 && *omci_alarm_arc(object, &arc_mask))
			continue;
		if (count == OMCI_ALARM_SNAPSHOT_LEN)
			break;
		agent->alarm_snapshot[count].class_id = object->class_id;
		agent->alarm_snapshot[count].entity_id = object->entity_id;
		agent->alarm_snapshot[count].alarms = object->alarms;
		count++;
	}
	agent->alarm_snapshot_count = count;
	agent->alarm_sequence = 0;

	return count;
}

static void omci_agent_alarm_next_locked(struct omci_agent *agent,
					 u16 sequence, u8 *content)
{
	const struct omci_alarm_entry *entry;

	memset(content, 0, 32);
	if (sequence >= agent->alarm_snapshot_count)
		return;

	entry = &agent->alarm_snapshot[sequence];
	put_unaligned_be16(entry->class_id, content);
	put_unaligned_be16(entry->entity_id, content + 2);
	content[4] = entry->alarms;
}

ssize_t omci_agent_show_alarms(struct omci_device *odev, char *buf)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	const char * const *names;
	unsigned long index;
	unsigned int count, i;
	ssize_t len = 0;
	u16 arc_mask;
	char sep;

	mutex_lock(&agent->lock);
	xa_for_each(&agent->mib, index, object) {
		if (!object->alarms)
			continue;
		if (object->class_id == OMCI_CLASS_ANI_G) {
			names = omci_ani_g_alarm_names;
			count = ARRAY_SIZE(omci_ani_g_alarm_names);
		} else {
			names = omci_pptp_alarm_names;
			count = ARRAY_SIZE(omci_pptp_alarm_names);
		}

		len += sysfs_emit_at(buf, len, "%u %#06x %s", object->class_id,
				     object->entity_id,
				     *omci_alarm_arc(object, &arc_mask) ?
				     "arc" : "reported");
		sep = ' ';
		for (i = 0; i < count; i++) {
			if (!(object->alarms & OMCI_ALARM(i)))
				continue;
			len += sysfs_emit_at(buf, len, "%c%s", sep, names[i]);
			sep = ',';
		}
		len += sysfs_emit_at(buf, len, "\n");
	}
	mutex_unlock(&agent->lock);

	return len;
}

/*
 * What the OLT provisioned for traffic management: the traffic descriptor
 * pointers of each GEM port network CTP (attributes 5 and 9) and bridge
 * port (ME 47 attributes 11 and 12), and every ME 280 with its rates in
 * kbit/s. Read-only; nothing here is applied to the hardware.
 */
ssize_t omci_agent_show_traffic(struct omci_device *odev, char *buf)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	unsigned long index;
	ssize_t len = 0;

	mutex_lock(&agent->lock);
	xa_for_each(&agent->mib, index, object) {
		if (!omci_mib_object_active(agent, object))
			continue;
		if (object->class_id == OMCI_CLASS_GEM_PORT_CTP && object->data_len >= 15)
			len += sysfs_emit_at(buf, len,
				"268 %#06x port=%u tcont=%#06x tm=%#06x td_up=%#06x td_down=%#06x\n",
				object->entity_id, get_unaligned_be16(object->data),
				get_unaligned_be16(object->data + 2),
				get_unaligned_be16(object->data + 5),
				get_unaligned_be16(object->data + 7),
				get_unaligned_be16(object->data + 13));
		else if (object->class_id == OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA &&
			 object->data_len >= 23)
			len += sysfs_emit_at(buf, len, "47 %#06x tp_type=%u td_out=%#06x td_in=%#06x\n",
				object->entity_id, object->data[3],
				get_unaligned_be16(object->data + 19),
				get_unaligned_be16(object->data + 21));
		else if (object->class_id == OMCI_CLASS_TRAFFIC_DESCRIPTOR && object->data_len >= 8)
			len += sysfs_emit_at(buf, len, "280 %#06x cir=%u pir=%u kbit/s len=%u\n",
				object->entity_id,
				(u32)DIV_ROUND_UP_ULL(get_unaligned_be32(object->data), 125),
				(u32)DIV_ROUND_UP_ULL(get_unaligned_be32(object->data + 4), 125),
				object->data_len);
	}
	mutex_unlock(&agent->lock);

	return len;
}

static int
omci_agent_build_response_locked(struct omci_device *odev,
				 const struct omci_wire_request *request,
				 u8 *content, size_t capacity,
				 size_t *content_len, bool *unsupported,
				 bool *fake, bool *profile_changed)
{
	struct omci_agent *agent = &odev->agent;
	u8 action = request->message_type & 0x1f;
	u16 class_id = request->class_id;
	u16 entity_id = request->entity_id;
	bool shadow_missing;
	u16 sequence;
	u16 mask = 0;
	u8 result = OMCI_RESULT_SUCCESS;
	int ret;

	if (!capacity)
		return -EMSGSIZE;
	memset(content, 0, capacity);
	shadow_missing = !omci_mib_lookup(agent, class_id, entity_id);
	*unsupported = false;
	*fake = false;
	*profile_changed = false;
	*content_len = 1;

	switch (action) {
	case OMCI_MSG_TYPE_CREATE:
		result = omci_agent_create_locked(odev, class_id, entity_id,
						 request->payload,
						 request->payload_len);
		if (omci_agent_should_fake_result(agent, class_id, result)) {
			result = OMCI_RESULT_SUCCESS;
			*fake = true;
		}
		content[0] = result;
		break;
	case OMCI_MSG_TYPE_DELETE:
		result = omci_agent_delete_locked(odev, class_id, entity_id,
						 profile_changed);
		if (omci_agent_should_fake_result(agent, class_id, result)) {
			result = OMCI_RESULT_SUCCESS;
			*fake = true;
		}
		content[0] = result;
		break;
	case OMCI_MSG_TYPE_SET:
	case OMCI_MSG_TYPE_SET_TABLE:
		result = omci_agent_set_locked(odev, class_id, entity_id, action,
					      request->payload,
					      request->payload_len,
					      profile_changed);
		if (omci_agent_should_fake_result(agent, class_id, result)) {
			result = OMCI_RESULT_SUCCESS;
			*fake = true;
		} else if (!result && omci_agent_fakes_unsupported(agent) &&
			   !agent->permissive && shadow_missing) {
			*fake = true;
		}
		if (request->payload_len >= 2)
			mask = get_unaligned_be16(request->payload);
		if (capacity < 5)
			return -EMSGSIZE;
		content[0] = result;
		put_unaligned_be16(0, content + 1);
		put_unaligned_be16(result ? mask : 0, content + 3);
		*content_len = 5;
		break;
	case OMCI_MSG_TYPE_GET:
	case OMCI_MSG_TYPE_GET_CURRENT_DATA:
		if (action == OMCI_MSG_TYPE_GET_CURRENT_DATA)
			result = omci_agent_get_current_locked(odev, class_id,
					entity_id, request->payload,
					request->payload_len, content,
					capacity, content_len);
		else
			result = omci_agent_get_locked(odev, class_id, entity_id,
						      request->payload,
						      request->payload_len, content,
						      capacity, content_len);
		if (omci_agent_should_fake_result(agent, class_id, result)) {
			content[0] = OMCI_RESULT_SUCCESS;
			*content_len = 1;
			*fake = true;
		} else if (!result && omci_agent_fakes_unsupported(agent) &&
			   !agent->permissive && shadow_missing) {
			*fake = true;
		}
		if (result && !*fake) {
			content[0] = result;
			*content_len = 1;
		}
		break;
	case OMCI_MSG_TYPE_GET_NEXT:
		result = omci_agent_get_next_locked(odev, class_id, entity_id,
						    request->device_id,
						    request->payload,
						    request->payload_len,
						    content, capacity,
						    content_len);
		if (result) {
			content[0] = result;
			*content_len = 1;
		}
		break;
	case OMCI_MSG_TYPE_GET_ALL_ALARMS:
		if (capacity < 2)
			return -EMSGSIZE;
		sequence = omci_agent_snapshot_alarms_locked(agent,
			request->payload_len ? request->payload[0] : 0);
		put_unaligned_be16(sequence, content);
		*content_len = 2;
		break;
	case OMCI_MSG_TYPE_GET_ALL_ALARMS_NEXT:
		if (request->payload_len < 2 || capacity < 32)
			return -EMSGSIZE;
		sequence = get_unaligned_be16(request->payload);
		omci_agent_alarm_next_locked(agent, sequence, content);
		*content_len = 32;
		break;
	case OMCI_MSG_TYPE_MIB_UPLOAD:
		if (capacity < 2)
			return -EMSGSIZE;
		agent->upload_index = 0;
		put_unaligned_be16(omci_mib_upload_count_locked(agent), content);
		*content_len = 2;
		break;
	case OMCI_MSG_TYPE_MIB_UPLOAD_NEXT:
		if (request->payload_len < 2 || capacity < 32)
			return -EMSGSIZE;
		sequence = get_unaligned_be16(request->payload);
		ret = omci_agent_upload_next_locked(agent, sequence, content);
		if (ret)
			memset(content, 0, 32);
		*content_len = 32;
		break;
	case OMCI_MSG_TYPE_MIB_RESET:
		if (omci_agent_mib_reset_locked(odev, false, profile_changed))
			content[0] = OMCI_RESULT_PROCESSING_ERROR;
		else
			content[0] = OMCI_RESULT_SUCCESS;
		break;
	case OMCI_MSG_TYPE_REBOOT:
		content[0] = omci_agent_reboot_locked(odev, class_id, entity_id,
						      request->payload,
						      request->payload_len);
		break;
	case OMCI_MSG_TYPE_SYNC_TIME:
		omci_agent_sync_time_locked(odev, request->payload,
					    request->payload_len);
		content[0] = OMCI_RESULT_SUCCESS;
		break;
	case OMCI_MSG_TYPE_TEST:
		content[0] = OMCI_RESULT_SUCCESS;
		break;
	case OMCI_MSG_TYPE_START_SW_DOWNLOAD:
		agent->sw_download_seen = true;
		fallthrough;
	case OMCI_MSG_TYPE_ACTIVATE_SW:
		if (action == OMCI_MSG_TYPE_ACTIVATE_SW &&
		    class_id == OMCI_CLASS_SOFTWARE_IMAGE) {
			content[0] = omci_agent_activate_sw_locked(odev, entity_id);
			break;
		}
		fallthrough;
	case OMCI_MSG_TYPE_DOWNLOAD_SECTION:
	case OMCI_MSG_TYPE_END_SW_DOWNLOAD:
	case OMCI_MSG_TYPE_COMMIT_SW:
	default:
		dev_info_ratelimited(odev->parent,
				     "OMCI message type %u for class %u instance %#x: %s\n",
				     action, class_id, entity_id,
				     omci_agent_fakes_unsupported(agent) ?
				     "acknowledged, not supported" : "not supported");
		if (omci_agent_fakes_unsupported(agent)) {
			content[0] = OMCI_RESULT_SUCCESS;
			*fake = true;
		} else {
			content[0] = OMCI_RESULT_NOT_SUPPORTED;
			*unsupported = true;
		}
		break;
	}

	return 0;
}

static void omci_agent_reset_duplicate_locked(struct omci_agent *agent)
{
	dev_kfree_skb(agent->last_request);
	dev_kfree_skb(agent->last_response);
	agent->last_request = NULL;
	agent->last_response = NULL;
	agent->last_request_hash = 0;
	agent->last_response_fake = false;
}

static int
omci_agent_cache_exchange_locked(struct omci_agent *agent,
				 const struct sk_buff *request,
				 const void *response, size_t response_len,
				 bool fake)
{
	struct sk_buff *cached_request;
	struct sk_buff *cached_response;

	cached_request = alloc_skb(request->len, GFP_KERNEL);
	if (!cached_request)
		return -ENOMEM;
	skb_put_data(cached_request, request->data, request->len);
	cached_response = alloc_skb(response_len, GFP_KERNEL);
	if (!cached_response) {
		kfree_skb(cached_request);
		return -ENOMEM;
	}
	skb_put_data(cached_response, response, response_len);

	omci_agent_reset_duplicate_locked(agent);
	agent->last_request = cached_request;
	agent->last_response = cached_response;
	agent->last_request_hash = jhash(request->data, request->len, 0);
	agent->last_response_fake = fake;
	return 0;
}

void omci_agent_receive(struct omci_device *odev, const struct sk_buff *skb)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_wire_request request;
	u8 *response = NULL;
	u8 *content = NULL;
	size_t response_len = 0;
	size_t content_len = 0;
	size_t content_capacity;
	u32 request_hash;
	bool unsupported = false;
	bool duplicate = false;
	bool fake = false;
	bool profile_changed = false;
	bool operational_changed = false;
	bool channel_up;
	bool ani_g_test;
	int ret;

	ret = omci_wire_decode(skb->data, skb->len, &request);
	if (ret)
		return;
	omci_agent_log_wire(odev, "RX", skb->data, skb->len);
	ani_g_test = omci_agent_is_ani_g_test(&request);
	request_hash = jhash(skb->data, skb->len, 0);

	response = kmalloc(OMCI_MAX_PDU_LEN, GFP_KERNEL);
	content_capacity = request.device_id == OMCI_BASELINE_DEV_ID ? 32 :
		OMCI_MAX_PDU_LEN - OMCI_EXTENDED_HEADER_LEN;
	content = kzalloc(content_capacity, GFP_KERNEL);
	if (!response || !content)
		goto out;

	mutex_lock(&agent->lock);
	if (!agent->enabled) {
		mutex_unlock(&agent->lock);
		goto out;
	}

	if (agent->last_request && agent->last_response &&
	    agent->last_request_hash == request_hash &&
	    agent->last_request->len == skb->len &&
	    !memcmp(agent->last_request->data, skb->data, skb->len)) {
		response_len = agent->last_response->len;
		memcpy(response, agent->last_response->data, response_len);
		fake = agent->last_response_fake;
		duplicate = true;
	} else {
		ret = omci_agent_build_response_locked(
			odev, &request, content, content_capacity, &content_len,
			&unsupported, &fake, &profile_changed);
		if (!ret)
			ret = omci_wire_encode_response(
				&request, request.message_type & 0x1f,
				content, content_len, response, OMCI_MAX_PDU_LEN,
				&response_len);
		if (ret) {
			mutex_unlock(&agent->lock);
			goto out;
		}
		omci_agent_cache_exchange_locked(agent, skb, response,
						 response_len, fake);
	}
	mutex_unlock(&agent->lock);

	omci_agent_log_wire(odev, "TX", response, response_len);
	ret = omci_device_xmit(odev, response, response_len);
	if (!ret) {
		atomic64_inc(&agent->responses);
		if (!duplicate && !fake && !unsupported) {
			spin_lock_bh(&odev->state_lock);
			channel_up = odev->channel_up;
			spin_unlock_bh(&odev->state_lock);

			mutex_lock(&agent->lock);
			if (channel_up && agent->enabled && !agent->operational) {
				agent->operational = true;
				operational_changed = true;
			}
			mutex_unlock(&agent->lock);
		}
		if (ani_g_test) {
			ret = omci_agent_send_ani_g_test_result(odev, &request);
			if (ret)
				dev_dbg(odev->parent,
					"OMCI ANI-G test result unavailable: %d\n",
					ret);
		}
	}
	if (duplicate)
		atomic64_inc(&agent->duplicates);
	if (unsupported) {
		struct omci_agent_unsupported *u;

		mutex_lock(&agent->lock);
		u = &agent->unsupported_log[agent->unsupported_next++ %
					   ARRAY_SIZE(agent->unsupported_log)];
		u->message_type = request.message_type & 0x1f;
		u->class_id = request.class_id;
		u->entity_id = request.entity_id;
		mutex_unlock(&agent->lock);
		atomic64_inc(&agent->unsupported);
		omci_device_notify(odev, OMCI_EVENT_UNSUPPORTED);
	}
	if (fake)
		atomic64_inc(&agent->fake_responses);
	if (profile_changed)
		omci_device_notify(odev, OMCI_EVENT_PROFILE_CHANGE);
	if (operational_changed)
		omci_agent_report_operational(odev, true);

out:
	kfree(content);
	kfree(response);
}

/*
 * Resume on the retained MIB: reinstall its services and tell whether that
 * yields a datapath, i.e. whether the agent may report itself operational
 * without having received OMCI in this session.
 */
static int omci_agent_resume_retained_locked(struct omci_device *odev)
{
	struct omci_agent *agent = &odev->agent;
	int ret;

	if (!agent->enabled)
		return -EOPNOTSUPP;
	ret = omci_agent_reconcile_services_locked(odev);
	if (ret)
		return ret;
	return xa_empty(&agent->services) ? -ENODATA : 0;
}

static bool omci_mib_has_olt_objects_locked(struct omci_agent *agent)
{
	struct omci_mib_object *object;
	unsigned long index;

	xa_for_each(&agent->mib, index, object)
		if (object->origin == OMCI_MIB_ORIGIN_OLT && !object->pending_delete)
			return true;

	return false;
}

void omci_agent_channel_changed(struct omci_device *odev, bool valid)
{
	struct omci_agent *agent = &odev->agent;
	bool was_operational, operational = false;

	if (READ_ONCE(odev->userspace_mode))
		return;
	mutex_lock(&agent->lock);
	was_operational = agent->operational;
	omci_agent_reset_duplicate_locked(agent);
	omci_agent_reset_table_snapshot_locked(agent);
	agent->upload_index = 0;
	omci_agent_clear_alarms_locked(agent);
	if (!valid) {
		agent->alarm_sequence = 0;
		omci_agent_clear_services_locked(odev);
	} else {
		/*
		 * A MIB without OLT-created objects, e.g. one re-seeded after a
		 * profile or ONU type change, cannot be in sync with the OLT.
		 * A stale non-zero MIB data sync would make the OLT skip the
		 * MIB reset and upload and leave the ONU unprovisioned.
		 */
		if (!omci_mib_has_olt_objects_locked(agent))
			omci_agent_set_mib_sync_locked(agent, 0);

		/*
		 * After an optical outage the OLT re-ranges the ONU but sends
		 * no OMCI while its view of the MIB still matches, so resume
		 * as soon as the retained MIB yields a datapath again instead
		 * of waiting for a request that may never come.  mib_sync is
		 * not a usable signal: the MIB Reset in routine provisioning
		 * clears it while the resulting services stay valid.
		 */
		operational = !omci_agent_resume_retained_locked(odev);
	}
	agent->operational = operational;
	mutex_unlock(&agent->lock);

	if (valid)
		mod_delayed_work(system_wq, &agent->alarm_work,
				 OMCI_ALARM_POLL_INTERVAL);
	if (was_operational != operational)
		omci_agent_report_operational(odev, operational);
}


int omci_agent_put_status(struct sk_buff *msg, struct omci_device *odev)
{
	struct omci_agent *agent = &odev->agent;
	u32 count;
	u32 profile_quirks;
	u16 sync;
	u8 profile_configured;
	u8 profile_effective;
	u8 profile_forced;
	u8 onu_type;
	u8 loid_status;
	bool enabled;
	bool permissive;
	bool fake_omci;
	bool operational;

	mutex_lock(&agent->lock);
	count = omci_mib_count_locked(agent);
	sync = agent->mib_sync;
	enabled = agent->enabled;
	permissive = agent->permissive;
	fake_omci = agent->fake_omci;
	operational = agent->operational;
	profile_configured = agent->config.olt_profile;
	profile_effective = agent->profile_effective;
	profile_forced = agent->config.olt_profile_force;
	onu_type = agent->config.onu_type;
	loid_status = agent->loid_status;
	profile_quirks = agent->profile_quirks;
	mutex_unlock(&agent->lock);

	if (nla_put_u8(msg, OMCI_ATTR_AGENT_ENABLED, enabled) ||
	    nla_put_u8(msg, OMCI_ATTR_AGENT_OPERATIONAL, operational) ||
	    nla_put_u8(msg, OMCI_ATTR_AGENT_PERMISSIVE, permissive) ||
	    nla_put_u8(msg, OMCI_ATTR_AGENT_FAKE_OMCI, fake_omci) ||
	    nla_put_u8(msg, OMCI_ATTR_ONU_TYPE, onu_type) ||
	    nla_put_u8(msg, OMCI_ATTR_LOID_STATUS, loid_status) ||
	    nla_put_u8(msg, OMCI_ATTR_OLT_PROFILE_CONFIGURED,
		       profile_configured) ||
	    nla_put_u8(msg, OMCI_ATTR_OLT_PROFILE_EFFECTIVE,
		       profile_effective) ||
	    nla_put_u8(msg, OMCI_ATTR_OLT_PROFILE_FORCED, profile_forced) ||
	    nla_put_u32(msg, OMCI_ATTR_OLT_PROFILE_QUIRKS, profile_quirks) ||
	    nla_put_u16(msg, OMCI_ATTR_MIB_SYNC, sync) ||
	    nla_put_u32(msg, OMCI_ATTR_MIB_OBJECTS, count) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_AGENT_RESPONSES,
			      atomic64_read(&agent->responses), OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_AGENT_DUPLICATES,
			      atomic64_read(&agent->duplicates), OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_AGENT_UNSUPPORTED,
			      atomic64_read(&agent->unsupported), OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_AGENT_FAKE_RESPONSES,
			      atomic64_read(&agent->fake_responses), OMCI_ATTR_PAD))
		return -EMSGSIZE;
	return 0;
}

int omci_agent_config_get(struct omci_device *odev, u16 key,
			  void *value, size_t *len)
{
	struct omci_agent *agent = &odev->agent;
	const void *source = NULL;
	size_t source_len = 0;
	u8 scalar;

	mutex_lock(&agent->lock);
	switch (key) {
	case OMCI_CONFIG_SERIAL_NUMBER:
		source = agent->config.serial_number;
		source_len = sizeof(agent->config.serial_number);
		break;
	case OMCI_CONFIG_VENDOR_ID:
		source = agent->config.vendor_id;
		source_len = sizeof(agent->config.vendor_id);
		break;
	case OMCI_CONFIG_VERSION:
	case OMCI_CONFIG_HARDWARE_VERSION:
		source = agent->config.version;
		source_len = strnlen(agent->config.version,
				     sizeof(agent->config.version));
		break;
	case OMCI_CONFIG_SOFTWARE_VERSION_0:
		source = agent->config.software_version[0];
		source_len = strnlen(agent->config.software_version[0],
				     sizeof(agent->config.software_version[0]));
		break;
	case OMCI_CONFIG_SOFTWARE_VERSION_1:
		source = agent->config.software_version[1];
		source_len = strnlen(agent->config.software_version[1],
				     sizeof(agent->config.software_version[1]));
		break;
	case OMCI_CONFIG_EQUIPMENT_ID:
		source = agent->config.equipment_id;
		source_len = strnlen(agent->config.equipment_id,
				     sizeof(agent->config.equipment_id));
		break;
	case OMCI_CONFIG_PASSWORD:
		source = agent->config.password;
		source_len = sizeof(agent->config.password);
		break;
	case OMCI_CONFIG_LOID:
		source = agent->config.loid;
		source_len = strnlen(agent->config.loid,
				     sizeof(agent->config.loid));
		break;
	case OMCI_CONFIG_LOID_PASSWORD:
		source = agent->config.loid_password;
		source_len = strnlen(agent->config.loid_password,
				     sizeof(agent->config.loid_password));
		break;
	case OMCI_CONFIG_TRAFFIC_MGMT_OPTION:
		scalar = agent->config.traffic_mgmt_option;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_DBA_STATUS_REPORTING:
		scalar = agent->config.dba_status_reporting;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_VEIP_SLOT:
		scalar = agent->config.veip_slot;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_ONU_TYPE:
		scalar = agent->config.onu_type;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_UNI_COUNT:
		scalar = agent->config.uni_count;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_AGENT_ENABLED:
		scalar = agent->enabled;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_AGENT_PERMISSIVE:
		scalar = agent->permissive;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_AGENT_FAKE_OMCI:
		scalar = agent->fake_omci;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_AGENT_REBOOT:
		scalar = agent->reboot;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_OMCC_VERSION:
		scalar = agent->config.omcc_version;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_OLT_PROFILE:
		scalar = agent->config.olt_profile;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_OLT_PROFILE_FORCE:
		scalar = agent->config.olt_profile_force;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	default:
		mutex_unlock(&agent->lock);
		return -EINVAL;
	}

	if (*len < source_len) {
		*len = source_len;
		mutex_unlock(&agent->lock);
		return -ENOSPC;
	}
	memcpy(value, source, source_len);
	*len = source_len;
	mutex_unlock(&agent->lock);
	return 0;
}

static int
__omci_agent_config_set_source(struct omci_device *odev, u16 key,
			       const void *value, size_t len, u8 source,
			       bool notify)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_identity identity;
	struct omci_olt_g *olt;
	bool profile_changed = false;
	u8 normalized[OMCI_MAX_CONFIG_VALUE];
	size_t normalized_len = sizeof(normalized);
	u8 old_profile_source;
	u8 old_force_source;
	u8 old_profile;
	u8 old_force;
	u8 scalar;
	bool profile_key = false;
	bool operational_changed = false;
	bool changed = false;
	int ret = 0;

	if ((key >= OMCI_CONFIG_SERIAL_NUMBER &&
	     key <= OMCI_CONFIG_PASSWORD) ||
	    key == OMCI_CONFIG_HARDWARE_VERSION ||
	    key == OMCI_CONFIG_SOFTWARE_VERSION_0 ||
	    key == OMCI_CONFIG_SOFTWARE_VERSION_1 ||
	    key == OMCI_CONFIG_LOID ||
	    key == OMCI_CONFIG_LOID_PASSWORD) {
		ret = omci_identity_normalize_config(key, value, len,
						     normalized, &normalized_len);
		if (ret)
			return ret;
		value = normalized;
		len = normalized_len;
	}

	mutex_lock(&agent->lock);
	if (odev->userspace_mode) {
		mutex_unlock(&agent->lock);
		return -EBUSY;
	}
	old_profile = agent->config.olt_profile;
	old_force = agent->config.olt_profile_force;
	old_profile_source = agent->config.olt_profile_source;
	old_force_source = agent->config.olt_profile_force_source;
	switch (key) {
	case OMCI_CONFIG_SERIAL_NUMBER:
		if (len != sizeof(agent->config.serial_number)) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.serial_number, value, len);
		memcpy(agent->config.serial_number, value, len);
		agent->config.serial_source = source;
		memcpy(agent->config.vendor_id, value,
		       sizeof(agent->config.vendor_id));
		agent->config.vendor_source = source;
		break;
	case OMCI_CONFIG_VENDOR_ID:
		if (len != sizeof(agent->config.vendor_id)) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.vendor_id, value, len);
		memcpy(agent->config.vendor_id, value, len);
		agent->config.vendor_source = source;
		memcpy(agent->config.serial_number, value,
		       sizeof(agent->config.vendor_id));
		agent->config.serial_source = max(agent->config.serial_source,
						  source);
		break;
	case OMCI_CONFIG_VERSION:
		if (len != sizeof(agent->config.version)) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.version, value, len) ||
			  memcmp(agent->config.software_version[0], value, len) ||
			  memcmp(agent->config.software_version[1], value, len);
		memcpy(agent->config.version, value, len);
		memcpy(agent->config.software_version[0], value, len);
		memcpy(agent->config.software_version[1], value, len);
		agent->config.version_source = source;
		agent->config.software_version_source[0] = source;
		agent->config.software_version_source[1] = source;
		break;
	case OMCI_CONFIG_HARDWARE_VERSION:
		if (len != sizeof(agent->config.version)) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.version, value, len);
		memcpy(agent->config.version, value, len);
		agent->config.version_source = source;
		break;
	case OMCI_CONFIG_SOFTWARE_VERSION_0:
	case OMCI_CONFIG_SOFTWARE_VERSION_1: {
		unsigned int image = key == OMCI_CONFIG_SOFTWARE_VERSION_0 ? 0 : 1;

		if (len != sizeof(agent->config.software_version[image])) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.software_version[image], value, len);
		memcpy(agent->config.software_version[image], value, len);
		agent->config.software_version_source[image] = source;
		break;
	}
	case OMCI_CONFIG_EQUIPMENT_ID:
		if (!len || len > sizeof(agent->config.equipment_id)) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.equipment_id, value, len) ||
			  memchr_inv(agent->config.equipment_id + len, 0,
				     sizeof(agent->config.equipment_id) - len);
		memset(agent->config.equipment_id, 0,
		       sizeof(agent->config.equipment_id));
		memcpy(agent->config.equipment_id, value, len);
		agent->config.equipment_source = source;
		break;
	case OMCI_CONFIG_PASSWORD:
		if (len != sizeof(agent->config.password)) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.password, value, len);
		memcpy(agent->config.password, value, len);
		agent->config.password_source = source;
		break;
	case OMCI_CONFIG_LOID:
		if (len != sizeof(agent->config.loid)) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.loid, value, len);
		memcpy(agent->config.loid, value, len);
		agent->config.loid_source = source;
		break;
	case OMCI_CONFIG_LOID_PASSWORD:
		if (len != sizeof(agent->config.loid_password)) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.loid_password, value, len);
		memcpy(agent->config.loid_password, value, len);
		agent->config.loid_password_source = source;
		break;
	case OMCI_CONFIG_OLT_PROFILE:
		if (len != sizeof(scalar)) {
			ret = -EINVAL;
			break;
		}
		scalar = *(const u8 *)value;
		if (!omci_profile_valid(scalar)) {
			ret = -EINVAL;
			break;
		}
		changed = agent->config.olt_profile != scalar;
		agent->config.olt_profile = scalar;
		agent->config.olt_profile_source = source;
		profile_key = true;
		break;
	case OMCI_CONFIG_OLT_PROFILE_FORCE:
		if (len != sizeof(scalar)) {
			ret = -EINVAL;
			break;
		}
		scalar = *(const u8 *)value;
		if (!omci_profile_forceable(scalar)) {
			ret = -EINVAL;
			break;
		}
		changed = agent->config.olt_profile_force != scalar;
		agent->config.olt_profile_force = scalar;
		agent->config.olt_profile_force_source = source;
		profile_key = true;
		break;
	case OMCI_CONFIG_AGENT_REBOOT:
		if (len != sizeof(scalar)) {
			ret = -EINVAL;
			break;
		}
		scalar = *(const u8 *)value;
		changed = agent->reboot != !!scalar;
		agent->reboot = !!scalar;
		agent->config.reboot_source = source;
		omci_agent_reset_duplicate_locked(agent);
		break;
	case OMCI_CONFIG_OMCC_VERSION:
		if (len != sizeof(scalar)) {
			ret = -EINVAL;
			break;
		}
		scalar = *(const u8 *)value;
		if (!scalar) {
			ret = -EINVAL;
			break;
		}
		changed = agent->config.omcc_version != scalar;
		agent->config.omcc_version = scalar;
		agent->config.omcc_version_source = source;
		break;
	case OMCI_CONFIG_DBA_STATUS_REPORTING:
		if (len != sizeof(scalar) || *(const u8 *)value > 1) {
			ret = -EINVAL;
			break;
		}
		scalar = *(const u8 *)value;
		changed = agent->config.dba_status_reporting != scalar;
		agent->config.dba_status_reporting = scalar;
		break;
	case OMCI_CONFIG_VEIP_SLOT:
		if (len != sizeof(scalar)) {
			ret = -EINVAL;
			break;
		}
		scalar = *(const u8 *)value;
		if (scalar == OMCI_ETHERNET_SLOT || scalar >= OMCI_GPON_SLOT) {
			ret = -EINVAL;
			break;
		}
		changed = agent->config.veip_slot != scalar;
		agent->config.veip_slot = scalar;
		agent->config.veip_slot_source = source;
		if (changed && omci_agent_mib_veip_slot(agent))
			ret = omci_agent_move_veip_default(odev,
					omci_agent_veip_slot(agent));
		break;
	case OMCI_CONFIG_TRAFFIC_MGMT_OPTION:
	case OMCI_CONFIG_ONU_TYPE:
	case OMCI_CONFIG_UNI_COUNT:
	case OMCI_CONFIG_AGENT_ENABLED:
	case OMCI_CONFIG_AGENT_PERMISSIVE:
	case OMCI_CONFIG_AGENT_FAKE_OMCI:
		if (len != sizeof(scalar)) {
			ret = -EINVAL;
			break;
		}
		scalar = *(const u8 *)value;
		if (key == OMCI_CONFIG_TRAFFIC_MGMT_OPTION) {
			changed = agent->config.traffic_mgmt_option != scalar;
			agent->config.traffic_mgmt_option = scalar;
		} else if (key == OMCI_CONFIG_ONU_TYPE) {
			if (scalar > OMCI_ONU_TYPE_CBU) {
				ret = -EINVAL;
				break;
			}
			changed = agent->config.onu_type != scalar;
			agent->config.onu_type = scalar;
			agent->config.onu_type_source = source;
		} else if (key == OMCI_CONFIG_UNI_COUNT) {
			scalar = clamp_t(u8, scalar, 1, 16);
			changed = agent->config.uni_count != scalar;
			agent->config.uni_count = scalar;
		} else if (key == OMCI_CONFIG_AGENT_ENABLED) {
			changed = agent->enabled != !!scalar;
			agent->enabled = !!scalar;
			if (!agent->enabled && agent->operational) {
				agent->operational = false;
				operational_changed = true;
			}
		} else if (key == OMCI_CONFIG_AGENT_PERMISSIVE) {
			changed = agent->permissive != !!scalar;
			agent->permissive = !!scalar;
		} else {
			changed = agent->fake_omci != !!scalar;
			agent->fake_omci = !!scalar;
		}
		omci_agent_reset_duplicate_locked(agent);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	if (!ret && profile_key) {
		olt = omci_agent_olt_g_locked(agent);
		ret = omci_agent_profile_refresh_locked(odev, olt,
							&profile_changed);
		if (ret) {
			agent->config.olt_profile = old_profile;
			agent->config.olt_profile_force = old_force;
			agent->config.olt_profile_source = old_profile_source;
			agent->config.olt_profile_force_source = old_force_source;
		} else if (!profile_changed) {
			/*
			 * Only the configured value changed, for example "auto"
			 * to the profile "auto" had already detected. The
			 * effective profile and its quirks are the same, so do
			 * not report a change that would restart the session.
			 */
			changed = false;
		}
	}
	if (!ret && changed && !profile_key) {
		omci_agent_refresh_identity_locked(agent);
		omci_agent_reset_duplicate_locked(agent);
		omci_agent_reset_table_snapshot_locked(agent);
		agent->upload_index = 0;
	}
	if (!ret && notify && changed)
		omci_agent_identity_state_locked(agent, &identity);
	mutex_unlock(&agent->lock);
	if (operational_changed)
		omci_agent_report_operational(odev, false);
	if (!ret && notify && changed && odev->ops->config_changed)
		odev->ops->config_changed(odev, key, &identity);
	return ret;
}

int omci_agent_config_set_source(struct omci_device *odev, u16 key,
				 const void *value, size_t len, u8 source)
{
	return __omci_agent_config_set_source(odev, key, value, len, source,
					      false);
}

int omci_agent_config_set(struct omci_device *odev, u16 key,
			  const void *value, size_t len)
{
	return omci_agent_config_set_userspace(odev, key, value, len,
					       OMCI_CONFIG_SOURCE_NETLINK);
}

int omci_agent_config_set_userspace(struct omci_device *odev, u16 key,
				    const void *value, size_t len, u8 source)
{
	if (source != OMCI_CONFIG_SOURCE_NETLINK &&
	    source != OMCI_CONFIG_SOURCE_SYSFS)
		return -EINVAL;

	return __omci_agent_config_set_source(odev, key, value, len, source,
					      true);
}

int omci_agent_config_source_get(struct omci_device *odev, u16 key, u8 *source)
{
	struct omci_agent *agent = &odev->agent;
	int ret = 0;

	if (!source)
		return -EINVAL;
	mutex_lock(&agent->lock);
	switch (key) {
	case OMCI_CONFIG_SERIAL_NUMBER:
		*source = agent->config.serial_source;
		break;
	case OMCI_CONFIG_VENDOR_ID:
		*source = agent->config.vendor_source;
		break;
	case OMCI_CONFIG_VERSION:
	case OMCI_CONFIG_HARDWARE_VERSION:
		*source = agent->config.version_source;
		break;
	case OMCI_CONFIG_SOFTWARE_VERSION_0:
		*source = agent->config.software_version_source[0];
		break;
	case OMCI_CONFIG_SOFTWARE_VERSION_1:
		*source = agent->config.software_version_source[1];
		break;
	case OMCI_CONFIG_EQUIPMENT_ID:
		*source = agent->config.equipment_source;
		break;
	case OMCI_CONFIG_PASSWORD:
		*source = agent->config.password_source;
		break;
	case OMCI_CONFIG_LOID:
		*source = agent->config.loid_source;
		break;
	case OMCI_CONFIG_LOID_PASSWORD:
		*source = agent->config.loid_password_source;
		break;
	case OMCI_CONFIG_ONU_TYPE:
		*source = agent->config.onu_type_source;
		break;
	case OMCI_CONFIG_OLT_PROFILE:
		*source = agent->config.olt_profile_source;
		break;
	case OMCI_CONFIG_OLT_PROFILE_FORCE:
		*source = agent->config.olt_profile_force_source;
		break;
	case OMCI_CONFIG_AGENT_REBOOT:
		*source = agent->config.reboot_source;
		break;
	case OMCI_CONFIG_OMCC_VERSION:
		*source = agent->config.omcc_version_source;
		break;
	case OMCI_CONFIG_VEIP_SLOT:
		*source = agent->config.veip_slot_source;
		break;
	default:
		*source = OMCI_CONFIG_SOURCE_UNSPEC;
		ret = -ENOENT;
		break;
	}
	mutex_unlock(&agent->lock);
	return ret;
}

int omci_agent_mib_get(struct omci_device *odev, u16 class_id, u16 entity_id,
		       struct omci_mib_object *object)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *stored;
	int ret = 0;

	mutex_lock(&agent->lock);
	if (odev->userspace_mode) {
		mutex_unlock(&agent->lock);
		return -EBUSY;
	}
	stored = omci_mib_lookup(agent, class_id, entity_id);
	if (!stored || !omci_mib_object_active(agent, stored))
		ret = -ENOENT;
	else
		memcpy(object, stored, sizeof(*object));
	mutex_unlock(&agent->lock);
	return ret;
}

int omci_agent_mib_set(struct omci_device *odev,
		       const struct omci_mib_object *object)
{
	struct omci_agent *agent = &odev->agent;
	const struct omci_me_desc *desc;
	struct omci_mib_object *stored;
	struct omci_mib_object *local;
	struct omci_mib_object *previous = NULL;
	bool datapath;
	int ret;

	local = kmemdup(object, sizeof(*local), GFP_KERNEL);
	if (!local)
		return -ENOMEM;

	mutex_lock(&agent->lock);
	if (odev->userspace_mode) {
		mutex_unlock(&agent->lock);
		return -EBUSY;
	}
	desc = omci_me_lookup(agent, local->class_id);
	if (!omci_me_active(agent, local->class_id)) {
		ret = -ENOENT;
		goto unlock;
	}
	local->origin = OMCI_MIB_ORIGIN_LOCAL;
	if (local->class_id == OMCI_CLASS_PRIORITY_QUEUE ||
	    local->class_id == OMCI_CLASS_TRAFFIC_SCHEDULER)
		local->qos_configured = true;
	local->owner_profile = desc && (desc->flags & OMCI_ME_F_VENDOR) ?
		agent->profile_effective : OMCI_OLT_PROFILE_UNSPEC;
	if (!local->data_len)
		local->data_len = desc ? desc->data_len : sizeof(local->data);
	if (local->data_len > sizeof(local->data)) {
		ret = -E2BIG;
		goto unlock;
	}
	if (local->class_id == OMCI_CLASS_OLT_G) {
		ret = omci_olt_g_parse_set(local, local->attr_mask, local->data,
					   local->data_len);
		if (ret)
			goto unlock;
	} else if (local->class_id == OMCI_CLASS_VLAN_TAGGING_FILTER_DATA) {
		omci_vlan_filter_parse_create(local, local->data);
	} else if (local->class_id == OMCI_CLASS_EXTENDED_VLAN) {
		omci_ext_vlan_parse_create(local, local->data);
		if (local->attr_mask & OMCI_EXT_VLAN_TABLE_MASK)
			omci_attr_vlan_rule_update(&local->extended_vlan,
						   local->data,
						   sizeof(local->data));
	}

	stored = omci_mib_lookup(agent, local->class_id, local->entity_id);
	if (stored) {
		previous = kmemdup(stored, sizeof(*previous), GFP_KERNEL);
		if (!previous) {
			ret = -ENOMEM;
			goto unlock;
		}
	}
	ret = omci_mib_store_locked(agent, local);
	if (ret)
		goto unlock;
	stored = omci_mib_lookup(agent, local->class_id, local->entity_id);
	if (local->class_id == OMCI_CLASS_OLT_G)
		ret = omci_agent_profile_refresh_locked(odev, &stored->olt_g,
							NULL);
	datapath = omci_agent_datapath_class(agent, local->class_id);
	if (!ret && previous && local->class_id == OMCI_CLASS_GEM_PORT_CTP &&
	    get_unaligned_be16(previous->data) != get_unaligned_be16(local->data))
		ret = omci_agent_gem_rate_locked(odev, previous, true);
	if (!ret && datapath)
		ret = omci_agent_reconcile_services_locked(odev);
	if (ret) {
		if (datapath)
			omci_agent_hw_update(odev, local, OMCI_MSG_TYPE_DELETE, local->data);
		if (previous) {
			*stored = *previous;
			omci_agent_hw_update(odev, stored, OMCI_MSG_TYPE_SET, stored->data);
		} else {
			xa_erase(&agent->mib, omci_mib_key(local->class_id, local->entity_id));
			kfree(stored);
		}
		if (datapath)
			omci_agent_reconcile_services_locked(odev);
	}
	if (!ret)
		omci_agent_reset_duplicate_locked(agent);

unlock:
	mutex_unlock(&agent->lock);
	kfree(previous);
	kfree(local);
	return ret;
}

int omci_agent_mib_delete(struct omci_device *odev, u16 class_id,
			  u16 entity_id)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	bool datapath;
	int ret = 0;

	mutex_lock(&agent->lock);
	if (odev->userspace_mode) {
		mutex_unlock(&agent->lock);
		return -EBUSY;
	}
	object = omci_mib_lookup(agent, class_id, entity_id);
	if (!object) {
		ret = -ENOENT;
		goto unlock;
	}
	object->pending_delete = true;
	datapath = omci_agent_datapath_class(agent, class_id);
	if (datapath)
		ret = omci_agent_reconcile_services_locked(odev);
	if (!ret)
		ret = omci_agent_hw_update(odev, object, OMCI_MSG_TYPE_DELETE, object->data);
	if (ret) {
		object->pending_delete = false;
		omci_agent_hw_update(odev, object, OMCI_MSG_TYPE_SET, object->data);
		if (datapath)
			omci_agent_reconcile_services_locked(odev);
		object = NULL;
		goto unlock;
	}
	xa_erase(&agent->mib, omci_mib_key(class_id, entity_id));
	if (class_id == OMCI_CLASS_OLT_G)
		ret = omci_agent_profile_refresh_locked(odev, NULL, NULL);
	if (!ret && datapath)
		ret = omci_agent_reconcile_services_locked(odev);
	if (!ret)
		omci_agent_reset_duplicate_locked(agent);

unlock:
	mutex_unlock(&agent->lock);
	kfree(object);
	return ret;
}

int omci_agent_mib_reset_result(struct omci_device *odev, bool all)
{
	struct omci_agent *agent = &odev->agent;
	int ret;

	mutex_lock(&agent->lock);
	if (odev->userspace_mode) {
		mutex_unlock(&agent->lock);
		return -EBUSY;
	}
	ret = omci_agent_mib_reset_locked(odev, all, NULL);
	mutex_unlock(&agent->lock);
	return ret;
}

void omci_agent_mib_reset(struct omci_device *odev, bool all)
{
	omci_agent_mib_reset_result(odev, all);
}

int omci_agent_mib_next(struct omci_device *odev, u32 index,
			struct omci_mib_object *object, u32 *next_index,
			const char **name)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *stored;
	unsigned long xa_index = index;
	int ret = 0;

	mutex_lock(&agent->lock);
	if (odev->userspace_mode) {
		mutex_unlock(&agent->lock);
		return -EBUSY;
	}
	for (;;) {
		stored = xa_find(&agent->mib, &xa_index, ULONG_MAX, XA_PRESENT);
		if (!stored) {
			ret = -ENOENT;
			break;
		}
		if (omci_mib_object_active(agent, stored))
			break;
		xa_index++;
	}
	if (!ret) {
		memcpy(object, stored, sizeof(*object));
		*next_index = xa_index + 1;
		*name = omci_me_class_name(stored->class_id);
	}
	mutex_unlock(&agent->lock);
	return ret;
}

int omci_device_reconcile_services(struct omci_device *odev)
{
	struct omci_agent *agent;
	int ret;

	if (!odev)
		return -EINVAL;

	agent = &odev->agent;
	if (READ_ONCE(odev->userspace_mode)) {
		omci_device_notify(odev, OMCI_EVENT_RESOURCE_CHANGE);
		return -EINPROGRESS;
	}
	mutex_lock(&agent->lock);
	ret = odev->userspace_mode ? -EINPROGRESS :
		omci_agent_reconcile_services_locked(odev);
	mutex_unlock(&agent->lock);
	if (ret == -EINPROGRESS)
		omci_device_notify(odev, OMCI_EVENT_RESOURCE_CHANGE);

	return ret;
}
EXPORT_SYMBOL_GPL(omci_device_reconcile_services);

/**
 * omci_device_resume_retained() - resume without waiting for OMCI
 * @odev: OMCI device
 *
 * For a MAC driver's no-OMCI fallback.  The vendor ONU turns WAN traffic on
 * after a bounded wait in O5 even if the OLT sends no OMCI; this is the
 * equivalent for omcid.  If the agent is not operational yet,
 * reinstall the services of the retained MIB and report the agent
 * operational when that yields a datapath.  An OLT which does provision
 * later still starts with a MIB Reset, which rebuilds everything.
 *
 * Return: 0 if the agent is operational, -EOPNOTSUPP if the agent is
 * disabled, -ENODATA if the retained MIB yields no service, or the backend
 * error if reinstalling a service fails.
 */
int omci_device_resume_retained(struct omci_device *odev)
{
	struct omci_agent *agent;
	bool resumed = false;
	int ret = 0;

	if (!odev)
		return -EINVAL;

	agent = &odev->agent;
	mutex_lock(&agent->lock);
	if (!agent->operational) {
		ret = omci_agent_resume_retained_locked(odev);
		resumed = !ret;
		agent->operational = resumed;
	}
	mutex_unlock(&agent->lock);

	if (resumed)
		omci_agent_report_operational(odev, true);
	return ret;
}
EXPORT_SYMBOL_GPL(omci_device_resume_retained);

/* Fresh takeover only: the provider's session lock is held by core.c. */
int omci_agent_handoff(struct omci_device *odev)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	unsigned long index;
	int ret;

	mutex_lock(&agent->lock);
	/* An active OLT MIB must not be silently discarded at a live handoff. */
	xa_for_each(&agent->mib, index, object)
		if (object->origin != OMCI_MIB_ORIGIN_DEFAULT) {
			ret = -EBUSY;
			goto out;
		}
	/* The new owner resets/replays resources only after validating its state. */
	ret = 0;
	agent->enabled = false;
	agent->operational = false;
	agent->pm_running = false;
	omci_agent_reset_duplicate_locked(agent);
	omci_agent_reset_table_snapshot_locked(agent);
	WRITE_ONCE(odev->userspace_mode, true);
out:
	mutex_unlock(&agent->lock);
	if (!ret) {
		cancel_delayed_work_sync(&agent->alarm_work);
		cancel_delayed_work_sync(&agent->pm_work);
		cancel_delayed_work_sync(&agent->reboot_work);
	}
	return ret;
}
