/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Register facts for the Ethernet MAC and switch core on the Realtek Luna
 * family (RTL9602C, RTL9603CVD, and the siblings that follow).
 *
 * ★ WHY THIS FILE EXISTS, AND WHAT IT MEASURED.  The family had TWO Ethernet
 * drivers -- rtl9602c_eth.c for the X111W and luna_eth.c for the G24W --
 * each carrying its own copy of the register map.  Compared symbol by symbol on
 * 2026-08-28, with the comments stripped so that a re-worded comment could not
 * masquerade as a different value:
 *
 *     34 symbols  IDENTICAL on both chips   -> family facts, and they live here
 *      4 symbols  genuinely per-chip        -> struct luna_sw_map, below
 *
 * That ratio is the argument for the whole port strategy: a new Luna board owes
 * a TABLE, not a driver.  Two copies of 34 agreeing constants is not redundancy,
 * it is two chances to edit one of them.
 *
 * ★★ THE PER-CHIP FOUR ARE NOT A MISTAKE, AND THEY ARE THE DANGEROUS KIND.
 * The switch LUT block MOVED between the two silicon revisions: the RTL9603CVD
 * inserted registers, so everything from LUT_UNKN_UC_DA_CTRL onward shifted.
 * Confirmed on 2026-08-28 from each chip's OWN chipdef in the vendor SDK, which
 * agrees with what both drivers already had -- two independent tiers:
 *
 *     register              rtl9602c   rtl9603cvd
 *     LUT_UNKN_SA_CTRL       0x1C004     0x1C004    (unmoved)
 *     LUT_UNKN_UC_DA_CTRL    0x1C008     0x1C00C    (+4)
 *     UNKN_L2_MC             0x1C010     0x1C018    (+8)
 *     UNKN_IP4_MC            0x1C014     0x1C01C    (+8)
 *     UNKN_IP6_MC            0x1C018     0x1C020    (+8)
 *     LUT_BC_FLOOD           0x1C020     0x1C028    (+8)
 *     LUT_UNKN_MC_FLOOD      0x1C024     0x1C02C    (+8)
 *     LUT_UNKN_UC_FLOOD      0x1C028     0x1C030    (+8)
 *
 * ⚠ READ THE FIRST AND LAST ROWS TOGETHER: the 9602C's LUT_UNKN_UC_FLOOD and
 * the 9603CVD's LUT_BC_FLOOD are BOTH 0x1C028.  Using one chip's constant on
 * the other's silicon does not fault and does not read back wrong -- it
 * configures unicast flooding when broadcast flooding was meant.  That is the
 * same shape as the CFG_PHY_CTRL defect that cost this project weeks, and it is
 * exactly why these four are a table and not a #define.
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

/* ───────────────────────── family-invariant facts ───────────────────────── */
/* MAC block, offsets from the MAC base. */
#define R_IDR0			0x00	/* station MAC [0:3], MSB first */
#define R_IDR4			0x04	/* station MAC [4:5] in [31:16] */
#define R_TCR			0x40	/* TX control */
#define R_RCR			0x44	/* RX control (bit0 = accept-all-physical) */
#define R_CPUTAGCR		0x48	/* CPU-tag insert config */
#define R_CONFIG		0x4C
#define R_CPUTAG1CR		0x50
/* ★ NAMED FROM THIS DRIVER'S OWN REPEATED COMMENTS (2026-09-04).  Each of
 * the three was written as a literal at two or three sites and commented
 * with the same name every time; the tree established them, the compiler
 * just could not see it.  The re8686 NIC window has no chipdef here, so
 * these are the file's own record and nothing more is claimed. */
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
#define RXD_RCDF		BIT(24)	/* RX: DMA error.  RCDF is the SILICON's own
					 * name (the vendor NIC driver's rx_info
					 * opts1 bitfield, `rcdf:1;//24`, same in
					 * the 4.4 and 5.10 SDK drops).  This one
					 * bit carried TWO shell names, RXD_DMAERR
					 * (luna_eth.c, invented -- in no vendor
					 * source) and RXD_RCDF (rtl9602c_eth.c),
					 * until 2026-09-03. */
#define RXD_LEN_MASK		0x1fff	/* RX length, low bits of opts1 */
#define TXD_LEN_MASK		0x1ffff	/* TX length */

/* Ring geometry.  RX is STOCK's: the RTL9603CVD's running vendor driver prints
 * RX_RING_SIZE[0]=1024 and the SDK's GMAC0_RX1_SIZE is 1024. With 64 the ring
 * filled, the switch sent PAUSE (p3_tx_pause 42774) and G24W forwarded
 * CPU-path v6 at ~40 Mb/s with the CPU 99% idle (measured 2026-09-22). */
#define RX_RING_SIZE		1024
/* TX is the SDK's GMAC0_TX1_SIZE: with 64 slots gpon0 (which drops rather than
 * stops its queue on a full ring) discarded ~5% of a CPU-path v6 flow. */
