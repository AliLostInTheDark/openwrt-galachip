/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * RTL9602C GPON/SWCORE register offsets, the per-SoC half of the driver.
 *
 * Selected at compile time rather than through a runtime table: each model
 * ships its own lean kernel, so a second chip is a second header instead of a
 * pointer indirection on every register access in the datapath. Adding a chip
 * means copying this file and editing the values.
 */
#ifndef _LUNA_GPON_REGS_H
#define _LUNA_GPON_REGS_H



#include <linux/bits.h>	/* BIT */
#include <linux/delay.h>	/* udelay in the SMI poll */
#include <linux/io.h>	/* ioread32 / iowrite32 */
#include "regtable.h"	/* flowcore: struct gpon_chip -- offsets as DATA */
#define GPON_PHYS_BASE	0x1b700000u
#define GPON_REG_SIZE	0x00010000u	/* covers GTC DS block at +0x1000 */

/*
 * SoC system-controller "IP enable" bank. Bit 5 powers the PON packet datapath
 * ("PONPBO" / PON-IP at phys 0x1bf00000); the neighbouring bits in this bank
 * gate other on-chip IPs (e.g. PCIe). U-Boot never sets the PON bit because it
 * does not run GPON, so the PON-IP block is unclocked and any access to it
 * hard-hangs the CPU bus until this bit is set.
 */
#define SOC_IP_ENABLE_PHYS	0x1800063cu
#define   SOC_IP_EN_PON		BIT(5)

#define GPON_INT_DLT		0x0000
#define GPON_RESET		0x000c
#define   GPON_SOFT_RST		BIT(0)		/* 1 = assert soft reset      */
#define   GPON_RST_DONE		BIT(8)		/* 1 = reset cycle complete   */
#define GPON_VERSION		0x0010
#define   GPON_VER_ID_MASK	0xffu
#define GPON_TEST		0x0014
#define GPON_AES_BYPASS		0x0020
#define GPON_INTR_MASK		0x0040
#define GPON_INTR_STS		0x0044
/*
 * In the SWCORE window these two offsets are a serial-command channel, not
 * interrupts. rtl9602c_sc_ldo_init() uses them to issue an SC read of reg
 * 0xfdca, clear the DRAM-LDO bits and commit. They belong to the SC_* family,
 * alongside SC_IND_WD.
 */
#define SC_CMD			0x0040	/* SWCORE: serial-command issue */
#define SC_DATA			0x0044	/* SWCORE: its read-back / data */
#define SW_INTR_IMR		0x1d00c	/* SWCORE: switch interrupt enable, every Luna die */
#define SW_INTR_IMS		0x1d010	/* SWCORE: its latched status, write 1 to clear */
#define   SW_INTR_GPON		BIT(10)
#define   SW_INTR_DYING_GASP	BIT(13)	/* stock's dying_gasp_isr, interrupt type 13 */
#define   GPON_INTR_GTC_DS	BIT(1)		/* GPON_INTR_MASK: GTC downstream events */
#define GPON_GTC_DS_INTR_DLT	0x1000
#define GPON_GTC_DS_INTR_MASK	0x1004
#define GPON_GTC_DS_INTR_STS	0x1008
#define   GPON_DS_INTR_STS_FEC	BIT(2)		/* downstream FEC in use */
#define GPON_GTC_DS_LOS_CFG_STS	0x1040		/* downstream LOS status/cfg  */
#define   GPON_CDR_LOS_SIG	BIT(10)		/* 1 = CDR not recovering clk */
#define   GPON_OPTIC_LOS_SIG	BIT(8)		/* 1 = no optical signal      */
#define   GPON_OPTIC_LOS_POLAR	BIT(1)		/* invert optical-LOS input   */
#define   GPON_OPTIC_LOS_EN	BIT(0)		/* enable optical-LOS monitor */
#define GPON_GTC_DS_ONU_ID_STATUS	0x1010
#define   GPON_ONU_STATE_MASK	0xfu		/* [3:0]  FSM state O1..O7    */
#define   GPON_ONU_ID_SHIFT	8		/* [15:8] ONU-ID             */
#define   GPON_ONU_ID_MASK	0xffu
#define GPON_GTC_US_ONU_ID	0x5010
#define GPON_GTC_US_MIN_DELAY	0x5040
#define GPON_GTC_US_EQD		0x5044
#define   GPON_EQD_INFRAME_MASK	0x3ffffu	/* [17:0]  in-frame delay    */
#define   GPON_EQD_MF_SHIFT	24		/* [26:24] multiframe count  */
#define   GPON_EQD_MF_MASK	0x7u
#define GPON_GTC_US_WRITE_PROTECT 0x5018	/* gate for US config writes   */
#define   GPON_US_WP_UNLOCK	0xcc19u		/* magic: enable protected US writes */
#define   GPON_US_WP_LOCK	0x0000u
#define GPON_GTC_US_CFG		0x5014		/* [11]LESS_RANDOM [10]IND_NRM_PLM
						 * [9]PLM_DIS [4]ENA_AUTO_DG
						 * [3]US_BEN_POLAR [0]SCRM_DIS */
#define   GPON_US_CFG_BEN_POLAR	BIT(3)	/* burst enable active HIGH, as stock */
#define   GPON_US_CFG_VAL	0x0c18u		/* online operating value: BEN_POLAR=1,
						 * scrambler on, PLOAM on (LESS_RANDOM|
						 * IND_NRM_PLM|ENA_AUTO_DG|US_BEN_POLAR) */
#define GPON_GTC_US_LASER	0x504c		/* [13:8] LON_TIME, [5:0] LOFF_TIME */
#define   GPON_US_LASER_VAL	0x2028u		/* LON=32, LOFF=40 burst-window edges */
#define GPON_GTC_US_BOH_CFG	0x5054		/* [11:8] BOH_REPEAT, [7:0] BOH_LENGTH */
#define GPON_GTC_US_BOH_DATA	0x5080		/* 12-entry burst-overhead byte array, stride 4 */

#define GPON_GTC_DS_PLOAM_CFG	0x101c		/* [9]BC_ACC [8]ONUID_FLT [7:0]NOMSG_ID */
#define GPON_GTC_DS_PLOAM_IND	0x1080		/* DS receive-buffer indicator */
#define   GPON_DS_PLM_BUF_EMPTY	BIT(5)		/* 1 = no DS PLOAM pending     */
#define   GPON_DS_PLM_BUF_FULL	BIT(4)
#define   GPON_DS_PLM_DEQ	BIT(0)		/* W: advance to next message  */
#define GPON_GTC_DS_PLOAM_MSG	0x10a0		/* 8-word received-message buf */
#define GPON_GTC_US_PLOAM_IND	0x50c0		/* US transmit-queue indicator */
#define   GPON_US_PLM_TYPE_SHIFT 8		/* [10:8] queue/type select    */
#define   GPON_US_PLM_NRM_EMPTY	BIT(7)		/* normal queue empty          */
#define   GPON_US_PLM_NRM_FULL	BIT(6)
#define   GPON_US_PLM_URG_EMPTY	BIT(5)		/* urgent queue empty          */
#define   GPON_US_PLM_URG_FULL	BIT(4)
#define   GPON_US_PLM_ENQ	BIT(0)		/* W: queue the composed msg   */
#define GPON_GTC_US_PLOAM_DATA	0x50e0		/* 8-word transmit-message buf */
#define GPON_GTC_US_PLOAM_CFG	0x5100		/* US PLOAM buffer control     */
#define   GPON_US_PLM_CRC_GEN_EN BIT(1)		/* HW computes US PLOAM CRC    */
#define   GPON_US_PLM_ONUID_OVRD BIT(0)		/* override ONU-ID field       */
#define   GPON_US_PLM_FLUSH_BUF	BIT(4)		/* PLM_FLUSH_BUF: rests at 1, 1->0->1 flushes;
						 * resting at 0 keeps flushing and no CPU PLOAM is
						 * sent */
