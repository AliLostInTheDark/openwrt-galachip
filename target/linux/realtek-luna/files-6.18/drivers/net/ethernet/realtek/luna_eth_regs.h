/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Register facts for the Ethernet MAC and switch core on the Realtek Luna
 * family (RTL9602C, RTL9603CVD, and the siblings that follow).
 *
 * Registers at the same address on every chip are #defines here. The ones that
 * differ are in struct luna_sw_map, so a new board adds a table row, not a
 * driver.
 *
 * The switch LUT block moved between the two revisions: the RTL9603CVD inserted
 * registers, so everything from LUT_UNKN_UC_DA_CTRL onward shifted. The
 * RTL9602C's LUT_UNKN_UC_FLOOD and the RTL9603CVD's LUT_BC_FLOOD are both
 * 0x1C028, so one chip's constant on the other does not fault or read back
 * wrong; it configures unicast flooding when broadcast flooding was meant.
 */
#ifndef _LUNA_ETH_REGS_H
#define _LUNA_ETH_REGS_H


#include <linux/etherdevice.h>	/* ETH_ALEN, ether_addr_equal */
#include <linux/delay.h>	/* udelay */
#include <linux/dma-mapping.h>	/* dma_map_single, DMA_FROM_DEVICE */
#include <linux/netdevice.h>	/* netdev_alloc_skb */
#include <linux/skbuff.h>	/* struct sk_buff, dev_kfree_skb_any */
#include <linux/io.h>		/* ioread32 / iowrite32 */
#include <linux/kernel.h>	/* sscanf */
#include <linux/types.h>
#include <linux/bits.h>
#include "luna_gmac_logic.h"	/* struct luna_rx_layout: the per-die RX field map */

/* Family-invariant facts */
/* MAC block, offsets from the MAC base. */
#define R_IDR0			0x00	/* station MAC [0:3], MSB first */
#define R_IDR4			0x04	/* station MAC [4:5] in [31:16] */
#define R_TCR			0x40	/* TX control */
#define R_RCR			0x44	/* RX control (bit0 = accept-all-physical) */
#define R_CPUTAGCR		0x48	/* CPU-tag insert config */
#define R_CONFIG		0x4C
#define R_CPUTAG1CR		0x50
/*
 * The re8686 NIC window has no chipdef, so these names are the driver's own
 * record.
 */
#define R_MAR0			0x08	/* multicast filter [31:0]      */
#define R_MAR4			0x0C	/* multicast filter [63:32]     */
#define R_CMD			0x3B	/* 8-bit CMD: RxChkSum|RxJumbo, bit0 = reset */
#define R_IMR			0x3c	/* 16-bit RX/TX IRQ mask (stock operating = 0xf835) */
#define R_ISR			0x3e	/* 16-bit RX/TX IRQ status, write-1-to-clear */
#define R_IMR0			0xd0	/* 32-bit per-ring TX-completion mask (stock = 0x3f) */
#define R_ISR1			0xd8	/* 32-bit per-ring TX-completion status, W1C */
#define R_RxDesNum		0x1430	/* RX ring0 size + flow-control thresholds */
#define R_IO_CMD		0x1434	/* DMA enable + per-ring TX kick (bit0 = ring0) */
#define R_IO_CMD1		0x1438

/* Interrupt bit groups, as stock programs them. */
#define IMR_RX_BITS		0xf835	/* RX-OK + RX-error + ring descriptor-unavailable */
#define IMR0_TX_BITS		0x3f	/* the 6 per-ring TX-completion IRQs */

/* Descriptor ownership and framing bits, shared by TX and RX rings. */
#define D_OWN			BIT(31)	/* 1 = owned by the DMA engine */
#define D_EOR			BIT(30)	/* end of ring (wrap) */
#define D_FS			BIT(29)	/* first segment */
#define D_LS			BIT(28)	/* last segment */
#define D_IPCS			BIT(27)	/* TX: insert IPv4 checksum */
#define D_TXCRC			BIT(23)	/* TX: append FCS */
#define RXD_CRCERR		BIT(27)	/* RX: CRC error */
#define RXD_RCDF		BIT(24)	/* RX: DMA error (rcdf in the vendor's rx_info) */
#define RXD_LEN_MASK		0x1fff	/* RX length, low bits of opts1 */
#define TXD_LEN_MASK		0x1ffff	/* TX length */

