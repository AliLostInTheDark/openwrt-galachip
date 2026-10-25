// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include "gpon_flow.h"
#include "luna_flow_logic.h"

static u32 low_mask(unsigned int bits)
{
	return (1u << bits) - 1;
}

static u32 preprocess(u32 value, u32 pattern, unsigned int bits)
{
	u32 mask = low_mask(bits), low = value & mask;
	unsigned int shift = (pattern >> bits) & 7;

	if (pattern & (1u << (bits + 3)))
		shift = bits - shift;
	low = ((low >> shift) | (low << (bits - shift))) & mask;
	return (value & ~mask) | ((low ^ pattern) & mask);
}

u32 luna_flow_entry_count(const struct luna_flow_hash_config *cfg)
{
	if (!cfg || (cfg->sram_bits != 10 && cfg->sram_bits != 12) ||
	    cfg->storage < LUNA_FLOW_SRAM || cfg->storage > LUNA_FLOW_DDR_32K)
		return 0;
	return 1u << (cfg->storage ? 12 + cfg->storage : cfg->sram_bits);
}

static bool ipv4_tcp_udp(const struct gpon_flow_key *key)
{
	return key && !key->ip_ver &&
	       (key->ip_protocol == 6 || key->ip_protocol == 17);
}

int luna_flow_netif_encode(const struct luna_flow_netif *e,
			  u8 port_bits, u8 external_port_bits,
			  u32 w[LUNA_FLOW_NETIF_WORDS])
{
	u64 ingress;

	if (!e || !w || !((port_bits == 6 && external_port_bits == 6) ||
			  (port_bits == 11 && external_port_bits == 18)))
		return -EINVAL;
	if (e->ports > low_mask(port_bits) ||
	    e->external_ports > low_mask(external_port_bits) ||
	    e->mtu >= 16384 || e->ingress_action > 2 || e->egress_action > 2 ||
	    e->pppoe < LUNA_FLOW_PPPOE_KEEP || e->pppoe > LUNA_FLOW_PPPOE_REMOVE)
		return -EINVAL;
	ingress = e->ports | ((u64)e->external_ports << port_bits);
	w[0] = e->address;
	w[1] = ((u32)e->mac[2] << 24) | ((u32)e->mac[3] << 16) |
	       ((u32)e->mac[4] << 8) | e->mac[5];
	w[2] = ((u32)e->mac[0] << 8) | e->mac[1] | ((u32)e->valid << 16) |
	       ((u32)e->mtu << 17) | ((u32)e->check_mtu << 31);
	w[3] = e->pppoe_sid | ((u32)e->pppoe << 16) |
	       ((u32)e->deny_ipv4 << 18) | ((u32)e->deny_ipv6 << 19) |
	       ((u32)e->ingress_action << 20) | ((u32)e->egress_action << 22) |
	       ((u32)ingress << 24);
	w[4] = ingress >> 8;
	return 0;
}

int luna_flow_hash(const struct luna_flow_hash_config *cfg,
		   const struct gpon_flow_key *key, u32 extra,
		   u16 *first, u16 *second)
{
	u32 count = luna_flow_entry_count(cfg), addr[2], sum, index, mask;
	unsigned int bits, i;

	if (!count || !first || !second || first == second || extra > 0xffffff)
		return -EINVAL;
	if (!ipv4_tcp_udp(key))
		return -EOPNOTSUPP;
	addr[0] = preprocess(key->ip_sa, cfg->prehash[2], 20);
	addr[1] = preprocess(key->ip_da, cfg->prehash[3], 20);
	sum = preprocess(key->l4_sport, cfg->prehash[0], 16) +
	      preprocess(key->l4_dport, cfg->prehash[1], 16);
	for (i = 0; i < 2; i++)
		sum += (addr[i] & 0xfffff) + (addr[i] >> 20);
	for (i = 0; i < 2; i++)
		sum = (sum & 0xfffff) + (sum >> 20);

	mask = cfg->sram_bits == 10 ? 0x7f83 : 0x7e03;
	if (cfg->storage == LUNA_FLOW_SRAM) {
		bits = cfg->sram_bits - 2;
		index = 0;
		for (i = 0; i < 24; i += bits)
			index += (sum >> i) & low_mask(bits);
		for (i = 0; i < 24; i += bits)
			index ^= (extra >> i) & low_mask(bits);
		index &= low_mask(bits);
		*first = index << 2;
		*second = ((index ^ mask) & low_mask(bits)) << 2;
	} else {
		bits = 12 + cfg->storage;
		sum = (sum & low_mask(bits)) + (sum >> bits);
		index = (sum & low_mask(bits)) ^
			((sum >> cfg->sram_bits) & low_mask(bits - cfg->sram_bits));
		index ^= (extra & low_mask(bits)) ^ (extra >> bits);
		*first = index;
		*second = (index ^ mask) & (count - 1);
	}
	return 0;
}

