// SPDX-License-Identifier: GPL-2.0-only
/*
 * The omcid backend: net/xpon's OMCI device on the Luna GPON MAC.
 *
 * PLOAM stays in luna_gpon.c. omcid owns the MIB and asks for T-CONTs, GEM
 * ports and services, which this file binds through luna_xpon_hw.h. The
 * services ride gpon0 through the CPU: upstream frames choose their GEM flow
 * by VLAN and get the service's network tag, as on the GC1601.
 */
#include <linux/ctype.h>
#include <linux/if_vlan.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/skbuff.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <net/xpon.h>
#include <net/xpon/omci.h>

#include "luna_gpon_nic.h"
#include "luna_xpon_hw.h"

#define GPON_O5		5
#define OMCI_PDU_LEN	48
#define LUNA_XPON_SERVICES	16
#define LUNA_XPON_UNIS		4
#define LUNA_XPON_VLAN_MAPS	8

static bool omcid = true;
module_param(omcid, bool, 0444);
MODULE_PARM_DESC(omcid, "Let a board whose device tree asks for it run omcid (0: the in-kernel responder everywhere)");

struct luna_xpon_tcont {
	u16 entity;
	u16 alloc;
	u8 qid;
	bool hw;	/* a data T-CONT of ours, not the OMCC's */
	bool valid;
};

struct luna_xpon_gem {
	u16 entity;
	u16 gem;
	u16 tcont;
	u8 dir;
	bool valid;
};

struct luna_xpon_service {
	u32 cookie;
	u16 gem;
	u16 vlan;
	u16 uni_vid;
	u8 flow;
	u8 pcp;
	u8 uni_mode;
	bool vlan_valid;
	bool pcp_valid;
	bool default_service;
	bool multicast;
	bool used;
};

static struct {
	struct xpon_device *xpon;
	struct omci_device *odev;
	struct net_device *wan;
	void *cookie;
	int (*tx)(void *, const u8 *, unsigned int);
	int (*uni_admin_set)(void *, unsigned int, bool);
	bool (*uni_link)(void *, unsigned int);
	struct mutex lock;
	struct luna_xpon_tcont tcont[LUNA_XPON_TCONTS];
	struct luna_xpon_gem gem[LUNA_XPON_GEMS];
	spinlock_t svc_lock;
	struct luna_xpon_service svc[LUNA_XPON_SERVICES];
	u16 uni_entity[LUNA_XPON_UNIS];
	u8 uni_port[LUNA_XPON_UNIS];
	unsigned int unis;
	/* omci-agent's LAN-side VLANs, ahead of the OLT's class 171 rules */
	u16 untag_vlan;
	struct luna_xpon_vlan_map {
		u16 wan;
		u16 lan;
		u8 mode;
	} vlan_map[LUNA_XPON_VLAN_MAPS];
	unsigned int vlan_maps;
	struct device *dev;
	struct work_struct reconcile_work;
	/* activation events waiting for process context */
	struct work_struct event_work;
	spinlock_t ev_lock;
	unsigned int ev;
	u8 ev_state;
	u8 ev_onu_id;
	u16 ev_gem;
	bool data_up;
} luna_xpon = {
	.lock = __MUTEX_INITIALIZER(luna_xpon.lock),
	.svc_lock = __SPIN_LOCK_UNLOCKED(luna_xpon.svc_lock),
	.ev_lock = __SPIN_LOCK_UNLOCKED(luna_xpon.ev_lock),
};

/* A board opts in with realtek,omci-userspace on its GPON node; the others
 * keep the in-kernel responder their images are built around. */
bool luna_xpon_wanted(void)
{
	struct device_node *np;
	bool wanted;

	if (!omcid)
		return false;
	np = of_find_node_with_property(NULL, "realtek,omci-userspace");
	wanted = np != NULL;
	of_node_put(np);
	return wanted;
}
EXPORT_SYMBOL(luna_xpon_wanted);

bool luna_xpon_active(void)
{
	return READ_ONCE(luna_xpon.odev) != NULL;
}
EXPORT_SYMBOL(luna_xpon_active);

bool luna_xpon_data_ready(void)
{
	return READ_ONCE(luna_xpon.data_up) && luna_xpon_hw_state() == GPON_O5;
}
EXPORT_SYMBOL(luna_xpon_data_ready);