/*
 * RX ring size matches stock: the RTL9603CVD's vendor driver prints
 * RX_RING_SIZE[0]=1024 and the SDK's GMAC0_RX1_SIZE is 1024. A 64-entry ring
 * filled, the switch sent PAUSE, and CPU-path IPv6 forwarding on the G24W fell
 * to about 40 Mb/s with the CPU 99% idle.
 */
#define RX_RING_SIZE		1024
/* TX is the SDK's GMAC0_TX1_SIZE: with 64 slots gpon0 (which drops rather than
 * stops its queue on a full ring) discarded ~5% of a CPU-path v6 flow. */
#define TX_RING_SIZE		2048
#define RX_BUF_SIZE		2048

/* RX flow-control assert / de-assert thresholds. */
#define TH_ON_VAL		0x10
#define TH_OFF_VAL		0x30

/*
 * Switch-core registers both chipdefs place at the same address, so they are
 * family constants rather than table fields. They keep the silicon's own names.
 */
/*
 * GPHY indirect access: a data word, a command word that says where the data
 * goes, and a result word that also carries BUSY. The OCP map per port is a
 * per-chip question and is not settled here.
 */
#define SW_GPHY_IND_WD		0x00000	/* GPHY_IND_WD: the data word */
#define SW_GPHY_IND_CMD		0x00004	/* GPHY_IND_CMD: where it goes */
#define SW_GPHY_IND_RD		0x00008	/* GPHY_IND_RD: the answer, and BUSY */
#define   GPHY_IND_BUSY		BIT(16)	/* in the RD word: still in flight */
#define   GPHY_IND_EN		BIT(21)	/* in the CMD word: start */
#define   GPHY_IND_WREN		BIT(22)	/* in the CMD word: write, not read */
#define   GPHY_IND_PHY(p)	((u32)(p) << 16)	/* in the CMD word */
/* The OCP address of MII register N on a PHY answering on page 0xA40; an FE PHY
 * with a FLAT map answers at (N & 0x1f) << 1 instead.  WHICH of the two a given
 * port uses is per chip and is decided by that chip's own driver. */
#define GPHY_MII_PAGE		0xA400
/*
 * 0x198, 0x1b8, 0x1d8 and 0x1f8 are not named here. The chipdef names three of
 * them P_ABLTY, BYPS_ABLTY_LOCK and MISCELLANEOUS_BONDING, but they are read as
 * one per-port array at stride 0x20 (p0, p1, p2 and CPU status), and the
 * ability word is reached through the per-chip table.
 */
#define ABLTY_LINK		BIT(4)	/* the LINK field of the ability trio	*/
#define SW_MAC_CPU_TAG_CTRL	0x23030	/* MAC_CPU_TAG_CTRL */
#define SW_CHIP_INFO		0x10004	/* CHIP_INFO: low 16 bits = the GPHY variant */
#define SW_METER_TB_CTRL	0x25000	/* METER_TB_CTRL: meter tick/token config */
#define SW_VLAN_EGRESS_TAG	0x2A000	/* VLAN_EGRESS_TAG */
#define SW_STAT_PORT_TX_MIB	0x32000	/* STAT_PORT_TX_MIB, +0x80 per port */
/*
 * RX MIB base, one per die, with the die in the name: only that die's own table
 * row may use it. A macro rather than a table value so the check at the end of
 * this file can be a static_assert.
 */
#define SW_STAT_PORT_RX_MIB_RTL9602C	0x32200
#define SW_STAT_PORT_MIB_STRIDE	0x80u	/* the per-port step, stated by the
					 * dump's own comment and confirmed by
					 * every TX address it reads */
/*
 * The RX MIB base differs per die, so it is in the table rather than here:
 *
 *   die          STAT_PORT_TX_MIB   STAT_PORT_RX_MIB   ports   stride
 *   RTL9602C          0x32000           0x32200         0..3    0x80
 *   RTL9603CVD        0x32000           0x32600         0..6    0x80
 *   RTL9607C          0x32000           0x32600         0..11   0x80
 *
 * A family #define would give the RTL9602C its siblings' 0x32600, which on that
 * die is STAT_ACL_CNT and answers a normal read.
 *
 * The block layout is family-invariant (the chipdefs list the same 23 RX and 27
 * TX fields at the same offsets in the 0x80 window), so luna_eth_mib.c has one
 * field table and the bases are data.
 */


