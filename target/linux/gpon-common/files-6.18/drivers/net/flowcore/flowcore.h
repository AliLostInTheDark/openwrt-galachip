/* SPDX-License-Identifier: GPL-2.0-only */
/* flowcore.h -- the hardware-decoupled half of L3/L4 FLOW ...
 * dev/MEASURED-flowcore.h.md sec 1. */
#ifndef _FLOWCORE_H
#define _FLOWCORE_H

#include <linux/types.h>

/** Reverse the bits of a byte. */
u8 flowcore_bitrev8(u8 v);
/** Reverse the bits of a 32-bit word. */
u32 flowcore_bitrev32(u32 v);

/* flowcore_key_bitrev() - reverse a key's WORD ORDER, ... -- dev/MEASURED-flowcore.h.md sec 2. */
void flowcore_key_bitrev(u32 *w, int n_words);

/* flowcore_crc32_reflected() - reflected CRC-32 (Ethernet ...
 * dev/MEASURED-flowcore.h.md sec 5. */
u32 flowcore_crc32_reflected(const u8 *p, u32 len);

/* flowcore_crc16_ccitt_reflected() - reflected CRC-16/CCITT ...
 * dev/MEASURED-flowcore.h.md sec 3. */
u16 flowcore_crc16_ccitt_reflected(const u8 *p, u32 len);

/* Packed-array slot math: where a `bits`-wide entry lives ...
 * dev/MEASURED-flowcore.h.md sec 4. */
struct pi_packed_slot {
	u32 reg;		/* byte address of the 32-bit word (driver-relative) */
	unsigned int shift;	/* bit position of the field inside that word */
	u32 mask;		/* field mask, unshifted */
};

struct pi_packed_slot pi_packed_locate(u32 base, unsigned int idx,
				       unsigned int bits);
u32 pi_packed_insert(u32 word, const struct pi_packed_slot *slot, u32 val);
u32 pi_packed_extract(u32 word, const struct pi_packed_slot *slot);

#endif /* _FLOWCORE_H */
