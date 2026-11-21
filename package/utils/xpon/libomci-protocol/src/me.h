/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NET_OMCI_ME_H
#define _NET_OMCI_ME_H

#include <linux/omci-protocol.h>
#ifdef __KERNEL__
#include <linux/errno.h>
#endif

#define OMCI_BIT(n) (1U << (n))
#define OMCI_GENMASK(h, l) ((~0U << (l)) & (~0U >> (31 - (h))))

struct omci_agent;
struct omci_device;
struct omci_me_class;
struct omci_mib_object;

#define OMCI_CLASS_ONU_DATA		2
#define OMCI_CLASS_CARDHOLDER		5
#define OMCI_CLASS_CIRCUIT_PACK		6
#define OMCI_CLASS_SOFTWARE_IMAGE	7
#define OMCI_CLASS_PPTP_ETHERNET_UNI	11
#define OMCI_CLASS_MAC_BRIDGE_SERVICE_PROFILE	45
#define OMCI_CLASS_MAC_BRIDGE_CONFIG_DATA	46
#define OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA	47
#define OMCI_CLASS_VLAN_TAGGING_FILTER_DATA	84
#define OMCI_CLASS_8021P_MAPPER		130

#define OMCI_CLASS_OLT_G		131
#define OMCI_CLASS_NETWORK_ADDRESS	137
#define OMCI_CLASS_AUTH_METHOD		148
#define OMCI_CLASS_EXTENDED_VLAN	171
#define OMCI_CLASS_ONU_G		256
#define OMCI_CLASS_ONU2_G		257

/*
 * ONU2-G security capability and security mode, G.988 9.1.3: 1 selects
 * AES-128, the only cipher GPON defines.
 */
#define OMCI_ONU2G_SECURITY_AES128	1

/*
 * ONU2-G connectivity capability, G.988 Table 9.1.3-1. One bit per forwarding
 * model: N:1 bridging, 1:M mapping, 1:P filtering, N:M bridge-mapping,
 * 1:MP map-filtering, N:P bridge-filtering and N:MP bridge-map-filtering.
 * The agent builds its datapath out of MAC bridges, GEM interworking TPs and
 * a VEIP, so advertise the full set the way a stock ONU does and let the OLT
 * pick.
 */
#define OMCI_ONU2G_CONNECTIVITY_CAPABILITY	0x007f
#define OMCI_CLASS_TCONT		262
#define OMCI_CLASS_ANI_G		263
#define OMCI_CLASS_UNI_G		264
#define OMCI_CLASS_GEM_IWTP		266
#define OMCI_CLASS_GEM_PORT_CTP	268
#define OMCI_CLASS_GAL_ETHERNET_PROFILE	272
#define OMCI_CLASS_PRIORITY_QUEUE	277
#define OMCI_CLASS_TRAFFIC_SCHEDULER	278
#define OMCI_CLASS_TRAFFIC_DESCRIPTOR	280
#define OMCI_CLASS_FEC_PM		312
#define OMCI_CLASS_MULTICAST_GEM_IWTP	281
#define OMCI_CLASS_MULTICAST_OPERATIONS_PROFILE	309
#define OMCI_CLASS_MULTICAST_SUBSCRIBER_CONFIG	310
#define OMCI_CLASS_OMCI		287
#define OMCI_CLASS_MANAGED_ENTITY	288
#define OMCI_CLASS_ATTRIBUTE		289
#define OMCI_CLASS_VEIP		329