static void luna_xpon_update_data(void)
{
	bool up = false;
	unsigned int i;

	for (i = 0; i < LUNA_XPON_GEMS; i++)
		up |= luna_xpon.gem[i].valid &&
		      (luna_xpon.gem[i].dir & OMCI_GEM_PORT_DIRECTION_UNI_TO_ANI);
	WRITE_ONCE(luna_xpon.data_up, up);
}

/* An OMCC install scrubs the CAMs: what we bound before it is gone, and omcid
 * replays its MIB on the channel event that follows. */
static void luna_xpon_forget(void)
{
	memset(luna_xpon.tcont, 0, sizeof(luna_xpon.tcont));
	memset(luna_xpon.gem, 0, sizeof(luna_xpon.gem));
	WRITE_ONCE(luna_xpon.data_up, false);
}

/* ---- OMCI device operations ---- */

static int luna_xpon_backend_begin(struct omci_device *odev)
{
	return mutex_trylock(&luna_xpon.lock) ? 0 : -EBUSY;
}

static void luna_xpon_backend_end(struct omci_device *odev)
{
	mutex_unlock(&luna_xpon.lock);
}

static int luna_xpon_backend_identity(struct omci_device *odev,
				      const struct omci_identity *id)
{
	luna_xpon_hw_set_identity(id->valid & OMCI_IDENTITY_F_SERIAL_NUMBER ?
				  id->serial_number : NULL,
				  id->valid & OMCI_IDENTITY_F_PASSWORD ?
				  id->password : NULL);
	return 0;
}

static int luna_xpon_backend_reset(struct omci_device *odev)
{
	unsigned int i;

	for (i = 0; i < LUNA_XPON_GEMS; i++)
		if (luna_xpon.gem[i].valid)
			luna_xpon_hw_gem_remove(i);
	for (i = 0; i < LUNA_XPON_TCONTS; i++)
		if (luna_xpon.tcont[i].valid && luna_xpon.tcont[i].hw)
			luna_xpon_hw_tcont_remove(i);
	luna_xpon_forget();

	spin_lock_bh(&luna_xpon.svc_lock);
	memset(luna_xpon.svc, 0, sizeof(luna_xpon.svc));
	spin_unlock_bh(&luna_xpon.svc_lock);
	return 0;
}

static int luna_xpon_start(struct omci_device *odev)
{
	return 0;
}

static void luna_xpon_stop(struct omci_device *odev)
{
}

static int luna_xpon_xmit(struct omci_device *odev, struct sk_buff *skb,
			  u16 gem_port_id)
{
	int rc;

	if (gem_port_id != luna_xpon_hw_omcc_gem() ||
	    luna_xpon_hw_state() != GPON_O5)
		return -ENOLINK;
	rc = luna_xpon.tx(luna_xpon.cookie, skb->data, skb->len);
	if (!rc)
		dev_kfree_skb_any(skb);
	return rc;
}

static int luna_xpon_get_ani_topology(struct omci_device *odev,
				      struct omci_ani_topology *t)
{
	*t = (struct omci_ani_topology) {
		.tcont_base = 0x8000,
		.scheduler_base = 0x8000,
		.queue_base = 0x8000,
		/* stock's own Priority Queue sizes */
		.maximum_queue_size = 3276,
		.allocated_queue_size = 3276,
		.tcont_count = LUNA_XPON_TCONTS,
		.queues_per_tcont = 8,
		.queue_config_option = 1,
		.scheduler_policy = 2,
	};
	return 0;
}

static struct luna_xpon_tcont *luna_xpon_tcont_find(u16 entity)
{
	unsigned int i;

	for (i = 0; i < LUNA_XPON_TCONTS; i++)
		if (luna_xpon.tcont[i].valid && luna_xpon.tcont[i].entity == entity)
			return &luna_xpon.tcont[i];
	return NULL;
}

static int luna_xpon_set_tcont(struct omci_device *odev, u16 entity,
			       u16 alloc, bool valid)
{
	struct luna_xpon_tcont *t = luna_xpon_tcont_find(entity);
	bool omcc = alloc == luna_xpon_hw_onu_id();
	unsigned int i;
	int rc;

	if (t && valid && t->alloc == alloc)
		return 0;
	if (t) {
		if (t->hw)
			luna_xpon_hw_tcont_remove(t - luna_xpon.tcont);
		t->valid = false;
	}
	if (!valid)
		return 0;
	/* Only an Alloc-ID the OLT assigned over PLOAM gets grants; omcid keeps
	 * the service pending until it does. The ONU-ID's own Alloc-ID is the
	 * OMCC's T-CONT, and data on it shares the OMCC queue. */
	if (!omcc && !luna_xpon_hw_alloc_known(alloc))
		return -EAGAIN;

	for (i = 0; i < LUNA_XPON_TCONTS; i++) {
		if (luna_xpon.tcont[i].valid)
			continue;
		if (!omcc) {
			rc = luna_xpon_hw_tcont_install(i, alloc);
			if (rc)
				return rc;
		}
		luna_xpon.tcont[i] = (struct luna_xpon_tcont) {
			.entity = entity,
			.alloc = alloc,
			.qid = omcc ? luna_xpon_hw_omcc_qid() : luna_xpon_hw_tcont_qid(i),
			.hw = !omcc,
			.valid = true,
		};
		return 0;
	}
	return -ENOSPC;
}

