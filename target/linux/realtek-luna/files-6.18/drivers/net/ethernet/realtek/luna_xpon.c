// SPDX-License-Identifier: GPL-2.0-only
/*
 * The omcid backend: net/xpon's OMCI device on the Luna GPON MAC.
 *
 * PLOAM stays in luna_gpon.c. omcid owns the MIB and asks for T-CONTs, GEM
 * ports and services, which this file binds through luna_xpon_hw.h. The
 * CPU fallback selects GEM flows and applies VLAN policy; the V2802RH SFU
 * offload worker consumes the same service bindings for hardware forwarding.
 */
#include <linux/ctype.h>
#include <linux/crc32.h>
#include <linux/if_vlan.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/skbuff.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <net/xpon.h>
#include <net/xpon/omci.h>
#include <linux/optical_frontend.h>

#include "luna_gpon_nic.h"
#include "luna_xpon_hw.h"

#define GPON_O5		5
#define OMCI_PDU_LEN	48
#define OMCI_BASELINE_ID	0x0a
#define OMCI_EXTENDED_ID	0x0b
#define OMCI_EXT_HEADER_LEN	10
#define OMCI_MIC_LEN	4
#define LUNA_XPON_SERVICES	64	/* omcid installs one per p-bit and GEM */
#define LUNA_XPON_UNIS		4
#define LUNA_XPON_VLAN_MAPS	8
#define LUNA_XPON_TCONT_BASE	0x8000

static bool omcid = true;
module_param(omcid, bool, 0444);
MODULE_PARM_DESC(omcid, "Let a board whose device tree asks for it run omcid (0: the in-kernel responder everywhere)");

struct luna_xpon_tcont {
	u16 entity;
	u16 alloc;
	u8 qid;
	u32 pool;	/* bank-relative queues taken beyond its own */
	bool hw;	/* a data T-CONT installed in the MAC */
	bool omcc;	/* the ONU-ID's own Alloc-ID: data shares the OMCC queue */
	bool valid;
};

struct luna_xpon_gem {
	u16 entity;
	u16 gem;
	u16 tcont;
	u8 dir;
	u8 qid;		/* the queue its SID drains to */
	bool hw;
	bool valid;
};

/* A bank queue as last programmed. */
struct luna_xpon_queue {
	u32 pir;
	u16 weight;
	bool wfq;
	bool valid;
};

struct luna_xpon_service {
	u16 uni_entity;
	u32 cookie;
	u16 gem;
	u16 vlan;
	u16 uni_vid;
	u8 flow;
	u8 pcp;
	u8 queue;
	u8 uni_mode;
	u8 direction;
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
	int (*uni_rate)(void *, unsigned int, u32, u32);
	struct mutex lock;
	struct luna_xpon_tcont tcont[LUNA_XPON_TCONTS];
	struct luna_xpon_gem gem[LUNA_XPON_GEMS];
	/* The OLT's scheduling survives OMCC reinstalls: omcid replays it in
	 * any order with the T-CONTs and GEMs it applies to. */
	struct omci_tcont_qos tcont_qos[LUNA_XPON_TCONTS];
	struct {
		u16 gem;
		u32 rate;
	} gem_rate[LUNA_XPON_GEMS];
	struct luna_xpon_queue queue[LUNA_XPON_BANK_QUEUES];
	spinlock_t svc_lock;
	struct luna_xpon_service svc[LUNA_XPON_SERVICES];
	u16 uni_entity[LUNA_XPON_UNIS];
	u8 uni_port[LUNA_XPON_UNIS];
	unsigned int unis;
	/* omci-agent's LAN-side VLANs, ahead of the OLT's class 171 rules */
	u16 untag_vlan;
	u8 lan_tagged;
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
	bool ev_los;
	bool los_known;
	unsigned long loss_started;
	unsigned long signal_since;
	bool data_up;
	bool operational;
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
	return READ_ONCE(luna_xpon.data_up) && READ_ONCE(luna_xpon.operational) &&
	       luna_xpon_hw_state() == GPON_O5;
}
EXPORT_SYMBOL(luna_xpon_data_ready);

static void luna_xpon_update_carrier(void)
{
	if (!luna_xpon.wan)
		return;
	if (luna_xpon_data_ready())
		netif_carrier_on(luna_xpon.wan);
	else
		netif_carrier_off(luna_xpon.wan);
}

static void luna_xpon_update_data(void)
{
	bool up = false;
	unsigned int i, j;

	spin_lock_bh(&luna_xpon.svc_lock);
	for (i = 0; i < LUNA_XPON_SERVICES && !up; i++) {
		const struct luna_xpon_service *s = &luna_xpon.svc[i];

		if (!s->used || !(s->direction & OMCI_GEM_PORT_DIRECTION_UNI_TO_ANI))
			continue;
		for (j = 0; j < LUNA_XPON_GEMS; j++)
			if (luna_xpon.gem[j].hw && luna_xpon.gem[j].gem == s->gem &&
			    (luna_xpon.gem[j].dir & OMCI_GEM_PORT_DIRECTION_UNI_TO_ANI)) {
				up = true;
				break;
			}
	}
	spin_unlock_bh(&luna_xpon.svc_lock);
	WRITE_ONCE(luna_xpon.data_up, up);
	rtl9601d_l2_changed();
	luna_xpon_update_carrier();
}

/* An OMCC install scrubs the CAMs: what we bound before it is gone, and omcid
 * replays its MIB on the channel event that follows. */
static void luna_xpon_forget(void)
{
	rtl9601d_l2_changed();
	memset(luna_xpon.tcont, 0, sizeof(luna_xpon.tcont));
	memset(luna_xpon.gem, 0, sizeof(luna_xpon.gem));
	memset(luna_xpon.queue, 0, sizeof(luna_xpon.queue));
	WRITE_ONCE(luna_xpon.data_up, false);
	WRITE_ONCE(luna_xpon.operational, false);
	spin_lock_bh(&luna_xpon.svc_lock);
	memset(luna_xpon.svc, 0, sizeof(luna_xpon.svc));
	spin_unlock_bh(&luna_xpon.svc_lock);
	luna_xpon_update_carrier();
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
	return luna_xpon_hw_set_identity(id->valid & OMCI_IDENTITY_F_SERIAL_NUMBER ?
					 id->serial_number : NULL,
					 id->valid & OMCI_IDENTITY_F_PASSWORD ?
					 id->password : NULL);
}

