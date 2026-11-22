// SPDX-License-Identifier: GPL-2.0-only
/*
 * GalaChip GC1601 packet engine: SFU flow offload in the switch
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include "gc1601-eth.h"

static int gc1601_hm_vlan_mod(struct gc1601_eth *priv, u32 fi,
			      enum gc1601_hm_op op, u16 vid)
{
	u32 cmd_addr = GC_HM_CMD_BASE + 4 * (fi - 1);
	u32 vlan_addr = op == GC_HM_POP ? 0 : fi;
	u32 cmds[4] = { gc1601_hm_cmds[op][0], gc1601_hm_cmds[op][1],
			GC_HM_CMD_END, GC_HM_CMD_END };
	u32 sub[2] = { vlan_addr << 26 | cmd_addr << 13, 0 };
	u32 info[3] = { 0, fi << 17, 0 };
	u32 vlan = vid;
	unsigned int i;
	int ret = 0;

	if (vlan_addr)
		ret = gc1601_hm_write(priv, GC_HM_VLANDAT, vlan_addr, &vlan, 1);
	for (i = 0; i < ARRAY_SIZE(cmds) && !ret; i++)
		ret = gc1601_hm_write(priv, GC_HM_CMDDAT, cmd_addr + i, &cmds[i], 1);
	if (!ret)
		ret = gc1601_hm_write(priv, GC_HM_SUBDAT, fi, sub, ARRAY_SIZE(sub));
	if (!ret)
		ret = gc1601_hm_write(priv, GC_HM_FLOWINFO, fi, info, ARRAY_SIZE(info));
	return ret;
}

/* The rewrite a flow direction needs for the service's LAN side, if any. */
static bool gc1601_sfu_flow_op(const struct gc1601_sfu_flow *f, bool up,
			       enum gc1601_hm_op *op, u16 *vid)
{
	*vid = f->vlan_id;
	switch (f->lan) {
	case GC1601_SFU_LAN_UNTAGGED:
		*op = up ? GC_HM_PUSH : GC_HM_POP;
		return true;
	case GC1601_SFU_LAN_TRANSLATE:
		*op = GC_HM_REPLACE;
		if (!up)
			*vid = f->lan_vid;
		return true;
	case GC1601_SFU_LAN_STACKED:
		*op = up ? GC_HM_PUSH_COPY : GC_HM_POP;
		return true;
	default:
		return false;
	}
}

/* Flow info of a slot's direction: 1 + 2 * slot up, 2 + 2 * slot down. */
static u32 gc1601_sfu_flow_fi(unsigned int slot, bool up)
{
	return up ? 1 + 2 * slot : 2 + 2 * slot;
}

/* A flow bound to a modification: the flow info index, split over two words. */
static void gc1601_sfu_flow_mod(u32 *w, u32 fi)
{
	w[0] |= (fi & 0x3f) << 26;
	w[1] |= 0x400 | fi >> 6;
}

/*
 * A flow for a VLAN-only match: parser rule 3 (outer VID) or 7
 * (untagged). The ingress key is the UNI (1) upstream or the GEM downstream,
 * split over word 3 [31:28] and word 4 [7:0]; the VID over word 4 [31:24]
 * and word 5 [7:0]. Upstream forwards to the GEM on the service's T-CONT,
 * downstream to the UNI. Upstream matches the LAN side of the service; a
 * P-bit flow uses parser rule 6 and the whole TCI (P-bit 15:13, VID 11:0).
 */
