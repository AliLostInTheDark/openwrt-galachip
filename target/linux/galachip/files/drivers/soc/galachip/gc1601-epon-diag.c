// SPDX-License-Identifier: GPL-2.0-only
/*
 * GalaChip GC1601 EPON diagnostics: load it on an ONU connected to an EPON
 * OLT and send the dmesg back. It changes nothing on the hardware.
 *
 * - on load, the EMAC, mode and SerDes status registers (the interrupt
 *   status registers are skipped: they may clear on read and the driver's
 *   poll needs them)
 * - every EMAC event the driver's poll sees, decoded
 * - the first downstream frames: descriptor minfo (GEM port in GPON, LLID
 *   or 0 in EPON?), ethertype, MPCP opcode, OAM flags and code
 * - the first upstream frames and whether pon0 sent or refused them
 * - a summary every summary_secs seconds
 *
 * /sys/kernel/debug/gc1601-epon/regs dumps the registers again.
 */

#include <linux/debugfs.h>
#include <linux/etherdevice.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include "gc1601-pon.h"

static unsigned int rx_log = 64;
module_param(rx_log, uint, 0644);
MODULE_PARM_DESC(rx_log, "Downstream frames to log (default 64)");

static unsigned int oam_log = 64;
module_param(oam_log, uint, 0644);
MODULE_PARM_DESC(oam_log, "OAM frames to log with a hex dump, either direction (default 64)");

static unsigned int tx_log = 32;
module_param(tx_log, uint, 0644);
MODULE_PARM_DESC(tx_log, "Upstream frames to log (default 32)");

static unsigned int event_log = 200;
module_param(event_log, uint, 0644);
MODULE_PARM_DESC(event_log, "EMAC events to log (default 200)");

static unsigned int summary_secs = 10;
module_param(summary_secs, uint, 0644);
MODULE_PARM_DESC(summary_secs, "Seconds between summaries, 0 for none (default 10)");

static struct gc1601_pon *diag_pon;
static struct dentry *diag_dir;
static struct delayed_work diag_summary;
static atomic_t rx_logged, oam_logged, tx_logged, events_logged;
static u64 diag_t0;

static const char * const llid_irq_names[4] = { "ack", "nack", "dereg", "rereg" };
static const char * const mpcp_irq_names[8] = {
	"sync", "preamble_err", "eop_err", "time_drift", "addr_err", "los", "bit6", "bit7",
};

static void diag_decode(char *buf, size_t size, u32 llid_irq, u32 mpcp_irq)
{
	size_t len = 0;
	int i, n;

	buf[0] = 0;
	for (i = 0; i < 32; i++) {
		if (!(llid_irq & BIT(i)))
			continue;
		len += scnprintf(buf + len, size - len, " %s%d", llid_irq_names[i / 8], i % 8);
	}
	for (i = 0; i < 8; i++)
		if (mpcp_irq & BIT(i))
			len += scnprintf(buf + len, size - len, " %s", mpcp_irq_names[i]);
	for (i = 8; i < 32; i++) {
		if (!(mpcp_irq & BIT(i)))
			continue;
		n = i % 8;
		len += scnprintf(buf + len, size - len, " %s%d",
				 i < 16 ? "crypt" : i < 24 ? "regdone" : "mpcp_loss", n);
	}
}

static void diag_event(struct gc1601_pon *pon, const struct gc1601_epon_event *ev)
{
	char bits[256];

	if (atomic_inc_return(&events_logged) > event_log)
		return;
	diag_decode(bits, sizeof(bits), ev->llid_irq, ev->mpcp_irq);
	dev_info(pon->dev, "epon-diag: +%llu ms event llid_irq=%08x mpcp_irq=%08x signal=%d sync=%d registered=%#x:%s\n",
		 div_u64(ev->ns - diag_t0, NSEC_PER_MSEC), ev->llid_irq, ev->mpcp_irq,
		 ev->signal, ev->sync, ev->registered, bits);
}

static const char *mpcp_opcode(u16 op)
{
	switch (op) {
	case 0x0001: return "PAUSE";
	case 0x0002: return "GATE";
	case 0x0003: return "REPORT";
	case 0x0004: return "REGISTER_REQ";
	case 0x0005: return "REGISTER";
	case 0x0006: return "REGISTER_ACK";
	default: return "?";
	}
}

