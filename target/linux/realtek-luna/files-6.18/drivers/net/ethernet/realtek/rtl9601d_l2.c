// SPDX-License-Identifier: GPL-2.0-only
/* V2802RH SFU: switch VLAN forwarding and upstream GEM classification.
 * Included by rtl9602c_eth.c, which owns the switch table engine.
 */
/* L2 unicast table and classifier registers. */
#define L2_CMD_WR		BIT(3)	/* 0 = read, 1 = write */
#define L2_CMD_METHOD_SH	4	/* [6:4] 0 = MAC-hash, 1 = direct address */
#define L2_RDATA		0x1201c	/* word0..2 @ +4 */
#define L34_WORDS_L2UC		3
#define L2_METHOD_MAC		0
#define L34_CF_CFG		0x1600c
#define L34_CF_P1_COUNT_SH	5
#define L34_CF_ACTION_CTRL	0x16038
#define L34_CF_SID_ENABLE	BIT(5)

#include <linux/if_bridge.h>
#include <linux/iopoll.h>
#include <linux/rtnetlink.h>
#include <net/switchdev.h>

#define V2802_L2_VLANS 64
#define V2802_L2_FIRST 32
#define V2802_L2_RULES (4 * LUNA_L2_SERVICES)
#define V2802_PISO_BITS 11
#define V2802_FLUSH_CTRL 0x17024
#define V2802_FLUSH_PORTS 0x17028

static DEFINE_SPINLOCK(v2802_l2_lock);

static struct {
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
} v2802_l2;

/* CPU-forwarded upstream frames have the LAN VID, but the OLT's MAC was
 * learned in the WAN VID. They are normal traffic, not a unicast storm. */
static int v2802_l2_storm(struct rtl9602c_eth *ep, bool hardware_only)
{
	u32 value = ioread32(ep->sw + SW_STORM_CTRL_UC_CTRL);

	value &= ~(BIT(0) | BIT(1) | BIT(ep->swm->pon_port));
	if (hardware_only)
		value |= BIT(0) | BIT(1);
	iowrite32(value, ep->sw + SW_STORM_CTRL_UC_CTRL);
	return ioread32(ep->sw + SW_STORM_CTRL_UC_CTRL) == value ? 0 : -EIO;
}

/* One indirect transaction, called with sw_table_lock held. */
static int v2802_l2_xact(u8 type, u16 addr, u32 *words, unsigned int n,
			   bool write)
{
	struct rtl9602c_eth *ep = v2802_l2.ep;
	struct hwio io = luna_sw_io(ep->sw);
	unsigned int i;
	int rc;

	rc = gpon_ind_poll(&io, reg_make(SW_TBL_STS), SW_TBL_BUSY,
			   SW_TBL_TRIES, rtl9602c_sw_tbl_pause);
	if (rc)
		return rc;
	if (write)
		for (i = 0; i < n; i++)
			iowrite32(words[i], ep->sw + SW_TBL_WRDATA + 4 * i);
	iowrite32(((u32)addr << 9) | BIT(4) | (write ? BIT(3) : 0) | type,
		  ep->sw + SW_TBL_CTRL);
	rc = gpon_ind_poll(&io, reg_make(SW_TBL_STS), SW_TBL_BUSY,
			   SW_TBL_TRIES, rtl9602c_sw_tbl_pause);
	if (!rc && !write)
		for (i = 0; i < n; i++)
			words[i] = ioread32(ep->sw + 0x1201c + 4 * i);
	return rc;
}

/* The state lock never waits for the indirect table engine. */
static int v2802_l2_table(u8 type, u16 addr, u32 *words, unsigned int n,
			    bool write)
{
	struct rtl9602c_eth *ep = v2802_l2.ep;
	unsigned long flags;
	u32 check[5];
	int rc;

	spin_lock_irqsave(&ep->sw_table_lock, flags);
	rc = v2802_l2_xact(type, addr, words, n, write);
	if (!rc && write) {
		rc = v2802_l2_xact(type, addr, check, n, false);
		if (!rc && memcmp(words, check, n * sizeof(*words)))
			rc = -EIO;
	}
	spin_unlock_irqrestore(&ep->sw_table_lock, flags);
	return rc;
}

/* RTL9602C L2_UC, as rtl9602c_lutStToTblData packs it: MAC [47:0], VID
 * [59:48], STATIC 62, IVL 63, SPA [66:65], AGE [69:67], VALID 77. */
#define V2802_L2UC_VID_LSP	48
#define V2802_L2UC_IVL_LSP	63

/* Insert or delete by the MAC hash; an insert is read back the same way. */
static int v2802_l2_own_entry(const u8 *mac, u16 vid, bool add)
{
	struct rtl9602c_eth *ep = v2802_l2.ep;
	struct hwio io = luna_sw_io(ep->sw);
	static const u8 bcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	u32 w[L34_WORDS_L2UC] = { 0 }, mask[L34_WORDS_L2UC] = { 0 };
	u32 check[L34_WORDS_L2UC];
	unsigned long flags;
	unsigned int i;
	int rc;

	/* the fields set below, for the read-back */
	l34_mac48_set(mask, L2UC_MAC_LSP, bcast);
	l34_field_set(mask, V2802_L2UC_VID_LSP, 12, 0xfff);
	l34_field_set(mask, V2802_L2UC_IVL_LSP, 1, 1);
	l34_field_set(mask, L2UC_STATIC_LSP, L2UC_STATIC_W, 1);
	l34_field_set(mask, L2UC_SPA_LSP, L2UC_SPA_W, 3);
	l34_field_set(mask, L2UC_VALID_LSP, L2UC_VALID_W, 1);

	l34_mac48_set(w, L2UC_MAC_LSP, mac);
	l34_field_set(w, V2802_L2UC_VID_LSP, 12, vid);
	l34_field_set(w, V2802_L2UC_IVL_LSP, 1, 1);
	if (add) {
		l34_field_set(w, L2UC_STATIC_LSP, L2UC_STATIC_W, 1);
		l34_field_set(w, L2UC_SPA_LSP, L2UC_SPA_W, ep->swm->cpu_port);
		l34_field_set(w, L2UC_AGE_LSP, L2UC_AGE_W, 1);
		l34_field_set(w, L2UC_VALID_LSP, L2UC_VALID_W, 1);
	}
	spin_lock_irqsave(&ep->sw_table_lock, flags);
	rc = gpon_ind_poll(&io, reg_make(SW_TBL_STS), SW_TBL_BUSY,
			   SW_TBL_TRIES, rtl9602c_sw_tbl_pause);
	for (i = 0; !rc && i < L34_WORDS_L2UC; i++)
		iowrite32(w[i], ep->sw + SW_TBL_WRDATA + 4 * i);
	if (!rc) {
		iowrite32(L2_CMD_WR | (L2_METHOD_MAC << L2_CMD_METHOD_SH), ep->sw + SW_TBL_CTRL);
		rc = gpon_ind_poll(&io, reg_make(SW_TBL_STS), SW_TBL_BUSY,
				   SW_TBL_TRIES, rtl9602c_sw_tbl_pause);
	}
	if (!rc && add) {
		iowrite32(L2_METHOD_MAC << L2_CMD_METHOD_SH, ep->sw + SW_TBL_CTRL);
		rc = gpon_ind_poll(&io, reg_make(SW_TBL_STS), SW_TBL_BUSY,
				   SW_TBL_TRIES, rtl9602c_sw_tbl_pause);
		for (i = 0; !rc && i < L34_WORDS_L2UC; i++)
			check[i] = ioread32(ep->sw + L2_RDATA + 4 * i);
		if (!rc && l34_l2uc_sts_index(ioread32(ep->sw + SW_TBL_STS)) < 0)
			rc = -EIO;
		for (i = 0; !rc && i < L34_WORDS_L2UC; i++)
			if ((check[i] ^ w[i]) & mask[i])
				rc = -EIO;
	}
	spin_unlock_irqrestore(&ep->sw_table_lock, flags);
	return rc;
}

/* The bridge, seen from the PON port: pon0 is its member in either topology,
 * eth0 + pon0 (a configuration from before the jacks had netdevs) or
 * lan1 + lan2 + pon0. */
static struct net_device *v2802_l2_bridge(struct rtl9602c_eth *ep)
{
	return ep->wan_ndev ? netdev_master_upper_dev_get(ep->wan_ndev) : NULL;
}

/* The LAN side of the bridge: eth0, or a jack's netdev. */
static bool v2802_l2_lan_member(struct rtl9602c_eth *ep, const struct net_device *dev)
{
	unsigned int p;

	if (dev == ep->ndev)
		return true;
	for (p = 0; p < LAN_PORTS; p++)
		if (ep->lan_ndev[p] && dev == ep->lan_ndev[p])
			return true;
	return false;
}

static u32 v2802_l2_cpu_vlan(bool present);

static bool v2802_l2_rewritten(const struct luna_l2_service *s)
{
	return s->translated || s->stacked;
}

#include "rtl9601d_vlan.c"

static void v2802_l2_own_mac_add(const u8 *mac)
{
	unsigned int m;

	for (m = 0; m < v2802_l2.own_macs; m++)
		if (ether_addr_equal(v2802_l2.own_mac[m], mac))
			return;
	ether_addr_copy(v2802_l2.own_mac[v2802_l2.own_macs++], mac);
}

/* LAN frames for the ONU itself arrive in a service VLAN, but the switch
 * learns the CPU's own MAC only in the untagged VLAN its frames leave in.
 * Under IVL they would miss, flood to the PON as unknown unicast and be cut
 * to about 1 Mbit/s by the storm meter: give each service VID a static
 * entry to the CPU. Called under RTNL. */
