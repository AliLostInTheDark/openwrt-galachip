// SPDX-License-Identifier: GPL-2.0-only
/*
 * GalaChip GC1601 GPON activation (G.984.3 O1-O7).
 *
 * Ported from the xpon_sdk state machine (local_handle_O1..O7, their entry
 * handlers, chipset_dn_plm_hook and the PLOAM/T-CONT/GEM accessors).
 * Downstream PLOAMs and LOS/LOF are sampled by an hrtimer and handled in
 * process context.
 *
 * Everything that makes the ONU transmit goes through gc1601_gpon_send() or
 * the O3 transition, both of which require allow_tx. Without it the machine
 * stops in O2 after receiving Upstream_Overhead. O3 also waits while the
 * first activation holds out for the userspace identity, so the OLT never
 * ranges the default serial number before the configured one.
 */

#include <linux/bitfield.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/hex.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/property.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>
#include <net/xpon/omci.h>

#include "gc1601-pon.h"

#define GPON_POLL_NS		(500 * NSEC_PER_USEC)
#define GPON_ACTIVATION_MS	10000	/* O3/O4 */
#define GPON_POPUP_MS		100	/* O6 */
#define GPON_PLOAM_REPEAT_NS	(1 * NSEC_PER_MSEC)
#define GPON_FRAME_BYTES	19440	/* upstream frame at 1.24416 Gb/s */
#define GPON_WATCHDOG_MS	1000
#define GPON_RECOVERY_MS	15000
#define DBRU_FREE_MIN		0x2fc	/* free packet buffers needed to act */
#define DBRU_LEVEL_DROP		900	/* taken off a stuck queue's reported level */
#define COUNTER_UP_BYTES	0x164	/* clear-on-read upstream data count */

static bool gpon_watchdog = true;
module_param(gpon_watchdog, bool, 0644);
MODULE_PARM_DESC(gpon_watchdog, "Re-initialise a hung GPON MAC and release stuck upstream reports in O5, as the stock SDK does (default on)");

/* ONU block */
#define ONU_RANDOM_DELAY	0x04
#define ONU_PRE_DELAY		0x10
#define ONU_EQD			0x14
#define ONU_ALLOC		0x40	/* two IDs per word, valid bits 12/28 */
#define ONU_ALLOC_SN		0x90	/* index 63, used for SN grants */
#define ONU_INSTANCE		0xc0	/* four 6-bit instances per word */
#define ONU_ALLOC_VALID		BIT(12)
#define ONU_INSTANCE_FREE	0x28
#define ONU_RES_RESET		0x6c	/* GMAC_res_reset writes 0; never read */

/* Optical TX block */
#define TX_PREAMBLE		0x08	/* T1 17:13, T2 12:8, T3 length 7:0 */
#define TX_T3_PATTERN		0x0c
#define TX_DELIMITER_LEN	0x10
#define TX_DELIMITER		0x14
#define TX_OVERHEAD_COMMIT	0x18
#define TX_PLOAM		0x1c	/* three big-endian words */
#define TX_PLOAM_GO		0x28
#define TX_SN_DELAY		0x38
#define TX_SN_EXTRA		0x54	/* Upstream_Overhead ss: extra SN transmissions */
#define TX_PLOAM_CTRL		0x58
#define TX_DBRU_TCONT		0x6c	/* T-CONT whose report is rewritten, bits 5:0 */
#define TX_DBRU_LEVEL		0x70
#define TX_DBRU_LOAD		0x74
#define TX_DBRU_CLEAR		0x84
#define TX_DBRU_ERR		0x108	/* bit n: T-CONT n's report is stuck */
#define TX_QUEUE_LEVEL(n)	(0x200 + 4 * (n))

/* Optical RX block: downstream PLOAM mailbox */
#define RX_PLOAM		0x60
#define RX_PLOAM_CRC		0x6c
#define RX_PLOAM_VALID		0x70

/* AES block */
#define AES_KEY			0x44
#define AES_SWITCH		0x54
#define AES_HANG		0x94	/* non-zero: the MAC hung (chipset_check_gmac) */
#define AES_STATE_A		0x9c	/* must agree with AES_STATE_B (chipset_check_gmac2) */
#define AES_STATE_B		0xac

#define GPON_BROADCAST		0xff
#define GPON_ALLOC_SN		0xfe
#define GPON_TCONT_SN		63

/* Downstream PLOAM message IDs */
#define DN_UPSTREAM_OVERHEAD	0x01
#define DN_ASSIGN_ONU_ID	0x03
#define DN_RANGING_TIME		0x04
#define DN_DEACTIVATE_ONU	0x05
#define DN_DISABLE_SN		0x06
#define DN_ENCRYPTED_PORT	0x08
#define DN_REQUEST_PASSWORD	0x09
#define DN_ASSIGN_ALLOC_ID	0x0a
#define DN_POPUP		0x0c
#define DN_REQUEST_KEY		0x0d
#define DN_CONFIGURE_PORT	0x0e
#define DN_PEE			0x0f
#define DN_BER_INTERVAL		0x12
#define DN_KEY_SWITCH_TIME	0x13
#define DN_EXTENDED_BURST	0x14
#define DN_VENDOR		0x81

/* Upstream PLOAM message IDs */
#define UP_PASSWORD		0x02
#define UP_ENCRYPTION_KEY	0x05
#define UP_ACK			0x09
#define UP_REI			0x08

enum { O1 = 1, O2, O3, O4, O5, O6, O7 };

static struct gc1601_pon *gpon_to_pon(struct gc1601_gpon *g)
{
	return container_of(g, struct gc1601_pon, gpon);
}

static void __iomem *onu_reg(struct gc1601_pon *pon, u32 off)
{
	return pon->mac + PON_ONU + off;
}

static void __iomem *tx_reg(struct gc1601_pon *pon, u32 off)
{
	return pon->mac + PON_OPT_TX + off;
}

/* ---- hardware accessors (chipset_*) ---- */

static void gc1601_gpon_set_onu_id(struct gc1601_pon *pon, u8 id)
{
	writel((readl(onu_reg(pon, 0)) & ~PON_ONU_ID) | id, onu_reg(pon, 0));
	WRITE_ONCE(pon->gpon.onu_id, id);
	if (id != GPON_BROADCAST)
		gc1601_xpon_report_onu_id(pon, id);
}

static int gc1601_gpon_set_alloc(struct gc1601_pon *pon, unsigned int index, u16 id)
{
	void __iomem *reg;
	u32 val;
	int ret;

	if (index == GPON_TCONT_SN) {
		reg = onu_reg(pon, ONU_ALLOC_SN);
		writel((readl(reg) & ~0x1fff) | (id & 0xfff) | ONU_ALLOC_VALID, reg);
		return 0;
	}
	if (test_bit(index, pon->gpon.tcont_dirty)) {
		ret = gc1601_xpon_clear_sfu(pon);
		if (!ret)
			ret = gc1601_pe_clear_tcont(index);
		if (ret)
			return ret;
		clear_bit(index, pon->gpon.tcont_dirty);
	}
	reg = onu_reg(pon, ONU_ALLOC + 4 * (index / 2));
	val = readl(reg);
	if (index % 2)
		val = (val & ~(0x1fff << 16)) | ((id & 0xfff) | ONU_ALLOC_VALID) << 16;
	else
		val = (val & ~0x1fff) | (id & 0xfff) | ONU_ALLOC_VALID;
	writel(val, reg);
	pon->gpon.alloc[index] = id;
	return gc1601_pe_enable_tcont(index);
}

static void gc1601_gpon_set_instance(struct gc1601_pon *pon, unsigned int index, u8 inst)
{
	void __iomem *reg = onu_reg(pon, ONU_INSTANCE + 4 * (index / 4));
	unsigned int shift = 8 * (index % 4);

	writel((readl(reg) & ~(0x3f << shift)) | (inst & 0x3f) << shift, reg);
}

static void gc1601_gpon_del_alloc(struct gc1601_pon *pon, unsigned int index)
{
	void __iomem *reg;
	u16 alloc_id;
	int ret, err;

	if (index == GPON_TCONT_SN) {
		reg = onu_reg(pon, ONU_ALLOC_SN);
		writel(readl(reg) & ~ONU_ALLOC_VALID, reg);
		return;
	}
	reg = onu_reg(pon, ONU_ALLOC + 4 * (index / 2));
	writel(readl(reg) & ~(0x1fff << (16 * (index % 2))), reg);
	gc1601_gpon_set_instance(pon, index, ONU_INSTANCE_FREE);
	alloc_id = pon->gpon.alloc[index];
	WRITE_ONCE(pon->gpon.alloc[index], U16_MAX);
	/* Stop grants before flushing and removing the old scheduling state. */
	ret = gc1601_xpon_withdraw_alloc(pon, alloc_id);
	err = gc1601_pe_clear_tcont(index);
	ret = ret ?: err;
	assign_bit(index, pon->gpon.tcont_dirty, ret != 0);
	if (ret)
		dev_warn(pon->dev, "T-CONT %u cleanup failed: %d\n", index, ret);
}

static int gc1601_gpon_find_alloc(struct gc1601_gpon *g, u16 id)
{
	int i;

	for (i = 0; i < GPON_TCONTS; i++)
		if (READ_ONCE(g->alloc[i]) == id)
			return i;
	return -ENOENT;
}

static void gc1601_gpon_set_gem(struct gc1601_pon *pon, u16 port, bool enable, bool aes)
{
	aes |= test_bit(port, pon->gpon.gem_aes);
	writel((enable ? BIT(0) : 0) | (enable && aes ? BIT(1) : 0),
	       pon->mac + PON_GEM + 4 * port);
	if (enable)
		set_bit(port, pon->gpon.gem);
	else
		clear_bit(port, pon->gpon.gem);
}

static void gc1601_gpon_set_omcc(struct gc1601_pon *pon, u16 port, bool valid)
{
	void __iomem *reg = pon->mac + PON_OMCC;

	writel((readl(reg) & ~(0xfff << 4)) | (port & 0xfff) << 4, reg);
	WRITE_ONCE(pon->gpon.omcc_port, port);
	WRITE_ONCE(pon->gpon.omcc_valid, valid);
	gc1601_xpon_report_channel(pon, port, valid);
}

