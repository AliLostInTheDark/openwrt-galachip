// SPDX-License-Identifier: GPL-2.0


#include "luna_ponmac.h"
#include "luna_ponmac_logic.h"	/* hoisted logic */
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/seq_file.h>
#include <linux/bits.h>

/* The opcodes, the step and the interpreter are the */
#include "gpon_regseq.h"

#define r960_op		gpon_regseq_op
#define R960_WR		GPON_REGSEQ_WR
#define R960_FLD	GPON_REGSEQ_FLD
#define R960_DLY	GPON_REGSEQ_DLY
#define R960_POLL	GPON_REGSEQ_POLL

#define WR(a, v)		GPON_WR((a), (v))
#define FLD(a, m, l, v)		GPON_FLD((a), (m), (l), (v))
#define DLY(ms)			GPON_DLY((ms))
#define POLL(a, bit, iters)	GPON_POLL((a), (bit), (iters))

/* The interpreter's shell half: this family's SLEEP. The core cannot call
 * mdelay()/udelay() -- a tier that sleeps cannot run on a host -- so the two
 * delays are handed over as ops. */
static void r960_delay_ms(unsigned int ms)
{
	mdelay(ms);
}

static void r960_delay_us(unsigned int us)
{
	udelay(us);
}

static int r960_run(const struct luna_ops *o,
		    const struct r960_op *seq, unsigned int n)
{
	const struct gpon_regseq_io io = {
		.rd		= o->rd,
		.wr		= o->wr,
		.delay_ms	= r960_delay_ms,
		.delay_us	= r960_delay_us,
	};

	return gpon_regseq_run(&io, seq, n);
}

/* RTL9602C GPON PON-MAC / SerDes bring-up */
#define C2_SWCORE_BASE		0x1B000000u	/* SDS / swcore offset base    */
#define C2_PONIP_BASE		0x1BF00000u	/* PON-IP datapath offset base */

/* swcore-relative register absolutes used by the ordered bring-up steps */
#define C2_SDS_CFG		0x1B0001D0u	/* [4:0] CFG_SDS_MODE          */
#define   C2_SDS_MODE_OFF	0x1Fu		/* illegal/off mode (park)     */
#define   C2_SDS_MODE_GPON	0x08u		/* GPON line-rate mode         */
#define C2_SW_SOFTWARE_RST	0x1B000104u	/* [7] SDS_CFG_RST [0] SDS_RST */
#define C2_WSDS_DIG_00		0x1B022030u	/* [0] STOP_CLK; run = 0xf30   */
#define   C2_WSDS_DIG00_RUN	0xF30u		/* operational run state       */
#define C2_WSDS_DIG_01		0x1B022034u	/* [31:0] CFG_DMY0 (force-SDS) */
#define C2_WSDS_DIG_02		0x1B022038u	/* [10] EN_PDOWN_BEN           */
#define C2_WSDS_DIG_03		0x1B02203Cu	/* [6:4] TXDIS_SEL_DLY [3:0]D2A*/
#define C2_WSDS_DIG_18		0x1B022090u	/* [12] BEN_OE [15:13] optic   */
#define C2_WSDS_DIG_1D		0x1B0220A4u	/* [16:14] interface reset-B   */
#define C2_WSDS_DIG_1E		0x1B0220A8u	/* [5:4] D2A interconnect      */
#define C2_SDS_REG7		0x1B02281Cu	/* [14] SP_CFG_NEG_CLKWR_A2D   */
#define C2_SDS_EXT_REG12	0x1B022A30u	/* [8] SEP_CFG_NEG_CLKRD_D2A   */
#define C2_SDS_FORCE_BEN	0x1B0220E4u	/* [0] BEN_FORCE_MODE          */
#define C2_SDS_ANA_COM_REG08	0x1B0225A0u	/* TX-CDR; [15] cdr_reset bit  */
#define C2_SDS_ANA_COM_REG12	0x1B0225B0u	/* [14] RX_SEL_CDR_AFEN        */
#define C2_SDS_ANA_COM_REG22	0x1B0225D8u	/* [5:3] TX_AMP [2:0] TX_EMP   */
#define C2_SDS_ANA_MISC_REG00	0x1B022500u	/* [5] FRC_RX_EN_VAL [4] _ON   */
#define C2_SDS_ANA_MISC_REG01	0x1B022504u	/* [7:5] SPDSEL_VAL [4] _ON    */
#define C2_SDS_ANA_MISC_REG02	0x1B022508u	/* [13] FRC_BER_NOTIFY_VAL [12] _ON */
#define C2_FIB_EXT_REG21	0x1B022E54u	/* [13] FEP_V2ANALOG (lock)    */
#define   C2_SDS_ANALOG_READY	13u		/* FIB_EXT_REG21 ready bit      */
#define C2_SDS_LOCK_POLL_MAX	1000u		/* x200us = up to 200 ms        */

/* Named PON-MAC / PON-IP config registers used by c2_ponmac_init; the names are
 * this chip's own. */