static int v2802_l2_own_macs(const struct luna_l2_service *service, int count, bool add)
{
	struct rtl9602c_eth *ep = v2802_l2.ep;
	struct net_device *br = v2802_l2_bridge(ep);
	unsigned int m, p;
	int i, rc, err = 0;

	if (add) {
		v2802_l2.own_macs = 0;
		if (br)
			v2802_l2_own_mac_add(br->dev_addr);
		v2802_l2_own_mac_add(ep->ndev->dev_addr);
		for (p = 0; p < LAN_PORTS; p++)
			if (ep->lan_ndev[p])
				v2802_l2_own_mac_add(ep->lan_ndev[p]->dev_addr);
	}
	for (m = 0; m < v2802_l2.own_macs; m++)
		for (i = 0; i < count; i++) {
			/* These look up in the shared FID, where the CPU learns anyway. */
			if (v2802_l2_rewritten(&service[i]))
				continue;
			rc = v2802_l2_own_entry(v2802_l2.own_mac[m], service[i].vid, add);
			if (rc && add)
				return rc;
			err = err ?: rc;
		}
	if (!add && !err)
		v2802_l2.own_macs = 0;
	return err;
}

/* RTL9602C isolationEntry_set: two 11-bit physical+extension masks per word.
 * Removing LAN<->PON egress leaves the CPU/OMCC path available. */
static int v2802_l2_gate_locked(void)
{
	struct rtl9602c_eth *ep = v2802_l2.ep;
	u32 w[2];
	unsigned int p, i;

	for (i = 0; i < ARRAY_SIZE(w); i++) {
		if (!v2802_l2.isolation_saved)
			v2802_l2.piso[i] = ioread32(ep->sw + SW_PISO_PORT + 4 * i);
		w[i] = v2802_l2.piso[i];
	}
	v2802_l2.isolation_saved = true;
	for (p = 0; p < 3; p++) {
		unsigned int shift = (p % 2) * V2802_PISO_BITS;
		u32 mask = p == 2 ? 3 : BIT(2);

		/* Mixed tagging needs software LAN relay while the PVIDs are withdrawn. */
		if (p < 2 && (luna_xpon_lan_tagged() || v2802_l2_vlan_filtering(ep)))
			mask |= BIT(1 - p);
		mask <<= shift;

		w[p / 2] &= ~mask;
	}
	for (i = 0; i < ARRAY_SIZE(w); i++)
		iowrite32(w[i], ep->sw + SW_PISO_PORT + 4 * i);
	for (i = 0; i < ARRAY_SIZE(w); i++)
		if (ioread32(ep->sw + SW_PISO_PORT + 4 * i) != w[i])
			return -EIO;
	v2802_l2.gated = true;
	v2802_l2.active = false;
	return 0;
}

static int v2802_l2_ungate_locked(void)
{
	struct rtl9602c_eth *ep = v2802_l2.ep;
	unsigned int i;

	if (!v2802_l2.gated)
		return 0;
	for (i = 0; i < ARRAY_SIZE(v2802_l2.piso); i++)
		iowrite32(v2802_l2.piso[i], ep->sw + SW_PISO_PORT + 4 * i);
	for (i = 0; i < ARRAY_SIZE(v2802_l2.piso); i++)
		if (ioread32(ep->sw + SW_PISO_PORT + 4 * i) != v2802_l2.piso[i]) {
			v2802_l2.gated = false;
			v2802_l2_gate_locked();
			return -EIO;
		}
	v2802_l2.gated = false;
	v2802_l2.isolation_saved = false;
	return 0;
}

/* Leaky traffic would bypass the withdrawal gate. Stock has these disabled. */
static bool v2802_l2_gate_supported(struct rtl9602c_eth *ep)
{
	unsigned int reg;

	if ((ioread32(ep->sw + 0x1c034) & 7) ||
	    (ioread32(ep->sw + 0x11058) & BIT(4)))
		return false;
	for (reg = 0x1c038; reg <= 0x1c080; reg += 4)
		if (ioread32(ep->sw + reg) & BIT(0))
			return false;
	return true;
}

/* A jack's steering VLAN (rtl9602c_eth.c): the switch table owns it. */
static bool v2802_l2_steer_vid(u16 vid)
{
	return vid >= SW_LAN_STEER_VID_BASE && vid < SW_LAN_STEER_VID_BASE + LAN_PORTS;
}

static bool v2802_l2_managed_vid(u16 vid)
{
	return vid && vid != SW_DEFAULT_VID && vid != pon_vid && !v2802_l2_steer_vid(vid);
}

static u32 v2802_l2_cpu_vlan(bool present)
{
	return present ? BIT(11) | SW_VLAN_IVL | BIT(3) | BIT(2) : 0;
}

/* RTNL serializes service/VLAN-device consumers with table reconciliation. */
static void v2802_l2_want_vids(void)
{
	bitmap_zero(v2802_l2.want_vids, VLAN_N_VID);
	luna_xpon_service_vids(v2802_l2.want_vids);
	bitmap_or(v2802_l2.want_vids, v2802_l2.want_vids,
		  v2802_l2.netdev_vids, VLAN_N_VID);
}

/* Flush dynamic LAN entries before returning to CPU-only service VLANs.
 * Otherwise IVL still resolves replies to a LAN port outside that VLAN. */
static int v2802_l2_flush_lan(struct rtl9602c_eth *ep)
{
	u32 saved, w;
	unsigned int p;
	int rc;

	rc = read_poll_timeout(ioread32, w, !(w & BIT(0)), 100, 20000,
			       false, ep->sw + V2802_FLUSH_CTRL);
	if (rc)
		return rc;
	saved = w;
	/* Mode 0 = by port, dynamic only (bit 4), no static entries (bit 3). */
	iowrite32((saved & ~GENMASK(17, 1)) | BIT(4), ep->sw + V2802_FLUSH_CTRL);
	for (p = 0; p < 2; p++) {
		iowrite32(BIT(p), ep->sw + V2802_FLUSH_PORTS);
		rc = read_poll_timeout(ioread32, w, !(w & BIT(0)), 100, 20000,
				       false, ep->sw + V2802_FLUSH_CTRL);
		if (rc)
			return rc;
	}
	iowrite32(saved & ~BIT(0), ep->sw + V2802_FLUSH_CTRL);
	return 0;
}

/* Worker/teardown only, under RTNL. A failure retains the isolation gate. */
static int v2802_l2_clear(void)
{
	struct rtl9602c_eth *ep = v2802_l2.ep;
	u32 zero[2] = { 0 }, vlan;
	unsigned long flags;
	unsigned int i;
	int rc;

	if (!v2802_l2.saved)
		return 0;
	spin_lock_irqsave(&v2802_l2_lock, flags);
	rc = v2802_l2_gate_locked();
	spin_unlock_irqrestore(&v2802_l2_lock, flags);
	if (rc)
		return rc;
	for_each_set_bit(i, v2802_l2.rules, V2802_L2_RULES) {
		rc = v2802_l2_table(4, 256 + V2802_L2_FIRST + i, zero, 2, true);
		if (rc)
			return rc;
		__clear_bit(i, v2802_l2.rules);
	}
	rc = v2802_l2_own_macs(v2802_l2.service, v2802_l2.count, false);
	if (rc)
		return rc;
	for (i = 0; i < v2802_l2.count; i++) {
		u16 vid = v2802_l2.service[i].vid;

		vlan = v2802_l2_managed_vid(vid) ?
			v2802_l2_cpu_vlan(test_bit(vid, v2802_l2.want_vids)) : v2802_l2.vlan[i];
		rc = v2802_l2_table(1, vid, &vlan, 1, true);
		if (rc)
			return rc;
		__clear_bit(vid, v2802_l2.wan_vids);
		if (v2802_l2_managed_vid(vid) && test_bit(vid, v2802_l2.want_vids))
			__set_bit(vid, v2802_l2.wan_vids);
	}
	if (v2802_l2.management_saved) {
		rc = v2802_l2_table(1, SW_DEFAULT_VID,
				     &v2802_l2.management_vlan, 1, true);
		if (rc)
			return rc;
	}
	iowrite32(v2802_l2.pvid, ep->sw + SW_VLAN_PB_VID);
	iowrite32(v2802_l2.accept, ep->sw + SW_VLAN_PORT_ACCEPT_FRAME_TYPE);
	iowrite32(v2802_l2.cfg, ep->sw + L34_CF_CFG);
	iowrite32(v2802_l2.flood[0], ep->sw + ep->swm->bc_flood);
	iowrite32(v2802_l2.flood[1], ep->sw + ep->swm->unkn_mc_flood);
	iowrite32(v2802_l2.flood[2], ep->sw + ep->swm->unkn_uc_flood);
	rc = v2802_l2_flush_lan(ep);
	if (rc)
		return rc;
	spin_lock_irqsave(&v2802_l2_lock, flags);
	rc = luna_xpon_lan_tagged() || v2802_l2_vlan_filtering(ep) ?
		0 : v2802_l2_ungate_locked();
	if (!rc) {
		v2802_l2.saved = false;
		v2802_l2.management_saved = false;
		v2802_l2.count = 0;
		/* The steering VLAN entries are back as they were. */
		WRITE_ONCE(ep->lan_steer_blocked, false);
	}
	spin_unlock_irqrestore(&v2802_l2_lock, flags);
	return rc;
}

void __rtl9601d_l2_changed(const char *why)
{
	unsigned long flags;
	int rc = 0;

	spin_lock_irqsave(&v2802_l2_lock, flags);
	if (v2802_l2.ep && !v2802_l2.stopping) {
		v2802_l2.generation++;
		strscpy(v2802_l2.changed_by, why);
		if (v2802_l2.saved || (v2802_l2.ep->hw_up &&
		    (luna_xpon_lan_tagged() || v2802_l2_vlan_filtering(v2802_l2.ep))))
			rc = v2802_l2_gate_locked();
		if (rc)
			v2802_l2.error = rc;
		mod_delayed_work(system_wq, &v2802_l2.work, 0);
	}
	spin_unlock_irqrestore(&v2802_l2_lock, flags);
}
EXPORT_SYMBOL(__rtl9601d_l2_changed);