/* Switch core: the VLAN block did NOT move between these two revisions. */
/*
 * SWCORE registers rtl9602c_datapath_tables_init writes. Each address resolves
 * to the same register in the rtl9602c and rtl9603cvd chipdefs.
 */
#define SW_VLAN_INGRESS			0x13004
#define SW_LUT_CFG			0x17000
#define SW_LUT_AGEOUT_CTRL		0x17004
#define SW_LUT_UNMATCHED_SA_CTRL	0x1C000
#define SW_LUT_UNKN_SA_CTRL		0x1C004
#define SW_PISO_PORT			0x27000
/*
 * An array, not four registers. Only element 0 has a chipdef name; the two maps
 * name different later elements (PISO_EXT at 0x27008 on the RTL9602C, 0x2700c
 * on the RTL9603CVD) because the array's extent differs, so the base is named
 * and elements are computed.
 */
#define SW_PISO_PORT_STRIDE	4u
#define SW_VLAN_PORT_ACCEPT_FRAME_TYPE		0x13000	/* per-port accept-frame-type (0 = accept all) */
#define SW_VLAN_CTRL		0x13008
/*
 * PVID is 12-bit elements packed two to a 32-bit word: port p is in word (p /
 * 2), bits [(p % 2) * 12 + 11 : (p % 2) * 12]. Stepping by 4 would set only
 * ports 0 and 2, and write EXT_VID at 0x13014 and 0x13018.
 */
#define SW_VLAN_PB_VID		0x1300C	/* per-port default VID (PVID) array base */
#define SW_VLAN_PB_VID_BITS		12u	/* element width */
#define SW_VLAN_PB_VID_PORTS		4u	/* descriptor max port index 3 */

/*
 * Most per-port registers on this switch core are arrays of bit-fields, not one
 * word per port: element port of a bits-wide array is in word (port /
 * (32/bits)) at lsb (port % (32/bits)) * bits.
 *
 * This is pure arithmetic, with no MMIO or device pointer, so callers keep
 * their own accessor. The per-chip table supplies the base and the width.
 */
/* The unknown-SA / unknown-DA per-port ACTION arrays: 2-bit elements, so 16
 * per 32-bit word. Own k0_kernel descriptors @0xca1360 (0x1c008) and
 * @0xca1378 (0x1c00c, a SEPARATE register) both state stride 2 bits. */
#define SW_DA_ACT_BITS		2u

#define SW_PACKED_PER_WORD(bits)	(32u / (bits))

/* Byte offset of the word holding element `port`. */
static inline u32 sw_packed_off(u32 base, unsigned int port, unsigned int bits)
{
	return base + (port / SW_PACKED_PER_WORD(bits)) * 4u;
}

/* Bit position of element `port` inside that word. */
static inline unsigned int sw_packed_lsb(unsigned int port, unsigned int bits)
{
	return (port % SW_PACKED_PER_WORD(bits)) * bits;
}

/* `word` with element `port` replaced by `val`; every other bit preserved. */
static inline u32 sw_packed_ins(u32 word, unsigned int port, unsigned int bits,
				u32 val)
{
	unsigned int lsb = sw_packed_lsb(port, bits);
	u32 mask = ((1u << bits) - 1u) << lsb;

	return (word & ~mask) | ((val << lsb) & mask);
}

/* SoC glue that is at the same physical address on every Luna part seen here. */
#define SWCORE_PHYS		0x1B000000UL
#define SOC_IP_SEL		((void __iomem *)0xb8000600ul)	/* per-engine clock/reset */

