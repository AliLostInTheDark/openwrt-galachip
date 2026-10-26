// SPDX-License-Identifier: GPL-2.0-or-later


#include "gpon_sn.h"	/* the one ONU-SN codec */
#include "gpon_ploam.h"
#include "gpon_gem_us.h"	/* gpon_omcc_decide: the Configure_Port-ID rule,
				 * shared with both family shells so the three
				 * copies of it cannot drift apart again */

/* Cadences and thresholds that were bare literals in the driver. Values
 * unchanged; named so a reader can see what they gate. None is a module_param
 * upstream, so they stay compile-time here. */
#define GPON_PLOAM_AVC_DELAY_TICKS	2500	/* :6609 hold O5 this long first  */
#define GPON_PLOAM_AVC_PERIOD_TICKS	150	/* :6610 then re-report this often */
#define GPON_PLOAM_AVC_MAX		3	/* :6608 reports per O5 entry     */
#define GPON_PLOAM_SN_REOFFER_TICKS	50	/* :6835 ~twice a second at O3    */
#define GPON_PLOAM_HEALTHY_O5_TICKS	500	/* :6355 ~5 s = a healthy provision */
#define GPON_PLOAM_KEEP_LOCK_GIVEUP_TICKS 3000	/* ~30 s below O4 after a keep-lock DEACT */
#define GPON_PLOAM_RERANGE_LOG_MS	2000	/* :6096 flap damping on the summary */

/* Guard/preamble defaults before the OLT has dictated any (:958-:959). */
#define GPON_PLOAM_BOH_PTN_DEFAULT	0xaa
#define GPON_PLOAM_BOH_DELIM_DEFAULT	{ 0xab, 0x59, 0x83 }

/* Small internal helpers. No file-scope state */
static bool ms_after(u32 a, u32 b)
{
	u32 diff = a - b;

	return diff != 0u && diff < 0x80000000u;
}

static void ev(const struct gpon_ploam *o, enum gpon_ploam_ev e, u32 a, u32 b)
{
	if (o->ops->trace)
		o->ops->trace(o->sh, e, a, b);
}

/* The core's outlet for */
static void unsup(const struct gpon_ploam *o, const char *kind,
		  enum gpon_unsup_class cls, u32 val, const char *want,
		  const u8 *d, unsigned int len)
{
	gpon_unsup_call(o->ops->unsupported, o->sh, kind, cls, val, want,
			d, len);
}

/* The per-type counter slot of a PLOAM type: the last slot holds every type >= it. */
static inline unsigned int ploam_slot(u8 type)
{
	return type < GPON_PLOAM_TYPE_SLOTS ? type : GPON_PLOAM_TYPE_SLOTS - 1;
}

/* The single point every upstream PLOAM leaves through. `queue` selects the
 * US_PLOAM_IND queue and is load-bearing: urgent (0x1) pre-empts the auto-SN
 * burst (0x6), which is what gets an Acknowledge out before the OLT raises
 * LOAi; No_message rides the hardware auto slot (0x7). */
static void ploam_tx(struct gpon_ploam *o, u8 queue,
		     const u8 m[GPON_PLOAM_US_LEN])
{
	o->ops->ploam_tx(o->sh, queue, m);
	o->tx_total++;
	o->us_queued[ploam_slot(m[1])]++;
}

/* Upstream message builders. Pure byte assembly */
static void send_sn(struct gpon_ploam *o)
{
	u8 m[GPON_PLOAM_US_LEN];

	m[0] = 0xff;			/* ONU-ID (unassigned)            */
	m[1] = PLM_US_SERIAL_NUMBER;	/* 0x01                           */
	memcpy(&m[2], o->sn, 8);	/* ONU-SN: ID(4) + serial(4)      */
	m[10] = 0x00;			/* random delay (HW may fill)     */
	m[11] = 0x04;			/* G-bit set, power level 0       */

	ploam_tx(o, PLM_US_QUEUE_SN, m);
	o->sn_tx++;
}

/* Password (US type 0x02). */
static int send_password(struct gpon_ploam *o)
{
	u8 p[GPON_PLOAM_US_LEN] = { 0 };
	int i;

	p[0] = o->onu_id;		/* our assigned ONU-ID — see FOLLOW-UP P6 */
	p[1] = PLM_US_PASSWORD;		/* 0x02 */
	memcpy(&p[2], o->password, sizeof(o->password));
	for (i = 0; i < 3; i++)
		ploam_tx(o, PLM_US_QUEUE_URG, p);
	return 3;
}

/* Acknowledge (US type 0x09). */
static int send_ack(struct gpon_ploam *o, const u8 *ds)
{
	u8 a[GPON_PLOAM_US_LEN] = { 0 };

	a[0] = o->onu_id;		/* our assigned ONU-ID (HW may override) */
	a[1] = PLM_US_ACKNOWLEDGE;	/* 0x09 */
	a[2] = ds[1];			/* acknowledged message type */
	a[3] = ds[0];			/* acknowledged message ONU-ID */
	a[4] = ds[1];			/* acknowledged message type (echo) */
	memcpy(&a[5], &ds[2], 7);	/* first 7 payload octets */
	ploam_tx(o, PLM_US_QUEUE_URG, a);
	return 1;
}

/* Encryption_Key (US type 0x05). */
static int send_key(struct gpon_ploam *o)
{
	u8 m[GPON_PLOAM_US_LEN];
	int row, rep;

	o->ops->rng(o->sh, o->aes_key, sizeof(o->aes_key));
	o->key_index++;
	for (rep = 0; rep < 3; rep++) {
		for (row = 0; row < 2; row++) {
			memset(m, 0, sizeof(m));
			m[0] = o->onu_id;	/* ONU-ID — see FOLLOW-UP P6   */
			m[1] = PLM_US_ENCRYPT_KEY;
			m[2] = o->key_index;
			m[3] = (u8)row;		/* fragment row 0/1            */
			memcpy(&m[4], o->aes_key + row * 8, 8);
			ploam_tx(o, PLM_US_QUEUE_URG, m);
		}
	}
	o->ops->aes_stage_key(o->sh, o->aes_key);
	o->key_staged = true;
	ev(o, GPON_PLOAM_EV_KEY_SENT, o->key_index, 0);
	return 6;
}

/* No_message (US type 0x04) on the hardware auto slot. The 0xaa fill and the
 * broadcast ONU-ID are what the driver built at :6117-6119 and :6850-6852. */