static int luna_xpon_backend_reset(struct omci_device *odev)
{
	unsigned int i;
	int rc;

	/* A MIB reset restores unlimited UNI rates as well as PON services. */
	if (luna_xpon.uni_rate) {
		for (i = 0; i < luna_xpon.unis; i++) {
			rc = luna_xpon.uni_rate(luna_xpon.cookie,
					       luna_xpon.uni_port[i], 0, 0);
			if (rc)
				return rc;
		}
	}
	for (i = 0; i < LUNA_XPON_GEMS; i++)
		if (luna_xpon.gem[i].hw)
			luna_xpon_hw_gem_remove(i);
	for (i = 0; i < LUNA_XPON_TCONTS; i++)
		if (luna_xpon.tcont[i].hw)
			luna_xpon_hw_tcont_remove(i);
	luna_xpon_forget();
	memset(luna_xpon.tcont_qos, 0, sizeof(luna_xpon.tcont_qos));
	memset(luna_xpon.gem_rate, 0, sizeof(luna_xpon.gem_rate));
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
	unsigned int pdu_len;
	u32 mic;
	int rc;

	if (gem_port_id != luna_xpon_hw_omcc_gem() ||
	    luna_xpon_hw_state() != GPON_O5)
		return -ENOLINK;

	/* The userspace engine supplies the PDU without its CRC. The Luna NIC
	 * consumes a complete OMCI frame, as the in-kernel responder sends.
	 */
	if (skb->len < 4)
		return -EMSGSIZE;
	if (skb->data[3] == OMCI_BASELINE_ID)
		pdu_len = OMCI_PDU_LEN - OMCI_MIC_LEN;
	else if (skb->data[3] == OMCI_EXTENDED_ID && skb->len >= OMCI_EXT_HEADER_LEN)
		pdu_len = OMCI_EXT_HEADER_LEN + get_unaligned_be16(skb->data + 8);
	else
		return -EPROTO;
	if (pdu_len + OMCI_MIC_LEN > OMCI_MAX_PDU_LEN)
		return -EMSGSIZE;
	if (skb->len == pdu_len) {
		if (skb_tailroom(skb) < OMCI_MIC_LEN &&
		    pskb_expand_head(skb, 0, OMCI_MIC_LEN, GFP_ATOMIC))
			return -ENOMEM;
		mic = ~crc32_be(~0U, skb->data, skb->len);
		put_unaligned_be32(mic, skb_put(skb, OMCI_MIC_LEN));
	} else if (skb->len != pdu_len + OMCI_MIC_LEN) {
		return -EMSGSIZE;
	}
	rc = luna_xpon.tx(luna_xpon.cookie, skb->data, skb->len);
	if (!rc)
		dev_kfree_skb_any(skb);
	else
		pr_warn_ratelimited("luna-xpon: OMCI TX failed: %d (len=%u GEM=%u)\n",
				    rc, skb->len, gem_port_id);
	return rc;
}

static int luna_xpon_get_ani_topology(struct omci_device *odev,
				      struct omci_ani_topology *t)
{
	*t = (struct omci_ani_topology) {
		.tcont_base = LUNA_XPON_TCONT_BASE,
		.scheduler_base = 0x8000,
		.queue_base = 0x8000,
		/* stock's own Priority Queue sizes */
		.maximum_queue_size = 3276,
		.allocated_queue_size = 3276,
		.tcont_count = LUNA_XPON_TCONTS,
		/* Stock's eight queues per T-CONT, WRR: Nokia provisions the
		 * internet GEM on queue 7 and resets an ONU that lacks it. They
		 * all drain through the T-CONT's one hardware queue. */
		.queues_per_tcont = 8,
		.queue_config_option = 1,
		.scheduler_policy = 2,
		/* ONU2-G as the GC1601 reports it, which Nokia accepts */
		.gem_port_count = 256,
		.qos_configuration = BIT(3) | BIT(4),
		.downstream_queues_per_uni = 8,
		/* Nokia's VEIP service arrives over PLOAM without OMCI Creates:
		 * omcid seeds it, as on the GC1601. */
		.flags = OMCI_TOPOLOGY_F_PROFILE_SERVICES |
			 OMCI_TOPOLOGY_F_FULL_UNI_ENTITY,
	};
	return 0;
}

static struct luna_xpon_tcont *luna_xpon_tcont_find(u16 entity)
{
	struct luna_xpon_tcont *t;

	if (entity < LUNA_XPON_TCONT_BASE ||
	    entity >= LUNA_XPON_TCONT_BASE + LUNA_XPON_TCONTS)
		return NULL;
	t = &luna_xpon.tcont[entity - LUNA_XPON_TCONT_BASE];
	return t->valid ? t : NULL;
}

/* ---- upstream QoS: the OLT's priority queues on the T-CONT queue bank ----
 *
 * A GEM drains to one queue through its SID. GEMs of a T-CONT sharing an
 * OMCI priority share a queue; a GEM with a peak rate gets one of its own,
 * since a queue shaper limits everything on it. The least urgent queue is
 * the slot's own, the others come from the pool above the eight slot queues
 * in ascending QID order, so a strict T-CONT's most urgent queue wins.
 */
struct luna_xpon_qgroup {
	u8 prio;	/* G.988 related-port priority, 0 most urgent */
	u8 owner;	/* GEM slot of a rated GEM, else LUNA_XPON_GEMS */
	u32 rate;	/* bytes/s, 0 unlimited */
	u8 qid;
};

static u32 luna_xpon_gem_rate(u16 gem)
{
	unsigned int i;

	for (i = 0; i < LUNA_XPON_GEMS; i++)
		if (luna_xpon.gem_rate[i].rate && luna_xpon.gem_rate[i].gem == gem)
			return luna_xpon.gem_rate[i].rate;
	return 0;
}

/* omcid resolves one priority queue per GEM; an unclassified GEM is least urgent. */
static u8 luna_xpon_gem_priority(u16 gem)
{
	unsigned int i;
	u8 prio = 7;

	spin_lock_bh(&luna_xpon.svc_lock);
	for (i = 0; i < LUNA_XPON_SERVICES; i++) {
		const struct luna_xpon_service *s = &luna_xpon.svc[i];

		if (s->used && s->gem == gem && !s->multicast &&
		    (s->direction & OMCI_GEM_PORT_DIRECTION_UNI_TO_ANI)) {
			prio = s->queue & 7;
			break;
		}
	}
	spin_unlock_bh(&luna_xpon.svc_lock);
	return prio;
}

static int luna_xpon_queue_program(u8 qid, bool wfq, u16 weight, u32 pir)
{
	struct luna_xpon_queue *q = &luna_xpon.queue[qid - luna_xpon_hw_bank_qid()];
	int rc;

	if (q->valid && q->wfq == wfq && q->weight == weight && q->pir == pir)
		return 0;
	rc = luna_xpon_hw_queue_set(qid, wfq, weight, pir);
	*q = (struct luna_xpon_queue) {
		.pir = pir, .weight = weight, .wfq = wfq, .valid = !rc,
	};
	if (!rc)
		pr_info("luna-xpon: queue %u: %s weight %u, PIR %u\n", qid,
			wfq ? "WFQ" : "strict", weight, pir);
	return rc;
}

/* Back to the install defaults, drained, for whichever T-CONT takes it next. */
static void luna_xpon_queue_free(u8 qid)
{
	luna_xpon_hw_queue_drain(qid);
	luna_xpon_queue_program(qid, false, 1, LUNA_XPON_PIR_MAX);
}

static void luna_xpon_qos_release(struct luna_xpon_tcont *t)
{
	u8 base = luna_xpon_hw_bank_qid();
	unsigned long pool = t->pool;
	unsigned int bit;

	luna_xpon_hw_tcont_queues(t - luna_xpon.tcont, BIT(t->qid - base));
	for_each_set_bit(bit, &pool, LUNA_XPON_BANK_QUEUES)
		luna_xpon_queue_free(base + bit);
	t->pool = 0;
}

