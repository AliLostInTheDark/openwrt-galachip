/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * TIER: CORE (prefix gpon_) — decides, never touches hardware, and compiles for
 * MIPS-BE, ARM64-LE and x86.  Canonical tier rule and guard: see "THE THREE
 * TIERS" in gpon_common.h (this directory).
 *
 * gpon_omci_core.h — the ITU-T G.988 OMCI baseline MESSAGE layer: the PDU facts
 * (message types, result codes, the 48-octet frame, the attribute-mask bit
 * rule), the parse of a downstream request, the dispatch by message type, and
 * the construction of the upstream response, trailer and MIC included.
 *
 * It holds NO managed-entity storage: the ME model lives in gpon_omci_me.h,
 * which this layer CALLS and never inlines.  G.988 message rules are identical
 * on every ONU ever built; which MEs a product serves is per board.
 *
 * ⚠ realtek-luna does NOT compile this yet.  It ships a third, independently
 *   written responder (rtl9602c_eth.c) that DIVERGES ON THE WIRE, so switching
 *   it over changes Luna's emitted bytes and needs its own board gate
 *   (follow-ups F1/F2/F3).  Each divergence is named at its own constant below.
 *
 * ENDIANNESS: all wire access is explicit byte math — ((u16)p[0] << 8) | p[1] —
 * never a cast and never htons/ntohs on a buffer, which is why ONE source emits
 * the same octets on big-endian MIPS and LE ARM64.
 */
#ifndef GPON_OMCI_CORE_H
#define GPON_OMCI_CORE_H

#include <linux/types.h>

#define OMCI_LEN		48	/* baseline message, incl. trailer+MIC */

/* Message types (G.988 Table 11.2.2-1). */
#define OMCI_MT_CREATE		0x04
#define OMCI_MT_DELETE		0x06
#define OMCI_MT_SET		0x08
#define OMCI_MT_GET		0x09
#define OMCI_MT_GET_ALL_ALARMS	0x0b
#define OMCI_MT_GET_ALL_ALRM_NX	0x0c
#define OMCI_MT_MIB_UPLOAD	0x0d
#define OMCI_MT_MIB_UPLOAD_NX	0x0e
#define OMCI_MT_MIB_RESET	0x0f
#define OMCI_MT_ALARM		0x10	/* 16 — ONU-autonomous alarm.  NOT Get
					 * Next: an OLT never sends 16, which
					 * is why mislabelling Get-Next as 0x10
					 * stayed invisible on this OLT. */
#define OMCI_MT_AVC		0x11	/* 17 — ONU-autonomous notification */
#define OMCI_MT_TEST		0x12
#define OMCI_MT_START_SW_DL	0x13
#define OMCI_MT_DOWNLOAD_SEC	0x14
#define OMCI_MT_END_SW_DL	0x15
#define OMCI_MT_ACTIVATE_SW	0x16
#define OMCI_MT_COMMIT_SW	0x17
#define OMCI_MT_SYNC_TIME	0x18
#define OMCI_MT_REBOOT		0x19
#define OMCI_MT_GET_NEXT	0x1a	/* 26.  DIVERGENCE, follow-up F1: Luna's
					 * rtl9602c_eth.c:1698 defines this as
					 * 0x10, the ALARM opcode above, so a
					 * real Get-Next falls through to its
					 * default arm.  Unfalsifiable on the
					 * wire; only a build-time constant
					 * extractor catches it. */
#define OMCI_MT_TEST_RESULT	0x1b	/* 27 -- ONU-initiated, after a Test */

/* Result codes (G.988 Table 11.2.2-2): the assigned set is the dense 0..7 plus
 * 9 — 8 and anything above 9 is unassigned and must never be sent. */
#define OMCI_RC_OK		0x00
#define OMCI_RC_NOT_SUPPORTED	0x02	/* command not supported by this ME */
#define OMCI_RC_PARAM_ERROR	0x03
#define OMCI_RC_UNKNOWN_ME	0x04
#define OMCI_RC_UNKNOWN_INST	0x05
#define OMCI_RC_INST_EXISTS	0x07
#define OMCI_RC_ATTR_FAILED	0x09

/* Attribute-mask bit: attr #n is bit (16-n), so bit15 = attr 1. */
#define OMCI_ATTR_BIT(n)	(1u << (16 - (n)))

/* The two ME class IDs the MESSAGE layer itself reasons ...
 * dev/MEASURED-gpon_omci_core.h.md sec 4. */
#define OMCI_ME_ONU_DATA	2
#define OMCI_ME_VEIP		329

/* Owned by the ME-model layer; the message layer only holds a pointer. */
struct omci_onu;
struct gpon_optic_reading;

enum omci_accept_kind {
	OMCI_ACCEPT_NONE,
	OMCI_ACCEPT_CONFIG,
	OMCI_ACCEPT_RESET,
};

/* One original request committed by the responder, not a hardware outcome.
 * applied_mask is the complete accepted Set mask; other actions leave it zero.
 * A fresh same-value or zero-mask Set is still an accepted transaction. */