static void build_template(u8 m[GPON_PLOAM_US_LEN], u8 type)
{
	memset(m, 0xaa, GPON_PLOAM_US_LEN);
	m[0] = 0xff;			/* ONU-ID (HW overrides via ONUID_OVRD) */
	m[1] = type;
}

/* Burst overhead and equalization delay */
static u32 apply_boh(struct gpon_ploam *o, bool ranged)
{
	u8 oh[GPON_PLOAM_BOH_LEN];
	u8 guard = o->boh_guard;
	u8 t3 = ranged ? o->boh_t3ranged : o->boh_t3pre;
	u8 rep, i, boh_len, size;
	unsigned int want;
	u32 cfg_word;

	if (guard > 32)
		guard = 32;
	rep = guard / 8;			/* boh_repeat = whole guard bytes */

	/* FOLLOW-UP P4, DISCHARGED 2026-09-02. */
	/* stock's length (gpon_res.c): the Type-1+2 preamble rides in front of
	 * the Type-3 bytes. Without it an OLT that sends Extended_Burst_Length
	 * gets a short preamble at O5, reads no burst and deactivates (LOAi). */
	want = t3 ? (unsigned int)rep + t3 + 3 + o->boh_t12 : GPON_PLOAM_BOH_LEN;
	if (want > GPON_PLOAM_BOH_MAX_LEN) {
		u8 dmp[2] = { guard, t3 };

		unsup(o, "boh_len", GPON_UNSUP_RANGE, want,
		      "at-most-252", dmp, sizeof(dmp));
		want = GPON_PLOAM_BOH_MAX_LEN;
	}
	boh_len = (u8)want;
	/* stored bytes (<= 12); the hardware synthesizes the rest */
	size = (boh_len > GPON_PLOAM_BOH_LEN) ? GPON_PLOAM_BOH_LEN : boh_len;
	if (size < 4)				/* need >= 1 fill + 3 delimiter */
		size = 4;

	memset(oh, 0xaa, sizeof(oh));
	for (i = 0; i < rep && i < (u8)(size - 3); i++)
		oh[i] = 0xaa;			/* guard bytes */
	for (; i < (u8)(size - 3); i++)
		oh[i] = o->boh_ptn;		/* Type-3 preamble fill */
	oh[size - 3] = o->boh_delim[0];		/* delimiter at the TRUE end */
	oh[size - 2] = o->boh_delim[1];
	oh[size - 1] = o->boh_delim[2];

	cfg_word = (u32)(((size - 4) & 0xf) << 8) | (boh_len & 0xffu);
	o->ops->boh_write(o->sh, cfg_word, oh, size);
	return cfg_word;	/* the caller emits GPON_PLOAM_EV_BOH, outside any HW window */
}

/* Upstream equalization delay. */
static void set_eqd(struct gpon_ploam *o, u32 value)
{
	u32 min_delay1 = o->ops->get_min_delay(o->sh);
	u32 eqd1  = value + min_delay1 * 128;
	u32 multi = eqd1 / GPON_PLOAM_EQD_FRAME_LEN;
	u32 intra = eqd1 - multi * GPON_PLOAM_EQD_FRAME_LEN;

	o->eqd = value;
	o->ops->set_eqd(o->sh, multi, intra);
}

/* Assigned, and addressed: an ONU without an ONU-ID answers nothing but SN
 * requests (G.984.3 10.2.5.1), even to a broadcast. */
static bool addressed(const struct gpon_ploam *o, u8 onu_id, bool bcast_ok)
{
	return o->onu_id != GPON_PLOAM_ONU_ID_BROADCAST &&
	       (onu_id == o->onu_id ||
		(bcast_ok && onu_id == GPON_PLOAM_ONU_ID_BROADCAST));
}

/* Disable_Serial_Number and the chip SDK's 0x81 variant. Enable-all (0x0f)
 * names no serial number. */
static bool sn_disables(const struct gpon_ploam *o, u8 type, const u8 *d)
{
	return !memcmp(&d[1], o->sn, 8) &&
	       d[0] == (type == PLM_DS_DISABLE_SN ? 0xff : 0x11);
}

static bool sn_enables(const struct gpon_ploam *o, u8 type, const u8 *d)
{
	if (type == PLM_DS_DISABLE_SN)
		return d[0] == 0x0f || (d[0] == 0x00 && !memcmp(&d[1], o->sn, 8));
	return (d[0] == 0x0f || d[0] == 0x10) && !memcmp(&d[1], o->sn, 8);
}

/* G.984.3 O6 without leaving O5: a LOS shorter than TO2 is ridden through,
 * and for TO2 after it a POPUP is answered as from O6. */
static bool popup_pending(const struct gpon_ploam *o)
{
	return o->state == GPON_O5_OPERATION && o->popup_until_tick &&
	       o->popup_until_tick - o->ticks <= o->cfg->los_rerange_ticks;
}



/* ---------------------------------------------------------------------------
 * State transitions, from luna_gpon.c:6638-6140.
 * ------------------------------------------------------------------------- */