/* The per-chip table */
/**
 * struct luna_sw_map - the switch-core facts that differ between Luna chips
 * @src_permit:       L2_SRC_PORT_PERMIT, one bit per port.  0xFFFFFFFF reads back
 *                    as 0xF on the RTL9602C and 0x3F on the RTL9603CVD.  What the
 *                    field gates is not established: forcing every bit changed no
 *                    observed forwarding on either die.  0 is written because it
 *                    is the reset value, not because it was shown to be
 *                    permissive, so no behaviour may be explained by this field.
 *                    On the RTL9602C, 0x1C114 is QOS_PB_PRI, not this register.
 * @piso_base:        per-port egress-forward (isolation) matrix
 * @cpu_port:         the switch port this GMAC is
 * @pon_port:         the fibre port (no copper PHY behind it)
 * @port_mask:        flood/member mask covering every port on this chip
 * @swcore_size:      ioremap length; must cover the highest block the driver
 *                    touches (MIB, PISO).  Too small and those reads land
 *                    outside the mapping instead of failing loudly.
 * @lut_unkn_sa:      unknown-SA action, 2 bits per port
 * @lut_unkn_uc_da:   per-port unknown-UC DLF action, 2 bits per port with every
 *                    port in one word.  The next word, +4, is a separate register
 *                    (LUT_LEARN_OVER_CTRL) and must never be written as this
 *                    one's port 2/3.
 * @unkn_l2_mc:       unknown L2 multicast action
 * @unkn_ip4_mc:      unknown IPv4 multicast action
 * @unkn_ip6_mc:      unknown IPv6 multicast action
 * @bc_flood:         broadcast flood, one bit per port
 * @unkn_mc_flood:    unknown-multicast flood, one bit per port
 * @unkn_uc_flood:    unknown-unicast flood, one bit per port
 * @omci_cpu_reason:  the CPU-tag RX `reason` code (rx opts2[28:21]) the switch
 *                    stamps on a downstream OMCI frame it traps to the CPU port.
 *                    Per chip: the vendor's NIC RX hook uses 246 for the RTL9602C
 *                    and 229 for the RTL9607C and RTL9603CVD.  The wrong value
 *                    matches nothing, so the trap stays silent and the log reads
 *                    as if the OLT sent no OMCI.
 *
 * A new chip adds one instance here and nothing else.  Every field is an absolute
 * offset within the switch core, never a delta from a sibling: a "+8 from the
 * 9602C" table silently follows the wrong chip the day a third revision moves only
 * half the block.
 */
struct luna_sw_map {
	/*
	 * Port numbers differ per chip: PON is port 2, 4 and 5 on the three
	 * chips.
	 */
	u32 src_permit;
	u32 piso_base;
	u8  cpu_port;
	u8  pon_port;
	/* Copper PHY ports, always 0..n_copper-1. */
	u8  n_copper;
	/*
	 * Per-port register interval (the vendor's macPpInfo.interval): 0x400
	 * on the RTL9602C, 0x100 on the RTL9603CVD and RTL9607C.
	 * RTL9602C-spaced writes on the RTL9603CVD land outside its per-port
	 * block, leaving unset the CPU port's RX_SPC, which makes the switch
	 * accept the 48-byte OMCI PDU instead of runt-filtering it.
	 */
	u16 macpp_stride;
	u32 port_mask;
	u32 swcore_size;
	u32 lut_unkn_sa;
	u32 lut_unkn_uc_da;
	u32 unkn_l2_mc;
	u32 unkn_ip4_mc;
	u32 unkn_ip6_mc;
	/*
	 * On the RTL9602C this address is CFG_UNHIOL (bit0 = IPG_COMPENSATION);
	 * on the RTL9603CVD the same address is CPU_TAG_AWARE. A family #define
	 * would configure CPU tagging where IPG compensation was meant, without
	 * faulting. 0 = this chip's bring-up does not touch it.
	 */
	/*
	 * WRAP_GPHY_MISC, bit0 = "PHY patch done", the sticky the stock switch
	 * init asserts at completion. The address differs per chip (9602C
	 * 0x110, 9607C 0x114, 9603CVD 0xEC).
	 */
	/*
	 * Per-port ability trio. The addresses overlap between chips without
	 * meaning the same thing: the RTL9603CVD has force_ablty at 0x198 and
	 * p_ablty at 0x1B8, while the RTL9602C has its force-ability array at
	 * 0x180 and its force-mode array at 0x1B4. 0 = this chip's driver does
	 * not use that member.
	 */
	/*
	 * ABLTY_LINK is the LINK field inside force_ablty (the value) and
	 * inside ablty_force (whether it is forced), used together to gate one
	 * port administratively.
	 */
	u32 force_ablty;	/* + 4*port: forced ability values	*/
	u32 p_ablty;		/* + 4*port: LIVE ability, read-only	*/
	u32 ablty_force;	/* + 4*port: which fields are forced	*/
	u32 gphy_misc;
	u32 cfg_unhiol;
	u32 bc_flood;
	u32 unkn_mc_flood;
	u32 unkn_uc_flood;
	/*
	 * STAT_PORT_TX_MIB is family-invariant; the RX base and the port count
	 * are not. 0 = this die's RX MIB base is not established and
	 * luna_eth_mib.c publishes no rx row, since 0 would read as "this port
	 * received nothing".
	 */
	u32 rx_mib;		/* STAT_PORT_RX_MIB base; 0 = not established */
	u8  n_mib_ports;	/* MIB port index range is 0..n_mib_ports-1	*/
	/*
	 * Offset from the MAC base, not the switch base, of the fourteen 16-bit
	 * GMAC counters (TXOKCNT RXOKCNT TXERR RXERR MISSPKT FAE ...), named
	 * and confirmed 16-bit by the RTL9602C's stock firmware. Not
	 * established on the RTL9603CVD or RTL9607C, whose tables leave it 0 so
	 * those boards publish no mac_* row.
	 */
	u32 gmac_mib16;		/* 16-bit MAC MIB array base; 0 = not established */
	u8  omci_cpu_reason;
	/*
	 * RX descriptor field positions for this die. NULL = not established;
	 * the parser then decodes nothing rather than borrowing a sibling's
	 * shifts.
	 */
	const struct luna_rx_layout *rx_layout;
};