#define TX_RING_SIZE		2048
#define RX_BUF_SIZE		2048

/* RX flow-control assert / de-assert thresholds. */
#define TH_ON_VAL		0x10
#define TH_OFF_VAL		0x30

/* Switch-core registers the two chipdefs place at the SAME address, so they are
 * family constants rather than table fields.  Cross-read 2026-08-28 from each
 * chip's own reg_list.c; the silicon's own names are kept, because a name that
 * follows the silicon is one a reader can look up. */
/* ★ THE GPHY INDIRECT PAIR, named from the RTL9602C's own chipdef
 * (2026-09-04).  rtl9602c_uboot_swcore_bringup() wrote eight values into
 * `ep->sw + 0x0` and `+ 0x4` with the comment `switch regs 0x10004/0x0/0x4`
 * -- the author knew they were a pair and had no name for it.  It is the
 * same shape as every other indirect block here: a DATA word, then a
 * COMMAND word that says where the data goes, and a RESULT word that also
 * carries BUSY.
 *
 * ★ THE COMMAND LAYOUT IS ESTABLISHED, and this note used to say it was not.
 *   luna_eth.c drives the same window field by field, and the literals the
 *   other shell writes decode exactly: 0x0060A400 is WREN|EN, phy 0, OCP
 *   0xA400, and 0x0061A400 is the same on phy 1 -- which is the BMCR
 *   power-down/power-up pair that file's own comments describe.  Leaving the
 *   note standing would keep two shells writing one register two ways.
 *
 * ⚠ WHAT IS *NOT* SETTLED IS THE OCP MAP PER PORT, which is a per-chip
 *   question and lives with the chip, not here. */
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
/* ⚠ 0x198 / 0x1b8 / 0x1d8 / 0x1f8 ARE NOT NAMED HERE, and the attempt is worth
 * recording (2026-09-04).  The chipdef names three of them P_ABLTY,
 * BYPS_ABLTY_LOCK and MISCELLANEOUS_BONDING -- three unrelated registers -- but
 * this driver's own /proc dump labels the four `p0_sts p1_sts p2_sts cpu_sts`,
 * a PER-PORT array at stride 0x20, and luna_eth.c already reaches the ability
 * word as SW_P_ABLTY(ep, p) built from the per-chip table.  So the chipdef is
 * naming ELEMENTS of an array, exactly as it does for the PISO block, and a
 * flat name for one element would be the same defect that made the G24W write
 * 0x2700c twice.  The compiler caught this one: SW_P_ABLTY was already taken. */
#define ABLTY_LINK		BIT(4)	/* the LINK field of the ability trio	*/
#define SW_MAC_CPU_TAG_CTRL	0x23030	/* MAC_CPU_TAG_CTRL */
#define SW_CHIP_INFO		0x10004	/* CHIP_INFO: low 16 bits = the GPHY variant */
#define SW_METER_TB_CTRL	0x25000	/* METER_TB_CTRL: meter tick/token config */
#define SW_VLAN_EGRESS_TAG	0x2A000	/* VLAN_EGRESS_TAG */
#define SW_STAT_PORT_TX_MIB	0x32000	/* STAT_PORT_TX_MIB, +0x80 per port */
/* ★ THE RX BASE, ONE PER DIE, AND THE DIE IS IN THE NAME ON PURPOSE.  These
 * exist so the number lives in exactly ONE place -- the table rows below are
 * initialised FROM them and never repeat the literal -- and so the pin at the
 * foot of this file can be a static_assert: a member of a `static const struct`
 * is not a constant expression in C, a macro is.
 * ⚠ THE SUFFIX IS A WARNING LABEL, NOT DECORATION.  A bare SW_STAT_PORT_RX_MIB
 * would look portable and a shared file would reach for it, which is exactly
 * how SW_MACPP_STRIDE_9602C got used on the RTL9603CVD.  Nothing outside that
 * die's own table row may name one of these. */
#define SW_STAT_PORT_RX_MIB_RTL9602C	0x32200
#define SW_STAT_PORT_MIB_STRIDE	0x80u	/* the per-port step, stated by the
					 * dump's own comment and confirmed by
					 * every TX address it reads */
