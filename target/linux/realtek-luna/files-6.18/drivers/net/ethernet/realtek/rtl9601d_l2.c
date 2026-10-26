// SPDX-License-Identifier: GPL-2.0-only
/* V2802RH SFU: switch VLAN forwarding and upstream GEM classification.
 * Included by rtl9602c_eth.c, which owns the switch table engine.
 */
#include <linux/if_bridge.h>
#include <linux/iopoll.h>
#include <linux/rtnetlink.h>
#include <net/switchdev.h>

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
	struct luna_l2_service service[LUNA_L2_SERVICES];
	u32 vlan[LUNA_L2_SERVICES];
	u32 pvid, cfg, flood[3], management_vlan, piso[2];
	u64 marked[3], trapped[3];
	u32 last_reason[3];
	u8 own_mac[2][ETH_ALEN];
	unsigned int own_macs;
	bool management_saved, isolation_saved;
	unsigned int count, generation, retries;
	int error;
	bool requested, active, ports_offloaded, saved, gated, stopping;
	DECLARE_BITMAP(rules, V2802_L2_RULES);
	DECLARE_BITMAP(netdev_vids, VLAN_N_VID);
	DECLARE_BITMAP(dirty_vids, VLAN_N_VID);
	DECLARE_BITMAP(want_vids, VLAN_N_VID);
	DECLARE_BITMAP(wan_vids, VLAN_N_VID);
} v2802_l2;

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
	u32 check[3];
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

/* LAN frames for the ONU itself arrive in a service VLAN, but the switch
 * learns the CPU's own MAC only in the untagged VLAN its frames leave in.
 * Under IVL they would miss, flood to the PON as unknown unicast and be cut
 * to about 1 Mbit/s by the storm meter: give each service VID a static
 * entry to the CPU. Called under RTNL. */
