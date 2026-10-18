// SPDX-License-Identifier: GPL-2.0-only
/*
 * GalaChip GC1601 EPON MAC (IEEE 802.3ah, 1G).
 *
 * Ported from xpon_sdk EMAC_hw_init, chipset_set_llid, chipset_enable_llid
 * and EMAC_int_handle. MPCP discovery,
 * registration, GATE and REPORT run in hardware: the driver programs the
 * LLID 0 MAC, allows registration once the transmitter is allowed, and
 * samples the interrupt and state registers with an hrtimer. Interrupts are
 * left masked; whether the status bits latch while masked is one of the
 * things the gc1601-epon-diag module reports.
 */

#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/of_net.h>
#include <linux/random.h>
#include <linux/rcupdate.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>

#include <net/xpon.h>

#include "gc1601-pon.h"

#define EPON_POLL_NS		(1 * NSEC_PER_MSEC)
#define EPON_NACK_SILENCE	(60 * HZ)

static struct gc1601_pon *gc1601_epon_pon;
static const struct gc1601_epon_diag_ops __rcu *gc1601_epon_diag;
static DEFINE_MUTEX(gc1601_epon_diag_lock);

/* chipset_set_llid: the MAC byte-swapped into a high and a low word. */
static void gc1601_epon_set_llid_mac(struct gc1601_pon *pon, unsigned int llid,
				     const u8 *mac)
{
	writel(get_unaligned_be16(mac) << 16, pon->emac + EMAC_LLID_MAC_HI(llid));
	writel(get_unaligned_be32(mac + 2), pon->emac + EMAC_LLID_MAC_LO(llid));
}

/* The factory MAC, as gcoamd hands gc_sys_info's MAC to eponSetLinkMac. */
static void gc1601_epon_default_mac(struct gc1601_pon *pon)
{
	struct gc1601_epon *e = &pon->epon;

	if (is_valid_ether_addr(e->mac))
		return;
	if (of_get_mac_address(pon->dev->of_node, e->mac)) {
		eth_random_addr(e->mac);
		dev_warn(pon->dev, "EPON: no factory MAC, LLID 0 uses %pM\n", e->mac);
	}
}

/*
 * chipset_set_hold_over: keep the link through a short loss of signal.
 * The time field counts in 128 ns units; turning it off clears bit 31 and
 * +0x16c and keeps the time. Called with pon->lock held.
 */
static void gc1601_epon_set_holdover(struct gc1601_pon *pon)
{
	u32 ms = pon->epon.holdover_ms;
	u32 val = readl(pon->emac + EMAC_HOLDOVER) & ~BIT(31);

	if (ms)
		val = (val & GENMASK(30, 27)) | BIT(31) |
		      (div_u64((u64)ms * NSEC_PER_MSEC, 128) & GENMASK(26, 0));
	writel(val, pon->emac + EMAC_HOLDOVER);
	writel(!!ms, pon->emac + EMAC_HOLDOVER_ON);
}