/* Back to an unprovisioned ONU: no Alloc-IDs, GEM ports or OMCI channel. */
static void gc1601_gpon_clear_resources(struct gc1601_pon *pon)
{
	struct gc1601_gpon *g = &pon->gpon;
	unsigned int i, port;

	for (i = 0; i < GPON_TCONTS; i++)
		if (g->alloc[i] != U16_MAX || test_bit(i, g->tcont_dirty))
			gc1601_gpon_del_alloc(pon, i);
	gc1601_gpon_del_alloc(pon, GPON_TCONT_SN);
	bitmap_zero(g->gem_aes, 4096);
	g->key_valid = false;
	for_each_set_bit(port, g->gem, 4096)
		gc1601_gpon_set_gem(pon, port, false, false);
	if (g->omcc_valid)
		gc1601_gpon_set_omcc(pon, 0, false);
}

static void gc1601_gpon_set_eqd(struct gc1601_pon *pon, u32 bits)
{
	u32 bytes = bits >> 3;
	u32 val = readl(onu_reg(pon, ONU_EQD)) & GENMASK(31, 24);

	val |= (bits & 7) << 21 | (bytes / GPON_FRAME_BYTES & 0x1f) << 16 |
	       bytes % GPON_FRAME_BYTES;
	writel(val, onu_reg(pon, ONU_EQD));
}

static void gc1601_gpon_set_t3(struct gc1601_pon *pon)
{
	struct gc1601_gpon *g = &pon->gpon;
	u8 len;

	if (!g->have_burst)
		return;
	len = g->state >= O5 && g->state <= O6 ? g->burst_t3[1] : g->burst_t3[0];
	writel((readl(tx_reg(pon, TX_PREAMBLE)) & ~0xff) | len, tx_reg(pon, TX_PREAMBLE));
	writel(1, tx_reg(pon, TX_OVERHEAD_COMMIT));
}

/* chipset_dn_plm_hook for Upstream_Overhead; the binary uses 24-bit delimiters. */
static void gc1601_gpon_program_overhead(struct gc1601_pon *pon, const u8 *d)
{
	u32 pre = readl(tx_reg(pon, TX_PREAMBLE));
	u32 val;

	pre &= ~(GENMASK(17, 13) | GENMASK(12, 8));
	pre |= (d[1] & 0x1f) << 13 | (d[2] & 0x1f) << 8;
	writel(pre, tx_reg(pon, TX_PREAMBLE));
	writel(d[3], tx_reg(pon, TX_T3_PATTERN));
	writel(3, tx_reg(pon, TX_DELIMITER_LEN));
	writel(get_unaligned_be24(d + 4), tx_reg(pon, TX_DELIMITER));

	if (d[7] & BIT(5)) {
		u32 bytes = get_unaligned_be16(d + 8) * 32;

		val = readl(onu_reg(pon, ONU_PRE_DELAY)) & ~GENMASK(20, 0);
		val |= (bytes / GPON_FRAME_BYTES & 0x1f) << 16 | bytes % GPON_FRAME_BYTES;
		writel(val, onu_reg(pon, ONU_PRE_DELAY));
	}
	/* Octet 10 is xxemsspp; pp (power level mode) is not applied. */
	if (d[7] & GENMASK(3, 2))
		writel(d[7] >> 2 & 3, tx_reg(pon, TX_SN_EXTRA));
	writel(1, tx_reg(pon, TX_OVERHEAD_COMMIT));
}

/* ---- diagnostics module hooks ---- */

static struct gc1601_pon *gc1601_gpon_pon;
const struct gc1601_gpon_diag_ops __rcu *gc1601_gpon_diag;
static DEFINE_MUTEX(gc1601_gpon_diag_lock);

struct gc1601_pon *gc1601_gpon_diag_register(const struct gc1601_gpon_diag_ops *ops)
{
	struct gc1601_pon *pon = READ_ONCE(gc1601_gpon_pon);

	if (!pon)
		return ERR_PTR(-ENODEV);
	mutex_lock(&gc1601_gpon_diag_lock);
	if (rcu_access_pointer(gc1601_gpon_diag)) {
		mutex_unlock(&gc1601_gpon_diag_lock);
		return ERR_PTR(-EBUSY);
	}
	rcu_assign_pointer(gc1601_gpon_diag, ops);
	mutex_unlock(&gc1601_gpon_diag_lock);
	return pon;
}
EXPORT_SYMBOL_GPL(gc1601_gpon_diag_register);

void gc1601_gpon_diag_unregister(void)
{
	mutex_lock(&gc1601_gpon_diag_lock);
	RCU_INIT_POINTER(gc1601_gpon_diag, NULL);
	mutex_unlock(&gc1601_gpon_diag_lock);
	synchronize_rcu();
}
EXPORT_SYMBOL_GPL(gc1601_gpon_diag_unregister);

/* chipset_send_plm. The only place a PLOAM is transmitted. */
/*
 * Addressed downstream and all upstream PLOAMs, which the broadcast flood
 * pushes out of ploam_log within a second. Written with pon->lock held.
 */
#define GC1601_PLOAM_TRACE	64

static struct {
	u64 ns;
	char dir;	/* D received, U sent, T upstream timed out */
	u8 msg[12];
} gc1601_ploam_trace[GC1601_PLOAM_TRACE];
static unsigned int gc1601_ploam_trace_pos;

static void gc1601_ploam_trace_add(char dir, const u8 *msg)
{
	unsigned int i = gc1601_ploam_trace_pos++ % GC1601_PLOAM_TRACE;

	gc1601_ploam_trace[i].ns = ktime_get_ns();
	gc1601_ploam_trace[i].dir = dir;
	memcpy(gc1601_ploam_trace[i].msg, msg, sizeof(gc1601_ploam_trace[i].msg));
}

static int gc1601_gpon_send(struct gc1601_pon *pon, u8 id, const u8 *data)
{
	struct gc1601_gpon *g = &pon->gpon;
	u8 msg[12] = { g->onu_id, id };
	u32 ctrl, go;
	int ret;

	memcpy(msg + 2, data, 10);
	if (!g->allow_tx || g->olt_tx_disabled) {
		g->tx_refused++;
		GPON_DIAG(ploam_tx, pon, msg, -EPERM);
		return -EPERM;
	}

	writel(get_unaligned_be32(msg), tx_reg(pon, TX_PLOAM));
	writel(get_unaligned_be32(msg + 4), tx_reg(pon, TX_PLOAM + 4));
	writel(get_unaligned_be32(msg + 8), tx_reg(pon, TX_PLOAM + 8));

	ctrl = readl(tx_reg(pon, TX_PLOAM_CTRL));
	switch (id) {
	case 1: case 2: case 3: case 5:
		ctrl |= BIT(7) | BIT(6);
		break;
	case 4:
		break;
	default:
		ctrl = (ctrl & ~BIT(7)) | BIT(6);
		break;
	}
	writel(ctrl, tx_reg(pon, TX_PLOAM_CTRL));
	writel(1, tx_reg(pon, TX_PLOAM_GO));

	ret = readl_poll_timeout(tx_reg(pon, TX_PLOAM_GO), go, !go, 100, 100000);
	gc1601_ploam_trace_add(ret ? 'T' : 'U', msg);
	if (ret)
		dev_err(pon->dev, "upstream PLOAM %02x timed out\n", id);
	else
		g->ploam_tx++;
	GPON_DIAG(ploam_tx, pon, msg, ret);
	return ret;
}

/* Acknowledge: the downstream ID and its first nine bytes, from O4 on. */
static void gc1601_gpon_ack(struct gc1601_pon *pon, const u8 *msg)
{
	u8 data[10];

	if (pon->gpon.state < O4)
		return;
	data[0] = msg[1];
	memcpy(data + 1, msg, 9);
	gc1601_gpon_send(pon, UP_ACK, data);
}

/* ---- state machine ---- */

/*
 * The stock SDK resets the whole chip (sdk_top_level_soft_reset) when the
 * MAC hangs. That pulse also resets the packet engine Ethernet runs on, so
 * here the GPON MAC alone is set up again and activation starts over.
 * Called with pon->lock held.
 */
/* GMAC_res_reset, on O2, O6, O7 and MAC re-initialisation. */
static void gc1601_gpon_res_reset(struct gc1601_pon *pon)
{
	writel(0, onu_reg(pon, ONU_RES_RESET));
}

static void gc1601_gpon_mac_reinit(struct gc1601_pon *pon)
{
	gc1601_gpon_res_reset(pon);
	gc1601_pon_gmac_init(pon);
	pon->gpon.key_valid = false;
	if (pon->gpon.allow_tx && !pon->gpon.olt_tx_disabled)
		gc1601_pon_tx_on(pon);
}

/* local_pre_handle_o1 / chipset_check_gmac2. Called with pon->lock held. */
static void gc1601_gpon_mac_check(struct gc1601_pon *pon)
{
	struct gc1601_gpon *g = &pon->gpon;
	u32 a, b;

	if (!gpon_watchdog || !g->running)
		return;
	/*
	 * Both are running frame counters that lag each other by one at
	 * times: only a difference that stays while neither moves counts.
	 */
	a = readl(pon->mac + PON_AES + AES_STATE_A);
	b = readl(pon->mac + PON_AES + AES_STATE_B);
	if (!((a ^ b) & 0xffff))
		return;
	udelay(500);
	if (readl(pon->mac + PON_AES + AES_STATE_A) != a ||
	    readl(pon->mac + PON_AES + AES_STATE_B) != b)
		return;
	g->mac_mismatches++;
	dev_warn(pon->dev, "GPON MAC state mismatch on O1 (%08x/%08x): re-initialising the MAC\n",
		 a, b);
	gc1601_gpon_mac_reinit(pon);
}

static void gc1601_gpon_timer_start(struct gc1601_gpon *g, unsigned int ms)
{
	mod_delayed_work(system_wq, &g->timer, msecs_to_jiffies(ms));
}

static void gc1601_gpon_timer_stop(struct gc1601_gpon *g)
{
	/* Called with pon->lock held; the timer work takes it, so never wait. */
	cancel_delayed_work(&g->timer);
}

/*
 * chipset_get_gpon_stats: every counter clears on read and the stock SDK
 * sums them. Names for the FEC words come from its FEC PM ioctl
 * (gcpon_gponGetFecStats, G.988 order); the rest have no name in any stock
 * binary and are shown by block and offset. Upstream bytes wrap within
 * about 34 s at line rate, so O5 samples them every second.
 */
