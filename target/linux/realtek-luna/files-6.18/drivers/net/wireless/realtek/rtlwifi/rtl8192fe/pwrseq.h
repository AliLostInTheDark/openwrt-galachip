/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2026  Realtek RTL8192FE clean-room driver authors */

#ifndef __RTL92F_PWRSEQ_H__
#define __RTL92F_PWRSEQ_H__

#include "../pwrseqcmd.h"

/* The RTL8192F power architecture exposes the same six ... -- dev/MEASURED-pwrseq.h.md sec 1. */
#define	RTL8192F_TRANS_CARDEMU_TO_ACT_STEPS	22
#define	RTL8192F_TRANS_ACT_TO_CARDEMU_STEPS	18
#define	RTL8192F_TRANS_CARDEMU_TO_SUS_STEPS	18
#define	RTL8192F_TRANS_SUS_TO_CARDEMU_STEPS	18
#define	RTL8192F_TRANS_CARDEMU_TO_PDN_STEPS	18
#define	RTL8192F_TRANS_PDN_TO_CARDEMU_STEPS	18
#define	RTL8192F_TRANS_ACT_TO_LPS_STEPS		23
#define	RTL8192F_TRANS_LPS_TO_ACT_STEPS		23
#define	RTL8192F_TRANS_END_STEPS		1

/* CARDEMU -> ACT : the RTL8192F disabled_to_emu + ... -- dev/MEASURED-pwrseq.h.md sec 2. */
#define RTL8192F_TRANS_CARDEMU_TO_ACT					\
	/* { offset, cut_msk, fab_msk|interface_msk, base|cmd, msk, value },*/\
	/* clear HW power down 0x04[15]=0 and HW suspend 0x04[11]=0 */	\
	{0x0005, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(7) | BIT(3), 0},		\
	/* enable LDOA12 macro block 0x20[0]=1 */			\
	{0x0020, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(0), BIT(0)},		\
	/* disable the shared BT/GPS pad 0x67[4]=0 (REG_PAD_CTRL1[28]) */\
	{0x0067, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(4), 0},			\
	/* settle the LDO before releasing isolation */			\
	{0x0000, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_DELAY, 1, PWRSEQ_DELAY_MS},		\
	/* release analog Ips to digital 0x00[5]=0 */			\
	{0x0000, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(5), 0},			\
	/* clear PCIe / HW suspend / SW LPS 0x04[12:10]=0 */		\
	{0x0005, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(4) | BIT(3) | BIT(2), 0},	\
	/* wait till 0x04[17] = 1 power ready */			\
	{0x0006, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_POLLING, BIT(1), BIT(1)},		\
	/* release WLON reset 0x04[16]=1 */				\
	{0x0006, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(0), BIT(0)},		\
	/* poll till 0x04[9:8]=0 (MAC off and MAC enable both cleared) */\
	{0x0005, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_POLLING, BIT(1) | BIT(0), 0},		\
	/* SWR OCP enable 0x10[2]=1 (REG_AFE_MISC[18]) */		\
	{0x0012, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(2), BIT(2)},		\
	/* clear HW power down 0x04[15]=0 (re-assert after WLON) */	\
	{0x0005, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(7), 0},			\
	/* clear PCIe / HW suspend 0x04[12:11]=0 */			\
	{0x0005, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(4) | BIT(3), 0},		\
	/* 0x7F[7]=1, LDO at max output capability (REG_LDO_SW_CTRL[31]) */\
	{0x007F, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(7), BIT(7)},		\
	/* enable MAC by HW state machine 0x04[8]=1 */			\
	{0x0005, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(0), BIT(0)},		\
	/* poll till 0x04[8]=0, MAC enable handshake complete */	\
	{0x0005, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_POLLING, BIT(0), 0},			\
	/* disable register lock 0x1C[7]=1 (REG_RSV_CTRL) */		\
	{0x001C, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(7), BIT(7)},		\
	/* enable RF path S1 0x1F=0x07 (RF_ENABLE|RF_RSTB|RF_SDMRSTB) */	\
	{0x001F, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, 0xFF, 0x07},			\
	/* reset RF path S0 0x7B=0x00 (vendor pairs S1@0x1F with S0@0x7B) */\
	{0x007B, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, 0xFF, 0x00},			\
	/* enable RF path S0 0x7B=0x07 (RF_ENABLE|RF_RSTB|RF_SDMRSTB) */	\
	{0x007B, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, 0xFF, 0x07},			\
	/* AFE_Ctrl 0x97[5]=1 (analog TX front-end power-on) */		\
	{0x0097, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(5), BIT(5)},		\
	/* AFE_Ctrl 0xDC=0xC4 (analog TX front-end power-on) */		\
	{0x00DC, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, 0xFF, 0xC4},			\
	/* re-enable register lock 0x1C[7]=0 */				\
	{0x001C, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(7), 0},