static void set_state(struct gpon_ploam *o, enum gpon_ostate st, u32 now_ms)
{
	/* ★ THE O4 CLOCK, set here for the same reason o5_entry_tick is: the
	 * ONLY place every transition passes through. Kept beside it so a
	 * future state cannot acquire a timer that some paths never start. */
	if (st == GPON_O4_RANGING)
		o->o4_entry_tick = o->ticks ? o->ticks : 1;
	else
		o->o4_entry_tick = 0;
	if (st >= GPON_O4_RANGING)
		o->keep_lock_tick = 0;

	enum gpon_ostate prev = o->state;

	/* hold: keep the FSM parked at O1 — refuse every advance past O1 so the
	 * GPON never ranges or deactivates and the shared switch datapath stops
	 * churning, leaving the LAN bridge and the WiFi AP stable. */
	if (o->cfg->hold && st > GPON_O1_INITIAL)
		return;
	if (o->state != st)
		ev(o, GPON_PLOAM_EV_STATE, o->state, st);
	if (prev != st)
		o->los_run = 0;		/* fresh LOS debounce on every transition */
	if (prev != st && st != GPON_O5_OPERATION)
		o->rei_interval_ms = 0;	/* the next activation sets its own */
	if (prev != st)
		o->popup_until_tick = 0;
	o->state = st;

	/* The EARLY clock, armed AFTER the state is really set */
	if (st <= GPON_O3_SERIAL) {
		if (prev != st || !o->early_entry_tick)
			o->early_entry_tick = o->ticks ? o->ticks : 1;
	} else {
		o->early_entry_tick = 0;
	}
	if (prev != st)
		o->early_report_tick = 0;

	if (st == GPON_O5_OPERATION && prev != GPON_O5_OPERATION) {
		/* Re-range completion */
		if (o->rerange_start_ms) {
			o->rerange_cnt++;
			o->last_outage_ms = now_ms - o->rerange_start_ms;
			o->rerange_start_ms = 0;
			if (!o->rerange_last_log_ms ||
			    ms_after(now_ms, o->rerange_last_log_ms +
					     GPON_PLOAM_RERANGE_LOG_MS)) {
				ev(o, GPON_PLOAM_EV_RERANGE_DONE,
				   o->rerange_cnt, o->last_outage_ms);
				/* FOLLOW-UP P8 */
				o->rerange_last_log_ms = now_ms;
			}
		}
		o->o5_entry_tick = o->ticks ? o->ticks : 1;
		o->avc_sent = 0;	/* re-report oper-up on each online */
	} else if (st != GPON_O5_OPERATION && prev == GPON_O5_OPERATION) {
		o->rerange_start_ms = now_ms ? now_ms : 1;  /* outage timer starts */
		o->o5_entry_tick = 0;
		/* The shell re-arms whatever it gates on O5 (Luna: the VLAN
		 * filter for the next config pass), keeping its own guard. */
		o->ops->on_below_o5(o->sh);
	}

	/* Reflect the state into the silicon's ONU_STATE field and the PON LED.
	 * The shell maps the canonical G.984.3 state to its own encoding —
	 * Luna's field is 1-based and therefore identical, Elnath's is 0-based. */
	o->ops->set_hw_state(o->sh, st);
}