#define C2_DYNGASP_CTRL		0x1B0001ECu	/* [3] DYNGASP_CMP_INV          */
#define C2_PON_BW_THRES		0x1BF02150u	/* upstream BW request thresholds */
#define C2_PON_SCH_CTRL		0x1BF02194u	/* [18] PON_GEN_PIR_DROP        */
#define C2_PON_TRAP_CFG		0x1B0111F8u	/* [2:0] OMCI_MPCP_PRIORITY     */

/* Register-PAGE address macros for the golden analog table below, so every entry
 * names its register instead of a bare address. The WSDS_DIG page numbers its
 * registers in HEX (DIG_0E follows DIG_0D); the four line-rate pages (SPD /
 * 1P25G / GPON / EPON) share one numbering that starts at REG32 per page base. */
#define C2_WSDS_ANA(n)		(0x1B022000u + (n) * 4u)	/* WSDS_ANA_nn      */
#define C2_WSDS_DIG(n)		(0x1B022030u + (n) * 4u)	/* WSDS_DIG_nn (hex) */
#define C2_SDS_ANA_COM_REG(n)	(0x1B022580u + (n) * 4u)	/* SDS_ANA_COM_REGnn */
#define C2_SDS_ANA_SPD_REG(n)	(0x1B022600u + ((n) - 32u) * 4u) /* REG32..  */
#define C2_SDS_ANA_1P25G_REG(n)	(0x1B022680u + ((n) - 32u) * 4u) /* REG32..  */
#define C2_SDS_ANA_GPON_REG(n)	(0x1B022700u + ((n) - 32u) * 4u) /* REG32..  */
#define C2_SDS_ANA_EPON_REG(n)	(0x1B022780u + ((n) - 32u) * 4u) /* REG32..  */
#define C2_FIB_REG(bank, n)	(0x1B022C00u + (bank) * 0x80u + (n) * 4u)

/* FIB_REG0 bank bases (absolute); FP_CFG_FIB_PDOWN bit11 cleared = fiber on. */
#define C2_FIB_REG0_PDOWN	BIT(11)
static const u32 c2_fib_reg0_banks[] = {
	C2_FIB_REG(0, 0), C2_FIB_REG(1, 0),	/* FIB_REG0 of each of the four banks */
	C2_FIB_REG(2, 0), C2_FIB_REG(3, 0),
};