/* EMAC_hw_init with the stock opt_polar_data_low = 1. Called with pon->lock held. */
void gc1601_epon_hw_init(struct gc1601_pon *pon)
{
	void __iomem *emac = pon->emac;

	writel((readl(emac + EMAC_CTRL) & ~0x101) | 0xe890, emac + EMAC_CTRL);
	writel(0x30002020, emac + EMAC_OPT_DELAY);
	writel(0, emac + EMAC_LLID_EN);		/* registration waits for allow_tx */
	writel(0, emac + EMAC_CFG68);
	writel((get_random_u32() & 0xff) | BIT(8), emac + EMAC_RANDOM_DELAY);
	writel(readl(emac + EMAC_CFG78) & ~EMAC_CFG78_POLAR, emac + EMAC_CFG78);
	writel(0x1e83, emac + EMAC_CFGF8);
	writel(0x13, emac + EMAC_CFG134);
	writel(0, emac + EMAC_SHAPER);		/* chipset_epon_shp_fill: disable upstream shaper */
	/* chipset_epon_wan_mtu_set(2000) from PMU_init; reset value is 1536 */
	writel((readl(emac + EMAC_MTU) & ~GENMASK(26, 16)) | 2000 << 16, emac + EMAC_MTU);
	/* Stock leaves +0x10 and +0x16c at reset (0x01900000, 1) until the OLT sets one. */
	if (pon->epon.holdover_ms)
		gc1601_epon_set_holdover(pon);
	writel(readl(emac + EMAC_CFG170) & ~BIT(2), emac + EMAC_CFG170);
	/* The poll reads the status registers; keep the interrupt lines quiet. */
	writel(U32_MAX, emac + EMAC_IRQ_LLID_MASK);
	writel(U32_MAX, emac + EMAC_IRQ_MPCP_MASK);

	gc1601_epon_default_mac(pon);
	gc1601_epon_set_llid_mac(pon, 0, pon->epon.mac);

	/* chipset_set_epon_rx_en(1): receive path only */
	writel(readl(emac + EMAC_CTRL) | EMAC_CTRL_RX_EN, emac + EMAC_CTRL);
}

/*
 * LLID 0 answers the discovery GATE (gcpon_eponEnableLinkReg) only with the
 * transmitter allowed and epon-oamd not holding it silent. Called with
 * pon->lock held.
 */
static void gc1601_epon_update_llid_en(struct gc1601_pon *pon)
{
	struct gc1601_epon *e = &pon->epon;

	if (pon->mode == GC1601_PON_EPON)
		writel(e->allow_tx && e->register_en && !e->nack_silent && !pon->tx_hold ?
		       BIT(0) : 0,
		       pon->emac + EMAC_LLID_EN);
}

/* Called with pon->lock held. */
int gc1601_epon_set_allow_tx(struct gc1601_pon *pon, bool allow)
{
	struct gc1601_epon *e = &pon->epon;
	int ret;

	if (!pon->emac)
		return allow ? -EOPNOTSUPP : 0;

	if (!allow) {
		e->allow_tx = false;
		gc1601_epon_update_llid_en(pon);
		gc1601_pon_tx_off(pon);
		return gc1601_bosa_soft_tx(pon, false);
	}
	if (pon->mode != GC1601_PON_EPON)
		return -EBUSY;
	if (pon->tx_hold) {
		/* Remembered; the laser comes on when the hold is released. */
		e->allow_tx = true;
		gc1601_epon_update_llid_en(pon);
		return 0;
	}

	ret = gc1601_bosa_soft_tx(pon, true);
	if (ret) {
		gc1601_pon_tx_off(pon);
		dev_err(pon->dev, "BOSA soft TX enable failed: %d\n", ret);
		return ret;
	}
	e->allow_tx = true;
	gc1601_pon_tx_on(pon);
	gc1601_epon_update_llid_en(pon);
	dev_warn(pon->dev, "EPON transmitter allowed: LLID 0 %pM registers with the OLT\n",
		 e->mac);
	return 0;
}

/* tx_hold: the laser and registration off, whatever allow_tx says. Lock held. */
void gc1601_epon_tx_hold(struct gc1601_pon *pon, bool hold)
{
	if (hold) {
		gc1601_epon_update_llid_en(pon);
		gc1601_pon_tx_off(pon);
		gc1601_bosa_soft_tx(pon, false);
	} else if (pon->epon.allow_tx) {
		gc1601_epon_set_allow_tx(pon, true);
	}
}

bool gc1601_epon_registered(struct gc1601_pon *pon, unsigned int llid)
{
	return READ_ONCE(pon->mode) == GC1601_PON_EPON &&
	       READ_ONCE(pon->epon.registered) & BIT(llid);
}

