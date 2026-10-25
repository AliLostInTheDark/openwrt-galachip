// SPDX-License-Identifier: GPL-2.0-or-later
/* TIER: CORE (prefix gpon_) — protocol only. NEVER touches ...
 * dev/MEASURED-gpon_omci_core.c.md sec 1. */
#include <linux/crc32.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "gpon_ddm.h"
#include "gpon_omci_core.h"
#include "gpon_omci_me.h"	/* struct omci_onu + the ME model / dynamic store */
#include "gpon_omci_mic.h"

/* MIC (bytes 44..47) = the I.363.5 / AAL5 CRC-32 over bytes ...
 * dev/MEASURED-gpon_omci_core.c.md sec 2. */
u32 omci_mic_compute(const u8 *msg)
{
	return ~crc32_be(~0u, msg, 44);
}

bool omci_mic_ok(const u8 *msg, unsigned int len)
{
	u32 c;

	/* A frame shorter than the baseline 48 cannot CARRY a MIC, so it cannot
	 * be verified -- and an unverifiable frame is exactly what must not be
	 * acted on.  This is the runt case that used to reach the MIB-Reset arm. */
	if (!msg || len < OMCI_LEN)
		return false;
	c = omci_mic_compute(msg);
	return msg[44] == (u8)(c >> 24) && msg[45] == (u8)(c >> 16) &&
	       msg[46] == (u8)(c >> 8)  && msg[47] == (u8)c;
}

/* ★ EXPORTED so a host test STAMPS WITH THE SHIPPED STAMPER ...
 * dev/MEASURED-gpon_omci_core.c.md sec 3. */
void omci_set_mic(u8 *msg)
{
	u32 c = omci_mic_compute(msg);

	msg[44] = (u8)(c >> 24);
	msg[45] = (u8)(c >> 16);
	msg[46] = (u8)(c >> 8);
	msg[47] = (u8)c;
}

/* Stamp the baseline trailer (40..43 = 00 00 00 28) + MIC. ...
 * dev/MEASURED-gpon_omci_core.c.md sec 4. */
void omci_finalize(u8 *msg)
{
	msg[40] = 0x00;
	msg[41] = 0x00;
	msg[42] = 0x00;
	msg[43] = 0x28;
	omci_set_mic(msg);
}

/* GET-response filler: result(8) + attr-mask(9,10) + ...
 * dev/MEASURED-gpon_omci_core.c.md sec 5. */
static u8 omci_get_fill(struct omci_onu *o, u16 class_id, u16 inst, u16 mask,
			u8 *resp)
{
	u16 rmask = 0, known = 0, unsup, failed;
	u8 rc;

	/* An absent instance of a class the MIB holds is 0x05, as stock (the X111W's
	 * own stock binary and a Huawei stock, 2026-10-03); ME 2 is never uploaded
	 * and is instance 0 only. */
	if ((omci_get_checks_inst(class_id) || omci_mib_has_class(o, class_id)) &&
	    !omci_inst_exists(o, class_id, inst))
		return OMCI_RC_UNKNOWN_INST;
	if (class_id == OMCI_ME_ONU_DATA && inst)
		return OMCI_RC_UNKNOWN_INST;
	if (omci_vendor_absent(o, class_id, inst)) {
		/* stock's answer to a vendor ME it does not expose: result 0,
		 * every mask zero (field X111W, production OLT, 2026-10-02) */
		memset(resp + 9, 0, 31);
		return OMCI_RC_OK;
	}
	rc = omci_me_fill(o, class_id, inst, mask, resp + 11, resp + 36,
			  &rmask, &known);

	if (rc == OMCI_RC_UNKNOWN_ME) {
		struct omci_me_inst *e = omci_store_find(o, class_id, inst);

		if (!e) {
			/* Nothing here at all.  If the OLT created OTHER
			 * instances of this class the class IS known and only
			 * the instance is not (0x05); otherwise the class
			 * itself is unknown (0x04). */
			omci_put_be16(resp + 9, 0);
			return omci_store_has_class(o, class_id) ?
					OMCI_RC_UNKNOWN_INST :
					OMCI_RC_UNKNOWN_ME;
		}
		/* Opaque set-by-create/set body: no descriptor table exists ...
		 * dev/MEASURED-gpon_omci_core.c.md sec 19. */
		memcpy(resp + 11, e->body, e->blen > 25 ? 25 : e->blen);
		rmask = mask;
		known = mask;
	}

	omci_put_be16(resp + 9, rmask);
	unsup = (u16)(mask & ~known);
	failed = (u16)(mask & known & ~rmask);
	omci_put_be16(resp + 36, unsup);
	omci_put_be16(resp + 38, failed);
	return (unsup | failed) ? OMCI_RC_ATTR_FAILED : OMCI_RC_OK;
}

