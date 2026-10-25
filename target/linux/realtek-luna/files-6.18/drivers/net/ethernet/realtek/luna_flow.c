// SPDX-License-Identifier: GPL-2.0-only
/* FLOWBASED backend, included by the family NIC shell. */
#if IS_ENABLED(CONFIG_LUNA_FLOWOFFLOAD)
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <net/pkt_cls.h>
#include "gpon_edge.h"
#include "gpon_flow_block.h"
#include "gpon_flow_offload.h"
#include "luna_flow_sram.h"
#include "luna_flow_mac.h"

static bool hw_nat = true;
module_param(hw_nat, bool, 0444);
MODULE_PARM_DESC(hw_nat, "Enable hardware flow offload at boot");

struct luna_flow_priv {
	struct luna_flow_slot slot;
	struct luna_flow_mac_ref mac;
	unsigned long last_hit;
};

struct luna_flow_engine {
	struct luna_flow_sram sram;
	struct luna_flow_mac_table macs;
	struct luna_flow_local_mac local[2];
	struct gpon_flow_offload *fo;
	struct gpon_edge edge;
	bool initialized, provisioned;
	unsigned long hits_at;	/* jiffies of the last hit-bit sweep */
#if IS_ENABLED(CONFIG_GPON_FLOW_DIAG)
	struct proc_dir_entry *proc;
	u32 offered, hits;
#endif
};

static DEFINE_MUTEX(luna_flow_mutex);
static LIST_HEAD(luna_flow_blocks);
static const struct net_device_ops luna_eth_netdev_ops, luna_eth_wan_ops;

static u32 luna_flow_rd(struct luna_eth *ep, u32 offset)
{
	return readl(ep->flow->sram.sw + offset);
}

static void luna_flow_wr(struct luna_eth *ep, u32 offset, u32 value)
{
	writel(value, ep->flow->sram.sw + offset);
}

static struct luna_eth *luna_flow_eth(struct net_device *dev)
{
	if (dev->netdev_ops == &luna_eth_netdev_ops)
		return netdev_priv(dev);
	if (dev->netdev_ops == &luna_eth_wan_ops)
		return *(struct luna_eth **)netdev_priv(dev);
	return NULL;
}

static bool luna_flow_lan(void *sh, struct net_device *dev)
{
	struct luna_eth *ep = sh;

	return dev && dev != ep->wan_ndev;
}