/* ACT -> CARDEMU : the RTL8192F active_to_emu teardown. Turns ...
 * dev/MEASURED-pwrseq.h.md sec 3. */
#define RTL8192F_TRANS_ACT_TO_CARDEMU					\
	/* { offset, cut_msk, fab_msk|interface_msk, base|cmd, msk, value },*/\
	/* 0x1F[7:0] = 0 turn off RF */					\
	{0x001F, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, 0xFF, 0},			\
	/* reset BB, RF enters power down 0x02[0]=0 (SYS_FUNC_BBRSTB) */	\
	{0x0002, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(0), 0},			\
	/* release WLON reset 0x04[16]=1 */				\
	{0x0006, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(0), BIT(0)},		\
	/* 0x04[9] = 1 turn off MAC by HW state machine */		\
	{0x0005, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(1), BIT(1)},		\
	/* wait till 0x04[9] = 0 polling until return 0 */		\
	{0x0005, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_POLLING, BIT(1), 0},			\
	/* analog Ips to digital, 1:isolation 0x00[5]=1 */		\
	{0x0000, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(5), BIT(5)},		\
	/* disable LDOA12 macro block 0x20[0]=0 */			\
	{0x0020, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(0), 0},

/* CARDEMU -> SUS : enter WL suspend. PCIe keeps ... -- dev/MEASURED-pwrseq.h.md sec 4. */
#define RTL8192F_TRANS_CARDEMU_TO_SUS					\
	/* { offset, cut_msk, fab_msk|interface_msk, base|cmd, msk, value },*/\
	/* 0x04[12:11] = 2b'11 enable WL suspend for PCIe */		\
	{0x0005, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_PCI_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(4) | BIT(3), BIT(4) | BIT(3)},\
	/* 0x04[12:11] = 2b'01 enable WL suspend for USB/SDIO */	\
	{0x0005, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK,			\
	 PWR_INTF_USB_MSK | PWR_INTF_SDIO_MSK, PWR_BASEADDR_MAC,		\
	 PWR_CMD_WRITE, BIT(4) | BIT(3), BIT(3)},			\
	/* SOP option to disable BG/MB 0x07=0x20 */			\
	{0x0007, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, 0xFF, 0x20},			\
	/* enable GPIO9 as EXT WAKEUP 0x16[0]=1 (REG_GPIO_INTM[16]) */	\
	{0x0016, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(0), BIT(0)},		\
	/* set SDIO suspend local register */				\
	{0x0086, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_SDIO_MSK,	\
	 PWR_BASEADDR_SDIO, PWR_CMD_WRITE, BIT(0), BIT(0)},		\
	/* wait power state to suspend */				\
	{0x0086, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_SDIO_MSK,	\
	 PWR_BASEADDR_SDIO, PWR_CMD_POLLING, BIT(1), 0},

/*
 * SUS -> CARDEMU : leave WL suspend.  Clears the EXT-wakeup, clears the
 * suspend controls and reverses the SDIO local handshake.
 */