enum {
	CNT_FEC_CORR_BYTES, CNT_FEC_CORR_CW, CNT_FEC_UNCORR_CW, CNT_FEC_TOTAL_CW,
	CNT_BIP_ERRORS, CNT_BIP_BLOCKS, CNT_UP_BYTES,
};

static const struct {
	const char *name;
	u32 off;
} gc1601_gpon_counters[GPON_COUNTERS] = {
	[CNT_FEC_CORR_BYTES]	= { "fec_corrected_bytes", PON_OPT_RX + 0x40 },
	[CNT_FEC_CORR_CW]	= { "fec_corrected_codewords", PON_OPT_RX + 0x44 },
	[CNT_FEC_UNCORR_CW]	= { "fec_uncorrectable_codewords", PON_OPT_RX + 0x48 },
	[CNT_FEC_TOTAL_CW]	= { "fec_total_codewords", PON_OPT_RX + 0x4c },
	[CNT_BIP_ERRORS]	= { "bip_errors", PON_OPT_RX + 0x58 },
	[CNT_BIP_BLOCKS]	= { "bip_blocks", PON_OPT_RX + 0x5c },
	[CNT_UP_BYTES]		= { "upstream_bytes", PON_COUNTERS + COUNTER_UP_BYTES },
	{ "counters_0x160", PON_COUNTERS + 0x160 },
	{ "omcc_0x18", PON_OMCC + 0x18 },
	{ "omcc_0x1c", PON_OMCC + 0x1c },
	{ "omcc_0x7c", PON_OMCC + 0x7c },
	{ "omcc_0xdc", PON_OMCC + 0xdc },
	{ "omcc_0xe0", PON_OMCC + 0xe0 },
	{ "omcc_0xe4", PON_OMCC + 0xe4 },
	{ "omcc_0xe8", PON_OMCC + 0xe8 },
	{ "omcc_0xec", PON_OMCC + 0xec },
	{ "omcc_0xf0", PON_OMCC + 0xf0 },
	{ "omcc_0xf4", PON_OMCC + 0xf4 },
	{ "omcc_0xf8", PON_OMCC + 0xf8 },
};

/* Read one counter into its total; returns what this read added. Lock held. */
static u32 gc1601_gpon_sample(struct gc1601_pon *pon, unsigned int i)
{
	u32 val = readl(pon->mac + gc1601_gpon_counters[i].off);

	pon->gpon.counter[i] += val;
	return val;
}

static void gc1601_gpon_sample_all(struct gc1601_pon *pon)
{
	unsigned int i;

	for (i = 0; i < GPON_COUNTERS; i++)
		gc1601_gpon_sample(pon, i);
}

/* Downstream FEC totals so far, for the OMCI agent's FEC PM. */
void gc1601_gpon_fec(struct gc1601_pon *pon, struct omci_telemetry *t)
{
	const u64 *c = pon->gpon.counter;

	t->fec_corrected_bytes = READ_ONCE(c[CNT_FEC_CORR_BYTES]);
	t->fec_corrected_codewords = READ_ONCE(c[CNT_FEC_CORR_CW]);
	t->fec_uncorrectable_codewords = READ_ONCE(c[CNT_FEC_UNCORR_CW]);
	t->fec_total_codewords = READ_ONCE(c[CNT_FEC_TOTAL_CW]);
	t->fec_seconds = READ_ONCE(pon->gpon.fec_seconds);
}

/* Downstream BIP errors so far, for the OMCI agent's SF/SD alarms. */
u64 gc1601_gpon_bip_errors(struct gc1601_pon *pon)
{
	return READ_ONCE(pon->gpon.counter[CNT_BIP_ERRORS]);
}

static unsigned long gc1601_gpon_rei_delay(u32 frames)
{
	return max_t(unsigned long, msecs_to_jiffies(20),
		     min_t(u64, nsecs_to_jiffies64((u64)frames * 125000),
			   MAX_JIFFY_OFFSET));
}

static void gc1601_gpon_rei(struct work_struct *work)
{
	struct gc1601_gpon *g = container_of(to_delayed_work(work),
					  struct gc1601_gpon, rei);
	struct gc1601_pon *pon = gpon_to_pon(g);
	u8 data[10] = {};
	u32 errors;

	mutex_lock(&pon->lock);
	if (!g->running || g->state != O5 || !g->ber_interval_frames)
		goto out;
	errors = gc1601_gpon_sample(pon, CNT_BIP_ERRORS);
	gc1601_gpon_sample(pon, CNT_BIP_BLOCKS);
	put_unaligned_be32(errors, data);
	data[4] = g->rei_sequence & 0xf;
	if (!gc1601_gpon_send(pon, UP_REI, data)) {
		g->rei_sent++;
		g->rei_sequence = (g->rei_sequence + 1) & 0xf;
	} else {
		g->rei_failed++;
	}
	mod_delayed_work(system_wq, &g->rei,
			 gc1601_gpon_rei_delay(g->ber_interval_frames));
out:
	mutex_unlock(&pon->lock);
}

static void gc1601_gpon_retain(struct gc1601_gpon *g)
{
	/* A second outage can arrive before encrypted GEMs have been replayed. */
	if (g->retained_valid)
		bitmap_or(g->retained_aes, g->retained_aes, g->gem_aes, 4096);
	else
		bitmap_copy(g->retained_aes, g->gem_aes, 4096);
	g->retained_omcc = g->omcc_port;
	g->retained_valid = true;
}

static void gc1601_gpon_enter(struct gc1601_pon *pon, int state)
{
	struct gc1601_gpon *g = &pon->gpon;
	int prev = g->state;
	u32 val;

	g->state = state;
	GPON_DIAG(state, pon, prev, state);
	if (state != O5) {
		cancel_delayed_work(&g->rei);
		cancel_delayed_work(&g->recovery);
		cancel_delayed_work(&g->watchdog);	/* the work takes pon->lock: never wait */
	}
	switch (state) {
	case O1:
		g->ber_interval_frames = 0;
		gc1601_gpon_mac_check(pon);
		gc1601_gpon_timer_stop(g);
		gc1601_gpon_clear_resources(pon);
		gc1601_gpon_set_onu_id(pon, GPON_BROADCAST);
		g->ranged = false;
		g->have_overhead = false;
		break;
	case O2:
		g->ber_interval_frames = 0;
		gc1601_gpon_timer_stop(g);
		gc1601_gpon_res_reset(pon);
		gc1601_gpon_clear_resources(pon);
		gc1601_gpon_set_onu_id(pon, GPON_BROADCAST);
		g->ranged = false;
		break;
	case O3:
		/* local_pre_handle_o3: answer SN grants on the special Alloc-ID. */
		gc1601_gpon_set_alloc(pon, GPON_TCONT_SN, GPON_ALLOC_SN);
		writel(0, tx_reg(pon, TX_SN_DELAY));
		writel(get_random_u32_below(233), onu_reg(pon, ONU_RANDOM_DELAY));
		gc1601_gpon_timer_start(g, GPON_ACTIVATION_MS);
		break;
	case O4:
		/* TO1 bounds O3 and O4 together; only a broadcast POPUP starts it here. */
		if (prev != O3)
			gc1601_gpon_timer_start(g, GPON_ACTIVATION_MS);
		break;
	case O5:
		gc1601_gpon_timer_stop(g);
		g->ranged = true;
		g->dbru_last = 0;
		if (g->ber_interval_frames)
			mod_delayed_work(system_wq, &g->rei,
					 gc1601_gpon_rei_delay(g->ber_interval_frames));
		mod_delayed_work(system_wq, &g->watchdog, msecs_to_jiffies(GPON_WATCHDOG_MS));
		g->recovery_attempt = 0;
		if (g->retained_valid)
			mod_delayed_work(system_wq, &g->recovery,
					 msecs_to_jiffies(GPON_RECOVERY_MS));
		break;
	case O6:
		if (prev == O5 && g->omcc_valid)
			gc1601_gpon_retain(g);
		gc1601_gpon_res_reset(pon);
		gc1601_gpon_timer_start(g, GPON_POPUP_MS);
		break;
	case O7:
		g->retained_valid = false;
		/* local_pre_handle_o7: stop everything, drop the provisioning. */
		g->ber_interval_frames = 0;
		gc1601_gpon_timer_stop(g);
		gc1601_gpon_res_reset(pon);
		gc1601_gpon_clear_resources(pon);
		gc1601_pon_tx_off(pon);
		break;
	}

	/* chipset_gpon_state_change_hook: hardware state is the O-number. */
	val = readl(onu_reg(pon, 0)) & ~PON_ONU_STATE;
	writel(val | FIELD_PREP(PON_ONU_STATE, state), onu_reg(pon, 0));
	gc1601_gpon_set_t3(pon);

	/* Leaving an activated state ends the OMCI session the OLT held. */
	if ((state == O1 || state == O2) && prev >= O3)
		gc1601_xpon_reset_session(pon);
	gc1601_xpon_report_state(pon, state);

	if (prev != state)
		dev_info(pon->dev, "GPON O%d -> O%d\n", prev, state);
}

static bool gc1601_gpon_sn_match(struct gc1601_gpon *g, const u8 *msg)
{
	return !memcmp(msg + 3, g->serial, sizeof(g->serial));
}

/* Answering serial number grants in O3 is the ONU's first transmission. */
static bool gc1601_gpon_may_range(struct gc1601_gpon *g)
{
	return g->allow_tx && !g->olt_tx_disabled && !g->identity_wait;
}

static void gc1601_gpon_overhead(struct gc1601_pon *pon, const u8 *data)
{
	struct gc1601_gpon *g = &pon->gpon;
	bool changed = !g->have_overhead || memcmp(g->overhead, data, sizeof(g->overhead));

	memcpy(g->overhead, data, sizeof(g->overhead));
	g->have_overhead = true;
	gc1601_gpon_program_overhead(pon, data);
	if (changed)
		dev_info(pon->dev,
			 "Upstream_Overhead: guard %u, preamble %u/%u bits pattern %02x, delimiter %02x%02x%02x, flags %02x, pre-assigned delay %u\n",
			 data[0], data[1], data[2], data[3], data[4], data[5], data[6],
			 data[7], get_unaligned_be16(data + 8));

	if (gc1601_gpon_may_range(g))
		gc1601_gpon_enter(pon, O3);
	else if (changed)
		dev_info(pon->dev, "staying in O2: %s\n",
			 g->allow_tx ? "waiting for the ONU identity" : "allow_tx is off");
}