#define   GPON_US_PLM_CFG_REST	(GPON_US_PLM_ONUID_OVRD | \
				 GPON_US_PLM_CRC_GEN_EN | \
				 GPON_US_PLM_FLUSH_BUF)	/* = 0x13, stock's word */

#define GPON_TEST_SCRATCH	0x12345678u
#define GPON_RST_POLL_MAX	1000		/* bounded RST_DONE poll      */

/*
 * PON SerDes (SDS) analog block. It lives in the SWCORE window (phys
 * 0x1B000000), NOT the GPON datapath sub-block — these are plain MMIO offsets
 * off the switch-core base. The SDS CMU/PLL recovers the line clock that feeds
 * the GPON MAC core; until it is configured and its analog-ready flag asserts,
 * the MAC core has no clock, the soft-reset never completes (RST_DONE stays 0)
 * and the GTC register banks read a floating pattern. The GPON-MAC reset is
 * issued as part of this sequence (the SDS_RST also resets the MAC).
 */
#define SWCORE_PHYS_BASE	0x1b000000u
#define SWCORE_REG_SIZE		0x00041000u	/* covers up to FIB_EXT_REG21  */

/*
 * Register offsets here are the TRUE switch-core offsets (verified against the
 * SoC register map). The SerDes digital/analog banks live at SWCORE + 0x22xxx
 * (phys 0x1b022xxx), NOT 0x40xxx — a direct access to 0x40xxx hits an unmapped
 * hole that returns the bus abort-fill 0xbad0bad0. SDS_CFG and SOFTWARE_RST are
 * in the low control page.
 */
#define SW_SOFTWARE_RST		0x00104
#define SDS_CFG			0x001d0		/* [4:0] CFG_SDS_MODE          */
#define   SDS_MODE_OFF		0x1fu
#define   SDS_MODE_GPON		0x08u
#define   SDS_FIB_SDS_SDET	BIT(17)		/* SDS-level optical sig-detect */

#define I2C_CONFIG0		0x23004		/* bus0, +I2C_BUS_STRIDE/bus   */
/*
 * The CONFIG and IND registers are two-entry arrays with a 32-bit array offset
 * in the 9602C, 9603CVD and 9607C chipdefs, so bus 1 is the next word. Stock
 * X100DG (BOSA on bus 1) shows it live: 0xb4/0xbc/0xc4/0xcc carry its last
 * transfer and 0x23008 = 0x0235001e (slave 0x54).
 */
#define   I2C_BUS_STRIDE	0x4u		/* CONFIG/IND block, per bus   */
#define   I2C_CFG_DEV_ID_LSB	14
#define   I2C_CFG_AW_LSB	12
#define   I2C_CFG_DW_LSB	10
#define   I2C_CFG_CLKDIV_LSB	0
/*
 * The parent clock is 31250, as loaded by the RTL9602C and RTL9603CVD stock
 * clock_set functions, which divide by the wanted kHz and subtract 1. So 0x270
 * = 624 gives 31250/625 = 50 kHz. The RTL9607C's formula is not verified.
 *
 * The value is deliberately unchanged. The X111W's RTL8290B is healthy at this
 * divisor, but that says nothing about other I2C slaves, and stock aims
 * elsewhere on each die: about 1000 kHz (div 30) on the X111W, about 99.2 kHz
 * (div 314) on the G24W.
 */
#define   I2C_CLKDIV_50K	0x270u		/* (31250/50)-1 = 624 -> 50 kHz */
#define I2C_IND_WD		0x000b0		/* [31:0] write data           */
#define I2C_IND_ADR		0x000b8		/* [31:0] target reg offset    */
#define I2C_IND_CMD		0x000c0		/* [0]CMD_EN [1]RW_EN [2]BUSY [3]NACK */
#define   I2C_CMD_EN		BIT(0)
#define   I2C_CMD_RW_WR		BIT(1)		/* 1=write 0=read              */
#define   I2C_CMD_BUSY		BIT(2)
#define   I2C_CMD_NACK		BIT(3)
#define I2C_IND_RD		0x000c8		/* [31:0] read data            */
#define I2C_BUSY_POLL_MAX	1000		/* x10us = up to 10 ms         */
/*
 * RTL8290B register space is paged by I2C slave address: the full 12-bit
 * register number's high byte selects a 256-register page (page0->0x50,
 * page1->0x51, page2->0x54, page3+ ->0x55) and the low byte is the offset
 * within that page. Confirmed: chip-ID reg 0x390 reads correctly at slave 0x55
 * offset 0x90. RX path registers (NUM/page mapping from the transceiver's
 * register map):
 */
#define BOSA_A0(n)		(0x000 + (n))	/* SFF-8472 A0h byte n (slave 0x50) */
#define BOSA_A2(n)		(0x100 + (n))	/* SFF-8472 A2h byte n (slave 0x51) */
#define BOSA_PAGE2(off)		(0x200 + (off))	/* byte @off of the analog/APC page (slave 0x54), no name established */
/* Wn is NOT linear: W0..W79 = 0x200..0x24f, then W80..W88 = 0x27c..0x284
 * (europa_reg_definition.h: W79=0x24F, W80=0x27C, W88=0x284). Nothing above
 * W88 is named there; 0x250..0x27b carry their own names (BOSA_REG_*). */
#define BOSA_W(n)		((n) < 80 ? BOSA_PAGE2(n) : BOSA_PAGE2(0x7c + (n) - 80))	/* analog/APC register Wn */
#define BOSA_R(n)		(0x300 + (n))	/* MCU control/status register Rn (slave 0x55) */
#define BOSA_REG_NUM		0x390		/* chip NUM (0x8290), 2 bytes  */
#define BOSA_REG_VID		0x394		/* manufacturer ID (0x0001)    */
#define BOSA_REG_W4		0x204		/* [4] EN_L booster (1=on)     */
#define BOSA_REG_W41		0x229		/* [4] RXI_PWDN_L (0=RX on)    */
#define BOSA_REG_CONTROL2	0x254		/* [6] LOS_PIN_TRI (0=drive SD)*/
/* the rest of 0x250..0x279 the transceiver's register definition names (europa_reg_definition.h) */
#define BOSA_REG_MAGIC_CODE	0x250
#define BOSA_REG_SW_VERSION	0x251
#define BOSA_REG_CONTROL1	0x253
#define BOSA_REG_CONTROL3	0x255
#define BOSA_REG_SW_INT_CODE	0x258
#define BOSA_REG_INT_MASK1	0x259
#define BOSA_REG_FAULT_INHIBIT	0x260
#define BOSA_REG_FAULT_INHIBIT2	0x261
#define BOSA_REG_SW_FAULT_INHIBIT1	0x262
#define BOSA_REG_SW_FAULT_INHIBIT2	0x263
#define BOSA_REG_DAC_HB	0x264
#define BOSA_REG_DAC_LB	0x265
#define BOSA_REG_EXT_RSSI_LOW_TH	0x266
#define BOSA_REG_EXT_RSSI_HIGH_TH	0x267
#define BOSA_REG_RX_TH	0x269
#define BOSA_REG_READY_DELAY	0x273
#define BOSA_REG_DDMI_W_LCT	0x274
#define BOSA_REG_DDMI_W_HCT	0x275
#define BOSA_REG_DEBUG_CTL	0x276
#define BOSA_REG_DEBUG_SL1	0x277
#define BOSA_REG_DEBUG_SL2	0x278
#define BOSA_REG_IMPD_TH_ORI	0x279
#define BOSA_REG_STATUS2	0x383		/* [2] RX_LOS_STATUS (0=signal)*/
#define BOSA_REG_W77		0x24d		/* APC max-enable strobes = the MCU command byte */
/*
 * W77's fields, as the transceiver's own register definition lays them out
 * (europa_reg_definition.h, W77 = 0x24D; the stock europa_drv.ko ignition
 * disassembly strobes the same bits). BACKUP[3:0] is named there, but what the
 * MCU does with it is unknown: BOSA_W77_BACKUP_B3 is the field's name, not a
 * meaning.
 */