static int luna_xpon_set_gem_port(struct omci_device *odev, u16 entity,
				  u16 gem, u16 tcont_entity, u8 dir,
				  bool valid, bool encrypted)
{
	struct luna_xpon_tcont *t = NULL;
	unsigned int i, slot = LUNA_XPON_GEMS;
	int rc;

	if (gem > 0xfff || !dir || dir > OMCI_GEM_PORT_DIRECTION_BIDIRECTIONAL)
		return -EINVAL;
	if (valid && gem == luna_xpon_hw_omcc_gem())
		return -EBUSY;

	for (i = 0; i < LUNA_XPON_GEMS; i++) {
		if (!luna_xpon.gem[i].valid) {
			if (slot == LUNA_XPON_GEMS)
				slot = i;
			continue;
		}
		if (luna_xpon.gem[i].entity == entity)
			break;
		if (valid && luna_xpon.gem[i].gem == gem)
			return -EEXIST;
	}
	if (i < LUNA_XPON_GEMS) {
		struct luna_xpon_gem *g = &luna_xpon.gem[i];

		if (valid && g->gem == gem && g->tcont == tcont_entity && g->dir == dir)
			return 0;
		luna_xpon_hw_gem_remove(i);
		g->valid = false;
		slot = i;
	}
	if (!valid) {
		luna_xpon_update_data();
		return 0;
	}
	if (slot == LUNA_XPON_GEMS)
		return -ENOSPC;
	if (dir & OMCI_GEM_PORT_DIRECTION_UNI_TO_ANI) {
		t = luna_xpon_tcont_find(tcont_entity);
		if (!t)
			return -EAGAIN;
	}
	/* Downstream decryption follows the PLOAM key; nothing to arm per GEM. */
	(void)encrypted;

	rc = luna_xpon_hw_gem_install(slot, gem, t ? t->qid : -1);
	if (rc)
		return rc;
	luna_xpon.gem[slot] = (struct luna_xpon_gem) {
		.entity = entity,
		.gem = gem,
		.tcont = tcont_entity,
		.dir = dir,
		.valid = true,
	};
	luna_xpon_update_data();
	return 0;
}

static int luna_xpon_uni_port(u16 entity)
{
	unsigned int i;

	for (i = 0; i < luna_xpon.unis; i++)
		if (luna_xpon.uni_entity[i] == entity)
			return luna_xpon.uni_port[i];
	return -ENOENT;
}

static int luna_xpon_set_uni(struct omci_device *odev, u16 entity, bool enable)
{
	int port = luna_xpon_uni_port(entity);

	/* A VEIP has no port of its own. */
	if (port < 0 || !luna_xpon.uni_admin_set)
		return 0;
	return luna_xpon.uni_admin_set(luna_xpon.cookie, port, !enable);
}

static int luna_xpon_get_uni_link(struct omci_device *odev, u16 entity,
				  bool *link_up)
{
	int port = luna_xpon_uni_port(entity);

	if (port < 0 || !luna_xpon.uni_link)
		return -ENOENT;
	*link_up = luna_xpon.uni_link(luna_xpon.cookie, port);
	return 0;
}

static int luna_xpon_gem_flow(u16 gem)
{
	unsigned int i;

	for (i = 0; i < LUNA_XPON_GEMS; i++)
		if (luna_xpon.gem[i].valid && luna_xpon.gem[i].gem == gem)
			return luna_xpon_hw_gem_flow(i);
	return -ENOENT;
}

