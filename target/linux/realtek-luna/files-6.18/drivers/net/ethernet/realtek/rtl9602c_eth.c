// SPDX-License-Identifier: GPL-2.0-only


#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/dma-mapping.h>
#include <linux/timer.h>
#include <linux/of.h>
#include <linux/of_net.h>	/* of_get_mac_address() -- the DT rung of the ladder */
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mii.h>	/* BMCR_* -- the standard MII bit names */
#include <linux/mdio.h>	/* the EEE registers */
#include <linux/interrupt.h>	/* request_irq, irqreturn_t, IRQF_SHARED */
#include <linux/delay.h>
#include <linux/devm-helpers.h>
#include <linux/gpio/consumer.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/ethtool.h>	/* the LAN jacks' link settings */
#include <linux/if_vlan.h>	/* the per-jack steering tag */
#include <linux/leds.h>		/* the board's PON/LOS lamps, handed to net/xpon */
#include <net/page_pool/helpers.h>
#include "luna_eth_regs.h"	/* the family MAC/switch register map + per-chip table */
#include "luna_gpon_regs.h"	/* the family's luna_sw_io(): a struct hwio over the SWCORE base */
#include "regtable.h"		/* flowcore: gpon_ind_poll() -- the ONE bounded busy-wait */
#include "gpon_omci_core.h"	/* the responder + omci_finalize + the AVC emitters */
#include "gpon_omci_trace.h"	/* G.988 decode-to-a-buffer for the log */
#include "gpon_omci_diag.h"
#include "gpon_omci_me.h"	/* the common OMCI ME store + context */
#include "rtl9602c_eth.h"
#include "rtl9601d_l2.h"
#include "luna_eth_mib.h"
#include "luna_gmac_logic.h"	/* family GMAC ring packings (flowcore) */
#include "gpon_hwaddr.h"	/* the ONE station-address ladder (drivers/net/gpon) */

/* US-OMCI TX settings (see rtl9602c_eth_omci_xmit) */
static const unsigned int omci_sid_idx = 4;	/* PON_SID one-hot shift (word3[28:23]); 4 = SID-64 classify slot (RX_SID group [4]) */
/* One netdev per LAN jack (the DT ethernet-ports node): the switch tells the
 * CPU which port a frame came in on (the RX descriptor's source port), and
 * this is how a frame the CPU sends reaches ONE jack. The switch's own
 * forwarding keys on the destination MAC: a plain frame from the CPU goes
 * wherever that MAC was learned, or to every LAN jack, so a bridge that sends
 * a copy per port would deliver each flood twice.
 *   0  plain: no steering, a copy per port floods to both jacks (duplicates)
 *   1  VLAN: each jack has a VLAN of its own, members the jack and the CPU,
 *      untagged on the jack; the CPU tags its copy with it
 *   2  CPU tag: Realtek's in-band 0x8899 tag with the jack's port mask
 * Writable at run time (/sys/module/rtl9602c_eth/parameters/lan_tx_steer);
 * the VLAN entries are installed at open whatever the mode. */
enum { LAN_STEER_PLAIN, LAN_STEER_VLAN, LAN_STEER_CPUTAG };
static int lan_tx_steer = -1;

static int rtl9602c_lan_tx_steer_set(const char *val, const struct kernel_param *kp)
{
	int mode, rc = kstrtoint(val, 0, &mode);

	if (rc)
		return rc;
	if (mode < -1 || mode > LAN_STEER_CPUTAG)
		return -EINVAL;
	WRITE_ONCE(*(int *)kp->arg, mode);
	return 0;
}

static const struct kernel_param_ops rtl9602c_lan_tx_steer_ops = {
	.set = rtl9602c_lan_tx_steer_set,
	.get = param_get_int,
};
module_param_cb(lan_tx_steer, &rtl9602c_lan_tx_steer_ops, &lan_tx_steer, 0644);
MODULE_PARM_DESC(lan_tx_steer,
		 "how a lanN netdev's frame is held to its own jack: -1=board default (V2802RH CPU tag, others VLAN), 0=plain (the switch floods), 1=per-jack VLAN, 2=in-band CPU tag");



/* ExtSpa / PON port (word3[22:16] = (port&0x7F)<<16). */
static const unsigned int omci_pon_port = 2;	/* ExtSpa / PON port (word3[22:16]) = our PON switch port */

/* Raw descriptor overrides. 0 = use the computed value. */
static const unsigned int omci_word2_ovr;
static const unsigned int omci_word3_ovr;

/*
 * HW ring for the US-OMCI descriptor ring. Real silicon's L2 switch strips an
 * inline 0x8899 tag, so US OMCI classifies only through the HW descriptor
 * (word3=0x00b20040, DST_SID 64).
 */
static const unsigned int omci_tx_ring = 0;	/* 0 = LAN ring; 4 = dedicated ring */

/* Doorbell override. 0xff = "auto": compute the kick from the SAME HW ring used
 * to arm TxFDP (h<4 -> R_IO_CMD bit h; h==4 -> R_IO_CMD1 |= 0x100). Any other
 * value forces an explicit R_IO_CMD bit number (debug only). */
static const unsigned int omci_doorbell_bit = 0xff;

/* GMAC bring-up mode. */
static const unsigned int gmac_reset = 1;	/* safe to reset: rtl9602c_uboot_swcore_bringup() re-runs U-Boot's GMAC<->switch resync after the IP-block reset, and the stock GMAC init re-establishes the GMAC0-TX->US-NIC direct link */





/* MSR (0x58) top byte. */
static const unsigned int msr_top = 0x10;

/* SW_MAC_CPU_TAG_CTRL (0x23030) = 0x300 */
static const unsigned int sw_tagaware = 0x300;

/* Recovery flavor */
static const unsigned int recover_rst = 1;

/* GMAC register offsets (from the NIC base). */
#define R_TxFDP1	0x1300	/* TX ring0 fetch-descriptor pointer (phys) */
#define R_TxCDO1	0x1304	/* TX ring0 current-descriptor offset (u16) */
/* Per-ring TX descriptor {FDP,CDO} pairs, 16-byte stride, NOT 8: stock's ring
 * init writes 0x1300/0x1310/0x1320/0x1330/0x1340 for rings 0..4.  The US-OMCI
 * path uses HW ring 4 (base 0x1340, kick R_IO_CMD1 |= 0x100); h is derived once
 * so the FDP arm and the doorbell can never name different rings. */
#define R_TxFDP(k)	(0x1300 + (k) * 16)	/* ring k fetch-descriptor pointer (stock stride 16) */
#define R_TxCDO(k)	(0x1304 + (k) * 16)	/* ring k current-descriptor offset (u16) */
/* RX multi-ring config block at 0x1380 + k*16 (RxFDP2 region), rings 1..5; ring 0
 * is the RxFDP/RxCDO pair below.  NOT a TX table -- a TX ring activates on TxFDP
 * + OWN(opts1) + kick.  Kept only to document the address. */
#define R_RxMRingCfg(k)	(0x1380 + (k) * 16)	/* RX multiring k config block (stock stride 16) */
#define R_RxFDP		0x13F0	/* RX ring0 fetch-descriptor pointer (phys) */
#define R_RxCDO		0x13F4	/* RX ring0 current-descriptor offset */
/*
 * RX multiring k's {FDP, CDO} pair sits at +0/+4 of its 16-byte block, the
 * layout ring 0 has at R_RxFDP/R_RxCDO and every TX ring has at
 * R_TxFDP/R_TxCDO. Read only, by the /proc dump.
 */
#define R_RxMRingCDO(k)	(R_RxMRingCfg(k) + 4)	/* [RxCDO:RxRingSize] of ring k */
static_assert(R_RxMRingCDO(1) == 0x1394 && R_RxMRingCDO(5) == 0x13d4,
	      "the RX multiring block moved");
#define R_MSR		0x58	/* media/flow status; top byte = force flow ctl (msr_top) */
/* The GMAC0 MAC-level MIB counters are one array. */
#define R_MIB16(n)	(0x10 + 2 * (n))
#define R_TXOKCNT	R_MIB16(0)	/* word = [TXOKCNT:RXOKCNT] */
#define R_TXERR		R_MIB16(2)	/* word = [TXERR:RXERR]     */
#define R_MISSPKT	R_MIB16(4)	/* word = [MISSPKT:FAE]     */
#define R_RXOKPHY	R_MIB16(8)	/* word = [RXOKPHY:RXOKBRD] */
#define R_RXOKMUL	R_MIB16(10)	/* word = [RXOKMUL:TXABT]   */
static_assert(R_TXOKCNT == 0x10 && R_TXERR == 0x14 && R_MISSPKT == 0x18 &&
	      R_RXOKPHY == 0x20 && R_RXOKMUL == 0x24, "the GMAC MIB array moved");
/* Live-stock 9602C operating values (read off a running stock ONU at O5). The
 * U-Boot value 0x400f3330 is its polled-TFTP config; the stock OS reprograms
 * IO_CMD/IO_CMD1 after the IP-block reset. Bits[3:0] of IO_CMD stay 0 here (the
 * self-clearing per-ring TX_POLL kicks). */
#define IOCMD_STOCK	0xc059f130
#define IOCMD1_STOCK	0x32000001
#define IOCMD_UBOOT	0x400F3330
#define IOCMD1_UBOOT	0x323F0001
/* SoC per-IP enable */
#define R_RRING_ROUTING1 0x1370	/* RX-ring routing by priority: PRI_n_ROUTE = ring# at nibble n (operational default 0x65432100). 0 => all priorities to ring 0. */

/* Descriptor opts1 bits (shared TX/RX where noted). */
#define D_IPCS		BIT(27)	/* TX: insert IPv4 csum */

/* TX cpu-tag DESCRIPTOR path: UNUSED */

#define OMCI_RESV	2	/* LAN xmit stops this many slots early so the sparse shared-ring OMCI inject always has room (never dropped) */
#define DUMMY_RING_SIZE	4	/* idle filler armed on the unused HW TX rings (gap rings) */
#define RX_CPU_PREFIX	2	/* switch CPU-port prepends a 2-byte offset word on RX */
#define POLL_INTERVAL	msecs_to_jiffies(2)	/* legacy pure-poll fallback (ep->irq<=0) */
#define REKICK_INTERVAL	msecs_to_jiffies(100)	/* slow TX-unpark backstop when IRQ-driven */

/* struct rx_desc / struct tx_desc are the FAMILY's */
#define SW_FORCE_P_ABLTY(ep, p)	((ep)->swm->force_ablty + ((p) << 2))
#define SW_ABLTY_FORCE_MODE(ep, p)	((ep)->swm->ablty_force + ((p) << 2))

/* The indirect PHY window has ONE owner on this shell too */
static DEFINE_MUTEX(rtl9602c_gphy_lock);

/* The UNI administrative lock, as a DESIRED state per port: OMCI's (the
 * OLT's Administrative State) and the netdev's (a lanN taken down). Either
 * holds the jack's link down; the hardware is driven from the union. */
static u32 rtl9602c_uni_locked_ports;	/* OMCI */
static u32 rtl9602c_uni_down_ports;	/* ip link set lanN down */

static bool rtl9602c_uni_port_locked(unsigned int port)
{
	return port < 32 &&
	       ((READ_ONCE(rtl9602c_uni_locked_ports) |
		 READ_ONCE(rtl9602c_uni_down_ports)) & BIT(port));
}
/* P_MISC, the per-port misc word */
#define SW_SYS_LRN_LIMITNO	0x17018	/* system MAC-learn limit [10:0]; 0 = no learning */
/* Registers realtek,stock-switch-config sets; names are Realtek's (G6 reg list). */
#define SW_ACCEPT_MAX_LEN_CTRL(p)	(0x11008 + 4 * (p))	/* [1:0] max-length profile */
#define SW_MAX_LENGTH_CFG1		0x11018
#define SW_MAX_LENGTH_CFG0		0x23034	/* [13:0] bytes */
#define SW_LUT_LRN_LIMITNO(p)		(0x17008 + 4 * ((p) / 2))	/* 11 bits per port */
#define SW_LUT_LRN_LIMIT_MAX		2047	/* the reset value: no limit */
#define SW_LUT_LEARN_OVER_CTRL		0x1c00c	/* 2 bits per port: 0 forward, 1 drop */
#define SW_LUT_AGE_DEFAULT		3000	/* [20:0] in 0.1 s; stock's 300 s */
#define SW_LAN_MAX_FRAME		1600	/* stock's LAN and CPU limit */
#define SW_PON_MAX_FRAME		2031	/* stock's PON limit */
#define SW_FC_PON_GLB_HI_TH		0x2311c
#define SW_FC_PON_GLB_LO_TH		0x23120
#define SW_STORM_CTRL_UM_CTRL		0x1703c	/* per-port enable, likewise UC/BC/DHCP */
#define SW_STORM_CTRL_BC_CTRL		0x17048
#define SW_STORM_CTRL_DHCP_CTRL		0x17060
#define SW_STORM_CTRL_UM_METER_IDX	0x1704c	/* 4-bit meter index per port */
#define SW_STORM_CTRL_UC_METER_IDX	0x17050
#define SW_STORM_CTRL_BC_METER_IDX	0x17058
#define SW_STORM_CTRL_DHCP_METER_IDX	0x1706c
#define SW_METER_GLB_CTRL(m)		(0x25004 + 8 * (m))	/* two words per meter */
#define SW_METER_PKT_RATE		0x25088	/* [m] meter m counts packets */
#define SW_IGR_BWCTRL_P_CTRL		0x20010
#define SW_EGR_BWCTRL_P_CTRL		0x2d804
#define UNI_RATE_MAX			0x7ffffu
#define SW_FIB_REG0			0x22c00	/* the fibre PHY's BMCR */
#define SW_DLF_ACT_TRAP2CPU	2
/* Forced ability value: 1000M (speed[1:0]=2) + full duplex (b2) + link-up (b4) */
#define SW_ABLTY_1G_FD_UP	(0x2 | BIT(2) | BIT(4))
/* A SerDes port as the V2802RH's stock forces it: the 1G code (HSGMII runs
 * the 2.5G link on it), full duplex, link up, TX+RX pause (b5/b6). */
#define SW_ABLTY_SERDES_UP	(SW_ABLTY_1G_FD_UP | BIT(5) | BIT(6))
/* MAC_CPU_TAG_CTRL */
#define SW_FC_P_LO_TH		0x230F0
#define SW_FC_P_FCOFF_HI_TH	0x230F4
#define SW_FC_P_FCOFF_LO_TH	0x230F8
#define SW_TAG_AWARE		BIT(9)
#define SW_TRAP_TAG_INSERT_EN	BIT(8)
/* Operational value: VLAN_CTRL=0x19 (filtering + VID0/VID4095 type bits). */
#define SW_VLAN_CTRL_VAL	0x19	/* filtering on at init: required for reliable
					 * onlining during ranging and config-apply.
					 * luna_lan_keep_open_poll() later sets bit0 from
					 * luna_vlan_filter_ok. */
/* The PON port's own VLAN (members PON + CPU), as stock does (PON 4025, LAN 4005): a WAN
 * broadcast then floods to the CPU only, never to a LAN port. 0 = the PON shares VID 1. */
uint rtl9602c_pon_vid = 4025;
module_param_named(pon_vid, rtl9602c_pon_vid, uint, 0444);
MODULE_PARM_DESC(pon_vid, "VLAN of the PON port, apart from the LAN's (default 4025 as stock; 0 = shared VID 1)");
#define SW_WAN_PVID	(rtl9602c_pon_vid ? rtl9602c_pon_vid : SW_DEFAULT_VID)
#define SW_TBL_VLAN_WR(vid)	(BIT(31) | (((vid) & 0xfff) << 9) | (1u << 4) | (1u << 3) | 1u)
#define CPUTAG1_OMCI_SID(s)	(((s) & 0x7f) << 8)	/* R_CPUTAG1CR[14:8] */
#define CPUTAG1_B1		0x2	/* bit1: live 9602C stock reads CPUTAG1CR=0x4002; not present in the 9607C register map */
/*
 * Stock does not OR in 0x4070: a live 9602C devmem read gives CPUTAG1CR =
 * 0x4002.
 */
#define CPUTAG1_LOW		0x02	/* live-stock ref ONU devmem: CPUTAG1CR = 0x4002 */


/* netdev_priv of a LAN jack's netdev */
struct rtl9602c_lan {
	struct rtl9602c_eth *ep;
	unsigned int	port;		/* switch port */
};

static struct rtl9602c_eth *g_ep;	/* single-instance, for /proc diag */

static int rtl9602c_eth_hw_ensure(struct rtl9602c_eth *ep);
static int rtl9602c_lan_gate(struct rtl9602c_eth *ep, unsigned int port, bool down);

/* The GMAC accepts every destination (RCR AcceptAllPhys) while ANY netdev
 * riding it is promiscuous: a bridge enslaves lan1, lan2 and pon0 -- and
 * eth0 on a configuration from before the jacks had netdevs of their own. */
static bool rtl9602c_eth_want_promisc(const struct rtl9602c_eth *ep)
{
	unsigned int p;

	if (ep->ndev->flags & (IFF_PROMISC | IFF_ALLMULTI))
		return true;
	if (ep->wan_ndev && (ep->wan_ndev->flags & (IFF_PROMISC | IFF_ALLMULTI)))
		return true;
	for (p = 0; p < LAN_PORTS; p++)
		if (ep->lan_ndev[p] &&
		    (ep->lan_ndev[p]->flags & (IFF_PROMISC | IFF_ALLMULTI)))
			return true;
	return false;
}

static inline u32 ep_rd(struct rtl9602c_eth *ep, u32 r) { return ioread32(ep->base + r); }
static inline void ep_wr(struct rtl9602c_eth *ep, u32 r, u32 v) { iowrite32(v, ep->base + r); }

/*
 * The LAN SerDes: port 0 to the external PHY. This is its mode register. [4:0]
 * is the SerDes mode in the family's coding (0x02 SGMII, 0x12 HSGMII, 0x1f
 * off), and 0x1f, every bit set, is also what a die with no SerDes port shows:
 * the UNI gate reads it as PHY_PWRDN_GUARD.
 *
 * The sequences below are the stock U-Boot's sdsmode command (2011.12.NA for
 * rtl9602C: sds_sgmii at 0x80d80854, sds_hisgmii at 0x80d808fc, read from the
 * V2802RH's flash) and are, register for register, what the stock RTOS's
 * _rtl9602c_lan_sds_mode_set writes for its modes 3 (SGMII) and 5 (HSGMII):
 * park the mode, the analog block, the mode, the forced ability, a reset pulse,
 * one misc word. U-Boot runs them only when the command is typed, so a Linux
 * that does not run them leaves the SerDes at its reset defaults, which link
 * and carry traffic, but not at the link's rate. The stock RTOS's link task
 * re-runs them, with the PHY's own SerDes option, whenever the copper speed
 * crosses 1G (rtl9602c_port0_link_poll).
 */
#define SW_LAN_SDS_MODE		0x00214
#define   LAN_SDS_MODE_MASK	0x1f
#define   LAN_SDS_MODE_OFF	0x1f
#define   LAN_SDS_MODE_SGMII	0x02
#define   LAN_SDS_MODE_HSGMII	0x12
#define SW_LAN_SDS_RST		0x21500	/* 0x10, 0x30, 0 after a mode change */
#define SW_LAN_SDS_MISC		0x2d8b8
#define   LAN_SDS_MISC_SGMII	0x0004ffff
#define   LAN_SDS_MISC_HSGMII	0x000a8000
#define SW_PHY_PWRDN_GUARD	SW_LAN_SDS_MODE
#define   PHY_PWRDN_GUARD_OK	LAN_SDS_MODE_OFF	/* no SerDes running: port 0 may be powered down */

struct rtl9602c_sds_reg { u32 off, val; };
static const struct rtl9602c_sds_reg lan_sds_sgmii_analog[] = {
	{ 0x21680, 0x0000 }, { 0x2168c, 0x3001 }, { 0x21698, 0x0068 },
	{ 0x215b8, 0x98c5 }, { 0x215b4, 0x1264 }, { 0x215bc, 0x400f }, { 0x215e4, 0xfc00 },
};
static const struct rtl9602c_sds_reg lan_sds_hsgmii_analog[] = {
	{ 0x21700, 0x1800 }, { 0x2170c, 0x7b01 }, { 0x21718, 0x0168 },
	{ 0x215b8, 0x98c5 }, { 0x215b4, 0x1564 }, { 0x215bc, 0x400f }, { 0x215e4, 0xfc00 },
};

/* The RTL8221B's side, as stock's rtl8226 library sets it
 * (Rtl8226_serdes_option_set, MMD 30): option 1 with a 1G copper link, 3 with
 * a 2.5G one, each after a reset pulse on the PHY's reset line. */
#define MDIO_MMD_VEND1_RTL		30
#define RTL8221B_VND1_SERDES_OPTION	0x697a	/* [5:0] the option; bit 6 cleared with it */
#define RTL8221B_VND1_SERDES_CTRL	0x75f3	/* bit 0 cleared for option 1 */
#define RTL8221B_VND1_SERDES_MODE	0x7580	/* [4:0] 0x02 SGMII, 0x12 HSGMII, 0x16 2500BASE-X, 0x0d USXGMII */
#define RTL8221B_SDS_OPTION_SGMII	1
#define RTL8221B_SDS_OPTION_HSGMII	3

static int ext_mdio_read_c45(struct rtl9602c_eth *ep, u8 dev, u16 reg);
static void ext_mdio_write_c45(struct rtl9602c_eth *ep, u8 dev, u16 reg, u16 val);
static int rtl9602c_lan_phy_read__locked(struct rtl9602c_eth *ep, unsigned int port, u8 reg);
static int rtl9602c_lan_phy_write__locked(struct rtl9602c_eth *ep, unsigned int port, u8 reg, u16 val);
static void rtl9602c_uni_rate_replay(struct rtl9602c_eth *ep);

/* The SoC side of one mode. The caller holds what serialises the switch
 * ability registers: the gphy lock, or the bring-up before the rings run. */
static void rtl9602c_lan_sds_program(struct rtl9602c_eth *ep, bool hsgmii)
{
	const struct rtl9602c_sds_reg *a = hsgmii ? lan_sds_hsgmii_analog : lan_sds_sgmii_analog;
	unsigned int i, n = hsgmii ? ARRAY_SIZE(lan_sds_hsgmii_analog) : ARRAY_SIZE(lan_sds_sgmii_analog);
	u32 v = SW_ABLTY_SERDES_UP & ~ep->port0_pause_off;

	iowrite32(LAN_SDS_MODE_OFF, ep->sw + SW_LAN_SDS_MODE);
	for (i = 0; i < n; i++)
		iowrite32(a[i].val, ep->sw + a[i].off);
	iowrite32(hsgmii ? LAN_SDS_MODE_HSGMII : LAN_SDS_MODE_SGMII, ep->sw + SW_LAN_SDS_MODE);
	if (ep->ext_phy_ok || rtl9602c_uni_port_locked(0))
		v &= ~ABLTY_LINK;
	iowrite32(v, ep->sw + SW_FORCE_P_ABLTY(ep, 0));
	iowrite32(0xfff, ep->sw + SW_ABLTY_FORCE_MODE(ep, 0));
	iowrite32(0x10, ep->sw + SW_LAN_SDS_RST);
	iowrite32(0x30, ep->sw + SW_LAN_SDS_RST);
	iowrite32(0x00, ep->sw + SW_LAN_SDS_RST);
	iowrite32(hsgmii ? LAN_SDS_MISC_HSGMII : LAN_SDS_MISC_SGMII, ep->sw + SW_LAN_SDS_MISC);
	ep->lan_sds_hsgmii = hsgmii;
	ep->lan_sds_set = true;
	rtl9602c_uni_rate_replay(ep);
}