static void gc1601_sfu_flow_words(u32 *w, bool up, const struct gc1601_sfu_flow *f,
				  unsigned int slot)
{
	u32 key = up ? 1 : f->gem_port;
	bool untagged = f->untagged || (up && f->lan == GC1601_SFU_LAN_UNTAGGED);
	u16 vid = f->vlan_id;
	enum gc1601_hm_op op;
	u16 mod_vid;

	if (up && (f->lan == GC1601_SFU_LAN_TRANSLATE ||
		   f->lan == GC1601_SFU_LAN_STACKED))
		vid = f->lan_vid;

	memset(w, 0, 15 * sizeof(*w));
	if (up) {
		u8 q = f->queue & 7;

		/* Queue in bits 5:3, bit 6 set when it is nonzero. */
		w[0] = BIT(25) | (f->gem_port & 0xfff) << 13 |
			(f->tcont & 0x3f) << 7 | (q & 7) << 3 |
			(q ? BIT(6) : 0) | 4;
		w[1] = 0x1000;
		w[3] = 0x0040c1c0;
	} else {
		w[0] = BIT(25) | 1 << 13 | 4;
		w[1] = 0x1c00;
		w[3] = 0x0040c180;
		w[4] = BIT(23);
	}
	w[3] |= (key & 0xf) << 28;
	w[4] |= key >> 4 & 0xff;
	if (untagged) {
		w[4] |= 7 << 14;
	} else if (up && f->pcp_match) {
		u16 tci = (f->pcp & 7) << 13 | (vid & 0xfff);

		/*
		 * Match class 5: EPON upload dropped without it. Its learned and
		 * mapped VLANs are all tagged P-bit flows; untagged-LAN GPON
		 * services do not use these.
		 */
		w[3] = (w[3] & ~0x0007c000) | (5 << 14);
		w[4] |= (tci & 0xff) << 24 | 6 << 14 | 0x100;
		w[5] = tci >> 8;
	} else {
		w[4] |= (vid & 0xff) << 24 | 3 << 14 | 0x100;
		w[5] = vid >> 8;
	}
	if (gc1601_sfu_flow_op(f, up, &op, &mod_vid))
		gc1601_sfu_flow_mod(w, gc1601_sfu_flow_fi(slot, up));
}

bool gc1601_pe_sfu_same_key(const struct gc1601_sfu_flow *a,
			    const struct gc1601_sfu_flow *b, bool upstream)
{
	u32 aw[15], bw[15];

	gc1601_sfu_flow_words(aw, upstream, a, 0);
	gc1601_sfu_flow_words(bw, upstream, b, 0);
	/* Words 0..2 carry actions; word 3 onwards is the encoded match. */
	return !memcmp(aw + 3, bw + 3, 12 * sizeof(*aw));
}
EXPORT_SYMBOL_GPL(gc1601_pe_sfu_same_key);

static bool gc1601_sfu_same_action(const struct gc1601_sfu_flow *a,
				   const struct gc1601_sfu_flow *b, bool up)
{
	enum gc1601_hm_op aop, bop;
	u16 avid, bvid;
	u32 aw[15], bw[15];
	bool amod, bmod;

	gc1601_sfu_flow_words(aw, up, a, 0);
	gc1601_sfu_flow_words(bw, up, b, 0);
	if (memcmp(aw, bw, 3 * sizeof(*aw)))
		return false;
	amod = gc1601_sfu_flow_op(a, up, &aop, &avid);
	bmod = gc1601_sfu_flow_op(b, up, &bop, &bvid);
	if (amod != bmod)
		return false;
	return !amod || (aop == bop && (aop == GC_HM_POP || avid == bvid));
}

/* Deduplicate only identical actions; ambiguous keys must use the CPU path. */
int gc1601_pe_sfu_normalize(struct gc1601_sfu_flow *flows, unsigned int n)
{
	unsigned int i, j, up, out = 0;

	for (i = 0; i < n; i++) {
		if (flows[i].up_only && flows[i].down_only)
			return -EINVAL;
		for (j = 0; j < i; j++) {
			for (up = 0; up < 2; up++) {
				if (up ? flows[i].down_only || flows[j].down_only :
					 flows[i].up_only || flows[j].up_only)
					continue;
				if (!gc1601_pe_sfu_same_key(&flows[i], &flows[j], up))
					continue;
				if (!gc1601_sfu_same_action(&flows[i], &flows[j], up))
					return -EEXIST;
				if (up)
					flows[i].down_only = true;
				else
					flows[i].up_only = true;
			}
		}
	}
	for (i = 0; i < n; i++)
		if (!flows[i].up_only || !flows[i].down_only)
			flows[out++] = flows[i];
	return out;
}
EXPORT_SYMBOL_GPL(gc1601_pe_sfu_normalize);