struct omci_accepted {
	enum omci_accept_kind kind;
	u16 class_id;
	u16 inst;
	u16 applied_mask;
	u8 mt;
};

#define OMCI_MIC_SELFCHECK_N	16	/* DS frames judged for the MIC convention */

/* The responder's counters in the /proc grammar every family prints. */
int omci_resp_fmt(const struct omci_onu *o, bool armed, u32 tx, u32 tx_fail,
		  char *out, size_t sz);

/* Clear @accepted on entry, including discard/replay paths. AR-clear commits
 * produce an event with a zero return value. The caller serializes all ONU
 * state access; a caller that skips input must initialize its own NONE result. */
int omci_onu_input_ex(struct omci_onu *o, const u8 *req, unsigned int len,
		      u8 *resp, struct omci_accepted *accepted);

/* Process one DS baseline PDU -> fill @resp (48 bytes, ...
 * dev/MEASURED-gpon_omci_core.h.md sec 1. */
int omci_onu_input(struct omci_onu *o, const u8 *req, unsigned int len, u8 *resp);

/* The AAL5-BE MIC over bytes 0..43 as a VALUE, and the stamper into bytes
 * 44..47.  Exposed so a caller that must SHOW the computed MIC does not respell
 * the convention: there is exactly one spelling, here.  A host test modelling
 * an OLT must use it too, or the RX MIC gate will correctly discard the frame. */
u32 omci_mic_compute(const u8 *msg);
void omci_set_mic(u8 *msg);

/* Stamp the G.988 baseline trailer (bytes 40..43 = 00 00 00 28) AND the MIC.
 * Call it LAST.  PUBLIC so a shell building its own PDU stamps it with THE
 * shipped stamper: a shell that respells it is how the MIC convention came to
 * be implemented twice, under two different polynomials, on one board. */
void omci_finalize(u8 *msg);

/* Autonomous VEIP (ME 329) operational-state-up AVC: the OLT never polls the
 * data MEs it created — it gates DOWNSTREAM user-data forwarding on this
 * report.  Fills @out (48 bytes, trailer + MIC done); returns OMCI_LEN. */
int omci_onu_emit_veip_up_avc(struct omci_onu *o, u8 *out);

/* ★★★ THE ALARM SURFACE (G.988 clause 11.2.2), in the core, ...
 * dev/MEASURED-gpon_omci_core.h.md sec 2. */
void omci_onu_set_alarms(struct omci_onu *o, u16 class_id, u16 inst,
			 u16 bitmap);

/* Build the ONU-autonomous alarm (MT 0x10) for a CHANGE, if ...
 * dev/MEASURED-gpon_omci_core.h.md sec 3. */
int omci_onu_emit_alarm(struct omci_onu *o, u8 *out);

/* Serialized by the caller; commit only after successful TX submission. */
int omci_onu_alarm_prepare(const struct omci_onu *o, u8 *out);
void omci_onu_alarm_sent(struct omci_onu *o, const u8 *sent);

/* How many alarm-bearing ME instances are asserting right now — what
 * Get-all-alarms must report.  It answered a constant 0 before. */
u16 omci_alarm_count(const struct omci_onu *o);

/* ANI-G self test (Test code 7). The core ACKs the Test and records it; the
 * shell reads the optic in a context that may sleep and hands the reading to
 * omci_onu_selftest_result(), which builds the Test Result. */
bool omci_onu_selftest_pending(const struct omci_onu *o);
int omci_onu_selftest_result(struct omci_onu *o,
			     const struct gpon_optic_reading *v, u8 *out);

/* The general autonomous-notification emitter behind the VEIP one above.
 * ★ EXPORTED 2026-09-01 FOR A SECOND CALLER, so nobody deletes it as unused:
 *   dev/OMCI-ONU-simulate raises AVCs this driver does not (ME 256), and a
 *   second packer there would test the OLT against OUR idea of an AVC. */
void omci_emit_avc(struct omci_onu *o, u16 class_id, u16 inst, u16 mask,
		   const u8 *val, unsigned int vlen, u8 *out);

/* An OLT-side Set with NO acknowledgement asked (AR=0, AK=0), on the frame
 * layout the AVC above uses.  The one spelling for a shell injecting a Set at
 * its own responder (the uni_test / uni-test diagnostics) and for a host test
 * modelling an OLT; both families spelled these octets by hand before. */
void omci_set_build(u8 *out, u16 tci, u16 class_id, u16 inst, u16 mask,
		    const u8 *val, unsigned int vlen);

/* Store a 16-bit field big-endian, by explicit byte math.
 * ★ IN THE HEADER BECAUSE A SECOND COPY ALREADY EXISTED: the Luna ethernet
 *   driver carried a byte-identical one purely because this was static inline
 *   inside the .c. */
static inline void omci_put_be16(u8 *p, u16 v)
{
	p[0] = (u8)(v >> 8);
	p[1] = (u8)v;
}

#endif /* GPON_OMCI_CORE_H */