static int v2802_l2_own_macs(const struct luna_l2_service *service, int count, bool add)
{
	struct net_device *br = netdev_master_upper_dev_get(v2802_l2.ep->ndev);
	unsigned int m;
	int i, rc, err = 0;

	if (add) {
		v2802_l2.own_macs = 0;
		if (br)
			ether_addr_copy(v2802_l2.own_mac[v2802_l2.own_macs++], br->dev_addr);
		if (!br || !ether_addr_equal(br->dev_addr, v2802_l2.ep->ndev->dev_addr))
			ether_addr_copy(v2802_l2.own_mac[v2802_l2.own_macs++],
					v2802_l2.ep->ndev->dev_addr);
	}
	for (m = 0; m < v2802_l2.own_macs; m++)
		for (i = 0; i < count; i++) {
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

	if (v2802_l2.gated)
		return 0;
	for (i = 0; i < ARRAY_SIZE(w); i++) {
		if (!v2802_l2.isolation_saved)
			v2802_l2.piso[i] = ioread32(ep->sw + SW_PISO_PORT + 4 * i);
		w[i] = v2802_l2.piso[i];
	}
	v2802_l2.isolation_saved = true;
	for (p = 0; p < 3; p++) {
		unsigned int shift = (p % 2) * V2802_PISO_BITS;
		u32 mask = (p == 2 ? 3 : BIT(2)) << shift;

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

static bool v2802_l2_managed_vid(u16 vid)
{
	return vid && vid != SW_DEFAULT_VID && vid != pon_vid;
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
	iowrite32(v2802_l2.cfg, ep->sw + L34_CF_CFG);
	iowrite32(v2802_l2.flood[0], ep->sw + ep->swm->bc_flood);
	iowrite32(v2802_l2.flood[1], ep->sw + ep->swm->unkn_mc_flood);
	iowrite32(v2802_l2.flood[2], ep->sw + ep->swm->unkn_uc_flood);
	rc = v2802_l2_flush_lan(ep);
	if (rc)
		return rc;
	spin_lock_irqsave(&v2802_l2_lock, flags);
	rc = v2802_l2_ungate_locked();
	if (!rc) {
		v2802_l2.saved = false;
		v2802_l2.management_saved = false;
		v2802_l2.count = 0;
	}
	spin_unlock_irqrestore(&v2802_l2_lock, flags);
	return rc;
}

void rtl9601d_l2_changed(void)
{
	unsigned long flags;
	int rc = 0;

	spin_lock_irqsave(&v2802_l2_lock, flags);
	if (v2802_l2.ep && !v2802_l2.stopping) {
		v2802_l2.generation++;
		if (v2802_l2.saved)
			rc = v2802_l2_gate_locked();
		if (rc)
			v2802_l2.error = rc;
		mod_delayed_work(system_wq, &v2802_l2.work, 0);
	}
	spin_unlock_irqrestore(&v2802_l2_lock, flags);
}
EXPORT_SYMBOL(rtl9601d_l2_changed);

/* Pattern 0 TCAM stores positive and negative care bits separately.
 * Rule bit 48 is validity; direction 31, C-tag 3, VID 11..22, UNI 0..2.
 */
static int v2802_l2_rule(unsigned int idx, const struct luna_l2_service *s,
			  unsigned int port)
{
	u32 care[2] = { BIT(31) | BIT(3) | 7, 0 };
	u32 data[2] = { port, 0 }, rule[2], mask[2];
	u32 action[3] = { ((u32)s->flow << 24) | BIT(23), 0, 0 };
	int rc;

	if (!s->untagged) {
		care[0] |= VLAN_VID_MASK << 11;
		data[0] |= ((u32)s->vid << 11) | BIT(3);
	}
	rule[0] = data[0] & care[0];
	rule[1] = BIT(16);
	mask[0] = ~data[0] & care[0];
	mask[1] = 0;
	rc = v2802_l2_table(4, idx, mask, 2, true);
	if (!rc)
		rc = v2802_l2_table(5, idx, action, 3, true);
	if (rc)
		return rc;
	/* Only the SID action is enabled; switch VLAN rules own tag insertion. */
	iowrite32(L34_CF_SID_ENABLE, v2802_l2.ep->sw + L34_CF_ACTION_CTRL + 4 * idx);
	__set_bit(idx - V2802_L2_FIRST, v2802_l2.rules);
	return v2802_l2_table(4, 256 + idx, rule, 2, true);
}

static int v2802_l2_install(const struct luna_l2_service *service, int count,
			     unsigned int generation)
{
	struct rtl9602c_eth *ep = v2802_l2.ep;
	unsigned long flags;
	u32 cfg, w[2], pvid, members, untag;
	unsigned int i, p;
	bool management_service = false;
	int rc = 0;

	if (count <= 0 || count > LUNA_L2_SERVICES)
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
		management_service |= service[i].vid == SW_DEFAULT_VID;
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
	/* PON classifier selected; unmatched upstream is permitted without PON. */
	iowrite32((cfg & ~3u) | BIT(3) | 1, ep->sw + L34_CF_CFG);
	for (i = 0; i < count; i++) {
		const struct luna_l2_service *s = &service[i];

		members = s->lan_mask | BIT(2) | BIT(3);
		/* Keep the service VID on CPU copies for accurate forwarding marks. */
		untag = s->untagged ? s->lan_mask : 0;
		/* IVL, as for the WAN VIDs: a shared FID drops unicast replies. */
		w[0] = BIT(11) | SW_VLAN_IVL | (untag << 4) | members;
		rc = v2802_l2_table(1, s->vid, w, 1, true);
		if (rc)
			goto rollback;
		for (p = 0; p < 2; p++) {
			if (!(s->lan_mask & BIT(p)))
				continue;
			if (s->untagged)
				pvid = sw_packed_ins(pvid, p, SW_VLAN_PB_VID_BITS, s->vid);
			rc = v2802_l2_rule(V2802_L2_FIRST + 4 * i + 2 * p, s, p);
			if (rc)
				goto rollback;
			if (s->untagged) {
				struct luna_l2_service tagged = *s;

				tagged.untagged = false;
				rc = v2802_l2_rule(V2802_L2_FIRST + 4 * i + 2 * p + 1,
						     &tagged, p);
				if (rc)
					goto rollback;
			}
		}
	}
	rc = v2802_l2_own_macs(service, count, true);
	if (rc)
		goto rollback;
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
		netdev_err(ep->ndev, "L2 offload rollback incomplete\n");
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

static bool v2802_l2_bridge_ready(struct rtl9602c_eth *ep)
{
	struct net_device *br = netdev_master_upper_dev_get(ep->ndev);
	unsigned long required = BR_LEARNING | BR_FLOOD | BR_MCAST_FLOOD | BR_BCAST_FLOOD;
	struct net_device *lower;
	struct list_head *iter;

	if (!br)
		return false;
	netdev_for_each_lower_dev(br, lower, iter)
		if (lower != ep->ndev && lower != ep->wan_ndev)
			return false;

	return br && netif_is_bridge_master(br) && ep->wan_ndev &&
	       netdev_master_upper_dev_get(ep->wan_ndev) == br &&
	       netif_running(ep->ndev) && netif_running(ep->wan_ndev) &&
	       ep->hw_up && !ep->closing && !ep->in_recovery &&
	       !br_vlan_enabled(br) && !br_multicast_enabled(br) &&
	       br_port_get_stp_state(ep->ndev) == BR_STATE_FORWARDING &&
	       br_port_get_stp_state(ep->wan_ndev) == BR_STATE_FORWARDING &&
	       v2802_l2_flags(ep->ndev, required) &&
	       v2802_l2_flags(ep->wan_ndev, required) &&
	       !br_port_flag_is_set(ep->ndev, BR_ISOLATED | BR_PORT_LOCKED | BR_HAIRPIN_MODE) &&
	       !br_port_flag_is_set(ep->wan_ndev, BR_ISOLATED | BR_PORT_LOCKED | BR_HAIRPIN_MODE);
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

static void v2802_l2_work(struct work_struct *work)
{
	struct luna_l2_service services[LUNA_L2_SERVICES];
	struct rtl9602c_eth *ep = v2802_l2.ep;
	unsigned long flags;
	unsigned int generation;
	int count, rc;
	bool retry = false;

	rtnl_lock();
	if (READ_ONCE(v2802_l2.stopping) || READ_ONCE(ep->in_recovery) || !ep->hw_up)
		goto unlock;
	generation = READ_ONCE(v2802_l2.generation);
	v2802_l2_want_vids();
	rc = v2802_l2_clear();
	if (rc)
		goto out;
	if (v2802_l2.ports_offloaded) {
		switchdev_bridge_port_unoffload(ep->ndev, ep, &v2802_l2.switchdev_nb, NULL);
		switchdev_bridge_port_unoffload(ep->wan_ndev, ep, &v2802_l2.switchdev_nb, NULL);
		v2802_l2.ports_offloaded = false;
	}
	rc = v2802_l2_sync_vids(ep);
	retry = !!rc;
	if (rc || !READ_ONCE(v2802_l2.requested))
		goto out;
	if (!v2802_l2_bridge_ready(ep)) {
		rc = -EOPNOTSUPP;
		goto out;
	}
	count = luna_xpon_l2_services(services);
	if (count <= 0) {
		rc = count;
		goto out;
	}
	rc = switchdev_bridge_port_offload(ep->ndev, ep->ndev, ep,
					 &v2802_l2.switchdev_nb, NULL, false, NULL);
	if (rc)
		goto out;
	rc = switchdev_bridge_port_offload(ep->wan_ndev, ep->wan_ndev, ep,
					 &v2802_l2.switchdev_nb, NULL, false, NULL);
	if (rc) {
		switchdev_bridge_port_unoffload(ep->ndev, ep, &v2802_l2.switchdev_nb, NULL);
		goto out;
	}
	v2802_l2.ports_offloaded = true;
	rc = v2802_l2_install(services, count, generation);
	if (rc && !v2802_l2.saved) {
		switchdev_bridge_port_unoffload(ep->ndev, ep, &v2802_l2.switchdev_nb, NULL);
		switchdev_bridge_port_unoffload(ep->wan_ndev, ep, &v2802_l2.switchdev_nb, NULL);
		v2802_l2.ports_offloaded = false;
	}
	if (!rc)
		netdev_info(ep->ndev, "V2802RH L2 offload: %d PON/LAN services\n", count);
out:
	WRITE_ONCE(v2802_l2.error, rc);
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
	br = netdev_master_upper_dev_get_rcu(ep->ndev);
	changed = dev == ep->ndev || dev == ep->wan_ndev || dev == br;
	if (event == NETDEV_CHANGEUPPER) {
		struct netdev_notifier_changeupper_info *info = ptr;

		changed |= br && info->upper_dev == br;
	}
	rcu_read_unlock();
	if (changed)
		rtl9601d_l2_changed();
	return NOTIFY_DONE;
}

static int v2802_l2_switchdev_event(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct switchdev_notifier_info *info = ptr;
	struct rtl9602c_eth *ep = v2802_l2.ep;
	struct net_device *br;

	if (event == SWITCHDEV_FDB_ADD_TO_DEVICE || event == SWITCHDEV_FDB_DEL_TO_DEVICE) {
		struct switchdev_notifier_fdb_info *fdb = ptr;

		if (info->ctx == ep) {
			if (event == SWITCHDEV_FDB_ADD_TO_DEVICE && fdb->added_by_user && !fdb->is_local)
				return notifier_from_errno(-EBUSY);
			return NOTIFY_DONE;
		}
		if (!fdb->added_by_user || fdb->is_local)
			return NOTIFY_DONE;
	} else if (event != SWITCHDEV_PORT_ATTR_SET) {
		return NOTIFY_DONE;
	}
	rcu_read_lock();
	br = netdev_master_upper_dev_get_rcu(ep->ndev);
	rcu_read_unlock();
	if (info->dev == ep->ndev || info->dev == ep->wan_ndev || info->dev == br)
		rtl9601d_l2_changed();
	return NOTIFY_DONE;
}

static bool v2802_l2_forwarded(struct sk_buff *skb, unsigned int port,
			       unsigned int reason)
{
	unsigned long flags;
	unsigned int i;
	u16 tci;
	bool tagged, forwarded = false;

	if (!READ_ONCE(v2802_l2.ep) || !READ_ONCE(v2802_l2.saved) || port > 2)
		return false;
	tagged = !__vlan_get_tag(skb, &tci);
	spin_lock_irqsave(&v2802_l2_lock, flags);
	if (v2802_l2.ep && v2802_l2.saved)
		for (i = 0; i < v2802_l2.count; i++) {
			const struct luna_l2_service *s = &v2802_l2.service[i];

			if ((port == 2 || (s->lan_mask & BIT(port))) &&
			    tagged && (tci & VLAN_VID_MASK) == s->vid) {
				/* Keep CPU copies usable until the old PVID is restored. */
				if (port < 2 && s->untagged) {
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
	ssize_t len;

	spin_lock_irqsave(&v2802_l2_lock, flags);
	len = sysfs_emit(buf, "requested=%u active=%u services=%u error=%d gated=%u generation=%u retries=%u\n",
			v2802_l2.requested, v2802_l2.active,
			v2802_l2.saved ? v2802_l2.count : 0, v2802_l2.error,
			v2802_l2.gated, v2802_l2.generation, v2802_l2.retries);
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
	for (i = 0; i < v2802_l2.count; i++) {
		const struct luna_l2_service *s = &v2802_l2.service[i];
		u32 vlan = 0;
		int rc = v2802_l2_table(1, s->vid, &vlan, 1, false);

		len += sysfs_emit_at(buf, len, "vid=%u sid=%u lan_mask=%#x untagged=%u vlan=%#x read_error=%d\n",
				    s->vid, s->flow, s->lan_mask, s->untagged, vlan, rc);
	}
	rtnl_unlock();
	return len;
}
static DEVICE_ATTR_RO(l2_offload_bindings);

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
	&dev_attr_l2_offload_bindings.attr,
	&dev_attr_l2_offload_counters.attr,
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
		netdev_err(ep->ndev, "L2 offload teardown incomplete; data isolation retained\n");
	if (v2802_l2.ports_offloaded) {
		switchdev_bridge_port_unoffload(ep->ndev, ep, &v2802_l2.switchdev_nb, NULL);
		switchdev_bridge_port_unoffload(ep->wan_ndev, ep, &v2802_l2.switchdev_nb, NULL);
	}
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
	if (hw_nat || port_vlans || !ep->sw || !ep->wan_ndev)
		return -EOPNOTSUPP;
	memset(&v2802_l2, 0, sizeof(v2802_l2));
	v2802_l2.requested = true;
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