/* Create / Set / Delete: APPLY or NAK, and move MIB-Data-Sync ...
 * dev/MEASURED-gpon_omci_core.c.md sec 6. */
static u8 omci_config_apply(struct omci_onu *o, u8 mt, u16 class_id, u16 inst,
			    const u8 *msg, unsigned int len, u8 *resp)
{
	struct omci_me_inst *e = omci_store_find(o, class_id, inst);
	u16 mask;

	/* T-CONTs, PPTP Ethernet UNIs and UNI-Gs are all ...
	 * dev/MEASURED-gpon_omci_core.c.md sec 7. */
	if ((class_id == OMCI_ME_TCONT || class_id == OMCI_ME_PPTP_ETH_UNI ||
	     class_id == OMCI_ME_UNI_G || class_id == OMCI_ME_ANI_G ||
	     class_id == OMCI_ME_MAC_BRIDGE_TABLE ||
	     class_id == OMCI_ME_MAC_BRIDGE_FILTER ||
	     class_id == OMCI_ME_PREASSIGN_FILTER) &&
	    (mt == OMCI_MT_CREATE || mt == OMCI_MT_DELETE))
		return OMCI_RC_NOT_SUPPORTED;

	switch (mt) {
	case OMCI_MT_CREATE:
		if (e)
			return OMCI_RC_INST_EXISTS;
		if (!omci_store_create(o, class_id, inst, msg + 8,
				    (len > 8) ? (int)(len - 8) : 0))
			return OMCI_RC_ATTR_FAILED;
		/* The bridge port's own table ME, ATOMICALLY: a store with no
		 * room for the companion undoes the port too, rather than
		 * leaving a bridge port whose ME 50 the OLT can never Get. */
		if (class_id == OMCI_ME_MAC_BRIDGE_PORT &&
		    !omci_store_put(o, OMCI_ME_MAC_BRIDGE_TABLE, inst,
				    NULL, 0)) {
			omci_store_del(o, class_id, inst);
			return OMCI_RC_ATTR_FAILED;
		}
		/* and its filter pre-assign table, the same way and for the
		 * dev/MEASURED-gpon_omci_core.c.md sec 20. */
		if (class_id == OMCI_ME_MAC_BRIDGE_PORT &&
		    !omci_store_create(o, OMCI_ME_PREASSIGN_FILTER, inst,
				       NULL, 0)) {
			omci_store_del(o, OMCI_ME_MAC_BRIDGE_TABLE, inst);
			omci_store_del(o, class_id, inst);
			return OMCI_RC_ATTR_FAILED;
		}
		/* and its MAC filter table, the third companion. Through ...
		 * dev/MEASURED-gpon_omci_core.c.md sec 8. */
		if (class_id == OMCI_ME_MAC_BRIDGE_PORT &&
		    !omci_store_put(o, OMCI_ME_MAC_BRIDGE_FILTER, inst,
				    NULL, 0)) {
			omci_store_del(o, OMCI_ME_PREASSIGN_FILTER, inst);
			omci_store_del(o, OMCI_ME_MAC_BRIDGE_TABLE, inst);
			omci_store_del(o, class_id, inst);
			return OMCI_RC_ATTR_FAILED;
		}
		break;
	case OMCI_MT_DELETE:
		if (!e)
			return OMCI_RC_UNKNOWN_INST;
		omci_store_del(o, class_id, inst);
		if (class_id == OMCI_ME_MAC_BRIDGE_PORT) {
			omci_store_del(o, OMCI_ME_MAC_BRIDGE_TABLE, inst);
			omci_store_del(o, OMCI_ME_PREASSIGN_FILTER, inst);
			omci_store_del(o, OMCI_ME_MAC_BRIDGE_FILTER, inst);
			/* the filter rows live outside the store, so the
			 * instance going away must take them with it --
			 * otherwise the next bridge port with this id inherits
			 * a stranger's MAC filter */
			gpon_mac_filter_del(&o->vlan, inst);
		}
		/* An ME 171 instance owns rows the dense store never held, so
		 * deleting the instance must drop them too -- otherwise the
		 * next instance with the same id inherits a stranger's VLAN. */
		if (class_id == OMCI_ME_EXT_VLAN)
			gpon_ext_vlan_del(&o->vlan, inst);
		break;
	default:					/* OMCI_MT_SET */
		if (len < 10)		/* no attribute mask on the wire */
			return OMCI_RC_PARAM_ERROR;
		if (!omci_inst_exists(o, class_id, inst))
			return (omci_class_modelled(class_id) ||
				omci_store_has_class(o, class_id)) ?
					OMCI_RC_UNKNOWN_INST :
					OMCI_RC_UNKNOWN_ME;
		mask = ((u16)msg[8] << 8) | msg[9];
		if (omci_me_mutable(class_id)) {
			u16 unsupported, failed;
			u8 rc = omci_me_set(o, class_id, inst, mask, msg + 10,
					   30, &unsupported, &failed);

			/* Set response masks immediately follow the result byte;
			 * Get reserves its masks at 36/38 instead. */
			omci_put_be16(resp + 9, unsupported);
			omci_put_be16(resp + 11, failed);
			if (rc != OMCI_RC_OK)
				return rc;
		} else if (e && mask) {
			omci_store_merge(e, msg + 10, (int)(len - 10));
		}
		/* An OLT Set of ME2 attr-1 is an explicit resync write: take
		 * its byte first, then this Set's own +1 still applies. */
		if (class_id == OMCI_ME_ONU_DATA && len >= 11 &&
		    (mask & 0x8000))
			o->mds = msg[10];
		break;
	}

	/* MIB-Data-Sync: +1 per applied config message (not per attribute),
	 * wrap 255 -> 1 (0 = just-reset). */
	if (++o->mds == 0)
		o->mds = 1;
	return OMCI_RC_OK;
}

