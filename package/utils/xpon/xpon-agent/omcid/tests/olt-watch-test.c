// SPDX-License-Identifier: GPL-2.0-only
#include "daemon.h"

static unsigned int checks;
#define CHECK(v) do { assert(v); checks++; } while (0)

static void report(struct omcid_olt_watch *w, unsigned long now, char *line, size_t size)
{
	FILE *out = fmemopen(line, size, "w");

	CHECK(out);
	CHECK(omcid_olt_watch_poll(w, now, 34, out, false, false) == -1);
	fclose(out);
}

int main(void)
{
	struct omcid_olt_watch w = {};
	u8 pdu[44] = {};
	char line[256];
	unsigned int i;

	/* Long blackout: MIB Reset and provisioning, reported after 10 s quiet. */
	omcid_olt_watch_link(&w, false, 1000);
	omcid_olt_watch_link(&w, true, 117000);
	pdu[2] = 0x40 | 15;
	omcid_olt_watch_rx(&w, pdu, sizeof(pdu), 118000);
	for (i = 0; i < 12; i++) {
		pdu[2] = 0x40 | 4;
		omcid_olt_watch_rx(&w, pdu, sizeof(pdu), 119000 + i);
	}
	CHECK(omcid_olt_watch_poll(&w, 120000, 34, stdout, false, false) == 129011 - 120000);
	report(&w, 129100, line, sizeof(line));
	CHECK(strstr(line, "after 116 s OMCC unavailable: OLT requested MIB reset; 13 messages"));
	CHECK(strstr(line, "MIB reset 1,") && strstr(line, "create 12,") && strstr(line, "MIB sync 34"));
	CHECK(omcid_olt_watch_poll(&w, 200000, 34, stdout, false, false) == -1);

	/* Short blackout the OLT only audits. */
	omcid_olt_watch_link(&w, false, 300000);
	omcid_olt_watch_link(&w, true, 311000);
	pdu[2] = 0x40 | 13;
	omcid_olt_watch_rx(&w, pdu, sizeof(pdu), 312000);
	report(&w, 322000, line, sizeof(line));
	CHECK(strstr(line, "after 11 s OMCC unavailable: audit only"));

	/* First activation with no OMCI at all: reported at the 120 s cap. */
	w = (struct omcid_olt_watch) {};
	omcid_olt_watch_link(&w, true, 5000);
	CHECK(omcid_olt_watch_poll(&w, 6000, 0, stdout, false, false) == 119000);
	report(&w, 125000, line, sizeof(line));
	CHECK(strstr(line, "OLT after activation: no OMCI; 0 messages"));

	/* A new outage must not include the preceding outage or online time. */
	w = (struct omcid_olt_watch) {};
	omcid_olt_watch_link(&w, false, 0);
	omcid_olt_watch_link(&w, false, 1000);
	omcid_olt_watch_link(&w, true, 10000);
	CHECK(w.outage_ms == 10000);
	pdu[2] = 9;
	omcid_olt_watch_rx(&w, pdu, sizeof(pdu), 11000);
	FILE *out = fmemopen(line, sizeof(line), "w");
	CHECK(out);
	CHECK(omcid_olt_watch_poll(&w, 12000, 34, out, false, true) == -1);
	fclose(out);
	CHECK(strstr(line, "after 10 s OMCC unavailable"));
	omcid_olt_watch_link(&w, false, 12000);
	omcid_olt_watch_link(&w, true, 312000);
	report(&w, 432000, line, sizeof(line));
	CHECK(strstr(line, "after 300 s OMCC unavailable"));

	/* Non-zero TX responses are deduplicated and summarized for OLT onboarding. */
	{
		u8 rx_set[16] = { 0, 1, 0x40 | 8, 0x0a, 0x01, 0x00, 0x00, 0x01 };
		u8 tx_ok[16] = { 0, 1, 0x20 | 8, 0x0a, 0x01, 0x00, 0x00, 0x01, 0 };
		u8 tx_unk[16] = { 0, 2, 0x20 | 9, 0x0a, 0xfe, 0x01, 0x00, 0x02, 4 };
		u8 tx_ext[16] = { 0, 3, 0x20 | 4, 0x0b, 0x01, 0x50, 0x00, 0x03, 0, 4, 2 };
		char big[1024];

		omcid_olt_watch_link(&w, true, 500000);
		omcid_olt_watch_rx(&w, rx_set, sizeof(rx_set), 500100);
		omcid_olt_watch_tx(&w, tx_ok, sizeof(tx_ok));
		omcid_olt_watch_tx(&w, tx_unk, sizeof(tx_unk));
		omcid_olt_watch_tx(&w, tx_unk, sizeof(tx_unk));
		omcid_olt_watch_tx(&w, tx_ext, sizeof(tx_ext));
		CHECK(w.issue_count == 2);
		CHECK(w.issues[0].class_id == 0xfe01 && w.issues[0].count == 2 &&
		      w.issues[0].result == 4);
		CHECK(w.issues[1].class_id == 0x0150 && w.issues[1].count == 1 &&
		      w.issues[1].result == 2);
		report(&w, 520000, big, sizeof(big));
		CHECK(strstr(big, "OLT non-zero responses (2 unique):"));
		CHECK(strstr(big, "me=65025/0x0002(mt=9,rc=4,x2)"));
		CHECK(strstr(big, "me=336/0x0003(mt=4,rc=2,x1)"));
	}

	printf("%u OLT activation summary checks passed\n", checks);
	return 0;
}
