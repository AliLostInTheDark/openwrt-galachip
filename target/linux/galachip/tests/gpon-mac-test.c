// SPDX-License-Identifier: GPL-2.0-only
/*
 * Unit tests for GC1601 GPON MAC PLOAM state machine and Nokia ISAM 7360 FX
 * recovery sequences (ITU-T G.984.3 compliance).
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define O1	1
#define O2	2
#define O3	3
#define O4	4
#define O5	5
#define O6	6
#define O7	7

#define GPON_BROADCAST		0xff
#define GPON_ACTIVATION_MS	10000	/* TO1: 10s */
#define GPON_POPUP_MS		100	/* TO2: 100ms */

#define DN_UPSTREAM_OVERHEAD	0x01
#define DN_ASSIGN_ONU_ID	0x03
#define DN_RANGING_TIME		0x04
#define DN_DEACTIVATE_ONU	0x05
#define DN_DISABLE_SN		0x06
#define DN_POPUP		0x0c

struct test_gpon {
	uint8_t state;
	uint8_t onu_id;
	uint8_t serial[8];
	bool ranged;
	bool have_overhead;
	bool allow_tx;
	bool olt_tx_disabled;
	bool identity_wait;
	bool running;
	bool los;
	bool lof;
	bool timer_running;
	unsigned int timer_ms;
	unsigned int to1_starts;
	uint32_t eqd;
	unsigned int o4_entries;
	unsigned int o5_entries;
	unsigned int o6_entries;
	unsigned int o1_entries;
	unsigned int o2_entries;
	unsigned int o7_entries;
	uint64_t ploam_rx;
	uint64_t ploam_crc_errors;
};

static void test_timer_start(struct test_gpon *g, unsigned int ms)
{
	g->timer_running = true;
	g->timer_ms = ms;
}

static void test_timer_stop(struct test_gpon *g)
{
	g->timer_running = false;
	g->timer_ms = 0;
}

static void test_gpon_enter(struct test_gpon *g, int state)
{
	int prev = g->state;
	g->state = state;

	switch (state) {
	case O1:
		g->o1_entries++;
		test_timer_stop(g);
		g->ranged = false;
		g->have_overhead = false;
		g->onu_id = GPON_BROADCAST;
		break;
	case O2:
		g->o2_entries++;
		test_timer_stop(g);
		g->ranged = false;
		g->onu_id = GPON_BROADCAST;
		break;
	case O3:
		g->to1_starts++;
		test_timer_start(g, GPON_ACTIVATION_MS);
		break;
	case O4:
		g->o4_entries++;
		/* G.984.3 10.2.1: TO1 bounds O3 and O4 together. */
		if (prev != O3) {
			g->to1_starts++;
			test_timer_start(g, GPON_ACTIVATION_MS);
		}
		break;
	case O5:
		g->o5_entries++;
		test_timer_stop(g);
		g->ranged = true;
		break;
	case O6:
		g->o6_entries++;
		test_timer_start(g, GPON_POPUP_MS);
		break;
	case O7:
		g->o7_entries++;
		test_timer_stop(g);
		break;
	}
}

static bool test_sn_match(struct test_gpon *g, const uint8_t *msg)
{
	return !memcmp(msg + 3, g->serial, sizeof(g->serial));
}

static void test_gpon_sync(struct test_gpon *g, bool sync)
{
	if (sync) {
		if (g->state == O1)
			test_gpon_enter(g, O2);
		return;
	}
	switch (g->state) {
	case O2:
	case O3:
	case O4:
		test_gpon_enter(g, O1);
		break;
	case O5:
		test_gpon_enter(g, O6);
		break;
	}
}

static void test_gpon_timer_expiry(struct test_gpon *g)
{
	if (!g->running || !g->timer_running)
		return;

	g->timer_running = false;
	if (g->state == O3 || g->state == O4) {
		test_gpon_enter(g, O2);
	} else if (g->state == O6) {
		test_gpon_enter(g, O1);
		if (!g->los && !g->lof)
			test_gpon_enter(g, O2);
	}
}