#define RTL8192F_TRANS_SUS_TO_CARDEMU					\
	/* { offset, cut_msk, fab_msk|interface_msk, base|cmd, msk, value },*/\
	/* clear EXT WAKEUP 0x16[0]=0 */				\
	{0x0016, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(0), 0},			\
	/* clear SDIO suspend local register */				\
	{0x0086, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_SDIO_MSK,	\
	 PWR_BASEADDR_SDIO, PWR_CMD_WRITE, BIT(0), 0},			\
	/* wait power state to resume */				\
	{0x0086, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_SDIO_MSK,	\
	 PWR_BASEADDR_SDIO, PWR_CMD_POLLING, BIT(1), BIT(1)},		\
	/* 0x04[12:11] = 2b'00 disable WL suspend */			\
	{0x0005, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(4) | BIT(3), 0},

/* CARDEMU -> CARDDIS : card disable. SOP disables BG/MB, the ...
 * dev/MEASURED-pwrseq.h.md sec 5. */
#define RTL8192F_TRANS_CARDEMU_TO_CARDDIS				\
	/* { offset, cut_msk, fab_msk|interface_msk, base|cmd, msk, value },*/\
	/* SOP option to disable BG/MB 0x07=0x20 */			\
	{0x0007, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, 0xFF, 0x20},			\
	/* unlock small LDO register 0xCC[2]=1 */			\
	{0x00CC, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_SDIO_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(2), BIT(2)},		\
	/* disable small LDO 0x11[0]=0 */				\
	{0x0011, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_SDIO_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(0), 0},			\
	/* 0x04[12:11] = 2b'01 enable WL suspend for USB/SDIO */	\
	{0x0005, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK,			\
	 PWR_INTF_USB_MSK | PWR_INTF_SDIO_MSK, PWR_BASEADDR_MAC,		\
	 PWR_CMD_WRITE, BIT(4) | BIT(3), BIT(3)},			\
	/* 0x04[10] = 1 enable SW LPS for PCIe */			\
	{0x0005, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_PCI_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(2), BIT(2)},		\
	/* enable GPIO9 as EXT WAKEUP 0x16[0]=1 */			\
	{0x0016, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(0), BIT(0)},		\
	/* set SDIO suspend local register */				\
	{0x0086, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_SDIO_MSK,	\
	 PWR_BASEADDR_SDIO, PWR_CMD_WRITE, BIT(0), BIT(0)},		\
	/* wait power state to suspend */				\
	{0x0086, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_SDIO_MSK,	\
	 PWR_BASEADDR_SDIO, PWR_CMD_POLLING, BIT(1), 0},

/*
 * CARDDIS -> CARDEMU : card enable.  Reverses the SDIO local handshake,
 * re-enables/locks the small LDO and drops the WL suspend controls.
 */
#define RTL8192F_TRANS_CARDDIS_TO_CARDEMU				\
	/* { offset, cut_msk, fab_msk|interface_msk, base|cmd, msk, value },*/\
	/* clear SDIO suspend local register */				\
	{0x0086, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_SDIO_MSK,	\
	 PWR_BASEADDR_SDIO, PWR_CMD_WRITE, BIT(0), 0},			\
	/* wait power state to resume */				\
	{0x0086, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_SDIO_MSK,	\
	 PWR_BASEADDR_SDIO, PWR_CMD_POLLING, BIT(1), BIT(1)},		\
	/* enable small LDO 0x11[0]=1 */				\
	{0x0011, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_SDIO_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(0), BIT(0)},		\
	/* lock small LDO register 0xCC[2]=0 */				\
	{0x00CC, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_SDIO_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(2), 0},			\
	/* clear EXT WAKEUP 0x16[0]=0 */				\
	{0x0016, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(0), 0},			\
	/* 0x04[12:11] = 2b'00 disable WL suspend */			\
	{0x0005, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(4) | BIT(3), 0},

/*
 * CARDEMU -> PDN : enter hardware power-down.  Drop WLON, raise the
 * HW-power-down request.
 */
