// SPDX-License-Identifier: GPL-2.0-or-later


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
#include "flowcore.h"
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
#include <linux/reboot.h>
#include <linux/interrupt.h>
#include "gpon_gem_diag.h"
#include "gpon_olt_diag.h"	/* the far-end capture: replayed off the board */
#include "gpon_mon.h"		/* the control plane as Ethernet frames for gponmon0 */
#include "gpon_lat.h"		/* gpon_lat_add(): the OMCI turnaround record */
#include <linux/etherdevice.h>
#include "luna_gpon_nic.h"
#include "gpon_omci_mic.h"	/* the DS OMCI MIC check omcid is handed */
#include "luna_xpon_hw.h"	/* the omcid backend's view of this MAC */
#include "luna_eth_regs.h"	/* SOC_SW_ENABLE + the family register map */
#include "luna_ponmac.h"		/* clean-room family PON-MAC/SerDes bring-up lib */

/*
 * The GPON/protocol log: a ring read at /proc/gpon_log, never printk, since
 * dmesg is for the device itself. Protocol events run in the FSM timer, where a
 * console line cost the production OLT's Password deadline. Lines keep the
 * dmesg spelling, so a reader only changes its source. luna_diag() below is the
 * exception: its dmesg lines wait for a quiet line.
 */
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

/* Anything this driver receives and cannot place */
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
/* The live one, from the selected chip's table. `swc` is assigned before any GPON
 * register is touched, and every use of this macro sits after that. */
#define GPON_OMCC_FLOW		((unsigned int)swc->omcc_flow)
/* The OMCC's PHYSICAL queue id, and it is PER CHIP */
#define GPON_OMCC_PHYS_QID	((unsigned int)swc->omcc_phys_qid)
#define GPON_OMCC_TCONT		16	/* the OMCC's T-CONT index; same on both dies */
#define GPON_DATA_TCONT		8	/* the WAN data GEM's own T-CONT; see GPON_DATA_ALLOC */

/* PLOAM message path */
#define GPON_GTC_US_ONU_ID_SHIFT 8		/* [15:8] OLT-assigned ONU-ID  */

/* SoC hardware I2C master (SWCORE register file). */

/* SoC GPIO controller */
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
/* No compiled-in serial. */
static char *onu_sn = "";
static DEFINE_MUTEX(bosa_lock);
static bool luna_driver_ready, luna_activation_ready, luna_stopping;
static bool bosa_gn_identified, bosa_cal_ready;
static int bosa_cal_error;
static int luna_optical_init_error;
static int luna_health_los = -1;
static int luna_los_reported = -1;	/* the last value net/xpon was told; -1 = none */
static struct proc_dir_entry *luna_health_proc;
static int luna_activate_locked(bool identity_changed);
static int luna_quiesce_locked(void);
static void luna_resume_poll(void);
static void luna_omci_identity_reset(const u8 sn[8]);
static void luna_data_suspend(void);
static void luna_omci_seq_show(struct seq_file *s);
static int luna_data_retire(void);
static void luna_xpon_hw_arm_broadcast(void);
static const bool ctrl_queue = true;
static bool luna_ctrl_flow_wanted(void);
static u8 luna_data_qid;
static bool gpon_sn_changed;		/* SN (re)provisioned -> FSM must re-range */

