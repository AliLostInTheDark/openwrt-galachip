/* SPDX-License-Identifier: GPL-2.0-or-later */
/* gpon_common.h -- THE CONTRACT between the HW-decoupled GPON ...
 * dev/MEASURED-gpon_common.h.md sec 10. */
#ifndef GPON_COMMON_H
#define GPON_COMMON_H

#include <linux/types.h>

/* HOST builds only; in kernel context compiler_attributes.h already defines it.
 * ★ A COMMENT WILL NOT DO: both targets build -Werror
 *   -Wimplicit-fallthrough=5 (measured 2026-08-05 from scripts/mod/.empty.o.cmd)
 *   and at level 5 only the ATTRIBUTE suppresses the diagnostic. */
#ifndef fallthrough
# if defined(__GNUC__) && __GNUC__ >= 7
#  define fallthrough	__attribute__((__fallthrough__))
# else
#  define fallthrough	do {} while (0)
# endif
#endif

/* G.984.3: the ONU carries 0xff until Assign_ONU-ID lands, and every re-range
 * path restores it. */
#define GPON_ONU_ID_BROADCAST	0xffu

/* The G.984 broadcast GEM port-id — one spec value, not a per-chip difference
 * (luna GPON_MCAST_GEM 0xfff, elnath CG_MCAST_GEM_ID 4095). */
#define GPON_MCAST_GEM_PORT	4095u

/* Octets of an upstream PLOAM handed to ->ploam_tx().  G.984.3 Table 9-3:
 * ONU-ID + Message-ID + 10 message octets; the 13th octet (CRC) is appended by
 * SILICON and is therefore NOT part of this buffer. */
#define GPON_PLOAM_US_LEN	12u

/* Canonical ONU activation state, G.984.3 clause 10 names, ...
 * dev/MEASURED-gpon_common.h.md sec 2. */
enum gpon_ostate {
	GPON_O1_INITIAL		= 1,	/* no downstream sync                */
	GPON_O2_STANDBY		= 2,	/* DS sync, awaiting parameters      */
	GPON_O3_SERIAL		= 3,	/* serial-number acquisition         */
	GPON_O4_RANGING		= 4,	/* ranging (EqD assignment)          */
	GPON_O5_OPERATION	= 5,	/* operational                       */
	GPON_O6_POPUP		= 6,	/* intermittent LOS/LOF, popup       */
	GPON_O7_EMERGENCY	= 7,	/* emergency stop (SN disabled)      */
};

/* The dwell rule, whoever runs the FSM (our core on Luna, the MAC silicon on Cortina): a state
 * held this many ticks without progress goes back to O1.  O3: an SN nobody hears
 * (MEASURED-gpon_ploam.c.md sec 47); O4: G.984.3 TO1, stock's 10 s (sec 48). */
#define GPON_DWELL_TICK_MS	10
#define GPON_DWELL_O3_TICKS	3000
#define GPON_DWELL_TO1_TICKS	1000

static inline bool gpon_dwell_expired(int state, u32 held_ticks, u32 to1_ticks)
{
	return (state == GPON_O3_SERIAL && held_ticks > GPON_DWELL_O3_TICKS) ||
	       (state == GPON_O4_RANGING && to1_ticks && held_ticks > to1_ticks);
}

/* ME 268 (GEM port network CTP) "direction", as it arrives in a Set-by-Create.
 * ★ G.988 clause 9.2.3 is the authority and 00-COMMON-LAYER-PLAN.md §3 has US
 *   and DS SWAPPED — a wrong name in the CONTRACT is a defect this project has
 *   already paid for twice (`mcgid`; mt_name[5]="Delete"). */
enum gpon_gem_dir {
	GPON_GEM_US	= 1,	/* upstream only   (UNI -> ANI) */
	GPON_GEM_DS	= 2,	/* downstream only (ANI -> UNI) */
	GPON_GEM_BIDIR	= 3,	/* bidirectional — THE data GEM */
};

/* Which upstream PLOAM queue a decision wants its message on: ...
 * dev/MEASURED-gpon_common.h.md sec 11. */
enum gpon_ploam_q {
	GPON_PLOAM_Q_SN,	/* Serial_Number_ONU: the auto-SN slot        */
	GPON_PLOAM_Q_URGENT,	/* Acknowledge, Password, Encryption_Key      */
	GPON_PLOAM_Q_NOMSG,	/* the idle No_message keepalive slot         */
};

/* Per-chip facts. A member belongs here only if BOTH hold: ...
 * dev/MEASURED-gpon_common.h.md sec 3. */
struct gpon_chip_cfg {
	/* F2 — one-past-the-end of the GET response value area.  36 is
	 * conformant (G.988 always reserves 36-37 and 38-39); realtek-luna
	 * writes to 40 (rtl9602c_eth.c:2265), overwriting both masks. */
	u16			get_val_end;
	/* F1 — the Get-Next message type.  0x1a is conformant; realtek-luna
	 * uses 0x10 (rtl9602c_eth.c:1698), the ALARM opcode, so its Get-Next
	 * falls through to NOT_SUPPORTED.  Unfalsifiable on the wire: only a
	 * build-time constant extractor catches it. */
	u8			mt_get_next;
};