static int rtl9602c_gphy_wait(struct rtl9602c_eth *ep)
{
	int i;

	for (i = 0; i < 10000; i++) {
		if (!(ioread32(ep->sw + SW_GPHY_IND_RD) & GPHY_IND_BUSY))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

/* -> 0 and *out set, or negative with *out LEFT EXACTLY AS IT WAS: an all-ones
 * MII read is a plausible register value, so a failure must not return one. */
static int rtl9602c_gphy_ocp_read__locked(struct rtl9602c_eth *ep, unsigned int phyad,
					  u16 ocp, u16 *out)
{
	u32 adr = GPHY_IND_PHY(phyad) | ocp;

	/*
	 * Preflight: a command issued while the previous transaction is still
	 * busy makes the window answer for the wrong one.
	 */
	if (rtl9602c_gphy_wait(ep))
		return -ETIMEDOUT;
	iowrite32(adr | GPHY_IND_EN, ep->sw + SW_GPHY_IND_CMD);
	if (rtl9602c_gphy_wait(ep))
		return -ETIMEDOUT;
	*out = ioread32(ep->sw + SW_GPHY_IND_RD) & 0xffff;
	return 0;
}

static int rtl9602c_gphy_ocp_write__locked(struct rtl9602c_eth *ep, unsigned int phyad,
					   u16 ocp, u16 val)
{
	u32 adr = GPHY_IND_PHY(phyad) | ocp;

	/*
	 * Preflight before the data register: the value would otherwise be
	 * consumed by a transaction still in flight.
	 */
	if (rtl9602c_gphy_wait(ep))
		return -ETIMEDOUT;
	iowrite32(val, ep->sw + SW_GPHY_IND_WD);
	iowrite32(adr | GPHY_IND_WREN | GPHY_IND_EN, ep->sw + SW_GPHY_IND_CMD);
	return rtl9602c_gphy_wait(ep);
}

static int rtl9602c_gphy_read__locked(struct rtl9602c_eth *ep, unsigned int phyad,
				      unsigned int reg, u16 *out)
{
	return rtl9602c_gphy_ocp_read__locked(ep, phyad, GPHY_MII_PAGE | ((reg & 0xf) << 1), out);
}

static int rtl9602c_gphy_write__locked(struct rtl9602c_eth *ep, unsigned int phyad,
				       unsigned int reg, u16 val)
{
	return rtl9602c_gphy_ocp_write__locked(ep, phyad, GPHY_MII_PAGE | ((reg & 0xf) << 1), val);
}

/* The boot loader's own command words, issued with */
static int rtl9602c_gphy_raw_cmd__locked(struct rtl9602c_eth *ep, u32 cmd)
{
	if (rtl9602c_gphy_wait(ep))
		return -ETIMEDOUT;
	iowrite32(cmd, ep->sw + SW_GPHY_IND_CMD);
	return rtl9602c_gphy_wait(ep);
}

static int rtl9602c_gphy_raw_write__locked(struct rtl9602c_eth *ep, u16 val, u32 cmd)
{
	if (rtl9602c_gphy_wait(ep))
		return -ETIMEDOUT;
	iowrite32(val, ep->sw + SW_GPHY_IND_WD);
	return rtl9602c_gphy_raw_cmd__locked(ep, cmd);
}

static inline unsigned int tx_slot(struct rtl9602c_eth *ep, unsigned int counter)
{
	return (ep->tx_rot + counter) % TX_RING_SIZE;
}

static inline unsigned int otx_slot(struct rtl9602c_eth *ep, unsigned int counter)
{
	return (ep->otx_rot + counter) % OTX_RING_SIZE;
}

static inline unsigned int tx_eor_slot(struct rtl9602c_eth *ep)
{
	return TX_RING_SIZE - 1;	/* wrap descriptor: fixed physical slot */
}

static inline unsigned int otx_eor_slot(struct rtl9602c_eth *ep)
{
	return OTX_RING_SIZE - 1;
}

/* Align the SW producers to the live engine positions. */
static void rtl9602c_tx_align(struct rtl9602c_eth *ep)
{
	ep->tx_rot = ioread16(ep->base + R_TxCDO1) % TX_RING_SIZE;
	ep->otx_rot = 0;
	ep->dbg_rearm++;
}

/* Arm the GMAC OMCI trap so DS frames on stream-id `sid` */
void rtl9602c_eth_set_omci_sid(unsigned int sid)
{
	struct rtl9602c_eth *ep = g_ep;

	if (!ep)
		return;
	/* cpu-tag (CPUTAGCR/CPUTAG1CR) is armed once in open() at */
	{
		unsigned int r;
		for (r = 0; r < 7; r++)
			ep_wr(ep, R_RRING_ROUTING1 + r * 4, 0);
	}
	ep->omci_trap_on = true;
	netdev_dbg(ep->ndev, "OMCI trap armed: SID %u (cpu-tag CPUTAGCR/CPUTAG1CR armed at open(); RX-ring routing zeroed)\n",
		    sid);
}
EXPORT_SYMBOL(rtl9602c_eth_set_omci_sid);

/* Provision the ONU identity */
void rtl9602c_eth_set_omci_identity(const u8 *sn8)
{
	luna_omci_set_sn(sn8);
}
EXPORT_SYMBOL(rtl9602c_eth_set_omci_identity);

/* DS OMCI frames that actually reached the CPU NIC ring (private dbg counter). */
u32 rtl9602c_eth_omci_rx_count(void)
{
	struct rtl9602c_eth *ep = g_ep;

	return ep ? ep->dbg_omci_rx : GPON_OMCI_RX_UNAVAIL;
}
EXPORT_SYMBOL(rtl9602c_eth_omci_rx_count);

/* gpon0 (WAN) RX packet count */
u32 rtl9602c_eth_wan_rx_count(void)
{
	struct rtl9602c_eth *ep = g_ep;

	return (ep && ep->wan_ndev) ? (u32)ep->wan_ndev->stats.rx_packets
				    : GPON_OMCI_RX_UNAVAIL;
}
EXPORT_SYMBOL(rtl9602c_eth_wan_rx_count);

/* US-OMCI TX-ring "dirty" cursor = number of OMCC */
u32 rtl9602c_eth_omci_tx_dirty(void)
{
	struct rtl9602c_eth *ep = g_ep;

	if (!ep)
		return 0;
	return ep->tx_dirty;
}
EXPORT_SYMBOL(rtl9602c_eth_omci_tx_dirty);

/* Minimal switch bring-up */
void rtl9602c_sw_tbl_pause(void)
{
	udelay(1);
}

static int rtl9602c_vlan_write(struct rtl9602c_eth *ep, u16 vid, u32 mbr, u32 untag, u32 ext,
			       bool ivl)
{
	struct hwio io = luna_sw_io(ep->sw);
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&ep->sw_table_lock, flags);
	rc = gpon_ind_poll(&io, reg_make(SW_TBL_STS), SW_TBL_BUSY,
                    SW_TBL_TRIES, rtl9602c_sw_tbl_pause);
	if (!rc) {
		iowrite32((ext << 11) | (ivl ? SW_VLAN_IVL : 0) | (untag << 4) | mbr,
			  ep->sw + SW_TBL_WRDATA);
		iowrite32(SW_TBL_VLAN_WR(vid), ep->sw + SW_TBL_CTRL);
		rc = gpon_ind_poll(&io, reg_make(SW_TBL_STS), SW_TBL_BUSY,
                     SW_TBL_TRIES, rtl9602c_sw_tbl_pause);
	}
	spin_unlock_irqrestore(&ep->sw_table_lock, flags);
	return rc;
}

/* The values the V2802RH's stock RTOS runs with (docs/v2802rh/
 * STOCK-REGISTER-CHECKLIST.md): storm control on meters 8-15, its frame
 * length limits, LUT_CFG bits and the unused fibre PHY powered down. */
static void rtl9602c_sw_stock_config(struct rtl9602c_eth *ep)
{
	void __iomem *sw = ep->sw;
	int i;

	for (i = 8; i < 16; i++) {
		iowrite32(0x100, sw + SW_METER_GLB_CTRL(i));
		iowrite32(0x27ffe, sw + SW_METER_GLB_CTRL(i) + 4);
	}
	iowrite32(BIT(10), sw + SW_METER_PKT_RATE);	/* the broadcast meter */
	iowrite32(0xd99, sw + SW_STORM_CTRL_UM_METER_IDX);
	iowrite32(0xfbb, sw + SW_STORM_CTRL_UC_METER_IDX);
	iowrite32(0xeaa, sw + SW_STORM_CTRL_BC_METER_IDX);
	iowrite32(0xc88, sw + SW_STORM_CTRL_DHCP_METER_IDX);
	iowrite32(0x7, sw + SW_STORM_CTRL_UM_CTRL);
	/* Not on the PON: its meters run at 0x100 x 8 Kbps, about 2 Mbit/s, and
	 * downstream frames to LAN clients the switch has not learned in the
	 * service VLAN (all of them while the CPU forwards) are unknown unicast. */
	iowrite32(0x7 & ~BIT(ep->swm->pon_port), sw + SW_STORM_CTRL_UC_CTRL);
	if (of_machine_is_compatible("vsol,v2802rh") && v2802_l2_storm(ep, false))
		dev_warn(ep->dev, "[L2-002] CPU fallback storm control readback failed\n");
	iowrite32(0x7, sw + SW_STORM_CTRL_BC_CTRL);
	iowrite32(0x7, sw + SW_STORM_CTRL_DHCP_CTRL);

	/* As stock once ranged: 1600 bytes on the LAN and CPU ports, 2031 on
	 * the PON. Its offline values point the PON at a 0-length profile. */
	iowrite32(2031, sw + SW_MAX_LENGTH_CFG0);
	iowrite32(1600, sw + SW_MAX_LENGTH_CFG1);
	iowrite32(3, sw + SW_ACCEPT_MAX_LEN_CTRL(0));
	iowrite32(3, sw + SW_ACCEPT_MAX_LEN_CTRL(1));
	iowrite32(3, sw + SW_ACCEPT_MAX_LEN_CTRL(ep->swm->cpu_port));
	iowrite32(0, sw + SW_ACCEPT_MAX_LEN_CTRL(ep->swm->pon_port));

	iowrite32(ioread32(sw + SW_LUT_CFG) | BIT(28) | BIT(26) | BIT(23),
		  sw + SW_LUT_CFG);

	/* Stock's PON flow-control watermarks, as it runs them once ranged. */
	iowrite32(0x02300224, sw + SW_FC_PON_GLB_HI_TH);
	iowrite32(0x02160207, sw + SW_FC_PON_GLB_LO_TH);
	iowrite32(ioread32(sw + SW_IGR_BWCTRL_P_CTRL) | BIT(1),
		  sw + SW_IGR_BWCTRL_P_CTRL);
	iowrite32(ioread32(sw + SW_FIB_REG0) | BMCR_PDOWN, sw + SW_FIB_REG0);
}

/* XZ005-G6 SDK: ingress rate is bits [20:2], egress [19:1],
 * both in 8-kbit/s units. Ingress lives in the per-port MAC block;
 * egress is a packed word array. Preserve IFG/flow-control and reserved bits. */
static int rtl9602c_uni_rate_write(struct rtl9602c_eth *ep, unsigned int port,
				 u32 down, u32 up)
{
	void __iomem *igr = ep->sw + SW_IGR_BWCTRL_P_CTRL + ep->swm->macpp_stride * port;
	void __iomem *egr = ep->sw + SW_EGR_BWCTRL_P_CTRL + 4 * port;
	bool hsgmii = port == 0 && ep->port0_serdes && ep->lan_sds_hsgmii;
	u32 old_line = hsgmii ? ioread32(ep->sw + SW_LAN_SDS_MISC) : 0;
	u32 line = down == UNI_RATE_MAX ? LAN_SDS_MISC_HSGMII & UNI_RATE_MAX :
		min_t(u32, down >> 1, LAN_SDS_MISC_HSGMII & UNI_RATE_MAX);
	u32 want_line = (old_line & ~UNI_RATE_MAX) | max_t(u32, line, 1);
	u32 old_igr = ioread32(igr), old_egr = ioread32(egr);
	u32 want_igr = (old_igr & ~GENMASK(20, 2)) | (up << 2);
	u32 want_egr = (old_egr & ~GENMASK(19, 1)) | (down << 1);

	iowrite32(want_igr, igr);
	iowrite32(want_egr, egr);
	/* Hardware trials: 6250 gives a nominal 100-Mbit/s HSGMII cap.
	 * The dedicated path uses 16-kbit/s steps, twice the normal field. */
	if (hsgmii)
		iowrite32(want_line, ep->sw + SW_LAN_SDS_MISC);
	if ((hsgmii && ioread32(ep->sw + SW_LAN_SDS_MISC) != want_line) ||(ioread32(igr) & GENMASK(20, 2)) != (want_igr & GENMASK(20, 2)) ||
	    (ioread32(egr) & GENMASK(19, 1)) != (want_egr & GENMASK(19, 1))) {
		iowrite32(old_igr, igr);
		iowrite32(old_egr, egr);
		if (hsgmii)
			iowrite32(old_line, ep->sw + SW_LAN_SDS_MISC);
		return -EIO;
	}
	return 0;
}

static int rtl9602c_uni_rate_set(void *cookie, unsigned int port,
			       u32 down_kbps, u32 up_kbps)
{
	struct rtl9602c_eth *ep = cookie;
	unsigned long flags;
	u32 down, up;
	int rc;

	if (!of_machine_is_compatible("vsol,v2802rh"))
		return down_kbps || up_kbps ? -EOPNOTSUPP : 0;
	if (port >= LAN_PORTS || !ep->lan_ndev[port])
		return -EINVAL;
	if ((down_kbps && down_kbps < 8) || down_kbps > UNI_RATE_MAX * 8 ||
	    (up_kbps && up_kbps < 8) || up_kbps > UNI_RATE_MAX * 8)
		return -ERANGE;
	/* A request must remain representable if LAN2 changes to HSGMII. */
	if (port == 0 && ep->port0_serdes && down_kbps && down_kbps < 16)
		return -ERANGE;
	down = down_kbps ? down_kbps >> 3 : UNI_RATE_MAX;
	up = up_kbps ? up_kbps >> 3 : UNI_RATE_MAX;
	spin_lock_irqsave(&ep->sw_table_lock, flags);
	/* omcid binds before the NIC opens. Defaults must not touch hardware
	 * or prevent its initial backend reset from completing. */
	rc = 0;
	if (ep->uni_rate[port].valid || down_kbps || up_kbps) {
		/* While the switch is unavailable, the replay applies the request. */
		if (!READ_ONCE(ep->in_recovery) && READ_ONCE(ep->hw_up))
			rc = rtl9602c_uni_rate_write(ep, port, down, up);
		if (!rc) {
			ep->uni_rate[port].down = down;
			ep->uni_rate[port].up = up;
			ep->uni_rate[port].valid = true;
		}
	}
	spin_unlock_irqrestore(&ep->sw_table_lock, flags);
	return rc;
}

/* Ageing and the frame limit are one setting for the whole switch, so the
 * shortest ageing and the largest frame any port asked for win. An OLT's usual
 * 1518 must not lose tagged frames, so the frame limit only rises from stock's. */
static int rtl9602c_uni_l2_write(struct rtl9602c_eth *ep)
{
	void __iomem *sw = ep->sw;
	u32 age = 0, frame = SW_LAN_MAX_FRAME, over, cfg;
	unsigned int p;
	int rc = 0;

	over = ioread32(sw + SW_LUT_LEARN_OVER_CTRL);
	for (p = 0; p < LAN_PORTS; p++) {
		u32 limit = ep->uni_l2[p].learn_limit;
		void __iomem *reg = sw + SW_LUT_LRN_LIMITNO(p);
		unsigned int shift = (p % 2) * 11;
		u32 word = ioread32(reg) & ~(SW_LUT_LRN_LIMIT_MAX << shift);

		/* Drop before lowering the limit: no frame floods in between. */
		over = (over & ~(3u << (p * 2))) | ((limit ? 1u : 0u) << (p * 2));
		iowrite32(over, sw + SW_LUT_LEARN_OVER_CTRL);
		limit = limit ? min_t(u32, limit, SW_LUT_LRN_LIMIT_MAX) : SW_LUT_LRN_LIMIT_MAX;
		word |= limit << shift;
		iowrite32(word, reg);
		if (ioread32(reg) != word)
			rc = -EIO;
		if (ep->uni_l2[p].ageing_s) {
			u32 ticks = min_t(u32, ep->uni_l2[p].ageing_s, GENMASK(20, 0) / 10) * 10;

			age = age ? min(age, ticks) : ticks;
		}
		frame = max(frame, min_t(u32, ep->uni_l2[p].max_frame, SW_PON_MAX_FRAME));
	}
	cfg = (ioread32(sw + SW_LUT_CFG) & ~GENMASK(20, 0)) | (age ?: SW_LUT_AGE_DEFAULT);
	iowrite32(cfg, sw + SW_LUT_CFG);
	iowrite32(frame, sw + SW_MAX_LENGTH_CFG1);
	if (ioread32(sw + SW_LUT_LEARN_OVER_CTRL) != over ||
	    (ioread32(sw + SW_LUT_CFG) & GENMASK(20, 0)) != (cfg & GENMASK(20, 0)) ||
	    ioread32(sw + SW_MAX_LENGTH_CFG1) != frame)
		rc = -EIO;
	return rc;
}

static int rtl9602c_uni_l2_set(void *cookie, unsigned int port, u32 ageing_s,
			       u32 learn_limit, u32 max_frame)
{
	struct rtl9602c_eth *ep = cookie;
	unsigned long flags;
	int rc = 0;

	if (!of_machine_is_compatible("vsol,v2802rh"))
		return ageing_s || learn_limit || max_frame ? -EOPNOTSUPP : 0;
	if (port >= LAN_PORTS || !ep->lan_ndev[port])
		return -EINVAL;
	spin_lock_irqsave(&ep->sw_table_lock, flags);
	ep->uni_l2[port].ageing_s = ageing_s;
	ep->uni_l2[port].learn_limit = learn_limit;
	ep->uni_l2[port].max_frame = max_frame;
	/* Defaults alone leave the switch untouched, as for the UNI rate. */
	ep->uni_l2_used |= ageing_s || learn_limit || max_frame;
	if (ep->uni_l2_used && !READ_ONCE(ep->in_recovery) && READ_ONCE(ep->hw_up))
		rc = rtl9602c_uni_l2_write(ep);
	spin_unlock_irqrestore(&ep->sw_table_lock, flags);
	return rc;
}

static void rtl9602c_uni_rate_replay(struct rtl9602c_eth *ep)
{
	unsigned long flags;
	unsigned int p;

	spin_lock_irqsave(&ep->sw_table_lock, flags);
	for (p = 0; p < LAN_PORTS; p++) {
		if (ep->uni_rate[p].valid &&
		    rtl9602c_uni_rate_write(ep, p, ep->uni_rate[p].down,
				    ep->uni_rate[p].up))
			dev_warn(ep->dev, "[LAN-006] UNI port %u rate replay failed\n", p);
	}
	if (ep->uni_l2_used && rtl9602c_uni_l2_write(ep))
		dev_warn(ep->dev, "[L2-003] UNI L2 settings replay failed\n");
	spin_unlock_irqrestore(&ep->sw_table_lock, flags);
}

static void rtl9602c_sw_min_init(struct rtl9602c_eth *ep)
{
	if (!ep->sw)
		return;

	/* Stock switch-init prerequisites */
	writel(readl(SOC_SW_ENABLE) | SW_EN_BIT, SOC_SW_ENABLE);

	/*
	 * (b) CFG_UNHIOL.IPG_COMPENSATION, swcore 0x23040 bit0. The stock
	 * switch init sets it (0xa9 against 0xa8 without it), and poking it
	 * live changed the OMCI forwarding class.
	 */
	if (ep->swm->cfg_unhiol)
		iowrite32(ioread32(ep->sw + ep->swm->cfg_unhiol) | BIT(0),
			  ep->sw + ep->swm->cfg_unhiol);

	/* (c) WRAP_GPHY_MISC.PATCH_PHY_DONE — swcore 0x110 bit0: the "switch ready /
	 * PHY patch done" latch the stock switch init asserts at completion. Without it
	 * the switch may not present itself as fully initialised to the direct-TX
	 * forwarding class. */
	if (ep->swm->gphy_misc)
		iowrite32(ioread32(ep->sw + ep->swm->gphy_misc) | BIT(0),
			  ep->sw + ep->swm->gphy_misc);

	/* Force NO port. */
	iowrite32(0, ep->sw + ep->swm->src_permit);
	/* Flood masks */
	iowrite32((ioread32(ep->sw + ep->swm->bc_flood) | ep->swm->port_mask) & ~BIT(ep->swm->pon_port),
		  ep->sw + ep->swm->bc_flood);
	iowrite32((ioread32(ep->sw + ep->swm->unkn_mc_flood) | ep->swm->port_mask) & ~BIT(ep->swm->pon_port),
		  ep->sw + ep->swm->unkn_mc_flood);
	iowrite32((ioread32(ep->sw + ep->swm->unkn_uc_flood) | ep->swm->port_mask) & ~BIT(ep->swm->pon_port),
		  ep->sw + ep->swm->unkn_uc_flood);
	/* Unknown-unicast that misses the L2 lookup */
	iowrite32(0, ep->sw + ep->swm->lut_unkn_uc_da);	/* all ports FORWARD */
	/* 0x27000 (PISO) is a 5-bit isolation-vector INDEX per */
	{
		int p, rc;

		u32 lan_ext = 0x7, pon = BIT(ep->swm->pon_port) | BIT(ep->swm->cpu_port);

		/* As stock's 4005: every port and the CPU's extension ports, all untagged */
		rc = rtl9602c_vlan_write(ep, SW_DEFAULT_VID, 0xf, 0xf, lan_ext, false);
		if (rc < 0)
			netdev_warn(ep->ndev,
				    "[L2-004] swcore VLAN 4k-table engine still BUSY after %u tries (rc=%d): default VLAN %u may not be installed, LAN egress may stay filtered\n",
				    SW_TBL_TRIES, rc, SW_DEFAULT_VID);
		/* As stock's 4025: the PON floods to the CPU only */
		if (rtl9602c_pon_vid && rtl9602c_vlan_write(ep, rtl9602c_pon_vid, pon, pon, 0x1, true) < 0)
			netdev_warn(ep->ndev,
				    "[L2-005] swcore VLAN table BUSY writing the PON VLAN %u: WAN broadcasts may reach the LAN ports\n",
				    rtl9602c_pon_vid);
		iowrite32(0, ep->sw + SW_VLAN_PORT_ACCEPT_FRAME_TYPE);	/* accept all frame types */
		/*
		 * PVID is a 12-bit element array packed two per word; see
		 * SW_VLAN_PB_VID.
		 */
		for (p = 0; p < SW_VLAN_PB_VID_PORTS; p++) {
			void __iomem *w;
			u32 vid = p == ep->swm->pon_port ? SW_WAN_PVID : SW_DEFAULT_VID;

			if (!(ep->swm->port_mask & BIT(p)))
				continue;	/* not a port on this chip */

			w = ep->sw + sw_packed_off(SW_VLAN_PB_VID, p,
						   SW_VLAN_PB_VID_BITS);
			iowrite32(sw_packed_ins(ioread32(w), p,
						SW_VLAN_PB_VID_BITS,
						vid), w);
		}
		/* One steering VLAN per LAN jack (lan_tx_steer): members the
		 * jack and the CPU, untagged on the jack, so a frame the CPU
		 * tags with it leaves on that jack and no other. The PVIDs
		 * above are untouched: what a jack receives is classified as
		 * before. SVL, as VLAN 1: one MAC table, so a host the switch
		 * learned is a known host here too. */
		for (p = 0; p < LAN_PORTS; p++) {
			if (!ep->lan_ndev[p])
				continue;
			if (rtl9602c_vlan_write(ep, SW_LAN_STEER_VID(p),
						BIT(p) | BIT(ep->swm->cpu_port),
						BIT(p), lan_ext, false) < 0)
				netdev_warn(ep->ndev,
					    "[L2-006] swcore VLAN table BUSY writing steering VLAN %u for %s: in VLAN mode its frames would reach no jack\n",
					    SW_LAN_STEER_VID(p), ep->lan_ndev[p]->name);
		}
		iowrite32(0xf, ep->sw + SW_VLAN_INGRESS);	/* ingress filter, ports 0-3 */
		iowrite32(SW_VLAN_CTRL_VAL, ep->sw + SW_VLAN_CTRL); /* enable VLAN function */
		luna_gpon_vlan_filter_ok(rtl9602c_pon_vid != 0);
	}
	/* Force the CPU port link up. */
	iowrite32(SW_ABLTY_1G_FD_UP, ep->sw + SW_FORCE_P_ABLTY(ep, ep->swm->cpu_port));
	iowrite32(0xfff, ep->sw + SW_ABLTY_FORCE_MODE(ep, ep->swm->cpu_port));

	/* Accept short (runt) frames on the PON port (2) and CPU */
	iowrite32(ioread32(ep->sw + SW_P_MISC_PORT_9602C(ep->swm->pon_port)) | BIT(2),
		  ep->sw + SW_P_MISC_PORT_9602C(ep->swm->pon_port));	/* port 2 (PON) */
	iowrite32(ioread32(ep->sw + SW_P_MISC_PORT_9602C(ep->swm->cpu_port)) | BIT(2),
		  ep->sw + SW_P_MISC_PORT_9602C(ep->swm->cpu_port));	/* port 3 (CPU) */

	/* Force the PON port link UP for the same reason */
	iowrite32(SW_ABLTY_1G_FD_UP, ep->sw + SW_FORCE_P_ABLTY(ep, ep->swm->pon_port));
	/* Force P2 link UP (0xfff). */
	iowrite32(0xfff, ep->sw + SW_ABLTY_FORCE_MODE(ep, ep->swm->pon_port));

	if (ep->stock_switch)
		rtl9602c_sw_stock_config(ep);
	rtl9602c_uni_rate_replay(ep);
}

/* The first rung of the declared precedence */
static char *mac_param;
module_param_named(mac, mac_param, charp, 0444);
MODULE_PARM_DESC(mac, "station MAC handed in at boot, aa:bb:cc:dd:ee:ff");

/* Thin wrappers over the family helpers, kept at their own names so the call
 * sites and this diff stay small. The BODIES live in luna_eth_regs.h. */
static void rtl9602c_eth_get_hwaddr(struct rtl9602c_eth *ep, u8 *mac)
{
	luna_idr_get(ep->base, mac);
}

static void rtl9602c_eth_set_hwaddr(struct rtl9602c_eth *ep, const u8 *mac)
{
	luna_idr_set(ep->base, mac);
}

/* Stock does NOT give the WAN (nas0_0) the LAN MAC */
static unsigned int wan_mac_offset = 3;
module_param(wan_mac_offset, uint, 0644);
MODULE_PARM_DESC(wan_mac_offset, "WAN (gpon0) MAC = board/LAN MAC + this offset (default 3, this model)");

/* OMCI MIB-Data-Sync (ME 2 attr 1) seed. */
static const unsigned int omci_mds_seed = OMCI_MDS_POISON_SEED;	/* 1..30: satisfies `rsync < 31` unconditionally, so the
					 * OLT provisions whatever it has stored. NOT 0 -- see above. */


/* The MDS walk's step is the CORE's */
static void rtl9602c_wan_mac(u8 *out, const u8 *base)
{
	rtl9602c_wan_mac_add(out, base, wan_mac_offset);
}

/* Program the station address into the hardware (IDR) on */
static int rtl9602c_eth_set_mac_address(struct net_device *ndev, void *p)
{
	struct rtl9602c_eth *ep = netdev_priv(ndev);
	int ret = eth_mac_addr(ndev, p);
	unsigned int port;

	if (ret)
		return ret;
	rtl9602c_eth_set_hwaddr(ep, ndev->dev_addr);
	/* The jacks carry the board MAC too, as DSA user ports their conduit's. */
	for (port = 0; port < LAN_PORTS; port++) {
		if (!ep->lan_ndev[port])
			continue;
		eth_hw_addr_set(ep->lan_ndev[port], ndev->dev_addr);
		call_netdevice_notifiers(NETDEV_CHANGEADDR, ep->lan_ndev[port]);
	}
	/* Keep the WAN (gpon0) identity tracking the board MAC provisioned onto eth0
	 * (rtk_factory), applying the stock model offset: WAN = LAN + wan_mac_offset
	 * (see rtl9602c_wan_mac). This is the MAC the ISP/OLT identifies the ONU by. */
	if (ep->wan_ndev) {
		u8 wmac[ETH_ALEN];

		rtl9602c_wan_mac(wmac, ndev->dev_addr);
		eth_hw_addr_set(ep->wan_ndev, wmac);
		luna_omci_set_wan_mac(wmac);
	}
	return 0;
}

/* Give RX descriptor @idx a fresh buffer and hand it to HW (own=1). */
/*
 * The body is in luna_eth_regs.h; this wrapper keeps the name and signature for
 * the call sites.
 */
static void rtl9602c_rx_arm(struct rtl9602c_eth *ep, unsigned int idx, struct page *page)
{
	ep->rx_page[idx] = page;
	ep->rx_ring[idx].addr = (page_pool_get_dma_addr(page) + NET_SKB_PAD) | DMA_BUS_WINDOW;
	luna_rx_rearm(ep->rx_ring, idx, ep->rx_n, RX_BUF_SIZE);
}

static int rtl9602c_eth_refill(struct rtl9602c_eth *ep, unsigned int idx)
{
	struct page *page = page_pool_dev_alloc_pages(ep->rx_pool);

	if (!page)
		return -ENOMEM;
	rtl9602c_rx_arm(ep, idx, page);
	return 0;
}

/* ===== M2: G.988 OMCI responder + upstream OMCC */
#define TXD0_OMCI_KEEP_DISLRN_PSEL 0x02240000u
/* TXD2_OMCI_CPUTAG / TXD2_OMCI_EFID: moved to rtl9602c_l34_logic.h with
 * the hoisted steering encode (rtl9602c_omci_txd_word2). */
/* word0 descriptor-flag ORs applied by the ring-submit path (it also re-ORs
 * word0 & 0x077e0000, see above). */
#define TXD0_DESC_FLAGS		(0xb8800000u | 0x40000000u)	/* = 0xf8800000 */
/* The 9602C opts3/word3 layout */
#define RTL9602C_OMCC_SID	64			/* OMCC US SID (== GPON flow 64) */
#define RTL8_4_TAG_LEN		8			/* software rtl8_4 0x8899 cpu-tag (also used by the LAN xmit below) */

/* LAYER BOUNDARY */
static void rtl9602c_eth_omci_reclaim(struct rtl9602c_eth *ep)
{
	while (ep->otx_dirty != ep->otx_head) {
		unsigned int i = otx_slot(ep, ep->otx_dirty);

		if (ep->otx_ring[i].opts1 & D_OWN)	/* HW still owns it (OWN in opts1/word0) */
			break;
		dma_unmap_single(ep->dev, ep->otx_buf_dma[i], ep->otx_buf_len[i],
				 DMA_TO_DEVICE);
		dev_consume_skb_any(ep->otx_skb[i]);
		ep->otx_skb[i] = NULL;
		ep->otx_dirty++;
	}
}

/* Forward decl: the shared-ring-0 OMCI path reclaims the LAN ring (defined with
 * the rest of the LAN datapath further down). */
static void rtl9602c_eth_tx_reclaim(struct rtl9602c_eth *ep);

/* Stock per-packet TX-fetch GO */
static void rtl9602c_eth_tx_fetch(struct rtl9602c_eth *ep)
{
	int n;

	if (!ep->txgo)
		return;
	iowrite32(ioread32(ep->txgo + 0x38) | BIT(31), ep->txgo + 0x38);
	for (n = 0; n < 100; n++) {
		if (!(ioread32(ep->txgo + 0x38) & BIT(31)))
			break;
		cpu_relax();
	}
}

/* the steered-descriptor LAN-ring-0 publish, written */
static inline u32 txd_word0_steered(unsigned int len)
{
	return D_FS | D_LS | D_TXCRC | D_IPCS | (len & TXD_LEN_MASK) |
	       TXD0_OMCI_KEEP_DISLRN_PSEL;
}

/* Claim the next LAN-ring-0 slot for @skb and describe */
static unsigned int txd_take_slot(struct rtl9602c_eth *ep, struct sk_buff *skb, dma_addr_t da, u32 len)
{
	unsigned int i = tx_slot(ep, ep->tx_head);

	ep->tx_skb[i] = skb;		/* the LAN ring reclaim frees it */
	ep->tx_buf_dma[i] = da;
	ep->tx_buf_len[i] = len;
	ep->tx_ring[i].addr = da | DMA_BUS_WINDOW;
	return i;
}

/* Hand slot @i to the hardware. */
static void txd_publish(struct rtl9602c_eth *ep, unsigned int i, u32 word0,
			bool may_skip_go)
{
	wmb();				/* descriptor body before ownership */
	ep->tx_ring[i].opts1 = word0 | D_OWN;	/* OWN in opts1: publish to HW */
	wmb();
	ep->tx_head++;
	/* Same TX-fetch GO as the LAN path. */
	rtl9602c_eth_tx_fetch(ep);	/* stock per-packet TX-fetch GO (0x18001038[31]) */
	ep_wr(ep, R_IO_CMD, ep_rd(ep, R_IO_CMD) | BIT(0));	/* kick LAN ring 0 */
}

/* Shared-ring-0 US-OMCI transmit */
static int rtl9602c_eth_omci_xmit_ring0(struct rtl9602c_eth *ep, const u8 *omci,
					unsigned int len)
{
	struct sk_buff *skb;
	unsigned long flags;
	unsigned int i;
	dma_addr_t da;
	u32 word0;

	/*
	 * LAN-ring HW-descriptor path: the bare OMCI PDU on LAN ring 0 (kick
	 * R_IO_CMD bit0) with the HW cpu-tag descriptor, not the SW 0x8899 tag.
	 */
	{	/* Stock sends the OMCI PDU raw (48 bytes, no Ethernet pad; its TX
		 * descriptor has len=0x30), so the US-NIC sees the same frame. */
		skb = netdev_alloc_skb(ep->ndev, len);
		if (!skb) {
			ep->dbg_omci_tx_drop++;
			return -ENOMEM;
		}
		skb_put(skb, len);
		memcpy(skb->data, omci, len);	/* bare OMCI PDU, raw length */
	}

	da = dma_map_single(ep->dev, skb->data, len, DMA_TO_DEVICE);
	if (dma_mapping_error(ep->dev, da)) {
		dev_kfree_skb_any(skb);
		ep->dbg_omci_tx_drop++;
		return -ENOMEM;
	}

	spin_lock_irqsave(&ep->tx_lock, flags);
	/* The GPON driver calls in from ITS OWN timer/softirq, so */
	if (ep->closing || !ep->tx_ring) {
		spin_unlock_irqrestore(&ep->tx_lock, flags);
		dma_unmap_single(ep->dev, da, len, DMA_TO_DEVICE);
		dev_kfree_skb_any(skb);
		ep->dbg_omci_tx_drop++;
		return -ENODEV;
	}
	/* Reclaim the LAN ring first so a free slot is visible; if the LAN ring is
	 * full, DROP the OMCI frame (do NOT stop the queue — that would stall LAN
	 * TX for a control frame). The OLT retransmits the OMCI request. */
	rtl9602c_eth_tx_reclaim(ep);
	/* If the shared LAN ring is full, the GMAC simply */
	{
		int spin = 64;

		while (luna_gmac_tx_ring_full(ep->tx_head, ep->tx_dirty,
					      TX_RING_SIZE, 0) && spin-- > 0) {
			ep_wr(ep, R_IO_CMD, ep_rd(ep, R_IO_CMD) | BIT(0));	/* retire ring0 */
			cpu_relax();
			rtl9602c_eth_tx_reclaim(ep);
		}
	}
	if (luna_gmac_tx_ring_full(ep->tx_head, ep->tx_dirty,
				   TX_RING_SIZE, 0)) {
		spin_unlock_irqrestore(&ep->tx_lock, flags);
		dma_unmap_single(ep->dev, da, len, DMA_TO_DEVICE);
		dev_kfree_skb_any(skb);
		ep->dbg_omci_tx_drop++;
		return -EBUSY;
	}
	i = txd_take_slot(ep, skb, da, len);

	/* word0 (opts1) = */
	word0 = txd_word0_steered(len);
	if (i == tx_eor_slot(ep))
		word0 |= D_EOR;

	/* HW cpu-tag steering words */
	ep->tx_ring[i].opts2 = rtl9602c_omci_txd_word2(omci_word2_ovr);
	ep->tx_ring[i].opts3 = rtl9602c_omci_txd_word3(omci_word3_ovr,
						       RTL9602C_OMCC_SID);
	ep->tx_ring[i].opts4 = 0;
	/*
	 * Recorded before the publish: naming the slot while it is still ours
	 * can never describe a descriptor the engine has already eaten.
	 */
	ep->omci_r0_last_slot = (int)i;
	txd_publish(ep, i, word0, false);  /* US OMCI: the GO is NOT optional */
	spin_unlock_irqrestore(&ep->tx_lock, flags);

	if (IS_ENABLED(CONFIG_LUNA_DEBUG) && ep->dbg_omci_tx < 30) {
		/* PRIME PROBE */
		u32 post_w0;
		udelay(120);			/* let the GMAC retire this 48B TX */
		post_w0 = ep->tx_ring[i].opts1;
		netdev_info(ep->ndev,
			"omci_tx[ring0]: w3=%08x slot=%u | post_w0=%08x (0x30000030=tag-inserted, 0x32240030=NOT)\n",
			ep->tx_ring[i].opts3, i, post_w0);
	}

	ep->dbg_omci_tx++;
	ep->ndev->stats.tx_packets++;
	ep->ndev->stats.tx_bytes += len;
	return 0;
}

/* US-OMCI upstream transmit on the DEDICATED OMCC ring. */
static netdev_tx_t rtl9602c_eth_wan_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct rtl9602c_eth *ep = *(struct rtl9602c_eth **)netdev_priv(ndev);
	unsigned long flags;
	unsigned int i, len;
	dma_addr_t da;
	u32 word0;
	int flow = -1;

	/* omcid's services: the frame's GEM flow, with the network-side tag */
	if (luna_xpon_active()) {
		if (!v2802_l2_wan_allowed(ep, skb)) {
			dev_kfree_skb_any(skb);
			ndev->stats.tx_dropped++;
			return NETDEV_TX_OK;
		}
		if (v2802_l2_priority(ep, skb)) {
			dev_kfree_skb_any(skb);
			ndev->stats.tx_dropped++;
			return NETDEV_TX_OK;
		}
		flow = luna_xpon_tx_flow(&skb, v2802_l2_vlan_filtering(ep));
		/* An untagged frame only now has the tag the priority belongs in. */
		if (flow < 0 || (skb && v2802_l2_priority(ep, skb))) {
			if (skb)
				dev_kfree_skb_any(skb);
			ndev->stats.tx_dropped++;
			return NETDEV_TX_OK;
		}
	}
	if (skb_put_padto(skb, ETH_ZLEN))	/* pad runts to the min Ethernet frame; frees skb on error */
		return NETDEV_TX_OK;
	len = skb->len;

	da = dma_map_single(ep->dev, skb->data, len, DMA_TO_DEVICE);
	if (dma_mapping_error(ep->dev, da)) {
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	spin_lock_irqsave(&ep->tx_lock, flags);
	if (ep->closing || !ep->tx_ring || !luna_gpon_data_ready()) {
		spin_unlock_irqrestore(&ep->tx_lock, flags);
		dma_unmap_single(ep->dev, da, len, DMA_TO_DEVICE);
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}
	rtl9602c_eth_tx_reclaim(ep);	/* shared LAN ring 0; drop on full (DHCP retransmits) */
	/* The OMCI reserve is honoured here too. */
	if (luna_gmac_tx_ring_full(ep->tx_head, ep->tx_dirty,
				   TX_RING_SIZE, OMCI_RESV)) {
		spin_unlock_irqrestore(&ep->tx_lock, flags);
		dma_unmap_single(ep->dev, da, len, DMA_TO_DEVICE);
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}
	i = txd_take_slot(ep, skb, da, len);
	word0 = txd_word0_steered(len);
	if (i == tx_eor_slot(ep))
		word0 |= D_EOR;
	ep->tx_ring[i].opts2 = rtl9602c_omci_txd_word2(0);	/* stock cputag|efid */
	if (flow < 0)
		flow = luna_wan_control_frame(skb->data, len) && luna_gpon_ctrl_flow_ready() ?
		       GPON_CTRL_FLOW : GPON_DATA_FLOW;
	ep->tx_ring[i].opts3 = rtl9602c_omci_txd_word3(0, flow);
	ep->tx_ring[i].opts4 = 0;
	txd_publish(ep, i, word0, true);   /* WAN: may skip the GO */
	spin_unlock_irqrestore(&ep->tx_lock, flags);

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += len;
	return NETDEV_TX_OK;
}

static int rtl9602c_eth_wan_open(struct net_device *ndev)
{
	struct rtl9602c_eth *ep = *(struct rtl9602c_eth **)netdev_priv(ndev);
	int ret;

	/* The rings are eth0's, but eth0 is a bridge member only on an old
	 * configuration: whichever netdev opens first brings the GMAC up. */
	ret = rtl9602c_eth_hw_ensure(ep);
	if (ret)
		return ret;
	/* Set the WAN identity the OLT/ISP uses to recognise */
	if (ep && ep->ndev && is_valid_ether_addr(ep->ndev->dev_addr)) {
		u8 wmac[ETH_ALEN];

		rtl9602c_wan_mac(wmac, ep->ndev->dev_addr);
		eth_hw_addr_set(ndev, wmac);
		luna_omci_set_wan_mac(wmac);
	}
	/* omcid owns pon0 readiness; opening it must not create a false link. */
	if (!luna_xpon_active() || luna_xpon_data_ready())
		netif_carrier_on(ndev);
	else
		netif_carrier_off(ndev);
	netif_start_queue(ndev);
	return 0;
}

static int rtl9602c_eth_wan_stop(struct net_device *ndev)
{
	netif_stop_queue(ndev);
	netif_carrier_off(ndev);
	return 0;
}

/* With the VLAN filter on, a tagged WAN needs its VID in the table: PON and CPU, tagged. */
static int rtl9602c_wan_vid(struct rtl9602c_eth *ep, u16 vid, bool add)
{
	u32 mbr = BIT(ep->swm->pon_port) | BIT(ep->swm->cpu_port);

	if (ep == READ_ONCE(v2802_l2.ep) && !READ_ONCE(v2802_l2.stopping))
		return v2802_l2_netdev_vid(ep, vid, add);
	if (!ep->sw || !rtl9602c_pon_vid || !vid || vid == SW_DEFAULT_VID || vid == rtl9602c_pon_vid)
		return 0;
	return rtl9602c_vlan_write(ep, vid, add ? mbr : 0, 0, add ? 0x1 : 0, add) < 0 ?
	       -EBUSY : 0;
}

static int rtl9602c_wan_vid_add(struct net_device *ndev, __be16 proto, u16 vid)
{
	return rtl9602c_wan_vid(*(struct rtl9602c_eth **)netdev_priv(ndev), vid, true);
}

static int rtl9602c_wan_vid_kill(struct net_device *ndev, __be16 proto, u16 vid)
{
	return rtl9602c_wan_vid(*(struct rtl9602c_eth **)netdev_priv(ndev), vid, false);
}

static void rtl9602c_eth_wan_set_rx_mode(struct net_device *ndev)
{
	struct rtl9602c_eth *ep = *(struct rtl9602c_eth **)netdev_priv(ndev);

	luna_eth_set_promisc(ep->base, rtl9602c_eth_want_promisc(ep));
}

static const struct net_device_ops rtl9602c_eth_wan_ops = {
	.ndo_get_port_parent_id = v2802_l2_parent_id,
	.ndo_open		= rtl9602c_eth_wan_open,
	.ndo_stop		= rtl9602c_eth_wan_stop,
	.ndo_start_xmit		= rtl9602c_eth_wan_xmit,
	.ndo_set_rx_mode	= rtl9602c_eth_wan_set_rx_mode,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_vlan_rx_add_vid	= rtl9602c_wan_vid_add,
	.ndo_vlan_rx_kill_vid	= rtl9602c_wan_vid_kill,
};

static int rtl9602c_eth_omci_xmit(struct rtl9602c_eth *ep, const u8 *omci,
				  unsigned int len)
{
	if (len < 8 || len > 1500)
		return -EINVAL;
	/* LAN ring 0, with the OMCI steering descriptor. */
	return rtl9602c_eth_omci_xmit_ring0(ep, omci, len);
}

/* OLT-INDEPENDENT US-OMCI datapath self-test */
void rtl9602c_eth_omci_selftest(void)
{
	struct rtl9602c_eth *ep = g_ep;
	u8 frame[48];

	if (!ep)
		return;
	/* Arm the GMAC OMCI cpu-tag routing (CPUTAGCR=0x901eff04) */
	rtl9602c_eth_set_omci_sid(RTL9602C_OMCC_SID);
	memset(frame, 0, sizeof(frame));
	frame[0] = 0x00; frame[1] = 0x01;	/* TID */
	frame[2] = 0x29;			/* MT = Get-response (0x09 | AK 0x20) */
	frame[3] = 0x0a;			/* DevId (baseline) */
	frame[4] = 0x01; frame[5] = 0x00;	/* ME class 256 (ONT-G) */
	rtl9602c_eth_omci_xmit(ep, frame, sizeof(frame));
}
EXPORT_SYMBOL(rtl9602c_eth_omci_selftest);

/* OMCI (ITU-T G.988): the responder is the common core's. */
static struct omci_onu luna_onu;

/* RX copies the original baseline prefix and length to the shared timer owner. */
static void rtl9602c_eth_omci_input(struct rtl9602c_eth *ep, const u8 *msg,
				unsigned int len)
{
	int rc;

	if (luna_xpon_active()) {
		luna_xpon_omci_rx(msg, len);
		return;
	}
	rc = luna_omci_enqueue(ep, msg, len);
	if (rc)
		ep->dbg_omci_unhandled++;
}

void rtl9602c_eth_omci_set_optical(s16 rx_level, s16 tx_level)
{
	luna_omci_set_optical((u16)rx_level, (u16)tx_level);
}

void rtl9602c_eth_omci_report_oper_up(void)
{
	luna_omci_report_oper_up();
}
EXPORT_SYMBOL(rtl9602c_eth_omci_report_oper_up);

/* `napi_ctx` says whether the caller is INSIDE napi->poll. */
static int rtl9602c_eth_rx(struct rtl9602c_eth *ep, int budget, bool napi_ctx)
{
	struct net_device *ndev = ep->ndev;
	int rx_done = 0;

	while (rx_done < budget) {
		unsigned int i = ep->rx_head;
		u32 opts1 = ep->rx_ring[i].opts1;
		struct page *page, *fresh;
		struct sk_buff *skb;
		u8 *data;
		u32 len;

		if (opts1 & D_OWN)		/* still HW-owned: nothing more */
			break;
		ep->dbg_filled++;		/* HW handed this descriptor back */

		/* Secure the REPLACEMENT before consuming the frame */
		fresh = page_pool_dev_alloc_pages(ep->rx_pool);
		if (!fresh) {
			ndev->stats.rx_dropped++;
			luna_rx_rearm(ep->rx_ring, i, ep->rx_n, RX_BUF_SIZE);
			ep->rx_head = (i + 1) % ep->rx_n;
			rx_done++;
			continue;
		}

		len = (opts1 & RXD_LEN_MASK);
		page = ep->rx_page[i];
		page_pool_dma_sync_for_cpu(ep->rx_pool, page, 0, RX_BUF_SIZE);
		data = (u8 *)page_address(page) + NET_SKB_PAD;

		/* DS-OMCI classifier + BOTH-ends length guard */
		if (rtl9602c_rx_is_ds_omci(ep->swm->rx_layout, ep->omci_trap_on,
					   ep->rx_ring[i].opts2,
					   ep->rx_ring[i].opts3,
					   data, len, ep->swm->pon_port,
					   ep->swm->omci_cpu_reason,
					   RX_CPU_PREFIX, RX_BUF_SIZE)) {
			/* DS OMCI on the OMCC. Capture for /proc, then hand the raw G.988
			 * message (prefix stripped) to the responder. */
			ep->dbg_omci_rx++;
			ep->dbg_omci_rxlen = len - RX_CPU_PREFIX;
			memcpy(ep->dbg_omci_rxbuf, data + RX_CPU_PREFIX,
			       min_t(unsigned int, len - RX_CPU_PREFIX,
				     sizeof(ep->dbg_omci_rxbuf)));
			rtl9602c_eth_omci_input(ep, data + RX_CPU_PREFIX,
						len - RX_CPU_PREFIX);
			page_pool_put_full_page(ep->rx_pool, page, false);
		} else if (rtl9602c_rx_frame_bad(opts1, RXD_CRCERR | RXD_RCDF,
						 len, RX_CPU_PREFIX,
						 RX_BUF_SIZE)) {
			/* bad-frame verdict hoisted to flowcore: completes the
			 * RX classification trio fuzz_rx.c drives on x86 */
			ndev->stats.rx_errors++;
			ep->dbg_err++;
			page_pool_put_full_page(ep->rx_pool, page, false);
		} else {
			struct net_device *rdev = ndev;	/* receive netdev: eth0, or gpon0 for PON-port WAN frames */

			skb = build_skb(page_address(page), PAGE_SIZE);
			if (!skb) {
				ndev->stats.rx_dropped++;
				page_pool_put_full_page(ep->rx_pool, page, false);
				goto rx_rearm;
			}
			skb_mark_for_recycle(skb);
			skb_reserve(skb, NET_SKB_PAD);
			skb->dev = ndev;
			ep->dbg_good++;
			/* Do NOT software-strip a 4-byte FCS here */
			skb_put(skb, len);
			/* Capture the raw frame (pre-pull) for /proc diag. */
			ep->dbg_rxlen = len;
			memcpy(ep->dbg_rxbuf, skb->data,
			       min_t(unsigned int, len, sizeof(ep->dbg_rxbuf)));
			/* opts3 src_port_num [19:16] = ingress port. WAN demux */
			{
				const u8 *dst = skb->data + RX_CPU_PREFIX;
				unsigned int sp = luna_gmac_rx_src_port(ep->swm->rx_layout,
						       ep->rx_ring[i].opts3);

				/* Route drained WAN frames. A LAN frame goes up
				 * its jack's netdev while that is running, else
				 * eth0: a bridge built on eth0 alone still works. */
				if (ep->wan_ndev &&
				    rtl9602c_rx_wan_demux(ep->swm->rx_layout,
						  ep->rx_ring[i].opts3, dst,
							  ep->wan_ndev->dev_addr,
							  ep->swm->pon_port))
					rdev = ep->wan_ndev;
				else if (sp < LAN_PORTS && ep->lan_ndev[sp] &&
					 netif_running(ep->lan_ndev[sp]))
					rdev = ep->lan_ndev[sp];
				/*
				 * DS-OFFER diag: log any DHCP-to-client (UDP
				 * dst port 68) DS frame and where it routed.
				 * dst = eth hdr; [12:13]=ethertype 0x0800,
				 * [23]=IP proto 0x11(UDP), [36:37]=UDP dst port
				 * 0x0044(68).
				 */
				if (IS_ENABLED(CONFIG_LUNA_DEBUG) &&
				    len >= RX_CPU_PREFIX + 38 &&
				    dst[12] == 0x08 && dst[13] == 0x00 &&
				    dst[23] == 0x11 &&
				    dst[36] == 0x00 && dst[37] == 0x44)
					pr_info("rtl9602c-eth: DHCP-DS sp=%u opts3=%08x dst=%pM -> %s\n",
						sp, ep->rx_ring[i].opts3, dst,
						rdev->name);
			}
			/* The switch CPU port prepends a 2-byte offset word ahead */
			skb_pull(skb, RX_CPU_PREFIX);
			skb->offload_fwd_mark = v2802_l2_forwarded(skb,
				luna_gmac_rx_src_port(ep->swm->rx_layout, ep->rx_ring[i].opts3),
				luna_gmac_rx_reason(ep->swm->rx_layout, ep->rx_ring[i].opts2,
						    ep->rx_ring[i].opts3));
			if (!netif_running(rdev)) {
				rdev->stats.rx_dropped++;
				dev_kfree_skb_any(skb);
				goto rx_rearm;
			}
			if (rdev == ep->wan_ndev && luna_xpon_active() &&
			    !v2802_l2_vlan_filtering(ep))
				luna_xpon_rx_fixup(skb);
			skb->protocol = eth_type_trans(skb, rdev);
			rdev->stats.rx_packets++;
			rdev->stats.rx_bytes += len;
			/* NAPI poll context: napi_gro_receive, not netif_rx */
			if (napi_ctx)
				napi_gro_receive(&ep->napi, skb);
			else
				netif_rx(skb);
		}
rx_rearm:
		/* hand the slot back to HW with the buffer secured above */
		rtl9602c_rx_arm(ep, i, fresh);
		ep->rx_head = (i + 1) % ep->rx_n;
		rx_done++;
	}
	return rx_done;
}

static void rtl9602c_eth_tx_reclaim(struct rtl9602c_eth *ep)
{
	while (ep->tx_dirty != ep->tx_head) {
		unsigned int i = tx_slot(ep, ep->tx_dirty);

		if (ep->tx_ring[i].opts1 & D_OWN)	/* not sent yet */
			break;
		dma_unmap_single(ep->dev, ep->tx_buf_dma[i], ep->tx_buf_len[i],
				 DMA_TO_DEVICE);
		dev_consume_skb_any(ep->tx_skb[i]);
		ep->tx_skb[i] = NULL;
		ep->tx_dirty++;
	}
	if (!luna_gmac_tx_ring_full(ep->tx_head, ep->tx_dirty,
				    TX_RING_SIZE, OMCI_RESV)) {
		unsigned int p;

		if (netif_queue_stopped(ep->ndev))
			netif_wake_queue(ep->ndev);
		/* The jacks' netdevs stop on the same ring. */
		for (p = 0; p < LAN_PORTS; p++)
			if (ep->lan_ndev[p] && netif_queue_stopped(ep->lan_ndev[p]))
				netif_wake_queue(ep->lan_ndev[p]);
	}
}

/* The GMAC TX DMA is a self-polling sequential fetch engine */
static void rtl9602c_eth_tx_rekick(struct rtl9602c_eth *ep)
{
	bool parked = false;

	/* Park detection keys on the OLDEST PENDING descriptor */
	if (ep->tx_head != ep->tx_dirty &&
	    (ep->tx_ring[tx_slot(ep, ep->tx_dirty)].opts1 & D_OWN)) {
		parked = true;
		ep_wr(ep, R_IO_CMD, ep_rd(ep, R_IO_CMD) | BIT(0));
	}

	/* Park watchdog. */
	if (ep->closing) {
		ep->stall_since = 0;
	} else if (!parked) {
		ep->stall_since = 0;
		ep->stall_level = 0;
	} else if (!ep->stall_since ||
		   ep->tx_dirty + ep->otx_dirty != ep->stall_lastdirty) {
		if (ep->stall_since)
			ep->stall_level = 0;	/* progress: restart the ladder */
		ep->stall_since = jiffies;
		ep->stall_lastdirty = ep->tx_dirty + ep->otx_dirty;
	} else if (time_after(jiffies,
			      ep->stall_since + msecs_to_jiffies(60))) {
	}
}

/* NAPI poll: drain RX up to `budget`, then */
static int rtl9602c_eth_napi_poll(struct napi_struct *napi, int budget)
{
	struct rtl9602c_eth *ep = container_of(napi, struct rtl9602c_eth, napi);
	unsigned long flags;
	int work;

	ep->dbg_poll++;
	work = rtl9602c_eth_rx(ep, budget, true);	/* inside napi->poll */
	spin_lock_irqsave(&ep->tx_lock, flags);
	rtl9602c_eth_tx_reclaim(ep);
	rtl9602c_eth_omci_reclaim(ep);
	rtl9602c_eth_tx_rekick(ep);	/* un-park a stalled TX ring */
	spin_unlock_irqrestore(&ep->tx_lock, flags);

	if (work < budget && napi_complete_done(napi, work)) {
		/* Re-arm. W1C-ack any status latched while masked FIRST so we do
		 * not immediately re-fire on a stale bit, THEN set the mask bits. */
		iowrite16(ioread16(ep->base + R_ISR), ep->base + R_ISR);
		ep_wr(ep, R_ISR1, ep_rd(ep, R_ISR1));
		iowrite16(ioread16(ep->base + R_IMR) | IMR_RX_BITS, ep->base + R_IMR);
		ep_wr(ep, R_IMR0, ep_rd(ep, R_IMR0) | IMR0_TX_BITS);
		if (luna_rx_ready(ep->rx_ring, ep->rx_head)) {	/* its bit went to the ack */
			ep->dbg_rx_repoll++;
			napi_schedule(napi);
		}
	}
	return work;
}

/* Slow TX-unpark backstop (IRQ-driven mode). */
static void rtl9602c_eth_rekick_timer(struct timer_list *t)
{
	struct rtl9602c_eth *ep = timer_container_of(ep, t, poll_timer);
	unsigned long flags;

	spin_lock_irqsave(&ep->tx_lock, flags);
	rtl9602c_eth_tx_reclaim(ep);
	rtl9602c_eth_omci_reclaim(ep);
	rtl9602c_eth_tx_rekick(ep);
	spin_unlock_irqrestore(&ep->tx_lock, flags);
	/* While a park is pending, tighten the cadence so the 60ms escalation
	 * ladder is not quantised by the 100ms backstop interval. */
	mod_timer(&ep->poll_timer, jiffies +
		  (ep->stall_since ? msecs_to_jiffies(20) : REKICK_INTERVAL));
}

/* Legacy pure-poll fallback (ep->irq <= 0) */
static void rtl9602c_eth_poll(struct timer_list *t)
{
	struct rtl9602c_eth *ep = timer_container_of(ep, t, poll_timer);
	unsigned long flags;

	ep->dbg_poll++;
	rtl9602c_eth_rx(ep, ep->rx_n, false);	/* timer callback, NOT napi->poll */
	spin_lock_irqsave(&ep->tx_lock, flags);
	rtl9602c_eth_tx_reclaim(ep);	/* under tx_lock (races the OMCI inject's locked reclaim on shared ring 0) */
	rtl9602c_eth_omci_reclaim(ep);
	rtl9602c_eth_tx_rekick(ep);	/* TDU-style keep-alive: un-park a stalled TX ring */
	spin_unlock_irqrestore(&ep->tx_lock, flags);
	mod_timer(&ep->poll_timer, jiffies + POLL_INTERVAL);
}

static int rtl9602c_eth_alloc_rings(struct rtl9602c_eth *ep)
{
	struct page_pool_params pp = {
		.flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
		.pool_size = ep->rx_n,
		.nid = NUMA_NO_NODE,
		.dev = ep->dev,
		.dma_dir = DMA_FROM_DEVICE,
		.offset = NET_SKB_PAD,
		.max_len = RX_BUF_SIZE,
	};
	struct page_pool *pool = page_pool_create(&pp);
	unsigned int i;

	if (IS_ERR(pool))
		return PTR_ERR(pool);
	ep->rx_pool = pool;
	ep->rx_ring = dma_alloc_coherent(ep->dev,
			ep->rx_n * sizeof(struct rx_desc),
			&ep->rx_ring_dma, GFP_KERNEL);
	ep->tx_ring = dma_alloc_coherent(ep->dev,
			TX_RING_SIZE * sizeof(struct tx_desc),
			&ep->tx_ring_dma, GFP_KERNEL);
	/* Dedicated US-OMCI TX ring (the OMCC ring, default ring 4). */
	ep->otx_ring = dma_alloc_coherent(ep->dev,
			OTX_RING_SIZE * sizeof(struct tx_desc),
			&ep->otx_ring_dma, GFP_KERNEL);
	/* Idle filler for the unused HW TX rings (see struct comment). */
	ep->dummy_ring = dma_alloc_coherent(ep->dev,
			DUMMY_RING_SIZE * sizeof(struct tx_desc),
			&ep->dummy_ring_dma, GFP_KERNEL);
	if (!ep->rx_ring || !ep->tx_ring || !ep->otx_ring || !ep->dummy_ring)
		return -ENOMEM;

	ep->rx_head = ep->tx_head = ep->tx_dirty = 0;
	ep->otx_head = ep->otx_dirty = 0;
	ep->tx_rot = ep->otx_rot = 0;	/* fresh rings: HW walk starts at slot 0 */
	ep->omci_r0_last_slot = -1;	/* no OMCI on shared LAN ring 0 yet */
	for (i = 0; i < TX_RING_SIZE; i++) {
		ep->tx_ring[i].opts1 = (i == TX_RING_SIZE - 1) ? D_EOR : 0;
		ep->tx_skb[i] = NULL;
	}
	for (i = 0; i < OTX_RING_SIZE; i++) {
		/* EOR (wrap) in word0 on the last slot; OWN (opts1 bit31) clear so
		 * the slot starts CPU-owned (idle) until the first OMCI submit. */
		ep->otx_ring[i].opts1 = (i == OTX_RING_SIZE - 1) ? D_EOR : 0;
		ep->otx_ring[i].opts2 = 0;
		ep->otx_skb[i] = NULL;
	}
	for (i = 0; i < DUMMY_RING_SIZE; i++) {
		/* OWN clear (opts1 bit31 = 0) => always CPU-owned => the TX engine
		 * idles on this ring; EOR on the last slot. Never written again. */
		ep->dummy_ring[i].opts1 = (i == DUMMY_RING_SIZE - 1) ? D_EOR : 0;
		ep->dummy_ring[i].opts2 = 0;
	}
	for (i = 0; i < ep->rx_n; i++) {
		if (rtl9602c_eth_refill(ep, i))
			return -ENOMEM;
	}
	return 0;
}

static void rtl9602c_eth_free_rings(struct rtl9602c_eth *ep)
{
	unsigned int i;

	for (i = 0; i < ep->rx_n; i++) {
		if (ep->rx_page[i]) {
			page_pool_put_full_page(ep->rx_pool, ep->rx_page[i], false);
			ep->rx_page[i] = NULL;
		}
	}
	if (ep->rx_pool) {
		page_pool_destroy(ep->rx_pool);
		ep->rx_pool = NULL;
	}
	for (i = 0; i < TX_RING_SIZE; i++) {
		if (ep->tx_skb[i]) {
			dma_unmap_single(ep->dev, ep->tx_buf_dma[i],
					 ep->tx_buf_len[i], DMA_TO_DEVICE);
			dev_kfree_skb_any(ep->tx_skb[i]);
			ep->tx_skb[i] = NULL;
		}
	}
	for (i = 0; i < OTX_RING_SIZE; i++) {
		if (ep->otx_skb[i]) {
			dma_unmap_single(ep->dev, ep->otx_buf_dma[i],
					 ep->otx_buf_len[i], DMA_TO_DEVICE);
			dev_kfree_skb_any(ep->otx_skb[i]);
			ep->otx_skb[i] = NULL;
		}
	}
	if (ep->rx_ring)
		dma_free_coherent(ep->dev, ep->rx_n * sizeof(struct rx_desc),
				  ep->rx_ring, ep->rx_ring_dma);
	if (ep->tx_ring)
		dma_free_coherent(ep->dev, TX_RING_SIZE * sizeof(struct tx_desc),
				  ep->tx_ring, ep->tx_ring_dma);
	if (ep->otx_ring)
		dma_free_coherent(ep->dev, OTX_RING_SIZE * sizeof(struct tx_desc),
				  ep->otx_ring, ep->otx_ring_dma);
	if (ep->dummy_ring)
		dma_free_coherent(ep->dev, DUMMY_RING_SIZE * sizeof(struct tx_desc),
				  ep->dummy_ring, ep->dummy_ring_dma);
	ep->rx_ring = NULL;
	ep->tx_ring = NULL;
	ep->otx_ring = NULL;
	ep->dummy_ring = NULL;
}

/* GMAC0 hard ISR (IRQF_SHARED, INTC input 26). */
static irqreturn_t rtl9602c_eth_isr(int irq, void *dev_id)
{
	struct rtl9602c_eth *ep = dev_id;
	u16 isr  = ioread16(ep->base + R_ISR) & IMR_RX_BITS;
	u32 isr1 = ep_rd(ep, R_ISR1) & IMR0_TX_BITS;

	if (!isr && !isr1)
		return IRQ_NONE;

	/* Mask first so the level line drops, then ack (W1C), then schedule. */
	iowrite16(ioread16(ep->base + R_IMR) & ~IMR_RX_BITS, ep->base + R_IMR);
	ep_wr(ep, R_IMR0, ep_rd(ep, R_IMR0) & ~IMR0_TX_BITS);
	if (isr)
		iowrite16(isr, ep->base + R_ISR);
	if (isr1)
		ep_wr(ep, R_ISR1, isr1);
	napi_schedule(&ep->napi);
	return IRQ_HANDLED;
}

/* Halt the GMAC DMA + IRQ machinery */
static void rtl9602c_hw_stop(struct rtl9602c_eth *ep)
{
	luna_eth_hw_stop(ep->base);
}

/* Full GMAC register program in the stock init ORDER, for */
static void rtl9602c_cpu_tag_arm(struct rtl9602c_eth *ep)
{
	ep_wr(ep, R_CPUTAGCR, 0x00000000u);	/* OFF first: re-latches the ADD engine */
	wmb();					/* the clear must land before the re-arm edge */
	ep_wr(ep, R_CPUTAGCR, 0x901eff04u);	/* ON: post-init stock value */
	ep_wr(ep, R_CPUTAG1CR,
	      CPUTAG1_OMCI_SID(RTL9602C_OMCC_SID) | CPUTAG1_LOW);	/* = 0x4002 (live stock) */

	if (!ep->sw)
		return;
	/* OFF->ON edge on the SWITCH CPU-port cpu-tag PARSER, mirroring the GMAC
	 * re-latch above. */
	iowrite32(0, ep->sw + SW_MAC_CPU_TAG_CTRL);
	wmb();					/* clear must land before the re-arm edge */
	iowrite32(sw_tagaware, ep->sw + SW_MAC_CPU_TAG_CTRL);
	/*
	 * aux flow-control thresholds, live-stock, named from the chip's own
	 * chipdef
	 */
	iowrite32(0x00400034, ep->sw + SW_FC_P_LO_TH);
	iowrite32(0x00f000ea, ep->sw + SW_FC_P_FCOFF_HI_TH);
	iowrite32(0x00400034, ep->sw + SW_FC_P_FCOFF_LO_TH);
}

static void rtl9602c_hw_program(struct rtl9602c_eth *ep)
{
	struct net_device *ndev = ep->ndev;
	unsigned int k, oring = rtl9602c_omci_hwring(omci_tx_ring);
	u32 desnum, rcr;

	iowrite8(0x0A, ep->base + R_CMD);	/* CMD: RxChkSum|RxJumboSupport */
	ep_wr(ep, R_TCR, 0x00000C00);		/* TX pad ON (bit0=0) */
	rcr = 0x0000000E;
	if (rtl9602c_eth_want_promisc(ep))
		rcr |= BIT(0);			/* re-apply AcceptAllPhys */
	ep_wr(ep, R_RCR, rcr);
	/* Stock O5 golden CONFIG = 0x21000000 (Rff 2k + rx-mring int split);
	 * after a true reset the split bit must be set by us, the per-ring
	 * IMR0/ISR1 model the driver already uses assumes it. */
	ep_wr(ep, R_CONFIG, 0x21000000);
	/* R_RXOKMUL is a MIB counter word ([RXOKMUL:TXABT]). */
	ep_wr(ep, R_RXOKMUL, 0x010c0000);	/* stock O5 counter snapshot */
	/* cpu-tag ADD engine: off-then-on edge re-latches it (see open notes) */
	rtl9602c_cpu_tag_arm(ep);

	/* Ring pointers, programmed while IO_CMD==0 (stock order; CDO is
	 * writable only in this stopped state). */
	ep_wr(ep, R_TxFDP1, ep->tx_ring_dma | DMA_BUS_WINDOW);
	iowrite16(0, ep->base + R_TxCDO1);
	for (k = 1; k <= 4; k++) {	/* idle dummy on the unused HW rings */
		if (k == oring)
			continue;
		ep_wr(ep, R_TxFDP(k), ep->dummy_ring_dma | DMA_BUS_WINDOW);
		iowrite16(0, ep->base + R_TxCDO(k));
	}
	ep_wr(ep, R_RxFDP, ep->rx_ring_dma | DMA_BUS_WINDOW);
	desnum = luna_gmac_rxdesnum_pack(ep->rx_n, TH_ON_VAL, TH_OFF_VAL);
	ep_wr(ep, R_RxDesNum, desnum);
	ep_wr(ep, R_RxCDO, luna_gmac_rxcdo_pack(ep->rx_n));
	for (k = 0; k < 7; k++)		/* every RX class -> ring 0 */
		ep_wr(ep, R_RRING_ROUTING1 + k * 4, 0);

	/* MSR top byte: 0xf0 kills sparse TX. */
	ep_wr(ep, R_MSR, (ep_rd(ep, R_MSR) & 0x00ffffff) | ((msr_top & 0xffu) << 24));
	rtl9602c_eth_set_hwaddr(ep, ndev->dev_addr);	/* IDR wiped by the reset */
	iowrite32(0xffffffff, ep->base + R_MAR0);		/* MAR0 */
	iowrite32(0xffffffff, ep->base + R_MAR4);		/* MAR4 */

	/* The enable edge: IO_CMD1 first, IO_CMD last (stock start order). */
	ep_wr(ep, R_IO_CMD1, IOCMD1_STOCK);
	ep_wr(ep, R_IO_CMD, IOCMD_STOCK);

	iowrite16(0xffff, ep->base + R_ISR);		/* ack anything latched */
	ep_wr(ep, R_ISR1, 0xffffffff);
	iowrite16(IMR_RX_BITS, ep->base + R_IMR);
	ep_wr(ep, R_IMR0, IMR0_TX_BITS);
}

/* Stock GMAC reset path: GMAC0 IP-block power-cycle. The block
 * is UNREADABLE while gated (MMIO would bus-abort), so callers must fence off
 * the ISR/diag readers first. */
/* The body and the hang warning are the FAMILY's (luna_eth_regs.h). */
static void rtl9602c_ipsel_cycle(void)
{
	luna_ipsel_cycle_gmac0();
}

/* TX-DMA park recovery */
static void rtl9602c_eth_recover_work(struct work_struct *work)
{
	struct rtl9602c_eth *ep = container_of(work, struct rtl9602c_eth,
					       recover_work);
	struct net_device *ndev = ep->ndev;
	unsigned long flags;
	unsigned int i;
	bool use_rst = !!recover_rst;

	if (ep->closing || !ep->hw_up)
		return;

	spin_lock_irqsave(&ep->sw_table_lock, flags);
	ep->in_recovery = true;
	spin_unlock_irqrestore(&ep->sw_table_lock, flags);
	rtl9601d_l2_changed();
	luna_gpon_nic_reset_begin();
	netdev_warn(ndev,
		    "[SYS-020] TX-DMA parked (head=%u dirty=%u otx=%u/%u cdo0=%u txok=%u IO_CMD=%08x ISR1=%08x): IP-block power-cycle recovery #%u\n",
		    ep->tx_head, ep->tx_dirty, ep->otx_head, ep->otx_dirty,
		    ioread16(ep->base + R_TxCDO(0)), ep_rd(ep, R_TXOKCNT) >> 16,
		    ep_rd(ep, R_IO_CMD), ep_rd(ep, R_ISR1),
		    ep->dbg_tx_recover + 1);

	netif_stop_queue(ndev);
	napi_disable(&ep->napi);
	timer_delete_sync(&ep->poll_timer);
	if (!use_rst && ep->irq > 0)
		disable_irq(ep->irq);	/* gated block MMIO would bus-abort
					 * (ipsel path only; RST is GMAC-local) */

	spin_lock_irqsave(&ep->tx_lock, flags);
	rtl9602c_hw_stop(ep);
	spin_unlock_irqrestore(&ep->tx_lock, flags);

	if (use_rst) {
		/* CMD.RST core soft-reset: microseconds, no IP-block gating.
		 * Self-clears on completion; fall through to the reprogram
		 * either way (a timeout leaves us no worse than before). */
		int n;

		iowrite8(0x0A | 0x01, ep->base + R_CMD);
		for (n = 0; n < 1000 && (ioread8(ep->base + R_CMD) & 1); n++)
			udelay(1);
	} else {
		rtl9602c_ipsel_cycle();
	}

	spin_lock_irqsave(&ep->tx_lock, flags);
	/*
	 * Drop everything in flight and re-arm both rings from scratch (engine
	 * stopped => CDO genuinely resets), align at 0.
	 */
	while (ep->tx_dirty != ep->tx_head) {
		i = tx_slot(ep, ep->tx_dirty);
		if (ep->tx_skb[i]) {
			dma_unmap_single(ep->dev, ep->tx_buf_dma[i],
					 ep->tx_buf_len[i], DMA_TO_DEVICE);
			dev_kfree_skb_any(ep->tx_skb[i]);
			ep->tx_skb[i] = NULL;
		}
		ep->tx_dirty++;
	}
	while (ep->otx_dirty != ep->otx_head) {
		i = otx_slot(ep, ep->otx_dirty);
		if (ep->otx_skb[i]) {
			dma_unmap_single(ep->dev, ep->otx_buf_dma[i],
					 ep->otx_buf_len[i], DMA_TO_DEVICE);
			dev_kfree_skb_any(ep->otx_skb[i]);
			ep->otx_skb[i] = NULL;
		}
		ep->otx_dirty++;
	}
	for (i = 0; i < TX_RING_SIZE; i++)
		ep->tx_ring[i].opts1 = (i == TX_RING_SIZE - 1) ? D_EOR : 0;
	for (i = 0; i < OTX_RING_SIZE; i++)
		ep->otx_ring[i].opts1 = (i == OTX_RING_SIZE - 1) ? D_EOR : 0;
	for (i = 0; i < ep->rx_n; i++)
		ep->rx_ring[i].opts1 = D_OWN | RX_BUF_SIZE |
				       ((i == ep->rx_n - 1) ? D_EOR : 0);
	ep->rx_head = 0;
	wmb();				/* ring state before the engine restart */
	rtl9602c_hw_program(ep);
	rtl9602c_tx_align(ep);		/* CDO=0 post-reset -> rot 0 */
	ep->dbg_tx_recover++;
	ep->stall_since = 0;
	spin_unlock_irqrestore(&ep->tx_lock, flags);
	if (!use_rst)
		gpon_pbo_init();	/* an IP-block reset owes its PBO re-init (sec 96) */

	if (!use_rst && ep->irq > 0)
		enable_irq(ep->irq);
	spin_lock_irqsave(&ep->sw_table_lock, flags);
	ep->in_recovery = false;
	spin_unlock_irqrestore(&ep->sw_table_lock, flags);
	rtl9602c_uni_rate_replay(ep);
	napi_enable(&ep->napi);
	mod_timer(&ep->poll_timer, jiffies + 1);
	netif_wake_queue(ndev);
	luna_gpon_nic_reset_end();
	rtl9601d_l2_changed();
}

#define GPHY_OCP_PARAM_ADDR	0xa436
#define GPHY_OCP_PARAM_DATA	0xa438
#define SW_EFUSE_IND_CMD	0x0001c
#define SW_EFUSE_IND_RD		0x00020
#define   EFUSE_IND_BUSY	BIT(16)

/* Stock's phyPatchArray_20160413, identical in the board's RTOS image and the
 * XZ005 kernel: PHY 1's DSP parameters, each followed by its address again. */
static const struct { u16 addr, val; } rtl9602c_gphy_params[] = {
	{ 0x809a, 0x4ad4 }, { 0x809e, 0x0506 }, { 0x809c, 0xd107 }, { 0x80a4, 0x930c },
	{ 0x80a2, 0x8b19 }, { 0x80b0, 0x8006 }, { 0x80ac, 0x429c }, { 0x80b4, 0x5319 },
	{ 0x80b2, 0xff7f }, { 0x808e, 0x15a4 }, { 0x8089, 0x6bf7 }, { 0x808a, 0xf708 },
	{ 0x8092, 0x401e }, { 0x8090, 0xe61f }, { 0x8088, 0xb06b },
};

/* The table's last four, written without the repeat. */
static const struct { u16 addr, val; } rtl9602c_gphy_params_last[] = {
	{ 0x8062, 0x2000 }, { 0x806a, 0x2300 }, { 0x8072, 0x2300 }, { 0x804d, 0x2444 },
};

static int rtl9602c_gphy_param_table__locked(struct rtl9602c_eth *ep)
{
	unsigned int i;
	int rc;

	rc = rtl9602c_gphy_ocp_write__locked(ep, 0, 0xa430, 0x019c);
	if (!rc)
		rc = rtl9602c_gphy_ocp_write__locked(ep, 1, 0xbce4, 0x0000);
	for (i = 0; !rc && i < ARRAY_SIZE(rtl9602c_gphy_params); i++) {
		u16 addr = rtl9602c_gphy_params[i].addr;

		rc = rtl9602c_gphy_ocp_write__locked(ep, 1, GPHY_OCP_PARAM_ADDR, addr);
		if (!rc)
			rc = rtl9602c_gphy_ocp_write__locked(ep, 1, GPHY_OCP_PARAM_DATA,
							     rtl9602c_gphy_params[i].val);
		if (!rc)
			rc = rtl9602c_gphy_ocp_write__locked(ep, 1, GPHY_OCP_PARAM_ADDR, addr);
	}
	for (i = 0; !rc && i < ARRAY_SIZE(rtl9602c_gphy_params_last); i++) {
		rc = rtl9602c_gphy_ocp_write__locked(ep, 1, GPHY_OCP_PARAM_ADDR,
						     rtl9602c_gphy_params_last[i].addr);
		if (!rc)
			rc = rtl9602c_gphy_ocp_write__locked(ep, 1, GPHY_OCP_PARAM_DATA,
							     rtl9602c_gphy_params_last[i].val);
	}
	return rc;
}

static int rtl9602c_efuse_read(struct rtl9602c_eth *ep, unsigned int idx, u16 *out)
{
	u32 v;
	int rc;

	iowrite32(EFUSE_IND_BUSY | idx, ep->sw + SW_EFUSE_IND_CMD);
	rc = read_poll_timeout_atomic(ioread32, v, !(v & EFUSE_IND_BUSY), 1, 1000, false,
				      ep->sw + SW_EFUSE_IND_RD);
	if (!rc)
		*out = v & 0xffff;
	return rc;
}

static int rtl9602c_efuse_read_n(struct rtl9602c_eth *ep, unsigned int idx, u16 *out,
				 unsigned int n)
{
	int rc = 0;

	while (!rc && n--)
		rc = rtl9602c_efuse_read(ep, idx++, out++);
	return rc;
}

/* Stock's _dal_rtl9602c_switch_default_patch: per-chip fixes burnt into the
 * eFuse. Words 0-2 say whether any are valid, word 11 counts the PHY ones, three
 * words each from word 12. Memory writes follow, four words each: their
 * addresses come from the fuse unchecked, so those are reported and not made. */
static int rtl9602c_gphy_fuse_patch__locked(struct rtl9602c_eth *ep, unsigned int *applied)
{
	unsigned int i, n, skipped = 0;
	u16 w[4];
	int rc;

	*applied = 0;
	rc = rtl9602c_efuse_read_n(ep, 0, w, 3);
	if (rc || ((w[0] ^ w[1]) & 1) == (w[2] & 1))
		return rc;
	rc = rtl9602c_efuse_read(ep, 11, &w[0]);
	if (rc)
		return rc;
	n = w[0] & 0xff;
	for (i = 0; i < n; i++) {
		unsigned int phy;

		rc = rtl9602c_efuse_read_n(ep, 12 + 3 * i, w, 3);
		if (rc)
			return rc;
		if (!w[0] && !w[1] && !w[2])
			return 0;
		if (w[0] == 0xffff && w[1] == 0xffff && w[2] == 0xffff)
			continue;
		for (phy = 0; phy < LAN_PORTS; phy++) {
			if (!(w[0] & BIT(phy)))
				continue;
			rc = rtl9602c_gphy_ocp_write__locked(ep, phy, w[1], w[2]);
			if (rc)
				return rc;
			(*applied)++;
		}
	}
	for (i = 0; i < (244 - 3 * n) / 4; i++) {
		rc = rtl9602c_efuse_read_n(ep, 12 + 3 * n + 4 * i, w, 4);
		if (rc)
			return rc;
		if (!(w[0] | w[1] | w[2] | w[3]))
			break;
		if ((w[0] & w[1] & w[2] & w[3]) == 0xffff)
			continue;
		dev_warn(ep->dev, "[SYS-021] eFuse asks for %04x%04x = %04x%04x; not applied\n",
			 w[0], w[1], w[2], w[3]);
		skipped++;
	}
	if (skipped)
		dev_warn(ep->dev, "[SYS-022] %u eFuse memory patches not applied\n", skipped);
	return 0;
}

/* A link the driver is about to power down is no drop for the counter to report. */
static void rtl9602c_lan_own_drop(struct rtl9602c_eth *ep, unsigned int port)
{
	if (ep->lan_link[port].up)
		ep->lan_own_drop |= BIT(port);
}

/* What stock does to the running PHYs in switch_init, ending as it does with
 * the PHY powered down, so its link comes back up on the new parameters. */
static void rtl9602c_gphy_tune(struct rtl9602c_eth *ep)
{
	unsigned int fused = 0;
	u16 bmcr;
	int rc;

	if (!of_machine_is_compatible("vsol,v2802rh"))
		return;
	mutex_lock(&rtl9602c_gphy_lock);
	rc = rtl9602c_gphy_param_table__locked(ep);
	if (!rc)
		rc = rtl9602c_gphy_fuse_patch__locked(ep, &fused);
	if (!rc)
		rc = rtl9602c_gphy_read__locked(ep, 1, MII_BMCR, &bmcr);
	if (!rc) {
		rtl9602c_lan_own_drop(ep, 1);
		rc = rtl9602c_gphy_write__locked(ep, 1, MII_BMCR, bmcr | BMCR_PDOWN);
	}
	mutex_unlock(&rtl9602c_gphy_lock);
	if (rc) {
		dev_warn(ep->dev, "[PHY-001] LAN1 PHY tuning failed: %d\n", rc);
		return;
	}
	msleep(10);
	dev_info(ep->dev, "LAN1 PHY tuned as stock (%u eFuse writes)\n", fused);
}

/* gpon_pbo_init() */
#define SW_RDY_FOR_PATCH_TRIES	200000	/* 200 ms at 1 us a try; the bound the wait below refuses at */

/* > 0, or negative and the caller must NOT bring */
static int rtl9602c_uboot_swcore_bringup(struct rtl9602c_eth *ep)
{
	void __iomem *sysstat = (void __iomem *)0xb8000044ul;	/* SoC SYSREG */
	int to, rc = 0;

	if (!ep->sw)
		return -ENODEV;
	/* wait for RDY_FOR_PATCH (0xB8000044 bit1) after the */
	for (to = 0; to < SW_RDY_FOR_PATCH_TRIES && !(readl(sysstat) & 0x2); to++)
		udelay(1);
	if (to == SW_RDY_FOR_PATCH_TRIES)
		netdev_warn(ep->ndev,
			    "[PHY-002] SoC never raised RDY_FOR_PATCH after %d us: the GMAC<->switch resync below runs against a block that did not announce itself\n",
			    SW_RDY_FOR_PATCH_TRIES);
	/* GPHY analog patch (0x6485 variant only) through the */
	iowrite32(0xa0000000, ep->sw + SW_CHIP_INFO);
	if ((ioread32(ep->sw + SW_CHIP_INFO) & 0xffff) == 0x6485) {
		static const u32 patch_cmd[] = {
			0x0061b844, 0x0021b906, 0x0021b906 /* indirect write, read, read */
		};
		unsigned int i;

		mutex_lock(&rtl9602c_gphy_lock);
		rc = rtl9602c_gphy_raw_write__locked(ep, 0xfffb, patch_cmd[0]);
		for (i = 1; !rc && i < ARRAY_SIZE(patch_cmd); i++)
			rc = rtl9602c_gphy_raw_cmd__locked(ep, patch_cmd[i]);
		mutex_unlock(&rtl9602c_gphy_lock);
	}
	/*
	 * The selector is put back before any return. SW_CHIP_INFO is left
	 * pointing at the variant window while the patch runs, and a bail-out
	 * that skipped this would hand the rest of the driver a register block
	 * that answers for something else.
	 */
	iowrite32(0x0, ep->sw + SW_CHIP_INFO);
	if (rc) {
		netdev_err(ep->ndev,
			   "[PHY-003] GPHY analog patch did not complete (%d): the 0x6485 variant fixup is NOT applied, so the patch-done bit is NOT asserted and this bring-up FAILS\n",
			   rc);
		return rc;
	}
	/* the SECOND site of the same register -- from the chip table too, or the
	 * conversion would be half done, which is worse than none: one write
	 * would follow a corrected offset and the other would not. */
	if (ep->swm->gphy_misc)
		iowrite32(1, ep->sw + ep->swm->gphy_misc);	/* PATCH_PHY_DONE */
	msleep(500);
	rtl9602c_gphy_tune(ep);
	/* The boot loader's own BMCR pair */
	{
		static const u16 bringup_bmcr[2] = { 0x3000, 0x1140 };
		unsigned int phy;

		mutex_lock(&rtl9602c_gphy_lock);
		for (phy = 0; phy < ARRAY_SIZE(bringup_bmcr); phy++) {
			bool want_down = rtl9602c_uni_port_locked(phy) ||
					 (phy == 0 && ep->port0_serdes);
			u16 v = bringup_bmcr[phy], back = 0;

			if (want_down)
				v |= BMCR_PDOWN;
			rc = rtl9602c_gphy_write__locked(ep, phy, 0, v);
			if (!rc)
				rc = rtl9602c_gphy_read__locked(ep, phy, 0, &back);
			if (rc) {
				netdev_err(ep->ndev,
					   "gphy: [PHY-004] phy %u BMCR %04x did not land (%d); its power state is UNKNOWN, not assumed\n",
					   phy, v, rc);
				break;
			}
			/* A desired mask is not proof the PHY stayed down. */
			if (want_down && !(back & BMCR_PDOWN)) {
				netdev_err(ep->ndev,
					   "gphy: [PHY-005] phy %u is administratively LOCKED and reads BMCR %04x -- the power-down did not hold across the reset; refusing to bring the interface up over it\n",
					   phy, back);
				rc = -EIO;
				break;
			}
			/* The unlocked case is a failure too. */
			if (!want_down && (back & BMCR_PDOWN)) {
				netdev_err(ep->ndev,
					   "gphy: [PHY-006] phy %u is not locked and reads BMCR %04x -- it is powered DOWN, so that socket would link and carry nothing\n",
					   phy, back);
				rc = -EIO;
				break;
			}
			msleep(500);
		}
		mutex_unlock(&rtl9602c_gphy_lock);
		if (rc)
			return rc;
	}
	/*
	 * 0x230c4 stays a literal: this chip's chipdef calls it
	 * SVLAN_UPLINK_PMSK, luna_gpon_regs.h calls the same address
	 * SMI_CTRL_3, and the two never run on one silicon.
	 */
	iowrite32(0,          ep->sw + 0x230c4);	/* SVLAN uplink port */
	iowrite32(0x003fffff, ep->sw + SW_PISO_PORT);	/* port isolation, element 0 */
	iowrite32(0x003fffff, ep->sw + SW_PISO_PORT + 1 * SW_PISO_PORT_STRIDE);
	/* The same two registers this file already reaches */
	iowrite32(0x00000196, ep->sw + SW_FORCE_P_ABLTY(ep, ep->swm->cpu_port));
	iowrite32(0x00000fff, ep->sw + SW_ABLTY_FORCE_MODE(ep, ep->swm->cpu_port));
	if (ep->port0_serdes) {
		/* SGMII first, the mode the PHY was given at probe; the
		 * link poll moves both to HSGMII behind a 2.5G link. */
		rtl9602c_lan_sds_program(ep, ep->lan_sds_hsgmii);
	}
	iowrite32(0x00012bbd, ep->sw + SW_METER_TB_CTRL);	/* meter tick-token */
	iowrite32(0,          ep->sw + SW_VLAN_CTRL);	/* VLAN function disable */
	iowrite32(1, ep->sw + SW_VLAN_EGRESS_TAG);			/* VLAN keep-format p0-3 */
	iowrite32(1, ep->sw + SW_VLAN_EGRESS_TAG + 1 * 4);
	iowrite32(1, ep->sw + SW_VLAN_EGRESS_TAG + 2 * 4);
	iowrite32(1, ep->sw + SW_VLAN_EGRESS_TAG + 3 * 4);
	iowrite32(0, ep->sw + SW_MAC_CPU_TAG_CTRL);			/* CPU_TAG_CTRL=0 (re-armed in hw_program) */
	writel(readl(sysstat) | 1, sysstat);		/* patch done: set 0xB8000044 bit0 */
	return 0;
}

static int rtl9602c_eth_hw_start(struct rtl9602c_eth *ep)
{
	struct net_device *ndev = ep->ndev;
	u32 desnum;
	int ret;

	ret = rtl9602c_eth_alloc_rings(ep);
	if (ret) {
		rtl9602c_eth_free_rings(ep);
		return ret;
	}
	luna_gpon_nic_reset_begin();
	ep->closing = false;
	ep->stall_since = 0;

	/* Stock-faithful cold start (the TX-park fix) */
	rtl9602c_hw_stop(ep);
	rtl9602c_ipsel_cycle();
	/* re-establish the GMAC<->switch sync the reset tore down (the
	 * catch-22 breaker) -- and it is the FABRIC, so a failure is not
	 * something to carry on past. */
	ret = rtl9602c_uboot_swcore_bringup(ep);
	if (ret)
		goto fail;
	/* Faithful full stock-equivalent datapath init, in */
	rtl9602c_datapath_tables_init();
	rtl9602c_hw_program(ep);
	rtl9602c_tx_align(ep);	/* fresh engine: CDO=0 -> rot 0 */
	/*
	 * Registers a same-board diff of stock against this driver found set
	 * only on stock
	 */
	{
		void __iomem *s100 = (void __iomem *)0xb8000100ul;
		void __iomem *s104 = (void __iomem *)0xb8000104ul;
		writel(readl(s100) | BIT(8), s100);	/* 0x18000100 bit8 -> stock */
		writel(readl(s104) | BIT(2), s104);	/* 0x18000104 bit2 -> stock */
	
	}
	/* network-engine / IP-mux page 0x18001000 (KSEG1 0xb8001000). RMW the exact
	 * same-board-diff bits to stock-WORKING values (don't clobber dynamic bits):
	 *   0x18001000: set bit19 (stock 0x10281e6f vs mine 0x10201e6f)
	 *   0x18001098: clear bits19,20; set bits14,18 (stock 0x0004e123 vs mine 0x0018a123) */
	void __iomem *n000 = (void __iomem *)0xb8001000ul;
	void __iomem *n098 = (void __iomem *)0xb8001098ul;
	writel(readl(n000) | BIT(19), n000);
	writel((readl(n098) & ~(BIT(19) | BIT(20))) | BIT(14) | BIT(18), n098);
	/* Re-establish the FULL PON US/DS-NIC datapath against */
	gpon_pbo_init();
	/* Once per boot: ndo_stop never takes this down. sec 96. */
	goto hw_ready;

	/* Program the ring pointers. */
	ep_wr(ep, R_TxFDP1, ep->tx_ring_dma | DMA_BUS_WINDOW);
	/* TxCDO is NOT writable on the live inherited engine */
	/* Arm the UNUSED HW TX rings */
	{
		unsigned int k, oring = rtl9602c_omci_hwring(omci_tx_ring);

		for (k = 1; k <= 4; k++) {
			if (k == oring)
				continue;	/* OMCI ring already armed at its own base */
			ep_wr(ep, R_TxFDP(k), ep->dummy_ring_dma | DMA_BUS_WINDOW);
			iowrite16(0, ep->base + R_TxCDO(k));
		}
	}
	ep_wr(ep, R_RxFDP, ep->rx_ring_dma | DMA_BUS_WINDOW);
	/* RX ring0 size + flow-control thresholds (GMAC field packing). */
	desnum = luna_gmac_rxdesnum_pack(ep->rx_n, TH_ON_VAL, TH_OFF_VAL);
	ep_wr(ep, R_RxDesNum, desnum);
	ep_wr(ep, R_RxCDO, luna_gmac_rxcdo_pack(ep->rx_n));
	
	ep_wr(ep, R_RCR, 0x0000000E);
	ep_wr(ep, R_TCR, 0x00000C00);
	ep_wr(ep, R_CONFIG, 0x20000000);
	iowrite8(0x0A, ep->base + R_CMD);	/* CMD = RxChkSum|RxJumboSupport (keep working-RX baseline) */
	
	rtl9602c_cpu_tag_arm(ep);
	/* GMAC config regs that a LIVE stock ONU at O5 SETS but */
	ep_wr(ep, R_RXOKMUL, 0x010c0000);	/* stock O5 counter snapshot -- a MIB word, not config; see rtl9602c_hw_program() */
	ep_wr(ep, R_IMR0, IMR0_TX_BITS);	/* per-ring TX-completion IRQ mask, rings 0-5 (stock 0x3f) */
	iowrite16(IMR_RX_BITS, ep->base + R_IMR);	/* RX IRQ mask: RX_OK + RX-err + RDU (stock 0xf835) */
	ep_wr(ep, R_MSR, (ep_rd(ep, R_MSR) & 0x00ffffff) |
			((msr_top & 0xffu) << 24));	/* MSR top byte (0xf0 kills sparse TX) */
	iowrite32(0xffffffff, ep->base + R_MAR0);	/* MAR0: accept-all-multicast */
	iowrite32(0xffffffff, ep->base + R_MAR4);	/* MAR4 */
	/* IO_CMD1 = the exact stock start value 0x323f0001 */
	ep_wr(ep, R_IO_CMD1, IOCMD1_UBOOT);	/* legacy: re-assert the inherited config */
	ep_wr(ep, R_IO_CMD, IOCMD_UBOOT);	/* full CMD_CONFIG, RX+TX DMA enable (last) */
	rtl9602c_tx_align(ep);	/* SW producer -> live engine position (CDO) */

hw_ready:
	rtl9602c_sw_min_init(ep);	/* flood ingress to the CPU port */

	napi_enable(&ep->napi);
	/* Clear any IRQ status latched during bring-up before unmasking the line. */
	iowrite16(ioread16(ep->base + R_ISR), ep->base + R_ISR);
	ep_wr(ep, R_ISR1, ep_rd(ep, R_ISR1));
	if (ep->irq > 0) {
		ret = request_irq(ep->irq, rtl9602c_eth_isr, IRQF_SHARED,
				  ndev->name, ep);
		if (ret) {
			netdev_warn(ndev, "[SYS-023] request_irq(%d) failed (%d); pure-poll fallback\n",
				    ep->irq, ret);
			ep->irq = -1;
		}
	}
	/* IRQ-driven: the ISR schedules NAPI */
	timer_setup(&ep->poll_timer,
		    (ep->irq > 0) ? rtl9602c_eth_rekick_timer : rtl9602c_eth_poll, 0);
	mod_timer(&ep->poll_timer,
		  jiffies + ((ep->irq > 0) ? REKICK_INTERVAL : POLL_INTERVAL));

	ep->hw_up = true;
	rtl9602c_uni_rate_replay(ep);
	luna_gpon_nic_reset_end();
	return 0;

fail:
	/* A half-programmed fabric must not be reported as */
	{
		unsigned long flags;

		spin_lock_irqsave(&ep->tx_lock, flags);
		ep->closing = true;
		spin_unlock_irqrestore(&ep->tx_lock, flags);
	}
	rtl9602c_hw_stop(ep);
	luna_gpon_nic_reset_end();
	rtl9602c_eth_free_rings(ep);
	netdev_err(ndev,
		   "[SYS-024] switch bring-up failed (%d): eth0 stays DOWN rather than up on a half-programmed fabric\n",
		   ret);
	return ret;
}

/* The GMAC and the switch come up once, on the first open of any netdev
 * riding them (eth0, a jack's, pon0); no ndo_stop takes them down. */
static int rtl9602c_eth_hw_ensure(struct rtl9602c_eth *ep)
{
	if (ep->hw_up)
		return 0;
	return rtl9602c_eth_hw_start(ep);
}

static int rtl9602c_eth_open(struct net_device *ndev)
{
	struct rtl9602c_eth *ep = netdev_priv(ndev);
	int ret;

	ret = rtl9602c_eth_hw_ensure(ep);
	if (ret)
		return ret;
	netif_carrier_on(ndev);
	netif_start_queue(ndev);
	return 0;
}

/*
 * The GMAC keeps running: the DS/US OMCI and gpon0 ride its rings, and an OLT
 * gives up on an ONU whose OMCI stops, for example with O5 reached between
 * preinit's close and netifd's open.
 */
static int rtl9602c_eth_stop(struct net_device *ndev)
{
	netif_stop_queue(ndev);
	netif_carrier_off(ndev);
	return 0;
}

/* The in-band Realtek cpu-tag (rtl8_4, between SA and the EtherType): the
 * switch strips it and forwards to the ports in its mask. lan_tx_steer=2
 * sends a jack's frame with that jack alone in the mask. */
static int rtl9602c_cpu_tag_push(struct sk_buff *skb, u16 port_mask)
{
	__be16 *t;

	if (skb_cow_head(skb, RTL8_4_TAG_LEN))
		return -ENOMEM;
	skb_push(skb, RTL8_4_TAG_LEN);
	memmove(skb->data, skb->data + RTL8_4_TAG_LEN, 2 * ETH_ALEN);
	t = (__be16 *)(skb->data + 2 * ETH_ALEN);
	t[0] = htons(0x8899);		/* Realtek EtherType */
	t[1] = htons(0x0400);		/* protocol 0x04 (rtl8_4), reason 0 */
	t[2] = htons(0x0020);		/* LEARN_DIS (rtl8_4 word2) */
	t[3] = htons(port_mask);	/* CPU->switch forwarding port mask */
	return 0;
}

static unsigned int rtl9602c_lan_tx_mode(const struct rtl9602c_eth *ep)
{
	int mode = READ_ONCE(lan_tx_steer);

	return mode < 0 ? ep->lan_tx_default : (unsigned int)mode;
}

/* Hold @skb to one jack, by the mode in force. Under tx_lock, after the
 * ring-full test: a frame the ring refuses is requeued by the stack as it
 * was, so it must not carry a tag yet. */
static int rtl9602c_lan_steer(struct rtl9602c_eth *ep, struct sk_buff **pskb,
			      unsigned int port)
{
	struct sk_buff *skb = *pskb;

	switch (rtl9602c_lan_tx_mode(ep)) {
	case LAN_STEER_VLAN:
		if (READ_ONCE(ep->lan_steer_blocked))
			return 0;
		skb = vlan_insert_tag_set_proto(skb, htons(ETH_P_8021Q),
						SW_LAN_STEER_VID(port));
		if (!skb) {
			*pskb = NULL;	/* freed by the insert */
			return -ENOMEM;
		}
		*pskb = skb;
		return 0;
	case LAN_STEER_CPUTAG:
		return rtl9602c_cpu_tag_push(skb, BIT(port));
	default:
		return 0;
	}
}

/* Hand one frame to LAN ring 0. A plain frame (@port < 0, eth0's) the switch
 * forwards by its own lookup; a jack's (lan_ndev[@port]'s) is first held to
 * that jack. The bootloader's own TX path writes no cpu-tag descriptor
 * either: the descriptor-inserted tag gave no egress on this silicon. */
static netdev_tx_t rtl9602c_eth_lan_submit(struct rtl9602c_eth *ep,
					   struct sk_buff *skb,
					   struct net_device *ndev, int port)
{
	unsigned int i;
	unsigned int len;
	unsigned long flags;
	dma_addr_t da;
	u32 opts1;

	/* tx_lock taken first so the ring-full test precedes any */
	spin_lock_irqsave(&ep->tx_lock, flags);
	if (luna_gmac_tx_ring_full(ep->tx_head, ep->tx_dirty,
				   TX_RING_SIZE, OMCI_RESV)) {
		spin_unlock_irqrestore(&ep->tx_lock, flags);
		netif_stop_queue(ndev);
		return NETDEV_TX_BUSY;
	}
	if (port >= 0 && v2802_l2_lan_prepare(ep, skb, port)) {
		spin_unlock_irqrestore(&ep->tx_lock, flags);
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	/* Steering and DMA must include the padding in skb->len. */
	if (skb_put_padto(skb, ETH_ZLEN)) {
		spin_unlock_irqrestore(&ep->tx_lock, flags);
		return NETDEV_TX_OK;	/* skb freed by skb_put_padto */
	}
	if (port >= 0 && rtl9602c_lan_steer(ep, &skb, port)) {
		spin_unlock_irqrestore(&ep->tx_lock, flags);
		ndev->stats.tx_dropped++;
		if (skb)
			dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	len = skb->len;
	da = dma_map_single(ep->dev, skb->data, len, DMA_TO_DEVICE);
	if (dma_mapping_error(ep->dev, da)) {
		spin_unlock_irqrestore(&ep->tx_lock, flags);
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	i = tx_slot(ep, ep->tx_head);
	ep->tx_skb[i] = skb;
	ep->tx_buf_dma[i] = da;
	ep->tx_buf_len[i] = len;

	ep->tx_ring[i].addr = da | DMA_BUS_WINDOW;	/* TX desc.addr bus window |= 0x20000000 */
	/* PLAIN descriptor: no GMAC-inserted cpu-tag (the cputag/TAG_AWARE trio
	 * gave 0 egress even with the correct GMAC CPUtagCR -- reverted). What
	 * holds a jack's frame to its jack is in the frame itself, above. */
	ep->tx_ring[i].opts2 = 0;
	ep->tx_ring[i].opts3 = 0;
	ep->tx_ring[i].opts4 = 0;
	opts1 = D_OWN | D_FS | D_LS | D_TXCRC | (len & TXD_LEN_MASK);
	if (i == tx_eor_slot(ep))
		opts1 |= D_EOR;
	wmb();				/* descriptor body before ownership */
	ep->tx_ring[i].opts1 = opts1;
	wmb();

	ep->tx_head++;
	rtl9602c_eth_tx_fetch(ep);	/* stock GO handshake on every submit */
	ep_wr(ep, R_IO_CMD, ep_rd(ep, R_IO_CMD) | BIT(0));	/* kick ring 0 */
	spin_unlock_irqrestore(&ep->tx_lock, flags);

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += len;
	return NETDEV_TX_OK;
}

static netdev_tx_t rtl9602c_eth_xmit(struct sk_buff *skb,
				     struct net_device *ndev)
{
	return rtl9602c_eth_lan_submit(netdev_priv(ndev), skb, ndev, -1);
}

/* A jack's netdev transmits. A frame that came in on a jack was already put
 * on the other jack by the switch (one VLAN, flooding on) and is never sent
 * back to its own: the bridge's copy for a LAN port is dropped here, counted,
 * whatever the steering mode -- a steered copy would be the second one. */
static netdev_tx_t rtl9602c_lan_xmit(struct sk_buff *skb,
				     struct net_device *ndev)
{
	struct rtl9602c_lan *lan = netdev_priv(ndev);
	struct rtl9602c_eth *ep = lan->ep;
	unsigned int p;

	for (p = 0; p < LAN_PORTS; p++) {
		if (ep->lan_ndev[p] && skb->skb_iif == ep->lan_ndev[p]->ifindex &&
		    ((!luna_xpon_lan_tagged() && !v2802_l2_vlan_filtering(ep)) ||
		     READ_ONCE(v2802_l2.active))) {
			ep->dbg_lan_relay_skipped++;
			dev_kfree_skb_any(skb);
			return NETDEV_TX_OK;
		}
	}
	return rtl9602c_eth_lan_submit(ep, skb, ndev, lan->port);
}

/* Honour promiscuous/all-multi (the bridge enslaving eth0 requests promisc).
 * Without AcceptAllPhys the GMAC drops unicast frames whose DA != our station
 * MAC — i.e. exactly the LAN-client frames a router/bridge must receive and
 * forward. RCR bit0 = AcceptAllPhys. */
static void rtl9602c_eth_set_rx_mode(struct net_device *ndev)
{
	struct rtl9602c_eth *ep = netdev_priv(ndev);

	luna_eth_set_promisc(ep->base, rtl9602c_eth_want_promisc(ep));
}

static void rtl9602c_lan_set_rx_mode(struct net_device *ndev)
{
	struct rtl9602c_lan *lan = netdev_priv(ndev);

	luna_eth_set_promisc(lan->ep->base, rtl9602c_eth_want_promisc(lan->ep));
}


static const struct net_device_ops rtl9602c_eth_netdev_ops = {
	.ndo_get_port_parent_id = v2802_l2_parent_id,
	.ndo_open		= rtl9602c_eth_open,
	.ndo_stop		= rtl9602c_eth_stop,
	.ndo_start_xmit		= rtl9602c_eth_xmit,
	.ndo_set_rx_mode	= rtl9602c_eth_set_rx_mode,
	.ndo_set_mac_address	= rtl9602c_eth_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
};

/* ---- the LAN jacks' netdevs ----------------------------------------------
 * lan1 and lan2 (the DT ethernet-ports node) ride eth0's rings: RX is demuxed
 * by the descriptor's source port, TX is held to the jack (lan_tx_steer).
 * eth0 stays the conduit -- the rings, the IRQ, the station address the
 * jacks inherit -- and a bridge port only on a configuration from before. */
static void rtl9602c_lan_carrier_sync(struct rtl9602c_eth *ep, unsigned int port);

static int rtl9602c_lan_open(struct net_device *ndev)
{
	struct rtl9602c_lan *lan = netdev_priv(ndev);
	struct rtl9602c_eth *ep = lan->ep;
	int ret;

	ret = rtl9602c_eth_hw_ensure(ep);
	if (ret)
		return ret;
	/* Taken down by ndo_stop: open the jack's link again. A jack never
	 * opened was never gated, so a bridge built on eth0 alone is unaffected. */
	if (READ_ONCE(rtl9602c_uni_down_ports) & BIT(lan->port)) {
		ret = rtl9602c_lan_gate(ep, lan->port, false);
		if (ret)
			return ret;
	}
	rtl9602c_lan_carrier_sync(ep, lan->port);
	netif_start_queue(ndev);
	return 0;
}

static int rtl9602c_lan_stop(struct net_device *ndev)
{
	struct rtl9602c_lan *lan = netdev_priv(ndev);
	int ret;

	netif_stop_queue(ndev);
	/* The switch would keep forwarding through a jack nobody watches:
	 * hold its link down while the netdev is, as the bridge expects. */
	ret = rtl9602c_lan_gate(lan->ep, lan->port, true);
	if (ret)
		netdev_warn(ndev, "[LAN-007] the jack's link could not be held down (%d): it keeps forwarding while this netdev is down\n",
			    ret);
	netif_carrier_off(ndev);
	return 0;
}

/* The switch's own counters: hardware-forwarded traffic never reaches the
 * CPU, so the netdev's software counts would read almost nothing. */
static void rtl9602c_lan_get_stats64(struct net_device *ndev,
				     struct rtnl_link_stats64 *s)
{
	struct rtl9602c_lan *lan = netdev_priv(ndev);

	if (luna_mib_port_stats(lan->ep->sw, lan->ep->swm, lan->port, s))
		netdev_stats_to_stats64(s, &ndev->stats);
	else
		s->tx_dropped += ndev->stats.tx_dropped;
}

static const struct net_device_ops rtl9602c_lan_netdev_ops = {
	.ndo_get_port_parent_id = v2802_l2_parent_id,
	.ndo_open		= rtl9602c_lan_open,
	.ndo_stop		= rtl9602c_lan_stop,
	.ndo_start_xmit		= rtl9602c_lan_xmit,
	.ndo_set_rx_mode	= rtl9602c_lan_set_rx_mode,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_get_stats64	= rtl9602c_lan_get_stats64,
};

static void rtl9602c_lan_supported(const struct rtl9602c_lan *lan, unsigned long *mask)
{
	linkmode_zero(mask);
	linkmode_set_bit(ETHTOOL_LINK_MODE_TP_BIT, mask);
	linkmode_set_bit(ETHTOOL_LINK_MODE_Autoneg_BIT, mask);
	linkmode_set_bit(ETHTOOL_LINK_MODE_10baseT_Half_BIT, mask);
	linkmode_set_bit(ETHTOOL_LINK_MODE_10baseT_Full_BIT, mask);
	linkmode_set_bit(ETHTOOL_LINK_MODE_100baseT_Half_BIT, mask);
	linkmode_set_bit(ETHTOOL_LINK_MODE_100baseT_Full_BIT, mask);
	linkmode_set_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT, mask);
	linkmode_set_bit(ETHTOOL_LINK_MODE_Pause_BIT, mask);
	linkmode_set_bit(ETHTOOL_LINK_MODE_Asym_Pause_BIT, mask);
	if (lan->port == 0 && lan->ep->port0_serdes)
		linkmode_set_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT, mask);
}

static bool rtl9602c_lan_is_ext(const struct rtl9602c_lan *lan)
{
	return lan->port == 0 && lan->ep->port0_serdes;
}

/* Keeps the power bit: a port the OLT locked stays down, and takes the new
 * settings when it is unlocked. */
static int rtl9602c_lan_bmcr_set__locked(struct rtl9602c_eth *ep, unsigned int port, u16 val)
{
	int bmcr = rtl9602c_lan_phy_read__locked(ep, port, MII_BMCR);

	if (bmcr < 0)
		return bmcr;
	return rtl9602c_lan_phy_write__locked(ep, port, MII_BMCR, (bmcr & BMCR_PDOWN) | val);
}

static int rtl9602c_lan_restart_aneg__locked(struct rtl9602c_eth *ep, unsigned int port)
{
	return rtl9602c_lan_bmcr_set__locked(ep, port, BMCR_ANENABLE | BMCR_ANRESTART);
}

static int rtl9602c_lan_advertise__locked(struct rtl9602c_lan *lan, const unsigned long *adv)
{
	struct rtl9602c_eth *ep = lan->ep;
	int anar, gbcr, rc;

	anar = rtl9602c_lan_phy_read__locked(ep, lan->port, MII_ADVERTISE);
	if (anar < 0)
		return anar;
	gbcr = rtl9602c_lan_phy_read__locked(ep, lan->port, MII_CTRL1000);
	if (gbcr < 0)
		return gbcr;
	anar &= ~(ADVERTISE_ALL | ADVERTISE_PAUSE_CAP | ADVERTISE_PAUSE_ASYM);
	gbcr &= ~(ADVERTISE_1000FULL | ADVERTISE_1000HALF);
	rc = rtl9602c_lan_phy_write__locked(ep, lan->port, MII_ADVERTISE,
					    anar | linkmode_adv_to_mii_adv_t(adv));
	if (!rc)
		rc = rtl9602c_lan_phy_write__locked(ep, lan->port, MII_CTRL1000,
						    gbcr | linkmode_adv_to_mii_ctrl1000_t(adv));
	if (rc)
		return rc;
	if (rtl9602c_lan_is_ext(lan)) {
		int v = ext_mdio_read_c45(ep, MDIO_MMD_AN, MDIO_AN_10GBT_CTRL);

		if (v < 0)
			return v;
		v &= ~MDIO_AN_10GBT_CTRL_ADV2_5G;
		if (linkmode_test_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT, adv))
			v |= MDIO_AN_10GBT_CTRL_ADV2_5G;
		ext_mdio_write_c45(ep, MDIO_MMD_AN, MDIO_AN_10GBT_CTRL, v);
	}
	return rtl9602c_lan_restart_aneg__locked(ep, lan->port);
}

/* Speed and duplex are what link_work last saw of the port. */
static int rtl9602c_lan_get_link_ksettings(struct net_device *ndev,
					   struct ethtool_link_ksettings *cmd)
{
	struct rtl9602c_lan *lan = netdev_priv(ndev);
	struct rtl9602c_eth *ep = lan->ep;
	const struct rtl9602c_lan_link *l = &ep->lan_link[lan->port];
	int bmcr, anar, gbcr, lpa, stat1000, ctrl10g = 0, stat10g = 0;

	mutex_lock(&rtl9602c_gphy_lock);
	bmcr = rtl9602c_lan_phy_read__locked(ep, lan->port, MII_BMCR);
	anar = rtl9602c_lan_phy_read__locked(ep, lan->port, MII_ADVERTISE);
	gbcr = rtl9602c_lan_phy_read__locked(ep, lan->port, MII_CTRL1000);
	lpa = rtl9602c_lan_phy_read__locked(ep, lan->port, MII_LPA);
	stat1000 = rtl9602c_lan_phy_read__locked(ep, lan->port, MII_STAT1000);
	if (rtl9602c_lan_is_ext(lan)) {
		ctrl10g = ext_mdio_read_c45(ep, MDIO_MMD_AN, MDIO_AN_10GBT_CTRL);
		stat10g = ext_mdio_read_c45(ep, MDIO_MMD_AN, MDIO_AN_10GBT_STAT);
	}
	mutex_unlock(&rtl9602c_gphy_lock);
	if (bmcr < 0 || anar < 0 || gbcr < 0 || lpa < 0 || stat1000 < 0 ||
	    ctrl10g < 0 || stat10g < 0)
		return -EIO;

	rtl9602c_lan_supported(lan, cmd->link_modes.supported);
	linkmode_zero(cmd->link_modes.advertising);
	linkmode_zero(cmd->link_modes.lp_advertising);
	linkmode_set_bit(ETHTOOL_LINK_MODE_TP_BIT, cmd->link_modes.advertising);
	if (bmcr & BMCR_ANENABLE)
		linkmode_set_bit(ETHTOOL_LINK_MODE_Autoneg_BIT, cmd->link_modes.advertising);
	mii_adv_mod_linkmode_adv_t(cmd->link_modes.advertising, anar);
	mii_ctrl1000_mod_linkmode_adv_t(cmd->link_modes.advertising, gbcr);
	linkmode_mod_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT, cmd->link_modes.advertising,
			 ctrl10g & MDIO_AN_10GBT_CTRL_ADV2_5G);
	if (l->up && (bmcr & BMCR_ANENABLE)) {
		mii_lpa_mod_linkmode_lpa_t(cmd->link_modes.lp_advertising, lpa);
		mii_stat1000_mod_linkmode_lpa_t(cmd->link_modes.lp_advertising, stat1000);
		linkmode_mod_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT,
				 cmd->link_modes.lp_advertising,
				 stat10g & MDIO_AN_10GBT_STAT_LP2_5G);
	}
	cmd->base.port = PORT_TP;
	cmd->base.phy_address = rtl9602c_lan_is_ext(lan) ? ep->ext_phy : lan->port;
	cmd->base.autoneg = bmcr & BMCR_ANENABLE ? AUTONEG_ENABLE : AUTONEG_DISABLE;
	cmd->base.speed = l->up ? l->speed : SPEED_UNKNOWN;
	cmd->base.duplex = !l->up ? DUPLEX_UNKNOWN :
			   l->full ? DUPLEX_FULL : DUPLEX_HALF;
	return 0;
}