/* G.988 classes with an attribute layout and no ONU-side behaviour yet */
#define OMCI_CLASS_ETHERNET_PM	24
#define OMCI_CLASS_MAC_BRIDGE_PORT_FILTER_TABLE	49
#define OMCI_CLASS_MAC_BRIDGE_PORT_BRIDGE_TABLE	50
#define OMCI_CLASS_MAC_BRIDGE_PM	51
#define OMCI_CLASS_MAC_BRIDGE_PORT_PM	52
#define OMCI_CLASS_PPTP_POTS_UNI	53
#define OMCI_CLASS_VOICE_SERVICE_PROFILE	58
#define OMCI_CLASS_MAC_BRIDGE_PORT_FILTER_PREASSIGN	79
#define OMCI_CLASS_ETHERNET_PM_2	89
#define OMCI_CLASS_ONU_POWER_SHEDDING	133
#define OMCI_CLASS_IP_HOST_CONFIG_DATA	134
#define OMCI_CLASS_TCP_UDP_CONFIG_DATA	136
#define OMCI_CLASS_VOIP_CONFIG_DATA	138
#define OMCI_CLASS_VOIP_VOICE_CTP	139
#define OMCI_CLASS_CALL_CONTROL_PM	140
#define OMCI_CLASS_VOIP_LINE_STATUS	141
#define OMCI_CLASS_VOIP_MEDIA_PROFILE	142
#define OMCI_CLASS_RTP_PROFILE_DATA	143
#define OMCI_CLASS_RTP_PM	144
#define OMCI_CLASS_NETWORK_DIAL_PLAN_TABLE	145
#define OMCI_CLASS_VOIP_APP_SERVICE_PROFILE	146
#define OMCI_CLASS_VOIP_FEATURE_ACCESS_CODES	147
#define OMCI_CLASS_SIP_AGENT_CONFIG_DATA	150
#define OMCI_CLASS_SIP_AGENT_PM	151
#define OMCI_CLASS_SIP_CALL_INIT_PM	152
#define OMCI_CLASS_SIP_USER_DATA	153
#define OMCI_CLASS_LARGE_STRING	157
#define OMCI_CLASS_ONU_REMOTE_DEBUG	158
#define OMCI_CLASS_THRESHOLD_DATA_1	273
#define OMCI_CLASS_THRESHOLD_DATA_2	274
#define OMCI_CLASS_GAL_ETHERNET_PM	276
#define OMCI_CLASS_ETHERNET_PM_3	296
#define OMCI_CLASS_DOT1_RATE_LIMITER	298
#define OMCI_CLASS_MULTICAST_SUBSCRIBER_MONITOR	311
#define OMCI_CLASS_ONU_DYNAMIC_POWER_MGMT	336
#define OMCI_CLASS_TR069_MANAGEMENT_SERVER	340
#define OMCI_CLASS_GEM_PORT_CTP_PM	341
#define OMCI_CLASS_ETHERNET_FRAME_EXT_PM	334
#define OMCI_CLASS_ETHERNET_FRAME_EXT_PM_64	425

#define OMCI_CLASS_HUAWEI_FLOW_MAPPING	350
#define OMCI_CLASS_HUAWEI_351		351
#define OMCI_CLASS_HUAWEI_VLAN_MAPPING	352
#define OMCI_CLASS_HUAWEI_SW_IMAGE_EXT	353
#define OMCI_CLASS_HUAWEI_MULTICAST_367	367
#define OMCI_CLASS_HUAWEI_MULTICAST_370	370
#define OMCI_CLASS_HUAWEI_MULTICAST_373	373
#define OMCI_CLASS_HUAWEI_EXT_ONT_G2	65398
#define OMCI_CLASS_HUAWEI_MULTICAST_65408	65408
#define OMCI_CLASS_HUAWEI_MULTICAST_65414	65414
#define OMCI_CLASS_HUAWEI_EXT_WAN_INFO	65415
#define OMCI_CLASS_HUAWEI_EXT_PPP	65417
#define OMCI_CLASS_HUAWEI_EXT_ONT3_ABILITY	65423
#define OMCI_CLASS_HUAWEI_MULTICAST_65425	65425
#define OMCI_CLASS_HUAWEI_MAINTENANCE	65426
#define OMCI_CLASS_HUAWEI_CAPABILITY	65427
#define OMCI_CLASS_HUAWEI_EXT_TYPE	65430
#define OMCI_CLASS_HUAWEI_EXT_OLT	65431
#define OMCI_CLASS_HUAWEI_EXT_80211	65444
#define OMCI_CLASS_HUAWEI_EXT_MAC	65447
#define OMCI_CLASS_HUAWEI_EXT_80211_IF	65450
#define OMCI_CLASS_HUAWEI_EXT_WAN_CFG	65451
#define OMCI_CLASS_HUAWEI_EXT_WAN_DATA3	65453
#define OMCI_CLASS_HUAWEI_EXT_80211_MGMT	65454

#define OMCI_CLASS_ZTE_PPTP_SCHED	242
#define OMCI_CLASS_ZTE_UNI_SCHED	243
#define OMCI_CLASS_ZTE_EXT_ONU_G	247
#define OMCI_CLASS_ZTE_SNTP		65294
#define OMCI_CLASS_ZTE_IP_FILTER	65305
#define OMCI_CLASS_ZTE_SSID_CONTROL	65314
#define OMCI_CLASS_ZTE_SSID_AUTH	65315

