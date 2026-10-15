// SPDX-License-Identifier: GPL-2.0-only
/*
 * GalaChip GC1601 provider for the xPON subsystem and its OMCI agent.
 *
 * OMCI frames ride the packet engine on the OMCC GEM port. The agent builds
 * PDUs without the trailer (OMCI_CAP_HW_MIC); the GC1601 has no hardware for
 * it, so the CRC-32 the vendor daemon computed in software is added here.
 *
 * The activation machine reports into the OMCI core while holding pon->lock,
 * so none of these operations take that lock.
 */

#include <linux/crc32.h>
#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/if_vlan.h>
#include <linux/netdevice.h>
#include <linux/of_net.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/soc/galachip/gc1601.h>
#include <linux/unaligned.h>
#include <net/xpon.h>
#include <net/xpon/omci.h>
#include <linux/optical_frontend.h>

#include "gc1601-pon.h"

static bool sfu_offload = true;
module_param(sfu_offload, bool, 0644);
MODULE_PARM_DESC(sfu_offload, "Forward the unicast services in the switch instead of pon0");

/* EPON frames carry the LLID index where G-PON has the GEM port (minfo 0). */
static bool epon_offload = true;
module_param(epon_offload, bool, 0644);
MODULE_PARM_DESC(epon_offload, "Forward EPON LLID 0 in the switch, keyed on minfo 0");

/*
 * The service's VLAN on the LAN side: the untagged VLAN and the manual
 * map, written by omci-agent from UCI, win over the class 171 rule.
 */
static void gc1601_xpon_lan_mode(struct gc1601_xpon *xp,
				 const struct gc1601_service *svc,
				 struct gc1601_sfu_flow *flow)
{
	unsigned int i;

	flow->lan = svc->uni_mode;
	flow->lan_vid = svc->uni_vid;
	for (i = 0; i < xp->vlan_maps; i++) {
		if (xp->vlan_map[i].wan == svc->vlan_id) {
			flow->lan = xp->vlan_map[i].mode;
			flow->lan_vid = xp->vlan_map[i].lan;
		}
	}
	if (xp->sfu_untag_vlan && svc->vlan_id == xp->sfu_untag_vlan) {
		flow->lan = GC1601_SFU_LAN_UNTAGGED;
		flow->lan_vid = 0;
	}
}

/*
 * An 802.1p profile gives one VLAN several GEM ports, one per priority
 * (Jio: P-bit 0 internet, 4 VoIP, 5 TR-069, 6 IPTV, 7 control, each on its
 * own T-CONT). Lower is the better carrier for unmarked traffic: the
 * default service, then one for any priority, then P-bit 0 upwards.
 */
static unsigned int gc1601_xpon_service_rank(const struct gc1601_service *svc)
{
	if (svc->default_service)
		return 0;
	if (!svc->pcp_valid)
		return 1;
	return 2 + svc->pcp;
}

/*
 * Flows for the switch, as the stock SDK classifies GEM ports (record type
 * 3, VLAN and P-bit, in sdk_parse_gemvlan_from_ponmap): every service gets
 * its downstream flow, keyed by its GEM port. Upstream, a VLAN whose
 * services split by P-bit gets one VLAN + P-bit flow per service; a VLAN
 * with a single service, or one the LAN side sends untagged, gets a single
 * flow for its best-effort service. Services left without an upstream
 * flow (default or any-priority services next to P-bit ones) go up through
 * pon0, which picks the GEM per frame.
 */
static unsigned int gc1601_xpon_sfu_collect(struct gc1601_pon *pon, bool untagged,
					    struct gc1601_sfu_flow *flows,
					    unsigned int max)
{
	struct gc1601_xpon *xp = &pon->xp;
	struct gc1601_service *best;
	unsigned int i, j, n = 0, split;

	for (i = 0; i < GPON_SERVICES && n < max; i++) {
		struct gc1601_service *svc = &xp->service[i];
		struct gc1601_sfu_flow *f = &flows[n];
		int tcont;

		if (!svc->used || svc->vlan_valid == untagged)
			continue;
		tcont = gc1601_gpon_alloc_index(pon, svc->alloc_id);
		if (tcont < 0)
			continue;

		/*
		 * The VLAN's best-effort service and how many split by P-bit.
		 * Split VLANs keep one upstream flow per P-bit even when every
		 * P-bit goes to the same GEM: the VID-only upstream key cut
		 * Airtel from 900/924 to about 440/140 Mbit/s.
		 */
		best = svc;
		split = 0;
		for (j = 0; j < GPON_SERVICES; j++) {
			struct gc1601_service *o = &xp->service[j];

			if (!o->used || o->vlan_valid == untagged ||
			    (!untagged && o->vlan_id != svc->vlan_id) ||
			    gc1601_gpon_alloc_index(pon, o->alloc_id) < 0)
				continue;
			split += o->pcp_valid;
			if (gc1601_xpon_service_rank(o) < gc1601_xpon_service_rank(best))
				best = o;
		}

		f->vlan_id = untagged ? 0 : svc->vlan_id;
		f->untagged = untagged;
		if (!untagged)
			gc1601_xpon_lan_mode(xp, svc, f);
		f->gem_port = svc->gem_port;
		f->tcont = tcont;
		f->queue = svc->queue;
		if (!untagged && split > 1 && f->lan != GC1601_SFU_LAN_UNTAGGED) {
			f->pcp_match = svc->pcp_valid;
			f->pcp = svc->pcp;
			f->down_only = !svc->pcp_valid;
		} else {
			f->down_only = svc != best;
		}

		/* One upstream flow per key: a second service with the same P-bit goes down only. */
		for (j = 0; j < n && !f->down_only; j++)
			if (!flows[j].down_only && flows[j].vlan_id == f->vlan_id &&
			    flows[j].pcp_match == f->pcp_match && flows[j].pcp == f->pcp)
				f->down_only = true;

		/* Downstream matches GEM and VLAN, not the upstream P-bit. */
		for (j = 0; j < n; j++)
			if (flows[j].gem_port == f->gem_port && flows[j].vlan_id == f->vlan_id &&
			    !flows[j].up_only)
				break;
		if (j < n) {
			if (f->down_only) {
				memset(f, 0, sizeof(*f));
				continue;
			}
			if (flows[j].down_only) {
				flows[j] = *f;
				memset(f, 0, sizeof(*f));
				continue;
			}
			f->up_only = true;
		}
		n++;
	}
	return n;
}

/*
 * EPON has no OMCI services: the LAN side VLANs come only from
 * sfu_untag_vlan and sfu_vlan_map, which epon-oamd sets from the OLT's CTC
 * Port VLAN. pon0 rewrites them in software, as the switch flows do.
 */
static void gc1601_xpon_epon_lan_map(struct gc1601_xpon *xp)
{
	unsigned int i;

	spin_lock_bh(&xp->service_lock);
	xp->lan_maps = 0;
	if (xp->sfu_untag_vlan)
		xp->lan_map[xp->lan_maps++] = (struct gc1601_vlan_map) {
			.wan = xp->sfu_untag_vlan,
			.mode = GC1601_SFU_LAN_UNTAGGED,
		};
	for (i = 0; i < xp->vlan_maps && xp->lan_maps < ARRAY_SIZE(xp->lan_map); i++)
		xp->lan_map[xp->lan_maps++] = xp->vlan_map[i];
	spin_unlock_bh(&xp->service_lock);
}

/*
 * LLID 0 on T-CONT queue set 0. Transparent mode installs only observed
 * VLAN/P-bit pairs, plus one untagged flow. Explicit tagged mappings cover
 * all eight priorities. Both use parser rule 6 (TCI match): a VID-only
 * upstream key capped upload to ~45% of wire speed on Airtel. The first
 * pair for a VLAN also handles downstream, independently of its P-bit.
 * Only while the OAM link is up.
 */
static unsigned int gc1601_xpon_epon_flows(struct gc1601_pon *pon,
					   struct gc1601_sfu_flow *flows)
{
	struct gc1601_xpon *xp = &pon->xp;
	unsigned int i, j, n = 0;