/* Full SerDes analog + WSDS golden table */
static const struct { u32 off; u32 val; } c2_analog[] = {
	/* WSDS analog front, plus the hex-numbered WSDS_DIG RX-path config. */
	{ C2_WSDS_ANA(0),             0x00000805 }, { C2_WSDS_ANA(2),             0x0000ffff },
	{ C2_WSDS_ANA(7),             0x0000ffff }, { C2_WSDS_ANA(8),             0x0000ffff },
	{ C2_WSDS_DIG(0x02),          0x00000900 }, { C2_WSDS_DIG(0x06),          0x000000ff },
	{ C2_WSDS_DIG(0x08),          0x00022300 }, { C2_WSDS_DIG(0x09),          0x00022310 },
	{ C2_WSDS_DIG(0x0A),          0x083d0100 }, { C2_WSDS_DIG(0x0C),          0x00000fff },
	{ C2_WSDS_DIG(0x0D),          0x0000cf45 }, { C2_WSDS_DIG(0x0E),          0x00000f45 },

	/* SDS_ANA_MISC: RX-enable force, speed select, BER-notify force. */
	{ C2_SDS_ANA_MISC_REG00,      0x00000030 }, { C2_SDS_ANA_MISC_REG01,      0x00000030 },
	{ C2_SDS_ANA_MISC_REG02,      0x00003000 },

	/* SDS_ANA_COM: CMU, RX CDR front-end, filters, bias.  REG14 (0x225b8) is
	 * not part of the golden set and is never written. */
	{ C2_SDS_ANA_COM_REG(0),      0x00003400 }, { C2_SDS_ANA_COM_REG(1),      0x000073a4 },
	{ C2_SDS_ANA_COM_REG(2),      0x00006df8 }, { C2_SDS_ANA_COM_REG(3),      0x00008941 },
	{ C2_SDS_ANA_COM_REG(4),      0x00008884 }, { C2_SDS_ANA_COM_REG(5),      0x0000413f },
	{ C2_SDS_ANA_COM_REG(6),      0x00004fc0 }, { C2_SDS_ANA_COM_REG(7),      0x00005682 },
	{ C2_SDS_ANA_COM_REG(8),      0x00000713 }, { C2_SDS_ANA_COM_REG(9),      0x000002f5 },
	{ C2_SDS_ANA_COM_REG(10),     0x00002793 }, { C2_SDS_ANA_COM_REG(11),     0x0000b000 },
	{ C2_SDS_ANA_COM_REG(12),     0x00004848 }, { C2_SDS_ANA_COM_REG(13),     0x000000c8 },
	{ C2_SDS_ANA_COM_REG(15),     0x000008f2 }, { C2_SDS_ANA_COM_REG(16),     0x00001042 },
	{ C2_SDS_ANA_COM_REG(17),     0x0000c391 }, { C2_SDS_ANA_COM_REG(18),     0x00006a00 },
	{ C2_SDS_ANA_COM_REG(19),     0x00006600 }, { C2_SDS_ANA_COM_REG(20),     0x0000c000 },

	/* COM_REG22 (TX_AMP/EMP) is set later in the TX section by field-writes, not
	 * as a full word here, so its upper bits keep their reset state. */
	{ C2_SDS_ANA_COM_REG(23),     0x00000418 }, { C2_SDS_ANA_COM_REG(24),     0x00008001 },
	{ C2_SDS_ANA_COM_REG(25),     0x0000001f }, { C2_SDS_ANA_COM_REG(26),     0x000011e4 },
	{ C2_SDS_ANA_COM_REG(27),     0x00009422 }, { C2_SDS_ANA_COM_REG(28),     0x00008502 },
	{ C2_SDS_ANA_COM_REG(29),     0x00000ff0 }, { C2_SDS_ANA_COM_REG(30),     0x0000000a },

	/* SDS_ANA_GPON page (REG34..REG55): the GPON line-rate CDR/PLL/PCM
	 * operating point - the page the lane uses in GPON mode. */
	{ C2_SDS_ANA_GPON_REG(34),    0x00000f00 }, { C2_SDS_ANA_GPON_REG(35),    0x0000b8c6 },
	{ C2_SDS_ANA_GPON_REG(36),    0x0000a112 }, { C2_SDS_ANA_GPON_REG(37),    0x00004280 },
	{ C2_SDS_ANA_GPON_REG(38),    0x0000f53f }, { C2_SDS_ANA_GPON_REG(39),    0x00004fdf },
	{ C2_SDS_ANA_GPON_REG(40),    0x00000001 }, { C2_SDS_ANA_GPON_REG(41),    0x0000309b },
	{ C2_SDS_ANA_GPON_REG(42),    0x0000225c }, { C2_SDS_ANA_GPON_REG(43),    0x00001061 },
	{ C2_SDS_ANA_GPON_REG(44),    0x0000110d }, { C2_SDS_ANA_GPON_REG(45),    0x00004854 },
	{ C2_SDS_ANA_GPON_REG(46),    0x000080c5 }, { C2_SDS_ANA_GPON_REG(47),    0x0000121e },
	{ C2_SDS_ANA_GPON_REG(48),    0x0000307b }, { C2_SDS_ANA_GPON_REG(49),    0x00000271 },
	{ C2_SDS_ANA_GPON_REG(50),    0x00000271 }, { C2_SDS_ANA_GPON_REG(51),    0x00001012 },
	{ C2_SDS_ANA_GPON_REG(52),    0x0000f162 }, { C2_SDS_ANA_GPON_REG(53),    0x00003026 },
	{ C2_SDS_ANA_GPON_REG(54),    0x0000a780 }, { C2_SDS_ANA_GPON_REG(55),    0x0000f000 },

	/* The other three line-rate pages, same REG34..REG55 layout. Stock's GPON
	 * bring-up never writes them; luna_c2_minimal_analog skips them through
	 * c2_off_overconfig(). */
	{ C2_SDS_ANA_SPD_REG(34),     0x00000f00 }, { C2_SDS_ANA_SPD_REG(35),     0x0000b8c6 },
	{ C2_SDS_ANA_SPD_REG(36),     0x0000a112 }, { C2_SDS_ANA_SPD_REG(37),     0x00004280 },
	{ C2_SDS_ANA_SPD_REG(38),     0x0000f53f }, { C2_SDS_ANA_SPD_REG(39),     0x00004fdf },
	{ C2_SDS_ANA_SPD_REG(40),     0x00000001 }, { C2_SDS_ANA_SPD_REG(41),     0x0000309b },
	{ C2_SDS_ANA_SPD_REG(42),     0x0000225c }, { C2_SDS_ANA_SPD_REG(43),     0x00001061 },
	{ C2_SDS_ANA_SPD_REG(44),     0x0000110d }, { C2_SDS_ANA_SPD_REG(45),     0x00004854 },
	{ C2_SDS_ANA_SPD_REG(46),     0x000080c5 }, { C2_SDS_ANA_SPD_REG(47),     0x0000121e },
	{ C2_SDS_ANA_SPD_REG(48),     0x0000307b }, { C2_SDS_ANA_SPD_REG(49),     0x00000271 },
	{ C2_SDS_ANA_SPD_REG(50),     0x00000271 }, { C2_SDS_ANA_SPD_REG(51),     0x00001012 },
	{ C2_SDS_ANA_SPD_REG(52),     0x0000f162 }, { C2_SDS_ANA_SPD_REG(53),     0x00003026 },
	{ C2_SDS_ANA_SPD_REG(54),     0x0000a780 }, { C2_SDS_ANA_SPD_REG(55),     0x0000f000 },

	{ C2_SDS_ANA_1P25G_REG(34),   0x00000f00 }, { C2_SDS_ANA_1P25G_REG(35),   0x0000b8c6 },
	{ C2_SDS_ANA_1P25G_REG(36),   0x0000a112 }, { C2_SDS_ANA_1P25G_REG(37),   0x00004280 },
	{ C2_SDS_ANA_1P25G_REG(38),   0x0000f53f }, { C2_SDS_ANA_1P25G_REG(39),   0x00004fdf },
	{ C2_SDS_ANA_1P25G_REG(40),   0x00000001 }, { C2_SDS_ANA_1P25G_REG(41),   0x0000309b },
	{ C2_SDS_ANA_1P25G_REG(42),   0x0000225c }, { C2_SDS_ANA_1P25G_REG(43),   0x00001062 },
	{ C2_SDS_ANA_1P25G_REG(44),   0x00002000 }, { C2_SDS_ANA_1P25G_REG(45),   0x00001050 },
	{ C2_SDS_ANA_1P25G_REG(46),   0x000080c1 }, { C2_SDS_ANA_1P25G_REG(47),   0x0000121e },
	{ C2_SDS_ANA_1P25G_REG(48),   0x0000107b }, { C2_SDS_ANA_1P25G_REG(49),   0x00000280 },
	{ C2_SDS_ANA_1P25G_REG(50),   0x00000280 }, { C2_SDS_ANA_1P25G_REG(51),   0x00001012 },
	{ C2_SDS_ANA_1P25G_REG(52),   0x0000f862 }, { C2_SDS_ANA_1P25G_REG(53),   0x00003938 },
	{ C2_SDS_ANA_1P25G_REG(54),   0x00003100 }, { C2_SDS_ANA_1P25G_REG(55),   0x0000f000 },

	{ C2_SDS_ANA_EPON_REG(34),    0x00000f00 }, { C2_SDS_ANA_EPON_REG(35),    0x0000b8c6 },
	{ C2_SDS_ANA_EPON_REG(36),    0x0000a112 }, { C2_SDS_ANA_EPON_REG(37),    0x00004280 },
	{ C2_SDS_ANA_EPON_REG(38),    0x0000f53f }, { C2_SDS_ANA_EPON_REG(39),    0x00004fdf },
	{ C2_SDS_ANA_EPON_REG(40),    0x00000001 }, { C2_SDS_ANA_EPON_REG(41),    0x0000309b },
	{ C2_SDS_ANA_EPON_REG(42),    0x0000225c }, { C2_SDS_ANA_EPON_REG(43),    0x00001062 },
	{ C2_SDS_ANA_EPON_REG(44),    0x00002000 }, { C2_SDS_ANA_EPON_REG(45),    0x00004850 },
	{ C2_SDS_ANA_EPON_REG(46),    0x000080c5 }, { C2_SDS_ANA_EPON_REG(47),    0x0000121e },
	{ C2_SDS_ANA_EPON_REG(48),    0x0000103e }, { C2_SDS_ANA_EPON_REG(49),    0x00000280 },
	{ C2_SDS_ANA_EPON_REG(50),    0x00000280 }, { C2_SDS_ANA_EPON_REG(51),    0x00001012 },
	{ C2_SDS_ANA_EPON_REG(52),    0x0000f862 }, { C2_SDS_ANA_EPON_REG(53),    0x00003938 },
	{ C2_SDS_ANA_EPON_REG(54),    0x0000b100 }, { C2_SDS_ANA_EPON_REG(55),    0x0000f000 },

	/* FIB (fiber optical front-end) config, 4 identical banks. FIB_REG0 carries
	 * FP_CFG_FIB_PDOWN at bit11, cleared separately below. */
	{ C2_FIB_REG(0, 0),           0x00001940 }, { C2_FIB_REG(0, 1),           0x00006109 },
	{ C2_FIB_REG(0, 2),           0x0000e001 }, { C2_FIB_REG(0, 3),           0x00003290 },
	{ C2_FIB_REG(0, 4),           0x000001a0 }, { C2_FIB_REG(0, 7),           0x00000004 },
	{ C2_FIB_REG(0, 15),          0x00008000 }, { C2_FIB_REG(0, 16),          0x00000083 },
	{ C2_FIB_REG(0, 18),          0x00005000 }, { C2_FIB_REG(0, 22),          0x00000001 },
	{ C2_FIB_REG(0, 23),          0x00004001 }, { C2_FIB_REG(0, 24),          0x00000004 },
	{ C2_FIB_REG(0, 25),          0x0000326a }, { C2_FIB_REG(0, 27),          0x0000115d },
	{ C2_FIB_REG(0, 28),          0x000033fa }, { C2_FIB_REG(0, 29),          0x0000e46a },
	{ C2_FIB_REG(0, 30),          0x0000071e },

	{ C2_FIB_REG(1, 0),           0x00001940 }, { C2_FIB_REG(1, 1),           0x00006109 },
	{ C2_FIB_REG(1, 2),           0x0000e001 }, { C2_FIB_REG(1, 3),           0x00003290 },
	{ C2_FIB_REG(1, 4),           0x000001a0 }, { C2_FIB_REG(1, 7),           0x00000004 },
	{ C2_FIB_REG(1, 15),          0x00008000 }, { C2_FIB_REG(1, 16),          0x00000083 },
	{ C2_FIB_REG(1, 18),          0x00005000 }, { C2_FIB_REG(1, 22),          0x00000001 },
	{ C2_FIB_REG(1, 23),          0x00004001 }, { C2_FIB_REG(1, 24),          0x00000004 },
	{ C2_FIB_REG(1, 25),          0x0000326a }, { C2_FIB_REG(1, 27),          0x0000115d },
	{ C2_FIB_REG(1, 28),          0x000033fa }, { C2_FIB_REG(1, 29),          0x0000e46a },
	{ C2_FIB_REG(1, 30),          0x0000071e },

	{ C2_FIB_REG(2, 0),           0x00001940 }, { C2_FIB_REG(2, 1),           0x00006109 },
	{ C2_FIB_REG(2, 2),           0x0000e001 }, { C2_FIB_REG(2, 3),           0x00003290 },
	{ C2_FIB_REG(2, 4),           0x000001a0 }, { C2_FIB_REG(2, 7),           0x00000004 },
	{ C2_FIB_REG(2, 15),          0x00008000 }, { C2_FIB_REG(2, 16),          0x00000083 },
	{ C2_FIB_REG(2, 18),          0x00005000 }, { C2_FIB_REG(2, 22),          0x00000001 },
	{ C2_FIB_REG(2, 23),          0x00004001 }, { C2_FIB_REG(2, 24),          0x00000004 },
	{ C2_FIB_REG(2, 25),          0x0000326a }, { C2_FIB_REG(2, 27),          0x0000115d },
	{ C2_FIB_REG(2, 28),          0x000033fa }, { C2_FIB_REG(2, 29),          0x0000e46a },
	{ C2_FIB_REG(2, 30),          0x0000071e },

	{ C2_FIB_REG(3, 0),           0x00001940 }, { C2_FIB_REG(3, 1),           0x00006109 },
	{ C2_FIB_REG(3, 2),           0x0000e001 }, { C2_FIB_REG(3, 3),           0x00003290 },
	{ C2_FIB_REG(3, 4),           0x000001a0 }, { C2_FIB_REG(3, 7),           0x00000004 },
	{ C2_FIB_REG(3, 15),          0x00008000 }, { C2_FIB_REG(3, 16),          0x00000083 },
	{ C2_FIB_REG(3, 18),          0x00005000 }, { C2_FIB_REG(3, 22),          0x00000001 },
	{ C2_FIB_REG(3, 23),          0x00004001 }, { C2_FIB_REG(3, 24),          0x00000004 },
	{ C2_FIB_REG(3, 25),          0x0000326a }, { C2_FIB_REG(3, 27),          0x0000115d },
	{ C2_FIB_REG(3, 28),          0x000033fa }, { C2_FIB_REG(3, 29),          0x0000e46a },
	{ C2_FIB_REG(3, 30),          0x0000071e },
};

