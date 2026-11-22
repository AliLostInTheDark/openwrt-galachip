/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_GALACHIP_GC1601_H
#define __LINUX_SOC_GALACHIP_GC1601_H

#include <linux/if_vlan.h>
#include <linux/types.h>

/* OMCI frames on the shared packet engine, owned by the Ethernet driver. */
typedef void (*gc1601_omci_rx_fn)(void *ctx, const u8 *data, unsigned int len);

int gc1601_pe_omci_register(gc1601_omci_rx_fn fn, void *ctx);
void gc1601_pe_omci_unregister(void);
int gc1601_pe_omci_xmit(const u8 *data, unsigned int len, u16 gem_port);

/*
 * Switch ports, numbered as the RX descriptor reports the ingress port.
 * The DSA switch driver uses the same numbers; the CPU port's is ours.
 */
#define GC1601_PORT_PON		0
#define GC1601_PORT_LAN		1
#define GC1601_PORT_CPU		2
#define GC1601_PORTS		3

/* The LAN port as the DSA switch driver drives it. */
struct phy_device;
int gc1601_pe_lan_attach(struct phy_device *phydev);
void gc1601_pe_lan_detach(void);
void gc1601_pe_lan_link_up(int speed, int duplex, bool tx_pause, bool rx_pause);
void gc1601_pe_lan_link_down(void);

/* PON data frames: downstream delivery and upstream queueing on a GEM port. */
struct napi_struct;
struct net_device;
struct sk_buff;

/* Where an upstream frame goes: its GEM port, T-CONT and queue. */
struct gc1601_pon_meta {
	u16 gem_port;
	u8 tcont;
	u8 queue;
	bool omci;
};

/*
 * The PON driver's side of the pon0 port. @rx builds the skb for a
 * downstream frame, or returns NULL to drop it; the packet engine delivers
 * it. @prepare rewrites an upstream frame for the PON and picks its GEM
 * port, or fails to drop it. @link returns the line rate, 0 while the ONU
 * is not registered.
 */
struct gc1601_pon_port_ops {
	struct sk_buff *(*rx)(void *ctx, struct napi_struct *napi, u16 gem_port,
			      const u8 *data, unsigned int len);
	int (*prepare)(void *ctx, struct sk_buff **skb, struct gc1601_pon_meta *meta);
	int (*link)(void *ctx);
	int (*get_sset_count)(void *ctx);
	void (*get_strings)(void *ctx, u8 *data);
	void (*get_stats)(void *ctx, u64 *data);
};

int gc1601_pe_pon_register(struct net_device *dev, const struct gc1601_pon_port_ops *ops,
			   void *ctx);
void gc1601_pe_pon_unregister(void);
int gc1601_pe_pon_xmit(struct sk_buff *skb, const struct gc1601_pon_meta *meta);
int gc1601_pe_pon_link(void);
int gc1601_pe_pon_sset_count(void);
void gc1601_pe_pon_strings(u8 *data);
void gc1601_pe_pon_stats(u64 *data);
bool gc1601_pe_link_up(void);
void gc1601_pe_flush_tcont(unsigned int tcont);
/* Largest frame: 2000 in the packet pool less the fabric's 34-byte margin. */
#define GC1601_MAX_FRAME	1966
/* Host MTU that still fits a QinQ frame (RFC 4638 PPPoE needs 1508). */
#define GC1601_MAX_MTU		(GC1601_MAX_FRAME - VLAN_ETH_HLEN - VLAN_HLEN - ETH_FCS_LEN)

int gc1601_pe_clear_tcont(unsigned int tcont);
int gc1601_pe_enable_tcont(unsigned int tcont);
int gc1601_pe_set_uni(bool enable);

/* The service's VLAN on the LAN side; the PON side carries vlan_id. */
enum gc1601_sfu_lan {
	GC1601_SFU_LAN_SAME,		/* vlan_id on both sides */
	GC1601_SFU_LAN_UNTAGGED,	/* push vlan_id up, pop it down */
	GC1601_SFU_LAN_TRANSLATE,	/* lan_vid <-> vlan_id */
	GC1601_SFU_LAN_STACKED,		/* push vlan_id over lan_vid up, pop down */
};

#define GC1601_SFU_FLOWS	16	/* services the switch offload takes */

/* Switch offload: an upstream and a downstream flow per service. */
struct gc1601_sfu_flow {
	u16 vlan_id;
	u16 gem_port;
	u8 tcont;
	u8 queue;
	bool untagged;		/* untagged on the PON too, vlan_id unused */
	u8 lan;			/* enum gc1601_sfu_lan */
	u16 lan_vid;
	u8 pcp;			/* upstream P-bit when pcp_match */
	bool pcp_match;		/* upstream matches VLAN and P-bit */
	bool down_only;		/* no upstream flow: another service owns it */
	bool up_only;		/* downstream GEM/VLAN rule already exists */
};

int gc1601_pe_sfu_set(const struct gc1601_sfu_flow *flows, unsigned int n);
bool gc1601_pe_sfu_same_key(const struct gc1601_sfu_flow *a,
			    const struct gc1601_sfu_flow *b, bool upstream);
int gc1601_pe_sfu_normalize(struct gc1601_sfu_flow *flows, unsigned int n);

/* For recovering a GPON MAC whose DBRu reports stall. */
int gc1601_pe_free_buffers(void);
int gc1601_pe_qmu_aful(int level);

#endif