/* G.984.3 Amd. 2 9.2.3.5: octets 3-4 carry a reason; an aware ONU only logs it. */
static void gc1601_gpon_deactivate(struct gc1601_pon *pon, const u8 *msg)
{
	dev_info(pon->dev, "Deactivate_ONU-ID%s, reason %#06x\n",
		 msg[0] == GPON_BROADCAST ? " (broadcast)" : "", get_unaligned_be16(msg + 2));
	pon->gpon.retained_valid = false;
	gc1601_gpon_enter(pon, O2);
}

static void gc1601_gpon_disable_sn(struct gc1601_pon *pon, const u8 *msg)
{
	struct gc1601_gpon *g = &pon->gpon;

	if (msg[0] != GPON_BROADCAST)
		return;
	if (g->state == O7) {
		/* Enable: 0x0f for everyone, 0x00 for a matching serial number. */
		if (msg[2] == 0x0f || (msg[2] == 0x00 && gc1601_gpon_sn_match(g, msg))) {
			if (g->allow_tx && !g->olt_tx_disabled)
				gc1601_pon_tx_on(pon);
			gc1601_gpon_enter(pon, O2);
		}
		return;
	}
	if (msg[2] == 0xff && gc1601_gpon_sn_match(g, msg))
		gc1601_gpon_enter(pon, O7);
}

static void gc1601_gpon_encrypted_port(struct gc1601_pon *pon, const u8 *msg)
{
	u16 port = msg[3] << 4 | msg[4] >> 4;

	if (msg[2] & BIT(1)) {
		assign_bit(port, pon->gpon.gem_aes, msg[2] & BIT(0));
		assign_bit(port, pon->gpon.retained_aes, msg[2] & BIT(0));
		gc1601_gpon_omci_set_gem(pon, port, test_bit(port, pon->gpon.gem), false);
		gc1601_xpon_report_alloc(pon);
	}
	gc1601_gpon_ack(pon, msg);
}

static void gc1601_gpon_assign_alloc(struct gc1601_pon *pon, const u8 *msg)
{
	struct gc1601_gpon *g = &pon->gpon;
	u16 id = msg[2] << 4 | msg[3] >> 4;
	bool changed = false, removed = false;
	int index;

	index = gc1601_gpon_find_alloc(g, id);
	if (msg[4] == 1 && index < 0) {
		/* The default allocation (ID = ONU-ID) is T-CONT 0, instance 0. */
		if (id == g->onu_id) {
			index = 0;
		} else {
			for (index = 1; index < GPON_TCONTS; index++)
				if (g->alloc[index] == U16_MAX)
					break;
		}
		if (index < GPON_TCONTS) {
			if (gc1601_gpon_set_alloc(pon, index, id)) {
				dev_warn(pon->dev, "T-CONT %u still needs cleanup\n", index);
				goto ack;
			}
			changed = true;
			/*
			 * Bind each T-CONT to the packet-engine instance of the
			 * same number: with the unused marker the MAC has no
			 * queue to burst from on the grants of this Alloc-ID.
			 */
			gc1601_gpon_set_instance(pon, index, index);
		} else {
			dev_warn(pon->dev, "no free T-CONT for Alloc-ID %u\n", id);
		}
	} else if (msg[4] == 0xff && index > 0) {
		gc1601_gpon_del_alloc(pon, index);
		changed = true;
		removed = true;
	}
	if (changed) {
		gc1601_xpon_report_alloc(pon);
		if (removed)
			gc1601_xpon_sfu_kick(pon);
	}
ack:
	gc1601_gpon_ack(pon, msg);
}

static void gc1601_gpon_configure_port(struct gc1601_pon *pon, const u8 *msg)
{
	struct gc1601_gpon *g = &pon->gpon;
	u16 port = msg[3] << 4 | msg[4] >> 4;

	if (msg[2] == 1) {
		gc1601_gpon_set_gem(pon, port, true, false);
		gc1601_gpon_set_omcc(pon, port, true);
		dev_info(pon->dev, "OMCI channel on GEM port %u\n", port);
	} else if (msg[2] == 0) {
		if (g->retained_valid && g->retained_omcc == port)
			g->retained_valid = false;
		gc1601_gpon_set_gem(pon, port, false, false);
		if (g->omcc_valid && g->omcc_port == port)
			gc1601_gpon_set_omcc(pon, 0, false);
	}
	gc1601_gpon_ack(pon, msg);
}

static void gc1601_gpon_request_key(struct gc1601_pon *pon)
{
	struct gc1601_gpon *g = &pon->gpon;
	u8 key[16], data[10];
	int i;

	/* The vendor used prandom here; a link key needs real randomness. */
	g->key_valid = false;
	get_random_bytes(key, sizeof(key));
	for (i = 0; i < 2; i++) {
		data[0] = g->key_index;
		data[1] = i;
		memcpy(data + 2, key + 8 * i, 8);
		if (gc1601_gpon_send(pon, UP_ENCRYPTION_KEY, data)) {
			memzero_explicit(key, sizeof(key));
			return;
		}
	}
	writel(get_unaligned_be32(key), pon->mac + PON_AES + AES_KEY);
	writel(get_unaligned_be32(key + 4), pon->mac + PON_AES + AES_KEY + 4);
	writel(get_unaligned_be32(key + 8), pon->mac + PON_AES + AES_KEY + 8);
	writel(get_unaligned_be32(key + 12), pon->mac + PON_AES + AES_KEY + 12);
	g->key_index++;
	g->key_valid = true;
	gc1601_xpon_report_alloc(pon);
	memzero_explicit(key, sizeof(key));
}

static void gc1601_gpon_handle(struct gc1601_pon *pon, const u8 *msg)
{
	struct gc1601_gpon *g = &pon->gpon;
	u8 onu = msg[0], id = msg[1];
	bool addressed = onu == g->onu_id && onu != GPON_BROADCAST;
	bool bcast = onu == GPON_BROADCAST;

	if (id == DN_DISABLE_SN) {
		gc1601_gpon_disable_sn(pon, msg);
		return;
	}
	if (id == DN_EXTENDED_BURST && bcast) {
		g->burst_t3[0] = msg[2];
		g->burst_t3[1] = msg[3];
		g->have_burst = true;
		gc1601_gpon_set_t3(pon);
		return;
	}
	if (id == DN_VENDOR && gc1601_gpon_sn_match(g, msg)) {
		if (msg[2] == 0x11) {
			g->olt_tx_disabled = true;
			gc1601_pon_tx_off(pon);
		} else if (msg[2] == 0x0f && g->allow_tx && !g->identity_wait && !pon->tx_hold &&
			   g->state != O7 && pon->bosa_state == GC1601_BOSA_READY) {
			if (!gc1601_bosa_soft_tx(pon, true)) {
				g->olt_tx_disabled = false;
				gc1601_pon_tx_on(pon);
			}
		}
		return;
	}

	switch (g->state) {
	case O2:
		if (id == DN_UPSTREAM_OVERHEAD && bcast)
			gc1601_gpon_overhead(pon, msg + 2);
		break;
	case O3:
		if (id == DN_ASSIGN_ONU_ID && gc1601_gpon_sn_match(g, msg)) {
			if (gc1601_gpon_set_alloc(pon, 0, msg[2]))
				break;
			gc1601_gpon_set_onu_id(pon, msg[2]);
			gc1601_gpon_set_instance(pon, 0, 0);
			gc1601_gpon_del_alloc(pon, GPON_TCONT_SN);
			dev_info(pon->dev, "assigned ONU-ID %u\n", msg[2]);
			gc1601_gpon_enter(pon, O4);
		}
		break;
	case O4:
	case O5:
		if (id == DN_DEACTIVATE_ONU && (bcast || addressed)) {
			gc1601_gpon_deactivate(pon, msg);
			break;
		}
		if (!addressed && !(bcast && (id == DN_PEE || id == DN_BER_INTERVAL ||
					      id == DN_KEY_SWITCH_TIME)))
			break;
		switch (id) {
		case DN_RANGING_TIME:
			/* Byte 2 selects the path; 0 is the main path. */
			if (msg[2] == 0) {
				gc1601_gpon_set_eqd(pon, get_unaligned_be32(msg + 3));
				if (g->state == O4) {
					dev_info(pon->dev, "ranged, EqD %u bits\n",
						 get_unaligned_be32(msg + 3));
					gc1601_gpon_enter(pon, O5);
				}
			}
			break;
		case DN_ENCRYPTED_PORT:
			gc1601_gpon_encrypted_port(pon, msg);
			break;
		case DN_REQUEST_PASSWORD:
			gc1601_gpon_send(pon, UP_PASSWORD, g->password);
			break;
		case DN_ASSIGN_ALLOC_ID:
			if (g->state == O5)
				gc1601_gpon_assign_alloc(pon, msg);
			break;
		case DN_REQUEST_KEY:
			if (g->state == O5)
				gc1601_gpon_request_key(pon);
			break;
		case DN_CONFIGURE_PORT:
			if (g->state == O5)
				gc1601_gpon_configure_port(pon, msg);
			break;
		case DN_PEE:
			/* The OLT's own equipment error: note it, the ONU has none to report. */
			if (g->state == O5)
				g->olt_pee++;
			break;
		case DN_BER_INTERVAL:
			if (g->state == O5) {
				g->ber_interval_frames = get_unaligned_be32(msg + 2);
				cancel_delayed_work(&g->rei);
				if (g->ber_interval_frames)
					mod_delayed_work(system_wq, &g->rei,
						gc1601_gpon_rei_delay(g->ber_interval_frames));
				gc1601_gpon_ack(pon, msg);
			}
			break;
		case DN_KEY_SWITCH_TIME:
			if (g->state == O5)
				gc1601_gpon_ack(pon, msg);
			break;
		}
		break;
	case O6:
		if (id == DN_DEACTIVATE_ONU && (bcast || addressed)) {
			gc1601_gpon_deactivate(pon, msg);
		} else if (id == DN_POPUP && (bcast || addressed)) {
			if (addressed && g->ranged) {
				gc1601_gpon_enter(pon, O5);
			} else {
				gc1601_gpon_enter(pon, O4);
			}
		}
		break;
	}
}

