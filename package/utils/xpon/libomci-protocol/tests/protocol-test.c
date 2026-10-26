// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "../src/me.h"
#include "../src/wire.h"

static unsigned int checks;
#define CHECK(expr) do { assert(expr); checks++; } while (0)

static void attributes(unsigned int profile, const struct omci_me_desc *desc)
{
	unsigned char object[OMCI_MAX_ATTR_DATA], input[OMCI_MAX_ATTR_DATA];
	unsigned char output[OMCI_MAX_ATTR_DATA + 2];
	unsigned int i, size;
	unsigned short mask;
	size_t used;
	int ret;

	for (i = 0; i < sizeof(input); i++)
		input[i] = i + 1;
	memset(object, 0, sizeof(object));
	mask = 0;
	CHECK(!omci_me_decode_create_data(desc, object, &mask, input, sizeof(input)));
	for (i = 0; i < desc->num_attrs; i++) {
		const struct omci_attr_desc *attr = &desc->attrs[i];
		unsigned short id = 0, class_id = 0;
		unsigned int index = 0;
		const struct omci_me_desc *found;
		const struct omci_attr_desc *field;

		CHECK(attr->offset + attr->len <= sizeof(object));
		CHECK(!omci_me_attr_id_profile(profile, desc->class_id, i, &id));
		CHECK(!omci_me_attr_get_profile(profile, id, &class_id, &index, &found, &field));
		CHECK(class_id == desc->class_id && index == i && found == desc && field == attr);
		memset(object, 0, sizeof(object));
		mask = 0;
		ret = omci_me_decode_set_data(desc, object, &mask, attr->mask, input, sizeof(input));
		if (!(attr->mask & desc->valid_attr_mask)) {
			CHECK(ret == -EINVAL);
		} else if (!(attr->access & OMCI_ATTR_ACCESS_WRITE)) {
			CHECK(ret == -EACCES && !mask);
		} else {
			CHECK(!ret && mask == attr->mask);
			CHECK(!memcmp(object + attr->offset, input, attr->len));
			if (attr->len)
				CHECK(omci_me_decode_set_data(desc, object, &mask, attr->mask,
						      input, attr->len - 1) == -EINVAL);
		}
	}
	memcpy(object, input, sizeof(object));
	for (size = 0; size <= OMCI_MAX_ATTR_DATA; size++) {
		memset(output, 0xa5, sizeof(output));
		mask = 0xffff;
		used = sizeof(output);
		ret = omci_me_encode_data(desc, object, 0xffff, output + 1, size, &mask, &used);
		CHECK(ret == 0 || ret == -ENOSPC);
		CHECK(used <= size && output[0] == 0xa5 && output[size + 1] == 0xa5);
		CHECK(!(mask & ~desc->valid_attr_mask));
	}
	CHECK(omci_me_decode_set_data(desc, NULL, &mask, 0, input, 0) == -EINVAL);
	CHECK(omci_me_decode_create_data(desc, object, NULL, input, 0) == -EINVAL);
	CHECK(omci_me_encode_data(desc, NULL, 0, output, 0, &mask, &used) == -EINVAL);
}

static void catalogue(void)
{
	unsigned int profile, classes = 0;

	for (profile = 0; profile <= OMCI_OLT_PROFILE_ZTE; profile++) {
		struct omci_me_class class, direct;
		unsigned int index = 0, next = 0, nattrs = 0;
		unsigned char seen[65536] = { 0 };
		int ret;

		while (!(ret = omci_me_class_next_profile(profile, index, &class, &next))) {
			const struct omci_me_desc *desc = omci_me_lookup_profile(profile, class.class_id);

			CHECK(next == index + 1 && !seen[class.class_id]);
			seen[class.class_id] = 1;
			CHECK(!omci_me_class_get_profile(profile, class.class_id, &direct));
			CHECK(!strcmp(class.name, direct.name));
			CHECK(class.category == direct.category && class.support == direct.support &&
			      class.flags == direct.flags && omci_me_active_profile(profile, class.class_id));
			if (desc) {
				unsigned int action;

				nattrs += desc->num_attrs;
				for (action = 0; action < 32; action++)
					CHECK(omci_me_action_allowed_profile(profile, class.class_id, action) ==
					      !!(desc->actions & OMCI_BIT(action)));
				CHECK(!omci_me_action_allowed_profile(profile, class.class_id, 32));
				attributes(profile, desc);
			}
			index = next;
			classes++;
		}
		CHECK(ret == -ENOENT && index > 0);
		CHECK(nattrs == omci_me_attr_count_profile(profile));
		CHECK(omci_me_attr_get_profile(profile, 0, NULL, NULL, NULL, NULL) == -EINVAL);
		CHECK(omci_me_attr_get_profile(profile, nattrs + 1, NULL, NULL, NULL, NULL) == -ENOENT);
	}
	for (; profile <= 255; profile++) {
		CHECK(!omci_me_lookup_profile(profile, OMCI_CLASS_ONU_G));
		CHECK(!omci_me_active_profile(profile, OMCI_CLASS_ONU_G));
		CHECK(!omci_me_attr_count_profile(profile));
	}
	CHECK(classes > 100);
}