static int luna_xpon_replace_service(struct omci_device *odev,
				     const struct omci_service_config *s)
{
	struct luna_xpon_service *svc = NULL;
	int flow = luna_xpon_gem_flow(s->gem_port_id);
	unsigned int i;

	if (!s->multicast && flow < 0)
		return -EAGAIN;

	spin_lock_bh(&luna_xpon.svc_lock);
	for (i = 0; i < LUNA_XPON_SERVICES; i++) {
		if (luna_xpon.svc[i].used && luna_xpon.svc[i].cookie == s->cookie) {
			svc = &luna_xpon.svc[i];
			break;
		}
		if (!svc && !luna_xpon.svc[i].used)
			svc = &luna_xpon.svc[i];
	}
	if (svc)
		*svc = (struct luna_xpon_service) {
			.cookie = s->cookie,
			.gem = s->gem_port_id,
			.vlan = s->vlan_id,
			.uni_vid = s->uni_vlan_id,
			.flow = flow < 0 ? 0 : flow,
			.pcp = s->pcp & 7,
			.uni_mode = s->uni_vlan_mode,
			.vlan_valid = s->vlan_valid,
			.pcp_valid = s->pcp_valid,
			.default_service = s->default_service,
			.multicast = s->multicast,
			.used = true,
		};
	spin_unlock_bh(&luna_xpon.svc_lock);
	if (!svc)
		return -ENOSPC;
	pr_info("luna-xpon: service %u: GEM %u flow %d VLAN %u%s\n", s->cookie,
		s->gem_port_id, flow, s->vlan_valid ? s->vlan_id : 0,
		s->multicast ? " multicast" : "");
	return 0;
}

static int luna_xpon_delete_service(struct omci_device *odev, u32 cookie)
{
	unsigned int i;

	spin_lock_bh(&luna_xpon.svc_lock);
	for (i = 0; i < LUNA_XPON_SERVICES; i++)
		if (luna_xpon.svc[i].used && luna_xpon.svc[i].cookie == cookie)
			luna_xpon.svc[i].used = false;
	spin_unlock_bh(&luna_xpon.svc_lock);
	return 0;
}

static int luna_xpon_get_telemetry(struct omci_device *odev,
				   struct omci_telemetry *t)
{
	struct luna_xpon_optics o;
	int rc = luna_xpon_hw_optics(&o);

	if (rc)
		return rc;
	t->bosa_temperature_mc = o.temp_mc;
	t->bosa_voltage_uv = o.vcc_uv;
	t->bosa_bias_ua = o.bias_ua;
	t->bosa_tx_power_nw = o.tx_nw;
	t->bosa_rx_power_nw = o.rx_nw;
	t->valid |= OMCI_TELEMETRY_F_BOSA_TEMPERATURE |
		    OMCI_TELEMETRY_F_BOSA_VOLTAGE |
		    OMCI_TELEMETRY_F_BOSA_BIAS |
		    OMCI_TELEMETRY_F_BOSA_TX_POWER |
		    OMCI_TELEMETRY_F_BOSA_RX_POWER;
	return 0;
}

static void luna_xpon_set_operational(struct omci_device *odev, bool operational)
{
	if (operational && luna_xpon_hw_state() == GPON_O5)
		netif_carrier_on(luna_xpon.wan);
	else
		netif_carrier_off(luna_xpon.wan);
}

static const struct omci_device_ops luna_xpon_ops = {
	.backend_begin		= luna_xpon_backend_begin,
	.backend_end		= luna_xpon_backend_end,
	.backend_identity	= luna_xpon_backend_identity,
	.backend_reset		= luna_xpon_backend_reset,
	.start			= luna_xpon_start,
	.stop			= luna_xpon_stop,
	.xmit			= luna_xpon_xmit,
	.get_ani_topology	= luna_xpon_get_ani_topology,
	.set_tcont		= luna_xpon_set_tcont,
	.set_gem_port		= luna_xpon_set_gem_port,
	.set_uni		= luna_xpon_set_uni,
	.replace_service	= luna_xpon_replace_service,
	.delete_service		= luna_xpon_delete_service,
	.get_telemetry		= luna_xpon_get_telemetry,
	.get_uni_link		= luna_xpon_get_uni_link,
	.set_operational	= luna_xpon_set_operational,
};

/* ---- reports from the activation machine ---- */

static void luna_xpon_reconcile_fn(struct work_struct *work)
{
	struct omci_device *odev = READ_ONCE(luna_xpon.odev);

	if (odev && luna_xpon_hw_state() == GPON_O5)
		omci_device_reconcile_services(odev);
}

/*
 * The activation machine reports from its timer and its PLOAM worker, under
 * spinlocks, while the OMCI device's notifications read the optics over I2C
 * and sleep. Events are recorded here and delivered from process context.
 */