	gc1601_xpon_epon_lan_map(xp);
	if (!epon_offload || !READ_ONCE(pon->epon.oam_link))
		return 0;
	spin_lock_bh(&xp->service_lock);
	if (!xp->lan_maps) {
		for (i = 0; i < xp->epon_learned_count; i++, n++) {
			flows[n].vlan_id = xp->epon_learned[i].vid;
			flows[n].pcp_match = true;
			flows[n].pcp = xp->epon_learned[i].pcp;
			for (j = 0; j < n; j++)
				if (flows[j].vlan_id == flows[n].vlan_id) {
					flows[n].up_only = true;
					break;
				}
		}
		flows[n++].untagged = true;
	}
	for (i = 0; i < xp->lan_maps && n < GC1601_SFU_FLOWS; i++) {
		if (xp->lan_map[i].mode == GC1601_SFU_LAN_UNTAGGED) {
			flows[n].vlan_id = xp->lan_map[i].wan;
			flows[n].lan = xp->lan_map[i].mode;
			flows[n].lan_vid = xp->lan_map[i].lan;
			n++;
		} else if (n + 8 <= GC1601_SFU_FLOWS) {
			unsigned int p;

			for (p = 0; p < 8; p++, n++) {
				flows[n].vlan_id = xp->lan_map[i].wan;
				flows[n].lan = xp->lan_map[i].mode;
				flows[n].lan_vid = xp->lan_map[i].lan;
				flows[n].pcp_match = true;
				flows[n].pcp = p;
				if (p > 0)
					flows[n].up_only = true;
			}
		} else {
			flows[n].vlan_id = xp->lan_map[i].wan;
			flows[n].lan = xp->lan_map[i].mode;
			flows[n].lan_vid = xp->lan_map[i].lan;
			n++;
		}
	}
	spin_unlock_bh(&xp->service_lock);
	return n;
}

/* EPON: the OAM link came up or went down; apply now, not a second later. */
void gc1601_xpon_sfu_kick(struct gc1601_pon *pon)
{
	if (!READ_ONCE(pon->epon.data_up)) {
		spin_lock_bh(&pon->xp.service_lock);
		pon->xp.epon_learned_count = 0;
		pon->xp.epon_capacity_misses = 0;
		spin_unlock_bh(&pon->xp.service_lock);
	}
	pon->xp.sfu_retries = 0;
	mod_delayed_work(system_wq, &pon->xp.sfu_work, 0);
}

/*
 * Hand the unicast services to the switch. An untagged flow would also take
 * the untagged frames meant for the device itself, so untagged services are
 * only offloaded when nothing is tagged. The agent re-applies every service
 * on each change: this runs a second after the last one, and only touches
 * the switch when the set changed.
 */
static void gc1601_xpon_sfu_work(struct work_struct *work)
{
	struct gc1601_xpon *xp = container_of(to_delayed_work(work),
					      struct gc1601_xpon, sfu_work);
	struct gc1601_pon *pon = container_of(xp, struct gc1601_pon, xp);
	struct gc1601_sfu_flow flows[GPON_SERVICES];
	unsigned int i, n;
	int ret;

	memset(flows, 0, sizeof(flows));	/* compared whole, padding included */
	if (READ_ONCE(pon->mode) == GC1601_PON_EPON) {
		n = gc1601_xpon_epon_flows(pon, flows);
		goto apply;
	}

	spin_lock_bh(&xp->service_lock);
	n = 0;
	if (READ_ONCE(pon->gpon.state) == GPON_STATE_O5) {
		n = gc1601_xpon_sfu_collect(pon, false, flows, ARRAY_SIZE(flows));
		if (!n)
			n = gc1601_xpon_sfu_collect(pon, true, flows, ARRAY_SIZE(flows));
	}
	/* pon0 rewrites as the switch does, offloaded or not. */
	xp->lan_maps = 0;
	for (i = 0; i < n; i++) {
		if (flows[i].lan == GC1601_SFU_LAN_SAME)
			continue;
		xp->lan_map[xp->lan_maps++] = (struct gc1601_vlan_map) {
			.wan = flows[i].vlan_id,
			.lan = flows[i].lan_vid,
			.mode = flows[i].lan,
		};
	}
	spin_unlock_bh(&xp->service_lock);
apply:
	if (n > ARRAY_SIZE(xp->sfu)) {
		dev_warn_ratelimited(pon->dev,
			"SFU needs %u entries, capacity %zu; using pon0 for all services\n",
			n, ARRAY_SIZE(xp->sfu));
		for (i = 0; i < n; i++)
			dev_info(pon->dev,
				"SFU wanted: VLAN %u%s GEM %u T-CONT %u queue %u P-bit %s%u%s%s\n",
				flows[i].vlan_id, flows[i].untagged ? " (untagged)" : "",
				flows[i].gem_port, flows[i].tcont, flows[i].queue,
				flows[i].pcp_match ? "" : "any/", flows[i].pcp,
				flows[i].down_only ? " down only" : "",
				flows[i].up_only ? " up only" : "");
		n = 0;
	}

	if (!sfu_offload)
		return;

	if (!READ_ONCE(xp->sfu_last_error) && n == xp->sfu_count &&
	    !memcmp(flows, xp->sfu, n * sizeof(*flows)))
		return;
	ret = gc1601_pe_sfu_set(flows, n);
	GPON_DIAG(flows, pon, flows, n, ret);
	spin_lock_bh(&xp->service_lock);
	xp->sfu_last_error = ret;
	if (!ret) {
		memcpy(xp->sfu, flows, n * sizeof(*flows));
		xp->sfu_count = n;
		xp->sfu_retries = 0;
	}
	spin_unlock_bh(&xp->service_lock);
	if (ret && xp->sfu_retries++ < 2)
		mod_delayed_work(system_wq, &xp->sfu_work, 5 * HZ);
}

static void gc1601_xpon_sfu_update(struct gc1601_xpon *xp)
{
	xp->sfu_retries = 0;
	mod_delayed_work(system_wq, &xp->sfu_work, HZ);
}

/* Learn only outbound client data, never PON broadcasts or management OAM. */
static void gc1601_xpon_epon_learn_vlan(struct gc1601_pon *pon,
				      const struct sk_buff *skb)
{
	struct gc1601_xpon *xp = &pon->xp;
	struct vlan_ethhdr buf;
	const struct vlan_ethhdr *hdr;
	unsigned int i;
	u16 tci, vid;
	u8 pcp;

	if (READ_ONCE(pon->mode) != GC1601_PON_EPON ||
	    !READ_ONCE(sfu_offload) || !READ_ONCE(epon_offload))
		return;
	hdr = skb_header_pointer(skb, 0, sizeof(buf), &buf);
	if (!hdr || hdr->h_vlan_proto != htons(ETH_P_8021Q) ||
	    !is_valid_ether_addr(hdr->h_dest) ||
	    ether_addr_equal(hdr->h_dest, xp->netdev->dev_addr))
		return;
	switch (hdr->h_vlan_encapsulated_proto) {
	case htons(ETH_P_IP):
	case htons(ETH_P_IPV6):
	case htons(ETH_P_PPP_DISC):
	case htons(ETH_P_PPP_SES):
		break;
	default:
		return;
	}
	tci = ntohs(hdr->h_vlan_TCI);
	vid = tci & VLAN_VID_MASK;
	if (!vid || vid == VLAN_VID_MASK || (tci & VLAN_CFI_MASK))
		return;
	pcp = (tci & VLAN_PRIO_MASK) >> VLAN_PRIO_SHIFT;

	spin_lock_bh(&xp->service_lock);
	if (!READ_ONCE(pon->epon.data_up) || xp->sfu_untag_vlan || xp->vlan_maps)
		goto out;
	for (i = 0; i < xp->epon_learned_count; i++)
		if (xp->epon_learned[i].vid == vid && xp->epon_learned[i].pcp == pcp)
			goto out;
	if (xp->epon_learned_count == ARRAY_SIZE(xp->epon_learned)) {
		xp->epon_capacity_misses++;
		goto out;
	}
	xp->epon_learned[xp->epon_learned_count].vid = vid;
	xp->epon_learned[xp->epon_learned_count++].pcp = pcp;
	gc1601_xpon_sfu_update(xp);
out:
	spin_unlock_bh(&xp->service_lock);
}

