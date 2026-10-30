/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NET_OMCI_INTERNAL_H
#define _NET_OMCI_INTERNAL_H

#ifdef __KERNEL__
#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>
#include <net/net_namespace.h>
#include <net/xpon/omci.h>
#else
#include "native-platform.h"
#include "device.h"
#endif

#include "me.h"
#include "wire.h"

#define OMCI_RX_QUEUE_LEN		256

struct omci_skb_cb {
	u64 sequence;
	u32 flags;
	u32 generation;
	u16 gem_port_id;
};

#define OMCI_SKB_CB(_skb) ((struct omci_skb_cb *)&((_skb)->cb[0]))

struct omci_mib_object {
	u16 class_id;
	u16 entity_id;
	u16 attr_mask;
	u8 origin;
	u8 owner_profile;
	u8 data_len;
	bool pending_delete;
	bool qos_configured;
	/* First byte of the G.988 alarm bitmap: alarm N is BIT(7 - N). */
	u8 alarms;
	/* Alarm free with ARC set since arc_clean_since. */
	bool arc_clean;
	unsigned long arc_clean_since;
	u8 data[OMCI_MAX_ATTR_DATA];
	struct omci_olt_g olt_g;
	struct omci_vlan_tagging_filter vlan_filter;
	struct omci_extended_vlan extended_vlan;
};

#define OMCI_ALARM_SNAPSHOT_LEN		8

struct omci_alarm_entry {
	u16 class_id;
	u16 entity_id;
	u8 alarms;
};

struct omci_service_state {
	struct omci_service_config config;
};

struct omci_agent_config {
	u8 serial_number[8];
	u8 vendor_id[4];
	u8 version[OMCI_HARDWARE_VERSION_LEN];
	u8 software_version[2][OMCI_SOFTWARE_VERSION_LEN];
	u8 equipment_id[20];
	u8 password[10];
	u8 loid[OMCI_LOID_LEN];
	u8 loid_password[OMCI_LOID_PASSWORD_LEN];
	u8 serial_source;
	u8 vendor_source;
	u8 version_source;
	u8 software_version_source[2];
	u8 omcc_version;
	u8 omcc_version_source;
	u8 equipment_source;
	u8 password_source;
	u8 loid_source;
	u8 loid_password_source;
	u8 traffic_mgmt_option;
	u8 dba_status_reporting;
	u8 veip_slot;
	u8 veip_slot_source;
	u8 onu_type;
	u8 onu_type_source;
	u8 uni_count;
	u8 olt_profile;
	u8 olt_profile_force;
	u8 olt_profile_source;
	u8 olt_profile_force_source;
	u8 reboot_source;
};

struct omci_agent {
	/* Serializes configuration, MIB and transaction state. */
	struct mutex lock;
	struct xarray mib;
	struct xarray services;
	struct omci_agent_config config;
	struct omci_ani_topology topology;
	u32 upload_index;
	u16 mib_sync;
	bool enabled;
	bool permissive;
	bool fake_omci;
	/* Honour an ONU-G Reboot from the OLT */
	bool reboot;
	/* Voice calls in progress, reported by the voice application */
	u8 voice_calls;
	bool sw_download_seen;	/* an image download began since boot */
	/* UNI attributes last handed to the device (set_uni_attrs) */
	struct xarray uni_attrs;
	struct delayed_work reboot_work;
	/* FEC PM: 15-minute intervals counted from the OLT's Synchronize Time */
	struct delayed_work pm_work;
	bool pm_running;
	u8 pm_interval;
	u64 pm_base[5];
	bool operational;
	u8 alarm_sequence;
	struct delayed_work alarm_work;
	/* Get All Alarms snapshot, read back by Get All Alarms Next. */
	struct omci_alarm_entry alarm_snapshot[OMCI_ALARM_SNAPSHOT_LEN];
	u8 alarm_snapshot_count;
	/* BIP error window behind the ANI-G SF and SD alarms. */
	bool ber_valid;
	u64 ber_base;
	unsigned long ber_since;
	/* ONU-G credentials status, written by the OLT after LOID checks. */
	u8 loid_status;
	u8 profile_effective;
	u32 profile_quirks;
	/* VEIP slot the OLT provisioned, kept across MIB resets. */
	u8 veip_learned;

	struct sk_buff *last_request;
	struct sk_buff *last_response;
	u32 last_request_hash;
	bool last_response_fake;
	u8 *table_snapshot;
	size_t table_snapshot_len;
	unsigned long table_snapshot_jiffies;
	u16 table_snapshot_class_id;
	u16 table_snapshot_entity_id;
	u16 table_snapshot_mask;

	atomic64_t responses;
	atomic64_t duplicates;
	atomic64_t unsupported;
	atomic64_t fake_responses;