/*
 * The adaptive MIB-Data-Sync walk.  See OMCI_MDS_WALK_STEP in gpon_omci_me.h for
 * why 0 is outside the search space rather than folded onto 1.
 */
void omci_mds_walk(struct omci_onu *o)
{
	if (!o->mds_adapt || !o->mds_adapt_reads)
		return;
	if (++o->audit_reads < o->mds_adapt_reads)
		return;
	o->audit_reads = 0;
	/* 1..255: an mds of 0 on entry (only reachable straight after an OLT
	 * MIB-Reset) steps to OMCI_MDS_WALK_STEP, staying inside the range. */
	o->mds = (u8)(1 + ((unsigned int)o->mds + OMCI_MDS_WALK_STEP - 1) % 255);
	o->mds_tries++;
}

int omci_resp_fmt(const struct omci_onu *o, bool armed, u32 tx, u32 tx_fail,
		  char *out, size_t sz)
{
	return scnprintf(out, sz,
			 "ds_omci_rx     = %u (short=%u)\n"
			 "omci_resp      = %s tx=%u fail=%u ds_crc ok=%u bad=%u"
			 "  mds=%u store=%u avc=%u unhandled=%u dup_replay=%u ext=%u no_ack=%u selftest=%u\n"
			 "omci_rx_bad_mic: %u (DS frames discarded on an invalid MIC)\n",
			 o->rx_total, o->rx_runt, armed ? "armed" : "off", tx, tx_fail,
			 o->mic_conv_ok, o->mic_conv_bad, o->mds, o->store_n,
			 o->avc_count, o->unhandled, o->dup_replay, o->rx_extended,
			 o->no_ack, o->selftest_sent, o->rx_bad_mic);
}

