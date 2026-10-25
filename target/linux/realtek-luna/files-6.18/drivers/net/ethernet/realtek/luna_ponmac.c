// SPDX-License-Identifier: GPL-2.0
/* TIER: FAMILY (prefix luna_) -- silicon shared by one ...
 * dev/MEASURED-luna_ponmac.c.md sec 1. */

#include "luna_ponmac.h"
#include "luna_ponmac_logic.h"	/* hoisted logic */
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/seq_file.h>
#include <linux/bits.h>

/* The opcodes, the step and the interpreter are the core's ...
 * dev/MEASURED-luna_ponmac.c.md sec 14. */
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

/* Per-chip bring-up tables and glue, from each chip's own ...
 * dev/MEASURED-luna_ponmac.c.md sec 2. */
#define C3_SWBASE		0x1b000000u

/* core / SerDes digital + analog block */
#define C3_SOFTWARE_RST		0x1b0000e0u /* global soft-reset command word     */
#define C3_SDS_CFG		0x1b000200u /* SerDes lane mode select            */
#define C3_DYNGASP_CTRL		0x1b00021cu /* dying-gasp comparator control      */
#define C3_P_MISC_PON		0x1b020404u /* P_MISC[pon4]: PpReg 0x20004 + 4*MACPP_INTERVAL(0x100) */
#define C3_PON_INBW_LBOUND	0x1b023180u /* DS in-band accumulation low bound  */
#define C3_WSDS_DIG_00		0x1b040030u /* SerDes digital: clock control      */
#define C3_WSDS_DIG_02		0x1b040038u /* SerDes digital: BEN power-down      */
#define C3_SDS_REG7		0x1b04081cu /* [14] SP_CFG_NEG_CLKWR_A2D          */
#define C3_WSDS_DIG_18		0x1b040090u /* SerDes digital: BEN output enable   */
#define C3_WSDS_DIG_1D		0x1b0400a4u /* SerDes digital: interface FIFO rstb */
#define C3_FORCE_BEN		0x1b0400e4u /* burst-enable force mode             */
#define C3_SDS_ANA_MISC02	0x1b040508u /* analog misc: BER-notify force/value */
#define C3_SDS_ANA_COM03	0x1b04058cu /* analog common: RX CDR / SD-por sel  */
#define C3_SDS_ANA_COM09	0x1b0405a4u /* analog common: BEN CML/TTL drive    */
#define C3_SDS_ANA_COM17	0x1b0405c4u /* analog common: CDR loop Kp          */
#define C3_SDS_ANA_COM20	0x1b0405d0u /* analog common: RX CMU charge-pump   */
#define C3_SDS_ANA_COM21	0x1b0405d4u /* analog common: RX CMU slew / KVCO   */
#define C3_SDS_ANA_COM26	0x1b0405e8u /* analog common: GPHY CMU LDO vref    */
#define C3_SDS_ANA_COM27	0x1b0405ecu /* analog common: GPHY CMU KVCO        */
#define C3_FIB_EXT_REG21	0x1b040e54u /* fiber ext: analog-ready status      */
#define C3_PON_TRAP_CFG		0x1b0110ecu /* OMCI/MPCP trap priority            */
/* PON-IP block */
#define C3_PON_SIDVALID		0x1bf0218cu /* per-flow SID-valid bitmap (1b/elem) */
#define C3_PON_BW_THRES		0x1bf021a0u /* upstream BW request thresholds     */
#define C3_PON_OMCI_CFG		0x1bf021a4u /* OMCI flow/SID select               */
#define C3_PON_SCH_CTRL		0x1bf021e4u /* scheduler control                  */
#define C3_PON_SID2QID		0x1bf0210cu /* flow(SID) -> physical queue (7-bit/elem) */

/* fixed chip parameters for the GPON datapath */
#define C3_SID_COUNT		128	/* classifier SID / flow slots          */
#define C3_OMCI_FLOW		127	/* flow id reserved for OMCI            */
/* Parking on 127 would alias every SID that becomes valid without its own qid
 * write onto the OMCI queue. This die's DAL parks on T-CONT 15 / queue 6 and
 * refuses physicalQid 127 for a non-OMCI flow (2026-09-10). */
#define C3_SCRATCH_QID		126	/* T-CONT 15 / queue 6, per the chip DAL */

/* SID-valid bitmap is packed 1 bit per flow: word = base + (idx/32)*4, bit idx%32 */
static inline void c3_sidvalid(const struct luna_ops *o, u32 idx, u32 v)
{
	u8 b = idx & 31u;

	luna_rfwr(o, C3_PON_SIDVALID + (idx >> 5) * 4u, b, b, v);
}

/* SID2QID: 7-bit physical-queue field per flow, 4 flows per 32-bit word */
static void c3_flow2queue(const struct luna_ops *o, u32 flow, u32 pqid)
{
	u32 lsb = (flow % 4u) * 7u;

	luna_rfwr(o, C3_PON_SID2QID + (flow / 4u) * 4u, lsb + 6u, lsb, pqid);
}

/* ponmac_init: PON-MAC global defaults applied once before ...
 * dev/MEASURED-luna_ponmac.c.md sec 15. */
static const struct r960_op c3_init[] = {
	FLD(C3_SDS_ANA_COM09,  0,  0, 1),	/* BEN drive: TTL output enabled  */
	FLD(C3_PON_BW_THRES,  29, 16, 5),	/* US last-grant BW threshold     */
	FLD(C3_PON_BW_THRES,  13,  0, 5),	/* US runt BW request threshold   */
	FLD(C3_PON_SCH_CTRL,  18, 18, 1),	/* drop on PIR overflow           */
	FLD(C3_PON_TRAP_CFG,   2,  0, 7),	/* OMCI/MPCP trap = top priority  */
	FLD(C3_DYNGASP_CTRL,   3,  3, 1),	/* invert dying-gasp comparator   */
};

/*
 * GPON SerDes/PON-MAC bring-up phase 1: analog pre-config with the lane held
 * off, ending before the lane is switched into GPON mode.
 */