/* Pattern 0 TCAM stores positive and negative care bits separately.
 * Rule bit 48 is validity; direction 31, C-tag 3, VID 11..22, tag p-bits 8..10,
 * UNI 0..2.
 */
static int v2802_l2_rule_write(u32 data, u32 care, u32 *action, u32 enable)
{
	u32 rule[2] = { data & care, BIT(16) }, mask[2] = { ~data & care, 0 };
	unsigned int idx = V2802_L2_FIRST + v2802_l2.next_rule;
	int rc;

	if (v2802_l2.next_rule == V2802_L2_RULES)
		return -ENOSPC;
	v2802_l2.next_rule++;
	rc = v2802_l2_table(4, idx, mask, 2, true);
	if (!rc)
		rc = v2802_l2_table(5, idx, action, 3, true);
	if (rc)
		return rc;
	iowrite32(enable, v2802_l2.ep->sw + L34_CF_ACTION_CTRL + 4 * idx);
	__set_bit(idx - V2802_L2_FIRST, v2802_l2.rules);
	return v2802_l2_table(4, 256 + idx, rule, 2, true);
}

/* CF_ACTION_US and _DS share these, (lsb, width): CS_ACT 0,3; SVID 3,12; SPRI
 * 15,3; CSPRI_ACT 18,3; CSVID_ACT 21,2; CACT 33,2; CVID 35,12; CPRI 47,3;
 * CVID_ACT 50,3; CPRI_ACT 53,3. Raw values, from Realtek's cfg2raw tables: tag
 * action 1 adds or rewrites, C-tag 2 deletes; VID and priority action 1 assigns,
 * priority action 2 copies the frame's. S-tag action 2 uses TPID 0x8100, like
 * luna_xpon_tx_flow(). */
#define V2802_CF_CTAG_REWRITE(vid)	(BIT(1) | ((u32)(vid) << 3) | BIT(18) | (2 << 21))
#define V2802_CF_CTAG_DELETE		(2 << 1)
#define V2802_CF_STAG_ADD(vid)		(2 | ((u32)(vid) << 3) | (2 << 18) | BIT(21))
#define V2802_CF_ENABLE_ALL		0x3f	/* stock's, on every rule */

/* Upstream on the LAN VID, downstream on the service's. The switch's VLAN rules
 * can't rewrite a VID, so the tag operation comes from the classifier. */
static int v2802_l2_rewrite_up(const struct luna_l2_service *s, unsigned int port,
			       unsigned int flow, int pri)
{
	u32 action[3] = { (flow << 24) | BIT(23), 0, 0 };
	u32 care = BIT(31) | (VLAN_VID_MASK << 11) | BIT(3) | 7;
	u32 data = ((u32)s->lan_vid << 11) | BIT(3) | port;

	if (s->stacked)
		action[0] |= V2802_CF_STAG_ADD(s->vid);
	else
		action[1] = V2802_CF_CTAG_REWRITE(s->vid);
	if (pri >= 0) {
		care |= 7 << 8;
		data |= (u32)pri << 8;
	}
	return v2802_l2_rule_write(data, care, action, V2802_CF_ENABLE_ALL);
}

static int v2802_l2_rewrite_down(const struct luna_l2_service *s)
{
	u32 action[3] = { 0, s->stacked ? V2802_CF_CTAG_DELETE :
			     V2802_CF_CTAG_REWRITE(s->lan_vid), 0 };

	return v2802_l2_rule_write(BIT(31) | ((u32)s->vid << 11) | BIT(3),
				   BIT(31) | (VLAN_VID_MASK << 11) | BIT(3),
				   action, V2802_CF_ENABLE_ALL);
}

static int v2802_l2_rule(const struct luna_l2_service *s, unsigned int port,
			  bool untagged, unsigned int flow, int pri)
{
	u32 care = BIT(31) | BIT(3) | 7, data = port;
	u32 action[3] = { (flow << 24) | BIT(23), 0, 0 };
	u32 enable = L34_CF_SID_ENABLE;
	int pcp = READ_ONCE(v2802_l2.pcp[port]);

	/* RTL9602C CF_ACTION_US: CPRI_ACT [55:53], CPRI [49:47].
	 * Assign (raw 1) while keeping the VID and tag operation unchanged. */
	if (pcp >= 0) {
		action[1] = BIT(21) | ((u32)pcp << 15);
		enable |= BIT(1);
	}
	if (!untagged) {
		care |= VLAN_VID_MASK << 11;
		data |= ((u32)s->vid << 11) | BIT(3);
	}
	if (pri >= 0) {
		care |= 7 << 8;
		data |= (u32)pri << 8;
	}
	/* Switch VLAN rules own tag insertion; CF may override upstream PCP. */
	return v2802_l2_rule_write(data, care, action, enable);
}

/* Clients can mark an untagged service's frames with a VID 0 tag, as Windows
 * does with its QoS scheduler on. They match neither of its other rules. */
static int v2802_l2_prio_tag_rule(const struct luna_l2_service *s, unsigned int port)
{
	u32 action[3] = { ((u32)s->flow << 24) | BIT(23), 0, 0 };

	return v2802_l2_rule_write(BIT(3) | port,
				   BIT(31) | (VLAN_VID_MASK << 11) | BIT(3) | 7,
				   action, L34_CF_SID_ENABLE);
}

/* One rule for a tagged frame's GEM, or one per p-bit when the service splits.
 * A p-bit without a GEM has no rule, so its frames don't reach the PON. A port's
 * forced priority picks a single GEM whatever the frame carries. */
static int v2802_l2_tagged_rules(const struct luna_l2_service *s, unsigned int port)
{
	int pcp = READ_ONCE(v2802_l2.pcp[port]), rc = 0;
	unsigned int k;

	if (s->pcp_mask && pcp >= 0 && !(s->pcp_mask & BIT(pcp)))
		return 0;
	if (!s->pcp_mask || pcp >= 0) {
		unsigned int flow = s->pcp_mask ? s->pcp_flow[pcp] : s->flow;

		return v2802_l2_rewritten(s) ? v2802_l2_rewrite_up(s, port, flow, -1) :
		       v2802_l2_rule(s, port, false, flow, -1);
	}
	for (k = 0; !rc && k < 8; k++) {
		if (!(s->pcp_mask & BIT(k)))
			continue;
		rc = v2802_l2_rewritten(s) ? v2802_l2_rewrite_up(s, port, s->pcp_flow[k], k) :
		     v2802_l2_rule(s, port, false, s->pcp_flow[k], k);
	}
	return rc;
}

static void v2802_l2_log_split(struct rtl9602c_eth *ep, const struct luna_l2_service *s)
{
	char line[64];
	unsigned int k;
	int len = 0;

	if (!s->pcp_mask)
		return;
	for (k = 0; k < 8; k++)
		if (s->pcp_mask & BIT(k))
			len += scnprintf(line + len, sizeof(line) - len, " p%u:SID%u", k, s->pcp_flow[k]);
	netdev_info(ep->ndev, "V2802RH L2 offload: VLAN %u split,%s, unmarked SID%u\n",
		    s->vid, line, s->flow);
}

