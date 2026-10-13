/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_GALACHIP_GC1601_H
#define __LINUX_SOC_GALACHIP_GC1601_H

#include <linux/types.h>

/* OMCI frames on the shared packet engine, owned by the Ethernet driver. */
typedef void (*gc1601_omci_rx_fn)(void *ctx, const u8 *data, unsigned int len);

int gc1601_pe_omci_register(gc1601_omci_rx_fn fn, void *ctx);
void gc1601_pe_omci_unregister(void);
int gc1601_pe_omci_xmit(const u8 *data, unsigned int len, u16 gem_port);

/* PON data frames: downstream delivery and upstream queueing on a GEM port. */
struct napi_struct;
struct net_device;
struct sk_buff;
typedef void (*gc1601_pon_rx_fn)(void *ctx, struct napi_struct *napi,
				 u16 gem_port, const u8 *data, unsigned int len);

int gc1601_pe_pon_register(struct net_device *dev, gc1601_pon_rx_fn fn, void *ctx);
void gc1601_pe_pon_unregister(void);
int gc1601_pe_pon_xmit(struct sk_buff *skb, u16 gem_port, u8 tcont, u8 queue);
bool gc1601_pe_link_up(void);
void gc1601_pe_flush_tcont(unsigned int tcont);
int gc1601_pe_set_uni(bool enable);

/* G.988 UNI attributes from OMCI; see struct omci_uni_attrs. */
struct gc1601_uni_attrs {
	u16 pause_quanta;
	u32 ageing_s;
	u16 learn_limit;
	u32 down_kbps;
	u32 up_kbps;
};

int gc1601_pe_set_uni_attrs(const struct gc1601_uni_attrs *attrs);

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

/* For the GPON DBRu stall fix (xpon_sdk chipset_GMAC_dbru_chk). */
int gc1601_pe_free_buffers(void);
int gc1601_pe_qmu_aful(int level);

#endif