static const struct r960_op c3_sds_pre[] = {
	/* SDS_REG7[14] SP_CFG_NEG_CLKWR_A2D is PER-CHIP: the RTL9602C ...
	 * dev/MEASURED-luna_ponmac.c.md sec 3. */
	FLD(C3_SDS_REG7,      14, 14, 1),	/* A2D clock edge (9603CVD = 1)   */
	FLD(C3_SDS_CFG,        4,  0, 0x1f),	/* lane mode: off (parked)        */
	FLD(C3_WSDS_DIG_00,    4,  4, 1),	/* force 125 MHz reference clock   */
	FLD(C3_WSDS_DIG_02,   10, 10, 0),	/* clear BEN power-down            */
	FLD(C3_SDS_ANA_COM03, 13, 13, 0),	/* RX CDR AFE: deselect            */
	FLD(C3_SDS_ANA_COM09,  4,  4, 0),	/* BEN driver: CML off             */
	FLD(C3_SDS_ANA_COM09,  0,  0, 1),	/* BEN driver: TTL output on       */
	FLD(C3_SDS_ANA_COM17, 15, 10, 0xc),	/* CDR loop proportional gain Kp   */
	FLD(C3_SDS_ANA_COM20, 11,  7, 0x1b),	/* RX CMU charge-pump current      */
	FLD(C3_SDS_ANA_COM20,  3,  2, 0x3),	/* RX CMU LDO reference            */
	FLD(C3_SDS_ANA_COM21, 13, 11, 0x2),	/* RX CMU slew rate                */
	FLD(C3_SDS_ANA_COM21,  6,  3, 0x4),	/* RX VCO gain band select         */
	FLD(C3_SDS_ANA_COM26,  3,  2, 0x3),	/* GPHY CMU LDO reference          */
	FLD(C3_SDS_ANA_COM27,  6,  3, 0x4),	/* GPHY VCO gain band select       */
};

/*
 * Phase 2: commit GPON mode and pulse the resets. BER-notify is released so the
 * reset takes, then re-armed so a later signal-detect drop cannot fell the MAC.
 */
static const struct r960_op c3_sds_mode[] = {
	FLD(C3_SDS_CFG,        4,  0, 0x8),	/* lane mode: GPON                 */
	FLD(C3_SDS_ANA_MISC02,12, 12, 0),	/* release BER-notify force         */
	FLD(C3_SOFTWARE_RST,   2,  0, 1),	/* reset SerDes + GPON MAC          */
	DLY(10),				/* let the reset settle             */
	FLD(C3_SDS_ANA_MISC02,13, 13, 1),	/* BER-notify hold value = 1        */
	FLD(C3_SDS_ANA_MISC02,12, 12, 1),	/* re-force BER-notify (MAC stays up)*/
	FLD(C3_SOFTWARE_RST,  10, 10, 1),	/* switch-core reset on mode change */
	DLY(10),				/* let the switch reset settle      */
};

/*
 * Phase 3: re-enable the datapath after the resets -- interface FIFO release-B,
 * burst-enable output, undersize frames on the PON port, BEN force mode off.
 */
static const struct r960_op c3_sds_post[] = {
	FLD(C3_WSDS_DIG_1D,   16, 16, 0),	/* TX interface FIFO: assert rstb   */
	FLD(C3_WSDS_DIG_1D,   16, 16, 1),	/* TX interface FIFO: release rstb  */
	FLD(C3_WSDS_DIG_1D,   15, 15, 0),	/* RX interface FIFO: assert rstb   */
	FLD(C3_WSDS_DIG_1D,   15, 15, 1),	/* RX interface FIFO: release rstb  */
	FLD(C3_WSDS_DIG_18,   12, 12, 1),	/* burst-enable output: on          */
	/* The optic-LOS force MUST be released and this chip's table never did.
	 * CFG_FRC_OPTIC_LOS=1 makes the GTC substitute CFG_FRCV_OPTIC_LOS for the
	 * pad, so FRC=1/FRCV=1 pins OPTIC_LOS_SIG at 1 -- an unfalsifiable "no
	 * downstream light" no real light can clear, and the FSM never leaves O1. */
	FLD(C3_WSDS_DIG_18,   15, 15, 0),	/* OPTIC_LOS_SEL_EPON = 0 (GPON)    */
	FLD(C3_WSDS_DIG_18,   14, 14, 0),	/* CFG_FRC_OPTIC_LOS  = 0 (use pad) */
	FLD(C3_WSDS_DIG_18,   13, 13, 0),	/* CFG_FRCV_OPTIC_LOS = 0           */
	/* COM03[13] REG_RX_SEL_CDR_AFEN is deliberately NOT re-armed here: stock on
	 * this board rests it at 0 at O5 (SDS_ANA_COM03 = 0x1929) with SDS_SDET
	 * asserted, so the value c3_sds_pre leaves is also stock's. */
	FLD(C3_P_MISC_PON,     2,  2, 1),	/* PON port: accept undersize       */
	FLD(C3_FORCE_BEN,      0,  0, 0),	/* burst-enable force mode: off     */
};

/* the analog CDR / SD-power-on select bit, bit 10 of the analog common word */
#define LUNA_SDS_ANA_CDR_SEL	BIT(10)

/* CDR re-seat, one owner for every Luna part: pulse the ...
 * dev/MEASURED-luna_ponmac.c.md sec 4. */
static int luna_cdr_reset(const struct luna_ops *o, u32 ana_reg, u32 wsds_reg)
{
	u32 v = o->rd(ana_reg);

	o->wr(ana_reg, v ^ LUNA_SDS_ANA_CDR_SEL);
	mdelay(10);
	o->wr(ana_reg, v);			/* restore the original word */

	luna_rfwr(o, wsds_reg, 14, 14, 0);	/* transfer FIFO: assert rstb  */
	mdelay(10);
	luna_rfwr(o, wsds_reg, 14, 14, 1);	/* transfer FIFO: release rstb */
	return 0;
}

static int c3_cdr_reset(const struct luna_ops *o)
{
	return luna_cdr_reset(o, C3_SDS_ANA_COM03, C3_WSDS_DIG_1D);
}