/*
 * ponmac_init scheduler / OMCI-egress steering. PIR_DROP is asserted then
 * cleared for rev-A, matching the sibling driver's set/clear pair.
 */
static const struct r960_op c2_ponmac_init[] = {
	/* REG01 (SDS_ANA_COM 0x22584) is handled in rtl9602c_ponmac_init() below: the
	 * stock post-reset value 0x73a4 cannot be reached by a golden-table write
	 * before the reset. See luna_c2_stock_analog. */
	FLD(C2_DYNGASP_CTRL,  3,  3, 1),	/* DYNGASP_CMP_INV = 1              */
	FLD(C2_PON_BW_THRES, 29, 16, 5),	/* last-grant threshold             */
	FLD(C2_PON_BW_THRES, 13,  0, 5),	/* runt-grant threshold             */
	FLD(C2_PON_SCH_CTRL, 18, 18, 1),	/* PON_GEN_PIR_DROP = 1             */
	FLD(C2_PON_TRAP_CFG,  2,  0, 7),	/* OMCI_MPCP_PRIORITY = 7           */
	FLD(C2_PON_SCH_CTRL, 18, 18, 0),	/* rev-A: clear PON_GEN_PIR_DROP    */
};

/* Drive SDS_ANA_COM REG01 (0x22584) = 0x73a4 */
int luna_c2_stock_analog = 1;

