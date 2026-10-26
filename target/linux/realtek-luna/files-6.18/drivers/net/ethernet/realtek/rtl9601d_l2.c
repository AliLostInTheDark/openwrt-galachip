// SPDX-License-Identifier: GPL-2.0-only
/* V2802RH SFU: switch VLAN forwarding and upstream GEM classification.
 * Included by rtl9602c_eth.c, which owns the switch table engine.
 */
#include <linux/if_bridge.h>
#include <linux/rtnetlink.h>
#include <net/switchdev.h>

#define V2802_L2_FIRST 32
#define V2802_L2_RULES (4 * LUNA_L2_SERVICES)

static struct {
	struct rtl9602c_eth *ep;
	struct work_struct work;
	struct notifier_block netdev_nb, switchdev_nb;
	struct luna_l2_service service[LUNA_L2_SERVICES];
	u32 vlan[LUNA_L2_SERVICES];
	u32 pvid, cfg, flood[3], management_vlan;
	bool management_saved;
	unsigned int count, generation;
	int error;
	bool requested, active, ports_offloaded, saved;
	DECLARE_BITMAP(want_vids, VLAN_N_VID);
	DECLARE_BITMAP(wan_vids, VLAN_N_VID);
} v2802_l2;

static int rtl9602c_wan_vid(struct rtl9602c_eth *ep, u16 vid, bool add);

