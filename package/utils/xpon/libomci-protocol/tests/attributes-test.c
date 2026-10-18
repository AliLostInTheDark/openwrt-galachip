// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "../src/attributes.h"

static unsigned int checks;
#define CHECK(expr) do { assert(expr); checks++; } while (0)

static void be32(unsigned char *data, unsigned int word)
{
	data[0] = word >> 24;
	data[1] = word >> 16;
	data[2] = word >> 8;
	data[3] = word;
}

static void olt_g(void)
{
	static const unsigned int sizes[] = { 4, 20, 14, 14 };
	unsigned char input[64];
	unsigned int mask, len, bit;

	memset(input, 'A', sizeof(input));
	for (mask = 0; mask < 16; mask++) {
		unsigned int need = 0;

		for (bit = 0; bit < 4; bit++)
			if (mask & (8U >> bit))
				need += sizes[bit];
		for (len = 0; len <= sizeof(input); len++) {
			struct omci_olt_g olt = { 0 };
			int ret = omci_attr_olt_g_set(&olt, mask << 12, input, len);

			CHECK(ret == (len >= need ? 0 : -EINVAL));
			if (ret)
				continue;
			CHECK(olt.valid == !!(mask & 14));
			CHECK(olt.vendor_id_valid == !!(mask & 8));
			CHECK(olt.equipment_id_valid == !!(mask & 4));
			CHECK(olt.version_valid == !!(mask & 2));
			CHECK(!olt.vendor_id[4] && !olt.equipment_id[20] && !olt.version[14]);
		}
	}
	CHECK(omci_attr_olt_g_set(NULL, 0, input, sizeof(input)) == -EINVAL);
}

static void vlan_filter(void)
{
	unsigned char input[26] = { 0 };
	unsigned int tci, mask, len;
	struct omci_vlan_tagging_filter filter;

	for (tci = 0; tci <= 65535; tci++) {
		input[0] = tci >> 8;
		input[1] = tci;
		input[24] = 7;
		input[25] = 255;
		memset(&filter, 0, sizeof(filter));
		CHECK(!omci_attr_vlan_filter_create(&filter, input, sizeof(input)));
		CHECK(filter.valid && filter.num_entries == OMCI_VLAN_FILTER_MAX_ENTRIES);
		CHECK(filter.forward_operation == 7 && filter.entries[0].tci == tci);
		CHECK(filter.entries[0].vid == (tci & 4095));
		CHECK(filter.entries[0].pbit == (tci >> 13));
		CHECK(filter.entries[0].dei == ((tci >> 12) & 1));
	}
	for (mask = 0; mask < 8; mask++) {
		unsigned int need = ((mask & 4) ? 24 : 0) + !!(mask & 2) + !!(mask & 1);

		for (len = 0; len <= sizeof(input); len++) {
			memset(&filter, 0, sizeof(filter));
			CHECK(omci_attr_vlan_filter_set(&filter, mask << 13, input, len) ==
			      (len >= need ? 0 : -EINVAL));
		}
	}
	CHECK(omci_attr_vlan_filter_create(&filter, input, 25) == -EINVAL);
	CHECK(omci_attr_vlan_filter_set(&filter, 0x8000, NULL, 24) == -EINVAL);
}