#define OMCI_CLASS_FH_WAN_PROFILE	65321
#define OMCI_CLASS_FH_WAN_CONFIG	65322
#define OMCI_CLASS_FH_PPPOE_CONFIG	65323
#define OMCI_CLASS_FH_WAN_PORT_BIND	65324
#define OMCI_CLASS_FH_WIFI_STA_1	65326
#define OMCI_CLASS_FH_WIFI_STA_2	65327
#define OMCI_CLASS_FH_WIFI_STA_3	65328
#define OMCI_CLASS_FH_WAN_IP_HOST	65329
#define OMCI_CLASS_FH_PON_SWITCHPORT	65333
#define OMCI_CLASS_FH_WAN_EXT_VLAN	65338
#define OMCI_CLASS_FH_OMCI_MODE		65363
#define OMCI_CLASS_FH_WIFI_STATUS	65411

#define OMCI_CLASS_CTC_PPPOE_WAN	250
#define OMCI_CLASS_CTC_VLAN_TRANS	65282
#define OMCI_CLASS_CTC_OPTICAL_INFO	65320
#define OMCI_CLASS_CTC_WAN_SERVICE	65321
#define OMCI_CLASS_CTC_WAN_CONN		65380
#define OMCI_CLASS_CTC_WAN_PROFILE	65381
#define OMCI_CLASS_CTC_WIFI_CFG		65385
#define OMCI_CLASS_CTC_WIFI_SEC		65386
#define OMCI_CLASS_CTC_LOOP_CTRL	65523
#define OMCI_CLASS_CTC_LOOP_DETECT	65528
#define OMCI_CLASS_CTC_ONU_CAP		65529
#define OMCI_CLASS_CTC_LOID_AUTH	65530
#define OMCI_CLASS_CTC_EXT_MCAST	65531

#define OMCI_CLASS_NOKIA_OPTICAL_SUPERVISION	65295
#define OMCI_CLASS_NOKIA_ONT_GENERIC_V2	65296
#define OMCI_CLASS_NOKIA_UNI_SUPPLEMENTAL_V2	65297
#define OMCI_CLASS_NOKIA_GEM_PM_PART2	65300
#define OMCI_CLASS_NOKIA_VLAN_DS_SUPPLEMENTAL	65305
#define OMCI_CLASS_NOKIA_VLAN_US_POLICER	65306

#define OMCI_ME_ACTION_CREATE		OMCI_BIT(4)
#define OMCI_ME_ACTION_DELETE		OMCI_BIT(6)
#define OMCI_ME_ACTION_SET		OMCI_BIT(8)
#define OMCI_ME_ACTION_GET		OMCI_BIT(9)
#define OMCI_ME_ACTION_GET_NEXT		OMCI_BIT(26)
#define OMCI_ME_ACTION_MIB_UPLOAD	OMCI_BIT(13)
#define OMCI_ME_ACTION_MIB_UPLOAD_NEXT	OMCI_BIT(14)
#define OMCI_ME_ACTION_MIB_RESET	OMCI_BIT(15)
#define OMCI_ME_ACTION_TEST		OMCI_BIT(18)
#define OMCI_ME_ACTION_GET_CURRENT_DATA	OMCI_BIT(28)
#define OMCI_ME_ACTION_SET_TABLE	OMCI_BIT(29)

#define OMCI_ME_F_NO_MIB_UPLOAD	OMCI_BIT(0)
#define OMCI_ME_F_ONU_CREATED		OMCI_BIT(1)
#define OMCI_ME_F_DATAPATH		OMCI_BIT(2)
#define OMCI_ME_F_SAFE_FAKE		OMCI_BIT(3)
#define OMCI_ME_F_VENDOR		OMCI_BIT(4)

#define OMCI_PROFILE_BIT(_profile)	OMCI_BIT(_profile)
#define OMCI_PROFILE_STANDARD_MASK	OMCI_GENMASK(7, 1)

enum omci_attr_access {
	OMCI_ATTR_ACCESS_READ		= OMCI_BIT(0),
	OMCI_ATTR_ACCESS_WRITE		= OMCI_BIT(1),
	OMCI_ATTR_ACCESS_SET_CREATE	= OMCI_BIT(2),
};

struct omci_attr_desc {
	__u16 mask;
	__u16 offset;
	__u8 len;
	__u8 access;
};