static int rtl9602c_lan_set_link_ksettings(struct net_device *ndev,
					   const struct ethtool_link_ksettings *cmd)
{
	struct rtl9602c_lan *lan = netdev_priv(ndev);
	__ETHTOOL_DECLARE_LINK_MODE_MASK(supported);
	int rc;

	if (cmd->base.autoneg == AUTONEG_ENABLE) {
		const unsigned long *adv = cmd->link_modes.advertising;

		rtl9602c_lan_supported(lan, supported);
		if (!linkmode_subset(adv, supported))
			return -EINVAL;
		if (!(linkmode_adv_to_mii_adv_t(adv) & ADVERTISE_ALL) &&
		    !linkmode_adv_to_mii_ctrl1000_t(adv) &&
		    !linkmode_test_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT, adv))
			return -EINVAL;
		mutex_lock(&rtl9602c_gphy_lock);
		rc = rtl9602c_lan_advertise__locked(lan, adv);
		mutex_unlock(&rtl9602c_gphy_lock);
		return rc;
	}
	/* Gigabit and up need autonegotiation for master and slave. */
	if (cmd->base.speed != SPEED_10 && cmd->base.speed != SPEED_100)
		return -EINVAL;
	if (cmd->base.duplex != DUPLEX_FULL && cmd->base.duplex != DUPLEX_HALF)
		return -EINVAL;
	mutex_lock(&rtl9602c_gphy_lock);
	rc = rtl9602c_lan_bmcr_set__locked(lan->ep, lan->port,
					   (cmd->base.speed == SPEED_100 ? BMCR_SPEED100 : 0) |
					   (cmd->base.duplex == DUPLEX_FULL ? BMCR_FULLDPLX : 0));
	mutex_unlock(&rtl9602c_gphy_lock);
	return rc;
}