#define   BOSA_W77_BIAS_MAX_EN		BIT(7)
#define   BOSA_W77_BIAS_MAX_LOADIN	BIT(6)
#define   BOSA_W77_MOD_MAX_EN		BIT(5)
#define   BOSA_W77_MOD_MAX_LOADIN	BIT(4)
#define   BOSA_W77_BACKUP_B3		BIT(3)		/* in BACKUP[3:0]; the one bit the ignition strobes */
#define BOSA_REG_R29		0x31d		/* FSU/MCU status; the MCU consumes each read */
#define WSDS_DIG_00		0x22030		/* SDS clock + soft-reset-B bank */
#define   WSDS_SFT_RSTB		BIT(8)		/* digital soft reset-B        */
#define   WSDS_DIG00_RUN	0xf30u		/* operational run state       */
#define WSDS_DIG_01		0x22034		/* [31:0] CFG_DMY0 (force-SDS)  */
#define WSDS_DIG_02		0x22038		/* [10]  EN_PDOWN_BEN          */
#define WSDS_DIG_03		0x2203c		/* [6:4] CFG_TXDIS_SEL_DLY     */
#define WSDS_DIG_1D		0x220a4		/* interface reset-B releases  */
#define   WSDS_SFT_RSTB_INF	BIT(14)		/* interface soft reset-B (FIFO r/w ptr re-sync) */
#define SDS_ANA_COM_REG27	0x225ec		/* TX CMU enable lives here    */
#define   SDS_CMU_EN		BIT(10)		/* TX CMU enable (toggle 1->0->1 to re-lock the PLL) */
#define SDS_ANA_COM_REG03	0x2258c		/* [15:14] CMU_ISTANK_SEL_RX   */
#define SDS_ANA_COM_REG08	0x225a0		/* TX-CDR (reg1418); [15] = the
						 * serdesCdr_reset toggle bit    */
#define SDS_ANA_COM_REG11	0x225ac		/* [7:0]  RX_FILT_CONFIG       */
#define SDS_ANA_COM_REG12	0x225b0		/* [14]   RX_SEL_CDR_AFEN      */
#define SDS_ANA_COM_REG22	0x225d8		/* [5:3] TX_AMP [2:0] TX_EMP   */
#define SDS_ANA_COM_REG26	0x225e8		/* [6:5] CMU_ISTANK_SEL_GPHY   */
#define SDS_ANA_GPON_REG42	0x22728		/* [2]   PCM_CMU_EN            */
#define SDS_ANA_GPON_REG46	0x22738		/* [9:7]KI [6:4]KP1 [3:1]KP2   */
#define SDS_ANA_MISC_REG00	0x22500		/* [5] FRC_RX_EN_VAL [4] _ON   */
#define SDS_ANA_MISC_REG01	0x22504		/* [7:5] SPDSEL_VAL [4] _ON    */
#define SDS_ANA_MISC_REG02	0x22508		/* [13] SD_VAL [12] SD_FORCE   */
#define   SDS_ANALOG_READY	BIT(13)
#define SDS_LOCK_POLL_MAX	1000		/* x200us = up to 200 ms       */
#define   SP_SDS_EN_RX		BIT(1)
/* Stock link-state polling behavior: when GPON_GTC_DS_INTR_STS reads this
 * exact sentinel the DS CDR is wedged; stock recovers by toggling SP_SDS_EN_RX
 * 1->0->1 (SDS_REG0[1]), waiting 10ms, then re-reading. */
#define GTC_DS_CDR_STUCK	0xca0eca0fu

/*
 * SoC IO pad routing for the optical front-end (switch-core register file, so
 * these are plain SWCORE offsets like the SDS block above). IO_MODE_EN's OEM_EN
 * bit enables the optical "e-mode" pads (TX_DISABLE, optical TX_SD / RX signal-
 * detect); IO_GPIO_EN is a 1-bit-per-pin GPIO-function-enable array (32 pins per
 * 32-bit word). The optical RX signal-detect shares pad GPIO 13: while that pin
 * is in GPIO mode the BOSA's signal-detect never reaches the GPON LOS input, so
 * OPTIC_LOS_SIG reads "loss" even with real light. Releasing GPIO 13 (function
 * disabled) routes the pad to the optical-SD input.
 */

/*
 * Front-panel LED controller (SWCORE window). Each panel LED has an index whose
 * 2-bit "force value" the CPU can drive directly -- 0=off, 1=on, 2=blink -- once
 * the index is placed in CPU force-mode and enabled for parallel (vs serial-
 * shift) output; a port lamp is instead typed to its switch port and lit by the
 * switch. Which index is which lamp is the BOARD's (DT gpon node). The block
 * base and DATA_LED_CFG are common; the addresses below are the RTL9602C's and
 * the per-die table (luna_led_regs in luna_gpon.c) carries each die's own.
 * Offsets are into swcore_base (phys 0x1b000000).
 */
#define LED_MODE_SEL		0x1e000		/* [0] 0 = parallel output      */
#define LED_DATA_CFG(idx)	(0x1e004 + (idx) * 4)	/* [20:16] type, force-mode */
#define LED_FORCE_VALUE		0x1e04c		/* [idx*2+1:idx*2] force value  */
#define LED_BLINK_RATE		0x1e050		/* [14:12] force blink period   */
#define LED_PARA_EN		0x1e05c		/* [n+1] LEDn parallel-enable    */
#define LED_IO_EN		0x23014		/* [n] LEDn pad-output enable    */
#define LED_FORCE_OFF		0u
#define LED_FORCE_ON		1u
#define LED_FORCE_BLINK		2u
#define LED_BLINK_512MS		4u		/* force blink period code       */
#define LED_IDX_NONE		0xffu		/* board declares no such lamp   */

#define GPIO_PHYS_BASE		0x18003300u
#define GPIO_REG_SIZE		0x40u
#define GPIO_DIR_ABCD		0x08
#define GPIO_DATA_ABCD		0x0c
#define GPIO_DIR_EFGH		0x24
#define GPIO_DATA_EFGH		0x28

#define PONIP_PHYS_BASE		0x1bf00000u
#define PONIP_REG_SIZE		0x00010000u	/* covers up to IO_CMD_1_DS     */


/*
 * Registers gpon_proc_show() reads whose names both the rtl9602c and rtl9603cvd
 * chipdefs agree on. The PI_ prefix is this header's convention for the PON-IP
 * window. The rest stay bare hex: no source names them, or only one chipdef
 * does.
 */
#define GPON_GTC_DS_CFG			0x01014
#define GPON_GTC_DS_SUPERFRAME_CNT	0x01048
#define   GPON_SUPERFRAME_MASK		GENMASK(29, 0)	/* one per 125 us DS frame */
#define GPON_GTC_DS_TOD_SUPERFRAME_CTRL	0x0104c
/*
 * The next GTC DS word, read by gpon_proc_show(). The RTL9603CVD chipdef names
 * it GPON_GTC_DS_PPS_CTRL at 0x701050; the RTL9602C chipdef does not name it,
 * but its neighbours 0x1048/0x104c carry the same names on both dies, so the
 * block is the same.
 */
