// SPDX-License-Identifier: GPL-2.0-only
/* Local bridge VLAN policy; included by rtl9601d_l2.c. */
static bool v2802_l2_vlan_filtering(struct rtl9602c_eth *ep)
{
	struct net_device *br;
	bool enabled;

	if (ep != READ_ONCE(v2802_l2.ep))
		return false;
	rcu_read_lock();
	br = ep->wan_ndev ? netdev_master_upper_dev_get_rcu(ep->wan_ndev) : NULL;
	enabled = br && netif_is_bridge_master(br) && br_vlan_enabled(br);
	rcu_read_unlock();
	return enabled;
}

static bool v2802_l2_vlan_info(const struct net_device *dev, u16 vid,
			      struct bridge_vlan_info *info)
{
	return dev && !br_vlan_get_info(dev, vid, info);
}

static bool v2802_l2_vlan_reserved(u16 vid)
{
	return vid == pon_vid ||
	       (vid >= SW_LAN_STEER_VID_BASE && vid < SW_LAN_STEER_VID_BASE + LAN_PORTS);
}

/* The bridge owns admission and egress format; OMCI still owns the GEM. */
static int v2802_l2_vlan_plan(struct rtl9602c_eth *ep,
			    struct luna_l2_service *services, int count)
{
	struct net_device *br = v2802_l2_bridge(ep);
	struct bridge_vlan_info info;
	unsigned int vid, p, i;
	u16 proto;

	if (!v2802_l2_vlan_filtering(ep))
		return count;
	if (br_vlan_get_proto(br, &proto) || proto != ETH_P_8021Q)
		return -EOPNOTSUPP;
	for (i = 0; i < count; i++) {
		services[i].untag_mask = 0;
		services[i].pvid_mask = 0;
		services[i].local = !v2802_l2_vlan_info(ep->wan_ndev, services[i].vid, &info);
	}
	for (vid = 1; vid < VLAN_VID_MASK; vid++) {
		u8 members = 0, untag = 0, pvid = 0;
		bool wan = v2802_l2_vlan_info(ep->wan_ndev, vid, &info);
		bool self = v2802_l2_vlan_info(br, vid, &info);

		/* A PON service must retain its network tag at the CPU boundary. */
		if (wan && !br_vlan_get_info(ep->wan_ndev, vid, &info) &&
		    (info.flags & (BRIDGE_VLAN_INFO_UNTAGGED | BRIDGE_VLAN_INFO_PVID)))
			return -EOPNOTSUPP;
		for (p = 0; p < LAN_PORTS; p++) {
			if (!v2802_l2_vlan_info(ep->lan_ndev[p], vid, &info))
				continue;
			members |= BIT(p);
			if (info.flags & BRIDGE_VLAN_INFO_UNTAGGED)
				untag |= BIT(p);
			if (info.flags & BRIDGE_VLAN_INFO_PVID)
				pvid |= BIT(p);
		}
		for (i = 0; i < count && services[i].vid != vid; i++)
			;
		if (!members && !wan && !self && i == count)
			continue;
		if (v2802_l2_vlan_reserved(vid))
			return -EBUSY;
		if (i == count) {
			if (count == V2802_L2_VLANS)
				return -ENOSPC;
			services[count++] = (struct luna_l2_service) {
				.vid = vid, .local = true,
			};
		} else {
			/* Local policy may narrow an OLT UNI binding, never widen it. */
			members &= services[i].lan_mask;
		}
		services[i].lan_mask = members;
		services[i].untag_mask = untag & members;
		services[i].pvid_mask = pvid & members;
	}
	return count;
}

static void v2802_l2_vlan_ingress(struct rtl9602c_eth *ep, u32 *pvid, u32 *accept)
{
	unsigned int p;

	for (p = 0; p < LAN_PORTS; p++) {
		u16 vid = 0;

		if (ep->lan_ndev[p])
			br_vlan_get_pvid(ep->lan_ndev[p], &vid);
		*pvid = sw_packed_ins(*pvid, p, SW_VLAN_PB_VID_BITS,
				     vid ?: SW_LAN_STEER_VID(p));
		/* VLAN_PORT_ACCEPT_FRAME_TYPE: 0 all, 1 tagged only. */
		*accept = sw_packed_ins(*accept, p, 2, vid ? 0 : 1);
	}
}

/* With hardware relay gated, retain VLAN tags for the Linux bridge. A
 * per-jack steering PVID distinguishes originally untagged CPU copies. */
