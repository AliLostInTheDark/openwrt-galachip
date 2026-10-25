// SPDX-License-Identifier: GPL-2.0-or-later
/* TIER: FAMILY -- the Luna GPON MAC shell: registers, DMA, ...
 * dev/MEASURED-luna_gpon.c.md sec 1. */

#include <linux/delay.h>
#include <linux/leds.h>
#include <linux/firmware.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <gpon_omci_core.h>
#include <gpon_omci_me.h>
#include <gpon_omci_vlan.h>
#include <gpon_omci_diag.h>
#include <gpon_omci_trace.h>
#include <linux/net.h>
#include <gpon_data_plan.h>
#include <linux/slab.h>
#include <gpon_range_gate.h>
#include <gn25l95_cal_logic.h>
#include "gpon_sn.h"	/* the common G.984.3 ONU-SN codec */
#include "gpon_ploam.h"	/* the core PLOAM FSM + its shell contract */
#include "gpon_ploam_diag.h"	/* the core's activation diagnostic: WHAT to read at Assign/Ranging/Deact */
#include "gpon_gem_us.h"	/* GPON_GEM_US_RANGE_OK: the core's own bound predicate */
#include "luna_gpon_logic.h"	/* hoisted logic */
#include <gpon_ddm.h>	/* the common SFF-8472 A2h decode and the ANI-G unit */
#include "hwio.h"	/* flowcore: the ONE canonical field-mask RMW */
#include "gpon_gtc_ploam.h"	/* the core: the DS PLOAM buffer unpack, fed this shell's gpon_io */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/gfp.h>		/* __get_free_pages / GFP_KERNEL for the US PBO DRAM pool */
#include <linux/mm.h>
#include <linux/mm.h>		/* virt_to_phys */
#include <linux/kernel.h>
#include <linux/limits.h>	/* S32_MIN: the "no optical reading" sentinel */
#include <linux/string.h>	/* strscpy for the n/a fields in /proc/gpon */
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/ktime.h>	/* the far-end capture timestamps its records */
#include <linux/sched/clock.h>	/* local_clock: the GPON log's timestamps */
#include <linux/math64.h>	/* div_u64: ns -> us without a 64-bit divide */
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/of.h>
#include <linux/of_address.h>	/* of_address_to_resource: the board's reserved PBO pools */
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include "gpon_gem_diag.h"
#include "gpon_olt_diag.h"	/* the far-end capture: replayed off the board */
#include "gpon_mon.h"		/* the control plane as Ethernet frames for gponmon0 */
#include "gpon_lat.h"		/* gpon_lat_add(): the OMCI turnaround record */
#include <linux/etherdevice.h>
#include "luna_gpon_nic.h"
#include "luna_eth_regs.h"	/* SOC_SW_ENABLE + the family register map */
#include "luna_ponmac.h"		/* clean-room family PON-MAC/SerDes bring-up lib */

/* The GPON/protocol log: a ring read at /proc/gpon_log, never printk (operator,
 * 2026-10-07: dmesg is for the device itself). Protocol events run in the FSM
 * timer, where a console line cost the production OLT's Password deadline.
 * Lines keep the dmesg spelling, so a reader only changes its source. */
#define LUNA_LOG_BYTES	(32 * 1024)
static char luna_log_buf[LUNA_LOG_BYTES];
static unsigned int luna_log_head;
static DEFINE_SPINLOCK(luna_log_lock);

static __printf(1, 2) void luna_log_raw(const char *fmt, ...)
{
	u64 ns = local_clock();
	u32 us = do_div(ns, NSEC_PER_SEC) / NSEC_PER_USEC;
	unsigned long flags;
	char line[320];
	va_list ap;
	int n, i;

	n = scnprintf(line, sizeof(line), "[%5llu.%06u] ", ns, us);
	va_start(ap, fmt);
	n += vscnprintf(line + n, sizeof(line) - n, fmt, ap);
	va_end(ap);
	spin_lock_irqsave(&luna_log_lock, flags);
	for (i = 0; i < n; i++)
		luna_log_buf[luna_log_head++ % LUNA_LOG_BYTES] = line[i];
	spin_unlock_irqrestore(&luna_log_lock, flags);
}

#define luna_log(fmt, ...)	luna_log_raw("luna-gpon: " fmt, ##__VA_ARGS__)
#define GPON_UNSUP_LOG		luna_log_raw

/* Anything this driver receives and cannot place leaves ...
 * dev/MEASURED-luna_gpon.c.md sec 2. */
#define GPON_UNSUP_SUBSYS	"luna-gpon"
#include "gpon_unsup.h"		/* shared: the UNSUP report + its rate limit */

static int luna_log_show(struct seq_file *s, void *v)
{
	char *snap = kvmalloc(LUNA_LOG_BYTES, GFP_KERNEL);
	unsigned int head, len, i, start = 0;
	unsigned long flags;

	if (!snap)
		return -ENOMEM;
	spin_lock_irqsave(&luna_log_lock, flags);
	head = luna_log_head;
	len = min_t(unsigned int, head, LUNA_LOG_BYTES);
	for (i = 0; i < len; i++)
		snap[i] = luna_log_buf[(head - len + i) % LUNA_LOG_BYTES];
	spin_unlock_irqrestore(&luna_log_lock, flags);
	if (head > LUNA_LOG_BYTES)	/* the oldest line was overwritten in part */
		while (start < len && snap[start++] != '\n')
			;
	seq_write(s, snap + start, len - start);
	kvfree(snap);
	return 0;
}

#define GPON_PHYS_BASE	0x1b700000u

#include "luna_gpon_regs.h"	/* the per-SoC offsets; the logic below is chip-agnostic */
#define   GPON_BOH_LEN		12		/* stored bytes (TOTAL_OVERHEAD_BITS(96)/8); HW extends via REPEAT */
#define   GPON_BOH_MAX_LEN	252		/* hardware BOH_LENGTH field cap */

#define   GEM_US_PORT_MAP_STRIDE 4u		/* MUST be 4: the register array's
						 * declared "32" is the element BIT width, so the
						 * byte stride is 32/8 and flow 64 lands at 0x6500.
						 * A 0x20 stride writes 0x6C00 instead -- the
						 * per-T-CONT idle-byte STAT counter, live and
						 * non-zero on any online ONU, so write and readback
						 * agree on a wrong offset while the real port-map
						 * slot stays unmapped and the OMCC drains nothing. */
/* Compile-time guard: the stride must stay 4 (see above); catch any regression. */
static_assert(GEM_US_PORT_MAP_STRIDE == 4u,
	      "GEM_US_PORT_MAP stride MUST be 4 (32-bit words) per chipdef array-offset 32");
/* Each die's own GPON_OMCI_FLOW_ID (vendor chipdef, tier 3; confirmed live on the
 * G24W's stock at O5). Named per chip so the range assertions below stay
 * COMPILE-TIME -- a runtime table value cannot be static_assert'ed. */
#define GPON_OMCC_FLOW_9602C	64u
#define GPON_OMCC_FLOW_9603CVD	127u
#define GPON_OMCC_FLOW_9607C	127u
/* The live one, from the selected chip's table. `swc` is assigned before any GPON
 * register is touched, and every use of this macro sits after that. */
#define GPON_OMCC_FLOW		((unsigned int)swc->omcc_flow)
/* The OMCC's PHYSICAL queue id, and it is PER CHIP: 64 / 127 ...
 * dev/MEASURED-luna_gpon.c.md sec 3. */
#define GPON_OMCC_PHYS_QID	((unsigned int)swc->omcc_phys_qid)
#define GPON_OMCC_TCONT		16	/* the OMCC's T-CONT index; same on both dies */
#define GPON_DATA_TCONT		8	/* the WAN data GEM's own T-CONT; see GPON_DATA_ALLOC */

/* PLOAM message path (G.984.3 management channel driving the ...
 * dev/MEASURED-luna_gpon.c.md sec 4. */
#define GPON_GTC_US_ONU_ID_SHIFT 8		/* [15:8] OLT-assigned ONU-ID  */

/* SoC hardware I2C master (SWCORE register file). The ... -- dev/MEASURED-luna_gpon.c.md sec 5. */

/* SoC GPIO controller (its own register page at phys ... -- dev/MEASURED-luna_gpon.c.md sec 6. */
#define PI_US_SRAM_NO		127u		/* 128 pages - 1               */
#define PI_DS_SRAM_NO		31u		/* 32 pages - 1                */
#define PI_US_SRAM_RUNOUT	126u		/* SRAM_NO - 1                 */
#define PI_DS_SRAM_RUNOUT	30u

/* ONU activation FSM states (HW encodes the G.984.3 O-states directly). */
static const char * const gpon_onu_state_name[] = {
	[0] = "unknown",   [1] = "O1-initial",  [2] = "O2-standby",
	[3] = "O3-serial", [4] = "O4-ranging",  [5] = "O5-operation",
	[6] = "O6-popup",  [7] = "O7-emergency",
};

static void __iomem *gpon_base;
static void __iomem *swcore_base;
static void __iomem *ponip_base;

/* PLOAM activation FSM state (the FSM is defined below the proc dump). onu_sn is
 * empty until provisioned; the parameter is writable, re-parses on write, and the
 * FSM re-reads the parsed serial for each Serial_Number_ONU. */
static void gpon_parse_sn(const char *s);	/* defined below; re-parses onu_sn */
static bool gpon_sn_differs(const char *s);	/* defined below; parsed-byte compare */
/* NO COMPILED-IN SERIAL. This defaulted to "XPON39013867", ...
 * dev/MEASURED-luna_gpon.c.md sec 271. */
static char *onu_sn = "";
static DEFINE_MUTEX(bosa_lock);
static bool luna_driver_ready, luna_activation_ready, luna_stopping;
static bool bosa_gn_identified, bosa_cal_ready;
static int bosa_cal_error;
static int luna_optical_init_error;
static int luna_health_los = -1;
static struct proc_dir_entry *luna_health_proc;
static int luna_activate_locked(bool identity_changed);
static int luna_quiesce_locked(void);
static void luna_resume_poll(void);
static void luna_omci_identity_reset(const u8 sn[8]);
static void luna_data_suspend(void);
static void luna_omci_seq_show(struct seq_file *s);
static int luna_data_retire(void);
static bool ctrl_queue = true;
module_param(ctrl_queue, bool, 0444);
static bool luna_ctrl_flow_wanted(void);
MODULE_PARM_DESC(ctrl_queue, "CPU WAN control frames (PPP LCP/auth/IPCP, PPPoE discovery, ARP) on "
		 "their own flow, the data T-CONT's next queue (default on)");
static u8 luna_data_qid;
static bool gpon_sn_changed;		/* SN (re)provisioned -> FSM must re-range */

/* Forward declaration: the core FSM object is defined with ...
 * dev/MEASURED-luna_gpon.c.md sec 272. */
static struct gpon_ploam luna_ploam;
static u8 gpon_sn_bytes[8];		/* defined here for the same reason */

/* gponmon0: every PLOAM and OMCI PDU as an Ethernet frame for packet sockets
 * (tcpdump, Wireshark), never printk. Down by default; while down a PDU costs
 * one flag test. Control plane only: the GEM data path never passes here. */
static struct net_device *luna_mon;

static netdev_tx_t luna_mon_xmit(struct sk_buff *skb, struct net_device *dev)
{
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static const struct net_device_ops luna_mon_ops = {
	.ndo_start_xmit = luna_mon_xmit,
};

static void luna_mon_setup(struct net_device *dev)
{
	ether_setup(dev);
	dev->netdev_ops = &luna_mon_ops;
	/* Receive-only: no ARP, no IPv6 address, no multicast report of its own,
	 * so a capture holds the control plane and nothing else. */
	dev->flags = (dev->flags | IFF_NOARP) & ~IFF_MULTICAST;
	dev->priv_flags |= IFF_NO_QUEUE | IFF_NO_ADDRCONF;
}

static void luna_mon_frame(enum gpon_mon_proto proto, int upstream, const u8 *pdu, int len)
{
	struct net_device *dev = READ_ONCE(luna_mon);
	struct sk_buff *skb;

	if (!dev || !netif_running(dev) || !pdu || len <= 0)
		return;
	skb = netdev_alloc_skb(dev, GPON_MON_HDR + len);
	if (!skb) {
		dev_core_stats_rx_dropped_inc(dev);
		return;
	}
	gpon_mon_encap(skb_put(skb, GPON_MON_HDR + len), proto, upstream, pdu, len);
	skb->protocol = eth_type_trans(skb, dev);
	netif_rx(skb);
}

static void luna_mon_register(void)
{
	struct net_device *dev = alloc_netdev(0, "gponmon%d", NET_NAME_ENUM, luna_mon_setup);

	if (!dev) {
		pr_err("luna-gpon: gponmon netdev not allocated -- no control-plane capture\n");
		return;
	}
	eth_hw_addr_random(dev);
	if (register_netdev(dev)) {
		pr_err("luna-gpon: gponmon netdev not registered -- no control-plane capture\n");
		free_netdev(dev);
		return;
	}
	WRITE_ONCE(luna_mon, dev);
}

static void luna_mon_unregister(void)
{
	struct net_device *dev = luna_mon;

	if (!dev)
		return;
	WRITE_ONCE(luna_mon, NULL);
	unregister_netdev(dev);
	free_netdev(dev);
}

#ifdef CONFIG_GPON_OLT_DIAG
/* The PLOAM half of the far end's conversation: the OMCI ring's codec and
 * clock, in a ring of its own because the OMCI burst alone overflows that one. */
GPON_OLT_CAPTURE(luna_ploam_cap, GPON_OLT_RECORDS);
GPON_OLT_CAPTURE(luna_ploam_unknown, GPON_OLT_PLOAM_UNKNOWN_RECORDS);
static DEFINE_SPINLOCK(luna_ploam_cap_lock);
#endif

/* When the downstream PLOAM being handled landed: its record is timed from
 * arrival, as G.984.3's 750 us answer budget is. 0 outside a drain. */
static u64 luna_ds_rx_ns;
static u32 luna_ds_irqs, luna_ds_unsignalled, luna_ds_parks;	/* interrupts; drains the tick found unannounced; times the line went off */

static void luna_ploam_capture(enum gpon_olt_dir dir, const u8 *m, unsigned int len)
{
#ifdef CONFIG_GPON_OLT_DIAG
	unsigned long flags;

	spin_lock_irqsave(&luna_ploam_cap_lock, flags);
	gpon_olt_capture_add(&luna_ploam_cap,
			     div_u64(dir == GPON_OLT_DS && luna_ds_rx_ns ? luna_ds_rx_ns :
				     ktime_get_boottime_ns(), 1000),
			     (u8)luna_ploam.state, dir, m, len);
	spin_unlock_irqrestore(&luna_ploam_cap_lock, flags);
#endif
	luna_mon_frame(GPON_MON_PLOAM, dir == GPON_OLT_US, m, len);
}

static int onu_sn_set(const char *val, const struct kernel_param *kp)
{
	u8 parsed[8];
	bool changed;
	int ret;

	/* Validate before publishing either the string or the on-wire identity. */
	ret = gpon_sn_parse(val, parsed);
	if (ret || !gpon_sn_is_set(parsed))
		return -EINVAL;
	mutex_lock(&bosa_lock);
	changed = gpon_sn_differs(val);
	if (luna_stopping) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (changed && luna_driver_ready) {
		ret = luna_quiesce_locked();
		if (ret)
			goto out;
		/* Keep the old serial and model until its data ownership is gone.
		 * A failed retirement stays inhibited and retries on the next write. */
		ret = luna_data_retire();
		if (ret)
			goto out;
	}
	ret = param_set_charp(val, kp);
	if (ret)
		goto out;
	if (changed) {
		memcpy(gpon_sn_bytes, parsed, sizeof(parsed));
		gpon_sn_changed = true;
		gpon_ploam_set_sn(&luna_ploam, gpon_sn_bytes);
		luna_omci_identity_reset(gpon_sn_bytes);
	}
	/* Early parameters only latch identity. A failed late attempt is retryable
	 * with the same bytes; a healthy duplicate does not re-range. */
	if (luna_driver_ready)
		ret = luna_activate_locked(changed);
out:
	luna_resume_poll();
	mutex_unlock(&bosa_lock);
	return ret;
}
static const struct kernel_param_ops onu_sn_ops = {
	.set = onu_sn_set,
	.get = param_get_charp,
};
module_param_cb(onu_sn, &onu_sn_ops, &onu_sn, 0644);
MODULE_PARM_DESC(onu_sn, "ONU serial number (G.984.3 ONU-SN): 4 ASCII ID chars + 8 hex digits");

/* The G.984.3 Password stock sends: the unit's own MIB value, which gpon_provision writes
 * here, else stock's default (operator 2026-09-30: "mandar lo mismo que mandaria stock").
 * The FSM timer is the only writer of the core copy. */
static char *onu_password = "1234567890";
static u8 luna_pwd[GPON_PLOAM_PASSWORD_LEN];
static bool luna_pwd_dirty;
static u32 luna_us_ploam_full_drops;	/* urgent PLOAMs refused on a full queue */
static DEFINE_SPINLOCK(luna_pwd_lock);

static int onu_password_set(const char *val, const struct kernel_param *kp)
{
	u8 pwd[GPON_PLOAM_PASSWORD_LEN];
	int ret;

	if (gpon_ploam_password_parse(val, pwd))
		return -EINVAL;
	ret = param_set_charp(val, kp);
	if (ret)
		return ret;
	spin_lock_bh(&luna_pwd_lock);
	memcpy(luna_pwd, pwd, sizeof(pwd));
	luna_pwd_dirty = true;
	spin_unlock_bh(&luna_pwd_lock);
	return 0;
}
static const struct kernel_param_ops onu_password_ops = {
	.set = onu_password_set,
	.get = param_get_charp,
};
module_param_cb(onu_password, &onu_password_ops, &onu_password, 0644);
MODULE_PARM_DESC(onu_password, "G.984.3 PLOAM Password: up to 10 printable characters, or the 20 hex digits a vendor MIB may store, sent zero-padded (default: stock's 1234567890)");
/* Invalidate unused alloc-CAM entries with the stock CLEAN operation before
 * management activation. Keep the existing parameter name for compatibility;
 * Alloc-ID 0xfff is assignable and cannot serve as an invalid entry value. */
static bool alloc_cam_park = true;
module_param(alloc_cam_park, bool, 0644);
MODULE_PARM_DESC(alloc_cam_park, "clean unused GTC allocation entries before management activation");

/* Diagnostic: skip the BOSA cold-init so that, on a warm boot where the BOSA is
 * already working, the SoC datapath and FSM run on top of it. */
static bool skip_bosa;
module_param(skip_bosa, bool, 0444);
MODULE_PARM_DESC(skip_bosa, "leave external BOSA as-is (warm-boot bisection)");

/* Route the 9607C I2C-indirect hole (0xB0-0xD8) via the SMI proxy? The proxy
 * currently reads back 0xffffffff (broken), so default OFF = direct MMIO, to
 * test whether the window decodes directly like the 9602C. */
static bool i2c_proxy;
module_param(i2c_proxy, bool, 0644);

/* Set once at init from the DT compatible: the RTL9607C shares the family
 * PON-MAC/GTC/PLOAM core but uses the c7 rev-C SerDes path, needs the rev>A
 * PON-IP power bit, and has NO external BOSA (internal SerDes front-end). */
static bool is_9607c;
/* The THIRD chip. Stock treats the RTL9603CVD as its own silicon -- its own DAL,
 * its own register table, PON SerDes at SWCORE +0x040000 -- and this tree already
 * carries its table; see the note at the assignment. */
static bool is_9603cvd;

/* The panel LED controller, per die: the same block base and per-index
 * DATA_LED_CFG, nearly everything else moved (each die's chipdef; the block's
 * version word read on each board's stock). LEDn is parallel-enabled at bit
 * n + 1 and pad-enabled at bit n; force values pack 16 indices per word. */
struct luna_led_regs {
	u32 force_value;	/* 2 bits per index, 16 indices per word */
	u32 blink_rate;		/* the force-blink period field [lsb+2:lsb] */
	u8  blink_lsb;
	u32 para_en;
	u32 pad_en;
	u32 seri_para;		/* serial-mode enables in para_en, kept clear */
	u32 seri_pad;		/* serial-mode enable in pad_en, kept clear */
	u8  cpu_force_bit;	/* DATA_LED_CFG: the CPU drives the force value */
	u8  pon_type;		/* DATA_LED_CFG [20:16] source code of a PON lamp */
	u16 link_act;		/* DATA_LED_CFG: link + activity at every speed */
	u8  indices;
};

/* The SWCORE offsets that move between family members. The ...
 * dev/MEASURED-luna_gpon.c.md sec 7. */
struct gpon_swc_map {
	const char *chip;
	/* ★ THE OMCI FLOW/SID INDEX IS A PER-CHIP FACT, AND IT IS NOT ...
	 * dev/MEASURED-luna_gpon.c.md sec 8. */
	u8  omcc_flow;		/* GPON_OMCI_FLOW_ID -- per-chip, never shared */
	/* ★ PON_SCH_QMAP's ENTRY WIDTH, and it is a WIDTH, not an ...
	 * dev/MEASURED-luna_gpon.c.md sec 9. */
	u8  sch_qmap_bits;	/* PON_SCH_QMAP array-offset (bits per T-CONT entry) */
	/* ★ PON_SID_RPV_TH's ENTRY COUNT, and it is an ARRAY BOUND, ...
	 * dev/MEASURED-luna_gpon.c.md sec 10. */
	u16 sid_rpv_entries;	/* PON_SID_RPV_TH declared entries, per chipdef */
	/* ★ THE US CLASSIFY INDEX SPACE, WHICH IS A DIFFERENT NUMBER ...
	 * dev/MEASURED-luna_gpon.c.md sec 11. */
	u16 classify_sid_num;	/* US classify SIDs to invalidate, per chipdef */
	/* ★ A POINTER, NOT A SECOND COPY. This file needs two ...
	 * dev/MEASURED-luna_gpon.c.md sec 12. */
	const struct luna_sw_map *sw;
	/* The OMCC's PHYSICAL queue id -- 64 / 127 / 120, see GPON_OMCC_PHYS_QID. */
	u8  omcc_phys_qid;
	/* ★★★ THE QUEUE THE INVALIDATION PRE-PASS PARKS EVERY OTHER ...
	 * dev/MEASURED-luna_gpon.c.md sec 13. */
	u8  scratch_phys_qid;
	/* ★★★ THE T-CONT -> PHYSICAL QUEUE FORMULA, PER CHIP -- ...
	 * dev/MEASURED-luna_gpon.c.md sec 14. */
	u8  tcont_queue_max;
	u8  tcont_group;
	bool alloc_idx_swap;	/* RTL9603CVD logical T-CONT 4/7 and 5/11 permutation */
	/* ★★★ ACCEPT_MAX_LEN_CTRL IS NOT THE SAME FIELD ON THESE ...
	 * dev/MEASURED-luna_gpon.c.md sec 15. */
	u8  amax_msb;
	u16 amax_val;
	/* ★★★ HOW FAR THE LOOP GOES, PER DIE AND PER EVIDENCE ...
	 * dev/MEASURED-luna_gpon.c.md sec 16. */
	u8  amax_last_port;
	/* ★★★ AND THE PON PORT TAKES IT TOO -- on the dies where it ...
	 * dev/MEASURED-luna_gpon.c.md sec 17. */
	u16 amax_pon_val;
	/* ★ THE DOWNSTREAM PBO's PAGE POOL, per chip. 0 = SRAM-only, ...
	 * dev/MEASURED-luna_gpon.c.md sec 18. */
	u8  ds_dram_order;	/* pool order; 0 = keep the SRAM-only DS path       */
	u32 ds_dsc_cfg;		/* PON_DSC_CFG_DS   word, from that chip's stock     */
	u32 ds_dscrunout;	/* DSCRUNOUT_DS     word                            */
	u32 ds_fc_config;	/* PON_FC_CONFIG_DS word                            */
	u32 io_mode_en;		/* IO_MODE_EN                                  */
	u8  io_i2c_en_bus0;	/* IO_MODE_EN.I2C_EN low bit == I2C bus 0      */
	u8  io_oem_en;		/* IO_MODE_EN.OEM_EN bit (optical e-mode pads) */
	u32 io_gpio_en;		/* IO_GPIO_EN word 0; 0 = not declared here    */
	u32 sds_fib_status;	/* SDS_FIB_STATUS; 0 = not declared here       */
	u32 sds_reg0;		/* SDS_REG0    [1] SP_SDS_EN_RX                */
	u32 fib_reg16;		/* FIB_REG16   [10] FRC_SD [2] SEL_RX_SD       */
	u32 fib_ext_reg21;	/* FIB_EXT_REG21 [13] analog-ready             */
	u32 wsds_dig_18;	/* WSDS_DIG_18 [15:12] optic-LOS force + BEN_OE */
	/* The indirect I2C master, whose whole block sits +4 on the ...
	 * dev/MEASURED-luna_gpon.c.md sec 19. */
	u32 i2c_ind_wd;		/* I2C_IND_WD  [31:0] write data               */
	u32 i2c_ind_adr;	/* I2C_IND_ADR [31:0] target reg offset        */
	u32 i2c_ind_cmd;	/* I2C_IND_CMD [0]CMD_EN [1]RW_EN [2]BUSY [3]NACK */
	u32 i2c_ind_rd;		/* I2C_IND_RD  [31:0] read data                */
	/* The SerDes window base. MEASURED from the three chipdefs ...
	 * dev/MEASURED-luna_gpon.c.md sec 273. */
	u32 sds_win;		/* base of the SDS_ANA_* / WSDS_DIG_* window   */
	const struct luna_pi_move *pi_moves;	/* PON-IP relocations, NULL = none */
	/* SDS_CFG is NOT in that window and does NOT follow it: it is 0x1d0 /
	 * 0x200 / 0x270 on the three chips, so it gets its own field. */
	u32 sds_cfg;		/* SDS_CFG [4:0] SDS_MODE                      */
	/* Base of the six contiguous OMCI packet counters. ⚠ AND IT ...
	 * dev/MEASURED-luna_gpon.c.md sec 20. */
	u32 omci_cnt;		/* OMCI_DROP_PKT_CNT, the first of six         */
	/* ── the IRREGULAR movers ... -- dev/MEASURED-luna_gpon.c.md sec 21. */
	u32 sc_ind_wd;               /* SC_IND_WD */
	u32 wrap_gphy_misc;          /* WRAP_GPHY_MISC */
	u32 thermal_ctrl_0;          /* THERMAL_CTRL_0 */
	u32 force_p_ablty;           /* FORCE_P_ABLTY */
	u32 ablty_force_mode;        /* ABLTY_FORCE_MODE */
	u32 dyngasp_ctrl;            /* DYNGASP_CTRL */
	u32 accept_max_len_ctrl;     /* ACCEPT_MAX_LEN_CTRL */
	u32 pon_trap_cfg;            /* PON_TRAP_CFG */
	u32 cf_cfg;                  /* CF_CFG -- ⚠ NOT IN 9603cvd's map */
	u32 lut_unkn_uc_da_ctrl;     /* LUT_UNKN_UC_DA_CTRL */
	u32 lut_learn_over_ctrl;     /* LUT_LEARN_OVER_CTRL */
	u32 unkn_l2_mc;              /* UNKN_L2_MC */
	u32 unkn_ip4_mc;             /* UNKN_IP4_MC */
	u32 unkn_mc_cfg;             /* UNKN_MC_CFG */
	u32 lut_bc_flood;            /* LUT_BC_FLOOD */
	u32 lut_unkn_mc_flood;       /* LUT_UNKN_MC_FLOOD */
	u32 lut_unkn_uc_flood;       /* LUT_UNKN_UC_FLOOD */
	u32 lut_sys_lrn_limit;       /* LUT_SYS_LRN_LIMIT */
	u32 rma_ctrl01;              /* RMA_CTRL01 */
	u32 rma_ctrl02;              /* RMA_CTRL02 */
	u32 rma_cfg;                 /* RMA_CFG */
	u32 qos_uni_trap_pri_ctrl;   /* QOS_UNI_TRAP_PRI_CTRL */
	u32 oam_ctrl_0;              /* OAM_CTRL_0 */
	u32 cfg_unhiol;              /* CFG_UNHIOL */
	const struct luna_led_regs *led;	/* NULL: no table for this die */
};
#define LUNA_CLASSIFY_SID_MAX	128	/* the largest classify_sid_num below */

/* The PON-IP relocation table's element type, declared HERE because the chip
 * map below points at one. The table itself is generated beside pi_x(). */
struct luna_pi_move { u32 from, to; };
extern const struct luna_pi_move luna_pi_moves_9603cvd[];

/* RTL9602C, and the RTL9601D, whose stock X100DG reads the same block version
 * 0x16030301 and drives its lamps at these offsets. */
static const struct luna_led_regs luna_led_9602c = {
	.force_value = LED_FORCE_VALUE, .blink_rate = LED_BLINK_RATE, .blink_lsb = 12,
	.para_en = LED_PARA_EN, .pad_en = LED_IO_EN,
	.seri_para = BIT(19) | BIT(18), .seri_pad = BIT(17),
	.cpu_force_bit = 12, .pon_type = 0x1b, .link_act = 0xf78, .indices = 16,
};

/* RTL9603CVD: 18 indices, so the force values take two words and 0x1e05c is the
 * blink rate here; the stock G24W's own capture names the same index set. */
static const struct luna_led_regs luna_led_9603cvd = {
	.force_value = 0x1e054, .blink_rate = 0x1e05c, .blink_lsb = 3,
	.para_en = 0x1e068, .pad_en = 0x23010,
	.seri_para = BIT(25), .seri_pad = BIT(23),
	.cpu_force_bit = 14, .pon_type = 0x07, .link_act = 0x1e78, .indices = 18,
};

static const struct gpon_swc_map gpon_swc_9602c = {
	.chip = "RTL9602C",
	.led = &luna_led_9602c,
	/* ★ REGISTERED, NOT LEFT TO THE COMPILER (2026-09-14, ...
	 * dev/MEASURED-luna_gpon.c.md sec 274. */
	.alloc_idx_swap = false,	/* the logical T-CONT permutation is the RTL9603CVD's alone */
	/* The DS DRAM-pool words both 9602C-family stocks run (X111W and X100DG,
	 * read 2026-09-30); written only for a board whose DT reserves a DS pool. */
	.ds_dsc_cfg = 0x1fff001fu, .ds_dscrunout = 0x1fb8001eu,
	.ds_fc_config = 0x1f321f52u,
	.pi_moves = NULL,		/* no PON-IP relocation table for this die */
	.omcc_flow = GPON_OMCC_FLOW_9602C,
	.ds_dram_order = 0,	/* SRAM-only DS, as this chip's stock does */
	.sch_qmap_bits = 32,	/* chipdef array offset 32 */
	.sid_rpv_entries = 65,	/* chipdef array index 0..64 */
	.classify_sid_num = 65,	/* CLASSIFY_SID_MAX 65 == PON_SID2QID's 65 entries */
	.sw = &rtl9602c_sw_map,
	.omcc_phys_qid = 64,	/* TCONT_QUEUE_MAX(32)*(16/8)+0 */
	/* ⚠ THE SAME QUEUE AS THE OMCC, AND DELIBERATELY UNCHANGED. ...
	 * dev/MEASURED-luna_gpon.c.md sec 22. */
	.scratch_phys_qid = 64,
	.tcont_queue_max = 32, .tcont_group = 8,	/* data T-CONT 8 -> qid 32 */
	/* four ports (PON=2, CPU=3): 0..3 already covers every one of them,
	 * and the PON takes @amax_val like the rest. UNCHANGED. */
	.amax_msb = 1, .amax_val = 0x3, .amax_pon_val = 0,  /* two SELECTORS */
	.amax_last_port = 3,
	.io_mode_en = 0x23018, .io_i2c_en_bus0 = 13, .io_oem_en = 19,
	.io_gpio_en = 0x00048,
	.sds_fib_status = 0x001e4,
	.sds_reg0 = 0x22800, .fib_reg16 = 0x22c40, .fib_ext_reg21 = 0x22e54,
	.wsds_dig_18 = 0x22090,
	.i2c_ind_wd = 0x000b0, .i2c_ind_adr = 0x000b8,
	.i2c_ind_cmd = 0x000c0, .i2c_ind_rd = 0x000c8,
	.sds_win = 0x22000, .sds_cfg = 0x001d0,
	.omci_cnt = 0x329b8,
	.sc_ind_wd = 0x0003c,
	.wrap_gphy_misc = 0x00110,
	.thermal_ctrl_0 = 0x00130,
	.force_p_ablty = 0x00180,
	.ablty_force_mode = 0x001b4,
	.dyngasp_ctrl = 0x001ec,
	.accept_max_len_ctrl = 0x11008,
	.pon_trap_cfg = 0x111f8,
	.cf_cfg = 0x1600c,
	.lut_unkn_uc_da_ctrl = 0x1c008,
	.lut_learn_over_ctrl = 0x1c00c,
	.unkn_l2_mc = 0x1c010,
	.unkn_ip4_mc = 0x1c014,
	.unkn_mc_cfg = 0x1c01c,
	.lut_bc_flood = 0x1c020,
	.lut_unkn_mc_flood = 0x1c024,
	.lut_unkn_uc_flood = 0x1c028,
	.lut_sys_lrn_limit = 0x1c02c,
	.rma_ctrl01 = 0x1c03c,
	.rma_ctrl02 = 0x1c040,
	.rma_cfg = 0x1c084,
	.qos_uni_trap_pri_ctrl = 0x1c0cc,
	.oam_ctrl_0 = 0x1c100,
	.cfg_unhiol = 0x23040,
};

static const struct gpon_swc_map gpon_swc_9603cvd = {
	.chip = "RTL9603CVD",
	.led = &luna_led_9603cvd,
	.alloc_idx_swap = true,
	.omcc_flow = GPON_OMCC_FLOW_9603CVD,
	/* the DS PBO DRAM pool, from the G24W's own stock live at O5 */
	.ds_dram_order = PI_DS_DRAM_ORDER,
	.ds_dsc_cfg = 0x1fff8007u, .ds_dscrunout = 0x1fcb0006u,
	.ds_fc_config = 0x1f950036u,
	.sch_qmap_bits = 8,	/* chipdef array offset 8 -- 4 entries per word */
	.sid_rpv_entries = 8,	/* chipdef array index 0..7 -- NOT 65 */
	.classify_sid_num = 128,	/* CLASSIFY_SID_MAX 128 == PON_SID2QID/SIDVALID entries */
	.sw = &rtl9603cvd_sw_map,
	.omcc_phys_qid = 127,	/* the DAL's OMCI special case, NOT the formula */
	/* T-CONT 15 / queue 6, the vendor's own pre-pass choice:
	 * TCONT_QUEUE_MAX(8)*15 + 6 = 126. Queue 7 there would be 127, which the
	 * DAL refuses because that IS the OMCI queue. */
	.scratch_phys_qid = 126,
	.tcont_queue_max = 8, .tcont_group = 1,	/* data T-CONT 8 -> qid 64 */
	/* ★★★ [T1] MEASURED ON STOCK SILICON, all six ports, G24W ...
	 * dev/MEASURED-luna_gpon.c.md sec 23. */
	.amax_msb = 13, .amax_val = 16368, .amax_pon_val = 2031,  /* a LENGTH */
	.amax_last_port = 5,	/* six ports, and stock writes all six */
	.io_mode_en = 0x23014, .io_i2c_en_bus0 = 11, .io_oem_en = 16,
	.io_gpio_en = 0x0003c,
	.sds_fib_status = 0x00214,
	.sds_reg0 = 0x40800, .fib_reg16 = 0x40c40, .fib_ext_reg21 = 0x40e54,
	.wsds_dig_18 = 0x40090,
	.i2c_ind_wd = 0x000b0, .i2c_ind_adr = 0x000b8,
	.i2c_ind_cmd = 0x000c0, .i2c_ind_rd = 0x000c8,
	.sds_win = 0x40000, .sds_cfg = 0x00200,
	/* 16 PON-IP registers relocate on this chip -- see pi_x(). Derived from
	 * this chip's OWN chipdef, and one of them is already confirmed by the
	 * board's stock-vs-ours capture. */
	.pi_moves = luna_pi_moves_9603cvd,
	.omci_cnt = 0x32f38,
	.sc_ind_wd = 0x00028,
	.wrap_gphy_misc = 0x000ec,
	.thermal_ctrl_0 = 0x0010c,
	.force_p_ablty = 0x00198,
	.ablty_force_mode = 0x001dc,
	.dyngasp_ctrl = 0x0021c,
	.accept_max_len_ctrl = 0x1100c,
	.pon_trap_cfg = 0x110ec,
	.cf_cfg = 0x1600c,
	.lut_unkn_uc_da_ctrl = 0x1c00c,
	.lut_learn_over_ctrl = 0x1c010,
	.unkn_l2_mc = 0x1c018,
	.unkn_ip4_mc = 0x1c01c,
	.unkn_mc_cfg = 0x1c024,
	.lut_bc_flood = 0x1c028,
	.lut_unkn_mc_flood = 0x1c02c,
	.lut_unkn_uc_flood = 0x1c030,
	.lut_sys_lrn_limit = 0x1c014,
	.rma_ctrl01 = 0x1c064,
	.rma_ctrl02 = 0x1c068,
	.rma_cfg = 0x1c0ac,
	.qos_uni_trap_pri_ctrl = 0x1c214,
	.oam_ctrl_0 = 0x1c254,
	.cfg_unhiol = 0x2304c,
};

static const struct gpon_swc_map gpon_swc_9607c = {
	.chip = "RTL9607C",
	.led = NULL,	/* no lamp table for this die yet */
	/* ★ REGISTERED, NOT LEFT TO THE COMPILER (2026-09-14, ...
	 * dev/MEASURED-luna_gpon.c.md sec 275. */
	.alloc_idx_swap = false,	/* the logical T-CONT permutation is the RTL9603CVD's alone */
	.ds_dsc_cfg = 0, .ds_dscrunout = 0,	/* DRAM-order DS words: .ds_dram_order is 0 here, so they are never written */
	.ds_fc_config = 0,		/* likewise -- gated on ds_dram_order at the write site */
	.pi_moves = NULL,		/* no PON-IP relocation table for this die */
	.omcc_flow = GPON_OMCC_FLOW_9607C,
	.ds_dram_order = 0,	/* not measured on this chip -- SRAM-only, unchanged */
	.sch_qmap_bits = 32,	/* no PON-IP block in this chipdef; keep the identity packing */
	.sid_rpv_entries = 0,	/* no PON-IP block in this chipdef */
	.classify_sid_num = 65,	/* UNESTABLISHED: no PON-IP block in this chipdef; unchanged */
	.sw = &rtl9607c_sw_map,
	.omcc_phys_qid = 120,	/* TCONT_QUEUE_MAX(32)*(31/8)+24 */
	/* ⚠ OWED: unchanged from what the code wrote before the per-chip split,
	 * and no evidence either way for this die. Not guessed. */
	.scratch_phys_qid = 120,
	/* ⚠ OWED: unchanged from the single #define, no evidence for this die. */
	.tcont_queue_max = 32, .tcont_group = 8,
	/* ⚠ OWED: this die has NOT been read. The values below reproduce exactly
	 * what the old loop wrote (0..3 plus the PON at 5), so nothing about this
	 * chip changes here. Closing it needs the same measurement the RTL9603CVD
	 * just had: `diag register get all` on ITS stock, ports 0..9. */
	.amax_msb = 13, .amax_val = 2031, .amax_pon_val = 2031,  /* a LENGTH */
	.amax_last_port = 3,
	.io_mode_en = 0x23014, .io_i2c_en_bus0 = 13, .io_oem_en = 19,
	/* IO_GPIO_EN is 0x38 here, but this chip's optical front-end is internal
	 * and none of the 9602C pad recipe applies, so it is left undeclared:
	 * nothing may write a GPIO pad-enable word on the 9607C. */
	.io_gpio_en = 0,
	/* SDS_FIB_STATUS is a 3-lane ARRAY here (0x28c, stride 0x20); lane 0 is
	 * the one the GPON RX uses. ddm_probe_9607c() scans all three. */
	.sds_fib_status = 0x0028c,
	.sds_reg0 = 0x40800, .fib_reg16 = 0x40c40, .fib_ext_reg21 = 0x40e54,
	.wsds_dig_18 = 0x40090,
	.i2c_ind_wd = 0x000b4, .i2c_ind_adr = 0x000bc,
	.i2c_ind_cmd = 0x000c4, .i2c_ind_rd = 0x000cc,
	.sds_win = 0x40000, .sds_cfg = 0x00270,
	.omci_cnt = 0x32f38,
	.sc_ind_wd = 0x00024,
	.wrap_gphy_misc = 0x00114,
	.thermal_ctrl_0 = 0x00150,
	.force_p_ablty = 0x001cc,
	.ablty_force_mode = 0x00238,
	.dyngasp_ctrl = 0x0029c,
	.accept_max_len_ctrl = 0x11028,
	.pon_trap_cfg = 0x11144,
	.cf_cfg = 0x16004,
	.lut_unkn_uc_da_ctrl = 0x1c00c,
	.lut_learn_over_ctrl = 0x1c010,
	.unkn_l2_mc = 0x1c018,
	.unkn_ip4_mc = 0x1c01c,
	.unkn_mc_cfg = 0x1c024,
	.lut_bc_flood = 0x1c028,
	.lut_unkn_mc_flood = 0x1c02c,
	.lut_unkn_uc_flood = 0x1c030,
	.lut_sys_lrn_limit = 0x1c014,
	.rma_ctrl01 = 0x1c0c8,
	.rma_ctrl02 = 0x1c0cc,
	.rma_cfg = 0x1c110,
	.qos_uni_trap_pri_ctrl = 0x1c2a8,
	.oam_ctrl_0 = 0x1c2ec,
	.cfg_unhiol = 0x23104,
};

/* Resolved from the DT compatible in probe(), BEFORE any sw_rd/sw_wr of a
 * chip-selected offset. Defaults to the 9602C so a boot on an undeclared board
 * behaves exactly as this driver did before the table existed. */
static const struct gpon_swc_map *swc = &gpon_swc_9602c;

/* The physical queue a T-CONT drains its logical queue 0 on. ...
 * dev/MEASURED-luna_gpon.c.md sec 24. */
static u8 luna_tcont_phys_qid(u8 tcont)
{
	if (tcont == GPON_OMCC_TCONT)
		return swc->omcc_phys_qid;
	return (u8)(swc->tcont_queue_max * (tcont / swc->tcont_group));
}

/* Own G24 stock tcont_{get,set,del} all apply this permutation to the alloc
 * CAM index. Scheduler indices remain logical; X111 uses the identity map. */
static u8 luna_tcont_cam_index(u8 tcont)
{
	if (!swc->alloc_idx_swap)
		return tcont;
	switch (tcont) {
	case 4: return 7;
	case 7: return 4;
	case 5: return 11;
	case 11: return 5;
	default: return tcont;
	}
}

/* ★ THE UPSTREAM-OPTICS OPERATING VALUES ARE A PER-CHIP ...
 * dev/MEASURED-luna_gpon.c.md sec 25. */
struct luna_gtc_tune {
	const char *chip;
	u32 us_laser;
	u32 us_optic_sd_th;
	bool us_pwr_sav_dg_tx_opt;
};

static const struct luna_gtc_tune luna_gtc_tune_9602c = {
	.chip = "RTL9602C", .us_laser = 0x2028u, .us_optic_sd_th = 0x00504bfau,
	.us_pwr_sav_dg_tx_opt = false,
};

static const struct luna_gtc_tune luna_gtc_tune_9603cvd = {
	.chip = "RTL9603CVD", .us_laser = 0x1820u, .us_optic_sd_th = 0x00a07fffu,
	.us_pwr_sav_dg_tx_opt = true,
};

static const struct luna_gtc_tune *gtune = &luna_gtc_tune_9602c;

/* Keep the register NAMES at the call sites; the value is now per chip. */
#define SOC_IO_MODE_EN		(swc->io_mode_en)
#define IO_I2C_EN_BUS0		(swc->io_i2c_en_bus0)
#define IO_OEM_EN		(1u << (swc)->io_oem_en)
#define SOC_IO_GPIO_EN		(swc->io_gpio_en)
#define SDS_FIB_STATUS		(swc->sds_fib_status)
#define SDS_REG0		(swc->sds_reg0)
#define FIB_REG16		(swc->fib_reg16)
#define FIB_EXT_REG21		(swc->fib_ext_reg21)
#define WSDS_DIG_18		(swc->wsds_dig_18)
/* These four are DEFINED IN luna_gpon_regs.h with the 9602C's values, which are
 * right for two of the three chips and silently wrong for the RTL9607C. Undef
 * and re-point at the table so BOTH the read and the write path get the same
 * per-chip answer -- the write path had never been corrected. */
#undef  I2C_IND_WD
#undef  I2C_IND_ADR
#undef  I2C_IND_CMD
#undef  I2C_IND_RD
#define I2C_IND_WD		(swc->i2c_ind_wd)
#define I2C_IND_ADR		(swc->i2c_ind_adr)
#define I2C_IND_CMD		(swc->i2c_ind_cmd)
#define I2C_IND_RD		(swc->i2c_ind_rd)

/* The SerDes window and SDS_CFG, from the table for the same reason the I2C
 * block is: luna_gpon_regs.h holds the 9602C's absolute addresses, which are
 * wrong by 0x1e000 on both other chips. SDS() is written with the 9602C
 * address the header already carries, so each line stays greppable against it. */
#define SDS(a9602c)		(swc->sds_win + ((a9602c) - 0x22000u))

/* The PON-IP block moves between the Luna chips, and NOT by a ...
 * dev/MEASURED-luna_gpon.c.md sec 26. */
const struct luna_pi_move luna_pi_moves_9603cvd[] = {
	{ 0x020e4, 0x020f8 },	/* PI_DRN_CMD */
	{ 0x0a0b4, 0x0a0cc },	/* PI_DSCRUNOUT_DS */
	{ 0x020e0, 0x020f4 },	/* PI_DSCRUNOUT_US */
	{ 0x02568, 0x026f4 },	/* PI_GPON_DPRU_RPT_PRD */
	{ 0x0a0b8, 0x0a0d0 },	/* PI_IP_MSTBASE_DS */
	{ 0x020e8, 0x020fc },	/* PI_IP_MSTBASE_US */
	{ 0x02170, 0x021c0 },	/* PI_MOCIR_FRC_MD */
	{ 0x02174, 0x021c4 },	/* PI_MOCIR_FRC_VAL */
	{ 0x02184, 0x021d4 },	/* PI_MOCIR_TH_H */
	{ 0x02188, 0x021d8 },	/* PI_MOCIR_TH_L */
	{ 0x0a0ac, 0x0a0c0 },	/* PI_PONIP_CTL_DS */
	{ 0x020d8, 0x020ec },	/* PI_PONIP_CTL_US */
	{ 0x0255c, 0x026e8 },	/* PI_PONIP_DBG_CTRL_US */
	{ 0x02578, 0x02708 },	/* PI_PONIP_SID_OVER_LATCH_STS */
	{ 0x0256c, 0x026f8 },	/* PI_PONIP_SID_OVER_STS */
	{ 0x02564, 0x026f0 },	/* PI_PONIP_SID_USED_PAGE_CNT_US */
	{ 0x02560, 0x026ec },	/* PI_PONIP_TOTAL_PAGE_CNT_US */
	{ 0x02150, 0x021a0 },	/* PI_PON_BW_THRES */
	{ 0x0a0cc, 0x0a0e4 },	/* PI_PON_DSC_CFG_DS */
	{ 0x0215c, 0x021ac },	/* PI_PON_DSC_CFG_US */
	{ 0x0a0c8, 0x0a0e0 },	/* PI_PON_DSC_STS_DS */
	{ 0x02158, 0x021a8 },	/* PI_PON_DSC_STS_US */
	{ 0x0a0bc, 0x0a0d4 },	/* PI_PON_DSC_USAGE_DS */
	{ 0x020ec, 0x02100 },	/* PI_PON_DSC_USAGE_US */
	{ 0x0a100, 0x0a114 },	/* PI_PON_DS_PBO_PAGE_Q0 */
	{ 0x0a0fc, 0x0a110 },	/* PI_PON_FC_CONFIG_DS */
	{ 0x0a0c0, 0x0a0d8 },	/* PI_PON_IPSTS_DS */
	{ 0x020f4, 0x02108 },	/* PI_PON_IPSTS_US */
	{ 0x0218c, 0x021dc },	/* PI_PON_OLT_BW_MTR_FULL */
	{ 0x02154, 0x021a4 },	/* PI_PON_OMCI_CFG */
	{ 0x02198, 0x021e8 },	/* PI_PON_QID_CIR_RATE */
	{ 0x0229c, 0x023e8 },	/* PI_PON_QID_PIR_RATE */
	{ 0x02194, 0x021e4 },	/* PI_PON_SCH_CTRL */
	{ 0x025d8, 0x0276c },	/* PI_PON_SCH_OPT */
	{ 0x023a0, 0x025e8 },	/* PI_PON_SCH_QMAP */
	{ 0x020f8, 0x0210c },	/* PI_PON_SID2QID */
	{ 0x0213c, 0x0218c },	/* PI_PON_SIDVALID */
	{ 0x02454, 0x026c4 },	/* PI_PON_SID_GLB_TH */
	{ 0x0a0e4, 0x0a0ec },	/* PI_PON_SID_Q_MAP_DS */
	{ 0x02458, 0x026c8 },	/* PI_PON_SID_RPV_TH */
	{ 0x02450, 0x026c0 },	/* PI_PON_SID_STOP_TH */
	{ 0x02190, 0x021e0 },	/* PI_PON_TB_CTRL */
	{ 0x023e4, 0x025fc },	/* PI_PON_TCONT_EN */
	{ 0x020f0, 0x02104 },	/* PI_PON_US_FIFO_CTL */
	{ 0x023e8, 0x02600 },	/* PI_PON_WFQ_TYPE */
	{ 0x023f8, 0x02614 },	/* PI_PON_WFQ_WEIGHT */
	{ 0x0a10c, 0x0a130 },	/* PI_RSVD_PONIP_DS */
	{ 0, 0 },
};

static u32 pi_x(u32 a9602c)
{
	const struct luna_pi_move *m = swc->pi_moves;

	if (!m)
		return a9602c;
	for (; m->from; m++)
		if (m->from == a9602c)
			return m->to;
	return a9602c;
}

#define PI_X(a9602c)		pi_x(a9602c)

/* ★ THE 9602C ADDRESS IS WRITTEN ONCE, IN THE HEADER ... -- dev/MEASURED-luna_gpon.c.md sec 27. */
enum {
	PI_A_DSCRUNOUT_DS = PI_DSCRUNOUT_DS,
	PI_A_DSCRUNOUT_US = PI_DSCRUNOUT_US,
	PI_A_IP_MSTBASE_US = PI_IP_MSTBASE_US,
	PI_A_IP_MSTBASE_DS = PI_IP_MSTBASE_DS,
	PI_A_PONIP_CTL_DS = PI_PONIP_CTL_DS,
	PI_A_PONIP_CTL_US = PI_PONIP_CTL_US,
	PI_A_PON_DSC_CFG_DS = PI_PON_DSC_CFG_DS,
	PI_A_PON_DSC_CFG_US = PI_PON_DSC_CFG_US,
	PI_A_PON_FC_CONFIG_DS = PI_PON_FC_CONFIG_DS,
	PI_A_PON_OMCI_CFG = PI_PON_OMCI_CFG,
	PI_A_DRN_CMD = PI_DRN_CMD,
	PI_A_PON_OLT_BW_MTR_FULL = PI_PON_OLT_BW_MTR_FULL,
	PI_A_PON_WFQ_TYPE = PI_PON_WFQ_TYPE,
	PI_A_RSVD_PONIP_DS = PI_RSVD_PONIP_DS,
	PI_A_PON_DSC_USAGE_DS = PI_PON_DSC_USAGE_DS,
	PI_A_PON_IPSTS_US = PI_PON_IPSTS_US,
	PI_A_PON_DSC_USAGE_US = PI_PON_DSC_USAGE_US,
	PI_A_PON_IPSTS_DS = PI_PON_IPSTS_DS,
	PI_A_PON_DSC_STS_DS = PI_PON_DSC_STS_DS,
	PI_A_PON_SID2QID = PI_PON_SID2QID,
	PI_A_PON_SIDVALID = PI_PON_SIDVALID,
	PI_A_PON_SID_GLB_TH = PI_PON_SID_GLB_TH,
	PI_A_PON_SID_Q_MAP_DS = PI_PON_SID_Q_MAP_DS,
	PI_A_PON_SID_RPV_TH = PI_PON_SID_RPV_TH,
	PI_A_PON_SID_STOP_TH = PI_PON_SID_STOP_TH,
	PI_A_PON_US_FIFO_CTL = PI_PON_US_FIFO_CTL,
	PI_A_PON_BW_THRES = PI_PON_BW_THRES,
	PI_A_PON_DSC_STS_US = PI_PON_DSC_STS_US,
	PI_A_MOCIR_FRC_MD = PI_MOCIR_FRC_MD,
	PI_A_MOCIR_FRC_VAL = PI_MOCIR_FRC_VAL,
	PI_A_MOCIR_TH_H = PI_MOCIR_TH_H,
	PI_A_MOCIR_TH_L = PI_MOCIR_TH_L,
	PI_A_PON_TB_CTRL = PI_PON_TB_CTRL,
	PI_A_PON_SCH_CTRL = PI_PON_SCH_CTRL,
	PI_A_PON_QID_CIR_RATE = PI_PON_QID_CIR_RATE,
	PI_A_PON_QID_PIR_RATE = PI_PON_QID_PIR_RATE,
	PI_A_PON_SCH_QMAP = PI_PON_SCH_QMAP,
	PI_A_PON_TCONT_EN = PI_PON_TCONT_EN,
	PI_A_PON_WFQ_WEIGHT = PI_PON_WFQ_WEIGHT,
	PI_A_PONIP_DBG_CTRL_US = PI_PONIP_DBG_CTRL_US,
	PI_A_PONIP_TOTAL_PAGE_CNT_US = PI_PONIP_TOTAL_PAGE_CNT_US,
	PI_A_PONIP_SID_USED_PAGE_CNT_US = PI_PONIP_SID_USED_PAGE_CNT_US,
	PI_A_GPON_DPRU_RPT_PRD = PI_GPON_DPRU_RPT_PRD,
	PI_A_PONIP_SID_OVER_STS = PI_PONIP_SID_OVER_STS,
	PI_A_PON_SCH_OPT = PI_PON_SCH_OPT,
	PI_A_PON_DS_PBO_PAGE_Q0 = PI_PON_DS_PBO_PAGE_Q0,
	PI_A_PONIP_SID_OVER_LATCH_STS = PI_PONIP_SID_OVER_LATCH_STS,
};

#undef  PI_DSCRUNOUT_DS
#define PI_DSCRUNOUT_DS	PI_X(PI_A_DSCRUNOUT_DS)
#undef  PI_DSCRUNOUT_US
#define PI_DSCRUNOUT_US	PI_X(PI_A_DSCRUNOUT_US)
#undef  PI_IP_MSTBASE_US
#define PI_IP_MSTBASE_US	PI_X(PI_A_IP_MSTBASE_US)
#undef  PI_IP_MSTBASE_DS
#define PI_IP_MSTBASE_DS	PI_X(PI_A_IP_MSTBASE_DS)
#undef  PI_PONIP_CTL_DS
#define PI_PONIP_CTL_DS	PI_X(PI_A_PONIP_CTL_DS)
#undef  PI_PONIP_CTL_US
#define PI_PONIP_CTL_US	PI_X(PI_A_PONIP_CTL_US)
#undef  PI_PON_DSC_CFG_DS
#define PI_PON_DSC_CFG_DS	PI_X(PI_A_PON_DSC_CFG_DS)
#undef  PI_PON_DSC_CFG_US
#define PI_PON_DSC_CFG_US	PI_X(PI_A_PON_DSC_CFG_US)
#undef  PI_PON_FC_CONFIG_DS
#define PI_PON_FC_CONFIG_DS	PI_X(PI_A_PON_FC_CONFIG_DS)
#undef  PI_PON_OMCI_CFG
#define PI_PON_OMCI_CFG	PI_X(PI_A_PON_OMCI_CFG)
#undef  PI_DRN_CMD
#define PI_DRN_CMD	PI_X(PI_A_DRN_CMD)
#undef  PI_PON_OLT_BW_MTR_FULL
#define PI_PON_OLT_BW_MTR_FULL	PI_X(PI_A_PON_OLT_BW_MTR_FULL)
#undef  PI_PON_WFQ_TYPE
#define PI_PON_WFQ_TYPE	PI_X(PI_A_PON_WFQ_TYPE)
#undef  PI_RSVD_PONIP_DS
#define PI_RSVD_PONIP_DS	PI_X(PI_A_RSVD_PONIP_DS)
#undef  PI_PON_DSC_USAGE_DS
#define PI_PON_DSC_USAGE_DS	PI_X(PI_A_PON_DSC_USAGE_DS)
#undef  PI_PON_IPSTS_US
#define PI_PON_IPSTS_US	PI_X(PI_A_PON_IPSTS_US)
#undef  PI_PON_DSC_USAGE_US
#define PI_PON_DSC_USAGE_US	PI_X(PI_A_PON_DSC_USAGE_US)
#undef  PI_PON_IPSTS_DS
#define PI_PON_IPSTS_DS	PI_X(PI_A_PON_IPSTS_DS)
#undef  PI_PON_DSC_STS_DS
#define PI_PON_DSC_STS_DS	PI_X(PI_A_PON_DSC_STS_DS)
#undef  PI_PON_SID2QID
#define PI_PON_SID2QID	PI_X(PI_A_PON_SID2QID)
#undef  PI_PON_SIDVALID
#define PI_PON_SIDVALID	PI_X(PI_A_PON_SIDVALID)
#undef  PI_PON_SID_GLB_TH
#define PI_PON_SID_GLB_TH	PI_X(PI_A_PON_SID_GLB_TH)
#undef  PI_PON_SID_Q_MAP_DS
#define PI_PON_SID_Q_MAP_DS	PI_X(PI_A_PON_SID_Q_MAP_DS)
#undef  PI_PON_SID_RPV_TH
#define PI_PON_SID_RPV_TH	PI_X(PI_A_PON_SID_RPV_TH)
#undef  PI_PON_SID_STOP_TH
#define PI_PON_SID_STOP_TH	PI_X(PI_A_PON_SID_STOP_TH)
#undef  PI_PON_US_FIFO_CTL
#define PI_PON_US_FIFO_CTL	PI_X(PI_A_PON_US_FIFO_CTL)
#undef  PI_PON_BW_THRES
#define PI_PON_BW_THRES	PI_X(PI_A_PON_BW_THRES)
#undef  PI_PON_DSC_STS_US
#define PI_PON_DSC_STS_US	PI_X(PI_A_PON_DSC_STS_US)
#undef  PI_MOCIR_FRC_MD
#define PI_MOCIR_FRC_MD	PI_X(PI_A_MOCIR_FRC_MD)
#undef  PI_MOCIR_FRC_VAL
#define PI_MOCIR_FRC_VAL	PI_X(PI_A_MOCIR_FRC_VAL)
#undef  PI_MOCIR_TH_H
#define PI_MOCIR_TH_H	PI_X(PI_A_MOCIR_TH_H)
#undef  PI_MOCIR_TH_L
#define PI_MOCIR_TH_L	PI_X(PI_A_MOCIR_TH_L)
#undef  PI_PON_TB_CTRL
#define PI_PON_TB_CTRL	PI_X(PI_A_PON_TB_CTRL)
#undef  PI_PON_SCH_CTRL
#define PI_PON_SCH_CTRL	PI_X(PI_A_PON_SCH_CTRL)
#undef  PI_PON_QID_CIR_RATE
#define PI_PON_QID_CIR_RATE	PI_X(PI_A_PON_QID_CIR_RATE)
#undef  PI_PON_QID_PIR_RATE
#define PI_PON_QID_PIR_RATE	PI_X(PI_A_PON_QID_PIR_RATE)
#undef  PI_PON_SCH_QMAP
#define PI_PON_SCH_QMAP	PI_X(PI_A_PON_SCH_QMAP)
#undef  PI_PON_TCONT_EN
#define PI_PON_TCONT_EN	PI_X(PI_A_PON_TCONT_EN)
#undef  PI_PON_WFQ_WEIGHT
#define PI_PON_WFQ_WEIGHT	PI_X(PI_A_PON_WFQ_WEIGHT)
#undef  PI_PONIP_DBG_CTRL_US
#define PI_PONIP_DBG_CTRL_US	PI_X(PI_A_PONIP_DBG_CTRL_US)
#undef  PI_PONIP_TOTAL_PAGE_CNT_US
#define PI_PONIP_TOTAL_PAGE_CNT_US	PI_X(PI_A_PONIP_TOTAL_PAGE_CNT_US)
#undef  PI_PONIP_SID_USED_PAGE_CNT_US
#define PI_PONIP_SID_USED_PAGE_CNT_US	PI_X(PI_A_PONIP_SID_USED_PAGE_CNT_US)
#undef  PI_GPON_DPRU_RPT_PRD
#define PI_GPON_DPRU_RPT_PRD	PI_X(PI_A_GPON_DPRU_RPT_PRD)
#undef  PI_PONIP_SID_OVER_STS
#define PI_PONIP_SID_OVER_STS	PI_X(PI_A_PONIP_SID_OVER_STS)
#undef  PI_PON_SCH_OPT
#define PI_PON_SCH_OPT	PI_X(PI_A_PON_SCH_OPT)
#undef  PI_PON_DS_PBO_PAGE_Q0
#define PI_PON_DS_PBO_PAGE_Q0	PI_X(PI_A_PON_DS_PBO_PAGE_Q0)
#undef  PI_PONIP_SID_OVER_LATCH_STS
#define PI_PONIP_SID_OVER_LATCH_STS	PI_X(PI_A_PONIP_SID_OVER_LATCH_STS)


#undef  SDS_ANA_COM_REG03
#define SDS_ANA_COM_REG03      SDS(0x2258c)
#undef  SDS_ANA_COM_REG08
#define SDS_ANA_COM_REG08      SDS(0x225a0)
#undef  SDS_ANA_COM_REG11
#define SDS_ANA_COM_REG11      SDS(0x225ac)
#undef  SDS_ANA_COM_REG12
#define SDS_ANA_COM_REG12      SDS(0x225b0)
#undef  SDS_ANA_COM_REG22
#define SDS_ANA_COM_REG22      SDS(0x225d8)
#undef  SDS_ANA_COM_REG26
#define SDS_ANA_COM_REG26      SDS(0x225e8)
#undef  SDS_ANA_COM_REG27
#define SDS_ANA_COM_REG27      SDS(0x225ec)
#undef  SDS_ANA_GPON_REG42
#define SDS_ANA_GPON_REG42     SDS(0x22728)
#undef  SDS_ANA_GPON_REG46
#define SDS_ANA_GPON_REG46     SDS(0x22738)
#undef  SDS_ANA_MISC_REG00
#define SDS_ANA_MISC_REG00     SDS(0x22500)
#undef  SDS_ANA_MISC_REG01
#define SDS_ANA_MISC_REG01     SDS(0x22504)
#undef  SDS_ANA_MISC_REG02
#define SDS_ANA_MISC_REG02     SDS(0x22508)
#undef  WSDS_DIG_00
#define WSDS_DIG_00            SDS(0x22030)
#undef  WSDS_DIG_01
#define WSDS_DIG_01            SDS(0x22034)
#undef  WSDS_DIG_02
#define WSDS_DIG_02            SDS(0x22038)
#undef  WSDS_DIG_03
#define WSDS_DIG_03            SDS(0x2203c)
#undef  WSDS_DIG_1D
#define WSDS_DIG_1D            SDS(0x220a4)
#undef  SDS_CFG
#define SDS_CFG			(swc->sds_cfg)
/* The six OMCI packet counters, contiguous from the per-chip ...
 * dev/MEASURED-luna_gpon.c.md sec 276. */
#define OMCI_DROP_PKT_CNT	(swc->omci_cnt + 0x00)
#define OMCI_TX_PKT_CNT		(swc->omci_cnt + 0x04)
#define OMCI_RX_PKT_CNT		(swc->omci_cnt + 0x08)
#define OMCI_TX_BYTE_CNT	(swc->omci_cnt + 0x0c)
#define OMCI_RX_BYTE_CNT	(swc->omci_cnt + 0x10)
#define OMCI_CRC_ERROR_PKT_CNT	(swc->omci_cnt + 0x14)
/* the irregular movers, named after the silicon's own names */
#define SC_IND_WD               (swc->sc_ind_wd)
#define WRAP_GPHY_MISC          (swc->wrap_gphy_misc)
#define THERMAL_CTRL_0          (swc->thermal_ctrl_0)
#define FORCE_P_ABLTY           (swc->force_p_ablty)
#define ABLTY_FORCE_MODE        (swc->ablty_force_mode)
#define DYNGASP_CTRL            (swc->dyngasp_ctrl)
#define ACCEPT_MAX_LEN_CTRL     (swc->accept_max_len_ctrl)
#define PON_TRAP_CFG            (swc->pon_trap_cfg)
#define CF_CFG                  (swc->cf_cfg)
#define LUT_UNKN_UC_DA_CTRL     (swc->lut_unkn_uc_da_ctrl)
#define LUT_LEARN_OVER_CTRL     (swc->lut_learn_over_ctrl)
#define UNKN_L2_MC              (swc->unkn_l2_mc)
#define UNKN_IP4_MC             (swc->unkn_ip4_mc)
#define UNKN_MC_CFG             (swc->unkn_mc_cfg)
#define LUT_BC_FLOOD            (swc->lut_bc_flood)
#define LUT_UNKN_MC_FLOOD       (swc->lut_unkn_mc_flood)
#define LUT_UNKN_UC_FLOOD       (swc->lut_unkn_uc_flood)
#define LUT_SYS_LRN_LIMIT       (swc->lut_sys_lrn_limit)
#define RMA_CTRL01              (swc->rma_ctrl01)
#define RMA_CTRL02              (swc->rma_ctrl02)
#define RMA_CFG                 (swc->rma_cfg)
#define QOS_UNI_TRAP_PRI_CTRL   (swc->qos_uni_trap_pri_ctrl)
#define OAM_CTRL_0              (swc->oam_ctrl_0)
#define CFG_UNHIOL              (swc->cfg_unhiol)
#define PISO_EXT                (swc->piso_ext)
/* Open the DS GEM unicast/broadcast pass gate ... -- dev/MEASURED-luna_gpon.c.md sec 28. */
static bool gem_gate_open;
module_param(gem_gate_open, bool, 0444);
MODULE_PARM_DESC(gem_gate_open, "open DS GEM pass gate (needs the PON-IP->host OMCI drain; default off = stable online)");
/* Install the WAN data-GEM datapath. CONFIRMED (stability ...
 * dev/MEASURED-luna_gpon.c.md sec 29. */
static bool data_gem_en = true;
module_param(data_gem_en, bool, 0644);
MODULE_PARM_DESC(data_gem_en, "install the WAN data GEM datapath during config (default on)");
/* trace=0 (default) silences the routine per-PLOAM/per-ACK dumps so the compact
 * O5 timeline survives the lossy serial console; key-PLOAM EVT + O5 lines always print. */
#ifdef CONFIG_GPON_PLOAM_DIAG
static bool trace;	/* default 0: per-PLOAM/ACK tracing is SLOW (printk over serial) and perturbs the
			 * activation timing (breaks ranging when on). Set luna_gpon.trace=1 only for short diagnostics. */
module_param(trace, bool, 0644);
MODULE_PARM_DESC(trace, "verbose per-PLOAM/per-ACK serial spam (default 0)");
#else
static const bool trace;
#endif

static bool cdr_reseat_on_reactivate = true;	/* default ON (A/B 2026-06-15): on a deactivate->O1 re-range, re-pulse the
			 * softirq-safe US-TX SerDes interface reset-B (WSDS_DIG_1D[16], the same primitive the
			 * boot path + the O3 re-sync use) so a marginal serializer lock from the prior activation
			 * is re-attempted immediately, cutting the cold-start O5<->O1 activation flapping. Opt-in
			 * A/B: TX-interface only (the locked DS RX framer is undisturbed). */
module_param(cdr_reseat_on_reactivate, bool, 0644);
MODULE_PARM_DESC(cdr_reseat_on_reactivate, "re-seat US-TX SerDes reset-B on re-range to cut activation flapping (default 0)");
#ifdef CONFIG_GPON_PLOAM_DIAG
static bool ploam_tx_dbg = true;	/* TEST: log per-send US-PLOAM TX (ENQ self-clear = HW transmitted) to
					 * prove whether the urgent-queue ACK/Password actually leaves the ONU
					 * (OLT raises LOAi = never gets our acks). Logs first 40 sends. */
module_param(ploam_tx_dbg, bool, 0644);
MODULE_PARM_DESC(ploam_tx_dbg, "log US-PLOAM CPU-TX ENQ self-clear per send (urgent-queue TX diagnostic)");
#else
static const bool ploam_tx_dbg;
#endif
/* o5_rearm_burst_gate: re-apply the US burst-gate cluster (0x5188/0x526c/0x6024/0x6260)
 * and re-arm the HW auto-No_message keepalive template on every O5 entry (not just __init),
 * so a re-ranged O5 after a GMAC/SDS reset does not run on US-side reset defaults. Default on;
 * A/B with luna_gpon.o5_rearm_burst_gate=0. */
static bool o5_rearm_burst_gate = true;
module_param(o5_rearm_burst_gate, bool, 0644);
MODULE_PARM_DESC(o5_rearm_burst_gate, "re-apply US burst-gate cluster + No_message keepalive on each O5 entry (default on)");
/* o5_ploam_keepalive_ticks: emit a No_message US PLOAM (HW auto queue 0x7) every N FSM ticks
 * (10ms each) while at O5 with an assigned ONU-ID, so a valid PLOAM is present in the OLT's
 * granted slots regardless of how the shared US-PLOAM buffer was last written, defeating the
 * OLT's PLOAM/ack-liveness timeout. 0 = disabled. Default 100 (~1s). */
static uint o5_ploam_keepalive_ticks;	/* default OFF (match stock: zero unsolicited US-PLOAM at O5) */
module_param(o5_ploam_keepalive_ticks, uint, 0644);
MODULE_PARM_DESC(o5_ploam_keepalive_ticks, "emit No_message US-PLOAM every N 10ms ticks at O5 (0=off, default 0 -- stock emits no unsolicited US-PLOAM at O5)");

/* o5_provision_watchdog_ticks: a "Laser out" boot reaches O5 ...
 * dev/MEASURED-luna_gpon.c.md sec 30. */
static uint o5_provision_watchdog_ticks;	/* default 0 = off (proven ineffective, see above) */
module_param(o5_provision_watchdog_ticks, uint, 0644);
MODULE_PARM_DESC(o5_provision_watchdog_ticks, "re-range if at O5 this many ticks with gpon0 RX=0 (0=off default; PROVEN INEFFECTIVE: re-range does not re-roll the cold-start serializer lock)");
/* o5_ungranted_ticks: leave O5 for O1 after this many ~10 ms ticks with no grant to our ONU-ID
 * (the OLT dropped us: "Laser out", Offline) -- the X111W sat at O5 13 min so (2026-10-12). A healthy
 * O5 sees grants every second; misses in a row double the wait, up to 32x. */
static uint o5_ungranted_ticks = 1000;
module_param(o5_ungranted_ticks, uint, 0644);
MODULE_PARM_DESC(o5_ungranted_ticks, "re-activate after this many 10 ms ticks at O5 with no grant to our ONU-ID, doubling per miss up to 32x (0=off, default 1000)");

/* los_rerange_ticks: autonomous downstream-LOS recovery. With ...
 * dev/MEASURED-luna_gpon.c.md sec 31. */
static uint los_rerange_ticks = 30;		/* ~300ms of (optic_los & !sds_sdet): catches a real
						 * 2-4s fiber pull, ~3x stock's 100ms TO2; a sub-second
						 * pad-steal can neither reach it nor (lacking
						 * !sds_sdet) be counted at all */
module_param(los_rerange_ticks, uint, 0644);
MODULE_PARM_DESC(los_rerange_ticks, "drop to O1 + re-range after a REAL downstream LOS (optic_los AND no SerDes sig-detect) persists this many ~10ms ticks (fiber-pull recovery; 0=off, default 30 ~300ms ~= stock TO2)");
static u32 gpon_los_run;			/* consecutive real-LOS (optic_los & !sds_sdet) tick count */

/* G.984.3 TO1: an ONU-ID with no Ranging_Time for this long goes back to O1.  Stock
 * runs TO1 10000 ms (G24W `diag gpon get`, ONU-G24W.md); without it an OLT that
 * restarts with its light on leaves this ONU at O4 for good. */
static uint o4_ranging_timeout_ticks = GPON_DWELL_TO1_TICKS;
module_param(o4_ranging_timeout_ticks, uint, 0644);
MODULE_PARM_DESC(o4_ranging_timeout_ticks, "G.984.3 TO1: back to O1 when no Ranging_Time follows the ONU-ID within this many ~10ms ticks (0=off, default 1000 = stock's 10 s)");

/* The O1/O2/O3 dwell report -- the half the activation ...
 * dev/MEASURED-luna_gpon.c.md sec 32. */
static uint early_dwell_report_ticks = 500;	/* ~5 s at the ~10ms tick */
module_param(early_dwell_report_ticks, uint, 0644);
MODULE_PARM_DESC(early_dwell_report_ticks, "report the PLOAM dwell at O1/O2/O3 every this many ~10ms ticks, with the GTC's serial-number and ranging request counters (0=silent, default 500 ~5s). Reports only; it never changes the FSM");

/* Fiber-pull / re-range diagnostic (event-driven, NO per-tick ...
 * dev/MEASURED-luna_gpon.c.md sec 33. */
static u32 gpon_rerange_cnt;
static u32 gpon_last_outage_ms;
static unsigned long gpon_rerange_start_j;
static unsigned long gpon_rerange_last_log_j;

/* last DS PLOAM type drained this cycle, surfaced on the periodic O5 line */
static u8 gpon_last_ds_type;
/* DIAGNOSTIC: force the upstream laser continuously on (US_CFG.FS_LON). Tests
 * whether the SoC SerDes-TX can drive the BOSA at all, independent of the GTC
 * burst scheduler. DEV-ONLY — continuous light jams a multi-ONU PON. */
static bool force_laser;
module_param(force_laser, bool, 0444);
MODULE_PARM_DESC(force_laser, "force US laser CW on (US_CFG FS_LON) — SerDes-TX emission diagnostic");
/* Laser burst bias/mod DACs: THIS unit's 12-bit codes from its own rtl8290b.data
 * (CAL_IBIAS/CAL_IMOD, code = uA * 4096 / 50000), set by the provisioning or the
 * command line; 0 keeps the golden seed. A compiled value would be one unit's
 * calibration shipped to every unit. Registered with their setter beside
 * luna_laser_dac_apply(). dev/MEASURED-luna_gpon.c.md sec 34. */
static unsigned int laser_bias;
static unsigned int laser_mod;
/* rev-A bring-up US-TX SerDes CMU/PLL + TX-LA-LDO writes ...
 * dev/MEASURED-luna_gpon.c.md sec 35. */
static unsigned int serdes_modev1_tx;	/* default 0: TESTED (COM_REG02/03/08/24/25 applied+readback-confirmed) =
					 * NO effect on US-TX burst (OLT still "Laser out", rxsid=0) and slightly
					 * degraded DS (CMU shared) -> the omitted ModeV1 TX SerDes regs are NOT the
					 * gap; reverted to off. Param kept for reference. */
module_param(serdes_modev1_tx, uint, 0444);
MODULE_PARM_DESC(serdes_modev1_tx, "1=apply the rev-A bring-up US-TX SerDes CMU/PLL + TXLA_LDOEN writes (no effect)");

/* serdes_tx_xtra: 0 (default) = ORACLE-PARITY — do NOT set the 3 SerDes-TX serializer-path bits
 * (0x220a8[5:4], 0x2281c[14], 0x22a30[8]) that the LIVE stock-ref ONU leaves clear (it ranges +
 * bursts without them). 1 = restore the old behaviour (set them) for A/B if parity regresses ranging. */
static unsigned int serdes_tx_xtra;
module_param(serdes_tx_xtra, uint, 0644);
MODULE_PARM_DESC(serdes_tx_xtra, "1=set legacy SerDes-TX D2A/clk-edge bits (stock=0; default 0=match stock)");
/* serdes_cdr_reset: replicate the stock SerDes CDR-reset ...
 * dev/MEASURED-luna_gpon.c.md sec 36. */
static bool serdes_cdr_reset = true;
module_param(serdes_cdr_reset, bool, 0644);
MODULE_PARM_DESC(serdes_cdr_reset, "pulse SDS_ANA_COM_REG12 (0x225b0) bit15 10ms (stock serdesCdr_reset RX_SD_POR_SEL) (stock ponmac step; default on)");
/* usnic_initrdy_poll: stock-aligned US-NIC readiness gate. ...
 * dev/MEASURED-luna_gpon.c.md sec 37. */
static bool usnic_initrdy_poll = true;
module_param(usnic_initrdy_poll, bool, 0644);
MODULE_PARM_DESC(usnic_initrdy_poll, "wait PON_IPSTS_US.PONIC_INITRDY (0x1bf020f4 bit0)==1 before US GMII latch (default on; bounded 200ms, read-only)");
/* usnic_initrdy_repulse: ACTIVE escalation (experiment). On PONIC_INITRDY timeout,
 * re-pulse the CDR (COM_REG12 bit15, same as serdes_cdr_reset) + re-poll once
 * — re-rolls the analog lock without reordering config/reset. Default off. */
static bool usnic_initrdy_repulse;
module_param(usnic_initrdy_repulse, bool, 0644);
MODULE_PARM_DESC(usnic_initrdy_repulse, "on PONIC_INITRDY timeout, re-pulse CDR (COM_REG12 bit15) + re-poll once (default 0)");
/* cdr_stuck_recover: replica of the stock runtime ... -- dev/MEASURED-luna_gpon.c.md sec 38. */
static bool cdr_stuck_recover = true;
module_param(cdr_stuck_recover, bool, 0644);
MODULE_PARM_DESC(cdr_stuck_recover, "recover a wedged DS CDR (GTC_DS_STS==0xca0eca0f) by toggling SP_SDS_EN_RX, like stock (default on)");

/* Periodic DS multiframe/BWmap ESD-recover (stock ... -- dev/MEASURED-luna_gpon.c.md sec 39. */
#define GTC_DS_STS_LOF		BIT(1)		/* loss-of-frame; clear = framer byte-locked */
#define GPON_ESD_INTERVAL_MS	5000		/* stock gpon_esdRecover_interval */
#define GPON_ESD_THRESHOLD	20		/* stock gpon_esdRecover_threshold */
static bool gpon_esd_recover = true;
module_param(gpon_esd_recover, bool, 0644);
MODULE_PARM_DESC(gpon_esd_recover, "periodic RX-CDR re-lock when the DS PLEND/LOM parse fails while byte-locked (stock gpon_esdRecover; fixes the ~1/4 grant-deaf churn; default on)");
#define GPON_CDR_STUCK_MAX	8	/* FAST re-acquire attempts before backing off */
/* RATE-BOUNDED, NEVER COUNT-CAPPED -- the standing rule, and ...
 * dev/MEASURED-luna_gpon.c.md sec 40. */
#define GPON_CDR_STUCK_SLOW_TICKS	6000	/* ~60 s at the 10 ms poll */
static unsigned int gpon_cdr_stuck_tries;	/* consecutive attempts THIS episode */
static unsigned int gpon_cdr_stuck_count;	/* diag: total wedges detected */
static unsigned int gpon_cdr_stuck_fixed;	/* diag: wedges cleared by the toggle */
static u32 gpon_gtc_ds_sts_last;		/* diag: last raw GTC DS status seen */
/* serdes_stock_seq: 1 = the EXACT stock rev-A bring-up ORDER ...
 * dev/MEASURED-luna_gpon.c.md sec 41. */
static bool serdes_stock_seq;	/* default 0 = our gpon_serdes_init (stock order tested = no improvement) */
module_param(serdes_stock_seq, bool, 0644);
MODULE_PARM_DESC(serdes_stock_seq, "1=stock rev-A SerDes bring-up order (gpon_serdes_init_stock); 0=our gpon_serdes_init");

/* family_lib: bring the SerDes up via the clean-room ... -- dev/MEASURED-luna_gpon.c.md sec 277. */
static bool family_lib = true;	/* default ON: the clean-room luna_ponmac family lib is the
				 * 9602C SerDes boot bring-up. HW-validated (O5 10/10 over two 5-boot
				 * runs, LAN ok, WAN leases at the analog rate) = equivalent to the
				 * inline path (its 9602C op-tables are a faithful, exact-match
				 * translation of gpon_serdes_init). luna_gpon.family_lib=0 = legacy inline. */
module_param(family_lib, bool, 0644);
MODULE_PARM_DESC(family_lib, "1=bring up SerDes via luna_ponmac family lib (9602C path, default); 0=inline gpon_serdes_init");
/* serdes_postmode_perturb: the family-lib path performs TWO ...
 * dev/MEASURED-luna_gpon.c.md sec 42. */
static bool serdes_postmode_perturb;	/* default false: skip post-mode perturbations (stock rev-A) */
module_param(serdes_postmode_perturb, bool, 0644);
MODULE_PARM_DESC(serdes_postmode_perturb, "1=do post-GPON-mode DIG_1D resync + serdesCdr_reset (legacy); 0=skip (stock rev-A, default)");
/* serdes_sds_cfgrst: the family-lib SerDes reset pulse used ...
 * dev/MEASURED-luna_gpon.c.md sec 43. */
static bool serdes_sds_cfgrst;	/* default false = stock bit0-only SDS reset */
module_param(serdes_sds_cfgrst, bool, 0644);
MODULE_PARM_DESC(serdes_sds_cfgrst, "1=legacy: also pulse CMD_SDS_CFG_RST_PS bit7 in the SDS reset; 0=stock bit0-only (default, the cold-start fix)");
/* serdes_stock_analog: drive SDS_ANA_COM REG01 ... -- dev/MEASURED-luna_gpon.c.md sec 44. */
static bool serdes_stock_analog = true;
module_param(serdes_stock_analog, bool, 0644);
MODULE_PARM_DESC(serdes_stock_analog, "1=match live-stock SDS REG01=0x73a4 + REG11 RX_FILT=0 post-reset (default, the cold-start fix); 0=legacy");
/* serdes_analog_postreset: program the FULL analog CMU/CDR ...
 * dev/MEASURED-luna_gpon.c.md sec 45. */
static bool serdes_analog_postreset = true;
module_param(serdes_analog_postreset, bool, 0644);
MODULE_PARM_DESC(serdes_analog_postreset, "1=program full analog CMU/CDR table AFTER the SDS reset (stock rev-A, default, the cold-start determinism fix); 0=legacy pre-reset");
/* serdes_cmu_settle_ms: ms to wait after forcing the 125M ref clock and BEFORE releasing
 * the SerDes interface reset-B (which latches the TX serializer phase) — lets the TX CMU PLL
 * lock first. 0 = legacy. Candidate fix for the cold-start ~50% US-TX "Laser out" metastability. */
static unsigned int serdes_cmu_settle_ms;
module_param(serdes_cmu_settle_ms, uint, 0644);
MODULE_PARM_DESC(serdes_cmu_settle_ms, "ms TX-CMU-lock settle between 125M ref force and reset-B release (0=legacy default)");
/* serdes_txpll_relock: at the O1/O2->O3 edge, re-lock the TX ...
 * dev/MEASURED-luna_gpon.c.md sec 46. */
static bool serdes_txpll_relock = true;
module_param(serdes_txpll_relock, bool, 0644);
MODULE_PARM_DESC(serdes_txpll_relock, "1=re-lock the TX CMU PLL (toggle CMU enable + FIFO re-sync) at O3 entry before the first US burst — fixes the ~50% cold-start lock-to-wrong-rate (default on); 0=skip");
/* The periodic US-TX interface reset-B pulse while un-ranged ...
 * dev/MEASURED-luna_gpon.c.md sec 47. */
static bool unranged_reseat = true;
module_param(unranged_reseat, bool, 0644);
MODULE_PARM_DESC(unranged_reseat, "pulse the US-TX SerDes interface reset-B every ~200 ticks while un-ranged (default 1 = as shipped; 0 = the O4-wall A/B)");
/* optical_poll: periodic ANI-G DDM optical read. DEFAULT OFF ...
 * dev/MEASURED-luna_gpon.c.md sec 278. */
static bool optical_poll;
module_param(optical_poll, bool, 0644);
MODULE_PARM_DESC(optical_poll, "1=periodic ANI-G DDM optical poll; 0=off default (the live /proc read + LuCI refresh already show current dBm without periodic BOSA I2C, which historically churned the OLT)");
/* bosa_i2c_restore_pad: optional cleanup — return bus-0's SoC ...
 * dev/MEASURED-luna_gpon.c.md sec 48. */
static bool bosa_i2c_restore_pad;
/* The I2C bus the BOSA hangs on, from DT (/pon-optics realtek,i2c-bus): 1 on
 * the X100DG, whose stock loads its BOSA driver with I2C_PORT=1. */
static unsigned int bosa_i2c_bus;
module_param(bosa_i2c_restore_pad, bool, 0644);
MODULE_PARM_DESC(bosa_i2c_restore_pad, "restore SOC_IO_MODE_EN[13]=0 (optical-SD pad) after each BOSA I2C transaction (default 0; optic_los works without it)");
/* lan_keep_open: LAN management must be reachable INDEPENDENT ...
 * dev/MEASURED-luna_gpon.c.md sec 49. */
static bool lan_keep_open = true;
module_param(lan_keep_open, bool, 0644);
MODULE_PARM_DESC(lan_keep_open, "keep LAN open from boot independent of GPON/WAN state (default 1); 0=legacy O5-gated");
/* force_soc_clk: before the SerDes bring-up, write the 3 SoC ...
 * dev/MEASURED-luna_gpon.c.md sec 50. */
static bool force_soc_clk;
module_param(force_soc_clk, bool, 0644);
MODULE_PARM_DESC(force_soc_clk, "1=write live-stock SoC clock regs 0x18000100/12c/140 before SerDes bring-up (cold-start fix candidate); 0=legacy");
/* serdes_clkgate_rstb: gate the SerDes word clock (STOP_CLK=1) across the interface
 * reset-B release and un-gate LAST, so the word divider restarts on one defined edge
 * (defeats the async-reset-on-running-divider ~50% serializer-phase coin-flip). The
 * single highest-value cold-start fix candidate (ideation rank-1 fix). 0 = legacy. */
static bool serdes_clkgate_rstb;
module_param(serdes_clkgate_rstb, bool, 0644);
MODULE_PARM_DESC(serdes_clkgate_rstb, "1=clock-gated (STOP_CLK) SerDes reset-B release, un-gate last (cold-start metastability fix candidate); 0=legacy free-running");
/* serdes_skip_rstb_dance: Live debug confirmed WSDS_DIG_1D is ...
 * dev/MEASURED-luna_gpon.c.md sec 51. */
static bool serdes_skip_rstb_dance;
module_param(serdes_skip_rstb_dance, bool, 0644);
MODULE_PARM_DESC(serdes_skip_rstb_dance, "1=skip the c2_sds_rstb DIG_1D reset-B dance (already-released; gratuitous phase-latching pulse); 0=legacy dance");
/* serdes_minimal_analog: our golden analog table does ~145 ...
 * dev/MEASURED-luna_gpon.c.md sec 52. */
static bool serdes_minimal_analog;
module_param(serdes_minimal_analog, bool, 0644);
MODULE_PARM_DESC(serdes_minimal_analog, "1=skip the golden-table writes stock omits (dup GPON banks + FIB bodies, ~134 writes) to match stock's minimal SerDes bring-up; 0=full golden table");
/* bosa_before_serdes: CROSS-SUBSYSTEM ORDER fix candidate ...
 * dev/MEASURED-luna_gpon.c.md sec 53. */
static bool bosa_before_serdes;
module_param(bosa_before_serdes, bool, 0644);
MODULE_PARM_DESC(bosa_before_serdes, "1=power+settle the BOSA RX before the SerDes bring-up (stock order); 0=legacy SerDes-first");
/* bosa_settle_ms: settle delay after bosa_rx_enable when bosa_before_serdes=1, emulating
 * the working firmware's ~21ms post-reset + ready-check analog-settle before the SerDes CMU locks. */
static uint bosa_settle_ms = 50;
module_param(bosa_settle_ms, uint, 0644);
MODULE_PARM_DESC(bosa_settle_ms, "ms to settle the BOSA analog before the SerDes bring-up when bosa_before_serdes=1 (default 50)");
/* sc_ldo_init: stock runs an LDO-init step (early in boot) ...
 * dev/MEASURED-luna_gpon.c.md sec 54. */
static bool sc_ldo_init = true;
module_param(sc_ldo_init, bool, 0644);
MODULE_PARM_DESC(sc_ldo_init, "run stock rtk_ldo_init (SC-indirect 0xfdca analog LDO + THERMAL_CTRL_0); default on");

/* Register accessor the family lib injects: absolute phys -> KSEG1 uncached MMIO
 * (phys < 0x20000000 on this SoC: swcore 0x1b000000 / PON-IP 0x1bf00000). */
static u32 r960_phys_rd(u32 phys)
{
	return ioread32((void __iomem *)(unsigned long)(0xa0000000u | phys));
}
static void r960_phys_wr(u32 phys, u32 val)
{
	iowrite32(val, (void __iomem *)(unsigned long)(0xa0000000u | phys));
}
static const struct luna_ops rtl9602c_r960_ops = {
	.rd = r960_phys_rd,
	.wr = r960_phys_wr,
};
/* DIAGNOSTIC: skip BOSA TX power-on + APC ignition (keep RX ...
 * dev/MEASURED-luna_gpon.c.md sec 55. */
static bool gpio_pad_9603cvd;
module_param(gpio_pad_9603cvd, bool, 0444);
MODULE_PARM_DESC(gpio_pad_9603cvd,
		 "apply the RTL9603CVD optical-SD pad recipe derived from its own stock (default 0)");

static bool laser_off;		/* default false; set via luna_gpon.laser_off=1 for the isolation test */
module_param(laser_off, bool, 0444);
MODULE_PARM_DESC(laser_off, "skip laser TX-enable+APC (DS-RX-vs-laser isolation: laser-on deafens DS RX)");
/* DEFAULT TRUE = THE RANGING FIX. Skip our clean-room APC ...
 * dev/MEASURED-luna_gpon.c.md sec 56. */
static bool apc_off = true;	/* apc_off=false was RE-TESTED and BREAKS ranging: the ~3 s CW
			 * seating loop deafens the shared-BOSA DS-RX, so the OLT only ever sees
			 * "Initial" and never O5. And the apc_off=true laser is NOT weak: bias
			 * stable at 0x19, no fault, EN_L=0 throughout, O5 reached and held ~8 s
			 * before the OLT sends Deactivate=LOS. So the LOS is not a bias-seat
			 * problem -- it is the upstream BURST not being decodable despite a
			 * healthy laser, the same wall as rxsid=0. */
module_param(apc_off, bool, 0444);
MODULE_PARM_DESC(apc_off, "skip bosa_apc_calibrate (1=A4 image alone; 0=run APC to seat OFFK laser bias)");
/* RTL8290B B-variant APC/OFFK ignition. DEFAULT FALSE so the ...
 * dev/MEASURED-luna_gpon.c.md sec 57. */
static bool apc_offk;		/* default OFF: OFFK converges (R29=0x3f) but does NOT lift WAN rate; not the WAN root cause */
module_param(apc_offk, bool, 0444);
MODULE_PARM_DESC(apc_offk, "run rtl8290b_apc_init B-variant OFFK ignition (completes modulator offset cal; default 0 = unchanged)");
/* TEMP default true (LAN+WiFi ACCESS build): hold the ONU FSM ...
 * dev/MEASURED-luna_gpon.c.md sec 279. */
static bool gpon_hold;	/* default false: range to O5 normally (so the O5 selftest fires) */
module_param(gpon_hold, bool, 0444);
MODULE_PARM_DESC(gpon_hold, "hold the GPON FSM at O1 (no ranging) -> stable br-lan/WiFi for LAN+WiFi access (GPON/WAN disabled)");
/* gpon_sn_bytes is defined near the top: the onu_sn setter needs it. */
static struct timer_list gpon_fsm_timer;
/* The tick and the downstream-PLOAM interrupt both drive the core under it. */
static DEFINE_SPINLOCK(luna_fsm_lock);
static int luna_gpon_irq;		/* 0: the device tree wires none */
static u8 gpon_fsm_state = 1;		/* O1 */

/* Deferred US-TX CDR-reset (re-range path). gpon_fsm_handle() ...
 * dev/MEASURED-luna_gpon.c.md sec 58. */
static struct work_struct gpon_cdr_reset_work;

/* Last upstream-burst-overhead parameters the OLT dictated. ...
 * dev/MEASURED-luna_gpon.c.md sec 59. */
static const u8 gpon_boh_guard;
static const u8 gpon_boh_ptn = 0xaa;			/* Type-3 preamble pattern */
static const u8 gpon_boh_delim[3] = { 0xab, 0x59, 0x83 };
static const u8 gpon_boh_t3pre;				/* Type-3 pre-ranged length */
static const u8 gpon_boh_t3ranged;			/* Type-3 ranged length */
/* THE TICK PERIOD IS DEFINED ONCE, AND THE TWO HALVES ARE NOT ...
 * dev/MEASURED-luna_gpon.c.md sec 60. */
static uint gpon_fsm_tick_req_ms = 10;
module_param(gpon_fsm_tick_req_ms, uint, 0644);
MODULE_PARM_DESC(gpon_fsm_tick_req_ms,
	"requested FSM/PLOAM poll period in ms (default 10; the timer rounds UP to a jiffy, so at HZ=250 this becomes 12 -- 4 is the floor). Lower it to test whether downstream-PLOAM latency is what the OLT is reacting to");

#define GPON_FSM_TICK_REQ_MS	gpon_fsm_tick_req_ms
#define GPON_FSM_TICK_JIFFIES	msecs_to_jiffies(GPON_FSM_TICK_REQ_MS)

/* The core's clock: ms-based protocol timers never depend on the FSM tick. */
static u32 luna_now_ms(void)
{
	return max_t(u32, 1, (u32)div_u64(ktime_get_boottime_ns(), NSEC_PER_MSEC));
}

static u32 gpon_fsm_ticks;
static u32 luna_fsm_first_ms;
static u8 gpon_sds_synced;	/* one-shot SDS TX re-sync done */
static u32 gpon_ds_rx;		/* total downstream PLOAMs drained (DS-lock liveness) */
/* The GEM the install SUCCEEDED on.  Deliberately NOT gpon_omcc_gem_port, which
 * gpon_install_omcc() stamps on entry and therefore also holds the port of an
 * install that FAILED -- believing that one would latch the failure as done and
 * never retry.  Written only beside the flag, cleared with it. */
static bool gpon_data_installed;	/* WAN data GEM (193) datapath installed (one-shot, on OMCI GEM-CTP create) */
static bool gpon_data_gem_solicited;	/* OLT has sent the OMCI GEM-CTP (ME268) Create -> only THEN install
					 * our data GEM, idempotently OVER the OLT's gem. Installing it
					 * proactively at PLOAM config (before the OLT's ME268) made the OLT
					 * unable to reconcile our gem on a 2nd+ admit and churn-lock (op=0xff
					 * reclaim->DEACT). Stock waits for the OLT's create. Cleared on Deactivate
					 * so each re-admit waits for the OLT's fresh ME268. Set from the eth OMCI rx. */
static bool gpon_data_tcont_installed;	/* the OLT's DATA Alloc-ID bound to the DATA T-CONT.
					 * SESSION STATE: cleared by EVERY teardown, like its three
					 * siblings above. It used to have no teardown clear at all --
					 * only the OLT's Assign_Alloc-ID DEALLOCATE, which additionally
					 * demands the Alloc-ID of the session that just ended. A
					 * re-config handing out a DIFFERENT one was then refused by the
					 * install guard, and the only recovery was waiting for an
					 * Alloc-ID the OLT will never send again: the WAN data T-CONT
					 * dark until a reboot. Pinned by gpon_data_bind{,_policy}_test. */
/* The WIRE GEM Port-ID the OLT assigned in its OMCI ME 268 (GEM Port Network CTP)
 * Create, attribute 1 — set from the eth OMCI RX snoop, and what the data-GEM
 * install actually programs. It is the OLT's to choose (measured on this lab OLT:
 * 223 to one board, 193 to another), exactly as the OMCC's GEM Port-ID comes from
 * Configure_Port-ID. GPON_DATA_GEM_DEFAULT is only the value held before the OLT
 * has spoken; the install is gated on gpon_data_gem_solicited, so it is never the
 * value that reaches the wire on a provisioned session. */
static u16 gpon_data_gem_port = GPON_DATA_GEM_DEFAULT;
static u16 gpon_omcc_alloc;	/* OMCC Alloc-ID override; 0 (default) = bind the LIVE ONU-ID.
		 * The OMCC's upstream Alloc-ID IS the ONU-ID (G.984.3 implicit default), and
		 * stock binds the GTC alloc-CAM entry to the live ONU-ID. The OLT grants on
		 * alloc = ONU-ID with NO Assign_Alloc-ID, the CAM resolves that grant and the
		 * framer drains. The OLD 0x100 default was WRONG: the CAM then held an alloc
		 * the OLT never grants, so every default-alloc grant MISSED it, the OMCC
		 * T-CONT was unreachable, DBRu reported 0 and the OLT never escalated the
		 * BWmap flags from overhead to payload. The "0x100 makes bwm_acpt>0" note was
		 * a RED HERRING: bwm_acpt counts ONU-ID-addressed grants, not CAM hits. */
#define GPON_OMCC_TCONT_ALT	1	/* Alternative T-CONT for alloc 0x100 (OLT's tcont 1) */
module_param(gpon_omcc_alloc, ushort, 0644);
MODULE_PARM_DESC(gpon_omcc_alloc, "OMCC Alloc-ID override (0=auto, 1=ONU-ID+1 for OLTs that grant T-CONT 1 not 0)");
static u16 gpon_data_alloc;		/* the OLT's data Alloc-ID, on T-CONT 8 */
/* data_tcont: route the WAN data GEM's upstream to the DATA ...
 * dev/MEASURED-luna_gpon.c.md sec 61. */
static bool data_tcont = true;	/* default ON: data US rides the DATA T-CONT 8's grants */
module_param(data_tcont, bool, 0644);
MODULE_PARM_DESC(data_tcont, "route the WAN data GEM US to the data T-CONT 8 queue 32 (default ON: measured 0.2 -> 106 Mbps upstream on HSGQ-G008; =0 rides the OMCC T-CONT 16 for an OLT that grants one Alloc-ID for both)");

static inline u32 gpon_rd(u32 off) { return ioread32(gpon_base + off); }
static inline void gpon_wr(u32 off, u32 v) { iowrite32(v, gpon_base + off); }

/* GPON_GTC_DS_MISC_CNTR_BWM_ACPT clears on read and has several readers: one fold keeps the
 * total, and each reader takes its own delta, so none reads another's grants as its own. */
static atomic_t gpon_bwm_acpt_total = ATOMIC_INIT(0);
static u32 gpon_bwm_acpt_ds_cntr_last, gpon_bwm_acpt_us_tx_last, gpon_bwm_acpt_diag_last;
static u32 gpon_bwm_acpt_watch_last;

static u32 gpon_bwm_acpt_since(u32 *last)
{
	u32 total = (u32)atomic_add_return((int)gpon_rd(GPON_GTC_DS_MISC_CNTR_BWM_ACPT),
					   &gpon_bwm_acpt_total);
	u32 delta = total - *last;

	*last = total;
	return delta;
}

/* RTL9607C SWCORE proxy: the I2C-indirect registers ... -- dev/MEASURED-luna_gpon.c.md sec 62. */
#define   SW_MDX_M_EN		BIT(10)

/* ★ THE PROXY BODIES ARE THE FAMILY'S, in luna_gpon_regs.h. ...
 * dev/MEASURED-luna_gpon.c.md sec 63. */


static inline u32 sw_proxy_rd(u32 swc_off)
{
	return luna_sw_proxy_rd(swcore_base, swc_off);
}

static inline void sw_proxy_wr(u32 swc_off, u32 val)
{
	luna_sw_proxy_wr(swcore_base, swc_off, val);
}

/*
 * SWCORE 32-bit access. On the 9607C route the I2C-indirect hole (0xB0-0xD8)
 * through the PHY-10 proxy; everything else (and all of the 9602C) is direct.
 */
static u32 sw_rd(u32 off)
{
	if (is_9607c && i2c_proxy && off >= 0xB0u && off <= 0xD8u)
		return sw_proxy_rd(off);
	return ioread32(swcore_base + off);
}

static void sw_wr(u32 off, u32 v)
{
	if (is_9607c && i2c_proxy && off >= 0xB0u && off <= 0xD8u) {
		sw_proxy_wr(off, v);
		return;
	}
	iowrite32(v, swcore_base + off);
}

static inline u32 pi_rd(u32 off) { return ioread32(ponip_base + off); }
static inline void pi_wr(u32 off, u32 v) { iowrite32(v, ponip_base + off); }

/* The three register blocks as flowcore `struct hwio`, so the ...
 * dev/MEASURED-luna_gpon.c.md sec 64. */
static u32  sw_hwio_rd(void *ctx, u32 off)		{ return sw_rd(off); }
static void sw_hwio_wr(void *ctx, u32 off, u32 v)	{ sw_wr(off, v); }
static const struct hwio sw_io = { .rd = sw_hwio_rd, .wr = sw_hwio_wr };
static u32  pi_hwio_rd(void *ctx, u32 off)		{ return pi_rd(off); }
static void pi_hwio_wr(void *ctx, u32 off, u32 v)	{ pi_wr(off, v); }
static const struct hwio pi_io = { .rd = pi_hwio_rd, .wr = pi_hwio_wr };
static u32  gpon_hwio_rd(void *ctx, u32 off)		{ return gpon_rd(off); }
static void gpon_hwio_wr(void *ctx, u32 off, u32 v)	{ gpon_wr(off, v); }
static const struct hwio gpon_io = { .rd = gpon_hwio_rd, .wr = gpon_hwio_wr };

/* The shell's sleep for the shared indirect-CAM helper (regtable.h) -- the
 * r960_delay_us idiom from luna_ponmac.c: the flowcore tier cannot call
 * udelay() itself, time is an explicit input there so the same function runs
 * on x86 under the write-stream differential. */
static void gpon_cam_delay_us(unsigned int us)
{
	udelay(us);
}

/* gpon_ind_poll()/gpon_ind_go() (flowcore/regtable.h) take ...
 * dev/MEASURED-luna_gpon.c.md sec 65. */
static void pi_pause_1us(void)
{
	udelay(1);
}

/* PONIP_DBG_CTRL_US strobe -- ONE spelling of a sequence this ...
 * dev/MEASURED-luna_gpon.c.md sec 66. */
static int pi_sid_page_cnt(u32 sid, u32 *cnt)
{
	int rc = gpon_ind_go(&pi_io, reg_make(0x255c),
			     0x00086000u | (sid & 0x7fu) | (1u << 7),
			     1u << 9, 2000, pi_pause_1us);

	*cnt = pi_rd(0x2564);
	return rc;
}

/* Read-modify-write the bit-field [msb:lsb] of the SWCORE register at off. */
static void sw_field(u32 off, unsigned int msb, unsigned int lsb, u32 val)
{
	hwio_rmw(&sw_io, off, msb, lsb, val);
}

/* Read-modify-write the bit-field [msb:lsb] of the PON-IP register at off. */
static void pi_field(u32 off, unsigned int msb, unsigned int lsb, u32 val)
{
	hwio_rmw(&pi_io, off, msb, lsb, val);
}

/* Packed pi-register array entry write (shared locate: pi_packed_locate in
 * flowcore_hash.c). Defined below with the SID2QID helpers; forward-
 * declared so the early scheduler init writes WFQ_TYPE through the SAME
 * addressing as everything else. */
static void pi_packed_set(u32 base, unsigned int idx, unsigned int bits, u32 val);
static u32 pi_packed_get(u32 base, unsigned int idx, unsigned int bits);

/* Front-panel PON/LOS LEDs. The stock unit lights the green ...
 * dev/MEASURED-luna_gpon.c.md sec 67. */
static bool gpon_leds = true;
module_param(gpon_leds, bool, 0644);
MODULE_PARM_DESC(gpon_leds, "drive the front-panel PON/LOS LEDs from GPON state (default on)");

static u32 gpon_led_pon_val = ~0u;	/* last value written (forces first update) */
static u32 gpon_led_los_val = ~0u;

/* The panel map is the BOARD's, from its DT gpon node: realtek,pon-led,
 * realtek,los-led and realtek,port-leds = <index type>... (hardware link/
 * activity). A board that declares nothing gets no lamp driven: the X111W's
 * indices on another board drove pins the panel does not use (V2801RGW,
 * 2026-10-07). What the lamps SAY is the same on every ONU (below). */
static u32 led_pon_idx = LED_IDX_NONE;
static u32 led_los_idx = LED_IDX_NONE;
static u32 led_ports[8];
static int led_nports;

static void __init luna_led_map_from_dt(void)
{
	struct device_node *np = of_find_node_by_path("/gpon");
	int n;

	if (!np)
		return;
	of_property_read_u32(np, "realtek,pon-led", &led_pon_idx);
	of_property_read_u32(np, "realtek,los-led", &led_los_idx);
	n = of_property_count_u32_elems(np, "realtek,port-leds");
	if (n > 0 && n % 2 == 0 && n <= (int)ARRAY_SIZE(led_ports) &&
	    !of_property_read_u32_array(np, "realtek,port-leds", led_ports, n))
		led_nports = n / 2;
	else if (n > 0)
		pr_err("luna-gpon: realtek,port-leds holds %d cells, not <index type> pairs (max %zu) -- no port LED driven\n",
		       n, ARRAY_SIZE(led_ports) / 2);
	of_node_put(np);
}

static bool gpon_led_valid(unsigned int idx)
{
	return swc->led && idx < swc->led->indices;
}

static u32 gpon_led_force_reg(unsigned int idx)
{
	return swc->led->force_value + idx / 16 * 4;
}

/* Drive one LED index's 2-bit parallel force value (0=off 1=on 2=blink). */
static void gpon_led_force(unsigned int idx, u32 val)
{
	if (!gpon_led_valid(idx))
		return;
	sw_field(gpon_led_force_reg(idx), idx % 16 * 2 + 1, idx % 16 * 2, val);
}

static u32 gpon_led_force_get(unsigned int idx)
{
	return (sw_rd(gpon_led_force_reg(idx)) >> (idx % 16 * 2)) & 3;
}

/* Parallel output and pad output for one index. */
static void gpon_led_enable(unsigned int idx)
{
	sw_field(swc->led->para_en, idx + 1, idx + 1, 1);
	sw_field(swc->led->pad_en, idx, idx, 1);
}

/* Claim one index as a CPU-forced PON-typed lamp, the word stock writes. */
static void gpon_led_claim(unsigned int idx)
{
	sw_wr(LED_DATA_CFG(idx), swc->led->pon_type << 16 | BIT(swc->led->cpu_force_bit));
	gpon_led_enable(idx);
}

/* Configure one LED index as a hardware-auto port-link/activity LED: the switch
 * lights it directly from the port's link state + traffic, no CPU involvement,
 * so plug/unplug is reflected with zero software. CPU_FORCE_MOD stays 0. */
static void gpon_led_port(unsigned int idx, u32 type)
{
	sw_wr(LED_DATA_CFG(idx), (type << 16) | swc->led->link_act);
	gpon_led_enable(idx);
}

/* One panel language on every ONU, so a field technician reads any unit alike
 * (operator, 2026-10-07): PON green solid at O5 and blinking in every other
 * state, no fibre included -- stock's own table turns it off at O1; LOS red
 * blinking while the downstream light is lost. Both blink at one rate: the
 * 9603CVD has a single force-blink period. */
static void gpon_led_pon_set(u8 st)
{
	u32 v = st == 5 ? LED_FORCE_ON : LED_FORCE_BLINK;

	if (!gpon_leds || v == gpon_led_pon_val)
		return;
	gpon_led_force(led_pon_idx, v);
	gpon_led_pon_val = v;
}

static void gpon_led_los_set(bool los)
{
	u32 v = los ? LED_FORCE_BLINK : LED_FORCE_OFF;

	if (!gpon_leds || v == gpon_led_los_val)
		return;
	gpon_led_force(led_los_idx, v);
	gpon_led_los_val = v;
}

/* ---- the STANDARD interface: /sys/class/leds ----------------------------
 *
 * The panel was already driven (gpon_led_force/_claim/_port above) and the
 * lamps were reachable from NOWHERE outside this file, so every led_* suite
 * case blocked on "this image exposes no front-panel LED instrument" -- true
 * about /sys/class/leds and misleading about the port. This registers the two
 * lamps the CPU actually FORCES, so their brightness is the truth and not a
 * guess; the driver keeps driving them from GPON state and a userspace write
 * is an override the next state change takes back.
 *
 * ⚠ THE LINK LAMPS (FE/GE) ARE DELIBERATELY NOT REGISTERED. They are
 * hardware-auto: the switch lights them from its own port link+activity and
 * CPU_FORCE_MOD stays 0, so their force field reads 0 while the lamp is LIT.
 * Publishing that as brightness=0 would be a counter that reads healthy while
 * describing something else -- the exact defect this tree keeps paying for.
 * Reporting them honestly means reading the PORT's link state, which lives in
 * the Ethernet driver; that is owed work, not a thing to fake here. */
struct luna_panel_led {
	struct led_classdev cdev;
	unsigned int idx;
	bool registered;
};

static struct luna_panel_led luna_panel_leds[] = {
	{ .cdev = { .name = "green:pon", .max_brightness = 1 } },
	{ .cdev = { .name = "red:los",   .max_brightness = 1 } },
};

static void luna_panel_led_set(struct led_classdev *c, enum led_brightness b)
{
	struct luna_panel_led *l = container_of(c, struct luna_panel_led, cdev);

	gpon_led_force(l->idx, b ? LED_FORCE_ON : LED_FORCE_OFF);
}

static enum led_brightness luna_panel_led_get(struct led_classdev *c)
{
	struct luna_panel_led *l = container_of(c, struct luna_panel_led, cdev);
	/* ⚠ SWCORE, not the PON IP. The first cut read this with pi_packed_get(),
	 * whose pi_rd() addresses a DIFFERENT BLOCK -- it would have returned a
	 * confident brightness for a register that is not this lamp's. */
	return gpon_led_force_get(l->idx) == LED_FORCE_OFF ? LED_OFF : LED_FULL;
}

static void gpon_led_sysfs_init(void)
{
	unsigned int i;
	int rc;

	luna_panel_leds[0].idx = led_pon_idx;
	luna_panel_leds[1].idx = led_los_idx;
	for (i = 0; i < ARRAY_SIZE(luna_panel_leds); i++) {
		if (!gpon_led_valid(luna_panel_leds[i].idx))
			continue;
		luna_panel_leds[i].cdev.brightness_set = luna_panel_led_set;
		luna_panel_leds[i].cdev.brightness_get = luna_panel_led_get;
		rc = led_classdev_register(NULL, &luna_panel_leds[i].cdev);
		if (rc) {
			pr_warn("luna-gpon: LED %s not exposed at /sys/class/leds (%d)\n",
				luna_panel_leds[i].cdev.name, rc);
			continue;
		}
		luna_panel_leds[i].registered = true;
	}
}

static void gpon_led_sysfs_exit(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(luna_panel_leds); i++) {
		if (luna_panel_leds[i].registered) {
			led_classdev_unregister(&luna_panel_leds[i].cdev);
			luna_panel_leds[i].registered = false;
		}
	}
}

/* Put the controller in parallel mode and claim the board's indices. Only the
 * indices the board declares are touched, so any other panel LED keeps its
 * power-on configuration. Every board has at least PON and LOS: a die with no
 * table or a board with no map is an error, never a quiet panel. */
static void gpon_led_init(void)
{
	const struct luna_led_regs *r = swc->led;
	int i;

	if (!gpon_leds)
		return;
	if (!r) {
		pr_err("luna-gpon: panel LEDs not driven (%s: no LED-controller table for this die)\n",
		       swc->chip);
		return;
	}
	if (!gpon_led_valid(led_pon_idx) || !gpon_led_valid(led_los_idx)) {
		pr_err("luna-gpon: panel LEDs not driven (%s: the DT gpon node declares no valid PON/LOS lamp)\n",
		       swc->chip);
		return;
	}
	sw_wr(r->para_en, sw_rd(r->para_en) & ~r->seri_para);
	sw_wr(r->pad_en, sw_rd(r->pad_en) & ~r->seri_pad);
	sw_field(LED_MODE_SEL, 0, 0, 0);			/* parallel output   */
	sw_field(r->blink_rate, r->blink_lsb + 2, r->blink_lsb, LED_BLINK_512MS);
	gpon_led_claim(led_pon_idx);
	gpon_led_claim(led_los_idx);
	gpon_led_force(led_pon_idx, LED_FORCE_BLINK);	/* not yet O5 */
	gpon_led_force(led_los_idx, LED_FORCE_OFF);
	gpon_led_pon_val = LED_FORCE_BLINK;
	gpon_led_los_val = LED_FORCE_OFF;

	/* Ethernet port-link LEDs, hardware-auto: the switch lights each from its
	 * OWN port's link + activity, no CPU. */
	for (i = 0; i < led_nports; i++)
		if (gpon_led_valid(led_ports[2 * i]))
			gpon_led_port(led_ports[2 * i], led_ports[2 * i + 1]);

	pr_info("luna-gpon: panel LEDs init (%s: PON idx%u / LOS idx%u force-mode; %d port LED(s) link-auto)\n",
		swc->chip, led_pon_idx, led_los_idx, led_nports);

	/* Only now: the class devices describe lamps this function just claimed,
	 * so a die that returned early above exposes NOTHING rather than a node
	 * whose brightness nobody drives. */
	gpon_led_sysfs_init();
}


/* Read-modify-write the bit-field [msb:lsb] of the GPON-block register at off.
 * Same canonical mask RMW as sw_field/pi_field (flowcore hwio.h). */
static void gpon_field(u32 off, unsigned int msb, unsigned int lsb, u32 val)
{
	hwio_rmw(&gpon_io, off, msb, lsb, val);
}

/* DEV live register poke (uses the driver's own ioremap — no ...
 * dev/MEASURED-luna_gpon.c.md sec 280. */
static int bosa_read_reg(u16 reg);	/* fwd decl: poke_set 'b'/'B' use these (defined below) */
static int bosa_write_reg(u16 reg, u8 val);
static char poke_buf[8];
static int poke_set_locked(const char *val, const struct kernel_param *kp);
static int poke_set(const char *val, const struct kernel_param *kp)
{
	int ret;

	mutex_lock(&bosa_lock);
	ret = luna_stopping ? -ESHUTDOWN : poke_set_locked(val, kp);
	mutex_unlock(&bosa_lock);
	return ret;
}

static int poke_set_locked(const char *val, const struct kernel_param *kp)
{
	char c = 0;
	unsigned int off = 0, v = 0;
	int n = sscanf(val, " %c %x %x", &c, &off, &v);

	if (n < 2) {
		pr_info("luna-gpon: poke usage: <g|G|p|P> <hexoff> [hexval]\n");
		return 0;
	}
	/* Raw writes cannot bypass the indirect-CAM owner or republish a live
	 * data path behind its ledger. Read-only diagnostics remain available. */
	if ((c == 'G' || c == 'P' || c == 'M') && READ_ONCE(luna_driver_ready))
		return -EBUSY;
	switch (c) {
	case 'g':
		pr_info("luna-gpon: poke GTC[%#x]=%#x\n", off, gpon_rd(off));
		break;
	case 'G':
		gpon_wr(off, v);
		pr_info("luna-gpon: poke GTC[%#x]<=%#x ->%#x\n", off, v, gpon_rd(off));
		break;
	case 'p':
		pr_info("luna-gpon: poke PI[%#x]=%#x\n", off, pi_rd(off));
		break;
	case 'P':
		pi_wr(off, v);
		pr_info("luna-gpon: poke PI[%#x]<=%#x ->%#x\n", off, v, pi_rd(off));
		break;
	case 'm': {	/* generic phys read (GMAC 0x18012048 CPUTAGCR, SWCORE 0x1b00xxxx) */
		void __iomem *a = ioremap(off, 4);
		if (a) { pr_info("luna-gpon: poke MEM[%#x]=%#x\n", off, ioread32(a)); iounmap(a); }
		break;
	}
	case 'M': {	/* generic phys write */
		void __iomem *a = ioremap(off, 4);
		if (a) { iowrite32(v, a); pr_info("luna-gpon: poke MEM[%#x]<=%#x ->%#x\n", off, v, ioread32(a)); iounmap(a); }
		break;
	}
	case 'b':	/* BOSA I2C read (12-bit reg; slave banking internal) — live US-TX-vs-stock diff */
		pr_info("luna-gpon: poke BOSA[%#x]=%#x\n", off, bosa_read_reg(off));
		break;
	case 'B':	/* BOSA I2C write — live laser/extinction tweak */
		bosa_write_reg(off, v);
		pr_info("luna-gpon: poke BOSA[%#x]<=%#x ->%#x\n", off, v, bosa_read_reg(off));
		break;
	default:
		pr_info("luna-gpon: poke bad cmd '%c'\n", c);
		break;
	}
	return 0;
}
static const struct kernel_param_ops poke_ops = { .set = poke_set };
module_param_cb(poke, &poke_ops, poke_buf, 0644);
MODULE_PARM_DESC(poke, "DEV: <g|G|p|P> <hexoff> [hexval] live GTC/PON-IP reg read/write");

/* RTL8290B BOSA state captured at probe for /proc display (-1 ...
 * dev/MEASURED-luna_gpon.c.md sec 69. */
static bool bosa_not_8290b;		/* set by bosa_probe() on a positive mismatch */
static int bosa_id_num __ro_after_init = -1;
static int bosa_id_vid __ro_after_init = -1;
static int bosa_w41 __ro_after_init = -1;
static int bosa_ctrl2 __ro_after_init = -1;
static int bosa_status2 __ro_after_init = -1;

/* Does this board HAVE an RTL8290B register interface to ...
 * dev/MEASURED-luna_gpon.c.md sec 70. */
static bool bosa_regs_live(void)
{
	return bosa_id_num == 0x8290 && !bosa_not_8290b;
}

/* Read one 8-bit register from an I2C slave via the SoC ...
 * dev/MEASURED-luna_gpon.c.md sec 71. */
#define I2C_XACT_SETTLE_US	600u
/* ⚠ THE NACK IS LATCHED ACROSS THE WHOLE TRANSACTION, not ...
 * dev/MEASURED-luna_gpon.c.md sec 72. */
static int i2c_wait_done(u32 ind_cmd, u32 *cmd_out)
{
	bool nacked = false;
	int i;

	/* sleep through the transfer: a 50 kHz read is ~0.8 ms, and spinning it
	 * cost ~3% CPU on the 50 ms laser-maintenance loop */
	usleep_range(I2C_XACT_SETTLE_US, I2C_XACT_SETTLE_US + 100);
	for (i = 0; i < I2C_BUSY_POLL_MAX; i++) {
		u32 cmd = sw_rd(ind_cmd);

		if (cmd_out)
			*cmd_out = cmd;
		nacked |= !!(cmd & I2C_CMD_NACK);
		if (!(cmd & (I2C_CMD_EN | I2C_CMD_BUSY)))
			return nacked ? -EIO : 0;
		udelay(10);
	}
	return -ETIMEDOUT;
}

static int bosa_i2c_read8(u8 slave, u8 reg)
{
	u32 cfg, pad_off = SOC_IO_MODE_EN;	/* per-chip: gpon_swc_map */
	u32 bus_off = bosa_i2c_bus * I2C_BUS_STRIDE;
	u32 i2c_pad = IO_I2C_EN_BUS0 + bosa_i2c_bus;
	u32 cfg_off = I2C_CONFIG0 + bus_off;
	u32 ind_adr = I2C_IND_ADR + bus_off;
	u32 ind_cmd = I2C_IND_CMD + bus_off;
	u32 ind_rd  = I2C_IND_RD + bus_off;
	int ret;

	lockdep_assert_held(&bosa_lock);

	/* Route I2C bus 0 to its pads. I2C_EN is a 2-bit field (one bit per bus)
	 * whose position MOVES: [14:13] on the 9602C/9607C, [12:11] on the
	 * 9603CVD. Writing bit 13 on a 9603CVD hits SLIC_ISI_EN, not I2C. */
	sw_field(pad_off, i2c_pad, i2c_pad, 1);

	/* CONFIG: slave addr, 8-bit reg-addr + 8-bit data, the existing divider. Preserve the
	 * electrical bits (open-drain / mode / delays) already programmed. */
	cfg = sw_rd(cfg_off);
	cfg &= ~((((1u << 7) - 1) << I2C_CFG_DEV_ID_LSB) |
		 (0x3u << I2C_CFG_AW_LSB) | (0x3u << I2C_CFG_DW_LSB) |
		 (0x3ffu << I2C_CFG_CLKDIV_LSB));
	cfg |= ((u32)(slave & 0x7f) << I2C_CFG_DEV_ID_LSB) |
	       (I2C_CLKDIV_50K << I2C_CFG_CLKDIV_LSB);
	sw_wr(cfg_off, cfg);

	sw_wr(ind_adr, reg);
	sw_wr(ind_cmd, I2C_CMD_EN);			/* RW_EN=0 -> read */

	ret = i2c_wait_done(ind_cmd, NULL);
	if (!ret)
		ret = (int)(sw_rd(ind_rd) & 0xff);

	/* Reconnect the shared optical-SD pad so optic_los stays live (see
	 * bosa_i2c_restore_pad). */
	if (bosa_i2c_restore_pad)
		sw_field(pad_off, i2c_pad, i2c_pad, 0);
	return ret;
}

/* RTL9607C optical-module probe (M3 diagnostic): read the ...
 * dev/MEASURED-luna_gpon.c.md sec 73. */
static void ddm_probe_9607c(void)
{
	u32 direct, proxy;
	int v0, v1, i, lane;
	u16 rxpwr;
	char vendor[17];

	/* SerDes-lane scan: SDS_FIB_STATUS is a 3-lane array (0x28c, stride 0x20).
	 * Look for signal-detect on ANY lane in case the optical RX is not lane 0. */
	for (lane = 0; lane < 3; lane++) {
		u32 fs = ioread32(swcore_base + 0x28cu + lane * 0x20u);

		pr_info("luna-gpon: SCAN sds_lane%d fib_sts=0x%08x sdet=%u link_ok=%u fib100=%u\n",
			lane, fs, !!(fs & BIT(17)), !!(fs & BIT(4)), !!(fs & BIT(2)));
	}

	direct = ioread32(swcore_base + SDS_CFG);	/* SDS_CFG: directly mapped */
	proxy  = sw_proxy_rd(SDS_CFG);
	pr_info("luna-gpon: DDM proxy self-test SDS_CFG direct=0x%x proxy=0x%x %s\n",
		direct, proxy, direct == proxy ? "OK" : "PROXY-BROKEN");

	{
		int raw[16];

		for (i = 0; i < 16; i++)
			raw[i] = bosa_i2c_read8(0x50, 20 + i);
		bosa_sff_text(vendor, raw, 16);	/* sanitize: luna_gpon_logic.c */
	}
	pr_info("luna-gpon: DDM A0(0x50) vendor='%s'\n", vendor);

	v0 = bosa_i2c_read8(0x51, 104);
	v1 = bosa_i2c_read8(0x51, 105);
	if (v0 < 0 || v1 < 0) {
		pr_info("luna-gpon: DDM A2(0x51) rx_power read failed (v0=%d v1=%d) — module/i2c not responding\n",
			v0, v1);
		return;
	}
	rxpwr = ((u16)v0 << 8) | (u16)v1;
	pr_info("luna-gpon: DDM A2 rx_power raw=0x%04x = %u.%u uW (light %s)\n",
		rxpwr, rxpwr / 10, rxpwr % 10, rxpwr ? "PRESENT" : "ABSENT");
}

/* Read one byte from (bus, slave, reg); *cmd_out = raw I2C_IND_CMD (NACK bit3). */
static int i2c_rd_bus(int bus, u8 slave, u8 reg, u32 *cmd_out)
{
	/* Per-bus stride off the chip's OWN indirect block. Every ...
	 * dev/MEASURED-luna_gpon.c.md sec 74. */
	u32 cfg_off = I2C_CONFIG0 + bus * I2C_BUS_STRIDE;
	u32 ind_adr = I2C_IND_ADR + bus * I2C_BUS_STRIDE;
	u32 ind_cmd = I2C_IND_CMD + bus * I2C_BUS_STRIDE;
	u32 ind_rd  = I2C_IND_RD  + bus * I2C_BUS_STRIDE;
	int ret;
	u32 cfg, cmd = 0;

	sw_field(SOC_IO_MODE_EN, IO_I2C_EN_BUS0 + bus, IO_I2C_EN_BUS0 + bus, 1);
	cfg = sw_rd(cfg_off);
	cfg &= ~((((1u << 7) - 1) << I2C_CFG_DEV_ID_LSB) |
		 (0x3u << I2C_CFG_AW_LSB) | (0x3u << I2C_CFG_DW_LSB) |
		 (0x3ffu << I2C_CFG_CLKDIV_LSB));
	cfg |= ((u32)(slave & 0x7f) << I2C_CFG_DEV_ID_LSB) |
	       (I2C_CLKDIV_50K << I2C_CFG_CLKDIV_LSB);
	sw_wr(cfg_off, cfg);
	sw_wr(ind_adr, reg);
	sw_wr(ind_cmd, I2C_CMD_EN);				/* read */
	ret = i2c_wait_done(ind_cmd, &cmd);
	if (!ret)
		ret = (int)(sw_rd(ind_rd) & 0xff);
	if (cmd_out)
		*cmd_out = cmd;
	return ret;
}

/* Scan both i2c buses for the common optical-module slaves. */
static void i2c_scan_9607c(void)
{
	/* 0x50 and 0x51 are the SFF-8472 standard addresses: page 0 / ...
	 * dev/MEASURED-luna_gpon.c.md sec 75. */
	static const u8 slaves[] = {
		0x50,	/* SFF-8472 A0 identification page */
		0x51,	/* SFF-8472 A2 diagnostics page (DDM) */
		0x54,	/* vendor page -- the analog/APC "W" registers on Board C */
	};
	int bus;
	unsigned int k;

	/* ★ WAS `k < 3`, a hand-written bound beside a three-entry array. It could
	 * only ever drift one way: add a slave and the scan silently keeps probing
	 * three. ARRAY_SIZE cannot. (k is unsigned because ARRAY_SIZE is size_t.) */
	for (bus = 0; bus < 2; bus++)
		for (k = 0; k < ARRAY_SIZE(slaves); k++) {
			u32 cmd = 0;
			int b = i2c_rd_bus(bus, slaves[k], 0, &cmd);

			pr_info("luna-gpon: SCAN i2c bus%d slave0x%02x reg0=%d cmd=0x%08x %s\n",
				bus, slaves[k], b, cmd,
				(cmd & I2C_CMD_NACK) ? "NACK(no-dev)" :
				(cmd & I2C_CMD_BUSY) ? "BUSY(timeout)" : "ACK");
		}
}

/* Probe the external RTL8290B "Europa" BOSA over I2C by ...
 * dev/MEASURED-luna_gpon.c.md sec 76. */
static int bosa_read_reg(u16 reg)
{
	return bosa_i2c_read8(bosa_slave_for(reg), reg & 0xff);
}

/* Read a big-endian 16-bit value from two consecutive BOSA ...
 * dev/MEASURED-luna_gpon.c.md sec 77. */
static int bosa_read16(u16 reg_hi)
{
	int h = bosa_read_reg(reg_hi);
	int l = bosa_read_reg(reg_hi + 1);

	if (h < 0 || l < 0)
		return -1;
	return ((h & 0xff) << 8) | (l & 0xff);
}

/* Median of three optical samples, taken under the caller's group mutex.
 * Returns -1 only if every sample failed. */
static int bosa_read16_median(u16 reg_hi)
{
	u32 v[3];
	unsigned int n = 0;
	int i;

	for (i = 0; i < 3; i++) {
		int r = bosa_read16(reg_hi);

		if (r >= 0)
			v[n++] = (u32)r;
	}
	if (n == 0)
		return -1;
	/* selection rule (sort + upper median): luna_gpon_logic.c */
	return (int)bosa_median_u32(v, n);
}

/* ANI-G (ME 263) live optical levels -- dev/MEASURED-luna_gpon.c.md sec 78. */
static s16 anig_rx_level = (s16)0xeedc;		/* #10 Optical signal level (DS RX) */
static s16 anig_tx_level = (s16)0x04d7;		/* #14 Transmit optical level (TX)  */

/* The module's own SFF-8472 A2h monitor, decoded. -> enum gpon_ddm_status */
static int bosa_a2h_read(struct gpon_ddm_a2h *d)
{
	u8 raw[GPON_DDM_A2H_LEN];
	int i, err = 0;

	for (i = 0; i < GPON_DDM_A2H_LEN; i++) {
		int b = bosa_i2c_read8(0x51, GPON_DDM_A2H_BASE + i);

		err |= b < 0;
		raw[i] = b < 0 ? 0 : b & 0xff;
	}
	return gpon_ddm_a2h_decode(raw, err, d);
}

static void bosa_seq_uw10(struct seq_file *s, u16 uw10)
{
	if (uw10)
		seq_printf(s, "%d", gpon_ddm_uw10_to_cdbm(uw10));
	else
		seq_puts(s, "-inf");
}

/* A module without RTL8290B registers is read the standard way, in the Cortina
 * shell's /proc format: every level beside the raw block it came from. */
static void bosa_a2h_show(struct seq_file *s)
{
	struct gpon_ddm_a2h d;
	unsigned int i;

	if (bosa_a2h_read(&d) != GPON_DDM_OK) {
		seq_printf(s, "optic_ddm      = %s\n", gpon_ddm_status_str(d.status));
		return;
	}
	seq_printf(s, "optic_ddm      = live (SFF-8472 A2h 0x%02x-0x%02x)\n",
		   GPON_DDM_A2H_BASE, GPON_DDM_A2H_BASE + GPON_DDM_A2H_LEN - 1);
	seq_printf(s, "optic_rx_raw: 0x%04x optic_rx_cdbm: ", d.rx_pwr);
	bosa_seq_uw10(s, d.rx_pwr);
	seq_printf(s, " optic_tx_raw: 0x%04x\n", d.tx_pwr);
	seq_printf(s, "optic_env:   temp_dc=%d bias_ua=%u tx_cdbm=",
		   gpon_ddm_temp_dc(d.temp), gpon_ddm_bias_ua(d.bias));
	bosa_seq_uw10(s, d.tx_pwr);
	seq_printf(s, " vcc_mv=%u\n", gpon_ddm_vcc_mv(d.vcc));
	seq_printf(s, "optic_ddm_raw: %02x..%02x =",
		   GPON_DDM_A2H_BASE, GPON_DDM_A2H_BASE + GPON_DDM_A2H_LEN - 1);
	for (i = 0; i < GPON_DDM_A2H_LEN; i++)
		seq_printf(s, " %02x", d.raw[i]);
	seq_putc(s, '\n');
}

/* Sample the calibrated DDM optical words and refresh the ANI-G level cache.
 * Called from process work at a slow cadence (never from the GET path);
 * a "not available" word keeps the previous cached value. */
static bool gpon_optical_cache_poll(void)
{
	s32 rx, tx;

	/* A module that is not an RTL8290B has no RTL8290B DDM words: 0x166/0x168
	 * are bytes of its identity EEPROM. The G24W's GN25L95 reported -30.00 dBm
	 * RX to the OLT while its own A2h monitor read -8.87 (2026-09-30). Ask it the
	 * standard way, through the decode the Cortina shell uses too. */
	if (bosa_gn_identified || bosa_not_8290b) {
		struct gpon_ddm_a2h d;

		/* a zero power word is no measurement: keep the previous level */
		if (bosa_a2h_read(&d) != GPON_DDM_OK || !d.rx_pwr || !d.tx_pwr)
			return false;
		anig_rx_level = gpon_ddm_cdbm_to_anig(gpon_ddm_uw10_to_cdbm(d.rx_pwr));
		anig_tx_level = gpon_ddm_cdbm_to_anig(gpon_ddm_uw10_to_cdbm(d.tx_pwr));
		return true;
	}
	rx = ddm_word_to_level(bosa_read16_median(0x168));	/* RX */
	tx = ddm_word_to_level(bosa_read16_median(0x166));	/* TX */
	if (rx == INT_MIN || tx == INT_MIN)
		return false;
	anig_rx_level = (s16)rx;
	anig_tx_level = (s16)tx;
	return true;
}

/* One process worker owns the 50 ms RTL servo and separate 3 s DDM. */
static struct delayed_work gpon_optical_work;
static void gpon_optical_work_fn(struct work_struct *w);

/* Write one 8-bit register to an I2C slave via the SoC HW I2C ...
 * dev/MEASURED-luna_gpon.c.md sec 79. */
static int bosa_i2c_write_raw(u8 slave, u8 reg, u8 val)
{
	u32 cfg, bus_off = bosa_i2c_bus * I2C_BUS_STRIDE;
	int ret;


	lockdep_assert_held(&bosa_lock);

	sw_field(SOC_IO_MODE_EN, IO_I2C_EN_BUS0 + bosa_i2c_bus, IO_I2C_EN_BUS0 + bosa_i2c_bus, 1);

	cfg = sw_rd(I2C_CONFIG0 + bus_off);
	cfg &= ~((((1u << 7) - 1) << I2C_CFG_DEV_ID_LSB) |
		 (0x3u << I2C_CFG_AW_LSB) | (0x3u << I2C_CFG_DW_LSB) |
		 (0x3ffu << I2C_CFG_CLKDIV_LSB));
	cfg |= ((u32)(slave & 0x7f) << I2C_CFG_DEV_ID_LSB) |
	       (I2C_CLKDIV_50K << I2C_CFG_CLKDIV_LSB);
	sw_wr(I2C_CONFIG0 + bus_off, cfg);

	sw_wr(I2C_IND_ADR + bus_off, reg);
	sw_wr(I2C_IND_WD + bus_off, val);
	sw_wr(I2C_IND_CMD + bus_off, I2C_CMD_EN | I2C_CMD_RW_WR);

	ret = i2c_wait_done(I2C_IND_CMD + bus_off, NULL);

	/* Reconnect the shared optical-SD pad so optic_los stays live (see
	 * bosa_i2c_restore_pad). */
	if (bosa_i2c_restore_pad)
		sw_field(SOC_IO_MODE_EN, IO_I2C_EN_BUS0 + bosa_i2c_bus,
			 IO_I2C_EN_BUS0 + bosa_i2c_bus, 0);
	return ret;
}

static int bosa_i2c_write8(u8 slave, u8 reg, u8 val)
{
	/* Refuse to write RTL8290B registers into a module that is ...
	 * dev/MEASURED-luna_gpon.c.md sec 80. */
	if (bosa_not_8290b || bosa_id_num != 0x8290) {
		pr_warn_once("luna-gpon: BOSA writes REFUSED -- no POSITIVE RTL8290B identification (chip id=0x%04x, want 0x8290%s); reg 0x%02x@0x%02x not written. A part-name string in the module's EEPROM is not evidence that its register interface exists.\n",
			     bosa_id_num,
			     bosa_not_8290b ? ", and SFF-8472 says it is another part" : "",
			     reg, slave);
		return -ENODEV;
	}

	return bosa_i2c_write_raw(slave, reg, val);
}

static int luna_gn_rd(void *ctx, u8 slave, u8 reg, u8 *val)
{
	int ret = bosa_i2c_read8(slave, reg);

	(void)ctx;
	if (ret < 0)
		return ret;
	*val = ret;
	return 0;
}

static int luna_gn_wr(void *ctx, u8 slave, u8 reg, u8 val)
{
	(void)ctx;
	return bosa_i2c_write_raw(slave, reg, val);
}

/* Caller holds bosa_lock across the entire paged probe and calibration. */
static int luna_gn_calibrate_locked(void)
{
	const struct firmware *fw;
	struct gn_op *ops;
	u8 notes[GN_PROBE_NOTES];
	struct gn_io io = { .rd = luna_gn_rd, .wr = luna_gn_wr,
		.notes = notes, .notes_max = ARRAY_SIZE(notes) };
	struct gn_fail fail = { 0 };
	int ret, n;

	if (bosa_regs_live())
		return -ENODEV;
	if (bosa_cal_ready)
		return 0;
	ret = request_firmware_direct(&fw, "rtkbosa_k.bin", NULL);
	if (ret)
		return ret;
	if (fw->size != GN_CAL_LEN) {
		ret = -EINVAL;
		goto release;
	}
	/* 7200 bytes exceeds the safe budget of the MIPS 8 KiB kernel stack. */
	ops = kcalloc(GN_OPS_MAX, sizeof(*ops), GFP_KERNEL);
	if (!ops) {
		ret = -ENOMEM;
		goto release;
	}
	if (!bosa_gn_identified) {
		n = gn25l95_probe_ops(ops, GN_OPS_MAX);
		ret = n < 0 ? n : gn25l95_cal_apply(ops, n, &io, &fail);
		if (ret)
			goto free;
		if (io.notes_n != GN_PROBE_NOTES || gn25l95_is_gn28l9x(notes)) {
			ret = -ENODEV;
			goto free;
		}
		/* This on-board part cannot change at runtime. Retain its positive
		 * physical identity after a partial calibration changes the cold
		 * probe's password predicate. An SFF name never establishes identity. */
		bosa_gn_identified = true;
	}
	io.notes = NULL;
	io.notes_max = 0;
	io.notes_n = 0;
	n = gn25l95_cal_ops(fw->data, fw->size, &gn_variant_g24w,
			      ops, GN_OPS_MAX);
	ret = n < 0 ? n : gn25l95_cal_apply(ops, n, &io, &fail);
	if (!ret)
		bosa_cal_ready = true;
free:
	if (ret)
		pr_err("luna-gpon: GN calibration refused: err=%d op=%u reg=0x%02x\n",
		       ret, fail.op, fail.reg);
	kfree(ops);
release:
	release_firmware(fw);
	return ret;
}

static int bosa_write_reg(u16 reg, u8 val)
{
	return bosa_i2c_write8(bosa_slave_for(reg), reg & 0xff, val);
}

/* Single-bit read-modify-write of a BOSA register. */
/* Masked field read-modify-write: val is the field value, placed at the mask's
 * low bit. */
static void bosa_set_field(u16 reg, u8 mask, u8 val)
{
	int r = bosa_read_reg(reg);
	u8 shift;

	if (r < 0 || !mask)
		return;
	shift = __ffs(mask);
	bosa_write_reg(reg, (r & ~mask) | ((val << shift) & mask));
}

/* One bit is a one-bit FIELD.  Every caller passes bit 0..7, so the mask fits
 * the u8 bosa_set_field takes, and (r & ~m) | ((set << bit) & m) computes the
 * same bytes the open-coded or/and-not did -- checked over all 97 call sites
 * before this became a wrapper (2026-09-04). */
static void bosa_set_bit(u16 reg, u8 bit, int set)
{
	bosa_set_field(reg, (u8)(1u << bit), set ? 1 : 0);
}

/* Read a masked field, right-justified. Returns 0 on I2C error. */
static u8 bosa_get_field(u16 reg, u8 mask)
{
	int r = bosa_read_reg(reg);

	if (r < 0 || !mask)
		return 0;
	return (r & mask) >> __ffs(mask);
}

/* Bounded poll of a BOSA status bit. Returns 1 if the bit reached @want before
 * the cap, 0 on timeout. @us is the per-iteration delay. */
static int bosa_poll_bit(u16 reg, u8 bit, int want, unsigned int us, int cap)
{
	int i;

	for (i = 0; i < cap; i++) {
		int r = bosa_read_reg(reg);

		if (r >= 0 && !!(r & (1u << bit)) == !!want)
			return 1;
		udelay(us);
	}
	return 0;
}

/* RTL8290B optical DDM: raw ADC -> calibrated dBm / temp / ...
 * dev/MEASURED-luna_gpon.c.md sec 81. */
#define BOSA_RX_CDBM_NA		S32_MIN

/* Per-board optical calibration (struct bosa_optical_cal: ...
 * dev/MEASURED-luna_gpon.c.md sec 281. */
static struct bosa_optical_cal bosa_cal = {
	.rx_vthr = 521509, .rx_r1 = 33000, .rx_r2 = 6200,
	.rx_poly_b = 8374, .rx_poly_c = 265, .temp_off = 20,
	.tx_slope = 2022, .tx_offset = 0,
};


/* 24-bit big-endian read of three consecutive BOSA analog-page registers. */
static u32 bosa_read24(u16 reg_hi)
{
	int a = bosa_read_reg(reg_hi);
	int b = bosa_read_reg(reg_hi + 1);
	int c = bosa_read_reg(reg_hi + 2);

	if (a < 0 || b < 0 || c < 0)
		return 0;
	return ((u32)(a & 0xff) << 16) | ((u32)(b & 0xff) << 8) | (u32)(c & 0xff);
}

/* Sample the sigma-delta ADC at a mux/gain byte: select the channel on reg
 * 0x212, kick the conversion, settle, then return the 24-bit code from
 * 0x30E-0x310 (stock rtl8290b_sdadc_code_get; RSSI uses gain bytes 0x82/0xC2). */
static u32 bosa_sdadc_read(u8 mux)
{
	u32 v;

	bosa_write_reg(0x212, mux);	/* channel/gain select, latch (bit3) clear */
	msleep(10);			/* ADC settle (stock: schedule_timeout(1)) */
	bosa_set_bit(0x212, 3, 1);	/* latch/convert trigger */
	v = bosa_read24(0x30E);
	bosa_set_bit(0x212, 3, 0);	/* ALWAYS release: a stuck bit3 freezes the
					 * result bank (0x302-0x316, temperature too) */
	return v;
}

/* Ratiometric RSSI voltage (micro-volts), re-expressed from ...
 * dev/MEASURED-luna_gpon.c.md sec 82. */
static u32 bosa_rssi_uv(void)
{
	u32 rssi   = bosa_sdadc_read(0xC2);	/* single in-range gain (0xC2) */
	u32 ref_lo = bosa_read24(0x314);	/* low reference tap (HW: 2.9M < RSSI) */
	u32 ref_hi = bosa_read24(0x305);	/* high reference tap (HW: 13.8M > RSSI) */
	u32 span   = (ref_hi > ref_lo) ? (ref_hi - ref_lo) : 1;
	u32 da     = (rssi > ref_lo) ? rssi - ref_lo : 0;

	/* ratiometric fraction in 1/10000 (0.221 -> 2210). The reference taps track
	 * the ADC gain/offset drift, so this is bench-stable where a raw single tap
	 * (0x311) is not. Averaging the two gain ranges was WRONG (they scale
	 * differently); one in-range gain is stable. */
	return (u32)div64_u64((u64)da * 10000, span);
}

/* Faithful RX power code (0.1uW): the shell samples the ...
 * dev/MEASURED-luna_gpon.c.md sec 282. */
static u32 bosa_rx_code(void)
{
	u32 rssi   = bosa_sdadc_read(0xC2);	/* single in-range gain */
	u32 tap_lo = bosa_read24(0x314);
	u32 tap_hi = bosa_read24(0x305);

	return bosa_rx_code_calc(rssi, tap_lo, tap_hi, &bosa_cal);
}

/* Median of 3 RX code reads -- de-noises the read-to-read variation (stock
 * medians 60; 3 is enough for a diagnostic and keeps the /proc read cheap).
 * Selection rule shared with bosa_read16_median (bosa_median_u32): NA == 0
 * sorts to the bottom, so one glitched sample is discarded. */
static u32 bosa_rx_code_median(void)
{
	u32 v[3];

	v[0] = bosa_rx_code();
	v[1] = bosa_rx_code();
	v[2] = bosa_rx_code();
	return bosa_median_u32(v, 3);
}

/* Public on-demand RX optical power in centi-dBm (for /proc + ...
 * dev/MEASURED-luna_gpon.c.md sec 83. */
static s32 bosa_rx_power_sff8472_cdbm(void)
{
	int hi = bosa_i2c_read8(0x51, 104);
	int lo = bosa_i2c_read8(0x51, 105);
	u32 uw;

	if (hi < 0 || lo < 0)
		return BOSA_RX_CDBM_NA;
	uw = ((u32)(hi & 0xff) << 8) | (u32)(lo & 0xff);
	if (uw == 0 || uw == 0xffffu)
		return BOSA_RX_CDBM_NA;
	return bosa_code_to_cdbm(uw);
}

static s32 bosa_rx_power_cdbm(void)
{
	u32 code;

	/* A module that is not an RTL8290B has no RTL8290B analog RSSI chain to
	 * read -- the "registers" that chain samples are bytes of its identity
	 * EEPROM. Ask it the standard way instead. */
	if (bosa_gn_identified || bosa_not_8290b)
		return bosa_rx_power_sff8472_cdbm();
	if (!bosa_regs_live())
		return BOSA_RX_CDBM_NA;

	code = bosa_rx_code_median();
	return code == BOSA_RX_CODE_NA ? BOSA_RX_CDBM_NA : bosa_code_to_cdbm(code);
}

/* Module temperature in deci-degC. Kelvin code from the temp ADC
 * (0x302[7:0]<<1 | 0x303[7], 233..383 K = -40..+110 C) minus the per-board
 * Kelvin trim (stock rtl8290b_temperature_get; stock stores SFF-8472 1/256 C). */
static s32 bosa_temp_dc(void)
{
	int a[14], b[14], i;

	/* 14 back-to-back samples of the free-running Kelvin ADC ...
	 * dev/MEASURED-luna_gpon.c.md sec 84. */
	for (i = 0; i < 14; i++) {
		a[i] = bosa_read_reg(0x302);
		b[i] = bosa_read_reg(0x303);
		if (a[i] < 0 || b[i] < 0)
			return INT_MIN;
	}
	return bosa_temp_dc_calc(a, b, bosa_cal.temp_off);
}

/* Laser bias current in micro-amps. 12-bit monitor code (0x321[7:0]<<4 |
 * 0x322[3:0]), full-scale ~100 mA at code 8192 (stock A2 word is 2 uA/LSB). */
static u32 bosa_bias_ua(void)
{
	int h = bosa_read_reg(0x321), l = bosa_read_reg(0x322);

	return bosa_bias_ua_calc(h, l);
}

/* ===== Live TX optical power (europa_drv ... -- dev/MEASURED-luna_gpon.c.md sec 85. */

static s32 bosa_vmpd_dark = INT_MIN;	/* MPD dark reference (mV); INT_MIN = not calibrated */
static u32 bosa_tx_dbg_code;		/* last MPD raw code + taps (optic_txchain diag) */
static s32 bosa_tx_dbg_hi, bosa_tx_dbg_zero;

/* One MPD voltage (mV) via SD-ADC ch2, ratiometric vs the reference taps.
 * tap_hi = 0x3B3 (live read) or 0x30B (dark cal). INT_MIN on ADC/tap error.
 * Reads code + taps while the latch (0x212 bit3) is held, then releases it. */
static s32 bosa_vmpd_mv(u16 tap_hi)
{
	u32 code;
	s32 hi, zero;

	bosa_set_bit(0x24A, 1, 0);		/* power up the ch2 (MPD) ADC */
	bosa_write_reg(0x212, 0x62);		/* ch2 select, latch (bit3) clear */
	msleep(10);				/* settle (stock: 1 jiffy @ HZ=100) */
	bosa_set_bit(0x212, 3, 1);		/* convert trigger */
	code = bosa_read24(0x30E) >> 2;
	bosa_set_bit(0x24A, 1, 1);		/* power down the ch2 ADC */
	hi   = (s32)(bosa_read24(tap_hi) >> 2);	/* ref tap (latched); positive 24-bit, plain >>2 */
	zero = (s32)(bosa_read24(0x314) >> 2);	/* zero tap */
	bosa_set_bit(0x212, 3, 0);		/* release the latch (after the taps) */
	bosa_tx_dbg_code = code;
	bosa_tx_dbg_hi = hi;
	bosa_tx_dbg_zero = zero;
	/* validity (dead-bus shape) + ratiometric mV: luna_gpon_logic.c */
	return bosa_vmpd_mv_calc(code, hi, zero);
}

/* One-time dark (laser-off) MPD calibration. MUST run at BOSA init BEFORE the
 * laser is enabled (forces TX off for ~200 ms); never at O5. */
static void bosa_vmpd_dark_calibrate(void)
{
	int save = bosa_read_reg(0x230), i, n = 0;
	s32 sum = 0, v;

	if (save < 0)
		return;
	bosa_write_reg(0x230, (save | 0xc0) & 0xff);	/* force TX off (bits 7:6) */
	bosa_set_field(0x24B, 0x0c, 3);			/* MPD mux = 3 (dark-cal path); set_field shifts by __ffs */
	for (i = 0; i < 20; i++) {
		v = bosa_vmpd_mv(0x30B);		/* dark-cal reference tap */
		if (v != INT_MIN) {
			sum += v;
			n++;
		}
	}
	bosa_set_field(0x24B, 0x0c, 2);			/* MPD mux = 2 (live) */
	bosa_write_reg(0x230, save);			/* restore TX state */
	bosa_set_bit(0x24A, 1, 1);
	if (n)
		bosa_vmpd_dark = sum / n;
	/* ★ NO SAMPLES, NO NUMBER. With n == 0 this printed ...
	 * dev/MEASURED-luna_gpon.c.md sec 283. */
	if (n == 0)
		pr_warn("luna-gpon: BOSA MPD dark cal NOT MEASURED (0/20 samples) -- no value is reported\n");
	else
		pr_info("luna-gpon: BOSA MPD dark cal = %d mV (%d/20 samples)\n",
			bosa_vmpd_dark, n);
}

/* Live TX optical power in centi-dBm. 10-sample mean of the range/bias-classified
 * MPD power code -> per-board 0.1uW word -> log. INT_MIN until the dark cal ran. */
static s32 bosa_tx_power_cdbm(void)
{
	u64 sum = 0;
	u32 word;
	int i, n = 0;

	if (bosa_vmpd_dark == INT_MIN)
		return INT_MIN;
	for (i = 0; i < 10; i++) {
		s32 vmpd;
		int iavg, range;

		bosa_set_field(0x24B, 0x0c, 2);		/* MPD mux = 2 (live/operational node) */
		vmpd = bosa_vmpd_mv(0x3B3);
		if (vmpd == INT_MIN)
			continue;
		iavg  = bosa_read_reg(0x23A) & 0xff;
		range = (bosa_read_reg(0x246) >> 6) & 3;
		/* voltage->code, bias class, range shift: luna_gpon_logic.c */
		sum  += bosa_tx_sample_contrib(vmpd, bosa_vmpd_dark, iavg, range);
		n++;
	}
	if (!n)
		return INT_MIN;
	/* word(0.1uW) = (avg*slope*10)>>8 + (offset*10>>5); slope=2022, offset=0 here. */
	word = bosa_tx_word_calc(sum, n, bosa_cal.tx_slope, bosa_cal.tx_offset);
	return bosa_code_to_cdbm(word);
}

/* One ANI-G self-test reading (the OLT's `show ont-optical`), in the units the
 * OMCI core takes. What this module cannot produce stays out of @v->have. */
static void luna_selftest_read(struct gpon_optic_reading *v)
{
	struct gpon_ddm_a2h d;
	s32 t;
	u32 bias;

	memset(v, 0, sizeof(*v));
	if (bosa_gn_identified || bosa_not_8290b) {
		if (bosa_a2h_read(&d) == GPON_DDM_OK)
			gpon_optic_from_a2h(v, &d);
		return;
	}
	if (!bosa_regs_live())
		return;
	v->rx_cdbm = bosa_rx_power_cdbm();
	if (v->rx_cdbm != BOSA_RX_CDBM_NA)
		v->have |= GPON_OPTIC_RX;
	v->tx_cdbm = bosa_tx_power_cdbm();
	if (v->tx_cdbm != INT_MIN)
		v->have |= GPON_OPTIC_TX;
	t = bosa_temp_dc();
	if (t != INT_MIN) {
		v->temp_256 = (s16)(t * 256 / 10);
		v->have |= GPON_OPTIC_TEMP;
	}
	bias = bosa_bias_ua();		/* 0 = a failed read (bosa_bias_ua_calc) */
	if (bias) {
		v->bias_2ua = bias / 2;
		v->have |= GPON_OPTIC_BIAS;
	}
}

/* Power up the RTL8290B optical receiver so its signal-detect ...
 * dev/MEASURED-luna_gpon.c.md sec 86. */
static const struct { u16 reg; u8 val; } bosa_rx_golden[] __initconst = {
	{ 0x204, 0x8e },	/* W4  booster/SS clock         */
	{ 0x223, 0x08 },	/* W35 RX DAC low               */
	{ 0x224, 0xba },	/* W36 RX DAC high              */
	{ 0x226, 0xd2 },	/* W38 RX mode/swing            */
	{ 0x227, 0xa7 },	/* W39 RX-LOS reference DAC     */
	{ 0x228, 0x63 },	/* W40 RX bias                  */
	{ 0x229, 0x2b },	/* W41 RX power (RXI_PWDN_L=0)  */
	{ 0x22a, 0xe4 },	/* W42 RX gain/impedance        */
	{ 0x22b, 0x00 },	/* W43 RX hysteresis            */
	{ 0x231, 0xac },	/* W49 RX/TX path config        */
	{ 0x254, 0x4d },	/* CONTROL2 (SD/LOS pin ctrl)   */
	{ 0x264, 0x43 },	/* APD bias DAC (operating value) */
	{ 0x269, 0x08 },	/* RX_TH LOS assert threshold   */
	{ 0x26a, 0x10 },	/* RX_DE_TH LOS de-assert       */
};

/* The receiver bring-up as a SEQUENCE, not as a resting ...
 * dev/MEASURED-luna_gpon.c.md sec 87. */
static bool bosa_rx_seq;
module_param(bosa_rx_seq, bool, 0644);
MODULE_PARM_DESC(bosa_rx_seq,
	"1 = bring the BOSA receiver up with stock's ORDERED rtl8290b_rx_init sequence instead of the bosa_rx_golden resting snapshot (default 0 = the snapshot, which is what the X111W is known to reach O5 with)");

static void __init bosa_rx_init_seq(void)
{
	/* analog front end: LDO, output polarity/swing, offset cancel */
	bosa_set_field(BOSA_REG_W41, 0x30, 2);
	bosa_set_bit(0x22a, 3, 0);
	bosa_set_field(0x22a, 0x07, 4);
	bosa_set_field(BOSA_REG_W41, 0xc0, 0);
	bosa_set_bit(BOSA_REG_W41, 3, 0);
	bosa_set_bit(0x27b, 2, 0);
	bosa_set_bit(0x22b, 7, 0);

	/* the LOS detector, held in reset while it is configured */
	bosa_set_bit(0x224, 1, 0);		/* rxlosResetb   */
	bosa_set_bit(0x224, 0, 0);		/* rxlosClkMode  */
	bosa_set_field(0x226, 0xc0, 3);
	bosa_set_field(0x226, 0x07, 0);		/* rxlosHystSel  */
	bosa_set_bit(0x226, 3, 0);		/* rxlosRangSel  */
	bosa_set_bit(0x226, 5, 0);		/* rxlosInputSel */
	bosa_set_field(0x227, 0x7f, 0);		/* rxlosRefDac -- stock ZEROES the
						 * comparator threshold; the
						 * snapshot carries 0x27 */
	bosa_set_field(0x228, 0xc0, 0);		/* rxlosChopperFreq */
	bosa_set_field(0x228, 0x30, 0);		/* rxlosSampleSel   */
	bosa_set_bit(BOSA_REG_W41, 0, 0);		/* rxlosChopperEn   */
	bosa_set_bit(BOSA_REG_W41, 2, 0);		/* rxlosLaMagComp   */
	bosa_set_bit(BOSA_REG_W41, 1, 0);		/* rxlosBufAutozero */
	bosa_set_bit(0x231, 1, 0);		/* rxlosTestMode    */
	bosa_set_bit(0x231, 0, 0);		/* rxlosAssertSel   */
	bosa_set_field(0x257, 0xc0, 0);		/* rxlosDebounceSel */
	bosa_set_bit(0x27b, 0, 0);		/* rxlosDebounceOpt */
	bosa_set_bit(0x27b, 1, 0);
	bosa_set_bit(0x27b, 3, 1);		/* rxlosSquelch     */
	bosa_set_bit(0x25e, 0, 0);		/* rxlosPolarity    */

	/* 0x256 txsdFaultTimer is skipped on purpose -- see the note above */

	/* pin control: SD out on, LOS pin off */
	bosa_set_bit(BOSA_REG_CONTROL2, 7, 1);		/* txSdPinEn  */
	bosa_set_bit(0x3c1, 7, 1);
	bosa_set_bit(0x3c1, 5, 1);
	bosa_set_field(BOSA_REG_CONTROL2, 0x03, 0);		/* txdisCtrl  */
	bosa_set_bit(BOSA_REG_CONTROL2, 6, 0);		/* rxlosPinEn */
	bosa_set_bit(0x281, 0, 1);
	bosa_set_bit(0x257, 0, 0);
	bosa_set_bit(0x27b, 4, 1);

	/* ★ THE TAIL IS WHOLE REGISTERS AND IT SUPERSEDES BITS SET ABOVE.
	 * 0x224 carries rxlosClkMode and rxlosResetb, so writing 0xbc here is
	 * what actually decides them -- a port that replicated only the field
	 * list would leave this register holding something else entirely. */
	bosa_write_reg(0x223, 0x08);
	bosa_write_reg(0x224, 0xbc);
	bosa_write_reg(0x264, 0x43);
}

static void __init bosa_rx_enable(void)
{
	int i, sdet;

	/* Apply the RX operating point to the BOSA: stock's ordered sequence, or
	 * the resting snapshot.  Exactly one of them, so the A/B compares two
	 * bring-ups and not a bring-up layered on another. */
	if (bosa_rx_seq) {
		bosa_rx_init_seq();
	} else {
		for (i = 0; i < ARRAY_SIZE(bosa_rx_golden); i++)
			bosa_write_reg(bosa_rx_golden[i].reg,
				       bosa_rx_golden[i].val);
	}
	mdelay(50);					/* RX amp + SD comparator settle */

	/* Re-read so /proc shows the post-config state. */
	bosa_w41     = bosa_read_reg(BOSA_REG_W41);
	bosa_ctrl2   = bosa_read_reg(BOSA_REG_CONTROL2);
	bosa_status2 = bosa_read_reg(BOSA_REG_STATUS2);
	sdet = !!(sw_rd(SDS_FIB_STATUS) & SDS_FIB_SDS_SDET);
	/* ★ "APPLIED" MUST MEAN APPLIED. On a module with no RTL8290B ...
	 * dev/MEASURED-luna_gpon.c.md sec 88. */
	if (bosa_not_8290b || bosa_id_num != 0x8290) {
		pr_warn("luna-gpon: BOSA RX config NOT APPLIED -- writes refused, no positive RTL8290B id (chip id=0x%04x); the values below are READ-BACKS of a module that is not one\n",
			bosa_id_num);
	}
	pr_info("luna-gpon: BOSA RX config %s: w4=0x%02x w41=0x%02x ctrl2=0x%02x status2=0x%02x apd=0x%02x w39=0x%02x sds_sdet=%d\n",
		(bosa_not_8290b || bosa_id_num != 0x8290) ? "read back (NOT applied)"
							  : "applied",
		bosa_read_reg(BOSA_REG_W4) & 0xff, bosa_w41 & 0xff,
		bosa_ctrl2 & 0xff, bosa_status2 & 0xff,
		bosa_read_reg(0x264) & 0xff, bosa_read_reg(0x227) & 0xff, sdet);
}

/* Upstream-laser (TX) operating point -- the values a ... -- dev/MEASURED-luna_gpon.c.md sec 89. */
static const struct { u16 reg; u8 val; } bosa_tx_golden[] __initconst = {
	{ BOSA_W(46), 0xb0 },	/* TX bias power + APC clocks  */
	{ BOSA_W(54), 0x19 },	/* laser BIAS DAC high          */
	{ BOSA_W(55), 0x67 },	/* laser MOD DAC high           */
	{ BOSA_W(56), 0x22 },	/* BIAS/MOD DAC low bits        */
	{ BOSA_W(57), 0x2d },	/* APCDIG bias DAC power        */
	{ BOSA_W(53), 0xcf },	/* TX/APC fault detection       */
	{ BOSA_W(60), 0x03 },	/* TIA power config             */
	{ BOSA_W(88), 0xf2 },	/* W88 DSR TX APC set-point         */
	{ BOSA_W(80), 0xe9 },	/* W80 TX backup/state             */
	{ BOSA_W(48), 0x0e },	/* TX_ENMODE (enable, last)    */
};

/* BOSA full register image -- the values a registered unit ...
 * dev/MEASURED-luna_gpon.c.md sec 90. */
static const struct { u16 reg; u8 val; } bosa_init_golden[] __initconst = {
	/* page 0 (I2C slave 0x50): SFF-8472 A0 identification-page ...
	 * dev/MEASURED-luna_gpon.c.md sec 91. */
	{BOSA_A0(0),0x02}, {BOSA_A0(1),0x04}, {BOSA_A0(2),0x0b}, {BOSA_A0(3),0xff},
	{BOSA_A0(4),0xff}, {BOSA_A0(5),0xff}, {BOSA_A0(6),0xff}, {BOSA_A0(7),0xff},
	{BOSA_A0(8),0xff}, {BOSA_A0(9),0xff}, {BOSA_A0(10),0xff}, {BOSA_A0(11),0x03},
	{BOSA_A0(12),0x0c}, {BOSA_A0(13),0x00}, {BOSA_A0(14),0x14}, {BOSA_A0(15),0xc8},
	{BOSA_A0(16),0x00}, {BOSA_A0(17),0x00}, {BOSA_A0(18),0x00}, {BOSA_A0(19),0x00},
	/* bytes 20..35, vendor name: "REALTEK         " */
	{BOSA_A0(20),'R'}, {BOSA_A0(21),'E'}, {BOSA_A0(22),'A'}, {BOSA_A0(23),'L'},
	{BOSA_A0(24),'T'}, {BOSA_A0(25),'E'}, {BOSA_A0(26),'K'}, {BOSA_A0(27),' '},
	{BOSA_A0(28),' '}, {BOSA_A0(29),' '}, {BOSA_A0(30),' '}, {BOSA_A0(31),' '},
	{BOSA_A0(32),' '}, {BOSA_A0(33),' '}, {BOSA_A0(34),' '}, {BOSA_A0(35),' '},
	/* bytes 36..39: reserved + vendor OUI, zero */
	{BOSA_A0(36),0x00}, {BOSA_A0(37),0x00}, {BOSA_A0(38),0x00}, {BOSA_A0(39),0x00},
	/* bytes 40..55, vendor part number: "RTL8290         " */
	{BOSA_A0(40),'R'}, {BOSA_A0(41),'T'}, {BOSA_A0(42),'L'}, {BOSA_A0(43),'8'},
	{BOSA_A0(44),'2'}, {BOSA_A0(45),'9'}, {BOSA_A0(46),'0'}, {BOSA_A0(47),' '},
	{BOSA_A0(48),' '}, {BOSA_A0(49),' '}, {BOSA_A0(50),' '}, {BOSA_A0(51),' '},
	{BOSA_A0(52),' '}, {BOSA_A0(53),' '}, {BOSA_A0(54),' '}, {BOSA_A0(55),' '},
	/* bytes 56..59, vendor rev: "0001" */
	{BOSA_A0(56),'0'}, {BOSA_A0(57),'0'}, {BOSA_A0(58),'0'}, {BOSA_A0(59),'1'},
	/* bytes 60..67: wavelength, CC_BASE position (0xff, checksum not
	 * maintained), options */
	{BOSA_A0(60),0x05},	/* wavelength 0x051e = 1310 nm (hi byte; lo is next) */
	{BOSA_A0(61),0x1e}, {BOSA_A0(62),0x00}, {BOSA_A0(63),0xff}, {BOSA_A0(64),0x00},
	{BOSA_A0(65),0x20}, {BOSA_A0(66),0x00}, {BOSA_A0(67),0x00},
	/* bytes 68..83, vendor serial-number field: holds mangled placeholder
	 * text, kept byte-exact: vendorpxrtn'mber */
	{BOSA_A0(68),'v'}, {BOSA_A0(69),'e'}, {BOSA_A0(70),'n'}, {BOSA_A0(71),'d'},
	{BOSA_A0(72),'o'}, {BOSA_A0(73),'r'}, {BOSA_A0(74),'p'}, {BOSA_A0(75),'x'},
	{BOSA_A0(76),'r'}, {BOSA_A0(77),'t'}, {BOSA_A0(78),'n'}, {BOSA_A0(79),'"'},
	{BOSA_A0(80),'m'}, {BOSA_A0(81),'b'}, {BOSA_A0(82),'e'}, {BOSA_A0(83),'r'},
	/* bytes 84..91, date code: "20140123" (2014-01-23) */
	{BOSA_A0(84),'2'}, {BOSA_A0(85),'0'}, {BOSA_A0(86),'1'}, {BOSA_A0(87),'4'},
	{BOSA_A0(88),'0'}, {BOSA_A0(89),'1'}, {BOSA_A0(90),'2'}, {BOSA_A0(91),'3'},
	/* bytes 92..94: DDM type=0x68, enhanced options=0x80, SFF-8472
	 * compliance=0x02 */
	{BOSA_A0(92),0x68}, {BOSA_A0(93),0x80}, {BOSA_A0(94),0x02},
	/* bytes 95..255: 0xff filler (unprogrammed EEPROM area, incl. the byte-95
	 * checksum position); the lone non-0xff bytes are broken out, as captured */
	{BOSA_A0(95),0xff}, {BOSA_A0(96),0xff}, {BOSA_A0(97),0xff}, {BOSA_A0(98),0xff},
	{BOSA_A0(99),0xff}, {BOSA_A0(100),0xff}, {BOSA_A0(101),0xff}, {BOSA_A0(102),0xff},
	{BOSA_A0(103),0xff}, {BOSA_A0(104),0xff}, {BOSA_A0(105),0xff}, {BOSA_A0(106),0xff},
	{BOSA_A0(107),0xff}, {BOSA_A0(108),0xff}, {BOSA_A0(109),0xff}, {BOSA_A0(110),0xff},
	{BOSA_A0(111),0xff}, {BOSA_A0(112),0xff}, {BOSA_A0(113),0xff}, {BOSA_A0(114),0xff},
	{BOSA_A0(115),0xff}, {BOSA_A0(116),0xff}, {BOSA_A0(117),0xff}, {BOSA_A0(118),0xff},
	{BOSA_A0(119),0xff}, {BOSA_A0(120),0xff}, {BOSA_A0(121),0xff}, {BOSA_A0(122),0xff},
	{BOSA_A0(123),0xff}, {BOSA_A0(124),0xff}, {BOSA_A0(125),0xff}, {BOSA_A0(126),0xff},
	{BOSA_A0(127),0xff}, {BOSA_A0(128),0xff},
	{BOSA_A0(129),0x10},	/* lone non-0xff byte, as captured */
	{BOSA_A0(130),0xff}, {BOSA_A0(131),0xff}, {BOSA_A0(132),0xff}, {BOSA_A0(133),0xff},
	{BOSA_A0(134),0xff},
	{BOSA_A0(135),0xfd},	/* lone non-0xff byte, as captured */
	{BOSA_A0(136),0xff}, {BOSA_A0(137),0xff},
	{BOSA_A0(138),0x54},	/* lone non-0xff byte, as captured */
	{BOSA_A0(139),0xff}, {BOSA_A0(140),0xff}, {BOSA_A0(141),0xff}, {BOSA_A0(142),0xff},
	{BOSA_A0(143),0xff}, {BOSA_A0(144),0xff}, {BOSA_A0(145),0xff}, {BOSA_A0(146),0xff},
	{BOSA_A0(147),0xff}, {BOSA_A0(148),0xff}, {BOSA_A0(149),0xff}, {BOSA_A0(150),0xff},
	{BOSA_A0(151),0xff}, {BOSA_A0(152),0xff}, {BOSA_A0(153),0xff}, {BOSA_A0(154),0xff},
	{BOSA_A0(155),0xff}, {BOSA_A0(156),0xff}, {BOSA_A0(157),0xff}, {BOSA_A0(158),0xff},
	{BOSA_A0(159),0xff}, {BOSA_A0(160),0xff}, {BOSA_A0(161),0xff}, {BOSA_A0(162),0xff},
	{BOSA_A0(163),0xff}, {BOSA_A0(164),0xff}, {BOSA_A0(165),0xff}, {BOSA_A0(166),0xff},
	{BOSA_A0(167),0xff}, {BOSA_A0(168),0xff}, {BOSA_A0(169),0xff}, {BOSA_A0(170),0xff},
	{BOSA_A0(171),0xff}, {BOSA_A0(172),0xff}, {BOSA_A0(173),0xff}, {BOSA_A0(174),0xff},
	{BOSA_A0(175),0xfd},	/* lone non-0xff byte, as captured */
	{BOSA_A0(176),0xff}, {BOSA_A0(177),0xff},
	{BOSA_A0(178),0x78},	/* lone non-0xff byte, as captured */
	{BOSA_A0(179),0xff}, {BOSA_A0(180),0xff},
	{BOSA_A0(181),0x4f},	/* lone non-0xff byte, as captured */
	{BOSA_A0(182),0xff}, {BOSA_A0(183),0xff}, {BOSA_A0(184),0xff}, {BOSA_A0(185),0xff},
	{BOSA_A0(186),0xff}, {BOSA_A0(187),0xff}, {BOSA_A0(188),0xff}, {BOSA_A0(189),0xff},
	{BOSA_A0(190),0xff}, {BOSA_A0(191),0xff}, {BOSA_A0(192),0xff}, {BOSA_A0(193),0xff},
	{BOSA_A0(194),0xff}, {BOSA_A0(195),0xff}, {BOSA_A0(196),0xff}, {BOSA_A0(197),0xff},
	{BOSA_A0(198),0xff}, {BOSA_A0(199),0xff}, {BOSA_A0(200),0xff}, {BOSA_A0(201),0xff},
	{BOSA_A0(202),0xff}, {BOSA_A0(203),0xff}, {BOSA_A0(204),0xff}, {BOSA_A0(205),0xff},
	{BOSA_A0(206),0xff}, {BOSA_A0(207),0xff}, {BOSA_A0(208),0xff}, {BOSA_A0(209),0xff},
	{BOSA_A0(210),0xff}, {BOSA_A0(211),0xff},
	{BOSA_A0(212),0x0c},	/* lone non-0xff byte, as captured */
	{BOSA_A0(213),0xff}, {BOSA_A0(214),0xff}, {BOSA_A0(215),0xff}, {BOSA_A0(216),0xff},
	{BOSA_A0(217),0xff}, {BOSA_A0(218),0xff}, {BOSA_A0(219),0xff}, {BOSA_A0(220),0xff},
	{BOSA_A0(221),0xff}, {BOSA_A0(222),0xff}, {BOSA_A0(223),0xff}, {BOSA_A0(224),0xff},
	{BOSA_A0(225),0xff}, {BOSA_A0(226),0xff}, {BOSA_A0(227),0xff}, {BOSA_A0(228),0xff},
	{BOSA_A0(229),0xff}, {BOSA_A0(230),0xff}, {BOSA_A0(231),0xff}, {BOSA_A0(232),0xff},
	{BOSA_A0(233),0xff}, {BOSA_A0(234),0xff}, {BOSA_A0(235),0xff}, {BOSA_A0(236),0xff},
	{BOSA_A0(237),0xff}, {BOSA_A0(238),0xff}, {BOSA_A0(239),0xff}, {BOSA_A0(240),0xff},
	{BOSA_A0(241),0xff}, {BOSA_A0(242),0xff}, {BOSA_A0(243),0xff},
	{BOSA_A0(244),0x6d},	/* lone non-0xff byte, as captured */
	{BOSA_A0(245),0xff},
	{BOSA_A0(246),0xfc},	/* lone non-0xff byte, as captured */
	{BOSA_A0(247),0xff}, {BOSA_A0(248),0xff}, {BOSA_A0(249),0xff},
	{BOSA_A0(250),0x70},	/* lone non-0xff byte, as captured */
	{BOSA_A0(251),0xff}, {BOSA_A0(252),0xff}, {BOSA_A0(253),0xff}, {BOSA_A0(254),0xff},
	{BOSA_A0(255),0xff},
	/* page 1 (I2C slave 0x51): SFF-8472 A2 diagnostics-page image
	 * dev/MEASURED-luna_gpon.c.md sec 92. */
	{BOSA_A2(0),0x7f}, {BOSA_A2(1),0xff}, {BOSA_A2(2),0xff}, {BOSA_A2(3),0xff},
	{BOSA_A2(4),0x7f}, {BOSA_A2(5),0xff}, {BOSA_A2(6),0xff}, {BOSA_A2(7),0xff},
	{BOSA_A2(8),0x8e}, {BOSA_A2(9),0x94}, {BOSA_A2(10),0x6d}, {BOSA_A2(11),0x60},
	{BOSA_A2(12),0x8c}, {BOSA_A2(13),0xa0}, {BOSA_A2(14),0x75}, {BOSA_A2(15),0x30},
	{BOSA_A2(16),0x75}, {BOSA_A2(17),0x30}, {BOSA_A2(18),0x05}, {BOSA_A2(19),0xdc},
	{BOSA_A2(20),0x61}, {BOSA_A2(21),0xa8}, {BOSA_A2(22),0x07}, {BOSA_A2(23),0xd0},
	{BOSA_A2(24),0x00}, {BOSA_A2(25),0x00}, {BOSA_A2(26),0x0f}, {BOSA_A2(27),0x8d},
	{BOSA_A2(28),0x00}, {BOSA_A2(29),0x0a}, {BOSA_A2(30),0x0c}, {BOSA_A2(31),0x5a},
	{BOSA_A2(32),0x00}, {BOSA_A2(33),0x0c}, {BOSA_A2(34),0x00}, {BOSA_A2(35),0x00},
	{BOSA_A2(36),0x00}, {BOSA_A2(37),0x00}, {BOSA_A2(38),0x00}, {BOSA_A2(39),0x00},
	/* A2 bytes 40..55: zero */
	{BOSA_A2(40),0x00}, {BOSA_A2(41),0x00}, {BOSA_A2(42),0x00}, {BOSA_A2(43),0x00},
	{BOSA_A2(44),0x00}, {BOSA_A2(45),0x00}, {BOSA_A2(46),0x00}, {BOSA_A2(47),0x00},
	{BOSA_A2(48),0x00}, {BOSA_A2(49),0x00}, {BOSA_A2(50),0x00}, {BOSA_A2(51),0x00},
	{BOSA_A2(52),0x00}, {BOSA_A2(53),0x00}, {BOSA_A2(54),0x00}, {BOSA_A2(55),0x00},
	/* A2 bytes 56..91, calibration-constant region: mostly zero; the
	 * 0x01,0x00 pairs are unity slopes, plus one 0x3f80 */
	{BOSA_A2(56),0x00}, {BOSA_A2(57),0x00}, {BOSA_A2(58),0x00}, {BOSA_A2(59),0x00},
	{BOSA_A2(60),0x00}, {BOSA_A2(61),0x00}, {BOSA_A2(62),0x3f}, {BOSA_A2(63),0x80},
	{BOSA_A2(64),0x00}, {BOSA_A2(65),0x00}, {BOSA_A2(66),0x00}, {BOSA_A2(67),0x00},
	{BOSA_A2(68),0x00}, {BOSA_A2(69),0x00}, {BOSA_A2(70),0x01}, {BOSA_A2(71),0x00},
	{BOSA_A2(72),0x00}, {BOSA_A2(73),0x00}, {BOSA_A2(74),0x01}, {BOSA_A2(75),0x00},
	{BOSA_A2(76),0x00}, {BOSA_A2(77),0x00}, {BOSA_A2(78),0x01}, {BOSA_A2(79),0x00},
	{BOSA_A2(80),0x00}, {BOSA_A2(81),0x00}, {BOSA_A2(82),0x01}, {BOSA_A2(83),0x00},
	{BOSA_A2(84),0x00}, {BOSA_A2(85),0x00}, {BOSA_A2(86),0x00}, {BOSA_A2(87),0x00},
	{BOSA_A2(88),0x00}, {BOSA_A2(89),0xff}, {BOSA_A2(90),0xff}, {BOSA_A2(91),0xff},
	/* A2 bytes 92..95: 0xff (incl. the byte-95 CC_DMI checksum position, not
	 * maintained) */
	{BOSA_A2(92),0xff}, {BOSA_A2(93),0xff}, {BOSA_A2(94),0xff}, {BOSA_A2(95),0xff},
	/* A2 bytes 96..119, live-diagnostics area: a captured snapshot (the MCU
	 * overwrites these with real readings) */
	{BOSA_A2(96),0x2c}, {BOSA_A2(97),0x38}, {BOSA_A2(98),0x84}, {BOSA_A2(99),0x98},
	{BOSA_A2(100),0x18}, {BOSA_A2(101),0x14},
	{BOSA_A2(102),0x3e},	/* TX optical power word hi (bosa_read16 reads it live) */
	{BOSA_A2(103),0x52},
	{BOSA_A2(104),0x00},	/* RX optical power word hi (bosa_read16) */
	{BOSA_A2(105),0xe5}, {BOSA_A2(106),0xff}, {BOSA_A2(107),0x00}, {BOSA_A2(108),0x00},
	{BOSA_A2(109),0x00}, {BOSA_A2(110),0xff}, {BOSA_A2(111),0x00}, {BOSA_A2(112),0x03},
	{BOSA_A2(113),0xc0}, {BOSA_A2(114),0x00}, {BOSA_A2(115),0x00}, {BOSA_A2(116),0x03},
	{BOSA_A2(117),0xc0}, {BOSA_A2(118),0x00}, {BOSA_A2(119),0x00},
	/* A2 bytes 120..127: zero */
	{BOSA_A2(120),0x00}, {BOSA_A2(121),0x00}, {BOSA_A2(122),0x00}, {BOSA_A2(123),0x00},
	{BOSA_A2(124),0x00}, {BOSA_A2(125),0x00}, {BOSA_A2(126),0x00}, {BOSA_A2(127),0x00},
	/* A2 bytes 128..255, page upper half (user/vendor area): opaque content,
	 * captured as-is; nothing in this driver reads it */
	{BOSA_A2(128),0x73}, {BOSA_A2(129),0x11}, {BOSA_A2(130),0x7d}, {BOSA_A2(131),0x39},
	{BOSA_A2(132),0x6d}, {BOSA_A2(133),0xdb}, {BOSA_A2(134),0xf9}, {BOSA_A2(135),0x54},
	{BOSA_A2(136),0xc3}, {BOSA_A2(137),0xc0}, {BOSA_A2(138),0x59}, {BOSA_A2(139),0x1a},
	{BOSA_A2(140),0x4b}, {BOSA_A2(141),0xe3}, {BOSA_A2(142),0xfb}, {BOSA_A2(143),0x94},
	{BOSA_A2(144),0x0c}, {BOSA_A2(145),0xa4}, {BOSA_A2(146),0x16}, {BOSA_A2(147),0xca},
	{BOSA_A2(148),0xf0}, {BOSA_A2(149),0x51}, {BOSA_A2(150),0xc1}, {BOSA_A2(151),0xe4},
	{BOSA_A2(152),0x08}, {BOSA_A2(153),0x09}, {BOSA_A2(154),0x6d}, {BOSA_A2(155),0x43},
	{BOSA_A2(156),0xe0}, {BOSA_A2(157),0x63}, {BOSA_A2(158),0x65}, {BOSA_A2(159),0x1d},
	{BOSA_A2(160),0x89}, {BOSA_A2(161),0x53}, {BOSA_A2(162),0x54}, {BOSA_A2(163),0x23},
	{BOSA_A2(164),0x7b}, {BOSA_A2(165),0xe5}, {BOSA_A2(166),0xda}, {BOSA_A2(167),0x2e},
	{BOSA_A2(168),0x7c}, {BOSA_A2(169),0xf5}, {BOSA_A2(170),0x7c}, {BOSA_A2(171),0xe7},
	{BOSA_A2(172),0xce}, {BOSA_A2(173),0x2b}, {BOSA_A2(174),0xd1}, {BOSA_A2(175),0x76},
	{BOSA_A2(176),0xf8}, {BOSA_A2(177),0xdc}, {BOSA_A2(178),0x72}, {BOSA_A2(179),0x92},
	{BOSA_A2(180),0x94}, {BOSA_A2(181),0x34}, {BOSA_A2(182),0x69}, {BOSA_A2(183),0x48},
	{BOSA_A2(184),0x85}, {BOSA_A2(185),0xff}, {BOSA_A2(186),0x30}, {BOSA_A2(187),0x0c},
	{BOSA_A2(188),0x23}, {BOSA_A2(189),0xdd}, {BOSA_A2(190),0x3c}, {BOSA_A2(191),0xdc},
	{BOSA_A2(192),0x53}, {BOSA_A2(193),0xd3}, {BOSA_A2(194),0x5d}, {BOSA_A2(195),0x5c},
	{BOSA_A2(196),0xc4}, {BOSA_A2(197),0xef}, {BOSA_A2(198),0xdb}, {BOSA_A2(199),0xe5},
	{BOSA_A2(200),0xc8}, {BOSA_A2(201),0xff}, {BOSA_A2(202),0xdb}, {BOSA_A2(203),0x52},
	{BOSA_A2(204),0x22}, {BOSA_A2(205),0x27}, {BOSA_A2(206),0xfd}, {BOSA_A2(207),0x37},
	{BOSA_A2(208),0x3b}, {BOSA_A2(209),0x33}, {BOSA_A2(210),0xc3}, {BOSA_A2(211),0x91},
	{BOSA_A2(212),0xa2}, {BOSA_A2(213),0x01}, {BOSA_A2(214),0xbd}, {BOSA_A2(215),0x7c},
	{BOSA_A2(216),0x4e}, {BOSA_A2(217),0xe6}, {BOSA_A2(218),0x0d}, {BOSA_A2(219),0x2d},
	{BOSA_A2(220),0x2e}, {BOSA_A2(221),0x94}, {BOSA_A2(222),0xa0}, {BOSA_A2(223),0xeb},
	{BOSA_A2(224),0xd9}, {BOSA_A2(225),0x31}, {BOSA_A2(226),0x39}, {BOSA_A2(227),0x91},
	{BOSA_A2(228),0x68}, {BOSA_A2(229),0xf6}, {BOSA_A2(230),0x7b}, {BOSA_A2(231),0x4b},
	{BOSA_A2(232),0x67}, {BOSA_A2(233),0xf4}, {BOSA_A2(234),0xc7}, {BOSA_A2(235),0x36},
	{BOSA_A2(236),0xfd}, {BOSA_A2(237),0x4d}, {BOSA_A2(238),0x86}, {BOSA_A2(239),0x76},
	{BOSA_A2(240),0x36}, {BOSA_A2(241),0x2c}, {BOSA_A2(242),0xf3}, {BOSA_A2(243),0x2e},
	{BOSA_A2(244),0x3c}, {BOSA_A2(245),0xbd}, {BOSA_A2(246),0xb5}, {BOSA_A2(247),0x01},
	{BOSA_A2(248),0x37}, {BOSA_A2(249),0x11}, {BOSA_A2(250),0x04}, {BOSA_A2(251),0x7b},
	{BOSA_A2(252),0x9d}, {BOSA_A2(253),0x01}, {BOSA_A2(254),0x99}, {BOSA_A2(255),0x3a},
	/* page 2 (I2C slave 0x54): analog/APC "W" registers ---- Wn = ...
	 * dev/MEASURED-luna_gpon.c.md sec 93. */
	{BOSA_W(0),0x02}, {BOSA_W(1),0x89}, {BOSA_W(2),0xa1}, {BOSA_W(3),0xfe},
	{BOSA_REG_W4,0x8e},	/* EN_L booster */
	{BOSA_W(5),0xb2}, {BOSA_W(6),0x9b}, {BOSA_W(7),0x90}, {BOSA_W(8),0x00},
	{BOSA_W(9),0x49}, {BOSA_W(10),0x9f}, {BOSA_W(11),0xff}, {BOSA_W(12),0x23},
	{BOSA_W(13),0x04}, {BOSA_W(14),0x78}, {BOSA_W(15),0x7f}, {BOSA_W(16),0xff},
	{BOSA_W(17),0x00},
	{BOSA_W(18),0x82},	/* DDM mux: channel/gain select (bosa_read_dd) */
	{BOSA_W(19),0x05}, {BOSA_W(20),0x00}, {BOSA_W(21),0x00}, {BOSA_W(22),0x01},
	{BOSA_W(23),0xf6}, {BOSA_W(24),0xce}, {BOSA_W(25),0x90}, {BOSA_W(26),0xc0},
	{BOSA_W(27),0x00}, {BOSA_W(28),0x00}, {BOSA_W(29),0x38}, {BOSA_W(30),0x24},
	{BOSA_W(31),0x40}, {BOSA_W(32),0x40}, {BOSA_W(33),0x00}, {BOSA_W(34),0x01},
	{BOSA_W(35),0x08},	/* RX config (RX bring-up also writes 0x08) */
	{BOSA_W(36),0xba},	/* RX config (RX bring-up writes 0xbc; image holds 0xba) */
	{BOSA_W(37),0x1e}, {BOSA_W(38),0xd2},
	{BOSA_W(39),0xa7},	/* "w39" of the RX status log */
	{BOSA_W(40),0x63},
	{BOSA_REG_W41,0x2b},	/* RXI_PWDN_L */
	{BOSA_W(42),0xe4}, {BOSA_W(43),0x00}, {BOSA_W(44),0xe0}, {BOSA_W(45),0x01},
	{BOSA_W(46),0xb0},	/* TX bias power + APC clocks (= bosa_tx_golden) */
	{BOSA_W(47),0x44},
	{BOSA_W(48),0x0e},	/* TX_ENMODE (= bosa_tx_golden) */
	{BOSA_W(49),0xac}, {BOSA_W(50),0x01}, {BOSA_W(51),0x08}, {BOSA_W(52),0x80},
	{BOSA_W(53),0xcf},	/* TX/APC fault detection (= bosa_tx_golden) */
	{BOSA_W(54),0x19},	/* laser BIAS DAC high (= bosa_tx_golden) */
	{BOSA_W(55),0x67},	/* laser MOD DAC high (= bosa_tx_golden) */
	{BOSA_W(56),0x22},	/* BIAS/MOD DAC low bits (= bosa_tx_golden) */
	{BOSA_W(57),0x2d},	/* APCDIG bias DAC power (= bosa_tx_golden) */
	{BOSA_W(58),0x62},	/* DCL P0 -- bosa_apc_calibrate overwrites (0x26) */
	{BOSA_W(59),0xcf},	/* DCL P1 -- bosa_apc_calibrate overwrites (0x50) */
	{BOSA_W(60),0x03},	/* TIA power config (= bosa_tx_golden) */
	{BOSA_W(61),0xa2},	/* DCL Pavg -- bosa_apc_calibrate overwrites (0x50) */
	{BOSA_W(62),0xfc}, {BOSA_W(63),0xfd}, {BOSA_W(64),0x02}, {BOSA_W(65),0x57},
	{BOSA_W(66),0xd0},
	{BOSA_W(67),0x80},	/* bit7 = APC hardware-servo enable */
	{BOSA_W(68),0x00}, {BOSA_W(69),0x00},
	{BOSA_W(70),0x3f},	/* bias-range bits [7:6] (read by the DDM path) */
	{BOSA_W(71),0xcc}, {BOSA_W(72),0x4d}, {BOSA_W(73),0x2a}, {BOSA_W(74),0x22},
	{BOSA_W(75),0x89}, {BOSA_W(76),0x85},
	{BOSA_REG_W77,BOSA_W77_BIAS_MAX_EN | BOSA_W77_MOD_MAX_EN | BOSA_W77_MOD_MAX_LOADIN},
	{BOSA_W(78),0x80}, {BOSA_W(79),0x3f}, {BOSA_REG_MAGIC_CODE,0x00}, {BOSA_REG_SW_VERSION,0x00},
	{BOSA_PAGE2(0x52),0x00}, {BOSA_REG_CONTROL1,0x00},
	{BOSA_REG_CONTROL2,0x4d},	/* TX_POW_CTL/ENLD_L/LOS_PIN_TRI */
	{BOSA_REG_CONTROL3,0x30},	/* CONTROL3: bit1 = fault-release strobe (re-arm path) */
	{BOSA_PAGE2(0x56),0x00}, {BOSA_PAGE2(0x57),0xf4}, {BOSA_REG_SW_INT_CODE,0x00}, {BOSA_REG_INT_MASK1,0xfe},
	{BOSA_PAGE2(0x5a),0xff}, {BOSA_PAGE2(0x5b),0x01}, {BOSA_PAGE2(0x5c),0x00}, {BOSA_PAGE2(0x5d),0xff},
	{BOSA_PAGE2(0x5e),0x00}, {BOSA_PAGE2(0x5f),0x02}, {BOSA_REG_FAULT_INHIBIT,0x00}, {BOSA_REG_FAULT_INHIBIT2,0x03},
	{BOSA_REG_SW_FAULT_INHIBIT1,0xff}, {BOSA_REG_SW_FAULT_INHIBIT2,0x07},
	{BOSA_REG_DAC_HB,0x43},	/* APD control (RX log "apd"; RX bring-up writes it too) */
	{BOSA_REG_DAC_LB,0x00}, {BOSA_REG_EXT_RSSI_LOW_TH,0xa0}, {BOSA_REG_EXT_RSSI_HIGH_TH,0xc0}, {BOSA_PAGE2(0x68),0x00},
	{BOSA_REG_RX_TH,0x08}, {BOSA_PAGE2(0x6a),0x10}, {BOSA_PAGE2(0x6b),0xe0}, {BOSA_PAGE2(0x6c),0xe0},
	{BOSA_PAGE2(0x6d),0xe0}, {BOSA_PAGE2(0x6e),0xff}, {BOSA_PAGE2(0x6f),0xf4}, {BOSA_PAGE2(0x70),0x84},
	{BOSA_PAGE2(0x71),0x82}, {BOSA_PAGE2(0x72),0x50}, {BOSA_REG_READY_DELAY,0x00}, {BOSA_REG_DDMI_W_LCT,0xff},
	{BOSA_REG_DDMI_W_HCT,0x00}, {BOSA_REG_DEBUG_CTL,0x10}, {BOSA_REG_DEBUG_SL1,0x00}, {BOSA_REG_DEBUG_SL2,0x00},
	{BOSA_REG_IMPD_TH_ORI,0xff}, {BOSA_PAGE2(0x7a),0x00}, {BOSA_PAGE2(0x7b),0x08},
	{BOSA_W(80),0xe9},	/* W80 TX backup/state (= bosa_tx_golden) */
	{BOSA_W(81),0x00}, {BOSA_W(82),0x00}, {BOSA_W(83),0x00}, {BOSA_W(84),0x00},
	{BOSA_W(85),0x01}, {BOSA_W(86),0x00}, {BOSA_W(87),0x88},
	{BOSA_W(88),0xf2},	/* W88 DSR TX APC set-point (= bosa_tx_golden) */
	{BOSA_PAGE2(0x85),0x00}, {BOSA_PAGE2(0x86),0x00}, {BOSA_PAGE2(0x87),0x08}, {BOSA_PAGE2(0x88),0x00},
	{BOSA_PAGE2(0x89),0x00}, {BOSA_PAGE2(0x8a),0x00}, {BOSA_PAGE2(0x8b),0x00}, {BOSA_PAGE2(0x8c),0x00},
	{BOSA_PAGE2(0x8d),0x00}, {BOSA_PAGE2(0x8e),0x00}, {BOSA_PAGE2(0x8f),0x00}, {BOSA_PAGE2(0x90),0x00},
	{BOSA_PAGE2(0x91),0x00}, {BOSA_PAGE2(0x92),0x08}, {BOSA_PAGE2(0x93),0x00}, {BOSA_PAGE2(0x94),0x00},
	{BOSA_PAGE2(0x95),0x00}, {BOSA_PAGE2(0x96),0x00}, {BOSA_PAGE2(0x97),0x00}, {BOSA_PAGE2(0x98),0x00},
	{BOSA_PAGE2(0x99),0x00}, {BOSA_PAGE2(0x9a),0x00}, {BOSA_PAGE2(0x9b),0x00}, {BOSA_PAGE2(0x9c),0x00},
	{BOSA_PAGE2(0x9d),0x00}, {BOSA_PAGE2(0x9e),0x00}, {BOSA_PAGE2(0x9f),0x00}, {BOSA_PAGE2(0xa0),0x00},
	{BOSA_PAGE2(0xa1),0x00}, {BOSA_PAGE2(0xa2),0x00}, {BOSA_PAGE2(0xa3),0x00}, {BOSA_PAGE2(0xa4),0x00},
	{BOSA_PAGE2(0xa5),0x00}, {BOSA_PAGE2(0xa6),0x00}, {BOSA_PAGE2(0xa7),0x08}, {BOSA_PAGE2(0xa8),0x00},
	{BOSA_PAGE2(0xa9),0x00}, {BOSA_PAGE2(0xaa),0x00}, {BOSA_PAGE2(0xab),0x00}, {BOSA_PAGE2(0xac),0x00},
	{BOSA_PAGE2(0xad),0x00}, {BOSA_PAGE2(0xae),0x00}, {BOSA_PAGE2(0xaf),0x08}, {BOSA_PAGE2(0xb0),0x00},
	{BOSA_PAGE2(0xb1),0x00}, {BOSA_PAGE2(0xb2),0x00}, {BOSA_PAGE2(0xb3),0x00}, {BOSA_PAGE2(0xb4),0x00},
	{BOSA_PAGE2(0xb5),0x00}, {BOSA_PAGE2(0xb6),0x00}, {BOSA_PAGE2(0xb7),0xca}, {BOSA_PAGE2(0xb8),0x00},
	{BOSA_PAGE2(0xb9),0x00}, {BOSA_PAGE2(0xba),0x00}, {BOSA_PAGE2(0xbb),0x00}, {BOSA_PAGE2(0xbc),0x00},
	{BOSA_PAGE2(0xbd),0x00}, {BOSA_PAGE2(0xbe),0x00}, {BOSA_PAGE2(0xbf),0x00}, {BOSA_PAGE2(0xc0),0x00},
	{BOSA_PAGE2(0xc1),0xfc}, {BOSA_PAGE2(0xc2),0x00}, {BOSA_PAGE2(0xc3),0x00}, {BOSA_PAGE2(0xc4),0x00},
	{BOSA_PAGE2(0xc5),0x02}, {BOSA_PAGE2(0xc6),0x00}, {BOSA_PAGE2(0xc7),0xe3}, {BOSA_PAGE2(0xc8),0x00},
	{BOSA_PAGE2(0xc9),0x00}, {BOSA_PAGE2(0xca),0x00}, {BOSA_PAGE2(0xcb),0x00}, {BOSA_PAGE2(0xcc),0x00},
	{BOSA_PAGE2(0xcd),0x00}, {BOSA_PAGE2(0xce),0x00}, {BOSA_PAGE2(0xcf),0x00}, {BOSA_PAGE2(0xd0),0x00},
	{BOSA_PAGE2(0xd1),0x00}, {BOSA_PAGE2(0xd2),0x00}, {BOSA_PAGE2(0xd3),0x00}, {BOSA_PAGE2(0xd4),0x00},
	{BOSA_PAGE2(0xd5),0x00}, {BOSA_PAGE2(0xd6),0x00}, {BOSA_PAGE2(0xd7),0x00}, {BOSA_PAGE2(0xd8),0x00},
	{BOSA_PAGE2(0xd9),0x00}, {BOSA_PAGE2(0xda),0x00}, {BOSA_PAGE2(0xdb),0x00}, {BOSA_PAGE2(0xdc),0x00},
	{BOSA_PAGE2(0xdd),0x00}, {BOSA_PAGE2(0xde),0x00}, {BOSA_PAGE2(0xdf),0x00}, {BOSA_PAGE2(0xe0),0x00},
	{BOSA_PAGE2(0xe1),0x00}, {BOSA_PAGE2(0xe2),0x00}, {BOSA_PAGE2(0xe3),0x00}, {BOSA_PAGE2(0xe4),0x00},
	{BOSA_PAGE2(0xe5),0x00}, {BOSA_PAGE2(0xe6),0x00}, {BOSA_PAGE2(0xe7),0x00}, {BOSA_PAGE2(0xe8),0x00},
	{BOSA_PAGE2(0xe9),0x00}, {BOSA_PAGE2(0xea),0x00}, {BOSA_PAGE2(0xeb),0x08}, {BOSA_PAGE2(0xec),0x00},
	{BOSA_PAGE2(0xed),0x00}, {BOSA_PAGE2(0xee),0x00}, {BOSA_PAGE2(0xef),0x00}, {BOSA_PAGE2(0xf0),0x00},
	{BOSA_PAGE2(0xf1),0x00}, {BOSA_PAGE2(0xf2),0x00}, {BOSA_PAGE2(0xf3),0x08}, {BOSA_PAGE2(0xf4),0x00},
	{BOSA_PAGE2(0xf5),0x00}, {BOSA_PAGE2(0xf6),0x00}, {BOSA_PAGE2(0xf7),0x00}, {BOSA_PAGE2(0xf8),0x00},
	{BOSA_PAGE2(0xf9),0x00}, {BOSA_PAGE2(0xfa),0x00}, {BOSA_PAGE2(0xfb),0x00}, {BOSA_PAGE2(0xfc),0x00},
	{BOSA_PAGE2(0xfd),0x00}, {BOSA_PAGE2(0xfe),0x08}, {BOSA_PAGE2(0xff),0x00},
	/* ---- page 3 (I2C slave 0x55): MCU control/status page ---- */
	{BOSA_R(0),0xd6}, {BOSA_R(1),0xca},
	{BOSA_R(2),0xa9},	/* ADC readout hi (polled by bosa_read_dd) */
	{BOSA_R(3),0x08},	/* ADC readout lo */
	{BOSA_R(4),0xc4}, {BOSA_R(5),0xe4}, {BOSA_R(6),0x78}, {BOSA_R(7),0x70},
	{BOSA_R(8),0xe5}, {BOSA_R(9),0xcd}, {BOSA_R(10),0xf8},
	/* 0x30b..0x325 not written: live MCU/APC-servo state sits here (R29
	 * 0x31d OFFK latch, R30 0x31e, ADC capture 0x320..0x322) -- left to the
	 * MCU */
	{BOSA_R(38),0x00}, {BOSA_R(39),0x00}, {BOSA_R(40),0x00}, {BOSA_R(41),0x08},
	{BOSA_R(42),0x00}, {BOSA_R(43),0x00}, {BOSA_R(44),0x00}, {BOSA_R(45),0x00},
	{BOSA_R(46),0x00}, {BOSA_R(47),0x00}, {BOSA_R(48),0x00}, {BOSA_R(49),0x00},
	{BOSA_R(50),0x00}, {BOSA_R(51),0x00}, {BOSA_R(52),0x00}, {BOSA_R(53),0x00},
	{BOSA_R(54),0x00}, {BOSA_R(55),0x00}, {BOSA_R(56),0x00}, {BOSA_R(57),0x00},
	{BOSA_R(58),0x00}, {BOSA_R(59),0x02}, {BOSA_R(60),0x00}, {BOSA_R(61),0x09},
	{BOSA_R(62),0x00}, {BOSA_R(63),0x00}, {BOSA_R(64),0x00}, {BOSA_R(65),0x00},
	{BOSA_R(66),0x00}, {BOSA_R(67),0x00}, {BOSA_R(68),0x00}, {BOSA_R(69),0x00},
	{BOSA_R(70),0x00}, {BOSA_R(71),0x00}, {BOSA_R(72),0x00}, {BOSA_R(73),0x00},
	{BOSA_R(74),0x00}, {BOSA_R(75),0x00}, {BOSA_R(76),0x00}, {BOSA_R(77),0x00},
	{BOSA_R(78),0x00}, {BOSA_R(79),0x00}, {BOSA_R(80),0x00}, {BOSA_R(81),0x00},
	{BOSA_R(82),0x00}, {BOSA_R(83),0x00}, {BOSA_R(84),0x00}, {BOSA_R(85),0x00},
	{BOSA_R(86),0x00}, {BOSA_R(87),0x00}, {BOSA_R(88),0x00}, {BOSA_R(89),0x00},
	{BOSA_R(90),0x00}, {BOSA_R(91),0x00}, {BOSA_R(92),0x00}, {BOSA_R(93),0x00},
	{BOSA_R(94),0x00}, {BOSA_R(95),0x00}, {BOSA_R(96),0x00}, {BOSA_R(97),0x00},
	{BOSA_R(98),0x00}, {BOSA_R(99),0x00}, {BOSA_R(100),0x00}, {BOSA_R(101),0x00},
	{BOSA_R(102),0x00}, {BOSA_R(103),0x00}, {BOSA_R(104),0x00}, {BOSA_R(105),0x00},
	{BOSA_R(106),0x00}, {BOSA_R(107),0x00}, {BOSA_R(108),0x00}, {BOSA_R(109),0x00},
	{BOSA_R(110),0x00}, {BOSA_R(111),0x00}, {BOSA_R(112),0x00}, {BOSA_R(113),0x00},
	{BOSA_R(114),0x00}, {BOSA_R(115),0x00}, {BOSA_R(116),0x00}, {BOSA_R(117),0x00},
	{BOSA_R(118),0x00}, {BOSA_R(119),0x00}, {BOSA_R(120),0x00}, {BOSA_R(121),0x00},
	{BOSA_R(122),0x00}, {BOSA_R(123),0x00}, {BOSA_R(124),0x00}, {BOSA_R(125),0x00},
	{BOSA_R(126),0x00}, {BOSA_R(127),0x00}, {BOSA_R(128),0x01}, {BOSA_R(129),0x01},
	{BOSA_R(130),0x04},	/* st1: read back once this image loads (cksum_err = bit5) */
	/* 0x383..0x38a not written: live status the runtime polls (status2
	 * 0x383, fault-service 0x389) */
	{BOSA_R(139),0x00}, {BOSA_R(140),0x00}, {BOSA_R(141),0x21}, {BOSA_R(142),0x00},
	{BOSA_R(143),0x00},
	{BOSA_REG_NUM,0x82},	/* BOSA_REG_NUM hi -- the image restores the chip id 0x8290 */
	{BOSA_REG_NUM + 1,0x90},	/* BOSA_REG_NUM lo */
	{BOSA_R(146),0x00}, {BOSA_R(147),0x00},
	{BOSA_REG_VID,0x01},	/* BOSA_REG_VID (0x0001) */
	{BOSA_R(149),0x00}, {BOSA_R(150),0x00}, {BOSA_R(151),0x00}, {BOSA_R(152),0x00},
	{BOSA_R(153),0x00},	/* bit0 = TOTAL_CHIP_RESET, left clear */
	{BOSA_R(154),0x00}, {BOSA_R(155),0x15}, {BOSA_R(156),0x00}, {BOSA_R(157),0x00},
	{BOSA_R(158),0x00},
	/* 0x39f (REG_LENGTH) intentionally not written */
	{BOSA_R(160),0x00}, {BOSA_R(161),0x00}, {BOSA_R(162),0x00}, {BOSA_R(163),0x02},
	{BOSA_R(164),0x00}, {BOSA_R(165),0x00}, {BOSA_R(166),0x00}, {BOSA_R(167),0x00},
	{BOSA_R(168),0x00}, {BOSA_R(169),0x00}, {BOSA_R(170),0x00}, {BOSA_R(171),0x00},
	{BOSA_R(172),0x00}, {BOSA_R(173),0x00}, {BOSA_R(174),0x00}, {BOSA_R(175),0x00},
	{BOSA_R(176),0x00}, {BOSA_R(177),0x00}, {BOSA_R(178),0x00}, {BOSA_R(179),0xb6},
	{BOSA_R(180),0x58}, {BOSA_R(181),0xf8}, {BOSA_R(182),0x01}, {BOSA_R(183),0x00},
	{BOSA_R(184),0x00}, {BOSA_R(185),0x00}, {BOSA_R(186),0x00}, {BOSA_R(187),0x00},
	{BOSA_R(188),0x00}, {BOSA_R(189),0x00}, {BOSA_R(190),0x00}, {BOSA_R(191),0x00},
	{BOSA_R(192),0x01}, {BOSA_R(193),0xa0}, {BOSA_R(194),0xac}, {BOSA_R(195),0x40},
	{BOSA_R(196),0x30}, {BOSA_R(197),0x00}, {BOSA_R(198),0x00}, {BOSA_R(199),0x00},
	{BOSA_R(200),0x00}, {BOSA_R(201),0x00}, {BOSA_R(202),0x00}, {BOSA_R(203),0x00},
	{BOSA_R(204),0x00}, {BOSA_R(205),0x14}, {BOSA_R(206),0x00}, {BOSA_R(207),0x00},
	{BOSA_R(208),0x00}, {BOSA_R(209),0x00}, {BOSA_R(210),0x00}, {BOSA_R(211),0x00},
	{BOSA_R(212),0x00}, {BOSA_R(213),0x00}, {BOSA_R(214),0x00}, {BOSA_R(215),0x00},
	{BOSA_R(216),0x00}, {BOSA_R(217),0x00}, {BOSA_R(218),0x00}, {BOSA_R(219),0x00},
	{BOSA_R(220),0x00}, {BOSA_R(221),0x00}, {BOSA_R(222),0x00}, {BOSA_R(223),0x00},
	{BOSA_R(224),0x00}, {BOSA_R(225),0x00}, {BOSA_R(226),0x00}, {BOSA_R(227),0x00},
	{BOSA_R(228),0x00}, {BOSA_R(229),0x00}, {BOSA_R(230),0x00}, {BOSA_R(231),0x00},
	{BOSA_R(232),0x44}, {BOSA_R(233),0x00}, {BOSA_R(234),0x00}, {BOSA_R(235),0x00},
	{BOSA_R(236),0x00}, {BOSA_R(237),0x00}, {BOSA_R(238),0x00}, {BOSA_R(239),0x00},
	{BOSA_R(240),0x00}, {BOSA_R(241),0x08}, {BOSA_R(242),0x00}, {BOSA_R(243),0x00},
	{BOSA_R(244),0x00}, {BOSA_R(245),0x00}, {BOSA_R(246),0x00}, {BOSA_R(247),0x00},
	{BOSA_R(248),0x00}, {BOSA_R(249),0x00}, {BOSA_R(250),0x00}, {BOSA_R(251),0x00},
	{BOSA_R(252),0x00}, {BOSA_R(253),0x00}, {BOSA_R(254),0x00}, {BOSA_R(255),0x00},
};

static void __init bosa_tx_enable(void)
{
	int i;

	/* Load the A4 register image (0x200-BOSA_W(80)) + base/control ...
	 * dev/MEASURED-luna_gpon.c.md sec 94. */
	for (i = 0; i < ARRAY_SIZE(bosa_init_golden); i++)
		bosa_write_reg(bosa_init_golden[i].reg, bosa_init_golden[i].val);
	mdelay(2);
	pr_info("luna-gpon: A4 image loaded: st1=0x%02x(cksum_err=%d) st2=0x%02x\n",
		bosa_read_reg(0x382) & 0xff,
		!!(bosa_read_reg(0x382) & 0x20), bosa_read_reg(0x383) & 0xff);

	for (i = 0; i < ARRAY_SIZE(bosa_tx_golden); i++)
		bosa_write_reg(bosa_tx_golden[i].reg, bosa_tx_golden[i].val);
	mdelay(10);
	pr_info("luna-gpon: BOSA TX/laser config %s (bias=0x%02x mod=0x%02x w46=0x%02x w48=0x%02x)\n",
		(bosa_not_8290b || bosa_id_num != 0x8290) ? "NOT APPLIED (writes refused)"
							  : "applied",
		bosa_read_reg(0x236) & 0xff, bosa_read_reg(0x237) & 0xff,
		bosa_read_reg(0x22e) & 0xff, bosa_read_reg(0x230) & 0xff);
}

/* Set once the cold ignition has run, so the periodic fault-service (driven from
 * the GPON FSM timer) only touches the laser after DIGITAL_POWER_ON. */
static int bosa_laser_up;
static int luna_laser_restrobe_owed;	/* each O3 entry: dev/MEASURED-luna_gpon.c.md sec 337 */

/* Program the burst bias/mod DACs from laser_bias/laser_mod (0 = golden seed):
 * hi-8 into 0x236/0x237, the low nibbles into 0x238, each latched by the 0x23d
 * strobe. Caller holds bosa_lock. */
static void luna_laser_dac_apply(void)
{
	unsigned int bias = laser_bias ? laser_bias : 0x190;
	unsigned int mod = laser_mod ? laser_mod : 0x670;

	bosa_set_bit(0x23d, 7, 0);
	bosa_set_field(0x236, 0xff, bias >> 4);
	bosa_set_field(0x238, 0x0f, bias & 0x0f);
	bosa_set_bit(0x23d, 7, 1);
	bosa_set_bit(0x23d, 7, 0);
	bosa_set_field(0x237, 0xff, mod >> 4);
	bosa_set_field(0x238, 0xf0, (mod & 0x0f) << 4);
	bosa_set_bit(0x23d, 7, 1);
	mdelay(2);
	pr_info("luna-gpon: laser DAC bias=0x%03x mod=0x%03x -> readback 0x236=0x%02x 0x237=0x%02x 0x238=0x%02x R30=0x%02x\n",
		bias, mod, bosa_read_reg(0x236) & 0xff, bosa_read_reg(0x237) & 0xff,
		bosa_read_reg(0x238) & 0xff, bosa_read_reg(0x31e) & 0xff);
}

/* The provisioning writes the unit's codes after the driver started: apply them now. */
static int laser_dac_set(const char *val, const struct kernel_param *kp)
{
	unsigned int code;

	if (kstrtouint(val, 0, &code) || code > 0xfff)
		return -EINVAL;
	mutex_lock(&bosa_lock);
	*(unsigned int *)kp->arg = code;
	if (bosa_laser_up)
		luna_laser_dac_apply();
	mutex_unlock(&bosa_lock);
	return 0;
}

static const struct kernel_param_ops laser_dac_ops = {
	.set = laser_dac_set,
	.get = param_get_uint,
};
module_param_cb(laser_bias, &laser_dac_ops, &laser_bias, 0644);
module_param_cb(laser_mod, &laser_dac_ops, &laser_mod, 0644);
MODULE_PARM_DESC(laser_bias, "burst bias DAC, this unit's 12-bit code (rtl8290b.data CAL_IBIAS uA*4096/50000); 0 = golden seed");
MODULE_PARM_DESC(laser_mod, "burst modulation DAC, this unit's 12-bit code (rtl8290b.data CAL_IMOD uA*4096/50000); 0 = golden seed");
static int apc_offk_armed;	/* rtl8290b_apc_init armed FSU/OFFK; servo will latch */
static int apc_offk_latched;	/* runtime servo latched OFFK (R29 0x31d &0x3c==0x3c) */
static u32 bosa_maint_faults;		/* recovery attempts (see the print below) */
#define BOSA_MAINT_NONE		0xffffffffu
static u32 bosa_maint_last = BOSA_MAINT_NONE;	/* last (0x383,0x389,R30) printed */
static u32 bosa_maint_since;		/* fault count when that state began */
static u32 bosa_stat_ticks;		/* heartbeat counter for the live status log */

/* Laser fault re-arm -- the BOSA "light re-arm" path for a ...
 * dev/MEASURED-luna_gpon.c.md sec 95. */
static void bosa_fault_rearm(void)
{
	bosa_set_bit(BOSA_REG_CONTROL2, 2, 1);		/* CONTROL2 TX_POW_CTL: re-enable TX drv */
	bosa_set_bit(BOSA_REG_CONTROL2, 3, 1);		/* CONTROL2 ENLD_L: re-enable laser-diode */
	bosa_set_bit(0x255, 1, 1);		/* CONTROL3 release strobe: assert */
	udelay(500);				/* 500us release-strobe settle */
	bosa_set_bit(0x255, 1, 0);		/* de-assert: 1->500us->0 clears latch */
	bosa_set_field(BOSA_REG_CONTROL2, 0x80, 0x00);	/* clear soft TX-disable (bit7) -> emit */
}

/* One laser-maintenance pass: poll the BOSA INT/fault status ...
 * dev/MEASURED-luna_gpon.c.md sec 96. */
static void bosa_laser_maint(void)
{
	int s2 = bosa_read_reg(0x383);
	int fs = bosa_read_reg(0x389);

	if (s2 < 0 || fs < 0)
		return;				/* I2C glitch — retry next tick */

	/* Live laser-state heartbeat (~every 2.5s) — bring-up visibility into whether
	 * the laser actually emits (mpd != 0) and holds bias once the FSM is in O3 and
	 * bursting upstream. EN_L = W4/0x204 bit4 (laser booster output enable). */
	if (trace && (bosa_stat_ticks++ % 50) == 0)
		luna_log("laser stat: 0x383=0x%02x 0x389=0x%02x R30=0x%02x bias=0x%02x mod=0x%02x mpd=%02x/%02x EN_L=%d state=O%u\n",
			s2 & 0xff, fs & 0xff, bosa_read_reg(0x31e) & 0xff,
			bosa_read_reg(0x236) & 0xff, bosa_read_reg(0x237) & 0xff,
			bosa_read_reg(0x320) & 0xff, bosa_read_reg(0x321) & 0xff,
			!!(bosa_read_reg(0x204) & 0x10), gpon_fsm_state);

	if (s2 & BIT(5))
		return;				/* STATUS_2 b5 DEBUG_MODE — BOSA wedged, don't poke */
	if (!(fs & 0xd1))
		return;				/* healthy: react only to genuine 0x389 TX-kill,
					 * not the benign aggregate 0x383 b4 (always set in
					 * the B-flow; re-arming on it tips the BOSA into
					 * DEBUG_MODE). */

	/*
	 * PRINT ON CHANGE, NOT EVERY 32nd TIME -- the count-based limit that was here
	 * is MEASURED to be useless. On the G24W 2026-08-31 this line came out every
	 * 3.22 s for 21 HOURS, so the fault path fires ~10 times a second and
	 * one-in-32 still floods. The cost was the WHOLE log: `dmesg | wc -l` was
	 * 1542 and the grep count for this message was ALSO 1542, with its oldest
	 * entry at t=72138 s on a board up 77087 s -- boot, probe, every other driver
	 * and whatever the GPON stack said about WHY the laser was faulting had all
	 * been evicted by the report of the fault.
	 * On-change loses nothing: every one of those 1542 lines was identical, so
	 * the new form emits ONE line for that whole run and carries the suppressed
	 * count -- strictly more informative than the flood it replaces.
	 */
	{
		u32 r30 = bosa_read_reg(0x31e) & 0xff;
		u32 now = ((s2 & 0xff) << 16) | ((fs & 0xff) << 8) | r30;

		bosa_maint_faults++;
		if (now != bosa_maint_last) {
			luna_log("laser maint re-arm (0x383=0x%02x 0x389=0x%02x R30=0x%02x)%s\n",
				s2 & 0xff, fs & 0xff, r30,
				bosa_maint_last == BOSA_MAINT_NONE ? "" :
				" [state changed]");
			if (bosa_maint_last != BOSA_MAINT_NONE)
				luna_log("... the previous laser-maint state held for %u re-arm(s)\n",
					bosa_maint_faults - bosa_maint_since);
			bosa_maint_last = now;
			bosa_maint_since = bosa_maint_faults;
		}
	}
	bosa_fault_rearm();
}

/* The W77 MCU command walk, once. The RTL8290B's on-chip 8051 ...
 * dev/MEASURED-luna_gpon.c.md sec 97. */
static const u8 bosa_w77_batch1[] = {
	BOSA_W77_BIAS_MAX_EN | BOSA_W77_MOD_MAX_EN | BOSA_W77_BACKUP_B3,		/* 0xa8 */
	BOSA_W77_BIAS_MAX_EN | BOSA_W77_MOD_MAX_EN | BOSA_W77_MOD_MAX_LOADIN,		/* 0xb0 */
	BOSA_W77_BIAS_MAX_EN | BOSA_W77_BIAS_MAX_LOADIN | BOSA_W77_MOD_MAX_LOADIN,	/* 0xd0 */
	BOSA_W77_BIAS_MAX_EN | BOSA_W77_BIAS_MAX_LOADIN | BOSA_W77_MOD_MAX_LOADIN
		| BOSA_W77_BACKUP_B3,							/* 0xd8 */
	BOSA_W77_BIAS_MAX_EN | BOSA_W77_BIAS_MAX_LOADIN | BOSA_W77_MOD_MAX_EN
		| BOSA_W77_BACKUP_B3,							/* 0xe8 */
	BOSA_W77_BIAS_MAX_EN | BOSA_W77_BIAS_MAX_LOADIN | BOSA_W77_MOD_MAX_EN,		/* 0xe0 */
};
static const u8 bosa_w77_batch2[] = {
	BOSA_W77_BIAS_MAX_EN | BOSA_W77_MOD_MAX_EN | BOSA_W77_MOD_MAX_LOADIN,		/* 0xb0 */
	BOSA_W77_BIAS_MAX_EN | BOSA_W77_BIAS_MAX_LOADIN | BOSA_W77_MOD_MAX_LOADIN,	/* 0xd0 */
	BOSA_W77_BIAS_MAX_EN | BOSA_W77_MOD_MAX_EN | BOSA_W77_MOD_MAX_LOADIN
		| BOSA_W77_BACKUP_B3,							/* 0xb8 */
	BOSA_W77_BIAS_MAX_EN | BOSA_W77_MOD_MAX_EN | BOSA_W77_MOD_MAX_LOADIN,		/* 0xb0 */
	BOSA_W77_BIAS_MAX_EN | BOSA_W77_BIAS_MAX_LOADIN | BOSA_W77_MOD_MAX_LOADIN,	/* 0xd0 */
	BOSA_W77_BIAS_MAX_EN | BOSA_W77_BIAS_MAX_LOADIN,					/* 0xc0 */
};
/* The named form MUST reproduce the stock bytes -- one line per distinct
 * command, so a re-defined bit cannot silently move a write. */
static_assert((BOSA_W77_BIAS_MAX_EN | BOSA_W77_MOD_MAX_EN | BOSA_W77_BACKUP_B3) == 0xa8, "W77 0xa8");
static_assert((BOSA_W77_BIAS_MAX_EN | BOSA_W77_MOD_MAX_EN | BOSA_W77_MOD_MAX_LOADIN) == 0xb0, "W77 0xb0");
static_assert((BOSA_W77_BIAS_MAX_EN | BOSA_W77_MOD_MAX_EN | BOSA_W77_MOD_MAX_LOADIN | BOSA_W77_BACKUP_B3) == 0xb8, "W77 0xb8");
static_assert((BOSA_W77_BIAS_MAX_EN | BOSA_W77_BIAS_MAX_LOADIN) == 0xc0, "W77 0xc0");
static_assert((BOSA_W77_BIAS_MAX_EN | BOSA_W77_BIAS_MAX_LOADIN | BOSA_W77_MOD_MAX_LOADIN) == 0xd0, "W77 0xd0");
static_assert((BOSA_W77_BIAS_MAX_EN | BOSA_W77_BIAS_MAX_LOADIN | BOSA_W77_MOD_MAX_LOADIN | BOSA_W77_BACKUP_B3) == 0xd8, "W77 0xd8");
static_assert((BOSA_W77_BIAS_MAX_EN | BOSA_W77_BIAS_MAX_LOADIN | BOSA_W77_MOD_MAX_EN) == 0xe0, "W77 0xe0");
static_assert((BOSA_W77_BIAS_MAX_EN | BOSA_W77_BIAS_MAX_LOADIN | BOSA_W77_MOD_MAX_EN | BOSA_W77_BACKUP_B3) == 0xe8, "W77 0xe8");

static void bosa_w77_walk(const u8 *cmds, unsigned int n, unsigned int settle_ms)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		bosa_write_reg(BOSA_REG_W77, cmds[i]);	/* the MCU command */
		mdelay(settle_ms);
		bosa_read_reg(BOSA_REG_R29);		/* status; the MCU consumes the read */
	}
}

/* Cold laser ignition -- the RTL8290B's MCU-driven APC ...
 * dev/MEASURED-luna_gpon.c.md sec 98. */
static void __init bosa_apc_calibrate(void)
{
	int i, k, locked = 0;
	u8 v;

	/* APC power setpoints (DCL P0/P1/Pavg) — the laser power ...
	 * dev/MEASURED-luna_gpon.c.md sec 99. */
	bosa_write_reg(0x23a, 0x26);		/* W58 DCL P0   */
	bosa_write_reg(0x23b, 0x50);		/* W59 DCL P1   */
	bosa_write_reg(0x23d, 0x50);		/* W61 DCL Pavg */

	/* MCU power-on gate: wait for the BOSA core boot/power-on-reset to finish
	 * (STATUS_2 0x383: LVCMP_TX_VALID(7) + TEMP_VALID(6); DEBUG_MODE(5) stays 0 in
	 * normal operation) plus the page-3 reset-done bit; then a 15ms settle. */
	for (i = 0; i < 2000; i++) {
		int s = bosa_read_reg(0x383);

		if (s >= 0 && (s & 0xc0) == 0xc0)
			break;
		udelay(1000);
	}
	if (i == 2000)
		pr_warn("luna-gpon: BOSA MCU power-on (0x383&0xc0) not ready\n");
	bosa_poll_bit(0x301, 7, 0, 1000, 2000);		/* page-3 reset-done clears */
	mdelay(15);

	/* --- APC-enable flow, step order 0,1,2,3,4,6,5,7 --- */
	bosa_set_bit(0x3c0, 0, 1);			/* idx0: entry reset/disable */

	/* idx1 CHECK_READY: STATUS_1(0x382) bit2 = READY_STATUS */
	if (!bosa_poll_bit(0x382, 2, 1, 1000, 2000))
		pr_warn("luna-gpon: BOSA APC CHECK_READY (0x382 bit2) timeout\n");

	/* idx2 BIAS_POWER_ON: arm the analog bias front-end + the APC bias targets.
	 * bias-max W72(0x248)=0x86 then 0x87, bias-min W73(0x249)=0x06 are the
	 * ignition ceilings; the servo converges the live bias well below them. */
	bosa_set_field(0x245, 0xff, 0x10);
	bosa_set_field(0x245, 0x0c, 0x00);		/* W6932 field (default 0) */
	bosa_write_reg(BOSA_W(88), 0x01);
	bosa_write_reg(BOSA_W(80), 0x08);
	bosa_write_reg(0x247, 0x05);
	bosa_write_reg(0x248, 0x86);			/* W72 bias-max */
	bosa_write_reg(0x239, 0xfc);
	bosa_set_bit(0x24a, 3, 1);
	bosa_write_reg(0x249, 0x06);			/* W73 bias-min */
	bosa_write_reg(0x24c, 0x71);
	bosa_write_reg(0x24c, 0x72);
	bosa_write_reg(0x247, 0x06);
	bosa_write_reg(0x248, 0x87);			/* W72 bias-max final */
	/* loop_mode 0 (W69=0x00): 0x23e source = !(0x239 bit3) */
	v = bosa_get_field(0x239, BIT(3)) ^ 1;
	bosa_set_field(0x23e, 0xff, v);
	bosa_write_reg(0x232, 0x07);
	bosa_write_reg(0x244, 0xf8);
	bosa_set_bit(0x252, 3, 1);			/* W82 DAC/loop commit */

	bosa_set_field(0x239, 0xff, 0xfc);		/* idx3 */
	bosa_set_field(0x23c, 0xff, 0xfd);		/* idx4 */

	/* RTL8290B MCU bias/mod-MAX loadin handshake (the APC-init ...
	 * dev/MEASURED-luna_gpon.c.md sec 100. */
	{
		bosa_set_bit(0x24e, 7, 1);		/* W78 b7 (apc_init prefix) */
		bosa_w77_walk(bosa_w77_batch1, ARRAY_SIZE(bosa_w77_batch1), 10);
		bosa_set_bit(0x243, 7, 1);		/* W67 b7 */
		bosa_set_field(BOSA_W(80), 0x08, 0x00);	/* W80 clear bit3 */
		bosa_w77_walk(bosa_w77_batch2, ARRAY_SIZE(bosa_w77_batch2), 10);
		pr_info("luna-gpon: DBG post-W77hs: EN_L=%d bias=0x%02x R29=0x%02x R33=0x%02x 0x383=0x%02x R30=0x%02x\n",
			!!(bosa_read_reg(0x204) & 0x10), bosa_read_reg(0x236) & 0xff,
			bosa_read_reg(0x31d) & 0xff, bosa_read_reg(0x321) & 0xff,
			bosa_read_reg(0x383) & 0xff, bosa_read_reg(0x31e) & 0xff);
	}

	v = bosa_get_field(0x31f, 0x03);		/* idx6 */
	bosa_set_field(0x232, 0xc0, v);
	bosa_set_field(0x249, 0x18, v);

	/* TEST: pre-load this board's calibrated laser bias/mod (per-board optical
	 * calibration LUT @25C = 0x18/0x34, 12-bit DAC = byte<<4) BEFORE turning the
	 * laser on, so it ignites at the right optical power instead of the hotter
	 * default that trips MPD_VHIGH at DIGITAL_POWER_ON. */
	bosa_set_bit(0x23d, 7, 0);
	bosa_set_field(0x236, 0xff, 0x18);		/* bias hi-8 (calibrated LUT @25C). NOTE: lowering to 0x0a did NOT save DS RX — laser-on deafens RX independent of optical power; the fix is burst-gating, not bias level. */
	bosa_set_field(0x238, 0x0f, 0x00);
	bosa_set_bit(0x23d, 7, 1);
	bosa_set_bit(0x23d, 7, 0);
	bosa_set_field(0x237, 0xff, 0x34);		/* mod hi-8 */
	bosa_set_field(0x238, 0xf0, 0x00);
	bosa_set_bit(0x23d, 7, 1);

	/* Disarm W53/0x235 fault-detect for the rest of ignition (after the W77
	 * handshake — disarming it BEFORE the handshake regressed O3->O1). Keeps a
	 * (false) MPD_VHIGH from latching 0x383 b4 at DPO and zeroing the bias; the safe
	 * subset is re-armed at txEnableFlow end. */
	bosa_set_field(0x235, 0xff, 0x00);

	/* idx5 DIGITAL_POWER_ON: turn on the digital/laser power, then blind settle */
	bosa_set_bit(BOSA_W(80), 4, 1);			/* W80 bit4 = 1 */
	bosa_set_bit(0x380, 0, 1);
	mdelay(101);
	pr_info("luna-gpon: DBG post-DPO: bias(0x236)=0x%02x 0x389=0x%02x 0x383=0x%02x R30=0x%02x\n",
		bosa_read_reg(0x236) & 0xff, bosa_read_reg(0x389) & 0xff,
		bosa_read_reg(0x383) & 0xff, bosa_read_reg(0x31e) & 0xff);

	bosa_set_bit(0x23c, 0, 1);			/* idx7 */
	bosa_set_bit(BOSA_REG_CONTROL2, 3, 1);			/* CONTROL2 bit3 = 1 */
	mdelay(5);

	/* HYPOTHESIS TEST: disable the laser TX (CONTROL2/0x254 bit7=1) during the
	 * offset-K calibration so the ADC zero-offset is measured with no emission and
	 * the TX path can't trip TX_FAULT while the analog block is mid-cal. The
	 * txEnableFlow below re-enables TX (0x254 bit7=0). */
	bosa_set_bit(BOSA_REG_CONTROL2, 7, 1);

	/* RTL8290B FSU (Field Setup Unit) offset/gain auto-cal plus ...
	 * dev/MEASURED-luna_gpon.c.md sec 101. */
	bosa_set_field(BOSA_W(80), 0xc0, 0x03);	/* apcLoopMode DCL: W80[7:6]=3 */
	bosa_set_bit(BOSA_W(80), 5, 0);		/* FSU arm: W80 b5 low */
	bosa_set_bit(BOSA_W(80), 4, 1);		/*          W80 b4 high */
	bosa_set_bit(0x20e, 7, 1);		/*          W14 b7 high (LOADIN) */
	bosa_set_bit(BOSA_W(80), 5, 1);		/*          W80 b5 high (path strobe) */
	bosa_set_bit(0x241, 6, 0);		/* fsuMode 0: W65 b6 */
	bosa_write_reg(BOSA_REG_W77, BOSA_W77_BIAS_MAX_EN | BOSA_W77_MOD_MAX_EN |
		       BOSA_W77_MOD_MAX_LOADIN);	/* W77=0xb0 arms the done-check */
	for (k = 0; k < 250; k++) {
		int r29 = bosa_read_reg(BOSA_REG_R29);

		if (r29 >= 0 && (r29 & 0x3c) == 0x3c) {
			locked = 1;
			break;
		}
		udelay(200);
	}
	bosa_set_bit(0x20e, 7, 0);		/* finalize: de-assert W14 LOADIN */
	bosa_set_bit(BOSA_W(80), 4, 0);		/*           de-assert W80 b4 -> latch */
	pr_info("luna-gpon: DBG post-FSU: done=%d R29=0x%02x bias=0x%02x R33=0x%02x 0x383=0x%02x BOSA_W(80)=0x%02x\n",
		locked, bosa_read_reg(0x31d) & 0xff, bosa_read_reg(0x236) & 0xff,
		bosa_read_reg(0x321) & 0xff, bosa_read_reg(0x383) & 0xff,
		bosa_read_reg(BOSA_W(80)) & 0xff);

	/* txEnableFlow (B-flow: laser output enable AFTER FSU ...
	 * dev/MEASURED-luna_gpon.c.md sec 284. */
	bosa_set_field(BOSA_REG_CONTROL2, 0xff, 0x8d);

	/* The laser bias/mod LUT -- the PER-BOARD calibrated ...
	 * dev/MEASURED-luna_gpon.c.md sec 102. */
	{
		u8 lut_bias = 0x18, lut_mod = 0x34;	/* laser LUT @ 25C (calibrated). lowering bias to 0x0a did NOT save DS RX (laser-on deafens RX regardless of optical power) -> the fix is burst-gating the TX path, not the bias level */

		bosa_set_bit(0x23d, 7, 0);		/* bias DAC: strobe low */
		bosa_set_field(0x236, 0xff, lut_bias);	/* W54 bias hi-8 (= bias12[11:4]) */
		bosa_set_field(0x238, 0x0f, 0x00);	/* W56 bias12[3:0] (LUT<<4 -> 0) */
		bosa_set_bit(0x23d, 7, 1);		/* latch */
		bosa_set_bit(0x23d, 7, 0);		/* mod DAC: strobe low */
		bosa_set_field(0x237, 0xff, lut_mod);	/* W55 mod hi-8 (= mod12[11:4]) */
		bosa_set_field(0x238, 0xf0, 0x00);	/* W56 mod12[3:0] (LUT<<4 -> 0) */
		bosa_set_bit(0x23d, 7, 1);		/* latch */
	}

	/* (removed an idx4 0x245 loop_mode write — the RTL8290B loop mode is W80[7:6],
	 * set by FSU/DCL) */
	bosa_set_field(0x230, 0xff, 0x00);		/* idx5 */
	bosa_set_field(BOSA_W(80), 0xff, 0xe9);		/* W80=0xe9: converged (DCL[7:6]=3 + b5 + 0x09) */
	mdelay(51);					/* idx6 */
	/* (removed a 0x24d low-nibble write — W77 owned by the FSU ...
	 * dev/MEASURED-luna_gpon.c.md sec 103. */
	bosa_set_field(BOSA_REG_CONTROL2, 0x40, 0x00);		/* CONTROL2 bit6 LOS_PIN_TRI = 0 */
	bosa_set_bit(BOSA_REG_W4, 4, 1);			/* W4 EN_L = 1: laser booster ON */
	mdelay(200);					/* 200ms laser-bias settle */
	bosa_set_field(BOSA_REG_CONTROL2, 0x40, 0x40);		/* CONTROL2 bit6 LOS_PIN_TRI = 1 */

	bosa_set_field(BOSA_REG_CONTROL2, 0x80, 0x00);		/* idx8 CONTROL2 bit7 = 0 */
	/* W53 fault-detect enables. The device default arms ALL ...
	 * dev/MEASURED-luna_gpon.c.md sec 104. */
	bosa_set_field(0x235, 0xff, 0xfc);		/* idx7: arm faults, MPD hi/lo OFF */
	bosa_set_field(0x25f, 0xff, 0x02);
	bosa_set_field(0x260, 0xff, 0x00);

	/* Post-enable settle + diagnostic. NO init re-arm loop here: ...
	 * dev/MEASURED-luna_gpon.c.md sec 285. */
	mdelay(50);
	pr_info("luna-gpon: post-txen: 0x389=0x%02x 0x383=0x%02x R30=0x%02x bias=0x%02x R33=0x%02x mod=0x%02x EN_L=%d\n",
		bosa_read_reg(0x389) & 0xff, bosa_read_reg(0x383) & 0xff,
		bosa_read_reg(0x31e) & 0xff, bosa_read_reg(0x236) & 0xff,
		bosa_read_reg(0x321) & 0xff, bosa_read_reg(0x320) & 0xff,
		!!(bosa_read_reg(0x204) & 0x10));

	/* BURST-GATE the laser. EN_L is the booster OUTPUT-enable: ...
	 * dev/MEASURED-luna_gpon.c.md sec 105. */
	bosa_set_bit(BOSA_REG_W4, 4, 0);			/* EN_L = 0: burst-gate (0x8e) */
	mdelay(5);
	pr_info("luna-gpon: burst-gate EN_L=0 -> 0x204=0x%02x R33=0x%02x R30=0x%02x 0x383=0x%02x\n",
		bosa_read_reg(0x204) & 0xff, bosa_read_reg(0x321) & 0xff,
		bosa_read_reg(0x31e) & 0xff, bosa_read_reg(0x383) & 0xff);

	/* Hand off to the continuous fault-service driven from the GPON FSM timer. */
	bosa_laser_up = 1;

	pr_info("luna-gpon: laser ignite: lock=%d R30=0x%02x(offk_done=%d txsd=%d) bias=0x%02x mod=0x%02x mpd=%02x/%02x\n",
		locked, bosa_read_reg(0x31e) & 0xff,
		!!(bosa_read_reg(0x31e) & BIT(7)), !!(bosa_read_reg(0x31e) & BIT(6)),
		bosa_read_reg(0x236) & 0xff, bosa_read_reg(0x238) & 0xff,
		bosa_read_reg(0x320) & 0xff, bosa_read_reg(0x321) & 0xff);
}

/* RTL8290B B-variant laser APC/OFFK ignition -- the flow ...
 * dev/MEASURED-luna_gpon.c.md sec 106. */
static void __init rtl8290b_apc_init(void)
{
	/* Exact stock B-variant laser APC init behavior (GPON/pon=1), ...
	 * dev/MEASURED-luna_gpon.c.md sec 107. */
	int i;
	u8 t8;

	pr_info("luna-gpon: rtl8290b_apc_init: B-variant OFFK (exact stock seq, base 0x578)\n");

	/* step 1: MCU power-on kick + gate + ~11ms settle */
	bosa_write_reg(0x380, 0x01);
	if ((bosa_read_reg(0x380) & 0xff) != 1) {
		pr_warn("luna-gpon: rtl8290b_apc_init: MCU power-on (0x380!=1) -> abort\n");
		return;
	}
	for (i = 0; i < 11; i++)
		udelay(1000);

	/* CHECK_READY: (0x383&0xe0)==0xc0, then (0x301 b7) */
	for (i = 0; i < 20000; i++) {
		int sret = bosa_read_reg(0x383);

		if (sret >= 0 && (sret & 0xe0) == 0xc0)
			break;
		udelay(50);
	}
	bosa_poll_bit(0x301, 7, 1, 50, 20000);

	/* step 2: OFFK enable trio (W78 b7; W62=W63=0xfd full bytes) */
	bosa_set_bit(0x24e, 7, 1);
	bosa_write_reg(0x23e, 0xfd);
	bosa_write_reg(0x23f, 0xfd);

	/* step 3: per-field config (optics-cfg table @0x578, pon=1 values) */
	bosa_write_reg(0x23a, 0x00);		/* apcIavg = 0			*/
	bosa_set_field(0x23b, 0xf0, 0x02);	/* apcEr (optics-cfg[0x1e]=2)		*/
	bosa_set_field(0x23b, 0x0c, 0x03);	/* apcApcTimer = 3		*/
	bosa_set_field(0x23b, 0x03, 0x03);	/* apcErcTimer = 3		*/
	bosa_set_field(0x240, 0x03, 0x00);	/* apcLpfBw = 0			*/
	bosa_set_bit(0x23e, 0, 0);		/* txsdMode = 0			*/
	bosa_set_field(0x24c, 0xe0, 0x01);	/* txsdTh = 1			*/
	bosa_set_bit(0x24c, 4, 1);		/* txsdTiaGain = 1		*/
	bosa_set_bit(0x24c, 3, 1);		/* txsdHighLoopGain = 1		*/
	for (i = 0; i < 11; i++)
		udelay(1000);
	bosa_read_reg(0x31d);

	/* step 4: W77 (0x24d) MCU-command walk BATCH 1 */
	bosa_w77_walk(bosa_w77_batch1, ARRAY_SIZE(bosa_w77_batch1), 11);

	/* step 5: ERC chopper + W80 b3 toggle */
	bosa_set_bit(0x243, 7, 1);
	bosa_set_bit(BOSA_W(88), 6, 1);
	bosa_set_bit(BOSA_W(88), 5, 1);
	bosa_set_bit(BOSA_W(88), 4, 1);		/* apcErcChopperEn = 1 (b6/b5/b4) */
	t8 = bosa_read_reg(BOSA_W(80)) & 0xff;
	bosa_write_reg(BOSA_W(80), t8 & 0xf7);
	bosa_write_reg(BOSA_W(80), t8 | 0x08);

	/* step 6: W77 walk BATCH 2 */
	bosa_w77_walk(bosa_w77_batch2, ARRAY_SIZE(bosa_w77_batch2), 11);

	/* step 7: OFFK offset compute+write -- STOCK SKIPS IT (optics-cfg[0x72]=0). */

	/* step 8: bias/mod init-code seeds (optics-cfg[0x65]=0 -> manual path) + loop mode */
	bosa_set_bit(0x23d, 7, 0);
	bosa_set_field(0x236, 0xff, 0x10);	/* Ibias init 0x100 >> 4 (stock seed)	*/
	bosa_set_field(0x238, 0x0f, 0x00);	/* Ibias[3:0] = 0		*/
	bosa_set_bit(0x23d, 7, 1);
	bosa_set_bit(0x23d, 7, 0);
	bosa_set_field(0x237, 0xff, 0x20);	/* Imod init 0x200 >> 4 (stock seed)	*/
	bosa_set_field(0x238, 0xf0, 0x00);	/* Imod[3:0] = 0		*/
	bosa_set_bit(0x23d, 7, 1);
	bosa_set_field(BOSA_W(80), 0xc0, 0x03);	/* apcLoopMode DCL (remap 1->3)	*/
	bosa_set_bit(0x24a, 4, 0);		/* apcLoopModeEx = 0		*/
	bosa_set_field(0x23d, 0x70, 0x04);	/* apcLaserOnDelay = 4		*/
	bosa_set_field(0x23d, 0x03, 0x02);	/* apcSettleCnt = 2		*/
	bosa_set_field(0x239, 0x38, 0x05);	/* apcApcLoopGain = 5		*/
	bosa_set_field(0x23c, 0x07, 0x00);	/* apcCmpd = 0			*/
	bosa_set_field(0x239, 0x07, 0x03);	/* apcErcLoopGain = 3		*/
	bosa_set_field(0x23c, 0xf8, 0x05);	/* apcErTrim = 5		*/

	/* step 9: FSU config (the previously-guessed block, now exact) */
	bosa_set_bit(0x241, 6, 1);		/* fsuMode = 1			*/
	bosa_set_field(0x241, 0x30, 0x01);	/* fsuApcLoopGain = 1 (b4-5)	*/
	bosa_set_field(0x241, 0x0c, 0x01);	/* fsuApcRampb = 1 (b2-3)	*/
	bosa_set_field(0x241, 0x03, 0x03);	/* fsuApcRampm = 3		*/
	bosa_set_bit(0x242, 7, 1);		/* fsuRstCount = 1		*/
	bosa_set_field(0x242, 0x60, 0x02);	/* fsuSettleCount = 2 (b5-6)	*/
	bosa_set_field(0x242, 0x18, 0x02);	/* fsuErcLoopGain = 2 (b3-4)	*/
	bosa_set_field(0x242, 0x06, 0x00);	/* fsuErcRampm = 0 (b1-2)	*/

	/* step 10: txsd off-rst */
	bosa_set_bit(BOSA_W(80), 3, 1);		/* txsdOffRstCount = 1		*/

	/* step 11: ceilings + enables (modmax 0xcc, biasmax 0x4d, biasmin 0x01) */
	bosa_set_bit(0x246, 2, 0);
	bosa_set_field(0x247, 0xff, 0xcc);
	bosa_set_bit(0x246, 2, 1);		/* apcModMax = 0xcc		*/
	bosa_set_bit(0x246, 1, 0);
	bosa_set_field(0x248, 0xff, 0x4d);
	bosa_set_bit(0x246, 1, 1);		/* apcBiasMax = 0x4d		*/
	bosa_set_bit(BOSA_W(88), 7, 0);		/* apcCrossEn = 0		*/
	bosa_set_bit(0x246, 0, 0);
	bosa_set_field(0x249, 0xff, 0x01);
	bosa_set_bit(0x246, 0, 1);		/* apcBiasMin = 0x01		*/
	bosa_set_bit(0x246, 5, 1);		/* apcModMaxEn = 1		*/
	bosa_set_bit(0x246, 3, 1);		/* apcBiasMinEn = 1		*/
	bosa_set_field(0x283, 0xff, 0x01);	/* apcCrossStr = 1		*/

	/* step 12: FSU ARM (BOSA_W(80) b5=0->b4=1->0x20e b7=1->BOSA_W(80) b5=1) + commit */
	bosa_set_bit(BOSA_W(80), 5, 0);
	bosa_set_bit(BOSA_W(80), 4, 1);
	bosa_set_bit(0x20e, 7, 1);
	bosa_set_bit(BOSA_W(80), 5, 1);
	bosa_write_reg(0x232, 0xc0);
	bosa_write_reg(0x24a, 0x60);

	apc_offk_armed = 1;
	pr_info("luna-gpon: rtl8290b_apc_init: armed; post-cfg R29=0x%02x R30=0x%02x 0x241=0x%02x 0x242=0x%02x 0x247=0x%02x 0x248=0x%02x; servo latches at O5\n",
		bosa_read_reg(0x31d) & 0xff, bosa_read_reg(0x31e) & 0xff,
		bosa_read_reg(0x241) & 0xff, bosa_read_reg(0x242) & 0xff,
		bosa_read_reg(0x247) & 0xff, bosa_read_reg(0x248) & 0xff);
}

static void __init bosa_probe(void)
{
	int hb  = bosa_read_reg(BOSA_REG_NUM);
	int lb  = bosa_read_reg(BOSA_REG_NUM + 1);
	int vid = bosa_read_reg(BOSA_REG_VID);
	/* ★★★ A SILENT RTL8290B REGISTER MAP IS NOT A SILENT BUS, AND ...
	 * dev/MEASURED-luna_gpon.c.md sec 108. */
	bool id_answered = (hb >= 0 && lb >= 0 && vid >= 0);

	if (id_answered) {
		bosa_id_num = (hb << 8) | lb;
		bosa_id_vid = vid;

		/* Read the RX-path registers (read-only): RX power-down, ...
		 * dev/MEASURED-luna_gpon.c.md sec 109. */
		bosa_w41     = bosa_read_reg(BOSA_REG_W41);
		bosa_ctrl2   = bosa_read_reg(BOSA_REG_CONTROL2);
		bosa_status2 = bosa_read_reg(BOSA_REG_STATUS2);
	} else {
		pr_warn("luna-gpon: the RTL8290B register map did not answer (hb=%d lb=%d vid=%d; -EIO is %d). That is the EXPECTED result for a module that is not an RTL8290B, so on its own it identifies NOTHING -- asking SFF-8472 at slave 0x50 what this module IS.\n",
			hb, lb, vid, -EIO);
	}

	/* An "UNEXPECTED" id used to be logged and then ignored. Ask ...
	 * dev/MEASURED-luna_gpon.c.md sec 110. */
	if (bosa_id_num != 0x8290) {
		int ident = bosa_i2c_read8(0x50, 0);
		int extid = bosa_i2c_read8(0x50, 1);
		int conn  = bosa_i2c_read8(0x50, 2);
		int br    = bosa_i2c_read8(0x50, 12);
		int raw[16];
		char vend[17];
		char part[17];
		int k;

		for (k = 0; k < 16; k++)
			raw[k] = bosa_i2c_read8(0x50, 20 + k);
		bosa_sff_text(vend, raw, 16);
		for (k = 0; k < 16; k++)
			raw[k] = bosa_i2c_read8(0x50, 40 + k);
		bosa_sff_text(part, raw, 16);
		/* Three-outcome identity decision, hoisted: the module's own ...
		 * dev/MEASURED-luna_gpon.c.md sec 111. */
		if (ident < 0 && !id_answered) {
			pr_warn("luna-gpon: BOSA identity probes FAILED: RTL8290B map silent AND SFF-8472 A0 at slave 0x50 silent (ident=%d). ⚠ A2 (slave 0x51) WAS NOT ASKED, and that is where a Semtech GN2xL9x answers -- so this is NOT evidence the I2C bus is down and must not be read as such.\n",
				ident);
			goto id_done;
		}

		switch (bosa_module_classify(ident, extid, vend, part)) {
		case BOSA_MODULE_NAMED_OURS:
			pr_info("luna-gpon: optical module identifies itself as vendor='%s' part='%s' -- RTL8290B register path stays ENABLED (the SFF-8472 page says WHAT it is; merely HAVING one never meant it was foreign)\n",
				vend, part);
			break;
		case BOSA_MODULE_FOREIGN:
			bosa_not_8290b = true;
			pr_warn("luna-gpon: optical module is NOT an RTL8290B -- SFF-8472 A0 ident=0x%02x extid=0x%02x connector=0x%02x br=%d00MBd vendor='%s'. RTL8290B register writes are now REFUSED; DDM must come from A2 (slave 0x51) bytes 104/105, not the analog banks.\n",
				ident, extid, conn, br, vend);
			break;
		case BOSA_MODULE_COULD_NOT_TELL:
		default:
			pr_warn("luna-gpon: BOSA id 0x%04x is not 0x8290 and SFF-8472 A0 did not identify it either (ident=%d) -- leaving the RTL8290B path enabled; this is 'could not tell', not 'it is an 8290B'\n",
				bosa_id_num, ident);
			break;
		}
	}

id_done:
	/* ★ AN UNREAD REGISTER IS NOT A REGISTER READING 0xff. When the map never
	 * answered, `bosa_w41`/`ctrl2`/`status2` still hold their -1 sentinels, and
	 * masking those with 0xff would print rxpwdn=1 los_tri=1 rx_los=1 -- three
	 * confident bits about an RX path nobody sampled. Say COULD NOT ASK. */
	if (!id_answered) {
		pr_info("luna-gpon: BOSA identity NOT ESTABLISHED -- the RTL8290B map is silent, so num/vid and the w41/ctrl2/status2 RX bits were never read and are NOT reported. RTL8290B register writes stay REFUSED.\n");
		return;
	}
	pr_info("luna-gpon: BOSA RTL8290B num=0x%04x vid=0x%02x %s | w41=0x%02x(rxpwdn=%d) ctrl2=0x%02x(los_tri=%d) status2=0x%02x(rx_los=%d)\n",
		bosa_id_num, bosa_id_vid,
		(bosa_id_num == 0x8290) ? "detected" : "UNEXPECTED",
		bosa_w41 & 0xff, (bosa_w41 >> 4) & 1,
		bosa_ctrl2 & 0xff, (bosa_ctrl2 >> 6) & 1,
		bosa_status2 & 0xff, (bosa_status2 >> 2) & 1);
}

/* One per-rate/lane analog bank of the table below: 22 ...
 * dev/MEASURED-luna_gpon.c.md sec 112. */
#define SDS_ANA_RATE_BANK(base, v24, v28, v2c, v30, v38, v3c, v40, v48, v4c, v50)	\
	{ (base) + 0x00, 0x00000f00 }, { (base) + 0x04, 0x0000b8c6 },	\
	{ (base) + 0x08, 0x0000a112 }, { (base) + 0x0c, 0x00004280 },	\
	{ (base) + 0x10, 0x0000f53f }, { (base) + 0x14, 0x00004fdf },	\
	{ (base) + 0x18, 0x00000001 }, { (base) + 0x1c, 0x0000309b },	\
	{ (base) + 0x20, 0x0000225c }, { (base) + 0x24, (v24) },	\
	{ (base) + 0x28, (v28) }, { (base) + 0x2c, (v2c) },	\
	{ (base) + 0x30, (v30) }, { (base) + 0x34, 0x0000121e },	\
	{ (base) + 0x38, (v38) }, { (base) + 0x3c, (v3c) },	\
	{ (base) + 0x40, (v40) }, { (base) + 0x44, 0x00001012 },	\
	{ (base) + 0x48, (v48) }, { (base) + 0x4c, (v4c) },	\
	{ (base) + 0x50, (v50) }, { (base) + 0x54, 0x0000f000 }

/* One FIB (fiber optical front-end) bank: the same 17 { off, ...
 * dev/MEASURED-luna_gpon.c.md sec 113. */
#define SDS_FIB_BANK(base)							\
	{ (base) + 0x00, 0x00001940 }, { (base) + 0x04, 0x00006109 },	\
	{ (base) + 0x08, 0x0000e001 }, { (base) + 0x0c, 0x00003290 },	\
	{ (base) + 0x10, 0x000001a0 }, { (base) + 0x1c, 0x00000004 },	\
	{ (base) + 0x3c, 0x00008000 }, { (base) + 0x40, 0x00000083 },	\
	{ (base) + 0x48, 0x00005000 }, { (base) + 0x58, 0x00000001 },	\
	{ (base) + 0x5c, 0x00004001 }, { (base) + 0x60, 0x00000004 },	\
	{ (base) + 0x64, 0x0000326a }, { (base) + 0x6c, 0x0000115d },	\
	{ (base) + 0x70, 0x000033fa }, { (base) + 0x74, 0x0000e46a },	\
	{ (base) + 0x78, 0x0000071e }

/* Full SerDes analog + WSDS configuration -- the operating ...
 * dev/MEASURED-luna_gpon.c.md sec 114. */
static const struct { u32 off; u32 val; } sds_analog_golden[] __initconst = {
	/* WSDS analog front + digital RX-path config */
	{ 0x22000, 0x00000805 }, { 0x22008, 0x0000ffff }, { 0x2201c, 0x0000ffff },
	{ 0x22020, 0x0000ffff }, { 0x22038, 0x00000900 }, { 0x22048, 0x000000ff },
	{ 0x22050, 0x00022300 }, { 0x22054, 0x00022310 }, { 0x22058, 0x083d0100 },
	{ 0x22060, 0x00000fff }, { 0x22064, 0x0000cf45 }, { 0x22068, 0x00000f45 },
	/* SDS_ANA_MISC (RX-enable force, speed-select, force-SD) */
	{ 0x22500, 0x00000030 }, { 0x22504, 0x00000030 }, { 0x22508, 0x00003000 },
	/* SDS_ANA_COM (CMU, RX CDR front-end, filters, bias) */
	{ 0x22580, 0x00003400 }, { 0x22584, 0x000073a4 }, { 0x22588, 0x00006df8 },
	{ 0x2258c, 0x00008941 }, { 0x22590, 0x00008884 }, { 0x22594, 0x0000413f },
	{ 0x22598, 0x00004fc0 }, { 0x2259c, 0x00005682 }, { 0x225a0, 0x00000713 },
	{ 0x225a4, 0x000002f5 }, { 0x225a8, 0x00002793 }, { 0x225ac, 0x0000b000 },
	{ 0x225b0, 0x00004848 }, { 0x225b4, 0x000000c8 }, { 0x225bc, 0x000008f2 },
	{ 0x225c0, 0x00001042 }, { 0x225c4, 0x0000c391 }, { 0x225c8, 0x00006a00 },
	{ 0x225cc, 0x00006600 }, { 0x225d0, 0x0000c000 },
	/* 0x225d8 (COM_REG22 TX_AMP/EMP) is set later, in gpon_serdes_init's TX
	 * section, to the rev-A (ModeV1) value 0x29 (TX_AMP=0x5, TX_EMP=0x1) via
	 * field-writes. NOT a full write here so the upper bits keep their reset
	 * state. (Boot default 0x39/TX_AMP=0x7 over-drives.) */
	{ 0x225dc, 0x00000418 }, { 0x225e0, 0x00008001 }, { 0x225e4, 0x0000001f },
	{ 0x225e8, 0x000011e4 }, { 0x225ec, 0x00009422 }, { 0x225f0, 0x00008502 },
	{ 0x225f4, 0x00000ff0 }, { 0x225f8, 0x0000000a },
	/* SDS_ANA_GPON (GPON-rate CDR/PLL/PCM config) */
	SDS_ANA_RATE_BANK(0x22708,
		/* +0x24 */ 0x00001061, /* +0x28 */ 0x0000110d,
		/* +0x2c */ 0x00004854, /* +0x30 */ 0x000080c5,
		/* +0x38 */ 0x0000307b, /* +0x3c */ 0x00000271,
		/* +0x40 */ 0x00000271, /* +0x48 */ 0x0000f162,
		/* +0x4c */ 0x00003026, /* +0x50 */ 0x0000a780),
	/* SDS_ANA_GPON additional per-rate/lane banks (the RX path selects among
	 * these; leaving them at reset starves the active RX/SD analog).
	 * 0x22608 repeats the 0x22708 tuning exactly; 0x22688 and 0x22788 are
	 * the two variant tunings, and the argument labels above make the
	 * per-bank differences readable side by side. */
	SDS_ANA_RATE_BANK(0x22608,
		/* +0x24 */ 0x00001061, /* +0x28 */ 0x0000110d,
		/* +0x2c */ 0x00004854, /* +0x30 */ 0x000080c5,
		/* +0x38 */ 0x0000307b, /* +0x3c */ 0x00000271,
		/* +0x40 */ 0x00000271, /* +0x48 */ 0x0000f162,
		/* +0x4c */ 0x00003026, /* +0x50 */ 0x0000a780),
	SDS_ANA_RATE_BANK(0x22688,
		/* +0x24 */ 0x00001062, /* +0x28 */ 0x00002000,
		/* +0x2c */ 0x00001050, /* +0x30 */ 0x000080c1,
		/* +0x38 */ 0x0000107b, /* +0x3c */ 0x00000280,
		/* +0x40 */ 0x00000280, /* +0x48 */ 0x0000f862,
		/* +0x4c */ 0x00003938, /* +0x50 */ 0x00003100),
	SDS_ANA_RATE_BANK(0x22788,
		/* +0x24 */ 0x00001062, /* +0x28 */ 0x00002000,
		/* +0x2c */ 0x00004850, /* +0x30 */ 0x000080c5,
		/* +0x38 */ 0x0000103e, /* +0x3c */ 0x00000280,
		/* +0x40 */ 0x00000280, /* +0x48 */ 0x0000f862,
		/* +0x4c */ 0x00003938, /* +0x50 */ 0x0000b100),
	/* FIB (fiber optical front-end) config — 4 identical banks (one macro,
	 * see SDS_FIB_BANK above). This block powers and configures the optical
	 * RX/SD path; leaving it at reset keeps the optical front-end down so
	 * the signal-detect never asserts. FIB_REG0 (bank base) carries
	 * FP_CFG_FIB_PDOWN at bit11, cleared separately below to turn fiber
	 * power on. */
	SDS_FIB_BANK(0x22c00),
	SDS_FIB_BANK(0x22c80),
	SDS_FIB_BANK(0x22d00),
	SDS_FIB_BANK(0x22d80),
};

/* FIB_REG0 bank bases; FP_CFG_FIB_PDOWN (bit11) cleared = fiber power on. */
#define FIB_REG0_PDOWN		BIT(11)
static const u32 fib_reg0_banks[] __initconst = {
	0x22c00, 0x22c80, 0x22d00, 0x22d80,
};

/* Bring up the PON SerDes so the GPON MAC core gets its line ...
 * dev/MEASURED-luna_gpon.c.md sec 115. */
static int gpon_serdes_init(void)	/* not __init: re-run on re-range from gpon_cdr_reset_worker */
{
	int i;

	/* 1. Park CFG_SDS_MODE at the illegal/off value (0x1f) while ...
	 * dev/MEASURED-luna_gpon.c.md sec 286. */
	sw_field(SDS_CFG, 4, 0, SDS_MODE_OFF);
	sw_wr(WSDS_DIG_01, 0);				/* clear force-SDS dummy   */
	sw_field(WSDS_DIG_00, 0, 0, 0);			/* STOP_CLK = 0            */

	/* 2. Program the FULL analog block to the operational values ...
	 * dev/MEASURED-luna_gpon.c.md sec 116. */
	for (i = 0; i < ARRAY_SIZE(sds_analog_golden); i++)
		sw_wr(SDS(sds_analog_golden[i].off), sds_analog_golden[i].val);
	for (i = 0; i < ARRAY_SIZE(fib_reg0_banks); i++)
		sw_wr(SDS(fib_reg0_banks[i]),
		      sw_rd(SDS(fib_reg0_banks[i])) & ~FIB_REG0_PDOWN);

	/* 3. Pulse the SDS reset to latch the analog config. Stock ...
	 * dev/MEASURED-luna_gpon.c.md sec 287. */
	if (serdes_sds_cfgrst)
		sw_field(SW_SOFTWARE_RST, 7, 7, 1);	/* legacy CMD_SDS_CFG_RST_PS */
	sw_field(SW_SOFTWARE_RST, 0, 0, 1);		/* CMD_SDS_RST_PS          */
	mdelay(10);

	/* 4. Release all datapath soft-reset-B lines and force the 125M ref clock
	 *    (golden WSDS_DIG_00 = 0xf30), then pulse the RX/TX interface reset-B
	 *    lines (golden WSDS_DIG_1D = 0x1c000). Re-clear FIB power-down, which the
	 *    reset re-asserts. */
	sw_wr(WSDS_DIG_00, WSDS_DIG00_RUN);
	sw_field(WSDS_DIG_1D, 15, 15, 0);		/* RX interface reset-B 0  */
	sw_field(WSDS_DIG_1D, 16, 16, 0);		/* TX interface reset-B 0  */
	sw_field(WSDS_DIG_1D, 14, 14, 1);		/* common interface rst-B  */
	sw_field(WSDS_DIG_1D, 15, 15, 1);		/* RX interface reset-B 1  */
	sw_field(WSDS_DIG_1D, 16, 16, 1);		/* TX interface reset-B 1  */
	mdelay(10);
	/* ★ AND THIS RE-CLEAR TOO -- it is the one that UNDOES what ...
	 * dev/MEASURED-luna_gpon.c.md sec 288. */
	for (i = 0; i < ARRAY_SIZE(fib_reg0_banks); i++)
		sw_wr(SDS(fib_reg0_banks[i]),
		      sw_rd(SDS(fib_reg0_banks[i])) & ~FIB_REG0_PDOWN);

	/* 5. Burst-enable output; leave optical-LOS un-forced so the real RX front-
	 *    end drives it (a working unit reaches O5 with FRC_OPTIC_LOS=0). */
	sw_field(WSDS_DIG_18, 12, 12, 1);		/* BEN_OE = 1              */
	sw_field(WSDS_DIG_18, 15, 15, 0);		/* OPTIC_LOS_SEL_EPON = 0  */
	/* Do NOT force optic_los. At O5 WSDS_DIG_18 = 0x1000 (no ...
	 * dev/MEASURED-luna_gpon.c.md sec 289. */
	sw_field(WSDS_DIG_18, 14, 14, 0);		/* CFG_FRC_OPTIC_LOS = 0   */
	sw_field(WSDS_DIG_18, 13, 13, 0);		/* CFG_FRCV_OPTIC_LOS = 0  */

	/* 6. Arm the RX in the required order: enable the RX-CDR ...
	 * dev/MEASURED-luna_gpon.c.md sec 290. */
	sw_field(SDS_ANA_COM_REG12, 14, 14, 0x1);	/* RX_SEL_CDR_AFEN = 1     */
	mdelay(10);
	sw_field(SDS_ANA_MISC_REG01, 7, 5, 0x1);	/* SPDSEL_VAL = GPON rate  */
	sw_field(SDS_ANA_MISC_REG01, 4, 4, 0x1);	/* SPDSEL force on         */
	sw_field(SDS_ANA_MISC_REG00, 4, 4, 0x1);	/* FRC_RX_EN_ON = 1        */
	sw_field(SDS_ANA_MISC_REG00, 5, 5, 0x0);	/* FRC_RX_EN_VAL 0 ...     */
	sw_field(SDS_ANA_MISC_REG00, 5, 5, 0x1);	/* ... -> 1 (start CDR)    */
	mdelay(50);
	sw_field(WSDS_DIG_02, 10, 10, 0x0);		/* EN_PDOWN_BEN = 0        */
	sw_field(WSDS_DIG_03, 6, 4, 0x0);		/* CFG_TXDIS_SEL_DLY = 0: the RTL9602C
							 * burst-mode TX-disable timing requires
							 * 0; 0x2 mis-times the burst TX-disable
							 * -> "Laser out". */
	sw_field(WSDS_DIG_03, 3, 0, 0x0);		/* CFG_D2ANLOG_SEL = 0 (TX data path) */
	/* FORCE_BEN (SDS 0x220e4) BEN_FORCE_MODE[0]=0: let the GTC ...
	 * dev/MEASURED-luna_gpon.c.md sec 291. */
	sw_field(SDS(0x220e4), 0, 0, 0x0);

	/* 6a-ModeV1. US-TX SerDes CMU/PLL and TX-LA-LDO -- the rev-A ...
	 * dev/MEASURED-luna_gpon.c.md sec 117. */
	if (serdes_modev1_tx) {
		sw_wr(SDS(0x22588), 0x6df8);		/* SDS_ANA_COM_REG02 TX CMU/PLL */
		sw_wr(SDS_ANA_COM_REG03, 0x8941);	/* SDS_ANA_COM_REG03 TX CMU (0x2258c) */
		sw_wr(SDS_ANA_COM_REG08, 0x0713);		/* SDS_ANA_COM_REG08 */
		sw_wr(SDS(0x225e4), 0x001f);		/* SDS_ANA_COM_REG25 */
		sw_wr(SDS(0x225e0), 0x8001);		/* SDS_ANA_COM_REG24 REG_TXLA_LDOEN */
		pr_info("luna-gpon: ModeV1 TX SerDes applied: COM_REG02=0x%04x 03=0x%04x 08=0x%04x 24=0x%04x 25=0x%04x\n",
			sw_rd(SDS(0x22588)) & 0xffff, sw_rd(SDS_ANA_COM_REG03) & 0xffff,
			sw_rd(SDS_ANA_COM_REG08) & 0xffff, sw_rd(SDS(0x225e0)) & 0xffff, sw_rd(SDS(0x225e4)) & 0xffff);
	}

	/* 6b. TX DATA PATH -- route the digital US-framer data into ...
	 * dev/MEASURED-luna_gpon.c.md sec 118. */
	sw_field(SDS(0x220a8), 5, 4, serdes_tx_xtra ? 0x3 : 0x0);	/* WSDS_DIG_1E D2A interconnect (stock=0) */
	sw_field(SDS(0x2281c), 14, 14, serdes_tx_xtra ? 0x1 : 0x0);	/* SDS_REG7 SP_CFG_NEG_CLKWR_A2D (stock=0) */
	sw_field(SDS(0x22a30), 8, 8, serdes_tx_xtra ? 0x1 : 0x0);	/* SDS_EXT_REG12 SEP_CFG_NEG_CLKRD_D2A (stock=0) */

	/* TX drive level: REG_TX_AMP=0x5, REG_TX_EMP=0x1, the rev-A ...
	 * dev/MEASURED-luna_gpon.c.md sec 119. */
	sw_field(SDS(0x225d8), 5, 3, 0x5);			/* SDS_ANA_COM_REG22 REG_TX_AMP = 0x5 */
	sw_field(SDS(0x225d8), 2, 0, 0x1);			/* SDS_ANA_COM_REG22 REG_TX_EMP = 0x1 */

	/* 7a. Force signal-detect on. RST_DONE is gated by ...
	 * dev/MEASURED-luna_gpon.c.md sec 120. */
	sw_field(SDS_ANA_MISC_REG02, 13, 13, 0x1);	/* signal-detect value=1   */
	sw_field(SDS_ANA_MISC_REG02, 12, 12, 0x1);	/* force signal-detect     */
	mdelay(10);

	/* 7b. Finally select GPON mode — the very last step, with the RX fully armed
	 *     (CFG_SDS_MODE switches to GPON only here). */
	sw_field(SDS_CFG, 4, 0, SDS_MODE_GPON);
	mdelay(50);

	/* TX-interface reset-B re-sync: with the D2A mux + sample ...
	 * dev/MEASURED-luna_gpon.c.md sec 121. */
	sw_field(WSDS_DIG_1D, 16, 16, 0);
	mdelay(2);
	sw_field(WSDS_DIG_1D, 16, 16, 1);
	mdelay(10);

	/* SerDes CDR-lock pulse -- the stock CDR-reset our init ...
	 * dev/MEASURED-luna_gpon.c.md sec 122. */
	if (serdes_cdr_reset) {
		u32 cdr = sw_rd(SDS_ANA_COM_REG12);

		sw_wr(SDS_ANA_COM_REG12, cdr ^ BIT(15));
		mdelay(10);
		sw_wr(SDS_ANA_COM_REG12, cdr);
		/* ★★ PRINT THE ADDRESS THAT WAS WRITTEN, NOT THE ONE THE 9602C
		 * dev/MEASURED-luna_gpon.c.md sec 123. */
		pr_info("luna-gpon: serdesCdr_reset pulse (COM_REG12 @ 0x%05x bit15), restored=0x%08x\n",
			SDS_ANA_COM_REG12, cdr);
	}

	sw_field(WSDS_DIG_00, 0, 0, 0);			/* keep MAC clock ungated  */

	pr_info("luna-gpon: SDS cfg=0x%08x dig00=0x%08x dig1d=0x%08x fib21=0x%08x fib_reg0=0x%08x\n",
		sw_rd(SDS_CFG), sw_rd(WSDS_DIG_00), sw_rd(WSDS_DIG_1D),
		sw_rd(FIB_EXT_REG21), sw_rd(SDS(0x22c00)));

	for (i = 0; i < SDS_LOCK_POLL_MAX; i++) {
		if (sw_rd(FIB_EXT_REG21) & SDS_ANALOG_READY)
			return 0;
		udelay(200);
	}
	return -ETIMEDOUT;
}

/* Stock rev-A GPON SerDes bring-up ORDER as observed from the ...
 * dev/MEASURED-luna_gpon.c.md sec 124. */
static int __init gpon_serdes_init_stock(void)
{
	/* offsets absent from our #define block - all confirmed via the chip register map */
	const u32 WSDS_DIG_1Eo         = 0x220a8;
	const u32 SDS_REG7o            = 0x2281c;
	const u32 SDS_EXT_REG12o       = 0x22a30;
	const u32 SDS_ANA_COM_REG02o   = 0x22588;
	const u32 SDS_ANA_COM_REG19o   = 0x225cc;
	const u32 SDS_ANA_COM_REG24o   = 0x225e0;
	const u32 SDS_ANA_COM_REG25o   = 0x225e4;
	const u32 SDS_ANA_1P25G_REG46o = 0x226b8;
	const u32 SDS_ANA_EPON_REG46o  = 0x227b8;
	int i;

	/* === stock step 1: serdes mode -> illegal (0x1f) === */
	sw_field(SDS_CFG, 4, 0, SDS_MODE_OFF);		/* CFG_SDS_MODE = 0x1f     */

	/* === stock step 2: no force sds === */
	sw_wr(WSDS_DIG_01, 0);				/* WSDS_DIG_01 = 0         */

	/* === stock step 3: RESET FIRST - pulse ONLY CMD_SDS_RST_PS (bit0).
	 * Stock does NOT touch CMD_SDS_CFG_RST_PS (bit7) here. === */
	sw_field(SW_SOFTWARE_RST, 0, 0, 1);		/* CMD_SDS_RST_PS = 1      */
	mdelay(10);					/* 10ms settle             */

	/* === stock step 4: BEN on === */
	sw_field(WSDS_DIG_18, 12, 12, 1);		/* BEN_OE = 1              */

	/* === stock step 5: rev-A else-branch - adjust
	 * TX_Burst's Burst Mode Sequence === */
	sw_field(WSDS_DIG_03, 6, 4, 0x0);		/* CFG_TXDIS_SEL_DLY = 0   */
	sw_field(WSDS_DIG_03, 3, 0, 0x0);		/* CFG_D2ANLOG_SEL   = 0   */

	/* OUR golden analog overlay + fiber power-on, applied AFTER ...
	 * dev/MEASURED-luna_gpon.c.md sec 125. */
	for (i = 0; i < ARRAY_SIZE(sds_analog_golden); i++)
		sw_wr(SDS(sds_analog_golden[i].off), sds_analog_golden[i].val);
	for (i = 0; i < ARRAY_SIZE(fib_reg0_banks); i++)
		sw_wr(SDS(fib_reg0_banks[i]),
		      sw_rd(SDS(fib_reg0_banks[i])) & ~FIB_REG0_PDOWN);	/* fiber on */

	/* stock step 6: the inlined rev-A bring-up analog config. --- ...
	 * dev/MEASURED-luna_gpon.c.md sec 292. */
	sw_wr(SDS_ANA_COM_REG02o, 0x6df8);	/* COM_REG02 (CMU/LDO/ISTANK) */
	sw_wr(SDS_ANA_COM_REG03, 0x8941);	/* COM_REG03 (0x2258c)        */
	sw_wr(SDS_ANA_COM_REG08, 0x0713);	/* COM_REG08 (0x225a0)        */
	sw_wr(SDS_ANA_COM_REG25o, 0x1f);	/* COM_REG25                  */

	/* WSDS_DIG_1E[5]=CFG_ANALOG2D_SEL, [4]=CFG_D2ANLOG_INF_SEL - stock SETs
	 * these =1 (opposite of our oracle-parity path which clears them). */
	sw_field(WSDS_DIG_1Eo, 5, 5, 0x1);	/* CFG_ANALOG2D_SEL = 1       */
	sw_field(WSDS_DIG_1Eo, 4, 4, 0x1);	/* CFG_D2ANLOG_INF_SEL = 1    */

	sw_wr(SDS_ANA_COM_REG24o, 0x8001);	/* COM_REG24 (REG_TXLA_LDOEN) */

	/* --- ### RX ### (ASIC, !FPGA) --- */
	sw_wr(SDS_ANA_1P25G_REG46o, 0x80c5);	/* FIB1G Rx 1.25 KP/KI        */
	sw_wr(SDS_ANA_EPON_REG46o,  0x80c5);	/* EPON  Rx 1.25 KP/KI        */
	sw_wr(SDS_ANA_GPON_REG46,   0x80c5);	/* GPON  Rx 2.488 KP/KI       */

	sw_field(SDS_ANA_COM_REG12, 14, 14, 0x1);	/* REG_RX_SEL_CDR_AFEN = 1   */
	sw_field(SDS_ANA_COM_REG11,  7,  0, 0x0);	/* REG_RX_FILT_CONFIG = 0    */
	sw_field(SDS_ANA_COM_REG19o, 14, 14, 0x1);	/* REG_CDR_RESET_MANUAL = 1  */
	sw_field(SDS_ANA_COM_REG19o, 10, 10, 0x1);	/* REG_CDR_EN_LPF_MANUAL = 1 */

	/* ### RX_EN toggle ### MISC_REG00: FRC_RX_EN_ON[4], FRC_RX_EN_VAL[5] */
	sw_wr(SDS_ANA_MISC_REG00, 0x10);	/* RX_EN force on, val=0       */
	sw_wr(SDS_ANA_MISC_REG00, 0x30);	/* RX_EN val 0->1 (start CDR)  */

	mdelay(50);				/* 50ms settle                 */

	sw_field(WSDS_DIG_02, 10, 10, 0x0);	/* REG_EN_PDOWN_BEN = 0        */

	/* TX-data sample clocks - stock SETs these =1. */
	sw_field(SDS_REG7o,      14, 14, 0x1);	/* SP_CFG_NEG_CLKWR_A2D = 1   */
	sw_field(SDS_EXT_REG12o,  8,  8, 0x1);	/* SEP_CFG_NEG_CLKRD_D2A = 1  */
	/* === end ModeV1 analog config === */

	/* === stock step 7: serdes mode -> GPON (0x8) === */
	sw_field(SDS_CFG, 4, 0, SDS_MODE_GPON);		/* CFG_SDS_MODE = 0x8      */

	/* === stock step 8 (!FPGA): force ber notify [13:12] = 0x3 === */
	sw_field(SDS_ANA_MISC_REG02, 13, 13, 0x1);	/* FRC_BER_NOTIFY_VAL = 1  */
	sw_field(SDS_ANA_MISC_REG02, 12, 12, 0x1);	/* FRC_BER_NOTIFY_ON  = 1  */

	pr_info("luna-gpon: stock rev-A SerDes init done: SDS_CFG=0x%08x DIG18=0x%08x MISC02=0x%08x fib21=0x%08x\n",
		sw_rd(SDS_CFG), sw_rd(WSDS_DIG_18), sw_rd(SDS_ANA_MISC_REG02),
		sw_rd(FIB_EXT_REG21));

	/* Local readiness gate (not part of the stock sequence) - mirrors gpon_serdes_init tail. */
	for (i = 0; i < SDS_LOCK_POLL_MAX; i++) {
		if (sw_rd(FIB_EXT_REG21) & SDS_ANALOG_READY)
			return 0;
		udelay(200);
	}
	return -ETIMEDOUT;
}

/*
 * Wait (bounded) for the GPON MAC to report RST_DONE after the SerDes sequence
 * has issued the SDS+MAC reset. Returns 0 on RST_DONE, -ETIMEDOUT otherwise.
 */
static int __init gpon_wait_rst_done(void)
{
	int i;

	for (i = 0; i < GPON_RST_POLL_MAX; i++) {
		if (gpon_rd(GPON_RESET) & GPON_RST_DONE)
			return 0;
		udelay(10);
	}
	return -ETIMEDOUT;
}

/* Faithful port of the stock all-module GPON datapath ...
 * dev/MEASURED-luna_gpon.c.md sec 126. */
static unsigned int full_datapath_init = 1;
module_param(full_datapath_init, uint, 0644);
MODULE_PARM_DESC(full_datapath_init, "1=run the full datapath init on the quiescent switch (default), 0=legacy minimal init");
static unsigned int table_engine_ok;	/* 0: the operational VLAN setup (our Ethernet driver) owns the VLAN table now; isolate the VLAN_CTRL=0x19 enable test */
module_param(table_engine_ok, uint, 0644);
MODULE_PARM_DESC(table_engine_ok, "1=also run the L2 LUT clear + VLAN-enable table-engine steps (risky), 0=field-writes only");
/* "harmful write" hypothesis: stock NEVER writes the US-NIC ...
 * dev/MEASURED-luna_gpon.c.md sec 127. */
static unsigned int usnic_strip;	/* 0 = KEEP the US-NIC writes: live-stock same-board diff shows stock HAS
					 * MOCIR 0x2170/0x2174=0x1ffff etc. set, so stripping them DIVERGED from
					 * stock (the "stock never writes these" hypothesis was wrong; live = truth). */
module_param(usnic_strip, uint, 0644);
MODULE_PARM_DESC(usnic_strip, "1=skip the speculative non-stock US-NIC writes (strip test), 0=keep them (match stock)");
/* Faithful port of the stock PON-MAC GPON mode-set branch as ...
 * dev/MEASURED-luna_gpon.c.md sec 293. */
static unsigned int ponmac_modeset = 1;	/* MUST be 1: the classify block and GMII
					 * re-latch at O5 are what make the GEM-US engine latch
					 * the GEM_US_PORT_MAP write. Without it the engine latches
					 * at boot, before gpon_install_omcc writes the port map,
					 * and no OMCI data reaches the US GEM. */
module_param(ponmac_modeset, uint, 0644);
MODULE_PARM_DESC(ponmac_modeset, "1=stock-ordered GPON mode_set classify block + GMII re-latch at O5");

/* finding 3 (2026-06-13): stock fires the US-NIC GMII_RX_EN ...
 * dev/MEASURED-luna_gpon.c.md sec 128. */
static unsigned int relatch_us = 1;
module_param(relatch_us, uint, 0644);
MODULE_PARM_DESC(relatch_us, "1=re-pulse US-NIC GMII_RX_EN edge after the scheduler binding (finding 3)");
static unsigned int serdes_recommit;	/* 0 = OFF: a full SDS re-commit (CMD_SDS_RST_PS) at O5 DROPS the locked DS framer (omcirx->0, DS dead) — DISPROVEN destructive. */
module_param(serdes_recommit, uint, 0644);
MODULE_PARM_DESC(serdes_recommit, "1=also re-commit SerDes inside the O5 mode_set block (risky; gate separately)");
/* PON_GEN_PIR_DROP (bit18 of PON_SCH_CTRL 0x2194). Default 0 ...
 * dev/MEASURED-luna_gpon.c.md sec 129. */
static unsigned int pir_drop;
module_param(pir_drop, uint, 0644);
MODULE_PARM_DESC(pir_drop, "PON_GEN_PIR_DROP bit18@0x2194: 0=rev-A erratum clear (default, drains T-CONT16), 1=set");

/* sch_ctrl_stock: write PON_SCH_CTRL to the EXACT value ...
 * dev/MEASURED-luna_gpon.c.md sec 130. */
static bool sch_ctrl_stock = true;
module_param(sch_ctrl_stock, bool, 0644);
MODULE_PARM_DESC(sch_ctrl_stock, "1=write PON_SCH_CTRL 0x2194=0x66000 verbatim from live stock (PIR_DROP+METER_OP+WFQ_BURSTSIZE); 0=legacy bit18-only (default on)");
/* DPRU_RPT_PRD (0x2568): DBA_BLKSIZE=48 (byte->block divisor the HW uses to encode the
 * DBRu queued-occupancy report the OLT reads to size grants). Stock writes 0x3002 once at
 * init; ours had regressed this write out -> the OLT reads 0 queued despite qid64 holding
 * pages -> grants once then stops -> gemus_omcc=0. Default on; A/B via luna_gpon.dbru_blksize=0. */
static bool dbru_blksize = true;
module_param(dbru_blksize, bool, 0644);
MODULE_PARM_DESC(dbru_blksize, "1=write DPRU_RPT_PRD 0x2568=0x3002 (DBA_BLKSIZE=48, DBRu report divisor; default on)");
/* Re-issue SIDVALID after the T-CONT queue-add arm -- the ...
 * dev/MEASURED-luna_gpon.c.md sec 131. */
static bool sidvalid_last = true;
module_param(sidvalid_last, bool, 0644);
MODULE_PARM_DESC(sidvalid_last, "1=re-issue SIDVALID[64] after the T-CONT-16 arm (stock queue_add tail does this; we had omitted it) (default on)");
/* Bind the OMCC Alloc-ID to a SECOND T-CONT as well. ... -- dev/MEASURED-luna_gpon.c.md sec 294. */
static bool omcc_alt_bind;
module_param(omcc_alt_bind, bool, 0644);
MODULE_PARM_DESC(omcc_alt_bind, "1=also bind the OMCC alloc to T-CONT 1 (non-stock double-bind that makes the DBRu report the empty T-CONT; default off = stock one-T-CONT-per-alloc)");
/* Re-assert AUTO_PROC_SSTART (US_PROC_MODE 0x5200 bit0) at ...
 * dev/MEASURED-luna_gpon.c.md sec 132. */
static bool o5_sstart = true;
module_param(o5_sstart, bool, 0644);
MODULE_PARM_DESC(o5_sstart, "1=re-assert AUTO_PROC_SSTART (0x5200 bit0) at O5 so the HW starts the US burst on each grant (default on)");
/* Re-arm the GEM-US US-feed run-state with the FULL WSDS ...
 * dev/MEASURED-luna_gpon.c.md sec 295. */
static bool o3_feed_reset;
module_param(o3_feed_reset, bool, 0644);
MODULE_PARM_DESC(o3_feed_reset, "1=pulse WSDS GPON datapath reset-B + light feed re-arm after the O3 TX-PLL relock to un-park the GEM-US framer (default off; risks DS lock)");
/* Per-tick US-feed re-arm at O5. DEFAULT OFF since ... -- dev/MEASURED-luna_gpon.c.md sec 133. */
static bool feed_rekick;
module_param(feed_rekick, bool, 0644);
MODULE_PARM_DESC(feed_rekick, "1=per-tick US-feed FIFO re-arm at O5 (DEFAULT OFF: the per-tick GMII edge truncates in-flight US bursts + kills sustained WAN data; opt-in diagnostic only)");
/* DIAGNOSTIC bisection: force the GTC framer to emit idle-GEM on grant windows
 * (FS_GEM_IDLE 0x6020 bit31=1), independent of the page feed. If idle16 then climbs, the
 * framer IS receiving T-CONT16 grants (fault is US-feed starvation -> feed_rekick). If
 * idle16 STAYS 0, grants never reach the framer (scheduling/drain dead). Stock=0; test only. */
static bool force_idle;
module_param(force_idle, bool, 0644);
MODULE_PARM_DESC(force_idle, "1=set FS_GEM_IDLE (0x6020 bit31) to force idle-GEM on grants -- bisection diagnostic (default off, stock=0)");

/* us_intr_svc: service (read-to-clear) the upstream GPON ...
 * dev/MEASURED-luna_gpon.c.md sec 134. */
static bool us_intr_svc;
module_param(us_intr_svc, bool, 0644);
MODULE_PARM_DESC(us_intr_svc, "1=read-to-clear the US GPON interrupt deltas (0x5000/0x5008/0x6000/0x6008) each O5 tick (default off)");

/* swcore TBL_ACCESS engine (L2/VLAN tables) */
#define TBL_BUSY_BIT	(1u << 13)
/* The bound, named the way LUNA_SMI_TRIES and L3FE_ACCESS_TRIES already are
 * in this tree, because it is now an ARGUMENT to the shared poll and not a
 * number buried in a loop.  Why 2000 and not 0x4000: see tbl_wait(). */
#define TBL_WAIT_TRIES	2000u

static bool tbl_ok = true;

/* The SWCORE table engine's pace for gpon_ind_poll(). 1 us ...
 * dev/MEASURED-luna_gpon.c.md sec 135. */
static void tbl_pause(void)
{
	static unsigned int spins;

	udelay(1);
	if ((++spins & 0xff) == 0)
		cond_resched();
}

static void tbl_wait(void)
{
	/* The budget is 2 ms, not 16 ms, and it cost this board every ...
	 * dev/MEASURED-luna_gpon.c.md sec 136. */
	if (gpon_ind_poll(&sw_io, reg_make(TBL_STS_OFF), TBL_BUSY_BIT,
			  TBL_WAIT_TRIES, tbl_pause) < 0)
		tbl_ok = false;		/* engine wedged / wrong encoding: stop */
}

static void tbl_write(u32 type, u32 addr)
{
	u32 ctrl;

	if (!tbl_ok)
		return;
	tbl_wait();
	if (!tbl_ok)
		return;
	ctrl = sw_rd(TBL_CTRL_OFF);
	ctrl = (ctrl & ~(0x7u << 0))   | ((type & 0x7u) << 0);	/* TBL_TYPE [2:0]   */
	ctrl = (ctrl & ~(0x1u << 3))   | (1u << 3);		/* CMD_TYPE=write   */
	ctrl = (ctrl & ~(0x7u << 4))   | (1u << 4);		/* ACCESS_METHOD=1  */
	ctrl = (ctrl & ~(0xfffu << 9)) | ((addr & 0xfffu) << 9);/* ADDR [20:9]      */
	sw_wr(TBL_CTRL_OFF, ctrl);
	tbl_wait();
}

void rtl9602c_datapath_tables_init(void)
{
	/* ⚠ THIS POINTER WAS CALLED `ipsel`, WHICH IS ANOTHER REGISTER'S NAME.
	 * SOC_IP_SEL is 0xb8000600; this is 0xb800063c, and the two gate
	 * different things. Renamed the day it was proven wrong. */
	void __iomem *sw_en = SOC_SW_ENABLE;
	int port, idx;

	/* This is EXPORTED and the caller is another driver, so it ...
	 * dev/MEASURED-luna_gpon.c.md sec 137. */
	if (!swcore_base) {
		pr_warn_once("luna-gpon: datapath_tables_init called before this driver probed (swcore_base is NULL) -- skipped; the switch fabric is NOT initialised\n");
		return;
	}

	if (!full_datapath_init)
		return;

	/* 1) switch_init -------------------------------------------------- */
	writel(readl(sw_en) | SW_EN_BIT, sw_en);	/* see the naming note in
							 * luna_eth_regs.h    */
	pi_field(PI_PON_TB_CTRL, 7, 0, 0x6e);			/* PON_TB_CTRL tick      */
	pi_field(PI_PON_TB_CTRL, 15, 8, 0x95);
	/* METER_TB_CTRL and PON_TB_CTRL have MIRRORED layouts, and this site had
	 * copied the line above onto a register where the two fields swap places:
	 * PON_TB_CTRL puts TICK_PERIOD at [7:0] and TKN at [15:8], METER_TB_CTRL
	 * puts TKN at [7:0] and TICK_PERIOD at [15:8] (the chipdef's own layout,
	 * structurally identical on 9607C/9601B/9603CVD).  So the bucket refilled
	 * 43 tokens every 189 ticks where stock refills 189 every 43 -- 19x
	 * slower, on a meter both firmwares leave ENABLED (METER_OP=1).
	 * MEASURED on the X111W: live stock reads 0x00012bbd and this board read
	 * 0x0001bd2b, the same nibbles transposed.  rtl9602c_eth.c already wrote
	 * stock's word; this site ran later and overwrote it. */
	sw_field(SW_METER_TB_CTRL, 7, 0, 189);			/* TKN         */
	sw_field(SW_METER_TB_CTRL, 15, 8, 43);			/* TICK_PERIOD */
	sw_field(SW_SCH_WFQ_TKN_CTRL, 0, 0, 1);		/* SCH_WFQ_TKN_CTRL      */
	sw_field(SW_LINE_RATE_2500M, 18, 0, 0x3ffff);	/* LINE_RATE_2500M       */
	sw_field(WRAP_GPHY_MISC, 0, 0, 1);			/* PATCH_PHY_DONE        */
	sw_field(CFG_UNHIOL, 0, 0, 1);			/* CFG_UNHIOL IPG_COMP   */
	sw_field(SW_P_MISC_PORT(swc->sw, swc->sw->cpu_port), 2, 2, 1);	/* P_MISC[CPU] RX_SPC */
	/* The ACCEPT_MAX_LEN half is closed, and only on the die that ...
	 * dev/MEASURED-luna_gpon.c.md sec 138. */
	for (port = 0; port <= swc->amax_last_port; port++)
		sw_field(ACCEPT_MAX_LEN_CTRL + port * 4, swc->amax_msb, 0,
			 (swc->amax_pon_val && port == swc->sw->pon_port)
			 ? swc->amax_pon_val : swc->amax_val); /* ACCEPT_MAX_LEN */
	/* the PON port when it lies BEYOND this die's covered range -- the
	 * RTL9607C's is port 5 and its loop stops at 3. */
	if (swc->amax_pon_val && swc->sw->pon_port > swc->amax_last_port)
		sw_field(ACCEPT_MAX_LEN_CTRL + swc->sw->pon_port * 4,
			 swc->amax_msb, 0, swc->amax_pon_val);

	/* 2) l2_init: per-port action defaults (FORWARD). NOTE: a ...
	 * dev/MEASURED-luna_gpon.c.md sec 296. */
	sw_field(SW_LUT_CFG, 22, 22, 1);			/* LUT LINKDOWN_AGEOUT   */
	/* ★★★ THE BINARY CAM, WHICH WE WERE LEAVING DISABLED BY INHERITANCE.
	 * sw_field() is a read-modify-write that names ONE bit, so every bit this
	 * init never mentions keeps whatever reset or U-Boot left -- and BCAM_DIS
	 * arrives SET. Stock clears it: a same-day differential on this board reads
	 * LUT_CFG 14800bb8 on stock against 00600bb8 on ours, and bit 21 is
	 * BCAM_DIS in this die's own chipdef.
	 * ⚠ IT IS A LOOKUP-STAGE BIT, which is why it is tried now and not sooner:
	 * with the die's log armed at LOG_FIRST_DROP a full transit latched NOTHING,
	 * so the engine is not looking these frames up and discarding them -- it is
	 * not looking them up at all, and a disabled search CAM is a mechanism for
	 * exactly that. */
	sw_field(SW_LUT_CFG, 21, 21, 0);			/* BCAM enabled, as stock */
	for (port = 0; port <= 3; port++) {
		sw_field(LUT_LEARN_OVER_CTRL, port * 2 + 1, port * 2, 0);
		sw_field(SW_LUT_AGEOUT_CTRL, port, port, 1);
		sw_field(SW_LUT_UNKN_SA_CTRL, port * 2 + 1, port * 2, 0);
		sw_field(SW_LUT_UNMATCHED_SA_CTRL, port * 2 + 1, port * 2, 0);
		sw_field(UNKN_IP4_MC, port * 2 + 1, port * 2, 0);
		sw_field(UNKN_L2_MC, port * 2 + 1, port * 2, 0);
		sw_field(LUT_UNKN_UC_DA_CTRL, port * 2 + 1, port * 2, 0);
		sw_field(LUT_SYS_LRN_LIMIT, port, port, 1);
	}
	for (idx = 0; idx < 0x200 && tbl_ok && table_engine_ok; idx++) {
		sw_wr(TBL_WRDATA_OFF + 0x0, 0);
		sw_wr(TBL_WRDATA_OFF + 0x4, 0);
		tbl_write(/*L2_UC*/ 0, idx);
	}

	/* 3) vlan_init: default VLAN-1 (all ports member) — only ENABLE
	 *    filtering if the member entry actually wrote (else keep VLAN off,
	 *    our working baseline). Risky table-engine path: gated by table_engine_ok. */
	for (port = 0; port <= 3; port++) {
		sw_field(SW_VLAN_PORT_ACCEPT_FRAME_TYPE, port * 2 + 1, port * 2, 0);	/* ACCEPT ALL    */
		sw_field(SW_VLAN_EGRESS_TAG + 4 * port, 1, 0, 0);		/* EGRESS ORIG   */
	}
	if (table_engine_ok) {
		tbl_ok = true;				/* retry engine for VLAN  */
		sw_wr(TBL_WRDATA_OFF + 0x0, (0xfu << 4) | 0xfu);  /* untag|mbr=all */
		sw_wr(TBL_WRDATA_OFF + 0x4, 0x7f);
		tbl_write(/*VLAN*/ 1, /*vid*/ 1);
		if (tbl_ok) {
			for (port = 0; port <= 3; port++)
				sw_field(SW_VLAN_INGRESS, port, port, 1);	/* VLAN_INGRESS  */
			/* VLAN_FILTER ON for ranging/config (reliable onlining); the FSM auto-clears
			 * it once stably at O5 to open LAN access (see vlan_lan_open in gpon_fsm_poll).
			 * lan_keep_open (default) keeps LAN open from boot -> never assert the filter,
			 * so a bad cold-start (no O5) or a WAN-disconnect can't kill LAN management. */
			if (!lan_keep_open)
				sw_field(SW_VLAN_CTRL, 0, 0, 1);	/* VLAN_FILTER on (config phase) */
			sw_field(SW_VLAN_CTRL, 4, 4, 0);
		}
	}

	/* 4) port_init: CPU + PON force link UP (already done in swcore bringup;
	 *    re-assert for stock fidelity/order). Ports 0,1 stay auto. */
	sw_field(FORCE_P_ABLTY + 3 * 4, 1, 0, 2); sw_field(FORCE_P_ABLTY + 3 * 4, 2, 2, 1);
	sw_field(FORCE_P_ABLTY + 3 * 4, 4, 4, 1); sw_wr(ABLTY_FORCE_MODE + 3 * 4, 0xfff);
	sw_field(FORCE_P_ABLTY + 2 * 4, 1, 0, 2); sw_field(FORCE_P_ABLTY + 2 * 4, 2, 2, 1);
	sw_field(FORCE_P_ABLTY + 2 * 4, 4, 4, 1); sw_wr(ABLTY_FORCE_MODE + 2 * 4, 0xfff);

	/* 5) cpu_init: TAG_AWARE AFTER the CPU port is forced link-up. */
	sw_field(SW_MAC_CPU_TAG_CTRL, 8, 8, 1);		/* TRAP_TAGET_INSERT_EN  */
	sw_field(SW_MAC_CPU_TAG_CTRL, 9, 9, 1);		/* TAG_AWARE             */

	/* 6) trap_init: RMA baseline */
	sw_field(RMA_CFG, 2, 0, 0);
	sw_field(UNKN_MC_CFG, 2, 0, 0);
	sw_field(RMA_CTRL01, 5, 4, 2);
	sw_field(RMA_CTRL02, 5, 4, 2);
	sw_field(OAM_CTRL_0, 2, 0, 0);
	sw_field(QOS_UNI_TRAP_PRI_CTRL, 0, 0, 0);

	/* 7) classification setup: CF_CFG = the EXACT ...
	 * dev/MEASURED-luna_gpon.c.md sec 139. */
	sw_wr(CF_CFG, 0x0001d009u);
	/* Port isolation + BUM-flood masks to the live-stock values ...
	 * dev/MEASURED-luna_gpon.c.md sec 140. */
	sw_wr(SW_PISO_PORT + 0 * SW_PISO_PORT_STRIDE, 0x000ff9ffu);
	sw_wr(SW_PISO_PORT + 1 * SW_PISO_PORT_STRIDE, 0x000ff9ffu);
	sw_wr(SW_PISO_PORT + 2 * SW_PISO_PORT_STRIDE, 0x000ff9ffu);
	sw_wr(SW_PISO_PORT + 3 * SW_PISO_PORT_STRIDE, 0x000ff9ffu);
	sw_wr(LUT_BC_FLOOD, 0x00000008u); sw_wr(LUT_UNKN_MC_FLOOD, 0x00000008u); sw_wr(LUT_UNKN_UC_FLOOD, 0x00000008u);

	/* 8) ponmac_init: PON-IP scheduler + OMCI egress steering */
	sw_field(DYNGASP_CTRL, 3, 3, 1);			/* DYNGASP_CMP_INV, bit 3 on all three dies */
	if (serdes_stock_analog) {
		/* Match live-stock post-reset SDS_ANA: REG01 (0x22584)=0x73a4 ...
		 * dev/MEASURED-luna_gpon.c.md sec 141. */
		sw_field(SDS(0x22584), 14, 14, 1);		/* REG01 REG_BEN_TTL_OUT = 1 (stock 0x73a4) */
		sw_field(SDS(0x22584),  0,  0, 0);		/* REG01 BENLA_LDOVREF[0] = 0 (stock) */
		sw_field(SDS(0x225ac),  7,  0, 0);		/* REG11 RX_FILT_CONFIG = 0 (stock)   */
	} else {
		sw_field(SDS(0x22584), 0, 0, 1);		/* legacy: BENLA_LDOVREF[0] = 1       */
	}
	pi_field(PI_PON_BW_THRES, 29, 16, 5);			/* PON_BW_THRES last     */
	pi_field(PI_PON_BW_THRES, 13, 0, 5);			/* PON_BW_THRES runt     */
	if (sch_ctrl_stock)
		/* Match LIVE STOCK exactly: PIR_DROP(b18)=1 | METER_OP(b17)=1 |
		 * WFQ_BURSTSIZE[15:0]=0x6000. See sch_ctrl_stock param comment. */
		pi_wr(PI_PON_SCH_CTRL, 0x00066000u);
	else
		pi_field(PI_PON_SCH_CTRL, 18, 18, pir_drop ? 1 : 0);	/* legacy bit18-only A/B */
	for (idx = 0; idx < 8; idx++) {
		pi_packed_set(PI_PON_WFQ_TYPE, idx, 1, 0);	/* WFQ_TYPE = STRICT (1b packed) */
		pi_field(PI_PON_QID_CIR_RATE + idx * PI_QID_RATE_STRIDE, 31, 0, 0);	/* QID_CIR_RATE = 0      */
	}
	sw_field(PON_TRAP_CFG, 2, 0, 7);			/* PON_TRAP_CFG OMCI_MPCP_PRIORITY=7 -> steer OMCI egress to PON queue 7 */
	/* PIR_DROP (0x2194 bit18) is cleared for rev-A above ...
	 * dev/MEASURED-luna_gpon.c.md sec 297. */
	sw_field(SW_P_MISC_PORT(swc->sw, swc->sw->pon_port), 2, 2, 1);	/* P_MISC[PON] RX_SPC */
	sw_field(SW_P_MISC_PORT(swc->sw, swc->sw->cpu_port), 2, 2, 1);	/* P_MISC[CPU] RX_SPC */

	pr_info("luna-gpon: datapath_tables_init done (tbl_ok=%d)\n", tbl_ok);
}
EXPORT_SYMBOL(rtl9602c_datapath_tables_init);

/* Configure the PON packet datapath (PON-IP) for GPON before ...
 * dev/MEASURED-luna_gpon.c.md sec 142. */
/* The US and DS packet pools may be RESERVED by the board (DT memory-region,
 * US then DS) instead of allocated: the 32 MB X100DG's stock keeps both at the
 * two holes of its memory map (MSTBASE_US 0x01eff000, MSTBASE_DS 0x016ff000,
 * read on stock 2026-09-30), and taking 1-2 MB from its 19 MB starved it. */
static u32 pbo_region[2];

static void luna_pbo_regions_from_dt(struct device_node *np)
{
	struct resource r;
	int i;

	for (i = 0; i < 2; i++) {
		struct device_node *rn = of_parse_phandle(np, "memory-region", i);

		if (rn && !of_address_to_resource(rn, 0, &r) &&
		    resource_size(&r) >= (PAGE_SIZE << PI_US_DRAM_ORDER))
			pbo_region[i] = (u32)r.start;
		else if (rn)
			pr_err("luna-gpon: memory-region %d is not a %lu-byte pool -- allocating instead\n",
			       i, PAGE_SIZE << PI_US_DRAM_ORDER);
		of_node_put(rn);
	}
	if (pbo_region[0] || pbo_region[1])
		pr_info("luna-gpon: PBO pools from DT: US 0x%08x DS 0x%08x\n",
			pbo_region[0], pbo_region[1]);
}

void gpon_pbo_init(void)
{
	bool keep_pool = READ_ONCE(luna_activation_ready) &&
		luna_ploam.state == GPON_O5_OPERATION;

	/* Exported like datapath_tables_init, and reached from the NIC's open on a
	 * board whose DT disables the GPON: X100DG 2026-09-30, NULL ponip_base,
	 * Oops at pi_hwio_rd inside rtl9602c_eth_open. */
	if (!ponip_base) {
		pr_warn_once("luna-gpon: gpon_pbo_init called before this driver probed (ponip_base is NULL) -- skipped\n");
		return;
	}
	luna_data_suspend();
	/* O5 GEM requests still consume these pools while the GMAC is reset. */
	if (!keep_pool) {
		/* Stop both GMII directions before changing either pool. */
		pi_field(PI_IO_CMD_0_US, 5, 4, 0);
		pi_field(PI_IO_CMD_0_DS, 5, 4, 0);
		pi_field(PI_PONIP_CTL_US, 0, 0, 0);		/* CFG_PBUF_EN = 0        */
		pi_field(PI_PONIP_CTL_DS, 0, 0, 0);

		/* 2. Descriptor accounting (128B pages). US uses a DRAM ...
		 * dev/MEASURED-luna_gpon.c.md sec 144. */
		{
			static unsigned long us_pool;

			if (!us_pool && !pbo_region[0])
				us_pool = __get_free_pages(GFP_KERNEL, PI_US_DRAM_ORDER);
			if (pbo_region[0])
				pi_wr(PI_IP_MSTBASE_US, pbo_region[0]);
			else if (us_pool)
				pi_wr(PI_IP_MSTBASE_US,
				      (u32)virt_to_phys((void *)us_pool));
			else
				pr_warn("luna-gpon: US PBO DRAM pool alloc failed; US-NIC RX may not work\n");
		}
		pi_field(PI_PON_DSC_CFG_US, 12, 0, PI_US_SRAM_NO);
		pi_field(PI_PON_DSC_CFG_US, 28, 16, PI_US_DRAM_PAGES);	/* RAM_NO = SRAM+DRAM (0x1fff) */
		pi_field(PI_DSCRUNOUT_US, 12, 0, PI_US_SRAM_RUNOUT);
		pi_field(PI_DSCRUNOUT_US, 28, 16, PI_US_DRAM_RUNOUT);	/* DRAM runout (0x1f58) */
		/* The DS twin. A chip that declares no DS pool keeps the SRAM-only words
		 * this driver has always written -- same fields, same values, same order. */
		if (!swc->ds_dram_order && !pbo_region[1]) {
			pi_field(PI_PON_DSC_CFG_DS, 12, 0, PI_DS_SRAM_NO);
			pi_field(PI_PON_DSC_CFG_DS, 28, 16, PI_DS_SRAM_NO);
			pi_field(PI_DSCRUNOUT_DS, 12, 0, PI_DS_SRAM_RUNOUT);
			pi_field(PI_DSCRUNOUT_DS, 28, 16, 0);
		} else {
			static unsigned long ds_pool;

			if (!ds_pool && !pbo_region[1])
				ds_pool = __get_free_pages(GFP_KERNEL, swc->ds_dram_order);
			if (pbo_region[1] || ds_pool) {
				pi_wr(PI_IP_MSTBASE_DS, pbo_region[1] ? pbo_region[1] :
				      (u32)virt_to_phys((void *)ds_pool));
				pi_wr(PI_PON_DSC_CFG_DS, swc->ds_dsc_cfg);
				pi_wr(PI_DSCRUNOUT_DS, swc->ds_dscrunout);
			} else {
				/* ★ NO POOL ⇒ KEEP THE SMALL SRAM GEOMETRY. Writing the
				 * DRAM-scale page counts with no pool behind them would
				 * point the PBO at physical address 0. */
				pr_warn("luna-gpon: DS PBO DRAM pool alloc failed; DS stays SRAM-only and DS OMCI may be dropped\n");
				pi_field(PI_PON_DSC_CFG_DS, 12, 0, PI_DS_SRAM_NO);
				pi_field(PI_PON_DSC_CFG_DS, 28, 16, PI_DS_SRAM_NO);
				pi_field(PI_DSCRUNOUT_DS, 12, 0, PI_DS_SRAM_RUNOUT);
				pi_field(PI_DSCRUNOUT_DS, 28, 16, 0);
			}
		}
	}

	/* PBO backpressure thresholds (SRAM-only, 128B pages). ...
	 * dev/MEASURED-luna_gpon.c.md sec 145. */
	pi_field(PI_PON_SID_STOP_TH, 12, 0, 0x1f30);
	pi_field(PI_PON_SID_GLB_TH, 28, 16, 0x1ee0);
	pi_field(PI_PON_SID_GLB_TH, 12, 0, 0x1e40);
	/* Per-SID reserved-page thresholds, 150/130 -- the vendor's own values
	 * for this init (tier 3, rtl9603cvd_raw_pbo_memUsage_init).  THE BOUND
	 * IS THE CHIP'S ARRAY, NOT THE CHIP'S SID COUNT: see sid_rpv_entries.
	 * A chip that declares none (no PON-IP block) writes none. */
	{
		unsigned int sid;

		for (sid = 0; sid < swc->sid_rpv_entries; sid++) {
			u32 off = PI_PON_SID_RPV_TH + sid * PI_RPV_TH_STRIDE;

			pi_field(off, 28, 16, 150);
			pi_field(off, 12, 0, 130);
		}
	}
	/* DS flow-control thresholds: on the SRAM-only geometry they are 2/22
	 * pages; a chip running the DRAM pool needs its own stock's scale, or the
	 * PBO sits permanently over-threshold against an 8192-page pool. */
	if ((swc->ds_dram_order || pbo_region[1]) && swc->ds_fc_config)
		pi_wr(PI_PON_FC_CONFIG_DS, swc->ds_fc_config);
	else {
		pi_field(PI_PON_FC_CONFIG_DS, 12, 0, 22);
		pi_field(PI_PON_FC_CONFIG_DS, 28, 16, 2);
	}

	/* 3. GPON mode (not EPON) + upstream RXC stop + US FIFO thresholds. */
	pi_field(PI_PONIP_CTL_US, 2, 2, 0);		/* CFG_EPON_MODE = 0      */
	pi_field(PI_PONIP_CTL_DS, 2, 2, 0);
	pi_field(PI_PONIP_CTL_US, 1, 1, 1);		/* CFG_STOP_RXC_EN = 1    */
	pi_field(PI_PON_US_FIFO_CTL, 5, 4, 1);		/* USFIFO_SPACE = 1       */
	pi_field(PI_PON_US_FIFO_CTL, 3, 0, 3);		/* USFIFO_START = 3       */

	/* 4. 128-byte page size everywhere (PON-IP descriptors + PONNIC pages). */
	if (!keep_pool) {
		pi_field(PI_PON_DSC_CFG_US, 14, 13, 0);
		pi_field(PI_PON_DSC_CFG_DS, 14, 13, 0);
	}
	pi_field(PI_IO_CMD_1_US, 5, 4, 0);		/* RPAGE_SIZE = 128B      */
	pi_field(PI_IO_CMD_1_US, 1, 0, 0);		/* TPAGE_SIZE = 128B      */
	pi_field(PI_IO_CMD_1_DS, 5, 4, 0);
	pi_field(PI_IO_CMD_1_DS, 1, 0, 0);
	pi_field(PI_IO_CMD_1_DS, 27, 27, 1);		/* PRECISE_DMA_EN — DS precise/aligned DMA transfers; the O5 DS IO_CMD_1 value is 0x08000000. Without it the DS RX DMA never lands a frame, so filled stays 0. */

	/* 5. PONNIC datapath: almost-full RX backpressure + TX ...
	 * dev/MEASURED-luna_gpon.c.md sec 146. */
	pi_field(PI_PROBE_SELECT_US, 7, 5, 2);		/* R_DBG_FUNC_SEL=010b => reg 0x40 */
	pi_field(PI_CFG_US, 26, 26, 1);			/* E_EN_RFF_AFULL         */
	pi_field(PI_CFG_US, 17, 17, 1);			/* EN_TX_STOP             */
	pi_field(PI_CFG_US, 16, 16, 1);			/* EN_TXE_EXTRA           */
	/* (0xD400 PROBE_SELECT_DS is set to the stock golden 0x40 at the end of this
	 * function, with 0xD404/0xD42C — the DS-NIC drain config, not a debug probe.) */
	pi_field(PI_CFG_DS, 26, 26, 1);
	pi_field(PI_CFG_DS, 17, 17, 1);
	pi_field(PI_CFG_DS, 16, 16, 1);
	/* CFG_DS[6:0] = RX_SID: the stream-id the DS-NIC STAMPS on every frame it egresses
	 * over the internal MII into GMAC0's GMII-RX. The GMAC's CPUtag1CR SID-64 trap only
	 * fires (and the GMAC only accepts the on-wire cpu-tag) when this SID = 64. Reset
	 * default is 0x40 but the pbo MAC reset can clear it; set it explicitly. */
	pi_field(PI_CFG_DS, 6, 0, GPON_OMCC_FLOW);	/* RX_SID = this chip's OMCC SID */
	/* CFG_US[6:0] = RX_SID for the US-NIC, symmetric to the DS ...
	 * dev/MEASURED-luna_gpon.c.md sec 147. */
	pi_field(PI_CFG_US, 6, 0, GPON_OMCC_FLOW);	/* US-NIC RX_SID = this chip's OMCC SID */

	/* 6. PONNIC TX framing (IFG, preamble, padding) + RX accept-CRC-error. */
	pi_field(PI_TX_CFG_US, 12, 10, 3);		/* IFG                    */
	pi_field(PI_TX_CFG_US, 2, 1, 1);		/* preamble length        */
	pi_field(PI_TX_CFG_US, 0, 0, 1);		/* TX padding             */
	pi_field(PI_RX_CFG_US, 5, 5, 1);		/* accept CRC error       */
	pi_field(PI_TX_CFG_DS, 12, 10, 3);
	pi_field(PI_TX_CFG_DS, 2, 1, 1);
	pi_field(PI_TX_CFG_DS, 0, 0, 1);
	pi_field(PI_RX_CFG_DS, 5, 5, 1);

	/* 7. Enable upstream and downstream packet buffers. (The O5 ...
	 * dev/MEASURED-luna_gpon.c.md sec 148. */
	if (!keep_pool) {
		pi_field(PI_PONIP_CTL_US, 0, 0, READ_ONCE(luna_activation_ready));
		pi_field(PI_PONIP_CTL_DS, 0, 0, 1);
	}
	pi_field(PI_PONIP_CTL_DS, 7, 7, 1);		/* CFG_TX_PAUSE low bit -> O5 value 0x81 (DS buffer release; safe now thresholds bound the buffer) */

	/* 8. Enable the full downstream PONNIC DMA drain (LAST — ...
	 * dev/MEASURED-luna_gpon.c.md sec 149. */
	pi_wr(PI_MEDIA_STS_DS, 0x106e8400u);
	pi_wr(PI_IO_CMD_0_DS, 0x90081070u);

	/* PON-IP DS-NIC drain config. A live stock ONU online and ...
	 * dev/MEASURED-luna_gpon.c.md sec 150. */
	pi_wr(PI_PROBE_SELECT_DS, 0x00000040u);
	pi_wr(PI_DS_NIC_CFG_D404, 0x11100348u);
	pi_wr(PI_CONFIG_CLK_DS, 0x00000040u);

	/* 9. US-NIC GMII enable — the VERY LAST write, after the ...
	 * dev/MEASURED-luna_gpon.c.md sec 151. */
	if (!usnic_strip)
		pi_wr(PI_MEDIA_STS_US, 0x106e8400u);	/* stock NEVER writes 0x1bf04058 */

	/* Pre-arm the OMCC classification BEFORE the GMII_RX_EN latch ...
	 * dev/MEASURED-luna_gpon.c.md sec 152. */
	{
		unsigned int f = GPON_OMCC_FLOW;
		unsigned int q2_lsb = (f % 4u) * 7u;	/* SID2QID: 7 bits, 4/word  */
		unsigned int v_bit  = f % 32u;		/* SIDVALID: 1 bit, 32/word */

		pi_field(PI_PON_SID2QID + (f / 4u) * PI_SID2QID_STRIDE,
			 q2_lsb + 6u, q2_lsb, GPON_OMCC_PHYS_QID & 0x7f);
		pi_field(PI_PON_SIDVALID + (f / 32u) * 4u, v_bit, v_bit, 1);
	}
	/* PON_OMCI_CFG[6:0] = OMCC SID 64 — the THIRD member of the ...
	 * dev/MEASURED-luna_gpon.c.md sec 153. */
	pi_field(PI_PON_OMCI_CFG, 6, 0, GPON_OMCC_FLOW);	/* [6:0] = this chip's OMCC SID */

	/* Data remains inhibited after a global NIC/PBO reset. The accepted OMCI
	 * owner retires any old queue and applies its selected qid on the timer. */
	pi_field(PI_PON_SIDVALID, GPON_DATA_FLOW, GPON_DATA_FLOW, 0);

	/* MOCIR force-mode (stock QoS init): PON-IP 0x2170 ...
	 * dev/MEASURED-luna_gpon.c.md sec 298. */
	if (!usnic_strip) {
		pi_wr(PI_MOCIR_FRC_MD, 0x0001ffffu);	/* MOCIR_FRC_MD  = 0x1FFFF (all flows forced)   */
		pi_wr(PI_MOCIR_FRC_VAL, 0x0001ffffu);	/* MOCIR_FRC_VAL = 0x1FFFF (forced CIR = max)   */
	}

	/* Stock PON-MAC US-NIC datapath credit/threshold cluster, ...
	 * dev/MEASURED-luna_gpon.c.md sec 154. */
	if (!usnic_strip) {
		pi_wr(PI_PON_BW_THRES, 0x00050005u);	/* PON US-NIC IP-status/BW threshold (reg924, 5+5)  */
		pi_wr(PI_PON_US_FIFO_CTL, 0x00000013u);	/* US-NIC datapath cfg                              */
		/* PONIP_DBG_CTRL_US MUST be 0x00086000 to match live stock: ...
		 * dev/MEASURED-luna_gpon.c.md sec 155. */
		pi_wr(PI_PONIP_DBG_CTRL_US, 0x00086000u);	/* stock value (DBG_IGNORE_TAG=1) */
		if (dbru_blksize)
			pi_wr(PI_GPON_DPRU_RPT_PRD, 0x00003002u);	/* DPRU_RPT_PRD: DBA_BLKSIZE=48 plus report period 2,
										 * the live-stock 0x3002. The HW DBA/DBRu engine uses
										 * this byte->block divisor to ENCODE per-SID queued
										 * occupancy into the report the OLT reads to size
										 * grants; unset, the OLT sees 0 queued while the
										 * queue holds pages, grants once and STOPS. Stock's
										 * only writer is its init; ours had regressed it. */
		/* 0x20f4 = PON_IPSTS_US, a READ-ONLY init-ready status reg (bit0=PONIC_INITRDY);
		 * stock never writes it. The old pi_wr(0x20f4,1) was a no-op write to a reserved
		 * bit -> removed. It is POLLED before the GMII latch edge below (usnic_initrdy_poll). */
		pi_wr(PI_MOCIR_TH_H, 0x00001000u);	/* MOCIR_TH_H (request/grant credit threshold)      */
		pi_wr(PI_MOCIR_TH_L, 0x00001000u);	/* MOCIR_TH_L                                       */
		pi_wr(PI_PON_OLT_BW_MTR_FULL, 0x0003ffffu);	/* PON_OLT_BW_MTR_FULL (maxFlow)                    */
		pi_wr(PI_PON_TB_CTRL, 0x0000956eu);	/* PON_TB_CTRL (token bucket)                       */
		if (!keep_pool) {
			pi_wr(PI_PON_SCH_QMAP, 0x0000000fu);
			pi_wr(PI_PON_TCONT_EN, 0x00010001u);
		}
	}

	/* GMAC0<->US-NIC internal link force, from the stock PON-MAC GPON mode-set.
	 * Without it a CPU direct-TX US-OMCI frame never reaches the US-NIC ingress.
	 * Stock releases the force once trained; we hold it up. */
	if (!usnic_strip) {
		sw_field(ABLTY_FORCE_MODE, 4, 0, 0xc);	/* ABLTY_FORCE_MODE[4:0]=0xc — GPON internal-link force   */
		/* RTL9602C only: 0x0f4 is CFG_FE_POLL_WD_1 here and SSC_CTRL_1 on the
		 * RTL9603CVD, which has no front-end poll/watchdog to translate to. */
		if (!is_9603cvd)
			sw_field(0x000f4, 5, 5, 1);	/* CFG_FE_POLL_WD_1[5]=1 — front-end GMII poll/watchdog */
		mdelay(10);			/* settle the forced internal link before PCS enables+edge */
		sw_field(SDS(0x22a70), 11, 11, 0);	/* SDS_EXT_REG28[11]=0 — release non-GPON SerDes-select    */
		/* The two WAN-PCS digital-enable writes below DIVERGE from stock on
		 * purpose: guarded off, the G24W measured optic_los=1 sdet=0 and stayed
		 * at O1 (2026-09-11). A stock-parity sweep must NOT close this gap. */
		sw_field(SDS(0x220e0), 9, 8, 1);	/* WSDS_DIG_2C[9:8]=1 — WAN-PCS digital enable */
		pi_field(PI_RSVD_PONIP_DS, 13, 13, 1);	/* RSVD_PONIP_DS[13]=1 — DS-side datapath enable           */
		pi_field(PI_RSVD_PONIP_DS, 12, 12, 1);	/* RSVD_PONIP_DS[12]=1                                     */
		sw_field(SDS(0x22080), 12, 12, 1);	/* WSDS_DIG_14[12]=1 — WAN-PCS digital enable (see above) */
	}

	/* Wait for the US PON-IP core to report init-ready before the GMII latch edge
	 * below. Stock has no such poll and relies on a fixed mdelay. */
	if (usnic_initrdy_poll) {
		int n;

		for (n = 0; n < SDS_LOCK_POLL_MAX; n++) {
			if (pi_rd(PI_PON_IPSTS_US) & BIT(0))		/* PONIC_INITRDY */
				break;
			udelay(200);
		}
		if (n >= SDS_LOCK_POLL_MAX) {
			pr_warn("luna-gpon: PON_IPSTS_US.PONIC_INITRDY not set after %dus; proceeding\n",
				SDS_LOCK_POLL_MAX * 200);
			if (usnic_initrdy_repulse) {		/* ACTIVE: re-roll the CDR lock */
				u32 cdr = sw_rd(SDS_ANA_COM_REG12);

				sw_wr(SDS_ANA_COM_REG12, cdr ^ BIT(15));
				mdelay(10);
				sw_wr(SDS_ANA_COM_REG12, cdr);
				for (n = 0; n < SDS_LOCK_POLL_MAX; n++) {
					if (pi_rd(PI_PON_IPSTS_US) & BIT(0))
						break;
					udelay(200);
				}
				pr_warn("luna-gpon: PONIC_INITRDY after CDR re-pulse: %s\n",
					(pi_rd(PI_PON_IPSTS_US) & BIT(0)) ? "ready" : "still-not-ready");
			}
		} else {
			pr_info("luna-gpon: PON_IPSTS_US.PONIC_INITRDY ready after %dus\n", n * 200);
		}
	}

	pi_wr(PI_IO_CMD_0_US, 0x90101070u);
}
EXPORT_SYMBOL(gpon_pbo_init);	/* re-run from rtl9602c_eth_open() after the GMAC reset */

static bool datapath_rearm = true;
module_param(datapath_rearm, bool, 0644);
MODULE_PARM_DESC(datapath_rearm,
	"re-arm the US-feed FSM at the end of device init (WSDS GPON soft-reset edge + re-run pbo_init), matching stock dataPath_reset; fixes the empty GEM-US TX bank (gemus_omcc=0) on the first grant");

static bool o5_feed_rearm = true;
module_param(o5_feed_rearm, bool, 0644);
MODULE_PARM_DESC(o5_feed_rearm,
	"re-arm the US-feed FIFO at O5 (after OMCC install), softirq-safe (feed FIFO edge only, NO WSDS soft-reset). Needed because our O3 TX-PLL relock (a SerDes reset) re-parks the US-feed AFTER the pre-ranging datapath_rearm, so it underflows on the first grant unless re-armed post-relock");

static u32 gpon_us_feed_rearm_cnt;	/* count of US-feed re-arms (pre-FSM + O5), shown in /proc/gpon */
static u32 gpon_us_intr_svc_cnt;	/* count of US-intr delta reads that found a latched event (us_intr_svc) */

/* Faithful re-express of the working firmware's GPON datapath ...
 * dev/MEASURED-luna_gpon.c.md sec 156. */
static void gpon_us_feed_rearm(void)
{
	if (!READ_ONCE(luna_activation_ready))
		return;
	sw_field(WSDS_DIG_00, 10, 10, 0);	/* assert GPON datapath reset-B */
	sw_field(WSDS_DIG_00, 10, 10, 1);	/* release -> soft-reset edge   */
	gpon_pbo_init();			/* re-arm US-feed (pool persists)*/
	gpon_us_feed_rearm_cnt++;
	luna_log("US-feed re-armed (WSDS GPON reset edge + pbo re-init, cnt=%u)\n",
		gpon_us_feed_rearm_cnt);
}

/* Softirq-safe US-feed FIFO re-arm: the pi_* feed writes of ...
 * dev/MEASURED-luna_gpon.c.md sec 157. */
static void gpon_us_feed_rearm_light(void)
{
	if (!READ_ONCE(luna_activation_ready))
		return;
	pi_wr(PI_IO_CMD_0_US, 0x90101050u);	/* GMII off: park the feed       */
	pi_field(PI_PONIP_CTL_US, 0, 0, 0);	/* PBUF_EN = 0                   */
	pi_field(PI_PON_US_FIFO_CTL, 5, 4, 1);	/* USFIFO_SPACE = 1              */
	pi_field(PI_PON_US_FIFO_CTL, 3, 0, 3);	/* USFIFO_START = 3 (feed edge)  */
	pi_field(PI_PONIP_CTL_US, 0, 0, 1);	/* PBUF_EN = 1                   */
	pi_wr(PI_IO_CMD_0_US, 0x90101070u);	/* GMII latch -> re-arm the feed */
	gpon_us_feed_rearm_cnt++;
	/* Heartbeat only: this fires every FSM tick, so logging each one floods the console
	 * and drowns serial diagnostics. The count is the useful signal -- log it periodically
	 * (still readable, no flood). */
	if (!(gpon_us_feed_rearm_cnt % 1000))
		luna_log("US-feed FIFO re-armed at O5 (feed edge, no reset, cnt=%u)\n",
			gpon_us_feed_rearm_cnt);
}

/* The O3 post-relock feed un-park, in ONE place. The TX-CMU ...
 * dev/MEASURED-luna_gpon.c.md sec 158. */
static void gpon_o3_feed_unpark(void)
{
	sw_field(WSDS_DIG_00, 10, 10, 0);
	sw_field(WSDS_DIG_00, 10, 10, 1);
	gpon_us_feed_rearm_light();
	luna_log("O3 post-relock WSDS feed-reset edge (un-park GEM-US framer)\n");
}

/* Full BOSA page2 (slave 0x54) + page3 (slave 0x55) register dump for diagnostics.
 * Reads all 512 regs via the kernel I2C path. */
static int bosadump_proc_show_locked(struct seq_file *s, void *v);
static int bosadump_proc_show(struct seq_file *s, void *v)
{
	int ret;

	mutex_lock(&bosa_lock);
	ret = luna_stopping ? -ESHUTDOWN : bosadump_proc_show_locked(s, v);
	mutex_unlock(&bosa_lock);
	return ret;
}

static int bosadump_proc_show_locked(struct seq_file *s, void *v)
{
	int i, j;

	for (i = 0; i < 256; i += 16) {
		seq_printf(s, "P2_%02x:", i);
		for (j = 0; j < 16; j++)
			seq_printf(s, " %02x", bosa_read_reg(0x200 + i + j) & 0xff);
		seq_puts(s, "\n");
	}
	for (i = 0; i < 256; i += 16) {
		seq_printf(s, "P3_%02x:", i);
		for (j = 0; j < 16; j++)
			seq_printf(s, " %02x", bosa_read_reg(0x300 + i + j) & 0xff);
		seq_puts(s, "\n");
	}
	return 0;
}

/* Full PON-IP register dump in the EXACT format of cross-compiler/stock_dump_good.txt
 * ("0x1bf0XXXX YYYYYYYY"), so mine-vs-stock diffing needs no reformatting. Covers the
 * 0x0000-0x54fc span the stock oracle captured. Read once: cat /proc/pidump. */
static int pidump_proc_show(struct seq_file *s, void *v)
{
	/* Only the mapped PON-IP windows (gaps bus-fault on read). Ranges match the
	 * readable spans of cross-compiler/stock_dump_good.txt. */
	static const u32 ranges[][2] = {
		{0x0000, 0x03fc}, {0x2000, 0x2bfc},	/* the four readable PON-IP windows */
		{0x4000, 0x40fc}, {0x5400, 0x54fc},	/* of stock_dump_good.txt, in order */
	};
	u32 off, val;
	int r;

	for (r = 0; r < (int)ARRAY_SIZE(ranges); r++)
		for (off = ranges[r][0]; off <= ranges[r][1]; off += 4) {
			val = pi_rd(off);
			if (val)	/* skip zeros: keeps the dump small for reliable serial read */
				seq_printf(s, "0x%08x %08x\n", 0x1bf00000u + off, val);
		}
	seq_puts(s, "0x1bf0ffff ffffffff\n");	/* end marker */
	return 0;
}

/* Switch-core + GMAC + SerDes + GTC dump (the GMAC0->switch->US-NIC handoff path that is
 * OUTSIDE PON-IP space). Same "0xADDR VAL" format as stock_dump_good.txt for direct diff.
 * 0x1bxxxxxx regs come via swcore_base (sw_rd); GMAC 0x18012/0x18013 via a local ioremap. */
static bool swdump_is_pcie(u32 phys)
{
	return phys >= 0x18b00000u && phys < 0x18c00000u;
}

static int swdump_proc_show(struct seq_file *s, void *v)
{
	static const u32 sw[][2] = {		/* offsets into swcore_base (phys 0x1b000000) */
		{0x00000, 0x000fc},
		/* ★ 0x00100..0x002fc: the three per-port ABILITY arrays ...
		 * dev/MEASURED-luna_gpon.c.md sec 159. */
		{0x00100, 0x002fc},
		{0x1c000, 0x1c0fc}, {0x20800, 0x2083c},
		{0x20c00, 0x20c3c}, {0x23000, 0x230fc}, {0x27000, 0x2703c},
		/* The registers this driver writes and COULD NOT READ BACK. ...
		 * dev/MEASURED-luna_gpon.c.md sec 160. */
		{0x11000, 0x1104c},	/* ACCEPT_MAX_LEN_CTRL, every die + its ports  */
		{0x110e0, 0x111fc},	/* PON_TRAP_CFG: 0x110ec / 0x111f8 / 0x11144   */
		{0x12000, 0x1200c},	/* TBL_CTRL_OFF / TBL_WRDATA_OFF               */
		{0x16000, 0x1601c},	/* CF_CFG                                      */
		{0x1c100, 0x1c2fc},	/* RMA_CFG, QOS_UNI_TRAP_PRI_CTRL, OAM_CTRL_0  */
		{0x1e000, 0x1e05c},	/* LED_MODE_SEL .. LED_PARA_EN + LED_DATA_CFG  */
		{0x23100, 0x2311c},	/* CFG_UNHIOL on the 9607C                     */
		{0x2d890, 0x2d8bc},	/* SW_SCH_WFQ_TKN_CTRL, SW_LINE_RATE_2500M     */
		/* SW_P_MISC_PORT is base 0x20004 + port * macpp_stride and ...
		 * dev/MEASURED-luna_gpon.c.md sec 161. */
		{0x13000, 0x1300c},	/* VLAN_PORT_ACCEPT_FRAME_TYPE/INGRESS/CTRL   */
		{0x17000, 0x1700c},	/* SW_LUT_CFG, SW_LUT_AGEOUT_CTRL             */
		{0x20400, 0x20430},	/* P_MISC[4] -- the RTL9603CVD's PON          */
		{0x20500, 0x20530},	/* P_MISC[5] -- its CPU, and the 9607C's PON  */
		{0x20900, 0x20930},	/* P_MISC[9] -- the RTL9607C's CPU            */
		{0x25000, 0x2500c},	/* SW_METER_TB_CTRL                           */
		{0x2a000, 0x2a024},	/* SW_VLAN_EGRESS_TAG + 4*port, ports 0..9    */
		/* The RTL9603CVD PON SerDes page: 0x022xxx on the RTL9602C, ...
		 * dev/MEASURED-luna_gpon.c.md sec 162. */
	};
	/* The GPON MAC / GTC block, through its OWN accessor. A range ...
	 * dev/MEASURED-luna_gpon.c.md sec 163. */
	static const u32 gt[][2] = {		/* offsets into gpon_base (phys 0x1b700000) */
		{0x010a0, 0x010c4}, {0x01180, 0x011c8},
		{0x01400, 0x015fc}, {0x02400, 0x027fc},
		{0x04280, 0x042a8}, {0x05080, 0x050ac},
		{0x050e0, 0x05100}, {0x06400, 0x065fc},
		/* swdump_readback_guard.py scanned only sw_field/sw_wr while ...
		 * dev/MEASURED-luna_gpon.c.md sec 164. */
		{0x0000c, 0x00014}, {0x01004, 0x01080}, {0x01200, 0x01208},
		{0x0200c, 0x0200c}, {0x03010, 0x03024}, {0x04064, 0x04098},
		{0x05010, 0x05054}, {0x050c0, 0x050c0}, {0x05140, 0x05140},
		{0x05200, 0x05200}, {0x0526c, 0x0526c}, {0x06020, 0x06024},
		{0x06260, 0x06260},
	};
	static const u32 gm[][2] = {		/* absolute phys (separate ioremap) */
		{0x18012000, 0x180120fc}, {0x18013400, 0x180134fc},
		/* The PCIe host controller, so its link state can be COMPARED ...
		 * dev/MEASURED-luna_gpon.c.md sec 165. */
		/* ⚠⚠ THIS SPAN STOPPED ONE PAGE SHORT OF THE ENGINE, and the page
		 * it did cover is the EMPTY one.  MEASURED 2026-09-16 on two
		 * independent stock captures of the RTL9603CVD: the NAPT control
		 * page 0x800000 answers 1024 words and every one is ZERO on that
		 * die, while the FLOWBASED engine at 0x801000 carries 43 non-zero
		 * words with the vendor forwarding.  So every capture of OURS held
		 * the empty page and not one word of the engine this port is
		 * missing -- swcore_blocks_from_capture said exactly that
		 * ("NO DECLARED ACCESSOR REACHES 0x00801008.."), and no
		 * stock-vs-ours diff could ever have shown it.
		 * ⚠ Both pages are kept: which model a die carries is a DIE fact,
		 * and a span that covered only the live one here would go blind on
		 * the NAPT dies where the other page is the live one. */
		{0x1b800000, 0x1b801ffc},	/* the flow/connection tables	*/
		{0x18b00700, 0x18b0073c},	/* HOSTCFG: 0x728 = LTSSM state	*/
		{0x18b01000, 0x18b0101c},	/* HOSTEXT: 0x008 = LTSSM enable	*/
		{0x18000040, 0x1800005c},	/* SOC_PINMUX at 0x4c		*/
	};
	/* The extra spans are PER DIE, and this set is the ...
	 * dev/MEASURED-luna_gpon.c.md sec 166. */
	static const u32 xsw9603[][2] = {	/* extra SWCORE spans, RTL9603CVD only */
		{0x10000, 0x1000c},	/*   4  MODEL_NAME_INFO/CHIP_INFO */
		{0x11058, 0x110c0},	/*  27  OUTPUT_DROP_CFG/OUTPUT_DROP_EN */
		{0x110d4, 0x110dc},	/*   3  PTP_P_EN */
		{0x12010, 0x1202c},	/*   8  TBL_ACCESS_RD_DATA */
		{0x13000, 0x13020},	/*   9  VLAN_PORT_ACCEPT_FRAME_TYPE/VLAN_INGRESS */
		{0x13040, 0x130e0},	/*  41  VLAN_PPB_VLAN_VAL/VLAN_PORT_PPB_VLAN */
		{0x14000, 0x1401c},	/*   8  SVLAN_P_SVID/SVLAN_EXT_SVID */
		{0x15008, 0x1518c},	/*  98  ACL_TEMPLATE_CTRL/ACL_EN */
		{0x15450, 0x15470},	/*   9  RNG_CHK_PKTLEN_RNG/RGF_VER_ALE_ACL */
		{0x17000, 0x171b8},	/* 111  LUT_CFG/LUT_AGEOUT_CTRL */
		{0x18000, 0x18000},	/*   1  RGF_VER_ALE_MLTVLAN */
		{0x1b008, 0x1b008},	/*   1  PTP_TIME_NSEC */
		{0x1b014, 0x1b020},	/*   4  PTP_TIME_OFFSET_8NSEC/PTP_TIME_FREQ */
		{0x1b02c, 0x1b02c},	/*   1  PTP_PON_TOD_NSEC */
		{0x1b038, 0x1b078},	/*  17  RGF_VER_ALE_EAV_AFBK/RSVD_ALE_EAV_AFBK */
		{0x1c458, 0x1c460},	/*   3  RGF_VER_ALE_DPM/RSVD_ALE_DPM */
		{0x1d000, 0x1d010},	/*   5  INTR_CTRL/INTR_DBGO_POS */
		{0x1d01c, 0x1d030},	/*   6  INTR_STAT_POS/INTR_STAT_NEG */
		{0x1e068, 0x1e070},	/*   3  LED_EN/SERI_LED_CLK_PER */
		{0x1e078, 0x1e078},	/*   1  PON_LED_CFG */
		{0x1e090, 0x1e09c},	/*   4  LED_FLT_MPCP/RGF_VER_LED */
		{0x1f000, 0x1f034},	/*  14  PHY_RG0X_CEN/PHY_RG1X_CEN */
		{0x1f040, 0x1f090},	/*  21  PHY_RG0X_PLL/PHY_RG1X_PLL */
		{0x20000, 0x20030},	/*  13  P_TX_ERR_CNT/P_MISC */
		{0x20100, 0x20130},	/*  13   */
		{0x20200, 0x20230},	/*  13   */
		{0x20300, 0x20330},	/*  13   */
		{0x22000, 0x220dc},	/*  56  EXTG_ACTYPE0/EXTG_ACTYPE1 */
		{0x23120, 0x23140},	/*   9  FC_PON_GLB_HI_TH/FC_PON_GLB_LO_TH */
		{0x23154, 0x231b0},	/*  24  SPG_GLB_CTRL/FC_SWPBO_GLB_HI_TH */
		{0x24000, 0x241fc},	/* 128  EXTHDR_DAT */
		{0x2518c, 0x2519c},	/*   5  METER_PKT_RATE/RGF_VER_ALE_METER */
		{0x26000, 0x26014},	/*   6  DOS_EN/DOS_CFG */
		{0x28040, 0x280a8},	/*  27  HSB_DATA */
		{0x280c0, 0x280c8},	/*   3  HSB_PARSER */
		{0x28100, 0x28158},	/*  23  HSA_DATA */
		{0x28164, 0x28234},	/*  53  HSM_DATA/FBHSA_DATA */
		{0x2a000, 0x2a120},	/*  73  VLAN_EGRESS_TAG/IP4MC_EGRESS_MODE */
		{0x2a130, 0x2a1a0},	/*  29  RMK_P_DSCP_SEL/RMK_P_1P_SEL */
		{0x2d000, 0x2d010},	/*   5  LOW_QUEUE_TH/HIGH_QUEUE_MSK */
		{0x2d020, 0x2d14c},	/*  76  FC_P_EGR_DROP_TH/FC_DBG_CTRL */
		{0x2d800, 0x2d88c},	/*  36  WFQ_CTRL/EGR_BWCTRL_P_CTRL */
		{0x2d8c0, 0x2d8f8},	/*  15  MOCIR_BPT/BYTE_TOKEN_METER */
		{0x31000, 0x3105c},	/*  24  SW_BIST_CFG_3/SW_BIST_CFG_6 */
		{0x31064, 0x310c8},	/*  26  SW_BIST_CFG_OQ_0/SW_BIST_CFG_OQ_1 */
		{0x32c80, 0x32f50},	/* 181  STAT_ACL_CNT/DOT3_Q_TX_FRAMES */
		{0x34000, 0x34000},	/*   1  STAT_CTRL */
		{0x34014, 0x3401c},	/*   3  STAT_PORT_RST/STAT_RST */
		{0x34034, 0x34034},	/*   1  RGF_VER_MIB_CTRL */
		{0x36000, 0x36038},	/*  15  EPON_FEC_CONFIG/EPON_ASIC_TIMING_ADJUST1 */
		{0x3609c, 0x360a4},	/*   3  EPON_MPCP_CTR/EPON_TX_CTRL */
		{0x360e8, 0x36108},	/*   9  EPON_SCB_DECRYP_KEY0/EPON_SCB_DECRYP_KEY1 */
		{0x36120, 0x361b8},	/*  39  EPON_SCH_TIMING/EPON_FEC_RST */
	};
	static const u32 xgt9603[][2] = {	/* extra GTC spans, RTL9603CVD only    */
		{0x00020, 0x00024},	/*   2  GPON_AES_BYPASS/GPON_FRAME_PULSE_CFG */
		{0x00040, 0x00044},	/*   2  GPON_INTR_MASK/GPON_INTR_STS */
		{0x00280, 0x00280},	/*   1  GPON_MAC_BIST_RSLT */
		{0x010cc, 0x010cc},	/*   1  GPON_GTC_DS_ALLOC_RD */
		{0x01100, 0x01104},	/*   2  GPON_GTC_DS_PORT_IND/GPON_GTC_DS_PORT_WR */
		{0x0110c, 0x0110c},	/*   1  GPON_GTC_DS_PORT_RD */
		{0x01140, 0x01140},	/*   1  GPON_GTC_DS_PORT_CNTR_IND */
		{0x01240, 0x01244},	/*   2  GPON_GTC_DS_BPOS_SUB/GPON_GTC_DS_BPOS_ADD */
		{0x01260, 0x0126c},	/*   4  GPON_GTC_DS_DUMMY_1/GPON_GTC_DS_DUMMY_2 */
		{0x01280, 0x0128c},	/*   4  DS_FEC/PLOAM */
		{0x01294, 0x012a4},	/*   5  BWMAP_CAP/BWMAP_BUF */
		{0x0200c, 0x02010},	/*   2  GPON_BWMAP_CTRL/GPON_BWMAP_STS */
		{0x03004, 0x03008},	/*   2  GPON_AES_INTR_MASK/GPON_AES_INTR_STS */
		{0x03280, 0x03290},	/*   5  CTL_INFO/CTL_DATA */
		{0x04040, 0x04040},	/*   1  GPON_GEM_DS_RX_CNTR_IND */
		{0x0404c, 0x0404c},	/*   1  GPON_GEM_DS_FWD_CNTR_IND */
		{0x04098, 0x040a0},	/*   3  GPON_GEM_DS_FRM_TIMEOUT/GPON_GEM_DS_MC_ADDR_PTN_IPV4 */
		{0x05004, 0x05008},	/*   2  GPON_GTC_US_INTR_MASK/GPON_GTC_US_INTR_STS */
		{0x05180, 0x05188},	/*   3  GPON_GTC_US_RDI/GPON_GTC_US_DG */
		{0x05260, 0x05270},	/*   5  G_DMY_XX_01/G_DMY_XX_02 */
		{0x05280, 0x05294},	/*   6  BWM_TBL/BWM_MEM0 */
		{0x06004, 0x06008},	/*   2  GPON_GEM_US_INTR_MASK/GPON_GEM_US_INTR_STS */
		{0x06048, 0x06048},	/*   1  GPON_GEM_US_ETH_GEM_RX_CNTR_IDX */
		{0x06054, 0x06054},	/*   1  GPON_GEM_US_PTN_CTRL */
		{0x06280, 0x06294},	/*   6  PCFG/GEM_BCNT */
	};
	const u32 (*xsw)[2] = NULL, (*xgt)[2] = NULL;
	int nxsw = 0, nxgt = 0;
	bool no_pcie;
	u32 off, a, val;
	int r;

	/* The per-die extras, selected by the SAME pointer every other per-chip
	 * fact in this driver comes from.  Any other die keeps NULL/0 and dumps
	 * exactly what it dumped before. */
	/* The RTL9601D has no PCIe host block: a read of 0x18b00720 hangs its bus
	 * until the watchdog resets it (measured on its stock, 2026-10-01). */
	no_pcie = of_machine_is_compatible("realtek,rtl9601d");
	if (swc == &gpon_swc_9603cvd) {
		xsw = xsw9603;
		nxsw = (int)ARRAY_SIZE(xsw9603);
		xgt = xgt9603;
		nxgt = (int)ARRAY_SIZE(xgt9603);
	}

	/* The dump declares its own COVERAGE first. Words are ...
	 * dev/MEASURED-luna_gpon.c.md sec 167. */
	for (r = 0; r < (int)ARRAY_SIZE(sw); r++)
		seq_printf(s, "#cover 0x%08x 0x%08x\n",
			   0x1b000000u + sw[r][0], 0x1b000000u + sw[r][1]);
	for (r = 0; r < (int)ARRAY_SIZE(gm); r++)
		if (!(no_pcie && swdump_is_pcie(gm[r][0])))
			seq_printf(s, "#cover 0x%08x 0x%08x\n", gm[r][0], gm[r][1]);
	for (r = 0; r < (int)ARRAY_SIZE(gt); r++)
		seq_printf(s, "#cover 0x%08x 0x%08x\n",
			   0x1b700000u + gt[r][0], 0x1b700000u + gt[r][1]);
	for (r = 0; r < nxsw; r++)
		seq_printf(s, "#cover 0x%08x 0x%08x\n",
			   0x1b000000u + xsw[r][0], 0x1b000000u + xsw[r][1]);
	for (r = 0; r < nxgt; r++)
		seq_printf(s, "#cover 0x%08x 0x%08x\n",
			   0x1b700000u + xgt[r][0], 0x1b700000u + xgt[r][1]);
	seq_printf(s, "#cover 0x%08x 0x%08x\n",
		   0x1b000000u + SDS(0x22000), 0x1b000000u + SDS(0x22ffc));

	/* The SerDes page is per chip, so its range must be DERIVED. ...
	 * dev/MEASURED-luna_gpon.c.md sec 168. */
	for (off = SDS(0x22000); off <= SDS(0x22ffc); off += 4)
		seq_printf(s, "0x%08x %08x\n", 0x1b000000u + off, sw_rd(off));

	for (r = 0; r < (int)ARRAY_SIZE(gt); r++)
		for (off = gt[r][0]; off <= gt[r][1]; off += 4)
			seq_printf(s, "0x%08x %08x\n", 0x1b700000u + off,
				   gpon_rd(off));
	for (r = 0; r < (int)ARRAY_SIZE(sw); r++)
		for (off = sw[r][0]; off <= sw[r][1]; off += 4) {
			val = sw_rd(off);
			if (val)
				seq_printf(s, "0x%08x %08x\n", 0x1b000000u + off, val);
		}
	for (r = 0; r < nxgt; r++)
		for (off = xgt[r][0]; off <= xgt[r][1]; off += 4)
			seq_printf(s, "0x%08x %08x\n", 0x1b700000u + off,
				   gpon_rd(off));
	for (r = 0; r < nxsw; r++)
		for (off = xsw[r][0]; off <= xsw[r][1]; off += 4) {
			val = sw_rd(off);
			if (val)
				seq_printf(s, "0x%08x %08x\n", 0x1b000000u + off, val);
		}
	for (r = 0; r < (int)ARRAY_SIZE(gm); r++) {
		void __iomem *b;

		if (no_pcie && swdump_is_pcie(gm[r][0]))
			continue;
		b = ioremap(gm[r][0], gm[r][1] - gm[r][0] + 4);
		if (!b)
			continue;
		for (a = gm[r][0]; a <= gm[r][1]; a += 4) {
			val = ioread32(b + (a - gm[r][0]));
			if (val)
				seq_printf(s, "0x%08x %08x\n", a, val);
		}
		iounmap(b);
	}
	seq_puts(s, "0xffffffff ffffffff\n");	/* end marker */
	return 0;
}

/* Serialize complete indirect transactions, including readback. A timed-out
 * CAM retains its own pending state after the lock is released. */
static DEFINE_SPINLOCK(gpon_ind_lock);
static bool gpon_cam_pending[2];

static int luna_cam_xact(bool alloc, u32 mode, u32 index, u16 *value, bool *hit)
{
	const struct gpon_gtc_regs *r = &luna_gpon_chip.gtc;
	struct reg ind = alloc ? r->ds_alloc_ind : r->ds_port_ind;
	struct reg wr = alloc ? r->ds_alloc_wr : r->ds_port_wr;
	struct reg rd = alloc ? r->ds_alloc_rd : r->ds_port_rd;
	u32 mask = alloc ? r->ds_alloc_idx_mask : r->ds_port_idx_mask;
	unsigned long flags;
	int rc;

	if (index > mask || (mode == GPON_GTC_CAM_OP_READ && (!value || !hit)))
		return -EINVAL;
	/* UNSET and DECLARED-ABSENT are different repairs, so the rc says which. */
	if (mode == GPON_GTC_CAM_OP_READ) {
		rc = reg_rc(rd);
		if (rc)
			return rc;
	}
	if (alloc)
		index = luna_tcont_cam_index(index);
	spin_lock_irqsave(&gpon_ind_lock, flags);
	rc = gpon_gtc_cam_xact_pending(&gpon_io, ind, mode, mask, index, wr,
				       mode == GPON_GTC_CAM_OP_WRITE && value ? *value : 0,
				       gpon_cam_delay_us,
				       &gpon_cam_pending[alloc]);
	if (rc >= 0 && mode == GPON_GTC_CAM_OP_READ) {
		*hit = !!(hwio_rd(&gpon_io, reg_at(ind)) & GPON_GTC_CAM_OP_HIT);
		*value = hwio_rd(&gpon_io, reg_at(rd)) & GPON_GTC_CAM_VAL_MASK;
	}
	spin_unlock_irqrestore(&gpon_ind_lock, flags);
	return rc < 0 ? rc : 0;
}

static int luna_port_cam_write(u32 flow, u16 gem)
{
	return luna_cam_xact(false, GPON_GTC_CAM_OP_WRITE, flow, &gem, NULL);
}

static int luna_alloc_cam_write(u32 tcont, u16 alloc)
{
	return luna_cam_xact(true, GPON_GTC_CAM_OP_WRITE, tcont, &alloc, NULL);
}

/* Per-flow downstream GEM Ethernet RX packet count ... -- dev/MEASURED-luna_gpon.c.md sec 169. */
static u32 gpon_gem_ds_rx_cnt(u8 flow)
{
	u32 cnt;
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&gpon_ind_lock, flags);
	rc = gpon_gtc_gem_ds_rx_cnt_read(&gpon_io, &luna_gpon_chip.gtc, flow,
					 &cnt, gpon_cam_delay_us);
	spin_unlock_irqrestore(&gpon_ind_lock, flags);
	return rc < 0 ? 0xffffffffu : cnt;
}

/* Per-flow downstream GEM FORWARDED-to-PON-IP count ... -- dev/MEASURED-luna_gpon.c.md sec 299. */
static u32 gpon_gem_ds_fwd_cnt(u8 flow)
{
	u32 cnt;
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&gpon_ind_lock, flags);
	rc = gpon_gtc_gem_ds_fwd_cnt_read(&gpon_io, &luna_gpon_chip.gtc, flow,
					 &cnt, gpon_cam_delay_us);
	spin_unlock_irqrestore(&gpon_ind_lock, flags);
	return rc < 0 ? 0xffffffffu : cnt;
}

/* Read back the DS GEM-port CAM entry for `flow`: does the ...
 * dev/MEASURED-luna_gpon.c.md sec 170. */
static int gpon_ds_cam_read(u8 flow)
{
	u16 gem;
	bool hit;
	int rc = luna_cam_xact(false, GPON_GTC_CAM_OP_READ, flow, &gem, &hit);

	if (rc < 0)
		return rc;
	return (hit ? BIT(16) : 0) | gem;
}

/* Invalidate non-management DS GEM-port CAM entries (CLEAN op ...
 * dev/MEASURED-luna_gpon.c.md sec 171. */
static int gpon_ds_cam_clean(unsigned int f)
{
	int rc = luna_cam_xact(false, GPON_GTC_CAM_OP_CLEAN, f, NULL, NULL);

	if (!rc)
		gpon_field(GPON_GTC_DS_TRAFFIC_CFG + f * DS_TRAFFIC_CFG_STRIDE, 4, 0, 0);
	return rc;
}

static int gpon_ds_cam_clear_all(void)
{
	unsigned int f;

	for (f = 0; f < 128; f++) {
		int rc = f == GPON_OMCC_FLOW ? 0 : gpon_ds_cam_clean(f);

		if (rc)
			return rc;
	}
	return 0;
}

/* The reset's garbage is gone (cleared once after it): a Configure_Port-ID then
 * cleans only the flows this driver programs, 127 transactions fewer (~0.6 ms on
 * the RLX) between the request and its Acknowledge. */
static bool luna_ds_cam_scrubbed;

/* GTC US MISC PM counter (GPON_GTC_US_MISC_CNTR_IDX 0x5140 ...
 * dev/MEASURED-luna_gpon.c.md sec 172. */
static u32 gpon_us_misc_cnt(u8 idx)
{
	unsigned long flags;
	u32 value;
	spin_lock_irqsave(&gpon_ind_lock, flags);
	gpon_wr(0x5140, idx & 0x7);
	udelay(5);
	value = gpon_rd(0x5148);
	spin_unlock_irqrestore(&gpon_ind_lock, flags);
	return value;
}

/* GEM DS MISC PM counter (clear-on-read) -- GLOBAL, ... -- dev/MEASURED-luna_gpon.c.md sec 173. */
static u32 gpon_gem_ds_misc_cnt(u8 idx)
{
	unsigned long flags;
	u32 value;
	int i;

	spin_lock_irqsave(&gpon_ind_lock, flags);
	gpon_wr(GPON_GEM_DS_MISC_IND, idx & 0xf);
	for (i = 0; i < 16; i++) {
		if (gpon_rd(GPON_GEM_DS_MISC_IND) & BIT(15))
			break;
		udelay(2);
	}
	value = gpon_rd(GPON_GEM_DS_MISC_CNTR_STAT);
	spin_unlock_irqrestore(&gpon_ind_lock, flags);
	return value;
}

/* Read back the GTC alloc CAM entry for a T-CONT (READ op, ...
 * dev/MEASURED-luna_gpon.c.md sec 174. */
static int gpon_alloc_cam_read(u8 tcont)
{
	u16 alloc;
	bool hit;
	int rc = luna_cam_xact(true, GPON_GTC_CAM_OP_READ, tcont, &alloc, &hit);

	if (rc < 0)
		return rc;
	return (hit ? BIT(16) : 0) | alloc;
}

/* Invalidate every allocation except the management T-CONT before activation.
 * Alloc-ID 4095 is assignable: writing 0xfff is not an invalidation. Both own
 * stock dies use CLEAN, with the CVD logical-index permutation in luna_cam_xact.
 * A failed audit/clean stops the caller before it publishes a new assignment. */
static int gpon_alloc_cam_clear_others(u8 keep)
{
	u8 t;
	int rc;

	if (keep >= 32)
		return -EINVAL;
	rc = luna_data_retire();
	if (rc)
		return rc;
	for (t = 0; t < 32; t++) {
		int rb, rc;

		if (t == keep)
			continue;
		rb = gpon_alloc_cam_read(t);
		if (rb < 0)
			return rb;
		if (rb & BIT(16)) {
			u8 dmp[4] = { t, keep, (u8)((rb >> 8) & 0xf), (u8)rb };

			gpon_unsup_report("alloc_cam_stale", GPON_UNSUP_RANGE,
					  rb & 0xfff,
					  "no-hit-outside-kept-tcont", dmp, sizeof(dmp));
		}
		rc = luna_cam_xact(true, GPON_GTC_CAM_OP_CLEAN, t, NULL, NULL);
		if (rc)
			return rc;
	}
	return 0;
}

/* DS-pipeline stage A: per-flow de-encapsulated GEM frame ...
 * dev/MEASURED-luna_gpon.c.md sec 175. */
static u32 gpon_gem_flow_cnt(u32 idx, int rsel)
{
	u32 cnt;
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&gpon_ind_lock, flags);
	rc = gpon_gtc_ds_port_cnt_read(&gpon_io, &luna_gpon_chip.gtc, idx,
				      (rsel & 1) != 0, &cnt,
				      gpon_cam_delay_us);
	spin_unlock_irqrestore(&gpon_ind_lock, flags);
	return rc < 0 ? 0xffffffffu : cnt;
}

static int gpon_proc_show_locked(struct seq_file *s, void *v);
static int gpon_proc_show(struct seq_file *s, void *v)
{
	int ret;

	mutex_lock(&bosa_lock);
	ret = luna_stopping ? -ESHUTDOWN : gpon_proc_show_locked(s, v);
	mutex_unlock(&bosa_lock);
	return ret;
}

static int gpon_proc_show_locked(struct seq_file *s, void *v)
{
	u32 rst    = gpon_rd(GPON_RESET);
	u32 status = gpon_rd(GPON_GTC_DS_ONU_ID_STATUS);
	u32 eqd    = gpon_rd(GPON_GTC_US_EQD);
	u32 state  = status & GPON_ONU_STATE_MASK;

	luna_omci_seq_show(s);
	seq_puts(s, OMCI_ID_FIELDS_LINE "\n");
	if (is_9607c)
		luna_c7_diag(&rtl9602c_r960_ops, s);
	seq_printf(s, "optics: backend=%s gn_cal_ready=%u upstream_ready=%u error=%d\n",
		bosa_regs_live() ? "RTL8290B" : bosa_gn_identified ? "GN25L95" : "unknown",
		bosa_cal_ready, luna_activation_ready, bosa_cal_error);
	seq_printf(s, "version:     0x%02x\n", gpon_rd(GPON_VERSION) & GPON_VER_ID_MASK);
	seq_printf(s, "reset:       0x%08x (soft_rst=%d rst_done=%d)\n",
		   rst, !!(rst & GPON_SOFT_RST), !!(rst & GPON_RST_DONE));
	seq_printf(s, "onu_state:   O%u (%s)\n", state,
		   state < ARRAY_SIZE(gpon_onu_state_name) &&
		   gpon_onu_state_name[state] ? gpon_onu_state_name[state] : "?");
#ifdef CONFIG_GPON_PLOAM_DIAG
	{	/* every DS PLOAM by type/addressing, every US one queued: no printk */
		char types[320];

		gpon_ploam_types_format(types, sizeof(types), &luna_ploam);
		seq_printf(s, "%s | us_full_drops %u\n", types, luna_us_ploam_full_drops);
	}
#endif
	/* fiber: one-glance fibre-pull / optical recovery verdict. ...
	 * dev/MEASURED-luna_gpon.c.md sec 300. */
	{
		u32 los = gpon_rd(GPON_GTC_DS_LOS_CFG_STS);
		s32 rx_cdbm = bosa_rx_power_cdbm();	/* calibrated ratiometric RX (was a linear 0x311 fit) */
		char rx_s[16], sdet_s[8];

		/* ★ NEITHER FIELD MAY PRINT A NUMBER IT DID NOT MEASURE. rx was a
		 * hard floor constant (-29.85 dBm) on every failed I2C read, and
		 * sdet was a read of whatever the 9602C's SDS_FIB_STATUS offset
		 * happens to be on this chip. Three states each: value / n/a. */
		if (rx_cdbm == BOSA_RX_CDBM_NA)
			strscpy(rx_s, "n/a", sizeof(rx_s));
		else
			scnprintf(rx_s, sizeof(rx_s), "%d.%02ddBm", rx_cdbm / 100,
				  (rx_cdbm < 0 ? -rx_cdbm : rx_cdbm) % 100);
		if (SDS_FIB_STATUS)
			scnprintf(sdet_s, sizeof(sdet_s), "%d",
				  !!(sw_rd(SDS_FIB_STATUS) & SDS_FIB_SDS_SDET));
		else
			strscpy(sdet_s, "n/a", sizeof(sdet_s));

		/* The two OLT-ASSIGNED identities of the WAN datapath are printed
		 * beside their install flags: without them a "solicited=1
		 * DATA_GEM_inst=1" line cannot tell a correct bind from a bind to
		 * a retired Alloc-ID or to another board's gem-port. */
		seq_printf(s, "fiber:       rerange=%u last_outage=%ums | optic_los=%d sdet=%s rx=%s | omcc_inst=%d DATA_GEM_inst=%d solicited=%d gem=%u alloc=0x%x tcont_bound=%d\n",
			   gpon_rerange_cnt, gpon_last_outage_ms,
			   !!(los & GPON_OPTIC_LOS_SIG),
			   sdet_s, rx_s,
			   luna_ploam.omcc_installed, gpon_data_installed, gpon_data_gem_solicited,
			   gpon_data_gem_port, gpon_data_alloc, gpon_data_tcont_installed);
	}
	seq_printf(s, "onu_id:      %u\n",
		   (status >> GPON_ONU_ID_SHIFT) & GPON_ONU_ID_MASK);
	seq_printf(s, "eqd:         inframe=%u multiframe=%u\n",
		   eqd & GPON_EQD_INFRAME_MASK,
		   (eqd >> GPON_EQD_MF_SHIFT) & GPON_EQD_MF_MASK);
	seq_printf(s, "min_delay:   0x%08x\n", gpon_rd(GPON_GTC_US_MIN_DELAY));
	seq_printf(s, "us_cfg:      0x%08x (ben_polar=%u scrm_dis=%u plm_dis=%u)\n",
		   gpon_rd(GPON_GTC_US_CFG),
		   (gpon_rd(GPON_GTC_US_CFG) >> 3) & 1,
		   gpon_rd(GPON_GTC_US_CFG) & 1,
		   (gpon_rd(GPON_GTC_US_CFG) >> 9) & 1);
	seq_printf(s, "us_laser:    0x%08x (lon=%u loff=%u)\n",
		   gpon_rd(GPON_GTC_US_LASER),
		   (gpon_rd(GPON_GTC_US_LASER) >> 8) & 0x3f,
		   gpon_rd(GPON_GTC_US_LASER) & 0x3f);
	{
		int i;

		seq_printf(s, "boh_cfg:     0x%08x (repeat=%u length=%u) data=",
			   gpon_rd(GPON_GTC_US_BOH_CFG),
			   (gpon_rd(GPON_GTC_US_BOH_CFG) >> 8) & 0xf,
			   gpon_rd(GPON_GTC_US_BOH_CFG) & 0xff);
		for (i = 0; i < GPON_BOH_LEN; i++)
			seq_printf(s, "%02x", gpon_rd(GPON_GTC_US_BOH_DATA + i * 4) & 0xff);
		seq_puts(s, "\n");
	}
	seq_printf(s, "test:        0x%08x\n", gpon_rd(GPON_TEST));
	seq_printf(s, "intr_mask:   0x%08x\n", gpon_rd(GPON_INTR_MASK));
	seq_printf(s, "intr_sts:    0x%08x\n", gpon_rd(GPON_INTR_STS));
	seq_printf(s, "gtc_ds_dlt:  0x%08x\n", gpon_rd(GPON_GTC_DS_INTR_DLT));
	seq_printf(s, "gtc_ds_mask: 0x%08x\n", gpon_rd(GPON_GTC_DS_INTR_MASK));
	seq_printf(s, "gtc_ds_sts:  0x%08x\n", gpon_rd(GPON_GTC_DS_INTR_STS));
	seq_printf(s, "cdr_recover: wedged=%u fixed=%u last_sts=0x%08x (sentinel=0x%08x)\n",
		   gpon_cdr_stuck_count, gpon_cdr_stuck_fixed, gpon_gtc_ds_sts_last,
		   GTC_DS_CDR_STUCK);
	{
		u32 los = gpon_rd(GPON_GTC_DS_LOS_CFG_STS);

		/* ★ cdr_los IS ONLY A STATUS WHILE ITS MONITOR IS ON. The ...
		 * dev/MEASURED-luna_gpon.c.md sec 301. */
		seq_printf(s, "los_cfg_sts: 0x%08x (optic_los=%d cdr_los=%s en=%d polar=%d)\n",
			   los, !!(los & GPON_OPTIC_LOS_SIG),
			   (los & GPON_CDR_LOS_EN)
				? ((los & GPON_CDR_LOS_SIG) ? "1" : "0")
				: "disabled",
			   !!(los & GPON_OPTIC_LOS_EN),
			   !!(los & GPON_OPTIC_LOS_POLAR));
	}

	/* PLOAM channel view, read-only. With no downstream PLOAM and ...
	 * dev/MEASURED-luna_gpon.c.md sec 302. */
	{
		u32 ds_ind  = gpon_rd(GPON_GTC_DS_PLOAM_IND);
		u32 us_ind  = gpon_rd(GPON_GTC_US_PLOAM_IND);
		u32 us_cfg  = gpon_rd(GPON_GTC_US_PLOAM_CFG);
		u32 us_onu  = gpon_rd(GPON_GTC_US_ONU_ID);

		seq_printf(s, "ds_ploam_ind:  0x%08x (buf_empty=%d buf_full=%d)\n",
			   ds_ind, !!(ds_ind & GPON_DS_PLM_BUF_EMPTY),
			   !!(ds_ind & GPON_DS_PLM_BUF_FULL));
		seq_printf(s, "us_ploam_ind:  0x%08x (nrm_empty=%d nrm_full=%d urg_empty=%d urg_full=%d)\n",
			   us_ind, !!(us_ind & GPON_US_PLM_NRM_EMPTY),
			   !!(us_ind & GPON_US_PLM_NRM_FULL),
			   !!(us_ind & GPON_US_PLM_URG_EMPTY),
			   !!(us_ind & GPON_US_PLM_URG_FULL));
		seq_printf(s, "us_ploam_cfg:  0x%08x (crc_gen=%d onuid_ovrd=%d)\n",
			   us_cfg, !!(us_cfg & GPON_US_PLM_CRC_GEN_EN),
			   !!(us_cfg & GPON_US_PLM_ONUID_OVRD));
		seq_printf(s, "us_onu_id:     %u\n",
			   (us_onu >> GPON_GTC_US_ONU_ID_SHIFT) & GPON_ONU_ID_MASK);
		seq_printf(s, "ds_ploam_cfg:  0x%08x\n",
			   gpon_rd(GPON_GTC_DS_PLOAM_CFG));
	}

	/*
	 * DS framer config against its O5 operating values. A mismatch in ds_cfg is
	 * the prime suspect for "configured correctly yet will not frame-lock".
	 */
	seq_printf(s, "gtc_cfg: ds_cfg=0x%08x intr_mask=0x%08x sf_cnt=0x%08x tod_sf_ctrl=0x%08x pps_ctrl=0x%08x\n",
		   gpon_rd(GPON_GTC_DS_CFG), gpon_rd(GPON_GTC_DS_INTR_MASK), gpon_rd(GPON_GTC_DS_SUPERFRAME_CNT),
		   gpon_rd(GPON_GTC_DS_TOD_SUPERFRAME_CTRL), gpon_rd(GPON_GTC_DS_PPS_CTRL));
	/* DS_MISC counters (GTC-relative, register-map base ...
	 * dev/MEASURED-luna_gpon.c.md sec 303. */
	{
		/* `active=` used to print this word raw: it is two 16-bit counters,
		 * the SN requests and the RANGING requests the GTC received (chipdef
		 * field names), and the split is what decides an activation fork. */
		u32 act = gpon_rd(GPON_GTC_DS_MISC_CNTR_ACTIVE);

		seq_printf(s, "ds_cntr: ploam_acpt=%u ploam_fail=%u bwm_acpt=%u bwm_fail=%u bwm_inv=%u sn_req=%u rng_req=%u (all clear-on-read)\n",
			   gpon_rd(GPON_GTC_DS_MISC_CNTR_PLOAM_ACPT), gpon_rd(GPON_GTC_DS_MISC_CNTR_PLOAM_FAIL),
			   gpon_bwm_acpt_since(&gpon_bwm_acpt_ds_cntr_last),
			   gpon_rd(GPON_GTC_DS_MISC_CNTR_BWM_FAIL), gpon_rd(GPON_GTC_DS_MISC_CNTR_BWM_INV),
			   GPON_GTC_DS_ACTIVE_SN_REQ(act), GPON_GTC_DS_ACTIVE_RNG_REQ(act));
	}
	/* ★ THE FLOW IS THE CHIP'S, AND THE LABEL PRINTS IT ...
	 * dev/MEASURED-luna_gpon.c.md sec 304. */
	seq_printf(s, "gem_ds_rx: omcc(f%u)=%u f0=%u f1=%u f2(mcast)=%u  (>0 => OLT is sending DS GEM/OMCI)\n",
		   GPON_OMCC_FLOW, gpon_gem_ds_rx_cnt(GPON_OMCC_FLOW),
		   gpon_gem_ds_rx_cnt(0),
		   gpon_gem_ds_rx_cnt(1), gpon_gem_ds_rx_cnt(GPON_MCAST_FLOW));
	/* GLOBAL CAM-independent de-assembly counters (the decisive OMCI detector):
	 * UC_RX>0 => DS unicast GEM reaches the de-assembler; OMCI_RX>0 => it de-encapped
	 * an OMCI frame; ETH_CRC_ERR>0 => frames arrive but fail FCS (then are dropped). */
	seq_printf(s, "ds_misc: UC_RX=%u MC_RX=%u OMCI_RX=%u ETH_CRC_ERR=%u OVER_INTL=%u MC_LEAK=%u\n",
		   gpon_gem_ds_misc_cnt(1), gpon_gem_ds_misc_cnt(0), gpon_gem_ds_misc_cnt(6),
		   gpon_gem_ds_misc_cnt(4), gpon_gem_ds_misc_cnt(5), gpon_gem_ds_misc_cnt(3));
	/* GTC-layer GEM health (direct regs): distinguishes "GEM arrives but FAILS" (FAIL
	 * or HEC climbs => garble/sync) from "no GEM at all" (all flat). NON_IDLE/IDLE =
	 * good frames; FAIL=0x11c0 LOS=0x11b4 HEC=0x11b8 frm_to(0x4098). */
	seq_printf(s, "ds_gem: NON_IDLE=%u IDLE=%u FAIL=%u LOS=%u HEC=%u | frm_to(0x4098)=0x%x\n",
		   gpon_rd(GPON_GTC_DS_MISC_CNTR_GEM_NON_IDLE), gpon_rd(GPON_GTC_DS_MISC_CNTR_GEM_IDLE), gpon_rd(GPON_GTC_DS_MISC_CNTR_GEM_FAIL),
		   gpon_rd(GPON_GTC_DS_MISC_CNTR_GEM_LOS), gpon_rd(GPON_GTC_DS_MISC_CNTR_HEC_CORRECT), gpon_rd(GPON_GEM_DS_FRM_TIMEOUT));
	/* DS pipeline stages for the OMCI frame (read with the gate open at O5):
	 * A=de-encap pkt(f64)+global non-idle; B=PBO HIGH-queue(Q0) page cur/max;
	 * C=DS SRAM pool used/peak; D=PON-IP->NIC RX_OK/MISS/ERR + init-ready.
	 * Walk A->D: first 0 (or non-zero-meets-zero boundary) = the stall stage. */
	{
		u32 a = gpon_gem_flow_cnt(GPON_OMCC_FLOW, 0);
		u32 q0 = pi_rd(PI_PON_DS_PBO_PAGE_Q0), us = pi_rd(PI_PON_DSC_USAGE_DS),
		    sts = pi_rd(PI_PON_DSC_STS_DS);
		u32 ok = pi_rd(PI_PKT_OK_CNT_DS), ms = pi_rd(PI_PKT_MISS_CNT_DS), er = pi_rd(PI_PKT_ERR_CNT_DS);

		seq_printf(s, "ds_pipe: A_deenc(f%u)=%u nonidle=%u | B_q0cur=%u q0max=%u | C_sram=%u peak=%u | D_rxok=%u miss=%u err=%u initrdy=%u\n",
			   GPON_OMCC_FLOW, a, gpon_rd(GPON_GTC_DS_MISC_CNTR_GEM_NON_IDLE),
			   q0 & 0x1fff, (q0 >> 13) & 0x1fff,
			   us & 0x1fff, sts & 0x1fff,
			   ok & 0xffff, (ms >> 16) & 0xffff, er & 0xffff,
			   pi_rd(PI_PON_IPSTS_DS) & 1);
		/* DECISIVE break-localizer: FWD(f64)=GEM frames the GTC FORWARDED to the
		 * PON-IP (0x404c/0x4050) vs A_deenc(=de-encap'd). media_sts(0x1bf0c058) bit18
		 * = internal DS-GMII FORCELINK (stock ~0x106e8400). de-encap>FWD => GTC drops;
		 * FWD>0 & C_sram flat => PON-IP rejects (descriptor base or DS-GMII down). */
		seq_printf(s, "ds_fwd: FWD(f%u)=%u FWD(f0)=%u | media_sts=0x%08x (bit18 link=%u) gmii_en=%u\n",
			   GPON_OMCC_FLOW, gpon_gem_ds_fwd_cnt(GPON_OMCC_FLOW),
			   gpon_gem_ds_fwd_cnt(0),
			   pi_rd(PI_MEDIA_STS_DS), (pi_rd(PI_MEDIA_STS_DS) >> 18) & 1,
			   (pi_rd(PI_IO_CMD_0_DS) >> 5) & 1);
		/* Read-back the DS-engine enables (a later reset may have ...
		 * dev/MEASURED-luna_gpon.c.md sec 176. */
		seq_printf(s, "ds_en: ctl_ds(0xa0ac)=0x%08x io0_ds(0xd434)=0x%08x io1_ds(0xd438)=0x%08x\n",
			   pi_rd(PI_PONIP_CTL_DS), pi_rd(PI_IO_CMD_0_DS), pi_rd(PI_IO_CMD_1_DS));
		seq_printf(s, "ds_nic: cfg_ds(0xc04c)=0x%08x[RX_SID=%u] rxcfg_ds(0xc044)=0x%08x media_ds(0xc058)=0x%08x rxfdp_ds(0xd3f0)=0x%08x\n",
			   pi_rd(PI_CFG_DS), pi_rd(PI_CFG_DS) & 0x7f, pi_rd(PI_RX_CFG_DS),
			   pi_rd(PI_MEDIA_STS_DS), pi_rd(PI_RXFDP1_DS));
		/* US-NIC. ★ EVERY ADDRESS PRINTED HERE IS THE ONE ACTUALLY ...
		 * dev/MEASURED-luna_gpon.c.md sec 177. */
		seq_printf(s, "us_nic: cfg_us(0x%05x)=0x%08x[RX_SID=%u] ctl_us(0x%05x)=0x%08x io0_us(0x%05x)=0x%08x io1_us(0x%05x)=0x%08x rxfdp_us(0x%05x)=0x%08x cnt_mask_us[omcc](0x%05x)=0x%08x\n",
			   PI_CFG_US, pi_rd(PI_CFG_US), pi_rd(PI_CFG_US) & 0x7f,
			   PI_PONIP_CTL_US, pi_rd(PI_PONIP_CTL_US),
			   PI_IO_CMD_0_US, pi_rd(PI_IO_CMD_0_US),
			   PI_IO_CMD_1_US, pi_rd(PI_IO_CMD_1_US),
			   PI_RXFDP1_US, pi_rd(PI_RXFDP1_US),
			   PI_CNT_MASK_US_WORD(0, GPON_OMCC_FLOW),
			   pi_rd(PI_CNT_MASK_US_WORD(0, GPON_OMCC_FLOW)));
		/* US-NIC per-group RX SID counters: good>0 confirms a ...
		 * dev/MEASURED-luna_gpon.c.md sec 178. */
		seq_printf(s, "us_rxsid[grp0..4]: good=%u/%u/%u/%u/%u bad=%u  (enrolled group is 0)\n",
			   pi_rd(PI_RX_SID_GOOD_CNT_US),
			   pi_rd(PI_RX_SID_GOOD_CNT_US + 1 * PI_SID_CNT_GROUP_STRIDE),
			   pi_rd(PI_RX_SID_GOOD_CNT_US + 2 * PI_SID_CNT_GROUP_STRIDE),
			   pi_rd(PI_RX_SID_GOOD_CNT_US + 3 * PI_SID_CNT_GROUP_STRIDE),
			   pi_rd(PI_RX_SID_GOOD_CNT_US + 4 * PI_SID_CNT_GROUP_STRIDE),
			   pi_rd(PI_RX_SID_BAD_CNT_US));
		/* ★ THE SPLIT THIS BOARD'S OPEN FAULT NEEDS: switch->PON-IP ingress
		 * (rx) beside PON->OLT egress (tx), for the same enrolled SID.  See
		 * PI_TX_SID_CNT_US for how to read the pair. */
		seq_printf(s, "us_sidcnt[grp0]: rx=%u tx=%u  (SID %u enrolled in CNT_MASK_US group 0)\n",
			   (u32)pi_rd(PI_RX_SID_CNT_US), (u32)pi_rd(PI_TX_SID_CNT_US),
			   GPON_OMCC_FLOW);
		/* The old "these bus-abort, never read them" note about ...
		 * dev/MEASURED-luna_gpon.c.md sec 179. */
		seq_printf(s, "us_arm: media_us(0x4058)=0x%08x io0_us(0x5434)=0x%08x gemus_map%u=0x%08x sidvld=0x%08x s2q=0x%08x s2q%u=%lu omcicfg=0x%08x\n",
			   pi_rd(PI_MEDIA_STS_US), pi_rd(PI_IO_CMD_0_US),
			   GPON_OMCC_FLOW,
			   gpon_rd(GPON_GEM_US_PORT_MAP + GPON_OMCC_FLOW * GEM_US_PORT_MAP_STRIDE),
			   pi_rd(PI_PON_SIDVALID + (GPON_OMCC_FLOW / 32u) * 4u),
			   pi_rd(PI_PON_SID2QID + (GPON_OMCC_FLOW / 4u) * PI_SID2QID_STRIDE),
			   GPON_OMCC_FLOW,
			   (pi_rd(PI_PON_SID2QID + (GPON_OMCC_FLOW / 4u) * PI_SID2QID_STRIDE)
			    >> ((GPON_OMCC_FLOW % 4u) * 7u)) & 0x7fUL,
			   pi_rd(PI_PON_OMCI_CFG));
		/* sched64: the OMCC queue's drain-side witnesses on one line. ...
		 * dev/MEASURED-luna_gpon.c.md sec 180. */
		{
			/* The word is the OMCC SID's and the latch is its own ...
			 * dev/MEASURED-luna_gpon.c.md sec 305. */
			unsigned int ow = (GPON_OMCC_FLOW / 32u) * 4u;
			unsigned int ob = GPON_OMCC_FLOW % 32u;

			seq_printf(s, "sched64: total_pg=%u over_sts%u=%u over_latch%u=%u bank_underfl=%u dis_dbru_latch=%u [over w0/1/2=%08x/%08x/%08x latch=%08x schopt=%08x gemintr=%08x]\n",
				   pi_rd(PI_PONIP_TOTAL_PAGE_CNT_US) & 0x1fffu,
				   GPON_OMCC_FLOW,
				   (pi_rd(PI_PONIP_SID_OVER_STS + ow) >> ob) & 1u,
				   GPON_OMCC_FLOW,
				   (pi_rd(PI_PONIP_SID_OVER_LATCH_STS + ow) >> ob) & 1u,
				   gpon_rd(GPON_GEM_US_INTR_STS) & 1u,
				   (pi_rd(PI_PON_SCH_OPT) >> 19) & 1u,
				   pi_rd(PI_PONIP_SID_OVER_STS), pi_rd(PI_PONIP_SID_OVER_STS + 4),
				   pi_rd(PI_PONIP_SID_OVER_STS + 8),
				   pi_rd(PI_PONIP_SID_OVER_LATCH_STS + ow), pi_rd(PI_PON_SCH_OPT),
				   gpon_rd(GPON_GEM_US_INTR_STS));
		}
		/* feed64: the US-feed / framer witnesses. dsc_sts carries ...
		 * dev/MEASURED-luna_gpon.c.md sec 306. */
		seq_printf(s, "feed64: dsc_sts=0x%08x[sram_used=%u dram_used=%u] sstart(0x5200)=%u feed_cnt=%u\n",
			   pi_rd(PI_PON_DSC_STS_US), pi_rd(PI_PON_DSC_STS_US) & 0x1fffu,
			   (pi_rd(PI_PON_DSC_STS_US) >> 16) & 0x1fffu,
			   gpon_rd(GPON_GTC_US_PROC_MODE) & 1u, gpon_us_feed_rearm_cnt);
		/* usdram: the AUTHORITATIVE US SRAM->DRAM staging state -- ...
		 * dev/MEASURED-luna_gpon.c.md sec 181. */
		seq_printf(s, "usdram: usage(0x20ec)=0x%08x[pipe_vld=%u] mstbase(0x20e8)=0x%08x dsccfg(0x215c)=0x%08x runout(0x20e0)=0x%08x ponipctl(0x20d8)=0x%08x[pbuf_en=%u]\n",
			   pi_rd(PI_PON_DSC_USAGE_US), (pi_rd(PI_PON_DSC_USAGE_US) >> 31) & 1u,
			   pi_rd(PI_IP_MSTBASE_US), pi_rd(PI_PON_DSC_CFG_US), pi_rd(PI_DSCRUNOUT_US),
			   pi_rd(PI_PONIP_CTL_US), pi_rd(PI_PONIP_CTL_US) & 1u);
		/* usintr: the GPON interrupt latch state -- the discriminator ...
		 * dev/MEASURED-luna_gpon.c.md sec 182. */
		seq_printf(s, "usintr: top[mask0x40=0x%08x sts0x44=0x%08x] gtcus[dlt=0x%08x mask=0x%08x sts=0x%08x] gemus[dlt=0x%08x mask=0x%08x sts=0x%08x] svc_cnt=%u\n",
			   gpon_rd(GPON_INTR_MASK), gpon_rd(GPON_INTR_STS),
			   gpon_rd(GPON_GTC_US_INTR_DLT), gpon_rd(GPON_GTC_US_INTR_MASK), gpon_rd(GPON_GTC_US_INTR_STS),
			   gpon_rd(GPON_GEM_US_INTR_DLT), gpon_rd(GPON_GEM_US_INTR_MASK), gpon_rd(GPON_GEM_US_INTR_STS),
			   gpon_us_intr_svc_cnt);
		/* PON-IP OMCI packet counters. Decoder: rx>0 with the NIC ...
		 * dev/MEASURED-luna_gpon.c.md sec 183. */
		seq_printf(s, "omci_pi: rx=%u drop=%u crcerr=%u ustx=%u trapcfg=0x%08x cpu_tx=%u cpu_drop=%u  (swcore OMCI block @0x%05x, PON_TRAP_CFG @0x%05x -- PER-CHIP)\n",
			   sw_rd(OMCI_RX_PKT_CNT), sw_rd(OMCI_DROP_PKT_CNT), sw_rd(OMCI_CRC_ERROR_PKT_CNT),
			   sw_rd(OMCI_TX_PKT_CNT), sw_rd(PON_TRAP_CFG),
			   rtl9602c_eth_omci_tx_dirty(), rtl9602c_eth_omci_tx_dropped(),
			   swc->omci_cnt, swc->pon_trap_cfg);
		/* The UNGATED US-NIC ingress witness, printed beside the two ...
		 * dev/MEASURED-luna_gpon.c.md sec 184. */
		seq_printf(s, "us_nic_cnt: rx_ok=%u tx_ok=%u err=%u miss=%u  (PI 0x04010/14/18, UNGATED)\n",
			   (u32)(pi_rd(PI_PKT_OK_CNT_US) & 0xffff),
			   (u32)((pi_rd(PI_PKT_OK_CNT_US) >> 16) & 0xffff),
			   (u32)pi_rd(PI_PKT_ERR_CNT_US), (u32)pi_rd(PI_PKT_MISS_CNT_US));
		/* GTC upstream grant accounting: if the ONU transmits ANY upstream GEM on
		 * the OLT's BWMAP grants these climb. */
		seq_printf(s, "us_tx: ploam_acpt(0x119c)=%u bwm_acpt(0x11b0)=%u bwm_fail(0x11a4)=%u bwm_inv(0x11a8)=%u | us_onu_id=%u ds_cfg(0x1014)=0x%08x\n",
			   gpon_rd(GPON_GTC_DS_MISC_CNTR_PLOAM_ACPT), gpon_bwm_acpt_since(&gpon_bwm_acpt_us_tx_last),
			   gpon_rd(GPON_GTC_DS_MISC_CNTR_BWM_FAIL), gpon_rd(GPON_GTC_DS_MISC_CNTR_BWM_INV),
			   (gpon_rd(GPON_GTC_US_ONU_ID) >> 8) & 0xff, gpon_rd(GPON_GTC_DS_CFG));
		/* Alloc CAM read-back: T-CONT 16 must hold alloc 0x400 (HIT) for the GTC to
		 * accept the OLT's operational BWMAP grant on that alloc-id. */
		{
			int a16 = gpon_alloc_cam_read(16);

			if (a16 < 0)
				seq_printf(s, "us_alloc: tc16=READ-FAILED rc=%d\n", a16);
			else
				seq_printf(s, "us_alloc: tc16=alloc0x%x hit%u\n", a16 & 0xfff, !!(a16 & BIT(16)));
		}
		/* bwmap: what the GTC BWMAP capture engine holds -- which ...
		 * dev/MEASURED-luna_gpon.c.md sec 185. */
		{
			u32 en = pi_rd(PI_PON_TCONT_EN);
			int i, nvalid = 0;

			gpon_wr(GPON_BWMAP_CTRL, gpon_rd(GPON_BWMAP_CTRL) | (1u << 15));	/* CAP_EN arm */
			seq_printf(s, "bwmap: ctrl(0x200c)=0x%08x sts(0x2010)=0x%08x[OVERFL=%lu] tcont_en=0x%08x alloc[0..2]=%08x/%08x %08x/%08x %08x/%08x\n",
				   gpon_rd(GPON_BWMAP_CTRL), gpon_rd(GPON_BWMAP_STS),
				   (gpon_rd(GPON_BWMAP_STS) >> 8) & 1UL, en,
				   /* allocation i = words 2i, 2i+1 of the 256-word array;
				    * word 0 is spelled as the base so the alias guard keeps
				    * its gpon_rd() window evidence for GPON_BWMAP_DATA */
				   gpon_rd(GPON_BWMAP_DATA), gpon_rd(BWMAP_DATA(1)),
				   gpon_rd(BWMAP_DATA(2)), gpon_rd(BWMAP_DATA(3)),
				   gpon_rd(BWMAP_DATA(4)), gpon_rd(BWMAP_DATA(5)));

			/* 32 allocations is what the vendor's own readers walk, of
			 * the 128 the address space holds; entries past 32 are
			 * never exercised by any vendor code, so we do not invent a
			 * meaning for them. */
			for (i = 0; i < 32; i++) {
				u32 w0 = gpon_rd(BWMAP_DATA(2 * i));	/* 8-byte allocation i */
				u32 w1 = gpon_rd(BWMAP_DATA(2 * i + 1));
				u8 tc, dmp[9];

				if (!(w0 & BIT(23)))		/* VALID */
					continue;
				nvalid++;
				tc = (u8)(w0 & 0x1f);
				if (en & BIT(tc))		/* configured: fine */
					continue;

				/* The dump is the whole allocation plus the
				 * bitmap it was judged against, so the line is
				 * self-contained: entry index, both captured
				 * words big-endian, and tcont_en. */
				dmp[0] = (u8)i;
				dmp[1] = (u8)(w0 >> 24); dmp[2] = (u8)(w0 >> 16);
				dmp[3] = (u8)(w0 >> 8);  dmp[4] = (u8)w0;
				dmp[5] = (u8)(en >> 24); dmp[6] = (u8)(en >> 16);
				dmp[7] = (u8)(en >> 8);  dmp[8] = (u8)en;
				gpon_unsup_report("bwmap_tcont", GPON_UNSUP_RANGE,
						  tc, "a-tcont-set-in-tcont_en",
						  dmp, sizeof(dmp));
				seq_printf(s, "bwmap_grant: entry%d -> T-CONT %u NOT in tcont_en=0x%08x (w0=%08x w1=%08x)\n",
					   i, tc, en, w0, w1);
			}
			seq_printf(s, "bwmap_scan: %d valid allocation(s) of 32 examined\n",
				   nvalid);
		}
		/* US GTC emission breakdown: PLOAM (idx2 cpu / idx3 auto) ...
		 * dev/MEASURED-luna_gpon.c.md sec 186. */
		seq_printf(s, "us_gtc: ploam_cpu=%u ploam_auto=%u | gem_byte=%u gem_dbru=%u | idle16=%u/%u idle8=%u gemus_omcc(f%u)=%u/%u gem2=%u(0x6810) | us_cfg=0x%04x pti=0x%08x\n",
			   gpon_us_misc_cnt(2), gpon_us_misc_cnt(3),
			   gpon_us_misc_cnt(4), gpon_us_misc_cnt(1),
			   gpon_rd(TCONT_IDLE_STAT(16)), gpon_rd(TCONT_IDLE_STAT(16) + 4),
			   gpon_rd(TCONT_IDLE_STAT(8)),
			   (unsigned int)GPON_OMCC_FLOW,
			   gpon_rd(GEM_US_STAT(GPON_OMCC_FLOW)),
			   gpon_rd(GEM_US_STAT(GPON_OMCC_FLOW) + 4),
			   gpon_rd(GEM_US_STAT(2)),
			   gpon_rd(GPON_GTC_US_CFG), gpon_rd(GPON_GEM_US_PTI_CFG));
		/* GEM_US_BYTE_STAT full scan (base 0x6800, stride 8): which flow does the
		 * port-2 OMCI actually land on? Non-zero on flow!=64 => SID-stamp/SID2QID
		 * misroute; all-zero => the US-NIC drops it before any flow (ingest gap). */
		{
			int f, n = 0; char gbuf[220];
			for (f = 0; f < 128 && n < 200; f++) {
				u32 gv = gpon_rd(GPON_GEM_US_BYTE_STAT + f * 8);
				if (gv)
					n += scnprintf(gbuf + n, sizeof(gbuf) - n, " f%d=%u", f, gv);
			}
			seq_printf(s, "gemus_scan:%s\n", n ? gbuf : " (all flows 0)");
		}
		/* US-scheduler readback: did the gpon_install_tcont writes ...
		 * dev/MEASURED-luna_gpon.c.md sec 187. */
		seq_printf(s, "us_sched: pirQ%u=0x%08x cirQ%u=0x%08x tcont_en=0x%08x qmapT%u=0x%08x wfqtypeQ%u=0x%08x wfqwtQ%u=0x%08x drn=0x%08x sch_ctrl=0x%08x[PIR_DROP=%lu]\n",
			   (unsigned int)GPON_OMCC_PHYS_QID,
			   pi_rd(PI_PON_QID_PIR_RATE + GPON_OMCC_PHYS_QID * PI_QID_RATE_STRIDE),
			   (unsigned int)GPON_OMCC_PHYS_QID,
			   pi_rd(PI_PON_QID_CIR_RATE + GPON_OMCC_PHYS_QID * PI_QID_RATE_STRIDE),
			   pi_rd(PI_PON_TCONT_EN),
			   (unsigned int)GPON_OMCC_TCONT,
			   pi_packed_get(PI_PON_SCH_QMAP, GPON_OMCC_TCONT, swc->sch_qmap_bits),
			   (unsigned int)GPON_OMCC_PHYS_QID,
			   pi_packed_get(PI_PON_WFQ_TYPE, GPON_OMCC_PHYS_QID, 1),
			   (unsigned int)GPON_OMCC_PHYS_QID,
			   pi_packed_get(PI_PON_WFQ_WEIGHT, GPON_OMCC_PHYS_QID, 10),
			   pi_rd(PI_DRN_CMD),
			   pi_rd(PI_PON_SCH_CTRL), (pi_rd(PI_PON_SCH_CTRL) >> 18) & 1UL);
		/* SID page-occupancy probe, an OLT-INDEPENDENT verify of the ...
		 * dev/MEASURED-luna_gpon.c.md sec 188. */
		{
			u32 pc;
			int prc = pi_sid_page_cnt(GPON_OMCC_FLOW, &pc);

			seq_printf(s, "sidpage_omcc: used=%u max=%u (max>0 = OMCI ENQUEUED to the OMCC queue) [r255c=0x%08x r2564=0x%08x poll=%d]\n",
				   pc & 0x1fff, (pc >> 16) & 0x1fff, pi_rd(PI_PONIP_DBG_CTRL_US), pc, prc);
		}
		/* Data-flow US datapath witness. The data flow rides the OMCC ...
		 * dev/MEASURED-luna_gpon.c.md sec 307. */
		{
			u32 pc1;
			int prc1;
			u32 s2q1 = (pi_rd(PI_PON_SID2QID) >> ((GPON_DATA_FLOW % 4) * 7)) & 0x7fu;
			u32 svl1 = (pi_rd(PI_PON_SIDVALID) >> GPON_DATA_FLOW) & 1u;

			/* Same strobe as sidpage_omcc above, different SID: one helper.
			 * pgpoll= is NEW and is the helper's rc -- this site used to
			 * compute the poll count and THROW IT AWAY, so a wedged strobe
			 * printed a stale pgbank1 that read exactly like a fresh one. */
			prc1 = pi_sid_page_cnt(GPON_DATA_FLOW, &pc1);
			/* `q32_idle` was REMOVED here (2026-09-10) and the removal is ...
			 * dev/MEASURED-luna_gpon.c.md sec 189. */
			seq_printf(s, "data1: s2q[1]=%u sidvld[1]=%u usmap1(0x%05x)=0x%x usbyte1(0x%05x)=%u pgbank1_used=%u max=%u pgpoll=%d\n",
				   s2q1, svl1,
				   GPON_GEM_US_PORT_MAP + GPON_DATA_FLOW * GEM_US_PORT_MAP_STRIDE,
				   gpon_rd(GPON_GEM_US_PORT_MAP + GPON_DATA_FLOW * GEM_US_PORT_MAP_STRIDE),
				   GEM_US_STAT(GPON_DATA_FLOW),
				   gpon_rd(GEM_US_STAT(GPON_DATA_FLOW)),
				   pc1 & 0x1fff, (pc1 >> 16) & 0x1fff,
				   prc1);
		}
		/* CAM read-back: does the DS GEM CAM actually map gem->flow 64 at runtime?
		 * e64 should read gem=2 (the OMCC) HIT=1; traffic_cfg[64] should be 0x4. */
		{
			int c64 = gpon_ds_cam_read(GPON_OMCC_FLOW), c0 = gpon_ds_cam_read(0);
			int c1 = gpon_ds_cam_read(GPON_DATA_FLOW);

			if (c64 < 0 || c0 < 0 || c1 < 0)
				seq_printf(s, "ds_cam: READ-FAILED e%u=%d e0=%d e%u=%d\n",
					   GPON_OMCC_FLOW, c64, c0, GPON_DATA_FLOW, c1);
			else
				seq_printf(s, "ds_cam: e%u=gem%u hit%u tcfg=0x%x | e0=gem%u hit%u | e%u(data)=gem%u hit%u tcfg=0x%x\n",
					   GPON_OMCC_FLOW, c64 & 0xfff, !!(c64 & BIT(16)),
					   gpon_rd(GPON_GTC_DS_TRAFFIC_CFG +
						   GPON_OMCC_FLOW * DS_TRAFFIC_CFG_STRIDE) & 0x1f,
					   c0 & 0xfff, !!(c0 & BIT(16)),
					   GPON_DATA_FLOW, c1 & 0xfff, !!(c1 & BIT(16)),
					   gpon_rd(GPON_GTC_DS_TRAFFIC_CFG + GPON_DATA_FLOW * 4) & 0x1f);
		}
	}
	/* Full per-flow de-encap sweep: does ANY flow de-encapsulate a GEM frame?
	 * If some flow > 0 => the OLT IS sending de-encodable GEM (find the OMCI
	 * flow). If NONE => frames arrive but de-encap is globally not happening
	 * (gem-port CAM / OMCI not classifying), or the OLT sends only idle GEM. */
	{
		int f, n = 0;

		seq_printf(s, "ds_deenc_sweep:");
		for (f = 0; f < 128; f++) {
			u32 v = gpon_gem_flow_cnt(f, 0);

			if (v && v != 0xffffffffu) {
				seq_printf(s, " f%d=%u", f, v);
				n++;
			}
		}
		seq_printf(s, "%s\n", n ? "" : " (NONE de-encap)");
	}
	/* The "operational" reference values are Board C's (RTL9602C) -- printing
	 * them beside another chip's reading invites a diff that means nothing, so
	 * they appear only on the chip they were measured on. Where the chip
	 * declares no IO_GPIO_EN the words are not read at all. */
	if (SOC_IO_GPIO_EN)
		seq_printf(s, "io: io_mode_en=0x%08x@0x%05x gpio_en0=0x%08x gpio_en1=0x%08x%s\n",
			   sw_rd(SOC_IO_MODE_EN), swc->io_mode_en,
			   sw_rd(SOC_IO_GPIO_EN), sw_rd(SOC_IO_GPIO_EN + 4),
			   swc == &gpon_swc_9602c
				? "  (operational 0x12050/0x40202006/0x819)" : "");
	else
		seq_printf(s, "io: io_mode_en=0x%08x@0x%05x gpio_en=n/a (%s declares no optical GPIO pad map)\n",
			   sw_rd(SOC_IO_MODE_EN), swc->io_mode_en, swc->chip);
	seq_printf(s, "io: oem_en(bit%u)=%d i2c_en_bus0(bit%u)=%d\n",
		   swc->io_oem_en, !!(sw_rd(SOC_IO_MODE_EN) & IO_OEM_EN),
		   swc->io_i2c_en_bus0,
		   !!(sw_rd(SOC_IO_MODE_EN) & (1u << swc->io_i2c_en_bus0)));

	/*
	 * SerDes RX/analog readback against the known-good O5 values with light
	 * present. Mismatches here explain an RX that will not lock.
	 */
	seq_printf(s, "sds: dig00=0x%08x dig1d=0x%08x  (golden 0xf30 / 0x1c000)\n",
		   sw_rd(WSDS_DIG_00), sw_rd(WSDS_DIG_1D));
	seq_printf(s, "sds: com03=0x%08x com26=0x%08x gpon42=0x%08x dig18=0x%08x@0x%05x\n",
		   sw_rd(SDS_ANA_COM_REG03), sw_rd(SDS_ANA_COM_REG26),
		   sw_rd(SDS_ANA_GPON_REG42), sw_rd(WSDS_DIG_18), swc->wsds_dig_18);
	/* ★ THE ONE LINE THAT SAYS WHETHER optic_los IS SAMPLED OR ...
	 * dev/MEASURED-luna_gpon.c.md sec 308. */
	{
		u32 d18 = sw_rd(WSDS_DIG_18);

		seq_printf(s, "optic_los_src: %s (dig18 frc=%d frcv=%d sel_epon=%d ben_oe=%d)\n",
			   (d18 & WSDS_FRC_OPTIC_LOS) ? "FORCED -- the pad is NOT being sampled"
						      : "the pad (sampled)",
			   !!(d18 & WSDS_FRC_OPTIC_LOS), !!(d18 & WSDS_FRCV_OPTIC_LOS),
			   !!(d18 & WSDS_OPTIC_LOS_SEL_EPON), !!(d18 & BIT(12)));
	}
	seq_printf(s, "sds: misc00=0x%08x misc01=0x%08x misc02=0x%08x fib21=0x%08x\n",
		   sw_rd(SDS_ANA_MISC_REG00), sw_rd(SDS_ANA_MISC_REG01),
		   sw_rd(SDS_ANA_MISC_REG02), sw_rd(FIB_EXT_REG21));
	/* TX-path verification: confirm my TX serializer writes actually landed. */
	seq_printf(s, "sds_tx: cfg=0x%08x com22_txamp=0x%08x reg24=0x%08x dig1e_d2a=0x%08x dig02_pdben=0x%08x dig03_txdis=0x%08x forceben=0x%08x\n",
		   sw_rd(SDS_CFG), sw_rd(SDS_ANA_COM_REG22), sw_rd(SDS(0x225e0)),
		   sw_rd(SDS(0x220a8)), sw_rd(WSDS_DIG_02), sw_rd(WSDS_DIG_03), sw_rd(SDS(0x220e4)));
	if (SDS_FIB_STATUS) {
		u32 fibsts = sw_rd(SDS_FIB_STATUS);

		seq_printf(s, "sds: fib_status=0x%08x@0x%05x (sds_sdet=%u fib100_sdet=%u link_ok=%u) fib_reg16=0x%08x@0x%05x\n",
			   fibsts, swc->sds_fib_status, !!(fibsts & SDS_FIB_SDS_SDET),
			   !!(fibsts & BIT(2)), !!(fibsts & BIT(4)),
			   sw_rd(FIB_REG16), swc->fib_reg16);
	} else {
		seq_printf(s, "sds: fib_status=n/a (%s declares no SDS_FIB_STATUS in this driver)\n",
			   swc->chip);
	}
	/* The probe's own cache is subject to the same rule as the live reads: these
	 * five come from bosa_probe(), which reads the RTL8290B pages, so on a module
	 * that does not carry them they are the same phantom. */
	if (bosa_regs_live())
		seq_printf(s, "bosa: rtl8290b num=0x%04x vid=0x%02x w41=0x%02x ctrl2=0x%02x status2=0x%02x\n",
			   bosa_id_num, bosa_id_vid, bosa_w41, bosa_ctrl2,
			   bosa_status2);
	else
		seq_puts(s, "bosa: n/a -- the RTL8290B chip-ID read did not complete, so nothing this driver holds about that device is a reading\n");
	seq_printf(s, "fsm: state=O%u onu_id=%u sn_tx=%u ds_rx=%u sds_sync=%u ticks=%u sn=%*phN\n",
		   gpon_fsm_state, luna_ploam.onu_id, luna_ploam.sn_tx, gpon_ds_rx,
		   gpon_sds_synced, gpon_fsm_ticks, 8, gpon_sn_bytes);
	/* Live BOSA laser-emission status: R30 (txsd/valid/apc-done/mpd-fault),
	 * R33 bias-DAC readback (nonzero => bias driven), R32 mod, FAULT_STATUS. */
	if (!bosa_regs_live()) {
		char idbuf[16];

		if (bosa_id_num < 0)
			strscpy(idbuf, "COULD NOT ASK", sizeof(idbuf));
		else
			scnprintf(idbuf, sizeof(idbuf), "0x%04x", bosa_id_num);
		seq_printf(s, "bosa_regs: n/a -- this module exposes no RTL8290B register interface (chip id %s, want 0x8290; I2C slaves 0x54/0x55 do not answer). The RTL8290B-derived lines are SUPPRESSED rather than printed as values.\n",
			   idbuf);
	} else {
		int r30 = bosa_read_reg(0x31e), r33 = bosa_read_reg(0x321);
		int r32 = bosa_read_reg(0x320), fault = bosa_read_reg(0x389);

		seq_printf(s, "bosa_tx: R30=0x%02x txsd=%d valid=%d apc_done=%d mpd_vhi=%d mpd_vlo=%d bias=0x%02x mod=0x%02x fault=0x%02x\n",
			   r30 & 0xff, !!(r30 & BIT(6)), !!(r30 & BIT(5)),
			   !!(r30 & BIT(7)), !!(r30 & BIT(3)), !!(r30 & BIT(2)),
			   r33 & 0xff, r32 & 0xff, fault & 0xff);
	}
	/* Fibre optical power. The words at 0x166-0x169 are NOT live ...
	 * dev/MEASURED-luna_gpon.c.md sec 309. */
	{
		/* RX optical power: the live RSSI ADC byte at 0x311 (slave ...
		 * dev/MEASURED-luna_gpon.c.md sec 310. */
		int rxc = bosa_read_reg(0x311) & 0xff;	/* raw 8-bit RSSI tap: kept as instrument */
		u32 rx_code = bosa_rx_code();
		u32 rssi_uv = bosa_rssi_uv();			/* ratiometric fraction*10000, info */
		int txw = bosa_read16_median(0x166);
		char cdbm_s[16];

		/* Same rule as the "fiber:" line: a chain that produced no reading
		 * prints n/a, never the -4000 floor. */
		if (rx_code == BOSA_RX_CODE_NA)
			strscpy(cdbm_s, "n/a", sizeof(cdbm_s));
		else
			scnprintf(cdbm_s, sizeof(cdbm_s), "%d",
				  bosa_code_to_cdbm(rx_code));
		if (bosa_regs_live()) {
			seq_printf(s, "optic_rx_raw: 0x%02x optic_rx_cdbm: %s optic_tx_raw: 0x%04x\n",
				   rxc, cdbm_s, txw & 0xffff);
			seq_printf(s, "optic_env:   temp_dc=%d bias_ua=%u tx_cdbm=%d\n",
				   bosa_temp_dc(), bosa_bias_ua(), bosa_tx_power_cdbm());
		} else {
			bosa_a2h_show(s);
		}
		/* ★ The module's OWN optical monitor, raw, beside the derived dBm --
		 * the witness that is independent of the SoC pad mux and the SerDes.
		 * Printed unconditionally so the raw words are visible even when the
		 * conversion declines to produce a number. */
		{
			int a2h = bosa_i2c_read8(0x51, 104);
			int a2l = bosa_i2c_read8(0x51, 105);
			s32 a2c = bosa_rx_power_sff8472_cdbm();
			char a2s[16];

			if (a2c == BOSA_RX_CDBM_NA)
				strscpy(a2s, "n/a", sizeof(a2s));
			else
				scnprintf(a2s, sizeof(a2s), "%d.%02ddBm", a2c / 100,
					  (a2c < 0 ? -a2c : a2c) % 100);
			seq_printf(s, "optic_a2:    rx_pwr_raw=%02x%02x (0.1uW) -> %s  [SFF-8472 A2 slave 0x51 b104/105 -- the MODULE's own monitor]\n",
				   a2h < 0 ? 0xff : a2h & 0xff,
				   a2l < 0 ? 0xff : a2l & 0xff, a2s);
			/* The rest of the same page, and on a module with no RTL8290B ...
			 * dev/MEASURED-luna_gpon.c.md sec 311. */
			{
				int k, b[8];
				char t[8][8];

				for (k = 0; k < 8; k++) {
					b[k] = bosa_i2c_read8(0x51, 96 + k);
					if (b[k] < 0)
						strscpy(t[k], "n/a", sizeof(t[k]));
					else
						scnprintf(t[k], sizeof(t[k]), "%02x", b[k]);
				}
				seq_printf(s, "optic_a2_env: temp=%s%s vcc=%s%s tx_bias=%s%s tx_pwr=%s%s  [b96..b103, raw]\n",
					   t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7]);
			}
		}
		if (bosa_regs_live()) {
			s32 v = bosa_vmpd_mv(0x3B3);	/* one live MPD sample -> latch its taps */

			seq_printf(s, "optic_txchain: dark=%d vmpd=%d code=%u hi=%d zero=%d iavg=%02x range=%d\n",
				   bosa_vmpd_dark, v, bosa_tx_dbg_code, bosa_tx_dbg_hi,
				   bosa_tx_dbg_zero, bosa_read_reg(0x23A) & 0xff,
				   (bosa_read_reg(0x246) >> 6) & 3);
		}
		/* Full RX chain intermediates: a HW read at a known attenuation pins the exact
		 * ref-tap roles + rx_thr against the -14 dBm anchor (code 398 = 0.1uW at -14 dBm). */
		if (bosa_regs_live())
			seq_printf(s, "optic_rxchain: rssi_uv=%u code=%u cdbm=%s | adcA=%06x ref305=%06x ref314=%06x\n",
				   rssi_uv, rx_code, cdbm_s, bosa_read24(0x30e), bosa_read24(0x305), bosa_read24(0x314));
		if (bosa_regs_live())
			seq_printf(s, "optic_dbg: 30c=%02x 30d=%02x 30e=%02x 30f=%02x 310=%02x 311=%02x 312=%02x | 166=%02x 167=%02x 168=%02x 169=%02x\n",
			   bosa_read_reg(0x30c) & 0xff, bosa_read_reg(0x30d) & 0xff,
			   bosa_read_reg(0x30e) & 0xff, bosa_read_reg(0x30f) & 0xff,
			   bosa_read_reg(0x310) & 0xff, bosa_read_reg(0x311) & 0xff,
			   bosa_read_reg(0x312) & 0xff, bosa_read_reg(0x166) & 0xff,
			   bosa_read_reg(0x167) & 0xff, bosa_read_reg(0x168) & 0xff,
			   bosa_read_reg(0x169) & 0xff);
		/* ANI-G ME263 #10/#14 cached levels (OMCI 0.002 dB, 2's-comp) reported to
		 * the OLT — refreshed from the DDM words on the FSM tick. */
		seq_printf(s, "anig_rx_level: 0x%04x anig_tx_level: 0x%04x\n",
			   (u16)anig_rx_level, (u16)anig_tx_level);
	}
	/* BOSA page0 (slave 0x50) control regs — compare against the O5 operating
	 * state to find the APCDIG clock/power enable (O5 p0: 02 04 0b ff ff ff ff 0c
	 * .. 52 54 20). */
	seq_printf(s, "bosa_p0: 00=%02x 01=%02x 02=%02x 03=%02x 04=%02x 05=%02x 08=%02x 0c=%02x 10=%02x 14=%02x 18=%02x 1c=%02x\n",
		   bosa_read_reg(0x00) & 0xff, bosa_read_reg(0x01) & 0xff,
		   bosa_read_reg(0x02) & 0xff, bosa_read_reg(0x03) & 0xff,
		   bosa_read_reg(0x04) & 0xff, bosa_read_reg(0x05) & 0xff,
		   bosa_read_reg(0x08) & 0xff, bosa_read_reg(0x0c) & 0xff,
		   bosa_read_reg(0x10) & 0xff, bosa_read_reg(0x14) & 0xff,
		   bosa_read_reg(0x18) & 0xff, bosa_read_reg(0x1c) & 0xff);
	if (bosa_regs_live()) {
		seq_printf(s, "bosa_p3: 31c=%02x 31d=%02x 31f=%02x 322=%02x 323=%02x (O5 00 33 00 da 00)\n",
			   bosa_read_reg(0x31c) & 0xff, bosa_read_reg(0x31d) & 0xff,
			   bosa_read_reg(0x31f) & 0xff, bosa_read_reg(0x322) & 0xff,
			   bosa_read_reg(0x323) & 0xff);
		seq_printf(s, "bosa_p2: W54_236=%02x W56_238=%02x W57_239=%02x W61_24d=%02x W88_284=%02x (O5 19 22 2d b0 76)\n",
			   bosa_read_reg(0x236) & 0xff, bosa_read_reg(0x238) & 0xff,
			   bosa_read_reg(0x239) & 0xff, bosa_read_reg(0x24d) & 0xff,
			   bosa_read_reg(BOSA_W(88)) & 0xff);
		seq_printf(s, "bosa_apc: W69_245=%02x(loopmode) W58_23a=%02x(iavg) W72_248=%02x(biasmax) W73_249=%02x(biasmin 0x2a)\n",
			   bosa_read_reg(0x245) & 0xff, bosa_read_reg(0x23a) & 0xff,
			   bosa_read_reg(0x248) & 0xff, bosa_read_reg(0x249) & 0xff);
	}
	/* SerDes/serializer run-state vs LIVE working-stock O5 ...
	 * dev/MEASURED-luna_gpon.c.md sec 312. */
	seq_printf(s, "sds_run: 280c=%04x[3106] 281c=%04x[1359] 225a0=%04x[713] 220a8=%04x[2] 225d8=%04x[29]\n",
		   sw_rd(SDS(0x2280c)) & 0xffff, sw_rd(SDS(0x2281c)) & 0xffff,
		   sw_rd(SDS_ANA_COM_REG08) & 0xffff, sw_rd(SDS(0x220a8)) & 0xffff,
		   sw_rd(SDS_ANA_COM_REG22) & 0xffff);
	seq_printf(s, "sds_ext: 22a2c=%04x[0] 22a30=%04x[4] 22a34=%04x[326a]\n",
		   sw_rd(SDS(0x22a2c)) & 0xffff, sw_rd(SDS(0x22a30)) & 0xffff,
		   sw_rd(SDS(0x22a34)) & 0xffff);
	return 0;
}

/* FSM state exposed to /proc (defined with the FSM below). ...
 * dev/MEASURED-luna_gpon.c.md sec 190. */
static void gpon_parse_sn_into(u8 *out, const char *s)
{
	if (!s || !*s)
		return;		/* nothing provisioned yet: not an error, leave `out` alone */
	if (gpon_sn_parse(s, out))
		pr_warn("gpon: ONU-SN \"%s\" is not 4 ID chars + 8 hex digits -- keeping the serial in force\n",
			s);
}

static void gpon_parse_sn(const char *s)
{
	gpon_parse_sn_into(gpon_sn_bytes, s);
}

/* True when `s` decodes to a DIFFERENT ONU-SN than the one in force.  The
 * caller uses this to decide whether a write is an identity change (re-range)
 * or a rewrite of the same serial (do nothing). */
static bool gpon_sn_differs(const char *s)
{
	u8 want[8];

	/* Match gpon_parse_sn_into()'s "leave untouched what the string does not
	 * supply" behaviour: seed from the serial in force, so a SHORT string
	 * compares as equal exactly when it leaves every byte alone. */
	memcpy(want, gpon_sn_bytes, sizeof(want));
	gpon_parse_sn_into(want, s);
	return memcmp(want, gpon_sn_bytes, sizeof(want)) != 0;
}

/* Compose and transmit a 12-byte upstream PLOAM on the given ...
 * dev/MEASURED-luna_gpon.c.md sec 191. */
static void gpon_send_cpu_ploam(u8 queue, const u8 m[12])
{
	u32 ind, full = queue == PLM_US_QUEUE_URG ? GPON_US_PLM_URG_FULL : 0;
	int i;

	if (!READ_ONCE(luna_activation_ready))
		return;

	/* The CPU US-PLOAM path has a SINGLE transmit buffer and a ...
	 * dev/MEASURED-luna_gpon.c.md sec 314. */
	for (i = 0; i < 1000; i++) {
		if (!(gpon_rd(GPON_GTC_US_PLOAM_IND) & GPON_US_PLM_ENQ))
			break;
		udelay(5);
	}

	ind = gpon_rd(GPON_GTC_US_PLOAM_IND);
	ind &= ~((0x7u << GPON_US_PLM_TYPE_SHIFT) | GPON_US_PLM_ENQ);
	ind |= ((u32)queue << GPON_US_PLM_TYPE_SHIFT);		/* select queue, ENQ=0 */
	gpon_wr(GPON_GTC_US_PLOAM_IND, ind);

	/* Never write a FULL queue, as stock refuses (RT_ERR_GPON_PLOAM_QUEUE_FULL):
	 * the single data buffer would overwrite the message still waiting for a
	 * PLOAMu grant. Give the grants a bounded spell to drain it, else drop. */
	for (i = 0; full && (gpon_rd(GPON_GTC_US_PLOAM_IND) & full) && i < 400; i++)
		udelay(5);
	if (full && (gpon_rd(GPON_GTC_US_PLOAM_IND) & full)) {
		luna_us_ploam_full_drops++;
		pr_warn_ratelimited("luna-gpon: US PLOAM queue %u still full after 2 ms: type 0x%02x dropped\n",
				    queue, m[1]);
		return;
	}

	for (i = 0; i < 6; i++)
		gpon_wr(GPON_GTC_US_PLOAM_DATA + i * 4,
			((u32)m[2 * i] << 8) | m[2 * i + 1]);
	/* The comment said 0x13 and the expression made 0x03 (fixed ...
	 * dev/MEASURED-luna_gpon.c.md sec 192. */
	gpon_wr(GPON_GTC_US_PLOAM_CFG, GPON_US_PLM_CFG_REST);	/* 0x13, as stock rests */

	ind |= GPON_US_PLM_ENQ;			/* ENQ 0->1 edge: transmit */
	gpon_wr(GPON_GTC_US_PLOAM_IND, ind);
	if (queue != PLM_US_QUEUE_DYING_GASP && !gpon_ploam_us_repetitive(m[1]))
		luna_ploam_capture(GPON_OLT_US, m, GPON_PLOAM_US_LEN);

	/* DBG (ploam_tx_dbg): did the HW actually TRANSMIT this CPU ...
	 * dev/MEASURED-luna_gpon.c.md sec 315. */
	if (ploam_tx_dbg) {
		static int dbgn;
		int j, cleared = 0;
		u32 i2;

		for (j = 0; j < 400; j++) {	/* up to ~2ms */
			i2 = gpon_rd(GPON_GTC_US_PLOAM_IND);
			if (!(i2 & GPON_US_PLM_ENQ)) { cleared = 1; break; }
			udelay(5);
		}
		if (dbgn++ < 40)
			luna_log("PLM_TX q=%u enq_cleared=%d(%dus) IND=0x%08x urg_e=%d urg_f=%d nrm_e=%d nrm_f=%d cputx=%u autotx=%u\n",
				queue, cleared, j * 5, i2,
				!!(i2 & GPON_US_PLM_URG_EMPTY), !!(i2 & GPON_US_PLM_URG_FULL),
				!!(i2 & GPON_US_PLM_NRM_EMPTY), !!(i2 & GPON_US_PLM_NRM_FULL),
				gpon_us_misc_cnt(2), gpon_us_misc_cnt(3));
	}
}


/* Respond to a downstream Request_Password (0x09) with the US ...
 * dev/MEASURED-luna_gpon.c.md sec 193. */
static void gpon_aes_stage_key(const u8 *key)
{
	int idx, i;

	gpon_wr(0x3010, 0x0000);			/* CFG_ACTIVE_KEY=0 (staged), REQ=0     */
	gpon_wr(0x3010, 0x8000);			/* KEY_CFG_REQ 0->1: config staged bank */
	for (idx = 0; idx < 8; idx++) {
		gpon_wr(0x3024, ((u32)key[2 * idx] << 8) | key[2 * idx + 1]);
		gpon_wr(0x3020, idx);			/* KEY_WORD_IDX=idx, KEY_WR_REQ=0       */
		gpon_wr(0x3020, idx | 0x8000);		/* KEY_WR_REQ 0->1: latch word idx      */
		for (i = 0; i < 200; i++) {		/* bounded; timeout != success          */
			if (gpon_rd(0x3020) & BIT(14))	/* KEY_WR_COMPL                         */
				break;
			udelay(5);
		}
	}
}


/* OMCI channel (OMCC) GEM datapath. After ranging the OLT ...
 * dev/MEASURED-luna_gpon.c.md sec 194. */
static_assert(GPON_GEM_US_RANGE_OK(GPON_OMCC_FLOW_9602C, 1u, GEM_US_PORT_MAP_IDX_MAX),
	      "the RTL9602C OMCC flow index runs past the US port-map array");
static_assert(GPON_GEM_US_RANGE_OK(GPON_OMCC_FLOW_9603CVD, 1u, GEM_US_PORT_MAP_IDX_MAX),
	      "the RTL9603CVD OMCC flow index runs past the US port-map array");
static_assert(GPON_GEM_US_RANGE_OK(GPON_OMCC_FLOW_9607C, 1u, GEM_US_PORT_MAP_IDX_MAX),
	      "the RTL9607C OMCC flow index runs past the US port-map array");
static_assert(GPON_GEM_US_RANGE_OK(GPON_DATA_FLOW, 1u, GEM_US_PORT_MAP_IDX_MAX),
	      "the data flow index runs past the US port-map array");
/* GPON_DATA_FLOW(1) / the OLT's gem-port-id — the WAN data GEM (clean-room nas0-equivalent) —
 * are defined in luna_gpon_nic.h (shared with the eth driver's gpon0 TX descriptor). */
#define GPON_OMCC_DSQ_HIGH	2

/* Dedicated DATA T-CONT for the WAN data GEM. Stock rides it ...
 * dev/MEASURED-luna_gpon.c.md sec 316. */
#define GPON_DATA_ALLOC		256u	/* OLT data Alloc-ID for THIS OLT (consistent; from ME262) */
static int gpon_install_tcont(u8 tcont, u16 alloc);	/* fwd: data-GEM install binds the data T-CONT */
/* fwd: the Assign_Alloc-ID handler binds the data T-CONT ...
 * dev/MEASURED-luna_gpon.c.md sec 195. */
static int gpon_omci_rx_cnt(void)
{
	u32 v = rtl9602c_eth_omci_rx_count();

	return v == GPON_OMCI_RX_UNAVAIL ? -1 : (int)v;
}

/* Set a `bits`-wide entry at index `idx` in the packed ...
 * dev/MEASURED-luna_gpon.c.md sec 196. */
static void pi_packed_set(u32 base, unsigned int idx, unsigned int bits, u32 val)
{
	struct pi_packed_slot slot = pi_packed_locate(base, idx, bits);

	pi_wr(slot.reg, pi_packed_insert(pi_rd(slot.reg), &slot, val));
}

/* pi_packed_set() of `val` into every entry below n not in `keep`, each register
 * word read and written once: the same end state, a 128-entry classify table in
 * 36 accesses instead of 512 (~0.5 ms before a Configure_Port-ID's Acknowledge). */
static void pi_packed_fill(u32 base, unsigned int n, unsigned int bits, u32 val,
			   const unsigned long *keep)
{
	unsigned int i = 0;

	while (i < n) {
		struct pi_packed_slot s = pi_packed_locate(base, i, bits);
		u32 reg = s.reg, w = pi_rd(reg);

		for (; i < n && (s = pi_packed_locate(base, i, bits)).reg == reg; i++)
			if (!test_bit(i, keep))
				w = pi_packed_insert(w, &s, val);
		pi_wr(reg, w);
	}
}

/* Read the `bits`-wide entry back through the SAME locate, so a /proc readback
 * shows the TRUE value -- the old contiguous read showed the wrong SID's, which
 * is why the SID2QID mis-addressing stayed invisible across many boots. */
static u32 pi_packed_get(u32 base, unsigned int idx, unsigned int bits)
{
	struct pi_packed_slot slot = pi_packed_locate(base, idx, bits);

	return pi_packed_extract(pi_rd(slot.reg), &slot);
}

/* Faithful port of the stock PON-MAC GPON mode-set branch, ...
 * dev/MEASURED-luna_gpon.c.md sec 197. */
static void rtl9602c_ponmac_modeset_gpon(bool keep_data)
{
	DECLARE_BITMAP(keep, LUNA_CLASSIFY_SID_MAX) = { 0 };

	if (!ponmac_modeset)
		return;

	/* (1) all-SID classify invalidation pre-pass, so the US-NIC resolves the
	 * OMCC SID cleanly at the re-latch, as stock does. */
	__set_bit(GPON_OMCC_FLOW, keep);
	if (keep_data) {
		__set_bit(GPON_DATA_FLOW, keep);
		if (luna_ctrl_flow_wanted())
			__set_bit(GPON_CTRL_FLOW, keep);
	}
	pi_packed_fill(PI_PON_SIDVALID, swc->classify_sid_num, 1, 0, keep);
	pi_packed_fill(PI_PON_SID2QID, swc->classify_sid_num, 7, swc->scratch_phys_qid & 0x7f, keep);

	/* (2) OMCI classify triple, in stock order (SID2QID -> SIDVALID -> OMCI_CFG). */
	pi_packed_set(PI_PON_SID2QID, GPON_OMCC_FLOW, 7, GPON_OMCC_PHYS_QID & 0x7f);
	pi_packed_set(PI_PON_SIDVALID, GPON_OMCC_FLOW, 1, 1);
	pi_field(PI_PON_OMCI_CFG, 6, 0, GPON_OMCC_FLOW);

	/* (2b) WAN data-GEM classify, re-asserted every modeset -- the all-SID loop
	 * above would otherwise wipe it, the same trap that hid the OMCC SID for ~30
	 * rounds. SID2QID is the data qid so the data US is reported to the OLT's DBA
	 * on the data Alloc; the legacy path rides the OMCC's T-CONT 16. */
	if (keep_data) {
		pi_packed_set(PI_PON_SID2QID, GPON_DATA_FLOW, 7,
			      luna_data_qid & 0x7f);
		pi_packed_set(PI_PON_SIDVALID, GPON_DATA_FLOW, 1, 1);
		if (luna_ctrl_flow_wanted()) {
			pi_packed_set(PI_PON_SID2QID, GPON_CTRL_FLOW, 7,
				      (luna_data_qid + 1) & 0x7f);
			pi_packed_set(PI_PON_SIDVALID, GPON_CTRL_FLOW, 1, 1);
		}
	}

	/* (3) PON-port + CPU-port RX_SPC: accept the sub-64B OMCI frame. Both the
	 * ports and the per-port stride are this chip's -- see SW_P_MISC_PORT(). */
	sw_field(SW_P_MISC_PORT(swc->sw, swc->sw->pon_port), 2, 2, 1);
	sw_field(SW_P_MISC_PORT(swc->sw, swc->sw->cpu_port), 2, 2, 1);

	/* (4) [risky, separately gated] SerDes RE-COMMIT: stock re-runs the SDS mode
	 * cycle inside the same window so the commit and the SID latch share one
	 * ordered window. Pulses CMD_SDS_RST_PS and can drop the locked DS framer. */
	if (serdes_recommit) {
		sw_field(SDS_CFG, 4, 0, SDS_MODE_OFF);		/* park = 0x1f      */
		sw_wr(WSDS_DIG_01, 0);
		sw_field(SW_SOFTWARE_RST, 0, 0, 1);		/* CMD_SDS_RST_PS   */
		mdelay(10);
		sw_field(WSDS_DIG_18, 12, 12, 1);		/* BEN_OE = 1       */
		sw_field(SDS_CFG, 4, 0, SDS_MODE_GPON);		/* commit GPON=0x08 */
	}

	/* (5) GMII re-latch: pulse GMII_RX_EN off->on so the US-NIC RX engine latches
	 * the now-complete SID classify table. This is the edge that was missing. */
	pi_wr(PI_IO_CMD_0_US, 0x90101050u);	/* GMII off */
	udelay(50);
	pi_wr(PI_IO_CMD_0_US, 0x90101070u);	/* GMII on -> rising edge latches classify */

	luna_log("ponmac_modeset_gpon: re-latched US-NIC SID classify (serdes_recommit=%u)\n",
		serdes_recommit);
}

/* The OMCC GEM Port-ID currently mapped to the management flow, or 0 before one
 * is. Read by the DS OMCI snoop so the core's data-GEM decision can refuse to
 * adopt the MANAGEMENT gem as the WAN one -- the refusal that would otherwise
 * point user traffic at the OMCC. */
static u16 gpon_omcc_gem_port;

static int gpon_install_omcc(u16 gem)
{
	int rc;

	/* the 12-bit G.984.3 wire width is the core's, once (gpon_gem_us.h):
	 * this site spelled it by hand, so a change to the field would have
	 * had to be made in two places to be made at all. */
	gpon_omcc_gem_port = gpon_gem_us_port_id(gem);

	/* Retire the owned data queue before a global table reset can erase its
	 * ownership evidence. Failed cleanup leaves Configure_Port-ID retryable. */
	rc = luna_data_retire();
	if (rc)
		return rc;
	rc = !luna_ds_cam_scrubbed ? gpon_ds_cam_clear_all() :
	     gpon_ds_cam_clean(GPON_DATA_FLOW) ?: gpon_ds_cam_clean(GPON_MCAST_FLOW);
	if (rc)
		return rc;
	luna_ds_cam_scrubbed = true;

	/* DS GEM-port CAM: map gem -> the OMCC flow, mark isOMCI. One indirect-CAM
	 * transaction through the shared gpon_gtc_ds_port_write() reading
	 * luna_gpon_chip; same addresses, values and access counts as the
	 * hand-spelled form, proven by gpon_regtable_diff_test. */
	rc = luna_port_cam_write(GPON_OMCC_FLOW, gem);
	if (rc < 0) {
		pr_err("luna-gpon: OMCC DS GEM install timeout\n");
		return rc;
	}
	gpon_wr(GPON_GTC_DS_TRAFFIC_CFG + GPON_OMCC_FLOW * DS_TRAFFIC_CFG_STRIDE,
		DS_TRAFFIC_IS_OMCI);

	/* DS OMCI PTI: tell the GTC how to detect the end of an OMCI GEM frame for
	 * reassembly. At reset this register is 0, so the GTC never recognises an OMCI
	 * frame boundary and drops every downstream OMCI frame. */
	gpon_wr(GPON_GTC_DS_OMCI_PTI, DS_OMCI_PTI_VAL);
	/* A live online stock ONU sets the ADJACENT DS-PTI registers to the same value
	 * -- GPON_GTC_DS_TDM_PTI and GPON_GTC_DS_ETH_PTI. Setting only the OMCI one
	 * produced NOTHING from the DS de-encap: reassembly needs all three. */
	gpon_wr(GPON_GTC_DS_TDM_PTI, DS_OMCI_PTI_VAL);	/* stock O5 = 0x11 */
	gpon_wr(GPON_GTC_DS_ETH_PTI, DS_OMCI_PTI_VAL);	/* stock O5 = 0x11 */

	/* GEM DS pass config: WITHOUT NON_MULTICAST_PASS the GTC ...
	 * dev/MEASURED-luna_gpon.c.md sec 198. */
	gpon_wr(GPON_GEM_DS_MC_CFG, GEM_DS_MC_CFG_VAL);

	/* GEM-DS reassembly flush/forward timer. The RESET default has OMCI_TR_MODE=1
	 * and stock only FIELD-writes the assemble timer, preserving it; a full write
	 * of 0x10 CLEARS OMCI_TR_MODE, and with OMCI transparent mode off the DS GEM
	 * de-assembler does not pass OMCI frames at all. Field-write only. */
	gpon_field(GPON_GEM_DS_FRM_TIMEOUT, 8, 8, 1);	/* OMCI_TR_MODE = 1 (stock reset default) */
	gpon_field(GPON_GEM_DS_FRM_TIMEOUT, 4, 0, 16);	/* ASSM_TIMEOUT_FRM = 16 frames */

	/* US GEM-port map for the OMCC flow: stamp the OLT-assigned ...
	 * dev/MEASURED-luna_gpon.c.md sec 199. */
	if (!gpon_gtc_us_gem_stamp(&gpon_io, &luna_gpon_chip.gtc,
				   GPON_OMCC_FLOW, gpon_gem_us_port_id(gem)))
		pr_err("luna-gpon: gpon_chip table declares no US port map -- OMCC gem %u not stamped\n",
		       gem);

	/* Re-assert PONIP_DBG_CTRL_US on every OMCC (re-)install: the initial pbo_init
	 * write sets DBG_IGNORE_TAG, but a DEACT/re-range cycle resets the US-NIC debug
	 * block and clears it. Without it the CPU tag is not stripped from US frames
	 * before GEM encapsulation, so the OMCI is malformed on every re-range. */
	pi_wr(PI_PONIP_DBG_CTRL_US, 0x00086000u);	/* stock (DBG_IGNORE_TAG=1) */

	/* PON-IP SID-valid + OMCI-SID, confirmed against live stock at O5. SID_Q_MAP_DS
	 * is 0 on stock: DS OMCI reaches the CPU purely via the GMAC CPU-tag trap, not
	 * a PBO queue, and our 2 MISROUTED the de-encapped OMCI away from the CPU. */
	pi_packed_set(PI_PON_SID2QID, GPON_OMCC_FLOW, 7, GPON_OMCC_PHYS_QID & 0x7f);
	pi_packed_set(PI_PON_SIDVALID, GPON_OMCC_FLOW, 1, 1);
	pi_field(PI_PON_OMCI_CFG, 6, 0, GPON_OMCC_FLOW);
	pi_packed_set(PI_PON_SID_Q_MAP_DS, GPON_OMCC_FLOW, 2, 0);
	/* Enrol THIS CHIP's OMCC SID in US counter-mask group 0: ...
	 * dev/MEASURED-luna_gpon.c.md sec 200. */
	{
		u32 mask_w = PI_CNT_MASK_US_WORD(0, GPON_OMCC_FLOW);

		pi_wr(mask_w, pi_rd(mask_w) | BIT(GPON_OMCC_FLOW % 32));
	}

	/* Read the packed entries back through pi_packed_get, the mirror of
	 * pi_packed_set, to confirm SID2QID landed at the TRUE word: the old per-word
	 * math wrote the wrong one and this readback was blind to it. */
	luna_log("pi readback sid2qid[64]=%u sidvalid[64]=%u sidqmapds[64]=%u\n",
		pi_packed_get(PI_PON_SID2QID, GPON_OMCC_FLOW, 7),
		pi_packed_get(PI_PON_SIDVALID, GPON_OMCC_FLOW, 1),
		pi_packed_get(PI_PON_SID_Q_MAP_DS, GPON_OMCC_FLOW, 2));

	/* Arm the NIC OMCI trap so DS OMCC frames reach the CPU netdev, and hand the
	 * eth driver this board's ONU-SN so its ONU-G reply matches what was ranged. */
	rtl9602c_eth_set_omci_sid(GPON_OMCC_FLOW);
	rtl9602c_eth_set_omci_identity(gpon_sn_bytes);

	/* Stock-ordered GPON mode-set classify block plus GMII re-latch. The SID
	 * classify above is set AFTER gpon_pbo_init's boot GMII edge, so the edge must
	 * be re-pulsed or the US-NIC never latches the classification. */
	rtl9602c_ponmac_modeset_gpon(gpon_data_installed);

	/* GMII re-latch AFTER ALL US-NIC config is written (GEM port ...
	 * dev/MEASURED-luna_gpon.c.md sec 317. */
	if (relatch_us) {
		pi_wr(PI_IO_CMD_0_US, 0x90101050u);	/* GMII RX OFF */
		pi_wr(PI_IO_CMD_0_US, 0x90101070u);	/* GMII RX ON -> re-latch */
		luna_log("relatch_us: re-pulsed GMII_RX_EN after OMCC install (io0_us=0x%08x)\n",
			pi_rd(PI_IO_CMD_0_US));
	}

	/* Full US-feed FIFO re-arm at O5. The O3 TX-PLL relock is a ...
	 * dev/MEASURED-luna_gpon.c.md sec 201. */
	if (o5_feed_rearm)
		gpon_us_feed_rearm_light();

	/* Re-assert AUTO_PROC_SSTART at O5: the HW's per-grant SStart auto-processing
	 * that STARTS the US burst. Written once at init, but the ranging reset can
	 * clear this US-side register and leave the framer parked on grants. */
	if (o5_sstart && READ_ONCE(luna_activation_ready)) {
		gpon_wr(GPON_GTC_US_WRITE_PROTECT, GPON_US_WP_UNLOCK);
		gpon_field(GPON_GTC_US_PROC_MODE, 0, 0, 1);	/* US_PROC_MODE.AUTO_PROC_SSTART = 1 */
		gpon_wr(GPON_GTC_US_WRITE_PROTECT, GPON_US_WP_LOCK);
		luna_log("O5 re-asserted AUTO_PROC_SSTART (0x5200 bit0=%u)\n",
			gpon_rd(GPON_GTC_US_PROC_MODE) & 1u);
	}

	luna_log("OMCC installed gem=%u flow=%u (compl %d)\n",
		gem, GPON_OMCC_FLOW, rc);
	return 0;
}

/* Install the WAN data GEM (the OLT's wire gem-port-id) on ...
 * dev/MEASURED-luna_gpon.c.md sec 202. */
/* The activation timeline of this boot, ms since boot, 0 = not yet: the first O5,
 * the first OMCI request, the data GEM installed. Benchmarks read it from
 * /proc/gpon_timing; a re-range keeps the first values. */
static struct {
	u32 o5_ms, omci_ms, data_ms;
} luna_marks;

static void luna_mark(u32 *m)
{
	if (!*m)
		*m = luna_now_ms();
}

static void luna_omci_turnaround(u64 rx_ns);

/* Stop the FSM: its tick, then a PLOAM interrupt already draining. One that
 * starts later finds the tick disarmed and leaves the queue to it. */
static void luna_fsm_stop(void)
{
	timer_delete_sync(&gpon_fsm_timer);
	spin_lock_bh(&luna_fsm_lock);
	spin_unlock_bh(&luna_fsm_lock);
}

#define LUNA_OMCI_QUEUE_LEN 64u
#define LUNA_OMCI_POLL_BUDGET 8u
#define LUNA_OMCI_ALARM_TICKS 300u
struct luna_omci_frame {
	u8 msg[OMCI_LEN];
	u16 len;
};
static_assert(sizeof(struct luna_omci_frame) == 50);
static DEFINE_SPINLOCK(luna_omci_lock);
static struct {
	struct omci_onu *onu;
	void *cookie;
	int (*tx)(void *, const u8 *, unsigned int);
	void (*tx_fence)(void *);
	int (*uni_admin_set)(void *, unsigned int, bool);
	struct luna_omci_frame queue[LUNA_OMCI_QUEUE_LEN];
	u64 rx_ns[LUNA_OMCI_QUEUE_LEN];		/* when each queued request arrived */
	struct gpon_lat turnaround;		/* request in -> response handed to TX */
	struct omci_data_binding binding;
	u16 head, count;
	u8 seed;
	u32 queued, overflow, closed, accepted, resets, tx_errors, tx_sent;
} luna_omci;
/* the provisioned OMCI identity, under luna_omci_lock (see onu_omci_id) */
static struct omci_identity luna_omci_id;
static bool luna_omci_id_set;
/* this unit's own stock T-CONT/queue/scheduler counts (see onu_capacity) */
static char *onu_capacity = "";
static bool luna_data_admitted;
static struct {
	struct gpon_data_armed armed;
	bool alloc_dirty, queue_dirty, gem_dirty, sid_dirty, stamp_dirty;
	int error;
} luna_data;

bool luna_gpon_data_ready(void)
{
	return READ_ONCE(luna_data_admitted) && READ_ONCE(luna_activation_ready);
}
EXPORT_SYMBOL(luna_gpon_data_ready);

/* The control flow exists only beside a dedicated data T-CONT: on the OMCC's queue
 * there is no second queue of ours to give it. */
static bool luna_ctrl_flow_wanted(void)
{
	return ctrl_queue && !luna_data.armed.rides_omcc && luna_data_qid != GPON_OMCC_PHYS_QID;
}

bool luna_gpon_ctrl_flow_ready(void)
{
	return READ_ONCE(gpon_data_installed) && luna_ctrl_flow_wanted();
}
EXPORT_SYMBOL(luna_gpon_ctrl_flow_ready);

/* Process-context NIC reset exclusion. The timer is the sole accepted-OMCI
 * and CAM/scheduler consumer; no model lock is held while waiting for it. */
void luna_gpon_nic_reset_begin(void)
{
	mutex_lock(&bosa_lock);
	if (luna_driver_ready)
		luna_fsm_stop();
	luna_data_suspend();
}
EXPORT_SYMBOL(luna_gpon_nic_reset_begin);

void luna_gpon_nic_reset_end(void)
{
	luna_resume_poll();
	mutex_unlock(&bosa_lock);
}
EXPORT_SYMBOL(luna_gpon_nic_reset_end);


/* The board's UNI panel, read from its own device tree. ★ IT ...
 * dev/MEASURED-luna_gpon.c.md sec 203. */
static u8 luna_uni_port[OMCI_UNI_MAX];
static u8 luna_uni_port_n;

/* The COMMON pending/apply owner for both Luna boards. ★ IT ...
 * dev/MEASURED-luna_gpon.c.md sec 204. */
static void luna_uni_apply_work_fn(struct work_struct *w);
static DECLARE_DELAYED_WORK(luna_uni_apply_work, luna_uni_apply_work_fn);

/* The enqueue gate. Read and written under the model spinlock ...
 * dev/MEASURED-luna_gpon.c.md sec 205. */
static bool luna_uni_closing;

static void luna_uni_apply_queue(unsigned long delay)
{
	unsigned long flags;

	spin_lock_irqsave(&luna_omci_lock, flags);
	/* ★★★ AND NOT BEFORE THE DRIVER IS READY. A module parameter ...
	 * dev/MEASURED-luna_gpon.c.md sec 206. */
	if (!luna_uni_closing && luna_driver_ready)
		schedule_delayed_work(&luna_uni_apply_work, delay);
	spin_unlock_irqrestore(&luna_omci_lock, flags);
}

/* ★★ bosa_lock IS THE LIFETIME OWNER, HELD ACROSS THE WHOLE ...
 * dev/MEASURED-luna_gpon.c.md sec 207. */
struct luna_uni_apply_ctx {
	struct omci_onu *onu;
	void *cookie;
	int (*set)(void *, unsigned int, bool);
};

static enum omci_uni_apply_rc luna_uni_apply_slot(void *sh, u8 slot, bool locked)
{
	struct luna_uni_apply_ctx *c = sh;

	if (!c->set || slot >= luna_uni_port_n) {
		pr_warn_once("luna-gpon: UNI slot %u has no %s; its administrative state is modelled and not applied\n",
			     slot, c->set ? "declared switch port" : "apply backend");
		return OMCI_UNI_NO_PORT;
	}
	return c->set(c->cookie, luna_uni_port[slot], locked) ?
		OMCI_UNI_TRANSIENT : OMCI_UNI_APPLIED;
}

static void luna_uni_apply_rearm(void *sh, u8 slot)
{
	struct luna_uni_apply_ctx *c = sh;
	unsigned long flags;

	spin_lock_irqsave(&luna_omci_lock, flags);
	omci_uni_mark_changed(&c->onu->pptp_eth_uni, slot);
	spin_unlock_irqrestore(&luna_omci_lock, flags);
}

static const struct omci_uni_apply_ops luna_uni_apply_ops = {
	.apply = luna_uni_apply_slot,
	.rearm = luna_uni_apply_rearm,
};

static void luna_uni_apply_work_fn(struct work_struct *w)
{
	int (*set)(void *, unsigned int, bool);
	u8 changed, admin[OMCI_UNI_MAX], n, i;
	struct luna_uni_apply_ctx ctx;
	struct omci_onu *onu;
	unsigned long flags;
	void *cookie;

	(void)w;
	mutex_lock(&bosa_lock);
	spin_lock_irqsave(&luna_omci_lock, flags);
	onu = luna_omci.onu;
	cookie = luna_omci.cookie;
	set = luna_omci.uni_admin_set;
	if (!onu) {
		spin_unlock_irqrestore(&luna_omci_lock, flags);
		mutex_unlock(&bosa_lock);
		return;		/* nothing consumed, so nothing is lost */
	}
	changed = omci_uni_take_changed(&onu->pptp_eth_uni);
	n = onu->pptp_eth_uni.n;
	for (i = 0; i < n && i < OMCI_UNI_MAX; i++)
		admin[i] = onu->pptp_eth_uni.admin[i];
	spin_unlock_irqrestore(&luna_omci_lock, flags);

	ctx.onu = onu;
	ctx.cookie = cookie;
	ctx.set = set;
	/* The WALK and the re-arm rule are G.988's and live in the core; this
	 * shell supplies only the port call and the lock the model is kept
	 * under.  RE-ARMED ON THE VERY MODEL THE SNAPSHOT CAME FROM: bosa_lock
	 * has kept it attached for the whole call. */
	if (omci_uni_apply_run(&luna_uni_apply_ops, &ctx, changed, n, admin))
		luna_uni_apply_queue(HZ);
	mutex_unlock(&bosa_lock);
}

void luna_uni_apply_kick(void)
{
	luna_uni_apply_queue(0);
}
EXPORT_SYMBOL(luna_uni_apply_kick);

/* Shut the gate.  Named, and here beside the gate rather than spelled inline in
 * the unload path, so the host differential executes the SHIPPING closure
 * instead of a copy of it. */
static void luna_uni_apply_close(void)
{
	unsigned long flags;

	spin_lock_irqsave(&luna_omci_lock, flags);
	luna_uni_closing = true;
	spin_unlock_irqrestore(&luna_omci_lock, flags);
}

#ifdef CONFIG_GPON_OMCI_DIAG
/* ★★★ A TEMPORARY DIAGNOSTIC INGRESS, AND IT COMES BACK OUT ...
 * dev/MEASURED-luna_gpon.c.md sec 208. */
static u16 luna_uni_test_tci = 0xd100;

/* The inventory a board run needs to RESTORE what it found. ★ ...
 * dev/MEASURED-luna_gpon.c.md sec 209. */
static void luna_uni_test_show(void)
{
	u8 admin[OMCI_UNI_MAX] = { 0 }, port[OMCI_UNI_MAX] = { 0 };
	u16 inst[OMCI_UNI_MAX] = { 0 };
	u8 changed = 0, n = 0, port_n = 0, i;
	struct omci_onu *onu;
	unsigned long flags;

	mutex_lock(&bosa_lock);
	spin_lock_irqsave(&luna_omci_lock, flags);
	onu = luna_omci.onu;
	if (onu) {
		n = onu->pptp_eth_uni.n;
		changed = onu->pptp_eth_uni.changed;
		for (i = 0; i < n && i < OMCI_UNI_MAX; i++) {
			inst[i] = onu->pptp_eth_uni.inst[i];
			admin[i] = onu->pptp_eth_uni.admin[i];
		}
	}
	/* ★ THE PORT MAP IS COPIED IN THE SAME CRITICAL SECTION as the
	 * dev/MEASURED-luna_gpon.c.md sec 318. */
	port_n = luna_uni_port_n;
	memcpy(port, luna_uni_port, sizeof(port));
	spin_unlock_irqrestore(&luna_omci_lock, flags);
	mutex_unlock(&bosa_lock);

	pr_info("luna-gpon: uni_test SNAPSHOT attached %d slots %u pending %#x\n",
		onu ? 1 : 0, n, changed);
	for (i = 0; i < n && i < OMCI_UNI_MAX; i++)
		pr_info("luna-gpon: uni_test SNAPSHOT slot %u inst %#06x admin %u port %d pending %u\n",
			i, inst[i], admin[i],
			i < port_n ? (int)port[i] : -1,
			!!(changed & BIT(i)));
}

static int luna_uni_test_set(const char *val, const struct kernel_param *kp)
{
	u8 msg[OMCI_LEN], admin_val;
	unsigned int inst, admin;
	unsigned long flags;
	bool declared = false;
	u16 tci;
	int rc;

	(void)kp;
	if (!strncmp(val, "show", 4)) {
		luna_uni_test_show();
		return 0;
	}
	if (sscanf(val, "%x %u", &inst, &admin) != 2 || inst > 0xffff || admin > 1)
		return -EINVAL;

	/* ★ THE INSTANCE IS CHECKED AGAINST THE BOARD'S DECLARED PANEL.  A typo
	 *   would otherwise inject a Set for a UNI this board does not have, and
	 *   the run would measure nothing while looking like it measured. */
	spin_lock_irqsave(&luna_omci_lock, flags);
	if (luna_omci.onu)
		declared = omci_uni_slot(&luna_omci.onu->pptp_eth_uni, (u16)inst) >= 0;
	tci = ++luna_uni_test_tci;
	spin_unlock_irqrestore(&luna_omci_lock, flags);
	if (!declared) {
		pr_err("luna-gpon: uni_test REFUSED: instance %#06x is not in this board's declared panel\n",
		       inst);
		return -ENODEV;
	}

	/* ME 11 attribute 5, administrative state: a UNIQUE tid per injection,
	 * AR=0 and AK=0 so no response is owed.  The octets are the core's. */
	admin_val = (u8)admin;
	omci_set_build(msg, tci, OMCI_ME_PPTP_ETH_UNI, (u16)inst, OMCI_ATTR_BIT(5),
		       &admin_val, 1);

	/* ★ THE REQUEST IS ANNOUNCED BEFORE IT IS SUBMITTED.  The FSM timer can
	 *   consume it and the apply can complete on another CPU before a line
	 *   printed after the call reaches the log, and a reader would then see
	 *   the completion BEFORE the request it belongs to. */
	pr_info("luna-gpon: uni_test REQUEST tid %#06x ME 11 #%#06x administrative state %u\n",
		tci, inst, admin);
	rc = luna_omci_enqueue(luna_omci.cookie, msg, OMCI_LEN);
	pr_info("luna-gpon: uni_test ADMITTED tid %#06x rc %d\n", tci, rc);
	return rc;
}

static const struct kernel_param_ops luna_uni_test_ops = { .set = luna_uni_test_set };
module_param_cb(uni_test, &luna_uni_test_ops, NULL, 0200);
MODULE_PARM_DESC(uni_test, "TEMPORARY board diagnostic: write \"<instance-hex> <0|1>\" to inject a local ME 11 administrative-state Set through the real OMCI queue, or \"show\" to log the declared UNI inventory (instance, administrative state, switch port, pending) taken under the lifetime owner");
#endif /* CONFIG_GPON_OMCI_DIAG */

static const void *luna_of_prop(void *np, const char *name, int *len)
{
	*len = -1;
	return of_get_property(np, name, len);
}

static void luna_omci_declare_uni_panel(struct omci_onu *onu)
{
	struct device_node *np = of_find_node_by_path("/omci-uni");
	struct omci_panel_decl d;

	if (!np)
		return;
	omci_onu_declare_panel(onu, luna_of_prop, np, luna_uni_port,
			       ARRAY_SIZE(luna_uni_port), &luna_uni_port_n, &d);
	of_node_put(np);
	if (d.unis_why)
		pr_err("luna-gpon: /omci-uni REFUSED: %s -- keeping the single-UNI default\n",
		       d.unis_why);
	if (d.unis && !luna_uni_port_n)
		pr_warn("luna-gpon: /omci-uni has no usable ethernet-uni-ports (%d bytes for %d UNIs): the administrative state is modelled and NEVER APPLIED\n",
			d.ports_len, d.unis);
	if (d.part)
		pr_err("luna-gpon: /omci-uni %s REFUSED: %s -- that part keeps its default\n",
		       d.part, d.part_why);
}

/* The unit's own counts over the board's, under luna_omci_lock. */
static void luna_omci_unit_capacity(struct omci_onu *onu)
{
	const char *why = "";

	if (omci_onu_declare_unit_capacity(onu, onu_capacity, &why) ==
	    OMCI_UNI_DECL_BAD)
		pr_err("luna-gpon: onu_capacity '%s' REFUSED: %s -- the board's counts stay\n",
		       onu_capacity, why);
}

/* The WAN MAC as last set by the NIC, so a model attached later starts
 * with it. */
static u8 luna_omci_wan_mac[ETH_ALEN];
static bool luna_omci_wan_mac_set;

void luna_omci_set_wan_mac(const u8 *mac)
{
	unsigned long flags;

	spin_lock_irqsave(&luna_omci_lock, flags);
	memcpy(luna_omci_wan_mac, mac, ETH_ALEN);
	luna_omci_wan_mac_set = true;
	if (luna_omci.onu)
		omci_onu_set_wan_mac(luna_omci.onu, mac);
	spin_unlock_irqrestore(&luna_omci_lock, flags);
}
EXPORT_SYMBOL(luna_omci_set_wan_mac);

int luna_omci_attach(struct omci_onu *onu, void *cookie, u8 seed,
		    int (*tx)(void *, const u8 *, unsigned int),
		    void (*tx_fence)(void *),
		    int (*uni_admin_set)(void *, unsigned int, bool))
{
	unsigned long flags;
	int rc = 0;

	if (!onu || !cookie || !tx || !tx_fence)
		return -EINVAL;
	mutex_lock(&bosa_lock);
	spin_lock_irqsave(&luna_omci_lock, flags);
	if (luna_omci.onu) {
		rc = -EBUSY;
	} else {
		omci_onu_init(onu, gpon_sn_bytes, seed);
		if (luna_omci_id_set)
			onu->id = luna_omci_id;
		if (luna_omci_wan_mac_set)
			omci_onu_set_wan_mac(onu, luna_omci_wan_mac);
		luna_omci_declare_uni_panel(onu);
		luna_omci_unit_capacity(onu);
		luna_omci.onu = onu;
		luna_omci.cookie = cookie;
		luna_omci.tx = tx;
		luna_omci.tx_fence = tx_fence;
		luna_omci.uni_admin_set = uni_admin_set;
		luna_omci.seed = seed;
		luna_omci.head = luna_omci.count = 0;
		memset(&luna_omci.binding, 0, sizeof(luna_omci.binding));
	}
	spin_unlock_irqrestore(&luna_omci_lock, flags);
	mutex_unlock(&bosa_lock);
	return rc;
}
EXPORT_SYMBOL(luna_omci_attach);

void luna_omci_detach(void *cookie)
{
	unsigned long flags;

	/* The only response/hardware consumer is this timer. Do not hold the
	 * model lock while waiting for it, or while taking the NIC TX lock. */
	mutex_lock(&bosa_lock);
	if (luna_driver_ready)
		luna_fsm_stop();
	spin_lock_irqsave(&luna_omci_lock, flags);
	if (luna_omci.cookie == cookie) {
		WRITE_ONCE(luna_data_admitted, false);
		luna_omci.onu = NULL;
		luna_omci.cookie = NULL;
		luna_omci.tx = NULL;
		luna_omci.tx_fence = NULL;
		luna_omci.uni_admin_set = NULL;
		luna_omci.count = 0;
		memset(&luna_omci.binding, 0, sizeof(luna_omci.binding));
	}
	spin_unlock_irqrestore(&luna_omci_lock, flags);
	luna_resume_poll();
	mutex_unlock(&bosa_lock);
}
EXPORT_SYMBOL(luna_omci_detach);

static void luna_omci_poll(void);
static void luna_data_reconcile(void);

/* A queued OMCI request is answered on arrival, in BH context, not at the next
 * FSM tick: the OLT sends its next request only after our answer, so a tick-paced
 * answer set its pace (20 ms per request, stock ~9 ms). The tick stays the
 * backstop; a stopped FSM answers nothing. */
static void luna_omci_work_fn(struct work_struct *work)
{
	unsigned int rounds = LUNA_OMCI_QUEUE_LEN;

	spin_lock(&luna_fsm_lock);
	while (rounds-- && timer_pending(&gpon_fsm_timer) &&
	       READ_ONCE(luna_activation_ready) && READ_ONCE(luna_omci.count)) {
		luna_omci_poll();
		luna_data_reconcile();
	}
	spin_unlock(&luna_fsm_lock);
}
static DECLARE_WORK(luna_omci_work, luna_omci_work_fn);

int luna_omci_enqueue(void *cookie, const u8 *msg, unsigned int len)
{
	struct luna_omci_frame *f;
	unsigned long flags;
	int rc = 0;

	if (!msg || len > 0xffffu)
		return -EINVAL;
	spin_lock_irqsave(&luna_omci_lock, flags);
	if (!luna_omci.onu || luna_omci.cookie != cookie ||
	    !READ_ONCE(luna_activation_ready)) {
		luna_omci.closed++;
		rc = -ESHUTDOWN;
	} else if (luna_omci.count == LUNA_OMCI_QUEUE_LEN) {
		luna_omci.overflow++;
		rc = -ENOSPC;
	} else {
		f = &luna_omci.queue[(luna_omci.head + luna_omci.count) % LUNA_OMCI_QUEUE_LEN];
		memset(f->msg, 0, sizeof(f->msg));
		memcpy(f->msg, msg, min_t(unsigned int, len, sizeof(f->msg)));
		f->len = len;
		luna_omci.rx_ns[f - luna_omci.queue] = ktime_get_ns();
		luna_omci.count++;
		luna_omci.queued++;
		luna_mark(&luna_marks.omci_ms);
	}
	spin_unlock_irqrestore(&luna_omci_lock, flags);
	if (!rc)
		queue_work(system_bh_highpri_wq, &luna_omci_work);
	return rc;
}
EXPORT_SYMBOL(luna_omci_enqueue);

void luna_omci_set_sn(const u8 sn[8])
{
	unsigned long flags;

	if (!sn)
		return;
	spin_lock_irqsave(&luna_omci_lock, flags);
	if (luna_omci.onu)
		omci_onu_set_sn(luna_omci.onu, sn);
	spin_unlock_irqrestore(&luna_omci_lock, flags);
}
EXPORT_SYMBOL(luna_omci_set_sn);

void luna_omci_set_optical(u16 rx, u16 tx)
{
	unsigned long flags;

	spin_lock_irqsave(&luna_omci_lock, flags);
	if (luna_omci.onu)
		omci_onu_set_optical(luna_omci.onu, rx, tx);
	spin_unlock_irqrestore(&luna_omci_lock, flags);
}
EXPORT_SYMBOL(luna_omci_set_optical);

static int luna_omci_send(const u8 *msg, int n)
{
	int ret;

	luna_mon_frame(GPON_MON_OMCI, 1, msg, n);
	ret = luna_omci.tx(luna_omci.cookie, msg, n);

	if (ret)
		luna_omci.tx_errors++;
	else
		luna_omci.tx_sent++;
	return ret;
}

/* The ANI-G self-test reading: taken by the optical worker, where the I2C may
 * sleep, and sent from the OMCI service, the one context that transmits. */
static struct gpon_optic_reading luna_selftest;
static bool luna_selftest_taken;

static bool luna_selftest_owed(void)
{
	unsigned long flags;
	bool owed;

	spin_lock_irqsave(&luna_omci_lock, flags);
	owed = luna_omci.onu && omci_onu_selftest_pending(luna_omci.onu) &&
	       !luna_selftest_taken;
	spin_unlock_irqrestore(&luna_omci_lock, flags);
	return owed;
}

static void luna_selftest_hand_over(const struct gpon_optic_reading *v)
{
	unsigned long flags;

	spin_lock_irqsave(&luna_omci_lock, flags);
	luna_selftest = *v;
	luna_selftest_taken = true;
	spin_unlock_irqrestore(&luna_omci_lock, flags);
}

static void luna_omci_report_selftest(void)
{
	u8 msg[OMCI_LEN];
	unsigned long flags;
	int n = 0;

	spin_lock_irqsave(&luna_omci_lock, flags);
	if (luna_selftest_taken && luna_omci.onu)
		n = omci_onu_selftest_result(luna_omci.onu, &luna_selftest, msg);
	luna_selftest_taken = false;
	spin_unlock_irqrestore(&luna_omci_lock, flags);
	if (n && luna_omci_send(msg, n))
		pr_warn_ratelimited("luna-gpon: ANI-G Test Result not sent (OMCI TX error)\n");
}

/* Timer consumer serializes alarm TX with Get All Alarms resynchronization. */
static void luna_omci_report_alarm(void)
{
	u8 msg[OMCI_LEN];
	unsigned long flags;
	int n = 0, ret;

	spin_lock_irqsave(&luna_omci_lock, flags);
	if (luna_omci.onu && READ_ONCE(luna_ploam.omcc_installed))
		n = omci_onu_alarm_prepare(luna_omci.onu, msg);
	spin_unlock_irqrestore(&luna_omci_lock, flags);
	if (!n)
		return;
	ret = luna_omci_send(msg, n);
	spin_lock_irqsave(&luna_omci_lock, flags);
	if (!ret)
		omci_onu_alarm_sent(luna_omci.onu, msg);
	spin_unlock_irqrestore(&luna_omci_lock, flags);
}

void luna_omci_rx_errors(u32 *bad_mic, u32 *runt)
{
	unsigned long flags;

	spin_lock_irqsave(&luna_omci_lock, flags);
	*bad_mic = luna_omci.onu ? luna_omci.onu->rx_bad_mic : 0;
	*runt = luna_omci.onu ? luna_omci.onu->rx_runt : 0;
	spin_unlock_irqrestore(&luna_omci_lock, flags);
}
EXPORT_SYMBOL(luna_omci_rx_errors);

/* Called only by the timer's existing AVC cadence. */
void luna_omci_report_oper_up(void)
{
	u8 msg[OMCI_LEN];
	unsigned long flags;
	int n = 0;

	spin_lock_irqsave(&luna_omci_lock, flags);
	if (luna_omci.onu && luna_gpon_data_ready())
		n = omci_onu_emit_veip_up_avc(luna_omci.onu, msg);
	spin_unlock_irqrestore(&luna_omci_lock, flags);
	if (n > 0 && luna_omci_send(msg, n)) {
		/* The common GET projection reflects delivered operational state. */
		spin_lock_irqsave(&luna_omci_lock, flags);
		luna_omci.onu->avc_veip_up_sent = false;
		spin_unlock_irqrestore(&luna_omci_lock, flags);
	}
}
EXPORT_SYMBOL(luna_omci_report_oper_up);

static void luna_omci_identity_reset(const u8 sn[8])
{
	unsigned long flags;

	/* Caller has stopped the timer under bosa_lock before publishing the SN. */
	WRITE_ONCE(luna_data_admitted, false);
	spin_lock_irqsave(&luna_omci_lock, flags);
	luna_omci.count = luna_omci.head = 0;
	/* A published SN is an identity change on a model attach() already
	 * initialised, so the declared UNI panel is carried across. */
	if (luna_omci.onu)
		omci_onu_reinit(luna_omci.onu, sn, luna_omci.seed);
	memset(&luna_omci.binding, 0, sizeof(luna_omci.binding));
	spin_unlock_irqrestore(&luna_omci_lock, flags);
	/* The reinit carries the panel and resets the administrative state, so
	 * whatever it unlocked is owed a physical turn. */
	luna_uni_apply_kick();
	gpon_ploam_data_alloc_reset(&luna_ploam);
	luna_data.armed.installed = false;
}



/* ⚠ DEFINED HERE BECAUSE IT IS USED HERE FIRST. It sat ~600 ...
 * dev/MEASURED-luna_gpon.c.md sec 211. */

static int gpon_avc_sent;	/* OMCI oper-state AVCs emitted this O5 (reset on re-range) */

/* ★★★ THE ALLOC-ID -> T-CONT BIND BELONGS TO Assign_Alloc-ID. ...
 * dev/MEASURED-luna_gpon.c.md sec 212. */
static void luna_data_tcont_from_assign(u16 alloc, bool assigned)
{
	enum gpon_gem_us_bind bind;

	if (!assigned || !data_tcont)
		return;
	bind = gpon_gem_us_tcont_decide(alloc, luna_ploam.onu_id,
					READ_ONCE(luna_data.armed.alloc_bound));
	if (bind == GPON_GEM_US_BIND_TCONT) {
		gpon_install_tcont(GPON_DATA_TCONT, alloc);
		return;
	}
	luna_log("Alloc 0x%x NOT bound to the data T-CONT: %s\n",
		alloc, gpon_gem_us_bind_name(bind));
}

/* ★★★ THE OWNERSHIP CONSUMER OWNS THE BIND, AND THIS OP *IS* ...
 * dev/MEASURED-luna_gpon.c.md sec 213. */
static void luna_data_alloc_changed(void *sh, u16 alloc, bool assigned)
{
	(void)sh;
	gpon_ploam_data_alloc_note(&luna_ploam, alloc, assigned);
	luna_data_tcont_from_assign(alloc, assigned);
}

#ifdef CONFIG_GPON_OLT_DIAG
/* The far end's own conversation.  Written under luna_omci_lock (the lock that
 * already serializes this shell's OMCI path) and read one record at a time, so
 * /proc/oltcap never holds it across a whole dump. */
GPON_OLT_CAPTURE(luna_olt_prov, GPON_OLT_LEDGER_RECORDS);
GPON_OLT_CAPTURE(luna_olt_unknown, GPON_OLT_UNKNOWN_RECORDS);
GPON_OLT_CAPTURE(luna_olt_cap, GPON_OLT_RECORDS, .ledger = &luna_olt_prov,
			.unknown = &luna_olt_unknown, .described = omci_class_described);

static int olt_capture_show(struct seq_file *s, struct gpon_olt_capture *c,
			    spinlock_t *lock)
{
	char line[160];
	unsigned int i, n;
	unsigned long flags;

	spin_lock_irqsave(lock, flags);
	n = gpon_olt_capture_count(c);
	gpon_olt_diag_header(c, line, sizeof(line));
	spin_unlock_irqrestore(lock, flags);
	seq_printf(s, "%s\n", line);
	for (i = 0; i < n; i++) {
		/* zero-initialised so a NULL fetch can never leave an uninitialised
		 * record in scope; the loop breaks on it anyway. */
		struct gpon_olt_rec r = { 0 };
		const struct gpon_olt_rec *p;

		spin_lock_irqsave(lock, flags);
		p = gpon_olt_capture_at(c, i);
		if (p)
			r = *p;
		spin_unlock_irqrestore(lock, flags);
		if (!p)
			break;
		if (gpon_olt_diag_line(&r, line, sizeof(line)))
			seq_printf(s, "%s\n", line);
	}
	return 0;
}

static int oltcap_proc_show(struct seq_file *s, void *v)
{
	return olt_capture_show(s, &luna_olt_cap, &luna_omci_lock);
}

static int gpon_prov_proc_show(struct seq_file *s, void *v)
{
	return olt_capture_show(s, &luna_olt_prov, &luna_omci_lock);
}

static int gpon_unknown_proc_show(struct seq_file *s, void *v)
{
	return olt_capture_show(s, &luna_olt_unknown, &luna_omci_lock);
}

static int ploam_unknown_proc_show(struct seq_file *s, void *v)
{
	return olt_capture_show(s, &luna_ploam_unknown, &luna_ploam_cap_lock);
}

static int ploamcap_proc_show(struct seq_file *s, void *v)
{
	return olt_capture_show(s, &luna_ploam_cap, &luna_ploam_cap_lock);
}
#endif /* CONFIG_GPON_OLT_DIAG */

static void luna_omci_poll(void)
{
	unsigned int budget = LUNA_OMCI_POLL_BUDGET;

	while (budget--) {
		struct omci_accepted accepted;
		struct luna_omci_frame frame;
		u8 response[OMCI_LEN];
		unsigned long flags;
		u64 rx_ns;
		int n;

		spin_lock_irqsave(&luna_omci_lock, flags);
		if (!luna_omci.onu || !luna_omci.count) {
			spin_unlock_irqrestore(&luna_omci_lock, flags);
			break;
		}
		frame = luna_omci.queue[luna_omci.head];
		rx_ns = luna_omci.rx_ns[luna_omci.head];
		luna_omci.head = (luna_omci.head + 1) % LUNA_OMCI_QUEUE_LEN;
		luna_omci.count--;
		n = omci_onu_input_ex(luna_omci.onu, frame.msg, frame.len,
				      response, &accepted);
#ifdef CONFIG_GPON_OLT_DIAG
		/* The clock is read HERE and handed in: the core tier reads none,
		 * which is what makes a capture replay deterministically. */
		gpon_olt_capture_exchange(&luna_olt_cap,
					  div_u64(ktime_get_boottime_ns(), 1000),
					  (u8)luna_ploam.state,
					  frame.msg, frame.len,
					  n == OMCI_LEN ? response : NULL, n);
#endif
		if (accepted.kind != OMCI_ACCEPT_NONE) {
			luna_omci.accepted++;
			if (accepted.kind == OMCI_ACCEPT_RESET) {
				luna_omci.resets++;
				gpon_avc_sent = luna_ploam.avc_sent = 0;
			}
			omci_data_binding_snapshot(luna_omci.onu, gpon_omcc_gem_port,
					   GPON_MCAST_GEM, &luna_omci.binding);
		}
		spin_unlock_irqrestore(&luna_omci_lock, flags);
		luna_mon_frame(GPON_MON_OMCI, 0, frame.msg, frame.len);
		if (n > 0) {
			luna_omci_send(response, n);
			luna_omci_turnaround(rx_ns);
		}
#ifdef CONFIG_GPON_OMCI_DIAG
		/* Preserve the shared request/response trace at its new owner. The
		 * common predicate limits only bulk diagnostics, never acceptance. */
		if (!gpon_omci_is_bulk(frame.msg, frame.len) || net_ratelimit()) {
			char line[GPON_OMCI_DIAG_LINE_MAX];

			if (gpon_omci_diag_line(frame.msg, frame.len,
					       n == OMCI_LEN ? response : NULL, n,
					       line, sizeof(line)))
				luna_log("OMCI DS: %s\n", line);
		}
#endif
		/* An accepted mutation gets its hardware turn before another PDU can
		 * replace the snapshot. In particular Reset must not collapse into a
		 * following Create/Set that happens to recreate the same effective pair. */
		if (accepted.kind != OMCI_ACCEPT_NONE) {
			/* A Set the model accepted and a MIB Reset that unlocked ...
			 * dev/MEASURED-luna_gpon.c.md sec 320. */
			luna_uni_apply_kick();
			break;
		}
	}
}

int gpon_install_data_gem(void)
{
	int rc;

	if (gpon_data_installed)
		return 0;
	if (!luna_ploam.omcc_installed)
		return -EAGAIN;

	/* DS GEM-port CAM: the OLT's gem -> the data flow, the same shared helper as
	 * the OMCC CAM. */
	luna_data.gem_dirty = true;
	rc = luna_port_cam_write(GPON_DATA_FLOW, gpon_data_gem_port);
	if (rc < 0) {
		pr_err("luna-gpon: DATA GEM DS install timeout\n");
		return rc;
	}
	/* DATA flow DS routing = 0x2 (BIT1). Oracle-confirmed ...
	 * dev/MEASURED-luna_gpon.c.md sec 214. */
	gpon_wr(GPON_GTC_DS_TRAFFIC_CFG + GPON_DATA_FLOW * DS_TRAFFIC_CFG_STRIDE, 0x2);

	/* US GEM-port map: the data flow -> the OLT's gem, the gem-id stamped on US
	 * data frames. Same shared gpon_gtc_us_gem_stamp() as the OMCC stamp, and the
	 * 12-bit wire mask is gpon_gem_us_port_id(), the one spelling. */
	luna_data.stamp_dirty = reg_has(luna_gpon_chip.gtc.gem_us_port_map);
	if (!gpon_gtc_us_gem_stamp(&gpon_io, &luna_gpon_chip.gtc, GPON_DATA_FLOW,
				   gpon_gem_us_port_id(gpon_data_gem_port))) {
		pr_err("luna-gpon: gpon_chip table declares no US port map -- data gem %u not stamped\n",
		       gpon_data_gem_port);
		return -ENODEV;
	}

	/* The accepted binding selects either its dedicated data queue or the
	 * existing management queue when both identities share the same Alloc-ID.
	 * Never install a second T-CONT for the management allocation. */
	luna_data.sid_dirty = true;
	pi_packed_set(PI_PON_SID2QID, GPON_DATA_FLOW, 7, luna_data_qid & 0x7f);
	pi_packed_set(PI_PON_SIDVALID, GPON_DATA_FLOW, 1, 1);
	pi_packed_set(PI_PON_SID_Q_MAP_DS, GPON_DATA_FLOW, 2, 0);
	if (luna_ctrl_flow_wanted()) {
		if (!gpon_gtc_us_gem_stamp(&gpon_io, &luna_gpon_chip.gtc, GPON_CTRL_FLOW,
					   gpon_gem_us_port_id(gpon_data_gem_port)))
			return -ENODEV;
		pi_packed_set(PI_PON_SID2QID, GPON_CTRL_FLOW, 7, (luna_data_qid + 1) & 0x7f);
		pi_packed_set(PI_PON_SIDVALID, GPON_CTRL_FLOW, 1, 1);
		pi_packed_set(PI_PON_SID_Q_MAP_DS, GPON_CTRL_FLOW, 2, 0);
	}

	/* Multicast/broadcast GEM -> its own flow, BRIDGED and ...
	 * dev/MEASURED-luna_gpon.c.md sec 215. */
	rc = luna_port_cam_write(GPON_MCAST_FLOW, GPON_MCAST_GEM);
	if (rc < 0) {
		pr_err("luna-gpon: MCAST GEM DS install timeout\n");
		return rc;
	}
	/* MCAST flow DS routing = 0x3 (BIT1|BIT0): BIT0 is the broadcast/flood
	 * variant, so DHCP-broadcast and ARP DS also drain to the NIC. */
	gpon_wr(GPON_GTC_DS_TRAFFIC_CFG + GPON_MCAST_FLOW * DS_TRAFFIC_CFG_STRIDE, 0x3);

	/* Data-flow SID classify commit -- the flow-1 US half-boot fix
	 * dev/MEASURED-luna_gpon.c.md sec 216. */
	if (relatch_us) {
		int tries;

		for (tries = 0; tries < 4; tries++) {
			/* (re)assert the flow-1 classify triple so a missed edge is re-armed */
			pi_packed_set(PI_PON_SID2QID, GPON_DATA_FLOW, 7,
				      luna_data_qid & 0x7f);
			pi_packed_set(PI_PON_SIDVALID, GPON_DATA_FLOW, 1, 1);
			if (luna_ctrl_flow_wanted()) {
				pi_packed_set(PI_PON_SID2QID, GPON_CTRL_FLOW, 7,
					      (luna_data_qid + 1) & 0x7f);
				pi_packed_set(PI_PON_SIDVALID, GPON_CTRL_FLOW, 1, 1);
			}

			/* Re-force the US-NIC<->GMAC0 internal-MII link UP before the commit edge,
			 * the symmetric twin of the DS re-force below: the ifup GMAC0 power-cycle
			 * can drop this link, so the classify edge latched only ~50/50. */
			pi_wr(PI_MEDIA_STS_US, 0x106e8400u);
			/* commit edge: GMII_RX_EN OFF -> ON latches the classify table */
			pi_wr(PI_IO_CMD_0_US, 0x90101050u);	/* GMII RX OFF */
			udelay(50);
			pi_wr(PI_IO_CMD_0_US, 0x90101070u);	/* GMII RX ON -> latch flow-1 */

			/* The classify registers are not write-protected, they latch on the edge,
			 * so a correct readback is the definitive "committed" signal. */
			if (pi_packed_get(PI_PON_SIDVALID, GPON_DATA_FLOW, 1) == 1 &&
			    pi_packed_get(PI_PON_SID2QID, GPON_DATA_FLOW, 7) ==
				    (luna_data_qid & 0x7f))
				break;
			udelay(100);
		}
		if (tries == 4) {
			pr_err("luna-gpon: DATA GEM classify readback timeout\n");
			return -ETIMEDOUT;
		}
		luna_log("data-gem: flow-%u classify committed after %d edge(s)\n",
			GPON_DATA_FLOW, tries + 1);

		/* DS half of the same fix: the DS-NIC drain config and GMII edge were latched
		 * at boot, before this window and before the ifup GMAC0 power-cycle may have
		 * perturbed the DS-NIC<->GMAC0 MII. Re-force the link and re-pulse the DS
		 * edge here so DS delivery to the GMAC RX ring is deterministic every boot. */
		pi_wr(PI_MEDIA_STS_DS, 0x106e8400u);	/* re-force DS-NIC<->GMAC0 internal MII link UP */
		pi_wr(PI_IO_CMD_0_DS, 0x90081050u);	/* DS GMII_RX_EN OFF */
		udelay(50);
		pi_wr(PI_IO_CMD_0_DS, 0x90081070u);	/* ON -> re-latch DS drain config + link */
	}
	/* Keep the pending flow through modeset without publishing success early. */
	rtl9602c_ponmac_modeset_gpon(true);
	if (pi_packed_get(PI_PON_SIDVALID, GPON_DATA_FLOW, 1) != 1 ||
	    pi_packed_get(PI_PON_SID2QID, GPON_DATA_FLOW, 7) != (luna_data_qid & 0x7f))
		return -EIO;
	if (luna_ctrl_flow_wanted() &&
	    (pi_packed_get(PI_PON_SIDVALID, GPON_CTRL_FLOW, 1) != 1 ||
	     pi_packed_get(PI_PON_SID2QID, GPON_CTRL_FLOW, 7) != ((luna_data_qid + 1) & 0x7f)))
		return -EIO;
	gpon_data_installed = true;
	gpon_ploam_set_data_installed(&luna_ploam, true);
	luna_mark(&luna_marks.data_ms);

	luna_log("DATA GEM installed gem=%u flow=%u qid=%u sid2qid=%u sidvalid=%u\n",
		gpon_data_gem_port, GPON_DATA_FLOW, luna_data_qid,
		pi_packed_get(PI_PON_SID2QID, GPON_DATA_FLOW, 7),
		pi_packed_get(PI_PON_SIDVALID, GPON_DATA_FLOW, 1));
	return 0;
}

static void luna_omci_turnaround(u64 rx_ns)
{
	u64 us = div_u64(ktime_get_ns() - rx_ns, NSEC_PER_USEC);
	unsigned long flags;

	spin_lock_irqsave(&luna_omci_lock, flags);
	gpon_lat_add(&luna_omci.turnaround, (u32)min_t(u64, us, U32_MAX));
	spin_unlock_irqrestore(&luna_omci_lock, flags);
}

/* /proc/gpon_timing: this boot's activation timeline and the OMCI turnaround,
 * read by the suite's gpon_timing benchmark. */
static int luna_timing_show(struct seq_file *s, void *v)
{
	struct {
		struct gpon_lat lat;
		u32 out[GPON_LAT_RING];
	} *t = kmalloc(sizeof(*t), GFP_KERNEL);
	unsigned long flags;
	unsigned int i, n;

	if (!t)
		return -ENOMEM;
	spin_lock_irqsave(&luna_omci_lock, flags);
	t->lat = luna_omci.turnaround;
	spin_unlock_irqrestore(&luna_omci_lock, flags);
	n = gpon_lat_samples(&t->lat, t->out);
	seq_printf(s, "fsm_tick_ms %u\no5_ms %u\nomci_first_ms %u\ndata_ms %u\n",
		   gpon_fsm_ticks > 1 ? (luna_now_ms() - luna_fsm_first_ms) / (gpon_fsm_ticks - 1) : 0,
		   luna_marks.o5_ms, luna_marks.omci_ms, luna_marks.data_ms);
	seq_printf(s, "ds_ploam irq=%u unsignalled=%u parked=%u\n", luna_ds_irqs, luna_ds_unsignalled,
		   luna_ds_parks);
	seq_printf(s, "omci_turnaround_us n=%u max=%u sum=%llu\nomci_turnaround_ring",
		   t->lat.n, t->lat.max_us, t->lat.sum_us);
	for (i = 0; i < n; i++)
		seq_printf(s, " %u", t->out[i]);
	seq_putc(s, '\n');
	kfree(t);
	return 0;
}

/* The OMCI identity stock's omci_app takes from the unit's MIB; gpon_provision
 * writes each one here before the serial.  luna_omci_id is what attach seeds a
 * model with; the charp only answers reads of the parameter. */
static char *onu_omci_id[OMCI_ID_FIELDS];

static int onu_omci_id_param_set(const char *val, const struct kernel_param *kp)
{
	enum omci_id_field f = (char **)kp->arg - onu_omci_id;
	unsigned long flags;
	int ret;

	if (!omci_id_set_str(NULL, f, val))
		return -EINVAL;
	ret = param_set_charp(val, kp);
	if (ret)
		return ret;
	spin_lock_irqsave(&luna_omci_lock, flags);
	if (!luna_omci_id_set)
		omci_id_default(&luna_omci_id);
	luna_omci_id_set = true;
	omci_id_set_str(&luna_omci_id, f, val);
	if (luna_omci.onu)
		luna_omci.onu->id = luna_omci_id;
	spin_unlock_irqrestore(&luna_omci_lock, flags);
	return 0;
}
static const struct kernel_param_ops onu_omci_id_ops = {
	.set = onu_omci_id_param_set,
	.get = param_get_charp,
};
#define LUNA_OMCI_ID_PARAM(id, name, member, numeric)			\
	module_param_cb(onu_##name, &onu_omci_id_ops, &onu_omci_id[OMCI_ID_##id], 0644); \
	MODULE_PARM_DESC(onu_##name, "OMCI identity " #name			\
			 " (the unit's MIB value; unset = the build default)");
OMCI_ID_LIST(LUNA_OMCI_ID_PARAM)

/* Two units of one board may run stocks that announce different counts, so
 * the unit's own arrives here, pinned beside its serial in its boot
 * arguments: luna_gpon.onu_capacity=<T-CONTs>,<queues>,<schedulers>. */
static int onu_capacity_param_set(const char *val, const struct kernel_param *kp)
{
	unsigned long flags;
	const char *why;
	int ret;

	if (omci_onu_declare_unit_capacity(NULL, val, &why) == OMCI_UNI_DECL_BAD)
		return -EINVAL;
	ret = param_set_charp(val, kp);
	if (ret)
		return ret;
	spin_lock_irqsave(&luna_omci_lock, flags);
	if (luna_omci.onu)
		luna_omci_unit_capacity(luna_omci.onu);
	spin_unlock_irqrestore(&luna_omci_lock, flags);
	return 0;
}
static const struct kernel_param_ops onu_capacity_ops = {
	.set = onu_capacity_param_set,
	.get = param_get_charp,
};
module_param_cb(onu_capacity, &onu_capacity_ops, &onu_capacity, 0644);
MODULE_PARM_DESC(onu_capacity, "this unit's stock T-CONTs,priority queues,schedulers "
		 "(unset = the board's onu-capacity)");


/* Bind an OLT-assigned Alloc-ID to a T-CONT in the GTC alloc ...
 * dev/MEASURED-luna_gpon.c.md sec 217. */
static int luna_queue_drain(u8 qid)
{
	int rc;

	rc = gpon_ind_poll(&pi_io, reg_make(PI_DRN_CMD), 1u, 10000,
			   pi_pause_1us);
	if (rc < 0)
		return rc;
	rc = gpon_ind_go(&pi_io, reg_make(PI_DRN_CMD),
			 (1u << 2) | ((qid & 0x7f) << 3) | (1u << 1),
			 1u, 10000, pi_pause_1us);
	return rc < 0 ? rc : 0;
}

static int gpon_install_tcont(u8 tcont, u16 alloc)
{
	int rc;

	if (tcont == GPON_OMCC_TCONT) {
		rc = luna_data_retire();
		if (rc)
			return rc;
	}

	/* The alloc CAM's own registers and 5-bit index mask come from luna_gpon_chip;
	 * the transaction is the same shared helper as the three DS GEM-port binds. */
	rc = luna_alloc_cam_write(tcont, alloc);
	if (rc < 0) {
		pr_err("luna-gpon: T-CONT alloc bind timeout\n");
		return rc;
	}

	/* PON-MAC US scheduler activation. The GTC alloc CAM above ...
	 * dev/MEASURED-luna_gpon.c.md sec 218. */
	{
		/* The queue comes from the PER-CHIP resolver, not a formula ...
		 * dev/MEASURED-luna_gpon.c.md sec 219. */
		u8 qid = (tcont == GPON_OMCC_TCONT_ALT) ? swc->omcc_phys_qid
						       : luna_tcont_phys_qid(tcont);
		int drain_rc;

		pi_packed_set(PI_PON_TCONT_EN, tcont, 1, 1);	/* PON_TCONT_EN[tcont] (1b packed) */
		/* Drain out the physical queue BEFORE binding it into the ...
		 * dev/MEASURED-luna_gpon.c.md sec 220. */
		drain_rc = luna_queue_drain(qid);
		if (drain_rc < 0) {
			pr_err("luna-gpon: qid %u drain-out failed (%d)\n", qid, drain_rc);
			return drain_rc;
		}
		if (ctrl_queue && tcont == GPON_DATA_TCONT) {
			drain_rc = luna_queue_drain(qid + 1);
			if (drain_rc < 0) {
				pr_err("luna-gpon: control qid %u drain-out failed (%d)\n",
				       qid + 1, drain_rc);
				return drain_rc;
			}
		}
		pi_packed_set(PI_PON_SCH_QMAP, tcont, swc->sch_qmap_bits,
			      ctrl_queue && tcont == GPON_DATA_TCONT ? 0x3 : 0x1);
						/* PON_SCH_QMAP[tcont] = logical-q0; the entry is 32b
						 * wide on the RTL9602C and 8b on the RTL9603CVD, so
						 * `base + tcont * 4` was a per-chip fact too. */
		pi_field(PI_PON_QID_PIR_RATE + qid * PI_QID_RATE_STRIDE, 17, 0, 0x3ffff);	/* PON_QID_PIR_RATE[qid] = MAX (1 word/qid) */
		pi_field(PI_PON_QID_CIR_RATE + qid * PI_QID_RATE_STRIDE, 17, 0, 0);		/* PON_QID_CIR_RATE[qid] = 0 (live-stock; STRICT uses PIR only) */
		pi_packed_set(PI_PON_WFQ_TYPE, qid, 1, 0);	/* PON_WFQ_TYPE[qid] = STRICT (1b packed) */
		/* PON_WFQ_WEIGHT[qid] = 1 (10 bits/entry, 3 entries per word): stock writes
		 * weight 1 even for a STRICT queue, because a zero-weight queue is skipped
		 * by the WFQ round. */
		pi_packed_set(PI_PON_WFQ_WEIGHT, qid, 10, 1);
		if (ctrl_queue && tcont == GPON_DATA_TCONT) {
			pi_field(PI_PON_QID_PIR_RATE + (qid + 1) * PI_QID_RATE_STRIDE, 17, 0, 0x3ffff);
			pi_field(PI_PON_QID_CIR_RATE + (qid + 1) * PI_QID_RATE_STRIDE, 17, 0, 0);
			pi_packed_set(PI_PON_WFQ_TYPE, qid + 1, 1, 0);
			pi_packed_set(PI_PON_WFQ_WEIGHT, qid + 1, 10, 1);
		}
		/* Re-issue the OMCC classifier's SID-valid bit NOW, after the ...
		 * dev/MEASURED-luna_gpon.c.md sec 221. */
		if (sidvalid_last && qid == GPON_OMCC_PHYS_QID) {
			/* arm_ctx witness: sample the queue's arm state at the ...
			 * dev/MEASURED-luna_gpon.c.md sec 222. */
			u32 en   = pi_packed_get(PI_PON_TCONT_EN, tcont, 1);
			u32 qmap = pi_packed_get(PI_PON_SCH_QMAP, tcont, swc->sch_qmap_bits) & 0x3u;
			u32 pir  = pi_rd(PI_PON_QID_PIR_RATE + qid * PI_QID_RATE_STRIDE) & 0x3ffffu;

			if (en && qmap && pir)
				luna_log("SIDVALID[%u] arm_ctx OK (tcont_en=1 sch_qmap=%u pir=0x%x) -> re-issuing on armed queue\n",
					GPON_OMCC_FLOW, qmap, pir);
			else
				pr_warn("luna-gpon: SIDVALID[%u] re-issued with arm INCOMPLETE (tcont_en=%u sch_qmap=%u pir=0x%x) -> binding committed against un-armed queue\n",
					GPON_OMCC_FLOW, en, qmap, pir);

			/* Plain re-issue = 1, matching stock's queue_add tail: a same-value RMW is
			 * still a real strobe, because the HW re-commits on the write ORDER and not
			 * on a 0->1 edge. NO clear: a momentary 0 can drop an in-flight US burst. */
			pi_packed_set(PI_PON_SIDVALID, GPON_OMCC_FLOW, 1, 1);	/* re-issue = 1 (RMW strobe) */
		}
		/* PON_SCH_CTRL is NOT written here: PIR_DROP is cleared for rev-A in
		 * rtl9602c_datapath_tables_init(), and this board's own stock k0 binary
		 * confirms rev-A clears it "due to the tcont 16". */
	}

	/* The GMII re-latch used to be here, at Assign_ONU-ID time, ...
	 * dev/MEASURED-luna_gpon.c.md sec 321. */

	luna_log("T-CONT %u <- alloc 0x%x bound (compl %d)\n",
		tcont, alloc, rc);
	return 0;
}

/* A failure leaves an ownership obligation, including an uncompleted CAM
 * request. Never erase that ledger merely because installed is false. */
static bool luna_data_dirty(void)
{
	return luna_data.alloc_dirty || luna_data.queue_dirty || luna_data.gem_dirty ||
	       luna_data.sid_dirty || luna_data.stamp_dirty;
}

static void luna_data_suspend(void)
{
	WRITE_ONCE(luna_data_admitted, false);
	luna_data.armed.installed = false;
	gpon_data_installed = false;
	gpon_ploam_set_data_installed(&luna_ploam, false);
}

static int luna_data_retire(void)
{
	u32 qmap;
	int rc;

	luna_data_suspend();
	/* A producer that already passed the WAN gate finishes publishing before
	 * hardware withdrawal. Already-owned descriptors are not claimed drained
	 * by this CPU fence; the selected dedicated PON queue is drained below. */
	if (luna_omci.tx_fence)
		luna_omci.tx_fence(luna_omci.cookie);
	if (luna_data.sid_dirty) {
		pi_packed_set(PI_PON_SIDVALID, GPON_DATA_FLOW, 1, 0);
		pi_packed_set(PI_PON_SIDVALID, GPON_CTRL_FLOW, 1, 0);
		if (pi_packed_get(PI_PON_SIDVALID, GPON_DATA_FLOW, 1) ||
		    pi_packed_get(PI_PON_SIDVALID, GPON_CTRL_FLOW, 1))
			return -EIO;
		luna_data.sid_dirty = false;
	}
	if (luna_data.stamp_dirty) {
		if (!gpon_gtc_us_gem_stamp(&gpon_io, &luna_gpon_chip.gtc, GPON_DATA_FLOW, 0) ||
		    !gpon_gtc_us_gem_stamp(&gpon_io, &luna_gpon_chip.gtc, GPON_CTRL_FLOW, 0))
			return -ENODEV;
		if (gpon_rd(reg_at(luna_gpon_chip.gtc.gem_us_port_map) +
			    GPON_DATA_FLOW * luna_gpon_chip.gtc.gem_us_port_stride) & 0xfff)
			return -EIO;
		luna_data.stamp_dirty = false;
	}
	if (luna_data.queue_dirty) {
		/* Only a dedicated data T-CONT is owned here. Never drain the OMCC. */
		if (luna_data.armed.rides_omcc || luna_data_qid == GPON_OMCC_PHYS_QID)
			return -EINVAL;
		qmap = pi_packed_get(PI_PON_SCH_QMAP, GPON_DATA_TCONT, swc->sch_qmap_bits);
		/* This owner has the data queue and the control queue next to it. An
		 * unexpected member is not ours to strand by cleaning the allocation. */
		if (qmap & ~(BIT(luna_data_qid % swc->tcont_queue_max) |
			     BIT((luna_data_qid + 1) % swc->tcont_queue_max)))
			return -EBUSY;
		rc = luna_queue_drain(luna_data_qid);
		if (!rc && (qmap & BIT((luna_data_qid + 1) % swc->tcont_queue_max)))
			rc = luna_queue_drain(luna_data_qid + 1);
		if (rc)
			return rc;
		qmap &= ~(BIT(luna_data_qid % swc->tcont_queue_max) |
			  BIT((luna_data_qid + 1) % swc->tcont_queue_max));
		pi_packed_set(PI_PON_SCH_QMAP, GPON_DATA_TCONT, swc->sch_qmap_bits, qmap);
		if (pi_packed_get(PI_PON_SCH_QMAP, GPON_DATA_TCONT, swc->sch_qmap_bits) != qmap)
			return -EIO;
		if (!qmap) {
			pi_packed_set(PI_PON_TCONT_EN, GPON_DATA_TCONT, 1, 0);
			if (pi_packed_get(PI_PON_TCONT_EN, GPON_DATA_TCONT, 1))
				return -EIO;
		}
		luna_data.queue_dirty = false;
	}
	if (luna_data.gem_dirty) {
		rc = luna_cam_xact(false, GPON_GTC_CAM_OP_CLEAN, GPON_DATA_FLOW, NULL, NULL);
		if (rc)
			return rc;
		/* Own stock clears these five traffic bits after CAM completion. */
		gpon_field(GPON_GTC_DS_TRAFFIC_CFG + GPON_DATA_FLOW * DS_TRAFFIC_CFG_STRIDE, 4, 0, 0);
		if (gpon_rd(GPON_GTC_DS_TRAFFIC_CFG + GPON_DATA_FLOW * DS_TRAFFIC_CFG_STRIDE) & 0x1f)
			return -EIO;
		luna_data.gem_dirty = false;
	}
	if (luna_data.alloc_dirty) {
		if (luna_data.armed.rides_omcc)
			return -EINVAL;
		rc = luna_cam_xact(true, GPON_GTC_CAM_OP_CLEAN, GPON_DATA_TCONT, NULL, NULL);
		if (rc)
			return rc;
		luna_data.alloc_dirty = false;
	}
	memset(&luna_data.armed, 0, sizeof(luna_data.armed));
	gpon_data_tcont_installed = false;
	luna_ploam.data_tcont_installed = false;
	return 0;
}

#ifdef CONFIG_GPON_GEM_DIAG
/* The FAMILY's half of CONFIG_GPON_GEM_DIAG. ★★ IT PRINTS ...
 * dev/MEASURED-luna_gpon.c.md sec 223. */
static void luna_gem_diag_report(const struct gpon_data_armed *armed,
				 const struct gpon_data_want *want,
				 const struct omci_data_binding *binding)
{
	/* Single caller, and luna_data_reconcile is not re-entered: the FSM poll
	 * that calls it is the one context this driver runs it from. */
	static char last[200];
	char line[200];
	int n;

	n = scnprintf(line, sizeof(line),
		      "ploam-auth=%u omci-gem-present=%u omci-alloc-written=%u | ",
		      gpon_ploam_data_alloc_known(&luna_ploam, binding->alloc_id) ? 1u : 0u,
		      binding->gem_present ? 1u : 0u,
		      binding->alloc_known ? 1u : 0u);
	n += gpon_gem_diag_line(armed, want, line + n, sizeof(line) - n);
	if (n <= 0 || !strcmp(line, last))
		return;
	strscpy(last, line, sizeof(last));
	luna_log("%s\n", line);
}
#endif /* CONFIG_GPON_GEM_DIAG */

static void luna_data_reconcile(void)
{
	struct omci_data_binding binding;
	struct gpon_data_armed visible = luna_data.armed;
	struct gpon_data_want want = { 0 };
	enum gpon_data_plan plan;
	unsigned long flags;
	int rc = 0;

	spin_lock_irqsave(&luna_omci_lock, flags);
	if (luna_omci.onu) {
		omci_data_binding_snapshot(luna_omci.onu, gpon_omcc_gem_port,
					   GPON_MCAST_GEM, &luna_omci.binding);
	}
	binding = luna_omci.binding;
	spin_unlock_irqrestore(&luna_omci_lock, flags);
	want.omcc_alloc = gpon_omcc_alloc ? gpon_omcc_alloc : luna_ploam.onu_id;
	want.omcc_up = READ_ONCE(luna_activation_ready) &&
		luna_ploam.state == GPON_O5_OPERATION && luna_ploam.omcc_installed;
	want.gem = binding.gem_present ? binding.gem_port : 0;
	want.alloc = binding.alloc_id;
	want.alloc_known = binding.gem_present && binding.alloc_known &&
		(want.alloc == want.omcc_alloc ||
		 gpon_ploam_data_alloc_known(&luna_ploam, want.alloc));
	/* Presence here includes submitted-but-uncertain hardware ownership, not
	 * a false claim that the original CAM installation completed. */
	if (luna_data_dirty())
		visible.alloc_bound = true;
	plan = gpon_data_plan_decide(&visible, &want);
#ifdef CONFIG_GPON_GEM_DIAG
	luna_gem_diag_report(&visible, &want, &binding);
#endif
	if (plan == GPON_DATA_TEARDOWN || plan == GPON_DATA_REPLACE ||
	    (luna_data_dirty() && !luna_data.armed.installed)) {
		rc = luna_data_retire();
		goto out; /* Replacement gets a fresh poll; at most one heavy attempt. */
	}
	if (plan == GPON_DATA_KEEP) {
		WRITE_ONCE(luna_data_admitted, want.omcc_up);
		goto out;
	}
	if (plan != GPON_DATA_INSTALL || !data_gem_en) {
		WRITE_ONCE(luna_data_admitted, false);
		goto out;
	}
	luna_data.armed.alloc = want.alloc;
	luna_data.armed.gem = want.gem;
	luna_data.armed.rides_omcc =
		gpon_gem_us_tcont_decide(want.alloc, want.omcc_alloc, false) ==
		GPON_GEM_US_BIND_IS_OMCC;
	luna_data_qid = luna_data.armed.rides_omcc ? GPON_OMCC_PHYS_QID :
		luna_tcont_phys_qid(GPON_DATA_TCONT);
	if (!luna_data.armed.rides_omcc) {
		/* Set obligations BEFORE the first write: a timeout can mean submitted. */
		luna_data.alloc_dirty = luna_data.queue_dirty = true;
		rc = gpon_install_tcont(GPON_DATA_TCONT, want.alloc);
		if (rc)
			goto out;
	}
	luna_data.armed.alloc_bound = true;
	gpon_data_gem_port = want.gem;
	gpon_data_gem_solicited = true;
	rc = gpon_install_data_gem();
	if (!rc) {
		luna_data.armed.installed = true;
		gpon_data_alloc = want.alloc;
		gpon_data_tcont_installed = !luna_data.armed.rides_omcc;
		luna_ploam.data_alloc = want.alloc;
		luna_ploam.data_tcont_installed = gpon_data_tcont_installed;
		WRITE_ONCE(luna_data_admitted, true);
	}
out:
	if (rc && rc != luna_data.error)
		pr_warn("luna-gpon: data reconciliation failed rc=%d; ownership retained\n", rc);
	luna_data.error = rc;
}

static void luna_omci_service(void)
{
	luna_omci_poll();
	luna_data_reconcile();
	luna_omci_report_selftest();
	if (!(luna_ploam.ticks % LUNA_OMCI_ALARM_TICKS))
		luna_omci_report_alarm();
}

/* ★★★ THE OLT'S VLAN SERVICE MODEL REACHES THIS FAMILY -- and what it can do
 * with it is REPORTED rather than claimed.
 *
 * The core models the whole of ME 171: it decodes each row, DECIDES it into a
 * filter+treatment, and hands it to a family through `struct gpon_vlan_ops`,
 * counting four outcomes. Measured 2026-09-16: NO family filled that table and
 * NOBODY called the walker, so the entire model -- decode, decide, emit -- was
 * dead code, and the service the OLT provisions reached nothing at all. That is
 * the same shape as `rtk_tr142.ko`, which is precisely this translation on the
 * vendor side, and the same shape as `pf_rg`'s missing half.
 *
 * ⚠⚠ `rule_install` REFUSES, ON PURPOSE, AND THAT IS NOT A STUB. No Luna VLAN
 * rule encoding has been MEASURED for these shapes: writing one from the switch
 * VLAN tables because they look close would put an unmeasured rule on a WAN that
 * works today, which is the defect this tree calls a value the author supplied
 * where a human had to register one. A refusal is a THIRD answer and the ledger
 * keeps it apart from "nobody was listening": `failed` means the family was
 * asked and said no, `no_installer` means nothing was there to ask.
 *
 * ⇒ what this buys NOW is the measurement that was missing: the OLT's rows, per
 * instance, with the shapes it actually asks for. The next increment implements
 * those shapes and not a generic VLAN engine.
 */
static int luna_vlan_rule_install(void *ctx, const struct gpon_vlan_rule *r)
{
	(void)ctx; (void)r;
	return -EOPNOTSUPP;	/* asked, and refused -- see the note above */
}

static int luna_vlan_rule_remove(void *ctx, const struct gpon_vlan_rule *r)
{
	(void)ctx; (void)r;
	return -EOPNOTSUPP;
}

static const struct gpon_vlan_ops luna_vlan_ops = {
	.rule_install	= luna_vlan_rule_install,
	.rule_remove	= luna_vlan_rule_remove,
};

/* Walk every ME 171 the OLT provisioned and render what came back.  It installs
 * nothing (see above), so reading this file cannot move the data path. */
static void luna_vlan_service_show(struct seq_file *s)
{
	struct gpon_vlan_commit_result tot = { 0 }, one;
	struct gpon_vlan_inst_attrs a = { 0 };
	struct omci_onu *onu;
	unsigned long flags;
	unsigned int i, insts = 0;

	spin_lock_irqsave(&luna_omci_lock, flags);
	onu = luna_omci.onu;
	spin_unlock_irqrestore(&luna_omci_lock, flags);
	if (!onu) {
		seq_puts(s, "vlan_service: no ONU attached -- could not ask\n");
		return;
	}
	/* ⚠ WALK THE MODEL'S OWN SLOTS. The first cut invented an accessor
	 * (`gpon_ext_vlan_inst`) that does not exist -- the record carries its
	 * own `inst` and `used`, so reading it is the honest enumeration. */
	for (i = 0; i < GPON_EXT_VLAN_MAX; i++) {
		u16 inst = onu->vlan.ext[i].inst;

		if (!onu->vlan.ext[i].used ||
		    !gpon_ext_vlan_nrow(&onu->vlan, inst))
			continue;
		insts++;
		gpon_vlan_commit_inst(&onu->vlan, &luna_vlan_ops, NULL, inst,
				      &a, &one);
		tot.installed += one.installed;
		tot.failed += one.failed;
		tot.no_installer += one.no_installer;
		tot.undecided += one.undecided;
	}
	seq_printf(s,
		   "vlan_service: me171_inst=%u rows{installed=%u refused=%u no_installer=%u undecided=%u} attr_owed=%u\n",
		   insts, tot.installed, tot.failed, tot.no_installer,
		   tot.undecided, onu->vlan.attr_owed);
	seq_puts(s,
		 "vlan_service: refused = this family was ASKED and said no (no Luna rule encoding is measured for these shapes); undecided = the CORE could not represent the row\n");
}

static void luna_omci_seq_show(struct seq_file *s)
{
	char resp[320] = "", far[96];
	unsigned long flags;
	u32 queued, count, overflow, closed, accepted, resets, tx_errors;

	spin_lock_irqsave(&luna_omci_lock, flags);
	if (luna_omci.onu) {
		omci_resp_fmt(luna_omci.onu, READ_ONCE(luna_activation_ready),
			      READ_ONCE(luna_omci.tx_sent), READ_ONCE(luna_omci.tx_errors),
			      resp, sizeof(resp));
	}
	omci_farend_fmt(luna_omci.onu, far, sizeof(far));
	queued = luna_omci.queued;
	count = luna_omci.count;
	overflow = luna_omci.overflow;
	closed = luna_omci.closed;
	accepted = luna_omci.accepted;
	resets = luna_omci.resets;
	tx_errors = READ_ONCE(luna_omci.tx_errors);
	spin_unlock_irqrestore(&luna_omci_lock, flags);
	seq_puts(s, resp);
	seq_puts(s, far);
	seq_printf(s, "omci_queue: queued=%u pending=%u overflow=%u closed=%u accepted=%u resets=%u tx_errors=%u\n",
		   queued, count, overflow, closed, accepted, resets, tx_errors);
	luna_vlan_service_show(s);
	seq_printf(s, "data_owner: admitted=%u installed=%u alloc_bound=%u alloc=%u gem=%u qid=%u error=%d\n",
		   luna_gpon_data_ready(), READ_ONCE(luna_data.armed.installed),
		   READ_ONCE(luna_data.armed.alloc_bound), READ_ONCE(luna_data.armed.alloc),
		   READ_ONCE(luna_data.armed.gem), READ_ONCE(luna_data_qid), READ_ONCE(luna_data.error));
}



/* Several upstream-config registers (US_CFG, US_LASER, ...
 * dev/MEASURED-luna_gpon.c.md sec 224. */
static void gpon_wr_us_protected(u32 off, u32 val)
{
	gpon_wr(GPON_GTC_US_WRITE_PROTECT, GPON_US_WP_UNLOCK);
	gpon_wr(off, val);
	gpon_wr(GPON_GTC_US_WRITE_PROTECT, GPON_US_WP_LOCK);
}

/* Program the upstream burst-mode overhead (PLOu preamble + ...
 * dev/MEASURED-luna_gpon.c.md sec 225. */
static void gpon_apply_boh(bool ranged)
{
	u8 oh[GPON_BOH_LEN];
	u8 guard = gpon_boh_guard, t3 = ranged ? gpon_boh_t3ranged : gpon_boh_t3pre;
	u8 rep, i, boh_len, size;
	unsigned int want;

	/* Exact port of the stock burst-overhead build. The old code ...
	 * dev/MEASURED-luna_gpon.c.md sec 322. */
	if (guard > 32)
		guard = 32;
	rep = guard / 8;			/* boh_repeat = whole guard bytes (fill index) */

	/* WIDEN BEFORE CLAMPING. `rep + t3 + 3` reaches 262 and used ...
	 * dev/MEASURED-luna_gpon.c.md sec 226. */
	want = t3 ? (unsigned int)rep + t3 + 3 : GPON_BOH_LEN;
	if (want > GPON_BOH_MAX_LEN) {
		/* The value came off the wire and the hardware field cannot hold
		 * it, so it is a RANGE finding, not support work: report it with
		 * the guard and t3 that produced it, then clamp. */
		u8 dmp[2] = { guard, t3 };

		gpon_unsup_report("boh_len", GPON_UNSUP_RANGE, want,
				  "at-most-252", dmp, sizeof(dmp));
		want = GPON_BOH_MAX_LEN;
	}
	boh_len = (u8)want;		/* total burst-overhead length */
	size = (boh_len > GPON_BOH_LEN) ? GPON_BOH_LEN : boh_len;	/* stored bytes (<=12) */
	if (size < 4)				/* need room for >=1 fill + 3 delimiter */
		size = 4;

	memset(oh, 0xaa, sizeof oh);
	for (i = 0; i < rep && i < (u8)(size - 3); i++)
		oh[i] = 0xaa;			/* guard bytes */
	for (; i < (u8)(size - 3); i++)
		oh[i] = gpon_boh_ptn;		/* Type-3 preamble fill (= bursthead[rep]) */
	oh[size - 3] = gpon_boh_delim[0];	/* delimiter at the TRUE end of the burst */
	oh[size - 2] = gpon_boh_delim[1];
	oh[size - 1] = gpon_boh_delim[2];

	/* BOH_REPEAT is NOT guard/8: it is the stored-byte index of ...
	 * dev/MEASURED-luna_gpon.c.md sec 227. */
	gpon_wr(GPON_GTC_US_BOH_CFG,
		(((size - 4) & 0xf) << 8) | (boh_len & 0xff));	/* REPEAT=size-4, LENGTH=full */
	for (i = 0; i < size; i++)
		gpon_wr(GPON_GTC_US_BOH_DATA + i * 4, oh[i]);

	luna_log("BOH %s guard=%u rep=%u boh_repeat=%u ptn=0x%02x delim=%02x%02x%02x t3=%u boh_len=%u size=%u oh=%*phN\n",
		ranged ? "ranged" : "prerng", guard, rep, (size - 4) & 0xf, gpon_boh_ptn,
		gpon_boh_delim[0], gpon_boh_delim[1], gpon_boh_delim[2],
		t3, boh_len, size, size, oh);
}

/* Upstream equalization delay. The OLT-visible burst time is ...
 * dev/MEASURED-luna_gpon.c.md sec 228. */
static void gpon_set_eqd(u32 value)
{
	u32 min_delay1 = (gpon_rd(GPON_GTC_US_MIN_DELAY) >> 7) & 0x1ff;
	u32 eqd1  = value + min_delay1 * 128;
	/* The CORE's constant, not a second spelling of it: this arithmetic and
	 * gpon_ploam.c's set_eqd() must divide by the same number or the A/B
	 * switch between them would range the ONU differently. */
	u32 multi = eqd1 / GPON_PLOAM_EQD_FRAME_LEN;
	u32 intra = eqd1 - multi * GPON_PLOAM_EQD_FRAME_LEN;

	gpon_wr(GPON_GTC_US_EQD,
		((multi & GPON_EQD_MF_MASK) << GPON_EQD_MF_SHIFT) |
		(intra & GPON_EQD_INFRAME_MASK));
}

/* HYBRID LAN/VLAN switch. VLAN filtering must be ON during ...
 * dev/MEASURED-luna_gpon.c.md sec 323. */
static unsigned int vlan_lan_o5_ticks = 4000;
module_param(vlan_lan_o5_ticks, uint, 0644);
MODULE_PARM_DESC(vlan_lan_o5_ticks, "poll ticks held at O5 before clearing VLAN_FILTER for LAN access (0=keep filtering on)");
static u32 gpon_o5_entry_tick;
static bool gpon_vlan_lan_open;

/* Re-lock the TX CMU PLL at O3 entry. A fresh power-on under ...
 * dev/MEASURED-luna_gpon.c.md sec 229. */
static void gpon_txpll_relock(void)
{
	if (!serdes_txpll_relock)
		return;
	if (sw_rd(SDS_ANA_COM_REG27) & SDS_CMU_EN) {
		sw_field(SDS_ANA_COM_REG27, 10, 10, 0);		/* CMU enable -> 0      */
		udelay(100);
		sw_field(SDS_ANA_COM_REG27, 10, 10, 1);		/* -> 1 (re-acquire)    */
		udelay(100);
	}
	sw_field(WSDS_DIG_1D, 14, 14, 0);			/* FIFO r/w ptr re-sync */
	sw_field(WSDS_DIG_1D, 14, 14, 1);
	luna_log("TX-PLL relock (CMU re-toggle + FIFO re-sync) at O3 entry\n");
}

/* The ACTION is lifted for the core's ops; the POLICY stays: the core decides
 * WHEN to reseat the CDR and when to arm the key switch, while whether this
 * board should skip a reseat after a healthy O5 is this shell's rule. */
static void gpon_cdr_reseat(void)
{
	/* Re-seat US-TX SerDes interface reset-B (softirq-safe, TX-only; the locked
	 * DS RX framer is undisturbed) so the next re-range starts from a fresh
	 * serializer lock instead of the prior marginal one. */
	sw_field(WSDS_DIG_1D, 16, 16, 0);
	udelay(500);
	sw_field(WSDS_DIG_1D, 16, 16, 1);
}

static void gpon_aes_arm_switch(u32 fc)
{
	gpon_wr(0x3014, fc);	/* AES_KEY_SWITCH_TIME[29:0] */
}


/* The REGISTER CLUSTER alone, and the split is the core's ...
 * dev/MEASURED-luna_gpon.c.md sec 230. */
static void gpon_o5_rearm_burst_regs(void)
{
	if (!READ_ONCE(luna_activation_ready))
		return;
	/* Re-apply the O5 packed-burst gate cluster on EVERY O5 entry, not just at
	 * __init: a re-range performs a GMAC/SDS reset that can clear these US-side
	 * registers ("isolated tolerates, packed exposes"). US-side only. */
	if (!o5_rearm_burst_gate)
		return;
	gpon_wr_us_protected(0x5188, gtune->us_optic_sd_th);	/* US_OPTIC_SD_TH, per chip */
	gpon_field(0x526c, 0, 0, 1);			/* US_PWR_SAV_MODE */
	if (gtune->us_pwr_sav_dg_tx_opt)
		gpon_field(0x526c, 16, 16, 1);		/* DG_TX_OPT, per chip */
	gpon_wr(GPON_GEM_US_PWR_SAV_CFG, (0x10u << 16) | 0x100u);	/* GEM_US_PWR_SAV_CFG */
	gpon_wr(GPON_GEM_US_EOB_MERGE, 0x00000028u);			/* GEM_US_EOB_MERGE */
}

/* The cluster PLUS the auto-No_message keepalive re-arm: what THIS driver's own
 * FSM has always done on an O5 entry, unchanged.  The core FSM must NOT reach
 * this one -- it sends its own No_message. */

static void gpon_below_o5(void)
{
	luna_data_suspend();
	gpon_rerange_start_j = jiffies ? jiffies : 1;	/* start the outage timer */
	gpon_o5_entry_tick = 0;
	if (gpon_vlan_lan_open && !lan_keep_open) {
		sw_field(SW_VLAN_CTRL, 0, 0, 1);	/* re-assert VLAN_FILTER for re-config */
		gpon_vlan_lan_open = false;
		luna_log("re-range -> VLAN_FILTER re-armed (config phase)\n");
	}
	/* lan_keep_open (default): leave VLAN_FILTER cleared so LAN management
	 * survives the WAN-down/re-range; the OLT re-config on resume tolerates it. */
}

static void gpon_fsm_set_state(u8 st)
{
	u8 prev = gpon_fsm_state;

	/* gpon_hold: keep the FSM parked at O1 — refuse every advance past O1 so the
	 * GPON never ranges/deactivates and the shared switch datapath stops churning,
	 * leaving br-lan + the WiFi AP stable for LAN+WiFi access. (GPON/WAN off.) */
	if ((!READ_ONCE(luna_activation_ready) || gpon_hold) && st > 1)
		return;
	/* The HW ONU_STATE field uses our 1-based numbers (O1..O5 = 1..5) and
	 * gates the GTC's auto-SN/ranging transmitter.  Written BEFORE any
	 * print: the console is synchronous, and a GTC left at O4 behind the
	 * ranged flush answered the OLT's O5 grants as ranging requests (field
	 * X111W, production OLT, 2026-10-02). */
	gpon_field(GPON_GTC_DS_ONU_ID_STATUS, 3, 0, st);
	if (gpon_fsm_state != st)
		luna_log("ONU state O%u -> O%u\n", gpon_fsm_state, st);
	if (prev != st)
		gpon_los_run = 0;	/* fresh LOS debounce window on every transition */
	gpon_fsm_state = st;

	/* Hybrid LAN/VLAN bookkeeping: mark O5 entry; on any drop below O5, re-arm VLAN
	 * filtering for the next config and reset the LAN-open timer. */
	if (st == 5 && prev != 5) {
		/* Re-range completion diagnostic: if we had dropped below O5 ...
		 * dev/MEASURED-luna_gpon.c.md sec 324. */
		if (gpon_rerange_start_j) {
			gpon_rerange_cnt++;
			gpon_last_outage_ms = jiffies_to_msecs(jiffies - gpon_rerange_start_j);
			gpon_rerange_start_j = 0;
			if (!gpon_rerange_last_log_j ||
			    time_after(jiffies, gpon_rerange_last_log_j + msecs_to_jiffies(2000))) {
				luna_log("re-range #%u -> O5 (outage ~%u ms); data-GEM re-install pending\n",
					gpon_rerange_cnt, gpon_last_outage_ms);
				gpon_rerange_last_log_j = jiffies;
			}
		}
		gpon_o5_entry_tick = gpon_fsm_ticks ? gpon_fsm_ticks : 1;
		luna_mark(&luna_marks.o5_ms);
		gpon_avc_sent = 0;	/* re-report oper-up to the OLT each online */
		/* Re-apply the O5 packed-burst gate cluster + re-arm the HW ...
		 * dev/MEASURED-luna_gpon.c.md sec 231. */
	}
	gpon_led_pon_set(st);
}

/* Process-context worker for the stock SerDes CDR-reset ...
 * dev/MEASURED-luna_gpon.c.md sec 232. */
static bool full_serdes_reinit;	/* default OFF: A/B 2026-06-15 found re-running the full
				 * gpon_serdes_init on re-range BREAKS the GPON (0/5 boots, link never
				 * reaches O5 — the CMU/SDS reset races the FSM re-acquisition). Keep the
				 * light CDR pulse instead. Do NOT enable. */
module_param(full_serdes_reinit, bool, 0644);
MODULE_PARM_DESC(full_serdes_reinit, "re-range: re-run full gpon_serdes_init (BROKEN, default off) vs light CDR pulse");
static void gpon_cdr_reset_worker(struct work_struct *w)
{
	if (!READ_ONCE(luna_activation_ready))
		return;
	u32 cdr;

	if (!serdes_cdr_reset)
		return;
	if (full_serdes_reinit) {
		gpon_serdes_init();	/* full analog re-init: fresh lock attempt */
		luna_log("re-range FULL serdes re-init\n");
		return;
	}
	/* The DS GTC runs on the recovered clock: while the CDR is reset, neither the
	 * GPON interrupt nor the tick (luna_fsm_lock) may read it. */
	if (luna_gpon_irq)
		disable_irq(luna_gpon_irq);
	spin_lock_bh(&luna_fsm_lock);
	cdr = sw_rd(SDS_ANA_COM_REG12);
	sw_wr(SDS_ANA_COM_REG12, cdr ^ BIT(15));
	mdelay(10);
	sw_wr(SDS_ANA_COM_REG12, cdr);
	spin_unlock_bh(&luna_fsm_lock);
	if (luna_gpon_irq)
		enable_irq(luna_gpon_irq);
	/* the computed address, for the reason given at the other call site */
	luna_log("re-range serdesCdr_reset pulse (COM_REG12 @ 0x%05x bit15), restored=0x%08x\n",
		SDS_ANA_COM_REG12, cdr);
}

/* THIS FSM IS THE DUPLICATE. DO NOT EXTEND IT (2026-08-27). ...
 * dev/MEASURED-luna_gpon.c.md sec 233. */
static void luna_op_ploam_tx(void *sh, u8 queue, const u8 m[GPON_PLOAM_US_LEN])
{
	(void)sh;			/* single-instance driver: state is file-scope */
	gpon_send_cpu_ploam(queue, m);
}

static void luna_op_arm_dying_gasp(void *sh, const u8 m[GPON_PLOAM_US_LEN])
{
	(void)sh;
	gpon_send_cpu_ploam(PLM_US_QUEUE_DYING_GASP, m);
}

static void luna_op_boh_write(void *sh, u32 cfg_word, const u8 *oh, u8 size)
{
	u8 i;

	(void)sh;
	/* The write half of gpon_apply_boh(): the core composed cfg_word and the
	 * overhead bytes, so only the registers are left here. */
	gpon_wr(GPON_GTC_US_BOH_CFG, cfg_word);
	for (i = 0; i < size; i++)
		gpon_wr(GPON_GTC_US_BOH_DATA + i * 4, oh[i]);
}

static u32 luna_op_get_min_delay(void *sh)
{
	(void)sh;
	return (gpon_rd(GPON_GTC_US_MIN_DELAY) >> 7) & 0x1ff;	/* MIN_DELAY1 */
}

static void luna_op_set_eqd(void *sh, u32 multiframe, u32 intraframe)
{
	(void)sh;
	gpon_wr(GPON_GTC_US_EQD,
		((multiframe & GPON_EQD_MF_MASK) << GPON_EQD_MF_SHIFT) |
		(intraframe & GPON_EQD_INFRAME_MASK));
}

static void luna_op_us_ploam_flush(void *sh)
{
	(void)sh;
	/* PLM_FLUSH_BUF is edge-triggered, 0 THEN 1, ... -- dev/MEASURED-luna_gpon.c.md sec 234. */
	gpon_field(GPON_GTC_US_PLOAM_CFG, 4, 4, 0);
	gpon_field(GPON_GTC_US_PLOAM_CFG, 4, 4, 1);
}

static void luna_op_set_hw_state(void *sh, enum gpon_ostate st)
{
	(void)sh;
	gpon_fsm_set_state((u8)st);
}

static void luna_op_set_hw_onu_id(void *sh, u8 onu_id)
{
	(void)sh;
	/* BOTH registers. The ONU-ID lives in two places on this GTC
	 * dev/MEASURED-luna_gpon.c.md sec 235. */
	gpon_field(GPON_GTC_DS_ONU_ID_STATUS, 15, 8, onu_id);
	gpon_field(GPON_GTC_US_ONU_ID, 15, 8, onu_id);
}

static void luna_op_on_below_o5(void *sh)
{
	(void)sh;
	gpon_below_o5();
}

static void luna_op_o5_rearm_burst(void *sh)
{
	(void)sh;
	/* The CLUSTER ONLY: gpon_ploam.h's contract ends with "the core emits the
	 * No_message that follows it", and it does. Wiring this op to
	 * gpon_o5_rearm_burst(), which sends one too, put TWO No_messages on the
	 * wire per O5 entry. The native FSM keeps the pair. */
	gpon_o5_rearm_burst_regs();
}

static void luna_op_install_data_gem(void *sh, u16 gem)
{
	(void)sh;
	(void)gem; /* Reconciliation below owns the accepted model and hardware. */
}

static void luna_op_cdr_reseat(void *sh)
{
	(void)sh;
	/* BOTH halves: the interface reset-B re-strobe does not ...
	 * dev/MEASURED-luna_gpon.c.md sec 236. */
	gpon_cdr_reseat();
	schedule_work(&gpon_cdr_reset_work);
}

static void luna_op_aes_arm_switch(void *sh, u32 superframe)
{
	(void)sh;
	gpon_aes_arm_switch(superframe);
}

static void luna_op_rng(void *sh, u8 *out, unsigned int len)
{
	(void)sh;
	/* The core wants a randomness SOURCE and this shell already uses the
	 * kernel's; gpon_send_key() reaches the same generator. */
	get_random_bytes(out, len);
}

static void luna_op_omci_report_oper_up(void *sh)
{
	(void)sh;
	/* Already an EXPORT_SYMBOL'd cross-module call this driver makes: the
	 * Ethernet driver owns the OMCI shell that reports the VEIP AVC. */
	rtl9602c_eth_omci_report_oper_up();
}

static int luna_op_ds_bip_errors(void *sh, u32 *count)
{
	(void)sh;
	*count = gpon_rd(GPON_GTC_DS_MISC_CNTR_BIP_ERR_BLK);	/* REI is its one reader */
	return 0;
}

static void luna_op_analog_relock(void *sh)
{
	(void)sh;
	gpon_txpll_relock();
	WRITE_ONCE(luna_laser_restrobe_owed, 1);
}

static void luna_op_o3_feed_reset(void *sh)
{
	(void)sh;
	/* The same body the native O3 branch runs, and it must be: this op is reached
	 * from gpon_ploam_ds() on the FSM timer, while gpon_us_feed_rearm() is the
	 * PROCESS-CONTEXT variant that reaches __get_free_pages(GFP_KERNEL). */
	gpon_o3_feed_unpark();
}

static void luna_op_aes_stage_key(void *sh, const u8 key[16])
{
	(void)sh;
	gpon_aes_stage_key(key);
}

static int luna_op_install_omcc(void *sh, u16 gem)
{
	(void)sh;
	return gpon_install_omcc(gem);
}

static int luna_op_install_tcont(void *sh, u8 tcont, u16 alloc)
{
	(void)sh;
	/* Park the unused CAM entries FIRST, so the entry about to be ...
	 * dev/MEASURED-luna_gpon.c.md sec 237. */
	if (alloc_cam_park && tcont == GPON_OMCC_TCONT) {
		int rc = gpon_alloc_cam_clear_others(GPON_OMCC_TCONT);

		if (rc)
			return rc;
	}
	return gpon_install_tcont(tcont, alloc);
}

/* `trace` is the one op left NULL-able: gpon_ploam.h calls it ...
 * dev/MEASURED-luna_gpon.c.md sec 238. */
static struct gpon_ploam_cfg luna_ploam_cfg_live __maybe_unused;

static const struct gpon_ploam_cfg luna_ploam_cfg __maybe_unused = {
	.hold			= false,	/* set from gpon_hold at init */
	.cdr_reseat_on_reactivate = true,	/* from cdr_reseat_on_reactivate */
	.o5_rearm_burst_gate	= true,		/* from o5_rearm_burst_gate */
	.o3_feed_reset		= false,	/* from o3_feed_reset */
	.data_gem_en		= true,		/* from data_gem_en */
	.omcc_alt_bind		= false,	/* from omcc_alt_bind */
	/* The STATIC default only: 0 means "use the LIVE ONU-ID", which is the rule.
	 * The live copy below carries gpon_omcc_alloc, a module_param the native FSM
	 * honours -- nothing copied it here, so with core_fsm=1 the SAME knob was
	 * silently ignored while core_fsm=0 obeyed it (ploam_fsm_diff_test W3). */
	.omcc_alloc_override	= 0,
	.omcc_tcont		= GPON_OMCC_TCONT,	/* 16 on Luna */
	.omcc_tcont_alt		= GPON_OMCC_TCONT_ALT,	/* 1, only when alt_bind */
	.data_tcont		= GPON_DATA_TCONT,	/* 8 on Luna */
};

/* The core's events, spoken out loud. `trace` is the ONE ...
 * dev/MEASURED-luna_gpon.c.md sec 239. */
static const char * const luna_ev_name[] = {
	[GPON_PLOAM_EV_STATE]		= "state",
	[GPON_PLOAM_EV_RERANGE_DONE]	= "rerange_done",
	[GPON_PLOAM_EV_BOH]		= "boh",
	[GPON_PLOAM_EV_DS]		= "ds",
	[GPON_PLOAM_EV_O3_FEED_RESET]	= "o3_feed_reset",
	[GPON_PLOAM_EV_ONU_ID]		= "onu_id",
	[GPON_PLOAM_EV_CAM_READBACK]	= "cam_readback",
	[GPON_PLOAM_EV_RANGING_TIME]	= "ranging_time",
	[GPON_PLOAM_EV_DISABLE_SN]	= "disable_sn",
	[GPON_PLOAM_EV_DEACT]		= "DEACT",
	[GPON_PLOAM_EV_DEACT_KEEP_LOCK]	= "DEACT_keep_lock",
	[GPON_PLOAM_EV_EXT_BURST]	= "ext_burst",
	[GPON_PLOAM_EV_ACK]		= "ack",
	[GPON_PLOAM_EV_ASSIGN_ALLOC]	= "assign_alloc",
	[GPON_PLOAM_EV_DATA_TCONT]	= "data_tcont",
	[GPON_PLOAM_EV_REQ_KEY]		= "req_key",
	[GPON_PLOAM_EV_KEY_SENT]	= "key_sent",
	[GPON_PLOAM_EV_REQ_PW]		= "req_pw",
	[GPON_PLOAM_EV_KEY_SWITCH_ARM]	= "key_switch_arm",
	[GPON_PLOAM_EV_KEY_SWITCH]	= "key_switch",
	[GPON_PLOAM_EV_UNHANDLED]	= "UNHANDLED",
	[GPON_PLOAM_EV_SN_REPROVISIONED] = "sn_reprovisioned",
	[GPON_PLOAM_EV_O5_WATCHDOG]	= "O5_WATCHDOG",
	[GPON_PLOAM_EV_LOS_RERANGE]	= "LOS_RERANGE",
	[GPON_PLOAM_EV_KEEP_LOCK_GIVEUP] = "KEEP_LOCK_GIVEUP",
	[GPON_PLOAM_EV_O3_UNHEARD]	= "O3_UNHEARD",
	[GPON_PLOAM_EV_O5_UNGRANTED]	= "O5_UNGRANTED",
};

/* The events that DECIDE activation, and therefore speak whatever `trace` says.
 * Everything else is per-message chatter. */
static bool luna_ev_is_decisive(enum gpon_ploam_ev ev)
{
	switch (ev) {
	case GPON_PLOAM_EV_STATE:
	case GPON_PLOAM_EV_DEACT:
	case GPON_PLOAM_EV_DEACT_KEEP_LOCK:
	case GPON_PLOAM_EV_KEEP_LOCK_GIVEUP:
	case GPON_PLOAM_EV_O3_UNHEARD:
	case GPON_PLOAM_EV_DISABLE_SN:
	case GPON_PLOAM_EV_RANGING_TIME:
	case GPON_PLOAM_EV_ONU_ID:
	case GPON_PLOAM_EV_O5_WATCHDOG:
	case GPON_PLOAM_EV_O5_UNGRANTED:
	case GPON_PLOAM_EV_LOS_RERANGE:
	case GPON_PLOAM_EV_SN_REPROVISIONED:
	case GPON_PLOAM_EV_UNHANDLED:
	/* a few per registration, and the identity an OLT may refuse us on */
	case GPON_PLOAM_EV_REQ_PW:
	case GPON_PLOAM_EV_REQ_KEY:
		return true;
	default:
		return false;
	}
}

/* The instrument must be REMOVABLE, because it is a SUSPECT. ...
 * dev/MEASURED-luna_gpon.c.md sec 240. */
#ifdef CONFIG_GPON_PLOAM_DIAG
static bool core_trace = true;
module_param(core_trace, bool, 0644);
MODULE_PARM_DESC(core_trace, "1=the core FSM's events are printed (default; needed to diagnose it). 0=silent, for measuring whether the printing itself perturbs activation");
#else
static const bool core_trace;
#endif

/* FAMILY half of the core's gpon_ploam_diag: read THIS ...
 * dev/MEASURED-luna_gpon.c.md sec 241. */
static unsigned long luna_poll_prev_jiffies;
static u32 luna_poll_gap_ms;	/* start of this poll - start of the previous, ms */
static u8 luna_rx_burst_idx;	/* DS PLOAMs this poll dequeued before the current one */

/* FAMILY half of the core's gpon_bwcap_diag: the GTC's ...
 * dev/MEASURED-luna_gpon.c.md sec 242. */
static struct gpon_bwcap_diag luna_bwcap;
static bool luna_bwcap_armed;

/* The longest window the 8-bit CAP_FRAME_NUM allows: 255 ...
 * dev/MEASURED-luna_gpon.c.md sec 243. */
#define LUNA_BWCAP_FRAMES	0xffu
#define LUNA_BWCAP_RAW_N	6	/* raw entries shown per point */

static u32 luna_bwcap_raw[2 * LUNA_BWCAP_RAW_N];	/* the first VALID entries since the last point */
static unsigned int luna_bwcap_raw_n;

static void luna_bwcap_arm(void)
{
	gpon_wr(GPON_BWMAP_CTRL, 0);
	gpon_wr(GPON_BWMAP_CTRL, GPON_BWMAP_CAP_CLR | LUNA_BWCAP_FRAMES);
	gpon_wr(GPON_BWMAP_CTRL, GPON_BWMAP_CAP_EN | LUNA_BWCAP_FRAMES);
}

/* DEBUG (CONFIG_GPON_PLOAM_DIAG): the first N ticks of every O5, one line per
 * tick -- what the upstream did with the grants the GTC accepted. Built for an
 * OLT we cannot see: "grants accepted, nothing sent" (2026-09-30, production
 * OLT). gpon_rd only: pi_rd hangs this context. */
static uint o5_window_ticks;
module_param(o5_window_ticks, uint, 0644);
MODULE_PARM_DESC(o5_window_ticks, "DEBUG: log the upstream every FSM tick for the first N ticks of each O5 (0 = off)");

static void luna_bwcap_arm(void);

static void luna_o5_window_sample(void)
{
	unsigned int i, n = 0, pl = 0;
	u32 first0 = 0, first1 = 0;

	for (i = 0; i < GPON_BWMAP_ENTRIES; i++) {
		u32 w0 = gpon_rd(BWMAP_DATA(2 * i));

		if (!(w0 & GPON_BWMAP_ENT_VALID))
			continue;
		if (!n++) {
			first0 = w0;
			first1 = gpon_rd(BWMAP_DATA(2 * i + 1));
		}
		if (w0 & GPON_BWMAP_ENT_PLOAMU)
			pl++;
	}
	luna_bwcap_arm();
	luna_log("o5win +%u ticks grants=%u ploamu=%u first=%08x/%08x | us_intr dlt=%x sts=%x | ploam_ind=%03x cpu_tx=%08x auto_tx=%08x | gem_byte=%u dbru=%u idle16=%u | eqd=%08x boh=%03x sstart=%u\n",
		gpon_fsm_ticks - gpon_o5_entry_tick, n, pl,
		first0, first1,
		gpon_rd(GPON_GTC_US_INTR_DLT), gpon_rd(GPON_GTC_US_INTR_STS),
		gpon_rd(GPON_GTC_US_PLOAM_IND) & 0x7ffu,
		gpon_us_misc_cnt(2), gpon_us_misc_cnt(3),
		gpon_us_misc_cnt(4), gpon_us_misc_cnt(1), gpon_rd(TCONT_IDLE_STAT(16)),
		gpon_rd(GPON_GTC_US_EQD), gpon_rd(GPON_GTC_US_BOH_CFG) & 0xfffu,
		gpon_rd(GPON_GTC_US_PROC_MODE) & 1u);
}

/* Empty the capture into the accumulator.  Does not re-arm. */
static void luna_bwcap_harvest(void)
{
	unsigned int i, found = 0;

	if (gpon_rd(GPON_BWMAP_STS) & GPON_BWMAP_CAP_OVERFL)
		luna_bwcap.overfl++;
	for (i = 0; i < GPON_BWMAP_ENTRIES; i++) {
		u32 w0 = gpon_rd(BWMAP_DATA(2 * i));
		u32 w1 = gpon_rd(BWMAP_DATA(2 * i + 1));
		u8 tc = w0 & GPON_BWMAP_ENT_TCONT_MASK;

		if (!(w0 & GPON_BWMAP_ENT_VALID))
			continue;
		found++;
		if (luna_bwcap_raw_n < LUNA_BWCAP_RAW_N) {
			luna_bwcap_raw[2 * luna_bwcap_raw_n] = w0;
			luna_bwcap_raw[2 * luna_bwcap_raw_n + 1] = w1;
			luna_bwcap_raw_n++;
		}
		luna_bwcap.entries++;
		luna_bwcap.tconts |= BIT(tc);
		if (!(w0 & GPON_BWMAP_ENT_PLOAMU))
			continue;
		luna_bwcap.ploamu++;
		if (tc == GPON_OMCC_TCONT) {
			luna_bwcap.omcc_ploamu++;
			luna_bwcap.last_raw0 = w0;
			luna_bwcap.last_raw1 = w1;
		}
	}
	luna_bwcap.harvests++;
	if (found)
		luna_bwcap.nonempty++;
	luna_bwcap.valid = GPON_BWCAP_HAS;
}

/* Called once per FSM poll, before the DS-PLOAM drain. */
static void luna_bwcap_poll(void)
{
	if (!IS_ENABLED(CONFIG_GPON_PLOAM_DIAG))
		return;
	if (gpon_fsm_state >= 5) {
		luna_bwcap_armed = false;	/* re-armed (with CLR) on the next activation */
		return;
	}
	if (luna_bwcap_armed)
		luna_bwcap_harvest();
	luna_bwcap_arm();
	luna_bwcap_armed = true;
}

static void luna_ploam_diag(enum gpon_ploam_diag_point p)
{
	struct gpon_ploam_diag d;
	char line[256];
	u32 cpu, auto_;

	if (!IS_ENABLED(CONFIG_GPON_PLOAM_DIAG))
		return;
	d.valid      = GPON_PDIAG_HAS_ALL;
	d.ploam_acpt = gpon_rd(GPON_GTC_DS_MISC_CNTR_PLOAM_ACPT);
	d.bwm_acpt   = gpon_bwm_acpt_since(&gpon_bwm_acpt_diag_last);
	d.bwm_fail   = gpon_rd(GPON_GTC_DS_MISC_CNTR_BWM_FAIL);
	d.bwm_inv    = gpon_rd(GPON_GTC_DS_MISC_CNTR_BWM_INV);
	/* US-GTC misc PM: idx 2 = PLOAM_CPU_TX, idx 3 = ...
	 * dev/MEASURED-luna_gpon.c.md sec 325. */
	cpu   = gpon_us_misc_cnt(2);
	auto_ = gpon_us_misc_cnt(3);
	d.us_ploam_tx = (cpu & 0xffff) + (cpu >> 16) + (auto_ & 0xffff) + (auto_ >> 16);
	d.us_sn_tx    = auto_ >> 16;
	d.us_onu_id  = (gpon_rd(GPON_GTC_US_ONU_ID) >> GPON_GTC_US_ONU_ID_SHIFT) & GPON_ONU_ID_MASK;
	d.ds_onu_id  = (gpon_rd(GPON_GTC_DS_ONU_ID_STATUS) >> GPON_ONU_ID_SHIFT) & GPON_ONU_ID_MASK;
	d.poll_gap_ms = luna_poll_gap_ms;
	d.rx_burst_idx = luna_rx_burst_idx;
	{
		u32 act = gpon_rd(GPON_GTC_DS_MISC_CNTR_ACTIVE);

		d.sn_req  = GPON_GTC_DS_ACTIVE_SN_REQ(act);
		d.rng_req = GPON_GTC_DS_ACTIVE_RNG_REQ(act);
	}
	gpon_ploam_diag_format(line, sizeof(line), p,
			       luna_now_ms(), gpon_fsm_state, &d);
	luna_log("%s\n", line);
	/* The accepted-grant capture since the previous point: empty the engine into
	 * the accumulator, report, then start the next window. */
	if (luna_bwcap_armed) {
		luna_bwcap_harvest();
		luna_bwcap_arm();
	}
	gpon_bwcap_diag_format(line, sizeof(line), p,
			       luna_now_ms(), gpon_fsm_state,
			       luna_bwcap_armed ? &luna_bwcap : NULL);
	luna_log("%s\n", line);
	if (luna_bwcap_raw_n) {
		/* The first entries since the last point, raw and family-specific, so a
		 * human can tell twelve real grants from one entry read twelve times. */
		unsigned int i;
		int pos = 0;

		for (i = 0; i < luna_bwcap_raw_n && pos < (int)sizeof(line) - 20; i++)
			pos += scnprintf(line + pos, sizeof(line) - pos, " %08x/%08x",
					 luna_bwcap_raw[2 * i], luna_bwcap_raw[2 * i + 1]);
		luna_log("bwcap-raw %s first%u=%s\n",
			gpon_ploam_diag_point_name(p), luna_bwcap_raw_n, line);
	}
	memset(&luna_bwcap, 0, sizeof(luna_bwcap));
	luna_bwcap_raw_n = 0;
}

static void luna_op_trace(void *sh, enum gpon_ploam_ev ev, u32 a, u32 b)
{
	const char *nm = NULL;
	enum gpon_ploam_diag_point p = gpon_ploam_diag_point_of(ev);

	(void)sh;
	if (p != GPON_PDIAG_NONE)	/* the core FSM path: the WHEN is the core's event */
		luna_ploam_diag(p);
	if (!core_trace)
		return;
	if (!luna_ev_is_decisive(ev) && !trace)
		return;
	if ((unsigned int)ev < ARRAY_SIZE(luna_ev_name))
		nm = luna_ev_name[ev];
	if (nm)
		luna_log("core %s a=%u b=%u\n", nm, a, b);
	else
		luna_log("core ev%u a=%u b=%u (no name in this shell -- the core gained an event)\n",
			(unsigned int)ev, a, b);
}

/* What the PLOAM core does not handle: the rate-limited report, and the whole
 * message kept per type so it can be read and replayed. */
static void luna_op_unsupported(void *sh, const char *kind, int cls, u32 val,
				const char *want, const u8 *data, unsigned int len)
{
#ifdef CONFIG_GPON_OLT_DIAG
	struct gpon_olt_rec r = { 0 };
	unsigned long flags;
#endif

	gpon_unsup_report(kind, cls, val, want, data, len);
#ifdef CONFIG_GPON_OLT_DIAG
	if (!data || !len)
		return;
	r.t_us = div_u64(luna_ds_rx_ns ? luna_ds_rx_ns : ktime_get_boottime_ns(), 1000);
	r.ostate = (u8)luna_ploam.state;
	r.dir = GPON_OLT_DS;
	r.len = len > GPON_OLT_PDU ? GPON_OLT_PDU : (u8)len;
	memcpy(r.pdu, data, r.len);
	spin_lock_irqsave(&luna_ploam_cap_lock, flags);
	r.seq = luna_ploam_cap.seq;
	gpon_olt_ploam_unknown_note(&luna_ploam_unknown, &r);
	spin_unlock_irqrestore(&luna_ploam_cap_lock, flags);
#endif
}

static const struct gpon_ploam_ops luna_ploam_ops __maybe_unused = {
	.ploam_tx	= luna_op_ploam_tx,
	.boh_write	= luna_op_boh_write,
	.get_min_delay	= luna_op_get_min_delay,
	.set_eqd	= luna_op_set_eqd,
	.us_ploam_flush	= luna_op_us_ploam_flush,
	.set_hw_state	= luna_op_set_hw_state,
	.set_hw_onu_id	= luna_op_set_hw_onu_id,
	.on_below_o5	= luna_op_on_below_o5,
	.o5_rearm_burst	= luna_op_o5_rearm_burst,
	.install_data_gem = luna_op_install_data_gem,
	.cdr_reseat	= luna_op_cdr_reseat,
	.aes_arm_switch	= luna_op_aes_arm_switch,
	.rng		= luna_op_rng,
	.omci_report_oper_up = luna_op_omci_report_oper_up,
	.ds_bip_errors = luna_op_ds_bip_errors,
	.analog_relock	= luna_op_analog_relock,
	.o3_feed_reset	= luna_op_o3_feed_reset,
	.aes_stage_key	= luna_op_aes_stage_key,
	.install_omcc	= luna_op_install_omcc,
	.install_tcont	= luna_op_install_tcont,
	.data_alloc_changed = luna_data_alloc_changed,
	.arm_dying_gasp	= luna_op_arm_dying_gasp,
	.trace		= luna_op_trace,
	.unsupported	= luna_op_unsupported,
};

static void gpon_fsm_handle(const u8 *m)
{
	u8 onu_id = m[0], type = m[1];
	const u8 *d = &m[2];		/* 10 data octets */

	/* Surface any DS PLOAM that is not the repetitive broadcast acquisition
	 * traffic, so activation progress is visible. */
	gpon_last_ds_type = type;
	if (trace && !gpon_ploam_ds_repetitive(type))
		luna_log("DS PLOAM onu_id=0x%02x type=0x%02x d=%*phN\n",
				    onu_id, type, 8, d);

	/* Say which FSM is dispatching, once, so a boot log is never ...
	 * dev/MEASURED-luna_gpon.c.md sec 244. */
	{
		static bool said;

		if (!said) {
			said = true;
			luna_log("PLOAM dispatch = COMMON core gpon_ploam.c\n");
		}
	}

	if (!READ_ONCE(luna_activation_ready))
		return;

	/* Ticks x 10, NOT the wall clock, and the more accurate clock ...
	 * dev/MEASURED-luna_gpon.c.md sec 326. */
	if (!gpon_ploam_ds_repetitive(type))
		luna_ploam_capture(GPON_OLT_DS, m, GPON_PLOAM_DS_LEN);
	gpon_ploam_ds(&luna_ploam, m, GPON_PLOAM_DS_LEN,
		      luna_now_ms());
}

/* Has the WAN netdev received NOTHING? Both watchdogs below ...
 * dev/MEASURED-luna_gpon.c.md sec 245. */
static bool gpon_wan_rx_silent(void)
{
	u32 n = rtl9602c_eth_wan_rx_count();

	return n != GPON_OMCI_RX_UNAVAIL && n == 0;
}

/* Apply identity cleanup while US is inhibited, before any rearm. The timer
 * also consumes early parameter changes through this same existing logic. */
static void gpon_apply_identity_change(void)
{
	gpon_ploam_sn_changed(&luna_ploam, luna_now_ms());
}

static bool luna_vlan_filter_ok;

/* The Ethernet driver's VLAN table carries the CPU's extension ports and a PON VLAN of its
 * own: the LAN then stays open WITH the filter, and the filter keeps WAN broadcasts off it. */
void luna_gpon_vlan_filter_ok(bool ok)
{
	WRITE_ONCE(luna_vlan_filter_ok, ok);
}
EXPORT_SYMBOL(luna_gpon_vlan_filter_ok);

/* Ahead of the activation gate: a board whose activation never becomes ready kept
 * VLAN_FILTER on and its LAN dark (X100DG, 2026-10-01). */
static void luna_lan_keep_open_poll(void)
{
	if (!lan_keep_open)
		return;
	sw_field(SW_VLAN_CTRL, 0, 0, READ_ONCE(luna_vlan_filter_ok));	/* LAN open, every state */
	if (!gpon_vlan_lan_open) {
		gpon_vlan_lan_open = true;
		luna_log("lan_keep_open -> LAN access open (VLAN_FILTER %d)\n",
			 READ_ONCE(luna_vlan_filter_ok));
	}
}

static bool ploam_irq = true;
module_param(ploam_irq, bool, 0644);
MODULE_PARM_DESC(ploam_irq, "answer a downstream PLOAM from its interrupt (default 1); 0 leaves it to the FSM tick, an A/B of the answer time");

static atomic64_t luna_ds_arrival_ns;	/* the oldest unread PLOAM's interrupt, 0 = none */
static u64 luna_ds_carry_ns;		/* the arrival of PLOAMs a full drain left queued */
#define GPON_FRAME_NS	125000u		/* one PLOAMd per downstream frame: queued PLOAMs are this far apart */
static bool luna_ds_waited;		/* the tick already gave a silent PLOAM one period */
/* Far above the drains one tick needs, far below a level event looping through
 * the drain (V2801RGW: 165k/s). */
#define LUNA_GPON_IRQ_TICK_BUDGET	64u
static unsigned int luna_ds_irq_burst;	/* drains the line came back from since the last tick */
static bool luna_gpon_irq_parked;	/* the line is off until the next tick */

/* Hand every queued downstream PLOAM to the core, timed from its interrupt; the
 * k-th of one drain landed no earlier than k frames after it. The tick gives an
 * unannounced PLOAM one period for its interrupt, then counts it unsignalled and
 * times it from this read. Under luna_fsm_lock. */
static void luna_ds_ploam_drain(bool from_irq)
{
	u64 arrival;
	int guard = 0;

	if (gpon_rd(GPON_GTC_DS_PLOAM_IND) & GPON_DS_PLM_BUF_EMPTY) {
		if (from_irq)
			atomic64_set(&luna_ds_arrival_ns, 0);	/* another GTC event's */
		luna_ds_waited = false;
		return;
	}
	arrival = luna_ds_carry_ns ?: (u64)atomic64_xchg(&luna_ds_arrival_ns, 0);
	if (!arrival && luna_gpon_irq) {
		if (from_irq)
			return;		/* its own interrupt is latched behind this one */
		if (!luna_ds_waited && !READ_ONCE(luna_gpon_irq_parked)) {	/* off: none will come */
			luna_ds_waited = true;
			return;
		}
	}
	if (!arrival) {
		luna_ds_unsignalled++;
		arrival = ktime_get_boottime_ns();
	}
	luna_ds_carry_ns = 0;
	luna_ds_waited = false;
	while (!(gpon_rd(GPON_GTC_DS_PLOAM_IND) & GPON_DS_PLM_BUF_EMPTY)) {
		u8 m[13];

		luna_ds_rx_ns = arrival + (u64)guard * GPON_FRAME_NS;
		if (guard++ == 16) {
			luna_ds_carry_ns = luna_ds_rx_ns;
			break;
		}
		/* The word-unpack is the core's (gpon_gtc_ploam.h, x86-proven); this shell
		 * contributes the accessor and the per-SoC offset. The DEQ below advances
		 * the queue even on a refusal: the queue discipline is this loop's. */
		luna_rx_burst_idx = (u8)(guard - 1);	/* 0 = the first this drain */
		if (gpon_gtc_ds_ploam_read(&gpon_io,
					   reg_make(GPON_GTC_DS_PLOAM_MSG), m))
			gpon_fsm_handle(m);
		gpon_ds_rx++;					/* DS-lock liveness */
		gpon_wr(GPON_GTC_DS_PLOAM_IND, GPON_DS_PLM_DEQ);	/* advance */
	}
	luna_ds_rx_ns = 0;
}

/* The line comes back after its drain; with nobody to drain, or past the tick's
 * budget, it stays off until the tick. */
static void luna_ds_ploam_work_fn(struct work_struct *work)
{
	bool live;

	spin_lock(&luna_fsm_lock);
	live = READ_ONCE(ploam_irq) && timer_pending(&gpon_fsm_timer);
	if (live)
		luna_ds_ploam_drain(true);
	if (live && ++luna_ds_irq_burst <= LUNA_GPON_IRQ_TICK_BUDGET) {
		enable_irq(luna_gpon_irq);
	} else {
		WRITE_ONCE(luna_gpon_irq_parked, true);
		luna_ds_parks++;
	}
	spin_unlock(&luna_fsm_lock);
}
static DECLARE_WORK(luna_ds_ploam_work, luna_ds_ploam_work_fn);

/* The drain runs in BH context, on this interrupt's exit: a PREEMPT_NONE kernel
 * left an interrupt thread 16 ms behind a kernel path (X100DG). The latch is
 * cleared before the event, so a PLOAM landing meanwhile raises it again; the
 * line is off until the drain, as a queued PLOAM keeps its event raised. */
static irqreturn_t luna_gpon_irq_handler(int irq, void *data)
{
	u32 events;

	if (!(gpon_rd(GPON_GTC_DS_PLOAM_IND) & GPON_DS_PLM_BUF_EMPTY))
		atomic64_cmpxchg(&luna_ds_arrival_ns, 0, ktime_get_boottime_ns());
	luna_ds_irqs++;
	sw_wr(SW_INTR_IMS, SW_INTR_GPON);
	events = gpon_rd(GPON_GTC_DS_INTR_DLT);		/* read-clear */
	if (!events)
		return IRQ_NONE;
	disable_irq_nosync(irq);
	queue_work(system_bh_highpri_wq, &luna_ds_ploam_work);
	return IRQ_HANDLED;
}

/* At the end of every tick, under luna_fsm_lock: a new budget, a parked line back on. */
static void luna_gpon_irq_unpark(void)
{
	luna_ds_irq_burst = 0;
	if (READ_ONCE(luna_gpon_irq_parked)) {
		WRITE_ONCE(luna_gpon_irq_parked, false);
		enable_irq(luna_gpon_irq);
	}
}

static void luna_gpon_irq_request(void)
{
	int ret;

	if (!luna_gpon_irq) {
		pr_info("luna-gpon: no GPON interrupt in the device tree: downstream PLOAM waits for the FSM tick\n");
		return;
	}
	ret = request_irq(luna_gpon_irq, luna_gpon_irq_handler, 0, "luna-gpon", NULL);
	if (ret) {
		pr_warn("luna-gpon: GPON interrupt %d refused (%d): downstream PLOAM waits for the FSM tick\n",
			luna_gpon_irq, ret);
		luna_gpon_irq = 0;
		return;
	}
	gpon_wr(GPON_INTR_MASK, GPON_INTR_GTC_DS);
	sw_wr(SW_INTR_IMS, SW_INTR_GPON);
	sw_field(SW_INTR_IMR, 10, 10, 1);
}

static void gpon_fsm_poll(struct timer_list *t)
{
	spin_lock(&luna_fsm_lock);
	gpon_fsm_ticks++;
	if (!luna_fsm_first_ms)
		luna_fsm_first_ms = luna_now_ms();
	spin_lock(&luna_pwd_lock);
	if (luna_pwd_dirty) {
		gpon_ploam_set_password(&luna_ploam, luna_pwd);
		luna_pwd_dirty = false;
	}
	spin_unlock(&luna_pwd_lock);
	{
		unsigned long now = jiffies;

		luna_poll_gap_ms = luna_poll_prev_jiffies ?
			jiffies_to_msecs(now - luna_poll_prev_jiffies) : 0;
		luna_poll_prev_jiffies = now;
	}
	luna_bwcap_poll();
	luna_lan_keep_open_poll();
	if (!READ_ONCE(luna_activation_ready))
		goto drain_downstream;
	/* The PERIODIC half of the FSM is the core's, as the ...
	 * dev/MEASURED-luna_gpon.c.md sec 246. */
	gpon_ploam_tick(&luna_ploam);
	WRITE_ONCE(luna_health_los,
		   !!(gpon_rd(GPON_GTC_DS_LOS_CFG_STS) & GPON_OPTIC_LOS_SIG));
	gpon_led_los_set(READ_ONCE(luna_health_los));
	/* The serial was (re)provisioned after ranging began. Drop to O1 and re-offer
	 * the new Serial_Number. */
	gpon_apply_identity_change();
drain_downstream:
	luna_ds_ploam_drain(false);
	if (!READ_ONCE(luna_activation_ready))
		goto next_poll;
	/* Both provisioning follow-ups are one core poll: the WAN ...
	 * dev/MEASURED-luna_gpon.c.md sec 247. */
	gpon_ploam_poll_provision(&luna_ploam, luna_now_ms());
	luna_omci_service();

	/* feed_rekick: per-tick self-terminating US-feed FIFO re-arm. The one-shot
	 * O5-entry re-arm is re-parked by later SerDes resets before the OLT's first
	 * grant, so pages stage in PON-IP SRAM but never build a DRAM descriptor and
	 * the framer emits nothing. Auto-stops once gemus_omcc advances. */
	if (feed_rekick && gpon_fsm_state == 5 && luna_ploam.omcc_installed) {
		u32 dsc = pi_rd(PI_PON_DSC_STS_US);

		if ((dsc & 0x1fffu) > 0 && ((dsc >> 16) & 0x1fffu) == 0 &&
		    gpon_rd(GEM_US_STAT(GPON_OMCC_FLOW)) == 0)
			gpon_us_feed_rearm_light();
	}

	/* us_intr_svc: ack the upstream GPON interrupt deltas the known-good unit
	 * services on every GTC_US event. The reads clear the sticky latch; if the
	 * fetch FSM was back-pressuring on it, the payload framer unstalls. */
	if (us_intr_svc && gpon_fsm_state == 5 && luna_ploam.omcc_installed) {
		u32 gtcus_dlt = gpon_rd(GPON_GTC_US_INTR_DLT);	/* read-to-clear GTC_US delta */
		u32 gemus_dlt = gpon_rd(GPON_GEM_US_INTR_DLT);	/* read-to-clear GEM_US delta */

		(void)gpon_rd(GPON_GTC_US_INTR_STS);			/* GTC_US_INTR_STS */
		(void)gpon_rd(GPON_GEM_US_INTR_STS);			/* GEM_US_INTR_STS */
		if (gtcus_dlt || gemus_dlt)
			gpon_us_intr_svc_cnt++;
	}

	/* O5 provisioning watchdog. A boot that reached O5 locally ...
	 * dev/MEASURED-luna_gpon.c.md sec 248. */
	gpon_ploam_poll_watchdog(&luna_ploam, gpon_wan_rx_silent(),
				 luna_now_ms());
	if (!(luna_ploam.ticks % 100))
		gpon_ploam_poll_grants(&luna_ploam, gpon_bwm_acpt_since(&gpon_bwm_acpt_watch_last),
				       luna_now_ms());

	/* Autonomous downstream-LOS recovery (fiber pull). The OLT ...
	 * dev/MEASURED-luna_gpon.c.md sec 327. */
	if (los_rerange_ticks && gpon_fsm_state >= 2) {
		bool optic_los = !!(gpon_rd(GPON_GTC_DS_LOS_CFG_STS) & GPON_OPTIC_LOS_SIG);
		/* A chip with no declared SDS_FIB_STATUS has no second witness, and an
		 * absent witness may not be manufactured into one. */
		bool sds_dark  = SDS_FIB_STATUS &&
				 !(sw_rd(SDS_FIB_STATUS) & SDS_FIB_SDS_SDET);

		/* Real DS-light loss = the GTC optical-LOS AND the SoC SerDes ...
		 * dev/MEASURED-luna_gpon.c.md sec 249. */
		gpon_ploam_poll_los(&luna_ploam, optic_los, sds_dark,
				    luna_now_ms());
	}

	/* Hybrid LAN/VLAN: clear VLAN_FILTER so the LAN ports forward ...
	 * dev/MEASURED-luna_gpon.c.md sec 250. */
	if (!lan_keep_open && gpon_fsm_state == 5 && !gpon_vlan_lan_open && vlan_lan_o5_ticks &&
		   gpon_o5_entry_tick && (gpon_fsm_ticks - gpon_o5_entry_tick) > vlan_lan_o5_ticks) {
		/* legacy O5-gated: keep filtering through ranging and config-apply, then
		 * clear once O5 has held. Re-armed on any drop below O5. */
		sw_field(SW_VLAN_CTRL, 0, 0, 0);		/* VLAN_FILTER off -> open LAN */
		gpon_vlan_lan_open = true;
		luna_log("O5 stable %u ticks -> VLAN_FILTER off (LAN access open)\n",
			gpon_fsm_ticks - gpon_o5_entry_tick);
	}
	if (IS_ENABLED(CONFIG_GPON_PLOAM_DIAG) && o5_window_ticks &&
	    gpon_fsm_state == 5 && gpon_o5_entry_tick &&
	    gpon_fsm_ticks - gpon_o5_entry_tick < o5_window_ticks)
		luna_o5_window_sample();
	if (trace && gpon_fsm_state == 5 && (gpon_fsm_ticks % 150) == 0)
		/* rxsid/ustx/dirty were REMOVED from this fast O5 print: pi_rd in this
		 * context reproducibly HANGS the poll right after OMCC install (two boots
		 * identical). It is an indirect polled PON-IP access, unsafe here unlike
		 * sw_rd/gpon_rd; read those counters through /proc instead. */
		luna_log("O5 t=%u last=0x%02x onu=%u hwst=%u eqd=0x%08x | dsrx_omcc=%u pirx=%u omcirx=%d | ploam_cpu=%u gem_byte=%u gemus_omcc=%u idle16=%u idle8=%u\n",
			gpon_fsm_ticks, gpon_last_ds_type, luna_ploam.onu_id,
			gpon_rd(GPON_GTC_DS_ONU_ID_STATUS) & 0xf, gpon_rd(GPON_GTC_US_EQD),
			gpon_gem_ds_rx_cnt(GPON_OMCC_FLOW), sw_rd(OMCI_RX_PKT_CNT),
			gpon_omci_rx_cnt(),
			gpon_us_misc_cnt(2), gpon_us_misc_cnt(4),
			gpon_rd(GEM_US_STAT(GPON_OMCC_FLOW)), gpon_rd(TCONT_IDLE_STAT(16)),
			gpon_rd(TCONT_IDLE_STAT(GPON_DATA_TCONT)));
	/* US-OMCI egress stall localizer, SAFE reads only (no pi_rd, ...
	 * dev/MEASURED-luna_gpon.c.md sec 251. */
	if (trace && gpon_fsm_state == 5 && (gpon_fsm_ticks % 150) == 0) {
		int j;
		/* OLT-independent US-OMCI datapath self-test: inject synthetic OMCI frames
		 * through OUR TX path, so the rxsid read below reflects whether our
		 * steering reaches the US-NIC with no dependency on the OLT. */
		for (j = 0; j < 1; j++)	/* de-burst: ONE inject/tick. The 4-burst in one softirq tick overran the GMAC TX fetch engine — it drained ~3, parked at the producer head, the ring filled, and (no TDU re-kick) stayed parked, freezing ALL GMAC TX. */
			rtl9602c_eth_omci_selftest();
		/* rxsid = RX_SID_GOOD_CNT_US[0..4]; group [4] is the OMCC SID. Non-zero
		 * means the US OMCI frame DOES reach the US-NIC classifier, so the stall
		 * is downstream at the queue/scheduler; zero with ustx=0 means it never
		 * reaches the US-NIC at all. pi_rd is safe in this context. */
		luna_log("USDIAG t=%u ustx=%u pirx=%u usdrop=%u uscrc=%u | rxsid=%u/%u/%u/%u/%u\n",
			gpon_fsm_ticks, sw_rd(OMCI_TX_PKT_CNT), sw_rd(OMCI_RX_PKT_CNT),
			sw_rd(OMCI_DROP_PKT_CNT), sw_rd(OMCI_CRC_ERROR_PKT_CNT),
			(u32)pi_rd(PI_RX_SID_GOOD_CNT_US),
			(u32)pi_rd(PI_RX_SID_GOOD_CNT_US + 1 * PI_SID_CNT_GROUP_STRIDE),
			(u32)pi_rd(PI_RX_SID_GOOD_CNT_US + 2 * PI_SID_CNT_GROUP_STRIDE),
			(u32)pi_rd(PI_RX_SID_GOOD_CNT_US + 3 * PI_SID_CNT_GROUP_STRIDE),
			(u32)pi_rd(PI_RX_SID_GOOD_CNT_US + 4 * PI_SID_CNT_GROUP_STRIDE));
	}
	/* DS-pipeline stage probe: A=de-encap, B=PBO high-queue, C=DS SRAM,
	 * D=PON-IP->NIC. The first zero along A->D is the stall stage. */
	if (gem_gate_open && gpon_fsm_state == 5 && (gpon_fsm_ticks % 100) == 0) {
		/* De-encap count per flow: localizes whether OMCI de-encaps ANYWHERE (a
		 * mapping issue) or nowhere (the OLT is not sending OMCI). */
		pr_emerg("DSPIPE deenc f64=%u f3=%u f2=%u f1=%u f0=%u | nonidle=%u idle=%u los=%u hec=%u | sram=%u q0=%u rxok=%u\n",
			 gpon_gem_flow_cnt(GPON_OMCC_FLOW, 0), gpon_gem_flow_cnt(3, 0),
			 gpon_gem_flow_cnt(2, 0), gpon_gem_flow_cnt(1, 0),
			 gpon_gem_flow_cnt(0, 0),
			 gpon_rd(GPON_GTC_DS_MISC_CNTR_GEM_NON_IDLE), gpon_rd(GPON_GTC_DS_MISC_CNTR_GEM_IDLE), gpon_rd(GPON_GTC_DS_MISC_CNTR_GEM_LOS),
			 gpon_rd(GPON_GTC_DS_MISC_CNTR_HEC_CORRECT),
			 pi_rd(PI_PON_DSC_USAGE_DS) & 0x1fff,
			 pi_rd(PI_PON_DS_PBO_PAGE_Q0) & 0x1fff,
			 pi_rd(PI_PKT_OK_CNT_DS) & 0xffff);
	}
	/* Periodic SerDes-TX re-sync while UN-RANGED. The upstream-burst serializer
	 * lock is non-deterministic, so re-pulse the TX-interface reset-B
	 * (WSDS_DIG_1D[16]) ~every 2 s to keep re-attempting a lock onto the framer
	 * burst data. TX-interface only, so the locked RX framer is undisturbed. */
	if (unranged_reseat && gpon_fsm_state >= 3 && luna_ploam.onu_id == 0xff &&
	    (gpon_fsm_ticks % 200) == 0) {
		gpon_cdr_reseat();
		gpon_sds_synced++;
	}
	/* While unregistered in O3, re-offer our Serial_Number_ONU ~twice a second
	 * (the OLT grants SN windows intermittently). */
	gpon_ploam_poll_sn_reoffer(&luna_ploam, luna_now_ms());
	/* Periodic O5 upstream-PLOAM keepalive. Once ranged the FSM ...
	 * dev/MEASURED-luna_gpon.c.md sec 252. */
	gpon_ploam_poll_keepalive(&luna_ploam, luna_now_ms());
	gpon_ploam_poll_rei(&luna_ploam, luna_now_ms());

	/* Runtime DS-CDR-wedge recovery -- the stock link-state-check ...
	 * dev/MEASURED-luna_gpon.c.md sec 253. */
	if (cdr_stuck_recover) {
		static int cdr_pending;
		u32 sts = gpon_rd(GPON_GTC_DS_INTR_STS);

		gpon_gtc_ds_sts_last = sts;
		if (cdr_pending) {
			sw_field(SDS_REG0, 1, 1, 1);		/* SP_SDS_EN_RX -> 1 */
			cdr_pending = 0;
			if (gpon_rd(GPON_GTC_DS_INTR_STS) != GTC_DS_CDR_STUCK)
				gpon_cdr_stuck_fixed++;
		} else if (sts == GTC_DS_CDR_STUCK) {
			if (gpon_cdr_stuck_tries < GPON_CDR_STUCK_MAX ||
			    (gpon_fsm_ticks % GPON_CDR_STUCK_SLOW_TICKS) == 0) {
				sw_field(SDS_REG0, 1, 1, 0);	/* SP_SDS_EN_RX -> 0 */
				cdr_pending = 1;
				gpon_cdr_stuck_tries++;
				gpon_cdr_stuck_count++;
				pr_warn_ratelimited("luna-gpon: DS CDR wedged (GTC_DS_STS=0x%08x); SP_SDS_EN_RX re-acquire #%u\n",
						    sts, gpon_cdr_stuck_count);
			}
		} else {
			gpon_cdr_stuck_tries = 0;	/* healthy -> fresh fast budget */
		}
	}

	/* Periodic DS multiframe/BWmap ESD-recover (stock ...
	 * dev/MEASURED-luna_gpon.c.md sec 328. */
	if (gpon_esd_recover && gpon_fsm_state >= 3) {
		static unsigned long esd_last_j;
		static bool esd_init;
		static u32 esd_relocks;

		if (!esd_init) {
			esd_last_j = jiffies;
			esd_init = true;
		} else if (time_after(jiffies,
				      esd_last_j + msecs_to_jiffies(GPON_ESD_INTERVAL_MS))) {
			u32 sts = gpon_rd(GPON_GTC_DS_INTR_STS);

			esd_last_j = jiffies;
			if (!(sts & GTC_DS_STS_LOF)) {		/* framer byte-locked */
				u32 v = gpon_rd(GPON_GTC_DS_MISC_CNTR_LOM);
				u32 fail = ((v >> 16) & 0xffff) + (v & 0xffff);

				if (fail > GPON_ESD_THRESHOLD) {
					esd_relocks++;
					pr_warn_ratelimited("luna-gpon: ESD-recover: DS PLEND/LOM fail=%u byte-locked at O%u (BWmap mis-phased) -> RX-CDR re-lock #%u\n",
							    fail, gpon_fsm_state, esd_relocks);
					schedule_work(&gpon_cdr_reset_work);
				}
			}
		}
	}


next_poll:
	/* Re-armed under the lock (see the BH drain). The wheel fires an expiry one jiffy
	 * late, so `- 1` makes the tick the period every *_ticks constant counts in. */
	if (!READ_ONCE(luna_stopping))
		mod_timer(&gpon_fsm_timer, jiffies + GPON_FSM_TICK_JIFFIES - 1);
	if (luna_gpon_irq)
		luna_gpon_irq_unpark();
	spin_unlock(&luna_fsm_lock);
}

/* Stock's omitted LDO init step, byte-exact. SC-indirect ...
 * dev/MEASURED-luna_gpon.c.md sec 254. */
static void __init rtl9602c_sc_ldo_init(void)
{
	u32 fdca, t130 = sw_rd(THERMAL_CTRL_0);

	if (!sc_ldo_init)
		return;
	sw_wr(SC_CMD, 0x0001fdcau);		/* SC read cmd: reg 0xfdca */
	udelay(1000);
	fdca = sw_rd(SC_DATA);
	pr_info("luna-gpon: sc_ldo_init BEFORE: 0xfdca=0x%08x THERMAL(0x130)=0x%08x (stock 0x130=0x00ec0005)\n",
		fdca, t130);
	sw_wr(SC_IND_WD, fdca & ~0xcu);		/* clear DRAM-LDO bits 2,3 (stock mask ~0xC) */
	udelay(1000);
	sw_wr(SC_CMD, 0x0003fdcau);		/* SC write commit */
	sw_wr(SC_CMD, 0x0001fdcau);		/* re-issue read (stock reads back for log) */
	udelay(1000);
	(void)sw_rd(SC_DATA);
	sw_wr(THERMAL_CTRL_0, sw_rd(THERMAL_CTRL_0) | 0x00800000u);			/* THERMAL: set bit23 */
	sw_wr(THERMAL_CTRL_0, (sw_rd(THERMAL_CTRL_0) & 0xff80ffffu) | 0x006c0000u);	/* preserve low-16 */
	pr_info("luna-gpon: sc_ldo_init AFTER: 0x130=0x%08x\n", sw_rd(THERMAL_CTRL_0));
}

/* The laser's enable pins are SoC GPIOs on some boards, and ...
 * dev/MEASURED-luna_gpon.c.md sec 255. */
static int laser_tx_dis_gpio = -1;	/* from DT; -1 = this board has none */
static int laser_tx_pwr_gpio = -1;
/* Board pad routing: IO_GPIO_EN w0 w1, GPIO dir/data ABCD, dir/data EFGH. A board
 * that declares none keeps its bootloader's pads (the X100DG's I2C bus 1, 2026-10-01). */
static u32 board_gpio_pads[6];
static bool board_gpio_pads_set;
/* from DT: the optical module runs its own laser driver and APC, so arming
 * needs no host calibration (neither RTL8290B nor GN25L95) */
static bool optics_self_managed;

/* Cached operational state only: reading the full dump consumes PM counters. */
static int luna_health_show(struct seq_file *s, void *v)
{
	bool active;

	mutex_lock(&bosa_lock);
	active = luna_driver_ready && luna_activation_ready && !luna_stopping;
	seq_printf(s, "driver_ready: %u\nactivation_ready: %u\ninit_error: %d\n"
		   "onu_state: %u\nlos: %d\noptics_backend: %s\n",
		   luna_driver_ready && !luna_stopping, active,
		   luna_optical_init_error, READ_ONCE(gpon_fsm_state),
		   active ? READ_ONCE(luna_health_los) : -1,
		   optics_self_managed ? "self-managed" : bosa_regs_live() ? "RTL8290B" :
		   bosa_gn_identified ? "GN25L95" : "unknown");
	mutex_unlock(&bosa_lock);
	return 0;
}

static void gpon_optical_work_fn(struct work_struct *w)
{
	static unsigned long optical_next;

	mutex_lock(&bosa_lock);
	if (luna_driver_ready && !luna_stopping) {
		if (bosa_regs_live() && !laser_off && !skip_bosa) {
			if (bosa_laser_up && xchg(&luna_laser_restrobe_owed, 0) &&
			    (laser_bias || laser_mod))
				luna_laser_dac_apply();
			if (bosa_laser_up)
				bosa_laser_maint();
			/* OFFK runtime servo (stock europa_LoopMon equivalent): latch ...
			 * dev/MEASURED-luna_gpon.c.md sec 329. */
			if (apc_offk_armed && !apc_offk_latched) {
				int r;

				bosa_write_reg(BOSA_REG_W77, BOSA_W77_BIAS_MAX_EN | BOSA_W77_MOD_MAX_EN |
					       BOSA_W77_MOD_MAX_LOADIN);	/* 0xb0, as the done-check */
				r = bosa_read_reg(BOSA_REG_R29);
				if (r >= 0 && (r & 0x3c) == 0x3c) {
					bosa_set_bit(0x20e, 7, 0);
					bosa_set_bit(BOSA_W(80), 4, 0);
					apc_offk_latched = 1;
					luna_log("OFFK LATCHED: R29(0x31d)=0x%02x (modulator nulled)\n",
						r & 0xff);
				}
			}
		}
		if (optical_poll && (bosa_regs_live() || bosa_cal_ready) &&
		    time_after_eq(jiffies, optical_next)) {
			if (gpon_optical_cache_poll())
				rtl9602c_eth_omci_set_optical(anig_rx_level, anig_tx_level);
			optical_next = jiffies + msecs_to_jiffies(3000);
		}
		if (luna_selftest_owed()) {
			struct gpon_optic_reading v;

			luna_selftest_read(&v);
			luna_selftest_hand_over(&v);
		}
	}
	mutex_unlock(&bosa_lock);
	if (!READ_ONCE(luna_stopping))
		schedule_delayed_work(&gpon_optical_work, msecs_to_jiffies(50));
}

static void __init luna_laser_gpio_from_dt(void)
{
	struct device_node *np = of_find_node_by_path("/pon-optics");
	u32 v;

	if (!np)
		return;
	if (!of_property_read_u32(np, "realtek,tx-disable-gpio", &v))
		laser_tx_dis_gpio = (int)v;
	if (!of_property_read_u32(np, "realtek,tx-power-gpio", &v))
		laser_tx_pwr_gpio = (int)v;
	board_gpio_pads_set = !of_property_read_u32_array(np, "realtek,gpio-pads",
							   board_gpio_pads, 6);
	optics_self_managed = of_property_read_bool(np, "realtek,optics-self-managed");
	if (!of_property_read_u32(np, "realtek,i2c-bus", &v)) {
		if (v <= 1)
			bosa_i2c_bus = v;
		else
			pr_err("luna-gpon: /pon-optics realtek,i2c-bus=%u: this die has buses 0 and 1 -- keeping 0\n", v);
	}
	of_node_put(np);
	pr_info("luna-gpon: /pon-optics: tx-disable-gpio=%d tx-power-gpio=%d i2c-bus=%u gpio-pads=%s optics=%s\n",
		laser_tx_dis_gpio, laser_tx_pwr_gpio, bosa_i2c_bus,
		board_gpio_pads_set ? "declared" : "none (bootloader's kept)",
		optics_self_managed ? "self-managed" : "host-calibrated");
}

/* Set level before enabling output to avoid a TX_DISABLE glitch. */
static int luna_gpio_drive(int pin, bool high, const char *what)
{
	void __iomem *gpio;
	u32 dir_off, dat_off, bit, v;
	int ret = 0;

	if (pin < 0)
		return 0; /* Boards without this GPIO keep their existing mechanism. */
	if (pin >= 64 || !swc->io_gpio_en)
		return -EINVAL;
	gpio = ioremap(GPIO_PHYS_BASE, GPIO_REG_SIZE);
	if (!gpio)
		return -ENOMEM;
	dir_off = pin < 32 ? GPIO_DIR_ABCD : GPIO_DIR_EFGH;
	dat_off = pin < 32 ? GPIO_DATA_ABCD : GPIO_DATA_EFGH;
	bit = 1u << (pin & 31);
	v = ioread32(gpio + dat_off);
	iowrite32(high ? v | bit : v & ~bit, gpio + dat_off);
	v = ioread32(gpio + dir_off);
	iowrite32(v | bit, gpio + dir_off);
	sw_field(SOC_IO_GPIO_EN + 4 * (pin / 32), pin & 31, pin & 31, 1);
	if (!!(ioread32(gpio + dat_off) & bit) != high ||
	    !(ioread32(gpio + dir_off) & bit) ||
	    !(sw_rd(SOC_IO_GPIO_EN + 4 * (pin / 32)) & bit))
		ret = -EIO;
	iounmap(gpio);
	if (ret)
		pr_err("luna-gpon: %s GPIO %d level %u failed: %d\n",
		       what, pin, high, ret);
	return ret;
}

static void luna_resume_poll(void)
{
	if (luna_driver_ready && !luna_stopping && !timer_pending(&gpon_fsm_timer))
		mod_timer(&gpon_fsm_timer, jiffies + msecs_to_jiffies(50));
}

/* Process context under bosa_lock. Timer callbacks never acquire this mutex. */
static int luna_quiesce_locked(void)
{
	WRITE_ONCE(luna_activation_ready, false);
	if (luna_driver_ready) {
		luna_fsm_stop();
		cancel_work_sync(&gpon_cdr_reset_work);
	}
	WRITE_ONCE(luna_health_los, -1);
	gpon_wr_us_protected(GPON_GTC_US_CFG, GPON_US_CFG_BEN_POLAR);	/* disarmed, BEN kept inactive */
	gpon_wr(GPON_GTC_US_PLOAM_CFG, 0);
	gpon_wr(GPON_GTC_US_PLOAM_IND, 0);
	gpon_wr(GPON_GTC_US_WRITE_PROTECT, GPON_US_WP_UNLOCK);
	gpon_field(GPON_GTC_US_PROC_MODE, 0, 0, 0);
	gpon_wr(GPON_GTC_US_WRITE_PROTECT, GPON_US_WP_LOCK);
	pi_field(PI_PONIP_CTL_US, 0, 0, 0);
	return luna_gpio_drive(laser_tx_dis_gpio, true, "TX_DISABLE inhibit");
}

static int luna_activate_locked(bool identity_changed)
{
	struct gpon_range_request request = {
		.identity_defined = gpon_sn_is_set(gpon_sn_bytes),
		.identity_changed = identity_changed,
		.ranging_now = luna_activation_ready,
		.activation_wanted = !gpon_hold,
		.laser_wanted = !laser_off && !skip_bosa,
	};
	struct gpon_range_action action;
	u8 nomsg[12];
	int ret;

	gpon_range_plan(&request, &action);
	if (!action.program) {
		if (action.quiesce) {
			ret = luna_quiesce_locked();
			if (ret)
				return ret;
		}
		return action.result;
	}
	ret = luna_quiesce_locked();
	if (ret)
		goto fail;
	ret = luna_gpio_drive(laser_tx_pwr_gpio, false, "laser TX power");
	if (ret)
		goto fail;
	if (bosa_regs_live()) {
		/* Preserve the positive RTL8290B boot path. GN never arms its servo. */
		ret = bosa_laser_up ? 0 : -ENODEV;
	} else if (optics_self_managed) {
		ret = 0;	/* declared by the board: the module calibrates itself */
	} else {
		ret = luna_gn_calibrate_locked();
	}
	if (ret)
		goto fail;
	gpon_apply_identity_change();
	/* US remains disabled while the checked physical inhibit is released. The
	 * timer is stopped until the complete arming sequence has finished. */
	ret = luna_gpio_drive(laser_tx_dis_gpio, false, "TX_DISABLE release");
	if (ret)
		goto fail;
	WRITE_ONCE(luna_activation_ready, true);
	gpon_wr_us_protected(GPON_GTC_US_CFG,
		GPON_US_CFG_VAL | (force_laser ? BIT(15) : 0));
	gpon_wr(GPON_GTC_US_PLOAM_CFG,
		GPON_US_PLM_CRC_GEN_EN | GPON_US_PLM_ONUID_OVRD);
	gpon_wr(GPON_GTC_US_WRITE_PROTECT, GPON_US_WP_UNLOCK);
	gpon_field(GPON_GTC_US_PROC_MODE, 0, 0, 1);
	gpon_wr(GPON_GTC_US_WRITE_PROTECT, GPON_US_WP_LOCK);
	memset(nomsg, 0xaa, sizeof(nomsg));
	nomsg[0] = 0xff;
	nomsg[1] = 0x04;
	gpon_send_cpu_ploam(PLM_US_QUEUE_NOMSG, nomsg);
	if (datapath_rearm)
		gpon_us_feed_rearm();
	bosa_cal_error = 0;
	luna_optical_init_error = 0;
	return 0;
fail:
	/* Reached only after an optical operation, never an identity/hold refusal. */
	luna_optical_init_error = ret;
	bosa_cal_error = ret;
	/* A release failure can leave the pin at an uncertain level. Re-inhibit,
	 * preserving the original error and reporting an independent inhibit error. */
	if (luna_quiesce_locked())
		pr_err("luna-gpon: optical activation failed and TX inhibit readback failed\n");
	return ret;
}

static int __init rtl9602c_gpon_init(void)
{
	u32 ver, rst, test;
	const char *missing;
	int ret;

	/* The op table is checked BEFORE any hardware is touched. C ...
	 * dev/MEASURED-luna_gpon.c.md sec 256. */
	missing = gpon_ploam_ops_missing(&luna_ploam_ops);
	if (missing) {
		pr_err("luna-gpon: realtek-luna's struct gpon_ploam_ops leaves the MANDATORY op ->%s NULL -- refusing to probe (nothing has been touched)\n",
		       missing);
		return -EINVAL;
	}

	/* The BOARD decides, before any register is touched: its device tree
	 * must carry an enabled realtek,luna-gpon node. A board whose PON wiring
	 * is unproven disables it, and nothing here keys a laser on a shared PON. */
	{
		struct device_node *np = of_find_compatible_node(NULL, NULL,
								 "realtek,luna-gpon");
		bool declared = np != NULL;
		bool on = declared && of_device_is_available(np);

		if (on) {
			luna_pbo_regions_from_dt(np);
			luna_gpon_irq = irq_of_parse_and_map(np, 0);
		}
		/* the board may forbid the laser outright: receive only, nothing
		 * transmitted into a shared PON, whatever the command line says */
		if (on && of_property_read_bool(np, "realtek,laser-off")) {
			laser_off = true;
			pr_info("luna-gpon: this board's device tree forbids the laser (realtek,laser-off): receive only\n");
		}
		of_node_put(np);
		if (!on) {
			pr_info("luna-gpon: this board's device tree %s the GPON (realtek,luna-gpon) -- nothing touched\n",
				declared ? "disables" : "does not declare");
			return -ENODEV;
		}
	}

	gpon_base = ioremap(GPON_PHYS_BASE, GPON_REG_SIZE);
	if (!gpon_base) {
		pr_err("luna-gpon: ioremap 0x%08x failed\n", GPON_PHYS_BASE);
		return -ENOMEM;
	}
	swcore_base = ioremap(SWCORE_PHYS_BASE, SWCORE_REG_SIZE);
	if (!swcore_base) {
		pr_err("luna-gpon: ioremap 0x%08x failed\n", SWCORE_PHYS_BASE);
		iounmap(gpon_base);
		return -ENOMEM;
	}

	/* Detect the chip from the DT root compatible. The RTL9607C ...
	 * dev/MEASURED-luna_gpon.c.md sec 330. */
	is_9607c = of_machine_is_compatible("realtek,rtl9607c");
	/* The THIRD chip. Its table was already in the tree, named by ...
	 * dev/MEASURED-luna_gpon.c.md sec 257. */
	is_9603cvd = of_machine_is_compatible("realtek,rtl9603cvd");

	/* Select this chip's SWCORE offsets BEFORE the first sw_rd/sw_wr of any
	 * chip-selected register (the BOSA I2C pad-mux below is the first). */
	swc = is_9607c ? &gpon_swc_9607c
	    : is_9603cvd ? &gpon_swc_9603cvd : &gpon_swc_9602c;
	gtune = is_9603cvd ? &luna_gtc_tune_9603cvd : &luna_gtc_tune_9602c;
	if (swc->classify_sid_num > LUNA_CLASSIFY_SID_MAX) {
		pr_err("luna-gpon: %s classifies %u SIDs, past LUNA_CLASSIFY_SID_MAX %u\n",
		       swc->chip, swc->classify_sid_num, LUNA_CLASSIFY_SID_MAX);
		ret = -EINVAL;
		goto fail_maps;
	}
	luna_laser_gpio_from_dt();
	luna_led_map_from_dt();
	ret = luna_gpio_drive(laser_tx_dis_gpio, true, "TX_DISABLE inhibit");
	if (ret)
		goto fail_maps;
	pr_info("luna-gpon: SWCORE map = %s (io_mode_en=0x%05x i2c_en_bus0=%u oem_en=%u gpio_en=0x%05x fib_status=0x%05x sds_reg0=0x%05x)\n",
		swc->chip, swc->io_mode_en, swc->io_i2c_en_bus0, swc->io_oem_en,
		swc->io_gpio_en, swc->sds_fib_status, swc->sds_reg0);

	/* IO_MODE_EN.OEM_EN routes the optical front-end pads -- ...
	 * dev/MEASURED-luna_gpon.c.md sec 258. */
	if (!is_9607c && !is_9603cvd) {
		u32 before = sw_rd(SOC_IO_MODE_EN);

		sw_field(SOC_IO_MODE_EN, swc->io_oem_en, swc->io_oem_en, 1);
		pr_info("luna-gpon: OEM_EN (optical pads) bit%u: io_mode_en 0x%08x -> 0x%08x\n",
			swc->io_oem_en, before, sw_rd(SOC_IO_MODE_EN));
	}

	if (is_9607c) {
		skip_bosa = true;
		/* Enable the switch-internal SMI master so the PHY-10 proxy can reach
		 * the I2C-indirect hole (0xB0-0xD8) for the optical-module DDM read. */
		iowrite32(ioread32(swcore_base + SW_IO_MODE_EN_9607C) | SW_MDX_M_EN,
			  swcore_base + SW_IO_MODE_EN_9607C);
		(void)ioread32(swcore_base + SW_IO_MODE_EN_9607C);
		pr_info("luna-gpon: RTL9607C detected — rev-C SerDes, internal front-end (skip BOSA), SMI proxy on\n");
	}

	/* Power up the PON packet-datapath IP domain (see SOC_IP_ENABLE_PHYS). */
	{
		void __iomem *ipen = ioremap(SOC_IP_ENABLE_PHYS, 4);

		if (ipen) {
			u32 mask = SOC_IP_EN_PON;

			/* The 9603CVD sets bit 25 unconditionally, not just the ...
			 * dev/MEASURED-luna_gpon.c.md sec 331. */
			if (is_9607c || is_9603cvd)
				mask |= SOC_IP_EN_PONPBO;	/* PON-IP window / packet datapath */
			writel(readl(ipen) | mask, ipen);
			(void)readl(ipen);		/* post the write */
			iounmap(ipen);
		}
	}

	/* PON-IP datapath window — only reachable now the IP-enable bit is set. */
	ponip_base = ioremap(PONIP_PHYS_BASE, PONIP_REG_SIZE);
	if (!ponip_base) {
		pr_err("luna-gpon: ioremap 0x%08x failed\n", PONIP_PHYS_BASE);
		iounmap(swcore_base);
		iounmap(gpon_base);
		return -ENOMEM;
	}

	ver  = gpon_rd(GPON_VERSION) & GPON_VER_ID_MASK;
	rst  = gpon_rd(GPON_RESET);
	test = gpon_rd(GPON_TEST);

	pr_info("luna-gpon: MAC @0x%08x ver=0x%02x reset=0x%08x test=0x%08x\n",
		GPON_PHYS_BASE, ver, rst, test);

	/* Self-test the register window with the GPON_TEST scratch register, then
	 * restore the power-on value. */
	gpon_wr(GPON_TEST, 0xa5a5a5a5u);
	if (gpon_rd(GPON_TEST) == 0xa5a5a5a5u)
		pr_info("luna-gpon: register R/W OK (scratch verified)\n");
	else
		pr_warn("luna-gpon: scratch R/W failed — MAC may be gated\n");
	gpon_wr(GPON_TEST, GPON_TEST_SCRATCH);

	/* The GPIO pad routing is Board C's, and its register MOVED. ...
	 * dev/MEASURED-luna_gpon.c.md sec 259. */
	if (board_gpio_pads_set && SOC_IO_GPIO_EN) {
		sw_wr(SOC_IO_GPIO_EN, board_gpio_pads[0]);
		sw_wr(SOC_IO_GPIO_EN + 4, board_gpio_pads[1]);
		{
			void __iomem *gpio = ioremap(GPIO_PHYS_BASE, GPIO_REG_SIZE);

			if (gpio) {
				iowrite32(board_gpio_pads[2], gpio + GPIO_DIR_ABCD);
				iowrite32(board_gpio_pads[3], gpio + GPIO_DATA_ABCD);
				iowrite32(board_gpio_pads[4], gpio + GPIO_DIR_EFGH);
				iowrite32(board_gpio_pads[5], gpio + GPIO_DATA_EFGH);
				(void)ioread32(gpio + GPIO_DIR_ABCD);	/* post writes */
				iounmap(gpio);
			}
		}
		pr_info("luna-gpon: GPIO pads set (gpio_en0=0x%08x gpio_en1=0x%08x)\n",
			sw_rd(SOC_IO_GPIO_EN), sw_rd(SOC_IO_GPIO_EN + 4));
	} else if (is_9603cvd && gpio_pad_9603cvd) {
		/* The 9603CVD's OWN pad recipe, from its OWN board: MEASURED ...
		 * dev/MEASURED-luna_gpon.c.md sec 260. */
		void __iomem *gpio = ioremap(GPIO_PHYS_BASE, GPIO_REG_SIZE);

		sw_wr(SOC_IO_GPIO_EN, 0x00008014u);	/* 0x3c on this chip */
		if (gpio) {
			iowrite32(0x00008014u, gpio + 0x08);
			iowrite32(0x0000c000u, gpio + 0x30);
			(void)ioread32(gpio + 0x08);	/* post the writes */
			iounmap(gpio);
		}
		pr_info("luna-gpon: GPIO optical-SD pad recipe APPLIED (%s, from its own stock in O5): io_gpio_en=0x%08x\n",
			swc->chip, sw_rd(SOC_IO_GPIO_EN));
	} else {
		pr_info("luna-gpon: GPIO optical-SD pad recipe skipped (%s: %s)\n",
			swc->chip,
			is_9603cvd ? "recipe exists but gpio_pad_9603cvd=0"
				   : "this board declares no realtek,gpio-pads");
	}

	/* The panel-LED block is Board C's and is gated off on the ...
	 * dev/MEASURED-luna_gpon.c.md sec 261. */
	if (!is_9607c)
		gpon_led_init();

	/* Bring up the PON SerDes so the MAC core gets its clock, ...
	 * dev/MEASURED-luna_gpon.c.md sec 332. */
	{
		const char *via;
		int sret;

		/* The SECOND writer of the GPIO pad array: this function ...
		 * dev/MEASURED-luna_gpon.c.md sec 333. */
		if (!is_9607c && !is_9603cvd)
			rtl9602c_sc_ldo_init();	/* 9602C SC-indirect LDO/thermal — different registers on 9603CVD/9607C */
		else if (is_9603cvd)
			pr_info("luna-gpon: sc_ldo_init skipped (%s: 0x3c/0x40/0x44 are IO_GPIO_EN here, not SC_IND_*)\n",
				swc->chip);
		luna_c2_postmode_perturb = serdes_postmode_perturb;	/* A/B: skip the post-GPON-mode US-TX perturbations (default = skip, stock rev-A) */
		luna_c2_sds_cfgrst = serdes_sds_cfgrst;	/* A/B: SDS reset = stock bit0-only (default) vs legacy bit7+bit0 */
		luna_c2_stock_analog = serdes_stock_analog;	/* A/B: match live-stock SDS REG01/REG11 post-reset (default) */
		luna_c2_analog_postreset = serdes_analog_postreset;	/* A/B: program full analog CMU/CDR table AFTER the SDS reset (stock rev-A, default) = cold-start determinism */
		luna_c2_cmu_settle_ms = serdes_cmu_settle_ms;	/* A/B: TX-CMU-lock settle before reset-B (cold-start metastability candidate) */
		luna_c2_clkgate_rstb = serdes_clkgate_rstb;	/* A/B: clock-gated reset-B release (rank-1 cold-start fix candidate) */
		luna_c2_skip_rstb_dance = serdes_skip_rstb_dance;	/* A/B: skip the gratuitous DIG_1D reset-B pulse (already released) */
		luna_c2_minimal_analog = serdes_minimal_analog;	/* A/B: skip the over-configure golden-table writes (match stock's minimal set) */
		if (force_soc_clk) {
			/* Match the live-stock values of three SoC sysreg words, ...
			 * dev/MEASURED-luna_gpon.c.md sec 262. */
			static const struct { u32 off, val; } soc_clk[] = {
				{ 0x18000100u, 0x00440e00u },	/* strap / pin-status word (bit 5 = CKSEL, read by stock _is_CKSEL_25MHz): stock's live value */
				{ 0x1800012cu, 0x024d024du },	/* DRAM auto-calibration result word A (two identical 16-bit halves), stock stage-1 store @0x9fc14b70: stock's value on the captured boot */
				{ 0x18000140u, 0x024d024du },	/* DRAM auto-calibration result word B, stock stage-1 store @0x9fc14b74: same encoding, same boot */
			};
			unsigned int k;
			for (k = 0; k < ARRAY_SIZE(soc_clk); k++) {
				void __iomem *a = ioremap(soc_clk[k].off, 4);
				if (a) {
					writel(soc_clk[k].val, a);
					pr_info("luna-gpon: force_soc_clk [%#x]<=%#x ->%#x\n",
						soc_clk[k].off, soc_clk[k].val, readl(a));
					iounmap(a);
				}
			}
		}
		/* CROSS-SUBSYSTEM ORDER (bosa_before_serdes): stage the external BOSA RX
		 * analog and settle BEFORE the SoC SerDes bring-up, matching stock, so the
		 * CMU locks against a settled front-end. The duplicate bosa_probe() and
		 * bosa_rx_enable() below are skipped when this runs. */
		if (bosa_before_serdes && !skip_bosa) {
			mutex_lock(&bosa_lock);
			bosa_probe();
			if (bosa_regs_live())
				bosa_rx_enable();
			mutex_unlock(&bosa_lock);
			if (bosa_settle_ms)
				mdelay(bosa_settle_ms);
			pr_info("luna-gpon: BOSA RX up + %ums settle BEFORE SerDes (stock order)\n",
				bosa_settle_ms);
		}
		if (family_lib) {
			/* Clean-room family lib. RTL9607C = c7 rev-C ModeV3 SerDes; RTL9602C
			 * = rev-A. Same ops for both (same SWCORE base; the lib never touches
			 * the 9607C I2C-indirect decode hole). */
			if (is_9607c)
				sret = luna_ponmac_mode_set(LUNA_CHIP_9607C, LUNA_REV_C,
							       LUNA_SUBTYPE_NONE,
							       &rtl9602c_r960_ops);
			else if (is_9603cvd)
				/* rev/subtype are ignored by this chip's path -- one SerDes
				 * variant for every rev (luna_ponmac.c:786). */
				sret = luna_ponmac_mode_set(LUNA_CHIP_9603CVD, LUNA_REV_A,
							       LUNA_SUBTYPE_NONE,
							       &rtl9602c_r960_ops);
			else
				sret = luna_ponmac_mode_set(LUNA_CHIP_9602C, LUNA_REV_A,
							       LUNA_SUBTYPE_NONE,
							       &rtl9602c_r960_ops);
			via = is_9607c ? "family-lib 9607C"
			    : is_9603cvd ? "family-lib 9603CVD" : "family-lib 9602C";
			/* Stability fallback if the lib path ever fails to bring the analog
			 * ready. It MUST NOT fire on the 9603CVD: the inline bring-up IS the
			 * 9602C recipe, so "falling back" would resume writing 0x1E000 low,
			 * into EXTG_ACTYPE on a working LAN path. */
			if (sret && !is_9607c && !is_9603cvd) {
				pr_warn("luna-gpon: family-lib SerDes not ready (0x%08x) -> inline fallback\n",
					sw_rd(FIB_EXT_REG21));
				sret = gpon_serdes_init();
				via = "inline fallback";
			}
		} else if (is_9603cvd) {
			/* Same reason, and the guard was missing on this path:
			 * gpon_serdes_init{,_stock}() ARE the 9602C recipe, and their golden
			 * table writes ~0x226xx, which on the 9603CVD is inside the switch's
			 * EXTG_ACTYPE match table. Refuse instead of corrupting a live LAN. */
			sret = -ENOTSUPP;
			via = "REFUSED (family_lib=0 has no 9603CVD SerDes recipe)";
			pr_err("luna-gpon: family_lib=0 is not available on %s -- the inline SerDes bring-up is the RTL9602C register recipe\n",
			       swc->chip);
		} else {
			sret = serdes_stock_seq ? gpon_serdes_init_stock() : gpon_serdes_init();
			via = serdes_stock_seq ? "stock rev-A order" : "GPON mode";
		}

		if (sret)
			pr_warn("luna-gpon: SerDes analog-ready not seen (%s, FIB_EXT_REG21=0x%08x)\n",
				via, sw_rd(FIB_EXT_REG21));
		else
			pr_info("luna-gpon: PON SerDes up (%s, analog ready)\n", via);
		/* Stock powers the laser driver as the LAST step of its PON-MAC
		 * mode set, i.e. right here, once the SerDes is up. */
		ret = luna_gpio_drive(laser_tx_pwr_gpio, false, "laser TX power");
		if (ret)
			goto fail_maps;

		/*
		 * M3 DIAGNOSTIC (temporary): does forcing FIB_REG16 FRC_SD/SEL_RX_SD make
		 * SDS_SDET assert? Splits SoC-side SD gating from no light at the RX pins.
		 */
		if (is_9607c) {
			const struct luna_ops *o = &rtl9602c_r960_ops;
			u32 s0 = o->rd(0x1b00028cu);
			u32 r0 = o->rd(0x1b040c40u);
			u32 s1, s2;

			o->wr(0x1b040c40u, r0 | (1u << 10));		/* FRC_SD */
			mdelay(5);
			s1 = o->rd(0x1b00028cu);
			o->wr(0x1b040c40u, r0 | (1u << 10) | (1u << 2));	/* + SEL_RX_SD */
			mdelay(5);
			s2 = o->rd(0x1b00028cu);
			o->wr(0x1b040c40u, r0);				/* restore */
			pr_info("luna-gpon: M3-SDprobe base_sts=0x%08x reg16=0x%08x | +frc_sd=0x%08x | +sel_rx_sd=0x%08x (SDS_SDET=bit17)\n",
				s0, r0, s1, s2);
			mutex_lock(&bosa_lock);
			ddm_probe_9607c();
			i2c_scan_9607c();
			mutex_unlock(&bosa_lock);
		}
	}

	/* Probe the external RTL8290B BOSA over I2C (read-only ...
	 * dev/MEASURED-luna_gpon.c.md sec 334. */
	mutex_lock(&bosa_lock);
	if (!bosa_before_serdes)		/* else already probed before the SerDes */
		bosa_probe();

	/* skip_bosa (warm boot): leave the externally-powered BOSA in ...
	 * dev/MEASURED-luna_gpon.c.md sec 335. */
	if (skip_bosa || !bosa_regs_live()) {
		pr_info("luna-gpon: RTL boot programming skipped; GN awaits per-unit calibration\n");
		goto skip_bosa_init;
	}

	/*
	 * Power on the BOSA optical receiver (clears its RX power-down), which is
	 * what makes the real optical signal-detect assert. Before the MAC reset.
	 */
	if (!bosa_before_serdes)		/* else already RX-enabled before the SerDes */
		bosa_rx_enable();

	/* Measure the MPD dark reference while the laser is still off, for the live
	 * TX-power DDM (bosa_tx_power_cdbm). Forces TX off ~200ms; must precede
	 * bosa_tx_enable and never run at O5. */
	if (!laser_off)
		bosa_vmpd_dark_calibrate();

	/* Power on the BOSA optical transmitter so the ONU can send upstream PLOAM
	 * bursts during activation. The APC offset calibration is deferred until
	 * after the PON-IP datapath and MAC reset: the APC digital block only clocks
	 * once the PON TX clock runs, so calibrating earlier leaves OFFK_DONE dead. */
	if (!laser_off) {
		bosa_tx_enable();
		/* This unit's burst bias/mod, when the command line already carries them. */
		if (laser_bias || laser_mod)
			luna_laser_dac_apply();
		/* bosa_laser_maint() (the ~50 ms fault re-ignite) is gated on ...
		 * dev/MEASURED-luna_gpon.c.md sec 263. */
		if (bosa_regs_live())
			bosa_laser_up = 1;
		else
			pr_warn("luna-gpon: laser NOT marked up -- the BOSA register writes were refused (no positive RTL8290B id), so nothing was applied and the ~50ms TX-fault maintenance stays OFF rather than servicing a part it does not fit\n");
	}

skip_bosa_init:
	mutex_unlock(&bosa_lock);
	/* Configure the PON-IP packet datapath so the MAC has somewhere to land
	 * downstream frames before it is reset. */
	gpon_pbo_init();
	pr_info("luna-gpon: PON-IP datapath configured (ctl_us=0x%08x ctl_ds=0x%08x)\n",
		pi_rd(PI_PONIP_CTL_US), pi_rd(PI_PONIP_CTL_DS));

	/* With the SerDes clock present, soft-reset the GPON MAC so its RST_DONE
	 * handshake completes and the GTC banks come out of reset. */
	gpon_wr(GPON_RESET, GPON_SOFT_RST);
	gpon_wr(GPON_RESET, 0);
	if (gpon_wait_rst_done())
		pr_warn("luna-gpon: RST_DONE not seen (reset=0x%08x)\n",
			gpon_rd(GPON_RESET));
	else
		pr_info("luna-gpon: MAC reset done, ONU state O%u\n",
			gpon_rd(GPON_GTC_DS_ONU_ID_STATUS) & GPON_ONU_STATE_MASK);
	luna_ds_cam_scrubbed = !gpon_ds_cam_clear_all();
	if (!luna_ds_cam_scrubbed)
		pr_warn("luna-gpon: DS GEM-port CAM not cleared after the reset: the first Configure_Port-ID clears it\n");

	/* Optical loss-of-signal monitoring, inverted polarity. The downstream framer
	 * gates on OPTIC_LOS_SIG; until the input is enabled with the correct
	 * polarity that status reads "loss" even with real light, holding the FSM at
	 * O1. A working (O5) unit runs this register at 0x03. */
	gpon_field(GPON_GTC_DS_LOS_CFG_STS, 0, 0, 1);	/* OPTIC_LOS_EN = 1     */
	gpon_field(GPON_GTC_DS_LOS_CFG_STS, 1, 1, 1);	/* OPTIC_LOS_POLAR = 1  */
	pr_info("luna-gpon: optical-LOS monitor enabled (los_cfg=0x%08x)\n",
		gpon_rd(GPON_GTC_DS_LOS_CFG_STS));

	/* Now that the PON-IP/MAC (and thus the SerDes TX clock) are running, run
	 * the laser APC offset calibration so the laser actually biases. */
	mutex_lock(&bosa_lock);
	if (bosa_regs_live() && !skip_bosa && !laser_off && apc_offk)
		rtl8290b_apc_init();		/* B-variant: completes OFFK (DS-safe) */
	else if (bosa_regs_live() && !skip_bosa && !laser_off && !apc_off)
		bosa_apc_calibrate();		/* legacy non-B flow (A/B fallback) */
	mutex_unlock(&bosa_lock);

	proc_create_single("gpon", 0444, NULL, gpon_proc_show);
	proc_create_single("gpon_log", 0444, NULL, luna_log_show);
	proc_create_single("gpon_timing", 0444, NULL, luna_timing_show);
	luna_mon_register();
	proc_create_single("bosadump", 0444, NULL, bosadump_proc_show);
	proc_create_single("pidump", 0444, NULL, pidump_proc_show);
	proc_create_single("swdump", 0444, NULL, swdump_proc_show);
#ifdef CONFIG_GPON_OLT_DIAG
	proc_create_single("oltcap", 0444, NULL, oltcap_proc_show);
	proc_create_single("gpon_prov", 0444, NULL, gpon_prov_proc_show);
	proc_create_single("gpon_unknown", 0444, NULL, gpon_unknown_proc_show);
	proc_create_single("ploam_unknown", 0444, NULL, ploam_unknown_proc_show);
	proc_create_single("ploamcap", 0444, NULL, ploamcap_proc_show);
#endif

	/* Upstream burst CONFIG + laser-enable timing. The GTC MAC ...
	 * dev/MEASURED-luna_gpon.c.md sec 264. */
	/* Only the burst-enable polarity now (the rest waits for activation): with
	 * 0 here BEN idled ACTIVE until activation, and a board that cannot reach
	 * activation kept its laser on -- the V2801RGW jammed GPON2 in O1. */
	gpon_wr_us_protected(GPON_GTC_US_CFG, GPON_US_CFG_BEN_POLAR);
	pr_info("luna-gpon: US_CFG burst-enable polarity set at init: 0x%08x\n",
		gpon_rd(GPON_GTC_US_CFG));
	if (force_laser)
		pr_info("luna-gpon: force_laser=1 -> US_CFG.FS_LON set (CW diagnostic)\n");
	/* US GEM-header PTI vector (not write-protected). Reset is 0, so every US GEM
	 * frame would carry PTI=000 even on end-of-fragment, and some OLTs only
	 * accept OMCI with NON_END_FRAG=0 and END_FRAG=1. Stock's vector (0,1,0,1)
	 * is 0x00001010, with FS_GEM_IDLE[31]=0 keeping auto-idle. */
	gpon_wr(GPON_GEM_US_PTI_CFG, force_idle ? 0x80001010u : 0x00001010u);	/* FS_GEM_IDLE(bit31)=force_idle: bisection diag (stock=0) */
	gpon_wr_us_protected(GPON_GTC_US_LASER, gtune->us_laser);	/* per chip: 0x2028 / 0x1820 */

	/* The laser bias is loaded LOW for acquisition (see ...
	 * dev/MEASURED-luna_gpon.c.md sec 265. */
	gpon_field(GPON_GTC_DS_ONU_ID_STATUS, 15, 8, 0xff);	/* DS ONU-ID = broadcast */
	gpon_field(GPON_GTC_US_ONU_ID, 15, 8, 0xff);		/* US ONU-ID = broadcast */
	/* DS_CFG's FEC detection threshold, asserted deliberately (2026-09-08): the
	 * bootloader left this board at FEC_DET_THRSH=1 while its own stock rests at
	 * 0, and the vendor sets it from configuration in its init path. Not a BWmap
	 * acceptance gate -- all four of those already matched. */
	gpon_field(0x1014, 3, 1, 0);				/* DS_CFG FEC_DET_THRSH = 0 (stock) */
	gpon_field(0x1014, 11, 11, 0);				/* DS_CFG BWM_NO_FLT = 0 (stock). The
								 * bring-up =1 "accept all grants" left bwm_acpt=0
								 * against stock's 130k+, i.e. it broke the BWMAP
								 * parser rather than relaxing it. */
	gpon_wr(GPON_GTC_US_PLOAM_CFG, 0);
	gpon_wr(GPON_GTC_US_WRITE_PROTECT, GPON_US_WP_UNLOCK);
	gpon_field(GPON_GTC_US_PROC_MODE, 0, 0, 0);
	gpon_wr(GPON_GTC_US_WRITE_PROTECT, GPON_US_WP_LOCK);
	gpon_field(GPON_GTC_DS_PLOAM_CFG, 9, 9, 1);		/* DS PLOAM BC_ACCEPT */
	gpon_field(GPON_GTC_DS_PLOAM_CFG, 8, 8, 1);		/* DS PLOAM ONUID_FILTER ON:
		* every Assign_ONU-ID this OLT sends is BROADCAST, so BC_ACC_EN already
		* delivers it. What the filter buys is real -- this bench puts two ONUs on
		* one splitter, and with it off we also accepted the other unit's PLOAMs.
		* Stock reads GPON_GTC_DS_PLOAM_CFG = 0x70b with the bit SET. */
	/* (6) DS_INTR_MASK = 0x070f, the O5 operating value: LOS/LOF/FEC/LOM (b0-3)
	 * plus SN_REQ(b8) / RNG_REQ(b9) / PLM_BUF(b10), which gate the GTC's
	 * upstream serial-number, ranging and PLOAM-buffer event handling. The MAC
	 * reset clears the register, so it must be re-set here. */
	gpon_wr(GPON_GTC_DS_INTR_MASK, 0x070f);

	/* Upstream burst TIMING. MIN_DELAY1 = 290 bits, MIN_DELAY2 = ...
	 * dev/MEASURED-luna_gpon.c.md sec 336. */
	gpon_wr_us_protected(GPON_GTC_US_MIN_DELAY, 0x9132);
	/* An __init caller of the equalization-delay computation, through the core's
	 * gpon_ploam_set_eqd(). Do NOT satisfy it by copying the arithmetic back in
	 * here -- that fork is what killed gpon_proto.c. */
	gpon_set_eqd(0);			/* pre-ranging EqD = 290*128 = 0x9100 */

	/* O5 grant-burst optical config -- the "Laser out"/LOAi wall ...
	 * dev/MEASURED-luna_gpon.c.md sec 266. */
	gpon_wr_us_protected(0x5188, gtune->us_optic_sd_th);	/* US_OPTIC_SD_TH: per chip (9602C live 0x00504bfa, 9603CVD live 0x00a07fff) */
	gpon_field(0x526c, 0, 0, 1);			/* US_PWR_SAV_MODE.PWR_SAV_MODE = 1 (live stock = 1) */
	if (gtune->us_pwr_sav_dg_tx_opt)
		gpon_field(0x526c, 16, 16, 1);		/* DG_TX_OPT: 9603CVD live 0x00010001 */
	gpon_wr(GPON_GEM_US_PWR_SAV_CFG, (0x10u << 16) | 0x100u);	/* GEM_US_PWR_SAV_CFG = 0x00100100 (live stock) */
	gpon_wr(GPON_GEM_US_EOB_MERGE, 0x00000028u);			/* GEM_US_EOB_MERGE = 0x28 (live stock; mine omitted) */

	/* Default upstream burst overhead (G.984.3): 0xAA preamble ...
	 * dev/MEASURED-luna_gpon.c.md sec 267. */
	gpon_apply_boh(false);

	/* US-FEED RE-ARM (stock dataPath_reset arm order): pbo_init ran early and the
	 * MAC/GTC bring-up above parked the US-feed FSM. Re-arm it now, the last
	 * datapath step before the FSM ranges. */
	if (datapath_rearm)
		gpon_us_feed_rearm();

	/* The SDS upstream-TX serializer registers are deliberately ...
	 * dev/MEASURED-luna_gpon.c.md sec 268. */
	gpon_parse_sn(onu_sn);

	/* Bring the core's FSM object up alongside ours; it does not ...
	 * dev/MEASURED-luna_gpon.c.md sec 269. */
	luna_ploam_cfg_live = luna_ploam_cfg;
	luna_ploam_cfg_live.hold = gpon_hold;
	luna_ploam_cfg_live.cdr_reseat_on_reactivate = cdr_reseat_on_reactivate;
	luna_ploam_cfg_live.o5_rearm_burst_gate = o5_rearm_burst_gate;
	luna_ploam_cfg_live.o3_feed_reset = o3_feed_reset;
	luna_ploam_cfg_live.data_gem_en = data_gem_en;
	luna_ploam_cfg_live.omcc_alt_bind = omcc_alt_bind;
	/* The OMCC Alloc-ID override: the native FSM reads gpon_omcc_alloc directly,
	 * so the core must get the same value or the two FSMs bind the OMCC T-CONT
	 * to different allocs under one module parameter. */
	luna_ploam_cfg_live.omcc_alloc_override = gpon_omcc_alloc;
	/* The three tick tunables. Leaving them out was a silent ...
	 * dev/MEASURED-luna_gpon.c.md sec 270. */
	luna_ploam_cfg_live.trace = trace;
	luna_ploam_cfg_live.los_rerange_ticks = los_rerange_ticks;
	luna_ploam_cfg_live.o4_ranging_timeout_ticks = o4_ranging_timeout_ticks;
	luna_ploam_cfg_live.o5_provision_watchdog_ticks = o5_provision_watchdog_ticks;
	luna_ploam_cfg_live.o5_ungranted_ticks = o5_ungranted_ticks;
	luna_ploam_cfg_live.o5_ploam_keepalive_ticks = o5_ploam_keepalive_ticks;
	/* The fourth, the same omission again: the core read early_dwell_report_ticks
	 * and nothing gave it a value, so the O1 diagnostic was compiled in and MUTE.
	 * ONU-test-case/dead_knob_guard.py catches this shape. */
	luna_ploam_cfg_live.early_dwell_report_ticks = early_dwell_report_ticks;
	/* The core hands back the name of any mandatory op still missing and REFUSES a
	 * half-wired table (GPON_MUST_CHECK). The check at the top of this function
	 * has already proved it complete, so this makes a FUTURE divergence a
	 * refusal instead of a warning. */
	missing = gpon_ploam_init(&luna_ploam, &luna_ploam_ops,
				  &luna_ploam_cfg_live, NULL, gpon_sn_bytes);
	if (missing) {
		pr_err("luna-gpon: gpon_ploam_init refused: mandatory op ->%s is NULL\n",
		       missing);
		return -EINVAL;
	}
	spin_lock_bh(&luna_pwd_lock);
	if (gpon_ploam_password_parse(onu_password, luna_pwd))
		pr_warn("luna-gpon: onu_password '%s' is not a G.984.3 password; sending an empty one\n",
			onu_password);
	gpon_ploam_set_password(&luna_ploam, luna_pwd);
	luna_pwd_dirty = false;
	spin_unlock_bh(&luna_pwd_lock);
	if (gpon_sn_is_set(gpon_sn_bytes))
		pr_info("luna-gpon: PLOAM FSM start, SN '%s' = %*phN\n",
			onu_sn, 8, gpon_sn_bytes);
	else
		pr_info("luna-gpon: PLOAM FSM start with NO serial provisioned -- parked at O1 until onu_sn is written\n");
	INIT_WORK(&gpon_cdr_reset_work, gpon_cdr_reset_worker);
	INIT_DELAYED_WORK(&gpon_optical_work, gpon_optical_work_fn);

	timer_setup(&gpon_fsm_timer, gpon_fsm_poll, 0);
	mutex_lock(&bosa_lock);
	luna_driver_ready = true;
	bosa_cal_error = luna_activate_locked(false);
	if (bosa_cal_error)
		pr_info("luna-gpon: upstream inhibited, activation pending: %d\n", bosa_cal_error);
	luna_resume_poll();
	mutex_unlock(&bosa_lock);
	luna_gpon_irq_request();
	luna_health_proc = proc_create_single("gpon_health", 0444, NULL, luna_health_show);
	if (!luna_health_proc)
		pr_warn("luna-gpon: cannot expose cached health status\n");
	schedule_delayed_work(&gpon_optical_work, msecs_to_jiffies(50));
	return 0;
fail_maps:
	if (ponip_base) {
		iounmap(ponip_base);
		ponip_base = NULL;
	}
	iounmap(swcore_base);
	swcore_base = NULL;
	iounmap(gpon_base);
	gpon_base = NULL;
	return ret;
}

static void __exit rtl9602c_gpon_exit(void)
{
	/* Before any unmap: a class device outliving its register window is a
	 * write through a stale ioremap the moment someone echoes to it. */
	gpon_led_sysfs_exit();
	mutex_lock(&bosa_lock);
	luna_stopping = true;
	if (luna_quiesce_locked())
		pr_err("luna-gpon: TX inhibit failed during unload\n");
	luna_driver_ready = false;
	/* Close the UNI enqueue gate before anything is cancelled, so a
	 * producer that is already running cannot re-arm behind the cancel. */
	luna_uni_apply_close();
	mutex_unlock(&bosa_lock);
	luna_fsm_stop();
	cancel_work_sync(&gpon_cdr_reset_work);	/* it masks the line, so before free_irq */
	if (luna_gpon_irq) {
		sw_field(SW_INTR_IMR, 10, 10, 0);
		free_irq(luna_gpon_irq, NULL);
		cancel_work_sync(&luna_ds_ploam_work);
	}
	cancel_work_sync(&luna_omci_work);	/* no enqueue since luna_activation_ready fell */
	cancel_delayed_work_sync(&gpon_optical_work);
	/* OUTSIDE bosa_lock, and after the producers: the worker holds that
	 * mutex for its whole apply, so cancelling under it would deadlock. */
	cancel_delayed_work_sync(&luna_uni_apply_work);
	proc_remove(luna_health_proc);
	luna_health_proc = NULL;
	remove_proc_entry("gpon", NULL);
	remove_proc_entry("gpon_log", NULL);
	remove_proc_entry("gpon_timing", NULL);
	luna_mon_unregister();
	remove_proc_entry("bosadump", NULL);
	remove_proc_entry("pidump", NULL);
	remove_proc_entry("swdump", NULL);
#ifdef CONFIG_GPON_OLT_DIAG
	remove_proc_entry("oltcap", NULL);
	remove_proc_entry("gpon_prov", NULL);
	remove_proc_entry("gpon_unknown", NULL);
	remove_proc_entry("ploam_unknown", NULL);
	remove_proc_entry("ploamcap", NULL);
#endif
	if (ponip_base)
		iounmap(ponip_base);
	if (swcore_base)
		iounmap(swcore_base);
	if (gpon_base)
		iounmap(gpon_base);
}

module_init(rtl9602c_gpon_init);
module_exit(rtl9602c_gpon_exit);

MODULE_DESCRIPTION("Realtek RTL9602C GPON MAC foundation driver");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE("rtkbosa_k.bin");