static void gc1601_xpon_reconcile_work(struct work_struct *work)
{
	struct gc1601_xpon *xp = container_of(work, struct gc1601_xpon, reconcile_work);
	struct gc1601_pon *pon = container_of(xp, struct gc1601_pon, xp);

	mutex_lock(&pon->lock);
	if (!READ_ONCE(xp->started) || !READ_ONCE(pon->gpon.running) ||
	    READ_ONCE(pon->gpon.state) != GPON_STATE_O5 ||
	    !READ_ONCE(pon->gpon.omcc_valid))
		goto out;
	omci_device_reconcile_services(xp->odev);
	if (READ_ONCE(pon->gpon.retained_valid) && xp->services)
		omci_device_resume_retained(xp->odev);
out:
	mutex_unlock(&pon->lock);
}

void gc1601_xpon_report_alloc(struct gc1601_pon *pon)
{
	WRITE_ONCE(pon->xp.sfu_count, U32_MAX);
	gc1601_xpon_sfu_update(&pon->xp);
	schedule_work(&pon->xp.reconcile_work);
}

static ssize_t sfu_untag_vlan_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(pon->xp.sfu_untag_vlan));
}

/*
 * The VLAN whose frames the LAN side sends and expects untagged: the switch
 * pushes it upstream and pops it downstream. Untagged frames can only map to
 * one VLAN; 0 keeps every service tagged on the LAN.
 */
static ssize_t sfu_untag_vlan_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	u16 vid;
	int ret;

	ret = kstrtou16(buf, 0, &vid);
	if (ret)
		return ret;
	if (vid > 4094)
		return -ERANGE;
	spin_lock_bh(&pon->xp.service_lock);
	WRITE_ONCE(pon->xp.sfu_untag_vlan, vid);
	pon->xp.epon_learned_count = 0;
	pon->xp.epon_capacity_misses = 0;
	spin_unlock_bh(&pon->xp.service_lock);
	gc1601_xpon_sfu_update(&pon->xp);
	return count;
}
static DEVICE_ATTR_RW(sfu_untag_vlan);

static ssize_t sfu_vlan_map_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	struct gc1601_xpon *xp = &pon->xp;
	unsigned int i;
	ssize_t len = 0;

	spin_lock_bh(&xp->service_lock);
	for (i = 0; i < xp->vlan_maps; i++)
		len += sysfs_emit_at(buf, len, "%c%u:%u ",
				     xp->vlan_map[i].mode == GC1601_SFU_LAN_STACKED ? 's' : 't',
				     xp->vlan_map[i].wan, xp->vlan_map[i].lan);
	spin_unlock_bh(&xp->service_lock);
	return len + sysfs_emit_at(buf, len, "\n");
}

/*
 * Manual LAN-side VLANs, ahead of what the OLT's class 171 rules ask for:
 * "tWAN:LAN" translates LAN to WAN, "sWAN:LAN" stacks WAN over the LAN tag,
 * separated by spaces. An empty write clears the map.
 */
static ssize_t sfu_vlan_map_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	struct gc1601_xpon *xp = &pon->xp;
	struct gc1601_vlan_map map[ARRAY_SIZE(xp->vlan_map)];
	unsigned int n = 0, wan, lan;
	const char *p = buf;
	char mode;
	int used;

	while (*p) {
		if (isspace(*p) || *p == ',') {
			p++;
			continue;
		}
		if (n == ARRAY_SIZE(map) ||
		    sscanf(p, "%c%u:%u%n", &mode, &wan, &lan, &used) != 3 ||
		    (mode != 't' && mode != 's') ||
		    !wan || wan > 4094 || !lan || lan > 4094)
			return -EINVAL;
		map[n++] = (struct gc1601_vlan_map) {
			.wan = wan,
			.lan = lan,
			.mode = mode == 's' ? GC1601_SFU_LAN_STACKED :
					      GC1601_SFU_LAN_TRANSLATE,
		};
		p += used;
	}

	spin_lock_bh(&xp->service_lock);
	memcpy(xp->vlan_map, map, n * sizeof(*map));
	xp->vlan_maps = n;
	xp->epon_learned_count = 0;
	xp->epon_capacity_misses = 0;
	spin_unlock_bh(&xp->service_lock);
	gc1601_xpon_sfu_update(xp);
	return count;
}
static DEVICE_ATTR_RW(sfu_vlan_map);

static ssize_t sfu_offload_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(sfu_offload));
}

/*
 * 0 takes the flows out of the switch, so every frame reaches pon0 again
 * (a VLAN scan needs the replies); 1 puts them back.
 */
static ssize_t sfu_offload_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	struct gc1601_xpon *xp = &pon->xp;
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;
	WRITE_ONCE(sfu_offload, on);
	cancel_delayed_work_sync(&xp->sfu_work);
	xp->sfu_count = U32_MAX;
	if (on)
		mod_delayed_work(system_wq, &xp->sfu_work, 0);
	else {
		ret = gc1601_pe_sfu_set(NULL, 0);
		spin_lock_bh(&xp->service_lock);
		xp->sfu_last_error = ret;
		if (!ret)
			xp->sfu_count = 0;
		spin_unlock_bh(&xp->service_lock);
		if (ret)
			return ret;
	}
	return count;
}
static DEVICE_ATTR_RW(sfu_offload);

static ssize_t epon_offload_status_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	struct gc1601_xpon *xp = &pon->xp;
	const char *policy;
	unsigned int i, j, cached;
	bool valid, learning;
	ssize_t len;

	spin_lock_bh(&xp->service_lock);
	if (READ_ONCE(pon->mode) != GC1601_PON_EPON)
		policy = "inactive";
	else if (!READ_ONCE(sfu_offload) || !READ_ONCE(epon_offload))
		policy = "disabled";
	else if (!READ_ONCE(pon->epon.data_up))
		policy = "link-down";
	else if (xp->sfu_untag_vlan || xp->vlan_maps)
		policy = "explicit";
	else
		policy = "learning";
	learning = !strcmp(policy, "learning");
	cached = READ_ONCE(xp->sfu_count);
	valid = cached <= ARRAY_SIZE(xp->sfu) && !xp->sfu_last_error;
	len = sysfs_emit(buf,
		"policy=%s learned=%u capacity=%zu capacity_misses=%lu\n",
		policy, xp->epon_learned_count, ARRAY_SIZE(xp->epon_learned),
		xp->epon_capacity_misses);
	len += sysfs_emit_at(buf, len,
		"cached_flows=%u cache_valid=%u last_error=%d hardware_hits=unavailable aging=off\n",
		cached <= ARRAY_SIZE(xp->sfu) ? cached : 0, valid, xp->sfu_last_error);
	for (i = 0; i < xp->epon_learned_count; i++) {
		bool up = false, down = false;

		for (j = 0; valid && learning && j < cached; j++) {
			const struct gc1601_sfu_flow *f = &xp->sfu[j];

			if (f->untagged || f->gem_port || f->tcont ||
			    f->lan != GC1601_SFU_LAN_SAME ||
			    f->vlan_id != xp->epon_learned[i].vid)
				continue;
			up |= !f->down_only && f->pcp_match &&
			      f->pcp == xp->epon_learned[i].pcp;
			down |= !f->up_only;
		}
		len += sysfs_emit_at(buf, len,
			"vlan=%u pbit=%u programmed_up=%u programmed_down=%u\n",
			xp->epon_learned[i].vid, xp->epon_learned[i].pcp, up, down);
	}
	spin_unlock_bh(&xp->service_lock);
	return len;
}
static DEVICE_ATTR_RO(epon_offload_status);

static struct attribute *gc1601_xpon_attrs[] = {
	&dev_attr_sfu_untag_vlan.attr,
	&dev_attr_sfu_offload.attr,
	&dev_attr_sfu_vlan_map.attr,
	&dev_attr_epon_offload_status.attr,
	NULL
};

const struct attribute_group gc1601_xpon_group = {
	.attrs = gc1601_xpon_attrs,
};