/* Program the FULL analog CMU/CDR golden table AFTER the */
int luna_c2_analog_postreset = 1;

static int rtl9602c_ponmac_init(const struct luna_ops *o)
{
	int ret = r960_run(o, c2_ponmac_init, ARRAY_SIZE(c2_ponmac_init));

	if (ret)
		return ret;
	if (luna_c2_stock_analog) {
		luna_rfwr(o, C2_SDS_ANA_COM_REG(1),  14, 14, 1); /* REG_BEN_TTL_OUT = 1 (stock) */
		luna_rfwr(o, C2_SDS_ANA_COM_REG(1),   0,  0, 0); /* BENLA_LDOVREF[0] = 0 (stock) */
		luna_rfwr(o, C2_SDS_ANA_COM_REG(11),  7,  0, 0); /* RX_FILT_CONFIG = 0 (stock) */
	} else {
		luna_rfwr(o, C2_SDS_ANA_COM_REG(1),   0,  0, 1); /* legacy BENLA_LDOVREF[0] = 1 */
	}
	return 0;
}

/* SerDes CDR-lock pulse (the stock CDR-reset behaviour) */
static int rtl9602c_serdes_cdr_reset(const struct luna_ops *o)
{
	u32 cdr = o->rd(C2_SDS_ANA_COM_REG12);

	o->wr(C2_SDS_ANA_COM_REG12, cdr ^ BIT(15));
	mdelay(10);
	o->wr(C2_SDS_ANA_COM_REG12, cdr);
	return 0;
}