int omci_onu_input_ex(struct omci_onu *o, const u8 *msg, unsigned int len,
		      u8 *resp, struct omci_accepted *accepted)
{
	u16 class_id, inst;
	u8 mt, devid, prio;

	if (accepted)
		memset(accepted, 0, sizeof(*accepted));

	/* ★ A BASELINE OMCI PDU IS 48 BYTES, FULL STOP (G.988 A.3). ...
	 * dev/MEASURED-gpon_omci_core.c.md sec 9. */
	o->rx_total++;
	if (len < OMCI_LEN) {
		o->rx_runt++;
		return 0;
	}
	if (o->mic_conv_ok + o->mic_conv_bad < OMCI_MIC_SELFCHECK_N) {
		if (gpon_omci_mic_conv(msg, len) == GPON_MIC_CONV_AAL5_BE)
			o->mic_conv_ok++;
		else
			o->mic_conv_bad++;
	}
	devid = msg[3];
	prio = msg[0] >> 7;		/* G.988 11.2.1: TCI bit 15 */
	mt = msg[2] & 0x1f;
	class_id = ((u16)msg[4] << 8) | msg[5];
	inst = ((u16)msg[6] << 8) | msg[7];

	if (devid != 0x0a) {
		/* Only the BASELINE message set is modelled. An ...
		 * dev/MEASURED-gpon_omci_core.c.md sec 10. */
		if (devid == 0x0b)
			o->rx_extended++;
		return 0;
	}

	/* ★ THE MIC GATE, BEFORE THE REPLAY CACHE. A corrupted frame ...
	 * dev/MEASURED-gpon_omci_core.c.md sec 21. */
	if (!omci_mic_ok(msg, len)) {
		o->rx_bad_mic++;
		return 0;
	}

	/* G.988 11.2.2.1 retained last response: the OMCC is ...
	 * dev/MEASURED-gpon_omci_core.c.md sec 11. */
	if (o->have_last[prio] && !memcmp(msg, o->last_req[prio], 40)) {
		memcpy(resp, o->last_resp[prio], OMCI_LEN);
		o->dup_replay++;
		return OMCI_LEN;
	}

	memset(resp, 0, OMCI_LEN);
	resp[0] = msg[0];			/* TID echo */
	resp[1] = msg[1];
	resp[2] = (msg[2] & 0x1f) | 0x20;	/* clear AR, set AK */
	resp[3] = 0x0a;
	resp[4] = msg[4];			/* class echo */
	resp[5] = msg[5];
	resp[6] = msg[6];			/* instance echo */
	resp[7] = msg[7];

	switch (mt) {
	case OMCI_MT_MIB_RESET:
		/* On-wire MIB-Reset: zero MIB-Data-Sync (the OLT recounts its
		 * lsync from 0; keeping a seed here = permanent mismatch ->
		 * Deactivate loop, proven) + drop the provisioned store. */
		o->mds = 0;
		memset(o->store, 0, sizeof(o->store));
		o->store_n = 0;
		omci_me_reset_values(o);
		/* a provisioning event is the walk's GOAL, reached: rearm it */
		o->audit_reads = 0;
		o->mds_tries = 0;
		/* ★★★ AND THE VEIP OPER-UP AVC MUST BE RE-EMITTED. A MIB-Reset
		 * dev/MEASURED-gpon_omci_core.c.md sec 12. */
		o->avc_veip_up_sent = false;
		resp[8] = OMCI_RC_OK;
		if (accepted)
			accepted->kind = OMCI_ACCEPT_RESET;
		break;
	case OMCI_MT_MIB_UPLOAD:
		/* Row count at contents[8..9], NO result byte (a result byte
		 * here made the OLT read count=0 and never walk, proven). */
		omci_put_be16(resp + 8, o->nrows + o->store_n);
		break;
	case OMCI_MT_GET:
		if (len < 10)	/* mask missing: a shorter GET would read
				 * stale bytes into the reply (info leak) */
			return 0;
		resp[8] = omci_get_fill(o, class_id, inst,
					((u16)msg[8] << 8) | msg[9], resp);
		/* ★ THE WALK IS CALLED FROM HERE AND NOWHERE ELSE: a GET is the
		 * unit the OLT's audit is made of, so it is the only event that
		 * means "read us again without provisioning". */
		omci_mds_walk(o);
		break;
	case OMCI_MT_SET:
	case OMCI_MT_CREATE:
	case OMCI_MT_DELETE:
		resp[8] = omci_config_apply(o, mt, class_id, inst, msg, len, resp);
		if (resp[8] == OMCI_RC_OK) {
			if (accepted)
				accepted->kind = OMCI_ACCEPT_CONFIG;
			/* the OLT provisioned: the walk reached its goal */
			o->audit_reads = 0;
			o->mds_tries = 0;
		}
		break;
	case OMCI_MT_GET_ALL_ALARMS:
		/* Alarm-entry count at contents[8..9], NO result byte — same ...
		 * dev/MEASURED-gpon_omci_core.c.md sec 13. */
		omci_put_be16(resp + 8, omci_alarm_count(o));
		o->alarm_snapshot_class = omci_alarm_count(o) ? o->alarm_class : 0;
		o->alarm_snapshot_inst = o->alarm_inst;
		o->alarm_snapshot_bits = o->alarm_active;
		o->alarm_seq = 0;
		break;
	case OMCI_MT_GET_ALL_ALRM_NX:
		if (!msg[8] && !msg[9] && o->alarm_snapshot_class) {
			omci_put_be16(resp + 8, o->alarm_snapshot_class);
			omci_put_be16(resp + 10, o->alarm_snapshot_inst);
			omci_put_be16(resp + 12, o->alarm_snapshot_bits);
		}
		break;
	case OMCI_MT_MIB_UPLOAD_NX: {
		/* Request seq at msg[8..9]; reply = class[8..9] + inst[10..11]
		 * + attr-mask[12..13] + values[14..39], NO result byte. */
		u16 seq;
		u16 wmask = 0, wknown = 0;

		if (len < 10)
			return 0;
		seq = ((u16)msg[8] << 8) | msg[9];
		if (seq < o->nrows) {
			const struct omci_mib_row *r = &o->rows[seq];

			omci_put_be16(resp + 8, r->class_id);
			omci_put_be16(resp + 10, r->inst);
			omci_me_fill(o, r->class_id, r->inst, r->mask,
				     resp + 14, resp + 40, &wmask, &wknown);
			omci_put_be16(resp + 12, wmask);
		} else if (seq < o->nrows + o->store_n) {
			/* Mapped dynamic values use the same encoder as Get.
			 * Other classes retain their present-only rows. */
			const struct omci_me_inst *e =
				omci_store_nth(o, seq - o->nrows);

			if (e) {
				omci_put_be16(resp + 8, e->class_id);
				omci_put_be16(resp + 10, e->inst);
				/* ★ EVERY DENSE CLASS, not just ME 268. A row that announces ...
				 * dev/MEASURED-gpon_omci_core.c.md sec 14. */
				if (omci_me_dense_len(e->class_id)) {
					omci_me_fill(o, e->class_id, e->inst, 0xffff,
						     resp + 14, resp + 40, &wmask, &wknown);
					omci_put_be16(resp + 12, wmask);
				}
			}
		}
		/* out-of-range seq -> all-zero row, still well-formed */
		break;
	}
	case OMCI_MT_TEST:
		/* An OLT reads the ONU's optical levels from the self test's
		 * RESULT; an ACK alone shows as no signal (-30 dBm on the G24W,
		 * 2026-09-30). The shell owes the result. */
		if (class_id == OMCI_ME_ANI_G && msg[8] == 7) {
			o->selftest_pending = true;
			o->selftest_tci = ((u16)msg[0] << 8) | msg[1];
			o->selftest_inst = inst;
		}
		resp[8] = OMCI_RC_OK;
		break;
	case OMCI_MT_SYNC_TIME:
	case OMCI_MT_REBOOT:		/* ACK, do NOT actually reboot */
	case OMCI_MT_START_SW_DL:
	case OMCI_MT_DOWNLOAD_SEC:
	case OMCI_MT_END_SW_DL:
	case OMCI_MT_ACTIVATE_SW:
	case OMCI_MT_COMMIT_SW:
		/* not performed (no SW image to flash), but must ACK OK so
		 * the OLT's provisioning FSM completes */
		resp[8] = OMCI_RC_OK;
		break;
	case OMCI_MT_GET_NEXT:
		/* Get Next walks a TABLE attribute. ⚠ THE MODEL NOW DEFINES ...
		 * dev/MEASURED-gpon_omci_core.c.md sec 15. */
		break;
	default:
		/* A message type with no ONU-side action. Answer result 0x00 ...
		 * dev/MEASURED-gpon_omci_core.c.md sec 16. */
		o->unhandled++;
		break;
	}

	/* Acceptance describes a committed request, independently of AR and
	 * response delivery. The replay path above never reaches this point. */
	if (accepted && accepted->kind != OMCI_ACCEPT_NONE) {
		accepted->mt = mt;
		accepted->class_id = class_id;
		accepted->inst = inst;
		if (mt == OMCI_MT_SET)
			accepted->applied_mask = ((u16)msg[8] << 8) | msg[9];
	}
	omci_finalize(resp);

	/* Refresh the retransmission cache. It may only ever hold the ...
	 * dev/MEASURED-gpon_omci_core.c.md sec 17. */
	if (msg[2] & 0x40) {
		memcpy(o->last_req[prio], msg, 40);
		memcpy(o->last_resp[prio], resp, OMCI_LEN);
		o->have_last[prio] = true;
	} else {
		o->have_last[0] = false;
		o->have_last[1] = false;
	}

	/* AR clear = the OLT asked for no acknowledgement (G.988): the message
	 * is APPLIED above, but nothing goes upstream.  Counted, because a
	 * silent path still has to be observable — /proc says whether this OLT
	 * ever uses it (spy-capability rule). */
	if (!(msg[2] & 0x40)) {
		o->no_ack++;
		return 0;
	}
	return OMCI_LEN;
}

