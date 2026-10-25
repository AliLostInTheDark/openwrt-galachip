/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2026  rtl8192fe clean-room contributors */

#ifndef __RTL92F_DEF_H__
#define __RTL92F_DEF_H__

/* RX DMA ring depth (descriptors). The TX ring depth (TX_DESC_NUM_92E = 512)
 * and the buffer-descriptor segment count (BUFDESC_SEG_NUM) are provided by
 * the shared rtlwifi PCI core (pci.h / wifi.h); this sub-driver reuses them.
 */
#define RX_DESC_NUM_92F					384

/* TX DMA ring depth (descriptors). MUST equal the SW ring ... -- dev/MEASURED-def.h.md sec 1. */
#define TX_DESC_NUM_92F					128

/* Buffer-descriptor segment count for this chip's PCIe BD ring.
 * 0: 2 seg, 1: 4 seg, 2: 8 seg.  The RTL8192F uses the 4-segment layout
 * (seg0 = TX descriptor, seg1 = payload), matching the core default.
 */
#define RTL8192FE_SEG_NUM				BUFDESC_SEG_NUM

#define HAL_PRIME_CHNL_OFFSET_DONT_CARE			0
#define HAL_PRIME_CHNL_OFFSET_LOWER			1
#define HAL_PRIME_CHNL_OFFSET_UPPER			2

#define RX_MPDU_QUEUE					0

#define IS_HT_RATE(_rate)	\
	((_rate) >= DESC_RATEMCS0)
#define IS_CCK_RATE(_rate)	\
	((_rate) >= DESC_RATE1M && (_rate) <= DESC_RATE11M)
#define IS_OFDM_RATE(_rate)	\
	((_rate) >= DESC_RATE6M && (_rate) <= DESC_RATE54M)

/* Chip-version magic for the RTL8192F (2T2R 802.11n). The ... -- dev/MEASURED-def.h.md sec 2. */
enum version_8192f {
	VERSION_TEST_CHIP_2T2R_8192F = 0x0020,
	VERSION_NORMAL_CHIP_2T2R_8192F = 0x1028,
	VERSION_UNKNOWN_8192F = 0xFF,
};

/* HW queue-selector values written into the TX descriptor QSEL field. */
enum rtl_desc_qsel {
	QSLT_BK = 0x2,
	QSLT_BE = 0x0,
	QSLT_VI = 0x5,
	QSLT_VO = 0x7,
	QSLT_BEACON = 0x10,
	QSLT_HIGH = 0x11,
	QSLT_MGNT = 0x12,
	QSLT_CMD = 0x13,
};

/* TX/RX descriptor rate codes (DESC_RATE1M..DESC_RATEMCS15) ... -- dev/MEASURED-def.h.md sec 3. */

#endif