/* ★★ THE RX MIB BASE IS SETTLED, AND IT IS PER-DIE -- WHICH IS WHY IT IS IN
 * THE TABLE AND NOT HERE (2026-09-11).  The 2026-09-04 note that used to sit
 * at this spot said there was "deliberately no SW_STAT_PORT_RX_MIB" because
 * the chipdef named 0x32400 STAT_PORT_OAM_MIB and 0x32600 STAT_ACL_CNT --
 * both true, and both the wrong register.  The chipdef DOES declare a
 * STAT_PORT_RX_MIB; nobody had looked it up by its own name:
 *
 *     die          STAT_PORT_TX_MIB   STAT_PORT_RX_MIB   ports   stride
 *     RTL9602C          0x32000           0x32200         0..3    0x80
 *     RTL9603CVD        0x32000           0x32600         0..6    0x80
 *     RTL9607C          0x32000           0x32600         0..11   0x80
 *
 * ⚠ READ THE TX AND RX COLUMNS TOGETHER.  TX does not move and RX does, so a
 * family #define for RX would give the RTL9602C the SIBLINGS' 0x32600 -- and
 * on that die 0x32600 is STAT_ACL_CNT, a real register that answers a real
 * read.  That is not a hypothetical: the /proc dump in rtl9602c_eth.c was
 * reading exactly that address under the label "p3 rx".  Same shape as the
 * LUT rows above, same shape as IO_LED_EN vs IO_MODE_EN.
 *
 * The block LAYOUT is family-invariant (the three chipdefs list the same 23
 * RX and 27 TX fields at the same byte offsets inside the 0x80 window), which
 * is why luna_eth_mib.c carries ONE field table and the bases are data.
 * Independently cross-checked: luna_eth.c's own RX unicast/multicast/broadcast
 * addresses on the RTL9603CVD (0x32620 / 0x32628 / 0x3262C) are exactly
 * base + IFINUCASTPKTS / IFINMULTICASTPKTS / IFINBROADCASTPKTS. */


/* Switch core: the VLAN block did NOT move between these two revisions. */
/* ★ NAMES BOTH CHIPDEFS AGREE ON, for the SWCORE registers
 * rtl9602c_datapath_tables_init writes (2026-09-04).  It spelled these as
 * bare hex; `bare_offset_chip_audit.py` resolves each address in the
 * rtl9602c AND rtl9603cvd chipdefs and only a SAME verdict is written here.
 * The SW_ prefix is this header's own convention for the SWCORE window.
 * ⚠ SW_VLAN_PORT_ACCEPT_FRAME_TYPE below is an ABBREVIATION the tree established earlier
 * (the chipdefs call 0x13000 VLAN_PORT_ACCEPT_FRAME_TYPE); it is left as it
 * is because renaming a name the tree already uses is a separate question
 * from naming a number that had none. */
#define SW_VLAN_INGRESS			0x13004
#define SW_LUT_CFG			0x17000
#define SW_LUT_AGEOUT_CTRL		0x17004
#define SW_LUT_UNMATCHED_SA_CTRL	0x1C000
#define SW_LUT_UNKN_SA_CTRL		0x1C004
#define SW_PISO_PORT			0x27000
/* ⚠ AN ARRAY, NOT FOUR REGISTERS.  Only element 0 carries a chipdef name;
 * the two maps name DIFFERENT later elements (PISO_EXT at 0x27008 on the
 * RTL9602C, at 0x2700c on the RTL9603CVD) because the array's extent
 * differs, not because anything moved.  Treating one element as a
 * standalone register made the G24W write 0x2700c twice and never write
 * 0x27008 -- so the BASE is named and the elements are arithmetic. */
#define SW_PISO_PORT_STRIDE	4u
#define SW_VLAN_PORT_ACCEPT_FRAME_TYPE		0x13000	/* per-port accept-frame-type (0 = accept all) */
#define SW_VLAN_CTRL		0x13008
/* ★★★ PVID IS PACKED TWO PER WORD -- "stride 4" WAS FALSE (measured 2026-09-12).
 * The array is 12-BIT elements in 32-bit words, so port p lives at
 * word (p / 2) in bits [(p % 2) * 12 + 11 : (p % 2) * 12], NOT at its own word.
 *
 * Proven three ways from our OWN stock kernel (cross-compiler/stock_nor/k0_kernel),
 * not from the SDK:
 *   descriptor @file 0xca15d0 : offset 0x1300c · element stride 0x000c = 12 BITS
 *                               · max port index 3
 *   _reg_addr_find @0x801b69dc: li 32 / div by stride / div port / sll 2 / addu
 *                               => addr = base + 4 * (port / (32 / 12))
 *   reg_array_field_write @0x801b72e8: mfhi after div / mult by stride
 *                               => shift = (port % (32 / 12)) * 12
 *
 * ⚠ WHAT THE FALSE STRIDE DID: a writer stepping by 4 set port 0 and port 2,
 *   left ports 1 and 3 INHERITED, and wrote its value into 0x13014/0x13018 --
 *   which are EXT_VID, a different register entirely. Three defects from one
 *   wrong word in a comment, and the code read correctly at every call site.
 */
#define SW_VLAN_PB_VID		0x1300C	/* per-port default VID (PVID) array base */
#define SW_VLAN_PB_VID_BITS		12u	/* element width */
#define SW_VLAN_PB_VID_PORTS		4u	/* descriptor max port index 3 */