/* Activation teardowns. */
int gpon_ploam_ds(struct gpon_ploam *o, const u8 *m, unsigned int len, u32 now_ms)
{
	u32 tx0 = o->tx_total;
	u8 onu_id, type;
	const u8 *d;

	/* A short frame is dropped with no state change. The driver could never
	 * be handed one (its reader always produces 13 bytes); the bound exists
	 * so the offline fuzzer cannot walk off the buffer. */
	if (!m || len < GPON_PLOAM_DS_LEN)
		return 0;

	onu_id = m[0];
	type   = m[1];
	d      = &m[2];			/* 10 data octets */

	if (onu_id == GPON_PLOAM_ONU_ID_BROADCAST)
		o->ds_bcast[ploam_slot(type)]++;
	else if (onu_id == o->onu_id)
		o->ds_own[ploam_slot(type)]++;
	else
		o->ds_other++;

	/* Surface any downstream PLOAM that is not the repetitive broadcast
	 * acquisition traffic, so activation progress is visible. */
	o->last_ds_type = type;
	if (o->cfg->trace && !gpon_ploam_ds_repetitive(type))
		ev(o, GPON_PLOAM_EV_DS, onu_id, type);

	/* NO DEFINED SERIAL, NO PARTICIPATION */
	if (!gpon_sn_is_set(o->sn))
		return 0;

	switch (type) {
	case PLM_DS_UPSTREAM_OVERHEAD:
		/* The OLT is acquiring ONUs */
		if (o->state < GPON_O3_SERIAL) {
			u32 pre_eqd = ((d[7] >> 5) & 1) ?
				(((u32)d[8] << 8) | d[9]) * 32 * 8 : 0;

			o->boh_guard    = d[0];
			o->boh_t12      = (u8)(((unsigned int)d[1] + d[2]) / 8);
			o->boh_ptn      = d[3];
			o->boh_delim[0] = d[4];
			o->boh_delim[1] = d[5];
			o->boh_delim[2] = d[6];
			ev(o, GPON_PLOAM_EV_BOH, 0,
			   apply_boh(o, false));	/* folds in any prior 0x14 t3pre */
			o->pre_eqd = pre_eqd;
			set_eqd(o, pre_eqd);
			set_state(o, GPON_O2_STANDBY, now_ms);
			set_state(o, GPON_O3_SERIAL, now_ms);
			/* Re-lock the TX CMU PLL now the downstream optics */
			o->ops->analog_relock(o->sh);
			if (o->cfg->o3_feed_reset) {
				/* The relock re-parks the GEM-US feed run-state.
				 * Un-park it with the FULL datapath reset-B edge
				 * the O5-light re-arm omits, here at O3 while
				 * the downstream is not yet locked. */
				o->ops->o3_feed_reset(o->sh);
				ev(o, GPON_PLOAM_EV_O3_FEED_RESET, 0, 0);
			}
			send_sn(o);		/* first SN immediately */
		}
		break;

	case PLM_DS_ASSIGN_ONU_ID:
		/* d[0] = assigned ONU-ID, d[1..8] = serial number to match. */
		if (gpon_ploam_assign_allowed(o->state, onu_id) &&
		    !memcmp(&d[1], o->sn, 8)) {
			struct gpon_omcc_tcont_plan plan;
			u16 tcont16_alloc;

			o->onu_id = d[0];
			o->ops->set_hw_onu_id(o->sh, o->onu_id);
			/* Bind the OMCC's T-CONT to its management Alloc-ID. */
			gpon_omcc_tcont_decide(o->cfg->omcc_alloc_override,
					     o->onu_id, o->cfg->omcc_alt_bind,
					     &plan);
			tcont16_alloc = plan.alloc;
			if (o->ops->install_tcont(o->sh, o->cfg->omcc_tcont,
						  tcont16_alloc))
				break; /* Keep O3: the next matching Assign retries. */
			ev(o, GPON_PLOAM_EV_CAM_READBACK, o->cfg->omcc_tcont,
			   tcont16_alloc);
			/* NON-STOCK double-bind (default off) */
			if (plan.bind_alt &&
			    o->ops->install_tcont(o->sh, o->cfg->omcc_tcont_alt,
						  tcont16_alloc))
				break;
			ev(o, GPON_PLOAM_EV_ONU_ID, o->onu_id, tcont16_alloc);
			set_state(o, GPON_O4_RANGING, now_ms);
		}
		break;

	case PLM_DS_RANGING_TIME:
		/* Accept only the main-path EqD (d[0] bit0 == 0); the
		 * protect-path EqD is not configurable. EqD is d[1..4]
		 * big-endian and is folded with MIN_DELAY1 by set_eqd(). */
		if (gpon_ploam_ranging_allowed(o->state, onu_id,
					       o->onu_id, d[0])) {
			u32 eqd = ((u32)d[1] << 24) | ((u32)d[2] << 16) |
				  ((u32)d[3] << 8) | d[4];
			u32 boh;

			/* In O5 a Ranging_Time moves the EqD and nothing else, as
			 * stock: its FSM handles RX_EQD in O4 only, so the burst
			 * header and the US-PLOAM flush are never redone under the
			 * OLT's O5 grants (the production OLT sends three). */
			if (o->state == GPON_O5_OPERATION) {
				set_eqd(o, eqd);
				ev(o, GPON_PLOAM_EV_RANGING_TIME, eqd, 0);
				break;
			}
			/* Re-apply the O5 packed-burst gate cluster and re-arm.
			 * Still in O4 (stock
			 * writes its template before any O5) and ahead of the group
			 * below, which nothing that may log is allowed into. */
			if (o->cfg->o5_rearm_burst_gate) {
				u8 nomsg[GPON_PLOAM_US_LEN];

				o->ops->o5_rearm_burst(o->sh);
				build_template(nomsg, PLM_US_NO_MESSAGE);
				ploam_tx(o, PLM_US_QUEUE_NOMSG, nomsg);
			}
			if (o->ops->arm_dying_gasp) {
				u8 dg[GPON_PLOAM_US_LEN];

				build_template(dg, PLM_US_DYING_GASP);
				o->ops->arm_dying_gasp(o->sh, dg);
			}
			set_eqd(o, eqd);
			boh = apply_boh(o, true);	/* switch to the ranged burst */
			/* Flush any pre-ranged-format upstream PLOAM still latched */
			o->ops->us_ploam_flush(o->sh);
			/* Stock's O4 -> O5: EqD, ranged BOH, flush, ONU_STATE, back
			 * to back, before any event: a trace hook may print to a
			 * synchronous console, and a GTC left at O4 behind the
			 * ranged flush answered the production OLT's O5 grants as
			 * ranging requests (field X111W 2026-10-02: rng_req 51 and
			 * 88 at the first O5 point, 1 with this order). */
			o->ops->set_hw_state(o->sh, GPON_O5_OPERATION);
			set_state(o, GPON_O5_OPERATION, now_ms);
			ev(o, GPON_PLOAM_EV_BOH, 1, boh);
			ev(o, GPON_PLOAM_EV_RANGING_TIME, eqd, 0);
		}
		break;

	case PLM_DS_DISABLE_SN:
#ifdef CONFIG_OLT_ZTE
	case PLM_DS_DISABLE_SN_ZTE:
#endif
		/* G.984.3 9.2.3.6, 10.2.5.1 i/j: broadcast only; a disable of our * serial number is O7 and silence until an enable, 0x0f = enable * every disabled ONU. */
		if (onu_id != GPON_PLOAM_ONU_ID_BROADCAST)
			break;
		if (o->state == GPON_O7_EMERGENCY) {
			if (sn_enables(o, type, d)) {
				ev(o, GPON_PLOAM_EV_DISABLE_SN, d[0], 1);
				set_state(o, GPON_O1_INITIAL, now_ms);
			}
			break;
		}
		if (!sn_disables(o, type, d))
			break;
		ev(o, GPON_PLOAM_EV_DISABLE_SN, d[0], 0);
		/* A real Disable_SN tears down exactly like an OLT Deactivate, so
		 * it drops into that case rather than duplicating the teardown.
		 * The attribute (not a comment) is what the kernel's
		 * -Wimplicit-fallthrough=5 accepts; see gpon_common.h. */
		fallthrough;

	case PLM_DS_DEACTIVATE_ONU:
		if (onu_id == o->onu_id || onu_id == 0xff) {
			if (o->state == GPON_O7_EMERGENCY)
				break;		/* only an enable leaves O7 */
			ev(o, GPON_PLOAM_EV_DEACT, o->onu_id, 0);
			/* TEARDOWN 1 of 4: OLT Deactivate / Disable_SN. */
			o->onu_id = 0xff;
			o->omcc_installed = false;
			o->omcc_gem = 0;
			o->tcont_installed = false;
			o->data_installed = false;
			o->data_gem_solicited = false;
			o->data_tcont_installed = false;
			o->data_alloc = 0;
			gpon_ploam_data_alloc_reset(o);
			o->aes_switch_time = 0xffffffff;
			o->key_staged = false;
			o->ops->set_hw_onu_id(o->sh, 0xff);
			/* Re-seat the serializer ONLY when the prior O5 */
			if (o->cfg->cdr_reseat_on_reactivate &&
			    !(o->state == GPON_O5_OPERATION && o->o5_entry_tick &&
			      (o->ticks - o->o5_entry_tick) > GPON_PLOAM_HEALTHY_O5_TICKS)) {
				o->ops->cdr_reseat(o->sh);
				o->keep_lock_tick = 0;
			} else if (o->cfg->cdr_reseat_on_reactivate) {
				ev(o, GPON_PLOAM_EV_DEACT_KEEP_LOCK,
				   o->o5_entry_tick ? o->ticks - o->o5_entry_tick : 0, 0);
				o->keep_lock_tick = o->ticks ? o->ticks : 1;
			}
			set_state(o, type == PLM_DS_DEACTIVATE_ONU ?
				     GPON_O1_INITIAL : GPON_O7_EMERGENCY, now_ms);
		}
		break;

	case PLM_DS_EXT_BURST_LENGTH:
		/* Extended_Burst_Length */
		o->boh_t3ranged = d[1];		/* applied at the O5 transition */
		if (o->onu_id == 0xff && o->boh_t3pre != d[0]) {
			o->boh_t3pre = d[0];
			ev(o, GPON_PLOAM_EV_BOH, 0, apply_boh(o, false));
			ev(o, GPON_PLOAM_EV_EXT_BURST, o->boh_t3pre,
			   o->boh_t3ranged);
		}
		break;

	case PLM_DS_CONFIG_PORT:
		/* Configure_Port-ID */
		if (addressed(o, onu_id, false)) {
			u16 gem = ((u16)d[1] << 4) | (d[2] >> 4);

			/* REBIND ON CHANGE, not once. */
			enum gpon_omcc_action act =
				gpon_omcc_decide(d[0] & 0x1, gem,
						 o->omcc_installed,
						 o->omcc_gem);

			if (act == GPON_OMCC_INSTALL ||
			    act == GPON_OMCC_REBIND) {
				if (!o->ops->install_omcc(o->sh, gem)) {
					o->omcc_installed = true;
					o->omcc_gem = gem;
				}
			}
			send_ack(o, m);
			if (o->cfg->trace)
				ev(o, GPON_PLOAM_EV_ACK, type, 0);
		}
		break;

	case PLM_DS_ASSIGN_ALLOC_ID:
		/* Assign_Alloc-ID: alloc = (d[0]<<4)|(d[1]>>4) */
		if (addressed(o, onu_id, false)) {
			u16 alloc = ((u16)d[0] << 4) | (d[1] >> 4);

			ev(o, GPON_PLOAM_EV_ASSIGN_ALLOC, alloc, d[2]);
			if (alloc != o->onu_id && gpon_data_alloc_valid(alloc) &&
			    (d[2] == 0x01 || d[2] == 0xff))
				gpon_ploam_data_alloc_note(o, alloc, d[2] == 0x01);
			if (o->ops->data_alloc_changed) {
				if (alloc != o->onu_id && gpon_data_alloc_valid(alloc) &&
				    (d[2] == 0x01 || d[2] == 0xff))
					o->ops->data_alloc_changed(o->sh, alloc, d[2] == 0x01);
			} else if (d[2] == 0x01) {
				if (alloc != o->onu_id && !o->data_tcont_installed) {
					if (!o->ops->install_tcont(o->sh,
							o->cfg->data_tcont, alloc)) {
						o->data_tcont_installed = true;
						o->data_alloc = alloc;
						ev(o, GPON_PLOAM_EV_DATA_TCONT,
						   alloc, o->cfg->data_tcont);
					}
				}
			} else if (d[2] == 0xff && o->data_tcont_installed &&
				   alloc == o->data_alloc) {
				/* OLT deallocated the data Alloc-ID: allow a
				 * clean re-bind on the next allocate. The CAM
				 * binding is idempotent; no teardown needed. */
				o->data_tcont_installed = false;
			}
			send_ack(o, m);
			if (o->cfg->trace)
				ev(o, GPON_PLOAM_EV_ACK, type, 0);
		}
		break;

	case PLM_DS_REQUEST_KEY:
		/* The OLT requests a downstream AES key; reply with
		 * Encryption_Key and load the same key into the staged bank. */
		if (addressed(o, onu_id, false)) {
			send_key(o);
			ev(o, GPON_PLOAM_EV_REQ_KEY, 0, 0);
		}
		break;

	case PLM_DS_REQUEST_PASSWORD:
		/* GROUND TRUTH: without the reply the OLT stalls at O5 spamming
		 * this and deactivates us with LOAi. The lab OLT ignores the value;
		 * a production OLT deactivated a zero one (field X111W, 2026-09-30).
		 * Broadcast is accepted, like stock. */
		if (addressed(o, onu_id, true)) {
			send_password(o);
			ev(o, GPON_PLOAM_EV_REQ_PW, 0, 0);
		}
		break;

	case PLM_DS_KEY_SWITCH:
		/* Key_Switching_Time */
		if (addressed(o, onu_id, true)) {
			u32 fc = ((u32)(d[0] & 0x3f) << 24) | ((u32)d[1] << 16) |
				 ((u32)d[2] << 8) | d[3];

			/* Arm only once a key has actually been loaded into the
			 * staged bank: arming a switch to an empty or stale bank
			 * would promote a garbage key and corrupt AES.
			 * Acknowledge either way so the OLT sees it handled. */
			if (o->key_staged && fc != o->aes_switch_time) {
				o->aes_switch_time = fc;
				o->ops->aes_arm_switch(o->sh, fc);
				ev(o, GPON_PLOAM_EV_KEY_SWITCH_ARM, fc, 0);
			}
			send_ack(o, m);
			ev(o, GPON_PLOAM_EV_KEY_SWITCH, o->key_staged ? 1 : 0,
			   o->aes_switch_time);
		}
		break;

	case PLM_DS_ENCRYPT_PORT:
		/* Encrypted_Port-ID: G.984.3 requires an Acknowledge; the OLT
		 * arms a ~43 s timer and Deactivates us if none arrives.  d[0]
		 * bit 0 marks the Port-ID encrypted, bit 1 must be set or the
		 * mark is ignored (9.2.3.8): a family that cannot decrypt says
		 * so instead of delivering ciphertext. */
		if (addressed(o, onu_id, false)) {
			u16 port = ((u16)d[1] << 4) | (d[2] >> 4);

			if ((d[0] & 2) && o->ops->port_encrypt)
				o->ops->port_encrypt(o->sh, port, d[0] & 1);
			else if ((d[0] & 3) == 3)
				unsup(o, "ds_port_encryption", GPON_UNSUP_UNKNOWN,
				      port, "a-family-that-decrypts", m,
				      GPON_PLOAM_DS_LEN);
			send_ack(o, m);
			if (o->cfg->trace)
				ev(o, GPON_PLOAM_EV_ACK, type, 0);
		}
		break;

	case PLM_DS_CFG_VPVC:
		/* Configure_VP/VC: legacy ATM connection setup, unsupported on
		 * a GEM ONU — but stock still Acknowledges, and a missing ACK
		 * to any acknowledge-required downstream PLOAM raises LOAi. */
		if (addressed(o, onu_id, false)) {
			send_ack(o, m);
			ev(o, GPON_PLOAM_EV_ACK, type, 0);
		}
		break;

	case PLM_DS_BER_INTERVAL:
		/* BER_interval */
		if (addressed(o, onu_id, true)) {
			u32 frames = ((u32)d[0] << 24) | ((u32)d[1] << 16) |
				     ((u32)d[2] << 8) | d[3];

			send_ack(o, m);
			ev(o, GPON_PLOAM_EV_ACK, type, 0);
			o->rei_interval_ms = frames / 8;	/* 125 us frames */
			o->rei_due_ms = now_ms + o->rei_interval_ms;
			o->rei_seq = 0;
		}
		break;

	case PLM_DS_RANGING_ADJ:
		/* G.984.3 Annex D: O5 only, octet 3 bit 1 = decrease (bit 0, as
		 * the chip SDK reads it, too); a unicast one is acknowledged.  An
		 * EqD below zero cannot be: it is reported, never written. */
		if (o->state == GPON_O5_OPERATION && addressed(o, onu_id, true)) {
			u32 delta = ((u32)d[1] << 24) | ((u32)d[2] << 16) |
				    ((u32)d[3] << 8) | d[4];

			if (!(d[0] & 3))
				set_eqd(o, o->eqd + delta);
			else if (delta <= o->eqd)
				set_eqd(o, o->eqd - delta);
			else
				unsup(o, "ranging_adjustment", GPON_UNSUP_RANGE,
				      delta, "at-most-the-current-eqd", m,
				      GPON_PLOAM_DS_LEN);
			if (onu_id == o->onu_id)
				send_ack(o, m);
			ev(o, GPON_PLOAM_EV_RANGING_TIME, o->eqd, 1);
		}
		break;

	case PLM_DS_POPUP:
	case PLM_DS_SWIFT_POPUP:
		/* G.984.3 10.2.5.1 f/g, Annex D: a broadcast POPUP re-ranges with
		 * the ONU-ID kept (O4, pre-ranged burst and EqD, TO1); a directed
		 * one or a Swift_POPUP is O5, which we never left. */
		if (!popup_pending(o) ||
		    (onu_id != o->onu_id && onu_id != GPON_PLOAM_ONU_ID_BROADCAST))
			break;
		o->popup_until_tick = 0;
		if (type == PLM_DS_POPUP && onu_id == GPON_PLOAM_ONU_ID_BROADCAST) {
			u32 boh = apply_boh(o, false);

			set_eqd(o, o->pre_eqd);
			set_state(o, GPON_O4_RANGING, now_ms);
			ev(o, GPON_PLOAM_EV_BOH, 0, boh);
		}
		break;

	case PLM_DS_NO_MESSAGE:
	case PLM_DS_SN_MASK:
	case PLM_DS_PON_ID:
		break;

	default:
		/* Anything addressed to us that we do not model */
		if (onu_id == o->onu_id || onu_id == 0xff) {
			ev(o, GPON_PLOAM_EV_UNHANDLED, type, onu_id);
			/* and say WHAT it was. */
			unsup(o, "ds_ploam_type", GPON_UNSUP_UNKNOWN, type,
			      "G.984.3-DS-type-this-ONU-models",
			      m, GPON_PLOAM_DS_LEN);
		}
		break;
	}

	o->ds_rx++;			/* downstream-lock liveness */
	return (int)(o->tx_total - tx0);
}