/* OAM needs the MPCP registration; data also the OAM link (gcpon_eponSetLinkUp). */
bool gc1601_epon_may_send(struct gc1601_pon *pon, bool oam)
{
	return gc1601_epon_registered(pon, 0) && (oam || READ_ONCE(pon->epon.oam_link));
}

/* chipset_set_churning_key: 24-bit key into key slot @index of @llid. */
static void gc1601_epon_set_key(struct gc1601_pon *pon, unsigned int llid,
				unsigned int index, u32 key)
{
	void __iomem *emac = pon->emac;
	u32 ctrl;

	writel(readl(emac + EMAC_CTRL) & ~BIT(10), emac + EMAC_CTRL);
	writel(key & 0xffffff, emac + EMAC_KEY);
	ctrl = readl(emac + EMAC_KEY_CTRL) | BIT(llid);
	ctrl = (ctrl & ~BIT(8)) | (index & 1) << 8;
	writel(ctrl | BIT(9), emac + EMAC_KEY_CTRL);
}

static void gc1601_epon_count(u64 *count, u32 bits)
{
	while (bits) {
		unsigned int bit = __ffs(bits);

		count[bit]++;
		bits &= bits - 1;
	}
}

static void gc1601_epon_discovery(struct work_struct *work)
{
	struct gc1601_epon *e = container_of(to_delayed_work(work),
					  struct gc1601_epon, discovery);
	struct gc1601_pon *pon = container_of(e, struct gc1601_pon, epon);

	mutex_lock(&pon->lock);
	if (e->running && pon->mode == GC1601_PON_EPON) {
		if (!READ_ONCE(e->registered))
			writel((get_random_u32() & 0xff) | BIT(8), pon->emac + EMAC_RANDOM_DELAY);
		mod_delayed_work(system_wq, &e->discovery, HZ);
	}
	mutex_unlock(&pon->lock);
}

static void gc1601_epon_nack_end(struct work_struct *work)
{
	struct gc1601_epon *e = container_of(to_delayed_work(work), struct gc1601_epon, nack);
	struct gc1601_pon *pon = container_of(e, struct gc1601_pon, epon);

	mutex_lock(&pon->lock);
	WRITE_ONCE(e->nack_silent, false);
	gc1601_epon_update_llid_en(pon);
	mutex_unlock(&pon->lock);
}

static enum hrtimer_restart gc1601_epon_poll(struct hrtimer *t)
{
	struct gc1601_epon *e = container_of(t, struct gc1601_epon, poll);
	struct gc1601_pon *pon = container_of(e, struct gc1601_pon, epon);
	u32 llid_irq = readl(pon->emac + EMAC_IRQ_LLID);
	u32 mpcp_irq = readl(pon->emac + EMAC_IRQ_MPCP);
	bool signal = readl(pon->emac + EMAC_SIGNAL) & BIT(0);
	bool sync = !readl(pon->emac + EMAC_SYNC);
	u8 registered = readl(pon->emac + EMAC_REGISTERED) & GENMASK(EPON_LLIDS - 1, 0);
	bool queue = false;

	spin_lock(&e->lock);
	/* Vendor EMAC IRQ status is read-clear; repeated bits are new events. */
	gc1601_epon_count(e->llid_irq_count, llid_irq);
	gc1601_epon_count(e->mpcp_irq_count, mpcp_irq);
	if (llid_irq & EMAC_IRQ_NACK(0))
		e->nack_pending = true;
	if ((e->registered & ~registered) || (e->signal && !signal))
		e->loss_pending = true;
	if ((e->registered & ~registered & BIT(0)) || (e->signal && !signal))
		e->oam_reset_pending = true;
	if (llid_irq || mpcp_irq ||
	    signal != e->signal || sync != e->sync || registered != e->registered) {
		struct gc1601_epon_event *ev = &e->ring[e->head % EPON_EVENTS];

		if (e->head - e->tail >= EPON_EVENTS) {
			e->dropped++;
			e->tail++;
		}
		e->head++;
		*ev = (struct gc1601_epon_event) {
			.ns = ktime_get_ns(),
			.llid_irq = llid_irq,
			.mpcp_irq = mpcp_irq,
			.signal = signal,
			.sync = sync,
			.registered = registered,
		};
		queue = true;
	}
	e->llid_irq_last = llid_irq;
	e->mpcp_irq_last = mpcp_irq;
	e->signal = signal;
	e->sync = sync;
	WRITE_ONCE(e->registered, registered);
	spin_unlock(&e->lock);