int omci_onu_input(struct omci_onu *o, const u8 *msg, unsigned int len, u8 *resp)
{
	return omci_onu_input_ex(o, msg, len, resp, NULL);
}

/* Autonomous AVC (MT 0x11, TID 0): report that (class, ...
 * dev/MEASURED-gpon_omci_core.c.md sec 22. */
void omci_onu_set_alarms(struct omci_onu *o, u16 class_id, u16 inst,
			 u16 bitmap)
{
	if (!o)
		return;
	/* ★ ONE ME'S WORTH TODAY, and the shape says so rather than ...
	 * dev/MEASURED-gpon_omci_core.c.md sec 23. */
	o->alarm_class = class_id;
	o->alarm_inst = inst;
	o->alarm_active = bitmap;
}

u16 omci_alarm_count(const struct omci_onu *o)
{
	/* G.988: the number of ME INSTANCES currently reporting an alarm, not
	 * the number of alarm BITS. One instance with three conditions is one
	 * entry the OLT would walk. */
	return (o && o->alarm_class && o->alarm_active) ? 1 : 0;
}

int omci_onu_alarm_prepare(const struct omci_onu *o, u8 *out)
{
	if (!o || !out)
		return 0;
	if (!o->alarm_class)
		return 0;			/* nobody has reported anything */
	if (o->alarm_active == o->alarm_told)
		return 0;			/* ★ no EDGE -> no message */

	memset(out, 0, OMCI_LEN);
	/* TCI stays 0: G.988 marks an ONU-autonomous notification with a zero
	 * transaction id, the same convention omci_emit_avc uses one function
	 * below. AR/AK stay clear -- the OLT does not acknowledge an alarm. */
	out[2] = OMCI_MT_ALARM;
	out[3] = 0x0a;				/* DevID: baseline */
	omci_put_be16(out + 4, o->alarm_class);
	omci_put_be16(out + 6, o->alarm_inst);

	/* Contents (octets 8..39). G.988 clause 11.2.2: the alarm ...
	 * dev/MEASURED-gpon_omci_core.c.md sec 18. */
	out[8] = (u8)(o->alarm_active >> 8);
	out[9] = (u8)(o->alarm_active & 0xff);

	out[39] = o->alarm_seq == 255 ? 1 : o->alarm_seq + 1;

	omci_finalize(out);
	return OMCI_LEN;
}