/* GPON bring-up driver: pre-config tables + flow/OMCI wiring + analog gate. */
static int c3_gpon_mode_set(const struct luna_ops *o)
{
	u32 f;
	int rc;

	/* park every data flow's SID as invalid + map to the default queue; the
	 * OMCI flow is armed afterwards. Without the SID2QID map the OMCI SID is
	 * valid but bound to no queue and no traffic passes. */
	for (f = 0; f < C3_SID_COUNT - 1u; f++) {
		c3_sidvalid(o, f, 0);
		c3_flow2queue(o, f, C3_SCRATCH_QID);
	}
	c3_sidvalid(o, C3_OMCI_FLOW, 1);		/* OMCI flow: SID valid    */
	c3_flow2queue(o, C3_OMCI_FLOW, C3_OMCI_FLOW);	/* OMCI flow -> queue 127  */
	luna_rfwr(o, C3_PON_OMCI_CFG, 6, 0, C3_OMCI_FLOW); /* OMCI SID select   */

	rc = r960_run(o, c3_sds_pre,  ARRAY_SIZE(c3_sds_pre));
	if (rc)
		return rc;
	rc = r960_run(o, c3_sds_mode, ARRAY_SIZE(c3_sds_mode));
	if (rc)
		return rc;
	rc = r960_run(o, c3_sds_post, ARRAY_SIZE(c3_sds_post));
	if (rc)
		return rc;

	/*
	 * Wait for analog-ready (FIB_EXT_REG21 bit 13), then drop the forced 125 MHz
	 * reference to save power. It stays on if the gate never asserts.
	 */
	rc = r960_run(o, (const struct r960_op[]){
		POLL(C3_FIB_EXT_REG21, 13, 1000),
	}, 1);
	if (rc == 0)
		luna_rfwr(o, C3_WSDS_DIG_00, 4, 4, 0); /* 125 MHz reference: off */

	/* DS in-band accumulation low bound for PBO */
	luna_rfwr(o, C3_PON_INBW_LBOUND, 23, 0, 0xfda000);

	return rc;
}

/* RTL9603CVD top-level entry points (thin wrappers over the c3_* internals). */
static int rtl9603cvd_ponmac_init(const struct luna_ops *o)
{
	return r960_run(o, c3_init, ARRAY_SIZE(c3_init));
}

static int rtl9603cvd_ponmac_mode_set(const struct luna_ops *o,
				      int rev, int subtype)
{
	(void)rev; (void)subtype;	/* single SerDes variant for every rev */
	return c3_gpon_mode_set(o);
}

static int rtl9603cvd_serdes_cdr_reset(const struct luna_ops *o)
{
	return c3_cdr_reset(o);
}

/* RTL9607C. SerDes here is DIRECT MMIO: every analog/digital ...
 * dev/MEASURED-luna_ponmac.c.md sec 5. */
#define C7_PON_SIDVALID		0x1bf02188u /* per-flow SID-valid bitmap (1b/elem)  */
#define C7_PON_OMCI_CFG		0x1bf021a0u /* OMCI flow/SID select                 */
#define C7_PON_BW_THRES		0x1bf0219cu /* upstream BW request thresholds       */
#define C7_PON_SCH_CTRL		0x1bf021e0u /* scheduler control                    */
#define C7_DRN_CMD		0x1bf020f4u /* T-cont drain command / status        */
#define C7_IO_CMD_0_US		0x1bf05434u /* upstream NIC GMII TX/RX enables       */
#define C7_PON_SID2QID		0x1bf02108u /* flow(SID) -> physical queue map       */
#define C7_PON_QID_CIR_RATE	0x1bf021e4u /* per-queue committed (CIR) rate        */
#define C7_PON_QID_PIR_RATE	0x1bf023e4u /* per-queue peak (PIR) rate             */
#define C7_PON_SCH_QMAP		0x1bf025e4u /* per-tcont queue membership mask       */
#define C7_PON_WFQ_TYPE		0x1bf02668u /* per-queue strict/WFQ select           */
#define C7_PON_WFQ_WEIGHT	0x1bf0267cu /* per-queue WFQ weight                  */
#define C7_PON_TCONT_EN		0x1bf02664u /* per-tcont schedule enable             */

/* PON trap / accept-length (0x1b011xxx) */
#define C7_PON_TRAP_CFG		0x1b011144u /* OMCI/MPCP trap priority              */
#define C7_ACCEPT_MAX_LEN	0x1b011028u /* per-port accept max length (stride 4) */

/* switch global (0x1b000xxx / 0x1b002xxx) */
#define C7_SOFTWARE_RST		0x1b000108u /* soft-reset: SW core / SerDes+GPON-MAC */
#define C7_DYNGASP_CTRL		0x1b00029cu /* dying-gasp comparator control         */
#define C7_SDS_CFG		0x1b000270u /* SerDes lane mode select               */
#define C7_PON_INBW_LBOUND	0x1b023288u /* DS in-band accumulation low bound     */
#define C7_P_MISC_PON		0x1b020504u /* per-port misc, PON port (base 0x20004 + port5*0x100) */

/* SerDes digital block (0x1b040xxx) */
#define C7_WSDS_DIG_00		0x1b040030u /* SerDes digital: 125 MHz clock control */
#define C7_WSDS_DIG_02		0x1b040038u /* SerDes digital: BEN power-down        */
#define C7_WSDS_DIG_03		0x1b04003cu /* SerDes digital: TX-disable sel delay  */
#define C7_WSDS_DIG_18		0x1b040090u /* SerDes digital: BEN output enable     */
#define C7_WSDS_DIG_1D		0x1b0400a4u /* SerDes digital: interface FIFO rstb   */
#define C7_FORCE_BEN		0x1b0400e4u /* burst-enable force mode               */

