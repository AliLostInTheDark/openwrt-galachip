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

	/* The user ports the agent reports, from the board's omci-uni-ports. */
	u8 uni_count;
};

int omci_validate_tx(struct omci_device *odev, struct sk_buff *skb);
int omci_device_xmit(struct omci_device *odev, const void *data, size_t len);
void omci_device_notify(struct omci_device *odev, u8 event);

int omci_backend_execute(struct omci_device *odev, u16 op, const void *data,
			 size_t len, void *output, size_t *output_len);
void omci_for_each_device(void (*fn)(struct omci_device *odev));

bool omci_profile_valid(u8 profile);
bool omci_profile_forceable(u8 profile);

#endif /* _NET_OMCI_INTERNAL_H */