static int v2802_l2_install(const struct luna_l2_service *service, int count,
			     unsigned int generation)
{
	struct rtl9602c_eth *ep = v2802_l2.ep;
	unsigned long flags;
	u32 cfg, w[2], pvid, members, untag, accept;
	unsigned int i, p;
	bool management_service = false;
	int rc = 0;

	if (count <= 0 || count > V2802_L2_VLANS)
		return -ENOSPC;
	if (!v2802_l2_gate_supported(ep))
		return -EOPNOTSUPP;
	if (generation != READ_ONCE(v2802_l2.generation)) {
		rc = -EAGAIN;
		goto out;
	}
	if (v2802_l2.saved) {
		rc = -EBUSY;
		goto out;
	}
	if (!(ioread32(ep->sw + SW_VLAN_CTRL) & SW_VLAN_FILTERING)) {
		rc = -EOPNOTSUPP;
		goto out;
	}
	cfg = ioread32(ep->sw + L34_CF_CFG);
	if (V2802_L2_FIRST + V2802_L2_RULES >
	    256 - ((cfg >> L34_CF_P1_COUNT_SH) & 255)) {
		rc = -ENOSPC;
		goto out;
	}
	/* Do not overwrite another classifier owner, including the stock firmware. */
	for (i = 0; i < V2802_L2_RULES; i++) {
		rc = v2802_l2_table(4, 256 + V2802_L2_FIRST + i, w, 2, false);
		if (rc)
			goto out;
		if (w[1] & BIT(16)) {
			rc = -EBUSY;
			goto out;
		}
	}
	for (i = 0; i < count; i++) {
		if ((service[i].cpu_only || v2802_l2_rewritten(&service[i])) && service[i].local &&
		    v2802_l2_vlan_reserved(service[i].vid)) {
			rc = -EBUSY;
			goto out;
		}
		management_service |= service[i].vid == SW_DEFAULT_VID;
		/* A service on a jack's steering VLAN takes that entry: a
		 * steered frame would then reach the PON too. Plain frames
		 * until it is withdrawn (v2802_l2_clear restores the entry). */
		if (v2802_l2_steer_vid(service[i].vid)) {
			if (!READ_ONCE(ep->lan_steer_blocked))
				netdev_warn(ep->ndev,
					    "[L2-008] service VLAN %u is a jack's steering VLAN: lanN frames go plain while it is provisioned\n",
					    service[i].vid);
			WRITE_ONCE(ep->lan_steer_blocked, true);
		}
		rc = v2802_l2_table(1, service[i].vid, &v2802_l2.vlan[i], 1, false);
		if (rc)
			goto out;
	}
	if (!management_service) {
		rc = v2802_l2_table(1, SW_DEFAULT_VID,
				     &v2802_l2.management_vlan, 1, false);
		if (rc)
			goto out;
	}
	spin_lock_irqsave(&v2802_l2_lock, flags);
	if (generation != v2802_l2.generation || v2802_l2.stopping) {
		spin_unlock_irqrestore(&v2802_l2_lock, flags);
		rc = -EAGAIN;
		goto out;
	}
	v2802_l2.management_saved = !management_service;
	memcpy(v2802_l2.service, service, count * sizeof(*service));
	v2802_l2.count = count;
	v2802_l2.pvid = pvid = ioread32(ep->sw + SW_VLAN_PB_VID);
	v2802_l2.accept = accept = ioread32(ep->sw + SW_VLAN_PORT_ACCEPT_FRAME_TYPE);
	v2802_l2.cfg = cfg;
	v2802_l2.flood[0] = ioread32(ep->sw + ep->swm->bc_flood);
	v2802_l2.flood[1] = ioread32(ep->sw + ep->swm->unkn_mc_flood);
	v2802_l2.flood[2] = ioread32(ep->sw + ep->swm->unkn_uc_flood);
	v2802_l2.saved = true;
	rc = v2802_l2_gate_locked();
	spin_unlock_irqrestore(&v2802_l2_lock, flags);
	if (rc)
		goto rollback;
	/* The management VLAN must not become a second downstream LAN path. */
	if (v2802_l2.management_saved) {
		w[0] = v2802_l2.management_vlan & ~(BIT(2) | BIT(6));
		rc = v2802_l2_table(1, SW_DEFAULT_VID, w, 1, true);
		if (rc)
			goto rollback;
	}
	v2802_l2.next_rule = 0;
	/* PON classifier selected; unmatched upstream is permitted without PON. */
	iowrite32((cfg & ~3u) | BIT(3) | 1, ep->sw + L34_CF_CFG);
	for (i = 0; i < count; i++) {
		const struct luna_l2_service *s = &service[i];

		members = (s->cpu_only && !s->local ? 0 : s->lan_mask) |
			  (s->local && !v2802_l2_rewritten(s) ? 0 : BIT(2)) | BIT(3);
		/* Keep the service VID on CPU copies for accurate forwarding marks. */
		untag = s->untag_mask;
		/* IVL, as for the WAN VIDs: a shared FID drops unicast replies. A
		 * rewritten service needs one though: clients are learned in the LAN
		 * VID and looked up in the service's, both of which hold their port. */
		w[0] = BIT(11) | (v2802_l2_rewritten(s) ? 0 : SW_VLAN_IVL) | (untag << 4) | members;
		rc = v2802_l2_table(1, s->vid, w, 1, true);
		if (rc)
			goto rollback;
		for (p = 0; p < 2; p++) {
			if (!(s->lan_mask & BIT(p)))
				continue;
			if (s->pvid_mask & BIT(p))
				pvid = sw_packed_ins(pvid, p, SW_VLAN_PB_VID_BITS, s->vid);
			if (s->local || s->cpu_only)
				continue;
			if (s->pvid_mask & BIT(p)) {
				rc = v2802_l2_rule(s, p, true, s->flow, -1);
				if (!rc)
					rc = v2802_l2_prio_tag_rule(s, p);
				if (rc)
					goto rollback;
			}
			rc = v2802_l2_tagged_rules(s, p);
			if (rc)
				goto rollback;
		}
		if (v2802_l2_rewritten(s) && !s->local) {
			rc = v2802_l2_rewrite_down(s);
			if (rc)
				goto rollback;
		}
	}
	rc = v2802_l2_own_macs(service, count, true);
	if (rc)
		goto rollback;
	if (v2802_l2_vlan_filtering(ep))
		v2802_l2_vlan_ingress(ep, &pvid, &accept);
	iowrite32(accept, ep->sw + SW_VLAN_PORT_ACCEPT_FRAME_TYPE);
	iowrite32(pvid, ep->sw + SW_VLAN_PB_VID);
	iowrite32(v2802_l2.flood[0] | BIT(2), ep->sw + ep->swm->bc_flood);
	iowrite32(v2802_l2.flood[1] | BIT(2), ep->sw + ep->swm->unkn_mc_flood);
	iowrite32(v2802_l2.flood[2] | BIT(2), ep->sw + ep->swm->unkn_uc_flood);
	spin_lock_irqsave(&v2802_l2_lock, flags);
	if (generation != v2802_l2.generation || v2802_l2.stopping) {
		rc = -EAGAIN;
	} else {
		rc = v2802_l2_ungate_locked();
		if (!rc)
			v2802_l2.active = true;
	}
	spin_unlock_irqrestore(&v2802_l2_lock, flags);
	if (rc)
		goto rollback;
	goto out;
rollback:
	if (!READ_ONCE(ep->in_recovery) && v2802_l2_clear())
		netdev_err(ep->ndev, "[L2-009] L2 offload rollback incomplete\n");
out:
	return rc;
}

static bool v2802_l2_flags(const struct net_device *dev, unsigned long flags)
{
	unsigned int bit;

	for_each_set_bit(bit, &flags, BITS_PER_LONG)
		if (!br_port_flag_is_set(dev, BIT(bit)))
			return false;
	return true;
}

/* One bridge port as the offload needs it. A jack with no cable, or one
 * taken down (the switch then holds its link down too), carries nothing the
 * offload could leak, so it may be DISABLED; anything else must forward. */
static bool v2802_l2_port_ready(const struct net_device *dev, bool jack)
{
	unsigned long required = BR_LEARNING | BR_FLOOD | BR_MCAST_FLOOD | BR_BCAST_FLOOD;
	u8 state;

	if (!v2802_l2_flags(dev, required) ||
	    br_port_flag_is_set(dev, BR_ISOLATED | BR_PORT_LOCKED | BR_HAIRPIN_MODE))
		return false;
	state = br_port_get_stp_state(dev);
	if (state == BR_STATE_FORWARDING)
		return netif_running(dev);
	return jack && state == BR_STATE_DISABLED &&
	       (!netif_running(dev) || !netif_carrier_ok(dev));
}

/* The bridge is pon0 and the LAN side: eth0 (a configuration from before the
 * jacks had netdevs) or every jack's netdev, never both, nothing else. Every
 * jack belongs to it: a jack outside the bridge is still a switch port the
 * offload would forward through. */
static bool v2802_l2_bridge_ready(struct rtl9602c_eth *ep)
{
	struct net_device *br = v2802_l2_bridge(ep);
	struct net_device *lower;
	struct list_head *iter;
	bool eth0 = false, jacks = false;
	unsigned int p;

	if (!br || !netif_is_bridge_master(br) || !ep->wan_ndev)
		return false;
	netdev_for_each_lower_dev(br, lower, iter) {
		if (lower == ep->wan_ndev)
			continue;
		if (lower == ep->ndev)
			eth0 = true;
		else if (v2802_l2_lan_member(ep, lower))
			jacks = true;
		else
			return false;
	}
	if (eth0 == jacks || (eth0 && v2802_l2_vlan_filtering(ep)))
		return false;
	if (!ep->hw_up || ep->closing || ep->in_recovery ||
	    br_multicast_enabled(br) ||
	    !v2802_l2_port_ready(ep->wan_ndev, false))
		return false;
	if (eth0)
		return v2802_l2_port_ready(ep->ndev, false);
	for (p = 0; p < LAN_PORTS; p++) {
		if (!ep->lan_ndev[p])
			continue;
		if (netdev_master_upper_dev_get(ep->lan_ndev[p]) != br ||
		    !v2802_l2_port_ready(ep->lan_ndev[p], true))
			return false;
	}
	return true;
}

/* The bridge ports, offloaded for their FDB notifications. */
static void v2802_l2_unoffload(struct rtl9602c_eth *ep)
{
	while (v2802_l2.n_offloaded)
		switchdev_bridge_port_unoffload(v2802_l2.offloaded[--v2802_l2.n_offloaded],
						ep, &v2802_l2.switchdev_nb, NULL);
}

static int v2802_l2_offload_port(struct rtl9602c_eth *ep, struct net_device *dev)
{
	int rc = switchdev_bridge_port_offload(dev, dev, ep, &v2802_l2.switchdev_nb,
					       NULL, false, NULL);

	if (!rc)
		v2802_l2.offloaded[v2802_l2.n_offloaded++] = dev;
	return rc;
}

static int v2802_l2_offload(struct rtl9602c_eth *ep)
{
	struct net_device *br = v2802_l2_bridge(ep);
	unsigned int p;
	int rc;

	rc = v2802_l2_offload_port(ep, ep->wan_ndev);
	if (!rc && netdev_master_upper_dev_get(ep->ndev) == br)
		rc = v2802_l2_offload_port(ep, ep->ndev);
	for (p = 0; !rc && p < LAN_PORTS; p++)
		if (ep->lan_ndev[p] && netdev_master_upper_dev_get(ep->lan_ndev[p]) == br)
			rc = v2802_l2_offload_port(ep, ep->lan_ndev[p]);
	if (rc)
		v2802_l2_unoffload(ep);
	return rc;
}

/* The VLAN filter drops a VID the table does not hold, the OLT's service
 * VLANs included: give each the PON and the CPU, as a pon0.<vid> would. The
 * offload saves these entries and restores them when it stops. */
static int v2802_l2_sync_vids(struct rtl9602c_eth *ep)
{
	unsigned long *want = v2802_l2.want_vids, *have = v2802_l2.wan_vids;
	unsigned int vid;
	u32 vlan;
	int rc;

	for (vid = 1; vid < VLAN_VID_MASK; vid++) {
		if (!v2802_l2_managed_vid(vid) ||
		    (test_bit(vid, want) == test_bit(vid, have) && !test_bit(vid, v2802_l2.dirty_vids)))
			continue;
		vlan = v2802_l2_cpu_vlan(test_bit(vid, want));
		__set_bit(vid, v2802_l2.dirty_vids);
		rc = v2802_l2_table(1, vid, &vlan, 1, true);
		if (rc)
			return rc;
		__assign_bit(vid, have, test_bit(vid, want));
		__clear_bit(vid, v2802_l2.dirty_vids);
	}
	return 0;
}