/* SerDes analog common / GPON / misc (0x1b0405xx..0x1b0407xx, 0x1b040exx) */
#define C7_SDS_ANA_MISC02	0x1b040508u /* analog misc: BER-notify force/value   */
#define C7_SDS_ANA_COM00	0x1b040580u /* analog common: CDR Kd (rev-B)         */
#define C7_SDS_ANA_COM02	0x1b040588u /* analog common: CDR Ki/Kp1/Kp2         */
#define C7_SDS_ANA_COM05	0x1b040594u /* analog common: RX EQ hold             */
#define C7_SDS_ANA_COM06	0x1b040598u /* analog common: RX filter / RX EQ in   */
#define C7_SDS_ANA_COM08	0x1b0405a0u /* analog common: RX Kp1_2 / Kp2_2       */
#define C7_SDS_ANA_COM09	0x1b0405a4u /* analog common: RX CDR/timer/re-seat   */
#define C7_SDS_ANA_COM12	0x1b0405b0u /* analog common: RX EQ2 select          */
#define C7_SDS_ANA_COM13	0x1b0405b4u /* analog common: TX amplitude           */
#define C7_SDS_ANA_COM14	0x1b0405b8u /* analog common: TX emphasis / Z0 P-adj */
#define C7_SDS_ANA_COM15	0x1b0405bcu /* analog common: Z0 N-adjust            */
#define C7_SDS_ANA_COM17	0x1b0405c4u /* analog common: BEN CML/TTL drive      */
#define C7_SDS_ANA_COM21	0x1b0405d4u /* analog common: RX CMU CCO/CP/KVCO/LPF */
#define C7_SDS_ANA_COM23	0x1b0405dcu /* analog common: CMU watchdog (RX)      */
#define C7_SDS_ANA_COM24	0x1b0405e0u /* analog common: TX CMU CP / LPF-CP     */
#define C7_SDS_ANA_COM25	0x1b0405e4u /* analog common: TX CMU LPF-RS / LC byp */
#define C7_SDS_ANA_COM26	0x1b0405e8u /* analog common: CMU watchdog (TX)      */
#define C7_SDS_ANA_COM30	0x1b0405f8u /* analog common: GPHY CMU CP/ICP/LPF-CP */
#define C7_SDS_ANA_COM31	0x1b0405fcu /* analog common: GPHY CMU LPF-RS        */
#define C7_SDS_ANA_GPON34	0x1b040708u /* analog GPON: GPHY CMU watchdog        */
#define C7_SDS_ANA_GPON36	0x1b040710u /* analog GPON: GPHY field lock-dn limit */
#define C7_SDS_ANA_GPON37	0x1b040714u /* analog GPON: GPHY dly-clk/lock-up lim */
#define C7_SDS_ANA_GPON43	0x1b04072cu /* analog GPON: TX delay-clock select    */
#define C7_FIB_EXT_REG21	0x1b040e54u /* fiber ext: analog-ready status        */
/* Read-only here, and deliberately so (2026-09-11). The three ...
 * dev/MEASURED-luna_ponmac.c.md sec 6. */
#define C7_FIB_REG0		0x1b040c00u /* [11] FP_CFG_FIB_PDOWN; diag read only */

/* fixed chip parameters for the GPON datapath */
#define C7_PON_PORT		5	/* PON port index for per-port registers */
#define C7_MACPP_STRIDE		0x100u	/* per-port MAC register stride          */
#define C7_SID_COUNT		128	/* classifier SID / flow slots           */
#define C7_GPON_TCONT_MAX	32	/* T-cont count                          */
#define C7_PON_QUEUE_MAX	128	/* physical PON queue count              */
#define C7_TCONT_QUEUE_MAX	32	/* queues per T-cont scheduler           */
#define C7_RATE_MAX		0x3ffffu/* CIR/PIR rate saturation value         */
#define C7_OMCI_FLOW		127	/* flow id reserved for OMCI            */
#define C7_OMCI_TCONT		31	/* T-cont id for the OMCI flow           */
#define C7_OMCI_QUEUE		24	/* queue id for the OMCI flow            */

/* one-shot init guard: drain T-conts only on a re-init */
static int c7_init_done;

/* Packed/strided array element write. For arroff<32 one ...
 * dev/MEASURED-luna_ponmac.c.md sec 16. */
static void c7_arr(const struct luna_ops *o, u32 base, u32 arroff,
		   u32 idx, u32 lsp, u32 len, u32 val)
{
	u32 phys, lsb;

	if (arroff % 32u) {
		u32 per_word = 32u / arroff;

		phys = base + (idx / per_word) * 4u;
		lsb  = (idx % per_word) * arroff + lsp;
	} else {
		phys = base + idx * (arroff / 8u);
		lsb  = lsp;
	}
	luna_rfwr(o, phys, lsb + len - 1u, lsb, val);
}

/* GPON physical queue id = TCONT_QUEUE_MAX*(sched/8) + logical queue */
static void c7_flow2queue(const struct luna_ops *o, u32 flow, u32 sched, u32 q)
{
	c7_arr(o, C7_PON_SID2QID, 7, flow, 0, 7,
	       C7_TCONT_QUEUE_MAX * (sched / 8u) + q);
}

/* CIR/PIR are stored as (rate-1) except for the 0/1/max sentinels */
static u32 c7_rate(u32 rate)
{
	if (rate != 0 && rate != 1 && rate != C7_RATE_MAX)
		return rate - 1u;
	return rate;
}

/* Drain one T-cont, busy-polling the drain flag. On timeout, recover the
 * upstream NIC by toggling its GMII enables (RX off, TX off->on, RX on). */
static void c7_tcont_drain(const struct luna_ops *o, u32 tcont)
{
	u32 i;

	/* queue-mode=0, drain-index=tcont, drain-pulse=1 */
	o->wr(C7_DRN_CMD, ((tcont & 0x7fu) << 3) | (1u << 1));

	for (i = 0; i < 200000u; i++)
		if (!(o->rd(C7_DRN_CMD) & 0x1u))	/* drain flag cleared */
			break;

	if (i >= 200000u) {
		luna_rfwr(o, C7_IO_CMD_0_US, 5, 5, 0);	/* US NIC RX off */
		luna_rfwr(o, C7_IO_CMD_0_US, 4, 4, 0);	/* US NIC TX off */
		luna_rfwr(o, C7_IO_CMD_0_US, 4, 4, 1);	/* US NIC TX on  */
		luna_rfwr(o, C7_IO_CMD_0_US, 5, 5, 1);	/* US NIC RX on  */
	}
}

/*
 * ponmac_init: PON-MAC global defaults applied once before mode selection.
 * (rev>A would also init switch-PBO; that lives in a separate subsystem.)
 */