#define LUNA_XPON_EV_STATE	BIT(0)
#define LUNA_XPON_EV_ONU_ID	BIT(1)
#define LUNA_XPON_EV_CHANNEL	BIT(2)

static void luna_xpon_post(unsigned int ev, u8 state, u8 onu_id, u16 gem)
{
	unsigned long flags;

	if (!READ_ONCE(luna_xpon.odev))
		return;
	spin_lock_irqsave(&luna_xpon.ev_lock, flags);
	luna_xpon.ev |= ev;
	if (ev & LUNA_XPON_EV_STATE)
		luna_xpon.ev_state = state;
	if (ev & LUNA_XPON_EV_ONU_ID)
		luna_xpon.ev_onu_id = onu_id;
	if (ev & LUNA_XPON_EV_CHANNEL)
		luna_xpon.ev_gem = gem;
	spin_unlock_irqrestore(&luna_xpon.ev_lock, flags);
	schedule_work(&luna_xpon.event_work);
}

void luna_xpon_report_state(u8 state)
{
	luna_xpon_post(LUNA_XPON_EV_STATE, state, 0, 0);
}

void luna_xpon_report_onu_id(u8 onu_id)
{
	luna_xpon_post(LUNA_XPON_EV_ONU_ID, 0, onu_id, 0);
}

void luna_xpon_report_channel(u16 gem)
{
	luna_xpon_post(LUNA_XPON_EV_CHANNEL, 0, 0, gem);
}

static void luna_xpon_deliver_state(struct omci_device *odev, u8 state)
{
	enum xpon_registration_state reg;

	omci_device_set_state(odev, state);
	if (state == GPON_O5)
		schedule_work(&luna_xpon.reconcile_work);
	else
		netif_carrier_off(luna_xpon.wan);
	xpon_device_report_carrier(luna_xpon.xpon, state == GPON_O5);
	switch (state) {
	case 2:
		reg = XPON_REGISTRATION_DISCOVERY;
		break;
	case 3:
	case 4:
	case 6:
		reg = XPON_REGISTRATION_REGISTERING;
		break;
	case GPON_O5:
		reg = XPON_REGISTRATION_OPERATIONAL;
		break;
	default:
		reg = XPON_REGISTRATION_DOWN;
		break;
	}
	xpon_device_report_registration(luna_xpon.xpon, reg);
}

static void luna_xpon_event_fn(struct work_struct *work)
{
	struct omci_device *odev = READ_ONCE(luna_xpon.odev);
	unsigned int ev;
	u8 state, onu_id;
	u16 gem;

	spin_lock_irq(&luna_xpon.ev_lock);
	ev = luna_xpon.ev;
	luna_xpon.ev = 0;
	state = luna_xpon.ev_state;
	onu_id = luna_xpon.ev_onu_id;
	gem = luna_xpon.ev_gem;
	spin_unlock_irq(&luna_xpon.ev_lock);
	if (!odev)
		return;

	if (ev & LUNA_XPON_EV_ONU_ID)
		omci_device_set_onu_id(odev, onu_id);
	if (ev & LUNA_XPON_EV_CHANNEL) {
		/* The OMCC install scrubbed the CAMs: what we bound is gone. */
		mutex_lock(&luna_xpon.lock);
		luna_xpon_forget();
		mutex_unlock(&luna_xpon.lock);
		omci_device_set_channel(odev, gem, true);
	}
	if (ev & LUNA_XPON_EV_STATE)
		luna_xpon_deliver_state(odev, state);
}

void luna_xpon_report_alloc(void)
{
	if (READ_ONCE(luna_xpon.odev))
		schedule_work(&luna_xpon.reconcile_work);
}

/* ---- the OMCI transport and gpon0 ---- */

void luna_xpon_omci_rx(const u8 *msg, unsigned int len)
{
	struct omci_device *odev = READ_ONCE(luna_xpon.odev);
	struct sk_buff *skb;
	int gem = luna_xpon_hw_omcc_gem();
	u32 flags = 0;

	if (!odev || gem < 0)
		return;
	if (len >= OMCI_PDU_LEN) {
		len = OMCI_PDU_LEN;
		flags = OMCI_F_MIC_PRESENT;
		if (luna_xpon_hw_mic_valid(msg, len))
			flags |= OMCI_F_MIC_VALID;
	}
	skb = alloc_skb(len, GFP_ATOMIC);
	if (!skb)
		return;
	skb_put_data(skb, msg, len);
	omci_device_receive(odev, skb, gem, flags);
}
EXPORT_SYMBOL(luna_xpon_omci_rx);