static u32 luna_xpon_pool_free(const struct luna_xpon_tcont *self)
{
	u32 used = GENMASK(LUNA_XPON_TCONTS - 1, 0);
	unsigned int i;

	for (i = 0; i < LUNA_XPON_TCONTS; i++)
		if (&luna_xpon.tcont[i] != self)
			used |= luna_xpon.tcont[i].pool;
	return ~used;
}

static u32 luna_xpon_pir(u32 rate)
{
	return rate ? clamp_t(u32, rate >> 10, 1, LUNA_XPON_PIR_MAX) : LUNA_XPON_PIR_MAX;
}

static int luna_xpon_qos_tcont(struct luna_xpon_tcont *t)
{
	const struct omci_tcont_qos *qos = &luna_xpon.tcont_qos[t - luna_xpon.tcont];
	struct luna_xpon_qgroup grp[LUNA_XPON_GEMS];
	u8 gem_grp[LUNA_XPON_GEMS], ord[LUNA_XPON_GEMS];
	u8 base = luna_xpon_hw_bank_qid();
	u32 avail = luna_xpon_pool_free(t), pool = 0;
	unsigned long gone;
	unsigned int i, j, n = 0, bit;
	bool wfq;
	int rc;

	for (i = 0; i < LUNA_XPON_GEMS; i++) {
		const struct luna_xpon_gem *g = &luna_xpon.gem[i];
		struct luna_xpon_qgroup want = { .owner = LUNA_XPON_GEMS };

		gem_grp[i] = LUNA_XPON_GEMS;
		if (!g->hw || g->tcont != t->entity ||
		    !(g->dir & OMCI_GEM_PORT_DIRECTION_UNI_TO_ANI))
			continue;
		want.prio = qos->enabled ? luna_xpon_gem_priority(g->gem) : 0;
		want.rate = luna_xpon_gem_rate(g->gem);
		if (want.rate)
			want.owner = i;
		for (j = 0; j < n; j++)
			if (grp[j].prio == want.prio && grp[j].owner == want.owner)
				break;
		if (j == n)
			grp[n++] = want;
		gem_grp[i] = j;
	}

	/* Least urgent first; it keeps the slot's own queue. */
	for (i = 0; i < n; i++) {
		for (j = i; j && grp[ord[j - 1]].prio < grp[i].prio; j--)
			ord[j] = ord[j - 1];
		ord[j] = i;
	}
	/* Keep the queues it holds, so a release elsewhere moves nothing live. */
	for (i = 1; i < n; i++) {
		u32 from = avail & t->pool ? avail & t->pool : avail;

		if (!from) {
			pr_warn_once("luna-xpon: upstream queue bank exhausted, sharing queues\n");
			break;
		}
		bit = __ffs(from);
		avail &= ~BIT(bit);
		pool |= BIT(bit);
	}
	avail = pool;
	for (i = 0; i < n; i++) {
		struct luna_xpon_qgroup *q = &grp[ord[i]];

		q->qid = t->qid;
		if (i && avail) {
			bit = __ffs(avail);
			avail &= ~BIT(bit);
			q->qid = base + bit;
		}
	}

	wfq = n > 1 && !(qos->enabled && qos->policy == 1);
	for (i = 0; i < n; i++) {
		const struct luna_xpon_qgroup *q = &grp[ord[i]];
		u16 weight = wfq && qos->enabled ? max_t(u16, qos->weight[q->prio], 1) : 1;

		/* merged groups keep the least urgent one's settings */
		if (i && q->qid == t->qid)
			continue;
		if (q->qid != t->qid && !(t->pool & BIT(q->qid - base))) {
			rc = luna_xpon_hw_queue_drain(q->qid);
			if (rc)
				return rc;
		}
		rc = luna_xpon_queue_program(q->qid, wfq, weight, luna_xpon_pir(q->rate));
		if (rc)
			return rc;
	}

	/* Old and new queues both schedule while SIDs move. */
	luna_xpon_hw_tcont_queues(t - luna_xpon.tcont, BIT(t->qid - base) | t->pool | pool);
	for (i = 0; i < LUNA_XPON_GEMS; i++) {
		struct luna_xpon_gem *g = &luna_xpon.gem[i];
		u8 qid;

		if (gem_grp[i] == LUNA_XPON_GEMS)
			continue;
		qid = grp[gem_grp[i]].qid;
		if (g->qid == qid)
			continue;
		rc = luna_xpon_hw_gem_queue(i, qid);
		if (rc)
			return rc;
		pr_info("luna-xpon: GEM %u -> queue %u (priority %u)\n", g->gem, qid,
			grp[gem_grp[i]].prio);
		g->qid = qid;
	}
	luna_xpon_hw_tcont_queues(t - luna_xpon.tcont, BIT(t->qid - base) | pool);
	gone = t->pool & ~pool;
	t->pool = pool;
	for_each_set_bit(bit, &gone, LUNA_XPON_BANK_QUEUES)
		luna_xpon_queue_free(base + bit);
	return 0;
}

/* Data keeps flowing on whatever queue it had: a failure is logged, never
 * reported to the OLT as a provisioning error. */
static void luna_xpon_qos_sync(void)
{
	unsigned int i;
	int rc;

	for (i = 0; i < LUNA_XPON_TCONTS; i++) {
		struct luna_xpon_tcont *t = &luna_xpon.tcont[i];

		if (!t->hw)
			continue;
		rc = luna_xpon_qos_tcont(t);
		if (rc)
			pr_warn_ratelimited("luna-xpon: T-CONT %#x QoS not applied: %d\n",
					    t->entity, rc);
	}
}

static void luna_xpon_gem_unbind(unsigned int slot)
{
	struct luna_xpon_gem *g = &luna_xpon.gem[slot];
	unsigned int i;

	rtl9601d_l2_changed();
	if (g->hw)
		luna_xpon_hw_gem_remove(slot);
	g->hw = false;
	spin_lock_bh(&luna_xpon.svc_lock);
	for (i = 0; i < LUNA_XPON_SERVICES; i++)
		if (luna_xpon.svc[i].used && luna_xpon.svc[i].gem == g->gem)
			luna_xpon.svc[i].used = false;
	spin_unlock_bh(&luna_xpon.svc_lock);
	luna_xpon_update_data();
}

static void luna_xpon_tcont_unbind(struct luna_xpon_tcont *t)
{
	unsigned int i;

	for (i = 0; i < LUNA_XPON_GEMS; i++)
		if (luna_xpon.gem[i].valid && luna_xpon.gem[i].tcont == t->entity)
			luna_xpon_gem_unbind(i);
	if (t->hw) {
		luna_xpon_qos_release(t);
		luna_xpon_hw_tcont_remove(t - luna_xpon.tcont);
	}
	t->hw = false;
	t->omcc = false;
}

/*
 * The MIB names T-CONTs and GEM ports before PLOAM grants their Alloc-IDs,
 * and seeds T-CONTs with none at all. Bindings are kept and reach the MAC
 * once the Alloc-ID is granted: the grant reconciles the MIB again. Until
 * then a service on them has no flow and replace_service defers it.
 */