/* VLAN-device callbacks share the OMCI VLAN owner, including while offloaded. */
static int v2802_l2_netdev_vid(struct rtl9602c_eth *ep, u16 vid, bool add)
{
	bool old;
	int rc;

	ASSERT_RTNL();
	if (!v2802_l2_managed_vid(vid))
		return 0;
	old = test_bit(vid, v2802_l2.netdev_vids);
	__assign_bit(vid, v2802_l2.netdev_vids, add);
	v2802_l2_want_vids();
	if (v2802_l2.saved) {
		rtl9601d_l2_changed();
		return 0;
	}
	rc = v2802_l2_sync_vids(ep);
	if (rc) {
		__assign_bit(vid, v2802_l2.netdev_vids, old);
		rtl9601d_l2_changed();
	}
	return rc;
}

static void v2802_ds_replay(void);

static void v2802_l2_work(struct work_struct *work)
{
	struct luna_l2_service *services = v2802_l2.next_plan;
	struct rtl9602c_eth *ep = v2802_l2.ep;
	unsigned long flags;
	unsigned int generation;
	int count, rc, storm_rc;
	unsigned int i, cpu_only = 0;
	bool retry = false;

	rtnl_lock();
	if (READ_ONCE(v2802_l2.stopping) || READ_ONCE(ep->in_recovery) || !ep->hw_up)
		goto unlock;
	generation = READ_ONCE(v2802_l2.generation);
	v2802_l2_want_vids();
	rc = v2802_l2_clear();
	if (rc)
		goto out;
	v2802_l2_unoffload(ep);
	spin_lock_irqsave(&v2802_l2_lock, flags);
	rc = luna_xpon_lan_tagged() || v2802_l2_vlan_filtering(ep) ?
		v2802_l2_gate_locked() : v2802_l2_ungate_locked();
	spin_unlock_irqrestore(&v2802_l2_lock, flags);
	if (rc)
		goto out;
	rc = v2802_l2_sync_vids(ep);
	if (!rc)
		rc = v2802_l2_vlan_fallback(ep);
	retry = !!rc;
	if (rc || !READ_ONCE(v2802_l2.requested))
		goto out;
	if (!v2802_l2_bridge_ready(ep)) {
		rc = -EOPNOTSUPP;
		goto out;
	}
	count = luna_xpon_l2_services(services, v2802_l2_vlan_filtering(ep));
	if (count >= 0)
		count = v2802_l2_vlan_plan(ep, services, count);
	if (count <= 0) {
		rc = count;
		goto out;
	}
	rc = v2802_l2_offload(ep);
	if (rc)
		goto out;
	rc = v2802_l2_install(services, count, generation);
	if (rc && !v2802_l2.saved)
		v2802_l2_unoffload(ep);
	if (!rc) {
		for (i = 0; i < count; i++) {
			cpu_only += services[i].cpu_only;
			v2802_l2_log_split(ep, &services[i]);
		}
		netdev_info(ep->ndev, "V2802RH L2 offload: %d VLAN bindings (%u CPU-only), after %s\n",
			    count, cpu_only, v2802_l2.changed_by);
	}
out:
	storm_rc = v2802_l2_storm(ep, v2802_l2.active && !cpu_only);
	if (storm_rc) {
		dev_err_ratelimited(ep->dev, "[L2-010] L2 fallback storm control readback failed: %d\n", storm_rc);
		rc = storm_rc;
		retry = true;
	}
	WRITE_ONCE(v2802_l2.error, rc);
	v2802_ds_replay();
	spin_lock_irqsave(&v2802_l2_lock, flags);
	if (rc && (v2802_l2.saved || retry) && !v2802_l2.stopping) {
		v2802_l2.retries++;
		mod_delayed_work(system_wq, &v2802_l2.work, msecs_to_jiffies(100));
	}
	spin_unlock_irqrestore(&v2802_l2_lock, flags);
unlock:
	rtnl_unlock();
}

static int v2802_l2_netdev_event(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct rtl9602c_eth *ep = v2802_l2.ep;
	struct net_device *br;
	bool changed;

	/* Only what v2802_l2_bridge_ready() and the own-MAC entries read;
	 * bridge and port attributes arrive through switchdev. Carrier or MTU
	 * events would otherwise withdraw and flush the offload for nothing. */
	if (event != NETDEV_UP && event != NETDEV_GOING_DOWN &&
	    event != NETDEV_CHANGEUPPER && event != NETDEV_UNREGISTER &&
	    event != NETDEV_CHANGEADDR)
		return NOTIFY_DONE;
	rcu_read_lock();
	br = ep->wan_ndev ? netdev_master_upper_dev_get_rcu(ep->wan_ndev) : NULL;
	changed = v2802_l2_lan_member(ep, dev) || dev == ep->wan_ndev || dev == br;
	if (event == NETDEV_CHANGEUPPER) {
		struct netdev_notifier_changeupper_info *info = ptr;

		changed |= br && info->upper_dev == br;
	}
	rcu_read_unlock();
	if (changed)
		rtl9601d_l2_changed();
	return NOTIFY_DONE;
}

/* Whether the running offload is already what a rebuild would install. */
static bool v2802_l2_plan_unchanged(struct rtl9602c_eth *ep)
{
	int count, i;

	ASSERT_RTNL();
	if (!READ_ONCE(v2802_l2.active) || !READ_ONCE(v2802_l2.requested) ||
	    !v2802_l2_bridge_ready(ep))
		return false;
	count = luna_xpon_l2_services(v2802_l2.plan, v2802_l2_vlan_filtering(ep));
	if (count >= 0)
		count = v2802_l2_vlan_plan(ep, v2802_l2.plan, count);
	if (count <= 0 || count != v2802_l2.count)
		return false;
	for (i = 0; i < count; i++) {
		const struct luna_l2_service *a = &v2802_l2.plan[i], *b = &v2802_l2.service[i];

		if (a->vid != b->vid || a->flow != b->flow || a->lan_vid != b->lan_vid ||
		    a->lan_mask != b->lan_mask || a->untag_mask != b->untag_mask ||
		    a->pvid_mask != b->pvid_mask || a->local != b->local ||
		    a->untagged != b->untagged || a->cpu_only != b->cpu_only ||
		    a->translated != b->translated || a->stacked != b->stacked ||
		    a->pcp_mask != b->pcp_mask || memcmp(a->pcp_flow, b->pcp_flow, sizeof(a->pcp_flow)))
			return false;
	}
	return true;
}

#define V2802_ACL_EN		0x15040
#define V2802_ACL_PERMIT	0x15044	/* unmatched frames pass, per port */
#define V2802_ACL_TEMPLATE	0x15008
#define V2802_ACL_ACT_CTRL	0x15048	/* per rule */
#define   V2802_ACL_ACT_LOG	BIT(3)
#define V2802_ACL_MATCH_TYPE	2	/* masks from 0, rules from V2802_ACL_RULE_BASE */
#define V2802_ACL_ACTION_TYPE	3
#define V2802_ACL_RULE_BASE	128
#define V2802_METER		0x25004
#define V2802_METER_RATE_MASK	0x7ffff	/* [50:32], 8 kbit/s */
#define V2802_METER_LAST	7	/* stock's storm controls hold 8-15 */
#define V2802_METER_BUCKET	0x3fff	/* [16:1], stock's: at 0xffff the meter never limits */
#define V2802_METER_EXCEED	0x25084
#define V2802_ACL_HIT		0x15000	/* last rule hit by each port */

static DEFINE_MUTEX(v2802_ds_lock);
static struct luna_ds_rate v2802_ds[LUNA_DS_RATES];
static unsigned int v2802_ds_count, v2802_ds_programmed;
/* What a rule matches: template, its field, the bits of the VID compared, and the
 * log action, and the ports it applies to (0 for PON). Settable through
 * l2_ds_rate_debug, as no OLT or fixture sets a rate. */
static struct {
	u32 tmpl, field, care, log, ports;
} v2802_ds_rule = { 0, 6, VLAN_VID_MASK, 0, 0 };

/* Which word of the 64-bit register holds the rate: it has one writable bit more.
 * Probed once, on the meter written first, before anything depends on it. */
static bool v2802_meter_rate_first(struct rtl9602c_eth *ep)
{
	void __iomem *m = ep->sw + V2802_METER + 8 * V2802_METER_LAST;
	static int rate_first = -1;

	if (rate_first < 0) {
		iowrite32(~0u, m);
		rate_first = ioread32(m) == V2802_METER_RATE_MASK;
	}
	return rate_first;
}

static void v2802_meter_write(struct rtl9602c_eth *ep, unsigned int idx, u32 bytes_per_s)
{
	void __iomem *m = ep->sw + V2802_METER + 8 * idx;
	bool rate_first = v2802_meter_rate_first(ep);
	u32 rate = clamp_t(u32, bytes_per_s / 1000, 1, V2802_METER_RATE_MASK);
	u32 bucket = V2802_METER_BUCKET << 1;

	iowrite32(rate_first ? rate : bucket, m);
	iowrite32(rate_first ? bucket : rate, m + 4);
}

/* Template 0 as stock has it: DMAC, SMAC, the C-tag in field 6, which is bits
 * [127:112] of a rule, and the S-tag in field 7. Active ports [15:10], tag flags [9:3], valid 144. The mask is
 * the rule's complement within what it cares about. Log mode [36:35], 0 polices, meter [34:30]. */