	if (queue)
		schedule_work(&e->work);
	hrtimer_forward_now(t, ns_to_ktime(EPON_POLL_NS));
	return HRTIMER_RESTART;
}

static void gc1601_epon_report(struct gc1601_pon *pon, bool signal, bool sync,
			       u8 registered)
{
	struct net_device *netdev = pon->xp.netdev;
	bool mpcp = registered & BIT(0);
	bool up = mpcp && pon->epon.oam_link;
	enum xpon_registration_state reg;

	if (up)
		reg = XPON_REGISTRATION_OPERATIONAL;
	else if (mpcp)
		reg = XPON_REGISTRATION_REGISTERING;
	else if (signal && sync)
		reg = pon->epon.allow_tx ? XPON_REGISTRATION_REGISTERING :
					   XPON_REGISTRATION_DISCOVERY;
	else
		reg = XPON_REGISTRATION_DOWN;

	/* Switch flows forward data only while the OAM link is up. */
	if (up != pon->epon.data_up) {
		pon->epon.data_up = up;
		gc1601_xpon_sfu_kick(pon);
	}

	/*
	 * The netdev carries OAM from MPCP registration on, or epon-oamd's
	 * discovery PDUs never reach the driver; gc1601_epon_may_send() holds
	 * data back until the OAM link is up.
	 */
	if (mpcp)
		netif_carrier_on(netdev);
	else
		netif_carrier_off(netdev);
	/* LOS means no light, as stock reads it (+0x7c); no sync is not LOS. */
	xpon_device_report_optical(pon->xp.xpon, signal, !signal);
	xpon_device_report_carrier(pon->xp.xpon, up);
	xpon_device_report_registration(pon->xp.xpon, reg);
	sysfs_notify(&pon->dev->kobj, NULL, "epon_state");
}

static void gc1601_epon_work(struct work_struct *w)
{
	struct gc1601_epon *e = container_of(w, struct gc1601_epon, work);
	struct gc1601_pon *pon = container_of(e, struct gc1601_pon, epon);
	const struct gc1601_epon_diag_ops *diag;
	struct gc1601_epon_event ev, last = { };
	bool have = false, loss, oam_reset;
	u32 nack = 0;

	for (;;) {
		spin_lock_irq(&e->lock);
		if (e->tail == e->head) {
			loss = e->loss_pending;
			oam_reset = e->oam_reset_pending;
			if (e->nack_pending)
				nack |= EMAC_IRQ_NACK(0);
			e->loss_pending = false;
			e->oam_reset_pending = false;
			e->nack_pending = false;
			spin_unlock_irq(&e->lock);
			break;
		}
		ev = e->ring[e->tail++ % EPON_EVENTS];
		spin_unlock_irq(&e->lock);

		rcu_read_lock();
		diag = rcu_dereference(gc1601_epon_diag);
		if (diag && diag->event)
			diag->event(pon, &ev);
		rcu_read_unlock();
		nack |= ev.llid_irq & EMAC_IRQ_NACK(0);
		last = ev;
		have = true;
	}
	if (!have)
		return;

	mutex_lock(&pon->lock);
	if (nack && !READ_ONCE(e->nack_silent)) {
		/* local_handle_register: a NACK sends the ONU silent for 60 s. */
		dev_warn(pon->dev, "EPON: OLT refused the registration, silent for 60 s\n");
		WRITE_ONCE(e->nack_silent, true);
		gc1601_epon_update_llid_en(pon);
		mod_delayed_work(system_wq, &e->nack, EPON_NACK_SILENCE);
	}

	/* local_pre_handle_init: back to discovery drops what LLIDs 0-3 queued. */
	if (loss || (e->reported && ((e->reported_registered & ~last.registered) ||
			    (e->reported_signal && !last.signal)))) {
		unsigned int t;

		for (t = 0; t < EPON_LLIDS; t++)
			gc1601_pe_flush_tcont(t);
	}

	if (last.registered != e->reported_registered || last.signal != e->reported_signal ||
	    last.sync != e->reported_sync || !e->reported || oam_reset) {
		if ((last.registered ^ e->reported_registered) & BIT(0)) {
			dev_info(pon->dev, "EPON: LLID 0 %s\n",
				 last.registered & BIT(0) ? "registered" : "deregistered");
			/* A new registration starts OAM discovery over. */
		}
		if (oam_reset || ((last.registered ^ e->reported_registered) & BIT(0))) {
			e->generation++;
			WRITE_ONCE(e->oam_link, false);
		}
		if (last.signal != e->reported_signal || !e->reported)
			dev_info(pon->dev, "EPON: optical signal %s\n",
				 last.signal ? "present" : "lost");
		e->reported = true;
		e->reported_registered = last.registered;
		e->reported_signal = last.signal;
		e->reported_sync = last.sync;
		gc1601_epon_report(pon, last.signal, last.sync, last.registered);
	}
	mutex_unlock(&pon->lock);
}