static int c7_ponmac_init(const struct luna_ops *o, int rev, int subtype)
{
	u32 i;

	(void)subtype; (void)rev;

	luna_rfwr(o, C7_SDS_ANA_COM17, 0, 0, 1);	/* BEN TTL output on    */
	luna_rfwr(o, C7_PON_BW_THRES, 29, 16, 5);	/* US last-grant thresh */
	luna_rfwr(o, C7_PON_BW_THRES, 13,  0, 5);	/* US runt-request thresh*/

	if (c7_init_done)
		for (i = 0; i < C7_GPON_TCONT_MAX; i++)
			c7_tcont_drain(o, i);

	for (i = 0; i < C7_GPON_TCONT_MAX - 1u; i++) {
		c7_arr(o, C7_PON_TCONT_EN, 1, i, 0, 1, 0);	/* T-cont disable */
		c7_arr(o, C7_PON_SCH_QMAP, 32, i, 0, 32, 0);	/* clear queue mask*/
	}

	luna_rfwr(o, C7_PON_SCH_CTRL, 18, 18, 1);	/* drop on PIR overflow */

	for (i = 0; i < C7_PON_QUEUE_MAX; i++) {
		c7_arr(o, C7_PON_WFQ_TYPE,    1,  i, 0,  1, 0);			/* strict   */
		c7_arr(o, C7_PON_QID_CIR_RATE,18, i, 0, 18, c7_rate(0));		/* CIR = 0  */
		c7_arr(o, C7_PON_QID_PIR_RATE,18, i, 0, 18, c7_rate(C7_RATE_MAX));/* PIR = max*/
		c7_arr(o, C7_PON_WFQ_WEIGHT,  10, i, 0, 10, 1);			/* weight=1 */
	}

	luna_rfwr(o, C7_PON_TRAP_CFG, 2, 0, 7);	/* OMCI/MPCP top priority */
	luna_rfwr(o, C7_DYNGASP_CTRL, 3, 3, 1);	/* invert dying-gasp cmp  */

	c7_init_done = 1;
	return 0;
}

/* Shared GPON SID/OMCI front matter, identical before each ...
 * dev/MEASURED-luna_ponmac.c.md sec 17. */
static void c7_gpon_pre(const struct luna_ops *o)
{
	u32 f;

	for (f = 0; f < C7_SID_COUNT - 1u; f++) {
		c7_flow2queue(o, f, 15, 31);
		c7_arr(o, C7_PON_SIDVALID, 1, f, 0, 1, 0);
	}
	c7_flow2queue(o, C7_OMCI_FLOW, C7_OMCI_TCONT, C7_OMCI_QUEUE);
	c7_arr(o, C7_PON_SIDVALID, 1, C7_OMCI_FLOW, 0, 1, 1);
	luna_rfwr(o, C7_PON_OMCI_CFG, 6, 0, C7_OMCI_FLOW);
}

/* Shared GPON tail, identical after each rev's SerDes patch. ...
 * dev/MEASURED-luna_ponmac.c.md sec 18. */
static int c7_gpon_post(const struct luna_ops *o)
{
	u32 i;

	luna_rfwr(o, C7_SOFTWARE_RST, 10, 10, 1);	/* switch-core reset */
	mdelay(10);

	luna_rfwr(o, C7_WSDS_DIG_1D, 16, 16, 0);	/* TX iface FIFO assert rstb  */
	luna_rfwr(o, C7_WSDS_DIG_1D, 16, 16, 1);	/* TX iface FIFO release rstb */
	luna_rfwr(o, C7_WSDS_DIG_1D, 15, 15, 0);	/* RX iface FIFO assert rstb  */
	luna_rfwr(o, C7_WSDS_DIG_1D, 15, 15, 1);	/* RX iface FIFO release rstb */

	luna_rfwr(o, C7_WSDS_DIG_18, 12, 12, 1);	/* BEN output on        */
	luna_rfwr(o, C7_P_MISC_PON, 2, 2, 1);	/* PON port accept undersize */
	luna_rfwr(o, C7_FORCE_BEN, 0, 0, 0);		/* BEN force mode off   */
	luna_rfwr(o, C7_ACCEPT_MAX_LEN + C7_PON_PORT * 4u, 13, 0, 2031); /* max len */

	for (i = 0; i < 10000u; i++) {		/* wait analog-ready (V2ANALOG) */
		if ((o->rd(C7_FIB_EXT_REG21) >> 13) & 0x1u)
			break;
		udelay(200);
	}
	if (i < 10000u)
		luna_rfwr(o, C7_WSDS_DIG_00, 4, 4, 0);	/* 125 MHz clock off */

	luna_rfwr(o, C7_PON_INBW_LBOUND, 23, 0, 0xfda000);	/* DS in-band lbound */
	return 0;
}

/* rev-A SerDes patch (mode V1): park the lane, force the 125 ...
 * dev/MEASURED-luna_ponmac.c.md sec 19. */