static int v2802_ds_rule_write(struct rtl9602c_eth *ep, unsigned int idx,
			       const struct luna_ds_rate *r)
{
	u32 ports = v2802_ds_rule.ports ?: BIT(ep->swm->pon_port);
	u32 rule[5] = { 0 }, mask[5] = { 0 }, action[3] = { 0 };
	unsigned int meter = V2802_METER_LAST - idx;
	unsigned long flags;
	unsigned int bit = 16 + 16 * v2802_ds_rule.field;
	int rc;

	if (r) {
		v2802_meter_write(ep, meter, r->bytes_per_s);
		rule[0] = (ports << 10) | v2802_ds_rule.tmpl;
		mask[0] = ((~ports & 0x3f) << 10) | (~v2802_ds_rule.tmpl & 7);
		rule[bit / 32] |= (r->vid & v2802_ds_rule.care) << (bit % 32);
		mask[bit / 32] |= (~r->vid & v2802_ds_rule.care) << (bit % 32);
		rule[4] |= BIT(16);
		action[0] = meter << 30;
		action[1] = (meter >> 2) | (v2802_ds_rule.log << 3);
	}
	iowrite32(0, ep->sw + V2802_ACL_ACT_CTRL + 4 * idx);
	/* The old rule goes first: a new mask must not widen what it matches. */
	rc = v2802_l2_table(V2802_ACL_MATCH_TYPE, V2802_ACL_RULE_BASE + idx,
			    (u32 [5]){ 0 }, 5, true);
	if (rc || !r)
		return rc;
	rc = v2802_l2_table(V2802_ACL_ACTION_TYPE, idx, action, 3, true);
	if (rc)
		return rc;
	iowrite32(V2802_ACL_ACT_LOG, ep->sw + V2802_ACL_ACT_CTRL + 4 * idx);
	rc = v2802_l2_table(V2802_ACL_MATCH_TYPE, V2802_ACL_RULE_BASE + idx, rule, 5, true);
	if (rc)
		return rc;
	/* Rule then mask, as stock. A mask reads back with the rule's valid bit,
	 * so it can't go through the checked write. */
	spin_lock_irqsave(&ep->sw_table_lock, flags);
	rc = v2802_l2_xact(V2802_ACL_MATCH_TYPE, idx, mask, 5, true);
	spin_unlock_irqrestore(&ep->sw_table_lock, flags);
	return rc;
}

/* Nothing touches the ACL block until a rate is set, and it is off again without one. */
static void v2802_ds_apply(void)
{
	struct rtl9602c_eth *ep = v2802_l2.ep;
	u32 port = (v2802_ds_rule.ports ?: BIT(ep->swm->pon_port)) & 0xf;
	unsigned int i;
	int rc = 0;

	lockdep_assert_held(&v2802_ds_lock);
	if (!ep->hw_up || READ_ONCE(ep->in_recovery) || (!v2802_ds_count && !v2802_ds_programmed))
		return;
	if (v2802_ds_count) {
		iowrite32(0x0080c101, ep->sw + V2802_ACL_TEMPLATE);
		iowrite32(0x01024305, ep->sw + V2802_ACL_TEMPLATE + 4);
		iowrite32(0x02240992, ep->sw + V2802_ACL_TEMPLATE + 8);
		iowrite32(0x014d43b1, ep->sw + V2802_ACL_TEMPLATE + 12);
		iowrite32(ioread32(ep->sw + V2802_ACL_PERMIT) | port, ep->sw + V2802_ACL_PERMIT);
	}
	for (i = 0; !rc && i < LUNA_DS_RATES; i++) {
		if (i < v2802_ds_count)
			rc = v2802_ds_rule_write(ep, i, &v2802_ds[i]);
		else if (i < v2802_ds_programmed)
			rc = v2802_ds_rule_write(ep, i, NULL);
	}
	iowrite32(v2802_ds_count ? port : 0, ep->sw + V2802_ACL_EN);
	if (rc)
		netdev_err(ep->ndev, "[QOS-016] downstream rate rules failed: %d\n", rc);
	v2802_ds_programmed = v2802_ds_count;
}

static ssize_t l2_ds_rate_debug_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct rtl9602c_eth *ep = v2802_l2.ep;
	void __iomem *m = ep->sw + V2802_METER + 8 * V2802_METER_LAST;
	u32 rule[5] = { 0 }, mask[5] = { 0 }, action[3] = { 0 };
	int rc;

	rc = v2802_l2_table(V2802_ACL_MATCH_TYPE, V2802_ACL_RULE_BASE, rule, 5, false) ?:
	     v2802_l2_table(V2802_ACL_MATCH_TYPE, 0, mask, 5, false) ?:
	     v2802_l2_table(V2802_ACL_ACTION_TYPE, 0, action, 3, false);
	return sysfs_emit(buf,
			  "rule: tmpl=%u field=%u care=%#x log=%u ports=%#x count=%u programmed=%u\n"
			  "en=%#x permit=%#x hit=%08x %08x exceed=%08x act_ctrl=%#x\n"
			  "tmpl0=%08x %08x tmpl1=%08x %08x\n"
			  "meter%u=%08x %08x rc=%d\n"
			  "acl_rule=%08x %08x %08x %08x %08x\n"
			  "acl_mask=%08x %08x %08x %08x %08x\n"
			  "acl_action=%08x %08x %08x\n",
			  v2802_ds_rule.tmpl, v2802_ds_rule.field, v2802_ds_rule.care,
			  v2802_ds_rule.log, v2802_ds_rule.ports, v2802_ds_count, v2802_ds_programmed,
			  ioread32(ep->sw + V2802_ACL_EN), ioread32(ep->sw + V2802_ACL_PERMIT),
			  ioread32(ep->sw + V2802_ACL_HIT), ioread32(ep->sw + V2802_ACL_HIT + 4),
			  ioread32(ep->sw + V2802_METER_EXCEED),
			  ioread32(ep->sw + V2802_ACL_ACT_CTRL),
			  ioread32(ep->sw + V2802_ACL_TEMPLATE), ioread32(ep->sw + V2802_ACL_TEMPLATE + 4),
			  ioread32(ep->sw + V2802_ACL_TEMPLATE + 8), ioread32(ep->sw + V2802_ACL_TEMPLATE + 12),
			  V2802_METER_LAST, ioread32(m), ioread32(m + 4), rc,
			  rule[0], rule[1], rule[2], rule[3], rule[4],
			  mask[0], mask[1], mask[2], mask[3], mask[4],
			  action[0], action[1], action[2]);
}

static ssize_t l2_ds_rate_debug_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	u32 tmpl, field, care, log, ports = 0;

	if (sscanf(buf, "%u %u %i %u %i", &tmpl, &field, &care, &log, &ports) < 4 ||
	    tmpl > 6 || field > 7 || care > 0xffff || log > 3 || ports > 0x3f)
		return -EINVAL;
	mutex_lock(&v2802_ds_lock);
	v2802_ds_rule.tmpl = tmpl;
	v2802_ds_rule.field = field;
	v2802_ds_rule.care = care;
	v2802_ds_rule.log = log;
	v2802_ds_rule.ports = ports;
	v2802_ds_apply();
	mutex_unlock(&v2802_ds_lock);
	return count;
}
static DEVICE_ATTR_RW(l2_ds_rate_debug);

/* Live register and table access for bring-up, printed to the kernel log:
 *   r <off> [words]             read switch registers
 *   w <off> <value>             write an ACL or meter register
 *   t <type> <addr> [words]     read a table entry
 *   T <type> <addr> <word>...   write a VLAN, ACL or classifier entry (types 1-5), unchecked
 *   s <ms>                      sample ACL hits and meter exceed under traffic
 * Only ranges known to answer: a stray switch access can hang the bus. */
static const u32 v2802_dbg_rd[][2] = {
	{ 0x11000, 0x111fc }, { 0x13000, 0x1300c }, { 0x15000, 0x153fc },
	{ 0x16000, 0x16434 }, { 0x17000, 0x1700c }, { 0x1c000, 0x1c2fc },
	{ 0x23000, 0x2311c }, { 0x25000, 0x25090 }, { 0x2a000, 0x2a024 },
	{ 0x2d800, 0x2d8bc },
};
static const u32 v2802_dbg_wr[][2] = {
	{ 0x15000, 0x153fc }, { 0x16038, 0x16434 }, { 0x25000, 0x25090 },
};

static bool v2802_dbg_allowed(const u32 (*range)[2], unsigned int n, u32 off)
{
	unsigned int i;

	if (off & 3)
		return false;
	for (i = 0; i < n; i++)
		if (off >= range[i][0] && off <= range[i][1])
			return true;
	return false;
}

/* 0x15000 holds, per action type, the rule that won it for the last frame. */
static void v2802_dbg_sample(struct rtl9602c_eth *ep, unsigned int ms)
{
	static const char * const slot[] = {
		"cvlan", "svlan", "pri", "log", "forward", "intr", "route"
	};
	unsigned long end = jiffies + msecs_to_jiffies(ms);
	unsigned int count[ARRAY_SIZE(slot)] = { 0 }, samples = 0, i;
	u8 last[ARRAY_SIZE(slot)];
	u32 exceed = 0;

	memset(last, 0xff, sizeof(last));
	while (time_before(jiffies, end)) {
		u32 w[2] = { ioread32(ep->sw + V2802_ACL_HIT),
			     ioread32(ep->sw + V2802_ACL_HIT + 4) };

		for (i = 0; i < ARRAY_SIZE(slot); i++) {
			u8 rule = w[i / 4] >> (8 * (i % 4));

			if (rule != 0xff) {
				count[i]++;
				last[i] = rule;
			}
		}
		exceed |= ioread32(ep->sw + V2802_METER_EXCEED);
		samples++;
		usleep_range(200, 500);
	}
	pr_info("l2dbg: %u samples in %u ms, meter exceed %04x\n", samples, ms, exceed);
	for (i = 0; i < ARRAY_SIZE(slot); i++)
		pr_info("l2dbg: %-7s hit %u/%u, last rule %u\n", slot[i], count[i], samples, last[i]);
}