/*
 * ★★★ THE PACKED PER-PORT ARRAY, ONCE FOR THE WHOLE FAMILY (2026-09-12).
 *
 * This switch core states most per-port registers as an ARRAY OF BIT-FIELDS,
 * not as one word per port: element `port` of a `bits`-wide array lives in word
 * (port / (32/bits)) at lsb (port % (32/bits)) * bits. Stock implements exactly
 * that arithmetic ONCE, generically, for every array it owns -- own k0_kernel
 * _reg_addr_find @0x801b69dc (`addr = base + 4 * (port / (32/bits))`) and
 * reg_array_field_write @0x801b72e8 (`shift = (port % (32/bits)) * bits`).
 *
 * ⚠ WE IMPLEMENTED IT PER CALL SITE AND GOT IT WRONG TWICE IN ONE FILE, each
 *   time by assuming one word per port:
 *     PVID  (12 bits): stepping by 4 set ports 0 and 2, left 1 and 3 inherited,
 *                      and wrote two words of EXT_VID.
 *     DLF   ( 2 bits): "16 bits per port" set port 0 only, and the second write
 *                      landed on LUT_LEARN_OVER_CTRL, a different register.
 *   Meanwhile luna_eth.c already knew the DLF field was 2 bits and said so in a
 *   comment -- the knowledge existed in one sibling and not the other, which is
 *   the shape this family header exists to end.
 *
 * ⇒ ONE statement of the arithmetic, here, for both Luna Ethernet shells. It is
 *   PURE -- no MMIO, no device pointer -- so each shell keeps its own accessor
 *   (ioread32/iowrite32 here, sw_rd/sw_wr there) and neither grows a copy of
 *   the layout. Per chip the TABLE supplies the base and the width; a new part
 *   costs a row, never a second implementation.
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

/* ─────────────────────────── the per-chip table ─────────────────────────── */
/**
 * struct luna_sw_map - the switch-core facts that differ between Luna chips
 * @src_permit:       L2_SRC_PORT_PERMIT -- one bit per port, and each die
 *                    states its OWN width on read-back: 0xFFFFFFFF comes back
 *                    0xF on the RTL9602C and 0x3F on the RTL9603CVD.
 *                    ★★ WHAT THE FIELD GATES IS NOT ESTABLISHED, AND THIS TREE
 *                    CLAIMED IT THREE WAYS, ALL INCOMPATIBLE: luna_eth.c said
 *                    "EN=1 DROPS the forwarded frame, 0 is the permissive
 *                    value"; rtl9602c_eth.c said "INVERTED polarity: EN=1
 *                    PERMITS a frame to egress its OWN ingress port"; the
 *                    RTL9607C bring-up said "all-zero means NO port may
 *                    forward" and wrote 0xFFFFFFFF to open it.
 *                    MEASURED 2026-09-11 on BOTH Luna boards by forcing every
 *                    bit and reading the EFFECT, never the read-back
 *                    (ONU-test-case/src_permit_polarity.py): the register KEEPS
 *                    the write and all three predictions FAIL.  Host<->board
 *                    forwarding 3/3 at the factory value, 3/3 forced, 3/3
 *                    restored on both dies; 0 of 12 of the host's own
 *                    broadcasts came back inbound either way; and on the
 *                    RTL9603CVD the CPU port's own flood return
 *                    (lan_flood_return) was `returns-internally` at 0x0 and at
 *                    0x3F alike.  ⇒ 0 is written because it IS the reset
 *                    value, not because 0 was proven permissive, and no
 *                    behaviour of this driver may be explained by this field
 *                    until a measurement finds one.  The claims are refuted for
 *                    the shipped configuration of these two dies; the RTL9607C
 *                    one is UNTESTED -- no such board is on this bench.
 *                    ⚠ ON THE RTL9602C THIS ADDRESS WAS WRONG ONCE, and the
 *                    note travels with the value: it was 0x1C114, which is
 *                    QOS_PB_PRI on that chip, so the CPU port's ingress permit
 *                    was never set and the fabric dropped every CPU-injected
 *                    frame after DMA -- TX counter climbing, nothing egressing.
 * @piso_base:        per-port egress-forward (isolation) matrix
 * @cpu_port:         the switch port this GMAC is
 * @pon_port:         the fibre port (no copper PHY behind it)
 * @port_mask:        flood/member mask covering every port on this chip
 * @swcore_size:      ioremap length; must cover the highest block the driver
 *                    touches (MIB, PISO).  Too small and those reads land
 *                    outside the mapping instead of failing loudly.
 * @lut_unkn_sa:      unknown-SA action, 2 bits per port
 * @lut_unkn_uc_da:   per-port unknown-UC DLF action. ★ 2 BITS PER PORT, every
 *                    port in ONE word -- "16 bits per port" was false and the
 *                    writer obeyed it (own k0_kernel descriptor @0xca1360:
 *                    stride 2 bits, max port 3). The next word, +4, is a
 *                    SEPARATE register (LUT_LEARN_OVER_CTRL, descriptor
 *                    @0xca1378) and must never be written as this one's port 2/3.
 * @unkn_l2_mc:       unknown L2 multicast action
 * @unkn_ip4_mc:      unknown IPv4 multicast action
 * @unkn_ip6_mc:      unknown IPv6 multicast action
 * @bc_flood:         broadcast flood, one bit per port
 * @unkn_mc_flood:    unknown-multicast flood, one bit per port
 * @unkn_uc_flood:    unknown-unicast flood, one bit per port
 * @omci_cpu_reason:  the CPU-tag RX `reason` code (rx opts2[28:21]) the switch
 *                    stamps on a DOWNSTREAM OMCI frame it traps to the CPU
 *                    port.  PER CHIP AND NOT GUESSABLE: the vendor's own NIC
 *                    RX hook switches on the chip id and picks 246 for the
 *                    RTL9602C but 229 for the RTL9607C and the RTL9603CVD
 *                    (rtl86900/sdk/src/module/gpon/gponapi.c,
 *                    rtk_gponapp_omci_rx_wrapper -- tier 3).  Reading the
 *                    9602C's 246 on this die matches a reason nothing ever
 *                    carries, so the trap stays silent and the log reads as
 *                    `the OLT sent no OMCI` -- the same shape as the eighteen
 *                    PON-IP offsets that were written with 9602C literals.
 *
 * A new chip adds ONE instance here and nothing else.  Every field is an
 * absolute offset within the switch core, never a delta from a sibling: a
 * "+8 from the 9602C" table is a table that silently follows the wrong chip the
 * day a third revision moves only half the block.
 */
