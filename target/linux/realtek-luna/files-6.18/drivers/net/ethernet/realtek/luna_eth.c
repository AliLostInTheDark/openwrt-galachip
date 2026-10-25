// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek Luna (MIPS interAptiv) GMAC0 + on-chip switch -- eth0.
 *
 * Clean-room driver for the SoC's CPU-port Gigabit MAC (the "GMAC0" engine at
 * phys 0x18012000) and a minimal open-L2 bring-up of the on-chip switch core
 * (phys 0x1b000000). The CPU reaches the LAN only through the switch:
 *
 *	CPU <-> GMAC0 (eth0) <-> switch CPU-port <-> physical LAN port <-> wire
 *
 * It serves TWO chips, so it carries the FAMILY name; the chip-specific half is
 * a TABLE (struct luna_eth_chip), never an #ifdef and never a second copy of
 * the file -- a duplicated driver is how a repair lands on one board only.
 *
 *	RTL9607C   engineering board, 11 switch ports, 3 CPU GMACs, SerDes uplink
 *	RTL9603CVD LANLY G24W,         6 switch ports, 1 CPU GMAC,  no SerDes
 *
 * The MAC engine is identical on both; the SWITCH registers are not. Of 1361
 * name-matched switch registers, 853 keep their offset and 508 MOVE [tier 3,
 * each chip's own SDK]. The trap that pays for the table: the RTL9607C's
 * P_ABLTY at 0x200 is SDS_CFG on the RTL9603CVD, so a driver that "just worked
 * because the family is the same" would read a SerDes configuration word and
 * call it a link state.
 *
 * Switch bring-up facts, established on the RTL9607C:
 *  - The LOAD-BEARING RX GATE is the per-port spanning-tree state (MSTI_CTRL).
 *    SRC_PORT_PERMIT was blamed alongside it and that reading is REFUTED on
 *    both measurable dies; the field has ONE statement and it is
 *    luna_eth_regs.h's @src_permit.  Do not restate it here.
 *  - MSTI_CTRL resets to 0x000000FF on BOTH chips, every port FORWARDING. Ports
 *    found non-forwarding at probe are the BOOT LOADER's doing, so the write
 *    below is a correction and must stay even if a future loader stops needing
 *    it.
 *  - The integrated copper PHYs need no analog calibration on either chip.
 *  - The ordered sequence matters: replaying only the final register values is
 *    not sufficient on this hardware.
 *
 * Copyright (C) 2026 Confiared <contact@confiared.com>
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mii.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/timer.h>
#include "luna_eth_regs.h"	/* the family MAC/switch register map + per-chip table */
#include "luna_gmac_logic.h"	/* family GMAC ring packings + this shell's hoisted RX verdicts (flowcore) */
#include "gpon_hwaddr.h"	/* the ONE station-address ladder (drivers/net/gpon) */
#include "luna_gpon_nic.h"	/* the GPON<->NIC glue -- this shell now IMPLEMENTS it */
#include "luna_eth_mib.h"
#include "luna_flow.h"
#include "gpon_omci_core.h"	/* omci_onu_input, omci_onu_emit_veip_up_avc, OMCI_LEN.
				 * NOT the OMCI_MT_* codes any more: this shell
				 * stopped decoding message types on 2026-09-10 */
#include "gpon_omci_me.h"	/* struct omci_onu, the common ME store */
#include "gpon_omci_trace.h"	/* G.988 decode-to-a-buffer for the board-side log */

/* ---- bring-up knobs (live-tunable; the datapath framing is HW-uncertain on
 * first contact, so expose the few values most likely to need a tweak) ------ */
static int rx_prefix = 2;
module_param(rx_prefix, int, 0644);
MODULE_PARM_DESC(rx_prefix, "bytes the CPU-port prepends ahead of each RX frame (stripped)");

static unsigned int backstop_ms = 10;
module_param(backstop_ms, uint, 0644);
MODULE_PARM_DESC(backstop_ms, "RX/TX drain backstop poll period (catches a missed IRQ)");

/* OMCI ME2 MIB-Data-Sync boot seed.  Same value and same purpose as the
 * sibling shell's: this ONU holds no persistent MIB, so a seed in 1..30
 * deliberately FAILS the OLT's ME2 audit (its gate reads rsync<31 as
 * not-in-sync) and makes a warm re-admit re-provision us from scratch. */
static unsigned int omci_mds_seed = OMCI_MDS_POISON_SEED;
module_param(omci_mds_seed, uint, 0644);
MODULE_PARM_DESC(omci_mds_seed,
		 "OMCI ME2 MIB-Data-Sync boot seed (1..30 forces the OLT to re-provision)");

static bool sw_cpu_tag;
module_param(sw_cpu_tag, bool, 0644);
MODULE_PARM_DESC(sw_cpu_tag, "enable the switch CPU-port tag engine (default off: plain-L2 forwarding)");

/* CONFIG_LUNA_DEBUG: the bring-up aids below are compiled in only on request;
 * off, each knob is a constant 0 and its branches are dead code. */
#if IS_ENABLED(CONFIG_LUNA_DEBUG)
static int rx_dump = 6;
module_param(rx_dump, int, 0644);
MODULE_PARM_DESC(rx_dump, "hex-dump the first N received frames (bring-up framing check)");

static int tx_dump = 6;
module_param(tx_dump, int, 0644);
MODULE_PARM_DESC(tx_dump, "hex-dump the first N transmitted frames + descriptors");

static unsigned int diag_ms = 3000;
module_param(diag_ms, uint, 0644);
MODULE_PARM_DESC(diag_ms, "period of the per-port real-link/rxpkts diagnostic (0 = off)");

static int diag_count = 12;
module_param(diag_count, int, 0644);
MODULE_PARM_DESC(diag_count, "number of periodic link/rxpkts diagnostic dumps");

static bool phy_survey = true;
module_param(phy_survey, bool, 0644);
MODULE_PARM_DESC(phy_survey, "at open, READ each copper port's BMCR/BMSR under BOTH OCP maps and dump them (read-only; turns a 3-boot experiment into a 1-boot one)");
#else
static const int rx_dump;
static const int tx_dump;
static const unsigned int diag_ms;
static const int diag_count;
static const bool phy_survey;
#endif

static bool copper_phy = true;
module_param(copper_phy, bool, 0644);
MODULE_PARM_DESC(copper_phy, "power up + auto-neg the internal copper PHYs (ports 0-4)");

static bool rtl8221b_phy = true;
module_param(rtl8221b_phy, bool, 0644);
MODULE_PARM_DESC(rtl8221b_phy, "de-assert the RTL8221B 2.5G PHY reset (SerDes-6 uplink)");

/* Default 0 = do NOT assert the GPHY reset, which is what the vendor does. A
 * param and not a deletion, so the old behaviour is one bootarg away. */
static bool gphy_reset;
module_param(gphy_reset, bool, 0444);
MODULE_PARM_DESC(gphy_reset,
		 "assert SOFTWARE_RST.CMD_GPHY_RST_PS during copper PHY bring-up "
		 "(default 0: the vendor never asserts it, and we run none of the "
		 "re-initialisation that reset would require)");

/* Which receive FIFOs stay in reset. Stock holds NONE; the one this board had
 * asserted came from our own GPON pad recipe writing CFG_PCSXF, which is 0x48
 * on this chip -- fixed at the source. -1 leaves the bootloader's value. */
static int rst_rxfifo;
module_param(rst_rxfifo, int, 0644);
MODULE_PARM_DESC(rst_rxfifo,
	"CFG_PCSXF RST_RXFIFO mask to program (-1 = leave the bootloader's value)");

/* The board's own MAC, handed in at boot as ... -- dev/MEASURED-luna_eth.c.md sec 1. */
static char *mac_param;
module_param_named(mac, mac_param, charp, 0444);
MODULE_PARM_DESC(mac,
	"the board's own MAC, handed in at boot (xx:xx:xx:xx:xx:xx). Empty = "
	"fall back to DT, then the engine, then a random locally-administered one");

/* CFG_PHY_CTRL BASE_PHYAD. MEASURED 2026-08-24 on the G24W: ...
 * dev/MEASURED-luna_eth.c.md sec 57. */
static int base_phyad = 0;
module_param(base_phyad, int, 0644);
MODULE_PARM_DESC(base_phyad,
	"CFG_PHY_CTRL BASE_PHYAD to program (-1 = leave the bootloader's value)");

/* The PHY survey is a probe-time dump, so testing one BASE_PHYAD used to cost
 * one build and one boot. This lets a single image answer the whole sweep. */
static struct luna_eth *survey_ep;

/* Let an OCP transaction START before sampling BUSY. The vendor uses a flat
 * mdelay(10) and no poll at all; 0 = sample immediately, the old behaviour. */
static int gphy_settle_us = 200;
module_param(gphy_settle_us, int, 0644);
MODULE_PARM_DESC(gphy_settle_us,
		 "microseconds to let an internal-PHY OCP transaction START before "
		 "sampling BUSY (0 = sample immediately, which returns success "
		 "before the transaction begins and reads a stale zero)");

/* Default 1 since 2026-08-23: emulating ... -- dev/MEASURED-luna_eth.c.md sec 58. */
static int gphy_map = 1;
/* The writable parameter itself is declared beside luna_gphy_lock: its setter
 * has to take that mutex, and a knob that steers an address must not be able to
 * move between the read and the write of one transaction. */

static int phy_settle_ms;
module_param(phy_settle_ms, int, 0644);
MODULE_PARM_DESC(phy_settle_ms, "ms to wait after the PHY patch-done bit (0 = current behaviour; the RTL9603CVD's own U-Boot waits 800)");

static bool cpu_no_loopback = true;
module_param(cpu_no_loopback, bool, 0644);
MODULE_PARM_DESC(cpu_no_loopback, "drop the CPU port from its own egress flood (stops self-loopback RX)");

/* ★ A FRAME THE CPU FLOODS COMES BACK IN ON THE CPU PORT, AND ...
 * dev/MEASURED-luna_eth.c.md sec 2. */
static bool lan_flood_direct = true;
module_param(lan_flood_direct, bool, 0644);
MODULE_PARM_DESC(lan_flood_direct, "send CPU-originated multicast/broadcast to an explicit egress mask (the flood set minus the CPU port) instead of letting the switch flood it back at us");

/* ---- GMAC0 register block (offsets from the DT reg base 0x18012000) -------- */
#define R_MAR0		0x08	/* multicast hash [31:0]				*/
#define R_MAR4		0x0C	/* multicast hash [63:32]			*/
#define R_CMD		0x3B	/* 8-bit command: bit0 RST			*/
#define   CMD_RXCHK	0x02	/* RX checksum offload				*/
#define   CMD_RXJUMBO	0x08	/* accept jumbo					*/
#define R_MSR		0x58	/* media/flow status; top byte = force flow ctl	*/
#define R_TxFDP0	0x1300	/* TX ring0 fetch-descriptor pointer		*/
#define R_TxCDO0	0x1304	/* TX ring0 current-descriptor offset (u16)	*/
#define R_RRING_ROUTE	0x1370	/* RX class -> ring routing			*/
#define R_RxFDP0	0x13F0	/* RX ring0 fetch-descriptor pointer		*/
#define R_RxCDO0	0x13F4	/* RX ring0: RxCDO[31:16] | RxRingSize[15:8]	*/
				/* 32-BIT register: the store must not be 16-bit. MSR(0x58) ...
				 * dev/MEASURED-luna_eth.c.md sec 3. */
static unsigned int msr_top = 0x10;
module_param(msr_top, uint, 0644);
MODULE_PARM_DESC(msr_top, "MSR(0x58) top byte (0x10 = healthy with our init; 0xf0 = stock's value, MEASURED to stall the LAN on the RTL9602C)");

#define IO_CMD_ENABLE	0xc059f130
#define IO_CMD1_ENABLE	0x32000001

/* CPU-tag engine config. CTEN_RX (bit31) makes the MAC strip the 8-byte switch
 * tag in hardware on RX and expose the parsed ingress port in the descriptor;
 * the rest selects tag sizes, the 0x04 protocol and the 0x8899 match. Clearing
 * CTEN_RX leaves the raw in-band tag in the delivered frame. */
#define CPUTAGCR_INIT	0x9022FF04
/* R_CPUTAG1CR[14:8] selects WHICH downstream stream-id the ...
 * dev/MEASURED-luna_eth.c.md sec 4. */
#define CPUTAG1CR_SID(s)	(((s) & 0x7fu) << 8)	/* R_CPUTAG1CR[14:8] */
#define CPUTAG1CR_SID_MASK	CPUTAG1CR_SID(0x7fu)
#define ABLTY_CPU_FORCE	0xBFFF		/* CPU-port forced-ability mode (keep)	*/


/* switch core (SWCORE), phys 0x1b000000 -- dev/MEASURED-luna_eth.c.md sec 5. */
#define   STP_STATE_MASK	0x3
#define   STP_FORWARDING	0x3

/* THE PER-CHIP TABLE. Everything in it MOVED between the two parts, and every
 * field was read from that chip's OWN SDK (tier 3) rather than inferred from
 * the sibling. A zero means "this chip has no such register" and the code must
 * SKIP the write -- offset 0 is the PHY indirect-access data register. */
struct luna_eth_chip {
	const char *name;
	const struct luna_flow_layout *flow;

	/* The switch-core map for THIS chip, and the ONLY home of its port
	 * numbers: a pointer into luna_eth_regs.h so the sibling driver and this
	 * one read the SAME numbers and a correction lands once. */
	const struct luna_sw_map *sw_map;

	/* --- switch port map ------------------------------------------------ */
	/* ★ Force the PON port's MAC link like the CPU port's -- 1 only on a
	 * chip where STOCK was MEASURED doing it. See the write site. */
	u8	force_pon_ablty;
	u8	last_port;	/* highest port to iterate, INCLUSIVE		*/
	u8	gphy_ports;	/* bitmap: ports whose PHY is a GPHY, not FE	*/

	/* --- switch registers that MOVED ------------------------------------ */
	/* force_ablty / p_ablty / ablty_force live in luna_sw_map (->sw_map) */
	u32	msti_ctrl;	/* + 4*port: per-port spanning-tree state	*/
	u32	cpu_tag_insert;
	u32	cpu_tag_aware;
	u32	swcore_rst;	/* swcore soft reset (bit10), excludes cfg	*/
	/* gphy_misc lives in luna_sw_map too: the RTL9602C driver needs the same fact */
	u32	fephy_poll;	/* 0 on a chip with no FE-PHY auto-poller	*/
	u32	cfg_phy_ini;	/* per-port PHY enable; U-Boot loads it from efuse*/
	u32	cfg_phy_ctrl;	/* MSK_MDI[8:5] | BASE_PHYAD[4:0]; 0 = not known */
	u32	cfg_pcsxf;	/* RST_RXFIFO[13:10] | MIIRX_IPG[9:5] | PCSXF[4:1] */

	/* --- port-isolation packing, which differs in SHAPE not just offset -- */
	u8	piso_per_word;	/* how many ports share one 32-bit word		*/
	u8	piso_bits;	/* width of one port's mask			*/
	u32	piso_all;	/* the all-open value for ONE port		*/

	/* --- SerDes uplink: present only on the bigger part ------------------ */
	u32	serdes_linemode;	/* 0 = no SerDes on this chip		*/
	u32	force_ablty_x;		/* SerDes/PBO ability trio, 0 if absent	*/
	u32	ablty_force_x;
	u32	sds_fib_status;		/* + 0x20*idx, 0 if absent		*/

	/* --- SoC glue OUTSIDE the switch's own register space ---------------- */
	u32	sys_status;	/* 0 = this chip's bring-up does not use it	*/

	/* ★ A MODEL FACT LIVING IN THE CHIP TABLE, said out loud ...
	 * dev/MEASURED-luna_eth.c.md sec 6. */
	u8	wan_mac_offset;
	u32	cold_phy_status;	/* native cold PHY recipe; 0 keeps the warm path */
};

/* The RTL9607C entry is this file's previous constants, value for value: the
 * engineering board that boots today must see a byte-identical register
 * sequence, so any behaviour change on it is a defect of the refactor. */
static const struct luna_eth_chip luna_chip_rtl9607c = {
	.sw_map		= &rtl9607c_sw_map,
	.name		= "RTL9607C",
	.flow		= NULL,	/* Flow layout not established on this die. */
	/* 0 DELIBERATELY, a scope statement and not a finding: nobody has diffed
	 * SWCORE 0x1cc/0x238 stock-vs-ours on the RTL9607C board, and this chip
	 * reaches its PON/PBO abilities through the force_ablty_x trio below. */
	.force_pon_ablty = 0,
	.last_port	= 11,	/* 0..4,8 copper; 5 PON; 6,7 SerDes; 9 CPU; 11 PBO */
	.gphy_ports	= 0x1f,	/* all five copper ports are GPHYs here	*/
	.msti_ctrl	= 0x1704C,
	.cpu_tag_insert	= 0x230F4,
	.cpu_tag_aware	= 0x230F8,
	.swcore_rst	= 0x00108,
	.fephy_poll	= 0,	/* every PHY here is a GPHY: no FE auto-poller	*/
	.cfg_phy_ini	= 0x0004C,
	/* ★ 0 WRITTEN DOWN, not left to the compiler (2026-09-14). ...
	 * dev/MEASURED-luna_eth.c.md sec 7. */
	.cfg_phy_ctrl	= 0,	/* NOT ESTABLISHED on this die -- never the sibling's */
	.cfg_pcsxf	= 0,	/* NOT ESTABLISHED on this die -- the write is skipped */
	.piso_per_word	= 1,
	.piso_bits	= 29,
	.piso_all	= 0x1FFFFFFF,
	.serdes_linemode = 0x00084,
	.force_ablty_x	= 0x002F4,
	.ablty_force_x	= 0x002FC,
	.sds_fib_status	= 0x0028C,
	.sys_status	= 0,
	/* 0 DELIBERATELY: nobody has captured a WAN identity on the engineering
	 * board, and it carries no ISP service.  Copying the G24W's 5 here would
	 * be one model's answer standing in for another's. */
	.wan_mac_offset	= 0,
	.cold_phy_status = 0xb8000044,
};

/* The RTL9603CVD entry -- every field read from THIS chip's ...
 * dev/MEASURED-luna_eth.c.md sec 8. */
static const struct luna_eth_chip luna_chip_rtl9603cvd = {
	.sw_map		= &rtl9603cvd_sw_map,
	.name		= "RTL9603CVD",
	.flow = &luna_flow_rtl9603cvd,
	/* MEASURED 2026-08-27, stock vs ours, SWCORE 0x180..0x1fc: ...
	 * dev/MEASURED-luna_eth.c.md sec 9. */
	.force_pon_ablty = 1,
	.last_port	= 5,	/* 0..2 FE; 3 GE; 4 PON; 5 CPU (6 = PBO loopback)*/
	.gphy_ports	= 0x08,	/* ONLY port 3 is a GPHY; 0..2 are FE PHYs	*/
	.msti_ctrl	= 0x1713C,
	.cpu_tag_insert	= 0x2303C,
	.cpu_tag_aware	= 0x23040,
	.swcore_rst	= 0x000E0,
	.fephy_poll	= 0x0000C,
	.cfg_phy_ini	= 0x00050,
	.cfg_phy_ctrl	= 0x0004C,
	.cfg_pcsxf	= 0x00048,
	.piso_per_word	= 2,
	.piso_bits	= 12,
	.piso_all	= 0xFFF,
	.serdes_linemode = 0,	/* no SerDes on this part			*/
	.force_ablty_x	= 0,
	.ablty_force_x	= 0,
	.sds_fib_status	= 0,
	.sys_status	= 0xB8000044,	/* SoC handshake, outside SWCORE		*/
	/* MEASURED on THIS unit's own stock, two independent sources ...
	 * dev/MEASURED-luna_eth.c.md sec 10. */
	.wan_mac_offset	= 5,
	.cold_phy_status = 0,	/* keep the existing warm PHY path on this die */
};

/* Accessors: the table lives in `ep->c`, so a per-port register is one call and
 * a chip that lacks a register is answered with 0 and SKIPPED by the caller. */
#define SW_FORCE_ABLTY(ep, p)	((ep)->c->sw_map->force_ablty + (p) * 4)
#define SW_P_ABLTY(ep, p)	((ep)->c->sw_map->p_ablty + (p) * 4)
#define SW_ABLTY_FORCE(ep, p)	((ep)->c->sw_map->ablty_force + (p) * 4)
#define SW_MSTI_CTRL(ep, p)	((ep)->c->msti_ctrl + (p) * 4)

/* VLAN: filtering must not gate CPU<->LAN egress (the boot loader may leave it on
 * with the CPU port outside the member set). */
#define   VLAN_FILTERING	BIT(0)

/* Per-port lookup-miss (unknown-DA) action, 2 bits/port; 0 = FORWARD. Needed for
 * the post-ARP unicast / IPv6-ND path (the first broadcast already floods). */
#define   DA_ACT_PORTS		0x3FFFFF	/* ports 0..10, 2 bits each		*/

#define ABLTY_1G_FULL_LINK	0x16	/* speed=1000, duplex=full, link=up	*/
#define ABLTY_FORCE_ALL		0xFFF	/* force all basic abilities		*/

/* The port map lives in the chip table: the RTL9607C has 11 ...
 * dev/MEASURED-luna_eth.c.md sec 11. */
#define RTL_CPU_TAG_LEN		8
#define RTL_CPU_TAG_ETYPE	0x8899

/* SOC_SW_ENABLE and its bits are the FAMILY's -- ... -- dev/MEASURED-luna_eth.c.md sec 12. */
#define   CMD_GPHY_RST_PS	BIT(6)	/* SOFTWARE_RST(0x0E0) bit 6	*/
#define   FEPHY_STOP_POLL	BIT(16)	/* in fephy_poll: 1 = auto-poller OFF	*/

/* Live link / speed (genuine, independent of the MAC force). Copper genuine link
 * = MDIO BMSR bit2; SerDes genuine link = SDS_FIB_STATUS. */
#define SW_SDS_FIB_STATUS(ep, s) ((ep)->c->sds_fib_status + (s) * 0x20)
#define   SDS_LINK_OK		BIT(4)
#define   SDS_SDET		BIT(17)

/* Per-port RX MIB counters (direct reads; block base 0x32600, stride 0x80). */
#define SW_MIB_RX_UCAST(p)	(0x32620 + (p) * 0x80)
#define SW_MIB_RX_MCAST(p)	(0x32628 + (p) * 0x80)
#define SW_MIB_RX_BCAST(p)	(0x3262C + (p) * 0x80)

/* RTL8221B 2.5G PHY reset line: DTS rtl8221b_dev0_reset = <&gpio1 28 1> (active
 * low). gpio1 = bank 1 (pins 32..63); pin 28 -> bit 28 of bank-1 DIR/DAT, plus
 * the GPIO function-enable for pins 32..63. */
#define SW_IO_GPIO_EN_HI	0x03c		/* pinmux function-enable, pins 32..63	*/
#define SOC_GPIO_B1_DIR	((void __iomem *)0xb8003324ul)	/* bank1 direction (1=out)*/
#define SOC_GPIO_B1_DAT	((void __iomem *)0xb8003328ul)	/* bank1 data		*/
#define RTL8221B_RST_BIT	BIT(28)
#define RTL8221B_PHYAD		6

/* MII BMCR/BMSR bits. The MII register bits are ... -- dev/MEASURED-luna_eth.c.md sec 13. */

struct luna_eth {
	const struct luna_eth_chip *c;	/* THE per-chip table -- never an #ifdef */
	struct net_device	*ndev;
	struct device		*dev;
	void __iomem		*base;	/* GMAC0			*/
	void __iomem		*sw;	/* switch core			*/
	int			irq;

	struct napi_struct	napi;
	struct timer_list	backstop;
	struct timer_list	diag;
	struct work_struct	diag_work;
	int			diag_left;
	spinlock_t		tx_lock;

	/* Plain streaming DMA: the kernel manages the L2 so dma_map/unmap flush +
	 * invalidate it -- no bounce buffers needed. */
	struct rx_desc		*rx_ring;
	dma_addr_t		rx_ring_dma;
	struct sk_buff		*rx_skb[RX_RING_SIZE];
	dma_addr_t		rx_buf_dma[RX_RING_SIZE];
	unsigned int		rx_head;

	struct tx_desc		*tx_ring;
	dma_addr_t		tx_ring_dma;
	struct sk_buff		*tx_skb[TX_RING_SIZE];
	dma_addr_t		tx_buf_dma[TX_RING_SIZE];
	unsigned int		tx_buf_len[TX_RING_SIZE];
	void			*tx_buf[TX_RING_SIZE];	/* per-slot linear copy buffer */
	unsigned int		tx_head, tx_dirty;	/* free-running counters	*/

	int			rx_dumped;
	int			tx_dumped;

	/* CPU-side OMCI (OMCC) -- dev/MEASURED-luna_eth.c.md sec 14. */
	bool			hw_up;		/* rings + DMA live from the first open on */
	bool			closing;
	bool			omci_trap_on;	/* armed at Configure_Port-ID */
	unsigned int		omci_sid;	/* the OMCC stream id the OLT gave us */
	u32			dbg_omci_rx;	/* DS OMCI frames trapped to the CPU */
	u32			dbg_omci_rxlen;
	u32			dbg_omci_tx;	/* US OMCI responses queued */
	u32			dbg_omci_tx_drop;
	u32			dbg_omci_unhandled;
	/* One log line per DISTINCT cpu-tag reason this die has ever stamped, and
	 * never a second for the same one. The OMCI reason is PER CHIP, a wrong
	 * one matches nothing, and a trap that matches nothing is
	 * indistinguishable from an OLT that sent nothing. */
	u32			rx_reason_seen[8];

	/* The egress mask a CPU-originated flood must use: the very set
	 * switch_init() programs into LUT_*_FLOOD, minus this chip's CPU port.
	 * Derived there so the flood registers and the descriptor can never
	 * disagree about which ports a broadcast belongs on. */
	u32			lan_flood_mask;

	/* ---- WAN (gpon0), the data GEM's netdev ------------------------- */
	struct net_device	*wan_ndev;
	struct luna_flow_engine *flow;

	/* The same ledger idea applied to opts3[19:16], and it is the ...
	 * dev/MEASURED-luna_eth.c.md sec 15. */
	u16			rx_src_port_seen;

	/* And one line per distinct PON STREAM ID ever delivered on ...
	 * dev/MEASURED-luna_eth.c.md sec 16. */
	u32			rx_pon_sid_seen[4];
};

/* The single instance, for the exported glue the GPON driver calls.  Same
 * shape as the sibling shell: this SoC has exactly one CPU-port GMAC. */
static struct luna_eth *g_ep;

/* The G.988 ONU model. The protocol is the CORE's: every byte of state lives in
 * this struct and every decision in gpon_omci_core.c. */
static struct omci_onu luna_eth_onu;

static inline u32 ep_rd(struct luna_eth *ep, u32 r) { return ioread32(ep->base + r); }
static inline void ep_wr(struct luna_eth *ep, u32 r, u32 v) { iowrite32(v, ep->base + r); }
static inline u32 sw_rd(struct luna_eth *ep, u32 r) { return ioread32(ep->sw + r); }
static inline void sw_wr(struct luna_eth *ep, u32 r, u32 v) { iowrite32(v, ep->sw + r); }
static inline void sw_or(struct luna_eth *ep, u32 r, u32 v) { sw_wr(ep, r, sw_rd(ep, r) | v); }

#include "luna_flow.c"

static inline unsigned int tx_slot(unsigned int counter) { return counter % TX_RING_SIZE; }

/* internal GPHY MDIO (indirect window) -- dev/MEASURED-luna_eth.c.md sec 17. */
static DEFINE_MUTEX(luna_gphy_lock);

/* gphy_map picks the OCP map, so it is part of the ADDRESS ...
 * dev/MEASURED-luna_eth.c.md sec 18. */
static int gphy_map_set(const char *val, const struct kernel_param *kp)
{
	int rc;

	mutex_lock(&luna_gphy_lock);
	rc = param_set_int(val, kp);
	mutex_unlock(&luna_gphy_lock);
	return rc;
}
static const struct kernel_param_ops gphy_map_ops = {
	.set = gphy_map_set,
	.get = param_get_int,
};
module_param_cb(gphy_map, &gphy_map_ops, &gphy_map, 0644);
MODULE_PARM_DESC(gphy_map, "internal-PHY OCP map: 0 = per the chip table, 1 = force GPHY page 0xA40 on every port, 2 = force the flat FE map (a bring-up experiment: the vendor SDK and its own OCP map disagree for the FE ports)");

/* The UNI administrative lock, as a DESIRED state per copper ...
 * dev/MEASURED-luna_eth.c.md sec 19. */
static u32 luna_uni_locked_ports;

static bool luna_uni_port_locked(unsigned int port)
{
	return port < 32 && (READ_ONCE(luna_uni_locked_ports) & BIT(port));
}

static int gphy_wait(struct luna_eth *ep)
{
	int i;

	/* let the transaction START before asking whether it has finished. */
	if (gphy_settle_us)
		udelay(gphy_settle_us);

	for (i = 0; i < 10000; i++) {
		if (!(sw_rd(ep, SW_GPHY_IND_RD) & GPHY_IND_BUSY))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

/* The OCP address of standard MII register `reg` on the PHY behind switch port
 * `p`. See the note above GPHY_MII_PAGE for why this is not one constant. */
static u32 gphy_ocp(struct luna_eth *ep, unsigned int p, unsigned int reg)
{
	if (gphy_map == 1 || (gphy_map == 0 && (ep->c->gphy_ports & BIT(p))))
		return GPHY_MII_PAGE | ((reg & 7) << 1);	/* GPHY page 0xA40 */
	return (reg & 0x1f) << 1;			/* FE PHY: flat map */
}

/* Read one OCP address on one PHY. The SURVEY needs this because it asks the
 * SAME register through BOTH maps, which `gphy_read()` cannot express: that one
 * consults the chip table (correctly) and so can only ever return one answer. */
static int gphy_read_ocp__locked(struct luna_eth *ep, unsigned int phyad,
				 u32 ocp, u16 *out)
{
	/* ★ PREFLIGHT.  Issuing a command while the previous transaction is
	 *   still BUSY makes the window answer for the wrong one. */
	if (gphy_wait(ep))
		return -ETIMEDOUT;
	sw_wr(ep, SW_GPHY_IND_CMD, (phyad << 16) | ocp | GPHY_IND_EN);
	if (gphy_wait(ep))
		return -ETIMEDOUT;
	*out = sw_rd(ep, SW_GPHY_IND_RD) & 0xffff;
	return 0;
}

/* > 0 and *out set, or negative with *out LEFT EXACTLY AS IT ...
 * dev/MEASURED-luna_eth.c.md sec 20. */
static int gphy_read__locked(struct luna_eth *ep, unsigned int phyad,
			     unsigned int reg, u16 *out)
{
	u32 adr = (phyad << 16) | gphy_ocp(ep, phyad, reg);

	if (gphy_wait(ep))
		goto timeout;
	sw_wr(ep, SW_GPHY_IND_CMD, adr | GPHY_IND_EN);
	if (gphy_wait(ep))
		goto timeout;
	*out = sw_rd(ep, SW_GPHY_IND_RD) & 0xffff;
	return 0;
timeout:
	dev_warn_ratelimited(ep->dev,
		"gphy: read timeout phy %u reg %u -- the indirect bus never cleared BUSY; NO value is returned\n",
		phyad, reg);
	return -ETIMEDOUT;
}

static int gphy_write_ocp__locked(struct luna_eth *ep, unsigned int phyad,
				  u32 ocp, u16 val)
{
	/* ★ PREFLIGHT BEFORE THE DATA REGISTER.  SW_GPHY_IND_WD was written while a
	 *   previous transaction could still be BUSY, so the value could be
	 *   consumed by that one instead of this one. */
	if (gphy_wait(ep))
		goto timeout;
	sw_wr(ep, SW_GPHY_IND_WD, val);
	sw_wr(ep, SW_GPHY_IND_CMD, GPHY_IND_PHY(phyad) | ocp |
				   GPHY_IND_WREN | GPHY_IND_EN);
	if (gphy_wait(ep))
		goto timeout;
	return 0;
timeout:
	dev_warn_ratelimited(ep->dev,
		"gphy: write timeout phy %u ocp %#06x val %04x -- the write did NOT land\n",
		phyad, ocp, val);
	return -ETIMEDOUT;
}

static int gphy_write__locked(struct luna_eth *ep, unsigned int phyad,
			      unsigned int reg, u16 val)
{
	return gphy_write_ocp__locked(ep, phyad, gphy_ocp(ep, phyad, reg), val);
}

/* The OCP address THIS BOARD'S OWN ADMINISTRATIVE PATH uses, ...
 * dev/MEASURED-luna_eth.c.md sec 21. */
static u32 gphy_ocp_miim(struct luna_eth *ep, unsigned int p, unsigned int reg)
{
	if (ep->c->gphy_ports & BIT(p))
		return GPHY_MII_PAGE | ((reg & 7) << 1);
	return (reg & 0x1f) << 1;
}

/* Power up + (re)start auto-negotiation on the integrated ...
 * dev/MEASURED-luna_eth.c.md sec 22. */
static void eth_phy_survey__locked(struct luna_eth *ep)
{
	unsigned int p;

	lockdep_assert_held(&luna_gphy_lock);

	if (ep->c->cfg_phy_ini)
		dev_info(ep->dev,
			 "phy survey: CFG_PHY_INI(%#05x) = %08x  (U-Boot loads its per-port field from the efuse; we do NOT write it -- the polarity is unresolved)\n",
			 ep->c->cfg_phy_ini, sw_rd(ep, ep->c->cfg_phy_ini));

	for (p = 0; p < ep->c->sw_map->n_copper; p++) {
		u16 g_bmcr = 0, g_bmsr = 0, f_bmcr = 0, f_bmsr = 0;
		int rc;

		/* four reads of ONE port through BOTH maps, inside the caller's
		 * single acquisition: a window taken per access would let
		 * another user answer in the middle of them */
		rc = gphy_read_ocp__locked(ep, p, GPHY_MII_PAGE | (0 << 1), &g_bmcr);
		if (!rc)
			rc = gphy_read_ocp__locked(ep, p, GPHY_MII_PAGE | (1 << 1), &g_bmsr);
		if (!rc)
			rc = gphy_read_ocp__locked(ep, p, 0 << 1, &f_bmcr);
		if (!rc)
			rc = gphy_read_ocp__locked(ep, p, 1 << 1, &f_bmsr);
		if (rc) {
			/* ★ NOT PRINTED AS VALUES.  The old code printed 0xffff
			 * and then reasoned about whether 0xffff meant anything. */
			dev_info(ep->dev,
				 "phy survey: port %u -- the bus did not answer (%d); no register values\n",
				 p, rc);
			continue;
		}

		dev_info(ep->dev,
			 "phy survey: port %u (%s by table)  gphy-page[bmcr=%04x bmsr=%04x]%s  flat[bmcr=%04x bmsr=%04x]%s\n",
			 p, (ep->c->gphy_ports & BIT(p)) ? "GPHY" : "FE",
			 g_bmcr, g_bmsr,
			 (g_bmcr == 0xffff && g_bmsr == 0xffff) ? " <- ALL-ONES, i.e. no answer" : "",
			 f_bmcr, f_bmsr,
			 (f_bmcr == 0xffff && f_bmsr == 0xffff) ? " <- ALL-ONES, i.e. no answer" : "");
	}
}

static void eth_phy_survey(struct luna_eth *ep)
{
	mutex_lock(&luna_gphy_lock);
	eth_phy_survey__locked(ep);
	mutex_unlock(&luna_gphy_lock);
}

/* Release the receive FIFOs. MEASURED 2026-08-24, stock vs ...
 * dev/MEASURED-luna_eth.c.md sec 23. */
static void eth_rxfifo_release(struct luna_eth *ep)
{
	u32 was, now;

	if (!ep->c->cfg_pcsxf || rst_rxfifo < 0)
		return;

	was = sw_rd(ep, ep->c->cfg_pcsxf);
	if (((was >> 10) & 0xf) == (u32)(rst_rxfifo & 0xf)) {
		dev_info(ep->dev,
			 "CFG_PCSXF(%#05x) = %08x already has RST_RXFIFO %#x -- left alone\n",
			 ep->c->cfg_pcsxf, was, rst_rxfifo & 0xf);
		return;
	}
	sw_wr(ep, ep->c->cfg_pcsxf,
	      (was & ~(0xfu << 10)) | ((u32)(rst_rxfifo & 0xf) << 10));
	now = sw_rd(ep, ep->c->cfg_pcsxf);
	dev_info(ep->dev,
		 "CFG_PCSXF(%#05x): RST_RXFIFO %#x -> %#x (%08x -> %08x)\n",
		 ep->c->cfg_pcsxf, (was >> 10) & 0xf, rst_rxfifo & 0xf, was, now);
}

/* Program CFG_PHY_CTRL, and SAY what was there before: the ...
 * dev/MEASURED-luna_eth.c.md sec 24. */
static int eth_phy_ctrl_apply__locked(struct luna_eth *ep)
{
	u32 was, back;

	lockdep_assert_held(&luna_gphy_lock);
	if (!ep->c->cfg_phy_ctrl || base_phyad < 0)
		return 0;

	was = sw_rd(ep, ep->c->cfg_phy_ctrl);
	if ((was & 0x1f) == (u32)(base_phyad & 0x1f)) {
		dev_info(ep->dev,
			 "CFG_PHY_CTRL(%#05x) = %08x already carries BASE_PHYAD %u -- left alone\n",
			 ep->c->cfg_phy_ctrl, was, base_phyad & 0x1f);
		return 0;
	}
	/* ADMISSION: the window must be idle before the address moves. */
	if (gphy_wait(ep)) {
		dev_err(ep->dev,
			"CFG_PHY_CTRL(%#05x): the indirect window never cleared BUSY, so BASE_PHYAD %u -> %u is NOT applied -- retargeting it now would re-point a transaction already in flight\n",
			ep->c->cfg_phy_ctrl, was & 0x1f, base_phyad & 0x1f);
		return -ETIMEDOUT;
	}
	sw_wr(ep, ep->c->cfg_phy_ctrl, (was & ~0x1fu) | (base_phyad & 0x1f));
	back = sw_rd(ep, ep->c->cfg_phy_ctrl);
	if ((back & 0x1f) != (u32)(base_phyad & 0x1f)) {
		dev_err(ep->dev,
			"CFG_PHY_CTRL(%#05x): BASE_PHYAD %u did NOT take -- it reads %08x, so every PHY address after this would be wrong\n",
			ep->c->cfg_phy_ctrl, base_phyad & 0x1f, back);
		return -EIO;
	}
	dev_info(ep->dev,
		 "CFG_PHY_CTRL(%#05x): BASE_PHYAD %u -> %u (%08x -> %08x)\n",
		 ep->c->cfg_phy_ctrl, was & 0x1f, base_phyad & 0x1f, was, back);
	return 0;
}

static int eth_phy_ctrl_apply(struct luna_eth *ep)
{
	int rc;

	mutex_lock(&luna_gphy_lock);
	rc = eth_phy_ctrl_apply__locked(ep);
	mutex_unlock(&luna_gphy_lock);
	return rc;
}

#if IS_ENABLED(CONFIG_LUNA_DEBUG)
/* Writing this re-applies BASE_PHYAD and dumps the survey again, so a whole
 * sweep costs one boot instead of one build each. */
static int resurvey_set(const char *val, const struct kernel_param *kp)
{
	struct luna_eth *ep;
	int rc;

	/* ⚠ A MODULE-PARAMETER WRITER CAN ARRIVE AT ANY MOMENT, ...
	 * dev/MEASURED-luna_eth.c.md sec 25. */
	mutex_lock(&luna_gphy_lock);
	ep = survey_ep;
	if (!ep) {
		mutex_unlock(&luna_gphy_lock);
		return -ENODEV;
	}
	rc = eth_phy_ctrl_apply__locked(ep);
	if (!rc)
		eth_phy_survey__locked(ep);
	mutex_unlock(&luna_gphy_lock);
	return rc;
}
#endif
/* Clear the diagnostic's subject.  Called from ndo_stop and, as the lifetime
 * guarantee, from a devres action that runs before devres frees @ep. */
static void luna_eth_survey_withdraw(void *cookie)
{
	struct luna_eth *ep = cookie;

	mutex_lock(&luna_gphy_lock);
	if (survey_ep == ep)
		survey_ep = NULL;
	mutex_unlock(&luna_gphy_lock);
}

#if IS_ENABLED(CONFIG_LUNA_DEBUG)
static const struct kernel_param_ops resurvey_ops = { .set = resurvey_set };
module_param_cb(resurvey, &resurvey_ops, NULL, 0200);
MODULE_PARM_DESC(resurvey, "write anything: re-apply base_phyad and re-dump the PHY survey");
#endif

/* The three public accessors: each takes the indirect window for the WHOLE
 * command/data sequence, and nothing inside takes it again. */
static int gphy_read(struct luna_eth *ep, unsigned int phyad, unsigned int reg,
		     u16 *out)
{
	int rc;

	mutex_lock(&luna_gphy_lock);
	rc = gphy_read__locked(ep, phyad, reg, out);
	mutex_unlock(&luna_gphy_lock);
	return rc;
}

/* The RTL9607C monitor's cold recipe (83c02524 in image 66ef3f7b), also
 * measured from a NAND cold boot on 2026-10-05. TFTP had hidden this missing
 * initialization. The done bit belongs LAST, after the patch and MIIM writes. */
static int eth_copper_cold_init__locked(struct luna_eth *ep)
{
	void __iomem *status;
	u32 disabled, ini;
	unsigned int p, i;
	int rc;

	lockdep_assert_held(&luna_gphy_lock);
	if (!ep->c->cold_phy_status)
		return 0;
	status = (void __iomem *)(uintptr_t)ep->c->cold_phy_status;
	if (readl(status) & BIT(0))
		return 0;
	for (i = 0; i < 1000; i++) {
		if (readl(status) & BIT(1))
			break;
		udelay(100);
	}
	if (i == 1000) {
		dev_err(ep->dev, "cold PHY: SoC ready timed out; no PHY writes\n");
		return -ETIMEDOUT;
	}
	rc = gphy_wait(ep);
	if (rc) {
		dev_err(ep->dev, "cold PHY: MIIM window busy (%d); no PHY writes\n", rc);
		return rc;
	}
	disabled = (sw_rd(ep, 0x23024) & 0x3f) >> 1;
	ini = sw_rd(ep, ep->c->cfg_phy_ini);
	sw_wr(ep, ep->c->cfg_phy_ini, (ini & ~(0x1fu << 10)) | (disabled << 10));
	if ((sw_rd(ep, ep->c->cfg_phy_ini) & (0x1fu << 10)) != (disabled << 10)) {
		dev_err(ep->dev, "cold PHY: strap-derived port mask did not take\n");
		return -EIO;
	}
	sw_wr(ep, ep->c->sw_map->gphy_misc, 1);
	if (!(sw_rd(ep, ep->c->sw_map->gphy_misc) & BIT(0))) {
		dev_err(ep->dev, "cold PHY: patch flag did not take\n");
		return -EIO;
	}
	msleep(800);
	p = ep->c->sw_map->cpu_port;
	sw_wr(ep, SW_FORCE_ABLTY(ep, p), 0x96);
	sw_wr(ep, SW_ABLTY_FORCE(ep, p), 0xffff);
	/* Bit 14 is reserved and reads zero on this die. */
	if ((sw_rd(ep, SW_FORCE_ABLTY(ep, p)) & ABLTY_CPU_FORCE) != 0x96 ||
	    (sw_rd(ep, SW_ABLTY_FORCE(ep, p)) & ABLTY_CPU_FORCE) != ABLTY_CPU_FORCE) {
		dev_err(ep->dev, "cold PHY: CPU port force did not take\n");
		return -EIO;
	}
	for (p = 0; p < ep->c->sw_map->n_copper; p++) {
		u16 bmcr = 0x1140;

		if (disabled & BIT(p))
			continue;
		if (luna_uni_port_locked(p))
			bmcr |= BMCR_PDOWN;
		rc = gphy_write_ocp__locked(ep, p, gphy_ocp_miim(ep, p, 0), bmcr);
		if (rc) {
			dev_err(ep->dev, "cold PHY: port %u write failed (%d); done not published\n", p, rc);
			return rc;
		}
	}
	sw_wr(ep, ep->c->sw_map->gphy_misc, 1);
	if (!(sw_rd(ep, ep->c->sw_map->gphy_misc) & BIT(0))) {
		dev_err(ep->dev, "cold PHY: final patch flag did not take\n");
		return -EIO;
	}
	writel(readl(status) | BIT(0), status);
	if (!(readl(status) & BIT(0))) {
		dev_err(ep->dev, "cold PHY: completion bit did not take\n");
		return -EIO;
	}
	/* The cold PHYs expose the programmed BMCR only after completion. */
	for (p = 0; p < ep->c->sw_map->n_copper; p++) {
		u16 bmcr, expected = 0x1140 | (luna_uni_port_locked(p) ? BMCR_PDOWN : 0);

		if (disabled & BIT(p))
			continue;
		rc = gphy_read_ocp__locked(ep, p, gphy_ocp_miim(ep, p, 0), &bmcr);
		if (rc) {
			dev_err(ep->dev, "cold PHY: port %u readback failed after completion (%d)\n", p, rc);
			return rc;
		}
		if (bmcr != expected) {
			dev_err(ep->dev, "cold PHY: port %u BMCR %04x, expected %04x after completion\n",
				p, bmcr, expected);
			return -EIO;
		}
	}
	return 1;
}

/* No gphy_write() wrapper: its only writer today is the ...
 * dev/MEASURED-luna_eth.c.md sec 26. */
static int eth_copper_phy_up(struct luna_eth *ep)
{
	unsigned int p;
	int rc = 0;

	/* ★ ONE ACQUISITION FOR THE WHOLE SEQUENCE, not just the BMCR ...
	 * dev/MEASURED-luna_eth.c.md sec 59. */
	mutex_lock(&luna_gphy_lock);
	rc = eth_copper_cold_init__locked(ep);
	if (rc) {
		if (rc > 0)
			rc = 0;
		goto out;
	}

	/* Save CFG_PHY_INI across the GPHY reset and put it back. The ...
	 * dev/MEASURED-luna_eth.c.md sec 27. */
	{
		u32 phy_ini = ep->c->cfg_phy_ini
			      ? sw_rd(ep, ep->c->cfg_phy_ini) : 0;

		/* The vendor NEVER asserts this, and we re-init nothing after ...
		 * dev/MEASURED-luna_eth.c.md sec 28. */
		if (gphy_reset) {
			sw_or(ep, ep->c->swcore_rst, CMD_GPHY_RST_PS);
			msleep(50);
		}

		if (ep->c->cfg_phy_ini) {
			u32 after_rst = sw_rd(ep, ep->c->cfg_phy_ini);

			sw_wr(ep, ep->c->cfg_phy_ini, phy_ini);
			dev_info(ep->dev,
				 "phy power: CFG_PHY_INI(%#05x) %08x -> %08x across the GPHY reset, restored to %08x (read back %08x)\n",
				 ep->c->cfg_phy_ini, phy_ini, after_rst,
				 phy_ini, sw_rd(ep, ep->c->cfg_phy_ini));
		}
	}

	/* The FE auto-poller is OFF at reset on the chips that have one, and that
	 * is a "configured OK but dead" trap: with it stopped the switch never
	 * learns FE link state, so a socket trains a good link and the switch
	 * forwards nothing through it. The chip's own boot loader clears it. */
	if (ep->c->fephy_poll)
		sw_wr(ep, ep->c->fephy_poll,
		      sw_rd(ep, ep->c->fephy_poll) & ~FEPHY_STOP_POLL);

	/* The UNI decision is made inside the same acquisition: read outside and
	 * an administrative lock landing in between is undone by this write. */
	for (p = 0; p < ep->c->sw_map->n_copper; p++) {
		u16 bmcr;

		/* ★★ A FAILED READ WRITES NOTHING.  With the old sentinel this
		 *    loop read 0xffff, modified it and wrote it back -- setting
		 *    loopback, isolate and reset on a port whose bus was merely
		 *    slow to answer. */
		rc = gphy_read__locked(ep, p, 0, &bmcr);
		if (rc) {
			dev_err(ep->dev,
				"gphy: port %u BMCR could not be read (%d) -- nothing is written back and this bring-up FAILS\n",
				p, rc);
			break;
		}
		if (luna_uni_port_locked(p)) {
			/* the OLT has this UNI locked: it stays powered down,
			 * and auto-negotiation is NOT restarted under it */
			bmcr |= BMCR_PDOWN;
			bmcr &= ~BMCR_ANRESTART;
		} else {
			bmcr &= ~BMCR_PDOWN;		/* leave power-down	*/
			bmcr |= BMCR_ANENABLE | BMCR_ANRESTART;	/* + restart	*/
		}
		rc = gphy_write__locked(ep, p, 0, bmcr);
		if (rc) {
			dev_err(ep->dev,
				"gphy: port %u BMCR write did not land (%d); its power state is UNKNOWN, not assumed\n",
				p, rc);
			break;
		}
	}
	/* ★★ AND A LOCKED PORT IS RE-APPLIED THROUGH THE ...
	 * dev/MEASURED-luna_eth.c.md sec 29. */
	for (p = 0; !rc && p < ep->c->sw_map->n_copper; p++) {
		u32 ocp = gphy_ocp_miim(ep, p, 0);
		u16 bmcr;

		if (!luna_uni_port_locked(p))
			continue;
		/* ⚠ A VALUE THAT COULD NOT BE READ IS ITS OWN ERROR, and it is
		 *   reported separately: printing "reads %04x" for a read that
		 *   never happened puts a number nobody measured in the log. */
		rc = gphy_read_ocp__locked(ep, p, ocp, &bmcr);
		if (!rc && !(bmcr & BMCR_PDOWN))
			rc = gphy_write_ocp__locked(ep, p, ocp,
						    (u16)(bmcr | BMCR_PDOWN));
		if (!rc)
			rc = gphy_read_ocp__locked(ep, p, ocp, &bmcr);
		if (rc) {
			dev_err(ep->dev,
				"gphy: port %u is administratively LOCKED and its administrative-map BMCR could not be read or written (%d) -- its power state is UNKNOWN, not assumed\n",
				p, rc);
			break;
		}
		/* ★ A DESIRED MASK IS NOT PROOF THE PHY STAYED DOWN. */
		if (!(bmcr & BMCR_PDOWN)) {
			dev_err(ep->dev,
				"gphy: port %u is administratively LOCKED and its administrative-map BMCR reads %04x -- the power-down did not hold across the bring-up; refusing to bring the interface up over it\n",
				p, bmcr);
			rc = -EIO;
			break;
		}
	}
	if (rc)
		goto out;
	sw_or(ep, ep->c->sw_map->gphy_misc, BIT(0));		/* patch-done sticky	*/

	/* The RTL9603CVD's own U-Boot sets the same patch-done bit and then waits
	 * 800 ms before touching the PHYs again; we set it and carried straight
	 * on. Default 0 is today's behaviour deliberately -- this lands with the
	 * survey, and changing two things at once makes one boot say nothing. */
	if (phy_settle_ms > 0)
		msleep(phy_settle_ms);
out:
	/* ONE acquisition, ONE release: the patch-done sticky and the settle are
	 * part of the same sequence, and handing the window back between them
	 * would let a diagnostic read a PHY mid-bring-up. */
	mutex_unlock(&luna_gphy_lock);
	return rc;
}

/* Drive ONE Ethernet UNI's administrative state onto its ...
 * dev/MEASURED-luna_eth.c.md sec 30. */
static int luna_eth_uni_admin_set(void *cookie, unsigned int port, bool locked)
{
	struct luna_eth *ep = cookie;
	u32 force, mode, back;
	u16 bmcr, want;
	int rc;

	/* ★ A UNI IS A COPPER PORT.  The port index comes from the board's own
	 *   device tree, and the CPU, PON and SerDes ports are in the same index
	 *   space -- so a mis-declared panel would otherwise drive a PHY that is
	 *   not there and report the reserved port LOCKED. */
	if (!ep || port >= ep->c->sw_map->n_copper || port >= 32) {
		pr_err("luna-eth: UNI port %u is not one of this board's %u copper ports -- REFUSED, nothing is written\n",
		       port, ep ? ep->c->sw_map->n_copper : 0);
		return -EINVAL;
	}

	mutex_lock(&luna_gphy_lock);
	WRITE_ONCE(luna_uni_locked_ports,
		   locked ? (luna_uni_locked_ports | BIT(port))
			  : (luna_uni_locked_ports & ~BIT(port)));

	force = sw_rd(ep, SW_FORCE_ABLTY(ep, port));
	mode = sw_rd(ep, SW_ABLTY_FORCE(ep, port));
	if (locked) {
		sw_wr(ep, SW_FORCE_ABLTY(ep, port), force & ~ABLTY_LINK);
		sw_wr(ep, SW_ABLTY_FORCE(ep, port), mode | ABLTY_LINK);
	} else {
		sw_wr(ep, SW_ABLTY_FORCE(ep, port), mode & ~ABLTY_LINK);
	}
	back = sw_rd(ep, SW_ABLTY_FORCE(ep, port));
	force = sw_rd(ep, SW_FORCE_ABLTY(ep, port));
	/* BOTH words, because the claim is two gates: the MODE word says the
	 * LINK ability is forced, and the VALUE word says what it is forced TO.
	 * Checking only the first would report a lock that forces the link UP. */
	if (!(back & ABLTY_LINK) != !locked ||
	    (locked && (force & ABLTY_LINK))) {
		mutex_unlock(&luna_gphy_lock);
		dev_err(ep->dev,
			"UNI port %u: the MAC gate did not take -- FORCE_P_ABLTY %08x, ABLTY_FORCE_MODE %08x, so the administrative state is NOT applied\n",
			port, force, back);
		return -EIO;
	}

	rc = gphy_read_ocp__locked(ep, port, gphy_ocp_miim(ep, port, 0), &bmcr);
	if (rc) {
		mutex_unlock(&luna_gphy_lock);
		dev_err(ep->dev,
			"UNI port %u: its BMCR could not be read (%d); nothing is written back and the state is owed\n",
			port, rc);
		return rc;
	}
	/* ★ ONLY THE POWER BIT MOVES. This board's own phyPowerDown ...
	 * dev/MEASURED-luna_eth.c.md sec 31. */
	want = (u16)(locked ? (bmcr | BMCR_PDOWN) : (bmcr & ~BMCR_PDOWN));
	rc = gphy_write_ocp__locked(ep, port, gphy_ocp_miim(ep, port, 0), want);
	if (!rc)
		rc = gphy_read_ocp__locked(ep, port,
					   gphy_ocp_miim(ep, port, 0), &bmcr);
	if (!rc && (!(bmcr & BMCR_PDOWN) != !locked))
		rc = -EIO;
	mutex_unlock(&luna_gphy_lock);
	if (rc) {
		dev_err(ep->dev,
			"UNI port %u: the PHY did not reach %s (%d, BMCR %04x); the administrative state is owed\n",
			port, locked ? "power-down" : "power-up", rc, bmcr);
		return rc;
	}
	dev_info(ep->dev, "UNI port %u %s (BMCR %04x, ABLTY_FORCE_MODE %08x)\n",
		 port, locked ? "LOCKED" : "UNLOCKED", bmcr, back);
	return 0;
}

/* Release the external RTL8221B 2.5G PHY from reset (active-low). This alone does
 * not bring up its SerDes link (HiSGMII mode + analog patch is a separate, larger
 * sequence) but lets the PHY run; combined with the boot-loader-warmed SerDes it
 * gives the host's 2.5G port a chance to stay up. */
static void eth_rtl8221b_reset_release(struct luna_eth *ep)
{
	/* select GPIO function for the pin (bank-1 / pins 32..63 word). */
	sw_or(ep, SW_IO_GPIO_EN_HI, RTL8221B_RST_BIT);
	/* drive it as an output and pulse reset: assert (low) then release (high). */
	writel(readl(SOC_GPIO_B1_DIR) | RTL8221B_RST_BIT, SOC_GPIO_B1_DIR);
	writel(readl(SOC_GPIO_B1_DAT) & ~RTL8221B_RST_BIT, SOC_GPIO_B1_DAT);
	msleep(10);
	writel(readl(SOC_GPIO_B1_DAT) | RTL8221B_RST_BIT, SOC_GPIO_B1_DAT);
	msleep(10);
}

/* ---- per-port real-link + RX-MIB diagnostic ------------------------------- */
static u32 eth_mib_rx_pkts(struct luna_eth *ep, unsigned int p)
{
	return sw_rd(ep, SW_MIB_RX_UCAST(p)) + sw_rd(ep, SW_MIB_RX_MCAST(p)) +
	       sw_rd(ep, SW_MIB_RX_BCAST(p));
}

/* Genuine link (independent of the MAC force): copper = MDIO BMSR bit2,
 * SerDes (ports 6,7) = SDS_FIB_STATUS bit4. Returns -1 for ports with no
 * directly-readable PHY/SerDes (5 PON, 8 RGMII, 9 CPU, 10, 11). */
static int eth_port_real_link(struct luna_eth *ep, unsigned int p)
{
	if (p >= ep->c->sw_map->n_copper && p != 6 && p != 7)
		return -1;
	if (p < ep->c->sw_map->n_copper) {
		u16 bmsr;

		/* -1 is this function's COULD NOT ASK, which it already uses for
		 * a port it cannot reach.  A bus timeout is that, never "down". */
		if (gphy_read(ep, p, 1, &bmsr))
			return -1;
		return !!(bmsr & BMSR_LSTATUS);		/* BMSR */
	}
	if (!ep->c->sds_fib_status)		/* no SerDes on this chip */
		return -1;
	return !!(sw_rd(ep, SW_SDS_FIB_STATUS(ep, p - 6)) & SDS_LINK_OK);
}

static void eth_diag_dump(struct luna_eth *ep)
{
	unsigned int p;

	for (p = 0; p <= ep->c->last_port; p++) {
		int link = eth_port_real_link(ep, p);
		u32 ablty = sw_rd(ep, SW_P_ABLTY(ep, p));
		const char *ls = link < 0 ? "n/a  " : (link ? "UP   " : "down ");

		dev_info(ep->dev, "  port %2u: link=%s spdcode=%u stp=%u rxpkts=%u (ablty=%04x)\n",
			 p, ls, (ablty & 3) | (((ablty >> 12) & 3) << 2),
			 sw_rd(ep, SW_MSTI_CTRL(ep, p)) & STP_STATE_MASK,
			 eth_mib_rx_pkts(ep, p), ablty);
	}
	if (ep->c->sds_fib_status)
		dev_info(ep->dev, "  serdes6 fib=%08x serdes7 fib=%08x\n",
			 sw_rd(ep, SW_SDS_FIB_STATUS(ep, 0)),
			 sw_rd(ep, SW_SDS_FIB_STATUS(ep, 1)));
}

/* ★★ THE DIAGNOSTIC'S PHY READS RUN IN A WORK ITEM, NOT THE ...
 * dev/MEASURED-luna_eth.c.md sec 32. */
static void eth_diag_work(struct work_struct *w)
{
	struct luna_eth *ep = container_of(w, struct luna_eth, diag_work);

	dev_info(ep->dev, "link/rxpkts diag (%d left):\n", ep->diag_left);
	eth_diag_dump(ep);
}

static void eth_diag_timer(struct timer_list *t)
{
	struct luna_eth *ep = timer_container_of(ep, t, diag);

	schedule_work(&ep->diag_work);
	/* the RE-ARM stays here, so the work never arms the timer and a
	 * timer_delete_sync() followed by a cancel_work_sync() is enough */
	if (--ep->diag_left > 0 && diag_ms)
		mod_timer(&ep->diag, jiffies + msecs_to_jiffies(diag_ms));
}

/* station address -- dev/MEASURED-luna_eth.c.md sec 33. */
static void eth_get_hwaddr(struct luna_eth *ep, u8 *mac)
{
	luna_idr_get(ep->base, mac);
}

static void eth_set_hwaddr(struct luna_eth *ep, const u8 *mac)
{
	luna_idr_set(ep->base, mac);
}

/* ---- rings ---------------------------------------------------------------- */
/* Allocate a fresh RX skb, stream-map it, and arm the descriptor on it. */
/* The body is the FAMILY's (luna_eth_regs.h); the wrapper keeps the old name
 * and signature so every call site is untouched. */
static int eth_refill(struct luna_eth *ep, unsigned int idx)
{
	return luna_rx_refill(ep->ndev, ep->dev, ep->rx_ring, ep->rx_skb,
				 ep->rx_buf_dma, idx, RX_RING_SIZE, RX_BUF_SIZE);
}

static void eth_free_rings(struct luna_eth *ep)
{
	unsigned int i;

	for (i = 0; i < RX_RING_SIZE; i++) {
		if (ep->rx_skb[i]) {
			dma_unmap_single(ep->dev, ep->rx_buf_dma[i], RX_BUF_SIZE,
					 DMA_FROM_DEVICE);
			dev_kfree_skb_any(ep->rx_skb[i]);
			ep->rx_skb[i] = NULL;
		}
	}
	for (i = 0; i < TX_RING_SIZE; i++) {
		if (ep->tx_skb[i]) {		/* zero-copy frames still in flight */
			dma_unmap_single(ep->dev, ep->tx_buf_dma[i],
					 ep->tx_buf_len[i], DMA_TO_DEVICE);
			dev_kfree_skb_any(ep->tx_skb[i]);
			ep->tx_skb[i] = NULL;
		}
		kfree(ep->tx_buf[i]);
		ep->tx_buf[i] = NULL;
	}
	if (ep->rx_ring)
		dma_free_coherent(ep->dev, RX_RING_SIZE * sizeof(struct rx_desc),
				  ep->rx_ring, ep->rx_ring_dma);
	if (ep->tx_ring)
		dma_free_coherent(ep->dev, TX_RING_SIZE * sizeof(struct tx_desc),
				  ep->tx_ring, ep->tx_ring_dma);
	ep->rx_ring = NULL;
	ep->tx_ring = NULL;
}

static int eth_alloc_rings(struct luna_eth *ep)
{
	unsigned int i;

	ep->rx_ring = dma_alloc_coherent(ep->dev,
			RX_RING_SIZE * sizeof(struct rx_desc),
			&ep->rx_ring_dma, GFP_KERNEL);
	ep->tx_ring = dma_alloc_coherent(ep->dev,
			TX_RING_SIZE * sizeof(struct tx_desc),
			&ep->tx_ring_dma, GFP_KERNEL);
	if (!ep->rx_ring || !ep->tx_ring)
		return -ENOMEM;

	ep->rx_head = ep->tx_head = ep->tx_dirty = 0;
	for (i = 0; i < TX_RING_SIZE; i++) {
		ep->tx_buf[i] = kmalloc(RX_BUF_SIZE, GFP_KERNEL);
		if (!ep->tx_buf[i])
			return -ENOMEM;
		ep->tx_ring[i].opts1 = (i == TX_RING_SIZE - 1) ? D_EOR : 0;
		ep->tx_skb[i] = NULL;
	}
	for (i = 0; i < RX_RING_SIZE; i++)
		if (eth_refill(ep, i))
			return -ENOMEM;
	return 0;
}

/* ---- switch open-L2 bring-up (ordered; see file header) ------------------- */
static int eth_switch_init(struct luna_eth *ep)
{
	unsigned int p;

	/* 1. enable the switch IP block (SoC control, not in SWCORE). */
	writel(readl(SOC_SW_ENABLE) | SW_EN_BIT | SW_PBO_BIT, SOC_SW_ENABLE);

	/* 1b. SoC handshake, where the chip declares one: the switch block asks
	 *     to be told the SoC is ready before it will accept its patches. The
	 *     chip's own boot loader spins on the same bit; a chip whose table
	 *     leaves it 0 simply has no such handshake. */
	if (ep->c->sys_status) {
		void __iomem *ss = (void __iomem *)(uintptr_t)ep->c->sys_status;
		int i;

		for (i = 0; i < 1000 && !(readl(ss) & BIT(1)); i++)
			udelay(100);
		if (!(readl(ss) & BIT(1)))
			dev_warn(ep->dev,
				 "switch: SoC never reported ready-for-patch (SYS_STATUS %08x after 100 ms) -- continuing, but the PHY patches may not stick\n",
				 readl(ss));
		writel(readl(ss) | BIT(0), ss);		/* soc_init_rdy */
	}

	/* 2. re-assert the SerDes egress line mode (boot-ROM leaves ...
	 * dev/MEASURED-luna_eth.c.md sec 60. */
	if (ep->c->serdes_linemode)
		sw_wr(ep, ep->c->serdes_linemode, 0x44);

	/* 3. bring the physical PHYs up so a host on a jack actually ...
	 * dev/MEASURED-luna_eth.c.md sec 34. */
	{
		int rc = eth_phy_ctrl_apply(ep);

		if (rc)
			return rc;
	}
	eth_rxfifo_release(ep);
	if (copper_phy) {
		int rc = eth_copper_phy_up(ep);

		if (rc)
			return rc;	/* the PHYs ARE the LAN: do not claim them up */
	}
	/* AFTER the power-up, so the survey reads the PHYs in the state the rest
	 * of the bring-up will actually see -- not the pre-power-up one, which
	 * would answer a question nobody asked. */
	if (phy_survey)
		eth_phy_survey(ep);
	/* The external 2.5G PHY hangs off the SerDes uplink, so it exists only on
	 * a chip that HAS one. Asking for it elsewhere would drive a GPIO chosen
	 * for a different board. */
	if (rtl8221b_phy && ep->c->sds_fib_status)
		eth_rtl8221b_reset_release(ep);

	/* 4. open the L2 forwarding plane. ★ THE PACKED ARITHMETIC IS ...
	 * dev/MEASURED-luna_eth.c.md sec 35. */
	for (p = 0; p <= ep->c->sw_map->cpu_port; p++) {
		u32 reg = sw_packed_off(ep->c->sw_map->lut_unkn_sa, p,
					SW_DA_ACT_BITS);

		/* unknown-source-MAC action 0 = learn + forward */
		sw_wr(ep, reg, sw_packed_ins(sw_rd(ep, reg), p,
					     SW_DA_ACT_BITS, 0));
	}
	/* The PON port is not a flood destination (2026-08-27): ...
	 * dev/MEASURED-luna_eth.c.md sec 36. */
	{
		u32 flood = ep->c->sw_map->port_mask;

		if (ep->c->force_pon_ablty)
			flood &= ~BIT(ep->c->sw_map->pon_port);
		sw_or(ep, ep->c->sw_map->bc_flood, flood);
		sw_or(ep, ep->c->sw_map->unkn_mc_flood, flood);
		sw_or(ep, ep->c->sw_map->unkn_uc_flood, flood);
		ep->lan_flood_mask = flood & ~BIT(ep->c->sw_map->cpu_port);
	}
	/* 0 is the RESET value and is written only because it is: forcing every
	 * bit moved no witness on either Luna die (luna_eth_regs.h @src_permit). */
	sw_wr(ep, ep->c->sw_map->src_permit, 0x00000000);

	/* 4a2. per-port unknown-DA lookup-miss action = FORWARD(0). We only set the
	 *      unknown-SOURCE action above; the unknown-DESTINATION action must also
	 *      forward, else post-ARP unicast / IPv6-ND to a not-yet-learned MAC is
	 *      dropped instead of flooded. Clear the 2-bit field for ports 0..10. */
	sw_wr(ep, ep->c->sw_map->lut_unkn_uc_da,  sw_rd(ep, ep->c->sw_map->lut_unkn_uc_da)  & ~DA_ACT_PORTS);
	sw_wr(ep, ep->c->sw_map->unkn_l2_mc,  sw_rd(ep, ep->c->sw_map->unkn_l2_mc)  & ~DA_ACT_PORTS);
	sw_wr(ep, ep->c->sw_map->unkn_ip4_mc, sw_rd(ep, ep->c->sw_map->unkn_ip4_mc) & ~DA_ACT_PORTS);
	sw_wr(ep, ep->c->sw_map->unkn_ip6_mc, sw_rd(ep, ep->c->sw_map->unkn_ip6_mc) & ~DA_ACT_PORTS);

	/* 4a3. VLAN must not gate CPU<->LAN egress. The boot loader can leave VLAN
	 *      filtering ON with the CPU port outside the member set, which silently
	 *      drops the CPU's reply to the host (RX fixed, but ping return blocked).
	 *      Report the live state, then disable filtering for flat-L2 forwarding. */
	{
		u32 vc = sw_rd(ep, SW_VLAN_CTRL);

		sw_wr(ep, SW_VLAN_CTRL, vc & ~VLAN_FILTERING);
		dev_info(ep->dev, "vlan_ctrl %08x (filtering %s) -> %08x\n",
			 vc, (vc & VLAN_FILTERING) ? "ON" : "off",
			 sw_rd(ep, SW_VLAN_CTRL));
	}

	/* 4b. set every port's spanning-tree state to FORWARDING. The ...
	 * dev/MEASURED-luna_eth.c.md sec 61. */
	for (p = 0; p <= ep->c->last_port; p++) {
		u32 v = sw_rd(ep, SW_MSTI_CTRL(ep, p));

		sw_wr(ep, SW_MSTI_CTRL(ep, p),
		      (v & ~STP_STATE_MASK) | STP_FORWARDING);
	}

	/* 4c. (optional) drop the CPU port from its own egress flood ...
	 * dev/MEASURED-luna_eth.c.md sec 62. */
	if (cpu_no_loopback) {
		unsigned int cp = ep->c->sw_map->cpu_port;
		unsigned int per = ep->c->piso_per_word;
		u32 reg = ep->c->sw_map->piso_base + (cp / per) * 4;
		unsigned int shift = (cp % per) * ep->c->piso_bits;
		u32 fld = ep->c->piso_all & ~BIT(cp);

		sw_wr(ep, reg,
		      (sw_rd(ep, reg) & ~(ep->c->piso_all << shift)) |
		      (fld << shift));
	}

	/* 5. force every port's MAC link up (no PHY autoneg). The CPU ...
	 * dev/MEASURED-luna_eth.c.md sec 63. */
	for (p = 0; p <= ep->c->last_port; p++) {
		if (p == ep->c->sw_map->cpu_port) {
			sw_or(ep, SW_FORCE_ABLTY(ep, p), BIT(4));
			sw_wr(ep, SW_ABLTY_FORCE(ep, p), ABLTY_CPU_FORCE);
		} else {
			/* Force NOTHING on a UTP port. This used to write ...
			 * dev/MEASURED-luna_eth.c.md sec 37. */
			continue;
		}
	}
	if (ep->c->force_ablty_x) {
		sw_wr(ep, ep->c->force_ablty_x, ABLTY_1G_FULL_LINK);
		sw_wr(ep, ep->c->ablty_force_x, ABLTY_FORCE_ALL);
	}

	/* 5b. The PON port is not a UTP port and the rule above does ...
	 * dev/MEASURED-luna_eth.c.md sec 38. */
	if (ep->c->force_pon_ablty) {
		unsigned int pp = ep->c->sw_map->pon_port;
		u32 was = sw_rd(ep, SW_FORCE_ABLTY(ep, pp));

		sw_wr(ep, SW_FORCE_ABLTY(ep, pp), ABLTY_1G_FULL_LINK);
		sw_wr(ep, SW_ABLTY_FORCE(ep, pp), ABLTY_CPU_FORCE);
		dev_info(ep->dev,
			 "switch: PON port %u forced up like the CPU port (stock's own pair): FORCE_P_ABLTY %08x -> %08x, ABLTY_FORCE_MODE -> %08x, P_ABLTY reads %08x\n",
			 pp, was, sw_rd(ep, SW_FORCE_ABLTY(ep, pp)),
			 sw_rd(ep, SW_ABLTY_FORCE(ep, pp)),
			 sw_rd(ep, SW_P_ABLTY(ep, pp)));
	}

	/* 6. CPU-tag engine. The switch frames RX to the CPU with the 0x8899 tag
	 *    by default, so the CPU port is tag-aware; keep it on unless asked. */
	if (sw_cpu_tag) {
		sw_or(ep, ep->c->cpu_tag_insert, BIT(0));
		sw_or(ep, ep->c->cpu_tag_aware, BIT(0));
	}

	dev_info(ep->dev,
		 "switch: %s open-L2 up (cpu-port %u of 0..%u, src-permit=%08x, cpu-ablty=%04x)\n",
		 ep->c->name, ep->c->sw_map->cpu_port, ep->c->last_port,
		 sw_rd(ep, ep->c->sw_map->src_permit),
		 sw_rd(ep, SW_P_ABLTY(ep, ep->c->sw_map->cpu_port)));
	/* Baseline real-link snapshot; the periodic diag (armed at open) then shows
	 * which port's genuine link comes up + rxpkts climb under host traffic. */
	eth_diag_dump(ep);
	return 0;
}

/* ---- MAC engine ----------------------------------------------------------- */
/* The body is the family's (luna_eth_regs.h): both drivers had it, identical
 * but for the struct that reached `->base`. */
static void eth_hw_stop(struct luna_eth *ep)
{
	luna_eth_hw_stop(ep->base);
}

/* GMAC0 IP-block power-cycle: the multi-ring fetch engine only latches its ring
 * state across this reset edge, so it must run before the descriptor program. */
/* The body and the hang warning are the FAMILY's (luna_eth_regs.h). */
static void eth_ipsel_cycle(void)
{
	luna_ipsel_cycle_gmac0();
}

static void eth_hw_program(struct luna_eth *ep)
{
	u32 desnum;

	iowrite8(CMD_RXCHK | CMD_RXJUMBO, ep->base + R_CMD);
	ep_wr(ep, R_TCR, 0x00000C01);		/* IFG=3, TX enabled via IO_CMD	*/
	ep_wr(ep, R_RCR, 0x0000000E);		/* accept broadcast + matching	*/
	ep_wr(ep, R_CONFIG, 0x21000000);
	/* Enable the CPU-tag engine so the MAC strips the in-band 8-byte switch
	 * tag on RX (CTEN_RX); TX stays plain (per-descriptor opts2.cputag=0 =>
	 * the switch forwards by L2 lookup). The IP-block reset above clears this,
	 * so it must be re-asserted here. */
	ep_wr(ep, R_CPUTAGCR, CPUTAGCR_INIT);
	/* the OMCC SID, from ep->omci_sid -- 0 until the OLT assigns one */
	ep_wr(ep, R_CPUTAG1CR, CPUTAG1CR_SID(ep->omci_sid));

	/* ring pointers (writable only while the engine is stopped). */
	ep_wr(ep, R_TxFDP0, ep->tx_ring_dma | DMA_BUS_WINDOW);
	iowrite16(0, ep->base + R_TxCDO0);
	ep_wr(ep, R_RxFDP0, ep->rx_ring_dma | DMA_BUS_WINDOW);
	/* Was this file's own spelling of the family packing -- the THIRD copy. */
	desnum = luna_gmac_rxdesnum_pack(RX_RING_SIZE, TH_ON_VAL, TH_OFF_VAL);
	ep_wr(ep, R_RxDesNum, desnum);
	/* 32-BIT, like the vendor and like rtl9602c_eth.c -- NOT ...
	 * dev/MEASURED-luna_eth.c.md sec 39. */
	ep_wr(ep, R_RxCDO0, luna_gmac_rxcdo_pack(RX_RING_SIZE));
	/* route every RX class to ring 0. */
	{
		unsigned int k;

		for (k = 0; k < 7; k++)
			ep_wr(ep, R_RRING_ROUTE + k * 4, 0);
	}

	/* MSR(0x58) top byte -- see the msr_top param note.  0xf0 is stock's
	 * value and it WEDGES our datapath. */
	ep_wr(ep, R_MSR,
	      (ep_rd(ep, R_MSR) & 0x00ffffff) | ((msr_top & 0xffu) << 24));
	eth_set_hwaddr(ep, ep->ndev->dev_addr);
	ep_wr(ep, R_MAR0, 0xffffffff);
	ep_wr(ep, R_MAR4, 0xffffffff);

	/* enable edge: IO_CMD1 first, then IO_CMD (latches the fetch engine). */
	ep_wr(ep, R_IO_CMD1, IO_CMD1_ENABLE);
	ep_wr(ep, R_IO_CMD, IO_CMD_ENABLE);

	iowrite16(0xffff, ep->base + R_ISR);
	ep_wr(ep, R_ISR1, 0xffffffff);
	iowrite16(IMR_RX_BITS, ep->base + R_IMR);
	ep_wr(ep, R_IMR0, IMR0_TX_BITS);
}

/* RX / TX datapath -- dev/MEASURED-luna_eth.c.md sec 40. */

static void eth_tx_reclaim(struct luna_eth *ep);	/* defined with the TX path */

/* How many ring slots LAN transmit keeps in hand so a sparse, ...
 * dev/MEASURED-luna_eth.c.md sec 64. */
#define LUNA_OMCI_RESV	2

/* Transmit one OMCI PDU upstream on the OMCC. The frame is ...
 * dev/MEASURED-luna_eth.c.md sec 41. */
static int luna_eth_omci_xmit(struct luna_eth *ep, const u8 *omci,
			      unsigned int len)
{
	unsigned long flags;
	unsigned int i;
	dma_addr_t da;
	void *buf;
	u32 opts1;

	if (!ep || !len || len > RX_BUF_SIZE)
		return -EINVAL;

	spin_lock_irqsave(&ep->tx_lock, flags);
	if (ep->closing || !ep->tx_ring) {
		spin_unlock_irqrestore(&ep->tx_lock, flags);
		return -ENODEV;
	}
	eth_tx_reclaim(ep);
	/* reserve 0: this IS the producer the LAN path holds slots back for. */
	if (luna_gmac_tx_ring_full(ep->tx_head, ep->tx_dirty, TX_RING_SIZE, 0)) {
		spin_unlock_irqrestore(&ep->tx_lock, flags);
		ep->dbg_omci_tx_drop++;
		return -EBUSY;
	}
	i = tx_slot(ep->tx_head);
	buf = ep->tx_buf[i];
	if (!buf) {
		spin_unlock_irqrestore(&ep->tx_lock, flags);
		ep->dbg_omci_tx_drop++;
		return -ENODEV;
	}
	memcpy(buf, omci, len);

	da = dma_map_single(ep->dev, buf, len, DMA_TO_DEVICE);
	if (dma_mapping_error(ep->dev, da)) {
		spin_unlock_irqrestore(&ep->tx_lock, flags);
		ep->dbg_omci_tx_drop++;
		return -ENOMEM;
	}
	ep->tx_buf_dma[i] = da;
	ep->tx_buf_len[i] = len;
	ep->tx_ring[i].addr = da | DMA_BUS_WINDOW;
	ep->tx_ring[i].opts2 = luna_gmac_cputag_txd_word2(ep->c->sw_map->pon_port);
	ep->tx_ring[i].opts3 = luna_gmac_cputag_txd_word3(ep->omci_sid);
	ep->tx_ring[i].opts4 = 0;
	/* D_IPCS is here and not on the LAN path, and both halves are ...
	 * dev/MEASURED-luna_eth.c.md sec 65. */
	opts1 = luna_gmac_txd_word0(D_FS | D_LS | D_TXCRC | D_IPCS, len,
				    TXD_LEN_MASK,
				    luna_gmac_slot_is_eor(i, TX_RING_SIZE),
				    D_EOR);
	wmb();				/* descriptor body before ownership */
	ep->tx_ring[i].opts1 = D_OWN | opts1;
	wmb();
	ep->tx_head++;
	ep_wr(ep, R_IO_CMD, ep_rd(ep, R_IO_CMD) | BIT(0));	/* kick ring 0 */
	spin_unlock_irqrestore(&ep->tx_lock, flags);

	ep->dbg_omci_tx++;
	/* THE CAP IS ON THE PRINT, not on the transmit: the running totals are in
	 * /proc/gpon, which is where a count belongs. A log line is a sample. */
	if (ep->dbg_omci_tx <= 4)
		netdev_info(ep->ndev,
			    "US OMCI #%u: %u B sid=%u opts2=%08x opts3=%08x\n",
			    ep->dbg_omci_tx, len, ep->omci_sid,
			    luna_gmac_cputag_txd_word2(ep->c->sw_map->pon_port),
			    luna_gmac_cputag_txd_word3(ep->omci_sid));
	return 0;
}

/* RX copies the original baseline prefix and length to the shared timer owner. */
static void luna_eth_omci_input(struct luna_eth *ep, const u8 *msg,
				unsigned int len)
{
	int rc = luna_omci_enqueue(ep, msg, len);

	if (rc)
		ep->dbg_omci_unhandled++;
}

static int eth_rx(struct luna_eth *ep, int budget)
{
	struct net_device *ndev = ep->ndev;
	int done = 0;

	while (done < budget) {
		unsigned int i = ep->rx_head;
		u32 opts1 = ep->rx_ring[i].opts1;
		struct sk_buff *skb, *fresh;
		dma_addr_t fresh_dma;
		u32 len;

		if (opts1 & D_OWN)		/* still HW-owned */
			break;

		/* Secure the REPLACEMENT before consuming the frame: every arm
		 * dev/MEASURED-luna_eth.c.md sec 42. */
		fresh = luna_rx_alloc(ndev, ep->dev, RX_BUF_SIZE, &fresh_dma);
		if (!fresh) {
			ndev->stats.rx_dropped++;
			luna_rx_rearm(ep->rx_ring, i, RX_RING_SIZE, RX_BUF_SIZE);
			ep->rx_head = (i + 1) % RX_RING_SIZE;
			done++;
			continue;
		}

		len = opts1 & RXD_LEN_MASK;
		skb = ep->rx_skb[i];
		dma_unmap_single(ep->dev, ep->rx_buf_dma[i], RX_BUF_SIZE,
				 DMA_FROM_DEVICE);

		if (rx_dump > 0 && ep->rx_dumped < rx_dump && len) {
			ep->rx_dumped++;
			/* The descriptor, beside the bytes. opts3[19:16] is the ...
			 * dev/MEASURED-luna_eth.c.md sec 43. */
			dev_info(ep->dev,
				 "rx0 desc: opts1=%08x opts2=%08x opts3=%08x len=%u (src_port=%u reason=%u)\n",
				 opts1, ep->rx_ring[i].opts2, ep->rx_ring[i].opts3,
				 len, luna_gmac_rx_src_port(ep->c->sw_map->rx_layout, ep->rx_ring[i].opts3),
				 (ep->rx_ring[i].opts2 >> 21) & 0xff);
			print_hex_dump(KERN_INFO, "rx0: ", DUMP_PREFIX_OFFSET,
				       16, 1, skb->data, min_t(u32, len, 32), false);
		}

		/* One line per DISTINCT cpu-tag reason, ever.  See the field. */
		{
			unsigned int rsn = (ep->rx_ring[i].opts2 >> 21) & 0xff;

			if (!(ep->rx_reason_seen[rsn >> 5] & BIT(rsn & 31))) {
				ep->rx_reason_seen[rsn >> 5] |= BIT(rsn & 31);
				netdev_info(ndev,
					    "rx cpu-tag reason %u seen (opts2=%08x opts3=%08x len=%u); this chip traps DS OMCI as %u\n",
					    rsn, ep->rx_ring[i].opts2,
					    ep->rx_ring[i].opts3, len,
					    ep->c->sw_map->omci_cpu_reason);
			}
		}

		/* One line per DISTINCT ingress port, ever.  See the field: this is
		 * what tells a WAN that counts nothing apart from an OLT that is
		 * sending nothing. */
		{
			unsigned int sp = luna_gmac_rx_src_port(ep->c->sw_map->rx_layout, ep->rx_ring[i].opts3);

			if (!(ep->rx_src_port_seen & BIT(sp))) {
				ep->rx_src_port_seen |= BIT(sp);
				netdev_info(ndev,
					    "rx ingress port %u seen (opts3=%08x len=%u); this chip's fibre port is %u, CPU port %u\n",
					    sp, ep->rx_ring[i].opts3, len,
					    ep->c->sw_map->pon_port,
					    ep->c->sw_map->cpu_port);
			}
		}

		/* ... and one per distinct PON stream id, fibre-ingress only: the
		 * field is extspa on a frame that came from anywhere else. */
		if (luna_gmac_rx_is_wan(ep->c->sw_map->rx_layout,
					ep->rx_ring[i].opts3,
					ep->c->sw_map->pon_port)) {
			unsigned int sid = luna_gmac_rx_pon_sid(ep->rx_ring[i].opts3);

			if (!(ep->rx_pon_sid_seen[sid >> 5] & BIT(sid & 31))) {
				ep->rx_pon_sid_seen[sid >> 5] |= BIT(sid & 31);
				netdev_info(ndev,
					    "rx PON stream %u seen (opts3=%08x len=%u); OMCC is %u, WAN data is %u, multicast is %u\n",
					    sid, ep->rx_ring[i].opts3, len,
					    ep->omci_sid, GPON_DATA_FLOW,
					    GPON_MCAST_FLOW);
			}
		}

		/* DS OMCI FIRST, before the Ethernet verdicts: an OMCI PDU has no DA, no
		 * SA and no ethertype, so eth_type_trans() would invent a protocol from
		 * bytes 12-13 of a G.988 header and the bridge would drop it silently. */
		if (luna_gmac_rx_is_ds_omci(ep->c->sw_map->rx_layout,
					    ep->omci_trap_on,
					    ep->rx_ring[i].opts2,
					    ep->rx_ring[i].opts3, len,
					    ep->c->sw_map->omci_cpu_reason,
					    (unsigned int)rx_prefix,
					    RX_BUF_SIZE)) {
			ep->dbg_omci_rx++;
			ep->dbg_omci_rxlen = len - (u32)rx_prefix;
			luna_eth_omci_input(ep, skb->data + rx_prefix,
					    len - (u32)rx_prefix);
			dev_kfree_skb_any(skb);
		} else if (luna_gmac_rx_frame_bad(opts1, RXD_CRCERR | RXD_RCDF, len,
					   (u32)rx_prefix + ETH_HLEN,
					   RX_BUF_SIZE)) {
			ndev->stats.rx_errors++;
			dev_kfree_skb_any(skb);
		} else {
			/* The CPU port frames a packet as:
			 *   [front prefix][DA][SA][switch tag][ethertype][payload]
			 * Strip the fixed front prefix, then excise the 8-byte 0x8899
			 * switch tag (if present) by sliding DA+SA over it. */
			skb_put(skb, len);
			if (rx_prefix)
				skb_pull(skb, rx_prefix);
			/* Tag classifier hoisted to flowcore; the excision
			 * memmove/skb_pull below stay -- buffer surgery, not
			 * a decision. */
			if (luna_gmac_rx_cpu_tag_present(skb->data, skb->len,
							 RTL_CPU_TAG_LEN)) {
				memmove(skb->data + RTL_CPU_TAG_LEN, skb->data,
					2 * ETH_ALEN);
				skb_pull(skb, RTL_CPU_TAG_LEN);
			}
			/* WAN demux FIRST: a frame that ingressed on the fibre is ...
			 * dev/MEASURED-luna_eth.c.md sec 44. */
			struct net_device *rdev = ndev;

			if (ep->wan_ndev &&
			    luna_gmac_rx_is_wan(ep->c->sw_map->rx_layout,
						ep->rx_ring[i].opts3,
						ep->c->sw_map->pon_port))
				rdev = ep->wan_ndev;

			/* Drop our own egress flooded back to the CPU port (source
			 * MAC == this netdev's own) so the bridge does not log
			 * "received packet ... with own address as source". */
			if (skb->len >= 2 * ETH_ALEN &&
			    ether_addr_equal(skb->data + ETH_ALEN, rdev->dev_addr)) {
				dev_kfree_skb_any(skb);
			} else if (!netif_running(rdev)) {
				rdev->stats.rx_dropped++;
				dev_kfree_skb_any(skb);
			} else {
				/* NAPI poll context: use the receive path, not netif_rx. */
				skb->protocol = eth_type_trans(skb, rdev);
				rdev->stats.rx_packets++;
				rdev->stats.rx_bytes += len;
				napi_gro_receive(&ep->napi, skb);
			}
		}

		/* re-arm with the skb secured above */
		luna_rx_arm(ep->rx_ring, ep->rx_skb, ep->rx_buf_dma, i,
			    RX_RING_SIZE, RX_BUF_SIZE, fresh, fresh_dma);
		ep->rx_head = (i + 1) % RX_RING_SIZE;
		done++;
	}
	return done;
}

static void eth_tx_reclaim(struct luna_eth *ep)
{
	/* The skb was already freed at xmit (its bytes were copied into tx_buf),
	 * so reclaim just unmaps the copy buffer and releases the ring slot once
	 * the DMA engine has handed the descriptor back (OWN cleared). */
	while (ep->tx_dirty != ep->tx_head) {
		unsigned int i = tx_slot(ep->tx_dirty);

		if (ep->tx_ring[i].opts1 & D_OWN)	/* not transmitted yet */
			break;
		dma_unmap_single(ep->dev, ep->tx_buf_dma[i], ep->tx_buf_len[i],
				 DMA_TO_DEVICE);
		if (ep->tx_skb[i]) {		/* a zero-copy frame is done now */
			dev_consume_skb_any(ep->tx_skb[i]);
			ep->tx_skb[i] = NULL;
		}
		ep->tx_dirty++;
	}
	/* The same reserve as eth_xmit(), and not cosmetic: waking the queue at a
	 * threshold the transmit path still refuses spins on NETDEV_TX_BUSY. */
	if (netif_queue_stopped(ep->ndev) &&
	    !luna_gmac_tx_ring_full(ep->tx_head, ep->tx_dirty,
				     TX_RING_SIZE, LUNA_OMCI_RESV))
		netif_wake_queue(ep->ndev);
}

static int eth_napi_poll(struct napi_struct *napi, int budget)
{
	struct luna_eth *ep = container_of(napi, struct luna_eth, napi);
	unsigned long flags;
	int work;

	work = eth_rx(ep, budget);
	spin_lock_irqsave(&ep->tx_lock, flags);
	eth_tx_reclaim(ep);
	spin_unlock_irqrestore(&ep->tx_lock, flags);

	if (work < budget && napi_complete_done(napi, work)) {
		/* W1C any status latched while masked, then re-unmask. */
		iowrite16(ioread16(ep->base + R_ISR), ep->base + R_ISR);
		ep_wr(ep, R_ISR1, ep_rd(ep, R_ISR1));
		iowrite16(IMR_RX_BITS, ep->base + R_IMR);
		ep_wr(ep, R_IMR0, IMR0_TX_BITS);
		if (luna_rx_ready(ep->rx_ring, ep->rx_head))	/* its bit went to the ack */
			napi_schedule(napi);
	}
	return work;
}

static irqreturn_t eth_irq(int irq, void *data)
{
	struct luna_eth *ep = netdev_priv((struct net_device *)data);
	u16 isr = ioread16(ep->base + R_ISR);

	if (!isr && !ep_rd(ep, R_ISR1))
		return IRQ_NONE;
	/* mask and let NAPI drain + re-unmask. */
	iowrite16(0, ep->base + R_IMR);
	ep_wr(ep, R_IMR0, 0);
	napi_schedule(&ep->napi);
	return IRQ_HANDLED;
}

/* Backstop drain: catches a missed/unrouted IRQ so the datapath always makes
 * progress during bring-up (ping does not need IRQ latency). */
static void eth_backstop(struct timer_list *t)
{
	struct luna_eth *ep = timer_container_of(ep, t, backstop);

	napi_schedule(&ep->napi);
	mod_timer(&ep->backstop, jiffies + msecs_to_jiffies(backstop_ms));
}

/* Transmit one frame on shared ring 0, for whichever netdev ...
 * dev/MEASURED-luna_eth.c.md sec 45. */
static netdev_tx_t eth_tx_frame(struct luna_eth *ep, struct net_device *ndev,
				struct sk_buff *skb, u32 opts2, u32 opts3,
				bool backpressure)
{
	unsigned long flags;
	unsigned int i, len = skb->len;
	dma_addr_t da;
	void *buf;
	u32 opts1;
	bool zero_copy;

	if (len > RX_BUF_SIZE) {		/* must fit a copy slot */
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	spin_lock_irqsave(&ep->tx_lock, flags);
	/* THE RINGS BELONG TO eth0 AND gpon0 OUTLIVES ITS ndo_stop, ...
	 * dev/MEASURED-luna_eth.c.md sec 46. */
	if (ep->closing || !ep->tx_ring ||
	    (!backpressure && !luna_gpon_data_ready())) {
		spin_unlock_irqrestore(&ep->tx_lock, flags);
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	/* Ring-space test hoisted to luna_gmac_tx_ring_full() ...
	 * dev/MEASURED-luna_eth.c.md sec 66. */
	if (luna_gmac_tx_ring_full(ep->tx_head, ep->tx_dirty, TX_RING_SIZE,
				   LUNA_OMCI_RESV)) {
		spin_unlock_irqrestore(&ep->tx_lock, flags);
		if (backpressure) {
			netif_stop_queue(ndev);
			return NETDEV_TX_BUSY;
		}
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	i = tx_slot(ep->tx_head);
	buf = ep->tx_buf[i];
	if (!buf) {		/* a partly-failed eth_alloc_rings leaves holes */
		spin_unlock_irqrestore(&ep->tx_lock, flags);
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	/* ZERO-COPY for a linear frame of at least ETH_ZLEN: the GMAC DMAs from
	 * skb->data directly, as rtl9602c_eth.c does on the same family engine, and
	 * the skb is held until reclaim. MEASURED 2026-09-22 on G24W: the per-packet
	 * copy capped CPU-path v6 at ~21 k pps on one core. Non-linear or runt
	 * frames keep the copy path (sec 47: a flat copy of a fragmented skb). */
	zero_copy = luna_gmac_tx_zero_copy(skb_is_nonlinear(skb), len, ETH_ZLEN);
	if (zero_copy) {
		buf = skb->data;
	} else if (skb_copy_bits(skb, 0, buf, len)) {
		spin_unlock_irqrestore(&ep->tx_lock, flags);
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	if (len < ETH_ZLEN) {		/* zero-pad runt frames (e.g. a 42-byte ARP reply)
					 * in the copy buffer and extend the DMA length;
					 * skb_padto only guarantees tailroom, it does NOT
					 * grow skb->len, so we pad here after the copy. */
		memset((u8 *)buf + len, 0, ETH_ZLEN - len);
		len = ETH_ZLEN;
	}

	if (ep->tx_dumped < tx_dump) {
		ep->tx_dumped++;
		print_hex_dump(KERN_INFO, "tx0: ", DUMP_PREFIX_OFFSET, 16, 1,
			       buf, min_t(unsigned int, len, 48), false);
	}

	da = dma_map_single(ep->dev, buf, len, DMA_TO_DEVICE);
	if (dma_mapping_error(ep->dev, da)) {
		spin_unlock_irqrestore(&ep->tx_lock, flags);
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	ep->tx_buf_dma[i] = da;
	ep->tx_buf_len[i] = len;
	ep->tx_skb[i] = zero_copy ? skb : NULL;
	ep->tx_ring[i].addr = da | DMA_BUS_WINDOW;
	ep->tx_ring[i].opts2 = opts2;
	ep->tx_ring[i].opts3 = opts3;
	ep->tx_ring[i].opts4 = 0;
	/* Body word composed by luna_gmac_txd_word0() (flowcore); the flag set, the
	 * length mask and D_EOR are passed, never re-spelled there. D_OWN is NOT
	 * part of it: ownership is publish-order, OR'd on after the barrier. */
	opts1 = luna_gmac_txd_word0(D_FS | D_LS | D_TXCRC, len, TXD_LEN_MASK,
				    luna_gmac_slot_is_eor(i, TX_RING_SIZE),
				    D_EOR);
	wmb();				/* descriptor body before ownership */
	ep->tx_ring[i].opts1 = D_OWN | opts1;
	wmb();

	ep->tx_head++;
	ep_wr(ep, R_IO_CMD, ep_rd(ep, R_IO_CMD) | BIT(0));	/* kick ring 0 */
	spin_unlock_irqrestore(&ep->tx_lock, flags);

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += len;
	if (!zero_copy)
		dev_consume_skb_any(skb);	/* bytes copied; release immediately */
	return NETDEV_TX_OK;
}


/* eth0: a plain frame the switch forwards by L2 destination -- except a
 * multicast or broadcast one, which the switch would flood back at us.  See
 * @lan_flood_direct. */
static netdev_tx_t eth_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct luna_eth *ep = netdev_priv(ndev);
	u32 opts2 = 0;

	if (lan_flood_direct && ep->lan_flood_mask &&
	    skb_headlen(skb) >= ETH_ALEN && is_multicast_ether_addr(skb->data))
		opts2 = luna_gmac_cputag_txd_pmask(ep->lan_flood_mask);

	return eth_tx_frame(ep, ndev, skb, opts2, 0, true);
}
static void eth_set_rx_mode(struct net_device *ndev)
{
	struct luna_eth *ep = netdev_priv(ndev);

	luna_eth_set_promisc(ep->base,
				 !!(ndev->flags & (IFF_PROMISC | IFF_ALLMULTI)));
}

/* Defined with the WAN netdev below; eth0's address setter needs it because the
 * WAN identity is derived from eth0's and must move when eth0's does. */
static void luna_eth_wan_hwaddr(struct luna_eth *ep, u8 out[ETH_ALEN]);

static int eth_set_mac_address(struct net_device *ndev, void *addr)
{
	struct luna_eth *ep = netdev_priv(ndev);
	int ret = eth_mac_addr(ndev, addr);

	if (ret)
		return ret;
	eth_set_hwaddr(ep, ndev->dev_addr);
	/* The WAN identity is DERIVED from this one, so it moves with it: the
	 * board MAC is provisioned onto eth0 well after probe, and a gpon0 left
	 * holding the placeholder it was created with would present the OLT an
	 * address no longer related to the board's. */
	if (ep->wan_ndev) {
		u8 wmac[ETH_ALEN];

		luna_eth_wan_hwaddr(ep, wmac);
		eth_hw_addr_set(ep->wan_ndev, wmac);
		luna_omci_set_wan_mac(wmac);
	}
	return 0;
}

/* ---- open / stop ---------------------------------------------------------- */
static int eth_hw_start(struct luna_eth *ep)
{
	struct net_device *ndev = ep->ndev;
	int ret;

	ret = eth_alloc_rings(ep);
	if (ret) {
		eth_free_rings(ep);
		return ret;
	}
	luna_gpon_nic_reset_begin();
	ep->closing = false;

	eth_hw_stop(ep);
	eth_ipsel_cycle();
	ret = eth_switch_init(ep);	/* program forwarding before the DMA starts */
	if (ret)
		goto fail;
	eth_hw_program(ep);

	if (ep->irq > 0) {
		ret = request_irq(ep->irq, eth_irq, 0, ndev->name, ndev);
		if (ret) {
			netdev_warn(ndev, "IRQ %d request failed (%d); poll-only\n",
				    ep->irq, ret);
			ep->irq = -1;
		}
	}
	napi_enable(&ep->napi);
	timer_setup(&ep->backstop, eth_backstop, 0);
	mod_timer(&ep->backstop, jiffies + msecs_to_jiffies(backstop_ms));

	/* periodic real-link/rxpkts diagnostic (bring-up: locate the host's jack). */
	ep->diag_left = diag_count;
	timer_setup(&ep->diag, eth_diag_timer, 0);
	INIT_WORK(&ep->diag_work, eth_diag_work);
	if (diag_ms && diag_count > 0)
		mod_timer(&ep->diag, jiffies + msecs_to_jiffies(diag_ms));

	/* The resurvey knob gets its subject only once the bring-up that gives
	 * the reading a meaning has finished, and under the window's own lock,
	 * which is what keeps a concurrent writer out of a half-built state. */
	mutex_lock(&luna_gphy_lock);
	survey_ep = ep;
	mutex_unlock(&luna_gphy_lock);
	netdev_info(ndev, "up: irq=%d rx_prefix=%d backstop=%ums copper_phy=%d rtl8221b=%d\n",
		    ep->irq, rx_prefix, backstop_ms, copper_phy, rtl8221b_phy);
	ep->hw_up = true;
	luna_gpon_nic_reset_end();
	return 0;

fail:
	/* A half-programmed switch must not be reported as an ...
	 * dev/MEASURED-luna_eth.c.md sec 48. */
	{
		unsigned long flags;

		spin_lock_irqsave(&ep->tx_lock, flags);
		ep->closing = true;
		spin_unlock_irqrestore(&ep->tx_lock, flags);
	}
	eth_hw_stop(ep);
	luna_gpon_nic_reset_end();
	eth_free_rings(ep);
	netdev_err(ndev,
		   "switch bring-up failed (%d): eth0 stays DOWN rather than up on a half-programmed fabric\n",
		   ret);
	return ret;
}

static int eth_open(struct net_device *ndev)
{
	struct luna_eth *ep = netdev_priv(ndev);
	int ret;

	if (!ep->hw_up) {
		ret = eth_hw_start(ep);
		if (ret)
			return ret;
	}
	netif_start_queue(ndev);
	netif_carrier_on(ndev);
	return 0;
}

/* The MAC keeps running: DS OMCI and the WAN netdev ride its rings, and an OLT
 * gives up on an ONU whose OMCI stops while eth0 is down (rtl9602c_eth sec 96). */
static int eth_stop(struct net_device *ndev)
{
	struct luna_eth *ep = netdev_priv(ndev);

	netif_stop_queue(ndev);
	netif_carrier_off(ndev);
	luna_flow_stop(ep);
	return 0;
}

static const struct net_device_ops luna_eth_netdev_ops = {
	.ndo_open		= eth_open,
	.ndo_stop		= eth_stop,
	.ndo_start_xmit		= eth_xmit,
	.ndo_set_rx_mode	= eth_set_rx_mode,
	.ndo_set_mac_address	= eth_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
#if IS_ENABLED(CONFIG_LUNA_FLOWOFFLOAD)
	.ndo_setup_tc		= luna_flow_setup_tc,
#endif
};

/* ===== gpon0: the WAN data-GEM netdev ... -- dev/MEASURED-luna_eth.c.md sec 49. */
static void luna_eth_wan_hwaddr(struct luna_eth *ep, u8 out[ETH_ALEN])
{
	gpon_hwaddr_derive(out, ep->ndev->dev_addr, ep->c->wan_mac_offset);
}

static netdev_tx_t luna_eth_wan_xmit(struct sk_buff *skb,
				     struct net_device *ndev)
{
	struct luna_eth *ep = *(struct luna_eth **)netdev_priv(ndev);

	/* The SAME cpu-tag direct-TX pair the OMCC uses, with the data stream in
	 * place of the OMCC's -- the whole point of the family helpers carrying a
	 * stream id rather than a hard-coded flow. */
	return eth_tx_frame(ep, ndev, skb,
			    luna_gmac_cputag_txd_word2(ep->c->sw_map->pon_port),
			    luna_gmac_cputag_txd_word3(GPON_DATA_FLOW),
			    false);
}

static int luna_eth_wan_open(struct net_device *ndev)
{
	struct luna_eth *ep = *(struct luna_eth **)netdev_priv(ndev);
	u8 wmac[ETH_ALEN];

	if (is_valid_ether_addr(ep->ndev->dev_addr)) {
		luna_eth_wan_hwaddr(ep, wmac);
		eth_hw_addr_set(ndev, wmac);
		luna_omci_set_wan_mac(wmac);
	}
	netif_carrier_on(ndev);
	netif_start_queue(ndev);
	netdev_info(ndev, "WAN up: identity %pM (eth0 %pM + %u), US stream %u, fibre port %u\n",
		    ndev->dev_addr, ep->ndev->dev_addr, ep->c->wan_mac_offset,
		    GPON_DATA_FLOW, ep->c->sw_map->pon_port);
	return 0;
}

static int luna_eth_wan_stop(struct net_device *ndev)
{
	struct luna_eth *ep = *(struct luna_eth **)netdev_priv(ndev);

	netif_stop_queue(ndev);
	netif_carrier_off(ndev);
	luna_flow_stop(ep);
	return 0;
}

static const struct net_device_ops luna_eth_wan_ops = {
	.ndo_open		= luna_eth_wan_open,
	.ndo_stop		= luna_eth_wan_stop,
	.ndo_start_xmit		= luna_eth_wan_xmit,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
#if IS_ENABLED(CONFIG_LUNA_FLOWOFFLOAD)
	.ndo_setup_tc		= luna_flow_setup_tc,
#endif
};

/* Create gpon0 beside eth0.  A failure is reported and LEFT non-fatal: eth0 and
 * the OMCC are what keep the board reachable and ranged, and losing the LAN
 * because the WAN could not be registered would turn a WAN defect into a brick. */
static void luna_eth_wan_register(struct luna_eth *ep, struct device *dev)
{
	struct net_device *wan;
	u8 wmac[ETH_ALEN];

	wan = devm_alloc_etherdev(dev, sizeof(struct luna_eth *));
	if (!wan) {
		dev_warn(dev, "gpon0 (WAN) allocation failed; no WAN datapath\n");
		return;
	}
	*(struct luna_eth **)netdev_priv(wan) = ep;
	SET_NETDEV_DEV(wan, dev);
	strscpy(wan->name, "gpon0", IFNAMSIZ);
	wan->netdev_ops = &luna_eth_wan_ops;
	if (ep->flow) {
		wan->hw_features |= NETIF_F_HW_TC;
		wan->features |= NETIF_F_HW_TC;
	}
	luna_eth_wan_hwaddr(ep, wmac);
	eth_hw_addr_set(wan, wmac);
	luna_omci_set_wan_mac(wmac);
	netif_carrier_off(wan);
	if (devm_register_netdev(dev, wan)) {
		dev_warn(dev, "gpon0 (WAN) register failed; no WAN datapath\n");
		return;
	}
	ep->wan_ndev = wan;
}

/* ---- probe ---------------------------------------------------------------- */
static int luna_eth_omci_send(void *cookie, const u8 *msg, unsigned int len)
{
	return luna_eth_omci_xmit(cookie, msg, len);
}

static void luna_eth_tx_fence(void *cookie)
{
	struct luna_eth *ep = cookie;
	unsigned long flags;

	spin_lock_irqsave(&ep->tx_lock, flags);
	spin_unlock_irqrestore(&ep->tx_lock, flags);
}

static void luna_eth_omci_release(void *cookie)
{
	WRITE_ONCE(g_ep, NULL);
	luna_omci_detach(cookie);
}

static int luna_eth_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct net_device *ndev;
	struct luna_eth *ep;
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
	/* ★ THE CHIP TABLE COMES FROM THE MATCH, AND ITS ABSENCE IS FATAL. There
	 * is deliberately NO default: falling back to "the chip we happened to
	 * write first" is precisely how a sibling's register map reaches a new
	 * board, which is the defect this whole table exists to prevent. */
	ep->c = of_device_get_match_data(dev);
	if (!ep->c) {
		dev_err(dev, "no chip table for this compatible -- refusing to probe rather than guess a register map\n");
		return -ENODEV;
	}
	ep->ndev = ndev;
	ep->dev = dev;
	spin_lock_init(&ep->tx_lock);

	ep->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ep->base))
		return PTR_ERR(ep->base);
	ep->sw = devm_ioremap(dev, SWCORE_PHYS, ep->c->sw_map->swcore_size);
	if (!ep->sw)
		return -ENOMEM;

	/* MAC: DT/nvmem, else the value the boot loader programmed ...
	 * dev/MEASURED-luna_eth.c.md sec 50. */
	{
		u8 dt[GPON_HWADDR_BYTES], eng[GPON_HWADDR_BYTES];
		bool have_dt = of_get_mac_address(dev->of_node, dt) == 0;
		enum gpon_hwaddr_src src;
		bool eng_ok;

		eth_get_hwaddr(ep, eng);
		eng_ok = !luna_mac_is_bringup_default(eng);
		src = gpon_hwaddr_resolve(mac_param, have_dt ? dt : NULL,
					  eng_ok ? eng : NULL, mac);
		eth_hw_addr_set(ndev, mac);

		if (src == GPON_HWADDR_RANDOM)
			dev_warn(dev,
				 "MAC engine holds %pM: %s. Using %s %pM instead -- this board's real MAC lives in the vendor config partition and is not read yet\n",
				 eng,
				 eng_ok
				 ? "not a valid unicast address"
				 : "the SILICON BRING-UP DEFAULT, identical on every board of this family, which would collide on a shared segment",
				 gpon_hwaddr_src_name(src), ndev->dev_addr);
		else
			/* an address from outside the device must be auditable */
			dev_info(dev, "MAC %pM from %s\n", ndev->dev_addr,
				 gpon_hwaddr_src_name(src));
	}

	ndev->netdev_ops = &luna_eth_netdev_ops;
	ret = luna_mib_attach(ndev, ep->sw, ep->c->sw_map);
	if (ret)
		return dev_err_probe(dev, ret, "switch statistics attach failed\n");
	netif_carrier_off(ndev);
	netif_napi_add(ndev, &ep->napi, eth_napi_poll);

	ep->irq = platform_get_irq_optional(pdev, 0);
	if (ep->irq < 0)
		ep->irq = -1;

	ret = luna_omci_attach(&luna_eth_onu, ep, (u8)omci_mds_seed,
			       luna_eth_omci_send, luna_eth_tx_fence,
			       luna_eth_uni_admin_set);
	if (ret)
		return ret;
	/* ★ IMMEDIATELY, BEFORE ANY OTHER FALLIBLE STEP.  The attachment now
	 *   holds this cookie; a failure between the attach and its release
	 *   action would return from probe, free the NIC, and leave the common
	 *   owner calling into it. */
	ret = devm_add_action_or_reset(dev, luna_eth_omci_release, ep);
	if (ret)
		return ret;
	/* devres releases in REVERSE order, so this runs after the netdev is
	 * unregistered (which stops it) and before @ep is freed. */
	ret = devm_add_action_or_reset(dev, luna_eth_survey_withdraw, ep);
	if (ret)
		return ret;
	WRITE_ONCE(g_ep, ep);
	ret = luna_flow_probe(ep);
	if (ret)
		dev_warn(dev, "hardware flow offload unavailable: %d\n", ret);
	if (ep->flow) {
		ndev->hw_features |= NETIF_F_HW_TC;
		ndev->features |= NETIF_F_HW_TC;
	}
	ret = devm_register_netdev(dev, ndev);
	if (ret)
		return ret;
	luna_eth_wan_register(ep, dev);

	dev_info(dev, "%s NIC at %pR, MAC %pM, irq %d\n", ep->c->name,
		 platform_get_resource(pdev, IORESOURCE_MEM, 0),
		 ndev->dev_addr, ep->irq);
	return 0;
}

static const struct of_device_id luna_eth_of_match[] = {
	{ .compatible = "realtek,rtl9607c-nic",   .data = &luna_chip_rtl9607c },
	{ .compatible = "realtek,rtl9603cvd-nic", .data = &luna_chip_rtl9603cvd },
	{ }
};
MODULE_DEVICE_TABLE(of, luna_eth_of_match);

static struct platform_driver luna_eth_driver = {
	.probe	= luna_eth_probe,
	.driver	= {
		/* ★ THE FAMILY'S NAME, NOT A DIE'S. This file serves the ...
		 * dev/MEASURED-luna_eth.c.md sec 51. */
		.name		= "luna-eth",
		.of_match_table	= luna_eth_of_match,
	},
};
module_platform_driver(luna_eth_driver);

/* GPON <-> NIC glue: the entry points the shared GPON FSM ...
 * dev/MEASURED-luna_eth.c.md sec 52. */
void rtl9602c_eth_set_omci_sid(unsigned int sid)
{
	struct luna_eth *ep = g_ep;
	u32 v;

	if (!ep)
		return;
	ep->omci_sid = sid;
	ep->omci_trap_on = true;

	v = ep_rd(ep, R_CPUTAG1CR);
	ep_wr(ep, R_CPUTAG1CR, (v & ~CPUTAG1CR_SID_MASK) | CPUTAG1CR_SID(sid));

	netdev_info(ep->ndev,
		    "OMCI trap armed: OMCC sid %u, DS cpu-tag reason %u, PON port %u, CPUTAG1CR 0x%08x -> 0x%08x\n",
		    sid, ep->c->sw_map->omci_cpu_reason, ep->c->sw_map->pon_port,
		    v, ep_rd(ep, R_CPUTAG1CR));
}
EXPORT_SYMBOL(rtl9602c_eth_set_omci_sid);

/* Provision the ONU identity into the responder's ME 256 ...
 * dev/MEASURED-luna_eth.c.md sec 53. */
void rtl9602c_eth_set_omci_identity(const u8 *sn8)
{
	luna_omci_set_sn(sn8);
}
EXPORT_SYMBOL(rtl9602c_eth_set_omci_identity);

/* DS OMCI frames that reached the CPU ring.  Now a real count: this shell HAS
 * a path, so GPON_OMCI_RX_UNAVAIL ("could not ask") would itself be a lie. */
u32 rtl9602c_eth_omci_rx_count(void)
{
	struct luna_eth *ep = g_ep;

	return ep ? ep->dbg_omci_rx : GPON_OMCI_RX_UNAVAIL;
}
EXPORT_SYMBOL(rtl9602c_eth_omci_rx_count);

/* gpon0 (WAN) RX packet count. THREE STATES, NOT TWO. No ...
 * dev/MEASURED-luna_eth.c.md sec 54. */
u32 rtl9602c_eth_wan_rx_count(void)
{
	struct luna_eth *ep = g_ep;

	return (ep && ep->wan_ndev) ? (u32)ep->wan_ndev->stats.rx_packets
				    : GPON_OMCI_RX_UNAVAIL;
}
EXPORT_SYMBOL(rtl9602c_eth_wan_rx_count);

/* US-OMCI responses the ring has actually accepted.  This shell shares the LAN
 * ring rather than owning a dedicated OMCC ring, so the honest answer to "is
 * the OMCC TX ring being fetched" is how many injects were queued. */
u32 rtl9602c_eth_omci_tx_dirty(void)
{
	struct luna_eth *ep = g_ep;

	return ep ? ep->dbg_omci_tx : 0;
}
EXPORT_SYMBOL(rtl9602c_eth_omci_tx_dirty);

/* US-OMCI responses the ring REFUSED (full / closing / no buffer). A separate
 * number from the queued count on purpose: "we queued N" and "the PON-IP
 * transmitted 0" only becomes a diagnosis once you know what was dropped. */
u32 rtl9602c_eth_omci_tx_dropped(void)
{
	struct luna_eth *ep = g_ep;

	return ep ? ep->dbg_omci_tx_drop : 0;
}
EXPORT_SYMBOL(rtl9602c_eth_omci_tx_dropped);

/* OLT-independent US-OMCI steering self-test: push one ... -- dev/MEASURED-luna_eth.c.md sec 55. */
void rtl9602c_eth_omci_selftest(void)
{
	struct luna_eth *ep = g_ep;
	u8 frame[OMCI_LEN];

	if (!ep || !ep->omci_trap_on)
		return;
	memset(frame, 0, sizeof(frame));
	frame[0] = 0x00; frame[1] = 0x01;	/* TID				*/
	frame[2] = 0x29;			/* MT = Get-response (0x09|AK)	*/
	frame[3] = 0x0a;			/* DevID = baseline		*/
	frame[4] = 0x01; frame[5] = 0x00;	/* ME class 256 (ONU-G)		*/
	luna_eth_omci_xmit(ep, frame, sizeof(frame));
}
EXPORT_SYMBOL(rtl9602c_eth_omci_selftest);

/* Report the HGU WAN egress (VEIP, ME 329) operational. The ...
 * dev/MEASURED-luna_eth.c.md sec 56. */
void rtl9602c_eth_omci_report_oper_up(void)
{
	luna_omci_report_oper_up();
}
EXPORT_SYMBOL(rtl9602c_eth_omci_report_oper_up);

/* Publish the live DDM optical levels into ME 263 (ANI-G) ...
 * dev/MEASURED-luna_eth.c.md sec 68. */
void rtl9602c_eth_omci_set_optical(s16 rx_level, s16 tx_level)
{
	luna_omci_set_optical((u16)rx_level, (u16)tx_level);
}
EXPORT_SYMBOL(rtl9602c_eth_omci_set_optical);

MODULE_DESCRIPTION("Realtek Luna (RTL9607C / RTL9603CVD) GMAC0 + switch Ethernet driver");
MODULE_LICENSE("GPL");