#define GPON_GTC_DS_PPS_CTRL		0x01050
#define GPON_GTC_DS_MISC_CNTR_BIP_ERR_BLK	0x01180	/* DS BIP block errors */
/* Downstream FEC counters; like their neighbours they clear on read. */
#define GPON_GTC_DS_MISC_CNTR_FEC_CORRECT_BYTE	0x0118c
#define GPON_GTC_DS_MISC_CNTR_FEC_CORRECT_CW	0x01190
#define GPON_GTC_DS_MISC_CNTR_FEC_UNCOR_CW	0x01194
#define GPON_GTC_DS_MISC_CNTR_PLOAM_ACPT	0x0119c
#define GPON_GTC_DS_MISC_CNTR_PLOAM_FAIL	0x011a0
#define GPON_GTC_DS_MISC_CNTR_BWM_FAIL	0x011a4
#define GPON_GTC_DS_MISC_CNTR_BWM_INV	0x011a8
#define GPON_GTC_DS_MISC_CNTR_ACTIVE	0x011ac	/* Not an "active" count: [31:16] CNTR_RANGING_REQ,
						 * [15:0] CNTR_SN_REQ, the ranging and Serial_Number
						 * requests the GTC received. Clear-on-read like its
						 * neighbours. */
#define   GPON_GTC_DS_ACTIVE_RNG_REQ(v)	((u32)(v) >> 16)
#define   GPON_GTC_DS_ACTIVE_SN_REQ(v)	((u32)(v) & 0xffffu)
#define GPON_GTC_DS_MISC_CNTR_BWM_ACPT	0x011b0
#define GPON_GTC_DS_MISC_CNTR_GEM_LOS	0x011b4
#define GPON_GTC_DS_MISC_CNTR_HEC_CORRECT	0x011b8
#define GPON_GTC_DS_MISC_CNTR_GEM_IDLE	0x011bc
#define GPON_GTC_DS_MISC_CNTR_GEM_FAIL	0x011c0
#define GPON_GTC_DS_MISC_CNTR_GEM_NON_IDLE	0x011c4
#define GPON_BWMAP_CTRL			0x0200c
#define GPON_BWMAP_STS			0x02010
/*
 * The upstream half of the same counter group. All four are 5-word arrays at a
 * 4-byte stride, gated by the same CNT_MASK_US enrolment, at the same offsets
 * on both chipdefs:
 *
 *   RX_SID_CNT_US    frames the PON-IP took from the switch for an enrolled SID
 *   RX_SID_GOOD/BAD  how the US-NIC classified them
 *   TX_SID_CNT_US    frames actually transmitted upstream for that SID
 *
 * RX climbing with TX flat means the frame reached the PON-IP with the right
 * SID and the scheduler is not serving it (queue map, T-CONT, PBO). Both flat
 * means it never got the SID (cpu-tag, P_MISC RX_SPC, classify).
 */
#define PI_TX_SID_CNT_US		0x02068		/* 5 groups, 4-byte stride */
#define PI_RX_SID_CNT_US		0x02094		/* 5 groups, 4-byte stride */
#define PI_SID_CNT_GROUP_STRIDE		4u
#define PI_RX_SID_GOOD_CNT_US		0x0203c	/* 5 groups, 4-byte stride */
#define PI_RX_SID_BAD_CNT_US		0x02054	/* 5 groups, 4-byte stride */
/*
 * Both chipdefs declare RX_SID_GOOD_CNT_US with stride 4 and 5 entries at
 * 0x0203c; the next named register is RX_ERR_CNT_US at 0x02050. Groups are
 * spelled as base + n * stride so a relocated base carries its array and the
 * per-chip pi_x() relocation is not bypassed.
 */
#define PI_RX_SID_CNT_GROUPS		5u
/*
 * CNT_MASK_US gates RX_SID_GOOD/BAD_CNT_US: one bit per SID, four groups of a
 * 128-bit mask (from the RTL9603CVD chipdef), so group g's word for SID s is
 * base + g*16 + (s/32)*4, bit s%32. Four 16-byte groups end where the chipdef
 * puts RX_DROP_CNT_US.
 */
#define PI_CNT_MASK_US			0x020a8
#define PI_CNT_MASK_US_GROUP_STRIDE	16u
#define PI_CNT_MASK_US_WORD(grp, sid)	(PI_CNT_MASK_US + (grp) * PI_CNT_MASK_US_GROUP_STRIDE + ((sid) / 32u) * 4u)
#define GPON_BWMAP_DATA			0x02400
/*
 * GPON_BWMAP_DATA is an array of 256 32-bit words, identical on the RTL9602C
 * and RTL9603CVD (the chipdef's "array offset 32" is in bits). gpon_proc_show()
 * reads it as 128 captured allocations of two words each, 2i and 2i+1.
 */
#define GPON_BWMAP_DATA_STRIDE	4u	/* bytes per word (chipdef array offset 32 bits) */
#define BWMAP_DATA(n)		(GPON_BWMAP_DATA + (u32)(n) * GPON_BWMAP_DATA_STRIDE)
/*
 * The capture engine's controls. The vendor's reader arms it as: write 0, write
 * CAP_CLR|frames, write CAP_EN|frames.
 *
 * On the RTL9602C, CAP_FRAME_NUM=32 captures 32 frames and CAP_EN then
 * self-clears; CAP_FRAME_NUM=0 keeps CAP_EN set and captures until read. The
 * buffer holds only allocations the GTC accepted, so it does not show other
 * ONUs' grants.
 */
#define GPON_BWMAP_CAP_EN	BIT(15)
#define GPON_BWMAP_CAP_CLR	BIT(14)
#define GPON_BWMAP_CAP_FRAME_NUM_MASK	0xffu	/* [7:0] */
#define GPON_BWMAP_CAP_OVERFL	BIT(8)		/* in GPON_BWMAP_STS */
/* One captured allocation = 2 words (8-byte stride), layout per the vendor's
 * reader (the same one gpon_proc_show() decodes): word 0 [27:24] frame#,
 * [23] VALID, [22] LST, [21] EoB, [20] SoB, [19] PLOAMu, [18] FEC,
 * [17:16] DBRu, [14:12] MF, [4:0] the T-CONT the alloc-CAM resolved it to;
 * word 1 [15:0] SStart, [31:16] SStop.  There is NO Alloc-ID in it. */
#define GPON_BWMAP_ENT_VALID	BIT(23)
#define GPON_BWMAP_ENT_PLOAMU	BIT(19)
#define GPON_BWMAP_ENT_TCONT_MASK	0x1fu
#define GPON_BWMAP_ENTRIES	32u		/* what the vendor's reader walks */
#define GPON_GEM_DS_FRM_TIMEOUT		0x04098
#define GPON_GTC_US_INTR_DLT		0x05000
#define GPON_GTC_US_INTR_MASK		0x05004
#define GPON_GTC_US_INTR_STS		0x05008
#define   GPON_US_INTR_STS_FEC		BIT(2)	/* upstream FEC in use */
#define GPON_GTC_US_PROC_MODE		0x05200
#define PI_RXFDP1_US			0x053f0
#define GPON_GEM_US_INTR_DLT		0x06000
#define GPON_GEM_US_INTR_MASK		0x06004
#define GPON_GEM_US_INTR_STS		0x06008
#define GPON_AES_KEY_SWITCH_TIME	0x03014
#define GPON_GEM_US_PTI_CFG		0x06020
#define GPON_GEM_US_PWR_SAV_CFG	0x06024
#define GPON_GEM_US_EOB_MERGE	0x06260
#define GPON_GEM_US_BYTE_STAT		0x06800
/*
 * The two US emission counter arrays. The stride is 8 bytes: the register map's
 * "array offset 64" counts bits.
 */
/*
 * The chipdef's name, on the RTL9602C, RTL9603CVD and RTL9607C alike, has no
 * GPON_ prefix.
 */
#define TCONT_IDLE_BYTE_STAT		0x06c00
#define GPON_US_BYTE_STAT_STRIDE	8u	/* bytes per entry (64-bit counter) */
/* Entry N of each array. Written once here so a caller never re-derives the
 * stride -- which is exactly how 0x7000 got written for 0x6c80. */
