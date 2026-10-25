/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LUNA_FLOW_LOGIC_H
#define _LUNA_FLOW_LOGIC_H

#include <linux/types.h>

struct gpon_flow_key;

#define LUNA_FLOW_WORDS 8
#define LUNA_FLOW_NETIF_WORDS 5

enum luna_flow_storage {
	LUNA_FLOW_SRAM,
	LUNA_FLOW_DDR_8K,
	LUNA_FLOW_DDR_16K,
	LUNA_FLOW_DDR_32K,
};

struct luna_flow_hash_config {
	u32 prehash[4]; /* source port, destination port, source IP, destination IP */
	enum luna_flow_storage storage;
	u8 sram_bits;
};

enum luna_flow_nat {
	LUNA_FLOW_ROUTE,
	LUNA_FLOW_SNAT,
	LUNA_FLOW_DNAT,
};

struct luna_flow_vlan {
	u16 vid;
	u8 priority;
	bool tagged;
};

struct luna_flow_path5 {
	u32 translated_dst;
	u16 translated_port;
	enum luna_flow_nat nat;
	struct luna_flow_vlan ctag, stag;
	u8 ingress_if, egress_if, dmac_index;
	u8 tos, ingress_priority, stream_id;
	bool ingress_ctag, ingress_stag, ingress_pppoe;
	bool check_tos, set_stream, locked;
};

enum luna_flow_pppoe {
	LUNA_FLOW_PPPOE_KEEP,
	LUNA_FLOW_PPPOE_ADD,
	LUNA_FLOW_PPPOE_REPLACE,
	LUNA_FLOW_PPPOE_REMOVE,
};

struct luna_flow_netif {
	u32 address, external_ports;
	u16 ports, mtu, pppoe_sid;
	u8 mac[6], ingress_action, egress_action;
	enum luna_flow_pppoe pppoe;
	bool valid, check_mtu, deny_ipv4, deny_ipv6;
};

int luna_flow_netif_encode(const struct luna_flow_netif *entry,
			  u8 port_bits, u8 external_port_bits,
			  u32 words[LUNA_FLOW_NETIF_WORDS]);

u32 luna_flow_entry_count(const struct luna_flow_hash_config *cfg);
int luna_flow_hash(const struct luna_flow_hash_config *cfg,
		   const struct gpon_flow_key *key, u32 extra,
		   u16 *first, u16 *second);
int luna_flow_wan_tag(bool ds_leg, u16 wan_vid, bool pop,
		      struct luna_flow_path5 *action);
int luna_flow_path5_encode(const struct gpon_flow_key *key,
			   const struct luna_flow_path5 *action,
			   u32 words[LUNA_FLOW_WORDS]);

#endif