static void gc1601_gpon_sync(struct gc1601_pon *pon, bool sync)
{
	struct gc1601_gpon *g = &pon->gpon;

	if (sync) {
		if (g->state == O1)
			gc1601_gpon_enter(pon, O2);
		return;
	}
	switch (g->state) {
	case O2:
	case O3:
	case O4:
		gc1601_gpon_enter(pon, O1);
		break;
	case O5:
		gc1601_gpon_enter(pon, O6);
		break;
	}
}

/* ---- polling and work ---- */

/* ITU-T G.984.3 Clause 9.2: CRC-8 with polynomial x^8 + x^2 + x + 1 (0x07). */
static u8 gc1601_gpon_crc8(const u8 *data, size_t len)
{
	u8 crc = 0;
	size_t i, j;

	for (i = 0; i < len; i++) {
		crc ^= data[i];
		for (j = 0; j < 8; j++) {
			if (crc & 0x80)
				crc = (crc << 1) ^ 0x07;
			else
				crc <<= 1;
		}
	}
	return crc;
}

/* Poll context: move one mailbox entry into the ring. */
static bool gc1601_gpon_read_ploam(struct gc1601_pon *pon)
{
	struct gc1601_gpon *g = &pon->gpon;
	void __iomem *rx = pon->mac + PON_OPT_RX;
	u32 valid = readl(rx + RX_PLOAM_VALID);
	struct gc1601_ploam *p;
	u8 onu;

	if (!(valid & BIT(0)))
		return false;

	if (g->head - g->tail >= GPON_PLOAM_LOG) {
		g->dropped++;
	} else {
		p = &g->ring[g->head % GPON_PLOAM_LOG];
		p->ns = ktime_get_ns();
		p->generation = g->generation;
		put_unaligned_be32(readl(rx + RX_PLOAM), p->msg);
		put_unaligned_be32(readl(rx + RX_PLOAM + 4), p->msg + 4);
		put_unaligned_be32(readl(rx + RX_PLOAM + 8), p->msg + 8);
		p->msg[12] = readl(rx + RX_PLOAM_CRC);
		if (gc1601_gpon_crc8(p->msg, 12) != p->msg[12]) {
			g->ploam_crc_errors++;
			writel(valid & ~BIT(0), rx + RX_PLOAM_VALID);
			return true;
		}
		g->head++;

		/* Key switch time is applied at once, as the vendor IRQ path does. */
		onu = p->msg[0];
		if (p->msg[1] == DN_KEY_SWITCH_TIME &&
		    (onu == GPON_BROADCAST || onu == READ_ONCE(g->onu_id))) {
			void __iomem *sw = pon->mac + PON_AES + AES_SWITCH;
			u32 val = readl(sw) & BIT(31);

			writel(val | BIT(30) | (get_unaligned_be32(p->msg + 2) & GENMASK(29, 0)),
			       sw);
		}
	}
	writel(valid & ~BIT(0), rx + RX_PLOAM_VALID);
	return true;
}

static enum hrtimer_restart gc1601_gpon_poll(struct hrtimer *t)
{
	struct gc1601_gpon *g = container_of(t, struct gc1601_gpon, poll);
	struct gc1601_pon *pon = gpon_to_pon(g);
	u32 status = readl(pon->mac + PON_OPT_RX + PON_OPT_RX_STATUS);
	bool los = status & PON_OPT_RX_LOS;
	bool lof = status & PON_OPT_RX_LOF;
	bool kick = false;
	int i;

	spin_lock(&g->lock);
	/* GMAC_int_handle bit assignments; count observations, not exact grants. */
	g->mac_irq_last = readl(pon->mac + PON_IRQ);
	g->sn_grant_samples += !!(g->mac_irq_last & PON_IRQ_SN_GRANT);
	g->sn_tx_samples += !!(g->mac_irq_last & PON_IRQ_SN_TX);
	g->ranging_grant_samples += !!(g->mac_irq_last & PON_IRQ_RANGING_GRANT);
	g->ranging_tx_samples += !!(g->mac_irq_last & PON_IRQ_RANGING_TX);
	g->ploam_irq_samples += !!(g->mac_irq_last & PON_IRQ_PLOAM);
	g->fifo_error_samples += !!(g->mac_irq_last & PON_IRQ_FIFO_ERROR);
	if (los != g->los || lof != g->lof) {
		if ((los && !g->los) || (lof && !g->lof)) {
			g->loss_pending = true;
			g->generation++;
		}
		g->los_events += los && !g->los;
		g->lof_events += lof && !g->lof;
		g->los = los;
		g->lof = lof;
		kick = true;
	}
	/* The mailbox holds one message; drain what arrived since the last tick. */
	for (i = 0; i < 4 && gc1601_gpon_read_ploam(pon); i++)
		kick = true;
	spin_unlock(&g->lock);

	if (kick)
		schedule_work(&g->work);

	hrtimer_forward_now(t, ns_to_ktime(GPON_POLL_NS));
	return HRTIMER_RESTART;
}

/* Consume loss before PLOAMs, even when the sampled signal is good again. */
static void gc1601_gpon_signal_update(struct gc1601_pon *pon, bool *sync)
{
	struct gc1601_gpon *g = &pon->gpon;
	bool loss, los, lof;

	spin_lock_irq(&g->lock);
	loss = g->loss_pending;
	g->loss_pending = false;
	los = g->los;
	lof = g->lof;
	spin_unlock_irq(&g->lock);
	*sync = !los && !lof;
	if (g->running) {
		if (loss)
			gc1601_gpon_sync(pon, false);
		gc1601_gpon_sync(pon, *sync);
		gc1601_xpon_report_signal(pon, los, lof);
	}
}

static void gc1601_gpon_work(struct work_struct *w)
{
	struct gc1601_gpon *g = container_of(w, struct gc1601_gpon, work);
	struct gc1601_pon *pon = gpon_to_pon(g);
	struct gc1601_ploam p;
	bool sync, pending;
	unsigned int budget;
	u32 generation;

	mutex_lock(&pon->lock);
	gc1601_gpon_signal_update(pon, &sync);
	spin_lock_irq(&g->lock);
	budget = min(g->head - g->tail, (unsigned int)GPON_PLOAM_LOG);
	spin_unlock_irq(&g->lock);
	/* New arrivals must not indefinitely hold the control and timer lock. */
	while (budget--) {
		gc1601_gpon_signal_update(pon, &sync);
		spin_lock_irq(&g->lock);
		if (g->loss_pending || g->tail == g->head) {
			spin_unlock_irq(&g->lock);
			break;
		}
		generation = g->generation;
		sync = !g->los && !g->lof;
		p = g->ring[g->tail % GPON_PLOAM_LOG];
		g->tail++;
		spin_unlock_irq(&g->lock);

		g->log[g->log_pos++ % GPON_PLOAM_LOG] = p;
		g->ploam_rx++;
		GPON_DIAG(ploam_rx, pon, p.msg);
		if (!sync || p.generation != generation) {
			spin_lock_irq(&g->lock);
			g->dropped++;
			spin_unlock_irq(&g->lock);
			continue;
		}

		/*
		 * The OLT sends a PLOAM in several consecutive frames and one poll
		 * drains them together. Act once: a repeated Request_Key would
		 * otherwise answer with a new key each time.
		 */
		if (p.generation == g->last.generation &&
		    p.ns - g->last.ns < GPON_PLOAM_REPEAT_NS &&
		    !memcmp(p.msg, g->last.msg, sizeof(p.msg))) {
			g->ploam_repeats++;
			continue;
		}
		g->last = p;
		if (g->running) {
			if (p.msg[0] != GPON_BROADCAST)
				gc1601_ploam_trace_add('D', p.msg);
			gc1601_gpon_handle(pon, p.msg);
		}
	}
	gc1601_gpon_signal_update(pon, &sync);
	spin_lock_irq(&g->lock);
	pending = g->head != g->tail || g->loss_pending;
	spin_unlock_irq(&g->lock);
	if (pending && g->running)
		schedule_work(&g->work);
	mutex_unlock(&pon->lock);
}

/*
 * chipset_GMAC_dbru_chk: a T-CONT whose upstream report is stuck while no
 * data goes up gets its reported queue level lowered and the report
 * reloaded, with the queue manager's almost-full level raised meanwhile.
 * Only an idle upstream qualifies. Log the first recovery pass and every
 * 100th after, without claiming that clearing a flag restored traffic. Called with
 * pon->lock held, once a second in O5.
 */
static void gc1601_gpon_dbru_check(struct gc1601_pon *pon)
{
	struct gc1601_gpon *g = &pon->gpon;
	struct gc1601_dbru_stats stats = g->dbru_stats;
	u32 stuck = readl(tx_reg(pon, TX_DBRU_ERR));
	unsigned long now = jiffies;
	unsigned int t, ms;
	u32 bytes, rate, rem;

	/* The watchdog sampled the counters just before. */
	bytes = g->counter[CNT_UP_BYTES] - g->dbru_bytes;
	g->dbru_bytes = g->counter[CNT_UP_BYTES];
	if (!stuck) {
		g->dbru_last = now;
		return;
	}
	stats.detected++;
	if (!g->dbru_last) {
		g->dbru_last = now;
		goto out;
	}
	ms = jiffies_to_msecs(now - g->dbru_last) ?: 1;
	g->dbru_last = now;
	rate = div_u64((u64)bytes * 8, ms);
	if (rate >= 2) {
		stats.busy++;
		goto out;
	}
	stats.before = stuck;
	stats.rate = rate;
	stats.rewritten = 0;
	stats.skipped = 0;

	for (t = 1; t < GPON_TCONTS; t++) {
		u32 level;
		int old;

		if (!(stuck & BIT(t)))
			continue;
		if (gc1601_pe_free_buffers() <= DBRU_FREE_MIN) {
			stats.buffer_skips++;
			stats.skipped |= BIT(t);
			continue;
		}
		old = gc1601_pe_qmu_aful(10);
		if (old < 0)
			stats.qmu_errors++;
		writel((readl(tx_reg(pon, TX_DBRU_TCONT)) & ~0x7f) | t, tx_reg(pon, TX_DBRU_TCONT));
		level = readl(tx_reg(pon, TX_QUEUE_LEVEL(t)));
		writel(level > DBRU_LEVEL_DROP ? level - DBRU_LEVEL_DROP : 0,
		       tx_reg(pon, TX_DBRU_LEVEL));
		writel(1, tx_reg(pon, TX_DBRU_LOAD));
		stats.rewrites++;
		stats.rewritten |= BIT(t);
		if (gc1601_pe_qmu_aful(old >= 0 ? old : 2) < 0)
			stats.qmu_errors++;
	}
	writel(1, tx_reg(pon, TX_DBRU_CLEAR));
	writel(0, tx_reg(pon, TX_DBRU_CLEAR));
	stats.after = readl(tx_reg(pon, TX_DBRU_ERR));
	if (stats.after & stuck)
		stats.flag_persists++;
	else
		stats.flag_clears++;
	div_u64_rem(stats.passes++, 100, &rem);
	if (!rem)
		dev_info(pon->dev, "GPON DBRu: flagged=%08x rewritten=%08x skipped=%08x after=%08x, %llu passes\n",
			 stuck, stats.rewritten, stats.skipped, stats.after, stats.passes);
out:
	spin_lock_irq(&g->lock);
	g->dbru_stats = stats;
	spin_unlock_irq(&g->lock);
}