/* ONE 802.1Q tag on the WAN is per-flow data in the path5 entry: the upstream
 * leg egresses tagged with the WAN VID, the downstream leg matches the tagged
 * frame the rule pops.  A pop and the provisioned WAN must agree. */
int luna_flow_wan_tag(bool ds_leg, u16 wan_vid, bool pop,
		      struct luna_flow_path5 *a)
{
	if (!a || wan_vid >= 4095)
		return -EINVAL;
	if (ds_leg ? pop != !!wan_vid : pop)
		return -EOPNOTSUPP;
	a->ingress_ctag = ds_leg && pop;
	a->ctag.vid = ds_leg ? 0 : wan_vid;
	a->ctag.tagged = !ds_leg && wan_vid;
	return 0;
}

int luna_flow_path5_encode(const struct gpon_flow_key *key,
			   const struct luna_flow_path5 *a,
			   u32 w[LUNA_FLOW_WORDS])
{
	if (!a || !w || a->nat < LUNA_FLOW_ROUTE || a->nat > LUNA_FLOW_DNAT ||
	    a->ingress_if >= 16 || a->egress_if >= 16 || a->stream_id >= 128 ||
	    a->ingress_priority >= 8 || a->ctag.priority >= 8 ||
	    a->stag.priority >= 8 || a->ctag.vid >= 4096 || a->stag.vid >= 4096)
		return -EINVAL;
	if (!ipv4_tcp_udp(key))
		return -EOPNOTSUPP;
	w[0] = 1 | (2u << 2) | ((u32)a->ingress_if << 4) |
	       ((u32)a->set_stream << 8) | ((u32)a->ingress_ctag << 17) |
	       ((u32)a->ingress_stag << 18) | ((u32)a->ingress_pppoe << 19) |
	       ((u32)a->egress_if << 20) | ((u32)a->tos << 24);
	w[1] = key->ip_sa;
	/* DNAT matches the interface gateway IP; word 2 carries the replacement. */
	w[2] = a->nat == LUNA_FLOW_DNAT ? a->translated_dst : key->ip_da;
	w[3] = key->l4_sport | ((u32)key->l4_dport << 16);
	w[4] = a->dmac_index | ((u32)a->stag.tagged << 8) |
	       ((u32)a->ctag.tagged << 12) |
	       ((u32)(a->nat == LUNA_FLOW_ROUTE ? 0 : a->translated_port) << 16);
	w[5] = a->stag.tagged | ((u32)a->stag.priority << 1) |
	       ((u32)a->stag.vid << 4) | ((u32)a->ctag.tagged << 16) |
	       ((u32)a->ctag.priority << 17) | ((u32)a->ctag.vid << 20);
	w[6] = (a->nat == LUNA_FLOW_SNAT) | ((u32)a->stream_id << 1) |
	       ((u32)(a->nat != LUNA_FLOW_ROUTE) << 8) |
	       (1u << 17) | ((u32)a->stag.tagged << 18) |
	       (1u << 19) | (1u << 20) |
	       ((u32)(key->ip_protocol == 6) << 26) |
	       ((u32)a->check_tos << 27) | ((u32)a->locked << 28) |
	       ((u32)a->ingress_priority << 29);
	w[7] = 0;
	return 0;
}