/* Slow protocols subtype 3: flags (2), code (1); code 0xfe is organisation specific. */
static void diag_oam(struct gc1601_pon *pon, const char *dir, const u8 *data,
		     unsigned int len)
{
	if (len < ETH_HLEN + 4 || atomic_inc_return(&oam_logged) > oam_log)
		return;
	dev_info(pon->dev, "epon-diag: %s OAM len=%u flags=%04x code=%02x%s\n", dir, len,
		 get_unaligned_be16(data + ETH_HLEN + 1), data[ETH_HLEN + 3],
		 data[ETH_HLEN + 3] == 0xfe && len >= ETH_HLEN + 7 ? " (organisation specific)" : "");
	print_hex_dump(KERN_INFO, "epon-diag: ", DUMP_PREFIX_OFFSET, 16, 1, data,
		       min(len, 128U), false);
}

/* minfo of downstream data frames (not MPCP or OAM): the offload key candidate. */
#define DIAG_KEYS	8
static struct {
	u16 minfo;
	u64 frames;
} diag_keys[DIAG_KEYS];
static u64 diag_keys_other;
static DEFINE_SPINLOCK(diag_keys_lock);

static void diag_count_key(u16 minfo)
{
	unsigned int i;

	spin_lock(&diag_keys_lock);
	for (i = 0; i < DIAG_KEYS; i++) {
		if (!diag_keys[i].frames)
			diag_keys[i].minfo = minfo;
		if (diag_keys[i].minfo == minfo) {
			diag_keys[i].frames++;
			break;
		}
	}
	if (i == DIAG_KEYS)
		diag_keys_other++;
	spin_unlock(&diag_keys_lock);
}

static void diag_rx(struct gc1601_pon *pon, u16 minfo, const u8 *data, unsigned int len)
{
	u16 type = len >= ETH_HLEN ? get_unaligned_be16(data + 2 * ETH_ALEN) : 0;

	if (type != ETH_P_PAUSE && type != ETH_P_SLOW)
		diag_count_key(minfo);

	if (type == ETH_P_SLOW && len > ETH_HLEN && data[ETH_HLEN] == 0x03)
		diag_oam(pon, "rx", data, len);
	if (atomic_inc_return(&rx_logged) > rx_log)
		return;
	if (type == ETH_P_PAUSE && len >= ETH_HLEN + 2)
		dev_info(pon->dev, "epon-diag: rx minfo=%u len=%u %pM <- %pM MPCP %s\n",
			 minfo, len, data, data + ETH_ALEN,
			 mpcp_opcode(get_unaligned_be16(data + ETH_HLEN)));
	else
		dev_info(pon->dev, "epon-diag: rx minfo=%u len=%u %pM <- %pM type %04x\n",
			 minfo, len, data, data + ETH_ALEN, type);
}

static void diag_tx(struct gc1601_pon *pon, const u8 *data, unsigned int len, int ret)
{
	u16 type = len >= ETH_HLEN ? get_unaligned_be16(data + 2 * ETH_ALEN) : 0;

	if (!ret && type == ETH_P_SLOW && len > ETH_HLEN && data[ETH_HLEN] == 0x03)
		diag_oam(pon, "tx", data, len);
	if (atomic_inc_return(&tx_logged) > tx_log)
		return;
	dev_info(pon->dev, "epon-diag: tx len=%u %pM -> %pM type %04x: %s\n", len,
		 data + ETH_ALEN, data, type,
		 ret ? "blocked by EPON registration/OAM gate" : "accepted for LLID 0 TX");
}

static const struct gc1601_epon_diag_ops diag_ops = {
	.event = diag_event,
	.rx = diag_rx,
	.tx = diag_tx,
};

/* EMAC offsets that are safe to read: everything but the interrupt status. */
static bool diag_readable(u32 off)
{
	return off != EMAC_IRQ_LLID && off != EMAC_IRQ_MPCP;
}

static void diag_dump_range(struct seq_file *m, struct gc1601_pon *pon, u32 start, u32 end)
{
	u32 off;

	for (off = start; off < end; off += 16) {
		u32 v[4];
		int i;

		for (i = 0; i < 4; i++)
			v[i] = diag_readable(off + 4 * i) ? readl(pon->emac + off + 4 * i) : 0xdeadbeef;
		if (m)
			seq_printf(m, "emac+%03x: %08x %08x %08x %08x\n", off, v[0], v[1], v[2], v[3]);
		else
			dev_info(pon->dev, "epon-diag: emac+%03x: %08x %08x %08x %08x\n", off,
				 v[0], v[1], v[2], v[3]);
	}
}