/* GPON SerDes bring-up, a faithful translation of the */
static const struct r960_op c2_sds_pre[] = {
	FLD(C2_SDS_CFG, 4, 0, C2_SDS_MODE_OFF),	/* park CFG_SDS_MODE = off (0x1f) */
	WR(C2_WSDS_DIG_01, 0),			/* clear force-SDS dummy          */
	FLD(C2_WSDS_DIG_00, 0, 0, 0),		/* STOP_CLK = 0                   */
};

/* Stock rev-A GPON ModeV1 pulses ONLY CMD_SDS_RST_PS (bit0). Our field-writes
 * are RMW, so asserting CMD_SDS_CFG_RST_PS (bit7) here would leave a reset
 * domain latched through the whole bring-up; it is applied conditionally in
 * rtl9602c_ponmac_mode_set behind luna_c2_sds_cfgrst. */
static const struct r960_op c2_sds_reset[] = {
	FLD(C2_SW_SOFTWARE_RST, 0, 0, 1),	/* CMD_SDS_RST_PS (bit0 only, stock)  */
	DLY(10),
};

/* Step 4: release all datapath soft-reset-B lines + force 125M ref (DIG_00 run
 * = 0xf30), then pulse the RX/TX interface reset-B lines (DIG_1D = 0x1c000). */
static const struct r960_op c2_sds_rstb[] = {
	WR(C2_WSDS_DIG_00, C2_WSDS_DIG00_RUN),	/* run state, 125M ref forced     */
	FLD(C2_WSDS_DIG_1D, 15, 15, 0),		/* RX interface reset-B 0         */
	FLD(C2_WSDS_DIG_1D, 16, 16, 0),		/* TX interface reset-B 0         */
	FLD(C2_WSDS_DIG_1D, 14, 14, 1),		/* common interface reset-B 1     */
	FLD(C2_WSDS_DIG_1D, 15, 15, 1),		/* RX interface reset-B 1         */
	FLD(C2_WSDS_DIG_1D, 16, 16, 1),		/* TX interface reset-B 1         */
	DLY(10),
};

/* Steps 5 + 6 */
static const struct r960_op c2_sds_rx_arm[] = {
	FLD(C2_WSDS_DIG_18, 12, 12, 1),		/* BEN_OE = 1                     */
	FLD(C2_WSDS_DIG_18, 15, 15, 0),		/* OPTIC_LOS_SEL_EPON = 0         */
	FLD(C2_WSDS_DIG_18, 14, 14, 0),		/* CFG_FRC_OPTIC_LOS = 0          */
	FLD(C2_WSDS_DIG_18, 13, 13, 0),		/* CFG_FRCV_OPTIC_LOS = 0         */
	FLD(C2_SDS_ANA_COM_REG12, 14, 14, 1),	/* RX_SEL_CDR_AFEN = 1            */
	DLY(10),
	FLD(C2_SDS_ANA_MISC_REG01, 7, 5, 1),	/* SPDSEL_VAL = GPON rate         */
	FLD(C2_SDS_ANA_MISC_REG01, 4, 4, 1),	/* SPDSEL force on                */
	FLD(C2_SDS_ANA_MISC_REG00, 4, 4, 1),	/* FRC_RX_EN_ON = 1               */
	FLD(C2_SDS_ANA_MISC_REG00, 5, 5, 0),	/* FRC_RX_EN_VAL 0 ...            */
	FLD(C2_SDS_ANA_MISC_REG00, 5, 5, 1),	/* ... -> 1 (start CDR)           */
	DLY(50),
	FLD(C2_WSDS_DIG_02, 10, 10, 0),		/* EN_PDOWN_BEN = 0               */
	FLD(C2_WSDS_DIG_03, 6, 4, 0),		/* CFG_TXDIS_SEL_DLY = 0          */
	FLD(C2_WSDS_DIG_03, 3, 0, 0),		/* CFG_D2ANLOG_SEL = 0 (TX path)  */
	FLD(C2_SDS_FORCE_BEN, 0, 0, 0),		/* BEN_FORCE_MODE = 0 (GTC gates) */
};

/* Step 6b + TX drive, with serdes_modev1_tx */
static const struct r960_op c2_sds_tx[] = {
	FLD(C2_WSDS_DIG_1E,    5,  4, 0),	/* D2A interconnect = 0 (stock)   */
	FLD(C2_SDS_REG7,      14, 14, 0),	/* SP_CFG_NEG_CLKWR_A2D = 0       */
	FLD(C2_SDS_EXT_REG12,  8,  8, 0),	/* SEP_CFG_NEG_CLKRD_D2A = 0      */
	FLD(C2_SDS_ANA_COM_REG22, 5, 3, 5),	/* REG_TX_AMP = 0x5               */
	FLD(C2_SDS_ANA_COM_REG22, 2, 0, 1),	/* REG_TX_EMP = 0x1               */
};