/* local_handle_O5 timers 3 and 4: MAC hang and DBRu, once a second. */
static void gc1601_gpon_watchdog(struct work_struct *w)
{
	struct gc1601_gpon *g = container_of(to_delayed_work(w), struct gc1601_gpon, watchdog);
	struct gc1601_pon *pon = gpon_to_pon(g);
	u32 hang;

	mutex_lock(&pon->lock);
	if (!g->running || g->state != O5)
		goto out;
	gc1601_gpon_sample_all(pon);
	if (g->counter[CNT_FEC_UNCORR_CW] != g->fec_uncorr_seen) {
		g->fec_uncorr_seen = g->counter[CNT_FEC_UNCORR_CW];
		g->fec_seconds++;
	}
	if (gpon_watchdog) {
		hang = readl(pon->mac + PON_AES + AES_HANG);
		if (hang) {
			g->mac_hangs++;
			dev_warn(pon->dev, "GPON MAC hung (%08x): re-initialising it and ranging again\n",
				 hang);
			gc1601_gpon_mac_reinit(pon);
			gc1601_gpon_enter(pon, O1);
			goto out;
		}
		gc1601_gpon_dbru_check(pon);
	}
	mod_delayed_work(system_wq, &g->watchdog, msecs_to_jiffies(GPON_WATCHDOG_MS));
out:
	mutex_unlock(&pon->lock);
}

static void gc1601_gpon_recovery(struct work_struct *w)
{
	struct gc1601_gpon *g = container_of(to_delayed_work(w),
						 struct gc1601_gpon, recovery);
	struct gc1601_pon *pon = gpon_to_pon(g);

	mutex_lock(&pon->lock);
	if (!g->running || g->state != O5 || !g->retained_valid ||
	    !g->allow_tx || g->olt_tx_disabled || g->los || g->lof || !pon->xp.odev)
		goto out;
	g->recovery_attempt++;
	g->recovery_runs++;
	if (!g->omcc_valid) {
		gc1601_gpon_set_gem(pon, g->retained_omcc, true, false);
		gc1601_gpon_set_omcc(pon, g->retained_omcc, true);
	}
	/* omcid replays its MIB on the event and reports operational itself. */
	omci_device_reconcile_services(pon->xp.odev);
out:
	mutex_unlock(&pon->lock);
}

static void gc1601_gpon_timer(struct work_struct *w)
{
	struct gc1601_gpon *g = container_of(to_delayed_work(w), struct gc1601_gpon, timer);
	struct gc1601_pon *pon = gpon_to_pon(g);

	mutex_lock(&pon->lock);
	if (g->running) {
		if (g->state == O3 || g->state == O4) {
			dev_info(pon->dev, "activation timed out in O%d\n", g->state);
			gc1601_gpon_enter(pon, O2);
		} else if (g->state == O6) {
			gc1601_gpon_enter(pon, O1);
			if (!g->los && !g->lof)
				gc1601_gpon_enter(pon, O2);
		}
	}
	mutex_unlock(&pon->lock);
}

/* ---- control ---- */

/* tx_hold: as the OLT's vendor transmitter-off, until released. Lock held. */
void gc1601_gpon_tx_hold(struct gc1601_pon *pon, bool hold)
{
	struct gc1601_gpon *g = &pon->gpon;

	if (hold) {
		g->olt_tx_disabled = true;
		gc1601_pon_tx_off(pon);
		return;
	}
	g->olt_tx_disabled = false;
	if (!g->running || !g->allow_tx || g->state == O7 ||
	    pon->bosa_state != GC1601_BOSA_READY || gc1601_bosa_soft_tx(pon, true))
		return;
	gc1601_pon_tx_on(pon);
	if (g->state == O2 && g->have_overhead && gc1601_gpon_may_range(g))
		gc1601_gpon_enter(pon, O3);
}

/* Called with pon->lock held. */
int gc1601_gpon_set_allow_tx(struct gc1601_pon *pon, bool allow)
{
	struct gc1601_gpon *g = &pon->gpon;
	int ret;

	if (!allow) {
		g->retained_valid = false;
		g->allow_tx = false;
		gc1601_pon_tx_off(pon);
		ret = gc1601_bosa_soft_tx(pon, false);
		if (g->state >= O3 && g->state != O7)
			gc1601_gpon_enter(pon, O2);
		return ret;
	}

	ret = gc1601_bosa_soft_tx(pon, true);
	if (ret) {
		g->allow_tx = false;
		gc1601_pon_tx_off(pon);
		dev_err(pon->dev, "BOSA soft TX enable failed: %d\n", ret);
		return ret;
	}
	g->allow_tx = true;
	if (g->state != O7 && !g->olt_tx_disabled)
		gc1601_pon_tx_on(pon);
	dev_warn(pon->dev, "transmitter allowed: the ONU will answer the OLT\n");
	if (g->state == O2 && g->have_overhead && gc1601_gpon_may_range(g))
		gc1601_gpon_enter(pon, O3);
	return 0;
}

/* Called with pon->lock held. */
void gc1601_gpon_start(struct gc1601_pon *pon)
{
	struct gc1601_gpon *g = &pon->gpon;

	if (g->running)
		return;
	if (!g->allow_tx)
		gc1601_pon_tx_off(pon);
	g->state = O1;
	g->retained_valid = false;
	gc1601_gpon_enter(pon, O1);
	spin_lock_irq(&g->lock);
	g->generation++;
	g->loss_pending = false;
	g->head = 0;
	g->tail = 0;
	memset(&g->last, 0, sizeof(g->last));
	g->los = true;
	g->lof = true;
	spin_unlock_irq(&g->lock);
	g->running = true;
	dev_info(pon->dev, "GPON serial number %.4s%08X\n", g->serial,
		 get_unaligned_be32(g->serial + 4));
	/* The poll reports only changes: start the LEDs from no signal. */
	gc1601_xpon_report_signal(pon, true, true);
	hrtimer_start(&g->poll, ns_to_ktime(GPON_POLL_NS), HRTIMER_MODE_REL);
	if (gc1601_xpon_start(pon))
		dev_warn(pon->dev, "OMCI transport did not start\n");
}

void gc1601_gpon_stop(struct gc1601_pon *pon)
{
	struct gc1601_gpon *g = &pon->gpon;

	if (!g->running)
		return;
	g->running = false;
	hrtimer_cancel(&g->poll);
	cancel_work_sync(&g->work);
	cancel_delayed_work_sync(&g->timer);
	cancel_delayed_work_sync(&g->watchdog);
	cancel_delayed_work_sync(&g->rei);
	cancel_delayed_work_sync(&g->recovery);
	cancel_delayed_work_sync(&g->identity_timer);
	g->identity_wait = false;
	g->retained_valid = false;
	gc1601_xpon_stop(pon);
}

/* Called with pon->lock held. */
static void gc1601_gpon_identity_release(struct gc1601_pon *pon)
{
	struct gc1601_gpon *g = &pon->gpon;

	g->identity_released = true;
	if (!g->identity_wait)
		return;
	g->identity_wait = false;
	dev_info(pon->dev, "GPON identity wait over\n");
	if (g->running && g->state == O2 && g->have_overhead && gc1601_gpon_may_range(g))
		gc1601_gpon_enter(pon, O3);
}

static void gc1601_gpon_identity_timeout(struct work_struct *w)
{
	struct gc1601_gpon *g = container_of(to_delayed_work(w), struct gc1601_gpon,
					     identity_timer);
	struct gc1601_pon *pon = gpon_to_pon(g);

	mutex_lock(&pon->lock);
	gc1601_gpon_identity_release(pon);
	mutex_unlock(&pon->lock);
}

/*
 * Boot-time start (galachip,gpon-autostart), receive-only until allow_tx.
 * With galachip,gpon-identity-wait-ms the first ranging also waits until
 * omci-agent writes 0 to gpon_identity_wait, or the wait runs out: the stock
 * "gpon startup 0", set the serial, "gpon startup 1" sequence.
 */
void gc1601_gpon_autostart(struct gc1601_pon *pon)
{
	struct gc1601_gpon *g = &pon->gpon;
	u32 ms = 0;

	device_property_read_u32(pon->dev, "galachip,gpon-identity-wait-ms", &ms);

	mutex_lock(&pon->lock);
	if (ms && !g->identity_released) {
		g->identity_wait = true;
		mod_delayed_work(system_wq, &g->identity_timer, msecs_to_jiffies(ms));
		dev_info(pon->dev, "GPON ranging waits up to %u ms for the ONU identity\n", ms);
	}
	gc1601_gpon_start(pon);
	mutex_unlock(&pon->lock);
}

/* ---- sysfs ---- */

static ssize_t gpon_enable_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;

	if (on) {
		mutex_lock(&pon->lock);
		if (pon->mode == GC1601_PON_GPON)
			gc1601_gpon_start(pon);
		else
			ret = -EBUSY;
		mutex_unlock(&pon->lock);
		if (ret)
			return ret;
	} else {
		/* The works take the lock; stop them before taking it here. */
		gc1601_gpon_stop(pon);
		mutex_lock(&pon->lock);
		gc1601_gpon_set_allow_tx(pon, false);
		gc1601_gpon_enter(pon, O1);
		mutex_unlock(&pon->lock);
	}
	return count;
}
static DEVICE_ATTR_WO(gpon_enable);