#define GEM_US_STAT(n)		(GPON_GEM_US_BYTE_STAT + \
				 (u32)(n) * GPON_US_BYTE_STAT_STRIDE)
#define TCONT_IDLE_STAT(n)	(TCONT_IDLE_BYTE_STAT + \
				 (u32)(n) * GPON_US_BYTE_STAT_STRIDE)
#define PI_PKT_OK_CNT_DS		0x0c010
#define PI_PKT_ERR_CNT_DS		0x0c014
#define PI_PKT_MISS_CNT_DS		0x0c018
/*
 * Upstream twins of the DS counters above. RX_SID_GOOD_CNT_US counts only SIDs
 * enrolled in a CNT_MASK_US group, so a mis-enrolled mask makes it read zero
 * whatever the hardware did. PKT_OK_CNT_US is ungated: it counts every frame
 * the US-NIC MAC accepts, and packs TX_OK_CNT[31:16] and RX_OK_CNT[15:0].
 */
#define PI_PKT_OK_CNT_US		0x04010		/* [31:16] TX_OK [15:0] RX_OK */
#define PI_PKT_ERR_CNT_US		0x04014
#define PI_PKT_MISS_CNT_US		0x04018
#define PI_RXFDP1_DS			0x0d3f0

#define PI_IO_CMD_0_US		0x05434		/* [5] GMII_RX_EN [4] GMII_TX_EN */
#define PI_IO_CMD_0_DS		0x0d434
/*
 * Internal-MII force-link for the two PON-IP NICs (symmetric pair, US =
 * DS-0x8000): MEDIA_STS_DS @0xc058 (read 0x106e8400 from the bootloader, the
 * DS-NIC<->GMAC0 RX link that DS OMCI rides) and MEDIA_STS_US @0x4058 (the
 * GMAC0-TX -> US-NIC link that US OMCI must ride). FORCELINK[18]
 * FORCEDFULLDUP[19] FORCE_SPD[17:16] FORCE_SPD_MODE[10]
 * TRXFCE/RXFCE/TXFCE[31:29]; golden 0x106e8400. No DAL code writes the US one,
 * so it stays down and GMAC0-TX OMCI is dropped at the US-NIC GMII ingress
 * before PON_SID2QID. Force it up like the DS side.
 */
#define PI_MEDIA_STS_US		0x04058
#define PI_MEDIA_STS_DS		0x0c058
#define PI_IO_CMD_1_US		0x05438		/* [5:4] RPAGE [1:0] TPAGE size  */
#define PI_IO_CMD_1_DS		0x0d438
#define PI_PONIP_CTL_US		0x020d8		/* US PON-IP control             */
#define PI_PONIP_CTL_DS		0x0a0ac		/* DS PON-IP control             */
#define PI_PON_US_FIFO_CTL	0x020f0		/* [5:4] SPACE [3:0] START       */
#define PI_PON_DSC_CFG_US	0x0215c		/* [28:16] RAM_NO [12:0] SRAM_NO */
#define PI_PON_DSC_CFG_DS	0x0a0cc		/* [14:13] PAGE_SIZE             */
#define PI_DSCRUNOUT_US		0x020e0		/* [28:16] DRAM [12:0] SRAM out  */
#define PI_IP_MSTBASE_US	0x020e8		/* CFG_PON_MSTBASE: phys base of the US PBO DRAM packet pool */
#define PI_IP_MSTBASE_DS	0x0a0b8u	/* the DS twin -- see gpon_swc_map.ds_dram_order */
/*
 * US PBO DRAM pool. The US-NIC RX path is a PBO that DMAs received US packets
 * into a DRAM pool at IP_MSTBASE_US. Without the pool every US OMCI frame is
 * dropped at descriptor fetch, before the MAC, leaving PKT_OK/ERR/MISS all 0.
 * Stock memory-usage init (usPageSize=128, us_mem_size=1MB): SRAM_NO=0x7f,
 * RAM_NO=min(size/128,0x1fff)=0x1fff, DRAM_RUNOUT=size/128-40-128=0x1f58.
 */
#define PI_US_DRAM_PAGES	0x1fffu		/* RAM_NO: total US PBO pages-1 (SRAM+DRAM) */
#define PI_US_DRAM_RUNOUT	0x1f58u		/* DSCRUNOUT_US DRAM portion (size/128-168) */
#define PI_US_DRAM_ORDER	8		/* __get_free_pages order: 2^8 * 4KB = 1MB */
/*
 * SRAM-only DS is an RTL9602C fact, not a family one. The G24W's stock gives
 * the downstream PBO the same 8192-page pool as the upstream one; with 31 SRAM
 * pages DS OMCI reached the GEM layer, found no descriptor page and was dropped
 * before the trap. The geometry is therefore a per-chip table entry
 * (gpon_swc_map.ds_dram_*), and a chip that declares order 0 keeps the
 * SRAM-only path.
 */
#define PI_DS_DRAM_ORDER	8		/* 2^8 * 4KB = 1MB, as for the US pool */
#define PI_DSCRUNOUT_DS		0x0a0b4
#define PI_PON_SID_STOP_TH	0x02450		/* [12:0] global stop-all page threshold */
#define PI_PON_SID_GLB_TH	0x02454		/* [28:16] ON_TH [12:0] OFF_TH (global) */
#define PI_PON_SID_RPV_TH	0x02458		/* per-SID reserved-page threshold, +sid*4 */
/*
 * PON_SID_RPV_TH has a 4-byte stride. Each chip's array ends one entry before
 * its next declared register:
 *
 *   RTL9602C    0x02458 + 65 * 4 == 0x0255c == PONIP_DBG_CTRL_US
 *   RTL9603CVD  0x026c8 +  8 * 4 == 0x026e8 == PONIP_DBG_CTRL_US
 *
 * The entry count is per chip, gpon_swc_map's .sid_rpv_entries (65 and 8): 65
 * on the RTL9603CVD overruns into 57 of its upstream scheduler registers.
 */
#define PI_RPV_TH_STRIDE	4u
#define PI_PON_FC_CONFIG_DS	0x0a0fc		/* [28:16] FC_ON_TH [12:0] FC_OFF_TH */
#define PI_CFG_US		0x0404c		/* [26] RFF_AFULL [17] TX_STOP   */
#define PI_CFG_DS		0x0c04c		/* [16] TXE_EXTRA                */
#define PI_TX_CFG_US		0x04040		/* [12:10] IFG [2:1] PRE [0] PAD */
#define PI_TX_CFG_DS		0x0c040
#define PI_RX_CFG_US		0x04044		/* [5] accept-CRC-error          */
#define PI_RX_CFG_DS		0x0c044
#define PI_PROBE_SELECT_US	0x05400		/* [1] debug func select         */
#define PI_PROBE_SELECT_DS	0x0d400		/* stock O5 = 0x40 (DS-NIC drain) */
#define PI_DS_NIC_CFG_D404	0x0d404		/* stock O5 = 0x11100348          */
#define PI_CONFIG_CLK_DS	0x0d42c		/* stock O5 = 0x40               */

#define GPON_GTC_DS_MISC_CNTR_LOM 0x1198	/* [31:16]=PLEND_FAIL [15:0]=SUPERFRAME_LOS(LOM); clear-on-read */

#define SMI_CTRL_0		0x230B8u
#define   SMI_CMD_EN		BIT(0)
#define SMI_CTRL_2		0x230C0u
#define SMI_CTRL_3		0x230C4u
#define SMI_BC_PHYID		0x230C8u

/*
 * SWCORE registers written by rtl9602c_datapath_tables_init() and
 * rtl9602c_ponmac_modeset_gpon(), named after the RTL9602C chipdef.
 * SCH_WFQ_TKN_CTRL and LINE_RATE_2500M exist in that chipdef only, so they are
 * RTL9602C facts.
 */