#define OMCI_TRAILER_LEN	4
#define OMCI_DEV_BASELINE	0x0a
#define OMCI_DEV_EXTENDED	0x0b
#define OMCI_BASELINE_PDU	44
#define OMCI_EXTENDED_HDR	10
#define OMCI_FRAME_MAX		2048

static u32 gc1601_omci_crc(const u8 *data, unsigned int len)
{
	return ~crc32_be(~0, data, len);
}

/* PDU length without a trailer, or 0 if the frame is not OMCI. */
static unsigned int gc1601_omci_pdu_len(const u8 *data, unsigned int len)
{
	if (len < OMCI_EXTENDED_HDR)
		return 0;
	if (data[3] == OMCI_DEV_BASELINE)
		return OMCI_BASELINE_PDU;
	if (data[3] == OMCI_DEV_EXTENDED)
		return OMCI_EXTENDED_HDR + get_unaligned_be16(data + 8);
	return 0;
}

/*
 * Packet engine NAPI context. The agent answers whatever it decodes and does
 * not look at the trailer flags, so a frame with a bad CRC stops here. Bytes
 * past the trailer are padding and would fail the agent's length check.
 */
static void gc1601_xpon_omci_rx(void *ctx, const u8 *data, unsigned int len)
{
	struct gc1601_pon *pon = ctx;
	unsigned int pdu = gc1601_omci_pdu_len(data, len);
	struct sk_buff *skb;
	u32 flags = 0;

	pon->xp.omci_rx_seen++;
	GPON_DIAG(omci, pon, false, data, len, 0);
	WRITE_ONCE(pon->xp.omci_rx_last_len, len);
	WRITE_ONCE(pon->xp.omci_rx_last_header,
		   len >= 4 ? get_unaligned_be32(data) : 0);
	WRITE_ONCE(pon->xp.omci_rx_last_crc, 0);
	WRITE_ONCE(pon->xp.omci_rx_expected_crc, 0);
	if (!pdu || len < pdu) {
		pon->xp.omci_rx_errors++;
		return;
	}
	if (len >= pdu + OMCI_TRAILER_LEN) {
		u32 expected = gc1601_omci_crc(data, pdu);
		u32 received = get_unaligned_be32(data + pdu);

		WRITE_ONCE(pon->xp.omci_rx_last_crc, received);
		WRITE_ONCE(pon->xp.omci_rx_expected_crc, expected);
		if (expected != received) {
			pon->xp.omci_crc_errors++;
			return;
		}
		flags = OMCI_F_MIC_PRESENT | OMCI_F_MIC_VALID;
		len = pdu + OMCI_TRAILER_LEN;
	} else {
		len = pdu;
	}

	skb = alloc_skb(len, GFP_ATOMIC);
	if (!skb) {
		pon->xp.omci_rx_alloc_failures++;
		return;
	}
	skb_put_data(skb, data, len);
	pon->xp.omci_rx_to_core++;
	omci_device_receive(pon->xp.odev, skb, READ_ONCE(pon->gpon.omcc_port), flags);
}

static int gc1601_xpon_omci_start(struct omci_device *odev)
{
	return gc1601_pe_omci_register(gc1601_xpon_omci_rx, omci_device_priv(odev));
}

static void gc1601_xpon_omci_stop(struct omci_device *odev)
{
	gc1601_pe_omci_unregister();
}

static int gc1601_xpon_omci_xmit(struct omci_device *odev, struct sk_buff *skb,
				 u16 gem_port_id)
{
	struct gc1601_pon *pon = omci_device_priv(odev);
	struct gc1601_gpon *g = &pon->gpon;
	unsigned int len = skb->len;
	u8 *frame;
	int ret;

	pon->xp.omci_tx_attempts++;
	/* OMCI only flows on an operational, transmitting ONU (PMU_xmit: O5). */
	if (!READ_ONCE(g->allow_tx)) {
		ret = -EPERM;
		goto out;
	}
	if (READ_ONCE(g->state) != GPON_STATE_O5) {
		ret = -ENOTCONN;
		goto out;
	}
	if (len + OMCI_TRAILER_LEN > OMCI_FRAME_MAX) {
		ret = -EMSGSIZE;
		goto out;
	}

	frame = kmalloc(len + OMCI_TRAILER_LEN, GFP_KERNEL);
	if (!frame) {
		ret = -ENOMEM;
		goto out;
	}
	skb_copy_bits(skb, 0, frame, len);
	put_unaligned_be32(gc1601_omci_crc(frame, len), frame + len);

	ret = gc1601_pe_omci_xmit(frame, len + OMCI_TRAILER_LEN, gem_port_id);
	GPON_DIAG(omci, pon, true, frame, len + OMCI_TRAILER_LEN, ret);
	kfree(frame);
	if (!ret)
		consume_skb(skb);
	goto done;
out:
	if (skb_headlen(skb) >= min(len, 8U))
		GPON_DIAG(omci, pon, true, skb->data, skb_headlen(skb), ret);
done:
	WRITE_ONCE(pon->xp.omci_tx_last_error, ret);
	if (ret)
		pon->xp.omci_tx_failures++;
	return ret;
}

static int gc1601_xpon_get_ani_topology(struct omci_device *odev,
					struct omci_ani_topology *topology)
{
	/* 15 OLT-assignable T-CONTs (index 0 is the default allocation), 8 queues each. */
	*topology = (struct omci_ani_topology) {
		.tcont_base = 0x8000,
		.scheduler_base = 0x8000,
		.queue_base = 0x8000,
		.maximum_queue_size = 0xffff,
		.allocated_queue_size = 4,
		.tcont_count = GPON_TCONTS - 1,
		.queues_per_tcont = 8,
		.queue_config_option = 1,
		.scheduler_policy = 1,
	};
	return 0;
}

/*
 * PLOAM Assign_Alloc-ID owns the hardware Alloc-ID table; OMCI only names
 * which T-CONT entity carries which Alloc-ID. Keep that for the data path.
 */
static int gc1601_xpon_set_tcont(struct omci_device *odev, u16 entity_id,
				 u16 alloc_id, bool valid)
{
	struct gc1601_xpon *xp = &((struct gc1601_pon *)omci_device_priv(odev))->xp;
	unsigned int i = entity_id & 0xff;

	if (i >= GPON_TCONTS)
		return -ERANGE;
	GPON_DIAG(tcont, omci_device_priv(odev), entity_id, alloc_id, valid);
	xp->tcont_entity[i] = entity_id;
	xp->tcont_alloc[i] = valid ? alloc_id : U16_MAX;
	gc1601_xpon_sfu_update(xp);
	return 0;
}

static int gc1601_xpon_set_gem_port(struct omci_device *odev, u16 entity_id,
				    u16 gem_port_id, u16 tcont_entity_id,
				    u8 direction, bool valid, bool encrypted)
{
	GPON_DIAG(gem, omci_device_priv(odev), entity_id, gem_port_id, tcont_entity_id,
		  direction, valid, encrypted);
	if (gem_port_id > 0xfff)
		return -ERANGE;
	gc1601_gpon_omci_set_gem(omci_device_priv(odev), gem_port_id, valid, encrypted);
	return 0;
}

static int gc1601_xpon_set_uni(struct omci_device *odev, u16 entity_id, bool enable)
{
	return gc1601_pe_set_uni(enable);
}

static int gc1601_xpon_set_uni_attrs(struct omci_device *odev, u16 entity_id,
				     const struct omci_uni_attrs *attrs)
{
	struct gc1601_uni_attrs a = {
		.pause_quanta = attrs->pause_quanta,
		.ageing_s = attrs->ageing_s,
		.learn_limit = attrs->learn_limit,
		.down_kbps = attrs->down_kbps,
		.up_kbps = attrs->up_kbps,
	};

	return gc1601_pe_set_uni_attrs(&a);
}