static int rtl9602c_lan_nway_reset(struct net_device *ndev)
{
	struct rtl9602c_lan *lan = netdev_priv(ndev);
	int bmcr, rc;

	mutex_lock(&rtl9602c_gphy_lock);
	bmcr = rtl9602c_lan_phy_read__locked(lan->ep, lan->port, MII_BMCR);
	if (bmcr < 0)
		rc = bmcr;
	else if (!(bmcr & BMCR_ANENABLE))
		rc = -EINVAL;
	else
		rc = rtl9602c_lan_restart_aneg__locked(lan->ep, lan->port);
	mutex_unlock(&rtl9602c_gphy_lock);
	return rc;
}

static void rtl9602c_lan_get_pauseparam(struct net_device *ndev,
					struct ethtool_pauseparam *pause)
{
	struct rtl9602c_lan *lan = netdev_priv(ndev);
	int anar;

	mutex_lock(&rtl9602c_gphy_lock);
	anar = rtl9602c_lan_phy_read__locked(lan->ep, lan->port, MII_ADVERTISE);
	mutex_unlock(&rtl9602c_gphy_lock);
	if (anar < 0)
		return;
	pause->autoneg = AUTONEG_ENABLE;
	pause->rx_pause = !!(anar & ADVERTISE_PAUSE_CAP);
	pause->tx_pause = !(anar & ADVERTISE_PAUSE_CAP) != !(anar & ADVERTISE_PAUSE_ASYM);
}