#define SW_SCH_WFQ_TKN_CTRL	0x2d89c		/* SCH_WFQ_TKN_CTRL (RTL9602C only) */
#define SW_LINE_RATE_2500M	0x2d8b8		/* LINE_RATE_2500M  (RTL9602C only) */
/*
 * P_MISC is the per-port MAC misc register: one word per switch port, at the
 * chip's per-port register interval. Base 0x20004 is P_MISC in all three
 * chipdefs. The interval is per chip (rtl9602c_macPpInfo 0x400, 0x100 on the
 * 9603CVD and 9607C), so P_MISC[2] (PON) = 0x20804 and P_MISC[3] (CPU) =
 * 0x20c04 on the RTL9602C, and a literal 0x20804 would be P_MISC[8] on the
 * other two.
 */
#define SW_P_MISC		0x20004		/* P_MISC[port 0] -- the BASE is family:
						 * the chipdefs of the RTL9602C, the
						 * RTL9603CVD and the RTL9607C all name
						 * 0x20004 P_MISC (checked by address). */
/*
 * The stride is not family-wide, hence the suffix: on the 9603CVD, base + 2 *
 * 0x400 is P_MISC[8].
 */
#define SW_MACPP_STRIDE_9602C	0x400u		/* RTL9602C macPpInfo.interval;
						 * 0x100 on the 9603CVD/9607C */
#define SW_P_MISC_PORT_9602C(p)	(SW_P_MISC + (p) * SW_MACPP_STRIDE_9602C)
/*
 * Shared files must use this form. It takes the stride from the chip's own
 * luna_sw_map, so P_MISC[pon] is 0x20804 on the RTL9602C and 0x20404 on the
 * RTL9603CVD. The _9602C form above is for RTL9602C-only code and the
 * static_assert that pins the arithmetic.
 */
#define SW_P_MISC_PORT(m, p)	(SW_P_MISC + (unsigned int)(p) * (m)->macpp_stride)
static_assert(SW_P_MISC_PORT_9602C(2) == 0x20804u &&
	      SW_P_MISC_PORT_9602C(3) == 0x20C04u,
	      "P_MISC moved: the PON (2) and CPU (3) port words are these two");

#define TBL_CTRL_OFF	0x12000u
#define TBL_STS_OFF	0x12004u	/* BUSY = bit13 */
#define TBL_WRDATA_OFF	0x12008u

#define PLM_DS_UPSTREAM_OVERHEAD	0x01
#define PLM_DS_ASSIGN_ONU_ID		0x03
#define PLM_DS_RANGING_TIME		0x04
#define PLM_DS_DEACTIVATE_ONU		0x05
#define PLM_DS_DISABLE_SN		0x06
#define PLM_DS_EXT_BURST_LENGTH		0x14
#define PLM_DS_ENCRYPT_PORT		0x08	/* Encrypted_Port-ID (ACK) */
#define PLM_DS_REQUEST_KEY		0x0d	/* Request_key (-> Encryption_Key) */
#define PLM_DS_KEY_SWITCH		0x13	/* Key_Switching_Time (-> arm HW key switch) */
#define PLM_DS_ASSIGN_ALLOC_ID		0x0a	/* Assign_Alloc-ID (ACK) */
#define PLM_DS_REQUEST_PASSWORD		0x09	/* Request_Password (-> US Password 0x02) */
#define PLM_DS_CONFIG_PORT		0x0e	/* Configure_Port-ID (ACK) */
#define PLM_DS_CFG_VPVC			0x07	/* Configure_VP/VC (ATM, unsupported; ACK) */
#define PLM_DS_BER_INTERVAL		0x12	/* BER interval (ACK; arms BER reporting) */
#define PLM_US_ENCRYPT_KEY		0x05	/* US Encryption_Key response */
#define PLM_US_PASSWORD			0x02	/* US Password response (to Request_Password) */
#define PLM_US_SERIAL_NUMBER		0x01
#define PLM_US_ACKNOWLEDGE		0x09	/* US Acknowledge message type */
#define PLM_US_QUEUE_SN			0x6	/* US_PLOAM_IND[10:8] auto-SN queue */
#define PLM_US_QUEUE_URG		0x1	/* US_PLOAM_IND[10:8] urgent queue (ACKs) */
#define PLM_US_QUEUE_NOMSG		0x7	/* US_PLOAM_IND[10:8] HW auto-No_message slot */

#define GPON_GTC_DS_PORT_IND	0x1100		/* CAM op: OP_MODE[9:8] OP_IDX[6:0]. The op bits are
						 * GPON_GTC_CAM_OP_* in flowcore/regtable.h. */
#define GPON_GTC_DS_PORT_WR	0x1104		/* [11:0] gemPortId */
#define GPON_GTC_DS_PORT_RD	0x110c		/* [11:0] RDATA: stored gemPortId (READ op) */
#define GPON_GTC_DS_TRAFFIC_CFG	0x1400		/* array: base 0x1400, stride 4 bytes, idx 0..127,
						 * [4:0] traffic-type; the map's "array offset"=32 is
						 * in bits */
#define   DS_TRAFFIC_CFG_STRIDE	4u
#define   DS_TRAFFIC_IS_OMCI	BIT(2)
#define   DS_TRAFFIC_AES	BIT(4)		/* decrypt (stock's dsGemPort aes_en) */
#define GPON_GEM_US_PORT_MAP	0x6400		/* array: base 0x6400, stride 4 (one 32-bit
						 * word/entry), idx 0..127, [11:0] gemPortId.
						 * Flow 64 (OMCC) = 0x6400 + 64*4 = 0x6500. */
/*
 * The last valid index of that array, used by the compile-time
 * GPON_GEM_US_RANGE_OK() assertions on the flows this driver stamps.
 * PI_US_SRAM_NO is also 127 but is a different block. A write past the end
 * lands in an unrelated register and is accepted without complaint.
 */
#define   GEM_US_PORT_MAP_IDX_MAX 127u

/*
 * The two PTI registers beside the OMCI one. Both vendor chipdefs name them
 * GPON_GTC_DS_TDM_PTI and GPON_GTC_DS_ETH_PTI, each with four fields like
 * GPON_GTC_DS_OMCI_PTI. The driver writes stock's 0x11 into all three.
 */
#define GPON_GTC_DS_TDM_PTI	0x1200		/* DS PTI config, TDM flows; stock O5 = 0x11 */
#define GPON_GTC_DS_ETH_PTI	0x1208		/* DS PTI config, Ethernet flows; stock O5 = 0x11 */
#define GPON_GTC_DS_OMCI_PTI	0x1204		/* [6:4] PTI_MASK [2:0] END_PTI */
#define   DS_OMCI_PTI_VAL	((1u << 4) | 1u)	/* mask=1 ptn=1 -> 0x11 */
#define GPON_GEM_DS_MC_CFG	0x4080		/* [6] BROADCAST_PASS [4] NON_MULTICAST_PASS [3] FCS_CHK_EN */
#define   GEM_DS_MC_CFG_VAL	0x59u		/* stock O5 operating value, read live from an online stock ONU: BROADCAST_PASS(6)|NON_MULTICAST_PASS(4)|FCS_CHK_EN(3)|bit0. 0x18 (no broadcast/bit0) is wrong. */

/*
 * The RTL9602C's GTC offsets as data: the populated struct gpon_chip
 * (flowcore/regtable.h). Offsets are within the GTC block, as gpon_rd/gpon_wr
 * address it.
 *
 * Values are literals so external tooling can check each one against the vendor
 * chipdef. Each is also the value of the named #define above or beside it.
 *
 * Every offset is spelled REG_AT(...). C zero-fills any member a designated
 * initialiser omits, and offset 0 is a real register on these parts, so without
 * the wrapper a forgotten field would read as the register at 0. Now it decodes
 * to UNSET and every helper refuses with -ENXIO. The plain values are the two
 * strides and the three index masks; a stride left at 0 stamps every flow into
 * one slot.
 */