static const struct r960_op c7_sds_v1[] = {
	FLD(C7_SDS_CFG,        4,  0, 0x1f),	/* lane mode: off (parked)        */
	FLD(C7_WSDS_DIG_00,    4,  4, 0x1),	/* force 125 MHz reference clock   */
	FLD(C7_WSDS_DIG_02,   10, 10, 0x0),	/* clear BEN power-down            */
	FLD(C7_WSDS_DIG_03,    6,  4, 0x2),	/* TX-disable select delay         */
	FLD(C7_SDS_ANA_COM17,  4,  4, 0x0),	/* BEN driver: CML off             */
	FLD(C7_SDS_ANA_COM17,  0,  0, 0x1),	/* BEN driver: TTL output on       */
	FLD(C7_SDS_ANA_COM24, 14, 11, 0xF),	/* TX CMU charge-pump              */
	FLD(C7_SDS_ANA_COM24,  3,  1, 0x0),	/* TX CMU LPF charge-pump          */
	FLD(C7_SDS_ANA_COM25, 15, 13, 0x7),	/* TX CMU LPF resistor             */
	FLD(C7_SDS_ANA_COM25,  1,  1, 0x0),	/* LC bypass off                   */
	FLD(C7_SDS_ANA_COM21, 15, 15, 0x1),	/* RX CMU CCO select               */
	FLD(C7_SDS_ANA_COM21, 14, 11, 0xC),	/* RX CMU charge-pump              */
	FLD(C7_SDS_ANA_COM21,  6,  6, 0x0),	/* RX CMU big-KVCO off             */
	FLD(C7_SDS_ANA_COM21,  4,  2, 0x3),	/* RX CMU LPF resistor             */
	FLD(C7_SDS_ANA_COM09, 13, 13, 0x0),	/* RX CDR AFE deselect             */
	FLD(C7_SDS_ANA_COM06,  7,  0, 0x2),	/* RX filter config                */
	FLD(C7_SDS_ANA_COM02, 15, 13, 0x1),	/* CDR Ki                          */
	FLD(C7_SDS_ANA_COM02, 12, 10, 0x4),	/* CDR Kp1                         */
	FLD(C7_SDS_ANA_COM02,  9,  7, 0x4),	/* CDR Kp2                         */
	FLD(C7_SDS_ANA_COM08, 14, 12, 0x4),	/* RX Kp1_2                        */
	FLD(C7_SDS_ANA_COM08, 11,  9, 0x4),	/* RX Kp2_2                        */
	FLD(C7_SDS_ANA_COM12,  7,  4, 0x1),	/* RX EQ2 select                   */
	FLD(C7_SDS_ANA_COM12,  3,  0, 0x1),	/* RX EQ2 select 2                 */
	FLD(C7_SDS_ANA_COM13,  7,  5, 0x2),	/* TX amplitude                    */
	FLD(C7_SDS_ANA_COM14, 11,  9, 0x0),	/* TX emphasis                     */
	FLD(C7_SDS_ANA_COM14,  8,  8, 0x1),	/* TX emphasis enable              */
	FLD(C7_SDS_CFG,        4,  0, 0x8),	/* lane mode: GPON                 */
	FLD(C7_SDS_ANA_MISC02,12, 12, 0x0),	/* release BER-notify force        */
	FLD(C7_SOFTWARE_RST,   2,  0, 0x1),	/* reset SerDes + GPON MAC         */
	DLY(10),				/* let the reset settle            */
	FLD(C7_SDS_ANA_MISC02,13, 13, 0x1),	/* BER-notify hold value = 1       */
	FLD(C7_SDS_ANA_MISC02,12, 12, 0x1),	/* re-force BER-notify              */
};

/* rev-B SerDes patch (mode V2): V1's layout with retuned CDR/RX gains plus a
 * CDR Kd write that only exists on this rev. */
static const struct r960_op c7_sds_v2[] = {
	FLD(C7_SDS_CFG,        4,  0, 0x1f),	/* lane mode: off (parked)        */
	FLD(C7_WSDS_DIG_00,    4,  4, 0x1),	/* force 125 MHz reference clock   */
	FLD(C7_WSDS_DIG_02,   10, 10, 0x0),	/* clear BEN power-down            */
	FLD(C7_WSDS_DIG_03,    6,  4, 0x2),	/* TX-disable select delay         */
	FLD(C7_SDS_ANA_COM17,  4,  4, 0x0),	/* BEN driver: CML off             */
	FLD(C7_SDS_ANA_COM17,  0,  0, 0x1),	/* BEN driver: TTL output on       */
	FLD(C7_SDS_ANA_COM24, 14, 11, 0xF),	/* TX CMU charge-pump              */
	FLD(C7_SDS_ANA_COM24,  3,  1, 0x0),	/* TX CMU LPF charge-pump          */
	FLD(C7_SDS_ANA_COM25, 15, 13, 0x7),	/* TX CMU LPF resistor             */
	FLD(C7_SDS_ANA_COM25,  1,  1, 0x0),	/* LC bypass off                   */
	FLD(C7_SDS_ANA_COM21, 15, 15, 0x1),	/* RX CMU CCO select               */
	FLD(C7_SDS_ANA_COM21, 14, 11, 0xC),	/* RX CMU charge-pump              */
	FLD(C7_SDS_ANA_COM21,  6,  6, 0x0),	/* RX CMU big-KVCO off             */
	FLD(C7_SDS_ANA_COM21,  4,  2, 0x3),	/* RX CMU LPF resistor             */
	FLD(C7_SDS_ANA_COM09, 13, 13, 0x0),	/* RX CDR AFE deselect             */
	FLD(C7_SDS_ANA_COM06,  7,  0, 0x2),	/* RX filter config                */
	FLD(C7_SDS_ANA_COM02, 15, 13, 0x1),	/* CDR Ki                          */
	FLD(C7_SDS_ANA_COM02, 12, 10, 0x0),	/* CDR Kp1 (rev-B)                 */
	FLD(C7_SDS_ANA_COM02,  9,  7, 0x6),	/* CDR Kp2 (rev-B)                 */
	FLD(C7_SDS_ANA_COM08, 14, 12, 0x1),	/* RX Kp1_2 (rev-B)                */
	FLD(C7_SDS_ANA_COM08, 11,  9, 0x1),	/* RX Kp2_2 (rev-B)                */
	FLD(C7_SDS_ANA_COM12,  7,  4, 0x1),	/* RX EQ2 select                   */
	FLD(C7_SDS_ANA_COM12,  3,  0, 0x1),	/* RX EQ2 select 2                 */
	FLD(C7_SDS_ANA_COM13,  7,  5, 0x2),	/* TX amplitude                    */
	FLD(C7_SDS_ANA_COM14, 11,  9, 0x0),	/* TX emphasis                     */
	FLD(C7_SDS_ANA_COM14,  8,  8, 0x1),	/* TX emphasis enable              */
	FLD(C7_SDS_ANA_COM00,  1,  1, 0x0),	/* CDR Kd (rev-B only)             */
	FLD(C7_SDS_CFG,        4,  0, 0x8),	/* lane mode: GPON                 */
	FLD(C7_SDS_ANA_MISC02,12, 12, 0x0),	/* release BER-notify force        */
	FLD(C7_SOFTWARE_RST,   2,  0, 0x1),	/* reset SerDes + GPON MAC         */
	DLY(10),				/* let the reset settle            */
	FLD(C7_SDS_ANA_MISC02,13, 13, 0x1),	/* BER-notify hold value = 1       */
	FLD(C7_SDS_ANA_MISC02,12, 12, 0x1),	/* re-force BER-notify              */
};