static int v2802_l2_vlan_fallback(struct rtl9602c_eth *ep)
{
	struct bridge_vlan_info info;
	struct net_device *br = v2802_l2_bridge(ep);
	bool filtering = v2802_l2_vlan_filtering(ep);
	unsigned int vid, p;
	u32 pvid, accept, vlan;
	int rc;

	if (!filtering && bitmap_empty(v2802_l2.bridge_vids, VLAN_N_VID))
		return 0;
	for (vid = 1; vid < VLAN_VID_MASK; vid++) {
		u32 members = 0;
		bool self = filtering && v2802_l2_vlan_info(br, vid, &info);

		if (filtering) {
			for (p = 0; p < LAN_PORTS; p++)
				if (v2802_l2_vlan_info(ep->lan_ndev[p], vid, &info))
					members |= BIT(p);
			if (v2802_l2_vlan_info(ep->wan_ndev, vid, &info))
				members |= BIT(2);
		}
		if (v2802_l2_vlan_reserved(vid)) {
			if (members || self)
				return -EBUSY;
			continue;
		}
		if (!members && !self && !test_bit(vid, v2802_l2.bridge_vids))
			continue;
		vlan = members || self ? BIT(11) | SW_VLAN_IVL | members | BIT(3) :
			v2802_l2_cpu_vlan(test_bit(vid, v2802_l2.want_vids));
		if (!filtering && vid == SW_DEFAULT_VID)
			vlan = BIT(11) | 0xff;
		rc = v2802_l2_table(1, vid, &vlan, 1, true);
		if (rc)
			return rc;
		__assign_bit(vid, v2802_l2.bridge_vids, members || self);
	}
	pvid = ioread32(ep->sw + SW_VLAN_PB_VID);
	accept = ioread32(ep->sw + SW_VLAN_PORT_ACCEPT_FRAME_TYPE);
	for (p = 0; p < LAN_PORTS; p++) {
		pvid = sw_packed_ins(pvid, p, SW_VLAN_PB_VID_BITS,
			filtering ? SW_LAN_STEER_VID(p) : SW_DEFAULT_VID);
		accept = sw_packed_ins(accept, p, 2, 0);
	}
	iowrite32(pvid, ep->sw + SW_VLAN_PB_VID);
	iowrite32(accept, ep->sw + SW_VLAN_PORT_ACCEPT_FRAME_TYPE);
	return 0;
}

static int v2802_l2_priority(struct rtl9602c_eth *ep, struct sk_buff *skb)
{
	unsigned int p;
	u16 tci;
	int pcp;

	for (p = 0; p < LAN_PORTS; p++) {
		if (!ep->lan_ndev[p] || skb->skb_iif != ep->lan_ndev[p]->ifindex)
			continue;
		pcp = READ_ONCE(v2802_l2.pcp[p]);
		if (pcp < 0 || __vlan_get_tag(skb, &tci))
			return 0;
		if (skb_ensure_writable(skb, VLAN_ETH_HLEN))
			return -ENOMEM;
		put_unaligned_be16((tci & ~VLAN_PRIO_MASK) | (pcp << VLAN_PRIO_SHIFT),
				   skb->data + ETH_HLEN);
		break;
	}
	return 0;
}

static ssize_t l2_port_priority_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d %d\n", READ_ONCE(v2802_l2.pcp[0]),
			  READ_ONCE(v2802_l2.pcp[1]));
}

static ssize_t l2_port_priority_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	int p0, p1;
	char extra;

	if (sscanf(buf, "%d %d %c", &p0, &p1, &extra) != 2 ||
	    p0 < -1 || p0 > 7 || p1 < -1 || p1 > 7)
		return -EINVAL;
	rtnl_lock();
	if (p0 != v2802_l2.pcp[0] || p1 != v2802_l2.pcp[1]) {
		WRITE_ONCE(v2802_l2.pcp[0], p0);
		WRITE_ONCE(v2802_l2.pcp[1], p1);
		rtl9601d_l2_changed();
	}
	rtnl_unlock();
	return count;
}
static DEVICE_ATTR_RW(l2_port_priority);

static ssize_t l2_vlan_reserved_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u %u %u\n", pon_vid,
			  SW_LAN_STEER_VID(0), SW_LAN_STEER_VID(1));
}
static DEVICE_ATTR_RO(l2_vlan_reserved);
