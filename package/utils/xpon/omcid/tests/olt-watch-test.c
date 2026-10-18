// SPDX-License-Identifier: GPL-2.0-only
#include "daemon.h"

static unsigned int checks;
#define CHECK(v) do { assert(v); checks++; } while (0)

static void report(struct omcid_olt_watch *w, unsigned long now, char *line, size_t size)
{
	FILE *out = fmemopen(line, size, "w");

	CHECK(out);
	CHECK(omcid_olt_watch_poll(w, now, 34, out) == -1);
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
	CHECK(omcid_olt_watch_poll(&w, 120000, 34, stdout) == 129011 - 120000);
	report(&w, 129100, line, sizeof(line));
	CHECK(strstr(line, "after 116 s dark: MIB reset and full reprovision; 13 messages"));
	CHECK(strstr(line, "MIB reset 1,") && strstr(line, "create 12,") && strstr(line, "MIB sync 34"));
	CHECK(omcid_olt_watch_poll(&w, 200000, 34, stdout) == -1);

	/* Short blackout the OLT only audits. */
	omcid_olt_watch_link(&w, false, 300000);
	omcid_olt_watch_link(&w, true, 311000);
	pdu[2] = 0x40 | 13;
	omcid_olt_watch_rx(&w, pdu, sizeof(pdu), 312000);
	report(&w, 322000, line, sizeof(line));
	CHECK(strstr(line, "after 11 s dark: audit only"));

	/* First activation with no OMCI at all: reported at the 120 s cap. */
	w = (struct omcid_olt_watch) {};
	omcid_olt_watch_link(&w, true, 5000);
	CHECK(omcid_olt_watch_poll(&w, 6000, 0, stdout) == 119000);
	report(&w, 125000, line, sizeof(line));
	CHECK(strstr(line, "OLT after activation: no OMCI; 0 messages"));

	printf("%u OLT activation summary checks passed\n", checks);
	return 0;
}