void omci_onu_alarm_sent(struct omci_onu *o, const u8 *sent)
{
	if (!o || !sent || sent[2] != OMCI_MT_ALARM || sent[3] != 0x0a ||
	    (((u16)sent[4] << 8) | sent[5]) != o->alarm_class ||
	    (((u16)sent[6] << 8) | sent[7]) != o->alarm_inst)
		return;
	o->alarm_told = ((u16)sent[8] << 8) | sent[9];
	o->alarm_seq = sent[39];
	o->alarm_emitted++;
}

bool omci_onu_selftest_pending(const struct omci_onu *o)
{
	return o && o->selftest_pending;
}

/* G.988 0.002 dB in dBuW: (dBm + 30) * 500, saturated to the 16-bit field. */
static u16 omci_cdbm_to_dbuw_field(s32 cdbm)
{
	s32 v = (cdbm + 3000) * 5;

	if (cdbm > 3553)
		v = 32767;
	else if (cdbm < -9553)
		v = -32768;
	return (u16)v;
}

static void omci_tlv(u8 *p, u8 type, u16 value)
{
	p[0] = type;
	omci_put_be16(p + 1, value);
}

int omci_onu_selftest_result(struct omci_onu *o,
			     const struct gpon_optic_reading *v, u8 *out)
{
	u8 *c = out + 8;

	if (!o || !v || !out || !o->selftest_pending)
		return 0;
	memset(out, 0, OMCI_LEN);
	omci_put_be16(out, o->selftest_tci);
	out[2] = OMCI_MT_TEST_RESULT;
	out[3] = 0x0a;
	omci_put_be16(out + 4, OMCI_ME_ANI_G);
	omci_put_be16(out + 6, o->selftest_inst);
	/* stock's layout: each TLV at a fixed offset, an unread one left zero */
	if (v->have & GPON_OPTIC_VCC)
		omci_tlv(c, 1, v->vcc_100uv / 200);
	if (v->have & GPON_OPTIC_RX)
		omci_tlv(c + 3, 3, omci_cdbm_to_dbuw_field(v->rx_cdbm));
	if (v->have & GPON_OPTIC_TX)
		omci_tlv(c + 6, 5, omci_cdbm_to_dbuw_field(v->tx_cdbm));
	if (v->have & GPON_OPTIC_BIAS)
		omci_tlv(c + 9, 9, v->bias_2ua);
	if (v->have & GPON_OPTIC_TEMP)
		omci_tlv(c + 12, 12, (u16)v->temp_256);
	omci_finalize(out);
	o->selftest_pending = false;
	o->selftest_sent++;
	return OMCI_LEN;
}