static ssize_t l2_sw_debug_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct rtl9602c_eth *ep = v2802_l2.ep;
	u32 a[7], w[5] = { 0 };
	unsigned long flags;
	unsigned int i;
	char op;
	int n, rc = 0;

	n = sscanf(buf, "%c %i %i %i %i %i %i %i", &op, &a[0], &a[1], &a[2],
		   &a[3], &a[4], &a[5], &a[6]) - 1;
	if (n < 1)
		return -EINVAL;

	switch (op) {
	case 'r':
		for (i = 0; i < (n > 1 ? min(a[1], 64u) : 1); i++) {
			u32 off = a[0] + 4 * i;

			if (!v2802_dbg_allowed(v2802_dbg_rd, ARRAY_SIZE(v2802_dbg_rd), off))
				return -EPERM;
			pr_info("l2dbg: reg %05x = %08x\n", off, ioread32(ep->sw + off));
		}
		break;
	case 'w':
		if (n != 2)
			return -EINVAL;
		if (!v2802_dbg_allowed(v2802_dbg_wr, ARRAY_SIZE(v2802_dbg_wr), a[0]))
			return -EPERM;
		iowrite32(a[1], ep->sw + a[0]);
		pr_info("l2dbg: reg %05x = %08x after write %08x\n", a[0],
			ioread32(ep->sw + a[0]), a[1]);
		break;
	case 't':
		if (n < 2 || a[0] < 1 || a[0] > 5 || a[1] > 0x1ff)
			return -EINVAL;
		n = n > 2 ? clamp(a[2], 1u, 5u) : 5;
		spin_lock_irqsave(&ep->sw_table_lock, flags);
		rc = v2802_l2_xact(a[0], a[1], w, n, false);
		spin_unlock_irqrestore(&ep->sw_table_lock, flags);
		pr_info("l2dbg: table %u[%u] = %08x %08x %08x %08x %08x rc=%d\n", a[0], a[1],
			w[0], w[1], w[2], w[3], w[4], rc);
		break;
	case 'T':
		if (n < 3 || a[0] < 1 || a[0] > 5 || a[1] > 0x1ff)
			return -EINVAL;
		spin_lock_irqsave(&ep->sw_table_lock, flags);
		rc = v2802_l2_xact(a[0], a[1], &a[2], n - 2, true);
		spin_unlock_irqrestore(&ep->sw_table_lock, flags);
		pr_info("l2dbg: wrote table %u[%u] rc=%d\n", a[0], a[1], rc);
		break;
	case 's':
		v2802_dbg_sample(ep, clamp(a[0], 1u, 30000u));
		break;
	default:
		return -EINVAL;
	}
	return rc ?: count;
}
static DEVICE_ATTR_WO(l2_sw_debug);

/* A chip reset loses the ACL block. */
static void v2802_ds_replay(void)
{
	mutex_lock(&v2802_ds_lock);
	if (v2802_ds_count)
		v2802_ds_apply();
	mutex_unlock(&v2802_ds_lock);
}

void rtl9601d_ds_rates_set(const struct luna_ds_rate *rates, unsigned int count)
{
	if (!v2802_l2.ep)
		return;
	mutex_lock(&v2802_ds_lock);
	count = min_t(unsigned int, count, LUNA_DS_RATES);
	if (count != v2802_ds_count || memcmp(rates, v2802_ds, count * sizeof(*rates))) {
		unsigned int i;

		memcpy(v2802_ds, rates, count * sizeof(*rates));
		v2802_ds_count = count;
		v2802_ds_apply();
		for (i = 0; i < count; i++)
			pr_info("luna-xpon: downstream limit: VLAN %u at %u byte/s\n",
				rates[i].vid, rates[i].bytes_per_s);
		if (!count)
			pr_info("luna-xpon: downstream limits removed\n");
	}
	mutex_unlock(&v2802_ds_lock);
}
EXPORT_SYMBOL(rtl9601d_ds_rates_set);

static int v2802_l2_switchdev_event(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct switchdev_notifier_info *info = ptr;
	struct rtl9602c_eth *ep = v2802_l2.ep;
	struct net_device *br;
	char why[32];
	int attr = -1;

	if (event == SWITCHDEV_FDB_ADD_TO_DEVICE || event == SWITCHDEV_FDB_DEL_TO_DEVICE) {
		struct switchdev_notifier_fdb_info *fdb = ptr;

		if (info->ctx == ep) {
			if (event == SWITCHDEV_FDB_ADD_TO_DEVICE && fdb->added_by_user && !fdb->is_local)
				return notifier_from_errno(-EBUSY);
			return NOTIFY_DONE;
		}
		if (!fdb->added_by_user || fdb->is_local)
			return NOTIFY_DONE;
	} else if (event == SWITCHDEV_PORT_OBJ_ADD || event == SWITCHDEV_PORT_OBJ_DEL) {
		const struct switchdev_notifier_port_obj_info *oi = ptr;

		if (oi->obj->id != SWITCHDEV_OBJ_ID_PORT_VLAN)
			return NOTIFY_DONE;
	} else if (event != SWITCHDEV_PORT_ATTR_SET) {
		return NOTIFY_DONE;
	}
	/* A jack's cable moves its bridge port between DISABLED and FORWARDING, and
	 * the offload is fine with either. Withdrawing and reinstalling the services
	 * on every plug would flush the LAN for nothing. VLAN objects are notified
	 * before the bridge applies them, so only attributes can be judged here. */
	if (event == SWITCHDEV_PORT_ATTR_SET) {
		const struct switchdev_notifier_port_attr_info *ai = ptr;

		attr = ai->attr->id;
		if (attr != SWITCHDEV_ATTR_ID_BRIDGE_VLAN_FILTERING && v2802_l2_plan_unchanged(ep))
			return NOTIFY_DONE;
	}
	rcu_read_lock();
	br = ep->wan_ndev ? netdev_master_upper_dev_get_rcu(ep->wan_ndev) : NULL;
	rcu_read_unlock();
	if (v2802_l2_lan_member(ep, info->dev) || info->dev == ep->wan_ndev ||
	    info->dev == br) {
		snprintf(why, sizeof(why), "switchdev event %lu attr %d", event, attr);
		__rtl9601d_l2_changed(why);
	}
	return NOTIFY_DONE;
}

static bool v2802_l2_forwarded(struct sk_buff *skb, unsigned int port,
			       unsigned int reason)
{
	unsigned long flags;
	unsigned int i;
	u16 tci;
	bool tagged, forwarded = false;

	if (READ_ONCE(v2802_l2.ep) && port < LAN_PORTS &&
	    v2802_l2_vlan_filtering(v2802_l2.ep) && !READ_ONCE(v2802_l2.saved) &&
	    !__vlan_get_tag(skb, &tci) &&
	    (tci & VLAN_VID_MASK) == SW_LAN_STEER_VID(port)) {
		memmove(skb->data + VLAN_HLEN, skb->data, 2 * ETH_ALEN);
		skb_pull(skb, VLAN_HLEN);
	}
	if (!READ_ONCE(v2802_l2.ep) || !READ_ONCE(v2802_l2.saved) || port > 2)
		return false;
	tagged = !__vlan_get_tag(skb, &tci);
	spin_lock_irqsave(&v2802_l2_lock, flags);
	if (v2802_l2.ep && v2802_l2.saved)
		for (i = 0; i < v2802_l2.count; i++) {
			const struct luna_l2_service *s = &v2802_l2.service[i];

			if (!s->cpu_only && (port == 2 || (s->lan_mask & BIT(port))) &&
			    tagged && (tci & VLAN_VID_MASK) == s->vid) {
				/* Keep CPU copies usable until the old PVID is restored. */
				if (port < 2 && (s->pvid_mask & BIT(port)) &&
				    !v2802_l2_vlan_filtering(v2802_l2.ep)) {
					memmove(skb->data + VLAN_HLEN, skb->data, 2 * ETH_ALEN);
					skb_pull(skb, VLAN_HLEN);
				}
				forwarded = v2802_l2.active && !reason;
				v2802_l2.last_reason[port] = reason;
				if (forwarded)
					v2802_l2.marked[port]++;
				else
					v2802_l2.trapped[port]++;
				break;
			}
		}
	spin_unlock_irqrestore(&v2802_l2_lock, flags);
	return forwarded;
}

static int v2802_l2_ingress_port(struct rtl9602c_eth *ep, const struct sk_buff *skb)
{
	unsigned int p;

	for (p = 0; p < LAN_PORTS; p++)
		if (ep->lan_ndev[p] && skb->skb_iif == ep->lan_ndev[p]->ifindex)
			return p;
	return -1;
}

static bool v2802_l2_wan_allowed(struct rtl9602c_eth *ep, const struct sk_buff *skb)
{
	int port;
	u16 tci;

	if (v2802_l2_vlan_filtering(ep) || !luna_xpon_lan_tagged())
		return true;
	port = v2802_l2_ingress_port(ep, skb);
	return port < 0 || !(luna_xpon_lan_tagged() & BIT(port)) ||
	       !__vlan_get_tag(skb, &tci) || !luna_xpon_native_vlan();
}

/* The bridge keeps the legacy native-service form. Tag only relayed service
 * frames: locally generated, untagged management replies remain untagged. */
static int v2802_l2_lan_prepare(struct rtl9602c_eth *ep, struct sk_buff *skb,
				unsigned int port)
{
	int vid, rc;
	u16 tci;
	bool tagged;

	if (v2802_l2_vlan_filtering(ep) || !luna_xpon_lan_tagged())
		return 0;
	vid = luna_xpon_native_vlan();
	if (vid <= 0)
		return vid;
	tagged = !__vlan_get_tag(skb, &tci);
	if (luna_xpon_lan_tagged() & BIT(port)) {
		if (!tagged && (v2802_l2_ingress_port(ep, skb) >= 0 ||
		    (ep->wan_ndev && skb->skb_iif == ep->wan_ndev->ifindex))) {
			rc = __vlan_insert_tag(skb, htons(ETH_P_8021Q), vid);
			if (!rc)
				skb->protocol = htons(ETH_P_8021Q);
			return rc;
		}
	} else if (tagged && (tci & VLAN_VID_MASK) == vid) {
		return __skb_vlan_pop(skb, &tci);
	}
	return 0;
}