static void gc1601_xpon_disable_unused_gem(struct gc1601_pon *pon, u16 gem_port)
{
	struct gc1601_xpon *xp = &pon->xp;
	bool used = false;
	unsigned int i;

	if (gem_port == U16_MAX ||
	    (READ_ONCE(pon->gpon.omcc_valid) &&
	     READ_ONCE(pon->gpon.omcc_port) == gem_port))
		return;

	spin_lock_bh(&xp->service_lock);
	for (i = 0; i < GPON_SERVICES; i++) {
		if ((xp->service[i].used && xp->service[i].gem_port == gem_port) ||
		    (xp->multicast[i].used && xp->multicast[i].gem_port == gem_port)) {
			used = true;
			break;
		}
	}
	spin_unlock_bh(&xp->service_lock);

	if (!used)
		gc1601_gpon_omci_set_gem(pon, gem_port, false, false);
}

/* Hardware forwarding comes with the data path; record what OMCI asked for. */
/*
 * The pon0 data path carries the unicast services through the CPU: upstream
 * frames pick their GEM port and T-CONT by VLAN. Multicast services are
 * downstream only and need no entry.
 */
static int gc1601_xpon_replace_service(struct omci_device *odev,
				       const struct omci_service_config *service)
{
	struct gc1601_pon *pon = omci_device_priv(odev);
	struct gc1601_xpon *xp = &pon->xp;
	struct gc1601_service *svc = NULL;
	u16 old_gem = U16_MAX;
	unsigned int i;
	int ret;

	/* The agent re-applies every service on each change; keep it quiet. */
	dev_dbg(pon->dev, "OMCI service %u: GEM %u Alloc-ID %u VLAN %u%s\n",
		service->cookie, service->gem_port_id, service->alloc_id,
		service->vlan_valid ? service->vlan_id : 0,
		service->multicast ? " multicast" : "");
	GPON_DIAG(service, pon, service);
	if (service->gem_port_id > 0xfff)
		return -ERANGE;
	if (service->multicast) {
		unsigned int free_slot = GPON_SERVICES;

		spin_lock_bh(&xp->service_lock);
		for (i = 0; i < GPON_SERVICES; i++) {
			if (xp->multicast[i].used &&
			    xp->multicast[i].cookie == service->cookie)
				break;
			if (!xp->multicast[i].used && free_slot == GPON_SERVICES)
				free_slot = i;
		}
		if (i == GPON_SERVICES)
			i = free_slot;
		if (i < GPON_SERVICES) {
			if (xp->multicast[i].used)
				old_gem = xp->multicast[i].gem_port;
			xp->multicast[i].cookie = service->cookie;
			xp->multicast[i].gem_port = service->gem_port_id;
			xp->multicast[i].used = true;
		}
		spin_unlock_bh(&xp->service_lock);
		if (i == GPON_SERVICES)
			return -ENOSPC;
		if (old_gem != service->gem_port_id)
			gc1601_xpon_disable_unused_gem(pon, old_gem);
		return 0;
	}
	ret = gc1601_gpon_restore_service(pon, service->gem_port_id, service->alloc_id);
	if (ret)
		return ret;

	spin_lock_bh(&xp->service_lock);
	for (i = 0; i < GPON_SERVICES; i++) {
		if (xp->service[i].used && xp->service[i].cookie == service->cookie) {
			svc = &xp->service[i];
			break;
		}
		if (!svc && !xp->service[i].used)
			svc = &xp->service[i];
	}
	if (svc) {
		if (svc->used)
			old_gem = svc->gem_port;
		if (!svc->used)
			xp->services++;
		*svc = (struct gc1601_service) {
			.cookie = service->cookie,
			.gem_port = service->gem_port_id,
			.alloc_id = service->alloc_id,
			.vlan_id = service->vlan_id,
			.queue = service->queue & 7,
			.pcp = service->pcp & 7,
			.pcp_valid = service->pcp_valid,
			.default_service = service->default_service,
			.vlan_valid = service->vlan_valid,
			.used = true,
			/* enum omci_uni_vlan_mode lists the modes in our order. */
			.uni_mode = service->uni_vlan_mode,
			.uni_vid = service->uni_vlan_id,
		};
	}
	spin_unlock_bh(&xp->service_lock);
	if (!svc) {
		dev_warn_ratelimited(pon->dev, "pon0 service table full, service %u not carried\n",
				     service->cookie);
		return -ENOSPC;
	}
	if (old_gem != service->gem_port_id)
		gc1601_xpon_disable_unused_gem(pon, old_gem);
	gc1601_xpon_sfu_update(xp);
	return 0;
}

static int gc1601_xpon_delete_service(struct omci_device *odev, u32 cookie)
{
	struct gc1601_pon *pon = omci_device_priv(odev);
	struct gc1601_xpon *xp = &pon->xp;
	u16 gem_port = U16_MAX;
	unsigned int i;

	GPON_DIAG(service_delete, pon, cookie);
	spin_lock_bh(&xp->service_lock);
	for (i = 0; i < GPON_SERVICES; i++) {
		if (xp->service[i].used && xp->service[i].cookie == cookie) {
			gem_port = xp->service[i].gem_port;
			xp->service[i].used = false;
			xp->services--;
		}
		if (xp->multicast[i].used && xp->multicast[i].cookie == cookie) {
			gem_port = xp->multicast[i].gem_port;
			xp->multicast[i].used = false;
		}
	}
	spin_unlock_bh(&xp->service_lock);
	gc1601_xpon_disable_unused_gem(pon, gem_port);
	gc1601_xpon_sfu_update(xp);
	return 0;
}

/* SFF-8472 A2 diagnostics from the BOSA, which applies its own calibration. */
static int gc1601_xpon_get_telemetry(struct omci_device *odev,
				     struct omci_telemetry *t)
{
	struct gc1601_pon *pon = omci_device_priv(odev);
	u8 d[18];
	u16 a;

	memset(t, 0, sizeof(*t));
	t->downstream_fec = readb(pon->mac + PON_OPT_RX + 0x13) & BIT(7) ?
			    OMCI_FEC_STATUS_UP : OMCI_FEC_STATUS_DOWN;
	t->valid = OMCI_TELEMETRY_F_FEC_DOWNSTREAM;
	/* Summed in O5 every second; the agent turns them into SF/SD. */
	if (READ_ONCE(pon->gpon.state) == GPON_STATE_O5) {
		t->bip_errors = gc1601_gpon_bip_errors(pon);
		t->valid |= OMCI_TELEMETRY_F_BIP_ERRORS;
		gc1601_gpon_fec(pon, t);
		t->valid |= OMCI_TELEMETRY_F_FEC_COUNTERS;
	}

	/* Before programming, the A2 diagnostics are uncalibrated noise. */
	if (READ_ONCE(pon->bosa_state) != GC1601_BOSA_READY ||
	    gc1601_bosa_read(pon, 0x60, d, sizeof(d)))
		return 0;
	t->bosa_temperature_mc = (s16)get_unaligned_be16(d) * 1000 / 256;
	t->bosa_voltage_uv = get_unaligned_be16(d + 2) * 100;
	t->bosa_bias_ua = get_unaligned_be16(d + 4) * 2;
	t->bosa_tx_power_nw = get_unaligned_be16(d + 6) * 100;
	t->bosa_rx_power_nw = get_unaligned_be16(d + 8) * 100;

	/* Alarm flags at 0x70/0x71 */
	a = get_unaligned_be16(d + 16);
	if (a & BIT(15))
		t->bosa_alarms |= OPTICAL_FRONTEND_ALARM_HIGH_TEMP;
	if (a & BIT(14))
		t->bosa_alarms |= OPTICAL_FRONTEND_ALARM_LOW_TEMP;
	if (a & BIT(13))
		t->bosa_alarms |= OPTICAL_FRONTEND_ALARM_HIGH_VOLTAGE;
	if (a & BIT(12))
		t->bosa_alarms |= OPTICAL_FRONTEND_ALARM_LOW_VOLTAGE;
	if (a & BIT(11))
		t->bosa_alarms |= OPTICAL_FRONTEND_ALARM_TX_HIGH_BIAS;
	if (a & BIT(10))
		t->bosa_alarms |= OPTICAL_FRONTEND_ALARM_TX_LOW_BIAS;
	if (a & BIT(9))
		t->bosa_alarms |= OPTICAL_FRONTEND_ALARM_TX_HIGH_POWER;
	if (a & BIT(8))
		t->bosa_alarms |= OPTICAL_FRONTEND_ALARM_TX_LOW_POWER;
	if (a & BIT(7))
		t->bosa_alarms |= OPTICAL_FRONTEND_ALARM_RX_HIGH_POWER;
	if (a & BIT(6))
		t->bosa_alarms |= OPTICAL_FRONTEND_ALARM_RX_LOW_POWER;