static void extended_vlan(void)
{
	static const unsigned int sizes[] = { 1, 2, 2, 2, 1, 16, 2, 24 };
	unsigned char input[64];
	unsigned int mask, len, bit;
	struct omci_extended_vlan vlan = { 0 };
	struct omci_extended_vlan_rule *rule;

	memset(input, 0x12, sizeof(input));
	for (mask = 0; mask < 256; mask++) {
		unsigned int need = 0;

		for (bit = 0; bit < 8; bit++)
			if (mask & (128U >> bit))
				need += sizes[bit];
		for (len = 0; len <= sizeof(input); len++) {
			memset(&vlan, 0, sizeof(vlan));
			CHECK(omci_attr_extended_vlan_set(&vlan, mask << 8, input, len) ==
			      (len >= need ? 0 : -EINVAL));
		}
	}
	CHECK(omci_attr_extended_vlan_create(&vlan, input, 25) == -EINVAL);
	CHECK(!omci_attr_extended_vlan_create(&vlan, input, 26));
	CHECK(vlan.association_type == 0x12 && vlan.associated_me == 0x1212);
	CHECK(vlan.valid && vlan.max_table_size == OMCI_EXT_VLAN_MAX_RULES);
	memset(&vlan, 0, sizeof(vlan));
	be32(input, (15U << 28) | (100U << 15) | (6U << 12));
	be32(input + 4, (3U << 28) | (660U << 15) | (5U << 12) | 4);
	be32(input + 8, (1U << 30) | (15U << 16) | (4096U << 3) | 2);
	be32(input + 12, (7U << 16) | (100U << 3) | 1);
	CHECK(!omci_attr_vlan_rule_update(&vlan, input, 16));
	rule = &vlan.rules[0];
	CHECK(vlan.rule_count == 1 && !memcmp(rule->raw, input, 16));
	CHECK(rule->filter_outer_pbit == 15 && rule->filter_outer_vid == 100 &&
	      rule->filter_outer_tpid_dei == 6);
	CHECK(rule->filter_inner_pbit == 3 && rule->filter_inner_vid == 660 &&
	      rule->filter_inner_tpid_dei == 5 && rule->filter_ethertype == 4);
	CHECK(rule->tags_to_remove == 1 && rule->treat_outer_pbit == 15 &&
	      rule->treat_outer_vid == 4096 && rule->treat_outer_tpid_dei == 2);
	CHECK(rule->treat_inner_pbit == 7 && rule->treat_inner_vid == 100 &&
	      rule->treat_inner_tpid_dei == 1 && !rule->delete);
	CHECK(omci_attr_vlan_rule_update(&vlan, input, 15) == -EINVAL);
	CHECK(omci_attr_extended_vlan_set(&vlan, 0x0400, NULL, 16) == -EINVAL);
}

static void table_updates(void)
{
	struct omci_extended_vlan vlan = { 0 }, saved;
	unsigned char input[16] = { 0 };
	unsigned int i;

	for (i = 0; i < OMCI_EXT_VLAN_MAX_RULES; i++) {
		input[0] = i;
		CHECK(!omci_attr_vlan_rule_update(&vlan, input, sizeof(input)));
		CHECK(vlan.rule_count == i + 1);
	}
	saved = vlan;
	input[0] = OMCI_EXT_VLAN_MAX_RULES;
	CHECK(omci_attr_vlan_rule_update(&vlan, input, sizeof(input)) == -ENOSPC);
	CHECK(!memcmp(&vlan, &saved, sizeof(vlan)));
	input[0] = 0;
	input[15] = 1;
	CHECK(!omci_attr_vlan_rule_update(&vlan, input, sizeof(input)));
	CHECK(vlan.rule_count == OMCI_EXT_VLAN_MAX_RULES && vlan.rules[0].raw[15] == 1);
	/* Deletion uses the eight-byte filter key, independent of treatment. */
	memset(input + 8, 255, 8);
	input[0] = OMCI_EXT_VLAN_MAX_RULES;
	CHECK(!omci_attr_vlan_rule_update(&vlan, input, sizeof(input)));
	CHECK(vlan.rule_count == OMCI_EXT_VLAN_MAX_RULES);
	for (i = 0; i < OMCI_EXT_VLAN_MAX_RULES; i++) {
		struct omci_extended_vlan_rule empty = { 0 };

		input[0] = i;
		CHECK(!omci_attr_vlan_rule_update(&vlan, input, sizeof(input)));
		CHECK(vlan.rule_count == OMCI_EXT_VLAN_MAX_RULES - i - 1);
		CHECK(!memcmp(&vlan.rules[vlan.rule_count], &empty, sizeof(empty)));
		if (vlan.rule_count)
			CHECK(vlan.rules[0].raw[0] == i + 1);
	}
	vlan.rule_count = OMCI_EXT_VLAN_MAX_RULES + 1;
	CHECK(omci_attr_vlan_rule_update(&vlan, input, sizeof(input)) == -EINVAL);
}

int main(void)
{
	olt_g();
	vlan_filter();
	extended_vlan();
	table_updates();
	printf("%u OLT-G/VLAN parsing and table checks passed\n", checks);
	return 0;
}
