// SPDX-License-Identifier: GPL-2.0-only
/* See cortina_gpon_logic.h. Moved verbatim; no line was rewritten.
 */
#include <linux/types.h>
#include <linux/errno.h>

#include "gpon_sn.h"
#include "gpon_common.h"

#include "cortina_gpon_logic.h"

/* One 32-bit register value from 4 wire-order bytes (endianness-agnostic). */
u32 cg_sn_word(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* ★★★ THE SERIAL-NUMBER CODEC MOVED TO THE COMMON CORE ...
 * dev/MEASURED-cortina_gpon_logic.c.md sec 1. */
int cg_sn_parse(const char *s, u8 out[8])
{
	return gpon_sn_parse(s, out) ? -EINVAL : 0;
}

/* The exact inverse of cg_sn_word() above: unpack a 32-bit ...
 * dev/MEASURED-cortina_gpon_logic.c.md sec 2. */
void cg_vendor_unpack(u32 v, char out[5])
{
	out[0] = (v >> 24) & 0xff;
	out[1] = (v >> 16) & 0xff;
	out[2] = (v >> 8) & 0xff;
	out[3] = v & 0xff;
	out[4] = '\0';
}

/* One PDC map entry's DATA words, moved verbatim from ...
 * dev/MEASURED-cortina_gpon_logic.c.md sec 3. */
void cg_pdc_map_entry(u32 idx, u32 omcc_gems, u32 *d0, u32 *d1)
{
	if (idx < omcc_gems) {
		*d0 = CG_PDC_D0_COS(6) | CG_PDC_D0_LDPID(CG_LPORT_CPU_0) |
		      CG_PDC_D0_LSPID(CG_LPORT_PON) |
		      CG_PDC_D0_FE_BYPASS | CG_PDC_D0_NO_DROP;
		*d1 = CG_PDC_D1_POL_ID(idx + 0x80);
	} else {
		*d0 = CG_PDC_D0_LDPID(CG_LPORT_L3_WAN) |
		      CG_PDC_D0_LSPID(CG_LPORT_PON);
		*d1 = CG_PDC_D1_POL_ID(idx - 8);
	}
}

/* The PUC pvtbl entry's DATA0/1/2 words for one T-CONT, moved ...
 * dev/MEASURED-cortina_gpon_logic.c.md sec 4. */
void cg_puc_pvtbl_words(u32 tcont, bool ena, u32 *d0, u32 *d1, u32 *d2)
{
	u32 voq[CG_PUC_QUEUE_PER_TCONT];
	u32 q;

	for (q = 0; q < CG_PUC_QUEUE_PER_TCONT; q++)
		voq[q] = (q + tcont * CG_PUC_QUEUE_PER_TCONT) | ((u32)ena << 8);

	*d0 = voq[0] | (voq[1] << 9) | (voq[2] << 18) |
	      ((voq[3] & 0x1f) << 27);
	*d1 = ((voq[3] >> 5) & 0xf) | (voq[4] << 4) | (voq[5] << 13) |
	      (voq[6] << 22) | ((voq[7] & 1) << 31);
	*d2 = ((voq[7] >> 1) & 0xff) | BIT(12);	/* schmode=0, entryvld=1 */
}

/* O5 exit = link down (vendor condition; the same G.984.3 ...
 * dev/MEASURED-cortina_gpon_logic.c.md sec 5. */
bool cg_link_down_transition(u8 last, u8 state)
{
	return (last == CG_STATE_OPERATION && state != CG_STATE_OPERATION &&
		state != CG_STATE_POPUP) ||
	       (last == CG_STATE_POPUP && state != CG_STATE_OPERATION &&
		state != CG_STATE_RANGING) ||
	       state == CG_STATE_ESTOP;
}

bool cg_dwell_step(struct cg_dwell *d, u8 cg_state, u32 now_ms)
{
	u8 state = cg_state + 1;	/* the MAC numbers O1..O7 from 0 */

	if (state != d->state) {
		d->state = state;
		d->since_ms = now_ms;
		return false;
	}
	if (!gpon_dwell_expired(state, (now_ms - d->since_ms) / GPON_DWELL_TICK_MS,
				GPON_DWELL_TO1_TICKS))
		return false;
	d->state = 0;	/* the next reading starts a new episode */
	return true;
}