static int luna_xpon_rank(const struct luna_xpon_service *s)
{
	if (s->default_service)
		return 0;
	return s->pcp_valid ? 2 + s->pcp : 1;
}

/* How a service's VLAN looks on the LAN side: the untagged VLAN and the
 * manual map, written by omci-agent, win over the class 171 rule. Called
 * with svc_lock held. */
static u8 luna_xpon_lan_mode(const struct luna_xpon_service *s, u16 *uni_vid)
{
	u8 mode = s->uni_mode;
	unsigned int i;

	*uni_vid = s->uni_vid;
	for (i = 0; i < luna_xpon.vlan_maps; i++) {
		if (luna_xpon.vlan_map[i].wan == s->vlan) {
			mode = luna_xpon.vlan_map[i].mode;
			*uni_vid = luna_xpon.vlan_map[i].lan;
		}
	}
	if (luna_xpon.untag_vlan && s->vlan == luna_xpon.untag_vlan)
		mode = OMCI_UNI_VLAN_UNTAGGED;
	return mode;
}

/* The VLAN a service's frames carry on the LAN side. */
static u16 luna_xpon_lan_vid(const struct luna_xpon_service *s, u8 mode,
			     u16 uni_vid)
{
	return mode == OMCI_UNI_VLAN_TRANSLATE || mode == OMCI_UNI_VLAN_STACKED ?
	       uni_vid : s->vlan;
}

/*
 * The upstream flow for a frame, after giving it the service's network tag:
 * a frame the LAN sends untagged takes the VLAN of the service whose UNI side
 * is untagged. Returns the flow or a negative errno; the skb may be replaced,
 * and is freed when vlan_insert_tag fails.
 */
int luna_xpon_tx_flow(struct sk_buff **pskb)
{
	const struct luna_xpon_service *svc = NULL, *s;
	struct sk_buff *skb = *pskb;
	u16 tci = 0, vid, uni_vid;
	u8 mode, svc_mode = OMCI_UNI_VLAN_SAME;
	bool tagged;
	unsigned int i;
	int flow = -ENOENT;

	tagged = !__vlan_get_tag(skb, &tci);
	vid = tci & VLAN_VID_MASK;

	spin_lock_bh(&luna_xpon.svc_lock);
	for (i = 0; i < LUNA_XPON_SERVICES; i++) {
		s = &luna_xpon.svc[i];
		if (!s->used || s->multicast)
			continue;
		mode = luna_xpon_lan_mode(s, &uni_vid);
		if (tagged) {
			if (!s->vlan_valid || mode == OMCI_UNI_VLAN_UNTAGGED ||
			    luna_xpon_lan_vid(s, mode, uni_vid) != vid)
				continue;
			if (s->pcp_valid && s->pcp == tci >> VLAN_PRIO_SHIFT) {
				svc = s;
				svc_mode = mode;
				break;
			}
		} else if (s->vlan_valid && mode != OMCI_UNI_VLAN_UNTAGGED) {
			continue;
		}
		if (!svc || luna_xpon_rank(s) < luna_xpon_rank(svc)) {
			svc = s;
			svc_mode = mode;
		}
	}
	if (svc)
		flow = svc->flow;
	if (svc && svc->vlan_valid) {
		if (!tagged || svc_mode == OMCI_UNI_VLAN_STACKED) {
			u16 net = svc->vlan;

			if (svc_mode == OMCI_UNI_VLAN_STACKED)
				net |= tci & VLAN_PRIO_MASK;
			else if (svc->pcp_valid)
				net |= svc->pcp << VLAN_PRIO_SHIFT;
			skb = vlan_insert_tag_set_proto(skb, htons(ETH_P_8021Q), net);
			if (!skb)
				flow = -ENOMEM;
		} else if (svc_mode == OMCI_UNI_VLAN_TRANSLATE) {
			if (skb_ensure_writable(skb, VLAN_ETH_HLEN))
				flow = -ENOMEM;
			else
				put_unaligned_be16((tci & ~VLAN_VID_MASK) | svc->vlan,
						   skb->data + ETH_HLEN);
		}
	}
	spin_unlock_bh(&luna_xpon.svc_lock);
	*pskb = skb;
	return flow;
}
EXPORT_SYMBOL(luna_xpon_tx_flow);