/* Synchronous peers without a fallible transport. Drivers use prepare/sent. */
int omci_onu_emit_alarm(struct omci_onu *o, u8 *out)
{
	int n = omci_onu_alarm_prepare(o, out);

	if (n)
		omci_onu_alarm_sent(o, out);
	return n;
}

/* ONE baseline frame layout for everything this core BUILDS, notification and
 * request alike: TCI, type, DevID 0x0a, class, instance, mask, <= 30 value
 * octets, then the shipped trailer and MIC. */
static void omci_frame_build(u8 *out, u16 tci, u8 mt, u16 class_id, u16 inst,
			     u16 mask, const u8 *val, unsigned int vlen)
{
	memset(out, 0, OMCI_LEN);
	omci_put_be16(out, tci);
	out[2] = mt;
	out[3] = 0x0a;
	omci_put_be16(out + 4, class_id);
	omci_put_be16(out + 6, inst);
	omci_put_be16(out + 8, mask);
	if (val && vlen) {
		if (vlen > 30)
			vlen = 30;
		memcpy(out + 10, val, vlen);
	}
	omci_finalize(out);
}

void omci_emit_avc(struct omci_onu *o, u16 class_id, u16 inst, u16 mask,
		   const u8 *val, unsigned int vlen, u8 *out)
{
	/* TCI 0: G.988 marks an ONU-autonomous notification with a zero TCI. */
	omci_frame_build(out, 0, OMCI_MT_AVC, class_id, inst, mask, val, vlen);
	o->avc_count++;
}

void omci_set_build(u8 *out, u16 tci, u16 class_id, u16 inst, u16 mask,
		    const u8 *val, unsigned int vlen)
{
	omci_frame_build(out, tci, OMCI_MT_SET, class_id, inst, mask, val, vlen);
}

int omci_onu_emit_veip_up_avc(struct omci_onu *o, u8 *out)
{
	/* VEIP inst 0x0601 attr #2 (operational state) mask 0x4000,
	 * value 0 = enabled (G.988). */
	static const u8 up = 0x00;

	omci_emit_avc(o, OMCI_ME_VEIP, 0x0601, OMCI_ATTR_BIT(2), &up, 1, out);
	o->avc_veip_up_sent = true;
	return OMCI_LEN;
}