struct omci_me_desc {
	__u16 class_id;
	const char *name;
	__u32 profile_mask;
	__u32 actions;
	__u32 flags;
	__u16 valid_attr_mask;
	__u16 mib_upload_mask;
	__u16 data_len;
	__u8 category;
	__u8 support;
	const struct omci_attr_desc *attrs;
	unsigned int num_attrs;
};

const struct omci_me_desc *omci_me_lookup_profile(__u8 profile, __u16 class_id);
bool omci_me_active_profile(__u8 profile, __u16 class_id);
bool omci_me_action_allowed_profile(__u8 profile, __u16 class_id,
				    __u8 message_type);

/* Decode into a staged object; a later invalid attribute can follow valid ones. */
int omci_me_decode_create_data(const struct omci_me_desc *desc,
			       __u8 *object_data, __u16 *object_mask,
			       const __u8 *data, size_t len);
int omci_me_decode_set_data(const struct omci_me_desc *desc,
			    __u8 *object_data, __u16 *object_mask, __u16 mask,
			    const __u8 *data, size_t len);
int omci_me_encode_data(const struct omci_me_desc *desc,
			const __u8 *object_data,
			__u16 mask, __u8 *data, size_t len,
			__u16 *encoded_mask, size_t *encoded_len);
#ifdef __KERNEL__
static inline const char *omci_me_class_name(__u16 class_id)
{
	return NULL;
}
#else
const char *omci_me_class_name(__u16 class_id);
#endif
int omci_me_class_get_profile(__u8 profile, __u16 class_id,
			      struct omci_me_class *class);
int omci_me_class_next_profile(__u8 profile, __u32 index,
			       struct omci_me_class *class, __u32 *next_index);
unsigned int omci_me_attr_count_profile(__u8 profile);
int omci_me_attr_id_profile(__u8 profile, __u16 class_id,
			    unsigned int attr_index, __u16 *attr_id);
int omci_me_attr_get_profile(__u8 profile, __u16 attr_id,
			     __u16 *class_id, unsigned int *attr_index,
			     const struct omci_me_desc **desc,
			     const struct omci_attr_desc **attr);

#if defined(__KERNEL__) || defined(OMCI_NATIVE_ENGINE)
const struct omci_me_desc *omci_me_lookup(const struct omci_agent *agent,
					  __u16 class_id);
bool omci_me_active(const struct omci_agent *agent, __u16 class_id);
bool omci_me_action_allowed(const struct omci_agent *agent, __u16 class_id,
			    __u8 message_type);

int omci_me_decode_create(const struct omci_me_desc *desc,
			  struct omci_mib_object *object,
			  const __u8 *data, size_t len);
int omci_me_decode_set(const struct omci_me_desc *desc,
		       struct omci_mib_object *object, __u16 mask,
		       const __u8 *data, size_t len);
int omci_me_encode_attributes(const struct omci_me_desc *desc,
			      const struct omci_mib_object *object,
			      __u16 mask, __u8 *data, size_t len,
			      __u16 *encoded_mask, size_t *encoded_len);

#ifdef __KERNEL__
static inline int omci_me_class_get(struct omci_device *odev, __u16 class_id,
				    struct omci_me_class *class)
{
	return -EOPNOTSUPP;
}

static inline int omci_me_class_next(struct omci_device *odev, __u32 index,
				     struct omci_me_class *class, __u32 *next_index)
{
	return -EOPNOTSUPP;
}
#else
int omci_me_class_get(struct omci_device *odev, __u16 class_id,
		      struct omci_me_class *class);
int omci_me_class_next(struct omci_device *odev, __u32 index,
		       struct omci_me_class *class, __u32 *next_index);
#endif
int omci_me_attr_id(const struct omci_agent *agent, __u16 class_id,
		    unsigned int attr_index, __u16 *attr_id);
int omci_me_attr_get(const struct omci_agent *agent, __u16 attr_id,
		     __u16 *class_id, unsigned int *attr_index,
		     const struct omci_me_desc **desc,
		     const struct omci_attr_desc **attr);
unsigned int omci_me_attr_count(const struct omci_agent *agent);
#endif

#ifndef __KERNEL__
#define OMCI_OLT_PROFILES_DEFAULT_PATH	"/etc/omci/olt-profiles.conf"
#define OMCI_OLT_PROFILE_CUSTOM_BASE	64
#define OMCI_OLT_PROFILE_CUSTOM_MAX	16

int omci_profiles_load_file(const char *path);
void omci_profiles_clear_custom(void);
unsigned int omci_profiles_custom_count(void);
#endif

#endif /* _NET_OMCI_ME_H */