static int luna_flow_reset(struct luna_eth *ep)
{
	u32 mask = BIT(0) | BIT(1) | BIT(3) | BIT(5) |
		   BIT(8) | BIT(10) | BIT(12);
	unsigned int i;

	luna_flow_wr(ep, 0x801104, mask);
	for (i = 0; i < 2000; i++) {
		if (!(luna_flow_rd(ep, 0x801104) & mask))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static int luna_flow_initialize(struct luna_eth *ep)
{
	struct luna_flow_engine *e = ep->flow;
	u32 v;
	int ret;
	unsigned int i;

	if (e->initialized)
		return 0;
	ret = luna_flow_reset(ep);
	if (ret)
		return ret;
	/* SRAM publication has no external DMA buffer or cache controller. */
	luna_flow_wr(ep, 0x801000, luna_flow_rd(ep, 0x801000) & ~3u);
	for (i = 0; i < 4; i++)
		luna_flow_wr(ep, 0x801010 + 4 * i, e->sram.hash.prehash[i]);
	v = luna_flow_rd(ep, 0x801008);
	v &= ~(BIT(25) | BIT(24) | BIT(22) | BIT(16));
	v |= BIT(31) | BIT(27) | BIT(24) | BIT(21) | BIT(20) |
	     BIT(19) | BIT(18) | BIT(17) | BIT(9) | BIT(1);
	luna_flow_wr(ep, 0x801008, v);
	v = luna_flow_rd(ep, 0x80100c) & ~(BIT(16) | (3u << 13) | (3u << 8));
	luna_flow_wr(ep, 0x80100c, v | BIT(13) | BIT(8));
	/* Preserve ordinary LAN switching; unresolved routed destinations trap. */
	luna_flow_wr(ep, 0x17000, luna_flow_rd(ep, 0x17000) | BIT(29) | BIT(25));
	e->initialized = true;
	return 0;
}

/* Every flow's stats call ran this whole-table sweep under luna_flow_mutex, so N offloaded
 * flows meant N sweeps a second: the waiting stats works piled up 48 kworkers that never left
 * (G24W, 2026-10-12, 200 TCP/s). One sweep serves every flow for HZ/4.
 */
static void luna_flow_hits(struct luna_eth *ep)
{
	struct luna_flow_engine *e = ep->flow;
	unsigned int count = luna_flow_entry_count(&e->sram.hash), base, bit;

	if (e->hits_at && time_before(jiffies, e->hits_at + HZ / 4))
		return;
	e->hits_at = jiffies;
	for (base = 0; base < count; base += 32) {
		u32 hits = luna_flow_rd(ep, 0x800000 + base / 8);

		for (bit = 0; bit < 32; bit++) {
			struct luna_flow_slot *s = e->sram.owner[base + bit];
			struct luna_flow_priv *p;

			if (!s || !(hits & BIT(bit)))
				continue;
			p = container_of(s, struct luna_flow_priv, slot);
			p->last_hit = jiffies;
#if IS_ENABLED(CONFIG_GPON_FLOW_DIAG)
			e->hits++;
#endif
		}
	}
}

static int luna_flow_provision(struct luna_eth *ep, const struct gpon_edge *edge)
{
	struct luna_flow_engine *e = ep->flow;
	struct luna_flow_netif n = {
		.valid = true, .check_mtu = true, .deny_ipv6 = true,
	};
	u32 words[2][LUNA_FLOW_NETIF_WORDS];
	unsigned int i;
	int ret;

	n.address = edge->lan_ip;
	/* SPA is checked against the egress NETIF's allowed ingress ports. */
	n.ports = BIT(ep->c->sw_map->pon_port);
	n.mtu = ep->ndev->mtu;
	n.pppoe = edge->wan_pppoe_sid ? LUNA_FLOW_PPPOE_REMOVE : LUNA_FLOW_PPPOE_KEEP;
	ether_addr_copy(n.mac, edge->lan_mac);
	ret = luna_flow_netif_encode(&n, ep->c->flow->port_bits,
				     ep->c->flow->external_port_bits, words[0]);
	if (ret)
		return ret;
	n.address = edge->wan_ip;
	n.ports = ep->lan_flood_mask;
	n.mtu = ep->wan_ndev->mtu - (edge->wan_pppoe_sid ? 8 : 0);
	n.pppoe_sid = edge->wan_pppoe_sid;
	n.pppoe = edge->wan_pppoe_sid ? LUNA_FLOW_PPPOE_ADD : LUNA_FLOW_PPPOE_KEEP;
	ether_addr_copy(n.mac, edge->wan_mac);
	ret = luna_flow_netif_encode(&n, ep->c->flow->port_bits,
				     ep->c->flow->external_port_bits, words[1]);
	if (ret)
		return ret;
	e->provisioned = false;
	for (i = 0; i < 2; i++) {
		ret = luna_flow_local_put(&e->macs, &e->local[i]);
		if (ret)
			return ret;
	}
	for (i = 0; i < 2; i++) {
		ret = luna_l34_tbl_op(e->sram.sw, e->sram.acc, 0, i, words[i],
				      LUNA_FLOW_NETIF_WORDS, true);
		if (ret)
			return ret;
		ret = luna_flow_local_get(&e->macs, &e->local[i],
					  i ? edge->wan_mac : edge->lan_mac,
					  ep->c->sw_map->cpu_port);
		if (ret)
			return ret;
	}
	e->edge = *edge;
	e->provisioned = true;
	return 0;
}

static int luna_flow_install(void *sh, const struct gpon_flow_key *key,
			     const struct gpon_flow_act *act,
			     const struct gpon_flow_ctx *ctx, void *priv,
			     u32 *index)
{
	struct luna_eth *ep = sh;
	struct luna_flow_engine *e = ep->flow;
	struct luna_flow_priv *p = priv;
	struct luna_flow_path5 a = { .locked = true };
	struct gpon_edge edge;
	const char *why;
	bool ds = ctx->ds_leg;
	int ret;

	if (!ep->wan_ndev || ep->closing || !netif_carrier_ok(ep->ndev) ||
	    !netif_carrier_ok(ep->wan_ndev) || !act->dmac_valid || !ctx->idev ||
	    !ctx->odev || ds != !!act->nat_is_da ||
	    (ds ? !luna_flow_lan(sh, ctx->odev) : ctx->odev != ep->wan_ndev))
		return -EOPNOTSUPP;
	if (!ds) {
		ret = gpon_edge_read_via(ep->ndev, ep->wan_ndev, key->ip_da,
					 ctx->odev, act->gw_dmac, &edge, &why);
		if (ret)
			return ret;
		edge.wan_pppoe_sid = act->pppoe_sid;
		/* PPPoE over gpon0.<vid> routes through ppp, which has no VID. */
		if (gpon_flow_wan_vid(act, edge.wan_vlan, &edge.wan_vlan) ||
		    edge.lan_vlan || act->nat_addr != edge.wan_ip)
			return -EOPNOTSUPP;
		if (!e->provisioned || !gpon_edge_same_iface(&edge, &e->edge)) {
			ret = gpon_flow_offload_flush(e->fo);
			if (ret)
				return ret;
			ret = luna_flow_initialize(ep);
			if (ret)
				return ret;
			ret = luna_flow_provision(ep, &edge);
			if (ret)
				return ret;
		}
	} else if (!e->provisioned || key->ip_da != e->edge.wan_ip) {
		return -EAGAIN;
	}
	ret = luna_flow_wan_tag(ds, e->edge.wan_vlan, act->vlan_pop, &a);
	if (ret)
		return ret;
	ret = luna_flow_mac_get(&e->macs, &p->mac, act->gw_dmac,
				ds ? ep->lan_flood_mask : BIT(ep->c->sw_map->pon_port));
	if (ret)
		return ret;
	a.ingress_if = ds ? 1 : 0;
	a.egress_if = ds ? 0 : 1;
	a.dmac_index = p->mac.index;
	a.nat = ds ? LUNA_FLOW_DNAT : LUNA_FLOW_SNAT;
	a.translated_dst = act->nat_addr;
	a.translated_port = act->nat_port;
	a.ingress_pppoe = ds && e->edge.wan_pppoe_sid;
	a.set_stream = !ds;
	a.stream_id = ds ? 0 : GPON_DATA_FLOW;
	luna_flow_hits(ep);
	ret = luna_flow_sram_add(&e->sram, &p->slot, key,
				(key->ip_protocol == IPPROTO_TCP) << 23, &a);
	if (ret)
		return ret;
	p->last_hit = jiffies;
	*index = p->slot.index;
	return 0;
}

static int luna_flow_remove(void *sh, u32 index, void *priv)
{
	struct luna_eth *ep = sh;
	struct luna_flow_priv *p = priv;
	int ret;

	ret = luna_flow_sram_del(&ep->flow->sram, &p->slot);
	if (ret)
		return ret;
	return luna_flow_mac_put(&ep->flow->macs, &p->mac);
}

static int luna_flow_stats(void *sh, u32 index, void *priv, unsigned long *lastused)
{
	struct luna_eth *ep = sh;
	struct luna_flow_priv *p = priv;

	if (!p->slot.owned || p->slot.index != index)
		return -ENOENT;
	luna_flow_hits(ep);
	*lastused = p->last_hit;
	return 0;
}

static const struct gpon_flow_ops luna_flow_ops = {
	.is_lan_side = luna_flow_lan,
	.wan_vlan = true,	/* the path5 entry carries the tag per flow */
	.install = luna_flow_install,
	.remove = luna_flow_remove,
	.abort_install = luna_flow_remove,
	.stats = luna_flow_stats,
	.priv_size = sizeof(struct luna_flow_priv),
};

static int luna_flow_block_cb(enum tc_setup_type type, void *data, void *cookie)
{
	struct luna_eth *ep = luna_flow_eth(cookie);
	struct flow_cls_offload *f = data;
	int ret;

	if (type != TC_SETUP_CLSFLOWER || !ep || !ep->flow)
		return -EOPNOTSUPP;
	mutex_lock(&luna_flow_mutex);
#if IS_ENABLED(CONFIG_GPON_FLOW_DIAG)
	if (f->command == FLOW_CLS_REPLACE)
		ep->flow->offered++;
#endif
	ret = gpon_flow_offload_tc(ep->flow->fo, f, cookie);
	if (ret && ret != -EOPNOTSUPP && ret != -ENOENT &&
	    ret != -EEXIST && ret != -EAGAIN && ret != -ENOSPC)
		pr_warn_ratelimited("luna-flow: operation %u failed: %d\n", f->command, ret);
	mutex_unlock(&luna_flow_mutex);
	return ret;
}

static int luna_flow_setup_tc(struct net_device *dev, enum tc_setup_type type,
			      void *data)
{
	struct luna_eth *ep = luna_flow_eth(dev);

	if (!ep || !ep->flow || (type != TC_SETUP_BLOCK && type != TC_SETUP_FT))
		return -EOPNOTSUPP;
	return gpon_flow_block_setup(dev, data, &luna_flow_blocks,
				     luna_flow_block_cb, NULL);
}

static void luna_flow_stop(struct luna_eth *ep)
{
	int ret;
	unsigned int i;

	if (!ep->flow)
		return;
	mutex_lock(&luna_flow_mutex);
	ret = gpon_flow_offload_flush(ep->flow->fo);
	if (!ret) {
		ep->flow->provisioned = false;
		ep->flow->initialized = false;
		ret = luna_flow_reset(ep);
	}
	for (i = 0; !ret && i < 2; i++)
		ret = luna_flow_local_put(&ep->flow->macs, &ep->flow->local[i]);
	if (ret)
		netdev_err(ep->ndev, "flow shutdown failed: %d\n", ret);
	mutex_unlock(&luna_flow_mutex);
}

#if IS_ENABLED(CONFIG_GPON_FLOW_DIAG)
static void luna_flow_show_row(struct seq_file *m, struct luna_flow_engine *e,
			       unsigned int table, unsigned int index,
			       unsigned int count)
{
	u32 words[LUNA_FLOW_WORDS];
	unsigned int i;
	int ret;

	ret = luna_l34_tbl_op(e->sram.sw, e->sram.acc, table, index,
			      words, count, false);
	seq_printf(m, "table=%u index=%u", table, index);
	if (ret) {
		seq_printf(m, " error=%d\n", ret);
		return;
	}
	for (i = 0; i < count; i++)
		seq_printf(m, " %08x", words[i]);
	seq_putc(m, '\n');
}

static int luna_flow_show(struct seq_file *m, void *v)
{
	struct luna_eth *ep = m->private;
	struct luna_flow_engine *e = ep->flow;
	struct gpon_flow_diag d = { .valid = GPON_FDIAG_HAS_ALL };
	char line[768];
	unsigned int i;

	mutex_lock(&luna_flow_mutex);
	d.offered = e->offered;
	d.capacity = luna_flow_entry_count(&e->sram.hash);
	for (i = 0; i < d.capacity; i++)
		d.live += !!e->sram.owner[i];
	d.hw_hits = e->hits;
	gpon_flow_offload_diag(e->fo, &d, line, sizeof(line));
	seq_printf(m, "engine=FLOWBASED storage=SRAM initialized=%u provisioned=%u\n%s\n",
		   e->initialized, e->provisioned, line);
	if (e->initialized) {
		for (i = 0; i < 2; i++)
			luna_flow_show_row(m, e, 0, i, LUNA_FLOW_NETIF_WORDS);
		for (i = 0; i < LUNA_FLOW_MAC_COUNT; i++)
			if (e->macs.entry[i].users)
				luna_flow_show_row(m, e, LUNA_FLOW_TABLE_MAC, i, 1);
		for (i = 0; i < d.capacity; i++)
			if (e->sram.owner[i])
				luna_flow_show_row(m, e, LUNA_FLOW_TABLE_PATH5,
						   i, LUNA_FLOW_WORDS);
	}
	mutex_unlock(&luna_flow_mutex);
	return 0;
}

#endif

static void luna_flow_release(void *data)
{
	struct luna_eth *ep = data;
	int ret;
	unsigned int i;

#if IS_ENABLED(CONFIG_GPON_FLOW_DIAG)
	proc_remove(ep->flow->proc);
#endif
	ret = luna_flow_reset(ep);
	if (ret)
		netdev_err(ep->ndev, "flow hardware reset failed during release: %d\n", ret);
	for (i = 0; i < 2; i++) {
		ret = luna_flow_local_put(&ep->flow->macs, &ep->flow->local[i]);
		if (ret)
			netdev_err(ep->ndev, "local MAC restore failed: %d\n", ret);
	}
	gpon_flow_offload_free(ep->flow->fo);
	kfree(ep->flow);
	ep->flow = NULL;
}

static int luna_flow_probe(struct luna_eth *ep)
{
	struct luna_flow_engine *e;
	int ret;

	if (!hw_nat || !ep->c->flow)
		return 0;
	if (!ep->c->flow->acc || !ep->c->flow->mac || !ep->c->flow->mac->l2 ||
	    ep->c->flow->window_size < 0x8011a0 ||
	    (ep->c->flow->sram_bits != 10 && ep->c->flow->sram_bits != 12))
		return -EINVAL;
	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return -ENOMEM;
	e->sram.sw = devm_ioremap(ep->dev, SWCORE_PHYS, ep->c->flow->window_size);
	if (!e->sram.sw) {
		kfree(e);
		return -ENOMEM;
	}
	e->macs.sw = e->sram.sw;
	e->sram.acc = e->macs.acc = ep->c->flow->acc;
	e->sram.hash.sram_bits = ep->c->flow->sram_bits;
	e->macs.layout = ep->c->flow->mac;
	e->fo = gpon_flow_offload_new(&luna_flow_ops, ep);
	if (!e->fo) {
		kfree(e);
		return -ENOMEM;
	}
	ep->flow = e;
#if IS_ENABLED(CONFIG_GPON_FLOW_DIAG)
	e->proc = proc_create_single_data("flowdump", 0444, NULL, luna_flow_show, ep);
	if (!e->proc)
		dev_warn(ep->dev, "flow diagnostics unavailable\n");
#endif
	ret = devm_add_action_or_reset(ep->dev, luna_flow_release, ep);
	return ret;
}
#else
static int luna_flow_probe(struct luna_eth *ep) { return 0; }
static void luna_flow_stop(struct luna_eth *ep) { }
#endif