/* Steps 7a + 7b */
int luna_c2_postmode_perturb = 1;

/* Milliseconds to wait after forcing the 125M ref and BEFORE releasing the
 * interface reset-B, so the TX CMU PLL locks before the phase is latched. */
int luna_c2_cmu_settle_ms;

/* 1 = gate the SerDes word clock (STOP_CLK=1) across the DIG_1D reset-B release
 * and un-gate LAST, so the word divider restarts on one defined edge instead of
 * being reset while free-running. 0 = legacy. */
int luna_c2_clkgate_rstb;

/* 1 = skip the c2_sds_rstb dance entirely. WSDS_DIG_1D is already 0x1c000
 * (interface reset-B released) before mode_set and the SDS reset does not clear
 * it, so the dance is a reset-B pulse on a running serializer that stock rev-A
 * never issues. 0 = legacy dance. */
int luna_c2_skip_rstb_dance;

/* 0 = pulse ONLY CMD_SDS_RST_PS bit0 in the SerDes reset, like stock rev-A;
 * 1 = also assert CMD_SDS_CFG_RST_PS bit7, which then stays RMW-latched through
 * the bring-up. */
int luna_c2_sds_cfgrst;

static const struct r960_op c2_sds_mode[] = {
	FLD(C2_SDS_ANA_MISC_REG02, 13, 13, 1),	/* FRC_BER_NOTIFY_VAL = 1         */
	FLD(C2_SDS_ANA_MISC_REG02, 12, 12, 1),	/* FRC_BER_NOTIFY_ON  = 1         */
	DLY(10),
	FLD(C2_SDS_CFG, 4, 0, C2_SDS_MODE_GPON),/* select GPON mode (very last)   */
	DLY(50),
};

/* Post-GPON-mode TX-interface reset-B re-sync (DIG_1D[16] 0->1) — stock rev-A
 * OMITS this; gated by luna_c2_postmode_perturb. */
static const struct r960_op c2_sds_txresync[] = {
	FLD(C2_WSDS_DIG_1D, 16, 16, 0),		/* TX interface reset-B 0         */
	DLY(2),
	FLD(C2_WSDS_DIG_1D, 16, 16, 1),		/* TX interface reset-B 1         */
	DLY(10),
};

/* 1 = skip the golden-table writes stock rev-A does not make: the 3 other
 * line-rate pages (SPD / 1P25G / EPON) and the 4 FIB-bank bodies, 134 of the
 * table's 199 writes. The SDS_ANA_GPON page and the FIB PDOWN-clear are kept. */
int luna_c2_minimal_analog;

/* Program the full analog golden table + clear fiber power-down on every FIB
 * bank. Factored so it runs either before or after the SDS reset. */
static void c2_program_analog(const struct luna_ops *o)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(c2_analog); i++) {
		if (luna_c2_minimal_analog && c2_off_overconfig(c2_analog[i].off))
			continue;	/* stock rev-A never writes these (over-configure) */
		o->wr(c2_analog[i].off, c2_analog[i].val);
	}
	for (i = 0; i < ARRAY_SIZE(c2_fib_reg0_banks); i++)
		o->wr(c2_fib_reg0_banks[i],
		      o->rd(c2_fib_reg0_banks[i]) & ~C2_FIB_REG0_PDOWN);
}