static const struct gpon_chip luna_gpon_chip = {
	.name = "RTL9602C",
	.gtc = {
		.ds_omci_pti		= REG_AT(0x1204),	/* = GPON_GTC_DS_OMCI_PTI; chipdef 0x701204 */
		.gem_us_port_map	= REG_AT(0x6400),	/* = GPON_GEM_US_PORT_MAP; chipdef 0x706400 */
		.gem_us_port_stride	= 4,		/* = GEM_US_PORT_MAP_STRIDE (luna_gpon.c); chipdef array-offset 32 bits */
		.gem_ds_mc_cfg		= REG_AT(0x4080),	/* = GPON_GEM_DS_MC_CFG; chipdef 0x704080 */
		.ds_traffic_cfg		= REG_AT(0x1400),	/* = GPON_GTC_DS_TRAFFIC_CFG; chipdef 0x701400 */
		.ds_traffic_stride	= 4,		/* = DS_TRAFFIC_CFG_STRIDE; chipdef array-offset 32 bits */
		/*
		 * The two indirect CAMs (gpon_gtc_cam_write). Addresses and bit
		 * layout are identical in both vendor chipdefs. The idx masks
		 * are each CAM's OP_IDX field width from those chipdefs
		 * (PORTID_OP_IDX len 7, ALLOCID_OP_IDX len 5): per-CAM facts,
		 * not tuning.
		 */
		.ds_port_ind		= REG_AT(0x1100),	/* = GPON_GTC_DS_PORT_IND; chipdef 0x701100 */
		.ds_port_wr		= REG_AT(0x1104),	/* = GPON_GTC_DS_PORT_WR; chipdef 0x701104 */
		.ds_port_rd		= REG_AT(0x110c),	/* = GPON_GTC_DS_PORT_RD; chipdef 0x70110C */
		.ds_port_idx_mask	= 0x7f,		/* 128 flows -- OP_IDX[6:0]; == GEM_US_PORT_MAP_IDX_MAX */
		.ds_alloc_ind		= REG_AT(0x10c0),	/* = GPON_GTC_DS_ALLOC_IND; chipdef 0x7010C0 */
		.ds_alloc_wr		= REG_AT(0x10c4),	/* = GPON_GTC_DS_ALLOC_WR; chipdef 0x7010C4 */
		.ds_alloc_rd		= REG_AT(0x10cc),	/* = GPON_GTC_DS_ALLOC_RD; chipdef 0x7010CC */
		.ds_alloc_idx_mask	= 0x1f,		/* 32 T-CONTs -- OP_IDX[4:0] */
		/*
		 * The three flow-indexed indirect counter muxes
		 * (gpon_gtc_cntr_read). Addresses and bit layout are identical
		 * in both vendor chipdefs. The MISC mux is deliberately absent:
		 * it has no ack field, and its idx width is the one counter
		 * fact that differs per chip.
		 */
		.gem_ds_rx_cntr_ind	= REG_AT(0x4040),	/* = GPON_GEM_DS_RX_CNTR_IND; chipdef 0x704040 */
		.gem_ds_rx_cntr_stat	= REG_AT(0x4044),	/* = GPON_GEM_DS_RX_CNTR_STAT; chipdef 0x704044 */
		.gem_ds_fwd_cntr_ind	= REG_AT(0x404c),	/* = GPON_GEM_DS_FWD_CNTR_IND; chipdef 0x70404C */
		.gem_ds_fwd_cntr_stat	= REG_AT(0x4050),	/* = GPON_GEM_DS_FWD_CNTR_STAT; chipdef 0x704050 */
		.ds_port_cntr_ind	= REG_AT(0x1140),	/* = GPON_GTC_DS_PORT_CNTR_IND; chipdef 0x701140 */
		.ds_port_cntr_stat	= REG_AT(0x1144),	/* = GPON_GTC_DS_PORT_CNTR_STAT; chipdef 0x701144 */
		.cntr_flow_idx_mask	= 0x7f,		/* IDX[6:0] on all three -- the 128-flow space */
	},
};

#define PI_PON_SID2QID		0x020f8		/* packed 7b/SID: physical queue */

/*
 * Each of these names a different register on the RTL9603CVD, so they must go
 * through PI_X(): a literal bypasses the per-chip translation. Names and 9602C
 * addresses are the vendor chipdef's own.
 */
#define PI_DRN_CMD		0x020e4u	/* SW_PBO_DSCCTRL on the 9603CVD   */
#define PI_PON_OLT_BW_MTR_FULL	0x0218cu	/* PON_SIDVALID on the 9603CVD     */
#define PI_PON_WFQ_TYPE		0x023e8u	/* PON_QID_PIR_RATE on the 9603CVD */
#define PI_RSVD_PONIP_DS	0x0a10cu	/* PON_Q_CONFIG_DS on the 9603CVD  */
/*
 * Read-only registers that moved on the RTL9603CVD. A literal here would print
 * the wrong register under the right label.
 */
#define PI_PON_DSC_USAGE_DS	0x0a0bcu	/* RX_STS_DS on the 9603CVD        */
#define PI_PON_IPSTS_US		0x020f4u	/* DSCRUNOUT_US on the 9603CVD     */
#define PI_PON_DSC_USAGE_US	0x020ecu	/* PONIP_CTL_US on the 9603CVD     */
#define PI_PON_IPSTS_DS		0x0a0c0u	/* PONIP_CTL_DS on the 9603CVD     */
#define PI_PON_DSC_STS_DS	0x0a0c8u	/* ARB_TIMEOUT_DS on the 9603CVD   */
/*
 * The SID2QID array is packed four SIDs per 32-bit word, so an element is the
 * base plus arithmetic. No per-address table can translate a flattened address
 * such as 0x2138 (0x20f8 + 16*4), since what it must translate is the base.
 */
#define PI_SID2QID_STRIDE	4u

/*
 * The upstream-scheduler cluster. The PON-IP move table is generated from the
 * PI_* names in this header, so a register spelled as a number here cannot be
 * translated. On the RTL9603CVD the RTL9602C addresses of PON_TCONT_EN and
 * PON_SCH_QMAP are undeclared holes: writes to them leave upstream T-CONTs
 * undrained, and read back as if correct.
 */
#define PI_PON_BW_THRES		0x02150u	/* [29:16] last [13:0] runt          */
#define PI_PON_DSC_STS_US	0x02158u	/* [12:0] SRAM [28:16] DRAM used     */
#define PI_MOCIR_FRC_MD		0x02170u	/* per-flow CIR force mask           */
#define PI_MOCIR_FRC_VAL	0x02174u	/* per-flow CIR force value          */
#define PI_MOCIR_TH_H		0x02184u	/* request/grant credit threshold hi */
#define PI_MOCIR_TH_L		0x02188u	/* ... lo                            */
#define PI_PON_TB_CTRL		0x02190u	/* token bucket tick [15:8]/[7:0]    */
#define PI_PON_SCH_CTRL		0x02194u	/* [18] PIR_DROP + METER_OP + burst  */
#define PI_PON_QID_CIR_RATE	0x02198u	/* [17:0] per-qid, one word per qid  */
#define PI_PON_QID_PIR_RATE	0x0229cu	/* [17:0] per-qid, one word per qid  */
#define PI_QID_RATE_STRIDE	4u		/* 18b field, 32/18 = 1 entry/word   */
#define PI_PON_SCH_QMAP		0x023a0u	/* T-CONT -> logical queue (packed)  */
#define PI_PON_TCONT_EN		0x023e4u	/* 1 bit per T-CONT: US scheduler on */
#define PI_PON_WFQ_WEIGHT	0x023f8u	/* packed 10b/qid                    */
#define PI_PONIP_DBG_CTRL_US	0x0255cu	/* [17] DBG_IGNORE_TAG               */
#define PI_PONIP_TOTAL_PAGE_CNT_US 0x02560u	/* [12:0] US pages staged       */
#define PI_PONIP_SID_USED_PAGE_CNT_US 0x02564u	/* [12:0] used [28:16] max   */
#define PI_GPON_DPRU_RPT_PRD	0x02568u	/* DBA block size + report period    */
#define PI_PONIP_SID_OVER_STS	0x0256cu	/* per-SID page-overflow status      */
#define PI_PONIP_SID_OVER_LATCH_STS 0x02578u	/* latched twin: a separate register, and the gap to
						 * it differs per chip (3 status words on the
						 * RTL9602C's 65 SIDs, 4 on the RTL9603CVD's 128) */