	t->valid |= OMCI_TELEMETRY_F_BOSA_TEMPERATURE | OMCI_TELEMETRY_F_BOSA_VOLTAGE |
		    OMCI_TELEMETRY_F_BOSA_BIAS | OMCI_TELEMETRY_F_BOSA_TX_POWER |
		    OMCI_TELEMETRY_F_BOSA_RX_POWER | OMCI_TELEMETRY_F_BOSA_ALARMS;
	return 0;
}

static int gc1601_xpon_get_uni_link(struct omci_device *odev, u16 entity_id,
				    bool *link_up)
{
	*link_up = gc1601_pe_link_up();
	return 0;
}

static int gc1601_xpon_set_olt_profile(struct omci_device *odev,
				       const struct omci_olt_profile_state *state)
{
	struct gc1601_pon *pon = omci_device_priv(odev);

	/* The agent pushes the profile on every change; say it while GPON runs. */
	if (pon->xp.olt_profile != state->effective && READ_ONCE(pon->gpon.running))
		dev_info(pon->dev, "OLT profile %s\n",
			 omci_olt_profile_name(state->effective));
	pon->xp.olt_profile = state->effective;
	return 0;
}

static void gc1601_xpon_set_operational(struct omci_device *odev, bool operational)
{
	struct gc1601_pon *pon = omci_device_priv(odev);

	dev_info(pon->dev, "OMCI agent %soperational\n", operational ? "" : "not ");
}

/* omci-agent pushes the ONU identity here; apply serial and password. */
static void gc1601_xpon_config_changed(struct omci_device *odev, u16 key,
				       const struct omci_identity *id)
{
	struct gc1601_pon *pon = omci_device_priv(odev);

	if (!id)
		return;
	gc1601_gpon_set_identity(pon,
				 id->valid & OMCI_IDENTITY_F_SERIAL_NUMBER ? id->serial_number : NULL,
				 id->valid & OMCI_IDENTITY_F_PASSWORD ? id->password : NULL);
}

static const struct omci_device_ops gc1601_xpon_omci_ops = {
	.start = gc1601_xpon_omci_start,
	.stop = gc1601_xpon_omci_stop,
	.xmit = gc1601_xpon_omci_xmit,
	.get_ani_topology = gc1601_xpon_get_ani_topology,
	.set_tcont = gc1601_xpon_set_tcont,
	.set_gem_port = gc1601_xpon_set_gem_port,
	.set_uni = gc1601_xpon_set_uni,
	.set_uni_attrs = gc1601_xpon_set_uni_attrs,
	.replace_service = gc1601_xpon_replace_service,
	.delete_service = gc1601_xpon_delete_service,
	.get_telemetry = gc1601_xpon_get_telemetry,
	.get_uni_link = gc1601_xpon_get_uni_link,
	.set_olt_profile = gc1601_xpon_set_olt_profile,
	.set_operational = gc1601_xpon_set_operational,
	.config_changed = gc1601_xpon_config_changed,
};

/* ---- pon0 ---- */

static int gc1601_pon0_open(struct net_device *dev)
{
	netif_start_queue(dev);
	return 0;
}

static int gc1601_pon0_stop(struct net_device *dev)
{
	netif_stop_queue(dev);
	return 0;
}

/* Upstream service of a frame: its VLAN, else the untagged/default one. */
static bool gc1601_pon0_service(struct gc1601_xpon *xp, struct sk_buff *skb,
				struct gc1601_service *out)
{
	const struct gc1601_service *svc = NULL, *fallback = NULL, *s;
	bool tagged;
	u16 tci = 0, vid;
	u8 pcp;
	unsigned int i;

	tagged = !__vlan_get_tag(skb, &tci);
	vid = tci & VLAN_VID_MASK;
	pcp = tci >> VLAN_PRIO_SHIFT;

	/* Same VLAN: the service for the frame's priority, else the best-effort one. */
	spin_lock(&xp->service_lock);
	for (i = 0; i < GPON_SERVICES; i++) {
		s = &xp->service[i];
		if (!s->used)
			continue;
		if (s->vlan_valid && tagged && s->vlan_id == vid) {
			if (s->pcp_valid && s->pcp == pcp) {
				svc = s;
				break;
			}
			if (!svc || gc1601_xpon_service_rank(s) < gc1601_xpon_service_rank(svc))
				svc = s;
		}
		if (!s->vlan_valid && !fallback)
			fallback = s;
	}
	if (!svc)
		svc = fallback;
	if (svc)
		*out = *svc;
	spin_unlock(&xp->service_lock);
	return svc;
}

/*
 * The LAN side's VLAN rewrite for a frame, as the switch flows do it:
 * broadcasts such as ARP reach the CPU, not the flows.
 */
static bool gc1601_xpon_lan_map(struct gc1601_xpon *xp, bool up, bool tagged,
				u16 vid, struct gc1601_vlan_map *out)
{
	bool found = false;
	unsigned int i;

	spin_lock(&xp->service_lock);
	for (i = 0; i < xp->lan_maps && !found; i++) {
		const struct gc1601_vlan_map *m = &xp->lan_map[i];

		if (up)
			found = tagged ? m->mode != GC1601_SFU_LAN_UNTAGGED && m->lan == vid :
					 m->mode == GC1601_SFU_LAN_UNTAGGED;
		else
			found = tagged && m->wan == vid;
		if (found)
			*out = *m;
	}
	spin_unlock(&xp->service_lock);
	return found;
}

static void gc1601_vlan_set_vid(u8 *frame, u16 vid)
{
	u16 tci = get_unaligned_be16(frame + ETH_HLEN);

	put_unaligned_be16((tci & ~VLAN_VID_MASK) | vid, frame + ETH_HLEN);
}

/*
 * EPON: LLID 0 on T-CONT queue set 0. OAM goes out once the OLT has
 * registered the LLID, everything else once epon-oamd has brought the OAM
 * link up. The GEM field is left 0; what the EPON MAC expects there is
 * still to be seen on hardware (gc1601-epon-diag logs both directions).
 */
static netdev_tx_t gc1601_pon0_xmit_epon(struct gc1601_pon *pon, struct sk_buff *skb,
					 struct net_device *dev)
{
	unsigned int len = skb->len;
	int ret;

	bool oam = len >= ETH_HLEN &&
		   get_unaligned_be16(skb->data + 2 * ETH_ALEN) == ETH_P_SLOW;