struct luna_sw_map {
	/* ★ PORT NUMBERS ARE PER-CHIP AND WERE HALF-TABULATED.  luna_eth.c
	 * already carried pon_port/cpu_port per chip while rtl9602c_eth.c
	 * hardcoded RTL9602C_PON_PORT=2 and SW_CPU_PORT=3 -- the same shape as
	 * the MSR value one sibling made tunable and the other did not.  The
	 * numbers differ genuinely: PON is port 2, 4 and 5 on the three chips. */
	u32 src_permit;
	u32 piso_base;
	u8  cpu_port;
	u8  pon_port;
	/* Copper PHY ports, always 0..n_copper-1.  It lives HERE, beside the
	 * other port numbers, because BOTH Ethernet shells need it: one to walk
	 * the PHYs, the other to refuse an administrative request aimed at a
	 * port that has no PHY behind it. */
	u8  n_copper;
	/* ★ THE PER-PORT REGISTER INTERVAL, AND IT IS NOT THE FAMILY'S.  The
	 * vendor states it as data -- macPpInfo.interval, 0x400 on the RTL9602C
	 * and 0x100 on the RTL9603CVD and RTL9607C (sdk chipdef chip.c).  It sat
	 * in luna_gpon_regs.h only as a NUMBER named for one chip
	 * (SW_MACPP_STRIDE_9602C), so a shared file could reach for it and did:
	 * luna_gpon.c -- which BOTH Luna boards build -- wrote
	 * SW_P_MISC_PORT_9602C(2) and (3) at five sites, which on the RTL9603CVD
	 * address P_MISC[8] and P_MISC[12].  That chip has SIX ports, so both
	 * writes land outside the declared per-port block (macPpInfo upper bound
	 * 0x203FF) and the CPU port's RX_SPC -- what makes the switch accept the
	 * 48-byte OMCI PDU instead of runt-filtering it -- was never set. */
	u16 macpp_stride;
	u32 port_mask;
	u32 swcore_size;
	u32 lut_unkn_sa;
	u32 lut_unkn_uc_da;
	u32 unkn_l2_mc;
	u32 unkn_ip4_mc;
	u32 unkn_ip6_mc;
	/* ★★ CFG_UNHIOL, and it is IN THE TABLE for a measured reason: on the
	 * RTL9602C this address is CFG_UNHIOL (bit0 = IPG_COMPENSATION), and on
	 * the RTL9603CVD the SAME address is CPU_TAG_AWARE -- two different
	 * blocks, one number.  A family #define here would configure CPU tagging
	 * where IPG compensation was meant, without faulting and without reading
	 * back wrong: the CFG_PHY_CTRL defect, re-created.  0 = this chip's
	 * bring-up does not touch it. */
	/* ★ WRAP_GPHY_MISC, bit0 = "PHY patch done" -- the sticky the stock
	 * switch init asserts at completion.  THREE addresses for one job
	 * (9602C 0x110, 9607C 0x114, 9603CVD 0xEC), so it is per-chip data.
	 * It lives HERE and not in luna_eth_chip so that both Luna ethernet
	 * drivers read the SAME number: the family driver reaches it through
	 * its `sw_map` pointer, exactly as it already does for the port
	 * numbers, and a correction lands once. */
	/* ★ PER-PORT ABILITY TRIO -- per chip, and DEMONSTRABLY so: the RTL9603CVD
	 * puts force_ablty at 0x198 and p_ablty at 0x1B8, while the RTL9602C's own
	 * driver puts its force-ability array at 0x180 and its force-MODE array at
	 * 0x1B4.  The numbers overlap between chips without meaning the same
	 * thing, which is the third register block today found to do that.
	 * 0 = this chip's driver does not use that member. */
	/* ABLTY_LINK is the LINK field inside force_ablty (the value) and inside
	 * ablty_force (whether it is forced): the pair both Ethernet shells use
	 * to gate one port administratively. */
	u32 force_ablty;	/* + 4*port: forced ability values	*/
	u32 p_ablty;		/* + 4*port: LIVE ability, read-only	*/
	u32 ablty_force;	/* + 4*port: which fields are forced	*/
	u32 gphy_misc;
	u32 cfg_unhiol;
	u32 bc_flood;
	u32 unkn_mc_flood;
	u32 unkn_uc_flood;
	/* ★ THE STATISTICS BLOCK.  STAT_PORT_TX_MIB is family-invariant (0x32000
	 * on all three dies) and lives as a #define above; the RX base MOVES and
	 * the port count differs, so both are data.  See the table in the RX-MIB
	 * note above for the chipdef evidence.
	 * 0 = this die's RX MIB base has not been established -> luna_eth_mib.c
	 * publishes NO rx row for it.  A base nobody established rendered as 0
	 * would read as "this port received nothing", which is a device finding
	 * nobody measured. */
	u32 rx_mib;		/* STAT_PORT_RX_MIB base; 0 = not established */
	u8  n_mib_ports;	/* MIB port index range is 0..n_mib_ports-1	*/
	/* ★ THE MAC-LEVEL MIB ARRAY, and it is ESTABLISHED ON ONE DIE ONLY.
	 * Offset (from the MAC base, not the switch base) of the fourteen 16-bit
	 * GMAC counters -- TXOKCNT RXOKCNT TXERR RXERR MISSPKT FAE ... -- named
	 * and 16-bit-confirmed by the RTL9602C's OWN stock firmware, which prints
	 * them four hex digits wide from /proc/rtl8686gmac/hw_reg (tier 2,
	 * dev/STOCK_GMAC_REGS.md).  Nothing has established the array on the
	 * RTL9603CVD or the RTL9607C -- neither shell reads it there and neither
	 * chipdef covers the MAC window -- so those tables leave it 0 and those
	 * boards publish no mac_* row at all.  Borrowing the sibling's offset
	 * would answer with whatever that window holds. */
	u32 gmac_mib16;		/* 16-bit MAC MIB array base; 0 = not established */
	/* ★ THE DS-OMCI CPU-TAG REASON, and it lives beside the port numbers for
	 * the same reason they do: BOTH Ethernet shells classify a trapped OMCI
	 * frame by it, so a second home is how the two come to disagree.  It was
	 * a bare 246 in flowcore (rtl9602c_l34_logic.h) named for one chip while
	 * being read as a family fact -- the exact spelling that makes a sibling's
	 * literal look portable. */
	u8  omci_cpu_reason;
	/* ★ RX descriptor field positions for THIS die. NULL = not established;
	 * the parser then decodes nothing rather than borrowing a sibling's
	 * shifts, which is the defect this field exists to end. */
	const struct luna_rx_layout *rx_layout;
};