/*
 * rev-C+ SerDes patch (mode V3): GPHY-CMU-centric tuning -- the per-lane CMU
 * watchdogs off, TX/RX and GPHY CMU retuned, then GPON and reset as the others.
 */
static const struct r960_op c7_sds_v3[] = {
	FLD(C7_SDS_CFG,        4,  0, 0x1f),	/* lane mode: off (parked)        */
	FLD(C7_WSDS_DIG_00,    4,  4, 0x1),	/* force 125 MHz reference clock   */
	FLD(C7_WSDS_DIG_02,   10, 10, 0x0),	/* clear BEN power-down            */
	FLD(C7_WSDS_DIG_03,    6,  4, 0x2),	/* TX-disable select delay         */
	FLD(C7_SDS_ANA_GPON43,11, 11, 0x1),	/* TX delay-clock select           */
	FLD(C7_SDS_ANA_COM26,  3,  3, 0x0),	/* TX CMU watchdog off             */
	FLD(C7_SDS_ANA_COM23, 15, 15, 0x0),	/* RX CMU watchdog off             */
	FLD(C7_SDS_ANA_GPON34, 7,  7, 0x0),	/* GPHY CMU watchdog off           */
	FLD(C7_SDS_ANA_COM24, 14, 11, 0x4),	/* TX CMU charge-pump              */
	FLD(C7_SDS_ANA_COM24,  3,  1, 0x1),	/* TX CMU LPF charge-pump          */
	FLD(C7_SDS_ANA_COM25, 15, 13, 0x3),	/* TX CMU LPF resistor             */
	FLD(C7_SDS_ANA_COM14,  4,  0, 0x7),	/* Z0 P-adjust                     */
	FLD(C7_SDS_ANA_COM15, 15, 12, 0x8),	/* Z0 N-adjust                     */
	FLD(C7_SDS_ANA_COM02, 15, 13, 0x6),	/* CDR Ki                          */
	FLD(C7_SDS_ANA_COM02, 12, 10, 0x1),	/* CDR Kp1                         */
	FLD(C7_SDS_ANA_COM02,  9,  7, 0x0),	/* CDR Kp2                         */
	FLD(C7_SDS_ANA_COM13,  7,  5, 0x2),	/* TX amplitude                    */
	FLD(C7_SDS_ANA_COM05,  2,  2, 0x1),	/* RX EQ hold                      */
	FLD(C7_SDS_ANA_COM06, 15,  9, 0x40),	/* RX EQ input                     */
	FLD(C7_SDS_ANA_COM09,  6,  2, 0x1f),	/* RX timer-BER                    */
	FLD(C7_SDS_ANA_GPON37, 5,  5, 0x1),	/* GPHY delay-clock select         */
	FLD(C7_SDS_ANA_GPON37,15,  6, 0x316),	/* GPHY lock-up limit              */
	FLD(C7_SDS_ANA_COM30, 15, 12, 0x3),	/* GPHY CMU charge-pump            */
	FLD(C7_SDS_ANA_COM30, 10, 10, 0x1),	/* GPHY CMU ICP low-BW             */
	FLD(C7_SDS_ANA_COM30,  4,  2, 0x2),	/* GPHY CMU LPF charge-pump        */
	FLD(C7_SDS_ANA_COM31, 15, 13, 0x0),	/* GPHY CMU LPF resistor           */
	FLD(C7_SDS_ANA_GPON36,15,  6, 0x302),	/* GPHY lock-down limit            */
	FLD(C7_SDS_CFG,        4,  0, 0x8),	/* lane mode: GPON                 */
	FLD(C7_SDS_ANA_MISC02,12, 12, 0x0),	/* release BER-notify force        */
	FLD(C7_SOFTWARE_RST,   2,  0, 0x1),	/* reset SerDes + GPON MAC         */
	DLY(10),				/* let the reset settle            */
	FLD(C7_SDS_ANA_MISC02,13, 13, 0x1),	/* BER-notify hold value = 1       */
	FLD(C7_SDS_ANA_MISC02,12, 12, 0x1),	/* re-force BER-notify              */
};

/* GPON mode-set: the common front matter, the rev-selected SerDes patch table,
 * then the common tail. rev A->V1, B->V2, C and later->V3. */
static int c7_gpon_mode_set(const struct luna_ops *o, int rev, int subtype)
{
	const struct r960_op *sds;
	unsigned int n;
	int rc;

	(void)subtype;

	c7_gpon_pre(o);

	switch (rev) {
	case LUNA_REV_A:
		sds = c7_sds_v1; n = ARRAY_SIZE(c7_sds_v1); break;
	case LUNA_REV_B:
		sds = c7_sds_v2; n = ARRAY_SIZE(c7_sds_v2); break;
	default:
		sds = c7_sds_v3; n = ARRAY_SIZE(c7_sds_v3); break;
	}

	rc = r960_run(o, sds, n);
	if (rc)
		return rc;

	return c7_gpon_post(o);
}

/*
 * SerDes CDR reseat: flip the RX CDR re-seat bit, settle, restore the analog
 * word, bounce the 16<->20-bit transfer FIFO release-B. No full re-bring-up.
 */
static int c7_cdr_reset(const struct luna_ops *o)
{
	return luna_cdr_reset(o, C7_SDS_ANA_COM09, C7_WSDS_DIG_1D);
}

/* RTL9607C top-level entry points (thin wrappers over the c7_* internals). */
static int rtl9607c_ponmac_init(const struct luna_ops *o)
{
	return c7_ponmac_init(o, LUNA_REV_A, LUNA_SUBTYPE_NONE);
}

static int rtl9607c_ponmac_mode_set(const struct luna_ops *o,
				    int rev, int subtype)
{
	return c7_gpon_mode_set(o, rev, subtype);
}

static int rtl9607c_serdes_cdr_reset(const struct luna_ops *o)
{
	return c7_cdr_reset(o);
}

/* RTL9602C GPON PON-MAC / SerDes bring-up, clean-room ...
 * dev/MEASURED-luna_ponmac.c.md sec 7. */
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
 * this chip's own, and the C3_/C7_ sections use them for the same roles. */
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

/* Full SerDes analog + WSDS golden table: the operating point ...
 * dev/MEASURED-luna_ponmac.c.md sec 8. */
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