static void test_handle_ploam(struct test_gpon *g, const uint8_t *msg)
{
	uint8_t onu = msg[0], id = msg[1];
	bool addressed = (onu == g->onu_id && onu != GPON_BROADCAST);
	bool bcast = (onu == GPON_BROADCAST);

	if (id == DN_DISABLE_SN) {
		if (bcast) {
			if (g->state == O7) {
				if (msg[2] == 0x0f || (msg[2] == 0x00 && test_sn_match(g, msg)))
					test_gpon_enter(g, O2);
				return;
			}
			if (msg[2] == 0xff && test_sn_match(g, msg))
				test_gpon_enter(g, O7);
		}
		return;
	}

	switch (g->state) {
	case O2:
		if (id == DN_UPSTREAM_OVERHEAD && bcast) {
			g->have_overhead = true;
			if (g->allow_tx && !g->olt_tx_disabled && !g->identity_wait)
				test_gpon_enter(g, O3);
		}
		break;
	case O3:
		if (id == DN_ASSIGN_ONU_ID && test_sn_match(g, msg)) {
			g->onu_id = msg[2];
			test_gpon_enter(g, O4);
		}
		break;
	case O4:
	case O5:
		if (id == DN_DEACTIVATE_ONU && (bcast || addressed)) {
			test_gpon_enter(g, O2);
			break;
		}
		if (!addressed && !bcast)
			break;
		if (id == DN_RANGING_TIME && msg[2] == 0) {
			g->eqd = (msg[3] << 24) | (msg[4] << 16) | (msg[5] << 8) | msg[6];
			if (g->state == O4)
				test_gpon_enter(g, O5);
		}
		break;
	case O6:
		if (id == DN_DEACTIVATE_ONU && (bcast || addressed)) {
			test_gpon_enter(g, O2);
		} else if (id == DN_POPUP && (bcast || addressed)) {
			if (addressed && g->ranged)
				test_gpon_enter(g, O5);
			else
				test_gpon_enter(g, O4);
		}
		break;
	}
}

static uint8_t test_gpon_crc8(const uint8_t *data, size_t len)
{
	uint8_t crc = 0;
	size_t i, j;

	for (i = 0; i < len; i++) {
		crc ^= data[i];
		for (j = 0; j < 8; j++) {
			if (crc & 0x80)
				crc = (crc << 1) ^ 0x07;
			else
				crc <<= 1;
		}
	}
	return crc;
}

static bool test_receive_ploam(struct test_gpon *g, const uint8_t *raw_msg13)
{
	if (test_gpon_crc8(raw_msg13, 12) != raw_msg13[12]) {
		g->ploam_crc_errors++;
		return false;
	}
	g->ploam_rx++;
	test_handle_ploam(g, raw_msg13);
	return true;
}