#define RTL8192F_TRANS_CARDEMU_TO_PDN					\
	/* { offset, cut_msk, fab_msk|interface_msk, base|cmd, msk, value },*/\
	/* 0x04[16] = 0 */						\
	{0x0006, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(0), 0},			\
	/* 0x04[15] = 1 */						\
	{0x0005, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(7), BIT(7)},

/*
 * PDN -> CARDEMU : leave hardware power-down.
 */
#define RTL8192F_TRANS_PDN_TO_CARDEMU					\
	/* { offset, cut_msk, fab_msk|interface_msk, base|cmd, msk, value },*/\
	/* 0x04[15] = 0 */						\
	{0x0005, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(7), 0},

/* ACT -> LPS : firmware-driven low power. Stop PCIe DMA, ... -- dev/MEASURED-pwrseq.h.md sec 6. */
#define RTL8192F_TRANS_ACT_TO_LPS					\
	/* { offset, cut_msk, fab_msk|interface_msk, base|cmd, msk, value },*/\
	/* PCIe DMA stop 0x301=0xFF */					\
	{0x0301, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_PCI_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, 0xFF, 0xFF},			\
	/* Tx pause 0x522=0xFF */					\
	{0x0522, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, 0xFF, 0xFF},			\
	/* should be zero if no packet is transmitting */		\
	{0x05F8, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_POLLING, 0xFF, 0},			\
	/* should be zero if no packet is transmitting */		\
	{0x05F9, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_POLLING, 0xFF, 0},			\
	/* should be zero if no packet is transmitting */		\
	{0x05FA, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_POLLING, 0xFF, 0},			\
	/* should be zero if no packet is transmitting */		\
	{0x05FB, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_POLLING, 0xFF, 0},			\
	/* CCK and OFDM are disabled and clock is gated 0x02[0]=0 */	\
	{0x0002, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(0), 0},			\
	/* delay 1us */							\
	{0x0002, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_DELAY, 0, PWRSEQ_DELAY_US},		\
	/* whole BB is reset 0x02[1]=0 */				\
	{0x0002, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(1), 0},			\
	/* reset MAC TRX 0x100=0x03 */					\
	{0x0100, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, 0xFF, 0x03},			\
	/* clear security enable 0x101[1]=0 */				\
	{0x0101, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(1), 0},			\
	/* when entering Sus / Disable enable LOP for BT */		\
	{0x0093, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_SDIO_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, 0xFF, 0x00},			\
	/* respond TxOK to scheduler 0x553[5]=1 */			\
	{0x0553, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(5), BIT(5)},

/* LPS -> ACT : firmware-driven wake. RPWM toggle, switch TSF ...
 * dev/MEASURED-pwrseq.h.md sec 7. */
#define RTL8192F_TRANS_LPS_TO_ACT					\
	/* { offset, cut_msk, fab_msk|interface_msk, base|cmd, msk, value },*/\
	/* SDIO RPWM */							\
	{0x0080, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_SDIO_MSK,	\
	 PWR_BASEADDR_SDIO, PWR_CMD_WRITE, 0xFF, 0x84},			\
	/* USB RPWM */							\
	{0xFE58, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_USB_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, 0xFF, 0x84},			\
	/* PCIe RPWM */							\
	{0x0361, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_PCI_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, 0xFF, 0x84},			\
	/* delay */							\
	{0x0002, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_DELAY, 0, PWRSEQ_DELAY_MS},		\
	/* 0x08[4] = 0 switch TSF to 40M */				\
	{0x0008, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(4), 0},			\
	/* poll 0x109[7]=0  TSF in 40M */				\
	{0x0109, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_POLLING, BIT(7), 0},			\
	/* 0x101[1] = 1 enable security */				\
	{0x0101, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(1), BIT(1)},		\
	/* 0x100[7:0] = 0xFF enable WMAC TRX */				\
	{0x0100, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, 0xFF, 0xFF},			\
	/* 0x02[1:0] = 2b'11 enable BB macro */				\
	{0x0002, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, BIT(1) | BIT(0), BIT(1) | BIT(0)},\
	/* 0x522 = 0 release Tx pause */				\
	{0x0522, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, 0xFF, 0},			\
	/* clear ISR 0x13D=0xFF */					\
	{0x013D, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 PWR_BASEADDR_MAC, PWR_CMD_WRITE, 0xFF, 0xFF},