static void profiles(void)
{
	static const struct { const char *vendor; unsigned int profile; } vendors[] = {
		{ "ALCL", OMCI_OLT_PROFILE_NOKIA_ALCL }, { "noki", OMCI_OLT_PROFILE_NOKIA_ALCL },
		{ "DSNW", OMCI_OLT_PROFILE_DASAN }, { "dasa", OMCI_OLT_PROFILE_DASAN },
		{ "HWTC", OMCI_OLT_PROFILE_HUAWEI }, { "hwti", OMCI_OLT_PROFILE_HUAWEI },
		{ "FHTT", OMCI_OLT_PROFILE_FIBERHOME }, { "fhtc", OMCI_OLT_PROFILE_FIBERHOME },
		{ "ZTEG", OMCI_OLT_PROFILE_ZTE }, { "ztec", OMCI_OLT_PROFILE_ZTE },
		{ "TEST", OMCI_OLT_PROFILE_GENERIC }
	};
	struct omci_olt_g olt = { .valid = true, .vendor_id_valid = true };
	unsigned int i;

	CHECK(omci_profile_detect(NULL) == OMCI_OLT_PROFILE_GENERIC);
	for (i = 0; i < sizeof(vendors) / sizeof(vendors[0]); i++) {
		memcpy(olt.vendor_id, vendors[i].vendor, 4);
		CHECK(omci_profile_detect(&olt) == vendors[i].profile);
	}
	olt.vendor_id_valid = false;
	olt.equipment_id_valid = true;
	strcpy(olt.equipment_id, "Huawei OLT");
	CHECK(omci_profile_detect(&olt) == OMCI_OLT_PROFILE_HUAWEI);
	olt.version_valid = true;
	memcpy(olt.version, "a\001b\177c", 6);
	omci_profile_sanitize_olt_g(&olt, OMCI_OLT_QUIRK_SANITIZE_VERSION);
	CHECK(!strcmp(olt.version, "a b c"));
	for (i = 0; i <= 255; i++) {
		unsigned short ani = 0;
		bool known = i >= OMCI_OLT_PROFILE_GENERIC && i <= OMCI_OLT_PROFILE_ZTE;
		struct omci_extended_vlan_rule rule = {
			.treat_inner_vid = 4096, .treat_outer_vid = 4096, .treat_inner_pbit = 15
		};

		CHECK(omci_profile_valid(i) == known);
		CHECK(omci_profile_forceable(i) == (i == 0 || (known && i != OMCI_OLT_PROFILE_AUTO)));
		CHECK(omci_profile_normalize_uni_entity(i, 0x1234) ==
		      (i == OMCI_OLT_PROFILE_FIBERHOME ? 0x1234 : 0x34));
		CHECK(omci_profile_resolve_multicast_ani(i, 0x1234, &ani) ==
		      (i == OMCI_OLT_PROFILE_DASAN ? 0 : -EOPNOTSUPP));
		omci_profile_normalize_vlan_rule(i, &rule);
		CHECK(rule.treat_inner_pbit == (i == OMCI_OLT_PROFILE_ZTE ? 8 : 15));
	}
}

int main(void)
{
	catalogue();
	profiles();
	printf("%u shared catalogue/attribute/profile checks passed\n", checks);
	return 0;
}