int main(void)
{
	struct test_gpon g = {
		.state = O1,
		.onu_id = GPON_BROADCAST,
		.serial = { 'T', 'P', 'L', 'G', 0x12, 0x34, 0x56, 0x78 },
		.allow_tx = true,
		.running = true,
	};
	unsigned int checks = 0;

#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { \
		fprintf(stderr, "Check failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
		return 1; \
	} \
} while (0)

	/* 1. Initial State O1 -> Optical Sync -> Standby State O2 */
	CHECK(g.state == O1);
	test_gpon_sync(&g, true);
	CHECK(g.state == O2);
	CHECK(!g.timer_running);

	/* 2. Upstream Overhead -> O3 (Serial Number State) with TO1 = 10s */
	uint8_t ploam_overhead[13] = { GPON_BROADCAST, DN_UPSTREAM_OVERHEAD, 0 };
	test_handle_ploam(&g, ploam_overhead);
	CHECK(g.state == O3);
	CHECK(g.timer_running && g.timer_ms == GPON_ACTIVATION_MS);

	/* 3. Assign ONU-ID -> O4 (Ranging State); the O3 TO1 keeps running */
	uint8_t ploam_assign_id[13] = {
		GPON_BROADCAST, DN_ASSIGN_ONU_ID, 12, /* ONU-ID 12 */
		'T', 'P', 'L', 'G', 0x12, 0x34, 0x56, 0x78
	};
	test_handle_ploam(&g, ploam_assign_id);
	CHECK(g.state == O4);
	CHECK(g.onu_id == 12);
	CHECK(g.timer_running && g.timer_ms == GPON_ACTIVATION_MS);
	CHECK(g.to1_starts == 1);
	CHECK(g.o4_entries == 1);

	/* 4. Ranging Time -> O5 (Operation State), TO1 stopped, ranged flag set */
	uint8_t ploam_ranging[13] = {
		12, DN_RANGING_TIME, 0x00, /* path 0 */
		0x00, 0x01, 0x23, 0x45    /* EqD */
	};
	test_handle_ploam(&g, ploam_ranging);
	CHECK(g.state == O5);
	CHECK(g.ranged == true);
	CHECK(!g.timer_running);
	CHECK(g.eqd == 0x00012345);

	/* 5. Nokia ISAM 7360 scenario A: Optical drop in O5 -> O6, TO2 = 100ms started */
	test_gpon_sync(&g, false);
	CHECK(g.state == O6);
	CHECK(g.timer_running && g.timer_ms == GPON_POPUP_MS);
	CHECK(g.o6_entries == 1);

	/* 6. Nokia ISAM directed Popup to ONU-ID 12 -> returns immediately to O5 */
	uint8_t ploam_popup_directed[13] = { 12, DN_POPUP, 0 };
	test_handle_ploam(&g, ploam_popup_directed);
	CHECK(g.state == O5);
	CHECK(!g.timer_running);
	CHECK(g.o5_entries == 2);

	/* 7. Nokia ISAM scenario B: Optical drop -> O6, followed by broadcast Popup */
	test_gpon_sync(&g, false);
	CHECK(g.state == O6);
	uint8_t ploam_popup_bcast[13] = { GPON_BROADCAST, DN_POPUP, 0 };
	test_handle_ploam(&g, ploam_popup_bcast);
	CHECK(g.state == O4);
	CHECK(g.timer_running && g.timer_ms == GPON_ACTIVATION_MS);
	CHECK(g.to1_starts == 2);	/* broadcast POPUP starts TO1 */
	CHECK(g.o4_entries == 2);

	/* Re-range to O5 */
	test_handle_ploam(&g, ploam_ranging);
	CHECK(g.state == O5);

	/* 8. Nokia ISAM scenario C: Fiber flap with fast optical recovery before TO2 expiry */
	test_gpon_sync(&g, false);
	CHECK(g.state == O6);
	/* Signal is good again */
	g.los = false;
	g.lof = false;
	/* TO2 expires after 100ms */
	test_gpon_timer_expiry(&g);
	/* Must seamlessly transition O6 -> O1 -> O2 instead of getting stuck in O1 */
	CHECK(g.state == O2);
	CHECK(!g.timer_running);

	/* 9. Standby O2 -> Overhead -> O3 -> TO1 timeout -> returns to O2 */
	test_handle_ploam(&g, ploam_overhead);
	CHECK(g.state == O3);
	test_gpon_timer_expiry(&g);
	CHECK(g.state == O2);

	/* 10. Re-enter O3 -> O4 -> TO1 timeout -> returns to O2 */
	test_handle_ploam(&g, ploam_overhead);
	CHECK(g.state == O3);
	test_handle_ploam(&g, ploam_assign_id);
	CHECK(g.state == O4);
	test_gpon_timer_expiry(&g);
	CHECK(g.state == O2);

	/* 11. Disable Serial Number (Emergency Stop O7) */
	test_handle_ploam(&g, ploam_overhead);
	test_handle_ploam(&g, ploam_assign_id);
	test_handle_ploam(&g, ploam_ranging);
	CHECK(g.state == O5);
	uint8_t ploam_disable_sn[13] = {
		GPON_BROADCAST, DN_DISABLE_SN, 0xff,
		'T', 'P', 'L', 'G', 0x12, 0x34, 0x56, 0x78
	};
	test_handle_ploam(&g, ploam_disable_sn);
	CHECK(g.state == O7);
	CHECK(g.o7_entries == 1);

	/* 12. Enable Serial Number from O7 back to O2 */
	uint8_t ploam_enable_sn[13] = { GPON_BROADCAST, DN_DISABLE_SN, 0x0f };
	test_handle_ploam(&g, ploam_enable_sn);
	CHECK(g.state == O2);

	/* 13. ITU-T G.984.3 Clause 9.2 CRC-8 test vectors from live hardware logs */
	const uint8_t live_vec1[12] = {
		0xff, 0x14, 0x79, 0x37, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	CHECK(test_gpon_crc8(live_vec1, 12) == 0xfa);

	const uint8_t live_vec2[12] = {
		0xff, 0x01, 0x20, 0x00, 0x00, 0xaa, 0xab, 0x60, 0x73, 0x00, 0x00, 0x00
	};
	CHECK(test_gpon_crc8(live_vec2, 12) == 0x44);

	/* 14. PLOAM ingress CRC-8 filtering and error counter verification */
	uint8_t ploam_valid[13] = {
		GPON_BROADCAST, DN_UPSTREAM_OVERHEAD, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	};
	ploam_valid[12] = test_gpon_crc8(ploam_valid, 12);

	uint8_t ploam_corrupt[13];
	memcpy(ploam_corrupt, ploam_valid, sizeof(ploam_corrupt));
	ploam_corrupt[12] ^= 0xa5; /* Corrupt CRC */

	CHECK(g.state == O2);
	CHECK(g.ploam_crc_errors == 0);
	CHECK(g.ploam_rx == 0);

	/* Corrupted PLOAM must be rejected and must not transition state */
	CHECK(!test_receive_ploam(&g, ploam_corrupt));
	CHECK(g.ploam_crc_errors == 1);
	CHECK(g.ploam_rx == 0);
	CHECK(g.state == O2);

	/* Valid PLOAM must be accepted and transition to O3 */
	CHECK(test_receive_ploam(&g, ploam_valid));
	CHECK(g.ploam_crc_errors == 1);
	CHECK(g.ploam_rx == 1);
	CHECK(g.state == O3);

	printf("%u Nokia ISAM 7360 FX PLOAM sequence / optical recovery checks passed\n", checks);
	return 0;
}