	if (!gc1601_epon_may_send(pon, oam)) {
		gc1601_epon_tx_seen(pon, skb->data, len, -ENOLINK);
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	gc1601_xpon_epon_learn_vlan(pon, skb);
	gc1601_epon_tx_seen(pon, skb->data, len, 0);
	ret = gc1601_pe_pon_xmit(skb, 0, 0, 0);
	if (ret == -EBUSY)
		return NETDEV_TX_BUSY;
	if (ret) {
		dev->stats.tx_errors++;
		return NETDEV_TX_OK;
	}
	dev->stats.tx_packets++;
	dev->stats.tx_bytes += len;
	return NETDEV_TX_OK;
}

static netdev_tx_t gc1601_pon0_xmit_frame(struct sk_buff *skb, struct net_device *dev)
{
	struct gc1601_pon *pon = *(struct gc1601_pon **)netdev_priv(dev);
	struct gc1601_vlan_map map;
	struct gc1601_service svc;
	unsigned int len;
	int tcont, ret;
	bool tagged;
	u16 vid = 0;

	/* OAM discovery must stay untagged, independent of the service VLAN. */
	if (READ_ONCE(pon->mode) == GC1601_PON_EPON &&
	    skb->len >= ETH_HLEN &&
	    get_unaligned_be16(skb->data + 2 * ETH_ALEN) == ETH_P_SLOW)
		return gc1601_pon0_xmit_epon(pon, skb, dev);

	tagged = !__vlan_get_tag(skb, &vid);
	if (gc1601_xpon_lan_map(&pon->xp, true, tagged, vid & VLAN_VID_MASK, &map)) {
		if (map.mode == GC1601_SFU_LAN_TRANSLATE) {
			if (skb_ensure_writable(skb, VLAN_ETH_HLEN))
				goto drop;
			gc1601_vlan_set_vid(skb->data, map.wan);
		} else {
			skb = vlan_insert_tag_set_proto(skb, htons(ETH_P_8021Q), map.wan);
			if (!skb) {
				dev->stats.tx_dropped++;
				return NETDEV_TX_OK;
			}
		}
	}
	if (READ_ONCE(pon->mode) == GC1601_PON_EPON)
		return gc1601_pon0_xmit_epon(pon, skb, dev);
	len = skb->len;

	if (READ_ONCE(pon->gpon.state) != GPON_STATE_O5)
		goto drop;
	if (!gc1601_pon0_service(&pon->xp, skb, &svc)) {
		/* No OMCI service carries this VLAN: what an unsupported OLT setup looks like. */
		GPON_DIAG(tx, pon, 0, -1, skb->data, len, -ENOENT);
		goto drop;
	}
	tcont = gc1601_gpon_alloc_index(pon, svc.alloc_id);
	if (tcont < 0)
		goto drop;

	GPON_DIAG(tx, pon, svc.gem_port, tcont, skb->data, len, 0);
	ret = gc1601_pe_pon_xmit(skb, svc.gem_port, tcont, svc.queue);
	if (ret == -EBUSY)
		return NETDEV_TX_BUSY;
	if (ret) {
		dev->stats.tx_errors++;
		return NETDEV_TX_OK;
	}
	dev->stats.tx_packets++;
	dev->stats.tx_bytes += len;
	return NETDEV_TX_OK;

drop:
	dev->stats.tx_dropped++;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static netdev_tx_t gc1601_pon0_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct sk_buff *frame;
	netdev_tx_t ret;

	/* Keep the qdisc's frame unchanged if the shared ring asks for a retry. */
	frame = skb_clone(skb, GFP_ATOMIC);
	if (!frame) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	ret = gc1601_pon0_xmit_frame(frame, dev);
	if (ret == NETDEV_TX_BUSY)
		dev_kfree_skb_any(frame);
	else
		dev_kfree_skb_any(skb);
	return ret;
}

/* Packet-engine NAPI context: a downstream frame from one of our GEM ports. */
static void gc1601_pon0_rx(void *ctx, struct napi_struct *napi, u16 gem_port,
			   const u8 *data, unsigned int len)
{
	struct gc1601_pon *pon = ctx;
	struct net_device *dev = pon->xp.netdev;
	struct gc1601_vlan_map map;
	struct sk_buff *skb;
	bool mapped, strip;

	if (READ_ONCE(pon->mode) == GC1601_PON_EPON)
		gc1601_epon_rx_seen(pon, gem_port, data, len);
	else
		GPON_DIAG(rx, pon, gem_port, data, len);
	if (!netif_running(dev)) {
		dev->stats.rx_dropped++;
		return;
	}
	mapped = len >= VLAN_ETH_HLEN &&
		 get_unaligned_be16(data + 2 * ETH_ALEN) == ETH_P_8021Q &&
		 gc1601_xpon_lan_map(&pon->xp, false, true,
				     get_unaligned_be16(data + ETH_HLEN) & VLAN_VID_MASK,
				     &map);
	/* Untagged and stacked LAN sides lose the network tag; translated swap it. */
	strip = mapped && map.mode != GC1601_SFU_LAN_TRANSLATE;
	skb = napi_alloc_skb(napi, strip ? len - VLAN_HLEN : len);
	if (!skb) {
		dev->stats.rx_dropped++;
		return;
	}
	if (strip) {
		skb_put_data(skb, data, 2 * ETH_ALEN);
		skb_put_data(skb, data + VLAN_ETH_HLEN - 2, len - (VLAN_ETH_HLEN - 2));
		len -= VLAN_HLEN;
	} else {
		skb_put_data(skb, data, len);
		if (mapped)
			gc1601_vlan_set_vid(skb->data, map.lan);
	}
	skb->protocol = eth_type_trans(skb, dev);
	dev->stats.rx_packets++;
	dev->stats.rx_bytes += len;
	napi_gro_receive(napi, skb);
}

static const struct net_device_ops gc1601_pon0_ops = {
	.ndo_open = gc1601_pon0_open,
	.ndo_stop = gc1601_pon0_stop,
	.ndo_start_xmit = gc1601_pon0_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

/* The PON line rate, for LuCI and ethtool: 2.488G G-PON downstream, 1G EPON. */
static int gc1601_pon0_get_link_ksettings(struct net_device *dev,
					  struct ethtool_link_ksettings *cmd)
{
	struct gc1601_pon *pon = *(struct gc1601_pon **)netdev_priv(dev);

	ethtool_link_ksettings_zero_link_mode(cmd, supported);
	ethtool_link_ksettings_add_link_mode(cmd, supported, FIBRE);
	ethtool_link_ksettings_zero_link_mode(cmd, advertising);
	cmd->base.port = PORT_FIBRE;
	cmd->base.autoneg = AUTONEG_DISABLE;
	if (!netif_carrier_ok(dev)) {
		cmd->base.speed = SPEED_UNKNOWN;
		cmd->base.duplex = DUPLEX_UNKNOWN;
		return 0;
	}
	cmd->base.speed = READ_ONCE(pon->mode) == GC1601_PON_EPON ? SPEED_1000 : SPEED_2500;
	cmd->base.duplex = DUPLEX_FULL;
	return 0;
}

static const struct ethtool_ops gc1601_pon0_ethtool_ops = {
	.get_link = ethtool_op_get_link,
	.get_link_ksettings = gc1601_pon0_get_link_ksettings,
};

static int gc1601_pon0_register(struct gc1601_pon *pon)
{
	struct net_device *dev;
	int ret;

	dev = alloc_etherdev(sizeof(pon));
	if (!dev)
		return -ENOMEM;
	*(struct gc1601_pon **)netdev_priv(dev) = pon;
	strscpy(dev->name, "pon%d", sizeof(dev->name));
	dev->netdev_ops = &gc1601_pon0_ops;
	dev->ethtool_ops = &gc1601_pon0_ethtool_ops;
	SET_NETDEV_DEV(dev, pon->dev);
	eth_hw_addr_random(dev);
	netif_carrier_off(dev);

	ret = register_netdev(dev);
	if (ret) {
		free_netdev(dev);
		return ret;
	}
	pon->xp.netdev = dev;

	ret = gc1601_pe_pon_register(dev, gc1601_pon0_rx, pon);
	if (ret) {
		unregister_netdev(dev);
		free_netdev(dev);
		pon->xp.netdev = NULL;
	}
	return ret;
}

static void gc1601_pon0_unregister(struct gc1601_pon *pon)
{
	if (!pon->xp.netdev)
		return;
	gc1601_pe_pon_unregister();
	unregister_netdev(pon->xp.netdev);
	free_netdev(pon->xp.netdev);
	pon->xp.netdev = NULL;
}

/* ---- reports from the activation machine ---- */

void gc1601_xpon_report_state(struct gc1601_pon *pon, int state)
{
	enum xpon_registration_state reg;

	if (!pon->xp.odev)
		return;
	omci_device_set_state(pon->xp.odev, state);
	if (state == GPON_STATE_O5) {
		netif_carrier_on(pon->xp.netdev);
		/* Re-ranging may have moved the Alloc-IDs to other T-CONTs. */
		WRITE_ONCE(pon->xp.sfu_count, U32_MAX);
		schedule_work(&pon->xp.reconcile_work);
	} else {
		netif_carrier_off(pon->xp.netdev);
	}
	gc1601_xpon_sfu_update(&pon->xp);
	xpon_device_report_carrier(pon->xp.xpon, state == GPON_STATE_O5);

	switch (state) {
	case 2:
		reg = XPON_REGISTRATION_DISCOVERY;
		break;
	case 3:
	case 4:
	case 6:
		reg = XPON_REGISTRATION_REGISTERING;
		break;
	case GPON_STATE_O5:
		reg = XPON_REGISTRATION_OPERATIONAL;
		break;
	default:
		reg = XPON_REGISTRATION_DOWN;
		break;
	}
	xpon_device_report_registration(pon->xp.xpon, reg);
}

void gc1601_xpon_report_signal(struct gc1601_pon *pon, bool los, bool lof)
{
	if (pon->xp.xpon)
		xpon_device_report_optical(pon->xp.xpon, !los, los || lof);
}

void gc1601_xpon_report_onu_id(struct gc1601_pon *pon, u8 onu_id)
{
	if (pon->xp.odev)
		omci_device_set_onu_id(pon->xp.odev, onu_id);
}

void gc1601_xpon_report_channel(struct gc1601_pon *pon, u16 port, bool valid)
{
	if (pon->xp.odev)
		omci_device_set_channel(pon->xp.odev, port, valid);
}

void gc1601_xpon_reset_session(struct gc1601_pon *pon)
{
	if (pon->xp.odev)
		omci_device_reset_session(pon->xp.odev);
}

int gc1601_xpon_start(struct gc1601_pon *pon)
{
	int ret;

	if (!pon->xp.odev || pon->xp.started)
		return 0;
	ret = omci_device_start(pon->xp.odev);
	if (!ret)
		pon->xp.started = true;
	return ret;
}

void gc1601_xpon_stop(struct gc1601_pon *pon)
{
	if (!pon->xp.started)
		return;
	pon->xp.started = false;
	cancel_work_sync(&pon->xp.reconcile_work);
	omci_device_stop(pon->xp.odev);
}

/*
 * Default identity: the vendor ID from the device tree plus the last four
 * bytes of the factory MAC, as the stock firmware derived it. omci-agent's
 * UCI values replace it at runtime.
 */
static int gc1601_xpon_default_identity(struct gc1601_pon *pon,
					struct omci_identity *id)
{
	u8 mac[ETH_ALEN];
	int ret;

	ret = omci_identity_load(pon->dev, id);
	if (ret == -EPROBE_DEFER)
		return ret;
	if (ret)
		memset(id, 0, sizeof(*id));
	if ((id->valid & OMCI_IDENTITY_F_SERIAL_NUMBER) ||
	    !(id->valid & OMCI_IDENTITY_F_VENDOR_ID))
		return 0;

	/* The MAC sits in the misc partition, which may not be up yet. */
	ret = of_get_mac_address(pon->dev->of_node, mac);
	if (ret == -EPROBE_DEFER)
		return ret;
	if (!ret) {
		memcpy(id->serial_number, id->vendor_id, 4);
		memcpy(id->serial_number + 4, mac + 2, 4);
		id->valid |= OMCI_IDENTITY_F_SERIAL_NUMBER;
		id->serial_source = OMCI_CONFIG_SOURCE_DRIVER;
	}
	return 0;
}

/*
 * The switch tags and untags the services itself; a VLAN device on pon0 in
 * a bridge tags their frames a second time. omci-agent replaces it with
 * pon0 and untag_vlan, but say so if one is bridged anyway.
 */
static int gc1601_xpon_netdev_event(struct notifier_block *nb,
				    unsigned long event, void *ptr)
{
	struct gc1601_xpon *xp = container_of(nb, struct gc1601_xpon, netdev_nb);
	struct netdev_notifier_changeupper_info *info = ptr;
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

	if (event == NETDEV_CHANGEUPPER && info->linking && is_vlan_dev(dev) &&
	    vlan_dev_real_dev(dev) == xp->netdev &&
	    netif_is_bridge_master(info->upper_dev))
		netdev_warn(dev, "bridged on %s: its frames get the VLAN twice; bridge %s and use sfu_untag_vlan %u\n",
			    netdev_name(info->upper_dev), netdev_name(xp->netdev),
			    vlan_dev_vlan_id(dev));
	return NOTIFY_DONE;
}

static int gc1601_xpon_set_mode(struct xpon_device *xpon, enum xpon_mode mode)
{
	struct gc1601_pon *pon = xpon_device_priv(xpon);

	int ret;

	switch (mode) {
	case XPON_MODE_GPON:
		ret = gc1601_pon_set_mode(pon, GC1601_PON_GPON);
		break;
	case XPON_MODE_EPON:
		ret = gc1601_pon_set_mode(pon, GC1601_PON_EPON);
		break;
	default:
		return -EOPNOTSUPP;
	}
	/* The LAN side VLANs come from OMCI services or from the CTC Port VLAN. */
	pon->xp.sfu_count = U32_MAX;
	gc1601_xpon_sfu_update(&pon->xp);
	return ret;
}

static const struct xpon_device_ops gc1601_xpon_ops = {
	.set_mode = gc1601_xpon_set_mode,
};

int gc1601_xpon_init(struct gc1601_pon *pon)
{
	struct xpon_device_desc desc = {
		.modes = XPON_MODE_CAP(XPON_MODE_GPON) |
			 (pon->emac ? XPON_MODE_CAP(XPON_MODE_EPON) : 0),
		.mode = XPON_MODE_GPON,
		.ops = &gc1601_xpon_ops,
		.priv = pon,
	};
	struct omci_identity id;
	int i, ret;

	ret = gc1601_xpon_default_identity(pon, &id);
	if (ret)
		return ret;

	for (i = 0; i < GPON_TCONTS; i++)
		pon->xp.tcont_alloc[i] = U16_MAX;
	pon->xp.olt_profile = -1;
	spin_lock_init(&pon->xp.service_lock);
	INIT_DELAYED_WORK(&pon->xp.sfu_work, gc1601_xpon_sfu_work);
	INIT_WORK(&pon->xp.reconcile_work, gc1601_xpon_reconcile_work);

	ret = gc1601_pon0_register(pon);
	if (ret)
		return dev_err_probe(pon->dev, ret, "pon netdev registration failed\n");

	desc.netdev = pon->xp.netdev;
	pon->xp.xpon = xpon_device_register(pon->dev, &desc);
	if (IS_ERR(pon->xp.xpon)) {
		ret = PTR_ERR(pon->xp.xpon);
		pon->xp.xpon = NULL;
		gc1601_pon0_unregister(pon);
		return dev_err_probe(pon->dev, ret, "xPON registration failed\n");
	}

	pon->xp.odev = omci_device_register(pon->xp.xpon,
					    OMCI_CAP_HW_MIC | OMCI_CAP_TELEMETRY,
					    &gc1601_xpon_omci_ops, pon);
	if (IS_ERR(pon->xp.odev)) {
		ret = PTR_ERR(pon->xp.odev);
		pon->xp.odev = NULL;
		xpon_device_unregister(pon->xp.xpon);
		pon->xp.xpon = NULL;
		gc1601_pon0_unregister(pon);
		return dev_err_probe(pon->dev, ret, "OMCI registration failed\n");
	}

	if (id.valid & OMCI_IDENTITY_F_SERIAL_NUMBER) {
		omci_device_set_identity_info(pon->xp.odev, &id);
		gc1601_gpon_set_identity(pon, id.serial_number,
					 id.valid & OMCI_IDENTITY_F_PASSWORD ? id.password : NULL);
	}
	pon->xp.netdev_nb.notifier_call = gc1601_xpon_netdev_event;
	if (register_netdevice_notifier(&pon->xp.netdev_nb))
		pon->xp.netdev_nb.notifier_call = NULL;
	return 0;
}

void gc1601_xpon_exit(struct gc1601_pon *pon)
{
	if (pon->xp.netdev_nb.notifier_call)
		unregister_netdevice_notifier(&pon->xp.netdev_nb);
	gc1601_xpon_stop(pon);
	cancel_work_sync(&pon->xp.reconcile_work);
	cancel_delayed_work_sync(&pon->xp.sfu_work);
	if (pon->xp.odev)
		omci_device_unregister(pon->xp.odev);
	if (pon->xp.xpon)
		xpon_device_unregister(pon->xp.xpon);
	pon->xp.odev = NULL;
	pon->xp.xpon = NULL;
	gc1601_pon0_unregister(pon);
}