static void diag_dump(struct seq_file *m, struct gc1601_pon *pon)
{
	u32 top0 = readl(pon->top + TOP_ENABLE), topc = readl(pon->top + TOP_WAN_CLK);
	u32 g4 = readl(pon->global + GLB_CTRL), g8 = readl(pon->global + 0x08);
	u32 gc = readl(pon->global + 0x0c), g14 = readl(pon->global + GLB_WAN_CFG);
	u32 scu = readl(pon->scu + SCU_PON_STATUS);

	if (m)
		seq_printf(m, "mode=%s top00=%08x top0c=%08x glb04=%08x glb08=%08x glb0c=%08x glb14=%08x scu=%08x serdes194=%02x serdes19c=%02x\n",
			   pon->mode == GC1601_PON_EPON ? "epon" : "gpon", top0, topc, g4, g8,
			   gc, g14, scu, readl(pon->serdes + 0x194), readl(pon->serdes + 0x19c));
	else
		dev_info(pon->dev, "epon-diag: mode=%s top00=%08x top0c=%08x glb04=%08x glb08=%08x glb0c=%08x glb14=%08x scu=%08x serdes194=%02x serdes19c=%02x\n",
			 pon->mode == GC1601_PON_EPON ? "epon" : "gpon", top0, topc, g4, g8,
			 gc, g14, scu, readl(pon->serdes + 0x194), readl(pon->serdes + 0x19c));
	/* The EPON MAC block is held off in GPON mode. */
	if (pon->mode != GC1601_PON_EPON) {
		if (m)
			seq_puts(m, "GPON mode: EMAC not read\n");
		else
			dev_info(pon->dev, "epon-diag: GPON mode: EMAC not read\n");
		return;
	}
	/* 0xdeadbeef: the interrupt status words, not read */
	diag_dump_range(m, pon, 0x000, 0x200);
	diag_dump_range(m, pon, 0x400, 0x420);
	diag_dump_range(m, pon, 0x800, 0x840);
}

static int diag_regs_show(struct seq_file *m, void *v)
{
	diag_dump(m, diag_pon);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(diag_regs);

static void diag_summary_work(struct work_struct *w)
{
	struct gc1601_pon *pon = diag_pon;
	struct gc1601_epon *e = &pon->epon;

	/* Quiet while the other PON mode runs. */
	if (READ_ONCE(pon->mode) != GC1601_PON_EPON)
		goto again;
	dev_info(pon->dev, "epon-diag: summary mode=%s running=%d allow_tx=%d signal=%d sync=%d registered=%#x (hw %#x) rx=%llu mpcp=%llu oam=%llu minfo_seen=%08x tx=%llu refused=%llu dropped_events=%u\n",
		 pon->mode == GC1601_PON_EPON ? "epon" : "gpon", e->running, e->allow_tx,
		 e->signal, e->sync, e->registered,
		 pon->mode == GC1601_PON_EPON ? readl(pon->emac + EMAC_REGISTERED) : 0,
		 e->rx_frames, e->rx_mpcp, e->rx_oam, e->rx_minfo_seen, e->tx_frames,
		 e->tx_refused, e->dropped);
	{
		char keys[DIAG_KEYS * 24] = "", *p = keys;
		unsigned int i;

		spin_lock_bh(&diag_keys_lock);
		for (i = 0; i < DIAG_KEYS && diag_keys[i].frames; i++)
			p += scnprintf(p, keys + sizeof(keys) - p, " %u:%llu",
				       diag_keys[i].minfo, diag_keys[i].frames);
		spin_unlock_bh(&diag_keys_lock);
		dev_info(pon->dev, "epon-diag: data frames by minfo (minfo:frames):%s other=%llu\n",
			 keys[0] ? keys : " none", diag_keys_other);
	}
again:
	if (summary_secs)
		schedule_delayed_work(&diag_summary, secs_to_jiffies(summary_secs));
}

static int __init gc1601_epon_diag_init(void)
{
	struct gc1601_pon *pon;

	diag_t0 = ktime_get_ns();
	pon = gc1601_epon_diag_register(&diag_ops);
	if (IS_ERR(pon))
		return PTR_ERR(pon);
	diag_pon = pon;

	dev_info(pon->dev, "epon-diag: loaded; LLID 0 MAC %pM\n", pon->epon.mac);
	diag_dump(NULL, pon);
	diag_dir = debugfs_create_dir("gc1601-epon", NULL);
	debugfs_create_file("regs", 0400, diag_dir, NULL, &diag_regs_fops);

	INIT_DELAYED_WORK(&diag_summary, diag_summary_work);
	if (summary_secs)
		schedule_delayed_work(&diag_summary, secs_to_jiffies(summary_secs));
	return 0;
}

static void __exit gc1601_epon_diag_exit(void)
{
	cancel_delayed_work_sync(&diag_summary);
	debugfs_remove_recursive(diag_dir);
	gc1601_epon_diag_unregister();
	dev_info(diag_pon->dev, "epon-diag: unloaded\n");
}

module_init(gc1601_epon_diag_init);
module_exit(gc1601_epon_diag_exit);

MODULE_DESCRIPTION("GalaChip GC1601 EPON diagnostics");
MODULE_LICENSE("GPL");