static int rtl9602c_ponmac_mode_set(const struct luna_ops *o,
				    int rev, int subtype)
{
	unsigned int i;
	int ret;

	(void)rev; (void)subtype;	/* single rev-A SerDes variant */

	/* Steps 1 + 3 share a write phase around step 2 (the analog table). */
	ret = r960_run(o, c2_sds_pre, ARRAY_SIZE(c2_sds_pre));
	if (ret)
		return ret;

	/* Step 2 (LEGACY placement): program the analog + turn fiber power on BEFORE
	 * the reset, which wipes it. Skipped by default. */
	if (!luna_c2_analog_postreset)
		c2_program_analog(o);

	/* Step 3: pulse the SDS reset. Stock pulses ONLY bit0 (CMD_SDS_RST_PS);
	 * bit7 (CMD_SDS_CFG_RST_PS) stays RMW-latched through bring-up, so it is
	 * applied only when explicitly enabled. */
	if (luna_c2_sds_cfgrst)
		luna_rfwr(o, C2_SW_SOFTWARE_RST, 7, 7, 1);	/* legacy CMD_SDS_CFG_RST_PS */
	ret = r960_run(o, c2_sds_reset, ARRAY_SIZE(c2_sds_reset));
	if (ret)
		return ret;

	/* Step 2 (STOCK rev-A placement, DEFAULT): program the analog golden table
	 * and clear fiber power-down AFTER the reset, so the CMU and GPON CDR hold
	 * their final operating point before the CMU re-locks and before the RX_EN
	 * 0->1 start edge in c2_sds_rx_arm. */
	if (luna_c2_analog_postreset)
		c2_program_analog(o);

	/* Step 4: force the 125M ref clock, optionally let the TX */
	if (luna_c2_skip_rstb_dance) {
		/* Skip the dance: DIG_1D is already 0x1c000 and DIG_00 already 0xf30, and
		 * the SDS reset does not clear them, so an assert->release would be a
		 * gratuitous reset-B pulse on a running serializer. */
		pr_info("luna-gpon: skip interface reset-B dance (DIG_1D=0x%x already released)\n",
			o->rd(C2_WSDS_DIG_1D));
	} else if (luna_c2_clkgate_rstb) {
		/* Synchronous clock-gated reset-B release */
		o->wr(C2_WSDS_DIG_00, C2_WSDS_DIG00_RUN | 1u);	/* STOP_CLK=1 (gate, 0xf31) */
		ret = r960_run(o, c2_sds_rstb + 1, ARRAY_SIZE(c2_sds_rstb) - 1); /* reset-B dance, gated */
		if (ret)
			return ret;
		o->wr(C2_WSDS_DIG_00, C2_WSDS_DIG00_RUN);	/* STOP_CLK=0 (un-gate LAST, 0xf30) */
		mdelay(10);
	} else {
		ret = r960_run(o, c2_sds_rstb, 1);	/* WR DIG_00 = RUN (125M ref forced) */
		if (ret)
			return ret;
		if (luna_c2_cmu_settle_ms)
			mdelay(luna_c2_cmu_settle_ms);
		ret = r960_run(o, c2_sds_rstb + 1, ARRAY_SIZE(c2_sds_rstb) - 1); /* reset-B dance */
		if (ret)
			return ret;
	}
	for (i = 0; i < ARRAY_SIZE(c2_fib_reg0_banks); i++)
		o->wr(c2_fib_reg0_banks[i],
		      o->rd(c2_fib_reg0_banks[i]) & ~C2_FIB_REG0_PDOWN);

	/* Steps 5 + 6: burst-enable + arm the RX-CDR. */
	ret = r960_run(o, c2_sds_rx_arm, ARRAY_SIZE(c2_sds_rx_arm));
	if (ret)
		return ret;

	/* Step 6b + TX drive (modev1/tx_xtra defaults baked off). */
	ret = r960_run(o, c2_sds_tx, ARRAY_SIZE(c2_sds_tx));
	if (ret)
		return ret;

	/* Re-apply the live-stock post-reset SDS_ANA values the golden table set
	 * before the SDS reset wiped them: REG01 = 0x73a4 and REG11 RX_FILT = 0.
	 * Here, post-reset and BEFORE the CFG_SDS_MODE=GPON commit below, so the
	 * serializer locks with the stock-good analog config. */
	if (luna_c2_stock_analog) {
		luna_rfwr(o, C2_SDS_ANA_COM_REG(1),  14, 14, 1); /* REG_BEN_TTL_OUT = 1 (stock) */
		luna_rfwr(o, C2_SDS_ANA_COM_REG(1),   0,  0, 0); /* BENLA_LDOVREF[0] = 0 (stock) */
		luna_rfwr(o, C2_SDS_ANA_COM_REG(11),  7,  0, 0); /* RX_FILT_CONFIG = 0 (stock) */
	}

	/* Step 7a + 7b: force-SD + commit GPON mode. */
	ret = r960_run(o, c2_sds_mode, ARRAY_SIZE(c2_sds_mode));
	if (ret)
		return ret;

	/* The TX reset-B re-sync + the post-mode serdesCdr_reset pulse are the two
	 * perturbations stock rev-A omits; do them only when explicitly enabled. */
	if (luna_c2_postmode_perturb) {
		ret = r960_run(o, c2_sds_txresync, ARRAY_SIZE(c2_sds_txresync));
		if (ret)
			return ret;
		rtl9602c_serdes_cdr_reset(o);
	}

	/* Keep the MAC clock ungated. */
	luna_rfwr(o, C2_WSDS_DIG_00, 0, 0, 0);

	/* Wait for the analog to report ready (FIB_EXT_REG21 bit13); ~200 ms cap.
	 * Return the poll result, matching the stock SerDes-init final return. */
	return r960_run(o, (const struct r960_op[]){
		POLL(C2_FIB_EXT_REG21, C2_SDS_ANALOG_READY, C2_SDS_LOCK_POLL_MAX),
	}, 1);
}

/* NO CALLER TODAY */
int luna_ponmac_init(enum luna_chip chip, int rev, int subtype,
			const struct luna_ops *o)
{
	(void)rev; (void)subtype;	/* per-chip init takes (o) only */
	switch (chip) {
	case LUNA_CHIP_9602C:
		return rtl9602c_ponmac_init(o);
	default:
		return -ENOTSUPP;
	}
}

int luna_ponmac_mode_set(enum luna_chip chip, int rev, int subtype,
			    const struct luna_ops *o)
{
	switch (chip) {
	case LUNA_CHIP_9602C:
		return rtl9602c_ponmac_mode_set(o, rev, subtype);
	default:
		return -ENOTSUPP;
	}
}

int luna_ponmac_serdes_cdr_reset(enum luna_chip chip,
				    const struct luna_ops *o)
{
	switch (chip) {
	case LUNA_CHIP_9602C:
		return rtl9602c_serdes_cdr_reset(o);
	default:
		return -ENOTSUPP;
	}
}