/* Pause follows negotiation on a copper port. Port 0's ability is forced, so its
 * bits move here too: b5 TX, b6 RX; the link poll writes them back. */
static int rtl9602c_lan_set_pauseparam(struct net_device *ndev,
				       struct ethtool_pauseparam *pause)
{
	struct rtl9602c_lan *lan = netdev_priv(ndev);
	struct rtl9602c_eth *ep = lan->ep;
	int anar, rc;
	u8 fc = (pause->rx_pause ? FLOW_CTRL_RX : 0) | (pause->tx_pause ? FLOW_CTRL_TX : 0);

	if (!pause->autoneg)
		return -EINVAL;
	mutex_lock(&rtl9602c_gphy_lock);
	anar = rtl9602c_lan_phy_read__locked(ep, lan->port, MII_ADVERTISE);
	if (anar < 0) {
		rc = anar;
		goto out;
	}
	anar &= ~(ADVERTISE_PAUSE_CAP | ADVERTISE_PAUSE_ASYM);
	rc = rtl9602c_lan_phy_write__locked(ep, lan->port, MII_ADVERTISE,
					    anar | mii_advertise_flowctrl(fc));
	if (rc)
		goto out;
	if (rtl9602c_lan_is_ext(lan))
		ep->port0_pause_off = (pause->tx_pause ? 0 : BIT(5)) |
				      (pause->rx_pause ? 0 : BIT(6));
	rc = rtl9602c_lan_restart_aneg__locked(ep, lan->port);
out:
	mutex_unlock(&rtl9602c_gphy_lock);
	return rc;
}