static int luna_xpon_tcont_bind(struct luna_xpon_tcont *t)
{
	unsigned int slot = t - luna_xpon.tcont;
	u8 onu_id = luna_xpon_hw_onu_id();
	bool omcc = onu_id != 0xff && t->alloc == onu_id &&
		    luna_xpon_hw_omcc_gem() >= 0;
	int rc;

	if (!omcc && !luna_xpon_hw_alloc_known(t->alloc)) {
		if (t->hw || t->omcc)
			luna_xpon_tcont_unbind(t);
		return 0;
	}
	if (t->hw || t->omcc)
		return 0;
	if (omcc) {
		t->omcc = true;
		t->qid = luna_xpon_hw_omcc_qid();
		return 0;
	}
	rc = luna_xpon_hw_tcont_install(slot, t->alloc);
	if (rc)
		return rc;
	t->hw = true;
	t->qid = luna_xpon_hw_tcont_qid(slot);
	/* what the install programmed */
	luna_xpon.queue[t->qid - luna_xpon_hw_bank_qid()] = (struct luna_xpon_queue) {
		.pir = LUNA_XPON_PIR_MAX, .weight = 1, .valid = true,
	};
	return 0;
}

static bool luna_xpon_tcont_ready(const struct luna_xpon_tcont *t)
{
	return t && (t->hw || t->omcc);
}

static int luna_xpon_set_tcont(struct omci_device *odev, u16 entity,
			       u16 alloc, bool valid)
{
	struct luna_xpon_tcont *t = luna_xpon_tcont_find(entity);
	unsigned int i, slot;

	if (entity < LUNA_XPON_TCONT_BASE ||
	    entity >= LUNA_XPON_TCONT_BASE + LUNA_XPON_TCONTS)
		return -ERANGE;
	slot = entity - LUNA_XPON_TCONT_BASE;
	/* 0x00ff and 0xffff both mean "no Alloc-ID" and may repeat. */
	if (valid && alloc != 0xffff && alloc != 0x00ff)
		for (i = 0; i < LUNA_XPON_TCONTS; i++)
			if (i != slot && luna_xpon.tcont[i].valid &&
			    luna_xpon.tcont[i].alloc == alloc)
				return -EEXIST;
	if (t && valid && t->alloc == alloc)
		return luna_xpon_tcont_bind(t);
	if (t) {
		luna_xpon_tcont_unbind(t);
		*t = (struct luna_xpon_tcont) { };
	}
	if (!valid)
		return 0;

	luna_xpon.tcont[slot] = (struct luna_xpon_tcont) {
		.entity = entity,
		.alloc = alloc,
		.valid = true,
	};
	return luna_xpon_tcont_bind(&luna_xpon.tcont[slot]);
}

static int luna_xpon_set_gem_port(struct omci_device *odev, u16 entity,
				  u16 gem, u16 tcont_entity, u8 dir,
				  bool valid, bool encrypted)
{
	struct luna_xpon_tcont *t = NULL;
	struct luna_xpon_gem *g;
	unsigned int i, found = LUNA_XPON_GEMS, slot = LUNA_XPON_GEMS;
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
		if (luna_xpon.gem[i].entity == entity) {
			found = i;
			continue;
		}
		if (valid && luna_xpon.gem[i].gem == gem)
			return -EEXIST;
	}
	i = found;
	if (i < LUNA_XPON_GEMS) {
		g = &luna_xpon.gem[i];
		if (valid && g->hw && g->gem == gem && g->tcont == tcont_entity &&
		    g->dir == dir)
			return 0;
		luna_xpon_gem_unbind(i);
		*g = (struct luna_xpon_gem) { };
		slot = i;
	}
	if (!valid) {
		luna_xpon_update_data();
		luna_xpon_qos_sync();
		return 0;
	}
	if (slot == LUNA_XPON_GEMS)
		return -ENOSPC;

	g = &luna_xpon.gem[slot];
	*g = (struct luna_xpon_gem) {
		.entity = entity,
		.gem = gem,
		.tcont = tcont_entity,
		.dir = dir,
		.valid = true,
	};
	if (dir & OMCI_GEM_PORT_DIRECTION_UNI_TO_ANI) {
		t = luna_xpon_tcont_find(tcont_entity);
		if (!luna_xpon_tcont_ready(t))
			return 0;
	}
	/* Downstream decryption follows the PLOAM key; nothing to arm per GEM. */
	(void)encrypted;

	rc = luna_xpon_hw_gem_install(slot, gem, t ? t->qid : -1);
	if (rc) {
		g->valid = false;
		return rc;
	}
	g->hw = true;
	g->qid = t ? t->qid : 0;
	luna_xpon_update_data();
	luna_xpon_qos_sync();
	return 0;
}

static int luna_xpon_set_tcont_qos(struct omci_device *odev, u16 entity,
				   const struct omci_tcont_qos *qos)
{
	if (entity < LUNA_XPON_TCONT_BASE ||
	    entity >= LUNA_XPON_TCONT_BASE + LUNA_XPON_TCONTS)
		return -ERANGE;
	luna_xpon.tcont_qos[entity - LUNA_XPON_TCONT_BASE] = *qos;
	luna_xpon_qos_sync();
	return 0;
}

/* Upstream peak rates shape the GEM's queue; downstream policing and burst
 * sizes have no hardware path here and run unlimited. */