	/* Last requests answered "not supported", for working out what an OLT wants. */
	struct omci_agent_unsupported {
		u8 message_type;
		u16 class_id;
		u16 entity_id;
	} unsupported_log[32];
	unsigned int unsupported_next;	/* under lock */
};

struct omci_device {
	struct list_head list;
	struct xpon_device *xpon;
	struct device *parent;
	const struct omci_device_ops *ops;
	void *priv;
	u32 id;
	u32 ifindex;
	u32 capabilities;
	/* Serializes provider transport ownership. */
	struct mutex lifecycle_lock;
	bool started;

	/* Protect the optional userspace observer. */
	struct mutex owner_lock;
	struct net *owner_net;
	u32 owner_portid;
	u32 owner_flags;
	/* Set when omcid first binds; only flagged owners may bind after. */
	bool userspace_mode;

	/* Protect channel state accessed from the hardware RX path. */
	spinlock_t state_lock;
	u16 onu_id;
	u16 gem_port_id;
	u32 generation;
	u8 state;
	bool channel_up;

	struct sk_buff_head rx_queue;
	struct work_struct rx_work;
	atomic64_t sequence;
	atomic64_t rx_packets;
	atomic64_t rx_bytes;
	atomic64_t rx_dropped;
	atomic64_t tx_packets;
	atomic64_t tx_bytes;
	atomic64_t tx_errors;
	atomic64_t trace_dropped;

	struct omci_agent agent;
};

int omci_validate_tx(struct omci_device *odev, struct sk_buff *skb);
int omci_device_xmit(struct omci_device *odev, const void *data, size_t len);
void omci_device_notify(struct omci_device *odev, u8 event);
/* Replay the retained MIB into the backend after a restart or re-range. */
int omci_device_resume_retained(struct omci_device *odev);

int omci_agent_init(struct omci_device *odev);
void omci_agent_cleanup(struct omci_device *odev);
int omci_agent_handoff(struct omci_device *odev);
int omci_backend_execute(struct omci_device *odev, u16 op, const void *data,
			 size_t len, void *output, size_t *output_len);
void omci_agent_restage(struct omci_device *odev);
void omci_for_each_device(void (*fn)(struct omci_device *odev));
void omci_agent_receive(struct omci_device *odev, const struct sk_buff *skb);
void omci_agent_channel_changed(struct omci_device *odev, bool valid);
int omci_agent_put_status(struct sk_buff *msg, struct omci_device *odev);
ssize_t omci_agent_show_alarms(struct omci_device *odev, char *buf);
ssize_t omci_agent_show_traffic(struct omci_device *odev, char *buf);
int omci_agent_config_get(struct omci_device *odev, u16 key,
			  void *value, size_t *len);
int omci_agent_config_set(struct omci_device *odev, u16 key,
			  const void *value, size_t len);
int omci_agent_config_set_userspace(struct omci_device *odev, u16 key,
				    const void *value, size_t len, u8 source);
int omci_agent_config_set_source(struct omci_device *odev, u16 key,
				 const void *value, size_t len, u8 source);
int omci_agent_config_source_get(struct omci_device *odev, u16 key, u8 *source);
int omci_identity_normalize_config(u16 key, const void *value, size_t len,
				   u8 *output, size_t *output_len);
int omci_agent_mib_get(struct omci_device *odev, u16 class_id, u16 entity_id,
		       struct omci_mib_object *object);
int omci_agent_mib_set(struct omci_device *odev,
		       const struct omci_mib_object *object);
int omci_agent_mib_delete(struct omci_device *odev, u16 class_id,
			  u16 entity_id);
void omci_agent_mib_reset(struct omci_device *odev, bool all);
int omci_agent_mib_reset_result(struct omci_device *odev, bool all);
int omci_agent_mib_next(struct omci_device *odev, u32 index,
			struct omci_mib_object *object, u32 *next_index,
			const char **name);
int omci_agent_olt_g_get(struct omci_device *odev, struct omci_olt_g *olt);

bool omci_profile_valid(u8 profile);
bool omci_profile_forceable(u8 profile);
u8 omci_profile_detect(const struct omci_olt_g *olt);
u32 omci_profile_quirks(u8 profile);
void omci_profile_sanitize_olt_g(struct omci_olt_g *olt, u32 quirks);
u16 omci_profile_normalize_uni_entity(u8 profile, u16 entity_id);
void omci_profile_normalize_vlan_rule(u8 profile,
				      struct omci_extended_vlan_rule *rule);
int omci_profile_resolve_multicast_ani(u8 profile,
				       u16 bridge_port_entity_id,
				       u16 *ani_entity_id);

int omci_sysfs_init(void);
void omci_sysfs_exit(void);
int omci_sysfs_register(struct omci_device *odev);
void omci_sysfs_unregister(struct omci_device *odev);

#endif /* _NET_OMCI_INTERNAL_H */