/* MDIO_AN_EEE_ADV or MDIO_AN_EEE_LPABLE; the GPHY maps them at page 0xa5d. */
static int rtl9602c_lan_eee_read__locked(struct rtl9602c_lan *lan, u16 reg)
{
	u16 v;
	int rc;

	if (rtl9602c_lan_is_ext(lan))
		return ext_mdio_read_c45(lan->ep, MDIO_MMD_AN, reg);
	rc = rtl9602c_gphy_ocp_read__locked(lan->ep, lan->port,
					    0xa5d0 + 2 * (reg - MDIO_AN_EEE_ADV), &v);
	return rc ?: v;
}

static int rtl9602c_lan_get_eee(struct net_device *ndev, struct ethtool_keee *eee)
{
	struct rtl9602c_lan *lan = netdev_priv(ndev);
	const struct rtl9602c_lan_link *l = &lan->ep->lan_link[lan->port];
	int adv, lp, adv2 = 0, lp2 = 0;
	bool common;

	mutex_lock(&rtl9602c_gphy_lock);
	adv = rtl9602c_lan_eee_read__locked(lan, MDIO_AN_EEE_ADV);
	lp = rtl9602c_lan_eee_read__locked(lan, MDIO_AN_EEE_LPABLE);
	if (rtl9602c_lan_is_ext(lan)) {
		adv2 = ext_mdio_read_c45(lan->ep, MDIO_MMD_AN, MDIO_AN_EEE_ADV2);
		lp2 = ext_mdio_read_c45(lan->ep, MDIO_MMD_AN, MDIO_AN_EEE_LPABLE2);
	}
	mutex_unlock(&rtl9602c_gphy_lock);
	if (adv < 0 || lp < 0 || adv2 < 0 || lp2 < 0)
		return -EIO;

	linkmode_set_bit(ETHTOOL_LINK_MODE_100baseT_Full_BIT, eee->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT, eee->supported);
	mii_eee_cap1_mod_linkmode_t(eee->advertised, adv);
	mii_eee_cap1_mod_linkmode_t(eee->lp_advertised, lp);
	if (rtl9602c_lan_is_ext(lan)) {
		linkmode_set_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT, eee->supported);
		linkmode_mod_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT, eee->advertised,
				 adv2 & MDIO_EEE_2_5GT);
		linkmode_mod_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT, eee->lp_advertised,
				 lp2 & MDIO_EEE_2_5GT);
	}
	switch (l->speed) {
	case SPEED_100:
		common = adv & lp & MDIO_EEE_100TX;
		break;
	case SPEED_1000:
		common = adv & lp & MDIO_EEE_1000T;
		break;
	case SPEED_2500:
		common = adv2 & lp2 & MDIO_EEE_2_5GT;
		break;
	default:
		common = false;
	}
	eee->eee_enabled = adv || adv2;
	eee->eee_active = l->up && common;
	eee->tx_lpi_enabled = eee->eee_enabled;
	return 0;
}

static int rtl9602c_lan_set_eee(struct net_device *ndev, struct ethtool_keee *eee)
{
	struct rtl9602c_lan *lan = netdev_priv(ndev);
	struct rtl9602c_eth *ep = lan->ep;
	u16 adv = MDIO_EEE_100TX | MDIO_EEE_1000T, adv2 = MDIO_EEE_2_5GT;
	int bmcr, rc = 0;

	if (!eee->eee_enabled) {
		adv = adv2 = 0;
	} else if (!linkmode_empty(eee->advertised)) {
		adv = linkmode_to_mii_eee_cap1_t(eee->advertised) & adv;
		adv2 = linkmode_test_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT, eee->advertised) ?
		       MDIO_EEE_2_5GT : 0;
	}
	mutex_lock(&rtl9602c_gphy_lock);
	if (rtl9602c_lan_is_ext(lan)) {
		ext_mdio_write_c45(ep, MDIO_MMD_AN, MDIO_AN_EEE_ADV, adv);
		ext_mdio_write_c45(ep, MDIO_MMD_AN, MDIO_AN_EEE_ADV2, adv2);
	} else {
		rc = rtl9602c_gphy_ocp_write__locked(ep, lan->port, 0xa5d0, adv);
	}
	bmcr = rc ?: rtl9602c_lan_phy_read__locked(ep, lan->port, MII_BMCR);
	if (bmcr < 0)
		rc = bmcr;
	else if (bmcr & BMCR_ANENABLE)
		rc = rtl9602c_lan_restart_aneg__locked(ep, lan->port);
	mutex_unlock(&rtl9602c_gphy_lock);
	return rc;
}

/* G.988's pause time asks the subscriber's equipment to stop for that many quanta:
 * one 802.3x PAUSE frame on a change, zero being the frame that resumes it. omcid
 * replays the attribute on every provisioning change, which must not pause again. */
static int rtl9602c_uni_pause_set(void *cookie, unsigned int port, u16 quanta)
{
	static const u8 pause_addr[ETH_ALEN] __aligned(2) = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x01 };
	struct rtl9602c_eth *ep = cookie;
	struct net_device *ndev;
	struct sk_buff *skb;
	u8 *p;

	if (port >= LAN_PORTS || !ep->lan_ndev[port])
		return quanta ? -EINVAL : 0;
	if (quanta == ep->uni_pause[port])
		return 0;
	ep->uni_pause[port] = quanta;
	ndev = ep->lan_ndev[port];
	if (!netif_carrier_ok(ndev))
		return 0;
	skb = netdev_alloc_skb(ndev, ETH_ZLEN);
	if (!skb)
		return -ENOMEM;
	p = skb_put_zero(skb, ETH_ZLEN);
	ether_addr_copy(p, pause_addr);
	ether_addr_copy(p + ETH_ALEN, ndev->dev_addr);
	put_unaligned_be16(ETH_P_PAUSE, p + 12);
	put_unaligned_be16(1, p + 14);	/* PAUSE opcode */
	put_unaligned_be16(quanta, p + 16);
	skb->protocol = htons(ETH_P_PAUSE);
	dev_queue_xmit(skb);
	dev_info(ep->dev, "%s: sent PAUSE, %u quanta\n", ndev->name, quanta);
	return 0;
}

/* G.988 PPTP Ethernet UNI auto-detection, as what the port advertises. */
static const struct {
	u8 mode;
	bool half, full;
	u8 speeds;	/* bit 0 10M, 1 100M, 2 1G, 3 2.5G */
} rtl9602c_uni_autodetect_modes[] = {
	{ 0x00, true, true, 0xf }, { 0x01, false, true, 0x1 }, { 0x02, false, true, 0x2 },
	{ 0x03, false, true, 0x4 }, { 0x04, false, true, 0xf }, { 0x10, true, true, 0x1 },
	{ 0x11, true, false, 0x1 }, { 0x12, true, false, 0x2 }, { 0x14, true, false, 0x3 },
	{ 0x20, false, true, 0x4 }, { 0x30, true, true, 0x2 },
};

static int rtl9602c_uni_autodetect_set(void *cookie, unsigned int port, u8 mode)
{
	struct rtl9602c_eth *ep = cookie;
	__ETHTOOL_DECLARE_LINK_MODE_MASK(adv);
	struct rtl9602c_lan *lan;
	unsigned int i;
	int anar, rc;

	if (port >= LAN_PORTS || !ep->lan_ndev[port])
		return mode ? -EINVAL : 0;
	if (mode == ep->uni_autodetect[port])
		return 0;
	for (i = 0; i < ARRAY_SIZE(rtl9602c_uni_autodetect_modes); i++)
		if (rtl9602c_uni_autodetect_modes[i].mode == mode)
			break;
	if (i == ARRAY_SIZE(rtl9602c_uni_autodetect_modes))
		return -EOPNOTSUPP;

	lan = netdev_priv(ep->lan_ndev[port]);
	linkmode_zero(adv);
	linkmode_set_bit(ETHTOOL_LINK_MODE_Autoneg_BIT, adv);
	if (rtl9602c_uni_autodetect_modes[i].speeds & BIT(0)) {
		linkmode_mod_bit(ETHTOOL_LINK_MODE_10baseT_Half_BIT, adv,
				 rtl9602c_uni_autodetect_modes[i].half);
		linkmode_mod_bit(ETHTOOL_LINK_MODE_10baseT_Full_BIT, adv,
				 rtl9602c_uni_autodetect_modes[i].full);
	}
	if (rtl9602c_uni_autodetect_modes[i].speeds & BIT(1)) {
		linkmode_mod_bit(ETHTOOL_LINK_MODE_100baseT_Half_BIT, adv,
				 rtl9602c_uni_autodetect_modes[i].half);
		linkmode_mod_bit(ETHTOOL_LINK_MODE_100baseT_Full_BIT, adv,
				 rtl9602c_uni_autodetect_modes[i].full);
	}
	if ((rtl9602c_uni_autodetect_modes[i].speeds & BIT(2)) &&
	    rtl9602c_uni_autodetect_modes[i].full)
		linkmode_set_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT, adv);
	if ((rtl9602c_uni_autodetect_modes[i].speeds & BIT(3)) &&
	    rtl9602c_uni_autodetect_modes[i].full && rtl9602c_lan_is_ext(lan))
		linkmode_set_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT, adv);

	mutex_lock(&rtl9602c_gphy_lock);
	anar = rtl9602c_lan_phy_read__locked(ep, port, MII_ADVERTISE);
	if (anar < 0) {
		rc = anar;
	} else {
		linkmode_mod_bit(ETHTOOL_LINK_MODE_Pause_BIT, adv, anar & ADVERTISE_PAUSE_CAP);
		linkmode_mod_bit(ETHTOOL_LINK_MODE_Asym_Pause_BIT, adv, anar & ADVERTISE_PAUSE_ASYM);
		rc = rtl9602c_lan_advertise__locked(lan, adv);
	}
	mutex_unlock(&rtl9602c_gphy_lock);
	if (!rc) {
		ep->uni_autodetect[port] = mode;
		dev_info(ep->dev, "%s: OLT set auto-detection %#x\n", ep->lan_ndev[port]->name, mode);
	}
	return rc;
}

static void rtl9602c_lan_get_drvinfo(struct net_device *ndev,
				     struct ethtool_drvinfo *di)
{
	strscpy(di->driver, "rtl9602c-eth", sizeof(di->driver));
	strscpy(di->bus_info, dev_name(ndev->dev.parent), sizeof(di->bus_info));
}

static const struct ethtool_ops rtl9602c_lan_ethtool_ops = {
	.get_drvinfo		= rtl9602c_lan_get_drvinfo,
	.get_link		= ethtool_op_get_link,
	.get_link_ksettings	= rtl9602c_lan_get_link_ksettings,
	.set_link_ksettings	= rtl9602c_lan_set_link_ksettings,
	.nway_reset		= rtl9602c_lan_nway_reset,
	.get_pauseparam		= rtl9602c_lan_get_pauseparam,
	.set_pauseparam		= rtl9602c_lan_set_pauseparam,
	.get_eee		= rtl9602c_lan_get_eee,
	.set_eee		= rtl9602c_lan_set_eee,
};

/* The board's jacks, from its DT:
 *	ethernet-ports {
 *		port@1 { reg = <1>; label = "lan1"; };
 *	};
 * reg is the switch port, label the netdev's name. No node: eth0 alone
 * carries the LAN, as before. */
static int rtl9602c_lan_register(struct rtl9602c_eth *ep)
{
	struct device *dev = ep->dev;
	struct device_node *ports, *pn;
	int ret = 0;

	ports = of_get_child_by_name(dev->of_node, "ethernet-ports");
	if (!ports)
		return 0;
	for_each_available_child_of_node(ports, pn) {
		struct net_device *ndev;
		struct rtl9602c_lan *lan;
		const char *label;
		u32 port;

		if (of_property_read_u32(pn, "reg", &port) || port >= LAN_PORTS ||
		    port >= ep->swm->n_copper || ep->lan_ndev[port] ||
		    of_property_read_string(pn, "label", &label) || !*label) {
			dev_err(dev, "[LAN-008] ethernet-ports/%pOFn: needs reg (one of this board's %u copper ports, once) and a label\n",
				pn, min_t(unsigned int, LAN_PORTS, ep->swm->n_copper));
			ret = -EINVAL;
			break;
		}
		ndev = devm_alloc_etherdev(dev, sizeof(*lan));
		if (!ndev) {
			ret = -ENOMEM;
			break;
		}
		SET_NETDEV_DEV(ndev, dev);
		strscpy(ndev->name, label, IFNAMSIZ);
		lan = netdev_priv(ndev);
		lan->ep = ep;
		lan->port = port;
		ndev->netdev_ops = &rtl9602c_lan_netdev_ops;
		ndev->ethtool_ops = &rtl9602c_lan_ethtool_ops;
		ndev->priv_flags |= IFF_LIVE_ADDR_CHANGE;
		ndev->needed_headroom = RTL8_4_TAG_LEN;	/* a steering tag's room */
		eth_hw_addr_set(ndev, ep->ndev->dev_addr);
		netif_carrier_off(ndev);
		ret = devm_register_netdev(dev, ndev);
		if (ret) {
			dev_err(dev, "[LAN-009] %s (switch port %u) not registered: %d\n",
				label, port, ret);
			break;
		}
		ep->lan_ndev[port] = ndev;
		dev_info(dev, "%s: switch port %u\n", ndev->name, port);
	}
	if (ret)
		of_node_put(pn);
	of_node_put(ports);
	return ret;
}

/* /proc/ethdump `omci_inject`: 5 baseline OMCI frames onto the US OMCC TX path,
 * with no OLT. Run at O5; ustx (0x1b0329bc) climbing means the steering works. */
static ssize_t rtl9602c_ethdump_write(struct file *f, const char __user *ubuf,
				      size_t cnt, loff_t *off)
{
	struct rtl9602c_eth *ep = g_ep;
	unsigned int k;
	u8 msg[48];
	char verb[24];

	if (!ep)
		return -ENODEV;
	if (!cnt || cnt >= sizeof(verb))
		return -EINVAL;
	if (copy_from_user(verb, ubuf, cnt))
		return -EFAULT;
	verb[cnt] = '\0';
	if (strncmp(verb, "omci_inject", 11))
		return -EINVAL;
	/* Arm the GMAC OMCI state (CPUTAGCR=0x901eff04, the cpu-tag trap + routing) as
	 * the OLT-driven path would via gpon_install_omcc -> set_omci_sid. Without the
	 * OLT the GMAC sits at the non-OMCI 0x981aff04, so the self-test must arm it to
	 * test the cpu-tag insertion representatively. Idempotent. */
	rtl9602c_eth_set_omci_sid(RTL9602C_OMCC_SID);
	for (k = 0; k < 5; k++) {
		memset(msg, 0, sizeof(msg));
		msg[0] = 0x00; msg[1] = 0x42;	/* TID */
		msg[2] = 0x2f;			/* MIB-Reset response (AK) */
		msg[3] = 0x0a;			/* DevID baseline */
		msg[4] = 0x00; msg[5] = 0x02;	/* ME ONU-data */
		omci_finalize(msg);		/* core: trailer + MIC */
		rtl9602c_eth_omci_xmit(ep, msg, sizeof(msg));
	}
	return cnt;
}