static int luna_xpon_set_gem_rate(struct omci_device *odev, u16 gem,
				  bool upstream, u32 rate, u32 burst)
{
	unsigned int i, slot = LUNA_XPON_GEMS;

	if (!upstream) {
		if (rate)
			pr_info_once("luna-xpon: downstream GEM rate limits are not applied\n");
		return 0;
	}
	if (burst)
		pr_info_once("luna-xpon: GEM burst sizes are not applied\n");
	for (i = 0; i < LUNA_XPON_GEMS; i++) {
		if (luna_xpon.gem_rate[i].rate && luna_xpon.gem_rate[i].gem == gem) {
			slot = i;
			break;
		}
		if (slot == LUNA_XPON_GEMS && !luna_xpon.gem_rate[i].rate)
			slot = i;
	}
	if (slot == LUNA_XPON_GEMS) {
		pr_warn_once("luna-xpon: too many rate-limited GEMs, GEM %u unlimited\n", gem);
		return 0;
	}
	luna_xpon.gem_rate[slot].gem = gem;
	luna_xpon.gem_rate[slot].rate = rate;
	luna_xpon_qos_sync();
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

/* Apply rates only after rejecting attributes this switch cannot honour. */
static int luna_xpon_set_uni_attrs(struct omci_device *odev, u16 entity,
				   const struct omci_uni_attrs *a)
{
	int port = luna_xpon_uni_port(entity);

	if (a->pause_quanta || a->ageing_s || a->learn_limit ||
	    a->auto_detection || a->max_frame_size)
		return -EOPNOTSUPP;
	if (port < 0 || !luna_xpon.uni_rate)
		return a->down_kbps || a->up_kbps ? -EOPNOTSUPP : 0;
	return luna_xpon.uni_rate(luna_xpon.cookie, port, a->down_kbps,
				 a->up_kbps);
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

static struct luna_xpon_gem *luna_xpon_gem_find(u16 gem)
{
	unsigned int i;

	for (i = 0; i < LUNA_XPON_GEMS; i++)
		if (luna_xpon.gem[i].valid && luna_xpon.gem[i].gem == gem)
			return &luna_xpon.gem[i];
	return NULL;
}

static bool luna_xpon_service_same(const struct luna_xpon_service *a,
				   const struct luna_xpon_service *b)
{
	return a->used && a->uni_entity == b->uni_entity && a->gem == b->gem &&
	       a->vlan == b->vlan && a->uni_vid == b->uni_vid &&
	       a->flow == b->flow && a->pcp == b->pcp && a->queue == b->queue &&
	       a->uni_mode == b->uni_mode && a->direction == b->direction &&
	       a->vlan_valid == b->vlan_valid && a->pcp_valid == b->pcp_valid &&
	       a->default_service == b->default_service &&
	       a->multicast == b->multicast;
}

static int luna_xpon_replace_service(struct omci_device *odev,
				     const struct omci_service_config *s)
{
	struct luna_xpon_service *svc = NULL, n;
	struct luna_xpon_gem *g = luna_xpon_gem_find(s->gem_port_id);
	struct luna_xpon_tcont *t = luna_xpon_tcont_find(s->tcont_entity_id);
	bool changed;
	int flow;
	unsigned int i;

	/* Multicast is downstream only: recorded, never an upstream flow. */
	if (s->multicast) {
		flow = -1;
		goto record;
	}
	if (!g || !g->hw)
		return -EAGAIN;
	if ((g->dir & s->direction) != s->direction)
		return -EINVAL;
	if ((s->direction & OMCI_GEM_PORT_DIRECTION_UNI_TO_ANI) &&
	    (!luna_xpon_tcont_ready(t) || t->alloc != s->alloc_id ||
	     g->tcont != s->tcont_entity_id))
		return -EAGAIN;
	flow = luna_xpon_hw_gem_flow(g - luna_xpon.gem);
record:
	spin_lock_bh(&luna_xpon.svc_lock);
	for (i = 0; i < LUNA_XPON_SERVICES; i++) {
		if (luna_xpon.svc[i].used && luna_xpon.svc[i].cookie == s->cookie) {
			svc = &luna_xpon.svc[i];
			break;
		}
		if (!svc && !luna_xpon.svc[i].used)
			svc = &luna_xpon.svc[i];
	}
	n = (struct luna_xpon_service) {
		.uni_entity = s->uni_entity_id,
		.cookie = s->cookie,
		.gem = s->gem_port_id,
		.vlan = s->vlan_id,
		.uni_vid = s->uni_vlan_id,
		.flow = flow < 0 ? 0 : flow,
		.pcp = s->pcp & 7,
		.queue = s->queue & 7,
		.uni_mode = s->uni_vlan_mode,
		.direction = s->direction,
		.vlan_valid = s->vlan_valid,
		.pcp_valid = s->pcp_valid,
		.default_service = s->default_service,
		.multicast = s->multicast,
		.used = true,
	};
	/* omcid replays every service on each change: only a new or changed
	 * one reaches the hardware and the log. */
	changed = svc && !luna_xpon_service_same(svc, &n);
	if (changed) {
		rtl9601d_l2_changed();
		*svc = n;
	}
	spin_unlock_bh(&luna_xpon.svc_lock);
	if (!svc)
		return -ENOSPC;
	if (!changed)
		return 0;
	luna_xpon_update_data();
	luna_xpon_qos_sync();
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
		if (luna_xpon.svc[i].used && luna_xpon.svc[i].cookie == cookie) {
			rtl9601d_l2_changed();
			luna_xpon.svc[i].used = false;
		}
	spin_unlock_bh(&luna_xpon.svc_lock);
	luna_xpon_update_data();
	luna_xpon_qos_sync();
	return 0;
}

static u8 luna_xpon_lan_mode(const struct luna_xpon_service *s, u16 *uni_vid);

/* The offload worker holds RTNL; backend transactions serialize this copy. */
int luna_xpon_l2_services(struct luna_l2_service *services, bool vlan_filtering)
{
	unsigned int i, j;
	u8 priorities[LUNA_L2_SERVICES] = { 0 };
	int port, n = 0, rc = 0;
	u16 uni_vid;
	u8 mode;

	mutex_lock(&luna_xpon.lock);
	spin_lock_bh(&luna_xpon.svc_lock);
	if (!luna_xpon_active() || !luna_xpon_data_ready())
		goto out;
	for (i = 0; i < LUNA_XPON_SERVICES; i++) {
		const struct luna_xpon_service *s = &luna_xpon.svc[i];
		struct luna_l2_service d;

		if (!s->used)
			continue;
		mode = vlan_filtering ? OMCI_UNI_VLAN_SAME :
			luna_xpon_lan_mode(s, &uni_vid);
		if (!s->vlan_valid || !s->vlan || s->vlan >= 4095 ||
		    s->multicast || s->direction != OMCI_GEM_PORT_DIRECTION_BIDIRECTIONAL ||
		    (mode != OMCI_UNI_VLAN_SAME && mode != OMCI_UNI_VLAN_UNTAGGED)) {
			rc = -EOPNOTSUPP;
			goto out;
		}
		port = luna_xpon_uni_port(s->uni_entity);
		/* A VEIP binds both physical LANs; emulated UNI identities stay intact. */
		if ((port < 0 && s->uni_entity != 0x601) || port > 1) {
			rc = -EOPNOTSUPP;
			goto out;
		}
		d = (struct luna_l2_service) {
			.vid = s->vlan,
			.flow = s->flow,
			.lan_mask = port < 0 ? 3 : BIT(port),
			.untagged = mode == OMCI_UNI_VLAN_UNTAGGED,
		};
		d.untag_mask = d.untagged ? d.lan_mask & ~luna_xpon.lan_tagged : 0;
		d.pvid_mask = d.untag_mask;
		for (j = 0; j < n; j++) {
			if (services[j].vid == d.vid) {
				if (services[j].flow != d.flow ||
				    services[j].lan_mask != d.lan_mask ||
				    services[j].untagged != d.untagged) {
					rc = -EOPNOTSUPP;
					goto out;
				}
				break;
			}
			if (services[j].untagged && d.untagged &&
			    (services[j].lan_mask & d.lan_mask)) {
				rc = -EOPNOTSUPP;
				goto out;
			}
		}
		if (j == n) {
			if (n == LUNA_L2_SERVICES) {
				rc = -ENOSPC;
				goto out;
			}
			services[n++] = d;
		}
		priorities[j] |= s->pcp_valid ? BIT(s->pcp) : 0xff;
	}
	/* A single-GEM mapper can collapse all eight p-bits into one rule. */
	for (j = 0; j < n; j++)
		if (priorities[j] != 0xff) {
			rc = -EOPNOTSUPP;
			goto out;
		}
	rc = n;
out:
	spin_unlock_bh(&luna_xpon.svc_lock);
	mutex_unlock(&luna_xpon.lock);
	return rc;
}
EXPORT_SYMBOL(luna_xpon_l2_services);

/* Sets the network VLAN of every provisioned service in @vids. */
void luna_xpon_service_vids(unsigned long *vids)
{
	unsigned int i;

	spin_lock_bh(&luna_xpon.svc_lock);
	for (i = 0; i < LUNA_XPON_SERVICES; i++) {
		const struct luna_xpon_service *s = &luna_xpon.svc[i];

		if (s->used && s->vlan_valid && s->vlan && s->vlan < VLAN_N_VID - 1)
			__set_bit(s->vlan, vids);
	}
	spin_unlock_bh(&luna_xpon.svc_lock);
}
EXPORT_SYMBOL(luna_xpon_service_vids);

/* The GN25L95 keeps chip registers where SFF-8472 has its levels and alarm
 * flags (A2h 0x18-0x27, 112-113). These are the BOSA's, in nW: a 2.5G PIN
 * receiver holds about -30 dBm and overloads near -7 dBm; the laser is set
 * for the class B+ 0.5 to 5 dBm launch. */
#define LUNA_BOSA_RX_ALARM_LOW	1000		/* -30 dBm */
#define LUNA_BOSA_RX_WARN_LOW	1585		/* -28 dBm */
#define LUNA_BOSA_RX_WARN_HIGH	158489		/* -8 dBm */
#define LUNA_BOSA_RX_ALARM_HIGH	199526		/* -7 dBm */
#define LUNA_BOSA_TX_ALARM_LOW	1000000		/* 0 dBm */
#define LUNA_BOSA_TX_WARN_LOW	1122018		/* 0.5 dBm */
#define LUNA_BOSA_TX_WARN_HIGH	3162278		/* 5 dBm */
#define LUNA_BOSA_TX_ALARM_HIGH	3981072		/* 6 dBm */

static u32 luna_xpon_optics_alarms(const struct luna_xpon_optics *o)
{
	u32 a = 0;

	if (o->rx_nw < LUNA_BOSA_RX_ALARM_LOW)
		a |= OPTICAL_FRONTEND_ALARM_RX_LOW_POWER;
	if (o->rx_nw > LUNA_BOSA_RX_ALARM_HIGH)
		a |= OPTICAL_FRONTEND_ALARM_RX_HIGH_POWER;
	/* Burst mode: the laser reads dark between grants and before O5. */
	if (luna_xpon_hw_state() == GPON_O5) {
		if (o->tx_nw < LUNA_BOSA_TX_ALARM_LOW)
			a |= OPTICAL_FRONTEND_ALARM_TX_LOW_POWER;
		if (o->tx_nw > LUNA_BOSA_TX_ALARM_HIGH)
			a |= OPTICAL_FRONTEND_ALARM_TX_HIGH_POWER;
	}
	return a;
}

static int luna_xpon_get_telemetry(struct omci_device *odev,
				   struct omci_telemetry *t)
{
	struct luna_xpon_optics o;
	struct luna_xpon_pm pm;

	luna_xpon_hw_pm(&pm);
	t->downstream_fec = pm.ds_fec ? OMCI_FEC_STATUS_UP : OMCI_FEC_STATUS_DOWN;
	t->upstream_fec = pm.us_fec ? OMCI_FEC_STATUS_UP : OMCI_FEC_STATUS_DOWN;
	t->valid |= OMCI_TELEMETRY_F_FEC_DOWNSTREAM | OMCI_TELEMETRY_F_FEC_UPSTREAM;
	if (luna_xpon_hw_state() == GPON_O5) {
		t->bip_errors = pm.bip_errors;
		t->fec_corrected_bytes = pm.fec_corrected_bytes;
		t->fec_corrected_codewords = pm.fec_corrected_codewords;
		t->fec_uncorrectable_codewords = pm.fec_uncorrectable_codewords;
		t->fec_total_codewords = pm.fec_total_codewords;
		t->fec_seconds = pm.fec_seconds;
		t->valid |= OMCI_TELEMETRY_F_BIP_ERRORS | OMCI_TELEMETRY_F_FEC_COUNTERS;
	}

	if (luna_xpon_hw_optics(&o))
		return 0;
	t->bosa_temperature_mc = o.temp_mc;
	t->bosa_voltage_uv = o.vcc_uv;
	t->bosa_bias_ua = o.bias_ua;
	t->bosa_tx_power_nw = o.tx_nw;
	t->bosa_rx_power_nw = o.rx_nw;
	t->bosa_alarms = luna_xpon_optics_alarms(&o);
	t->valid |= OMCI_TELEMETRY_F_BOSA_TEMPERATURE |
		    OMCI_TELEMETRY_F_BOSA_VOLTAGE |
		    OMCI_TELEMETRY_F_BOSA_BIAS |
		    OMCI_TELEMETRY_F_BOSA_TX_POWER |
		    OMCI_TELEMETRY_F_BOSA_RX_POWER |
		    OMCI_TELEMETRY_F_BOSA_ALARMS;
	return 0;
}

static void luna_xpon_set_operational(struct omci_device *odev, bool operational)
{
	if (operational != READ_ONCE(luna_xpon.operational)) {
		if (operational)
			pr_info("luna-xpon: OMCI agent operational; service provisioning applied %u ms after signal detection\n",
				jiffies_to_msecs(jiffies - READ_ONCE(luna_xpon.signal_since)));
		else
			pr_info("luna-xpon: OMCI agent not operational\n");
	}
	WRITE_ONCE(luna_xpon.operational, operational);
	rtl9601d_l2_changed();
	luna_xpon_update_carrier();
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
	.set_tcont_qos		= luna_xpon_set_tcont_qos,
	.set_gem_rate		= luna_xpon_set_gem_rate,
	.set_gem_port		= luna_xpon_set_gem_port,
	.set_uni		= luna_xpon_set_uni,
	.set_uni_attrs		= luna_xpon_set_uni_attrs,
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
#define LUNA_XPON_EV_IDENTITY	BIT(3)
#define LUNA_XPON_EV_LOS	BIT(4)

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

void luna_xpon_report_los(bool los)
{
	unsigned long flags, lost_ms = 0;
	bool changed;

	if (!READ_ONCE(luna_xpon.odev))
		return;
	spin_lock_irqsave(&luna_xpon.ev_lock, flags);
	changed = !luna_xpon.los_known || luna_xpon.ev_los != los;
	if (changed) {
		if (los)
			luna_xpon.loss_started = jiffies;
		else {
			if (luna_xpon.los_known)
				lost_ms = jiffies_to_msecs(jiffies - luna_xpon.loss_started);
			luna_xpon.signal_since = jiffies;
		}
	}
	luna_xpon.los_known = true;
	luna_xpon.ev |= LUNA_XPON_EV_LOS;
	luna_xpon.ev_los = los;
	spin_unlock_irqrestore(&luna_xpon.ev_lock, flags);
	if (changed) {
		if (los)
			pr_info("luna-xpon: downstream optical signal lost; waiting for signal\n");
		else if (lost_ms)
			pr_info("luna-xpon: downstream optical signal restored after %lu ms; waiting for registration\n", lost_ms);
		else
			pr_info("luna-xpon: downstream optical signal detected\n");
	}
	schedule_work(&luna_xpon.event_work);
}

void luna_xpon_report_state(u8 state)
{
	if (state == GPON_O5)
		pr_info("luna-xpon: registered with OLT (O5), %u ms after signal detection; restoring service provisioning\n",
			jiffies_to_msecs(jiffies - READ_ONCE(luna_xpon.signal_since)));
	if (state != GPON_O5)
		rtl9601d_l2_changed();
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

/* gpon_provision set the serial or password after omcid bound */
void luna_xpon_report_identity(void)
{
	luna_xpon_post(LUNA_XPON_EV_IDENTITY, 0, 0, 0);
}

/* The registration identity this driver ranges with, as omcid's default. */
static void luna_xpon_push_identity(struct omci_device *odev)
{
	struct omci_identity id = { 0 };

	luna_xpon_hw_get_identity(id.serial_number, id.password);
	if (!memchr_inv(id.serial_number, 0, sizeof(id.serial_number)))
		return;
	memcpy(id.vendor_id, id.serial_number, sizeof(id.vendor_id));
	id.valid = OMCI_IDENTITY_F_SERIAL_NUMBER | OMCI_IDENTITY_F_VENDOR_ID |
		   OMCI_IDENTITY_F_PASSWORD;
	omci_device_set_identity_info(odev, &id);
	omci_device_set_identity(odev, id.serial_number, id.password);
}

static void luna_xpon_deliver_state(struct omci_device *odev, u8 state)
{
	enum xpon_registration_state reg;

	if (state != GPON_O5)
		rtl9601d_l2_changed();
	if (state != GPON_O5)
		omci_device_set_channel(odev, 0, false);
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

/* Optical I2C reads sleep: sample only on transitions, from work context.
 * TX is a burst-mode sample and can read low between upstream grants. */
static void luna_xpon_log_optics(void)
{
	struct luna_xpon_optics o;
	int rc = luna_xpon_hw_optics(&o);

	if (rc) {
		pr_warn_ratelimited("luna-xpon: optical diagnostics unavailable (%d)\n", rc);
		return;
	}
	pr_info("luna-xpon: optical sample: RX=%u.%03u uW TX=%u.%03u uW (burst sample), bias=%u uA, temperature=%d mC, voltage=%u uV\n",
		o.rx_nw / 1000, o.rx_nw % 1000, o.tx_nw / 1000, o.tx_nw % 1000,
		o.bias_ua, o.temp_mc, o.vcc_uv);
}

static void luna_xpon_event_fn(struct work_struct *work)
{
	struct omci_device *odev = READ_ONCE(luna_xpon.odev);
	unsigned int ev;
	u8 state, onu_id;
	u16 gem;
	bool los;

	spin_lock_irq(&luna_xpon.ev_lock);
	ev = luna_xpon.ev;
	luna_xpon.ev = 0;
	state = luna_xpon.ev_state;
	onu_id = luna_xpon.ev_onu_id;
	gem = luna_xpon.ev_gem;
	los = luna_xpon.ev_los;
	spin_unlock_irq(&luna_xpon.ev_lock);
	if (!odev)
		return;

	if (ev & LUNA_XPON_EV_LOS)
		xpon_device_report_optical(luna_xpon.xpon, !los, los);
	if (ev & LUNA_XPON_EV_IDENTITY)
		luna_xpon_push_identity(odev);
	if (ev & LUNA_XPON_EV_ONU_ID)
		omci_device_set_onu_id(odev, onu_id);
	if (ev & LUNA_XPON_EV_CHANNEL) {
		/* The OMCC install scrubbed the CAMs: what we bound is gone. */
		mutex_lock(&luna_xpon.lock);
		luna_xpon_forget();
		mutex_unlock(&luna_xpon.lock);
		omci_device_set_channel(odev, gem, true);
		/* Reinstalling the same OMCC still erased the data CAMs. */
		schedule_work(&luna_xpon.reconcile_work);
	}
	if (ev & LUNA_XPON_EV_STATE)
		luna_xpon_deliver_state(odev, state);
	if ((ev & LUNA_XPON_EV_LOS) ||
	    ((ev & LUNA_XPON_EV_STATE) && state == GPON_O5))
		luna_xpon_log_optics();
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
	/* Only baseline PDUs have a fixed length. Extended messages carry
	 * their content length at offset 8; discard DMA padding, not content.
	 */
	if (len >= OMCI_PDU_LEN && msg[3] == OMCI_BASELINE_ID) {
		len = OMCI_PDU_LEN;
		flags = OMCI_F_MIC_PRESENT;
		if (luna_xpon_hw_mic_valid(msg, len))
			flags |= OMCI_F_MIC_VALID;
	} else if (len >= OMCI_EXT_HEADER_LEN && msg[3] == OMCI_EXTENDED_ID) {
		unsigned int pdu_len = OMCI_EXT_HEADER_LEN +
			get_unaligned_be16(msg + 8) + OMCI_MIC_LEN;

		if (pdu_len > len || pdu_len > OMCI_MAX_PDU_LEN)
			return;
		len = pdu_len;
		flags = OMCI_F_MIC_PRESENT;
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

/* Physical bits: LAN2 is port 0, LAN1 is port 1. */
u8 luna_xpon_lan_tagged(void)
{
	return READ_ONCE(luna_xpon.lan_tagged);
}
EXPORT_SYMBOL(luna_xpon_lan_tagged);

/* CPU fallback uses the same native service as the switch's PVID. */
int luna_xpon_native_vlan(void)
{
	int vid = 0;
	unsigned int i;
	u16 uni_vid;

	spin_lock_bh(&luna_xpon.svc_lock);
	for (i = 0; i < LUNA_XPON_SERVICES; i++) {
		const struct luna_xpon_service *s = &luna_xpon.svc[i];

		if (!s->used || !s->vlan_valid || s->multicast ||
		    luna_xpon_lan_mode(s, &uni_vid) != OMCI_UNI_VLAN_UNTAGGED)
			continue;
		if (vid && vid != s->vlan) {
			vid = -EOPNOTSUPP;
			break;
		}
		vid = s->vlan;
	}
	spin_unlock_bh(&luna_xpon.svc_lock);
	return vid;
}
EXPORT_SYMBOL(luna_xpon_native_vlan);

/* The VLAN a service's frames carry on the LAN side. */
static u16 luna_xpon_lan_vid(const struct luna_xpon_service *s, u8 mode,
			     u16 uni_vid)
{
	return mode == OMCI_UNI_VLAN_TRANSLATE || mode == OMCI_UNI_VLAN_STACKED ?
	       uni_vid : s->vlan;
}

/* The better of two candidate services for a frame with priority @pcp. */
static bool luna_xpon_better(const struct luna_xpon_service *s,
			     const struct luna_xpon_service *best, int pcp)
{
	if (!best)
		return true;
	if (pcp >= 0 && s->pcp_valid && s->pcp == pcp)
		return !(best->pcp_valid && best->pcp == pcp);
	return luna_xpon_rank(s) < luna_xpon_rank(best);
}

/*
 * The upstream flow for a frame, after giving it the service's network tag:
 * a frame the LAN sends untagged takes the VLAN of the service whose UNI side
 * is untagged. A tagged frame that matches no LAN-side VLAN but carries a
 * service's network VLAN, as omci-vlanscan's probes do, goes out unchanged;
 * else a VLAN-transparent service takes it. Returns the flow or a negative
 * errno; the skb may be replaced, and is freed when vlan_insert_tag fails.
 */
int luna_xpon_tx_flow(struct sk_buff **pskb, bool network_vlan)
{
	const struct luna_xpon_service *svc = NULL, *net_svc = NULL, *any = NULL, *s;
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
		if (!s->used || s->multicast ||
		    !(s->direction & OMCI_GEM_PORT_DIRECTION_UNI_TO_ANI))
			continue;
		mode = network_vlan ? OMCI_UNI_VLAN_SAME :
			luna_xpon_lan_mode(s, &uni_vid);
		if (tagged) {
			if (s->vlan_valid && s->vlan == vid &&
			    luna_xpon_better(s, net_svc, tci >> VLAN_PRIO_SHIFT))
				net_svc = s;
			if (!s->vlan_valid && luna_xpon_better(s, any, -1))
				any = s;
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
	if (!svc && (net_svc || any)) {
		svc = net_svc ?: any;
		svc_mode = OMCI_UNI_VLAN_SAME;
	}
	if (svc)
		flow = svc->flow;
	if (svc && svc->vlan_valid && svc_mode != OMCI_UNI_VLAN_SAME) {
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
		if (s->used && s->vlan_valid && s->vlan == vid &&
		    (s->direction & OMCI_GEM_PORT_DIRECTION_ANI_TO_UNI)) {
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
	rtl9601d_l2_changed();
	return count;
}
static DEVICE_ATTR_RW(sfu_untag_vlan);

static ssize_t sfu_lan_tagged_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", luna_xpon_lan_tagged());
}

static ssize_t sfu_lan_tagged_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	u8 mask;
	int rc = kstrtou8(buf, 0, &mask);

	if (rc)
		return rc;
	if (mask > 3)
		return -ERANGE;
	if (!of_machine_is_compatible("vsol,v2802rh"))
		return -EOPNOTSUPP;
	if (mask && luna_xpon_native_vlan() < 0)
		return -EOPNOTSUPP;
	spin_lock_bh(&luna_xpon.svc_lock);
	if (mask != luna_xpon.lan_tagged) {
		luna_xpon.lan_tagged = mask;
		rtl9601d_l2_changed();
	}
	spin_unlock_bh(&luna_xpon.svc_lock);
	return count;
}
static DEVICE_ATTR_RW(sfu_lan_tagged);

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
	rtl9601d_l2_changed();
	return count;
}
static DEVICE_ATTR_RW(sfu_vlan_map);

/* 0 keeps the ONU receive-only: no ranging, laser off. */
static ssize_t allow_tx_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	return sysfs_emit(buf, "%d\n", luna_xpon_hw_allow_tx());
}

static ssize_t allow_tx_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	bool on;
	int rc = kstrtobool(buf, &on);

	if (!rc)
		rc = luna_xpon_hw_set_allow_tx(on);
	return rc ?: count;
}
static DEVICE_ATTR_RW(allow_tx);

/* SFF-8472 A2h diagnostics 0x60-0x71, in the units omci_telemetry uses. */
static ssize_t bosa_ddm_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	u8 d[10];
	int rc = luna_xpon_hw_a2h(0x60, d, sizeof(d));

	if (rc)
		return rc;
	return sysfs_emit(buf,
			  "temp_mc=%d vcc_uv=%u bias_ua=%u tx_nw=%u rx_nw=%u raw=%*phN\n",
			  (s16)get_unaligned_be16(d) * 1000 / 256,
			  get_unaligned_be16(d + 2) * 100, get_unaligned_be16(d + 4) * 2,
			  get_unaligned_be16(d + 6) * 100, get_unaligned_be16(d + 8) * 100,
			  10, d);
}
static DEVICE_ATTR_RO(bosa_ddm);

/* SFF-8472 A2h 0x18-0x27: the module's TX and RX power alarm and warning
 * levels (high alarm, low alarm, high warning, low warning), in nW. */
static ssize_t bosa_thresholds_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf,
			  "tx_alarm_high_nw=%u tx_alarm_low_nw=%u tx_warn_high_nw=%u tx_warn_low_nw=%u "
			  "rx_alarm_high_nw=%u rx_alarm_low_nw=%u rx_warn_high_nw=%u rx_warn_low_nw=%u\n",
			  LUNA_BOSA_TX_ALARM_HIGH, LUNA_BOSA_TX_ALARM_LOW,
			  LUNA_BOSA_TX_WARN_HIGH, LUNA_BOSA_TX_WARN_LOW,
			  LUNA_BOSA_RX_ALARM_HIGH, LUNA_BOSA_RX_ALARM_LOW,
			  LUNA_BOSA_RX_WARN_HIGH, LUNA_BOSA_RX_WARN_LOW);
}
static DEVICE_ATTR_RO(bosa_thresholds);

static struct attribute *luna_xpon_attrs[] = {
	&dev_attr_bosa_ddm.attr,
	&dev_attr_bosa_thresholds.attr,
	&dev_attr_sfu_untag_vlan.attr,
	&dev_attr_sfu_lan_tagged.attr,
	&dev_attr_sfu_vlan_map.attr,
	&dev_attr_allow_tx.attr,
	NULL,
};

static umode_t luna_xpon_attr_visible(struct kobject *kobj,
				     struct attribute *attr, int index)
{
	if (attr == &dev_attr_sfu_lan_tagged.attr &&
	    !of_machine_is_compatible("vsol,v2802rh"))
		return 0;
	return attr->mode;
}

static const struct attribute_group luna_xpon_group = {
	.attrs = luna_xpon_attrs,
	.is_visible = luna_xpon_attr_visible,
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
		     bool (*uni_link)(void *, unsigned int),
		     int (*uni_rate)(void *, unsigned int, u32, u32),
		     struct led_classdev *pon_led, struct led_classdev *los_led)
{
	struct xpon_device_desc desc = {
		.modes = XPON_MODE_CAP(XPON_MODE_GPON),
		.mode = XPON_MODE_GPON,
		.ops = &luna_xpon_device_ops,
		.netdev = wan,
		.pon_led = pon_led,
		.los_led = los_led,
	};
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
	luna_xpon.uni_rate = uni_rate;
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

	luna_xpon_push_identity(odev);
	WRITE_ONCE(luna_xpon.odev, odev);

	/* Whatever the activation machine reached before omcid's device existed;
	 * an OMCC counts only once ranged. */
	if (luna_xpon_hw_onu_id() != 0xff)
		luna_xpon_report_onu_id(luna_xpon_hw_onu_id());
	gem = luna_xpon_hw_omcc_gem();
	if (gem >= 0 && luna_xpon_hw_state() == GPON_O5)
		luna_xpon_report_channel(gem);
	luna_xpon_report_state(luna_xpon_hw_state());
	/* A LOS sampled before this device existed was told to nobody. */
	if (luna_xpon_hw_los() >= 0)
		luna_xpon_report_los(luna_xpon_hw_los() == 1);

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