/* Poll islands. */
u32 gpon_ploam_tick(struct gpon_ploam *o)
{
	o->ticks++;
	return o->ticks;
}

/* 6564-6579 — the serial number was (re)provisioned */
int gpon_ploam_sn_changed(struct gpon_ploam *o, u32 now_ms)
{
	u32 tx0 = o->tx_total;

	if (o->sn_changed) {
		o->sn_changed = false;
		gpon_ploam_data_alloc_reset(o);
		if (o->state > GPON_O1_INITIAL) {
			o->onu_id = 0xff;
			o->omcc_installed = false;
			o->omcc_gem = 0;
			o->tcont_installed = false;
			o->data_installed = false;
			o->data_gem_solicited = false;
			o->data_tcont_installed = false;
			o->data_alloc = 0;
			o->aes_switch_time = 0xffffffff;
			o->key_staged = false;
			o->ops->set_hw_onu_id(o->sh, 0xff);
			set_state(o, GPON_O1_INITIAL, now_ms);
			ev(o, GPON_PLOAM_EV_SN_REPROVISIONED, 0, 0);
		}
	}
	return (int)(o->tx_total - tx0);
}

/* 6604-6613 — two O5 gates that must stay adjacent and */
int gpon_ploam_poll_provision(struct gpon_ploam *o, u32 now_ms)
{
	(void)now_ms;

	if (o->state == GPON_O5_OPERATION && o->cfg->data_gem_en &&
	    o->omcc_installed && o->data_gem_solicited && !o->data_installed)
		o->ops->install_data_gem(o->sh, o->data_gem_port);

	if (o->state == GPON_O5_OPERATION && o->omcc_installed &&
	    o->avc_sent < GPON_PLOAM_AVC_MAX && o->o5_entry_tick &&
	    (o->ticks - o->o5_entry_tick) > GPON_PLOAM_AVC_DELAY_TICKS &&
	    ((o->ticks - o->o5_entry_tick) % GPON_PLOAM_AVC_PERIOD_TICKS) == 0) {
		o->ops->omci_report_oper_up(o->sh);
		o->avc_sent++;
	}
	return 0;		/* emits no upstream PLOAM */
}