static int v2802_l2_parent_id(struct net_device *dev, struct netdev_phys_item_id *id)
{
	if (!of_machine_is_compatible("vsol,v2802rh"))
		return -EOPNOTSUPP;
	id->id_len = ETH_ALEN;
	if (!g_ep)
		return -ENODEV;
	memcpy(id->id, "V2802R", ETH_ALEN);
	return 0;
}

static ssize_t l2_offload_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned long flags;
	unsigned int i, hardware = 0, cpu = 0;
	ssize_t len;

	spin_lock_irqsave(&v2802_l2_lock, flags);
	if (v2802_l2.saved)
		for (i = 0; i < v2802_l2.count; i++) {
			cpu += v2802_l2.service[i].cpu_only;
			hardware += !v2802_l2.service[i].cpu_only && !v2802_l2.service[i].local;
		}
	len = sysfs_emit(buf, "requested=%u active=%u services=%u error=%d gated=%u generation=%u retries=%u hw_services=%u cpu_bindings=%u\n",
			v2802_l2.requested, v2802_l2.active,
			v2802_l2.saved ? v2802_l2.count : 0, v2802_l2.error,
			v2802_l2.gated, v2802_l2.generation, v2802_l2.retries, hardware, cpu);
	spin_unlock_irqrestore(&v2802_l2_lock, flags);
	return len;
}

static ssize_t l2_offload_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	bool enable;
	int rc = kstrtobool(buf, &enable);

	if (rc)
		return rc;
	WRITE_ONCE(v2802_l2.requested, enable);
	rtl9601d_l2_changed();
	flush_delayed_work(&v2802_l2.work);
	rc = READ_ONCE(v2802_l2.error);
	return rc ? rc : count;
}
static DEVICE_ATTR_RW(l2_offload);

static ssize_t l2_offload_bindings_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	unsigned int i;
	ssize_t len = 0;

	rtnl_lock();
	for (i = 0; i < v2802_l2.count && len < PAGE_SIZE - 192; i++) {
		const struct luna_l2_service *s = &v2802_l2.service[i];
		u32 vlan = 0;
		int rc = v2802_l2_table(1, s->vid, &vlan, 1, false);

		len += sysfs_emit_at(buf, len, "vid=%u sid=%u lan_mask=%#x untagged=%u untag_mask=%#x pvid_mask=%#x local=%u vlan=%#x read_error=%d cpu_only=%u lan_vid=%u translated=%u stacked=%u pcp_mask=%#x pcp_sid=%*phC\n",
				    s->vid, s->flow, s->lan_mask, s->untagged, s->untag_mask,
				    s->pvid_mask, s->local, vlan, rc, s->cpu_only, s->lan_vid,
				    s->translated, s->stacked, s->pcp_mask,
				    (int)sizeof(s->pcp_flow), s->pcp_flow);
	}
	rtnl_unlock();
	return len;
}
static DEVICE_ATTR_RO(l2_offload_bindings);

/* The classifier rules in force, decoded; a field not compared reads "any". */
static ssize_t l2_offload_rules_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	unsigned int i;
	ssize_t len = 0;

	rtnl_lock();
	for_each_set_bit(i, v2802_l2.rules, V2802_L2_RULES) {
		u32 mask[2], rule[2], action[3], care;
		unsigned int idx = V2802_L2_FIRST + i;
		int rc;

		if (len >= PAGE_SIZE - 128)
			break;
		rc = v2802_l2_table(4, idx, mask, 2, false) ?:
		     v2802_l2_table(4, 256 + idx, rule, 2, false) ?:
		     v2802_l2_table(5, idx, action, 3, false);
		if (rc) {
			len += sysfs_emit_at(buf, len, "rule %u read_error=%d\n", idx, rc);
			continue;
		}
		care = mask[0] | rule[0];
		len += sysfs_emit_at(buf, len, "rule %u %s valid=%u uni=%d tag=%d vid=%d pcp=%d sid=%d action=%08x,%08x enable=%#x\n",
				     idx, rule[0] & BIT(31) ? "down" : "up", !!(rule[1] & BIT(16)),
				     care & 7 ? (int)(rule[0] & 7) : -1,
				     care & BIT(3) ? !!(rule[0] & BIT(3)) : -1,
				     care & (VLAN_VID_MASK << 11) ? (int)((rule[0] >> 11) & VLAN_VID_MASK) : -1,
				     care & (7 << 8) ? (int)((rule[0] >> 8) & 7) : -1,
				     !(rule[0] & BIT(31)) && action[0] & BIT(23) ? (int)((action[0] >> 24) & 0x7f) : -1,
				     action[0], action[1],
				     ioread32(v2802_l2.ep->sw + L34_CF_ACTION_CTRL + 4 * idx));
	}
	rtnl_unlock();
	return len;
}
static DEVICE_ATTR_RO(l2_offload_rules);

static ssize_t l2_offload_counters_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	unsigned long flags;
	unsigned int p;
	ssize_t len = 0;

	spin_lock_irqsave(&v2802_l2_lock, flags);
	for (p = 0; p < 3; p++)
		len += sysfs_emit_at(buf, len, "port=%u marked=%llu cpu_only=%llu last_reason=%u\n", p,
				    v2802_l2.marked[p], v2802_l2.trapped[p], v2802_l2.last_reason[p]);
	spin_unlock_irqrestore(&v2802_l2_lock, flags);
	return len;
}
static DEVICE_ATTR_RO(l2_offload_counters);

static struct attribute *v2802_l2_attrs[] = {
	&dev_attr_l2_offload.attr,
	&dev_attr_l2_port_priority.attr,
	&dev_attr_l2_vlan_reserved.attr,
	&dev_attr_l2_offload_bindings.attr,
	&dev_attr_l2_offload_counters.attr,
	&dev_attr_l2_offload_rules.attr,
	&dev_attr_l2_ds_rate_debug.attr,
	&dev_attr_l2_sw_debug.attr,
	NULL,
};
static const struct attribute_group v2802_l2_group = {
	.attrs = v2802_l2_attrs,
};

static void v2802_l2_fini(void *data)
{
	struct rtl9602c_eth *ep = data;
	unsigned long flags;

	sysfs_remove_group(&ep->dev->kobj, &v2802_l2_group);
	spin_lock_irqsave(&v2802_l2_lock, flags);
	v2802_l2.stopping = true;
	v2802_l2.requested = false;
	if (v2802_l2.saved)
		v2802_l2_gate_locked();
	spin_unlock_irqrestore(&v2802_l2_lock, flags);
	unregister_netdevice_notifier(&v2802_l2.netdev_nb);
	unregister_switchdev_blocking_notifier(&v2802_l2.switchdev_blocking_nb);
	unregister_switchdev_notifier(&v2802_l2.switchdev_nb);
	cancel_delayed_work_sync(&v2802_l2.work);
	rtnl_lock();
	v2802_l2_want_vids();
	if (v2802_l2_clear())
		netdev_err(ep->ndev, "[L2-011] L2 offload teardown incomplete; data isolation retained\n");
	v2802_l2_unoffload(ep);
	spin_lock_irqsave(&v2802_l2_lock, flags);
	v2802_l2.ep = NULL;
	spin_unlock_irqrestore(&v2802_l2_lock, flags);
	rtnl_unlock();
}

static int v2802_l2_init(struct rtl9602c_eth *ep)
{
	unsigned long flags;
	int rc;

	if (!of_machine_is_compatible("vsol,v2802rh") || !luna_xpon_active())
		return 0;
	if (!ep->sw || !ep->wan_ndev)
		return -EOPNOTSUPP;
	memset(&v2802_l2, 0, sizeof(v2802_l2));
	v2802_l2.requested = true;
	memset(v2802_l2.pcp, -1, sizeof(v2802_l2.pcp));
	INIT_DELAYED_WORK(&v2802_l2.work, v2802_l2_work);
	spin_lock_irqsave(&v2802_l2_lock, flags);
	v2802_l2.ep = ep;
	spin_unlock_irqrestore(&v2802_l2_lock, flags);
	v2802_l2.netdev_nb.notifier_call = v2802_l2_netdev_event;
	v2802_l2.switchdev_nb.notifier_call = v2802_l2_switchdev_event;
	v2802_l2.switchdev_blocking_nb.notifier_call = v2802_l2_switchdev_event;
	rc = register_switchdev_notifier(&v2802_l2.switchdev_nb);
	if (rc)
		goto fail;
	rc = register_switchdev_blocking_notifier(&v2802_l2.switchdev_blocking_nb);
	if (rc)
		goto atomic;
	rc = register_netdevice_notifier(&v2802_l2.netdev_nb);
	if (rc)
		goto blocking;
	rc = devm_add_action_or_reset(ep->dev, v2802_l2_fini, ep);
	if (rc)
		return rc;
	return sysfs_create_group(&ep->dev->kobj, &v2802_l2_group);
blocking:
	unregister_switchdev_blocking_notifier(&v2802_l2.switchdev_blocking_nb);
atomic:
	unregister_switchdev_notifier(&v2802_l2.switchdev_nb);
fail:
	spin_lock_irqsave(&v2802_l2_lock, flags);
	v2802_l2.stopping = true;
	v2802_l2.ep = NULL;
	spin_unlock_irqrestore(&v2802_l2_lock, flags);
	cancel_delayed_work_sync(&v2802_l2.work);
	return rc;
}