static int rtl9602c_ethdump_show(struct seq_file *m, void *v)
{
	struct rtl9602c_eth *ep = g_ep;
	unsigned int i, own = 0, hwfilled = 0;

	if (!ep) { seq_puts(m, "no device\n"); return 0; }
	if (ep->in_recovery) {	/* GMAC may be power-gated: MMIO would bus-abort */
		seq_printf(m, "GMAC recovery in progress (recovers=%u)\n",
			   ep->dbg_tx_recover);
		return 0;
	}

	for (i = 0; i < ep->rx_n; i++) {
		if (ep->rx_ring[i].opts1 & D_OWN)
			own++;
		else
			hwfilled++;
	}
	seq_printf(m, "poll=%u filled=%u good=%u err=%u rx_head=%u rx_repoll=%u\n",
		   ep->dbg_poll, ep->dbg_filled, ep->dbg_good, ep->dbg_err,
		   ep->rx_head, ep->dbg_rx_repoll);
	seq_printf(m, "last RX frame (pre-pull, len=%u): %*ph\n",
		   ep->dbg_rxlen, (int)sizeof(ep->dbg_rxbuf), ep->dbg_rxbuf);
	seq_printf(m, "omci: trap=%u rx=%u lastlen=%u msg=%*ph\n",
		   ep->omci_trap_on, ep->dbg_omci_rx, ep->dbg_omci_rxlen,
		   (int)sizeof(ep->dbg_omci_rxbuf), ep->dbg_omci_rxbuf);
	seq_printf(m, "omci_tx: resp=%u drop=%u unhandled=%u mds=%u sn=%*ph\n",
		   ep->dbg_omci_tx, ep->dbg_omci_tx_drop, ep->dbg_omci_unhandled,
		   ep->omci_mds, 8, ep->omci_sn);
	/*
	 * The shared core's own DS discard counters. Two numbers, not one: a
	 * runt is a framing or GEM-reassembly fault upstream of OMCI, and a bad
	 * MIC is corruption on a well-framed PDU.
	 */
	{
		u32 bad_mic, runt;

		luna_omci_rx_errors(&bad_mic, &runt);
		seq_printf(m, "omci_rx_drop: bad_mic=%u runt=%u\n", bad_mic, runt);
	}
	{
		/* Same flowcore derivation as the xmit kick, so the diag can
		 * never disagree with the doorbell actually used. */

		/* Shared-ring-0 OMCI test path */
		int own_omci = -1;
		int slot = ep->omci_r0_last_slot;

		if (ep->tx_ring && slot >= 0 && slot < TX_RING_SIZE)
			own_omci = !!(ep->tx_ring[slot].opts1 & D_OWN);

		seq_printf(m,
			"omci_txring: PATH=shared-LAN-ring0 sid_idx=%u pon=%u doorbell=R_IO_CMD bit0  LANring head=%u dirty=%u  omci_slot=%d own[omci_slot]=%d  TxFDP1=%08x lanRingDMA=%08x IO_CMD=%08x\n",
			omci_sid_idx, omci_pon_port,
			ep->tx_head, ep->tx_dirty, slot, own_omci,
			ep_rd(ep, R_TxFDP1), (u32)ep->tx_ring_dma,
			ep_rd(ep, R_IO_CMD));
	}
	seq_printf(m, "rxring: HW-owned(D_OWN=1)=%u  CPU-owned(filled)=%u\n",
		   own, hwfilled);
	seq_printf(m, "lan: steer=%u blocked=%u relay_skipped=%u lan2(p0)=%s/%u lan1(p1)=%s/%u\n",
		   rtl9602c_lan_tx_mode(ep), ep->lan_steer_blocked, ep->dbg_lan_relay_skipped,
		   ep->lan_link[0].up ? "up" : "down", ep->lan_link[0].speed,
		   ep->lan_link[1].up ? "up" : "down", ep->lan_link[1].speed);
	if (ep->port0_serdes && ep->sw) {
		int opt = -1, mode = -1;

		if (ep->ext_phy_ok && mutex_trylock(&rtl9602c_gphy_lock)) {
			opt = ext_mdio_read_c45(ep, MDIO_MMD_VEND1_RTL, RTL8221B_VND1_SERDES_OPTION);
			mode = ext_mdio_read_c45(ep, MDIO_MMD_VEND1_RTL, RTL8221B_VND1_SERDES_MODE);
			mutex_unlock(&rtl9602c_gphy_lock);
		}
		/* soc mode: 02 SGMII, 12 HSGMII, 1f off; phy mode: 02 SGMII, 12 HSGMII, 16 2500BASE-X */
		seq_printf(m, "lan2 serdes: lan_sds=%u set=%u soc 0x214=%08x (%s) force_ablty=%08x phy 0x697a=%04x (wrote %u) 0x7580=%04x\n",
			   1, ep->lan_sds_set, ioread32(ep->sw + SW_LAN_SDS_MODE),
			   ep->lan_sds_hsgmii ? "hsgmii" : "sgmii",
			   ioread32(ep->sw + SW_FORCE_P_ABLTY(ep, 0)), opt, ep->ext_phy_option, mode);
	}
	{
		/* TX ring OWN bitmap (slot 0 = LSB of the first hex word):
		 * separates "HW transmits but never clears OWN" from reclaim
		 * bugs at a glance. */
		u32 bm0 = 0, bm1 = 0;

		for (i = 0; i < 32; i++) {
			if (ep->tx_ring[i].opts1 & D_OWN)
				bm0 |= 1u << i;
			if (ep->tx_ring[i + 32].opts1 & D_OWN)
				bm1 |= 1u << i;
		}
		seq_printf(m, "txring own[31:0]=%08x own[63:32]=%08x head=%u dirty=%u rot=%u\n",
			   bm0, bm1, ep->tx_head, ep->tx_dirty, ep->tx_rot);
	}
	seq_printf(m, "GMAC IO_CMD=%08x IO_CMD1=%08x MSR(0x58)=%08x\n",
		   ep_rd(ep, R_IO_CMD), ep_rd(ep, R_IO_CMD1),
		   ep_rd(ep, R_MSR));
	/* TX-DMA park forensics: per-ring HW fetch cursors (descriptor index
	 * relative to TxFDP), the ring rotations, and the recovery counters.
	 * cdo0+rot vs (dirty%64) localises a park instantly. */
	seq_printf(m, "txdma cdo0=%u cdo1=%u cdo2=%u cdo3=%u cdo4=%u rot=%u/%u stall_ms=%u rearms=%u recovers=%u gmac_reset=%u\n",
		   ioread16(ep->base + R_TxCDO(0)), ioread16(ep->base + R_TxCDO(1)),
		   ioread16(ep->base + R_TxCDO(2)), ioread16(ep->base + R_TxCDO(3)),
		   ioread16(ep->base + R_TxCDO(4)), ep->tx_rot, ep->otx_rot,
		   ep->stall_since ? jiffies_to_msecs(jiffies - ep->stall_since) : 0,
		   ep->dbg_rearm, ep->dbg_tx_recover, 1);
	seq_printf(m, "GMAC RCR=%08x TCR=%08x CONFIG=%08x CPUTAGCR=%08x\n",
		   ep_rd(ep, R_RCR), ep_rd(ep, R_TCR), ep_rd(ep, R_CONFIG),
		   ep_rd(ep, R_CPUTAGCR));
	/* (GMAC1 0x18014000 / GMAC2 0x18016000 are DEAD MMIO on the 9602C — reading
	 * them bus-aborts the whole diag. The 9602C has only GMAC0; the d1 gmac_id=2
	 * is a 9607C-ism. US OMCI must egress GMAC0.) */
	seq_printf(m, "GMAC RxFDP=%08x RxCDO=%08x RxDesNum=%08x ringDMA=%08x\n",
		   ep_rd(ep, R_RxFDP), ep_rd(ep, R_RxCDO), ep_rd(ep, R_RxDesNum),
		   (u32)ep->rx_ring_dma);
	/* Full GMAC config diff vs LIVE stock O5 */
	seq_printf(m, "GMACcfg 10=%08x[f:04a80457] 20=%08x[034c0003] 24=%08x[010c0000] 38=%08x[0a] 3c=%08x[f8350240]\n",
		   ep_rd(ep, R_TXOKCNT), ep_rd(ep, R_RXOKPHY), ep_rd(ep, R_RXOKMUL),
		   ep_rd(ep, 0x38), ep_rd(ep, 0x3c));
	seq_printf(m, "GMACcfg 44=%08x[0f] 58=%08x[f0638000] 5c=%08x[04000000] d0=%08x[3f] d8=%08x[11110000]\n",
		   ep_rd(ep, R_RCR), ep_rd(ep, R_MSR), ep_rd(ep, 0x5c),
		   ep_rd(ep, R_IMR0), ep_rd(ep, R_ISR1));
	/* GMAC0 MAC-level MIB counters */
	seq_printf(m, "GMAC_MIB txok=%u rxok=%u txerr=%u rxerr=%u miss=%u\n",
		   ep_rd(ep, R_TXOKCNT) >> 16, ep_rd(ep, R_TXOKCNT) & 0xffff,
		   ep_rd(ep, R_TXERR) >> 16, ep_rd(ep, R_TXERR) & 0xffff,
		   ep_rd(ep, R_MISSPKT) >> 16);
	/* NIC interrupt status */
	seq_printf(m, "NIC ISR(0x3c=[IMR:ISR])=%08x ISR1(0xd8)=%08x  perRingRxCDO r0=%04x r1=%04x r2=%04x r3=%04x r4=%04x r5=%04x\n",
		   ep_rd(ep, 0x3c), ep_rd(ep, R_ISR1),
		   ep_rd(ep, R_RxCDO) >> 16, ep_rd(ep, R_RxMRingCDO(1)) >> 16,
		   ep_rd(ep, R_RxMRingCDO(2)) >> 16, ep_rd(ep, R_RxMRingCDO(3)) >> 16,
		   ep_rd(ep, R_RxMRingCDO(4)) >> 16, ep_rd(ep, R_RxMRingCDO(5)) >> 16);
	if (ep->sw) {
		seq_printf(m, "SW permit(1c088)=%08x flood bc/mc/uc=%08x/%08x/%08x\n",
			   ioread32(ep->sw + ep->swm->src_permit),
			   ioread32(ep->sw + ep->swm->bc_flood),
			   ioread32(ep->sw + ep->swm->unkn_mc_flood),
			   ioread32(ep->sw + ep->swm->unkn_uc_flood));
		seq_printf(m, "SW vlan_ctrl(13008)=%08x cputag_ctrl(23030)=%08x\n",
			   ioread32(ep->sw + SW_VLAN_CTRL),
			   ioread32(ep->sw + SW_MAC_CPU_TAG_CTRL));
		/* These four are read, not understood. */
		seq_printf(m, "SW sw(198)=%08x sw(1b8)=%08x sw(1d8)=%08x sw(1f8)=%08x\n",
			   ioread32(ep->sw + 0x198), ioread32(ep->sw + 0x1b8),
			   ioread32(ep->sw + 0x1d8), ioread32(ep->sw + 0x1f8));
		/* The RX addresses below do not follow the formula. */
		{
			unsigned int p;

			for (p = 0; p < LAN_PORTS; p++) {
				if (!ep->lan_ndev[p])
					continue;
				seq_printf(m, "link_drops %s: %u\n", ep->lan_ndev[p]->name,
					   ep->lan_drops[p]);
				/* Not BMSR: reading it would clear the latch the link poll counts drops by. */
				if (mutex_trylock(&rtl9602c_gphy_lock)) {
					struct rtl9602c_lan *lan = netdev_priv(ep->lan_ndev[p]);

					seq_printf(m, "phy %s: bmcr=%04x anar=%04x gbcr=%04x lpa=%04x stat1000=%04x eee_adv=%04x eee_lp=%04x\n",
						   ep->lan_ndev[p]->name,
						   rtl9602c_lan_phy_read__locked(ep, p, MII_BMCR) & 0xffff,
						   rtl9602c_lan_phy_read__locked(ep, p, MII_ADVERTISE) & 0xffff,
						   rtl9602c_lan_phy_read__locked(ep, p, MII_CTRL1000) & 0xffff,
						   rtl9602c_lan_phy_read__locked(ep, p, MII_LPA) & 0xffff,
						   rtl9602c_lan_phy_read__locked(ep, p, MII_STAT1000) & 0xffff,
						   rtl9602c_lan_eee_read__locked(lan, MDIO_AN_EEE_ADV) & 0xffff,
						   rtl9602c_lan_eee_read__locked(lan, MDIO_AN_EEE_LPABLE) & 0xffff);
					mutex_unlock(&rtl9602c_gphy_lock);
				}
				seq_printf(m, "uni_rate %s: cached=%u down_raw=%u up_raw=%u hw_down_raw=%u hw_up_raw=%u\n",
					   ep->lan_ndev[p]->name, ep->uni_rate[p].valid,
					   ep->uni_rate[p].down, ep->uni_rate[p].up,
					   (ioread32(ep->sw + SW_EGR_BWCTRL_P_CTRL + 4 * p) >> 1) & UNI_RATE_MAX,
					   (ioread32(ep->sw + SW_IGR_BWCTRL_P_CTRL + ep->swm->macpp_stride * p) >> 2) & UNI_RATE_MAX);
			}
		}
		if (ep->port0_serdes)
			seq_printf(m, "uni_rate_hsgmii: active=%u raw=%u word=%08x\n",
				   ep->lan_sds_hsgmii,
				   ioread32(ep->sw + SW_LAN_SDS_MISC) & UNI_RATE_MAX,
				   ioread32(ep->sw + SW_LAN_SDS_MISC));
		seq_printf(m, "MIB p0(LAN) tx=%08x | p1(LAN) tx=%08x\n",
			   ioread32(ep->sw + SW_STAT_PORT_TX_MIB), ioread32(ep->sw + SW_STAT_PORT_TX_MIB + 1 * SW_STAT_PORT_MIB_STRIDE));
		seq_printf(m, "MIB p2(PON) tx=%08x %08x %08x | rx=%08x %08x %08x\n",
			   ioread32(ep->sw + SW_STAT_PORT_TX_MIB + 2 * SW_STAT_PORT_MIB_STRIDE), ioread32(ep->sw + SW_STAT_PORT_TX_MIB + 2 * SW_STAT_PORT_MIB_STRIDE + 4),
			   ioread32(ep->sw + SW_STAT_PORT_TX_MIB + 2 * SW_STAT_PORT_MIB_STRIDE + 8), ioread32(ep->sw + 0x32500),
			   ioread32(ep->sw + 0x32504), ioread32(ep->sw + 0x32508));
		seq_printf(m, "MIB p3(CPU) tx=%08x %08x %08x | rx=%08x %08x %08x\n",
			   ioread32(ep->sw + SW_STAT_PORT_TX_MIB + 3 * SW_STAT_PORT_MIB_STRIDE), ioread32(ep->sw + SW_STAT_PORT_TX_MIB + 3 * SW_STAT_PORT_MIB_STRIDE + 4),
			   ioread32(ep->sw + SW_STAT_PORT_TX_MIB + 3 * SW_STAT_PORT_MIB_STRIDE + 8), ioread32(ep->sw + 0x32600),
			   ioread32(ep->sw + 0x32604), ioread32(ep->sw + 0x32608));
	}
	return 0;
}

static int rtl9602c_ethdump_open(struct inode *ino, struct file *fp)
{
	return single_open(fp, rtl9602c_ethdump_show, NULL);
}

static const struct proc_ops rtl9602c_ethdump_pops = {
	.proc_open	= rtl9602c_ethdump_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= rtl9602c_ethdump_write,
};

static int rtl9602c_eth_omci_send(void *cookie, const u8 *msg, unsigned int len)
{
	return rtl9602c_eth_omci_xmit(cookie, msg, len);
}

static void rtl9602c_eth_tx_fence(void *cookie)
{
	struct rtl9602c_eth *ep = cookie;
	unsigned long flags;

	spin_lock_irqsave(&ep->tx_lock, flags);
	spin_unlock_irqrestore(&ep->tx_lock, flags);
}

/* A UNI is a COPPER port.  The PON and CPU ports share this index space, so a
 * mis-declared panel would otherwise drive a PHY that is not there and then
 * report the reserved port LOCKED. */
static bool rtl9602c_uni_port_ok(struct rtl9602c_eth *ep, unsigned int port)
{
	if (ep && port < ep->swm->n_copper)
		return true;
	pr_err("rtl9602c-eth: [LAN-010] UNI port %u is not one of this board's %u copper ports -- REFUSED, nothing is written\n",
	       port, ep ? ep->swm->n_copper : 0);
	return false;
}

/* Drive one jack's link gate from the union of the OMCI lock and the
 * netdev's administrative state (rtl9602c_uni_port_locked); @why names the
 * caller in the log. The unlock of a GPHY port is a power-down pulse, so
 * the far end re-negotiates. */
static int rtl9602c_uni_gate_apply(struct rtl9602c_eth *ep, unsigned int port,
				   const char *why)
{
	u32 force, mode, guard;
	bool phy_allowed = true, locked;
	u16 bmcr, want;
	int rc;

	if (!rtl9602c_uni_port_ok(ep, port))
		return -EINVAL;

	mutex_lock(&rtl9602c_gphy_lock);
	locked = rtl9602c_uni_port_locked(port);

	/* A SerDes port has no GPHY in its path: the forced link IS the gate. */
	if (port == 0 && ep->port0_serdes) {
		force = SW_ABLTY_SERDES_UP;
		if (locked)
			force &= ~ABLTY_LINK;
		iowrite32(force, ep->sw + SW_FORCE_P_ABLTY(ep, port));
		iowrite32(0xfff, ep->sw + SW_ABLTY_FORCE_MODE(ep, port));
		force = ioread32(ep->sw + SW_FORCE_P_ABLTY(ep, port));
		mutex_unlock(&rtl9602c_gphy_lock);
		if (!(force & ABLTY_LINK) != locked) {
			dev_err(ep->dev,
				"[PHY-007] UNI port %u: the SerDes link gate did not take -- FORCE_P_ABLTY %08x\n",
				port, force);
			return -EIO;
		}
		dev_info(ep->dev, "UNI port %u %s by %s (SerDes, FORCE_P_ABLTY %08x)\n",
			 port, locked ? "LOCKED" : "UNLOCKED", why, force);
		return 0;
	}

	force = ioread32(ep->sw + SW_FORCE_P_ABLTY(ep, port));
	mode = ioread32(ep->sw + SW_ABLTY_FORCE_MODE(ep, port));
	if (locked) {
		iowrite32(force & ~ABLTY_LINK, ep->sw + SW_FORCE_P_ABLTY(ep, port));
		iowrite32(mode | ABLTY_LINK, ep->sw + SW_ABLTY_FORCE_MODE(ep, port));
	} else {
		iowrite32(mode & ~ABLTY_LINK, ep->sw + SW_ABLTY_FORCE_MODE(ep, port));
	}
	force = ioread32(ep->sw + SW_FORCE_P_ABLTY(ep, port));
	mode = ioread32(ep->sw + SW_ABLTY_FORCE_MODE(ep, port));
	/* BOTH words: the mode says the LINK ability is forced, the value says
	 * what it is forced TO. */
	if (!(mode & ABLTY_LINK) != !locked || (locked && (force & ABLTY_LINK))) {
		mutex_unlock(&rtl9602c_gphy_lock);
		dev_err(ep->dev,
			"[LAN-011] UNI port %u: the MAC gate did not take -- FORCE_P_ABLTY %08x, ABLTY_FORCE_MODE %08x, so the administrative state is NOT applied\n",
			port, force, mode);
		return -EIO;
	}

	if (port == 0) {
		guard = ioread32(ep->sw + SW_PHY_PWRDN_GUARD);
		phy_allowed = (guard & PHY_PWRDN_GUARD_OK) == PHY_PWRDN_GUARD_OK;
	}
	if (!phy_allowed) {
		/* This is a failure, not a partial success. */
		mutex_unlock(&rtl9602c_gphy_lock);
		dev_err(ep->dev,
			"[PHY-008] UNI port %u: the MAC gate took but this die guards port 0's PHY power on %#05x, which reads %08x -- the administrative state is NOT fully applied and stays owed\n",
			port, SW_PHY_PWRDN_GUARD, guard);
		return -EBUSY;
	}

	rc = rtl9602c_gphy_read__locked(ep, port, 0, &bmcr);
	if (rc) {
		mutex_unlock(&rtl9602c_gphy_lock);
		dev_err(ep->dev,
			"[PHY-009] UNI port %u: its BMCR could not be read (%d); nothing is written back and the state is owed\n",
			port, rc);
		return rc;
	}
	/* Only the power bit moves. */
	want = (u16)(bmcr | BMCR_PDOWN);
	rtl9602c_lan_own_drop(ep, port);
	rc = rtl9602c_gphy_write__locked(ep, port, 0, want);
	if (!rc && !locked) {
		u16 mid;

		rc = rtl9602c_gphy_read__locked(ep, port, 0, &mid);
		if (!rc && !(mid & BMCR_PDOWN)) {
			dev_err(ep->dev,
				"[PHY-010] UNI port %u: the power-down half of the unlock pulse did not land (BMCR %04x) -- the far end would never re-negotiate\n",
				port, mid);
			rc = -EIO;
		}
		if (!rc)
			rc = rtl9602c_gphy_write__locked(ep, port, 0,
							 (u16)(mid & ~BMCR_PDOWN));
	}
	if (!rc)
		rc = rtl9602c_gphy_read__locked(ep, port, 0, &bmcr);
	if (!rc && (!(bmcr & BMCR_PDOWN) != !locked))
		rc = -EIO;
	mutex_unlock(&rtl9602c_gphy_lock);
	if (rc) {
		dev_err(ep->dev,
			"[PHY-011] UNI port %u: the PHY did not reach %s (%d, BMCR %04x); the administrative state is owed\n",
			port, locked ? "power-down" : "power-up", rc, bmcr);
		return rc;
	}
	dev_info(ep->dev, "UNI port %u %s by %s (BMCR %04x, ABLTY_FORCE_MODE %08x)\n",
		 port, locked ? "LOCKED" : "UNLOCKED", why, bmcr, mode);
	return 0;
}

/* OMCI's Administrative State for a UNI (the OLT's, through omcid or the
 * in-kernel responder): a DESIRED state, applied with the netdev's. */
static int rtl9602c_uni_admin_set(void *cookie, unsigned int port, bool locked)
{
	struct rtl9602c_eth *ep = cookie;

	if (!rtl9602c_uni_port_ok(ep, port))
		return -EINVAL;
	mutex_lock(&rtl9602c_gphy_lock);
	WRITE_ONCE(rtl9602c_uni_locked_ports,
		   locked ? (rtl9602c_uni_locked_ports | BIT(port))
			  : (rtl9602c_uni_locked_ports & ~BIT(port)));
	mutex_unlock(&rtl9602c_gphy_lock);
	return rtl9602c_uni_gate_apply(ep, port, "OMCI");
}

/* ip link set lanN down / up. */
static int rtl9602c_lan_gate(struct rtl9602c_eth *ep, unsigned int port, bool down)
{
	if (!rtl9602c_uni_port_ok(ep, port))
		return -EINVAL;
	mutex_lock(&rtl9602c_gphy_lock);
	WRITE_ONCE(rtl9602c_uni_down_ports,
		   down ? (rtl9602c_uni_down_ports | BIT(port))
			: (rtl9602c_uni_down_ports & ~BIT(port)));
	mutex_unlock(&rtl9602c_gphy_lock);
	return rtl9602c_uni_gate_apply(ep, port, "netdev");
}

/* ---- the external PHY's bit-banged MDIO ----------------------------------
 * The V2802RH's RTL8221B hangs off GPIO 5 (MDC) and GPIO 10 (MDIO) at address
 * 7, read from the stock RTOS's own bit-bang routines and its PHY-address
 * global. The frame timing follows drivers/net/mdio/mdio-bitbang.c. */
#define EXT_MDIO_C22_START	1
#define EXT_MDIO_C45_START	0
#define EXT_MDIO_OP_ADDR	0
#define EXT_MDIO_OP_WRITE	1
#define EXT_MDIO_OP_READ_C22	2
#define EXT_MDIO_OP_READ_C45	3

static void ext_mdio_send_bit(struct rtl9602c_eth *ep, int val)
{
	gpiod_set_value(ep->ext_mdio, val);
	ndelay(400);
	gpiod_set_value(ep->ext_mdc, 1);
	ndelay(400);
	gpiod_set_value(ep->ext_mdc, 0);
}

static int ext_mdio_get_bit(struct rtl9602c_eth *ep)
{
	ndelay(400);
	gpiod_set_value(ep->ext_mdc, 1);
	ndelay(400);
	gpiod_set_value(ep->ext_mdc, 0);
	return gpiod_get_value(ep->ext_mdio);
}

static void ext_mdio_send_num(struct rtl9602c_eth *ep, u16 val, int bits)
{
	while (bits--)
		ext_mdio_send_bit(ep, (val >> bits) & 1);
}

static u16 ext_mdio_get_num(struct rtl9602c_eth *ep, int bits)
{
	u16 val = 0;

	while (bits--)
		val = (val << 1) | ext_mdio_get_bit(ep);
	return val;
}

static void ext_mdio_cmd(struct rtl9602c_eth *ep, int start, int op, u8 reg)
{
	int i;

	gpiod_direction_output(ep->ext_mdio, 1);
	for (i = 0; i < 32; i++)
		ext_mdio_send_bit(ep, 1);
	ext_mdio_send_num(ep, start, 2);
	ext_mdio_send_num(ep, op, 2);
	ext_mdio_send_num(ep, ep->ext_phy, 5);
	ext_mdio_send_num(ep, reg, 5);
}

static void ext_mdio_turnaround_write(struct rtl9602c_eth *ep, u16 val)
{
	ext_mdio_send_bit(ep, 1);
	ext_mdio_send_bit(ep, 0);
	ext_mdio_send_num(ep, val, 16);
	gpiod_direction_input(ep->ext_mdio);
	ext_mdio_get_bit(ep);
}

static int ext_mdio_turnaround_read(struct rtl9602c_eth *ep)
{
	int val;

	gpiod_direction_input(ep->ext_mdio);
	if (ext_mdio_get_bit(ep)) {
		ext_mdio_get_num(ep, 16);	/* nothing answered: flush the frame */
		ext_mdio_get_bit(ep);
		return -EIO;
	}
	val = ext_mdio_get_num(ep, 16);
	ext_mdio_get_bit(ep);
	return val;
}

static int ext_mdio_read(struct rtl9602c_eth *ep, u8 reg)
{
	ext_mdio_cmd(ep, EXT_MDIO_C22_START, EXT_MDIO_OP_READ_C22, reg);
	return ext_mdio_turnaround_read(ep);
}

static void ext_mdio_c45_addr(struct rtl9602c_eth *ep, u8 dev, u16 reg)
{
	ext_mdio_cmd(ep, EXT_MDIO_C45_START, EXT_MDIO_OP_ADDR, dev);
	ext_mdio_turnaround_write(ep, reg);
}

static int ext_mdio_read_c45(struct rtl9602c_eth *ep, u8 dev, u16 reg)
{
	ext_mdio_c45_addr(ep, dev, reg);
	ext_mdio_cmd(ep, EXT_MDIO_C45_START, EXT_MDIO_OP_READ_C45, dev);
	return ext_mdio_turnaround_read(ep);
}

static void ext_mdio_write_c45(struct rtl9602c_eth *ep, u8 dev, u16 reg, u16 val)
{
	ext_mdio_c45_addr(ep, dev, reg);
	ext_mdio_cmd(ep, EXT_MDIO_C45_START, EXT_MDIO_OP_WRITE, dev);
	ext_mdio_turnaround_write(ep, val);
}

static void ext_mdio_write(struct rtl9602c_eth *ep, u8 reg, u16 val)
{
	ext_mdio_cmd(ep, EXT_MDIO_C22_START, EXT_MDIO_OP_WRITE, reg);
	ext_mdio_turnaround_write(ep, val);
}

/* A jack's PHY by its standard registers: the GPHY's, or the external one's. */
static int rtl9602c_lan_phy_read__locked(struct rtl9602c_eth *ep, unsigned int port, u8 reg)
{
	u16 v;
	int rc;

	if (port == 0 && ep->port0_serdes)
		return ep->ext_phy_ok ? ext_mdio_read(ep, reg) : -ENODEV;
	rc = rtl9602c_gphy_read__locked(ep, port, reg, &v);
	return rc ?: v;
}

static int rtl9602c_lan_phy_write__locked(struct rtl9602c_eth *ep, unsigned int port, u8 reg, u16 val)
{
	if (port == 0 && ep->port0_serdes) {
		if (!ep->ext_phy_ok)
			return -ENODEV;
		ext_mdio_write(ep, reg, val);
		return 0;
	}
	return rtl9602c_gphy_write__locked(ep, port, reg, val);
}

/* The RTL8221B's LED setup, exactly as stock leaves it (read back through
 * stock's own "debug ext-mdio c45"): LED0 link at any speed with activity
 * blink, LED1 1G link, LED2 2.5G link, all three enabled active-high.
 * Without it the PHY's power-on default leaves the LAN2 lamp dark. */
static const struct { u16 reg, val; } rtl8221b_stock_leds[] = {
	{ 0xd032, 0x00a7 },	/* LED0 link 10/100/1000/2500 */
	{ 0xd034, 0x0004 },	/* LED1 link 1000 */
	{ 0xd036, 0x0020 },	/* LED2 link 2500 */
	{ 0xd040, 0x0901 },	/* LCR6: activity on LED0 */
	{ 0xd044, 0x00f8 },	/* LCR7: LED0-2 enabled, active-high */
};

#define MDIO_MMD_VEND2_RTL	31

static void rtl9602c_ext_phy_leds(struct rtl9602c_eth *ep)
{
	unsigned int i;
	int v;

	for (i = 0; i < ARRAY_SIZE(rtl8221b_stock_leds); i++)
		ext_mdio_write_c45(ep, MDIO_MMD_VEND2_RTL, rtl8221b_stock_leds[i].reg,
				   rtl8221b_stock_leds[i].val);
	v = ext_mdio_read_c45(ep, MDIO_MMD_VEND2_RTL, 0xd044);
	if (v < 0)
		dev_warn_ratelimited(ep->dev, "[PHY-012] RTL8221B LED setup read failed: %d\n", v);
	else if (v != 0x00f8)
		dev_warn(ep->dev, "[PHY-013] RTL8221B LED setup read back %04x, not 00f8\n", v);
}

/* Stock's reset: the line low for 30 ms, then 30 ms for the PHY to come up. */
static void rtl9602c_ext_phy_reset(struct rtl9602c_eth *ep)
{
	if (!ep->ext_phy_reset)
		return;
	gpiod_set_value_cansleep(ep->ext_phy_reset, 1);
	msleep(30);
	gpiod_set_value_cansleep(ep->ext_phy_reset, 0);
	msleep(30);
}

static bool rtl9602c_ext_phy_ready(struct rtl9602c_eth *ep, unsigned int option)
{
	int v;

	v = ext_mdio_read_c45(ep, MDIO_MMD_VEND1_RTL, RTL8221B_VND1_SERDES_OPTION);
	if (v < 0 || v == 0xffff)
		return false;
	if (option == RTL8221B_SDS_OPTION_SGMII) {
		v = ext_mdio_read_c45(ep, MDIO_MMD_VEND1_RTL, RTL8221B_VND1_SERDES_CTRL);
		if (v < 0 || v == 0xffff)
			return false;
	}
	return true;
}