/* Drive SDS_ANA_COM REG01 (0x22584) = 0x73a4 (BEN_TTL_OUT ...
 * dev/MEASURED-luna_ponmac.c.md sec 9. */
int luna_c2_stock_analog = 1;

/* Program the FULL analog CMU/CDR golden table AFTER the SDS ...
 * dev/MEASURED-luna_ponmac.c.md sec 20. */
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

/* SerDes CDR-lock pulse (the stock CDR-reset behaviour): ...
 * dev/MEASURED-luna_ponmac.c.md sec 10. */
static int rtl9602c_serdes_cdr_reset(const struct luna_ops *o)
{
	u32 cdr = o->rd(C2_SDS_ANA_COM_REG12);

	o->wr(C2_SDS_ANA_COM_REG12, cdr ^ BIT(15));
	mdelay(10);
	o->wr(C2_SDS_ANA_COM_REG12, cdr);
	return 0;
}

/* GPON SerDes bring-up, a faithful translation of the stock ...
 * dev/MEASURED-luna_ponmac.c.md sec 11. */
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

/* Steps 5 + 6: burst-enable output with optic-LOS left ...
 * dev/MEASURED-luna_ponmac.c.md sec 12. */
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

/* Step 6b + TX drive, with serdes_modev1_tx and ... -- dev/MEASURED-luna_ponmac.c.md sec 21. */
static const struct r960_op c2_sds_tx[] = {
	FLD(C2_WSDS_DIG_1E,    5,  4, 0),	/* D2A interconnect = 0 (stock)   */
	FLD(C2_SDS_REG7,      14, 14, 0),	/* SP_CFG_NEG_CLKWR_A2D = 0       */
	FLD(C2_SDS_EXT_REG12,  8,  8, 0),	/* SEP_CFG_NEG_CLKRD_D2A = 0      */
	FLD(C2_SDS_ANA_COM_REG22, 5, 3, 5),	/* REG_TX_AMP = 0x5               */
	FLD(C2_SDS_ANA_COM_REG22, 2, 0, 1),	/* REG_TX_EMP = 0x1               */
};

/* Steps 7a + 7b: force signal-detect on so the MAC reset ...
 * dev/MEASURED-luna_ponmac.c.md sec 13. */
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

/* Registers the c7 SerDes/fiber DIAGNOSTIC reads. They were declared inside the
 * removed EPON section, which is why a GPON-only build could not link. */
#define C7_SDS_FIB_STATUS	0x1b00028cu
#define C7_GTC_DS_LOS		0x1b701040u
#define C7_FIB_REG16		0x1b040c40u

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

	/* Step 4: force the 125M ref clock, optionally let the TX CMU ...
	 * dev/MEASURED-luna_ponmac.c.md sec 22. */
	if (luna_c2_skip_rstb_dance) {
		/* Skip the dance: DIG_1D is already 0x1c000 and DIG_00 already 0xf30, and
		 * the SDS reset does not clear them, so an assert->release would be a
		 * gratuitous reset-B pulse on a running serializer. */
		pr_info("luna-gpon: skip interface reset-B dance (DIG_1D=0x%x already released)\n",
			o->rd(C2_WSDS_DIG_1D));
	} else if (luna_c2_clkgate_rstb) {
		/* Synchronous clock-gated reset-B release: legacy releases ...
		 * dev/MEASURED-luna_ponmac.c.md sec 23. */
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

void luna_c7_diag(const struct luna_ops *o, struct seq_file *s)
{
	u32 fib0, com09, fib21, sds_sts, gtc_los, sds_cfg, wsd18, fib16;

	if (!o || !o->rd)
		return;

	fib0    = o->rd(C7_FIB_REG0);
	com09   = o->rd(C7_SDS_ANA_COM09);
	fib21   = o->rd(C7_FIB_EXT_REG21);
	sds_sts = o->rd(C7_SDS_FIB_STATUS);
	gtc_los = o->rd(C7_GTC_DS_LOS);
	sds_cfg = o->rd(C7_SDS_CFG);
	wsd18   = o->rd(C7_WSDS_DIG_18);
	fib16   = o->rd(C7_FIB_REG16);

	seq_printf(s,
		"c7_sd: fib_reg0=0x%08x com09=0x%08x fib21=0x%08x sds_fib_sts=0x%08x gtc_los=0x%08x sds_cfg=0x%08x wsd18=0x%08x fib_reg16=0x%08x\n",
		fib0, com09, fib21, sds_sts, gtc_los, sds_cfg, wsd18, fib16);
	seq_printf(s,
		"c7_sd: sds_sdet=%u fib100_sdet=%u link_ok=%u analog_ready=%u optic_los=%u frc_los=%u sel_rx_sd=%u frc_sd=%u\n",
		!!(sds_sts & BIT(17)), !!(sds_sts & BIT(2)), !!(sds_sts & BIT(4)),
		!!(fib21 & BIT(13)), !!(gtc_los & BIT(8)),
		!!(wsd18 & BIT(14)), !!(fib16 & BIT(2)), !!(fib16 & BIT(10)));
}

/* NO CALLER TODAY (measured 2026-09-10 over the whole overlay)
 * dev/MEASURED-luna_ponmac.c.md sec 24. */
int luna_ponmac_init(enum luna_chip chip, int rev, int subtype,
			const struct luna_ops *o)
{
	(void)rev; (void)subtype;	/* per-chip init takes (o) only */
	switch (chip) {
	case LUNA_CHIP_9602C:
		return rtl9602c_ponmac_init(o);
	case LUNA_CHIP_9603CVD:
		return rtl9603cvd_ponmac_init(o);
	case LUNA_CHIP_9607C:
		return rtl9607c_ponmac_init(o);
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
	case LUNA_CHIP_9603CVD:
		return rtl9603cvd_ponmac_mode_set(o, rev, subtype);
	case LUNA_CHIP_9607C:
		return rtl9607c_ponmac_mode_set(o, rev, subtype);
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
	case LUNA_CHIP_9603CVD:
		return rtl9603cvd_serdes_cdr_reset(o);
	case LUNA_CHIP_9607C:
		return rtl9607c_serdes_cdr_reset(o);
	default:
		return -ENOTSUPP;
	}
}