static ssize_t allow_tx_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;

	mutex_lock(&pon->lock);
	if (on && pon->bosa_state != GC1601_BOSA_READY) {
		dev_err(dev, "transmitter refused: the BOSA is not programmed\n");
		ret = -EAGAIN;
	} else if (pon->mode == GC1601_PON_EPON) {
		ret = gc1601_epon_set_allow_tx(pon, on);
	} else {
		ret = gc1601_gpon_set_allow_tx(pon, on);
	}
	mutex_unlock(&pon->lock);
	return ret ?: count;
}

static ssize_t allow_tx_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(pon->mode) == GC1601_PON_EPON ?
			  pon->epon.allow_tx : pon->gpon.allow_tx);
}
static DEVICE_ATTR_RW(allow_tx);

static ssize_t gpon_state_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	struct gc1601_gpon *g = &pon->gpon;
	ssize_t len;
	int i;

	mutex_lock(&pon->lock);
	len = sysfs_emit(buf,
			 "running=%d allow_tx=%d identity_wait=%d state=O%d onu_id=%u los=%d lof=%d\n"
			 "ploam_rx=%llu ploam_repeats=%llu ploam_crc_errors=%llu ploam_tx=%llu tx_refused=%llu dropped=%u los_events=%llu lof_events=%llu olt_pee=%llu\n"
			 "omcc_port=%u omcc_valid=%u gem_ports=%u omci_services=%u omci_rx_errors=%lu omci_crc_errors=%lu sfu_error=%d\n",
			 g->running, g->allow_tx, g->identity_wait, g->state, g->onu_id,
			 g->los, g->lof,
			 g->ploam_rx, g->ploam_repeats, g->ploam_crc_errors, g->ploam_tx, g->tx_refused, g->dropped,
			 g->los_events, g->lof_events, g->olt_pee, g->omcc_port, g->omcc_valid,
			 bitmap_weight(g->gem, 4096), pon->xp.services,
			 pon->xp.omci_rx_errors, pon->xp.omci_crc_errors,
			 READ_ONCE(pon->xp.sfu_last_error));
	len += sysfs_emit_at(buf, len,
		"omcc_hw=%08x omcc_gem_hw=%08x omci_rx_seen=%lu omci_rx_to_core=%lu omci_rx_alloc_failures=%lu\n"
		"omci_rx_last_len=%u omci_rx_last_header=%08x omci_rx_last_crc=%08x omci_rx_expected_crc=%08x\n"
		"omci_tx_attempts=%lu omci_tx_failures=%lu omci_tx_last_error=%d\n",
		readl(pon->mac + PON_OMCC),
		readl(pon->mac + PON_GEM + 4 * g->omcc_port),
		READ_ONCE(pon->xp.omci_rx_seen),
		READ_ONCE(pon->xp.omci_rx_to_core),
		READ_ONCE(pon->xp.omci_rx_alloc_failures),
		READ_ONCE(pon->xp.omci_rx_last_len),
		READ_ONCE(pon->xp.omci_rx_last_header),
		READ_ONCE(pon->xp.omci_rx_last_crc),
		READ_ONCE(pon->xp.omci_rx_expected_crc),
		READ_ONCE(pon->xp.omci_tx_attempts),
		READ_ONCE(pon->xp.omci_tx_failures),
		READ_ONCE(pon->xp.omci_tx_last_error));
	len += sysfs_emit_at(buf, len,
		"retained_valid=%u retained_omcc=%u key_valid=%u recovery_attempt=%u recovery_runs=%llu recovery_successes=%llu recovery_error=%d\n",
		g->retained_valid, g->retained_omcc, g->key_valid,
		g->recovery_attempt, g->recovery_runs, g->recovery_successes,
		g->recovery_error);
	len += sysfs_emit_at(buf, len, "generation=%u loss_pending=%u\n",
			     READ_ONCE(g->generation), READ_ONCE(g->loss_pending));
	len += sysfs_emit_at(buf, len, "alloc_ids=");
	for (i = 0; i < GPON_TCONTS; i++)
		if (g->alloc[i] != U16_MAX)
			len += sysfs_emit_at(buf, len, " %d:%u", i, g->alloc[i]);
	len += sysfs_emit_at(buf, len, "\n");
	if (g->have_overhead)
		len += sysfs_emit_at(buf, len, "overhead=%10phN\n", g->overhead);
	mutex_unlock(&pon->lock);
	return len;
}
static DEVICE_ATTR_RO(gpon_state);

static ssize_t gpon_events_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	struct gc1601_gpon *g = &pon->gpon;
	u64 sn_grant, sn_tx, ranging_grant, ranging_tx, ploam, fifo;
	unsigned int queued, dropped;
	struct gc1601_dbru_stats dbru;
	u64 hangs, mismatches;
	u32 irq;

	/* Stay readable even if activation work is blocked on pon->lock. */
	spin_lock_irq(&g->lock);
	irq = g->mac_irq_last;
	sn_grant = g->sn_grant_samples;
	sn_tx = g->sn_tx_samples;
	ranging_grant = g->ranging_grant_samples;
	ranging_tx = g->ranging_tx_samples;
	ploam = g->ploam_irq_samples;
	fifo = g->fifo_error_samples;
	queued = g->head - g->tail;
	dropped = g->dropped;
	dbru = g->dbru_stats;
	spin_unlock_irq(&g->lock);
	hangs = READ_ONCE(g->mac_hangs);
	mismatches = READ_ONCE(g->mac_mismatches);
	return sysfs_emit(buf,
			 "mac_irq=%08x sn_grant_samples=%llu sn_tx_samples=%llu ranging_grant_samples=%llu ranging_tx_samples=%llu ploam_irq_samples=%llu fifo_error_samples=%llu queued=%u dropped=%u\n"
			 "watchdog=%d mac_hangs=%llu mac_mismatches=%llu dbru_fixes=%llu\n"
			 "dbru_detected=%llu dbru_busy=%llu dbru_rewrites=%llu dbru_buffer_skips=%llu dbru_qmu_errors=%llu dbru_flag_clears=%llu dbru_flag_persists=%llu\n"
			 "dbru_last_before=%08x dbru_last_rewritten=%08x dbru_last_skipped=%08x dbru_last_after=%08x dbru_last_rate_kbps=%u\n",
			 irq, sn_grant, sn_tx, ranging_grant, ranging_tx, ploam, fifo,
			 queued, dropped, gpon_watchdog, hangs, mismatches, dbru.passes,
			 dbru.detected, dbru.busy, dbru.rewrites, dbru.buffer_skips,
			 dbru.qmu_errors, dbru.flag_clears, dbru.flag_persists,
			 dbru.before, dbru.rewritten, dbru.skipped, dbru.after, dbru.rate);
}
static DEVICE_ATTR_RO(gpon_events);

static ssize_t ploam_log_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	struct gc1601_gpon *g = &pon->gpon;
	unsigned int i, n;
	ssize_t len = 0;

	mutex_lock(&pon->lock);
	n = min_t(unsigned int, g->log_pos, GPON_PLOAM_LOG);
	for (i = g->log_pos - n; i < g->log_pos; i++) {
		struct gc1601_ploam *p = &g->log[i % GPON_PLOAM_LOG];
		u32 us;
		u64 sec = div_u64_rem(div_u64(p->ns, NSEC_PER_USEC), USEC_PER_SEC, &us);

		len += sysfs_emit_at(buf, len, "%llu.%06u %*phN\n", sec, us,
				     GPON_PLOAM_LEN, p->msg);
	}
	mutex_unlock(&pon->lock);
	return len;
}
static DEVICE_ATTR_RO(ploam_log);

/* chipset_set_sn: optical TX +0x30/+0x34, big-endian. Called with pon->lock. */
static void gc1601_gpon_write_serial(struct gc1601_pon *pon, const u8 *sn)
{
	memcpy(pon->gpon.serial, sn, sizeof(pon->gpon.serial));
	writel(get_unaligned_be32(sn), tx_reg(pon, 0x30));
	writel(get_unaligned_be32(sn + 4), tx_reg(pon, 0x34));
}

/*
 * The OLT knows the ONU by the serial number it ranged with: after a change,
 * leave the session so the new one ranges. Called with pon->lock held.
 */
static void gc1601_gpon_rerange(struct gc1601_pon *pon)
{
	struct gc1601_gpon *g = &pon->gpon;

	if (!g->running || g->state < O3 || g->state == O7)
		return;
	dev_info(pon->dev, "serial number changed in O%d: ranging again\n", g->state);
	gc1601_gpon_enter(pon, O2);
	if (g->have_overhead && gc1601_gpon_may_range(g))
		gc1601_gpon_enter(pon, O3);
}

void gc1601_gpon_set_identity_locked(struct gc1601_pon *pon, const u8 *serial,
				     const u8 *password)
{
	if (serial && memcmp(serial, pon->gpon.serial, sizeof(pon->gpon.serial))) {
		pon->gpon.retained_valid = false;
		gc1601_gpon_write_serial(pon, serial);
		/* Quiet at probe and in EPON mode: gc1601_gpon_start says it. */
		if (pon->gpon.running)
			dev_info(pon->dev, "GPON serial number %.4s%08X\n", serial,
				 get_unaligned_be32(serial + 4));
		gc1601_gpon_rerange(pon);
	}
	if (password && memcmp(password, pon->gpon.password, sizeof(pon->gpon.password))) {
		pon->gpon.retained_valid = false;
		memcpy(pon->gpon.password, password, sizeof(pon->gpon.password));
	}
}

void gc1601_gpon_set_identity(struct gc1601_pon *pon, const u8 *serial,
			      const u8 *password)
{
	mutex_lock(&pon->lock);
	gc1601_gpon_set_identity_locked(pon, serial, password);
	mutex_unlock(&pon->lock);
}

/* T-CONT, and so packet-engine instance, of an Alloc-ID the OLT assigned. */
int gc1601_gpon_alloc_index(struct gc1601_pon *pon, u16 alloc_id)
{
	return gc1601_gpon_find_alloc(&pon->gpon, alloc_id);
}