#define PI_PON_SCH_OPT		0x025d8u	/* [19] scheduler option bit         */
#define PI_PON_DS_PBO_PAGE_Q0	0x0a100u	/* [12:0] cur [25:13] max, DS q0     */
/*
 * PON_SCH_QMAP is 32 bits per entry on the RTL9602C and 8 bits per entry on the
 * RTL9603CVD, so base + tcont * 4 is RTL9602C-only. The per-chip width is
 * gpon_swc_map.sch_qmap_bits; entries_per_word = 32 / width, the same rule
 * pi_packed_locate() implements for SID2QID/SIDVALID.
 */

#define PI_PON_SIDVALID		0x0213c		/* packed 1b/SID */
#define PI_PON_OMCI_CFG		0x02154		/* [6:0] OMCI SID */
#define PI_PON_SID_Q_MAP_DS	0x0a0e4		/* packed 2b/SID: DS PBO queue */

#define GPON_GTC_DS_ALLOC_IND	0x10c0		/* T-CONT alloc CAM: OP_IDX[4:0]=tcont */
#define GPON_GTC_DS_ALLOC_WR	0x10c4		/* [11:0] allocateId */
#define GPON_GTC_DS_ALLOC_RD	0x10cc		/* [11:0] RDATA: stored allocateId (READ op) */

/*
 * The three flow-indexed indirect counter muxes and the MISC mux. Names and
 * addresses are the vendor chipdef's own, identical on the RTL9602C and
 * RTL9603CVD (GPON_GEM_DS_RX_CNTR_IND 0x704040 / _STAT 0x704044,
 * GPON_GEM_DS_FWD_CNTR_IND 0x70404C / _STAT 0x704050, GPON_GEM_DS_MISC_IND
 * 0x704064 / GPON_GEM_DS_MISC_CNTR_STAT 0x704068, GPON_GTC_DS_PORT_CNTR_IND
 * 0x701140 / _STAT 0x701144). The STAT registers are all clear-on-read.
 */
#define GPON_GEM_DS_RX_CNTR_IND		0x4040	/* IDX[6:0]=flow, R_ACK[15] */
#define GPON_GEM_DS_RX_CNTR_STAT	0x4044	/* [31:0] ETH_PKT_RX */
#define GPON_GEM_DS_FWD_CNTR_IND	0x404c	/* IDX[6:0]=flow, R_ACK[15] */
#define GPON_GEM_DS_FWD_CNTR_STAT	0x4050	/* [31:0] ETH_PKT_FWD */
/*
 * MISC has no R_ACK: bit 15 is reserved on both chipdefs, the vendor DAL reads
 * it with no poll, and stock's resting value shows it clear (0x04064=0x6) while
 * the three counters above latch theirs set. Its IDX is also the one counter
 * field whose width differs between the chips: [3:0] on the RTL9602C, [2:0] on
 * the RTL9603CVD (both cover the 7 declared types).
 */
#define GPON_GEM_DS_MISC_IND		0x4064	/* IDX only; NO ack field */
#define GPON_GEM_DS_MISC_CNTR_STAT	0x4068	/* [31:0] MISC_CNTR */
#define GPON_GTC_DS_PORT_CNTR_IND	0x1140	/* IDX[6:0]=flow, RSEL[8], R_ACK[15] */
#define GPON_GTC_DS_PORT_CNTR_STAT	0x1144	/* [31:0] GEM_CNTR */


/* The optical-LOS forcing path. */
#define   GPON_CDR_LOS_EN	BIT(2)		/* enable CDR-LOS monitor     */
#define   FIB_FP_CFG_FRC_SD	BIT(10)		/* FIB_REG16[10] force sig-detect */
#define   WSDS_OPTIC_LOS_SEL_EPON	BIT(15)
#define   WSDS_FRC_OPTIC_LOS		BIT(14)
#define   WSDS_FRCV_OPTIC_LOS		BIT(13)




/*
 * The SMI/MDIO master, and the SWCORE proxy built on it. These take the mapped
 * base so they stay shared family code.
 */
/* The SMI transaction is the SAME handshake the rest of this port already
 * routes through the core: write a command word carrying an enable bit, poll
 * the same register until the block clears it.  Only the bit differs
 * (SMI_CMD_EN is bit0, not the BIT(31) the GTC and NI blocks use), and
 * gpon_ind_poll() takes the busy bit as a parameter for exactly that reason.
 * The bound and the pace are the ones both vendor copies had. */
#define LUNA_SMI_TRIES		1000u

static inline u32 luna_sw_hwio_rd(void *ctx, u32 off)
{
	return ioread32((void __iomem *)ctx + off);
}

static inline void luna_sw_hwio_wr(void *ctx, u32 off, u32 val)
{
	iowrite32(val, (void __iomem *)ctx + off);
}

static inline void luna_smi_pause(void)
{
	udelay(10);
}

static inline struct hwio luna_sw_io(void __iomem *sw)
{
	struct hwio io = { .rd = luna_sw_hwio_rd, .wr = luna_sw_hwio_wr,
			   .ctx = (void *)sw };

	return io;
}

/**
 * luna_smi_read - one SMI read through the switch's MDIO master
 * @out: the 16-bit result, written only when the transaction completed
 *
 * Returns an error when the transaction never cleared CMD_EN.  Reading SMI_CTRL_3
 * anyway would return the previous transaction's data as this one's.
 */
static inline int luna_smi_read(void __iomem *sw, u8 phy, u8 reg, u16 *out)
{
	struct hwio io = luna_sw_io(sw);

	iowrite32(phy << 5, sw + SMI_BC_PHYID);
	iowrite32(BIT(phy), sw + SMI_CTRL_2);		/* target port mask */
	/* MAIN_PAGE=0x1FFF (broadcast), REG=phyreg, CMD=1 (trigger read) */
	iowrite32((0x1FFFu << 11) | ((u32)reg << 6) | SMI_CMD_EN,
		  sw + SMI_CTRL_0);
	if (gpon_ind_poll(&io, reg_make(SMI_CTRL_0), SMI_CMD_EN, LUNA_SMI_TRIES,
			  luna_smi_pause) < 0)
		return -ETIMEDOUT;
	*out = (u16)(ioread32(sw + SMI_CTRL_3) >> 16);
	return 0;
}

static inline void luna_smi_write(void __iomem *sw, u8 phy, u8 reg, u16 val)
{

	struct hwio io = luna_sw_io(sw);

	iowrite32(phy << 5, sw + SMI_BC_PHYID);
	iowrite32(BIT(phy), sw + SMI_CTRL_2);
	iowrite32(val, sw + SMI_CTRL_3);
	iowrite32((0x1FFFu << 11) | ((u32)reg << 6) | BIT(4) | SMI_CMD_EN,
		  sw + SMI_CTRL_0);
	/* the WRITE's completion is polled and the result deliberately unused,
	 * exactly as both vendor copies had it: there is nothing to hand back,
	 * and the next transaction re-arms the bus regardless. */
	gpon_ind_poll(&io, reg_make(SMI_CTRL_0), SMI_CMD_EN, LUNA_SMI_TRIES,
		      luna_smi_pause);
}


#endif /* _LUNA_GPON_REGS_H */