static const struct luna_sw_map rtl9602c_sw_map = {
	.force_ablty	= 0x00180,
	.p_ablty	= 0x00198,	/* P_ABLTY in Realtek's RTL9602C register list */
	.ablty_force	= 0x001B4,
	.gphy_misc	= 0x00110,
	.cfg_unhiol	= 0x23040,	/* CFG_UNHIOL on this chip; see the field */
	.src_permit	= 0x1C088,
	.piso_base	= 0x27000,
	.cpu_port	= 3,
	.pon_port	= 2,
	.n_copper	= 2,	/* 0 = FE, 1 = GE -- this board's own stock map,
				 * where ME 0x0101 (the GE UNI) is port 1, and
				 * the boot loader's BMCR pair agrees: a 100M
				 * value to phy 0, a 1G value to phy 1 */
	.macpp_stride	= 0x400,	/* macPpInfo.interval, this chip's own */
	.port_mask	= 0xf,
	.swcore_size	= 0x40000,	/* must cover MIB @0x32000 + PISO @0x27000 */
	.lut_unkn_sa	= 0x1C004,	/* unmoved: the SAME offset on both chips */
	.lut_unkn_uc_da	= 0x1C008,
	.unkn_l2_mc	= 0x1C010,
	.unkn_ip4_mc	= 0x1C014,
	.unkn_ip6_mc	= 0x1C018,
	.bc_flood	= 0x1C020,
	.unkn_mc_flood	= 0x1C024,
	.unkn_uc_flood	= 0x1C028,
	.rx_mib		= SW_STAT_PORT_RX_MIB_RTL9602C,	/* [tier 3, its chipdef] */
	.n_mib_ports	= 4,		/* chipdef port index 0..3 */
	.gmac_mib16	= 0x10,		/* the 16-bit MAC MIB array [tier 2, stock hw_reg] */
	.omci_cpu_reason = 246,	/* RTL9602C_CHIP_ID -> omciRsn 246 [tier 3, gponapi.c] */
	.rx_layout	= &luna_rx_layout_rtl9602c,
};


/* ---- station address: the family's BRING-UP DEFAULT ----------------------- */
/*
 * The address the silicon/bootloader leaves in IDR0/IDR4 when nothing has
 * programmed a real one. It is a valid unicast address, so
 * is_valid_ether_addr() accepts it and it has to be refused by name for the
 * random fallback to fire.
 *
 * Seen on both the RTL9603CVD and the RTL9602C. Two ONUs sharing it on one L2
 * segment raises no error; the switch just learns the address on whichever port
 * spoke last.
 */
#define LUNA_MAC_BRINGUP_DEFAULT	{ 0x00, 0xe0, 0x4c, 0x86, 0x70, 0x01 }

static inline bool luna_mac_is_bringup_default(const u8 *mac)
{
	static const u8 dflt[ETH_ALEN] = LUNA_MAC_BRINGUP_DEFAULT;

	return ether_addr_equal(mac, dflt);
}


/* ---- the station address in the MAC engine (IDR0/IDR4) -------------------- */
static inline void luna_idr_get(void __iomem *base, u8 *mac)
{
	u32 lo = ioread32(base + R_IDR0), hi = ioread32(base + R_IDR4);

	mac[0] = lo >> 24; mac[1] = lo >> 16; mac[2] = lo >> 8; mac[3] = lo;
	mac[4] = hi >> 24; mac[5] = hi >> 16;
}