/* Caller owns sw_table_lock, shared with the NIC's VLAN callbacks. */
static int v2802_l2_table(u8 type, u16 addr, u32 *words, unsigned int n,
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

static int v2802_l2_clear_locked(void)
{
	struct rtl9602c_eth *ep = v2802_l2.ep;
	u32 zero[2] = { 0 };
	unsigned int i;
	int rc, error = 0;

	v2802_l2.active = false;
	if (!v2802_l2.saved)
		return 0;
	for (i = 0; i < V2802_L2_RULES; i++) {
		rc = v2802_l2_table(4, 256 + V2802_L2_FIRST + i, zero, 2, true);
		if (rc)
			error = rc;
	}
	for (i = 0; i < v2802_l2.count; i++) {
		rc = v2802_l2_table(1, v2802_l2.service[i].vid,
				     &v2802_l2.vlan[i], 1, true);
		if (rc)
			error = rc;
	}
	if (v2802_l2.management_saved) {
		rc = v2802_l2_table(1, SW_DEFAULT_VID,
				     &v2802_l2.management_vlan, 1, true);
		if (rc)
			error = rc;
	}
	iowrite32(v2802_l2.pvid, ep->sw + SW_VLAN_PB_VID);
	iowrite32(v2802_l2.cfg, ep->sw + L34_CF_CFG);
	iowrite32(v2802_l2.flood[0], ep->sw + ep->swm->bc_flood);
	iowrite32(v2802_l2.flood[1], ep->sw + ep->swm->unkn_mc_flood);
	iowrite32(v2802_l2.flood[2], ep->sw + ep->swm->unkn_uc_flood);
	if (!error) {
		v2802_l2.saved = false;
		v2802_l2.management_saved = false;
		v2802_l2.count = 0;
	}
	return error;
}

void rtl9601d_l2_changed(void)
{
	struct rtl9602c_eth *ep = READ_ONCE(v2802_l2.ep);
	unsigned long flags;
	int rc;

	if (!ep)
		return;
	spin_lock_irqsave(&ep->sw_table_lock, flags);
	v2802_l2.generation++;
	rc = v2802_l2_clear_locked();
	if (rc)
		v2802_l2.error = rc;
	spin_unlock_irqrestore(&ep->sw_table_lock, flags);
	schedule_work(&v2802_l2.work);
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

	spin_lock_irqsave(&ep->sw_table_lock, flags);
	if (generation != v2802_l2.generation) {
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
	v2802_l2.management_saved = !management_service;
	memcpy(v2802_l2.service, service, count * sizeof(*service));
	v2802_l2.count = count;
	v2802_l2.pvid = pvid = ioread32(ep->sw + SW_VLAN_PB_VID);
	v2802_l2.cfg = cfg;
	v2802_l2.flood[0] = ioread32(ep->sw + ep->swm->bc_flood);
	v2802_l2.flood[1] = ioread32(ep->sw + ep->swm->unkn_mc_flood);
	v2802_l2.flood[2] = ioread32(ep->sw + ep->swm->unkn_uc_flood);
	v2802_l2.saved = true;
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
	iowrite32(pvid, ep->sw + SW_VLAN_PB_VID);
	iowrite32(v2802_l2.flood[0] | BIT(2), ep->sw + ep->swm->bc_flood);
	iowrite32(v2802_l2.flood[1] | BIT(2), ep->sw + ep->swm->unkn_mc_flood);
	iowrite32(v2802_l2.flood[2] | BIT(2), ep->sw + ep->swm->unkn_uc_flood);
	v2802_l2.active = true;
	goto out;
rollback:
	if (v2802_l2_clear_locked())
		netdev_err(ep->ndev, "L2 offload rollback incomplete\n");
out:
	spin_unlock_irqrestore(&ep->sw_table_lock, flags);
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
static void v2802_l2_sync_vids(struct rtl9602c_eth *ep)
{
	unsigned long *want = v2802_l2.want_vids, *have = v2802_l2.wan_vids;
	unsigned int vid;

	bitmap_zero(want, VLAN_N_VID);
	luna_xpon_service_vids(want);
	for_each_set_bit(vid, want, VLAN_N_VID)
		if (!test_bit(vid, have) && !rtl9602c_wan_vid(ep, vid, true))
			__set_bit(vid, have);
	for_each_set_bit(vid, have, VLAN_N_VID)
		if (!test_bit(vid, want) && !rtl9602c_wan_vid(ep, vid, false))
			__clear_bit(vid, have);
}

static void v2802_l2_work(struct work_struct *work)
{
	struct luna_l2_service services[LUNA_L2_SERVICES];
	struct rtl9602c_eth *ep = v2802_l2.ep;
	int count, rc = 0;
	unsigned int generation = READ_ONCE(v2802_l2.generation);

	rtnl_lock();
	if (v2802_l2.ports_offloaded) {
		switchdev_bridge_port_unoffload(ep->ndev, ep, NULL, NULL);
		switchdev_bridge_port_unoffload(ep->wan_ndev, ep, NULL, NULL);
		v2802_l2.ports_offloaded = false;
	}
	if (v2802_l2.saved) {
		rc = -EIO;
		goto out;
	}
	v2802_l2_sync_vids(ep);
	if (!v2802_l2.requested)
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
	rc = switchdev_bridge_port_offload(ep->ndev, ep->ndev, ep, NULL, NULL, false, NULL);
	if (rc)
		goto out;
	rc = switchdev_bridge_port_offload(ep->wan_ndev, ep->wan_ndev, ep, NULL, NULL, false, NULL);
	if (rc) {
		switchdev_bridge_port_unoffload(ep->ndev, ep, NULL, NULL);
		goto out;
	}
	v2802_l2.ports_offloaded = true;
	rc = v2802_l2_install(services, count, generation);
	if (!rc)
		netdev_info(ep->ndev, "V2802RH L2 offload: %d PON/LAN services\n", count);
out:
	v2802_l2.error = rc;
	rtnl_unlock();
}

static int v2802_l2_netdev_event(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct rtl9602c_eth *ep = v2802_l2.ep;

	if (dev == ep->ndev || dev == ep->wan_ndev)
		rtl9601d_l2_changed();
	return NOTIFY_DONE;
}

static int v2802_l2_switchdev_event(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct switchdev_notifier_info *info = ptr;
	struct rtl9602c_eth *ep = v2802_l2.ep;
	struct net_device *br;

	rcu_read_lock();
	br = netdev_master_upper_dev_get_rcu(ep->ndev);
	rcu_read_unlock();
	if (event == SWITCHDEV_PORT_ATTR_SET &&
	    (info->dev == ep->ndev || info->dev == ep->wan_ndev || info->dev == br))
		rtl9601d_l2_changed();
	return NOTIFY_DONE;
}

static bool v2802_l2_forwarded(struct sk_buff *skb, unsigned int port)
{
	struct rtl9602c_eth *ep = READ_ONCE(v2802_l2.ep);
	unsigned long flags;
	unsigned int i;
	u16 tci;
	bool tagged, forwarded = false;

	if (!ep || port > 2)
		return false;
	tagged = !__vlan_get_tag(skb, &tci);
	spin_lock_irqsave(&ep->sw_table_lock, flags);
	if (v2802_l2.active)
		for (i = 0; i < v2802_l2.count; i++) {
			const struct luna_l2_service *s = &v2802_l2.service[i];

			if ((port == 2 || (s->lan_mask & BIT(port))) &&
			    tagged && (tci & VLAN_VID_MASK) == s->vid) {
				/* LAN copies need the same untagging as downstream pon0 copies. */
				if (port < 2 && s->untagged) {
					memmove(skb->data + VLAN_HLEN, skb->data, 2 * ETH_ALEN);
					skb_pull(skb, VLAN_HLEN);
				}
				forwarded = true;
				break;
			}
		}
	spin_unlock_irqrestore(&ep->sw_table_lock, flags);
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
	return sysfs_emit(buf, "requested=%u active=%u services=%u error=%d\n",
			  READ_ONCE(v2802_l2.requested), READ_ONCE(v2802_l2.active),
			  v2802_l2.active ? v2802_l2.count : 0, v2802_l2.error);
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
	return count;
}
static DEVICE_ATTR_RW(l2_offload);

static void v2802_l2_fini(void *data)
{
	struct rtl9602c_eth *ep = data;

	device_remove_file(ep->dev, &dev_attr_l2_offload);
	v2802_l2.requested = false;
	unregister_netdevice_notifier(&v2802_l2.netdev_nb);
	unregister_switchdev_notifier(&v2802_l2.switchdev_nb);
	rtl9601d_l2_changed();
	cancel_work_sync(&v2802_l2.work);
	rtnl_lock();
	if (v2802_l2.ports_offloaded) {
		switchdev_bridge_port_unoffload(ep->ndev, ep, NULL, NULL);
		switchdev_bridge_port_unoffload(ep->wan_ndev, ep, NULL, NULL);
	}
	WRITE_ONCE(v2802_l2.ep, NULL);
	rtnl_unlock();
}

static int v2802_l2_init(struct rtl9602c_eth *ep)
{
	int rc;

	if (!of_machine_is_compatible("vsol,v2802rh") || !luna_xpon_active())
		return 0;
	if (hw_nat || port_vlans || !ep->sw || !ep->wan_ndev)
		return -EOPNOTSUPP;
	INIT_WORK(&v2802_l2.work, v2802_l2_work);
	v2802_l2.ep = ep;
	v2802_l2.netdev_nb.notifier_call = v2802_l2_netdev_event;
	v2802_l2.switchdev_nb.notifier_call = v2802_l2_switchdev_event;
	rc = register_switchdev_notifier(&v2802_l2.switchdev_nb);
	if (rc)
		goto fail;
	rc = register_netdevice_notifier(&v2802_l2.netdev_nb);
	if (rc) {
		unregister_switchdev_notifier(&v2802_l2.switchdev_nb);
		goto fail;
	}
	rc = devm_add_action_or_reset(ep->dev, v2802_l2_fini, ep);
	if (rc)
		return rc;
	return device_create_file(ep->dev, &dev_attr_l2_offload);
fail:
	v2802_l2.ep = NULL;
	return rc;
}
