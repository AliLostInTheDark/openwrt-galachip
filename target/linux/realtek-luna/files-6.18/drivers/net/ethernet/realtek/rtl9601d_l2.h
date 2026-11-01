/* SPDX-License-Identifier: GPL-2.0-only */
/* V2802RH L2 offload: what rtl9601d_l2.c and rtl9601d_vlan.c share. */
#ifndef _RTL9601D_L2_H
#define _RTL9601D_L2_H

#include <linux/device.h>
#include <linux/if_vlan.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>

#include "rtl9602c_eth.h"

#define V2802_L2_VLANS 64
#define V2802_L2_RULES (4 * LUNA_L2_SERVICES)

struct v2802_l2_state {
	struct rtl9602c_eth *ep;
	struct delayed_work work;
	struct notifier_block netdev_nb, switchdev_nb, switchdev_blocking_nb;
	struct luna_l2_service service[V2802_L2_VLANS];
	u32 vlan[V2802_L2_VLANS];
	u32 pvid, cfg, flood[3], management_vlan, piso[2], accept;
	s8 pcp[LAN_PORTS];
	u64 marked[3], trapped[3];
	u32 last_reason[3];
	u8 own_mac[2 + LAN_PORTS][ETH_ALEN];
	unsigned int own_macs;
	bool management_saved, isolation_saved;
	unsigned int count, generation, retries;
	int error;
	bool requested, active, saved, gated, stopping;
	/* The bridge ports offloaded: pon0 and the LAN side (eth0, or the jacks). */
	struct net_device *offloaded[2 + LAN_PORTS];
	unsigned int n_offloaded;
	DECLARE_BITMAP(rules, V2802_L2_RULES);
	unsigned int next_rule;
	DECLARE_BITMAP(bridge_vids, VLAN_N_VID);
	DECLARE_BITMAP(netdev_vids, VLAN_N_VID);
	DECLARE_BITMAP(dirty_vids, VLAN_N_VID);
	DECLARE_BITMAP(want_vids, VLAN_N_VID);
	DECLARE_BITMAP(wan_vids, VLAN_N_VID);
	char changed_by[32];	/* the caller behind the latest rebuild */
	struct luna_l2_service plan[V2802_L2_VLANS];	/* under RTNL */
	struct luna_l2_service next_plan[V2802_L2_VLANS];	/* under RTNL */
};

extern struct v2802_l2_state v2802_l2;
extern struct device_attribute dev_attr_l2_port_priority;
extern struct device_attribute dev_attr_l2_vlan_reserved;

int v2802_l2_table(u8 type, u16 addr, u32 *words, unsigned int n, bool write);
struct net_device *v2802_l2_bridge(struct rtl9602c_eth *ep);
u32 v2802_l2_cpu_vlan(bool present);
bool v2802_l2_vlan_reserved(u16 vid);
int v2802_l2_vlan_plan(struct rtl9602c_eth *ep, struct luna_l2_service *services, int count);
void v2802_l2_vlan_ingress(struct rtl9602c_eth *ep, u32 *pvid, u32 *accept);
int v2802_l2_vlan_fallback(struct rtl9602c_eth *ep);

static inline bool v2802_l2_rewritten(const struct luna_l2_service *s)
{
	return s->translated || s->stacked;
}

#endif /* _RTL9601D_L2_H */