/* 6650-6671 — O5 provisioning watchdog. */
/* Forget the activation and re-enter O1 (the O5 watchdogs' common exit). */
static void leave_o5(struct gpon_ploam *o, u32 now_ms)
{
	o->onu_id = 0xff;
	o->omcc_installed = false;
	o->omcc_gem = 0;
	o->tcont_installed = false;
	o->data_installed = false;
	o->data_tcont_installed = false;
	o->data_alloc = 0;
	o->aes_switch_time = 0xffffffff;
	o->key_staged = false;
	o->ops->set_hw_onu_id(o->sh, 0xff);
	if (o->cfg->cdr_reseat_on_reactivate)
		o->ops->cdr_reseat(o->sh);
	set_state(o, GPON_O1_INITIAL, now_ms);
}

int gpon_ploam_poll_watchdog(struct gpon_ploam *o, bool wan_rx_zero, u32 now_ms)
{
	u32 tx0 = o->tx_total;

	/* THE EARLY DWELL REPORT, AND IT ACTS ON */
	if (o->cfg->early_dwell_report_ticks && o->state <= GPON_O3_SERIAL) {
		/* ⚠ DEFENSIVE, AND SAID PLAINLY. */
		u32 held = o->ticks >= o->early_entry_tick
			 ? o->ticks - o->early_entry_tick : 0;
		u32 since = o->ticks >= o->early_report_tick
			  ? o->ticks - o->early_report_tick : 0;

		if (!o->early_report_tick ||
		    since >= o->cfg->early_dwell_report_ticks) {
			o->early_report_tick = o->ticks ? o->ticks : 1;
			ev(o, GPON_PLOAM_EV_EARLY_DWELL, (u32)o->state, held);
		}
	}

	/* A kept lock that never gets back past O3 was a bad one: the laptop X111W
	 * (2026-09-30) decoded the DS as garbage after it and never ranged again. */
	if (o->keep_lock_tick && o->state < GPON_O4_RANGING &&
	    (o->ticks - o->keep_lock_tick) > GPON_PLOAM_KEEP_LOCK_GIVEUP_TICKS) {
		ev(o, GPON_PLOAM_EV_KEEP_LOCK_GIVEUP, o->ticks - o->keep_lock_tick,
		   (u32)o->state);
		o->keep_lock_tick = 0;
		o->ops->cdr_reseat(o->sh);
		set_state(o, GPON_O1_INITIAL, now_ms);
		return (int)(o->tx_total - tx0);
	}

	/* An SN nobody hears is never assigned nor deactivated: leave O3 so the next * entry relocks. */
	if (o->state == GPON_O3_SERIAL && o->early_entry_tick &&
	    gpon_dwell_expired(o->state, o->ticks - o->early_entry_tick, 0)) {
		ev(o, GPON_PLOAM_EV_O3_UNHEARD, o->ticks - o->early_entry_tick, o->sn_tx);
		set_state(o, GPON_O1_INITIAL, now_ms);
		return (int)(o->tx_total - tx0);
	}

	/* THE RANGING TIMER (G.984.3). */
	if (o->state == GPON_O4_RANGING && o->o4_entry_tick &&
	    gpon_dwell_expired(o->state, o->ticks - o->o4_entry_tick,
			       o->cfg->o4_ranging_timeout_ticks)) {
		ev(o, GPON_PLOAM_EV_O4_TIMEOUT, o->ticks - o->o4_entry_tick, 0);
		o->onu_id = 0xff;
		o->omcc_installed = false;	/* a POPUP O4 still held them */
		o->omcc_gem = 0;
		o->tcont_installed = false;
		o->data_installed = false;
		o->data_tcont_installed = false;
		o->data_alloc = 0;
		o->aes_switch_time = 0xffffffff;
		o->key_staged = false;
		o->ops->set_hw_onu_id(o->sh, 0xff);
		set_state(o, GPON_O1_INITIAL, now_ms);
		return (int)(o->tx_total - tx0);
	}

	if (o->cfg->o5_provision_watchdog_ticks &&
	    o->state == GPON_O5_OPERATION && o->onu_id != 0xff &&
	    o->o5_entry_tick &&
	    (o->ticks - o->o5_entry_tick) > o->cfg->o5_provision_watchdog_ticks &&
	    wan_rx_zero) {
		ev(o, GPON_PLOAM_EV_O5_WATCHDOG, o->ticks - o->o5_entry_tick, 0);
		leave_o5(o, now_ms);
	}
	return (int)(o->tx_total - tx0);
}