/* Called with pon->lock held. */
void gc1601_epon_start(struct gc1601_pon *pon)
{
	struct gc1601_epon *e = &pon->epon;

	if (e->running || !pon->emac)
		return;
	spin_lock_irq(&e->lock);
	e->head = e->tail = 0;
	e->loss_pending = false;
	e->oam_reset_pending = false;
	e->nack_pending = false;
	e->signal = e->sync = false;
	e->registered = 0;
	e->llid_irq_last = e->mpcp_irq_last = 0;
	spin_unlock_irq(&e->lock);
	e->reported = false;
	e->oam_link = false;
	e->generation++;
	e->running = true;
	mod_delayed_work(system_wq, &e->discovery, HZ);
	gc1601_epon_report(pon, false, false, 0);
	hrtimer_start(&e->poll, ns_to_ktime(EPON_POLL_NS), HRTIMER_MODE_REL);
	dev_info(pon->dev, "EPON MAC running, LLID 0 %pM, laser off until allow_tx\n",
		 e->mac);
}

void gc1601_epon_stop(struct gc1601_pon *pon)
{
	struct gc1601_epon *e = &pon->epon;
	int ret;

	if (!e->running)
		return;
	e->running = false;
	hrtimer_cancel(&e->poll);
	cancel_work_sync(&e->work);
	cancel_delayed_work_sync(&e->discovery);
	cancel_delayed_work_sync(&e->nack);
	WRITE_ONCE(e->nack_silent, false);
	WRITE_ONCE(e->registered, 0);
	mutex_lock(&pon->lock);
	e->oam_link = false;
	e->data_up = false;
	e->generation++;
	gc1601_xpon_sfu_kick(pon);
	ret = gc1601_xpon_clear_sfu(pon);
	if (ret)
		dev_warn(pon->dev, "EPON offload cleanup failed: %d\n", ret);
	gc1601_epon_report(pon, false, false, 0);
	mutex_unlock(&pon->lock);
	if (pon->xp.netdev)
		netif_carrier_off(pon->xp.netdev);
}