/* Indirect L3 table read into w; bit 27 of the index word selects a read. */
int gc1601_sr_tbl_read(struct gc1601_eth *priv, u32 table, u32 index,
		       u32 *w, unsigned int n)
{
	void __iomem *l3 = priv->switch_base + GC_SR_L3;
	unsigned int k;
	u32 done;
	int ret;

	writel(BIT(27) | table << 22 | index, l3 + 0x14);
	ret = readl_poll_timeout(l3 + 0x18, done, done == 1, 1, 1000);
	for (k = 0; k < n && !ret; k++)
		w[k] = readl(l3 + 0x1c + 4 * k);
	return ret;
}

/* local_crc32: MSB first, no reflection, initial and final value 0. */
static u32 gc1601_sr_crc(const u8 *data, unsigned int len, u32 poly)
{
	u32 crc = 0;
	int bit;

	while (len--) {
		crc ^= (u32)*data++ << 24;
		for (bit = 0; bit < 8; bit++)
			crc = crc & BIT(31) ? crc << 1 ^ poly : crc << 1;
	}
	return crc;
}

/*
 * The hash key is entry bits 119..471 (from word 3 bit 23,
 * bit 123 left out), packed into 45 bytes and byte-reversed. Checked against
 * the four default flows the stock firmware keeps in table 2.
 */
static u32 gc1601_sr_flow_hash(const u32 *w, u32 poly)
{
	u8 key[45] = { };
	unsigned int i;

	for (i = 0; i < 353; i++) {
		unsigned int src = 119 + i;

		if (src != 123 && (w[src / 32] >> (src % 32) & 1))
			key[44 - i / 8] |= BIT(i % 8);
	}
	return gc1601_sr_crc(key, sizeof(key), poly);
}

/*
 * Hashed tables 2..5 hold 256, 128, 64 and 64 flows, at the
 * key's CRC-32 (0x04c11db7) or CRC-32C (0x1edc6f41) modulo the size. An
 * entry goes to the first table whose slot is free.
 */
static int gc1601_sr_hash_insert(struct gc1601_eth *priv, const u32 *w, bool up)
{
	static const struct { u32 table, size, poly; } tables[] = {
		{ 2, 256, 0x04c11db7 }, { 3, 128, 0x1edc6f41 },
		{ 4, 64, 0x04c11db7 },  { 5, 64, 0x1edc6f41 },
	};
	struct {
		u32 index;
		u32 words[4];
	} occupied[ARRAY_SIZE(tables)];
	u32 old[15];
	unsigned int t;
	int ret;

	for (t = 0; t < ARRAY_SIZE(tables); t++) {
		u32 index = gc1601_sr_flow_hash(w, tables[t].poly) % tables[t].size;

		ret = gc1601_sr_tbl_read(priv, tables[t].table, index, old, ARRAY_SIZE(old));
		if (ret)
			return ret;
		if (old[3] & GC_SR_FLOW_VALID) {
			occupied[t].index = index;
			occupied[t].words[0] = old[0];
			occupied[t].words[1] = old[3];
			occupied[t].words[2] = old[4];
			occupied[t].words[3] = old[5];
			continue;
		}
		ret = gc1601_sr_tbl_write(priv, tables[t].table, index, w, 15);
		if (ret)
			return ret;
		priv->sfu_hashed[priv->sfu_hashed_count] = tables[t].table << 16 | index;
		priv->sfu_hashed_up[priv->sfu_hashed_count++] = up;
		return 0;
	}
	dev_err(priv->dev, "SFU hash full: new=%08x/%08x/%08x/%08x\n",
		w[0], w[3], w[4], w[5]);
	for (t = 0; t < ARRAY_SIZE(tables); t++)
		dev_err(priv->dev,
			"SFU hash table=%u index=%u old=%08x/%08x/%08x/%08x\n",
			tables[t].table, occupied[t].index,
			occupied[t].words[0], occupied[t].words[1],
			occupied[t].words[2], occupied[t].words[3]);
	return -ENOSPC;
}