static inline void luna_idr_set(void __iomem *base, const u8 *mac)
{
	iowrite32(((u32)mac[0] << 24) | ((u32)mac[1] << 16) |
		  ((u32)mac[2] << 8) | mac[3], base + R_IDR0);
	iowrite32(((u32)mac[4] << 24) | ((u32)mac[5] << 16), base + R_IDR4);
}

/*
 * The bootarg parser and the precedence are in drivers/net/gpon/gpon_hwaddr.h;
 * luna_mac_is_bringup_default() stays here because which constant this silicon
 * powers up holding is a family fact. Never spell an address in a header or a
 * DTS: it is per-unit.
 */


/* GMAC stop, and the promiscuous bit */
static inline void luna_eth_hw_stop(void __iomem *base)
{
	iowrite32(0, base + R_IO_CMD);
	iowrite32(0, base + R_IO_CMD1);
	iowrite16(0, base + R_IMR);
	iowrite32(0, base + R_IMR0);
	iowrite16(0xffff, base + R_ISR);
	iowrite32(0xffffffff, base + R_ISR1);
	udelay(10);
}

/* A bridge enslaving the CPU netdev sets promisc; accept-all-physical (RCR
 * bit0) is then REQUIRED to receive LAN-client frames whose DA is not our MAC.
 * Without it a bridged port silently forwards nothing it did not address. */
static inline void luna_eth_set_promisc(void __iomem *base, bool on)
{
	u32 rcr = ioread32(base + R_RCR);

	if (on)
		rcr |= BIT(0);
	else
		rcr &= ~BIT(0);
	iowrite32(rcr, base + R_RCR);
}


/*
 * The DMA descriptor address bus window, zero on this SoC. dma_alloc_coherent()
 * already yields correct bus addresses here, so or-ing in a non-zero window
 * corrupts them: it was observed to be a no-op for TX egress and to degrade RX.
 */
#define DMA_BUS_WINDOW		0x00000000u

/*
 * The DMA descriptors. The layout is the silicon's: opts1 carries OWN/EOR and
 * the buffer length, addr is the bus address (with DMA_BUS_WINDOW already or'd
 * in), opts2/opts3 carry the per-frame classification words and opts4 the
 * TX-only extension.
 */
struct rx_desc { u32 opts1, addr, opts2, opts3; };
struct tx_desc { u32 opts1, addr, opts2, opts3, opts4; };

/*
 * A frame waits at @head: read from the ring, never from R_ISR. A NAPI poll
 * acks the status it masked, so a descriptor handed back after its drain and
 * before that ack loses its bit and would wait for the next frame's interrupt.
 * A DS OMCI request delayed that way waited 7 s for the OLT's retry, and the
 * OLT deactivated the ONU.
 */
static inline bool luna_rx_ready(const struct rx_desc *ring, unsigned int head)
{
	return !(READ_ONCE(ring[head].opts1) & D_OWN);
}

/*
 * Arm one RX slot with a fresh skb.
 *
 * EOR is set on the last slot and nowhere else. Getting that wrong does not
 * fault: the engine walks off the end of the ring into whatever follows it. @nr
 * is the ring's own entry count.
 */
/* Hand slot @idx back to HW with the buffer it ALREADY holds.
 *
 * The DROP path.  Nothing is unmapped and nothing is freed: the frame sitting
 * in that buffer is abandoned and the device overwrites it.  This is what a
 * poll loop must do when it cannot secure a replacement buffer -- see the
 * ownership contract on luna_rx_alloc(). */
static inline void luna_rx_rearm(struct rx_desc *ring, unsigned int idx,
				 unsigned int nr, unsigned int buf_size)
{
	u32 opts1 = D_OWN | buf_size;

	ring[idx].opts2 = 0;
	ring[idx].opts3 = 0;
	if (idx == nr - 1)
		opts1 |= D_EOR;
	ring[idx].opts1 = opts1;
}

/*
 * Allocate and map one replacement buffer. Returns the skb, or NULL.
 *
 * A poll loop hands its skb away on every arm (napi_gro_receive and netif_rx
 * take it, dev_kfree_skb_any frees it), so once the loop body has run there is
 * nothing left to put back. A refill that fails after that point cannot retry
 * next poll: it leaves skbs[idx] pointing at memory the driver no longer owns,
 * behind a descriptor whose D_OWN is clear, so the next poll re-delivers a
 * freed skb and unmaps the same address twice.
 *
 * Call this before consuming the frame, and on NULL drop the frame with
 * luna_rx_rearm(). Never NULL out skbs[idx] as a shortcut: the loops have no
 * !skb guard, so that turns the use-after-free into a NULL deref and still
 * leaves the stale dmas[idx] to be unmapped twice.
 */