/* NAPI context: a downstream frame in EPON mode. */
void gc1601_epon_rx_seen(struct gc1601_pon *pon, u16 minfo, const u8 *data,
			 unsigned int len)
{
	struct gc1601_epon *e = &pon->epon;
	const struct gc1601_epon_diag_ops *diag;
	u16 type = len >= ETH_HLEN ? get_unaligned_be16(data + 2 * ETH_ALEN) : 0;

	e->rx_frames++;
	e->rx_mpcp += type == ETH_P_PAUSE;	/* MAC control: MPCP */
	e->rx_oam += type == ETH_P_SLOW && len > ETH_HLEN && data[ETH_HLEN] == 0x03;
	e->rx_minfo_last = minfo;
	if (minfo < 32)
		e->rx_minfo_seen |= BIT(minfo);

	rcu_read_lock();
	diag = rcu_dereference(gc1601_epon_diag);
	if (diag && diag->rx)
		diag->rx(pon, minfo, data, len);
	rcu_read_unlock();
}

void gc1601_epon_tx_seen(struct gc1601_pon *pon, const u8 *data, unsigned int len,
			 int ret)
{
	struct gc1601_epon *e = &pon->epon;
	const struct gc1601_epon_diag_ops *diag;

	if (ret == -ENOLINK)
		e->tx_refused++;
	else if (!ret)
		e->tx_frames++;

	rcu_read_lock();
	diag = rcu_dereference(gc1601_epon_diag);
	if (diag && diag->tx)
		diag->tx(pon, data, len, ret);
	rcu_read_unlock();
}

/* ---- diagnostics module hooks ---- */

struct gc1601_pon *gc1601_epon_diag_register(const struct gc1601_epon_diag_ops *ops)
{
	struct gc1601_pon *pon = READ_ONCE(gc1601_epon_pon);

	if (!pon || !pon->emac)
		return ERR_PTR(-ENODEV);
	mutex_lock(&gc1601_epon_diag_lock);
	if (rcu_access_pointer(gc1601_epon_diag)) {
		mutex_unlock(&gc1601_epon_diag_lock);
		return ERR_PTR(-EBUSY);
	}
	rcu_assign_pointer(gc1601_epon_diag, ops);
	mutex_unlock(&gc1601_epon_diag_lock);
	return pon;
}
EXPORT_SYMBOL_GPL(gc1601_epon_diag_register);

void gc1601_epon_diag_unregister(void)
{
	mutex_lock(&gc1601_epon_diag_lock);
	RCU_INIT_POINTER(gc1601_epon_diag, NULL);
	mutex_unlock(&gc1601_epon_diag_lock);
	synchronize_rcu();
}
EXPORT_SYMBOL_GPL(gc1601_epon_diag_unregister);

/* ---- sysfs ---- */

static ssize_t epon_state_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	struct gc1601_epon *e = &pon->epon;
	ssize_t len;
	int i;

	if (!pon->emac)
		return sysfs_emit(buf, "unsupported\n");

	/* In GPON mode nothing has read the factory MAC for LLID 0 yet. */
	mutex_lock(&pon->lock);
	gc1601_epon_default_mac(pon);
	mutex_unlock(&pon->lock);

	spin_lock_irq(&e->lock);
	len = sysfs_emit(buf,
			 "mode=%s running=%d allow_tx=%d register=%d nack_silent=%d oam_link=%d signal=%d sync=%d registered=%#x mac=%pM holdover_ms=%u generation=%u\n"
			 "llid_irq=%08x mpcp_irq=%08x events_dropped=%u\n"
			 "rx_frames=%llu rx_mpcp=%llu rx_oam=%llu rx_minfo_seen=%08x rx_minfo_last=%u\n"
			 "tx_frames=%llu tx_refused=%llu\n",
			 READ_ONCE(pon->mode) == GC1601_PON_EPON ? "epon" : "gpon",
			 e->running, e->allow_tx, e->register_en, e->nack_silent, e->oam_link,
			 e->signal, e->sync, e->registered, e->mac, e->holdover_ms,
			 READ_ONCE(e->generation),
			 e->llid_irq_last, e->mpcp_irq_last, e->dropped,
			 e->rx_frames, e->rx_mpcp, e->rx_oam, e->rx_minfo_seen, e->rx_minfo_last,
			 e->tx_frames, e->tx_refused);
	for (i = 0; i < 32; i++)
		if (e->llid_irq_count[i])
			len += sysfs_emit_at(buf, len, "llid_irq bit %d: %llu\n", i,
					     e->llid_irq_count[i]);
	for (i = 0; i < 32; i++)
		if (e->mpcp_irq_count[i])
			len += sysfs_emit_at(buf, len, "mpcp_irq bit %d: %llu\n", i,
					     e->mpcp_irq_count[i]);
	spin_unlock_irq(&e->lock);
	return len;
}
static DEVICE_ATTR_RO(epon_state);