/* An O5 the OLT no longer grants is an O5 on our side only: the OLT has dropped us ("Laser out",
 * Offline) and sends no Deactivate to act on -- the X111W sat so 13 min after a cold boot
 * (2026-10-12). Back to O1, so the next O3 entry relocks and the OLT re-admits us. A granted O5,
 * provisioned or idle, never trips; a run of misses backs off so a dead path cannot churn the OLT.
 * `grants`: grants addressed to our ONU-ID since the last call.
 */
int gpon_ploam_poll_grants(struct gpon_ploam *o, u32 grants, u32 now_ms)
{
	u32 from, limit;

	if (o->state != GPON_O5_OPERATION || grants) {
		if (grants && o->state == GPON_O5_OPERATION)
			o->ungranted_streak = 0;
		o->grant_tick = o->ticks;
		return 0;
	}
	if (!o->cfg->o5_ungranted_ticks || o->onu_id == 0xff || !o->o5_entry_tick)
		return 0;
	from = (s32)(o->grant_tick - o->o5_entry_tick) > 0 ? o->grant_tick : o->o5_entry_tick;
	limit = o->cfg->o5_ungranted_ticks << (o->ungranted_streak < 5 ? o->ungranted_streak : 5);
	if (o->ticks - from <= limit)
		return 0;
	ev(o, GPON_PLOAM_EV_O5_UNGRANTED, o->ticks - from, o->ungranted_streak);
	o->ungranted_streak++;
	leave_o5(o, now_ms);
	return 1;
}

/* 6680-6724 — autonomous downstream-LOS recovery */
int gpon_ploam_poll_los(struct gpon_ploam *o, bool optic_los, bool sds_dark,
			u32 now_ms)
{
	u32 tx0 = o->tx_total;

	if (o->cfg->los_rerange_ticks && o->state >= GPON_O2_STANDBY &&
	    o->state != GPON_O7_EMERGENCY) {
		if (optic_los && sds_dark) {
			if (++o->los_run == o->cfg->los_rerange_ticks) {
				ev(o, GPON_PLOAM_EV_LOS_RERANGE, o->los_run, 0);
				o->onu_id = 0xff;
				o->omcc_installed = false;
				o->omcc_gem = 0;
				o->tcont_installed = false;
				o->data_installed = false;
				/* The Alloc-ID is the OLT's to reissue on
				 * re-admit, so the data T-CONT binding is
				 * session state and goes with the rest. */
				o->data_tcont_installed = false;
				o->data_alloc = 0;
				/* 2026-07-05: do NOT clear data_gem_solicited on a */
				o->aes_switch_time = 0xffffffff;
				o->key_staged = false;
				o->ops->set_hw_onu_id(o->sh, 0xff);
				if (o->cfg->cdr_reseat_on_reactivate)
					o->ops->cdr_reseat(o->sh);
				set_state(o, GPON_O1_INITIAL, now_ms);
			}
		} else {
			if (o->los_run && o->state == GPON_O5_OPERATION)
				o->popup_until_tick = o->ticks + o->cfg->los_rerange_ticks;
			o->los_run = 0;
		}
	}
	return (int)(o->tx_total - tx0);
}

/* :6834-6836 — while unregistered at O3, re-offer the Serial_Number_ONU about
 * twice a second (the OLT grants SN windows intermittently). */
int gpon_ploam_poll_sn_reoffer(struct gpon_ploam *o, u32 now_ms)
{
	u32 tx0 = o->tx_total;

	(void)now_ms;
	if (o->state >= GPON_O3_SERIAL && o->state != GPON_O7_EMERGENCY &&
	    o->onu_id == 0xff && (o->ticks % GPON_PLOAM_SN_REOFFER_TICKS) == 0)
		send_sn(o);
	return (int)(o->tx_total - tx0);
}