/* Forward declaration */
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
		pr_err("luna-gpon: [SYS-001] gponmon netdev not allocated -- no control-plane capture\n");
		return;
	}
	eth_hw_addr_random(dev);
	if (register_netdev(dev)) {
		pr_err("luna-gpon: [SYS-002] gponmon netdev not registered -- no control-plane capture\n");
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
static u32 luna_fsm_gap_max_ms, luna_ds_latency_max_ms;	/* worst seen since boot */
static u32 luna_ds_irqs, luna_ds_unsignalled, luna_ds_parks;	/* interrupts; drains the tick found unannounced; times the line went off */

static void luna_ploam_capture(enum gpon_olt_dir dir, const u8 *m, unsigned int len)
{
#ifdef CONFIG_GPON_OLT_DIAG
	/* The OLT repeats its PON-ID every 10 s: keep only a change, or the
	 * ring loses the activation it was holding within the hour. */
	static u8 pon_id[12];
	unsigned long flags;

	if (dir == GPON_OLT_DS && len >= sizeof(pon_id) && m[1] == PLM_DS_PON_ID) {
		if (!memcmp(pon_id, m, sizeof(pon_id)))
			goto mon;
		memcpy(pon_id, m, sizeof(pon_id));
	}
	spin_lock_irqsave(&luna_ploam_cap_lock, flags);
	gpon_olt_capture_add(&luna_ploam_cap,
			     div_u64(dir == GPON_OLT_DS && luna_ds_rx_ns ? luna_ds_rx_ns :
				     ktime_get_boottime_ns(), 1000),
			     (u8)luna_ploam.state, dir, m, len);
	spin_unlock_irqrestore(&luna_ploam_cap_lock, flags);
mon:
#endif
	luna_mon_frame(GPON_MON_PLOAM, dir == GPON_OLT_US, m, len);
}

/* A new serial re-ranges: quiesce, retire the old data path, re-activate.
 * @kp is the module parameter when the write came through it. */
static int luna_sn_apply(const char *val, const struct kernel_param *kp)
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
	ret = kp ? param_set_charp(val, kp) : 0;
	if (ret)
		goto out;
	if (changed) {
		memcpy(gpon_sn_bytes, parsed, sizeof(parsed));
		luna_xpon_report_identity();
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

static int onu_sn_set(const char *val, const struct kernel_param *kp)
{
	return luna_sn_apply(val, kp);
}
static const struct kernel_param_ops onu_sn_ops = {
	.set = onu_sn_set,
	.get = param_get_charp,
};
module_param_cb(onu_sn, &onu_sn_ops, &onu_sn, 0644);
MODULE_PARM_DESC(onu_sn, "ONU serial number (G.984.3 ONU-SN): 4 ASCII ID chars + 8 hex digits");

/*
 * The G.984.3 Password stock sends: the unit's own MIB value, which
 * gpon_provision writes here, else stock's default. The FSM timer is the only
 * writer of the core copy.
 */
static char *onu_password = "1234567890";
static u8 luna_pwd[GPON_PLOAM_PASSWORD_LEN];
static bool luna_pwd_dirty;
static u32 luna_us_ploam_full_drops;	/* urgent PLOAMs refused on a full queue */
static DEFINE_SPINLOCK(luna_pwd_lock);

/* The FSM timer picks a new password up; it is the core copy's only writer. */
static void luna_pwd_apply(const u8 pwd[GPON_PLOAM_PASSWORD_LEN])
{
	spin_lock_bh(&luna_pwd_lock);
	memcpy(luna_pwd, pwd, GPON_PLOAM_PASSWORD_LEN);
	luna_pwd_dirty = true;
	spin_unlock_bh(&luna_pwd_lock);
	luna_xpon_report_identity();
}

static int onu_password_set(const char *val, const struct kernel_param *kp)
{
	u8 pwd[GPON_PLOAM_PASSWORD_LEN];
	int ret;

	if (gpon_ploam_password_parse(val, pwd))
		return -EINVAL;
	ret = param_set_charp(val, kp);
	if (ret)
		return ret;
	luna_pwd_apply(pwd);
	return 0;
}
static const struct kernel_param_ops onu_password_ops = {
	.set = onu_password_set,
	.get = param_get_charp,
};
module_param_cb(onu_password, &onu_password_ops, &onu_password, 0644);
MODULE_PARM_DESC(onu_password, "G.984.3 PLOAM Password: up to 10 printable characters, or the 20 hex digits a vendor MIB may store, sent zero-padded (default: stock's 1234567890)");

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

/* The SWCORE offsets that move between family members. */
struct gpon_swc_map {
	const char *chip;
	/* The OMCI flow/SID index is per chip. */
	u8  omcc_flow;		/* GPON_OMCI_FLOW_ID -- per-chip, never shared */
	/* PON_SCH_QMAP's entry width */
	u8  sch_qmap_bits;	/* PON_SCH_QMAP array-offset (bits per T-CONT entry) */
	/* PON_SID_RPV_TH's entry count */
	u16 sid_rpv_entries;	/* PON_SID_RPV_TH declared entries, per chipdef */
	/* The US classify index space */
	u16 classify_sid_num;	/* US classify SIDs to invalidate, per chipdef */
	/* A pointer, not a second copy. */
	const struct luna_sw_map *sw;
	/* The OMCC's PHYSICAL queue id -- 64 / 127 / 120, see GPON_OMCC_PHYS_QID. */
	u8  omcc_phys_qid;
	/* The invalidation pre-pass's scratch queue */
	u8  scratch_phys_qid;
	/* The T-CONT -> physical queue formula, per chip */
	u8  tcont_queue_max;
	u8  tcont_group;
	/* ACCEPT_MAX_LEN_CTRL is not the same field on every die. */
	u8  amax_msb;
	u16 amax_val;
	/* How far the loop goes, per die */
	u8  amax_last_port;
	/* The PON port takes it too. */
	u16 amax_pon_val;
	/* The downstream PBO's page pool, per chip */
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
	/* The indirect I2C master, whose whole block sits +4 on */
	u32 i2c_ind_wd;		/* I2C_IND_WD  [31:0] write data               */
	u32 i2c_ind_adr;	/* I2C_IND_ADR [31:0] target reg offset        */
	u32 i2c_ind_cmd;	/* I2C_IND_CMD [0]CMD_EN [1]RW_EN [2]BUSY [3]NACK */
	u32 i2c_ind_rd;		/* I2C_IND_RD  [31:0] read data                */
	/* The SerDes window base. */
	u32 sds_win;		/* base of the SDS_ANA_* / WSDS_DIG_* window   */
	/* SDS_CFG is NOT in that window and does NOT follow it: it is 0x1d0 /
	 * 0x200 / 0x270 on the three chips, so it gets its own field. */
	u32 sds_cfg;		/* SDS_CFG [4:0] SDS_MODE                      */
	/* Base of the six contiguous OMCI packet counters. */
	u32 omci_cnt;		/* OMCI_DROP_PKT_CNT, the first of six         */
	/* The irregular registers */
	u32 sc_ind_wd;               /* SC_IND_WD */
	u32 wrap_gphy_misc;          /* WRAP_GPHY_MISC */
	u32 thermal_ctrl_0;          /* THERMAL_CTRL_0 */
	u32 force_p_ablty;           /* FORCE_P_ABLTY */
	u32 ablty_force_mode;        /* ABLTY_FORCE_MODE */
	u32 dyngasp_ctrl;            /* DYNGASP_CTRL */
	u32 accept_max_len_ctrl;     /* ACCEPT_MAX_LEN_CTRL */
	u32 pon_trap_cfg;            /* PON_TRAP_CFG */
	u32 cf_cfg;                  /* CF_CFG -- not in 9603cvd's map */
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

/* RTL9602C, and the RTL9601D, whose stock X100DG reads the same block version
 * 0x16030301 and drives its lamps at these offsets. */
static const struct luna_led_regs luna_led_9602c = {
	.force_value = LED_FORCE_VALUE, .blink_rate = LED_BLINK_RATE, .blink_lsb = 12,
	.para_en = LED_PARA_EN, .pad_en = LED_IO_EN,
	.seri_para = BIT(19) | BIT(18), .seri_pad = BIT(17),
	.cpu_force_bit = 12, .pon_type = 0x1b, .link_act = 0xf78, .indices = 16,
};

static const struct gpon_swc_map gpon_swc_9602c = {
	.chip = "RTL9602C",
	.led = &luna_led_9602c,
	/* Registered, not left to the compiler */
	/*
	 * The DS DRAM-pool words both 9602C-family stocks run (X111W and
	 * X100DG); written only for a board whose DT reserves a DS pool.
	 */
	.ds_dsc_cfg = 0x1fff001fu, .ds_dscrunout = 0x1fb8001eu,
	.ds_fc_config = 0x1f321f52u,
	.omcc_flow = GPON_OMCC_FLOW_9602C,
	.ds_dram_order = 0,	/* SRAM-only DS, as this chip's stock does */
	.sch_qmap_bits = 32,	/* chipdef array offset 32 */
	.sid_rpv_entries = 65,	/* chipdef array index 0..64 */
	.classify_sid_num = 65,	/* CLASSIFY_SID_MAX 65 == PON_SID2QID's 65 entries */
	.sw = &rtl9602c_sw_map,
	.omcc_phys_qid = 64,	/* TCONT_QUEUE_MAX(32)*(16/8)+0 */
	/* The same queue as the OMCC, deliberately unchanged. */
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

/* Resolved from the DT compatible in probe(), BEFORE any sw_rd/sw_wr of a
 * chip-selected offset. Defaults to the 9602C so a boot on an undeclared board
 * behaves exactly as this driver did before the table existed. */
static const struct gpon_swc_map *swc = &gpon_swc_9602c;

/* The physical queue a T-CONT drains its logical queue 0 on. */
static u8 luna_tcont_phys_qid(u8 tcont)
{
	if (tcont == GPON_OMCC_TCONT)
		return swc->omcc_phys_qid;
	return (u8)(swc->tcont_queue_max * (tcont / swc->tcont_group));
}

/* Upstream-optics operating values, per chip */
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

#define PI_X(a9602c)		(a9602c)

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
/* The six OMCI packet counters, contiguous from the */
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
/* Install the WAN data-GEM datapath. */
static const bool data_gem_en = true;
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

static const bool cdr_reseat_on_reactivate = true;	/* on a deactivate->O1 re-range, re-pulse the softirq-safe US-TX
			 * SerDes interface reset-B (WSDS_DIG_1D[16], the same primitive
			 * the boot path and the O3 re-sync use) so a marginal serializer
			 * lock from the prior activation is re-attempted immediately,
			 * cutting cold-start O5<->O1 activation flapping. TX-interface
			 * only: the locked DS RX framer is undisturbed. */
#ifdef CONFIG_GPON_PLOAM_DIAG
static bool ploam_tx_dbg = true;	/* log per-send US-PLOAM TX (ENQ self-clear = HW
					 * transmitted), to show whether the urgent-queue
					 * ACK/Password leaves the ONU. Logs first 40 sends. */
module_param(ploam_tx_dbg, bool, 0644);
#else
static const bool ploam_tx_dbg;
#endif
/*
 * o5_rearm_burst_gate: re-apply the US burst-gate cluster
 * (0x5188/0x526c/0x6024/0x6260) and re-arm the HW auto-No_message keepalive
 * template on every O5 entry (not just __init), so a re-ranged O5 after a
 * GMAC/SDS reset does not run on US-side reset defaults.
 */
static const bool o5_rearm_burst_gate = true;
/*
 * o5_ploam_keepalive_ticks: emit a No_message US PLOAM (HW auto queue 0x7)
 * every N FSM ticks (10ms each) while at O5 with an assigned ONU-ID, so a valid
 * PLOAM is present in the OLT's granted slots regardless of how the shared
 * US-PLOAM buffer was last written. 0 = disabled.
 */
static const uint o5_ploam_keepalive_ticks;	/* default OFF (match stock: zero unsolicited US-PLOAM at O5) */

static const uint o5_provision_watchdog_ticks;	/* 0 = off */
/*
 * o5_ungranted_ticks: leave O5 for O1 after this many ~10 ms ticks with no
 * grant to this ONU-ID, meaning the OLT dropped it. A healthy O5 sees grants
 * every second; misses in a row double the wait, up to 32x.
 */
static const uint o5_ungranted_ticks = 1000;

/* los_rerange_ticks: autonomous downstream-LOS recovery. */
static const uint los_rerange_ticks = 30;		/* ~300ms of (optic_los & !sds_sdet): catches a real
						 * 2-4s fiber pull, ~3x stock's 100ms TO2; a sub-second
						 * pad-steal can neither reach it nor (lacking
						 * !sds_sdet) be counted at all */
static u32 gpon_los_run;			/* consecutive real-LOS (optic_los & !sds_sdet) tick count */

/* G.984.3 TO1: an ONU-ID with no Ranging_Time for this long goes back to O1.  Stock
 * runs TO1 10000 ms (G24W `diag gpon get`, ONU-G24W.md); without it an OLT that
 * restarts with its light on leaves this ONU at O4 for good. */
static const uint o4_ranging_timeout_ticks = GPON_DWELL_TO1_TICKS;

/* The O1/O2/O3 dwell report */
static const uint early_dwell_report_ticks = 500;	/* ~5 s at the ~10ms tick */

/* Fiber-pull / re-range diagnostic */
static u32 gpon_rerange_cnt;
static u32 gpon_last_outage_ms;
static unsigned long gpon_rerange_start_j;
static unsigned long gpon_rerange_last_log_j;

/* last DS PLOAM type drained this cycle, surfaced on the periodic O5 line */
static u8 gpon_last_ds_type;
/*
 * Diagnostic: force the upstream laser continuously on (US_CFG.FS_LON). Tests
 * whether the SoC SerDes-TX can drive the BOSA at all, independent of the GTC
 * burst scheduler. Continuous light jams a multi-ONU PON.
 */
static bool force_laser;
module_param(force_laser, bool, 0444);
MODULE_PARM_DESC(force_laser, "force US laser CW on (US_CFG FS_LON) — SerDes-TX emission diagnostic");


#define GPON_CDR_STUCK_MAX	8	/* FAST re-acquire attempts before backing off */
/* RATE-BOUNDED, NEVER COUNT-CAPPED */
#define GPON_CDR_STUCK_SLOW_TICKS	6000	/* ~60 s at the 10 ms poll */
static unsigned int gpon_cdr_stuck_tries;	/* consecutive attempts THIS episode */
static unsigned int gpon_cdr_stuck_count;	/* diag: total wedges detected */
static unsigned int gpon_cdr_stuck_fixed;	/* diag: wedges cleared by the toggle */
static u32 gpon_gtc_ds_sts_last;		/* diag: last raw GTC DS status seen */

/* The I2C bus the BOSA hangs on, from DT (/pon-optics realtek,i2c-bus): 1 on
 * the X100DG, whose stock loads its BOSA driver with I2C_PORT=1. */
static unsigned int bosa_i2c_bus;
static const bool lan_keep_open = true;
static const bool sc_ldo_init = true;

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
/* DIAGNOSTIC: skip BOSA TX power-on + APC ignition */
static bool laser_off;		/* default false; set via luna_gpon.laser_off=1 for the isolation test */
module_param(laser_off, bool, 0444);
MODULE_PARM_DESC(laser_off, "skip laser TX-enable+APC (DS-RX-vs-laser isolation: laser-on deafens DS RX)");
/* Receive-only: the FSM stays at O1 and the laser off. The ONU boots held,
 * as the GC1601 does, and omci-agent releases it through allow_tx unless
 * omci.main.transmit is 0. */
static bool gpon_hold = true;
module_param(gpon_hold, bool, 0444);
MODULE_PARM_DESC(gpon_hold, "hold the ONU receive-only at O1 (no ranging)");
/* gpon_sn_bytes is defined near the top: the onu_sn setter needs it. */
static struct timer_list gpon_fsm_timer;
/* The tick and the downstream-PLOAM interrupt both drive the core under it. */
static DEFINE_SPINLOCK(luna_fsm_lock);
static int luna_gpon_irq;		/* 0: the device tree wires none */
static u8 gpon_fsm_state = 1;		/* O1 */

/* Deferred US-TX CDR-reset (re-range path). */
static struct work_struct gpon_cdr_reset_work;

/* Last upstream-burst-overhead parameters the OLT dictated. */
static const u8 gpon_boh_guard;
static const u8 gpon_boh_ptn = 0xaa;			/* Type-3 preamble pattern */
static const u8 gpon_boh_delim[3] = { 0xab, 0x59, 0x83 };
static const u8 gpon_boh_t3pre;				/* Type-3 pre-ranged length */
static const u8 gpon_boh_t3ranged;			/* Type-3 ranged length */
/* The tick period is defined once. */
static const uint gpon_fsm_tick_req_ms = 10;

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
static u32 gpon_ds_blank;	/* of which all zero: not a message, see gpon_fsm_handle() */
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
					 * Session state, cleared by every teardown like its
					 * three siblings above: a re-config handing out a
					 * different one would otherwise be refused by the
					 * install guard, leaving the WAN data T-CONT dark
					 * until a reboot. */
/*
 * The wire GEM Port-ID the OLT assigned in its OMCI ME 268 (GEM Port Network
 * CTP) Create, attribute 1, set from the eth OMCI RX snoop, and what the
 * data-GEM install programs. It is the OLT's to choose, as the OMCC's GEM
 * Port-ID comes from Configure_Port-ID. GPON_DATA_GEM_DEFAULT is only the value
 * held before the OLT has spoken; the install is gated on
 * gpon_data_gem_solicited, so it never reaches the wire on a provisioned
 * session.
 */
static u16 gpon_data_gem_port = GPON_DATA_GEM_DEFAULT;
#define GPON_OMCC_TCONT_ALT	1	/* Alternative T-CONT for alloc 0x100 (OLT's tcont 1) */
static u16 gpon_data_alloc;		/* the OLT's data Alloc-ID, on T-CONT 8 */
static bool dying_gasp_reset;
module_param(dying_gasp_reset, bool, 0644);
MODULE_PARM_DESC(dying_gasp_reset, "Restart 100 ms after the dying gasp interrupt, as stock resets the chip");
static u32 luna_dying_gasps;
static bool early_tcont = true;
module_param(early_tcont, bool, 0644);
MODULE_PARM_DESC(early_tcont, "Answer an assigned Alloc-ID's grants before omcid has bound its T-CONT");
/* What the data T-CONTs answer, under luna_fsm_lock: every bound slot, and those
 * bound by Assign_Alloc-ID alone, which no OMCI T-CONT owns yet. */
static u16 luna_tcont_alloc[LUNA_XPON_TCONTS];
static u8 luna_tcont_used, luna_tcont_early;
static const bool data_tcont = true;	/* default ON: data US rides the DATA T-CONT 8's grants */

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

static u32 sw_rd(u32 off)
{
	return ioread32(swcore_base + off);
}

static void sw_wr(u32 off, u32 v)
{
	iowrite32(v, swcore_base + off);
}

static inline u32 pi_rd(u32 off) { return ioread32(ponip_base + off); }
static inline void pi_wr(u32 off, u32 v) { iowrite32(v, ponip_base + off); }

/* The three register blocks as flowcore `struct hwio`, so */
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

/* gpon_ind_poll()/gpon_ind_go() (flowcore/regtable.h) */
static void pi_pause_1us(void)
{
	udelay(1);
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


static u32 gpon_led_pon_val = ~0u;	/* last value written (forces first update) */
static u32 gpon_led_los_val = ~0u;

/*
 * The panel map is the board's, from its DT gpon node: realtek,pon-led,
 * realtek,los-led and realtek,port-leds = <index type>... (hardware
 * link/activity). A board that declares nothing gets no lamp driven, since
 * another board's indices can drive pins the panel does not use. What the lamps
 * say is the same on every ONU (below).
 */
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
		pr_err("luna-gpon: [SYS-003] realtek,port-leds holds %d cells, not <index type> pairs (max %zu) -- no port LED driven\n",
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

/*
 * One panel language on every ONU, so a field technician reads any unit alike:
 * PON green solid at O5 and blinking in every other state, no fibre included
 * (stock's own table turns it off at O1); LOS red blinking while the downstream
 * light is lost. Both blink at one rate: the 9603CVD has a single force-blink
 * period.
 */
static void gpon_led_pon_set(u8 st)
{
	u32 v = st == 5 ? LED_FORCE_ON : LED_FORCE_BLINK;

	if (v == gpon_led_pon_val)
		return;
	gpon_led_force(led_pon_idx, v);
	gpon_led_pon_val = v;
}

static void gpon_led_los_set(bool los)
{
	u32 v = los ? LED_FORCE_BLINK : LED_FORCE_OFF;

	if (v == gpon_led_los_val)
		return;
	gpon_led_force(led_los_idx, v);
	gpon_led_los_val = v;
}

/*
 * The standard interface: /sys/class/leds. This registers the two lamps the CPU
 * forces, so their brightness is true. The driver keeps driving them from GPON
 * state, and a userspace write is an override the next state change takes back.
 *
 * The link lamps (FE/GE) are deliberately not registered. They are
 * hardware-auto: the switch lights them from its own port link and activity and
 * CPU_FORCE_MOD stays 0, so their force field reads 0 while the lamp is lit.
 * Reporting them means reading the port's link state, which lives in the
 * Ethernet driver.
 */
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
	/*
	 * SWCORE, not the PON IP: pi_packed_get()'s pi_rd() addresses a
	 * different block.
	 */
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
			pr_warn("luna-gpon: [SYS-004] LED %s not exposed at /sys/class/leds (%d)\n",
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
 * power-on configuration. A die with no table, or a board that declares
 * neither PON/LOS lamps nor port LEDs, is an error, never a quiet panel. A
 * board whose PON/LOS lamps sit on GPIOs (V2802RH) declares port LEDs only. */
static void gpon_led_init(void)
{
	const struct luna_led_regs *r = swc->led;
	bool pon_los;
	int i;

	if (!r) {
		pr_err("luna-gpon: [SYS-005] panel LEDs not driven (%s: no LED-controller table for this die)\n",
		       swc->chip);
		return;
	}
	pon_los = gpon_led_valid(led_pon_idx) && gpon_led_valid(led_los_idx);
	if (!pon_los && !led_nports) {
		pr_err("luna-gpon: [SYS-006] panel LEDs not driven (%s: the DT gpon node declares no valid PON/LOS lamp and no port LED)\n",
		       swc->chip);
		return;
	}
	sw_wr(r->para_en, sw_rd(r->para_en) & ~r->seri_para);
	sw_wr(r->pad_en, sw_rd(r->pad_en) & ~r->seri_pad);
	sw_field(LED_MODE_SEL, 0, 0, 0);			/* parallel output   */
	sw_field(r->blink_rate, r->blink_lsb + 2, r->blink_lsb, LED_BLINK_512MS);
	if (pon_los) {
		gpon_led_claim(led_pon_idx);
		gpon_led_claim(led_los_idx);
		gpon_led_force(led_pon_idx, LED_FORCE_BLINK);	/* not yet O5 */
		gpon_led_force(led_los_idx, LED_FORCE_OFF);
		gpon_led_pon_val = LED_FORCE_BLINK;
		gpon_led_los_val = LED_FORCE_OFF;
	}

	/* Ethernet port-link LEDs, hardware-auto: the switch lights each from its
	 * OWN port's link + activity, no CPU. */
	for (i = 0; i < led_nports; i++)
		if (gpon_led_valid(led_ports[2 * i]))
			gpon_led_port(led_ports[2 * i], led_ports[2 * i + 1]);

	pr_debug("luna-gpon: panel LEDs init (%s: PON idx%u / LOS idx%u force-mode; %d port LED(s) link-auto)\n",
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

/* DEV live register poke */
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
	default:
		pr_info("luna-gpon: poke bad cmd '%c'\n", c);
		break;
	}
	return 0;
}
static const struct kernel_param_ops poke_ops = { .set = poke_set };
module_param_cb(poke, &poke_ops, poke_buf, 0644);
MODULE_PARM_DESC(poke, "DEV: <g|G|p|P> <hexoff> [hexval] live GTC/PON-IP reg read/write");

/* Read one 8-bit register from an I2C slave via the */
#define I2C_XACT_SETTLE_US	600u
/* The NACK is latched across the whole transaction. */
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

	/* Reconnect the shared optical-SD pad so optic_los stays live. */
	return ret;
}

/* ANI-G (ME 263) live optical */
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

/* One process worker owns the 50 ms RTL servo and separate 3 s DDM. */
static struct delayed_work gpon_optical_work;
static void gpon_optical_work_fn(struct work_struct *w);

/* Write one 8-bit register to an I2C slave via the SoC HW */
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

	/* Reconnect the shared optical-SD pad so optic_los stays live. */
	return ret;
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
	const struct firmware *fw, *type_fw = NULL;
	struct gn_op *ops;
	char type[5] = { 0 };
	u8 lan_type = 0;
	u8 notes[GN_PROBE_NOTES];
	struct gn_io io = { .rd = luna_gn_rd, .wr = luna_gn_wr,
		.notes = notes, .notes_max = ARRAY_SIZE(notes) };
	struct gn_fail fail = { 0 };
	int ret, n;

	if (bosa_cal_ready)
		return 0;
	ret = request_firmware_direct(&fw, "vsol/v2802rh-gn25l95.bin", NULL);
	if (ret)
		return ret;
	if (fw->size != GN_CAL_LEN) {
		ret = -EINVAL;
		goto release;
	}
	ret = request_firmware_direct(&type_fw, "vsol/v2802rh-gn25l95-lan-type", NULL);
	if (ret)
		goto release;
	if (!type_fw->size || type_fw->size >= sizeof(type)) {
		ret = -EINVAL;
		goto release;
	}
	memcpy(type, type_fw->data, type_fw->size);
	ret = kstrtou8(type, 10, &lan_type);
	if (ret)
		goto release;
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
	n = gn25l95_cal_ops(fw->data, fw->size, ops, GN_OPS_MAX);
	ret = n < 0 ? n : gn25l95_cal_apply(ops, n, &io, &fail);
	if (!ret) {
		bosa_cal_ready = true;
		pr_info("luna-gpon: GN25L95 calibrated (%d operations)\n", n);
	}
free:
	if (ret)
		pr_err("luna-gpon: [OPT-001] GN calibration refused: err=%d op=%u reg=0x%02x\n",
		       ret, fail.op, fail.reg);
	kfree(ops);
release:
	release_firmware(type_fw);
	release_firmware(fw);
	return ret;
}

/* One ANI-G self-test reading (the OLT's `show ont-optical`), in the units the
 * OMCI core takes, from the module's SFF-8472 A2h monitor. */
static void luna_selftest_read(struct gpon_optic_reading *v)
{
	struct gpon_ddm_a2h d;

	memset(v, 0, sizeof(*v));
	if (bosa_a2h_read(&d) == GPON_DDM_OK)
		gpon_optic_from_a2h(v, &d);
}

/* One per-rate/lane analog bank of the table below */
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

/* One FIB (fiber optical front-end) bank */
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

/* Full SerDes analog + WSDS configuration */
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

/* Bring up the PON SerDes so the GPON MAC core gets its */
static int gpon_serdes_init(void)	/* not __init: re-run on re-range from gpon_cdr_reset_worker */
{
	int i;

	/* 1. Park CFG_SDS_MODE at the illegal/off value (0x1f) */
	sw_field(SDS_CFG, 4, 0, SDS_MODE_OFF);
	sw_wr(WSDS_DIG_01, 0);				/* clear force-SDS dummy   */
	sw_field(WSDS_DIG_00, 0, 0, 0);			/* STOP_CLK = 0            */

	/* 2. Program the FULL analog block to the operational */
	for (i = 0; i < ARRAY_SIZE(sds_analog_golden); i++)
		sw_wr(SDS(sds_analog_golden[i].off), sds_analog_golden[i].val);
	for (i = 0; i < ARRAY_SIZE(fib_reg0_banks); i++)
		sw_wr(SDS(fib_reg0_banks[i]),
		      sw_rd(SDS(fib_reg0_banks[i])) & ~FIB_REG0_PDOWN);

	/* 3. Pulse the SDS reset to latch the analog config. */
	/* legacy CMD_SDS_CFG_RST_PS */
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
	/* Re-clear fiber power-down too. */
	for (i = 0; i < ARRAY_SIZE(fib_reg0_banks); i++)
		sw_wr(SDS(fib_reg0_banks[i]),
		      sw_rd(SDS(fib_reg0_banks[i])) & ~FIB_REG0_PDOWN);

	/* 5. Burst-enable output; leave optical-LOS un-forced so the real RX front-
	 *    end drives it (a working unit reaches O5 with FRC_OPTIC_LOS=0). */
	sw_field(WSDS_DIG_18, 12, 12, 1);		/* BEN_OE = 1              */
	sw_field(WSDS_DIG_18, 15, 15, 0);		/* OPTIC_LOS_SEL_EPON = 0  */
	/* Do NOT force optic_los. */
	sw_field(WSDS_DIG_18, 14, 14, 0);		/* CFG_FRC_OPTIC_LOS = 0   */
	sw_field(WSDS_DIG_18, 13, 13, 0);		/* CFG_FRCV_OPTIC_LOS = 0  */

	/* 6. Arm the RX in the required order */
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
	/* FORCE_BEN (SDS 0x220e4) BEN_FORCE_MODE[0]=0 */
	sw_field(SDS(0x220e4), 0, 0, 0x0);

	/* 6a-ModeV1. US-TX SerDes CMU/PLL and TX-LA-LDO */

	/* 6b. TX DATA PATH */
	sw_field(SDS(0x220a8), 5, 4, 0x0);	/* WSDS_DIG_1E D2A interconnect (stock=0) */
	sw_field(SDS(0x2281c), 14, 14, 0x0);	/* SDS_REG7 SP_CFG_NEG_CLKWR_A2D (stock=0) */
	sw_field(SDS(0x22a30), 8, 8, 0x0);	/* SDS_EXT_REG12 SEP_CFG_NEG_CLKRD_D2A (stock=0) */

	/* TX drive level */
	sw_field(SDS(0x225d8), 5, 3, 0x5);			/* SDS_ANA_COM_REG22 REG_TX_AMP = 0x5 */
	sw_field(SDS(0x225d8), 2, 0, 0x1);			/* SDS_ANA_COM_REG22 REG_TX_EMP = 0x1 */

	/* 7a. Force signal-detect on. */
	sw_field(SDS_ANA_MISC_REG02, 13, 13, 0x1);	/* signal-detect value=1   */
	sw_field(SDS_ANA_MISC_REG02, 12, 12, 0x1);	/* force signal-detect     */
	mdelay(10);

	/* 7b. Finally select GPON mode — the very last step, with the RX fully armed
	 *     (CFG_SDS_MODE switches to GPON only here). */
	sw_field(SDS_CFG, 4, 0, SDS_MODE_GPON);
	mdelay(50);

	/* TX-interface reset-B re-sync */
	sw_field(WSDS_DIG_1D, 16, 16, 0);
	mdelay(2);
	sw_field(WSDS_DIG_1D, 16, 16, 1);
	mdelay(10);

	/* SerDes CDR-lock pulse */
	{
		u32 cdr = sw_rd(SDS_ANA_COM_REG12);

		sw_wr(SDS_ANA_COM_REG12, cdr ^ BIT(15));
		mdelay(10);
		sw_wr(SDS_ANA_COM_REG12, cdr);
		/* Print the address that was written. */
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

static const unsigned int table_engine_ok;	/* 0: the Ethernet driver's VLAN setup owns the VLAN table */

static const unsigned int relatch_us = 1;
static const unsigned int serdes_recommit;	/* 0 = off: a full SDS re-commit (CMD_SDS_RST_PS) at O5 drops the locked DS framer (omcirx->0, DS dead) */

/* Bind the OMCC Alloc-ID to a SECOND T-CONT as well. */
static const bool omcc_alt_bind;
/* Re-arm the GEM-US US-feed run-state with the FULL */
static const bool o3_feed_reset;


/* swcore TBL_ACCESS engine (L2/VLAN tables) */
#define TBL_BUSY_BIT	(1u << 13)
/* The bound, named the way LUNA_SMI_TRIES and L3FE_ACCESS_TRIES already are
 * in this tree, because it is now an ARGUMENT to the shared poll and not a
 * number buried in a loop.  Why 2000 and not 0x4000: see tbl_wait(). */
#define TBL_WAIT_TRIES	2000u

static bool tbl_ok = true;

/* The SWCORE table engine's pace for gpon_ind_poll(). */
static void tbl_pause(void)
{
	static unsigned int spins;

	udelay(1);
	if ((++spins & 0xff) == 0)
		cond_resched();
}

static void tbl_wait(void)
{
	/* The budget is 2 ms, not 16 ms, and it cost this board */
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
	/*
	 * This is 0xb800063c, not SOC_IP_SEL at 0xb8000600; the two gate
	 * different things.
	 */
	void __iomem *sw_en = SOC_SW_ENABLE;
	int port, idx;

	/* This is EXPORTED and the caller is another driver, so */
	if (!swcore_base) {
		pr_warn_once("luna-gpon: [SYS-007] datapath_tables_init called before this driver probed (swcore_base is NULL) -- skipped; the switch fabric is NOT initialised\n");
		return;
	}


	/* 1) switch_init -------------------------------------------------- */
	writel(readl(sw_en) | SW_EN_BIT, sw_en);	/* see the naming note in
							 * luna_eth_regs.h    */
	pi_field(PI_PON_TB_CTRL, 7, 0, 0x6e);			/* PON_TB_CTRL tick      */
	pi_field(PI_PON_TB_CTRL, 15, 8, 0x95);
	/*
	 * METER_TB_CTRL and PON_TB_CTRL have mirrored layouts: PON_TB_CTRL puts
	 * TICK_PERIOD at [7:0] and TKN at [15:8], METER_TB_CTRL puts TKN at
	 * [7:0] and TICK_PERIOD at [15:8] (the chipdef's own layout,
	 * structurally identical on 9607C/9601B/9603CVD). Stock refills 189
	 * tokens every 43 ticks and reads 0x00012bbd, on a meter both firmwares
	 * leave enabled (METER_OP=1). rtl9602c_eth.c writes the same word
	 * earlier, and this site runs later.
	 */
	sw_field(SW_METER_TB_CTRL, 7, 0, 189);			/* TKN         */
	sw_field(SW_METER_TB_CTRL, 15, 8, 43);			/* TICK_PERIOD */
	sw_field(SW_SCH_WFQ_TKN_CTRL, 0, 0, 1);		/* SCH_WFQ_TKN_CTRL      */
	sw_field(SW_LINE_RATE_2500M, 18, 0, 0x3ffff);	/* LINE_RATE_2500M       */
	sw_field(WRAP_GPHY_MISC, 0, 0, 1);			/* PATCH_PHY_DONE        */
	sw_field(CFG_UNHIOL, 0, 0, 1);			/* CFG_UNHIOL IPG_COMP   */
	sw_field(SW_P_MISC_PORT(swc->sw, swc->sw->cpu_port), 2, 2, 1);	/* P_MISC[CPU] RX_SPC */
	/* The ACCEPT_MAX_LEN half is closed, and only on the die */
	for (port = 0; port <= swc->amax_last_port; port++)
		sw_field(ACCEPT_MAX_LEN_CTRL + port * 4, swc->amax_msb, 0,
			 (swc->amax_pon_val && port == swc->sw->pon_port)
			 ? swc->amax_pon_val : swc->amax_val); /* ACCEPT_MAX_LEN */
	/* the PON port when it lies BEYOND this die's covered range -- the
	 * RTL9607C's is port 5 and its loop stops at 3. */
	if (swc->amax_pon_val && swc->sw->pon_port > swc->amax_last_port)
		sw_field(ACCEPT_MAX_LEN_CTRL + swc->sw->pon_port * 4,
			 swc->amax_msb, 0, swc->amax_pon_val);

	/* 2) l2_init: per-port action defaults (FORWARD). NOTE */
	sw_field(SW_LUT_CFG, 22, 22, 1);			/* LUT LINKDOWN_AGEOUT   */
	/*
	 * The binary CAM. sw_field() is a read-modify-write that names one bit,
	 * so every bit this init never mentions keeps whatever reset or U-Boot
	 * left, and BCAM_DIS arrives set. Stock clears it: bit 21 is BCAM_DIS
	 * in this die's own chipdef, and LUT_CFG reads 14800bb8 on stock. A
	 * disabled search CAM stops frames being looked up at all.
	 */
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

	/*
	 * 3) vlan_init: default VLAN-1 (all ports member). Only enable
	 * filtering if the member entry actually wrote, else keep VLAN off.
	 * Risky table-engine path: gated by table_engine_ok.
	 */
	for (port = 0; port <= 3; port++) {
		sw_field(SW_VLAN_PORT_ACCEPT_FRAME_TYPE, port * 2 + 1, port * 2, 0);	/* ACCEPT ALL    */
		sw_field(SW_VLAN_EGRESS_TAG + 4 * port, 1, 0, 0);		/* EGRESS ORIG   */
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

	/* 7) classification setup */
	sw_wr(CF_CFG, 0x0001d009u);
	/* Port isolation + BUM-flood masks to the live-stock */
	sw_wr(SW_PISO_PORT + 0 * SW_PISO_PORT_STRIDE, 0x000ff9ffu);
	sw_wr(SW_PISO_PORT + 1 * SW_PISO_PORT_STRIDE, 0x000ff9ffu);
	sw_wr(SW_PISO_PORT + 2 * SW_PISO_PORT_STRIDE, 0x000ff9ffu);
	sw_wr(SW_PISO_PORT + 3 * SW_PISO_PORT_STRIDE, 0x000ff9ffu);
	sw_wr(LUT_BC_FLOOD, 0x00000008u); sw_wr(LUT_UNKN_MC_FLOOD, 0x00000008u); sw_wr(LUT_UNKN_UC_FLOOD, 0x00000008u);

	/* 8) ponmac_init: PON-IP scheduler + OMCI egress steering */
	sw_field(DYNGASP_CTRL, 3, 3, 1);			/* DYNGASP_CMP_INV, bit 3 on all three dies */
	/* Match live-stock post-reset SDS_ANA */
	sw_field(SDS(0x22584), 14, 14, 1);		/* REG01 REG_BEN_TTL_OUT = 1 (stock 0x73a4) */
	sw_field(SDS(0x22584),  0,  0, 0);		/* REG01 BENLA_LDOVREF[0] = 0 (stock) */
	sw_field(SDS(0x225ac),  7,  0, 0);		/* REG11 RX_FILT_CONFIG = 0 (stock)   */
	pi_field(PI_PON_BW_THRES, 29, 16, 5);			/* PON_BW_THRES last     */
	pi_field(PI_PON_BW_THRES, 13, 0, 5);			/* PON_BW_THRES runt     */
	pi_wr(PI_PON_SCH_CTRL, 0x00066000u);	/* legacy bit18-only value */
	for (idx = 0; idx < 8; idx++) {
		pi_packed_set(PI_PON_WFQ_TYPE, idx, 1, 0);	/* WFQ_TYPE = STRICT (1b packed) */
		pi_field(PI_PON_QID_CIR_RATE + idx * PI_QID_RATE_STRIDE, 31, 0, 0);	/* QID_CIR_RATE = 0      */
	}
	sw_field(PON_TRAP_CFG, 2, 0, 7);			/* PON_TRAP_CFG OMCI_MPCP_PRIORITY=7 -> steer OMCI egress to PON queue 7 */
	/* PIR_DROP (0x2194 bit18) is cleared for rev-A */
	sw_field(SW_P_MISC_PORT(swc->sw, swc->sw->pon_port), 2, 2, 1);	/* P_MISC[PON] RX_SPC */
	sw_field(SW_P_MISC_PORT(swc->sw, swc->sw->cpu_port), 2, 2, 1);	/* P_MISC[CPU] RX_SPC */

	pr_debug("luna-gpon: datapath_tables_init done (tbl_ok=%d)\n", tbl_ok);
}
EXPORT_SYMBOL(rtl9602c_datapath_tables_init);

/* Configure the PON packet datapath (PON-IP) for GPON */
/*
 * The US and DS packet pools may be reserved by the board (DT memory-region, US
 * then DS) instead of allocated: the 32 MB X100DG's stock keeps both at the two
 * holes of its memory map (MSTBASE_US 0x01eff000, MSTBASE_DS 0x016ff000), and
 * taking 1-2 MB from its 19 MB starved it.
 */
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
			pr_err("luna-gpon: [SYS-008] memory-region %d is not a %lu-byte pool -- allocating instead\n",
			       i, PAGE_SIZE << PI_US_DRAM_ORDER);
		of_node_put(rn);
	}
	if (pbo_region[0] || pbo_region[1])
		pr_debug("luna-gpon: PBO pools from DT: US 0x%08x DS 0x%08x\n",
			pbo_region[0], pbo_region[1]);
}

void gpon_pbo_init(void)
{
	bool keep_pool = READ_ONCE(luna_activation_ready) &&
		luna_ploam.state == GPON_O5_OPERATION;

	/*
	 * Exported like datapath_tables_init, and reached from the NIC's open
	 * on a board whose DT disables the GPON, where ponip_base is NULL.
	 */
	if (!ponip_base) {
		pr_warn_once("luna-gpon: [QOS-001] gpon_pbo_init called before this driver probed (ponip_base is NULL) -- skipped\n");
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

		/* 2. Descriptor accounting (128B pages). */
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
				pr_warn("luna-gpon: [QOS-002] US PBO DRAM pool alloc failed; US-NIC RX may not work\n");
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
				/*
				 * No pool: keep the small SRAM geometry.
				 * Writing the DRAM-scale page counts with no
				 * pool behind them would point the PBO at
				 * physical address 0.
				 */
				pr_warn("luna-gpon: [QOS-003] DS PBO DRAM pool alloc failed; DS stays SRAM-only and DS OMCI may be dropped\n");
				pi_field(PI_PON_DSC_CFG_DS, 12, 0, PI_DS_SRAM_NO);
				pi_field(PI_PON_DSC_CFG_DS, 28, 16, PI_DS_SRAM_NO);
				pi_field(PI_DSCRUNOUT_DS, 12, 0, PI_DS_SRAM_RUNOUT);
				pi_field(PI_DSCRUNOUT_DS, 28, 16, 0);
			}
		}
	}

	/* PBO backpressure thresholds (SRAM-only, 128B pages). */
	pi_field(PI_PON_SID_STOP_TH, 12, 0, 0x1f30);
	pi_field(PI_PON_SID_GLB_TH, 28, 16, 0x1ee0);
	pi_field(PI_PON_SID_GLB_TH, 12, 0, 0x1e40);
	/*
	 * Per-SID reserved-page thresholds, 150/130: the vendor's own values
	 * for this init (rtl9603cvd_raw_pbo_memUsage_init). The bound is the
	 * chip's array, not the chip's SID count: see sid_rpv_entries. A chip
	 * that declares none (no PON-IP block) writes none.
	 */
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

	/* 5. PONNIC datapath */
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
	/* CFG_US[6:0] = RX_SID for the US-NIC, symmetric to the */
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

	/* 7. Enable upstream and downstream packet buffers. */
	if (!keep_pool) {
		pi_field(PI_PONIP_CTL_US, 0, 0, READ_ONCE(luna_activation_ready));
		pi_field(PI_PONIP_CTL_DS, 0, 0, 1);
	}
	pi_field(PI_PONIP_CTL_DS, 7, 7, 1);		/* CFG_TX_PAUSE low bit -> O5 value 0x81 (DS buffer release; safe now thresholds bound the buffer) */

	/* 8. Enable the full downstream PONNIC DMA drain */
	pi_wr(PI_MEDIA_STS_DS, 0x106e8400u);
	pi_wr(PI_IO_CMD_0_DS, 0x90081070u);

	/* PON-IP DS-NIC drain config. */
	pi_wr(PI_PROBE_SELECT_DS, 0x00000040u);
	pi_wr(PI_DS_NIC_CFG_D404, 0x11100348u);
	pi_wr(PI_CONFIG_CLK_DS, 0x00000040u);

	/* 9. US-NIC GMII enable */
	pi_wr(PI_MEDIA_STS_US, 0x106e8400u);	/* stock NEVER writes 0x1bf04058 */

	/* Pre-arm the OMCC classification BEFORE the GMII_RX_EN */
	{
		unsigned int f = GPON_OMCC_FLOW;
		unsigned int q2_lsb = (f % 4u) * 7u;	/* SID2QID: 7 bits, 4/word  */
		unsigned int v_bit  = f % 32u;		/* SIDVALID: 1 bit, 32/word */

		pi_field(PI_PON_SID2QID + (f / 4u) * PI_SID2QID_STRIDE,
			 q2_lsb + 6u, q2_lsb, GPON_OMCC_PHYS_QID & 0x7f);
		pi_field(PI_PON_SIDVALID + (f / 32u) * 4u, v_bit, v_bit, 1);
	}
	/* PON_OMCI_CFG[6:0] = OMCC SID 64 */
	pi_field(PI_PON_OMCI_CFG, 6, 0, GPON_OMCC_FLOW);	/* [6:0] = this chip's OMCC SID */

	/* Data remains inhibited after a global NIC/PBO reset. The accepted OMCI
	 * owner retires any old queue and applies its selected qid on the timer. */
	pi_field(PI_PON_SIDVALID, GPON_DATA_FLOW, GPON_DATA_FLOW, 0);

	/* MOCIR force-mode (stock QoS init) */
	pi_wr(PI_MOCIR_FRC_MD, 0x0001ffffu);	/* MOCIR_FRC_MD  = 0x1FFFF (all flows forced)   */
	pi_wr(PI_MOCIR_FRC_VAL, 0x0001ffffu);	/* MOCIR_FRC_VAL = 0x1FFFF (forced CIR = max)   */

	/* Stock PON-MAC US-NIC datapath credit/threshold */
	pi_wr(PI_PON_BW_THRES, 0x00050005u);	/* PON US-NIC IP-status/BW threshold (reg924, 5+5)  */
	pi_wr(PI_PON_US_FIFO_CTL, 0x00000013u);	/* US-NIC datapath cfg                              */
	/* PONIP_DBG_CTRL_US MUST be 0x00086000 to match live stock */
	pi_wr(PI_PONIP_DBG_CTRL_US, 0x00086000u);	/* stock value (DBG_IGNORE_TAG=1) */
	pi_wr(PI_GPON_DPRU_RPT_PRD, 0x00003002u);	/* DPRU_RPT_PRD: DBA_BLKSIZE=48 plus report period 2,
									 * the live-stock 0x3002. The HW DBA/DBRu engine uses
									 * this byte->block divisor to encode per-SID queued
									 * occupancy into the report the OLT reads to size
									 * grants; unset, the OLT sees 0 queued while the
									 * queue holds pages, grants once and stops. Stock's
									 * only writer is its init. */
	/*
	 * 0x20f4 = PON_IPSTS_US, a read-only init-ready status reg
	 * (bit0=PONIC_INITRDY); stock never writes it. It is polled before the
	 * GMII latch edge below.
	 */
	pi_wr(PI_MOCIR_TH_H, 0x00001000u);	/* MOCIR_TH_H (request/grant credit threshold)      */
	pi_wr(PI_MOCIR_TH_L, 0x00001000u);	/* MOCIR_TH_L                                       */
	pi_wr(PI_PON_OLT_BW_MTR_FULL, 0x0003ffffu);	/* PON_OLT_BW_MTR_FULL (maxFlow)                    */
	pi_wr(PI_PON_TB_CTRL, 0x0000956eu);	/* PON_TB_CTRL (token bucket)                       */
	if (!keep_pool) {
		pi_wr(PI_PON_SCH_QMAP, 0x0000000fu);
		pi_wr(PI_PON_TCONT_EN, 0x00010001u);
	}

	/*
	 * GMAC0<->US-NIC internal link force, from the stock PON-MAC GPON
	 * mode-set. Without it a CPU direct-TX US-OMCI frame never reaches the
	 * US-NIC ingress. Stock releases the force once trained; this driver
	 * holds it up.
	 */
	/* ABLTY_FORCE_MODE[4:0]=0xc — GPON internal-link force. This word is
	 * port 0's; a board that already forces port 0 whole (V2802RH, its
	 * SerDes LAN, stock 0xfff) keeps it, or the port loses its forced link. */
	if ((sw_rd(ABLTY_FORCE_MODE) & 0x1f) != 0x1f)
		sw_field(ABLTY_FORCE_MODE, 4, 0, 0xc);
	sw_field(0x000f4, 5, 5, 1);	/* CFG_FE_POLL_WD_1[5]=1 — front-end GMII poll/watchdog */
	mdelay(10);			/* settle the forced internal link before PCS enables+edge */
	sw_field(SDS(0x22a70), 11, 11, 0);	/* SDS_EXT_REG28[11]=0 — release non-GPON SerDes-select    */
	/*
	 * The two WAN-PCS digital-enable writes below diverge from stock on
	 * purpose: guarded off, the G24W measured optic_los=1 sdet=0 and stayed
	 * at O1. A stock-parity sweep must not close this gap.
	 */
	sw_field(SDS(0x220e0), 9, 8, 1);	/* WSDS_DIG_2C[9:8]=1 — WAN-PCS digital enable */
	pi_field(PI_RSVD_PONIP_DS, 13, 13, 1);	/* RSVD_PONIP_DS[13]=1 — DS-side datapath enable           */
	pi_field(PI_RSVD_PONIP_DS, 12, 12, 1);	/* RSVD_PONIP_DS[12]=1                                     */
	sw_field(SDS(0x22080), 12, 12, 1);	/* WSDS_DIG_14[12]=1 — WAN-PCS digital enable (see above) */

	/* Wait for the US PON-IP core to report init-ready before the GMII latch edge
	 * below. Stock has no such poll and relies on a fixed mdelay. */
	{
		int n;

		for (n = 0; n < SDS_LOCK_POLL_MAX; n++) {
			if (pi_rd(PI_PON_IPSTS_US) & BIT(0))		/* PONIC_INITRDY */
				break;
			udelay(200);
		}
		if (n >= SDS_LOCK_POLL_MAX) {
			pr_debug("luna-gpon: PON_IPSTS_US.PONIC_INITRDY not set after %dus; proceeding\n",
				SDS_LOCK_POLL_MAX * 200);
		} else {
			pr_debug("luna-gpon: PON_IPSTS_US.PONIC_INITRDY ready after %dus\n", n * 200);
		}
	
	}

	pi_wr(PI_IO_CMD_0_US, 0x90101070u);
}
EXPORT_SYMBOL(gpon_pbo_init);	/* re-run from rtl9602c_eth_open() after the GMAC reset */



static u32 gpon_us_feed_rearm_cnt;	/* count of US-feed re-arms (pre-FSM + O5), shown in /proc/gpon */
static u32 gpon_us_intr_svc_cnt;	/* count of US-intr delta reads that found a latched event */

/* Faithful re-express of the working firmware's GPON */
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

/* Softirq-safe US-feed FIFO re-arm */
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

/* The O3 post-relock feed un-park, in ONE place. */
static void gpon_o3_feed_unpark(void)
{
	sw_field(WSDS_DIG_00, 10, 10, 0);
	sw_field(WSDS_DIG_00, 10, 10, 1);
	gpon_us_feed_rearm_light();
	luna_log("O3 post-relock WSDS feed-reset edge (un-park GEM-US framer)\n");
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

static int swdump_proc_show(struct seq_file *s, void *v)
{
	static const u32 sw[][2] = {		/* offsets into swcore_base (phys 0x1b000000) */
		{0x00000, 0x000fc},
		/* 0x00100..0x002fc */
		{0x00100, 0x002fc},
		{0x1c000, 0x1c0fc}, {0x20800, 0x2083c},
		{0x20c00, 0x20c3c}, {0x23000, 0x230fc}, {0x27000, 0x2703c},
		/*
		 * Registers this driver writes and could not otherwise read
		 * back
		 */
		{0x11000, 0x1104c},	/* ACCEPT_MAX_LEN_CTRL, every die + its ports  */
		{0x110e0, 0x111fc},	/* PON_TRAP_CFG: 0x110ec / 0x111f8 / 0x11144   */
		{0x12000, 0x1200c},	/* TBL_CTRL_OFF / TBL_WRDATA_OFF               */
		{0x16000, 0x1601c},	/* CF_CFG                                      */
		{0x1c100, 0x1c2fc},	/* RMA_CFG, QOS_UNI_TRAP_PRI_CTRL, OAM_CTRL_0  */
		{0x1e000, 0x1e05c},	/* LED_MODE_SEL .. LED_PARA_EN + LED_DATA_CFG  */
		{0x23100, 0x2311c},	/* CFG_UNHIOL on the 9607C                     */
		{0x2d890, 0x2d8bc},	/* SW_SCH_WFQ_TKN_CTRL, SW_LINE_RATE_2500M     */
		/* SW_P_MISC_PORT is base 0x20004 + port * macpp_stride */
		{0x13000, 0x1300c},	/* VLAN_PORT_ACCEPT_FRAME_TYPE/INGRESS/CTRL   */
		{0x17000, 0x1700c},	/* SW_LUT_CFG, SW_LUT_AGEOUT_CTRL             */
		{0x20400, 0x20430},	/* P_MISC[4] -- the RTL9603CVD's PON          */
		{0x20500, 0x20530},	/* P_MISC[5] -- its CPU, and the 9607C's PON  */
		{0x20900, 0x20930},	/* P_MISC[9] -- the RTL9607C's CPU            */
		{0x25000, 0x2500c},	/* SW_METER_TB_CTRL                           */
		{0x2a000, 0x2a024},	/* SW_VLAN_EGRESS_TAG + 4*port, ports 0..9    */
		/* The RTL9603CVD PON SerDes page */
	};
	/* The GPON MAC / GTC block, through its OWN accessor. */
	static const u32 gt[][2] = {		/* offsets into gpon_base (phys 0x1b700000) */
		{0x010a0, 0x010c4}, {0x01180, 0x011c8},
		{0x01400, 0x015fc}, {0x02400, 0x027fc},
		{0x04280, 0x042a8}, {0x05080, 0x050ac},
		{0x050e0, 0x05100}, {0x06400, 0x065fc},
		/* swdump_readback_guard.py scanned only sw_field/sw_wr */
		{0x0000c, 0x00014}, {0x01004, 0x01080}, {0x01200, 0x01208},
		{0x0200c, 0x0200c}, {0x03010, 0x03024}, {0x04064, 0x04098},
		{0x05010, 0x05054}, {0x050c0, 0x050c0}, {0x05140, 0x05140},
		{0x05200, 0x05200}, {0x0526c, 0x0526c}, {0x06020, 0x06024},
		{0x06260, 0x06260},
	};
	static const u32 gm[][2] = {		/* absolute phys (separate ioremap) */
		{0x18012000, 0x180120fc}, {0x18013400, 0x180134fc},
		/*
		 * Both pages are kept: which model a die carries is a die fact.
		 * On the RTL9603CVD the NAPT control page 0x800000 reads all
		 * zero while the FLOWBASED engine at 0x801000 is live; on the
		 * NAPT dies the other page is the live one.
		 */
		{0x1b800000, 0x1b801ffc},	/* the flow/connection tables	*/
		{0x18000040, 0x1800005c},	/* SOC_PINMUX at 0x4c		*/
	};
	u32 off, a, val;
	int r;

	/* The dump declares its own COVERAGE first. */
	for (r = 0; r < (int)ARRAY_SIZE(sw); r++)
		seq_printf(s, "#cover 0x%08x 0x%08x\n",
			   0x1b000000u + sw[r][0], 0x1b000000u + sw[r][1]);
	for (r = 0; r < (int)ARRAY_SIZE(gm); r++)
		seq_printf(s, "#cover 0x%08x 0x%08x\n", gm[r][0], gm[r][1]);
	for (r = 0; r < (int)ARRAY_SIZE(gt); r++)
		seq_printf(s, "#cover 0x%08x 0x%08x\n",
			   0x1b700000u + gt[r][0], 0x1b700000u + gt[r][1]);
	seq_printf(s, "#cover 0x%08x 0x%08x\n",
		   0x1b000000u + SDS(0x22000), 0x1b000000u + SDS(0x22ffc));

	/* The SerDes page is per chip, so its range must be DERIVED. */
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
	for (r = 0; r < (int)ARRAY_SIZE(gm); r++) {
		void __iomem *b;

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

/* GEM Port-IDs the OLT marked encrypted with Encrypted_Port-ID. Nokia
 * encrypts the OMCC as well as the data GEMs, as stock's flows show. */
static DECLARE_BITMAP(gpon_aes_ports, 4096);

static void gpon_ds_traffic_set(u32 flow, u16 gem, u32 type)
{
	if (test_bit(gem & 0xfff, gpon_aes_ports))
		type |= DS_TRAFFIC_AES;
	gpon_wr(GPON_GTC_DS_TRAFFIC_CFG + flow * DS_TRAFFIC_CFG_STRIDE, type);
}

/* Per-flow downstream GEM Ethernet RX packet */
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

/* Invalidate non-management DS GEM-port CAM entries */
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

/* GTC US MISC PM counter */
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

/* Read back the GTC alloc CAM entry for a T-CONT */
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
	luna_tcont_used = 0;
	luna_tcont_early = 0;
	return 0;
}

/* DS-pipeline stage A */
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

/* GEM DS MISC PM counter (clear-on-read), global */
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

/* Read back the DS GEM-port CAM entry for `flow`: does the */
static int gpon_ds_cam_read(u8 flow)
{
	u16 gem;
	bool hit;
	int rc = luna_cam_xact(false, GPON_GTC_CAM_OP_READ, flow, &gem, &hit);

	if (rc < 0)
		return rc;
	return (hit ? BIT(16) : 0) | gem;
}

/* Per-flow downstream GEM forwarded-to-PON-IP count */
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

/* The first of the flows luna_xpon_hw_gem_install() hands omcid's GEMs. */
#define LUNA_XPON_FLOW_BASE	4

/* PONIP_DBG_CTRL_US strobe -- ONE spelling of a sequence this */
static int pi_sid_page_cnt(u32 sid, u32 *cnt)
{
	int rc = gpon_ind_go(&pi_io, reg_make(0x255c),
			     0x00086000u | (sid & 0x7fu) | (1u << 7),
			     1u << 9, 2000, pi_pause_1us);

	*cnt = pi_rd(0x2564);
	return rc;
}

static int v2802rh_gpon_show_locked(struct seq_file *s)
{
	u32 rst    = gpon_rd(GPON_RESET);
	u32 status = gpon_rd(GPON_GTC_DS_ONU_ID_STATUS);
	u32 eqd    = gpon_rd(GPON_GTC_US_EQD);
	u32 state  = status & GPON_ONU_STATE_MASK;

	luna_omci_seq_show(s);
	seq_puts(s, OMCI_ID_FIELDS_LINE "\n");
	seq_printf(s, "optics: GN25L95 identified=%u gn_cal_ready=%u upstream_ready=%u error=%d\n",
		   bosa_gn_identified, bosa_cal_ready, luna_activation_ready, bosa_cal_error);
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
	/* fiber: one-glance fibre-pull / optical recovery verdict. */
	{
		u32 los = gpon_rd(GPON_GTC_DS_LOS_CFG_STS);
		struct gpon_ddm_a2h d;
		s32 rx_cdbm = bosa_a2h_read(&d) == GPON_DDM_OK && d.rx_pwr ?
			      gpon_ddm_uw10_to_cdbm(d.rx_pwr) : S32_MIN;
		char rx_s[16], sdet_s[8];

		/*
		 * Neither field may print a number it did not measure: rx would
		 * be a floor constant on a failed I2C read, and sdet a read of
		 * whatever the 9602C's SDS_FIB_STATUS offset is on this chip.
		 * Three states each: value / n/a.
		 */
		if (rx_cdbm == S32_MIN)
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

		/* cdr_los is only a status while its monitor is on. */
		seq_printf(s, "los_cfg_sts: 0x%08x (optic_los=%d cdr_los=%s en=%d polar=%d)\n",
			   los, !!(los & GPON_OPTIC_LOS_SIG),
			   (los & GPON_CDR_LOS_EN)
				? ((los & GPON_CDR_LOS_SIG) ? "1" : "0")
				: "disabled",
			   !!(los & GPON_OPTIC_LOS_EN),
			   !!(los & GPON_OPTIC_LOS_POLAR));
	}

	/* PLOAM channel view, read-only. With no downstream PLOAM and */
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
	/* DS_MISC counters (GTC-relative, register-map base */
	{
		/*
		 * This word is two 16-bit counters, the SN requests and the
		 * ranging requests the GTC received (chipdef field names), and
		 * the split decides an activation fork.
		 */
		u32 act = gpon_rd(GPON_GTC_DS_MISC_CNTR_ACTIVE);

		seq_printf(s, "ds_cntr: ploam_acpt=%u ploam_fail=%u bwm_acpt=%u bwm_fail=%u bwm_inv=%u sn_req=%u rng_req=%u (all clear-on-read)\n",
			   gpon_rd(GPON_GTC_DS_MISC_CNTR_PLOAM_ACPT), gpon_rd(GPON_GTC_DS_MISC_CNTR_PLOAM_FAIL),
			   gpon_bwm_acpt_since(&gpon_bwm_acpt_ds_cntr_last),
			   gpon_rd(GPON_GTC_DS_MISC_CNTR_BWM_FAIL), gpon_rd(GPON_GTC_DS_MISC_CNTR_BWM_INV),
			   GPON_GTC_DS_ACTIVE_SN_REQ(act), GPON_GTC_DS_ACTIVE_RNG_REQ(act));
	}
	/* The flow is the chip's, and the label prints it. */
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
		/* Read-back the DS-engine enables (a later reset may have */
		seq_printf(s, "ds_en: ctl_ds(0xa0ac)=0x%08x io0_ds(0xd434)=0x%08x io1_ds(0xd438)=0x%08x\n",
			   pi_rd(PI_PONIP_CTL_DS), pi_rd(PI_IO_CMD_0_DS), pi_rd(PI_IO_CMD_1_DS));
		seq_printf(s, "ds_nic: cfg_ds(0xc04c)=0x%08x[RX_SID=%u] rxcfg_ds(0xc044)=0x%08x media_ds(0xc058)=0x%08x rxfdp_ds(0xd3f0)=0x%08x\n",
			   pi_rd(PI_CFG_DS), pi_rd(PI_CFG_DS) & 0x7f, pi_rd(PI_RX_CFG_DS),
			   pi_rd(PI_MEDIA_STS_DS), pi_rd(PI_RXFDP1_DS));
		/*
		 * US-NIC. Every address printed here is the one actually read.
		 */
		seq_printf(s, "us_nic: cfg_us(0x%05x)=0x%08x[RX_SID=%u] ctl_us(0x%05x)=0x%08x io0_us(0x%05x)=0x%08x io1_us(0x%05x)=0x%08x rxfdp_us(0x%05x)=0x%08x cnt_mask_us[omcc](0x%05x)=0x%08x\n",
			   PI_CFG_US, pi_rd(PI_CFG_US), pi_rd(PI_CFG_US) & 0x7f,
			   PI_PONIP_CTL_US, pi_rd(PI_PONIP_CTL_US),
			   PI_IO_CMD_0_US, pi_rd(PI_IO_CMD_0_US),
			   PI_IO_CMD_1_US, pi_rd(PI_IO_CMD_1_US),
			   PI_RXFDP1_US, pi_rd(PI_RXFDP1_US),
			   PI_CNT_MASK_US_WORD(0, GPON_OMCC_FLOW),
			   pi_rd(PI_CNT_MASK_US_WORD(0, GPON_OMCC_FLOW)));
		/* US-NIC per-group RX SID counters: good>0 confirms a */
		seq_printf(s, "us_rxsid[grp0..4]: good=%u/%u/%u/%u/%u bad=%u  (enrolled group is 0)\n",
			   pi_rd(PI_RX_SID_GOOD_CNT_US),
			   pi_rd(PI_RX_SID_GOOD_CNT_US + 1 * PI_SID_CNT_GROUP_STRIDE),
			   pi_rd(PI_RX_SID_GOOD_CNT_US + 2 * PI_SID_CNT_GROUP_STRIDE),
			   pi_rd(PI_RX_SID_GOOD_CNT_US + 3 * PI_SID_CNT_GROUP_STRIDE),
			   pi_rd(PI_RX_SID_GOOD_CNT_US + 4 * PI_SID_CNT_GROUP_STRIDE),
			   pi_rd(PI_RX_SID_BAD_CNT_US));
		/*
		 * The split: switch->PON-IP ingress (rx) beside PON->OLT egress
		 * (tx), for the same enrolled SID. See PI_TX_SID_CNT_US for how
		 * to read the pair.
		 */
		seq_printf(s, "us_sidcnt[grp0]: rx=%u tx=%u  (SID %u enrolled in CNT_MASK_US group 0)\n",
			   (u32)pi_rd(PI_RX_SID_CNT_US), (u32)pi_rd(PI_TX_SID_CNT_US),
			   GPON_OMCC_FLOW);
		/* The old "these bus-abort, never read them" note about */
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
		/* sched64: the OMCC queue's drain-side witnesses on one line. */
		{
			/* The word is the OMCC SID's and the latch is its own */
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
		/* feed64: the US-feed / framer witnesses. dsc_sts carries */
		seq_printf(s, "feed64: dsc_sts=0x%08x[sram_used=%u dram_used=%u] sstart(0x5200)=%u feed_cnt=%u\n",
			   pi_rd(PI_PON_DSC_STS_US), pi_rd(PI_PON_DSC_STS_US) & 0x1fffu,
			   (pi_rd(PI_PON_DSC_STS_US) >> 16) & 0x1fffu,
			   gpon_rd(GPON_GTC_US_PROC_MODE) & 1u, gpon_us_feed_rearm_cnt);
		/* usdram: the AUTHORITATIVE US SRAM->DRAM staging state -- */
		seq_printf(s, "usdram: usage(0x20ec)=0x%08x[pipe_vld=%u] mstbase(0x20e8)=0x%08x dsccfg(0x215c)=0x%08x runout(0x20e0)=0x%08x ponipctl(0x20d8)=0x%08x[pbuf_en=%u]\n",
			   pi_rd(PI_PON_DSC_USAGE_US), (pi_rd(PI_PON_DSC_USAGE_US) >> 31) & 1u,
			   pi_rd(PI_IP_MSTBASE_US), pi_rd(PI_PON_DSC_CFG_US), pi_rd(PI_DSCRUNOUT_US),
			   pi_rd(PI_PONIP_CTL_US), pi_rd(PI_PONIP_CTL_US) & 1u);
		/* usintr: the GPON interrupt latch state -- the discriminator */
		seq_printf(s, "usintr: top[mask0x40=0x%08x sts0x44=0x%08x] gtcus[dlt=0x%08x mask=0x%08x sts=0x%08x] gemus[dlt=0x%08x mask=0x%08x sts=0x%08x] svc_cnt=%u\n",
			   gpon_rd(GPON_INTR_MASK), gpon_rd(GPON_INTR_STS),
			   gpon_rd(GPON_GTC_US_INTR_DLT), gpon_rd(GPON_GTC_US_INTR_MASK), gpon_rd(GPON_GTC_US_INTR_STS),
			   gpon_rd(GPON_GEM_US_INTR_DLT), gpon_rd(GPON_GEM_US_INTR_MASK), gpon_rd(GPON_GEM_US_INTR_STS),
			   gpon_us_intr_svc_cnt);
		/* PON-IP OMCI packet counters. Decoder: rx>0 with the NIC */
		seq_printf(s, "omci_pi: rx=%u drop=%u crcerr=%u ustx=%u trapcfg=0x%08x cpu_tx=%u cpu_drop=%u  (swcore OMCI block @0x%05x, PON_TRAP_CFG @0x%05x -- PER-CHIP)\n",
			   sw_rd(OMCI_RX_PKT_CNT), sw_rd(OMCI_DROP_PKT_CNT), sw_rd(OMCI_CRC_ERROR_PKT_CNT),
			   sw_rd(OMCI_TX_PKT_CNT), sw_rd(PON_TRAP_CFG),
			   rtl9602c_eth_omci_tx_dirty(), rtl9602c_eth_omci_tx_dropped(),
			   swc->omci_cnt, swc->pon_trap_cfg);
		/* The UNGATED US-NIC ingress witness, printed beside the two */
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
		/* bwmap: what the GTC BWMAP capture engine holds -- which */
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

			/*
			 * 32 allocations is what the vendor's own readers walk,
			 * of the 128 the address space holds; entries past 32
			 * are never exercised by vendor code and have no known
			 * meaning.
			 */
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
		/* US GTC emission breakdown: PLOAM (idx2 cpu / idx3 auto) */
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
		/* US-scheduler readback: did the gpon_install_tcont writes */
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
		/* SID page-occupancy probe, an OLT-INDEPENDENT verify of the */
		{
			u32 pc;
			int prc = pi_sid_page_cnt(GPON_OMCC_FLOW, &pc);

			seq_printf(s, "sidpage_omcc: used=%u max=%u (max>0 = OMCI ENQUEUED to the OMCC queue) [r255c=0x%08x r2564=0x%08x poll=%d]\n",
				   pc & 0x1fff, (pc >> 16) & 0x1fff, pi_rd(PI_PONIP_DBG_CTRL_US), pc, prc);
		}
		/* Data-flow US datapath witness. The data flow rides the OMCC */
		{
			u32 pc1;
			int prc1;
			u32 s2q1 = (pi_rd(PI_PON_SID2QID) >> ((GPON_DATA_FLOW % 4) * 7)) & 0x7fu;
			u32 svl1 = (pi_rd(PI_PON_SIDVALID) >> GPON_DATA_FLOW) & 1u;

			/*
			 * Same strobe as sidpage_omcc above, different SID: one
			 * helper. pgpoll= is the helper's rc, so a wedged
			 * strobe does not print a stale pgbank1 that reads like
			 * a fresh one.
			 */
			prc1 = pi_sid_page_cnt(GPON_DATA_FLOW, &pc1);
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
	/* The omcid GEM flows: CAM, AES and routing, then frames each way since
	 * the last read (the counters clear on read). */
	{
		unsigned int f;

		for (f = LUNA_XPON_FLOW_BASE; f < LUNA_XPON_FLOW_BASE + LUNA_XPON_GEMS; f++) {
			int c = gpon_ds_cam_read(f);

			if (c < 0 || !(c & BIT(16)))
				continue;
			seq_printf(s, "xflow%u: gem=%u tcfg=0x%x ds_rx=%u ds_fwd=%u us_bytes=%u | sidvld=%u s2q=%u usmap=0x%x\n",
				   f, c & 0xfff,
				   gpon_rd(GPON_GTC_DS_TRAFFIC_CFG + f * DS_TRAFFIC_CFG_STRIDE) & 0x1f,
				   gpon_gem_ds_rx_cnt(f), gpon_gem_ds_fwd_cnt(f),
				   gpon_rd(GPON_GEM_US_BYTE_STAT + f * 8),
				   pi_packed_get(PI_PON_SIDVALID, f, 1),
				   pi_packed_get(PI_PON_SID2QID, f, 7),
				   gpon_rd(GPON_GEM_US_PORT_MAP + f * GEM_US_PORT_MAP_STRIDE));
		}
	}
	{
		struct luna_xpon_pm pm;

		luna_xpon_hw_pm(&pm);
		seq_printf(s, "ds_ploam_blank: %u\n", gpon_ds_blank);
		seq_printf(s, "dying_gasp_irqs: %u\n", luna_dying_gasps);
		seq_printf(s, "pm: fec ds=%d us=%d corrected_bytes=%llu corrected_cw=%llu uncorrectable_cw=%llu total_cw=%llu fec_seconds=%llu bip_errors=%llu\n",
			   pm.ds_fec, pm.us_fec, pm.fec_corrected_bytes,
			   pm.fec_corrected_codewords, pm.fec_uncorrectable_codewords,
			   pm.fec_total_codewords, pm.fec_seconds, pm.bip_errors);
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
	/* The one line that says whether optic_los is sampled or forced. */
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
	/* TX-path verification: confirm the TX serializer writes landed. */
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
	seq_printf(s, "fsm: state=O%u onu_id=%u sn_tx=%u ds_rx=%u sds_sync=%u ticks=%u sn=%*phN\n",
		   gpon_fsm_state, luna_ploam.onu_id, luna_ploam.sn_tx, gpon_ds_rx,
		   gpon_sds_synced, gpon_fsm_ticks, 8, gpon_sn_bytes);
	{
		bosa_a2h_show(s);
		seq_printf(s, "anig_rx_level: 0x%04x anig_tx_level: 0x%04x\n",
			   (u16)anig_rx_level, (u16)anig_tx_level);
	}
	/* SerDes/serializer run-state vs LIVE working-stock O5 */
	seq_printf(s, "sds_run: 280c=%04x[3106] 281c=%04x[1359] 225a0=%04x[713] 220a8=%04x[2] 225d8=%04x[29]\n",
		   sw_rd(SDS(0x2280c)) & 0xffff, sw_rd(SDS(0x2281c)) & 0xffff,
		   sw_rd(SDS_ANA_COM_REG08) & 0xffff, sw_rd(SDS(0x220a8)) & 0xffff,
		   sw_rd(SDS_ANA_COM_REG22) & 0xffff);
	seq_printf(s, "sds_ext: 22a2c=%04x[0] 22a30=%04x[4] 22a34=%04x[326a]\n",
		   sw_rd(SDS(0x22a2c)) & 0xffff, sw_rd(SDS(0x22a30)) & 0xffff,
		   sw_rd(SDS(0x22a34)) & 0xffff);
	return 0;
}

static int gpon_proc_show(struct seq_file *s, void *v)
{
	int ret;

	mutex_lock(&bosa_lock);
	if (luna_stopping)
		ret = -ESHUTDOWN;
	else
		ret = v2802rh_gpon_show_locked(s);
	mutex_unlock(&bosa_lock);
	return ret;
}

/* FSM state exposed to /proc (defined with the FSM below). */
static void gpon_parse_sn_into(u8 *out, const char *s)
{
	if (!s || !*s)
		return;		/* nothing provisioned yet: not an error, leave `out` alone */
	if (gpon_sn_parse(s, out))
		pr_warn("gpon: [SYS-009] ONU-SN \"%s\" is not 4 ID chars + 8 hex digits -- keeping the serial in force\n",
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

/* Compose and transmit a 12-byte upstream PLOAM on the */
static void gpon_send_cpu_ploam(u8 queue, const u8 m[12])
{
	u32 ind, full = queue == PLM_US_QUEUE_URG ? GPON_US_PLM_URG_FULL : 0;
	int i;

	if (!READ_ONCE(luna_activation_ready))
		return;

	/* The CPU US-PLOAM path has a SINGLE transmit buffer and */
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
		pr_warn_ratelimited("luna-gpon: [QOS-004] US PLOAM queue %u still full after 2 ms: type 0x%02x dropped\n",
				    queue, m[1]);
		return;
	}

	for (i = 0; i < 6; i++)
		gpon_wr(GPON_GTC_US_PLOAM_DATA + i * 4,
			((u32)m[2 * i] << 8) | m[2 * i + 1]);
	/* The comment said 0x13 and the expression made 0x03 */
	gpon_wr(GPON_GTC_US_PLOAM_CFG, GPON_US_PLM_CFG_REST);	/* 0x13, as stock rests */

	ind |= GPON_US_PLM_ENQ;			/* ENQ 0->1 edge: transmit */
	gpon_wr(GPON_GTC_US_PLOAM_IND, ind);
	if (queue != PLM_US_QUEUE_DYING_GASP && !gpon_ploam_us_repetitive(m[1]))
		luna_ploam_capture(GPON_OLT_US, m, GPON_PLOAM_US_LEN);

	/* DBG (ploam_tx_dbg) */
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


/* Respond to a downstream Request_Password (0x09) with the */
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


/* OMCI channel (OMCC) GEM datapath. */
static_assert(GPON_GEM_US_RANGE_OK(GPON_OMCC_FLOW_9602C, 1u, GEM_US_PORT_MAP_IDX_MAX),
	      "the RTL9602C OMCC flow index runs past the US port-map array");
static_assert(GPON_GEM_US_RANGE_OK(GPON_DATA_FLOW, 1u, GEM_US_PORT_MAP_IDX_MAX),
	      "the data flow index runs past the US port-map array");
/* GPON_DATA_FLOW(1) / the OLT's gem-port-id — the WAN data GEM (clean-room nas0-equivalent) —
 * are defined in luna_gpon_nic.h (shared with the eth driver's gpon0 TX descriptor). */
#define GPON_OMCC_DSQ_HIGH	2

/* Dedicated DATA T-CONT for the WAN data GEM. */
#define GPON_DATA_ALLOC		256u	/* OLT data Alloc-ID for THIS OLT (consistent; from ME262) */
static int gpon_install_tcont(u8 tcont, u16 alloc);	/* fwd: data-GEM install binds the data T-CONT */
/* fwd: the Assign_Alloc-ID handler binds the data */
static int gpon_omci_rx_cnt(void)
{
	u32 v = rtl9602c_eth_omci_rx_count();

	return v == GPON_OMCI_RX_UNAVAIL ? -1 : (int)v;
}

/* Set a `bits`-wide entry at index `idx` in the */
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

/*
 * Read the bits-wide entry back through the same locate, so a /proc readback
 * shows the true value.
 */
static u32 pi_packed_get(u32 base, unsigned int idx, unsigned int bits)
{
	struct pi_packed_slot slot = pi_packed_locate(base, idx, bits);

	return pi_packed_extract(pi_rd(slot.reg), &slot);
}

/* The classify SIDs of the GEMs omcid installed (luna_xpon_hw_gem_install). */
static DECLARE_BITMAP(luna_xpon_sids, LUNA_CLASSIFY_SID_MAX);

/* Faithful port of the stock PON-MAC GPON mode-set */
static void rtl9602c_ponmac_modeset_gpon(bool keep_data)
{
	DECLARE_BITMAP(keep, LUNA_CLASSIFY_SID_MAX) = { 0 };


	/* (1) all-SID classify invalidation pre-pass, so the US-NIC resolves the
	 * OMCC SID cleanly at the re-latch, as stock does. */
	__set_bit(GPON_OMCC_FLOW, keep);
	if (keep_data) {
		__set_bit(GPON_DATA_FLOW, keep);
		if (luna_ctrl_flow_wanted())
			__set_bit(GPON_CTRL_FLOW, keep);
		bitmap_or(keep, keep, luna_xpon_sids, LUNA_CLASSIFY_SID_MAX);
	}
	pi_packed_fill(PI_PON_SIDVALID, swc->classify_sid_num, 1, 0, keep);
	pi_packed_fill(PI_PON_SID2QID, swc->classify_sid_num, 7, swc->scratch_phys_qid & 0x7f, keep);

	/* (2) OMCI classify triple, in stock order (SID2QID -> SIDVALID -> OMCI_CFG). */
	pi_packed_set(PI_PON_SID2QID, GPON_OMCC_FLOW, 7, GPON_OMCC_PHYS_QID & 0x7f);
	pi_packed_set(PI_PON_SIDVALID, GPON_OMCC_FLOW, 1, 1);
	pi_field(PI_PON_OMCI_CFG, 6, 0, GPON_OMCC_FLOW);

	/*
	 * (2b) WAN data-GEM classify, re-asserted every modeset: the all-SID
	 * loop above would otherwise wipe it. SID2QID is the data qid so the
	 * data US is reported to the OLT's DBA on the data Alloc; the legacy
	 * path rides the OMCC's T-CONT 16.
	 */
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

	/*
	 * (5) GMII re-latch: pulse GMII_RX_EN off->on so the US-NIC RX engine
	 * latches the now-complete SID classify table.
	 */
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

	rtl9601d_l2_changed();
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

	/*
	 * DS GEM-port CAM: map gem -> the OMCC flow, mark isOMCI. One
	 * indirect-CAM transaction through the shared gpon_gtc_ds_port_write()
	 * reading luna_gpon_chip.
	 */
	rc = luna_port_cam_write(GPON_OMCC_FLOW, gem);
	if (rc < 0) {
		pr_err("luna-gpon: [OMC-001] OMCC DS GEM install timeout\n");
		return rc;
	}
	gpon_ds_traffic_set(GPON_OMCC_FLOW, gem, DS_TRAFFIC_IS_OMCI);

	/* DS OMCI PTI: tell the GTC how to detect the end of an OMCI GEM frame for
	 * reassembly. At reset this register is 0, so the GTC never recognises an OMCI
	 * frame boundary and drops every downstream OMCI frame. */
	gpon_wr(GPON_GTC_DS_OMCI_PTI, DS_OMCI_PTI_VAL);
	/* A live online stock ONU sets the ADJACENT DS-PTI registers to the same value
	 * -- GPON_GTC_DS_TDM_PTI and GPON_GTC_DS_ETH_PTI. Setting only the OMCI one
	 * produced NOTHING from the DS de-encap: reassembly needs all three. */
	gpon_wr(GPON_GTC_DS_TDM_PTI, DS_OMCI_PTI_VAL);	/* stock O5 = 0x11 */
	gpon_wr(GPON_GTC_DS_ETH_PTI, DS_OMCI_PTI_VAL);	/* stock O5 = 0x11 */

	/* GEM DS pass config */
	gpon_wr(GPON_GEM_DS_MC_CFG, GEM_DS_MC_CFG_VAL);

	/* GEM-DS reassembly flush/forward timer. The RESET default has OMCI_TR_MODE=1
	 * and stock only FIELD-writes the assemble timer, preserving it; a full write
	 * of 0x10 CLEARS OMCI_TR_MODE, and with OMCI transparent mode off the DS GEM
	 * de-assembler does not pass OMCI frames at all. Field-write only. */
	gpon_field(GPON_GEM_DS_FRM_TIMEOUT, 8, 8, 1);	/* OMCI_TR_MODE = 1 (stock reset default) */
	gpon_field(GPON_GEM_DS_FRM_TIMEOUT, 4, 0, 16);	/* ASSM_TIMEOUT_FRM = 16 frames */

	/* US GEM-port map for the OMCC flow */
	if (!gpon_gtc_us_gem_stamp(&gpon_io, &luna_gpon_chip.gtc,
				   GPON_OMCC_FLOW, gpon_gem_us_port_id(gem)))
		pr_err("luna-gpon: [OMC-002] gpon_chip table declares no US port map -- OMCC gem %u not stamped\n",
		       gem);

	/* Re-assert PONIP_DBG_CTRL_US on every OMCC (re-)install: the initial pbo_init
	 * write sets DBG_IGNORE_TAG, but a DEACT/re-range cycle resets the US-NIC debug
	 * block and clears it. Without it the CPU tag is not stripped from US frames
	 * before GEM encapsulation, so the OMCI is malformed on every re-range. */
	pi_wr(PI_PONIP_DBG_CTRL_US, 0x00086000u);	/* stock (DBG_IGNORE_TAG=1) */

	/*
	 * PON-IP SID-valid + OMCI-SID, confirmed against live stock at O5.
	 * SID_Q_MAP_DS is 0 on stock: DS OMCI reaches the CPU purely via the
	 * GMAC CPU-tag trap, not a PBO queue, and a value of 2 misroutes the
	 * de-encapped OMCI away from the CPU.
	 */
	pi_packed_set(PI_PON_SID2QID, GPON_OMCC_FLOW, 7, GPON_OMCC_PHYS_QID & 0x7f);
	pi_packed_set(PI_PON_SIDVALID, GPON_OMCC_FLOW, 1, 1);
	pi_field(PI_PON_OMCI_CFG, 6, 0, GPON_OMCC_FLOW);
	pi_packed_set(PI_PON_SID_Q_MAP_DS, GPON_OMCC_FLOW, 2, 0);
	/* Enrol THIS CHIP's OMCC SID in US counter-mask group 0 */
	{
		u32 mask_w = PI_CNT_MASK_US_WORD(0, GPON_OMCC_FLOW);

		pi_wr(mask_w, pi_rd(mask_w) | BIT(GPON_OMCC_FLOW % 32));
	}

	/*
	 * Read the packed entries back through pi_packed_get, the mirror of
	 * pi_packed_set, to confirm SID2QID landed at the true word.
	 */
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

	/* GMII re-latch AFTER ALL US-NIC config is written */
	pi_wr(PI_IO_CMD_0_US, 0x90101050u);	/* GMII RX OFF */
	pi_wr(PI_IO_CMD_0_US, 0x90101070u);	/* GMII RX ON -> re-latch */
	luna_log("relatch_us: re-pulsed GMII_RX_EN after OMCC install (io0_us=0x%08x)\n",
		pi_rd(PI_IO_CMD_0_US));

	/* Full US-feed FIFO re-arm at O5. */
	gpon_us_feed_rearm_light();

	/* Re-assert AUTO_PROC_SSTART at O5: the HW's per-grant SStart auto-processing
	 * that STARTS the US burst. Written once at init, but the ranging reset can
	 * clear this US-side register and leave the framer parked on grants. */
	if (READ_ONCE(luna_activation_ready)) {
		gpon_wr(GPON_GTC_US_WRITE_PROTECT, GPON_US_WP_UNLOCK);
		gpon_field(GPON_GTC_US_PROC_MODE, 0, 0, 1);	/* US_PROC_MODE.AUTO_PROC_SSTART = 1 */
		gpon_wr(GPON_GTC_US_WRITE_PROTECT, GPON_US_WP_LOCK);
		luna_log("O5 re-asserted AUTO_PROC_SSTART (0x5200 bit0=%u)\n",
			gpon_rd(GPON_GTC_US_PROC_MODE) & 1u);
	}

	luna_log("OMCC installed gem=%u flow=%u (compl %d)\n",
		gem, GPON_OMCC_FLOW, rc);
	if (luna_xpon_active()) {
		luna_xpon_hw_arm_broadcast();
		luna_xpon_report_channel(gpon_omcc_gem_port);
	}
	return 0;
}

/* Install the WAN data GEM (the OLT's wire gem-port-id) */
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
	if (luna_xpon_active())
		return luna_xpon_data_ready() && READ_ONCE(luna_activation_ready);
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


/* The board's UNI panel, read from its own device tree. */
static u8 luna_uni_port[OMCI_UNI_MAX];
static u8 luna_uni_port_n;

/* The COMMON pending/apply owner for both Luna boards. */
static void luna_uni_apply_work_fn(struct work_struct *w);
static DECLARE_DELAYED_WORK(luna_uni_apply_work, luna_uni_apply_work_fn);

/* The enqueue gate. */
static bool luna_uni_closing;

static void luna_uni_apply_queue(unsigned long delay)
{
	unsigned long flags;

	spin_lock_irqsave(&luna_omci_lock, flags);
	/* And not before the driver is ready. */
	if (!luna_uni_closing && luna_driver_ready)
		schedule_delayed_work(&luna_uni_apply_work, delay);
	spin_unlock_irqrestore(&luna_omci_lock, flags);
}

/* bosa_lock is the lifetime owner. */
struct luna_uni_apply_ctx {
	struct omci_onu *onu;
	void *cookie;
	int (*set)(void *, unsigned int, bool);
};

static enum omci_uni_apply_rc luna_uni_apply_slot(void *sh, u8 slot, bool locked)
{
	struct luna_uni_apply_ctx *c = sh;

	if (!c->set || slot >= luna_uni_port_n) {
		pr_warn_once("luna-gpon: [LAN-001] UNI slot %u has no %s; its administrative state is modelled and not applied\n",
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
	/*
	 * The walk and the re-arm rule are G.988's and live in the core; this
	 * driver supplies only the port call and the lock the model is kept
	 * under. Re-armed on the same model the snapshot came from: bosa_lock
	 * has kept it attached for the whole call.
	 */
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
/* A temporary diagnostic ingress */
static u16 luna_uni_test_tci = 0xd100;

/* The inventory a board run needs to RESTORE what it found. */
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
	/* The port map is copied in the same critical section. */
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

	/*
	 * The instance is checked against the board's declared panel. A typo
	 * would otherwise inject a Set for a UNI this board does not have.
	 */
	spin_lock_irqsave(&luna_omci_lock, flags);
	if (luna_omci.onu)
		declared = omci_uni_slot(&luna_omci.onu->pptp_eth_uni, (u16)inst) >= 0;
	tci = ++luna_uni_test_tci;
	spin_unlock_irqrestore(&luna_omci_lock, flags);
	if (!declared) {
		pr_err("luna-gpon: [OMC-003] uni_test REFUSED: instance %#06x is not in this board's declared panel\n",
		       inst);
		return -ENODEV;
	}

	/* ME 11 attribute 5, administrative state: a UNIQUE tid per injection,
	 * AR=0 and AK=0 so no response is owed.  The octets are the core's. */
	admin_val = (u8)admin;
	omci_set_build(msg, tci, OMCI_ME_PPTP_ETH_UNI, (u16)inst, OMCI_ATTR_BIT(5),
		       &admin_val, 1);

	/*
	 * The request is announced before it is submitted. The FSM timer can
	 * consume it and the apply can complete on another CPU before a line
	 * printed after the call reaches the log, and a reader would see the
	 * completion before its request.
	 */
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
		pr_err("luna-gpon: [OMC-004] /omci-uni REFUSED: %s -- keeping the single-UNI default\n",
		       d.unis_why);
	if (d.unis && !luna_uni_port_n)
		pr_warn("luna-gpon: [OMC-005] /omci-uni has no usable ethernet-uni-ports (%d bytes for %d UNIs): the administrative state is modelled and NEVER APPLIED\n",
			d.ports_len, d.unis);
	if (d.part)
		pr_err("luna-gpon: [LAN-002] /omci-uni %s REFUSED: %s -- that part keeps its default\n",
		       d.part, d.part_why);
}

/* The unit's own counts over the board's, under luna_omci_lock. */
static void luna_omci_unit_capacity(struct omci_onu *onu)
{
	const char *why = "";

	if (omci_onu_declare_unit_capacity(onu, onu_capacity, &why) ==
	    OMCI_UNI_DECL_BAD)
		pr_err("luna-gpon: [OMC-006] onu_capacity '%s' REFUSED: %s -- the board's counts stay\n",
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
		pr_warn_ratelimited("luna-gpon: [OMC-007] ANI-G Test Result not sent (OMCI TX error)\n");
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



static int gpon_avc_sent;	/* OMCI oper-state AVCs emitted this O5 (reset on re-range) */

/* The Alloc-ID -> T-CONT bind belongs to Assign_Alloc-ID. */
static void luna_data_tcont_from_assign(u16 alloc, bool assigned)
{
	enum gpon_gem_us_bind bind;

	if (!assigned)
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

static void luna_tcont_grants_changed(u16 alloc, bool assigned);

/* The ownership consumer owns the bind. */
static void luna_data_alloc_changed(void *sh, u16 alloc, bool assigned)
{
	(void)sh;
	gpon_ploam_data_alloc_note(&luna_ploam, alloc, assigned);
	if (luna_xpon_active()) {
		luna_tcont_grants_changed(alloc, assigned);
		luna_xpon_report_alloc();
		return;
	}
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
			/* A Set the model accepted and a MIB Reset that */
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
		pr_err("luna-gpon: [OMC-008] DATA GEM DS install timeout\n");
		return rc;
	}
	/* DATA flow DS routing = 0x2 (BIT1). */
	gpon_ds_traffic_set(GPON_DATA_FLOW, gpon_data_gem_port, 0x2);

	/* US GEM-port map: the data flow -> the OLT's gem, the gem-id stamped on US
	 * data frames. Same shared gpon_gtc_us_gem_stamp() as the OMCC stamp, and the
	 * 12-bit wire mask is gpon_gem_us_port_id(), the one spelling. */
	luna_data.stamp_dirty = reg_has(luna_gpon_chip.gtc.gem_us_port_map);
	if (!gpon_gtc_us_gem_stamp(&gpon_io, &luna_gpon_chip.gtc, GPON_DATA_FLOW,
				   gpon_gem_us_port_id(gpon_data_gem_port))) {
		pr_err("luna-gpon: [OMC-009] gpon_chip table declares no US port map -- data gem %u not stamped\n",
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

	/* Multicast/broadcast GEM -> its own flow, BRIDGED */
	rc = luna_port_cam_write(GPON_MCAST_FLOW, GPON_MCAST_GEM);
	if (rc < 0) {
		pr_err("luna-gpon: [OMC-010] MCAST GEM DS install timeout\n");
		return rc;
	}
	/* MCAST flow DS routing = 0x3 (BIT1|BIT0): BIT0 is the broadcast/flood
	 * variant, so DHCP-broadcast and ARP DS also drain to the NIC. */
	gpon_ds_traffic_set(GPON_MCAST_FLOW, GPON_MCAST_GEM, 0x3);

	/* Data-flow SID classify commit */
	{
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
			pr_err("luna-gpon: [OMC-011] DATA GEM classify readback timeout\n");
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
	seq_printf(s, "fsm_tick_gap_max_ms %u\nds_ploam_latency_max_ms %u\n",
		   luna_fsm_gap_max_ms, luna_ds_latency_max_ms);
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


/* Bind an OLT-assigned Alloc-ID to a T-CONT in the GTC */
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
		pr_err("luna-gpon: [QOS-005] T-CONT alloc bind timeout\n");
		return rc;
	}

	/* PON-MAC US scheduler activation. */
	{
		/* The queue comes from the PER-CHIP resolver, not a */
		u8 qid = (tcont == GPON_OMCC_TCONT_ALT) ? swc->omcc_phys_qid
						       : luna_tcont_phys_qid(tcont);
		int drain_rc;

		pi_packed_set(PI_PON_TCONT_EN, tcont, 1, 1);	/* PON_TCONT_EN[tcont] (1b packed) */
		/* Drain out the physical queue BEFORE binding it into */
		drain_rc = luna_queue_drain(qid);
		if (drain_rc < 0) {
			pr_err_ratelimited("luna-gpon: [QOS-006] qid %u drain-out failed (%d): drain_cmd=%08x US_PBUF=%08x state=O%u activation_ready=%u\n",
				qid, drain_rc, pi_rd(PI_DRN_CMD), pi_rd(PI_PONIP_CTL_US),
				gpon_fsm_state, READ_ONCE(luna_activation_ready));
			return drain_rc;
		}
		if (tcont == GPON_DATA_TCONT) {
			drain_rc = luna_queue_drain(qid + 1);
			if (drain_rc < 0) {
				pr_err_ratelimited("luna-gpon: [QOS-007] control qid %u drain-out failed (%d)\n",
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
		if (tcont == GPON_DATA_TCONT) {
			pi_field(PI_PON_QID_PIR_RATE + (qid + 1) * PI_QID_RATE_STRIDE, 17, 0, 0x3ffff);
			pi_field(PI_PON_QID_CIR_RATE + (qid + 1) * PI_QID_RATE_STRIDE, 17, 0, 0);
			pi_packed_set(PI_PON_WFQ_TYPE, qid + 1, 1, 0);
			pi_packed_set(PI_PON_WFQ_WEIGHT, qid + 1, 10, 1);
		}
		/* Re-issue the OMCC classifier's SID-valid bit NOW, after */
		if (qid == GPON_OMCC_PHYS_QID) {
			/* arm_ctx witness */
			u32 en   = pi_packed_get(PI_PON_TCONT_EN, tcont, 1);
			u32 qmap = pi_packed_get(PI_PON_SCH_QMAP, tcont, swc->sch_qmap_bits) & 0x3u;
			u32 pir  = pi_rd(PI_PON_QID_PIR_RATE + qid * PI_QID_RATE_STRIDE) & 0x3ffffu;

			if (en && qmap && pir)
				luna_log("SIDVALID[%u] arm_ctx OK (tcont_en=1 sch_qmap=%u pir=0x%x) -> re-issuing on armed queue\n",
					GPON_OMCC_FLOW, qmap, pir);
			else
				pr_warn("luna-gpon: [QOS-008] SIDVALID[%u] re-issued with arm INCOMPLETE (tcont_en=%u sch_qmap=%u pir=0x%x) -> binding committed against un-armed queue\n",
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
/* The FAMILY's half of CONFIG_GPON_GEM_DIAG. */
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
	want.omcc_alloc = luna_ploam.onu_id;
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
	if (plan != GPON_DATA_INSTALL) {
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
		pr_warn("luna-gpon: [QOS-009] data reconciliation failed rc=%d; ownership retained\n", rc);
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

/*
 * The OLT's VLAN service model reaches this family, and what it can do with it
 * is reported rather than claimed. The core decodes each ME 171 row, decides it
 * into a filter and treatment, and hands it to a family through struct
 * gpon_vlan_ops, counting four outcomes.
 *
 * rule_install refuses on purpose. No Luna VLAN rule encoding has been measured
 * for these shapes. The ledger keeps a refusal apart from nobody listening:
 * failed means the family was asked and said no, no_installer means nothing was
 * there to ask.
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



/* Several upstream-config registers */
static void gpon_wr_us_protected(u32 off, u32 val)
{
	gpon_wr(GPON_GTC_US_WRITE_PROTECT, GPON_US_WP_UNLOCK);
	gpon_wr(off, val);
	gpon_wr(GPON_GTC_US_WRITE_PROTECT, GPON_US_WP_LOCK);
}

/* Program the upstream burst-mode overhead */
static void gpon_apply_boh(bool ranged)
{
	u8 oh[GPON_BOH_LEN];
	u8 guard = gpon_boh_guard, t3 = ranged ? gpon_boh_t3ranged : gpon_boh_t3pre;
	u8 rep, i, boh_len, size;
	unsigned int want;

	/* Exact port of the stock burst-overhead build. */
	if (guard > 32)
		guard = 32;
	rep = guard / 8;			/* boh_repeat = whole guard bytes (fill index) */

	/* Widen before clamping. */
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

	/* BOH_REPEAT is NOT guard/8 */
	gpon_wr(GPON_GTC_US_BOH_CFG,
		(((size - 4) & 0xf) << 8) | (boh_len & 0xff));	/* REPEAT=size-4, LENGTH=full */
	for (i = 0; i < size; i++)
		gpon_wr(GPON_GTC_US_BOH_DATA + i * 4, oh[i]);

	luna_log("BOH %s guard=%u rep=%u boh_repeat=%u ptn=0x%02x delim=%02x%02x%02x t3=%u boh_len=%u size=%u oh=%*phN\n",
		ranged ? "ranged" : "prerng", guard, rep, (size - 4) & 0xf, gpon_boh_ptn,
		gpon_boh_delim[0], gpon_boh_delim[1], gpon_boh_delim[2],
		t3, boh_len, size, size, oh);
}

/* Upstream equalization delay. */
static void gpon_set_eqd(u32 value)
{
	u32 min_delay1 = (gpon_rd(GPON_GTC_US_MIN_DELAY) >> 7) & 0x1ff;
	u32 eqd1  = value + min_delay1 * 128;
	/*
	 * The core's constant: this arithmetic and gpon_ploam.c's set_eqd()
	 * must divide by the same number or they would range the ONU
	 * differently.
	 */
	u32 multi = eqd1 / GPON_PLOAM_EQD_FRAME_LEN;
	u32 intra = eqd1 - multi * GPON_PLOAM_EQD_FRAME_LEN;

	gpon_wr(GPON_GTC_US_EQD,
		((multi & GPON_EQD_MF_MASK) << GPON_EQD_MF_SHIFT) |
		(intra & GPON_EQD_INFRAME_MASK));
}

static u32 gpon_o5_entry_tick;
static bool gpon_vlan_lan_open;

/* Re-lock the TX CMU PLL at O3 entry. */
static void gpon_txpll_relock(void)
{
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


/* The REGISTER CLUSTER alone, and the split is the */
static void gpon_o5_rearm_burst_regs(void)
{
	if (!READ_ONCE(luna_activation_ready))
		return;
	/* Re-apply the O5 packed-burst gate cluster on EVERY O5 entry, not just at
	 * __init: a re-range performs a GMAC/SDS reset that can clear these US-side
	 * registers ("isolated tolerates, packed exposes"). US-side only. */
	gpon_wr_us_protected(0x5188, gtune->us_optic_sd_th);	/* US_OPTIC_SD_TH, per chip */
	gpon_field(0x526c, 0, 0, 1);			/* US_PWR_SAV_MODE */
	if (gtune->us_pwr_sav_dg_tx_opt)
		gpon_field(0x526c, 16, 16, 1);		/* DG_TX_OPT, per chip */
	gpon_wr(GPON_GEM_US_PWR_SAV_CFG, (0x10u << 16) | 0x100u);	/* GEM_US_PWR_SAV_CFG */
	gpon_wr(GPON_GEM_US_EOB_MERGE, 0x00000028u);			/* GEM_US_EOB_MERGE */
}

/*
 * The cluster plus the auto-No_message keepalive re-arm, for this driver's own
 * FSM on an O5 entry. The core FSM must not reach this one: it sends its own
 * No_message.
 */

/* The marks belong to one activation: the OLT sends them again after it. */
static void gpon_aes_ports_clear(void)
{
	unsigned int f;

	bitmap_zero(gpon_aes_ports, 4096);
	for (f = 0; f < 128; f++)
		gpon_field(GPON_GTC_DS_TRAFFIC_CFG + f * DS_TRAFFIC_CFG_STRIDE, 4, 4, 0);
}

static void gpon_below_o5(void)
{
	gpon_aes_ports_clear();
	luna_data_suspend();
	gpon_rerange_start_j = jiffies ? jiffies : 1;	/* start the outage timer */
	gpon_o5_entry_tick = 0;
	/*
	 * lan_keep_open: leave VLAN_FILTER cleared so LAN management survives
	 * the WAN-down/re-range; the OLT re-config on resume tolerates it.
	 */
}

static void gpon_fsm_set_state(u8 st)
{
	u8 prev = gpon_fsm_state;

	/* gpon_hold: keep the FSM parked at O1 — refuse every advance past O1 so the
	 * GPON never ranges/deactivates and the shared switch datapath stops churning,
	 * leaving br-lan + the WiFi AP stable for LAN+WiFi access. (GPON/WAN off.) */
	if ((!READ_ONCE(luna_activation_ready) || gpon_hold) && st > 1)
		return;
	/*
	 * The HW ONU_STATE field uses the same 1-based numbers (O1..O5 = 1..5)
	 * and gates the GTC's auto-SN/ranging transmitter. Written before any
	 * print: the console is synchronous, and a GTC left at O4 behind the
	 * ranged flush answers the OLT's O5 grants as ranging requests.
	 */
	gpon_field(GPON_GTC_DS_ONU_ID_STATUS, 3, 0, st);
	if (gpon_fsm_state != st)
		luna_log("ONU state O%u -> O%u\n", gpon_fsm_state, st);
	if (prev != st)
		gpon_los_run = 0;	/* fresh LOS debounce window on every transition */
	gpon_fsm_state = st;
	if (prev != st)
		luna_xpon_report_state(st);

	/* Hybrid LAN/VLAN bookkeeping: mark O5 entry; on any drop below O5, re-arm VLAN
	 * filtering for the next config and reset the LAN-open timer. */
	if (st == 5 && prev != 5) {
		/* Re-range completion diagnostic */
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
		/* Re-apply the O5 packed-burst gate cluster + re-arm the */
	}
	gpon_led_pon_set(st);
}

static void gpon_cdr_reset_worker(struct work_struct *w)
{
	if (!READ_ONCE(luna_activation_ready))
		return;
	u32 cdr;

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

/* This FSM is the duplicate. Do not extend it. */
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
	/* PLM_FLUSH_BUF is edge-triggered, 0 THEN */
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
	/* BOTH registers. */
	gpon_field(GPON_GTC_DS_ONU_ID_STATUS, 15, 8, onu_id);
	gpon_field(GPON_GTC_US_ONU_ID, 15, 8, onu_id);
	luna_xpon_report_onu_id(onu_id);
}

static void luna_op_on_below_o5(void *sh)
{
	(void)sh;
	gpon_below_o5();
}

static void luna_op_o5_rearm_burst(void *sh)
{
	(void)sh;
	/*
	 * The cluster only: gpon_ploam.h's contract ends with "the core emits
	 * the No_message that follows it". gpon_o5_rearm_burst() sends one too,
	 * which would put two No_messages on the wire per O5 entry. The native
	 * FSM keeps the pair.
	 */
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
	/* BOTH halves */
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

/*
 * What the OLT sent and what the ONU did about it, as "OLT:" and "ONU:" lines
 * in dmesg. Protocol events run in the FSM timer (see the log ring above), so
 * they are queued and printed in small batches, never in O2-O4 and not until
 * O5 has settled past OMCI provisioning. The time in each line is the event's.
 */
#define LUNA_DIAG_EVENTS	256	/* a power of two: the indices wrap */
#define LUNA_DIAG_BATCH		4
#define LUNA_DIAG_O5_SETTLE	(15 * HZ)

struct luna_diag_event {
	u64 ns;
	const char *fmt;
	u32 a, b;
};

static struct luna_diag_event luna_diag_events[LUNA_DIAG_EVENTS];
static unsigned int luna_diag_head, luna_diag_tail, luna_diag_lost;
static unsigned long luna_diag_o5_at;
static u32 luna_diag_o5_omci;	/* OMCI frames the CPU had received at that O5 */
static DEFINE_SPINLOCK(luna_diag_lock);

static void luna_diag_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(luna_diag_work, luna_diag_work_fn);

static void luna_diag_print(const struct luna_diag_event *e)
{
	u64 s = e->ns;
	u32 ms = do_div(s, NSEC_PER_SEC) / NSEC_PER_MSEC;
	char line[96];

	scnprintf(line, sizeof(line), e->fmt, e->a, e->b);
	pr_info("luna-gpon: %s (at %llu.%03u s)\n", line, s, ms);
}

static void luna_diag_work_fn(struct work_struct *work)
{
	struct luna_diag_event e;
	unsigned int n, lost = 0;
	unsigned long flags;
	u8 st = READ_ONCE(gpon_fsm_state);
	bool more = false;

	if ((st >= 2 && st <= 4) ||
	    (st == 5 && time_before(jiffies, READ_ONCE(luna_diag_o5_at) +
					     LUNA_DIAG_O5_SETTLE))) {
		schedule_delayed_work(&luna_diag_work, HZ);
		return;
	}
	for (n = 0; n < LUNA_DIAG_BATCH; n++) {
		spin_lock_irqsave(&luna_diag_lock, flags);
		more = luna_diag_head != luna_diag_tail;
		if (more)
			e = luna_diag_events[luna_diag_tail++ % LUNA_DIAG_EVENTS];
		else
			swap(lost, luna_diag_lost);
		spin_unlock_irqrestore(&luna_diag_lock, flags);
		if (!more)
			break;
		luna_diag_print(&e);
	}
	if (lost)
		pr_info("luna-gpon: %u later PON events not logged, queue full\n", lost);
	if (more)
		schedule_delayed_work(&luna_diag_work, HZ / 2);
}

/* @fmt takes two u32s and must be a string literal: it is printed later. */
static void luna_diag(const char *fmt, u32 a, u32 b)
{
	unsigned long flags;

	spin_lock_irqsave(&luna_diag_lock, flags);
	if (luna_diag_head - luna_diag_tail == LUNA_DIAG_EVENTS)
		luna_diag_lost++;
	else
		luna_diag_events[luna_diag_head++ % LUNA_DIAG_EVENTS] = (struct luna_diag_event) {
			.ns = local_clock(), .fmt = fmt, .a = a, .b = b,
		};
	spin_unlock_irqrestore(&luna_diag_lock, flags);
	schedule_delayed_work(&luna_diag_work, 5 * HZ);
}

/*
 * Whether the ONU itself was held up: PLOAM and the FSM run at bottom-half
 * priority, so only interrupts or bottom halves held off can delay them this
 * far. Late activation without these lines points at the OLT instead.
 */
#define LUNA_DIAG_LATE_MS	100

static void luna_diag_late(const char *fmt, u32 ms, u32 *max)
{
	*max = max(*max, ms);
	if (ms >= LUNA_DIAG_LATE_MS)
		luna_diag(fmt, ms, READ_ONCE(gpon_fsm_state));
}

/*
 * The downstream BIP and FEC counters clear on read. REI and the OMCI
 * telemetry both want them, so every read is folded into totals since boot
 * and each consumer takes what it needs from those.
 */
#define GPON_FEC_CW_PER_FRAME	77	/* RS(255,239) over a 19440-byte frame */

static DEFINE_SPINLOCK(luna_pm_lock);
static struct luna_xpon_pm luna_pm;
static u64 luna_pm_rei_last, luna_pm_uncor_seen;
static u32 luna_pm_superframe;
static bool luna_pm_superframe_valid;

static void luna_pm_fold_locked(void)
{
	luna_pm.bip_errors += gpon_rd(GPON_GTC_DS_MISC_CNTR_BIP_ERR_BLK);
	luna_pm.fec_corrected_bytes += gpon_rd(GPON_GTC_DS_MISC_CNTR_FEC_CORRECT_BYTE);
	luna_pm.fec_corrected_codewords += gpon_rd(GPON_GTC_DS_MISC_CNTR_FEC_CORRECT_CW);
	luna_pm.fec_uncorrectable_codewords += gpon_rd(GPON_GTC_DS_MISC_CNTR_FEC_UNCOR_CW);
}

/* Once a second: count FEC codewords and FEC seconds while in O5. */
static void luna_pm_tick(void)
{
	unsigned long flags;
	u32 sf, o5_s;
	bool o5 = READ_ONCE(gpon_fsm_state) == 5;
	bool ds_was, us_was;

	spin_lock_irqsave(&luna_pm_lock, flags);
	ds_was = luna_pm.ds_fec;
	us_was = luna_pm.us_fec;
	luna_pm.ds_fec = o5 && (gpon_rd(GPON_GTC_DS_INTR_STS) & GPON_DS_INTR_STS_FEC);
	luna_pm.us_fec = o5 && (gpon_rd(GPON_GTC_US_INTR_STS) & GPON_US_INTR_STS_FEC);
	if (!o5) {
		luna_pm_superframe_valid = false;
		goto out;
	}
	luna_pm_fold_locked();
	sf = gpon_rd(GPON_GTC_DS_SUPERFRAME_CNT) & GPON_SUPERFRAME_MASK;
	if (luna_pm_superframe_valid && luna_pm.ds_fec)
		luna_pm.fec_total_codewords += (u64)((sf - luna_pm_superframe) &
						     GPON_SUPERFRAME_MASK) *
					       GPON_FEC_CW_PER_FRAME;
	luna_pm_superframe = sf;
	luna_pm_superframe_valid = true;
	if (luna_pm.fec_uncorrectable_codewords != luna_pm_uncor_seen) {
		luna_pm_uncor_seen = luna_pm.fec_uncorrectable_codewords;
		luna_pm.fec_seconds++;
	}
out:
	spin_unlock_irqrestore(&luna_pm_lock, flags);
	/* Both bits come from the OLT's frames; nothing here writes FEC on. */
	if (!o5)
		return;
	o5_s = (jiffies - READ_ONCE(luna_diag_o5_at)) / HZ;
	if (luna_pm.ds_fec != ds_was)
		luna_diag(luna_pm.ds_fec ? "OLT: downstream FEC on, %u s after O5" :
			  "OLT: downstream FEC off, %u s after O5", o5_s, 0);
	if (luna_pm.us_fec != us_was)
		luna_diag(luna_pm.us_fec ? "OLT: upstream FEC on, %u s after O5" :
			  "OLT: upstream FEC off, %u s after O5", o5_s, 0);
}

void luna_xpon_hw_pm(struct luna_xpon_pm *pm)
{
	unsigned long flags;

	spin_lock_irqsave(&luna_pm_lock, flags);
	luna_pm_fold_locked();
	*pm = luna_pm;
	spin_unlock_irqrestore(&luna_pm_lock, flags);
}

static int luna_op_ds_bip_errors(void *sh, u32 *count)
{
	unsigned long flags;

	(void)sh;
	spin_lock_irqsave(&luna_pm_lock, flags);
	luna_pm_fold_locked();
	*count = (u32)(luna_pm.bip_errors - luna_pm_rei_last);
	luna_pm_rei_last = luna_pm.bip_errors;
	spin_unlock_irqrestore(&luna_pm_lock, flags);
	return 0;
}

static void luna_op_analog_relock(void *sh)
{
	(void)sh;
	gpon_txpll_relock();
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

static void luna_op_port_encrypt(void *sh, u16 port, bool encrypted)
{
	unsigned int f, n = 0;

	(void)sh;
	if (encrypted == test_bit(port, gpon_aes_ports))
		return;
	assign_bit(port, gpon_aes_ports, encrypted);
	for (f = 0; f < 128; f++) {
		int e = gpon_ds_cam_read(f);

		if (e < 0 || !(e & BIT(16)) || (e & 0xfff) != port)
			continue;
		gpon_field(GPON_GTC_DS_TRAFFIC_CFG + f * DS_TRAFFIC_CFG_STRIDE,
			   4, 4, encrypted);
		n++;
	}
	luna_log("GEM %u: AES %s (%u flow%s)\n", port, encrypted ? "on" : "off",
		 n, n == 1 ? "" : "s");
	luna_diag(encrypted ? "OLT: Encrypted_Port-ID GEM %u on" :
		  "OLT: Encrypted_Port-ID GEM %u off", port, 0);
}

static int luna_op_install_omcc(void *sh, u16 gem)
{
	(void)sh;
	return gpon_install_omcc(gem);
}

static int luna_op_install_tcont(void *sh, u8 tcont, u16 alloc)
{
	(void)sh;
	/* Park the unused CAM entries FIRST, so the entry about to */
	if (tcont == GPON_OMCC_TCONT) {
		int rc = gpon_alloc_cam_clear_others(GPON_OMCC_TCONT);

		if (rc)
			return rc;
	}
	return gpon_install_tcont(tcont, alloc);
}

/* `trace` is the one op left NULL-able */
static struct gpon_ploam_cfg luna_ploam_cfg_live __maybe_unused;

static const struct gpon_ploam_cfg luna_ploam_cfg __maybe_unused = {
	.hold			= false,	/* set from gpon_hold at init */
	.cdr_reseat_on_reactivate = true,
	.o5_rearm_burst_gate	= true,
	.o3_feed_reset		= false,
	.data_gem_en		= true,
	.omcc_alt_bind		= false,
	/* 0 means use the live ONU-ID. */
	.omcc_alloc_override	= 0,
	.omcc_tcont		= GPON_OMCC_TCONT,	/* 16 on Luna */
	.omcc_tcont_alt		= GPON_OMCC_TCONT_ALT,	/* 1, only when alt_bind */
	.data_tcont		= GPON_DATA_TCONT,	/* 8 on Luna */
};

/* The core's events, spoken out loud. */
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

/* The instrument must be REMOVABLE, because it is a SUSPECT. */
#ifdef CONFIG_GPON_PLOAM_DIAG
static bool core_trace = true;
module_param(core_trace, bool, 0644);
#else
static const bool core_trace;
#endif

/* FAMILY half of the core's gpon_ploam_diag */
static unsigned long luna_poll_prev_jiffies;
static u32 luna_poll_gap_ms;	/* start of this poll - start of the previous, ms */
static u8 luna_rx_burst_idx;	/* DS PLOAMs this poll dequeued before the current one */

/* FAMILY half of the core's gpon_bwcap_diag */
static struct gpon_bwcap_diag luna_bwcap;
static bool luna_bwcap_armed;

/* The longest window the 8-bit CAP_FRAME_NUM allows */
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

/*
 * Debug (CONFIG_GPON_PLOAM_DIAG): the first N ticks of every O5, one line per
 * tick: what the upstream did with the grants the GTC accepted. gpon_rd only:
 * pi_rd hangs this context.
 */
static const uint o5_window_ticks;

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
	/* US-GTC misc PM */
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

static void luna_diag_ploam(enum gpon_ploam_ev ev, u32 a, u32 b)
{
	switch (ev) {
	case GPON_PLOAM_EV_ONU_ID:
		luna_diag("OLT: Assign_ONU-ID %u", a, 0);
		break;
	case GPON_PLOAM_EV_RANGING_TIME:
		luna_diag("OLT: Ranging_Time EqD %u", a, 0);
		break;
	case GPON_PLOAM_EV_DEACT:
		luna_diag("OLT: [PON-001] Deactivate_ONU-ID, was ONU %u", a, 0);
		if (READ_ONCE(gpon_fsm_state) == 5)
			luna_diag("ONU: [OMC-012] deactivated %u ms after O5, %u OMCI frames reached the CPU",
				  jiffies_to_msecs(jiffies - READ_ONCE(luna_diag_o5_at)),
				  rtl9602c_eth_omci_rx_count() - luna_diag_o5_omci);
		break;
	case GPON_PLOAM_EV_DISABLE_SN:
		luna_diag(b ? "OLT: Disable_Serial_Number enable, code %#x" :
			  "OLT: Disable_Serial_Number disable, code %#x", a, 0);
		break;
	case GPON_PLOAM_EV_SN_REPROVISIONED:
		luna_diag("OLT: [PON-002] serial number given a new ONU-ID, restarting at O1", 0, 0);
		break;
	case GPON_PLOAM_EV_ASSIGN_ALLOC:
		luna_diag("OLT: Assign_Alloc-ID %u, type %u", a, b);
		break;
	case GPON_PLOAM_EV_REQ_PW:
		luna_diag("OLT: Request_Password", 0, 0);
		break;
	case GPON_PLOAM_EV_REQ_KEY:
		luna_diag("OLT: Request_Key", 0, 0);
		break;
	case GPON_PLOAM_EV_KEY_SWITCH_ARM:
		luna_diag("OLT: Key_Switching_Time, superframe %u", a, 0);
		break;
	case GPON_PLOAM_EV_UNHANDLED:
		luna_diag("OLT: [PON-013] unhandled PLOAM type %#x for ONU %u", a, b);
		break;
	case GPON_PLOAM_EV_STATE:
		if (b == 5) {
			luna_diag_o5_omci = rtl9602c_eth_omci_rx_count();
			WRITE_ONCE(luna_diag_o5_at, jiffies);
		}
		luna_diag("ONU: state O%u -> O%u", a, b);
		break;
	case GPON_PLOAM_EV_KEY_SENT:
		luna_diag("ONU: sent encryption key, index %u", a, 0);
		break;
	case GPON_PLOAM_EV_DEACT_KEEP_LOCK:
		luna_diag("ONU: [PON-003] kept downstream lock through deactivation, %u ticks at O5", a, 0);
		break;
	case GPON_PLOAM_EV_KEEP_LOCK_GIVEUP:
		luna_diag("ONU: [PON-004] stopped keeping lock after %u ticks, state %u", a, b);
		break;
	case GPON_PLOAM_EV_RERANGE_DONE:
		luna_diag("ONU: re-range %u done, outage %u ms", a, b);
		break;
	case GPON_PLOAM_EV_LOS_RERANGE:
		luna_diag("ONU: [PON-005] re-ranging after %u LOS ticks", a, 0);
		break;
	case GPON_PLOAM_EV_O3_UNHEARD:
		luna_diag("ONU: [PON-006] no OLT answer in O3, %u ticks, %u serial number offers", a, b);
		break;
	case GPON_PLOAM_EV_O4_TIMEOUT:
		luna_diag("ONU: [PON-007] no Ranging_Time after %u ticks in O4", a, 0);
		break;
	case GPON_PLOAM_EV_O5_WATCHDOG:
		luna_diag("ONU: [PON-008] O5 watchdog, %u ticks with no WAN RX", a, 0);
		break;
	case GPON_PLOAM_EV_O5_UNGRANTED:
		luna_diag("ONU: [PON-009] no upstream grant at O5, %u ticks, streak %u", a, b);
		break;
	default:
		break;
	}
}

static void luna_op_trace(void *sh, enum gpon_ploam_ev ev, u32 a, u32 b)
{
	const char *nm = NULL;
	enum gpon_ploam_diag_point p = gpon_ploam_diag_point_of(ev);

	(void)sh;
	luna_diag_ploam(ev, a, b);
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
	.port_encrypt	= luna_op_port_encrypt,
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

	/* All zero is no G.984.3 type, and its CRC of zero still passes. The line
	 * counters did not move when these arrived: count them, do not act. */
	if (!memchr_inv(m, 0, GPON_PLOAM_DS_LEN)) {
		gpon_ds_blank++;
		return;
	}

	/* Surface any DS PLOAM that is not the repetitive broadcast acquisition
	 * traffic, so activation progress is visible. */
	gpon_last_ds_type = type;
	if (trace && !gpon_ploam_ds_repetitive(type))
		luna_log("DS PLOAM onu_id=0x%02x type=0x%02x d=%*phN\n",
				    onu_id, type, 8, d);

	/* Say which FSM is dispatching, once, so a boot log is */
	{
		static bool said;

		if (!said) {
			said = true;
			luna_log("PLOAM dispatch = COMMON core gpon_ploam.c\n");
		}
	}

	if (!READ_ONCE(luna_activation_ready))
		return;

	/* Ticks x 10, NOT the wall clock, and the more accurate */
	if (!gpon_ploam_ds_repetitive(type))
		luna_ploam_capture(GPON_OLT_DS, m, GPON_PLOAM_DS_LEN);
	gpon_ploam_ds(&luna_ploam, m, GPON_PLOAM_DS_LEN,
		      luna_now_ms());
}

/* Has the WAN netdev received NOTHING? Both watchdogs */
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

/*
 * Ahead of the activation gate: a board whose activation never becomes ready
 * would otherwise keep VLAN_FILTER on and its LAN dark.
 */
static void luna_lan_keep_open_poll(void)
{
	sw_field(SW_VLAN_CTRL, 0, 0, READ_ONCE(luna_vlan_filter_ok));	/* LAN open, every state */
	if (!gpon_vlan_lan_open) {
		gpon_vlan_lan_open = true;
		luna_log("lan_keep_open -> LAN access open (VLAN_FILTER %d)\n",
			 READ_ONCE(luna_vlan_filter_ok));
	}
}

static const bool ploam_irq = true;

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
	luna_diag_late("ONU: [PON-010] PLOAM read %u ms after its interrupt, in O%u",
		       div_u64(ktime_get_boottime_ns() - arrival, NSEC_PER_MSEC),
		       &luna_ds_latency_max_ms);
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
/* The hardware sends the template by itself: this is only the record of it, and
 * what a dip that does not cut power needs, as the OLT now takes the ONU for dead.
 * Masked after the first, so a board whose line floats cannot storm. */
static bool luna_dying_gasp_irq(void)
{
	if (!(sw_rd(SW_INTR_IMS) & SW_INTR_DYING_GASP))
		return false;
	sw_field(SW_INTR_IMR, 13, 13, 0);
	sw_wr(SW_INTR_IMS, SW_INTR_DYING_GASP);
	luna_dying_gasps++;
	pr_emerg("luna-gpon: [PON-011] dying gasp interrupt\n");
	if (dying_gasp_reset) {
		mdelay(100);
		emergency_restart();
	}
	return true;
}

static irqreturn_t luna_gpon_irq_handler(int irq, void *data)
{
	bool gasp = luna_dying_gasp_irq();
	u32 events;

	if (!(gpon_rd(GPON_GTC_DS_PLOAM_IND) & GPON_DS_PLM_BUF_EMPTY))
		atomic64_cmpxchg(&luna_ds_arrival_ns, 0, ktime_get_boottime_ns());
	luna_ds_irqs++;
	sw_wr(SW_INTR_IMS, SW_INTR_GPON);
	events = gpon_rd(GPON_GTC_DS_INTR_DLT);		/* read-clear */
	if (!events)
		return gasp ? IRQ_HANDLED : IRQ_NONE;
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
		pr_warn("luna-gpon: [SYS-011] GPON interrupt %d refused (%d): downstream PLOAM waits for the FSM tick\n",
			luna_gpon_irq, ret);
		luna_gpon_irq = 0;
		return;
	}
	gpon_wr(GPON_INTR_MASK, GPON_INTR_GTC_DS);
	sw_wr(SW_INTR_IMS, SW_INTR_GPON | SW_INTR_DYING_GASP);
	sw_field(SW_INTR_IMR, 10, 10, 1);
	sw_field(SW_INTR_IMR, 13, 13, 1);
}

#define GTC_DS_STS_LOF		BIT(1)		/* loss-of-frame; clear = framer byte-locked */
#define GPON_ESD_INTERVAL_MS	5000		/* stock gpon_esdRecover_interval */
#define GPON_ESD_THRESHOLD	20		/* stock gpon_esdRecover_threshold */

static void gpon_fsm_poll(struct timer_list *t)
{
	static u64 prev_ns;
	u64 now_ns = ktime_get_boottime_ns();

	spin_lock(&luna_fsm_lock);
	if (prev_ns)
		luna_diag_late("ONU: [PON-012] FSM ticks %u ms apart, in O%u",
			       div_u64(now_ns - prev_ns, NSEC_PER_MSEC),
			       &luna_fsm_gap_max_ms);
	prev_ns = now_ns;
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
	/* The PERIODIC half of the FSM is the core's, as */
	gpon_ploam_tick(&luna_ploam);
	WRITE_ONCE(luna_health_los,
		   !!(gpon_rd(GPON_GTC_DS_LOS_CFG_STS) & GPON_OPTIC_LOS_SIG));
	gpon_led_los_set(READ_ONCE(luna_health_los));
	/* The LOS lamp a board puts on a GPIO is net/xpon's: told on change,
	 * from this tick's own sample (luna_health_los is -1 while quiesced). */
	if (READ_ONCE(luna_health_los) != luna_los_reported) {
		luna_los_reported = READ_ONCE(luna_health_los);
		luna_xpon_report_los(luna_los_reported == 1);
	}
	/* The serial was (re)provisioned after ranging began. Drop to O1 and re-offer
	 * the new Serial_Number. */
	gpon_apply_identity_change();
drain_downstream:
	luna_ds_ploam_drain(false);
	if (!READ_ONCE(luna_activation_ready))
		goto next_poll;
	/* Both provisioning follow-ups are one core poll */
	gpon_ploam_poll_provision(&luna_ploam, luna_now_ms());
	luna_omci_service();

	/* O5 provisioning watchdog. */
	gpon_ploam_poll_watchdog(&luna_ploam, gpon_wan_rx_silent(),
				 luna_now_ms());
	if (!(luna_ploam.ticks % 100))
		gpon_ploam_poll_grants(&luna_ploam, gpon_bwm_acpt_since(&gpon_bwm_acpt_watch_last),
				       luna_now_ms());

	/* Autonomous downstream-LOS recovery (fiber pull). */
	if (los_rerange_ticks && gpon_fsm_state >= 2) {
		bool optic_los = !!(gpon_rd(GPON_GTC_DS_LOS_CFG_STS) & GPON_OPTIC_LOS_SIG);
		/* A chip with no declared SDS_FIB_STATUS has no second witness, and an
		 * absent witness may not be manufactured into one. */
		bool sds_dark  = SDS_FIB_STATUS &&
				 !(sw_rd(SDS_FIB_STATUS) & SDS_FIB_SDS_SDET);

		/* Real DS-light loss = the GTC optical-LOS AND the SoC */
		gpon_ploam_poll_los(&luna_ploam, optic_los, sds_dark,
				    luna_now_ms());
	}

	/* Hybrid LAN/VLAN */
	if (IS_ENABLED(CONFIG_GPON_PLOAM_DIAG) && o5_window_ticks &&
	    gpon_fsm_state == 5 && gpon_o5_entry_tick &&
	    gpon_fsm_ticks - gpon_o5_entry_tick < o5_window_ticks)
		luna_o5_window_sample();
	if (trace && gpon_fsm_state == 5 && (gpon_fsm_ticks % 150) == 0)
		/*
		 * rxsid/ustx/dirty are left out of this fast O5 print: pi_rd in
		 * this context hangs the poll right after OMCC install. It is
		 * an indirect polled PON-IP access, unsafe here unlike
		 * sw_rd/gpon_rd; read those counters through /proc instead.
		 */
		luna_log("O5 t=%u last=0x%02x onu=%u hwst=%u eqd=0x%08x | dsrx_omcc=%u pirx=%u omcirx=%d | ploam_cpu=%u gem_byte=%u gemus_omcc=%u idle16=%u idle8=%u\n",
			gpon_fsm_ticks, gpon_last_ds_type, luna_ploam.onu_id,
			gpon_rd(GPON_GTC_DS_ONU_ID_STATUS) & 0xf, gpon_rd(GPON_GTC_US_EQD),
			gpon_gem_ds_rx_cnt(GPON_OMCC_FLOW), sw_rd(OMCI_RX_PKT_CNT),
			gpon_omci_rx_cnt(),
			gpon_us_misc_cnt(2), gpon_us_misc_cnt(4),
			gpon_rd(GEM_US_STAT(GPON_OMCC_FLOW)), gpon_rd(TCONT_IDLE_STAT(16)),
			gpon_rd(TCONT_IDLE_STAT(GPON_DATA_TCONT)));
	/* US-OMCI egress stall localizer, SAFE reads only */
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
	/* Periodic SerDes-TX re-sync while UN-RANGED. The upstream-burst serializer
	 * lock is non-deterministic, so re-pulse the TX-interface reset-B
	 * (WSDS_DIG_1D[16]) ~every 2 s to keep re-attempting a lock onto the framer
	 * burst data. TX-interface only, so the locked RX framer is undisturbed. */
	if (gpon_fsm_state >= 3 && luna_ploam.onu_id == 0xff && (gpon_fsm_ticks % 200) == 0) {
		gpon_cdr_reseat();
		gpon_sds_synced++;
	}
	/* While unregistered in O3, re-offer our Serial_Number_ONU ~twice a second
	 * (the OLT grants SN windows intermittently). */
	gpon_ploam_poll_sn_reoffer(&luna_ploam, luna_now_ms());
	/* Periodic O5 upstream-PLOAM keepalive. */
	gpon_ploam_poll_keepalive(&luna_ploam, luna_now_ms());
	gpon_ploam_poll_rei(&luna_ploam, luna_now_ms());

	/* Runtime DS-CDR-wedge recovery */
	{
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
				pr_warn_ratelimited("luna-gpon: [OPT-003] DS CDR wedged (GTC_DS_STS=0x%08x); SP_SDS_EN_RX re-acquire #%u\n",
						    sts, gpon_cdr_stuck_count);
			}
		} else {
			gpon_cdr_stuck_tries = 0;	/* healthy -> fresh fast budget */
		}
	
	}

	/* Periodic DS multiframe/BWmap ESD-recover */
	if (gpon_fsm_state >= 3) {
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
					pr_warn_ratelimited("luna-gpon: [OPT-004] ESD-recover: DS PLEND/LOM fail=%u byte-locked at O%u (BWmap mis-phased) -> RX-CDR re-lock #%u\n",
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

/* Stock's omitted LDO init step, byte-exact. */
static void __init rtl9602c_sc_ldo_init(void)
{
	u32 fdca, t130 = sw_rd(THERMAL_CTRL_0);

	sw_wr(SC_CMD, 0x0001fdcau);		/* SC read cmd: reg 0xfdca */
	udelay(1000);
	fdca = sw_rd(SC_DATA);
	pr_debug("luna-gpon: sc_ldo_init BEFORE: 0xfdca=0x%08x THERMAL(0x130)=0x%08x (stock 0x130=0x00ec0005)\n",
		fdca, t130);
	sw_wr(SC_IND_WD, fdca & ~0xcu);		/* clear DRAM-LDO bits 2,3 (stock mask ~0xC) */
	udelay(1000);
	sw_wr(SC_CMD, 0x0003fdcau);		/* SC write commit */
	sw_wr(SC_CMD, 0x0001fdcau);		/* re-issue read (stock reads back for log) */
	udelay(1000);
	(void)sw_rd(SC_DATA);
	sw_wr(THERMAL_CTRL_0, sw_rd(THERMAL_CTRL_0) | 0x00800000u);			/* THERMAL: set bit23 */
	sw_wr(THERMAL_CTRL_0, (sw_rd(THERMAL_CTRL_0) & 0xff80ffffu) | 0x006c0000u);	/* preserve low-16 */
	pr_debug("luna-gpon: sc_ldo_init AFTER: 0x130=0x%08x\n", sw_rd(THERMAL_CTRL_0));
}

/* The laser's enable pins are SoC GPIOs on some boards */
static int laser_tx_dis_gpio = -1;	/* from DT; -1 = this board has none */
static int laser_tx_pwr_gpio = -1;
/*
 * Board pad routing: IO_GPIO_EN w0 w1, GPIO dir/data ABCD, dir/data EFGH. A
 * board that declares none keeps its bootloader's pads.
 */
static u32 board_gpio_pads[6];
static bool board_gpio_pads_set;

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
		   bosa_gn_identified ? "GN25L95" : "unknown");
	mutex_unlock(&bosa_lock);
	return 0;
}

static void gpon_optical_work_fn(struct work_struct *w)
{
	/* Jiffies start 300 s before their wrap: a zero here would hold it off. */
	static unsigned long pm_next = INITIAL_JIFFIES;

	if (luna_driver_ready && !luna_stopping &&
	    time_after_eq(jiffies, pm_next)) {
		luna_pm_tick();
		pm_next = jiffies + HZ;
	}
	mutex_lock(&bosa_lock);
	if (luna_driver_ready && !luna_stopping) {
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
	if (!of_property_read_u32(np, "realtek,tx-disable-pin", &v) ||
	    !of_property_read_u32(np, "realtek,tx-disable-gpio", &v))
		laser_tx_dis_gpio = (int)v;
	if (!of_property_read_u32(np, "realtek,tx-power-gpio", &v))
		laser_tx_pwr_gpio = (int)v;
	board_gpio_pads_set = !of_property_read_u32_array(np, "realtek,gpio-pads",
							   board_gpio_pads, 6);
	if (!of_property_read_u32(np, "realtek,i2c-bus", &v)) {
		if (v <= 1)
			bosa_i2c_bus = v;
		else
			pr_err("luna-gpon: [OPT-005] /pon-optics realtek,i2c-bus=%u: this die has buses 0 and 1 -- keeping 0\n", v);
	}
	of_node_put(np);
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
		pr_err("luna-gpon: [OPT-006] %s GPIO %d level %u failed: %d\n",
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
		.laser_wanted = !laser_off,
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
	ret = luna_gn_calibrate_locked();
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
	gpon_us_feed_rearm();
	/* Quiesce disables PBUF even when O5 pool accounting is retained. */
	pi_field(PI_PONIP_CTL_US, 0, 0, 1);
	if (!(pi_rd(PI_PONIP_CTL_US) & BIT(0))) {
		ret = -EIO;
		goto fail;
	}
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
		pr_err("luna-gpon: [OPT-007] optical activation failed and TX inhibit readback failed\n");
	return ret;
}

/* ---- the omcid backend's hardware (luna_xpon.c) ----
 * Data T-CONTs 8..15 share PON queue group 1, one queue each. Their GEM flows
 * start at 4: flows 1-3 belong to the in-kernel data path, and
 * gpon_install_omcc() cleans flows 1 and 2 on every OMCC install. */

u8 luna_xpon_hw_state(void)
{
	return READ_ONCE(gpon_fsm_state);
}

/* The receiver's optical LOS as last sampled: 1 lost, 0 light, -1 not
 * sampled (the activation machine is quiesced). */
int luna_xpon_hw_los(void)
{
	return READ_ONCE(luna_health_los);
}

u8 luna_xpon_hw_onu_id(void)
{
	return READ_ONCE(luna_ploam.onu_id);
}

int luna_xpon_hw_omcc_gem(void)
{
	return luna_ploam.omcc_installed ? READ_ONCE(gpon_omcc_gem_port) : -1;
}

bool luna_xpon_hw_alloc_known(u16 alloc)
{
	return gpon_ploam_data_alloc_known(&luna_ploam, alloc);
}

u8 luna_xpon_hw_omcc_qid(void)
{
	return GPON_OMCC_PHYS_QID;
}

u8 luna_xpon_hw_tcont_qid(unsigned int slot)
{
	return luna_tcont_phys_qid(GPON_DATA_TCONT) + slot;
}

/* Under luna_fsm_lock, as are the next three. */
static int luna_tcont_program(unsigned int slot, u16 alloc)
{
	u8 tcont = GPON_DATA_TCONT + slot, qid = luna_xpon_hw_tcont_qid(slot);
	int rc;

	rc = luna_alloc_cam_write(tcont, alloc);
	if (rc >= 0) {
		pi_packed_set(PI_PON_TCONT_EN, tcont, 1, 1);
		rc = luna_queue_drain(qid);
	}
	if (!rc) {
		pi_packed_set(PI_PON_SCH_QMAP, tcont, swc->sch_qmap_bits,
			      BIT(qid % swc->tcont_queue_max));
		pi_field(PI_PON_QID_PIR_RATE + qid * PI_QID_RATE_STRIDE, 17, 0, 0x3ffff);
		pi_field(PI_PON_QID_CIR_RATE + qid * PI_QID_RATE_STRIDE, 17, 0, 0);
		pi_packed_set(PI_PON_WFQ_TYPE, qid, 1, 0);
		pi_packed_set(PI_PON_WFQ_WEIGHT, qid, 10, 1);
		luna_tcont_alloc[slot] = alloc;
		luna_tcont_used |= BIT(slot);
		luna_tcont_early &= ~BIT(slot);
	}
	return rc;
}

static void luna_tcont_clear(unsigned int slot)
{
	u8 tcont = GPON_DATA_TCONT + slot;

	luna_queue_drain(luna_xpon_hw_tcont_qid(slot));
	pi_packed_set(PI_PON_SCH_QMAP, tcont, swc->sch_qmap_bits, 0);
	pi_packed_set(PI_PON_TCONT_EN, tcont, 1, 0);
	luna_cam_xact(true, GPON_GTC_CAM_OP_CLEAN, tcont, NULL, NULL);
	luna_tcont_used &= ~BIT(slot);
	luna_tcont_early &= ~BIT(slot);
}

static int luna_tcont_find(u16 alloc)
{
	unsigned int slot;

	for (slot = 0; slot < LUNA_XPON_TCONTS; slot++)
		if ((luna_tcont_used & BIT(slot)) && luna_tcont_alloc[slot] == alloc)
			return slot;
	return -ENOENT;
}

/* The OLT grants an Alloc-ID as soon as it assigns it and deactivates an ONU that
 * leaves the grants unanswered, within tens of ms. omcid may be restoring its MIB
 * or may just have reset it, so the bind cannot wait for its T-CONT. */
static void luna_tcont_answer_grants(u16 alloc)
{
	unsigned int slot;

	if (!early_tcont || !luna_ploam.omcc_installed || luna_tcont_find(alloc) >= 0)
		return;
	slot = ffz(luna_tcont_used);
	if (slot >= LUNA_XPON_TCONTS || luna_tcont_program(slot, alloc))
		return;
	luna_tcont_early |= BIT(slot);
	luna_log("xpon: T-CONT %u <- Alloc-ID %u ahead of OMCI\n", GPON_DATA_TCONT + slot, alloc);
	luna_diag("ONU: T-CONT %u answering Alloc-ID %u ahead of OMCI",
		  GPON_DATA_TCONT + slot, alloc);
}

/* From the PLOAM handler. An OMCI T-CONT's own removal follows from the reconcile. */
static void luna_tcont_grants_changed(u16 alloc, bool assigned)
{
	int slot = luna_tcont_find(alloc);

	if (assigned)
		luna_tcont_answer_grants(alloc);
	else if (slot >= 0 && (luna_tcont_early & BIT(slot)))
		luna_tcont_clear(slot);
}

int luna_xpon_hw_tcont_install(unsigned int slot, u16 alloc)
{
	u8 tcont = GPON_DATA_TCONT + slot, qid = luna_xpon_hw_tcont_qid(slot);
	bool displaced;
	u16 old;
	int other, rc;

	spin_lock_bh(&luna_fsm_lock);
	/* OMCI may give an Alloc-ID another slot than Assign_Alloc-ID's order did. */
	other = luna_tcont_find(alloc);
	if (other >= 0 && other != slot)
		luna_tcont_clear(other);
	displaced = (luna_tcont_used & BIT(slot)) && luna_tcont_alloc[slot] != alloc;
	old = luna_tcont_alloc[slot];
	rc = luna_tcont_program(slot, alloc);
	if (!rc && displaced && gpon_ploam_data_alloc_known(&luna_ploam, old))
		luna_tcont_answer_grants(old);
	spin_unlock_bh(&luna_fsm_lock);
	if (rc < 0)
		return rc;
	luna_log("xpon: T-CONT %u <- Alloc-ID %u, queue %u\n", tcont, alloc, qid);
	luna_diag("ONU: T-CONT %u bound to Alloc-ID %u", tcont, alloc);
	return 0;
}

void luna_xpon_hw_tcont_remove(unsigned int slot)
{
	bool bound;
	u16 alloc;

	spin_lock_bh(&luna_fsm_lock);
	bound = luna_tcont_used & BIT(slot);
	alloc = luna_tcont_alloc[slot];
	luna_tcont_clear(slot);
	/* A MIB reset drops the T-CONT, not the assignment: the OLT still grants it. */
	if (bound && gpon_ploam_data_alloc_known(&luna_ploam, alloc))
		luna_tcont_answer_grants(alloc);
	spin_unlock_bh(&luna_fsm_lock);
}

u8 luna_xpon_hw_gem_flow(unsigned int slot)
{
	return LUNA_XPON_FLOW_BASE + slot;
}

/* The US-NIC latches the SID classify table on a GMII RX enable edge, the
 * commit gpon_install_data_gem() makes for its flow. */
static int luna_xpon_sid_commit(u8 flow, u8 qid)
{
	int tries;

	for (tries = 0; tries < 4; tries++) {
		pi_packed_set(PI_PON_SID2QID, flow, 7, qid & 0x7f);
		pi_packed_set(PI_PON_SIDVALID, flow, 1, 1);
		pi_wr(PI_MEDIA_STS_US, 0x106e8400u);
		pi_wr(PI_IO_CMD_0_US, 0x90101050u);
		udelay(50);
		pi_wr(PI_IO_CMD_0_US, 0x90101070u);
		if (pi_packed_get(PI_PON_SIDVALID, flow, 1) == 1 &&
		    pi_packed_get(PI_PON_SID2QID, flow, 7) == (qid & 0x7f))
			return 0;
		udelay(100);
	}
	return -ETIMEDOUT;
}

void luna_xpon_hw_gem_remove(unsigned int slot)
{
	u8 flow = luna_xpon_hw_gem_flow(slot);

	spin_lock_bh(&luna_fsm_lock);
	__clear_bit(flow, luna_xpon_sids);
	pi_packed_set(PI_PON_SIDVALID, flow, 1, 0);
	gpon_gtc_us_gem_stamp(&gpon_io, &luna_gpon_chip.gtc, flow, 0);
	luna_cam_xact(false, GPON_GTC_CAM_OP_CLEAN, flow, NULL, NULL);
	gpon_field(GPON_GTC_DS_TRAFFIC_CFG + flow * DS_TRAFFIC_CFG_STRIDE, 4, 0, 0);
	spin_unlock_bh(&luna_fsm_lock);
}

/* A downstream-only GEM passes qid < 0. */
int luna_xpon_hw_gem_install(unsigned int slot, u16 gem, int qid)
{
	u8 flow = luna_xpon_hw_gem_flow(slot);
	int rc;

	spin_lock_bh(&luna_fsm_lock);
	rc = luna_port_cam_write(flow, gem);
	if (rc < 0)
		goto out;
	/* DS routing 0x2: drain the flow to the NIC, as the data flow does. */
	gpon_ds_traffic_set(flow, gem, 0x2);
	if (qid < 0)
		goto out;
	if (!gpon_gtc_us_gem_stamp(&gpon_io, &luna_gpon_chip.gtc, flow,
				   gpon_gem_us_port_id(gem))) {
		rc = -ENODEV;
		goto out;
	}
	pi_packed_set(PI_PON_SID_Q_MAP_DS, flow, 2, 0);
	rc = luna_xpon_sid_commit(flow, qid);
	if (!rc) {
		__set_bit(flow, luna_xpon_sids);
		rtl9602c_ponmac_modeset_gpon(true);
	}
out:
	spin_unlock_bh(&luna_fsm_lock);
	if (rc < 0) {
		luna_xpon_hw_gem_remove(slot);
		return rc;
	}
	luna_log("xpon: GEM %u on flow %u, queue %d\n", gem, flow, qid);
	luna_diag("ONU: GEM %u bound to flow %u", gem, flow);
	return 0;
}

/* The shared queue bank of data T-CONTs 8..15; SCH_QMAP bits are relative
 * to it. Each slot's own queue is the bank's first eight. */
u8 luna_xpon_hw_bank_qid(void)
{
	return luna_tcont_phys_qid(GPON_DATA_TCONT);
}

/* One queue's scheduling: WFQ_TYPE 0 is strict, and among a T-CONT's strict
 * queues the higher QID wins (stock packs its highest priority last). PIR is
 * in the scheduler's 1024 B/s units, stock's OMCI bytes/s >> 10. */
int luna_xpon_hw_queue_set(u8 qid, bool wfq, u16 weight, u32 pir)
{
	u32 rate = PI_PON_QID_PIR_RATE + qid * PI_QID_RATE_STRIDE;
	int rc = 0;

	if (qid < luna_xpon_hw_bank_qid() ||
	    qid >= luna_xpon_hw_bank_qid() + LUNA_XPON_BANK_QUEUES ||
	    !weight || weight > 1023 || pir > LUNA_XPON_PIR_MAX)
		return -ERANGE;
	spin_lock_bh(&luna_fsm_lock);
	pi_field(rate, 17, 0, pir);
	pi_field(PI_PON_QID_CIR_RATE + qid * PI_QID_RATE_STRIDE, 17, 0, 0);
	pi_packed_set(PI_PON_WFQ_TYPE, qid, 1, wfq);
	pi_packed_set(PI_PON_WFQ_WEIGHT, qid, 10, weight);
	if ((pi_rd(rate) & 0x3ffff) != pir ||
	    (pi_rd(PI_PON_QID_CIR_RATE + qid * PI_QID_RATE_STRIDE) & 0x3ffff) ||
	    pi_packed_get(PI_PON_WFQ_TYPE, qid, 1) != wfq ||
	    pi_packed_get(PI_PON_WFQ_WEIGHT, qid, 10) != weight)
		rc = -EIO;
	spin_unlock_bh(&luna_fsm_lock);
	return rc;
}

int luna_xpon_hw_queue_drain(u8 qid)
{
	int rc;

	if (qid < luna_xpon_hw_bank_qid() ||
	    qid >= luna_xpon_hw_bank_qid() + LUNA_XPON_BANK_QUEUES)
		return -ERANGE;
	spin_lock_bh(&luna_fsm_lock);
	rc = luna_queue_drain(qid);
	spin_unlock_bh(&luna_fsm_lock);
	return rc;
}

int luna_xpon_hw_tcont_queues(unsigned int slot, u32 mask)
{
	int rc = 0;

	if (slot >= LUNA_XPON_TCONTS)
		return -ERANGE;
	spin_lock_bh(&luna_fsm_lock);
	pi_packed_set(PI_PON_SCH_QMAP, GPON_DATA_TCONT + slot, swc->sch_qmap_bits, mask);
	if (pi_packed_get(PI_PON_SCH_QMAP, GPON_DATA_TCONT + slot,
			  swc->sch_qmap_bits) != mask)
		rc = -EIO;
	spin_unlock_bh(&luna_fsm_lock);
	return rc;
}

int luna_xpon_hw_gem_queue(unsigned int slot, u8 qid)
{
	int rc;

	if (slot >= LUNA_XPON_GEMS || qid < luna_xpon_hw_bank_qid() ||
	    qid >= luna_xpon_hw_bank_qid() + LUNA_XPON_BANK_QUEUES)
		return -ERANGE;
	spin_lock_bh(&luna_fsm_lock);
	rc = luna_xpon_sid_commit(luna_xpon_hw_gem_flow(slot), qid);
	spin_unlock_bh(&luna_fsm_lock);
	return rc;
}

bool luna_xpon_hw_mic_valid(const u8 *msg, unsigned int len)
{
	return gpon_omci_mic_conv(msg, len) == GPON_MIC_CONV_AAL5_BE;
}

int luna_xpon_hw_optics(struct luna_xpon_optics *o)
{
	struct gpon_ddm_a2h d;
	int rc;

	mutex_lock(&bosa_lock);
	if (!bosa_cal_ready) {
		mutex_unlock(&bosa_lock);
		return -EAGAIN;	/* uncalibrated A2h readings are noise */
	}
	rc = bosa_a2h_read(&d);
	mutex_unlock(&bosa_lock);
	if (rc != GPON_DDM_OK)
		return -EIO;
	o->temp_mc = (s16)d.temp * 1000 / 256;
	o->vcc_uv = gpon_ddm_vcc_mv(d.vcc) * 1000;
	o->bias_ua = gpon_ddm_bias_ua(d.bias);
	o->tx_nw = (u32)d.tx_pwr * 100;
	o->rx_nw = (u32)d.rx_pwr * 100;
	return 0;
}

/* omci-agent's transmitter permission: 0 holds the ONU receive-only. */
bool luna_xpon_hw_allow_tx(void)
{
	return !READ_ONCE(gpon_hold);
}

int luna_xpon_hw_set_allow_tx(bool on)
{
	int ret = 0;

	mutex_lock(&bosa_lock);
	if (luna_stopping) {
		ret = -ESHUTDOWN;
	} else if (READ_ONCE(gpon_hold) == on) {
		WRITE_ONCE(gpon_hold, !on);
		WRITE_ONCE(luna_ploam_cfg_live.hold, !on);	/* the PLOAM core's copy */
		/* An activation that cannot run yet (no serial) retries later. */
		if (luna_driver_ready)
			ret = luna_activate_locked(false);
		if (ret) {
			if (on) {
				WRITE_ONCE(gpon_hold, true);
				WRITE_ONCE(luna_ploam_cfg_live.hold, true);
			}
			pr_err_ratelimited("luna-gpon: [OPT-008] transmitter %s failed (%d); activation_ready=%u\n",
					on ? "release" : "hold", ret, READ_ONCE(luna_activation_ready));
		} else {
			pr_info("luna-gpon: transmitter %s\n", on ? "released" : "held");
		}
		luna_resume_poll();
	}
	mutex_unlock(&bosa_lock);
	return ret;
}

/* Raw SFF-8472 A2h bytes, once the module is calibrated. */
int luna_xpon_hw_a2h(u8 off, u8 *buf, unsigned int len)
{
	unsigned int i;
	int b, ret = 0;

	mutex_lock(&bosa_lock);
	if (!bosa_cal_ready)
		ret = -EAGAIN;
	for (i = 0; !ret && i < len; i++) {
		b = bosa_i2c_read8(0x51, off + i);
		if (b < 0)
			ret = b;
		else
			buf[i] = b;
	}
	mutex_unlock(&bosa_lock);
	return ret;
}

void luna_xpon_hw_get_identity(u8 sn[8], u8 password[10])
{
	memcpy(sn, gpon_sn_bytes, 8);
	spin_lock_bh(&luna_pwd_lock);
	memcpy(password, luna_pwd, 10);
	spin_unlock_bh(&luna_pwd_lock);
}

/* An identity from omci-agent, through the same paths as the parameters.
 * An all-zero field is no identity at all and leaves ours in place. */
int luna_xpon_hw_set_identity(const u8 *sn, const u8 *password)
{
	char buf[13];
	int ret = 0;

	if (password && memchr_inv(password, 0, GPON_PLOAM_PASSWORD_LEN)) {
		bool same;

		spin_lock_bh(&luna_pwd_lock);
		same = !memcmp(luna_pwd, password, GPON_PLOAM_PASSWORD_LEN);
		spin_unlock_bh(&luna_pwd_lock);
		if (!same)
			luna_pwd_apply(password);
	}
	if (sn && memchr_inv(sn, 0, 8) && memcmp(sn, gpon_sn_bytes, 8)) {
		snprintf(buf, sizeof(buf), "%c%c%c%c%02X%02X%02X%02X",
			 sn[0], sn[1], sn[2], sn[3], sn[4], sn[5], sn[6], sn[7]);
		ret = luna_sn_apply(buf, NULL);
	}
	return ret;
}

/* The broadcast GEM floods to the NIC whatever OMCI provisions, as the
 * in-kernel data path arms it beside its data GEM. Called on the FSM path. */
static void luna_xpon_hw_arm_broadcast(void)
{
	if (luna_port_cam_write(GPON_MCAST_FLOW, GPON_MCAST_GEM) >= 0)
		gpon_ds_traffic_set(GPON_MCAST_FLOW, GPON_MCAST_GEM, 0x3);
}

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
	/*
	 * Walk the model's own slots: the record carries its own inst and used.
	 */
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

static int __init rtl9602c_gpon_init(void)
{
	u32 ver, rst, test;
	const char *missing;
	int ret;

	/* The op table is checked BEFORE any hardware is touched. */
	missing = gpon_ploam_ops_missing(&luna_ploam_ops);
	if (missing) {
		pr_err("luna-gpon: [SYS-012] realtek-luna's struct gpon_ploam_ops leaves the MANDATORY op ->%s NULL -- refusing to probe (nothing has been touched)\n",
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
		pr_err("luna-gpon: [SYS-013] ioremap 0x%08x failed\n", GPON_PHYS_BASE);
		return -ENOMEM;
	}
	swcore_base = ioremap(SWCORE_PHYS_BASE, SWCORE_REG_SIZE);
	if (!swcore_base) {
		pr_err("luna-gpon: [SYS-014] ioremap 0x%08x failed\n", SWCORE_PHYS_BASE);
		iounmap(gpon_base);
		return -ENOMEM;
	}

	luna_laser_gpio_from_dt();
	luna_led_map_from_dt();
	ret = luna_gpio_drive(laser_tx_dis_gpio, true, "TX_DISABLE inhibit");
	if (ret)
		goto fail_maps;
	/* IO_MODE_EN.OEM_EN routes the optical front-end pads. */
	sw_field(SOC_IO_MODE_EN, swc->io_oem_en, swc->io_oem_en, 1);

	/* Power up the PON packet-datapath IP domain (see SOC_IP_ENABLE_PHYS). */
	{
		void __iomem *ipen = ioremap(SOC_IP_ENABLE_PHYS, 4);

		if (ipen) {
			writel(readl(ipen) | SOC_IP_EN_PON, ipen);
			(void)readl(ipen);		/* post the write */
			iounmap(ipen);
		}
	}

	/* PON-IP datapath window — only reachable now the IP-enable bit is set. */
	ponip_base = ioremap(PONIP_PHYS_BASE, PONIP_REG_SIZE);
	if (!ponip_base) {
		pr_err("luna-gpon: [SYS-015] ioremap 0x%08x failed\n", PONIP_PHYS_BASE);
		iounmap(swcore_base);
		iounmap(gpon_base);
		return -ENOMEM;
	}

	ver  = gpon_rd(GPON_VERSION) & GPON_VER_ID_MASK;
	rst  = gpon_rd(GPON_RESET);
	test = gpon_rd(GPON_TEST);

	pr_debug("luna-gpon: MAC ver 0x%02x\n", ver);

	/* Self-test the register window with the GPON_TEST scratch register, then
	 * restore the power-on value. */
	gpon_wr(GPON_TEST, 0xa5a5a5a5u);
	if (gpon_rd(GPON_TEST) != 0xa5a5a5a5u)
		pr_warn("luna-gpon: [SYS-016] scratch R/W failed — MAC may be gated\n");
	gpon_wr(GPON_TEST, GPON_TEST_SCRATCH);

	/* The GPIO pad routing is Board C's, and its register MOVED. */
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
	}

	/* The panel-LED block is Board C's and is gated off on */
	gpon_led_init();

	/* Bring up the PON SerDes so the MAC core gets its */
	{
		const char *via;
		int sret;

		/* The SECOND writer of the GPIO pad array */
		rtl9602c_sc_ldo_init();
		luna_c2_postmode_perturb = false;	/* skip the post-GPON-mode US-TX perturbations, as stock rev-A */
		luna_c2_sds_cfgrst = false;	/* SDS reset = stock bit0-only */
		luna_c2_stock_analog = true;	/* match live-stock SDS REG01/REG11 post-reset */
		luna_c2_analog_postreset = true;	/* program the full analog CMU/CDR table after the SDS reset (stock rev-A), for cold-start determinism */
		luna_c2_cmu_settle_ms = 0;	/* no TX-CMU-lock settle before reset-B */
		luna_c2_clkgate_rstb = false;	/* no clock-gated reset-B release */
		luna_c2_skip_rstb_dance = false;	/* keep the DIG_1D reset-B pulse */
		luna_c2_minimal_analog = false;	/* keep the full golden-table writes */
		sret = luna_ponmac_mode_set(LUNA_CHIP_9602C, LUNA_REV_A,
					       LUNA_SUBTYPE_NONE,
					       &rtl9602c_r960_ops);
		via = "family-lib 9602C";
		if (sret) {
			pr_warn("luna-gpon: [OPT-009] family-lib SerDes not ready (0x%08x) -> inline fallback\n",
				sw_rd(FIB_EXT_REG21));
			sret = gpon_serdes_init();
			via = "inline fallback";
		}

		if (sret)
			pr_warn("luna-gpon: [OPT-010] SerDes analog-ready not seen (%s, FIB_EXT_REG21=0x%08x)\n",
				via, sw_rd(FIB_EXT_REG21));
		else
			pr_debug("luna-gpon: PON SerDes up (%s, analog ready)\n", via);
		/* Stock powers the laser driver as the LAST step of its PON-MAC
		 * mode set, i.e. right here, once the SerDes is up. */
		ret = luna_gpio_drive(laser_tx_pwr_gpio, false, "laser TX power");
		if (ret)
			goto fail_maps;
	}

	/* Configure the PON-IP packet datapath so the MAC has somewhere to land
	 * downstream frames before it is reset. */
	gpon_pbo_init();
	pr_debug("luna-gpon: PON-IP datapath configured (ctl_us=0x%08x ctl_ds=0x%08x)\n",
		pi_rd(PI_PONIP_CTL_US), pi_rd(PI_PONIP_CTL_DS));

	/* With the SerDes clock present, soft-reset the GPON MAC so its RST_DONE
	 * handshake completes and the GTC banks come out of reset. */
	gpon_wr(GPON_RESET, GPON_SOFT_RST);
	gpon_wr(GPON_RESET, 0);
	if (gpon_wait_rst_done())
		pr_warn("luna-gpon: [OPT-011] RST_DONE not seen (reset=0x%08x)\n",
			gpon_rd(GPON_RESET));
	else
		pr_debug("luna-gpon: MAC reset done, ONU state O%u\n",
			gpon_rd(GPON_GTC_DS_ONU_ID_STATUS) & GPON_ONU_STATE_MASK);
	luna_ds_cam_scrubbed = !gpon_ds_cam_clear_all();
	if (!luna_ds_cam_scrubbed)
		pr_warn("luna-gpon: [OMC-013] DS GEM-port CAM not cleared after the reset: the first Configure_Port-ID clears it\n");

	/* Optical loss-of-signal monitoring, inverted polarity. The downstream framer
	 * gates on OPTIC_LOS_SIG; until the input is enabled with the correct
	 * polarity that status reads "loss" even with real light, holding the FSM at
	 * O1. A working (O5) unit runs this register at 0x03. */
	gpon_field(GPON_GTC_DS_LOS_CFG_STS, 0, 0, 1);	/* OPTIC_LOS_EN = 1     */
	gpon_field(GPON_GTC_DS_LOS_CFG_STS, 1, 1, 1);	/* OPTIC_LOS_POLAR = 1  */
	pr_debug("luna-gpon: optical-LOS monitor enabled (los_cfg=0x%08x)\n",
		gpon_rd(GPON_GTC_DS_LOS_CFG_STS));

	proc_create_single("gpon", 0444, NULL, gpon_proc_show);
	proc_create_single("gpon_log", 0444, NULL, luna_log_show);
	proc_create_single("gpon_timing", 0444, NULL, luna_timing_show);
	luna_mon_register();
	proc_create_single("pidump", 0444, NULL, pidump_proc_show);
	proc_create_single("swdump", 0444, NULL, swdump_proc_show);
#ifdef CONFIG_GPON_OLT_DIAG
	proc_create_single("oltcap", 0444, NULL, oltcap_proc_show);
	proc_create_single("gpon_prov", 0444, NULL, gpon_prov_proc_show);
	proc_create_single("gpon_unknown", 0444, NULL, gpon_unknown_proc_show);
	proc_create_single("ploam_unknown", 0444, NULL, ploam_unknown_proc_show);
	proc_create_single("ploamcap", 0444, NULL, ploamcap_proc_show);
#endif

	/* Upstream burst CONFIG + laser-enable timing. */
	/* Only the burst-enable polarity now (the rest waits for activation): with
	 * 0 here BEN idled ACTIVE until activation, and a board that cannot reach
	 * activation kept its laser on -- the V2801RGW jammed GPON2 in O1. */
	gpon_wr_us_protected(GPON_GTC_US_CFG, GPON_US_CFG_BEN_POLAR);
	pr_debug("luna-gpon: US_CFG burst-enable polarity set at init: 0x%08x\n",
		gpon_rd(GPON_GTC_US_CFG));
	if (force_laser)
		pr_info("luna-gpon: force_laser=1 -> US_CFG.FS_LON set (CW diagnostic)\n");
	/* US GEM-header PTI vector (not write-protected). Reset is 0, so every US GEM
	 * frame would carry PTI=000 even on end-of-fragment, and some OLTs only
	 * accept OMCI with NON_END_FRAG=0 and END_FRAG=1. Stock's vector (0,1,0,1)
	 * is 0x00001010, with FS_GEM_IDLE[31]=0 keeping auto-idle. */
	gpon_wr(GPON_GEM_US_PTI_CFG, 0x00001010u);	/* FS_GEM_IDLE(bit31) = 0, as stock */
	gpon_wr_us_protected(GPON_GTC_US_LASER, gtune->us_laser);	/* per chip: 0x2028 / 0x1820 */

	/* The laser bias is loaded LOW for acquisition */
	gpon_field(GPON_GTC_DS_ONU_ID_STATUS, 15, 8, 0xff);	/* DS ONU-ID = broadcast */
	gpon_field(GPON_GTC_US_ONU_ID, 15, 8, 0xff);		/* US ONU-ID = broadcast */
	/*
	 * DS_CFG's FEC detection threshold, asserted deliberately: the
	 * bootloader leaves FEC_DET_THRSH=1 where stock rests at 0, and the
	 * vendor sets it from configuration in its init path. Not a BWmap
	 * acceptance gate.
	 */
	gpon_field(0x1014, 3, 1, 0);				/* DS_CFG FEC_DET_THRSH = 0 (stock) */
	gpon_field(0x1014, 11, 11, 0);				/* DS_CFG BWM_NO_FLT = 0 (stock). =1 ("accept all
								 * grants") leaves bwm_acpt=0 against stock's 130k+:
								 * it breaks the BWMAP parser rather than relaxing
								 * it. */
	gpon_wr(GPON_GTC_US_PLOAM_CFG, 0);
	gpon_wr(GPON_GTC_US_WRITE_PROTECT, GPON_US_WP_UNLOCK);
	gpon_field(GPON_GTC_US_PROC_MODE, 0, 0, 0);
	gpon_wr(GPON_GTC_US_WRITE_PROTECT, GPON_US_WP_LOCK);
	gpon_field(GPON_GTC_DS_PLOAM_CFG, 9, 9, 1);		/* DS PLOAM BC_ACCEPT */
	gpon_field(GPON_GTC_DS_PLOAM_CFG, 8, 8, 1);		/* DS PLOAM ONUID_FILTER on. Every Assign_ONU-ID this OLT sends is
		 * broadcast, so BC_ACC_EN already delivers it; the filter keeps another
		 * ONU's PLOAMs on the same splitter out. Stock reads GPON_GTC_DS_PLOAM_CFG
		 * = 0x70b with the bit set. */
	/* (6) DS_INTR_MASK = 0x070f, the O5 operating value: LOS/LOF/FEC/LOM (b0-3)
	 * plus SN_REQ(b8) / RNG_REQ(b9) / PLM_BUF(b10), which gate the GTC's
	 * upstream serial-number, ranging and PLOAM-buffer event handling. The MAC
	 * reset clears the register, so it must be re-set here. */
	gpon_wr(GPON_GTC_DS_INTR_MASK, 0x070f);

	/* Upstream burst TIMING. */
	gpon_wr_us_protected(GPON_GTC_US_MIN_DELAY, 0x9132);
	/* An __init caller of the equalization-delay computation, through the core's
	 * gpon_ploam_set_eqd(). Do NOT satisfy it by copying the arithmetic back in
	 * here -- that fork is what killed gpon_proto.c. */
	gpon_set_eqd(0);			/* pre-ranging EqD = 290*128 = 0x9100 */

	/* O5 grant-burst optical config */
	gpon_wr_us_protected(0x5188, gtune->us_optic_sd_th);	/* US_OPTIC_SD_TH: per chip (9602C live 0x00504bfa, 9603CVD live 0x00a07fff) */
	gpon_field(0x526c, 0, 0, 1);			/* US_PWR_SAV_MODE.PWR_SAV_MODE = 1 (live stock = 1) */
	if (gtune->us_pwr_sav_dg_tx_opt)
		gpon_field(0x526c, 16, 16, 1);		/* DG_TX_OPT: 9603CVD live 0x00010001 */
	gpon_wr(GPON_GEM_US_PWR_SAV_CFG, (0x10u << 16) | 0x100u);	/* GEM_US_PWR_SAV_CFG = 0x00100100 (live stock) */
	gpon_wr(GPON_GEM_US_EOB_MERGE, 0x00000028u);			/* GEM_US_EOB_MERGE = 0x28 (live stock; mine omitted) */

	/* Default upstream burst overhead (G.984.3) */
	gpon_apply_boh(false);

	/* US-FEED RE-ARM (stock dataPath_reset arm order): pbo_init ran early and the
	 * MAC/GTC bring-up above parked the US-feed FSM. Re-arm it now, the last
	 * datapath step before the FSM ranges. */
	gpon_us_feed_rearm();

	/* The SDS upstream-TX serializer registers are */
	gpon_parse_sn(onu_sn);

	/* Bring the core's FSM object up alongside ours */
	luna_ploam_cfg_live = luna_ploam_cfg;
	luna_ploam_cfg_live.hold = gpon_hold;
	luna_ploam_cfg_live.cdr_reseat_on_reactivate = true;
	luna_ploam_cfg_live.o5_rearm_burst_gate = true;
	luna_ploam_cfg_live.o3_feed_reset = false;
	luna_ploam_cfg_live.data_gem_en = true;
	luna_ploam_cfg_live.omcc_alt_bind = false;
	/* The OMCC Alloc-ID override: 0 means use the live ONU-ID. */
	luna_ploam_cfg_live.omcc_alloc_override = 0;
	/* The three tick tunables. */
	luna_ploam_cfg_live.trace = trace;
	luna_ploam_cfg_live.los_rerange_ticks = los_rerange_ticks;
	luna_ploam_cfg_live.o4_ranging_timeout_ticks = o4_ranging_timeout_ticks;
	luna_ploam_cfg_live.o5_provision_watchdog_ticks = o5_provision_watchdog_ticks;
	luna_ploam_cfg_live.o5_ungranted_ticks = o5_ungranted_ticks;
	luna_ploam_cfg_live.o5_ploam_keepalive_ticks = o5_ploam_keepalive_ticks;
	luna_ploam_cfg_live.early_dwell_report_ticks = early_dwell_report_ticks;
	/*
	 * The core hands back the name of any mandatory op still missing and
	 * refuses a half-wired table (GPON_MUST_CHECK). The check at the top of
	 * this function has already proved it complete, so this makes a future
	 * divergence a refusal instead of a warning.
	 */
	missing = gpon_ploam_init(&luna_ploam, &luna_ploam_ops,
				  &luna_ploam_cfg_live, NULL, gpon_sn_bytes);
	if (missing) {
		pr_err("luna-gpon: [SYS-017] gpon_ploam_init refused: mandatory op ->%s is NULL\n",
		       missing);
		return -EINVAL;
	}
	spin_lock_bh(&luna_pwd_lock);
	if (gpon_ploam_password_parse(onu_password, luna_pwd))
		pr_warn("luna-gpon: [SYS-018] onu_password '%s' is not a G.984.3 password; sending an empty one\n",
			onu_password);
	gpon_ploam_set_password(&luna_ploam, luna_pwd);
	luna_pwd_dirty = false;
	spin_unlock_bh(&luna_pwd_lock);
	if (gpon_sn_is_set(gpon_sn_bytes))
		pr_info("luna-gpon: PLOAM FSM start, SN '%s' = %*phN\n",
			onu_sn, 8, gpon_sn_bytes);
	else
		pr_debug("luna-gpon: PLOAM FSM start with NO serial provisioned -- parked at O1 until onu_sn is written\n");
	INIT_WORK(&gpon_cdr_reset_work, gpon_cdr_reset_worker);
	INIT_DELAYED_WORK(&gpon_optical_work, gpon_optical_work_fn);

	timer_setup(&gpon_fsm_timer, gpon_fsm_poll, 0);
	mutex_lock(&bosa_lock);
	luna_driver_ready = true;
	bosa_cal_error = luna_activate_locked(false);
	if (bosa_cal_error)
		pr_debug("luna-gpon: upstream inhibited, activation pending: %d\n", bosa_cal_error);
	luna_resume_poll();
	mutex_unlock(&bosa_lock);
	luna_gpon_irq_request();
	luna_health_proc = proc_create_single("gpon_health", 0444, NULL, luna_health_show);
	if (!luna_health_proc)
		pr_warn("luna-gpon: [SYS-019] cannot expose cached health status\n");
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
		pr_err("luna-gpon: [OPT-012] TX inhibit failed during unload\n");
	luna_driver_ready = false;
	/* Close the UNI enqueue gate before anything is cancelled, so a
	 * producer that is already running cannot re-arm behind the cancel. */
	luna_uni_apply_close();
	mutex_unlock(&bosa_lock);
	luna_fsm_stop();
	cancel_work_sync(&gpon_cdr_reset_work);	/* it masks the line, so before free_irq */
	if (luna_gpon_irq) {
		sw_field(SW_INTR_IMR, 10, 10, 0);
		sw_field(SW_INTR_IMR, 13, 13, 0);
		free_irq(luna_gpon_irq, NULL);
		cancel_work_sync(&luna_ds_ploam_work);
	}
	cancel_work_sync(&luna_omci_work);	/* no enqueue since luna_activation_ready fell */
	cancel_delayed_work_sync(&gpon_optical_work);
	/* OUTSIDE bosa_lock, and after the producers: the worker holds that
	 * mutex for its whole apply, so cancelling under it would deadlock. */
	cancel_delayed_work_sync(&luna_uni_apply_work);
	cancel_delayed_work_sync(&luna_diag_work);
	proc_remove(luna_health_proc);
	luna_health_proc = NULL;
	remove_proc_entry("gpon", NULL);
	remove_proc_entry("gpon_log", NULL);
	remove_proc_entry("gpon_timing", NULL);
	luna_mon_unregister();
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
MODULE_FIRMWARE("vsol/v2802rh-gn25l95.bin");
MODULE_FIRMWARE("vsol/v2802rh-gn25l95-lan-type");