static const struct luna_sw_map rtl9602c_sw_map = {
	.force_ablty	= 0x00180,
	.p_ablty	= 0x00198,	/* P_ABLTY in Realtek's RTL9602C register list */
	.ablty_force	= 0x001B4,
	.gphy_misc	= 0x00110,
	.cfg_unhiol	= 0x23040,	/* CFG_UNHIOL on THIS chip -- see the field */
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
/* The address the silicon/bootloader leaves in IDR0/IDR4 when nothing has
 * programmed a real one.  It is a VALID unicast address, which is exactly why
 * it has to be NAMED: `is_valid_ether_addr()` accepts it, so a plain validity
 * check ships it and the random fallback never fires.
 *
 * ★ A FAMILY FACT, MEASURED ON TWO BOARDS OF DIFFERENT SILICON: the LANLY G24W
 * (RTL9603CVD, 2026-08-20) and the HSGQ X111W (RTL9602C, 2026-08-28 -- read
 * from the lab host's own ARP table while the board was answering pings, so
 * the two readings are independent of each other and of any one driver).
 * Two different chips holding ONE address is the whole problem: this lab runs
 * three ONUs on ONE L2 segment, and two of them sharing a MAC raises no error
 * anywhere -- it produces a switch that learns the address on whichever port
 * spoke last, and measurements on somebody else's bench that fail for no
 * visible reason.
 *
 * ⚠ IT LIVES IN THE FAMILY HEADER BECAUSE THE TWO-COPY VERSION ALREADY COST A
 * LIVE DEFECT.  The refusal was written into luna_eth.c alone, while the
 * RTL9602C's own driver carried a byte-identical IDR reader and a plain
 * `is_valid_ether_addr()` test -- so the 9602C shipped the shared default for
 * as long as the two copies existed, and nothing anywhere said so.  A family
 * fact kept in two chip shells is a repair with a delay fuse on it. */
#define LUNA_MAC_BRINGUP_DEFAULT	{ 0x00, 0xe0, 0x4c, 0x86, 0x70, 0x01 }

static inline bool luna_mac_is_bringup_default(const u8 *mac)
{
	static const u8 dflt[ETH_ALEN] = LUNA_MAC_BRINGUP_DEFAULT;

	return ether_addr_equal(mac, dflt);
}


/* ---- the station address in the MAC engine (IDR0/IDR4) -------------------- */
/* ★ SHARED BY TAKING THE **MAPPED BASE**, NOT THE DRIVER STRUCT.  Both Luna
 * ethernet drivers had a byte-identical pair of these, differing only in the
 * struct they dereferenced to reach `->base`.  That is the whole obstacle to
 * sharing driver code in this tree, and passing the io handle removes it: the
 * register layout is a FAMILY fact (R_IDR0/R_IDR4 above are already shared),
 * only the container was per-chip.
 *
 * ⚠ AND THE DUPLICATE PAIR IS WHY THE BRING-UP-DEFAULT REFUSAL BELOW REACHED
 * ONLY ONE OF THEM for eight days.  Two copies of one fact do not merely cost
 * lines; they cost the NEXT repair, which lands in whichever copy the author
 * happened to be reading. */
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

/* The bootarg parser and the whole precedence moved to
 * drivers/net/gpon/gpon_hwaddr.h on 2026-09-10; luna_mac_is_bringup_default()
 * stays here because WHICH constant this silicon powers up holding is a family
 * fact. Never spell an address in a header or a DTS: it is per-unit.
 */


/* ---- GMAC stop, and the promiscuous bit -----------------------------------
 * Two more bodies that were written twice, differing only in the struct they
 * dereferenced to reach `->base`.  Both are pure register work, so the (hwio)
 * conversion is the whole of it: take the mapped base.
 *
 * ★ THE COMMENT THAT EXPLAINS THE PROMISC BIT LIVED IN ONLY ONE OF THE TWO
 * COPIES, which is the quieter half of duplication: the code survives the
 * copy, the REASON does not, and the next reader of the poorer copy has to
 * re-derive it or guess. */
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


/* The DMA descriptor address bus window.  ZERO on this SoC, and it is a named
 * knob rather than a bare 0 for a measured reason:
 *
 * ⚠ OR-ING A NON-ZERO WINDOW INTO A DESCRIPTOR ADDRESS IS HARMFUL HERE.  It was
 * observed to be a NO-OP for TX egress and to DEGRADE RX -- `dma_alloc_coherent`
 * already yields correct bus addresses on this SoC (an artifact of its 1:1 map),
 * so or-ing the window CORRUPTS them.
 *
 * ★ THAT PARAGRAPH EXISTED IN ONE OF THE TWO COPIES ONLY.  Both drivers defined
 * this constant, one as 0x00000000u with the warning above and one as 0u with a
 * single line that says none of it.  The value survived being copied; the
 * measurement behind it did not. */
#define DMA_BUS_WINDOW		0x00000000u

/* ---- the DMA descriptors ---------------------------------------------------
 * ★ ONE TYPE, not two identical declarations.  Both Luna ethernet drivers
 * declared `struct rx_desc` and `struct tx_desc` with the same names and the
 * same layout, in their own files.  A duplicated TYPE is worse than a
 * duplicated function: the compiler checks each copy against itself, so the day
 * one of them gains a field the two silently describe different memory and the
 * engine reads a ring nobody wrote.
 *
 * The layout is the SILICON's, which is why it belongs to the family and not to
 * either chip: opts1 carries OWN/EOR and the buffer length, addr is the bus
 * address (with DMA_BUS_WINDOW already or'd in), opts2/opts3 carry the per-frame
 * classification words and opts4 the TX-only extension. */
struct rx_desc { u32 opts1, addr, opts2, opts3; };
struct tx_desc { u32 opts1, addr, opts2, opts3, opts4; };

/* A frame waits at @head: read from the ring, never from R_ISR.  A NAPI poll acks
 * the status it masked, so a descriptor handed back after its drain and before that
 * ack loses its bit and would wait for the NEXT frame's interrupt: on 2026-10-08 a
 * DS OMCI request waited 7 s for the OLT's retry, and the OLT deactivated the ONU. */
static inline bool luna_rx_ready(const struct rx_desc *ring, unsigned int head)
{
	return !(READ_ONCE(ring[head].opts1) & D_OWN);
}

/* ---- RX refill --------------------------------------------------------------
 * Arm ONE RX slot with a fresh skb.  Shared because both drivers had it
 * character for character apart from the struct they reached `->rx_ring`
 * through; the pieces are passed explicitly rather than behind a new container,
 * so neither driver has to change who owns its rings.
 *
 * ⚠ EOR IS SET ON THE LAST SLOT AND NOWHERE ELSE.  Getting that wrong does not
 * fault: the engine simply walks off the end of the ring into whatever follows
 * it, which is the quietest possible corruption.  `nr` is the ring's own entry
 * count, passed in, never a #define read from whichever driver compiled last. */
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

/* Allocate and map ONE replacement buffer.  -> the skb, or NULL.
 *
 * ★★ THE OWNERSHIP CONTRACT, and it is the whole point of the split.  A poll
 * loop hands its skb away on EVERY arm (napi_gro_receive / netif_rx take it,
 * dev_kfree_skb_any frees it), so once the loop body has run there is nothing
 * left to put back.  A refill that fails AFTER that point therefore cannot
 * "retry next poll": it leaves skbs[idx] pointing at memory the driver no
 * longer owns, behind a descriptor whose D_OWN is clear -- so the next poll
 * walks straight past the ownership guard and re-delivers a freed skb (and
 * unmaps the same address a second time, which fires first).
 *
 * ⇒ CALL THIS BEFORE CONSUMING THE FRAME, and on NULL drop the frame with
 * luna_rx_rearm() instead.  Never NULL out skbs[idx] as a shortcut: the loops
 * have no !skb guard, so that converts the use-after-free into a NULL deref
 * and still leaves the stale dmas[idx] to be unmapped twice. */
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


/* ---- the GMAC0 clock/reset gate -------------------------------------------
 * ★ ONE NAME FOR ONE BIT.  It was `IPSEL_GMAC0` in one driver and
 * `IPSEL_EN_GMAC0` in the other, both BIT(1) of the same SOC_IP_SEL word this
 * header already declared.  Two spellings of one register field is the shape
 * this project has paid for repeatedly: a correction lands on one name and the
 * other keeps the old behaviour, and nothing says so. */
#define IPSEL_GMAC0		BIT(1)

/* Power-cycle GMAC0's clock domain.
 *
 * ⚠ RESTORE ONLY THE GMAC BIT.  A trial that also OR'd the stock NIC bring-up
 * mask 0x1805 here -- taken from a chip-id-0x6266-conditional stock path --
 * coincided with a HARD SoC HANG on the RTL9602C: the sibling IP_SEL bits gate
 * other clock domains and are not ours to touch.
 *
 * ★ THAT WARNING EXISTED IN ONLY ONE OF THE TWO COPIES, which is the expensive
 * half of duplication -- the code survives being copied, the reason it is
 * shaped that way does not, and the next author of the poorer copy has nothing
 * to stop them re-running the experiment that hung the board. */
static inline void luna_ipsel_cycle_gmac0(void)
{
	writel(readl(SOC_IP_SEL) & ~IPSEL_GMAC0, SOC_IP_SEL);
	msleep(12);
	writel(readl(SOC_IP_SEL) | IPSEL_GMAC0, SOC_IP_SEL);
	msleep(2);
}


/* ---- SoC peripheral clock/enable word -------------------------------------
 * ⚠ THIS IS **NOT** `SOC_IP_SEL`.  They are 0x3c apart in the same SoC window,
 * and one of the three places that used this address called its pointer
 * `ipsel` -- which is the other register's name.  Two SoC control words a
 * stone's throw apart, one of them wearing the other's name, is precisely the
 * confusion this project renames on sight.
 *
 * ★ IT HAD THREE SPELLINGS: a #define here in one driver, a bare literal in the
 * second, and a locally-named pointer in the third.  One home now.
 *
 * ⚠⚠ AND THE TWO DRIVERS DISAGREE ABOUT WHAT BIT 5 IS, which is recorded
 * rather than resolved: luna_eth.c calls it "switch-core enable" and
 * gpon-rtl960x.c calls the SAME bit at the SAME address "PONPBO IP enable".
 * Both set it.  Either one name is wrong, or the bit gates a block both need
 * and neither name says so.  OWED: settle it from the chip's own SDK
 * (tier 3) or a live read, and rename on the spot -- do NOT pick one by
 * majority, which is how two wrong names cancel and survive. */
#define SOC_SW_ENABLE	((void __iomem *)0xb800063cul)
#define   SW_EN_BIT	BIT(5)		/* see the disagreement above	*/
#define   SW_PBO_BIT	BIT(25)		/* required on rev > A		*/

/* ★ THE ARITHMETIC IS PINNED, so replacing eleven literals with expressions is
 * proven by the BUILD rather than by having read it carefully.  Each line is
 * one address the /proc dump used to spell as a number. */
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