static ssize_t epon_mac_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);

	mutex_lock(&pon->lock);
	gc1601_epon_default_mac(pon);
	mutex_unlock(&pon->lock);
	return sysfs_emit(buf, "%pM\n", pon->epon.mac);
}

/* The LLID 0 MAC the OLT sees; some OLTs authorise ONUs by it. */
static ssize_t epon_mac_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	u8 mac[ETH_ALEN];

	if (!mac_pton(buf, mac) || !is_valid_ether_addr(mac))
		return -EINVAL;
	if (!pon->emac)
		return -EOPNOTSUPP;

	mutex_lock(&pon->lock);
	ether_addr_copy(pon->epon.mac, mac);
	if (pon->mode == GC1601_PON_EPON)
		gc1601_epon_set_llid_mac(pon, 0, mac);
	mutex_unlock(&pon->lock);
	return count;
}
static DEVICE_ATTR_RW(epon_mac);

static ssize_t epon_oam_link_show(struct device *dev, struct device_attribute *attr,
				  char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(pon->epon.oam_link));
}

/* Qualify an OAM link-up with its generation so stale discovery cannot win. */
static ssize_t epon_oam_link_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	struct gc1601_epon *e = &pon->epon;
	unsigned int value, generation;
	char extra;
	bool on;
	int ret, fields;

	fields = sscanf(buf, "%u %u %c", &value, &generation, &extra);
	if (fields == 2 && value <= 1) {
		on = value;
		ret = 0;
	} else {
		ret = kstrtobool(buf, &on);
	}
	if (ret)
		return ret;
	mutex_lock(&pon->lock);
	if (on && ((fields == 2 && generation != e->generation) ||
		   READ_ONCE(e->oam_reset_pending))) {
		ret = -ESTALE;
		goto out;
	}
	if (on && !gc1601_epon_registered(pon, 0)) {
		ret = -ENOTCONN;
		goto out;
	}
	WRITE_ONCE(e->oam_link, on && gc1601_epon_registered(pon, 0));
	if (e->running)
		gc1601_epon_report(pon, e->signal, e->sync, e->registered);
out:
	mutex_unlock(&pon->lock);
	return ret ?: count;
}
static DEVICE_ATTR_RW(epon_oam_link);

static ssize_t epon_register_show(struct device *dev, struct device_attribute *attr,
				  char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(pon->epon.register_en));
}

/* epon-oamd: 0 keeps LLID 0 off the discovery window (silence after an auth failure). */
static ssize_t epon_register_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;
	mutex_lock(&pon->lock);
	pon->epon.register_en = on;
	if (!on)
		WRITE_ONCE(pon->epon.oam_link, false);
	gc1601_epon_update_llid_en(pon);
	mutex_unlock(&pon->lock);
	return count;
}
static DEVICE_ATTR_RW(epon_register);

/* epon-oamd, after a CTC churning New-Key Response: "<llid> <index> <key>". */
static ssize_t epon_key_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	unsigned int llid, index, key;

	if (sscanf(buf, "%u %u %x", &llid, &index, &key) != 3 ||
	    llid >= EPON_LLIDS || index > 1 || key > 0xffffff)
		return -EINVAL;
	mutex_lock(&pon->lock);
	if (pon->mode != GC1601_PON_EPON) {
		mutex_unlock(&pon->lock);
		return -EBUSY;
	}
	gc1601_epon_set_key(pon, llid, index, key);
	mutex_unlock(&pon->lock);
	return count;
}
static DEVICE_ATTR_WO(epon_key);