/* Terminator entry that ends every concatenated flow. */
#define RTL8192F_TRANS_END						\
	/* { offset, cut_msk, fab_msk|interface_msk, base|cmd, msk, value },*/\
	{0xFFFF, PWR_CUT_ALL_MSK, PWR_FAB_ALL_MSK, PWR_INTF_ALL_MSK,	\
	 0, PWR_CMD_END, 0, 0},

extern struct wlan_pwr_cfg rtl8192f_power_on_flow
					[RTL8192F_TRANS_CARDEMU_TO_ACT_STEPS +
					 RTL8192F_TRANS_END_STEPS];
extern struct wlan_pwr_cfg rtl8192f_radio_off_flow
					[RTL8192F_TRANS_ACT_TO_CARDEMU_STEPS +
					 RTL8192F_TRANS_END_STEPS];
extern struct wlan_pwr_cfg rtl8192f_card_disable_flow
					[RTL8192F_TRANS_ACT_TO_CARDEMU_STEPS +
					 RTL8192F_TRANS_CARDEMU_TO_PDN_STEPS +
					 RTL8192F_TRANS_END_STEPS];
extern struct wlan_pwr_cfg rtl8192f_card_enable_flow
					[RTL8192F_TRANS_ACT_TO_CARDEMU_STEPS +
					 RTL8192F_TRANS_CARDEMU_TO_PDN_STEPS +
					 RTL8192F_TRANS_END_STEPS];
extern struct wlan_pwr_cfg rtl8192f_suspend_flow
					[RTL8192F_TRANS_ACT_TO_CARDEMU_STEPS +
					 RTL8192F_TRANS_CARDEMU_TO_SUS_STEPS +
					 RTL8192F_TRANS_END_STEPS];
extern struct wlan_pwr_cfg rtl8192f_resume_flow
					[RTL8192F_TRANS_ACT_TO_CARDEMU_STEPS +
					 RTL8192F_TRANS_CARDEMU_TO_SUS_STEPS +
					 RTL8192F_TRANS_END_STEPS];
extern struct wlan_pwr_cfg rtl8192f_hwpdn_flow
					[RTL8192F_TRANS_ACT_TO_CARDEMU_STEPS +
					 RTL8192F_TRANS_CARDEMU_TO_PDN_STEPS +
					 RTL8192F_TRANS_END_STEPS];
extern struct wlan_pwr_cfg rtl8192f_enter_lps_flow
					[RTL8192F_TRANS_ACT_TO_LPS_STEPS +
					 RTL8192F_TRANS_END_STEPS];
extern struct wlan_pwr_cfg rtl8192f_leave_lps_flow
					[RTL8192F_TRANS_LPS_TO_ACT_STEPS +
					 RTL8192F_TRANS_END_STEPS];

/* RTL8192FE power configuration command flows for the PCIe interface. */
#define RTL8192F_NIC_PWR_ON_FLOW	rtl8192f_power_on_flow
#define RTL8192F_NIC_RF_OFF_FLOW	rtl8192f_radio_off_flow
#define RTL8192F_NIC_DISABLE_FLOW	rtl8192f_card_disable_flow
#define RTL8192F_NIC_ENABLE_FLOW	rtl8192f_card_enable_flow
#define RTL8192F_NIC_SUSPEND_FLOW	rtl8192f_suspend_flow
#define RTL8192F_NIC_RESUME_FLOW	rtl8192f_resume_flow
#define RTL8192F_NIC_PDN_FLOW		rtl8192f_hwpdn_flow
#define RTL8192F_NIC_LPS_ENTER_FLOW	rtl8192f_enter_lps_flow
#define RTL8192F_NIC_LPS_LEAVE_FLOW	rtl8192f_leave_lps_flow

#endif