static inline struct sk_buff *luna_rx_alloc(struct net_device *ndev,
					    struct device *dev,
					    unsigned int buf_size,
					    dma_addr_t *da)
{
	struct sk_buff *skb = netdev_alloc_skb(ndev, buf_size);

	if (!skb)
		return NULL;
	*da = dma_map_single(dev, skb->data, buf_size, DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, *da)) {
		dev_kfree_skb_any(skb);
		return NULL;
	}
	return skb;
}

/* Install an already-allocated buffer in slot @idx and hand it to HW. */
static inline void luna_rx_arm(struct rx_desc *ring, struct sk_buff **skbs,
			       dma_addr_t *dmas, unsigned int idx,
			       unsigned int nr, unsigned int buf_size,
			       struct sk_buff *skb, dma_addr_t da)
{
	skbs[idx] = skb;
	dmas[idx] = da;
	ring[idx].addr = da | DMA_BUS_WINDOW;
	luna_rx_rearm(ring, idx, nr, buf_size);
}

/* alloc + arm, for callers with nothing to lose: the OPEN path, where the slot
 * is still NULL and a failure simply propagates. */
static inline int luna_rx_refill(struct net_device *ndev, struct device *dev,
				    struct rx_desc *ring, struct sk_buff **skbs,
				    dma_addr_t *dmas, unsigned int idx,
				    unsigned int nr, unsigned int buf_size)
{
	dma_addr_t da;
	struct sk_buff *skb = luna_rx_alloc(ndev, dev, buf_size, &da);

	if (!skb)
		return -ENOMEM;
	luna_rx_arm(ring, skbs, dmas, idx, nr, buf_size, skb, da);
	return 0;
}


/* GMAC0 clock/reset gate */
#define IPSEL_GMAC0		BIT(1)

/*
 * Power-cycle GMAC0's clock domain.
 *
 * Restore only the GMAC bit. The sibling IP_SEL bits gate other clock domains;
 * also or-ing the stock NIC bring-up mask 0x1805 here, from a
 * chip-id-0x6266-conditional stock path, coincided with a hard SoC hang on the
 * RTL9602C.
 */
static inline void luna_ipsel_cycle_gmac0(void)
{
	writel(readl(SOC_IP_SEL) & ~IPSEL_GMAC0, SOC_IP_SEL);
	msleep(12);
	writel(readl(SOC_IP_SEL) | IPSEL_GMAC0, SOC_IP_SEL);
	msleep(2);
}


/*
 * SoC peripheral clock/enable word. This is not SOC_IP_SEL, which sits 0x3c
 * away in the same window.
 *
 * What bit 5 gates is unsettled: earlier drivers called the same bit
 * "switch-core enable" and "PONPBO IP enable", and both set it.
 */
#define SOC_SW_ENABLE	((void __iomem *)0xb800063cul)
#define   SW_EN_BIT	BIT(5)		/* see the disagreement above	*/
#define   SW_PBO_BIT	BIT(25)		/* required on rev > A		*/

/* Pin the computed addresses at build time. */
static_assert(SW_STAT_PORT_TX_MIB + 1 * SW_STAT_PORT_MIB_STRIDE == 0x32080u,
	      "p1 TX MIB moved");
static_assert(SW_STAT_PORT_TX_MIB + 2 * SW_STAT_PORT_MIB_STRIDE == 0x32100u,
	      "p2 TX MIB moved");
static_assert(SW_STAT_PORT_TX_MIB + 3 * SW_STAT_PORT_MIB_STRIDE == 0x32180u,
	      "p3 TX MIB moved");
static_assert(SW_STAT_PORT_RX_MIB_RTL9602C == 0x32200,
	      "the RX MIB base no longer matches the chipdef");
static_assert(SW_VLAN_EGRESS_TAG + 3 * 4 == 0x2a00cu,
	      "the VLAN egress-tag array moved");
static_assert(SW_PISO_PORT + 1 * SW_PISO_PORT_STRIDE == 0x27004u,
	      "the port-isolation array moved");

#endif /* _LUNA_ETH_REGS_H */