static ssize_t epon_holdover_show(struct device *dev, struct device_attribute *attr,
				  char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(pon->epon.holdover_ms));
}

/* epon-oamd, CTC holdover (0x0008): time in ms, 0 to turn it off. */
static ssize_t epon_holdover_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	unsigned int ms;
	int ret;

	ret = kstrtouint(buf, 0, &ms);
	if (ret)
		return ret;
	if (ms > 1000)
		return -ERANGE;
	if (!pon->emac)
		return -EOPNOTSUPP;
	mutex_lock(&pon->lock);
	pon->epon.holdover_ms = ms;
	if (pon->mode == GC1601_PON_EPON)
		gc1601_epon_set_holdover(pon);
	mutex_unlock(&pon->lock);
	return count;
}
static DEVICE_ATTR_RW(epon_holdover);

/*
 * The EMAC counters chipset_get_epon_stats reads (G7 and the newer FD511G
 * SDK on the same chip). Stock copies them as running totals; none has a
 * name in any stock binary, so they are shown by offset.
 */
static const u16 gc1601_epon_counters[] = {
	0x408, 0x40c, 0x410, 0x818, 0x81c, 0x820, 0x824, 0x828, 0x82c, 0x830,
	0x834, 0x838, 0x100c, 0x1018, 0x1080, 0x1084, 0x1088, 0x108c, 0x1090,
	0x1094, 0x1098, 0x109c, 0x10a0, 0x10a4, 0x10a8, 0x10ac, 0x10b0, 0x10b4,
};

static ssize_t epon_counters_show(struct device *dev, struct device_attribute *attr,
				  char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	ssize_t len = 0;
	unsigned int i;

	if (!pon->emac)
		return -EOPNOTSUPP;
	if (READ_ONCE(pon->mode) != GC1601_PON_EPON)
		return sysfs_emit(buf, "not in EPON mode\n");
	for (i = 0; i < ARRAY_SIZE(gc1601_epon_counters); i++)
		len += sysfs_emit_at(buf, len, "emac_%#x=%u\n", gc1601_epon_counters[i],
				     readl(pon->emac + gc1601_epon_counters[i]));
	return len;
}
static DEVICE_ATTR_RO(epon_counters);

static struct attribute *gc1601_epon_attrs[] = {
	&dev_attr_epon_state.attr,
	&dev_attr_epon_mac.attr,
	&dev_attr_epon_oam_link.attr,
	&dev_attr_epon_register.attr,
	&dev_attr_epon_key.attr,
	&dev_attr_epon_holdover.attr,
	&dev_attr_epon_counters.attr,
	NULL
};

const struct attribute_group gc1601_epon_group = {
	.attrs = gc1601_epon_attrs,
};

int gc1601_epon_init(struct gc1601_pon *pon)
{
	struct gc1601_epon *e = &pon->epon;

	spin_lock_init(&e->lock);
	e->register_en = true;
	INIT_WORK(&e->work, gc1601_epon_work);
	INIT_DELAYED_WORK(&e->discovery, gc1601_epon_discovery);
	INIT_DELAYED_WORK(&e->nack, gc1601_epon_nack_end);
	hrtimer_setup(&e->poll, gc1601_epon_poll, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	WRITE_ONCE(gc1601_epon_pon, pon);
	return 0;
}

void gc1601_epon_exit(struct gc1601_pon *pon)
{
	WRITE_ONCE(gc1601_epon_pon, NULL);
	gc1601_epon_stop(pon);
	mutex_lock(&pon->lock);
	gc1601_epon_set_allow_tx(pon, false);
	mutex_unlock(&pon->lock);
}
