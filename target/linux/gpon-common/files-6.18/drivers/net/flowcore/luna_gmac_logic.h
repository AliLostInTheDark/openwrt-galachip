/* SPDX-License-Identifier: GPL-2.0-only */
/* luna_gmac_logic.h -- pure logic of the Luna FAMILY GMAC ...
 * dev/MEASURED-luna_gmac_logic.h.md sec 1. */
#ifndef _LUNA_GMAC_LOGIC_H
#define _LUNA_GMAC_LOGIC_H

#include <linux/types.h>

/* ★★★ THE RX DESCRIPTOR FIELD POSITIONS ARE A PER-DIE DATUM ...
 * dev/MEASURED-luna_gmac_logic.h.md sec 2. */
struct luna_rx_layout {
	u8 src_lsb;		/* source-port field, always in opts3 */
	u8 src_bits;
	bool rsn_in_opts3;	/* reason word: opts3 (9602C) or opts2 (9603CVD) */
	u8 rsn_lsb;
	u8 rsn_bits;
};

extern const struct luna_rx_layout luna_rx_layout_rtl9602c;
extern const struct luna_rx_layout luna_rx_layout_rtl9603cvd;

unsigned int luna_gmac_rx_src_port(const struct luna_rx_layout *l, u32 opts3);
unsigned int luna_gmac_rx_reason(const struct luna_rx_layout *l, u32 opts2,
				 u32 opts3);
bool luna_gmac_rx_is_wan(const struct luna_rx_layout *rxl, u32 opts3, unsigned int pon_port);

u32 luna_gmac_rxdesnum_pack(unsigned int ring_size, unsigned int th_on,
			    unsigned int th_off);
u32 luna_gmac_rxcdo_pack(unsigned int ring_size);
bool luna_gmac_tx_zero_copy(bool nonlinear, unsigned int len,
			    unsigned int min_len);
bool luna_gmac_rx_frame_bad(u32 opts1, u32 err_mask, u32 len,
			    u32 hdr_floor, u32 buf_size);
bool luna_gmac_rx_cpu_tag_present(const u8 *data, u32 len,
				  unsigned int tag_len);

/* The family's cpu-tag descriptor decisions. Every one of ...
 * dev/MEASURED-luna_gmac_logic.h.md sec 3. */
bool luna_gmac_rx_is_ds_omci(const struct luna_rx_layout *rxl,
			     bool trap_on, u32 opts2, u32 opts3, u32 len,
			     unsigned int omci_reason,
			     unsigned int cpu_prefix, u32 buf_size);
/* The cpu-tag TX steering word2 for an EXPLICIT egress port ...
 * dev/MEASURED-luna_gmac_logic.h.md sec 8. */
u32 luna_gmac_cputag_txd_pmask(u32 port_mask);
u32 luna_gmac_cputag_txd_word2(unsigned int pon_port);
u32 luna_gmac_cputag_txd_word3(unsigned int stream_id);

/* The switch port a received frame ingressed on, and whether ...
 * dev/MEASURED-luna_gmac_logic.h.md sec 4. */
unsigned int luna_gmac_rx_pon_sid(u32 opts3);


/* ── family GMAC RING arithmetic (folded in from ... -- dev/MEASURED-luna_gmac_logic.h.md sec 5. */
static inline bool luna_gmac_tx_ring_full(unsigned int head, unsigned int dirty,
					  unsigned int size, unsigned int reserve)
{
	return (head - dirty) >= size - 1 - reserve;
}

/* Is @slot the ring's wrap descriptor -- the one that carries ...
 * dev/MEASURED-luna_gmac_logic.h.md sec 6. */
static inline bool luna_gmac_slot_is_eor(unsigned int slot, unsigned int size)
{
	return slot == size - 1;
}

/* The TX descriptor BODY word (opts1/word0) of a ... -- dev/MEASURED-luna_gmac_logic.h.md sec 7. */
static inline u32 luna_gmac_txd_word0(u32 flags, u32 len, u32 len_mask,
				      bool eor, u32 eor_bit)
{
	return flags | (len & len_mask) | (eor ? eor_bit : 0u);
}

#endif /* _LUNA_GMAC_LOGIC_H */