/* Downstream: the network tag back to the service's UNI-side form. */
void luna_xpon_rx_fixup(struct sk_buff *skb)
{
	const struct luna_xpon_service *s;
	u8 mode = OMCI_UNI_VLAN_SAME;
	u16 vid, uni_vid = 0;
	unsigned int i;

	if (skb->len < VLAN_ETH_HLEN ||
	    get_unaligned_be16(skb->data + 2 * ETH_ALEN) != ETH_P_8021Q)
		return;
	vid = get_unaligned_be16(skb->data + ETH_HLEN) & VLAN_VID_MASK;

	spin_lock_bh(&luna_xpon.svc_lock);
	for (i = 0; i < LUNA_XPON_SERVICES; i++) {
		s = &luna_xpon.svc[i];
		if (s->used && s->vlan_valid && s->vlan == vid) {
			mode = luna_xpon_lan_mode(s, &uni_vid);
			break;
		}
	}
	spin_unlock_bh(&luna_xpon.svc_lock);

	switch (mode) {
	case OMCI_UNI_VLAN_UNTAGGED:
	case OMCI_UNI_VLAN_STACKED:
		memmove(skb->data + VLAN_HLEN, skb->data, 2 * ETH_ALEN);
		skb_pull(skb, VLAN_HLEN);
		break;
	case OMCI_UNI_VLAN_TRANSLATE:
		put_unaligned_be16((get_unaligned_be16(skb->data + ETH_HLEN) &
				    ~VLAN_VID_MASK) | uni_vid,
				   skb->data + ETH_HLEN);
		break;
	}
}
EXPORT_SYMBOL(luna_xpon_rx_fixup);

/* The board's UNIs as the OLT addresses them, from the omci-uni node. */
static void luna_xpon_load_unis(void)
{
	struct device_node *np = of_find_node_by_name(NULL, "omci-uni");
	int n;

	if (!np)
		return;
	n = of_property_read_variable_u16_array(np, "ethernet-uni-instances",
						luna_xpon.uni_entity, 1,
						LUNA_XPON_UNIS);
	if (n > 0 && !of_property_read_u8_array(np, "ethernet-uni-ports",
						luna_xpon.uni_port, n))
		luna_xpon.unis = n;
	of_node_put(np);
}

/* ---- omci-agent's controls, on the device behind pon0 ---- */

static ssize_t sfu_untag_vlan_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(luna_xpon.untag_vlan));
}

static ssize_t sfu_untag_vlan_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	u16 vid;
	int rc = kstrtou16(buf, 0, &vid);

	if (rc)
		return rc;
	if (vid > 4094)
		return -ERANGE;
	spin_lock_bh(&luna_xpon.svc_lock);
	luna_xpon.untag_vlan = vid;
	spin_unlock_bh(&luna_xpon.svc_lock);
	return count;
}
static DEVICE_ATTR_RW(sfu_untag_vlan);

static ssize_t sfu_vlan_map_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	unsigned int i;

	spin_lock_bh(&luna_xpon.svc_lock);
	for (i = 0; i < luna_xpon.vlan_maps; i++)
		len += sysfs_emit_at(buf, len, "%c%u:%u ",
				     luna_xpon.vlan_map[i].mode == OMCI_UNI_VLAN_STACKED ?
				     's' : 't',
				     luna_xpon.vlan_map[i].wan, luna_xpon.vlan_map[i].lan);
	spin_unlock_bh(&luna_xpon.svc_lock);
	return len + sysfs_emit_at(buf, len, "\n");
}

/* "tWAN:LAN" translates LAN to WAN, "sWAN:LAN" stacks WAN over the LAN tag,
 * separated by spaces. An empty write clears the map. */
static ssize_t sfu_vlan_map_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct luna_xpon_vlan_map map[LUNA_XPON_VLAN_MAPS];
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
		map[n++] = (struct luna_xpon_vlan_map) {
			.wan = wan,
			.lan = lan,
			.mode = mode == 's' ? OMCI_UNI_VLAN_STACKED : OMCI_UNI_VLAN_TRANSLATE,
		};
		p += used;
	}
	spin_lock_bh(&luna_xpon.svc_lock);
	memcpy(luna_xpon.vlan_map, map, n * sizeof(map[0]));
	luna_xpon.vlan_maps = n;
	spin_unlock_bh(&luna_xpon.svc_lock);
	return count;
}
static DEVICE_ATTR_RW(sfu_vlan_map);

/* The laser follows the PLOAM machine here; there is no hold to release. */
static ssize_t allow_tx_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	return sysfs_emit(buf, "1\n");
}

static ssize_t allow_tx_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	bool on;
	int rc = kstrtobool(buf, &on);

	if (rc)
		return rc;
	return on ? count : -EOPNOTSUPP;
}
static DEVICE_ATTR_RW(allow_tx);