/* 6845-6854 — periodic O5 upstream-PLOAM keepalive. */
int gpon_ploam_poll_keepalive(struct gpon_ploam *o, u32 now_ms)
{
	u32 tx0 = o->tx_total;

	(void)now_ms;
	if (o->state == GPON_O5_OPERATION && o->onu_id != 0xff &&
	    o->cfg->o5_ploam_keepalive_ticks &&
	    (o->ticks % o->cfg->o5_ploam_keepalive_ticks) == 0) {
		u8 nomsg[GPON_PLOAM_US_LEN];

		build_template(nomsg, PLM_US_NO_MESSAGE);
		ploam_tx(o, PLM_US_QUEUE_NOMSG, nomsg);
	}
	return (int)(o->tx_total - tx0);
}

/* Remote_Error_Indication, as stock: every BER interval at O5, the DS BIP
 * block-error count big-endian in octets 3..6 and a 4-bit sequence in octet 7.
 * A late poll sends one REI, never a catch-up burst. */
int gpon_ploam_poll_rei(struct gpon_ploam *o, u32 now_ms)
{
	u8 p[GPON_PLOAM_US_LEN] = { 0 };
	u32 bip;

	if (o->state != GPON_O5_OPERATION || !o->rei_interval_ms ||
	    ms_after(o->rei_due_ms, now_ms))
		return 0;
	o->rei_due_ms = now_ms + o->rei_interval_ms;
	if (!o->ops->ds_bip_errors || o->ops->ds_bip_errors(o->sh, &bip))
		return 0;
	p[0] = o->onu_id;
	p[1] = PLM_US_REI;
	p[2] = (u8)(bip >> 24);
	p[3] = (u8)(bip >> 16);
	p[4] = (u8)(bip >> 8);
	p[5] = (u8)bip;
	p[6] = o->rei_seq;
	o->rei_seq = (o->rei_seq + 1) & 0xf;
	ploam_tx(o, PLM_US_QUEUE_URG, p);
	return 1;
}

/* Identity and lifecycle. */

/* Up to 10 printable octets, or exactly 20 hex digits -- the form some vendor
 * MIBs store (the X100DG's GPON_PLOAM_PASSWD "31323334353637383930"); a 20-char
 * printable password does not exist, so the two cannot be confused. One
 * trailing newline tolerated; zero-padded. -> 0 | -1 */
int gpon_ploam_password_parse(const char *s, u8 out[GPON_PLOAM_PASSWORD_LEN])
{
	unsigned int i, n = 0;

	memset(out, 0, GPON_PLOAM_PASSWORD_LEN);
	if (!s)
		return -1;
	while (s[n] && !(s[n] == '\n' && !s[n + 1]))
		n++;
	if (n == 2 * GPON_PLOAM_PASSWORD_LEN) {
		for (i = 0; i < n && gpon_hex_digit(s[i]) >= 0; i++)
			;
		if (i == n) {
			for (i = 0; i < GPON_PLOAM_PASSWORD_LEN; i++)
				out[i] = (u8)(gpon_hex_digit(s[2 * i]) << 4 | gpon_hex_digit(s[2 * i + 1]));
			return 0;
		}
	}
	if (n > GPON_PLOAM_PASSWORD_LEN)
		return -1;
	for (i = 0; i < n; i++) {
		if (s[i] < 0x20 || s[i] > 0x7e)
			return -1;
		out[i] = (u8)s[i];
	}
	return 0;
}

void gpon_ploam_set_password(struct gpon_ploam *o, const u8 pwd[GPON_PLOAM_PASSWORD_LEN])
{
	memcpy(o->password, pwd, sizeof(o->password));
}

void gpon_ploam_set_sn(struct gpon_ploam *o, const u8 sn[8])
{
	memcpy(o->sn, sn, sizeof(o->sn));
	o->sn_changed = true;
}

void gpon_ploam_set_data_gem_solicited(struct gpon_ploam *o, bool solicited,
				       u16 port_id)
{
	o->data_gem_solicited = solicited;
	if (!solicited)
		return;
	/* THE MULTICAST GEM IS NOT THE WAN DATA GEM (G.988). */
	if ((port_id & GPON_GEM_US_PORT_MASK) == GPON_MCAST_GEM_PORT) {
		o->data_gem_solicited = false;
		return;
	}
	/* A Port-ID that MOVED re-arms the install, so the datapath follows the
	 * OLT instead of keeping a retired GEM port on the wire. Repeating the
	 * same one is idempotent: the install pulses the upstream-NIC classify
	 * latch, so it must not run once per ME 268. */
	if (o->data_installed && port_id != o->data_gem_port)
		o->data_installed = false;
	o->data_gem_port = port_id;
}

void gpon_ploam_set_data_installed(struct gpon_ploam *o, bool installed)
{
	o->data_installed = installed;
}

/* THE PURE-VIRTUAL ANALOGUE. */
const char *gpon_ploam_ops_missing(const struct gpon_ploam_ops *ops)
{
	if (!ops)
		return "ops";
#define GPON_PLOAM_OPS_CHECK_ONE(name)					\
	if (!ops->name)							\
		return #name;
	GPON_PLOAM_OPS_MANDATORY(GPON_PLOAM_OPS_CHECK_ONE)
#undef GPON_PLOAM_OPS_CHECK_ONE
	return NULL;
}

const char *gpon_ploam_init(struct gpon_ploam *o,
			    const struct gpon_ploam_ops *ops,
			    const struct gpon_ploam_cfg *cfg, void *sh,
			    const u8 sn[8])
{
	static const u8 delim_default[3] = GPON_PLOAM_BOH_DELIM_DEFAULT;
	const char *missing = gpon_ploam_ops_missing(ops);

	/* REFUSE BEFORE INSTALLING ANYTHING. */
	memset(o, 0, sizeof(*o));
	if (missing)
		return missing;

	o->ops = ops;
	o->cfg = cfg;
	o->sh  = sh;

	if (sn)
		memcpy(o->sn, sn, sizeof(o->sn));
	o->onu_id = 0xff;
	o->state  = GPON_O1_INITIAL;
	o->boh_ptn = GPON_PLOAM_BOH_PTN_DEFAULT;
	memcpy(o->boh_delim, delim_default, sizeof(o->boh_delim));
	o->aes_switch_time = 0xffffffff;
	return NULL;			/* the table is complete */
}
