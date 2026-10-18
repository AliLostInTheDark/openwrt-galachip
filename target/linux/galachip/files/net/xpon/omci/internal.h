/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NET_OMCI_INTERNAL_H
#define _NET_OMCI_INTERNAL_H

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <net/net_namespace.h>
#include <net/xpon/omci.h>

#define OMCI_RX_QUEUE_LEN		256

/* OMCI frame formats (G.988 11.2), for checking what omcid sends. */
#define OMCI_BASELINE_DEV_ID		0x0a
#define OMCI_EXTENDED_DEV_ID		0x0b
#define OMCI_BASELINE_LEN		48
#define OMCI_BASELINE_LEN_NO_MIC	44
#define OMCI_EXTENDED_HEADER_LEN	10
#define OMCI_MIC_LEN			4

struct omci_skb_cb {
	u64 sequence;
	u32 flags;
	u32 generation;
	u16 gem_port_id;
};

#define OMCI_SKB_CB(_skb) ((struct omci_skb_cb *)&((_skb)->cb[0]))

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
	u8 onu_type;
	u8 onu_type_source;
	u8 uni_count;
	u8 olt_profile;
	u8 olt_profile_force;
	u8 olt_profile_source;
	u8 olt_profile_force_source;
	u8 reboot_source;
};

/* The configuration omcid starts from; the protocol state lives in omcid. */
struct omci_agent {
	/* Serializes the configuration. */
	struct mutex lock;
	struct omci_agent_config config;
	bool enabled;
	bool permissive;
	bool fake_omci;
	/* Honour an ONU-G Reboot from the OLT */
	bool reboot;
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

int omci_agent_init(struct omci_device *odev);
void omci_agent_cleanup(struct omci_device *odev);
int omci_backend_execute(struct omci_device *odev, u16 op, const void *data,
			 size_t len, void *output, size_t *output_len);
void omci_for_each_device(void (*fn)(struct omci_device *odev));
int omci_agent_put_status(struct sk_buff *msg, struct omci_device *odev);
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

bool omci_profile_valid(u8 profile);
bool omci_profile_forceable(u8 profile);
u32 omci_profile_quirks(u8 profile);

int omci_sysfs_init(void);
void omci_sysfs_exit(void);
int omci_sysfs_register(struct omci_device *odev);
void omci_sysfs_unregister(struct omci_device *odev);

#endif /* _NET_OMCI_INTERNAL_H */