int gc1601_gpon_restore_service(struct gc1601_pon *pon, u16 gem_port, u16 alloc_id)
{
	struct gc1601_gpon *g = &pon->gpon;
	bool aes;
	int ret = 0;

	if (gem_port > 0xfff)
		return -ERANGE;
	spin_lock_bh(&pon->xp.service_lock);
	if (!READ_ONCE(g->running) || READ_ONCE(g->state) != O5 ||
	    READ_ONCE(g->los) || READ_ONCE(g->lof) ||
	    READ_ONCE(g->loss_pending) ||
	    gc1601_gpon_find_alloc(g, alloc_id) < 0) {
		ret = -EAGAIN;
		goto out;
	}
	aes = g->retained_valid && test_bit(gem_port, g->retained_aes);
	if (aes && !g->key_valid) {
		ret = -EAGAIN;
		goto out;
	}
	if (aes)
		set_bit(gem_port, g->gem_aes);
	gc1601_gpon_set_gem(pon, gem_port, true, aes);
out:
	spin_unlock_bh(&pon->xp.service_lock);
	return ret;
}

/*
 * From the OMCI agent, which runs without pon->lock. A GEM Port CTP create
 * keeps the encryption Encrypted_Port-ID set, whichever of the two came first.
 */
void gc1601_gpon_omci_set_gem(struct gc1601_pon *pon, u16 port, bool valid, bool aes)
{
	struct gc1601_gpon *g = &pon->gpon;

	if (valid && !(READ_ONCE(g->omcc_valid) && READ_ONCE(g->omcc_port) == port)) {
		valid = READ_ONCE(g->running) && READ_ONCE(g->state) == O5 &&
			!READ_ONCE(g->los) && !READ_ONCE(g->lof) && !READ_ONCE(g->loss_pending);
		if (g->retained_valid && test_bit(port, g->retained_aes)) {
			aes = true;
			valid = valid && g->key_valid;
			if (valid)
				set_bit(port, g->gem_aes);
		}
	}
	gc1601_gpon_set_gem(pon, port, valid, aes);
}

/* "VVVVSSSSSSSS": four vendor characters and eight hex digits. */
static ssize_t serial_number_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	char vendor[5], tail[9];
	u8 sn[8];
	int i;

	if (sscanf(buf, "%4c%8s", vendor, tail) != 2 || strlen(tail) != 8 ||
	    hex2bin(sn + 4, tail, 4))
		return -EINVAL;
	for (i = 0; i < 4; i++) {
		if (!isalnum(vendor[i]))
			return -EINVAL;
		sn[i] = vendor[i];
	}

	mutex_lock(&pon->lock);
	if (memcmp(sn, pon->gpon.serial, sizeof(sn))) {
		gc1601_gpon_write_serial(pon, sn);
		gc1601_gpon_rerange(pon);
	}
	mutex_unlock(&pon->lock);
	return count;
}

static ssize_t serial_number_show(struct device *dev, struct device_attribute *attr,
				  char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	const u8 *sn = pon->gpon.serial;

	return sysfs_emit(buf, "%.4s%08X\n", sn, get_unaligned_be32(sn + 4));
}
static DEVICE_ATTR_RW(serial_number);

/* Ten password bytes sent on Request_Password; shorter input is zero padded. */
static ssize_t password_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	size_t len = count;

	if (len && buf[len - 1] == '\n')
		len--;
	if (len > sizeof(pon->gpon.password))
		return -EINVAL;

	mutex_lock(&pon->lock);
	memset(pon->gpon.password, 0, sizeof(pon->gpon.password));
	memcpy(pon->gpon.password, buf, len);
	mutex_unlock(&pon->lock);
	return count;
}
static DEVICE_ATTR_WO(password);

/* Reads 1 while the first ranging waits; omci-agent writes 0 to end it. */
static ssize_t gpon_identity_wait_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(pon->gpon.identity_wait));
}

static ssize_t gpon_identity_wait_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	bool wait;
	int ret;

	ret = kstrtobool(buf, &wait);
	if (ret)
		return ret;
	if (wait)
		return -EINVAL;

	cancel_delayed_work(&pon->gpon.identity_timer);
	mutex_lock(&pon->lock);
	gc1601_gpon_identity_release(pon);
	mutex_unlock(&pon->lock);
	return count;
}
static DEVICE_ATTR_RW(gpon_identity_wait);

static ssize_t gpon_ber_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	struct gc1601_gpon *g = &pon->gpon;
	ssize_t len;

	mutex_lock(&pon->lock);
	len = sysfs_emit(buf, "interval_frames=%u sequence=%u sent=%llu failed=%llu "
			 "bip_errors=%llu bip_blocks=%llu\n",
			 g->ber_interval_frames, g->rei_sequence, g->rei_sent,
			 g->rei_failed, g->counter[CNT_BIP_ERRORS], g->counter[CNT_BIP_BLOCKS]);
	mutex_unlock(&pon->lock);
	return len;
}
static DEVICE_ATTR_RO(gpon_ber);

/* Totals since the driver loaded; read fresh in O5. */
static ssize_t pon_counters_show(struct device *dev, struct device_attribute *attr,
				 char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	struct gc1601_gpon *g = &pon->gpon;
	ssize_t len = 0;
	unsigned int i;

	mutex_lock(&pon->lock);
	if (g->running && g->state == O5)
		gc1601_gpon_sample_all(pon);
	for (i = 0; i < GPON_COUNTERS; i++)
		len += sysfs_emit_at(buf, len, "%s=%llu\n", gc1601_gpon_counters[i].name,
				     g->counter[i]);
	mutex_unlock(&pon->lock);
	return len;
}
static DEVICE_ATTR_RO(pon_counters);

/*
 * chipset_enable_tcont_stat: the counter block +0x50 holds four slots, each a
 * T-CONT index with bit 6 enabling it. Slot k counts at +0x80 + 4k (GEM
 * frames), +0xa0 + 4k (bytes) and +0xc0 + 4k (two 16-bit fields, the high
 * one following the grants). All are clear-on-read; totals are kept here.
 */
#define GC1601_TCONT_STAT_SEL	0x43424140	/* T-CONT 0..3 in slots 0..3 */

static ssize_t tcont_stats_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	static u64 frames[4], bytes[4], lo[4], hi[4];
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	void __iomem *cnt = pon->mac + PON_COUNTERS;
	ssize_t len = 0;
	unsigned int k;
	u32 v;

	mutex_lock(&pon->lock);
	if (readl(cnt + 0x50) != GC1601_TCONT_STAT_SEL)
		writel(GC1601_TCONT_STAT_SEL, cnt + 0x50);
	len += sysfs_emit_at(buf, len, "instances=%08x alloc=%08x %08x\n",
			     readl(pon->mac + PON_ONU + 0xc0),
			     readl(pon->mac + PON_ONU + 0x40),
			     readl(pon->mac + PON_ONU + 0x44));
	for (k = 0; k < 4; k++) {
		frames[k] += readl(cnt + 0x80 + 4 * k);
		bytes[k] += readl(cnt + 0xa0 + 4 * k);
		v = readl(cnt + 0xc0 + 4 * k);
		lo[k] += v & 0xffff;
		hi[k] += v >> 16;
		len += sysfs_emit_at(buf, len,
				     "tcont%u frames=%llu bytes=%llu c0_lo=%llu c0_hi=%llu\n",
				     k, frames[k], bytes[k], lo[k], hi[k]);
	}
	mutex_unlock(&pon->lock);
	return len;
}
static DEVICE_ATTR_RO(tcont_stats);

static ssize_t ploam_trace_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	unsigned int i, n;
	ssize_t len = 0;

	mutex_lock(&pon->lock);
	n = min_t(unsigned int, gc1601_ploam_trace_pos, GC1601_PLOAM_TRACE);
	for (i = gc1601_ploam_trace_pos - n; i != gc1601_ploam_trace_pos; i++) {
		unsigned int j = i % GC1601_PLOAM_TRACE;
		u32 us;
		u64 sec = div_u64_rem(div_u64(gc1601_ploam_trace[j].ns, NSEC_PER_USEC),
				      USEC_PER_SEC, &us);

		len += sysfs_emit_at(buf, len, "%llu.%06u %c %12phN\n", sec, us,
				     gc1601_ploam_trace[j].dir, gc1601_ploam_trace[j].msg);
	}
	mutex_unlock(&pon->lock);
	return len;
}
static DEVICE_ATTR_RO(ploam_trace);

static struct attribute *gc1601_gpon_attrs[] = {
	&dev_attr_pon_counters.attr,
	&dev_attr_ploam_trace.attr,
	&dev_attr_tcont_stats.attr,
	&dev_attr_gpon_enable.attr,
	&dev_attr_allow_tx.attr,
	&dev_attr_gpon_identity_wait.attr,
	&dev_attr_gpon_ber.attr,
	&dev_attr_gpon_state.attr,
	&dev_attr_gpon_events.attr,
	&dev_attr_ploam_log.attr,
	&dev_attr_serial_number.attr,
	&dev_attr_password.attr,
	NULL
};

const struct attribute_group gc1601_gpon_group = {
	.attrs = gc1601_gpon_attrs,
};

int gc1601_gpon_init(struct gc1601_pon *pon)
{
	struct gc1601_gpon *g = &pon->gpon;
	int i;

	spin_lock_init(&g->lock);
	WRITE_ONCE(gc1601_gpon_pon, pon);
	INIT_WORK(&g->work, gc1601_gpon_work);
	INIT_DELAYED_WORK(&g->timer, gc1601_gpon_timer);
	INIT_DELAYED_WORK(&g->watchdog, gc1601_gpon_watchdog);
	INIT_DELAYED_WORK(&g->rei, gc1601_gpon_rei);
	INIT_DELAYED_WORK(&g->recovery, gc1601_gpon_recovery);
	INIT_DELAYED_WORK(&g->identity_timer, gc1601_gpon_identity_timeout);
	hrtimer_setup(&g->poll, gc1601_gpon_poll, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	for (i = 0; i < GPON_TCONTS; i++)
		g->alloc[i] = U16_MAX;
	g->state = O1;
	g->onu_id = GPON_BROADCAST;
	g->los = true;
	g->lof = true;
	return 0;
}

void gc1601_gpon_exit(struct gc1601_pon *pon)
{
	WRITE_ONCE(gc1601_gpon_pon, NULL);
	gc1601_gpon_stop(pon);
	mutex_lock(&pon->lock);
	gc1601_gpon_set_allow_tx(pon, false);
	mutex_unlock(&pon->lock);
}