static int gc1601_sfu_apply(struct gc1601_eth *priv,
			    const struct gc1601_sfu_flow *flows, unsigned int n)
{
	u32 w[15] = { };
	unsigned int i;
	int ret;

	n = min_t(unsigned int, n, GC1601_SFU_FLOWS);
	if (n && !priv->sr_ready) {
		ret = gc1601_sr_hw_init(priv);
		if (ret)
			return ret;
		priv->sr_ready = true;
	}
	if (n && !priv->sfu_rules) {
		ret = gc1601_sfu_rules(priv);
		if (ret)
			return ret;
		priv->sfu_rules = true;
	}

	/*
	 * Take the last set's flows out before rewriting the header changes
	 * they point at, or a reordered slot would tag with the new set's VID
	 * meanwhile. Until the new flows are in, frames take the pon0 path.
	 */
	memset(w, 0, sizeof(w));
	for (i = 0; i < priv->sfu_hashed_count; i++) {
		ret = gc1601_sr_tbl_write(priv, priv->sfu_hashed[i] >> 16,
					  priv->sfu_hashed[i] & 0xffff, w, ARRAY_SIZE(w));
		if (ret)
			return ret;
	}
	priv->sfu_hashed_count = 0;
	if (priv->sr_ready) {
		for (i = 0; i < GC_SR_FLOW_SLOTS; i++) {
			ret = gc1601_sr_tbl_write(priv, 8, i, w, ARRAY_SIZE(w));
			if (ret)
				return ret;
		}
	}

	for (i = 0; i < 2 * n; i++) {
		bool up = !(i & 1);
		enum gc1601_hm_op op;
		u16 vid;

		if ((up && flows[i / 2].down_only) ||
		    (!up && flows[i / 2].up_only) ||
		    !gc1601_sfu_flow_op(&flows[i / 2], up, &op, &vid))
			continue;
		ret = gc1601_hm_vlan_mod(priv, gc1601_sfu_flow_fi(i / 2, up), op, vid);
		if (ret)
			return ret;
	}

	/* The first services in the linear table 8, the rest hashed. */
	for (i = 0; i < GC_SR_FLOW_SLOTS || i < 2 * n; i++) {
		bool empty = i >= 2 * n || (!(i & 1) && flows[i / 2].down_only) ||
			     ((i & 1) && flows[i / 2].up_only);

		if (!empty)
			gc1601_sfu_flow_words(w, !(i & 1), &flows[i / 2], i / 2);
		else
			memset(w, 0, sizeof(w));
		if (i < GC_SR_FLOW_SLOTS)
			ret = gc1601_sr_tbl_write(priv, 8, i, w, ARRAY_SIZE(w));
		else if (!empty)
			ret = gc1601_sr_hash_insert(priv, w, !(i & 1));
		if (ret && !empty) {
			dev_err(priv->dev, "SFU flow %u %s GEM %u VLAN %u failed: %d\n",
				i / 2, i & 1 ? "down" : "up",
				flows[i / 2].gem_port, flows[i / 2].vlan_id, ret);
			return ret;
		}
		if (ret)
			return ret;
	}

	/*
	 * As the stock firmware runs: flow lookup on, and whatever no flow
	 * matches goes to the CPU through default class 9 (queue 7). That keeps
	 * the device itself and services without a flow on the pon0 path.
	 */
	if (priv->sr_ready) {
		gc1601_sr_bypass(priv, false, 0);
		writel(0x00050003, priv->switch_base + GC_SR_SRC + 0x60);
		writel(0x3, priv->switch_base + GC_SR_SRC + 0x64);
		writel(0x03000503, priv->switch_base + GC_SR_SRC + 0x70);
		writel(0x00030000, priv->switch_base + GC_SR_SRC + 0x74);
		writel(0, priv->switch_base + GC_SR_L3 + 0xec);
		writel(7 << 9, priv->switch_base + GC_SR_L3 + 0xe8);
		writel(0x545, priv->switch_base + GC_SR_L3 + 0x0c);
	}
	priv->sfu_flows = n;
	return 0;
}