static int rtl9602c_ext_phy_serdes_option(struct rtl9602c_eth *ep, unsigned int option)
{
	int v;

	if (option == RTL8221B_SDS_OPTION_SGMII) {
		v = ext_mdio_read_c45(ep, MDIO_MMD_VEND1_RTL, RTL8221B_VND1_SERDES_CTRL);
		if (v < 0)
			return v;
		ext_mdio_write_c45(ep, MDIO_MMD_VEND1_RTL, RTL8221B_VND1_SERDES_CTRL,
				   v & ~BIT(0));
		v = ext_mdio_read_c45(ep, MDIO_MMD_VEND1_RTL, RTL8221B_VND1_SERDES_CTRL);
		if (v < 0 || (v & BIT(0)))
			return v < 0 ? v : -EIO;
	}
	v = ext_mdio_read_c45(ep, MDIO_MMD_VEND1_RTL, RTL8221B_VND1_SERDES_OPTION);
	if (v < 0)
		return v;
	ext_mdio_write_c45(ep, MDIO_MMD_VEND1_RTL, RTL8221B_VND1_SERDES_OPTION,
			   (v & ~GENMASK(6, 0)) | option);
	v = ext_mdio_read_c45(ep, MDIO_MMD_VEND1_RTL, RTL8221B_VND1_SERDES_OPTION);
	if (v < 0 || (v & GENMASK(6, 0)) != option)
		return v < 0 ? v : -EIO;
	ep->ext_phy_option = option;
	return 0;
}

/* Reset once per target change. The PHY can answer MDIO before its vendor
 * registers are ready; resetting on every retry restarts that initialization. */
static int rtl9602c_ext_phy_setup(struct rtl9602c_eth *ep, unsigned int option)
{
	bool ready;
	int rc;

	if (option) {
		if (ep->ext_phy_target != option) {
			ep->ext_phy_target = option;
			ep->ext_phy_option = 0;
			rtl9602c_ext_phy_reset(ep);
		}
		/* Vendor registers can lag behind the Clause 22 PHY ID after reset. */
		rc = read_poll_timeout(rtl9602c_ext_phy_ready, ready, ready,
				       10000, 1000000, false, ep, option);
		if (rc)
			return rc;
		rc = rtl9602c_ext_phy_serdes_option(ep, option);
		if (rc)
			return rc;
	}
	rtl9602c_ext_phy_leds(ep);
	return 0;
}

static int rtl9602c_ext_phy_init(struct rtl9602c_eth *ep)
{
	struct device *dev = ep->dev;
	int id1, id2;
	u32 addr;

	ep->ext_mdc = devm_gpiod_get_index_optional(dev, "realtek,ext-mdio", 0,
						    GPIOD_OUT_LOW);
	if (IS_ERR(ep->ext_mdc))
		return PTR_ERR(ep->ext_mdc);
	if (!ep->ext_mdc)
		return 0;
	ep->ext_mdio = devm_gpiod_get_index(dev, "realtek,ext-mdio", 1,
					    GPIOD_OUT_HIGH);
	if (IS_ERR(ep->ext_mdio))
		return PTR_ERR(ep->ext_mdio);
	/* Released (not in reset) from the request on: GPIOD_OUT_LOW is the
	 * logical level, the line is active-low in the DT. */
	ep->ext_phy_reset = devm_gpiod_get_optional(dev, "realtek,ext-phy-reset",
						    GPIOD_OUT_LOW);
	if (IS_ERR(ep->ext_phy_reset))
		return PTR_ERR(ep->ext_phy_reset);
	if (of_property_read_u32(dev->of_node, "realtek,ext-phy-addr", &addr) ||
	    addr > 31) {
		dev_err(dev, "[OPT-015] realtek,ext-mdio-gpios without a valid realtek,ext-phy-addr\n");
		return -EINVAL;
	}
	ep->ext_phy = addr;

	id1 = ext_mdio_read(ep, MII_PHYSID1);
	id2 = ext_mdio_read(ep, MII_PHYSID2);
	if (id1 < 0 || id2 < 0) {
		dev_err(dev, "[PHY-014] external PHY at MDIO address %u did not answer\n", addr);
		return 0;
	}
	dev_info(dev, "external PHY %04x:%04x at MDIO address %u\n", id1, id2, addr);
	ep->ext_phy_ok = true;
	if (id1 != 0x001c || (id2 & 0xfff0) != 0xc840)
		return 0;

	/* The SoC side starts in SGMII (rtl9602c_uboot_swcore_bringup). */
	if (rtl9602c_ext_phy_setup(ep, RTL8221B_SDS_OPTION_SGMII))
		dev_warn(dev, "[PHY-015] RTL8221B SerDes setup pending; link polling will retry\n");
	return 0;
}

/* Link changes in phylink's words, per LAN jack: LAN1 is the GPHY port,
 * LAN2 the SerDes one. Pause is the ability word's TX (b5) / RX (b6). */
static const char *rtl9602c_speed_str(u32 speed)
{
	switch (speed) {
	case SPEED_10: return "10Mbps";
	case SPEED_100: return "100Mbps";
	case SPEED_1000: return "1Gbps";
	case SPEED_2500: return "2.5Gbps";
	default: return "Unknown";
	}
}

static void rtl9602c_link_log(struct rtl9602c_eth *ep, unsigned int lan, bool up,
			      u32 speed, bool full, u32 ablty)
{
	static const char * const pause[] = { "off", "tx", "rx", "rx/tx" };

	if (!up) {
		dev_info(ep->dev, "lan%u: [LAN-012] Link is Down\n", lan);
		return;
	}
	dev_info(ep->dev, "lan%u: Link is Up - %s/%s - flow control %s\n", lan,
		 rtl9602c_speed_str(speed), full ? "Full" : "Half",
		 pause[(ablty >> 5) & 0x3]);
}

/* What the jack's netdev shows: carrier now, speed and duplex to ethtool. */
static void rtl9602c_lan_link_set(struct rtl9602c_eth *ep, unsigned int port,
				  bool up, u32 speed, bool full)
{
	struct net_device *ndev;

	if (port >= LAN_PORTS)
		return;
	ep->lan_link[port].seen = true;
	ep->lan_link[port].up = up;
	ep->lan_link[port].speed = speed;
	ep->lan_link[port].full = full;
	ndev = ep->lan_ndev[port];
	if (!ndev)
		return;
	if (up)
		netif_carrier_on(ndev);
	else
		netif_carrier_off(ndev);
}

static void rtl9602c_lan_carrier_sync(struct rtl9602c_eth *ep, unsigned int port)
{
	if (port < LAN_PORTS)
		rtl9602c_lan_link_set(ep, port, ep->lan_link[port].up,
				      ep->lan_link[port].speed,
				      ep->lan_link[port].full);
}

/* RTL8221B PHYSR (VEND2 0xa434): duplex bit 3, speed bits 10:9 and 5:4. */
static u32 rtl9602c_port0_speed(struct rtl9602c_eth *ep, bool *full)
{
	int sr = ext_mdio_read_c45(ep, MDIO_MMD_VEND2_RTL, 0xa434);

	*full = sr >= 0 && (sr & BIT(3));
	switch (sr < 0 ? -1 : sr & 0x630) {
	case 0x000: return SPEED_10;
	case 0x010: return SPEED_100;
	case 0x020: return SPEED_1000;
	case 0x210: return SPEED_2500;
	default: return SPEED_UNKNOWN;
	}
}

/* A GPHY port links on its own; its live ability word says how. The log
 * names the jack as the panel does: port 1 is LAN1, port 0 LAN2. */
static void rtl9602c_gphy_link_poll(struct rtl9602c_eth *ep, unsigned int port)
{
	static const u32 speed[] = { SPEED_10, SPEED_100, SPEED_1000, SPEED_UNKNOWN };
	u32 sts = ioread32(ep->sw + ep->swm->p_ablty + 4 * port) &
		  (ABLTY_LINK | BIT(2) | BIT(5) | BIT(6) | 0x3);
	bool up = sts & ABLTY_LINK;
	u16 bmsr = BMSR_LSTATUS;
	bool own;

	/* BMSR's link bit latches low until read: low with the link up on both
	 * polls means it dropped and came back in between. */
	mutex_lock(&rtl9602c_gphy_lock);
	if (rtl9602c_gphy_read__locked(ep, port, MII_BMSR, &bmsr))
		bmsr = BMSR_LSTATUS;
	own = ep->lan_own_drop & BIT(port);
	if (ep->lan_link[port].seen && ep->lan_link[port].up &&
	    (!up || !(bmsr & BMSR_LSTATUS))) {
		ep->lan_own_drop &= ~BIT(port);
		if (!own) {
			ep->lan_drops[port]++;
			if (up)
				dev_info(ep->dev, "lan%u: [LAN-013] link dropped and recovered within a second (%u drops)\n",
					 port == 0 ? 2 : 1, ep->lan_drops[port]);
		}
	}
	mutex_unlock(&rtl9602c_gphy_lock);

	if (sts == ep->gphy_sts[port] && ep->lan_link[port].seen)
		return;
	ep->gphy_sts[port] = sts;
	/* The first look at a port with no link is not news. */
	if (sts || ep->lan_link[port].seen)
		rtl9602c_link_log(ep, port == 0 ? 2 : 1, up, speed[sts & 0x3],
				  sts & BIT(2), sts);
	rtl9602c_lan_link_set(ep, port, up, speed[sts & 0x3], sts & BIT(2));
}

/* Mirror the external PHY's link into port 0's forced link, so a LAN2 jack
 * with no cable is a down port to the switch, as a copper port would be. An
 * administrative lock keeps it down whatever the cable does. BMSR's link bit
 * latches low, so the second read is the live state. The bit-banged bus has
 * one user at a time: the gphy lock, which the gate holds too. */
static void rtl9602c_port0_link_poll(struct rtl9602c_eth *ep)
{
	u32 cur, want = SW_ABLTY_SERDES_UP & ~ep->port0_pause_off, speed = SPEED_UNKNOWN;
	int bmsr, latched;
	bool up, link, full = false;

	mutex_lock(&rtl9602c_gphy_lock);
	latched = ext_mdio_read(ep, MII_BMSR);
	bmsr = ext_mdio_read(ep, MII_BMSR);
	up = bmsr >= 0 && (bmsr & BMSR_LSTATUS);
	if (ep->lan_link[0].seen && ep->lan_link[0].up) {
		if (!up) {
			ep->lan_drops[0]++;
		} else if (latched >= 0 && !(latched & BMSR_LSTATUS)) {
			ep->lan_drops[0]++;
			dev_info(ep->dev, "lan2: [LAN-014] link dropped and recovered within a second (%u drops)\n",
				 ep->lan_drops[0]);
		}
	}
	if (up)
		speed = rtl9602c_port0_speed(ep, &full);
	if (!ep->lan_sds_set) {
		up = false;
	} else {
		bool hsgmii = ep->ext_phy_target == RTL8221B_SDS_OPTION_HSGMII;
		unsigned int option;

		/* A failed speed read is not evidence of a 1G link. */
		if (speed != SPEED_UNKNOWN)
			hsgmii = speed == SPEED_2500;
		option = hsgmii ? RTL8221B_SDS_OPTION_HSGMII : RTL8221B_SDS_OPTION_SGMII;
		if (hsgmii != ep->lan_sds_hsgmii || ep->ext_phy_option != option) {
			int rc;

			cur = ioread32(ep->sw + SW_FORCE_P_ABLTY(ep, 0));
			iowrite32(cur & ~ABLTY_LINK, ep->sw + SW_FORCE_P_ABLTY(ep, 0));
			if (ep->lan_link[0].up)
				rtl9602c_link_log(ep, 2, false, SPEED_UNKNOWN, false, cur);
			rtl9602c_lan_link_set(ep, 0, false, SPEED_UNKNOWN, false);
			rc = rtl9602c_ext_phy_setup(ep, option);
			if (rc) {
				dev_warn_ratelimited(ep->dev, "[PHY-016] LAN2 PHY SerDes option %u failed: %d; retrying\n",
						     option, rc);
			} else {
				rtl9602c_lan_sds_program(ep, hsgmii);
				dev_info(ep->dev, "LAN2 SerDes configured for %s\n",
					 hsgmii ? "HSGMII" : "SGMII");
			}
			/* Reset invalidated this sample; report up only after a fresh poll. */
			up = false;
		}
	}
	if (speed == SPEED_UNKNOWN && !ep->lan_link[0].up)
		up = false;
	link = up && !rtl9602c_uni_port_locked(0);
	if (!link)
		want &= ~ABLTY_LINK;
	else if (speed == SPEED_UNKNOWN && ep->lan_link[0].up) {
		speed = ep->lan_link[0].speed;
		full = ep->lan_link[0].full;
	}
	cur = ioread32(ep->sw + SW_FORCE_P_ABLTY(ep, 0));
	if (cur != want)
		iowrite32(want, ep->sw + SW_FORCE_P_ABLTY(ep, 0));
	if (link != ep->lan_link[0].up || !ep->lan_link[0].seen ||
	    (link && (speed != ep->lan_link[0].speed || full != ep->lan_link[0].full))) {
		if (link || ep->lan_link[0].seen)
			rtl9602c_link_log(ep, 2, link, speed, full, want);
		rtl9602c_lan_link_set(ep, 0, link, link ? speed : SPEED_UNKNOWN, full);
	}
	mutex_unlock(&rtl9602c_gphy_lock);
}

/* RLDP, the switch's own loop detection: it sends a probe out of each enabled
 * port every 2 s and flags a port whose probes come back. Stock ships it off. */
#define SW_RLDP_EN		0x1a000
#define SW_RLDP_PORT_TX_EN	0x1a01c
#define SW_RLDP_LP_ENTER	0x1a020	/* write 1 to clear */
#define SW_RLDP_LP_LEAVE	0x1a024	/* write 1 to clear */
#define SW_RLDP_LP_STS		0x1a028
#define SW_RLDP_LP_PNUM		0x1a02c	/* 3 bits per port: the port it came back on */
#define RLDP_CUT_SECONDS	30

enum { LOOP_GUARD_OFF, LOOP_GUARD_LOG, LOOP_GUARD_CUT };
static int lan_loop_guard = LOOP_GUARD_CUT;
module_param(lan_loop_guard, int, 0644);
MODULE_PARM_DESC(lan_loop_guard, "LAN loop detection: 0=off, 1=log, 2=also take the looped jack down for 30 s at a time");

static void rtl9602c_loop_cut(struct rtl9602c_eth *ep, int port)
{
	if (ep->loop_cut_port >= 0) {
		dev_info(ep->dev, "[LAN-017] %s: back up after the loop cut\n",
			 ep->lan_ndev[ep->loop_cut_port]->name);
		rtl9602c_lan_gate(ep, ep->loop_cut_port, false);
	}
	ep->loop_cut_port = port;
	if (port >= 0) {
		dev_warn(ep->dev, "[LAN-016] %s: down for %u s to break the loop\n",
			 ep->lan_ndev[port]->name, RLDP_CUT_SECONDS);
		rtl9602c_lan_gate(ep, port, true);
		ep->loop_cut_left = RLDP_CUT_SECONDS;
	}
}

static void rtl9602c_loop_poll(struct rtl9602c_eth *ep)
{
	int mode = READ_ONCE(lan_loop_guard);
	u32 ports = 0, sts, changed;
	unsigned int p;

	if (!ep->stock_switch)
		return;
	for (p = 0; p < LAN_PORTS; p++)
		if (ep->lan_ndev[p])
			ports |= BIT(p);
	if (mode != ep->loop_mode) {
		iowrite32(mode ? ports : 0, ep->sw + SW_RLDP_PORT_TX_EN);
		iowrite32(mode ? 1 : 0, ep->sw + SW_RLDP_EN);
		ep->loop_mode = mode;
	}
	if (mode != LOOP_GUARD_CUT && ep->loop_cut_port >= 0)
		rtl9602c_loop_cut(ep, -1);
	if (!mode)
		return;
	sts = ioread32(ep->sw + SW_RLDP_LP_STS) & ports;
	iowrite32(ports, ep->sw + SW_RLDP_LP_ENTER);
	iowrite32(ports, ep->sw + SW_RLDP_LP_LEAVE);
	changed = sts ^ ep->loop_ports;
	ep->loop_ports = sts;
	for (p = 0; p < LAN_PORTS; p++) {
		if (!(changed & BIT(p)))
			continue;
		if (sts & BIT(p))
			dev_warn(ep->dev, "[LAN-015] %s: loop detected, probes return on switch port %u\n",
				 ep->lan_ndev[p]->name,
				 (ioread32(ep->sw + SW_RLDP_LP_PNUM) >> (3 * p)) & 7);
		else
			dev_info(ep->dev, "%s: loop cleared\n", ep->lan_ndev[p]->name);
	}
	if (ep->loop_cut_port >= 0) {
		if (!--ep->loop_cut_left)
			rtl9602c_loop_cut(ep, -1);
	} else if (mode == LOOP_GUARD_CUT && sts) {
		/* Both ends of a cable report it, and one cut breaks the loop. */
		rtl9602c_loop_cut(ep, fls(sts) - 1);
	}
}

static void rtl9602c_link_work(struct work_struct *work)
{
	struct rtl9602c_eth *ep = container_of(to_delayed_work(work),
					       struct rtl9602c_eth, link_work);
	unsigned int p;

	for (p = 0; p < LAN_PORTS && p < ep->swm->n_copper; p++) {
		if (p == 0 && ep->port0_serdes) {
			if (ep->ext_phy_ok)
				rtl9602c_port0_link_poll(ep);
		} else {
			rtl9602c_gphy_link_poll(ep, p);
		}
	}
	rtl9602c_loop_poll(ep);
	schedule_delayed_work(&ep->link_work, HZ);
}

static bool rtl9602c_uni_link(void *cookie, unsigned int port)
{
	struct rtl9602c_eth *ep = cookie;

	return ioread32(ep->sw + ep->swm->p_ablty + 4 * port) & ABLTY_LINK;
}

static void rtl9602c_eth_omci_release(void *cookie)
{
	WRITE_ONCE(g_ep, NULL);
	luna_xpon_detach(cookie);
	luna_omci_detach(cookie);
}

/* One of the board's lamps by its led-names entry; none is not an error. */
static int rtl9602c_eth_get_led(struct device *dev, char *name,
				struct led_classdev **out)
{
	struct led_classdev *led = devm_led_get(dev, name);

	*out = NULL;
	if (!IS_ERR(led)) {
		*out = led;
		return 0;
	}
	if (PTR_ERR(led) == -ENOENT)
		return 0;
	return dev_err_probe(dev, PTR_ERR(led), "the %s LED\n", name);
}

static int rtl9602c_eth_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct net_device *ndev;
	struct rtl9602c_eth *ep;
	u8 mac[ETH_ALEN];
	int ret;

	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	ndev = devm_alloc_etherdev(dev, sizeof(*ep));
	if (!ndev)
		return -ENOMEM;
	SET_NETDEV_DEV(ndev, dev);
	platform_set_drvdata(pdev, ndev);

	ep = netdev_priv(ndev);
	ep->ndev = ndev;
	ep->dev = dev;
	/* CPU-tag steering bypasses stale shared-LUT destinations and disables
	 * source learning on CPU copies of client frames. */
	ep->lan_tx_default = of_machine_is_compatible("vsol,v2802rh") ?
		LAN_STEER_CPUTAG : LAN_STEER_VLAN;
	spin_lock_init(&ep->tx_lock);
	spin_lock_init(&ep->sw_table_lock);
	INIT_WORK(&ep->recover_work, rtl9602c_eth_recover_work);

	/* The board's PON/LOS lamps (this node's leds / led-names) are
	 * net/xpon's to drive. Not registered yet = probe again later, before
	 * the GMAC is touched. */
	ret = rtl9602c_eth_get_led(dev, "pon", &ep->pon_led);
	if (!ret)
		ret = rtl9602c_eth_get_led(dev, "los", &ep->los_led);
	if (ret)
		return ret;

	ep->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ep->base))
		return PTR_ERR(ep->base);

	/* A board may run a shorter RX ring than the family's stock 1024: the
	 * 32 MB X100DG's own stock runs 256, and 1024 buffers do not fit it. */
	ep->rx_n = RX_RING_SIZE;
	if (!of_property_read_u32(dev->of_node, "realtek,rx-ring-entries", &ep->rx_n) &&
	    (ep->rx_n < 64 || ep->rx_n > RX_RING_SIZE)) {
		dev_err(dev, "[SYS-025] realtek,rx-ring-entries %u is outside 64..%u\n", ep->rx_n,
			RX_RING_SIZE);
		return -EINVAL;
	}
	/* The board's own WAN MAC offset from its stock (X100DG: LAN+1). */
	of_property_read_u32(dev->of_node, "realtek,wan-mac-offset", &wan_mac_offset);
	ep->port0_serdes = of_property_read_bool(dev->of_node, "realtek,port0-serdes");
	ep->loop_cut_port = -1;
	ep->stock_switch = of_property_read_bool(dev->of_node, "realtek,stock-switch-config");
	ret = rtl9602c_ext_phy_init(ep);
	if (ret)
		return ret;

	/* Switch core (best-effort; minimal L2 flood enabled at open). */
	ep->swm = &rtl9602c_sw_map;
	ep->sw = devm_ioremap(dev, SWCORE_PHYS, ep->swm->swcore_size);
	/* network-engine TX-fetch GO register page */
	ep->txgo = devm_ioremap(dev, 0x18001000, 0x1000);

	/* Snapshot the bootloader's live GMAC0 control config to re-assert at open. */
	ep->ub_tcr = ep_rd(ep, R_TCR);
	ep->ub_rcr = ep_rd(ep, R_RCR);
	ep->ub_config = ep_rd(ep, R_CONFIG);
	ep->ub_cputagcr = ep_rd(ep, R_CPUTAGCR);
	ep->ub_cputag1cr = ep_rd(ep, R_CPUTAG1CR);
	ep->ub_iocmd = ep_rd(ep, R_IO_CMD);
	ep->ub_iocmd1 = ep_rd(ep, R_IO_CMD1);

	/* The bring-up default is refused, not merely validated. */
	{
		u8 dt[GPON_HWADDR_BYTES], eng[GPON_HWADDR_BYTES];
		bool have_dt = of_get_mac_address(dev->of_node, dt) == 0;
		enum gpon_hwaddr_src src;
		bool eng_ok;

		rtl9602c_eth_get_hwaddr(ep, eng);
		eng_ok = !luna_mac_is_bringup_default(eng);
		src = gpon_hwaddr_resolve(mac_param, have_dt ? dt : NULL,
					  eng_ok ? eng : NULL, mac);
		eth_hw_addr_set(ndev, mac);

		/* gpon_provision sets this unit's MAC from its config partition. */
		dev_dbg(dev, "MAC %pM from %s\n", ndev->dev_addr,
			gpon_hwaddr_src_name(src));
	}


	ndev->netdev_ops = &rtl9602c_eth_netdev_ops;
	ret = luna_mib_attach(ndev, ep->sw, ep->swm);
	if (ret)
		return dev_err_probe(dev, ret, "switch statistics attach failed\n");
	/* Permit a LIVE MAC change (no iface down/up) */
	ndev->priv_flags |= IFF_LIVE_ADDR_CHANGE;
	netif_carrier_off(ndev);

	/* INTC input 26 (GMAC0). <=0 -> no DT mapping: open() runs pure-poll. */
	ep->irq = platform_get_irq(pdev, 0);
	if (ep->irq < 0)
		ep->irq = -1;
	netif_napi_add(ndev, &ep->napi, rtl9602c_eth_napi_poll);

	if (!luna_xpon_wanted()) {
		ret = luna_omci_attach(&luna_onu, ep, (u8)omci_mds_seed,
				       rtl9602c_eth_omci_send, rtl9602c_eth_tx_fence,
				       rtl9602c_uni_admin_set);
		if (ret)
			return ret;
	}
	ret = devm_add_action_or_reset(dev, rtl9602c_eth_omci_release, ep);
	if (ret)
		return ret;
	WRITE_ONCE(g_ep, ep);
	ret = devm_register_netdev(dev, ndev);
	if (ret)
		return ret;
	ret = rtl9602c_lan_register(ep);
	if (ret)
		return ret;

	dev_dbg(dev, "inherited IO_CMD %08x\n", ep->ub_iocmd);

	/* gpon0: the WAN data-GEM netdev (clean-room nas0-equivalent). Shares ep's RX/TX
	 * rings + NAPI (owned by eth0); RX is demux'd by ingress port (PON port 2 -> gpon0)
	 * and US frames steer to GPON_DATA_FLOW. Its MAC is the WAN identity = board MAC +
	 * model offset (stock nas0_0 = base+3); see rtl9602c_wan_mac. */
	{
		struct net_device *wan = devm_alloc_etherdev(dev, sizeof(struct rtl9602c_eth *));
		u8 wmac[ETH_ALEN];

		if (wan) {
			*(struct rtl9602c_eth **)netdev_priv(wan) = ep;
			SET_NETDEV_DEV(wan, dev);
			/* omci-agent and LuCI find omcid's PON netdev as pon0. */
			strscpy(wan->name, luna_xpon_wanted() ? "pon0" : "gpon0", IFNAMSIZ);
			wan->netdev_ops = &rtl9602c_eth_wan_ops;
			/* Keep the LAN and WAN feature declarations consistent. */
			wan->hw_features |= NETIF_F_HW_TC;
			wan->features |= NETIF_F_HW_TC | NETIF_F_HW_VLAN_CTAG_FILTER;
			/* Initial WAN MAC = board MAC + offset; re-derived at open + on eth0 MAC
			 * changes once rtk_factory provisions the real board MAC onto eth0. */
			rtl9602c_wan_mac(wmac, ndev->dev_addr);
			eth_hw_addr_set(wan, wmac);
			luna_omci_set_wan_mac(wmac);
			netif_carrier_off(wan);
			if (devm_register_netdev(dev, wan) == 0)
				ep->wan_ndev = wan;
			else
				dev_warn(dev, "[SYS-026] gpon0 (WAN) register failed; WAN datapath disabled\n");
		}
	}

	if (ep->wan_ndev && luna_xpon_wanted()) {
		ret = luna_xpon_attach(dev, ep->wan_ndev, ep, rtl9602c_eth_omci_send,
				       rtl9602c_uni_admin_set, rtl9602c_uni_link,
				       rtl9602c_uni_rate_set,
				       rtl9602c_uni_l2_set,
				       rtl9602c_uni_autodetect_set,
				       rtl9602c_uni_pause_set,
				       ep->pon_led, ep->los_led);
		if (ret)
			dev_err(dev, "[OMC-016] omcid backend not registered: %d\n", ret);
	}

	ret = v2802_l2_init(ep);
	if (ret)
		dev_warn(dev, "[L2-007] V2802RH L2 offload unavailable: %d\n", ret);

	/* The SerDes mirror needs the poll; so does any jack with a netdev. */
	if ((ep->port0_serdes && ep->ext_phy_ok) || ep->lan_ndev[0] || ep->lan_ndev[1]) {
		ret = devm_delayed_work_autocancel(dev, &ep->link_work,
						   rtl9602c_link_work);
		if (ret)
			return ret;
		schedule_delayed_work(&ep->link_work, HZ);
	}

	ep->omci_mds = (u8)omci_mds_seed;	/* poison seed: fail the OLT's ME2 audit so a warm
						 * re-admit re-provisions (we hold no persistent MIB) */
	proc_create("ethdump", 0644, NULL, &rtl9602c_ethdump_pops);
	return 0;
}

static const struct of_device_id rtl9602c_eth_of_match[] = {
	{ .compatible = "realtek,rtl9602c-nic" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtl9602c_eth_of_match);

static struct platform_driver rtl9602c_eth_driver = {
	.probe	= rtl9602c_eth_probe,
	.driver	= {
		.name		= "rtl9602c-eth",
		.of_match_table	= rtl9602c_eth_of_match,
	},
};
module_platform_driver(rtl9602c_eth_driver);

MODULE_DESCRIPTION("Realtek RTL9602C Luna Ethernet driver");
MODULE_LICENSE("GPL");