/* ANYTHING THE CORE DID NOT UNDERSTAND — two classes, and ...
 * dev/MEASURED-gpon_common.h.md sec 4. */
enum gpon_unsup_class {
	GPON_UNSUP_UNKNOWN = 0,
	GPON_UNSUP_RANGE   = 1,
};

/* The op signature, named ONCE: `unsupported` is declared in BOTH op tables
 * (here and struct gpon_ploam_ops), and a member spelled twice drifts. */
typedef void (*gpon_unsup_fn)(void *sh, const char *kind, int cls, u32 val,
			      const char *want, const u8 *data,
			      unsigned int len);

/* The NULL-safe caller every core uses: the guard lives in ONE place so no call
 * site can forget it, and "the shell does not implement this" stays legal. */
static inline void gpon_unsup_call(gpon_unsup_fn fn, void *sh, const char *kind,
				   enum gpon_unsup_class cls, u32 val,
				   const char *want, const u8 *data,
				   unsigned int len)
{
	if (fn)
		fn(sh, kind, (int)cls, val, want, data, len);
}

/* CALL CONTEXT — mixing the two classes is the largest ...
 * dev/MEASURED-gpon_common.h.md sec 5. */
struct gpon_shell_ops {
	/* PLOAM activation. One consumer today (realtek-luna): elnath ...
	 * dev/MEASURED-gpon_common.h.md sec 6. */
	void (*ploam_tx)(void *sh, enum gpon_ploam_q q,
			 const u8 msg[GPON_PLOAM_US_LEN]);

	/* <- luna_gpon.c:6638 gpon_fsm_set_state() */
	void (*set_hw_state)(void *sh, enum gpon_ostate st);

	/* GPON_ONU_ID_BROADCAST on every teardown.  ONE op and not two register
	 * writes because luna must write BOTH the downstream and the upstream
	 * copy.  <- luna_gpon.c:6805-6236 and the four teardown paths */
	void (*set_hw_onu_id)(void *sh, u8 onu_id);

	/* Upstream equalization delay in bits, as Ranging_Time gave it
	 * dev/MEASURED-gpon_common.h.md sec 7. */
	void (*set_eqd)(void *sh, u32 eqd_bits);

	/* <- luna_gpon.c:6940 gpon_apply_boh() */
	void (*apply_boh)(void *sh, bool ranged);

	/* Re-lock the transmit analog at O3 entry: the cold-start ...
	 * dev/MEASURED-gpon_common.h.md sec 12. */
	void (*analog_relock)(void *sh);

	/* Load a 128-bit AES key into the STAGED bank; hardware promotes
	 * staged->active at the switch time below.
	 * <- luna_gpon.c:5843 gpon_aes_stage_key() */
	void (*aes_stage_key)(void *sh, const u8 key[16]);

	/* Arm the superframe at which the staged key is promoted, from
	 * Key_Switching_Time.  An un-armed switch corrupts AES.
	 * <- luna_gpon.c:7551 */
	void (*aes_set_switch_time)(void *sh, u32 superframe);

	/* Fill @out with @len random octets.  INJECTED rather than called so the
	 * core stays deterministic under replay and fuzzing. */
	void (*rng)(void *sh, u8 *out, unsigned int len);

	/* GEM / T-CONT. THE GRANULARITY IS "INSTALL", NEVER "WRITE ...
	 * dev/MEASURED-gpon_common.h.md sec 8. */
	int (*omcc_install)(void *sh, u16 alloc, u16 gem);

	/* The user-data path, as snooped from ME 262 / ME 268.
	 * <- luna_gpon.c:6602 gpon_install_data_gem()
	 *    cortina-gpon.c:2171 cg_data_try_install() */
	int (*data_install)(void *sh, u16 alloc, u16 gem);

	/* In whatever order that silicon requires.  NULL on realtek-luna.
	 * <- cortina-gpon.c:2113 cg_data_teardown() */
	void (*data_teardown)(void *sh);

	/* @len is carried even though baseline OMCI is always OMCI_LEN: an
	 * implicit length is what makes a buffer unfuzzable.
	 * <- cortina-gpon.c:2393 cg_omci_tx() */
	int (*omci_tx)(void *sh, const u8 *pdu, unsigned int len);

	/* diagnostics: NEVER load-bearing. A NULL ->trace must change ...
	 * dev/MEASURED-gpon_common.h.md sec 13. */
	void (*trace)(void *sh, unsigned int ev, u32 a, u32 b);

	/* ONE datum the core received and could not place. Same ...
	 * dev/MEASURED-gpon_common.h.md sec 9. */
	gpon_unsup_fn unsupported;
};

/* Each core owns a CONTEXT STRUCT carrying { ops, sh, chip } plus its own state,
 * and takes a pointer to it on every entry point.  NEVER file-scope globals:
 * the abandoned gpon_proto.c kept its FSM state in ~11 externs, which is what
 * made it impossible to instantiate twice and run against the host oracle. */
struct gpon_core_base {
	const struct gpon_shell_ops	*ops;
	void				*sh;	/* opaque shell handle       */
	const struct gpon_chip_cfg	*chip;
};

#endif /* GPON_COMMON_H */