int gc1601_pe_sfu_set(const struct gc1601_sfu_flow *flows, unsigned int n)
{
	struct gc1601_sfu_flow normalized[GC1601_SFU_FLOWS];
	struct net_device *netdev = READ_ONCE(gc1601_pe_netdev);
	struct gc1601_eth *priv;
	unsigned int i;
	bool same;
	int ret;

	if (n > ARRAY_SIZE(normalized) || (n && !flows))
		return -EINVAL;
	if (n)
		memcpy(normalized, flows, n * sizeof(*flows));
	ret = gc1601_pe_sfu_normalize(normalized, n);
	if (ret < 0)
		return ret;
	n = ret;
	flows = normalized;

	if (!netdev)
		return -ENODEV;
	priv = netdev_priv(netdev);
	if (!priv->switch_base)
		return -ENODEV;

	rtnl_lock();
	/* The same flows again, as after an omcid restart, are not news. */
	same = n && n == priv->sfu_logged &&
	       !memcmp(flows, priv->sfu_active, n * sizeof(*flows));
	ret = gc1601_sfu_apply(priv, flows, n);
	if (ret) {
		int restore;

		restore = gc1601_sfu_apply(priv, priv->sfu_active, priv->sfu_flows);
		if (restore)
			dev_err(priv->dev, "SFU rollback failed: %d\n", restore);
	} else if (n) {
		memcpy(priv->sfu_active, flows,
		       min_t(unsigned int, n, GC1601_SFU_FLOWS) * sizeof(*flows));
		priv->sfu_logged = n;
	}
	rtnl_unlock();
	if (ret) {
		dev_err(priv->dev, "SFU offload failed: %d\n", ret);
		return ret;
	}
	for (i = 0; !same && i < priv->sfu_flows; i++) {
		char how[64];
		int len;

		len = scnprintf(how, sizeof(how), ", %s",
				flows[i].down_only ? "downstream only" :
				flows[i].up_only ? "upstream only" : "bidirectional");
		if (!flows[i].down_only) {
			len += scnprintf(how + len, sizeof(how) - len,
					 ", queue %u", flows[i].queue);
			if (flows[i].pcp_match)
				scnprintf(how + len, sizeof(how) - len,
					  ", P-bit %u", flows[i].pcp);
		}
		if (flows[i].untagged)
			dev_info(priv->dev, "SFU offload: untagged <-> GEM %u, T-CONT %u%s\n",
				 flows[i].gem_port, flows[i].tcont, how);
		else if (flows[i].lan == GC1601_SFU_LAN_UNTAGGED)
			dev_info(priv->dev, "SFU offload: LAN untagged <-> VLAN %u, GEM %u, T-CONT %u%s\n",
				 flows[i].vlan_id, flows[i].gem_port, flows[i].tcont, how);
		else if (flows[i].lan == GC1601_SFU_LAN_TRANSLATE)
			dev_info(priv->dev, "SFU offload: LAN VLAN %u <-> VLAN %u, GEM %u, T-CONT %u%s\n",
				 flows[i].lan_vid, flows[i].vlan_id,
				 flows[i].gem_port, flows[i].tcont, how);
		else if (flows[i].lan == GC1601_SFU_LAN_STACKED)
			dev_info(priv->dev, "SFU offload: LAN VLAN %u <-> VLAN %u.%u, GEM %u, T-CONT %u%s\n",
				 flows[i].lan_vid, flows[i].vlan_id, flows[i].lan_vid,
				 flows[i].gem_port, flows[i].tcont, how);
		else
			dev_info(priv->dev, "SFU offload: VLAN %u <-> GEM %u, T-CONT %u%s\n",
				 flows[i].vlan_id, flows[i].gem_port, flows[i].tcont, how);
	}
	if (!priv->sfu_flows)
		dev_dbg(priv->dev, "SFU offload off\n");
	return 0;
}
EXPORT_SYMBOL_GPL(gc1601_pe_sfu_set);