static struct attribute *luna_xpon_attrs[] = {
	&dev_attr_sfu_untag_vlan.attr,
	&dev_attr_sfu_vlan_map.attr,
	&dev_attr_allow_tx.attr,
	NULL,
};

static const struct attribute_group luna_xpon_group = {
	.attrs = luna_xpon_attrs,
};

static int luna_xpon_set_mode(struct xpon_device *xpon, enum xpon_mode mode)
{
	return mode == XPON_MODE_GPON ? 0 : -EOPNOTSUPP;
}

static const struct xpon_device_ops luna_xpon_device_ops = {
	.set_mode = luna_xpon_set_mode,
};

int luna_xpon_attach(struct device *dev, struct net_device *wan, void *cookie,
		     int (*tx)(void *, const u8 *, unsigned int),
		     int (*uni_admin_set)(void *, unsigned int, bool),
		     bool (*uni_link)(void *, unsigned int))
{
	struct xpon_device_desc desc = {
		.modes = XPON_MODE_CAP(XPON_MODE_GPON),
		.mode = XPON_MODE_GPON,
		.ops = &luna_xpon_device_ops,
		.netdev = wan,
	};
	struct omci_identity id = { 0 };
	struct omci_device *odev;
	struct xpon_device *xpon;
	int gem, rc;

	if (!omcid)
		return -EOPNOTSUPP;
	if (luna_xpon.odev)
		return -EBUSY;

	INIT_WORK(&luna_xpon.reconcile_work, luna_xpon_reconcile_fn);
	INIT_WORK(&luna_xpon.event_work, luna_xpon_event_fn);
	luna_xpon.wan = wan;
	luna_xpon.cookie = cookie;
	luna_xpon.tx = tx;
	luna_xpon.uni_admin_set = uni_admin_set;
	luna_xpon.uni_link = uni_link;
	luna_xpon_load_unis();
	netif_carrier_off(wan);

	rc = sysfs_create_group(&dev->kobj, &luna_xpon_group);
	if (rc)
		return rc;
	luna_xpon.dev = dev;
	xpon = xpon_device_register(dev, &desc);
	if (IS_ERR(xpon)) {
		sysfs_remove_group(&dev->kobj, &luna_xpon_group);
		return PTR_ERR(xpon);
	}
	odev = omci_device_register(xpon, OMCI_CAP_TELEMETRY, &luna_xpon_ops, NULL);
	if (IS_ERR(odev)) {
		xpon_device_unregister(xpon);
		sysfs_remove_group(&dev->kobj, &luna_xpon_group);
		return PTR_ERR(odev);
	}
	luna_xpon.xpon = xpon;

	/* The registration identity this driver already ranges with. */
	luna_xpon_hw_get_identity(id.serial_number, id.password);
	memcpy(id.vendor_id, id.serial_number, sizeof(id.vendor_id));
	id.valid = OMCI_IDENTITY_F_SERIAL_NUMBER | OMCI_IDENTITY_F_VENDOR_ID |
		   OMCI_IDENTITY_F_PASSWORD;
	omci_device_set_identity_info(odev, &id);
	omci_device_set_identity(odev, id.serial_number, id.password);
	WRITE_ONCE(luna_xpon.odev, odev);

	if (luna_xpon_hw_onu_id() != 0xff)
		luna_xpon_report_onu_id(luna_xpon_hw_onu_id());
	gem = luna_xpon_hw_omcc_gem();
	if (gem >= 0)
		luna_xpon_report_channel(gem);
	luna_xpon_report_state(luna_xpon_hw_state());

	rc = omci_device_start(odev);
	if (rc)
		luna_xpon_detach(cookie);
	return rc;
}
EXPORT_SYMBOL(luna_xpon_attach);

void luna_xpon_detach(void *cookie)
{
	struct omci_device *odev = luna_xpon.odev;

	if (!odev || luna_xpon.cookie != cookie)
		return;
	WRITE_ONCE(luna_xpon.odev, NULL);
	cancel_work_sync(&luna_xpon.event_work);
	cancel_work_sync(&luna_xpon.reconcile_work);
	omci_device_stop(odev);
	omci_device_unregister(odev);
	xpon_device_unregister(luna_xpon.xpon);
	luna_xpon.xpon = NULL;
	sysfs_remove_group(&luna_xpon.dev->kobj, &luna_xpon_group);
	luna_xpon_forget();
}
EXPORT_SYMBOL(luna_xpon_detach);
